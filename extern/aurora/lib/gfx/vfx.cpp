#include <aurora/vfx.hpp>

#include "../gx/fifo.hpp"
#include "../gx/gx.hpp"
#include "../gx/texture.hpp"
#include "../logging.hpp"
#include "recording.hpp"
#include "resource_cache.hpp"
#include "texture.hpp"

#include <aurora/gfx.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

// Remastered's VFX particle materials, drawn natively (see aurora/vfx.hpp). The formulas are the
// ones recovered from the Remastered shaders (build/mpr/vfx/SPEC.md section 2); the tone curve,
// gamma and fog repeat what the PBR path of the GX shader does to its output.
namespace aurora::gfx::vfx {
namespace {
Module Log("aurora::gfx::vfx");

constexpr uint32_t MaxQuads = 1u << 14; // 65536 vertices
constexpr uint32_t UniformSize = 256;
constexpr auto EvictAfter = std::chrono::seconds(10);

// ---- Uniforms (all vec4-sized members, so the layout is the same everywhere) ----
struct Uniform {
  float proj[16];
  float pn[12];
  float params0[4];
  float tone[3][4];
  float fogColor[4];
  float fog[4];   // type, a, b, c
  float misc[4];  // modulate, erosion row, erosion component, reversed Z
  float ramp[4];  // ramp rows 0 and 1, blend
  float slots[4]; // uv set 0, uv set 1, layers 0, layers 1
};
static_assert(sizeof(Uniform) == UniformSize);

// ---- Array textures ----
// A slot's texture as a texture_2d_array: a 1-layer view of the GX texture itself, or, for an atlas,
// a copy of its cells that an encoder task makes on first use (a draw can't copy: it only has a
// render pass encoder).
enum class State : int { New = 0, Requested = 1, Ready = 2, Failed = 3 };

struct ArrayEntry {
  uint32_t id = 0;
  TextureHandle source; // keeps the TextureRef (and so its address) alive
  uint32_t cols = 1, rows = 1, layers = 1;
  bool copy = false;
  std::atomic<State> state{State::New};
  wgpu::Texture texture; // the copy; render thread only until state is Ready
  wgpu::TextureView view;
  std::chrono::steady_clock::time_point lastUse;
};

struct State_ {
  std::mutex mutex;
  std::map<std::tuple<const void*, uint32_t, uint32_t, uint32_t>, std::shared_ptr<ArrayEntry>> byKey;
  std::unordered_map<uint32_t, std::shared_ptr<ArrayEntry>> byId;
  uint32_t nextId = 1;
  DrawTypeId drawType = InvalidDrawType;
  EncoderTaskId task = InvalidEncoderTask;
  // Render thread only
  wgpu::BindGroupLayout bindLayout;
  wgpu::PipelineLayout pipelineLayout;
  std::map<std::array<uint64_t, 6>, wgpu::RenderPipeline> pipelines;
  std::map<std::array<const void*, 5>, wgpu::BindGroup> groups;
  wgpu::Texture dummyTexture;
  wgpu::TextureView dummyView;
};
State_ g_state;

struct Payload {
  uint32_t features;
  uint32_t blend;
  uint32_t compare; // wgpu::CompareFunction
  uint32_t depthWrite;
  uint32_t quadCount;
  Range verts;
  Range indices;
  Range uniform;
  uint32_t arrayId[2]; // 0: the dummy
  uint8_t wrapS[2];
  uint8_t wrapT[2];
  uint8_t linear[2];
};
static_assert(sizeof(Payload) <= InlineDrawPayloadSize);

const char* ShaderSource = R"(
struct U {
  proj: mat4x4f,
  pn: mat3x4f,
  params0: vec4f,
  tone0: vec4f,
  tone1: vec4f,
  tone2: vec4f,
  fogColor: vec4f,
  fog: vec4f,
  misc: vec4f,
  ramp: vec4f,
  slots: vec4f,
}
@group(0) @binding(0) var<uniform> u: U;
@group(0) @binding(1) var t0: texture_2d_array<f32>;
@group(0) @binding(2) var t1: texture_2d_array<f32>;
@group(0) @binding(3) var s0: sampler;
@group(0) @binding(4) var s1: sampler;

struct VIn {
  @location(0) pos: vec3f,
  @location(1) uv0: vec3f,
  @location(2) uv1: vec3f,
  @location(3) color: vec4f,
  @location(4) e0: vec4f,
  @location(5) e1: vec4f,
  @location(6) e2: vec4f,
}
struct VOut {
  @builtin(position) pos: vec4f,
  @location(0) uv0: vec3f,
  @location(1) uv1: vec3f,
  @location(2) color: vec4f,
  @location(3) e0: vec4f,
  @location(4) e1: vec4f,
  @location(5) e2: vec4f,
}

@vertex
fn vs_main(in: VIn) -> VOut {
  var out: VOut;
  let mv = vec4f(in.pos, 1.0) * u.pn;
  out.pos = vec4f(mv, 1.0) * u.proj;
  out.uv0 = in.uv0;
  out.uv1 = in.uv1;
  out.color = in.color;
  out.e0 = in.e0;
  out.e1 = in.e1;
  out.e2 = in.e2;
  return out;
}

fn layer(z: f32, n: f32) -> i32 {
  // round() is round-half-to-even
  return i32(round(clamp(z, 0.0, max(n - 1.0, 0.0))));
}
fn uv_of(in: VOut, set: f32) -> vec3f {
  return select(in.uv0, in.uv1, set > 0.5);
}
fn row_of(in: VOut, i: i32) -> vec4f {
  if (i <= 0) { return in.e0; }
  if (i == 1) { return in.e1; }
  return in.e2;
}

fn tone(c: vec3f) -> vec3f {
  var o = c;
  var tm: vec3f;
  if (u.tone1.x > 0.0) {
    if (u.tone0.w > 0.0) { o = o * u.tone0.w; }
    let toe = ((u.tone0.x * o + u.tone0.y) * o + u.tone0.z) * o;
    let line = u.tone1.x * o + u.tone1.y;
    let st = max(u.tone2.y * o + u.tone2.z, vec3f(0.0));
    let sh = u.tone2.x * st / (1.0 + st) + u.tone2.w;
    tm = select(select(sh, line, o < vec3f(u.tone1.w)), toe, o < vec3f(u.tone1.z));
  } else {
    tm = min(o, vec3f(0.6)) + 0.4 * (1.0 - exp(-max(o - 0.6, vec3f(0.0)) / 0.4));
  }
  return pow(clamp(tm, vec3f(0.0), vec3f(1.0)), vec3f(1.0 / 2.2));
}

fn fog_factor(fz: f32) -> f32 {
  let ft = i32(u.fog.x);
  if (ft == 0) { return 0.0; }
  let depth = select(fz, 1.0 - fz, u.misc.w > 0.5);
  var base: f32;
  if ((ft & 8) != 0) { base = u.fog.y * depth; } else { base = u.fog.y / (u.fog.z - depth); }
  var f = clamp(base - u.fog.w, 0.0, 1.0);
  var r = f;
  switch (ft & 7) {
    case 4: { r = 1.0 - exp2(-8.0 * f); }
    case 5: { r = 1.0 - exp2(-8.0 * f * f); }
    case 6: { r = exp2(-8.0 * (1.0 - f)); }
    case 7: { f = 1.0 - f; r = exp2(-8.0 * f * f); }
    default: {}
  }
  return clamp(r, 0.0, 1.0);
}

@fragment
fn fs_main(in: VOut) -> @location(0) vec4f {
  let vc = in.color;
  let M = u.misc.x;
  let uvA = uv_of(in, u.slots.x);
  let uvB = uv_of(in, u.slots.y);
  var rgb = vc.rgb * M;
  var x = 1.0;
  if ((FEAT & 8u) != 0u) {
    // ramp
    let r = textureSample(t0, s0, uvA.xy, layer(uvA.z, u.slots.z));
    let t3 = r.x * r.x * r.x;
    let c0 = row_of(in, i32(u.ramp.x));
    let c1 = row_of(in, i32(u.ramp.y));
    rgb = mix(c0.rgb, c1.rgb, t3) * vc.rgb * M;
    x = r.y * mix(c0.w, c1.w, t3);
    if ((FEAT & 2u) != 0u) {
      x = x * textureSample(t1, s1, uvB.xy, layer(uvB.z, u.slots.w)).x;
    }
  } else if ((FEAT & 16u) != 0u) {
    // indirect warp: slot 1 offsets the lookup in slot 0
    let d = textureSample(t1, s1, uvB.xy, layer(uvB.z, u.slots.w)).xy;
    let uv = uvA.xy + (d * 0.99609375 - vec2f(0.5)) * u.params0.xy;
    let t = textureSample(t0, s0, uv, layer(uvA.z, u.slots.z));
    rgb = t.rgb * vc.rgb * M;
    x = t.w;
  } else if ((FEAT & 1u) != 0u) {
    let c = textureSample(t0, s0, uvA.xy, layer(uvA.z, u.slots.z));
    rgb = c.rgb * vc.rgb * M;
    x = c.w;
    if ((FEAT & 2u) != 0u) {
      x = x * textureSample(t1, s1, uvB.xy, layer(uvB.z, u.slots.w)).x;
    }
  } else if ((FEAT & 2u) != 0u) {
    x = textureSample(t0, s0, uvA.xy, layer(uvA.z, u.slots.z)).x;
  }
  if ((FEAT & 4u) != 0u) {
    let e = row_of(in, i32(u.misc.y))[i32(u.misc.z)];
    x = max(0.0, x - e);
  }
  let a = x * vc.w;
  if (a <= 0.0) { discard; }
  let alpha = clamp(a, 0.0, 1.0);
  var col = tone(rgb);
  // Fog fades towards the fog colour, but an additive draw adds nothing in the distance and a
  // premultiplied one adds the fog colour weighted by its alpha.
  var fc = u.fogColor.rgb;
  if (BLEND == 2u) { fc = vec3f(0.0); }
  if (BLEND == 1u) { fc = fc * alpha; }
  col = mix(col, fc, fog_factor(in.pos.z));
  return vec4f(col, alpha);
}
)";

// ---- Helpers ----
wgpu::AddressMode to_address(uint8_t mode) {
  switch (mode) {
  case GX_REPEAT:
    return wgpu::AddressMode::Repeat;
  case GX_MIRROR:
    return wgpu::AddressMode::MirrorRepeat;
  default:
    return wgpu::AddressMode::ClampToEdge;
  }
}

wgpu::CompareFunction to_compare(GXCompare func) {
  const bool rev = uses_reversed_z();
  switch (func) {
  case GX_NEVER:
    return wgpu::CompareFunction::Never;
  case GX_LESS:
    return rev ? wgpu::CompareFunction::Greater : wgpu::CompareFunction::Less;
  case GX_EQUAL:
    return wgpu::CompareFunction::Equal;
  case GX_LEQUAL:
    return rev ? wgpu::CompareFunction::GreaterEqual : wgpu::CompareFunction::LessEqual;
  case GX_GREATER:
    return rev ? wgpu::CompareFunction::Less : wgpu::CompareFunction::Greater;
  case GX_NEQUAL:
    return wgpu::CompareFunction::NotEqual;
  case GX_GEQUAL:
    return rev ? wgpu::CompareFunction::LessEqual : wgpu::CompareFunction::GreaterEqual;
  default:
    return wgpu::CompareFunction::Always;
  }
}

uint32_t block_size(wgpu::TextureFormat format) {
  switch (format) {
  case wgpu::TextureFormat::BC1RGBAUnorm:
  case wgpu::TextureFormat::BC1RGBAUnormSrgb:
  case wgpu::TextureFormat::BC2RGBAUnorm:
  case wgpu::TextureFormat::BC2RGBAUnormSrgb:
  case wgpu::TextureFormat::BC3RGBAUnorm:
  case wgpu::TextureFormat::BC3RGBAUnormSrgb:
  case wgpu::TextureFormat::BC4RUnorm:
  case wgpu::TextureFormat::BC4RSnorm:
  case wgpu::TextureFormat::BC5RGUnorm:
  case wgpu::TextureFormat::BC5RGSnorm:
  case wgpu::TextureFormat::BC6HRGBUfloat:
  case wgpu::TextureFormat::BC6HRGBFloat:
  case wgpu::TextureFormat::BC7RGBAUnorm:
  case wgpu::TextureFormat::BC7RGBAUnormSrgb:
  case wgpu::TextureFormat::ASTC4x4Unorm:
  case wgpu::TextureFormat::ASTC4x4UnormSrgb:
    return 4;
  default:
    return 1;
  }
}

wgpu::BlendState blend_state(Blend blend) {
  wgpu::BlendComponent c{.operation = wgpu::BlendOperation::Add};
  switch (blend) {
  case Blend::Alpha:
    c.srcFactor = wgpu::BlendFactor::SrcAlpha;
    c.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
    break;
  case Blend::Premultiplied:
    c.srcFactor = wgpu::BlendFactor::One;
    c.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
    break;
  case Blend::Additive:
    c.srcFactor = wgpu::BlendFactor::SrcAlpha;
    c.dstFactor = wgpu::BlendFactor::One;
    break;
  case Blend::Opaque:
    c.srcFactor = wgpu::BlendFactor::One;
    c.dstFactor = wgpu::BlendFactor::Zero;
    break;
  }
  return {.color = c, .alpha = c};
}

void ensure_static(const wgpu::Device& device) {
  if (g_state.pipelineLayout) {
    return;
  }
  wgpu::BindGroupLayoutEntry entries[5]{};
  entries[0] = {
      .binding = 0,
      .visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment,
      .buffer = {.type = wgpu::BufferBindingType::Uniform, .hasDynamicOffset = true, .minBindingSize = UniformSize},
  };
  for (uint32_t i = 0; i < 2; ++i) {
    entries[1 + i] = {
        .binding = 1 + i,
        .visibility = wgpu::ShaderStage::Fragment,
        .texture = {.sampleType = wgpu::TextureSampleType::Float, .viewDimension = wgpu::TextureViewDimension::e2DArray},
    };
    entries[3 + i] = {
        .binding = 3 + i,
        .visibility = wgpu::ShaderStage::Fragment,
        .sampler = {.type = wgpu::SamplerBindingType::Filtering},
    };
  }
  const wgpu::BindGroupLayoutDescriptor layoutDescriptor{
      .label = "VFX Bind Group Layout",
      .entryCount = 5,
      .entries = entries,
  };
  g_state.bindLayout = device.CreateBindGroupLayout(&layoutDescriptor);
  const wgpu::PipelineLayoutDescriptor pipelineLayoutDescriptor{
      .label = "VFX Pipeline Layout",
      .bindGroupLayoutCount = 1,
      .bindGroupLayouts = &g_state.bindLayout,
  };
  g_state.pipelineLayout = device.CreatePipelineLayout(&pipelineLayoutDescriptor);
}

wgpu::RenderPipeline make_pipeline(const DrawContext& ctx, const Payload& p) {
  const std::string source = "const FEAT: u32 = " + std::to_string(p.features) + "u;\nconst BLEND: u32 = " +
                             std::to_string(p.blend) + "u;\n" + ShaderSource;
  wgpu::ShaderSourceWGSL wgsl{};
  wgsl.code = source.c_str();
  const wgpu::ShaderModuleDescriptor moduleDescriptor{.nextInChain = &wgsl, .label = "VFX Module"};
  const wgpu::ShaderModule module = ctx.device.CreateShaderModule(&moduleDescriptor);

  static constexpr std::array<wgpu::VertexAttribute, 7> attributes{{
      {.format = wgpu::VertexFormat::Float32x3, .offset = 0, .shaderLocation = 0},
      {.format = wgpu::VertexFormat::Float32x3, .offset = 12, .shaderLocation = 1},
      {.format = wgpu::VertexFormat::Float32x3, .offset = 24, .shaderLocation = 2},
      {.format = wgpu::VertexFormat::Float32x4, .offset = 36, .shaderLocation = 3},
      {.format = wgpu::VertexFormat::Float32x4, .offset = 52, .shaderLocation = 4},
      {.format = wgpu::VertexFormat::Float32x4, .offset = 68, .shaderLocation = 5},
      {.format = wgpu::VertexFormat::Float32x4, .offset = 84, .shaderLocation = 6},
  }};
  const wgpu::VertexBufferLayout vertexLayout{
      .stepMode = wgpu::VertexStepMode::Vertex,
      .arrayStride = sizeof(Vertex),
      .attributeCount = attributes.size(),
      .attributes = attributes.data(),
  };

  const wgpu::BlendState blend = blend_state(static_cast<Blend>(p.blend));
  std::array<wgpu::ColorTargetState, MaxColorAttachments> targets{};
  for (uint32_t i = 0; i < ctx.layout.colorAttachmentCount; ++i) {
    const bool scene = i == SceneColorAttachmentIndex;
    targets[i] = {
        .format = ctx.layout.colorAttachments[i].format,
        .blend = scene ? &blend : nullptr,
        .writeMask = scene ? wgpu::ColorWriteMask::All : wgpu::ColorWriteMask::None,
    };
  }
  const wgpu::FragmentState fragment{
      .module = module,
      .entryPoint = "fs_main",
      .targetCount = ctx.layout.colorAttachmentCount,
      .targets = targets.data(),
  };
  const wgpu::DepthStencilState depth{
      .format = ctx.layout.depthStencilFormat,
      .depthWriteEnabled = p.depthWrite != 0,
      .depthCompare = static_cast<wgpu::CompareFunction>(p.compare),
  };
  const wgpu::RenderPipelineDescriptor descriptor{
      .label = "VFX Pipeline",
      .layout = g_state.pipelineLayout,
      .vertex = {.module = module, .entryPoint = "vs_main", .bufferCount = 1, .buffers = &vertexLayout},
      .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList, .cullMode = wgpu::CullMode::None},
      .depthStencil = ctx.layout.depthStencilFormat != wgpu::TextureFormat::Undefined ? &depth : nullptr,
      .multisample = {.count = ctx.layout.sampleCount, .mask = UINT32_MAX},
      .fragment = &fragment,
  };
  return ctx.device.CreateRenderPipeline(&descriptor);
}

void ensure_dummy(const DrawContext& ctx) {
  if (g_state.dummyView) {
    return;
  }
  const wgpu::TextureDescriptor descriptor{
      .label = "VFX Dummy",
      .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst,
      .size = {1, 1, 1},
      .format = wgpu::TextureFormat::RGBA8Unorm,
  };
  g_state.dummyTexture = ctx.device.CreateTexture(&descriptor);
  const uint8_t white[4] = {255, 255, 255, 255};
  const wgpu::TexelCopyTextureInfo dst{.texture = g_state.dummyTexture};
  const wgpu::TexelCopyBufferLayout layout{.bytesPerRow = 4, .rowsPerImage = 1};
  const wgpu::Extent3D size{1, 1, 1};
  ctx.queue.WriteTexture(&dst, white, 4, &layout, &size);
  const wgpu::TextureViewDescriptor view{.dimension = wgpu::TextureViewDimension::e2DArray, .arrayLayerCount = 1};
  g_state.dummyView = g_state.dummyTexture.CreateView(&view);
}

std::shared_ptr<ArrayEntry> find_entry(uint32_t id) {
  const std::lock_guard lock{g_state.mutex};
  const auto it = g_state.byId.find(id);
  return it == g_state.byId.end() ? nullptr : it->second;
}

// Render thread: the view a slot samples, or an empty one if it can't be drawn yet.
wgpu::TextureView entry_view(uint32_t id) {
  if (id == 0) {
    return g_state.dummyView;
  }
  const auto entry = find_entry(id);
  if (!entry) {
    return {};
  }
  if (!entry->copy) {
    if (!entry->view) {
      const wgpu::TextureViewDescriptor view{.dimension = wgpu::TextureViewDimension::e2DArray, .arrayLayerCount = 1};
      entry->view = entry->source->texture.CreateView(&view);
      entry->state.store(State::Ready, std::memory_order_release);
    }
    return entry->view;
  }
  return entry->state.load(std::memory_order_acquire) == State::Ready ? entry->view : wgpu::TextureView{};
}

void build_array(const EncoderTaskContext& ctx, const wgpu::CommandEncoder& cmd, const void* payload, size_t size,
                 void*) {
  uint32_t id;
  if (size != sizeof(id)) {
    return;
  }
  std::memcpy(&id, payload, sizeof(id));
  const auto entry = find_entry(id);
  if (!entry || entry->state.load() != State::Requested) {
    return;
  }
  const auto& src = *entry->source;
  const uint32_t cw = src.size.width / entry->cols;
  const uint32_t ch = src.size.height / entry->rows;
  const uint32_t block = block_size(src.format);
  const auto fail = [&](const char* why) {
    Log.warn("VFX atlas {}x{} layers {} not built: {}", src.size.width, src.size.height, entry->layers, why);
    entry->state.store(State::Failed, std::memory_order_release);
  };
  if (cw == 0 || ch == 0 || cw % block != 0 || ch % block != 0) {
    return fail("cell size");
  }
  // Mip m of a cell is its own copy only if the cell halves evenly that far (and stays block aligned).
  uint32_t mips = 1;
  while (mips < src.mipCount && (cw % (1u << mips)) == 0 && (ch % (1u << mips)) == 0 &&
         ((cw >> mips) % block) == 0 && ((ch >> mips) % block) == 0) {
    ++mips;
  }
  const wgpu::TextureDescriptor descriptor{
      .label = "VFX Atlas",
      .usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst,
      .size = {cw, ch, entry->layers},
      .format = src.format,
      .mipLevelCount = mips,
  };
  entry->texture = ctx.device.CreateTexture(&descriptor);
  for (uint32_t layer = 0; layer < entry->layers; ++layer) {
    const uint32_t col = layer % entry->cols;
    const uint32_t row = layer / entry->cols;
    for (uint32_t mip = 0; mip < mips; ++mip) {
      const uint32_t w = cw >> mip;
      const uint32_t h = ch >> mip;
      const wgpu::TexelCopyTextureInfo from{.texture = src.texture, .mipLevel = mip, .origin = {col * w, row * h, 0}};
      const wgpu::TexelCopyTextureInfo to{.texture = entry->texture, .mipLevel = mip, .origin = {0, 0, layer}};
      const wgpu::Extent3D extent{w, h, 1};
      cmd.CopyTextureToTexture(&from, &to, &extent);
    }
  }
  const wgpu::TextureViewDescriptor view{.dimension = wgpu::TextureViewDimension::e2DArray};
  entry->view = entry->texture.CreateView(&view);
  entry->state.store(State::Ready, std::memory_order_release);
}

bool ensure_registered() {
  if (g_state.task == InvalidEncoderTask) {
    g_state.task = register_encoder_task_type(EncoderTaskDescriptor{.label = "VFX Atlas", .callback = build_array});
  }
  if (g_state.drawType == InvalidDrawType) {
    g_state.drawType = register_draw_type(DrawTypeDescriptor{
        .label = "VFX Quads",
        .draw = [](const DrawContext& ctx, const wgpu::RenderPassEncoder& pass, const void* data, size_t size, void*) {
          if (size != sizeof(Payload)) {
            return;
          }
          Payload p;
          std::memcpy(&p, data, sizeof(p));
          ensure_static(ctx.device);
          ensure_dummy(ctx);
          const wgpu::TextureView views[2] = {entry_view(p.arrayId[0]), entry_view(p.arrayId[1])};
          if (!views[0] || !views[1]) {
            return;
          }
          wgpu::Sampler samplers[2];
          for (int i = 0; i < 2; ++i) {
            const auto filter = p.linear[i] ? wgpu::FilterMode::Linear : wgpu::FilterMode::Nearest;
            samplers[i] = sampler_ref(wgpu::SamplerDescriptor{
                .addressModeU = to_address(p.wrapS[i]),
                .addressModeV = to_address(p.wrapT[i]),
                .magFilter = filter,
                .minFilter = filter,
                .mipmapFilter = p.linear[i] ? wgpu::MipmapFilterMode::Linear : wgpu::MipmapFilterMode::Nearest,
                .maxAnisotropy = 1,
            });
          }
          const std::array<uint64_t, 6> pipelineKey{p.features & ~uint32_t(DepthSoften),
                                                    p.blend,
                                                    p.compare,
                                                    p.depthWrite,
                                                    ctx.layout.key,
                                                    ctx.layout.sampleCount};
          auto pipeline = g_state.pipelines.find(pipelineKey);
          if (pipeline == g_state.pipelines.end()) {
            Payload keyed = p;
            keyed.features = p.features & ~uint32_t(DepthSoften);
            pipeline = g_state.pipelines.emplace(pipelineKey, make_pipeline(ctx, keyed)).first;
          }
          const std::array<const void*, 5> groupKey{views[0].Get(), views[1].Get(), samplers[0].Get(),
                                                    samplers[1].Get(), ctx.uniformBuffer.Get()};
          auto group = g_state.groups.find(groupKey);
          if (group == g_state.groups.end()) {
            if (g_state.groups.size() > 512) {
              g_state.groups.clear();
            }
            const std::array<wgpu::BindGroupEntry, 5> entries{{
                {.binding = 0, .buffer = ctx.uniformBuffer, .offset = 0, .size = UniformSize},
                {.binding = 1, .textureView = views[0]},
                {.binding = 2, .textureView = views[1]},
                {.binding = 3, .sampler = samplers[0]},
                {.binding = 4, .sampler = samplers[1]},
            }};
            const wgpu::BindGroupDescriptor descriptor{
                .label = "VFX Bind Group",
                .layout = g_state.bindLayout,
                .entryCount = entries.size(),
                .entries = entries.data(),
            };
            group = g_state.groups.emplace(groupKey, ctx.device.CreateBindGroup(&descriptor)).first;
          }
          pass.SetPipeline(pipeline->second);
          pass.SetBindGroup(0, group->second, 1, &p.uniform.offset);
          pass.SetVertexBuffer(0, ctx.vertexBuffer, p.verts.offset, p.verts.size);
          pass.SetIndexBuffer(ctx.indexBuffer, wgpu::IndexFormat::Uint32, p.indices.offset, p.indices.size);
          pass.DrawIndexed(p.quadCount * 6);
        }});
  }
  return g_state.task != InvalidEncoderTask && g_state.drawType != InvalidDrawType;
}

// Game thread: the entry for a slot's texture and layout, its array build requested if it needs one.
// False if the draw has to wait for it.
bool resolve_slot(const Texture& tex, uint32_t& id) {
  id = 0;
  if (tex.obj == nullptr) {
    return true;
  }
  const auto handle = gx::texture::resolve_static_texture(*reinterpret_cast<const GXTexObj_*>(tex.obj));
  if (!handle) {
    return false;
  }
  const uint32_t cols = std::max(tex.cols, 1u);
  const uint32_t rows = std::max(tex.rows, 1u);
  const uint32_t layers = std::max(tex.layers, 1u);
  const bool copy = layers > 1 || cols > 1 || rows > 1;
  std::shared_ptr<ArrayEntry> entry;
  bool request = false;
  {
    const std::lock_guard lock{g_state.mutex};
    const auto now = std::chrono::steady_clock::now();
    auto& slot = g_state.byKey[{handle.get(), cols, rows, layers}];
    if (!slot) {
      slot = std::make_shared<ArrayEntry>();
      slot->id = g_state.nextId++;
      slot->source = handle;
      slot->cols = cols;
      slot->rows = rows;
      slot->layers = layers;
      slot->copy = copy;
      g_state.byId[slot->id] = slot;
    }
    slot->lastUse = now;
    entry = slot;
    request = copy && entry->state.load() == State::New;
  }
  id = entry->id;
  if (!copy) {
    return true;
  }
  if (request) {
    State expected = State::New;
    // Requested only once the task is really recorded, so a refusal retries on the next call.
    if (push_encoder_task(g_state.task, &entry->id, sizeof(entry->id))) {
      entry->state.compare_exchange_strong(expected, State::Requested);
    }
  }
  return entry->state.load(std::memory_order_acquire) == State::Ready;
}

void evict_idle() {
  const auto now = std::chrono::steady_clock::now();
  const std::lock_guard lock{g_state.mutex};
  for (auto it = g_state.byKey.begin(); it != g_state.byKey.end();) {
    if (now - it->second->lastUse > EvictAfter && it->second->state.load() != State::Requested) {
      g_state.byId.erase(it->second->id);
      it = g_state.byKey.erase(it);
    } else {
      ++it;
    }
  }
}
} // namespace

void draw_quads(const DrawDesc& desc, const Vertex* verts, uint32_t quadCount) {
  if (verts == nullptr || quadCount == 0 || quadCount > MaxQuads || !ensure_registered()) {
    return;
  }
  evict_idle();
  const uint32_t features = desc.features & ~uint32_t(DepthSoften);
  Texture slots[2] = {desc.tex[0], desc.tex[1]};
  // An opacity map alone may be given as either slot; the shader reads it from slot 0.
  if ((features & (ColorTex | Ramp | Indirect)) == 0 && (features & OpacityTex) != 0 && slots[0].obj == nullptr) {
    std::swap(slots[0], slots[1]);
  }
  uint32_t ids[2];
  const bool ready0 = resolve_slot(slots[0], ids[0]);
  const bool ready1 = resolve_slot(slots[1], ids[1]);
  if (!ready0 || !ready1) {
    return;
  }

  // The GX state the draw sees is the one after everything recorded so far.
  gx::fifo::drain();
  const auto& gx = gx::g_gxState;
  Uniform u{};
  Mat4x4<float> proj = gx.proj;
  if (gx::UseReversedZ) {
    proj.m2 = proj.m2 * Vec4<float>{-1.f, -1.f, -1.f, -1.f};
  } else {
    proj.m2 = proj.m2 + proj.m3;
  }
  static_assert(sizeof(proj) == sizeof(u.proj));
  std::memcpy(u.proj, &proj, sizeof(u.proj));
  static_assert(sizeof(gx.pnMtx[0].pos) == sizeof(u.pn));
  std::memcpy(u.pn, &gx.pnMtx[gx.currentPnMtx].pos, sizeof(u.pn));
  std::memcpy(u.params0, desc.params0, sizeof(u.params0));
  static_assert(sizeof(gx.pbrTone) == sizeof(u.tone));
  std::memcpy(u.tone, &gx.pbrTone, sizeof(u.tone));
  std::memcpy(u.fogColor, &gx.fog.color, sizeof(u.fogColor));
  u.fog[0] = float(gx.fog.type);
  u.fog[1] = gx.fog.a;
  u.fog[2] = gx.fog.b;
  u.fog[3] = gx.fog.c;
  u.misc[0] = desc.modulate;
  u.misc[1] = float(desc.erosionRow);
  u.misc[2] = float(desc.erosionComp);
  u.misc[3] = gx::UseReversedZ ? 1.f : 0.f;
  u.ramp[0] = float(desc.rampRow[0]);
  u.ramp[1] = float(desc.rampRow[1]);
  u.ramp[2] = float(uint32_t(desc.blend));
  u.slots[0] = float(slots[0].uvSet);
  u.slots[1] = float(slots[1].uvSet);
  u.slots[2] = float(std::max(slots[0].layers, 1u));
  u.slots[3] = float(std::max(slots[1].layers, 1u));

  const uint32_t vertexCount = quadCount * 4;
  std::vector<uint32_t> indices(size_t(quadCount) * 6);
  for (uint32_t q = 0; q < quadCount; ++q) {
    const uint32_t b = q * 4;
    const uint32_t i[6] = {b, b + 1, b + 2, b, b + 2, b + 3};
    std::memcpy(&indices[size_t(q) * 6], i, sizeof(i));
  }
  Payload p{};
  p.features = features;
  p.blend = uint32_t(desc.blend);
  p.compare = uint32_t(gx.depthCompare ? to_compare(gx.depthFunc) : wgpu::CompareFunction::Always);
  p.depthWrite = gx.depthCompare && gx.depthUpdate ? 1 : 0;
  p.quadCount = quadCount;
  p.verts = push_verts(reinterpret_cast<const uint8_t*>(verts), size_t(vertexCount) * sizeof(Vertex), 4);
  p.indices = push_indices(reinterpret_cast<const uint8_t*>(indices.data()), indices.size() * sizeof(uint32_t), 4);
  p.uniform = push_uniform(reinterpret_cast<const uint8_t*>(&u), sizeof(u));
  if (overflowed(p.verts) || overflowed(p.indices) || overflowed(p.uniform) || p.verts.size == 0 ||
      p.indices.size == 0 || p.uniform.size == 0) {
    return;
  }
  for (int i = 0; i < 2; ++i) {
    p.arrayId[i] = ids[i];
    p.wrapS[i] = uint8_t(slots[i].wrapS);
    p.wrapT[i] = uint8_t(slots[i].wrapT);
    p.linear[i] = slots[i].linear ? 1 : 0;
  }
  push_custom_draw(g_state.drawType, &p, sizeof(p));
}

void shutdown() {
  g_state.pipelines.clear();
  g_state.groups.clear();
  g_state.dummyView = {};
  g_state.dummyTexture = {};
  g_state.bindLayout = {};
  g_state.pipelineLayout = {};
  const auto task = g_state.task;
  const auto draw = g_state.drawType;
  {
    const std::lock_guard lock{g_state.mutex};
    g_state.byKey.clear();
    g_state.byId.clear();
  }
  g_state.task = InvalidEncoderTask;
  g_state.drawType = InvalidDrawType;
  if (task != InvalidEncoderTask) {
    unregister_encoder_task_type(task);
  }
  if (draw != InvalidDrawType) {
    unregister_draw_type(draw);
  }
}
} // namespace aurora::gfx::vfx
