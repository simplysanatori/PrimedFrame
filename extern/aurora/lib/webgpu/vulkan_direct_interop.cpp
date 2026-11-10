// SPDX-License-Identifier: GPL-3.0-or-later
#include <aurora/vulkan_direct_interop.h>

#include "../internal.hpp"
#include "../stereo.hpp"
#include "../stereo_overlay.hpp"
#include "gpu.hpp"

// PrimedGun's patched Dawn (quest/dawn) adds the entry points this bridge calls
// (quest/dawn/aurora_vulkan_interop.inc). The Quest links its Dawn statically, built with them and
// with Vulkan multiview. On the PC (AURORA_VULKAN_DIRECT_DESKTOP, aurora_core.cmake) a stock Dawn
// must still start, with only this bridge reporting itself unavailable: Windows looks them up in
// webgpu_dawn.dll at run time, and Linux, whose Dawn is a static libwebgpu_dawn.a, refers to them
// weakly (they live in VulkanBackend.cpp's object, which the Vulkan backend links in anyway).
// Elsewhere the stubs below.
#if defined(WEBGPU_DAWN) && ((defined(__ANDROID__) && defined(WGPU_DAWN_RENDER_PASS_MULTIVIEW_INIT)) ||          \
                             ((defined(_WIN32) || defined(__linux__)) && defined(AURORA_VULKAN_DIRECT_DESKTOP)))

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <vulkan/vulkan_core.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <type_traits>
#include <vector>

#if !defined(_WIN32)
#if defined(__ANDROID__)
#define AURORA_DAWN_VULKAN_IMPORT
#else
// Null when the Dawn linked in is a stock one (see above).
#define AURORA_DAWN_VULKAN_IMPORT __attribute__((weak))
#endif
extern "C" {
AURORA_DAWN_VULKAN_IMPORT uint32_t AuroraDawnVulkanVersion();
AURORA_DAWN_VULKAN_IMPORT int AuroraDawnVulkanConfigure(const AuroraDawnVulkanHooks* hooks);
AURORA_DAWN_VULKAN_IMPORT int AuroraDawnVulkanGetHandles(void* device, AuroraDawnVulkanHandles* out);
AURORA_DAWN_VULKAN_IMPORT void* AuroraDawnVulkanWrap(void* device, const void* textureDescriptor, uint64_t image);
AURORA_DAWN_VULKAN_IMPORT int AuroraDawnVulkanRelease(void* device, void* const* textures, uint32_t count);
AURORA_DAWN_VULKAN_IMPORT void* AuroraDawnVulkanLock(void* device);
AURORA_DAWN_VULKAN_IMPORT void AuroraDawnVulkanUnlock(void* guard);
AURORA_DAWN_VULKAN_IMPORT int AuroraDawnVulkanDrain(void* device);
}
#undef AURORA_DAWN_VULKAN_IMPORT
#endif

namespace aurora::vulkan_direct {
namespace {

Module Log("aurora::vulkan_direct");

// The patched Dawn's entry points (aurora/dawn_vulkan_abi.h), all null when the Dawn in use lacks
// any of them.
struct DawnAbi {
  AuroraDawnVulkanVersionFn version = nullptr;
  AuroraDawnVulkanConfigureFn configure = nullptr;
  AuroraDawnVulkanHandlesFn handles = nullptr;
  AuroraDawnVulkanWrapFn wrap = nullptr;
  AuroraDawnVulkanReleaseFn release = nullptr;
  AuroraDawnVulkanLockFn lock = nullptr;
  AuroraDawnVulkanUnlockFn unlock = nullptr;
  AuroraDawnVulkanDrainFn drain = nullptr;
};

const DawnAbi& dawn() noexcept {
  static const DawnAbi abi = [] {
    DawnAbi entries{};
#if defined(_WIN32)
    const HMODULE module = GetModuleHandleW(L"webgpu_dawn.dll");
    if (module == nullptr) {
      return entries;
    }
    const auto load = [module](auto& entry, const char* name) {
      entry = reinterpret_cast<std::remove_reference_t<decltype(entry)>>(
          reinterpret_cast<void*>(GetProcAddress(module, name)));
      return entry != nullptr;
    };
    if (!load(entries.version, "AuroraDawnVulkanVersion") || !load(entries.configure, "AuroraDawnVulkanConfigure") ||
        !load(entries.handles, "AuroraDawnVulkanGetHandles") || !load(entries.wrap, "AuroraDawnVulkanWrap") ||
        !load(entries.release, "AuroraDawnVulkanRelease") || !load(entries.lock, "AuroraDawnVulkanLock") ||
        !load(entries.unlock, "AuroraDawnVulkanUnlock") || !load(entries.drain, "AuroraDawnVulkanDrain")) {
      return DawnAbi{};
    }
#else
    entries = {&AuroraDawnVulkanVersion, &AuroraDawnVulkanConfigure, &AuroraDawnVulkanGetHandles,
               &AuroraDawnVulkanWrap,    &AuroraDawnVulkanRelease,   &AuroraDawnVulkanLock,
               &AuroraDawnVulkanUnlock,  &AuroraDawnVulkanDrain};
    // A weak reference a stock Dawn leaves unresolved: all or nothing, as on Windows.
    if (!entries.version || !entries.configure || !entries.handles || !entries.wrap || !entries.release ||
        !entries.lock || !entries.unlock || !entries.drain) {
      return DawnAbi{};
    }
#endif
    return entries;
  }();
  return abi;
}

int64_t to_vk_format(wgpu::TextureFormat format) noexcept {
  switch (format) {
  case wgpu::TextureFormat::RGBA8Unorm:
    return VK_FORMAT_R8G8B8A8_UNORM;
  case wgpu::TextureFormat::RGBA8UnormSrgb:
    return VK_FORMAT_R8G8B8A8_SRGB;
  case wgpu::TextureFormat::BGRA8Unorm:
    return VK_FORMAT_B8G8R8A8_UNORM;
  case wgpu::TextureFormat::BGRA8UnormSrgb:
    return VK_FORMAT_B8G8R8A8_SRGB;
  case wgpu::TextureFormat::RGBA16Float:
    return VK_FORMAT_R16G16B16A16_SFLOAT;
  default:
    return VK_FORMAT_UNDEFINED;
  }
}

// CopyTextureToTexture needs the two formats to match modulo sRGB encoding (the swapchain is
// declared sRGB so the compositor decodes Aurora's gamma-encoded bytes).
int copy_family(int64_t format) noexcept {
  switch (format) {
  case VK_FORMAT_R8G8B8A8_UNORM:
  case VK_FORMAT_R8G8B8A8_SRGB:
    return 1;
  case VK_FORMAT_B8G8R8A8_UNORM:
  case VK_FORMAT_B8G8R8A8_SRGB:
    return 2;
  case VK_FORMAT_R16G16B16A16_SFLOAT:
    return 3;
  default:
    return 0;
  }
}

bool same_copy_family(int64_t left, int64_t right) noexcept {
  const int family = copy_family(left);
  return family != 0 && family == copy_family(right);
}

wgpu::TextureFormat to_wgpu_format(int64_t format) noexcept {
  switch (format) {
  case VK_FORMAT_R8G8B8A8_UNORM:
    return wgpu::TextureFormat::RGBA8Unorm;
  case VK_FORMAT_R8G8B8A8_SRGB:
    return wgpu::TextureFormat::RGBA8UnormSrgb;
  case VK_FORMAT_B8G8R8A8_UNORM:
    return wgpu::TextureFormat::BGRA8Unorm;
  case VK_FORMAT_B8G8R8A8_SRGB:
    return wgpu::TextureFormat::BGRA8UnormSrgb;
  case VK_FORMAT_R16G16B16A16_SFLOAT:
    return wgpu::TextureFormat::RGBA16Float;
  default:
    return wgpu::TextureFormat::Undefined;
  }
}

// Draws eyes into a swapchain image: a triangle covering each target's region, each pixel loaded
// from its eye. On the Quest's Adreno a transfer copy into the runtime's images took about 2.6 ms a
// frame for both eyes, the draw about 0.4 ms. Several regions of one image (both eyes packed side by
// side, PF2_STEREO_PACKED) are drawn in one pass, which clears the image once.
struct BlitRegion {
  const stereo::EyeImage* source = nullptr;
  uint32_t x = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  // The target's index (its parameter buffer, and the eye a stamp names), and its stamp (0: none).
  uint32_t slot = 0;
  uint64_t stamp = 0;
};

class Blitter final {
public:
  static constexpr uint32_t kSlots = AURORA_STEREO_EYE_COUNT + 1;

  bool Encode(wgpu::CommandEncoder& encoder, const BlitRegion* regions, uint32_t count,
              const wgpu::TextureView& target, wgpu::TextureFormat format) noexcept {
    if (count == 0 || !Initialize()) {
      return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
      const auto* source = regions[i].source;
      if (source == nullptr || source->view == nullptr || !*source->view || regions[i].slot >= kSlots) {
        return false;
      }
    }
    const wgpu::RenderPipeline& pipeline = PipelineFor(format);
    if (!pipeline) {
      return false;
    }
    std::array<wgpu::BindGroup, kSlots> bindGroups{};
    for (uint32_t i = 0; i < count; ++i) {
      const BlitRegion& region = regions[i];
      // The region's origin in the image, and the stamp: (x, y, stamp low, stamp high, eye, stamp on).
      const std::array<uint32_t, 8> params{region.x,
                                           0u,
                                           static_cast<uint32_t>(region.stamp & 0xffffffffu),
                                           static_cast<uint32_t>(region.stamp >> 32),
                                           region.slot,
                                           region.stamp != 0 ? 1u : 0u,
                                           0u,
                                           0u};
      webgpu::g_queue.WriteBuffer(m_params[region.slot], 0, params.data(), sizeof(params));
      const std::array<wgpu::BindGroupEntry, 2> entries{
          wgpu::BindGroupEntry{.binding = 0, .textureView = *region.source->view},
          wgpu::BindGroupEntry{.binding = 1, .buffer = m_params[region.slot], .offset = 0, .size = kParamsSize},
      };
      const wgpu::BindGroupDescriptor bindGroupDescriptor{
          .label = "OpenXR eye blit",
          .layout = m_layout,
          .entryCount = entries.size(),
          .entries = entries.data(),
      };
      bindGroups[i] = webgpu::g_device.CreateBindGroup(&bindGroupDescriptor);
    }
    const wgpu::RenderPassColorAttachment attachment{
        .view = target,
        .loadOp = wgpu::LoadOp::Clear,
        .storeOp = wgpu::StoreOp::Store,
        .clearValue = {0.0, 0.0, 0.0, 0.0},
    };
    const wgpu::RenderPassDescriptor passDescriptor{
        .label = "OpenXR eye blit",
        .colorAttachmentCount = 1,
        .colorAttachments = &attachment,
    };
    const auto pass = encoder.BeginRenderPass(&passDescriptor);
    pass.SetPipeline(pipeline);
    for (uint32_t i = 0; i < count; ++i) {
      const BlitRegion& region = regions[i];
      pass.SetViewport(static_cast<float>(region.x), 0.0f, static_cast<float>(region.width),
                       static_cast<float>(region.height), 0.0f, 1.0f);
      pass.SetScissorRect(region.x, 0, region.width, region.height);
      pass.SetBindGroup(0, bindGroups[i]);
      pass.Draw(3);
    }
    pass.End();
    return true;
  }

private:
  static constexpr uint64_t kParamsSize = 32;

  bool Initialize() noexcept {
    if (m_layout) {
      return true;
    }
    static constexpr char kShader[] = R"(
struct Params {
    origin: vec2u,
    stamp_lo: u32,
    stamp_hi: u32,
    eye_index: u32,
    stamp_on: u32,
}

@group(0) @binding(0) var eye: texture_2d<f32>;
@group(0) @binding(1) var<uniform> params: Params;

@vertex
fn vs_main(@builtin(vertex_index) index: u32) -> @builtin(position) vec4f {
    let uv = vec2f(f32((index << 1u) & 2u), f32(index & 2u));
    return vec4f(uv * vec2f(2.0, -2.0) + vec2f(-1.0, 1.0), 0.0, 1.0);
}

// PF2_FRAME_STAMP (tests): rows 0 and 1 the 64-bit stamp and its complement, row 2 the eye, its
// complement and the marker 1010; the rest of the 64x15 block black.
fn stamp_bit(p: vec2u) -> f32 {
    var bit = 0u;
    if (p.y <= 1u) {
        let word = select(params.stamp_hi, params.stamp_lo, p.x < 32u);
        bit = (word >> (p.x & 31u)) & 1u;
        if (p.y == 1u) {
            bit = 1u - bit;
        }
    } else if (p.y == 2u) {
        if (p.x == 0u) {
            bit = params.eye_index & 1u;
        } else if (p.x == 1u) {
            bit = 1u - (params.eye_index & 1u);
        } else if (p.x == 2u || p.x == 4u) {
            bit = 1u;
        }
    }
    return f32(bit);
}

fn eye_texel(position: vec4f) -> vec4f {
    let p = vec2u(position.xy) - params.origin;
    if (params.stamp_on != 0u && p.x < 64u && p.y < 15u) {
        let v = stamp_bit(p);
        return vec4f(v, v, v, 1.0);
    }
    return textureLoad(eye, vec2i(p), 0);
}

@fragment
fn fs_raw(@builtin(position) position: vec4f) -> @location(0) vec4f {
    return eye_texel(position);
}

// The eyes hold gamma-encoded bytes and an sRGB attachment encodes what it is given: decode first.
@fragment
fn fs_srgb(@builtin(position) position: vec4f) -> @location(0) vec4f {
    let color = eye_texel(position);
    let low = color.rgb / 12.92;
    let high = pow((color.rgb + 0.055) / 1.055, vec3f(2.4));
    return vec4f(select(high, low, color.rgb <= vec3f(0.04045)), color.a);
}
)";
    wgpu::ShaderSourceWGSL wgsl{};
    wgsl.code = kShader;
    const wgpu::ShaderModuleDescriptor moduleDescriptor{.nextInChain = &wgsl, .label = "OpenXR eye blit"};
    m_module = webgpu::g_device.CreateShaderModule(&moduleDescriptor);
    const std::array<wgpu::BindGroupLayoutEntry, 2> entries{
        wgpu::BindGroupLayoutEntry{
            .binding = 0,
            .visibility = wgpu::ShaderStage::Fragment,
            .texture =
                {
                    .sampleType = wgpu::TextureSampleType::UnfilterableFloat,
                    .viewDimension = wgpu::TextureViewDimension::e2D,
                },
        },
        wgpu::BindGroupLayoutEntry{
            .binding = 1,
            .visibility = wgpu::ShaderStage::Fragment,
            .buffer = {.type = wgpu::BufferBindingType::Uniform, .minBindingSize = kParamsSize},
        },
    };
    const wgpu::BindGroupLayoutDescriptor layoutDescriptor{
        .label = "OpenXR eye blit",
        .entryCount = entries.size(),
        .entries = entries.data(),
    };
    m_layout = webgpu::g_device.CreateBindGroupLayout(&layoutDescriptor);
    const wgpu::PipelineLayoutDescriptor pipelineLayoutDescriptor{
        .label = "OpenXR eye blit",
        .bindGroupLayoutCount = 1,
        .bindGroupLayouts = &m_layout,
    };
    m_pipelineLayout = webgpu::g_device.CreatePipelineLayout(&pipelineLayoutDescriptor);
    for (auto& buffer : m_params) {
      const wgpu::BufferDescriptor descriptor{
          .label = "OpenXR eye blit parameters",
          .usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
          .size = kParamsSize,
      };
      buffer = webgpu::g_device.CreateBuffer(&descriptor);
    }
    return m_module && m_layout && m_pipelineLayout &&
           std::all_of(m_params.begin(), m_params.end(), [](const wgpu::Buffer& buffer) { return bool(buffer); });
  }

  const wgpu::RenderPipeline& PipelineFor(wgpu::TextureFormat format) noexcept {
    for (const auto& [cached, pipeline] : m_pipelines) {
      if (cached == format) {
        return pipeline;
      }
    }
    const bool srgb =
        format == wgpu::TextureFormat::RGBA8UnormSrgb || format == wgpu::TextureFormat::BGRA8UnormSrgb;
    const wgpu::ColorTargetState target{.format = format};
    const wgpu::FragmentState fragment{
        .module = m_module,
        .entryPoint = srgb ? "fs_srgb" : "fs_raw",
        .targetCount = 1,
        .targets = &target,
    };
    const wgpu::RenderPipelineDescriptor descriptor{
        .label = "OpenXR eye blit",
        .layout = m_pipelineLayout,
        .vertex = {.module = m_module, .entryPoint = "vs_main"},
        .primitive = {.topology = wgpu::PrimitiveTopology::TriangleList},
        .multisample = {.count = 1},
        .fragment = &fragment,
    };
    m_pipelines.emplace_back(format, webgpu::g_device.CreateRenderPipeline(&descriptor));
    return m_pipelines.back().second;
  }

  wgpu::ShaderModule m_module;
  wgpu::BindGroupLayout m_layout;
  wgpu::PipelineLayout m_pipelineLayout;
  std::array<wgpu::Buffer, kSlots> m_params;
  std::vector<std::pair<wgpu::TextureFormat, wgpu::RenderPipeline>> m_pipelines;
};

bool abi_matches() noexcept {
  static const bool matches = dawn().version != nullptr && dawn().version() == AURORA_DAWN_VULKAN_ABI;
  return matches;
}

// A swapchain image wrapped as a Dawn texture of its own format, kept while its swapchain lives.
struct Wrap {
  uint64_t image = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
  wgpu::Texture texture;
  wgpu::TextureView view;
};

// The eyes, then the settings panel's layer image after them.
constexpr uint32_t kMaxImages = AURORA_STEREO_EYE_COUNT + 1;

// The width of the whole image a target is a region of.
uint32_t ImageWidth(const AuroraVulkanDirectTarget& target) noexcept {
  return target.imageWidth != 0 ? target.imageWidth : target.width;
}

class Bridge final {
public:
  Bridge(AuroraVulkanDirectSubmittedCallback callback, void* userdata) noexcept
  : m_callback(callback), m_userdata(userdata) {}

  bool SetTargets(uint64_t token, const AuroraVulkanDirectTarget* targets, uint32_t count,
                  const AuroraVulkanDirectTarget* panel) noexcept {
    if (token == 0 || targets == nullptr || count == 0 || count > AURORA_STEREO_EYE_COUNT) {
      return false;
    }
    const auto valid = [](const AuroraVulkanDirectTarget& target) {
      return target.image != 0 && target.width != 0 && target.height != 0 &&
             (target.imageWidth == 0 ? target.x == 0
                                     : static_cast<uint64_t>(target.x) + target.width <= target.imageWidth);
    };
    for (uint32_t i = 0; i < count; ++i) {
      if (!valid(targets[i])) {
        return false;
      }
    }
    if (panel != nullptr && !valid(*panel)) {
      return false;
    }
    std::lock_guard lock(m_mutex);
    if (m_token != 0) {
      return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
      m_targets[i] = targets[i];
    }
    m_panel = panel != nullptr;
    if (m_panel) {
      m_targets[count] = *panel;
    }
    m_count = count;
    m_images = count + (m_panel ? 1u : 0u);
    m_encoded = false;
    m_token = token;
    m_targetsSet.notify_all();
    return true;
  }

  // Only while the worker is not encoding this token (it holds the lock then) and has not.
  bool Cancel(uint64_t token) noexcept {
    std::unique_lock lock(m_mutex, std::try_to_lock);
    if (!lock.owns_lock() || m_encoded || m_token == 0 || m_token != token) {
      return false;
    }
    m_token = 0;
    m_cancelled = token;
    return true;
  }

  bool Forget(const uint64_t* images, uint32_t count) noexcept {
    {
      std::lock_guard lock(m_mutex);
      if (m_token != 0) {
        return false;
      }
    }
    // Every copy into the images retires before their textures go.
    if (!webgpu::g_device || dawn().drain(webgpu::g_device.Get()) == 0) {
      return false;
    }
    std::lock_guard lock(m_mutex);
    std::erase_if(m_wraps, [&](const Wrap& wrap) { return std::find(images, images + count, wrap.image) != images + count; });
    m_active = {};
    m_activeViews = {};
    return true;
  }

  void Clear() noexcept {
    std::lock_guard lock(m_mutex);
    m_token = 0;
    m_cancelled = 0;
    m_encoded = false;
    m_active = {};
    m_activeViews = {};
    m_wraps.clear();
  }

  bool Encode(wgpu::CommandEncoder& encoder, const stereo::SinkFrame& frame) noexcept {
    std::unique_lock lock(m_mutex);
    if (m_token == 0 && frame.frameToken != m_cancelled) {
      // Pipelined pacing sets a packet's targets once the previous packet's eyes were
      // submitted, which can be moments before its own encode reaches this point. Any
      // targets end the wait: another packet's mean this one's were withdrawn.
      m_targetsSet.wait_for(lock, std::chrono::milliseconds(20), [&] { return m_token != 0; });
    }
    if (m_token == 0 || m_encoded || frame.frameToken != m_token) {
      return false;
    }
    std::array<stereo::EyeImage, kMaxImages> sources{};
    for (uint32_t i = 0; i < m_count; ++i) {
      sources[i] = frame.eyes[i];
    }
    if (m_panel && !stereo_overlay::layer_source(encoder, m_targets[m_count].width, m_targets[m_count].height,
                                                 sources[m_count])) {
      return Decline("the settings panel's layer image is unavailable");
    }
    // Every target is wrapped before any copy is recorded.
    for (uint32_t i = 0; i < m_images; ++i) {
      const auto& source = sources[i];
      const auto& target = m_targets[i];
      if (source.texture == nullptr || !*source.texture) {
        return Decline("an eye image is missing");
      }
      if (!same_copy_family(to_vk_format(source.format), target.vkFormat)) {
        return Decline("an eye's format cannot be copied into its swapchain image");
      }
      const wgpu::TextureFormat format = to_wgpu_format(target.vkFormat);
      const uint32_t imageWidth = ImageWidth(target);
      auto it = std::find_if(m_wraps.begin(), m_wraps.end(),
                             [&](const Wrap& wrap) { return wrap.image == target.image; });
      if (it != m_wraps.end() && (it->width != imageWidth || it->height != target.height || it->format != format)) {
        // The runtime reuses VkImage handles across swapchain recreation: wrap the image again.
        m_wraps.erase(it);
        it = m_wraps.end();
      }
      if (it == m_wraps.end()) {
        wgpu::TextureDescriptor descriptor{};
        descriptor.label = "OpenXR swapchain image";
        descriptor.usage = wgpu::TextureUsage::CopyDst | wgpu::TextureUsage::RenderAttachment;
        descriptor.size = {imageWidth, target.height, 1};
        descriptor.format = format;
        void* wrapped = dawn().wrap(webgpu::g_device.Get(), &descriptor, target.image);
        if (wrapped == nullptr) {
          return Decline("Dawn could not wrap a swapchain image");
        }
        Wrap wrap{target.image, imageWidth, target.height, format,
                  wgpu::Texture::Acquire(static_cast<WGPUTexture>(wrapped)), {}};
        wrap.view = wrap.texture.CreateView();
        m_wraps.push_back(std::move(wrap));
        it = m_wraps.end() - 1;
      }
      m_active[i] = it->texture;
      m_activeViews[i] = it->view;
    }
    // Targets sharing an image (both eyes packed side by side) are drawn in one pass, in target order.
    std::array<bool, kMaxImages> drawn{};
    for (uint32_t first = 0; first < m_images; ++first) {
      if (drawn[first]) {
        continue;
      }
      std::array<BlitRegion, kMaxImages> regions{};
      std::array<uint32_t, kMaxImages> members{};
      uint32_t regionCount = 0;
      for (uint32_t i = first; i < m_images; ++i) {
        if (drawn[i] || m_targets[i].image != m_targets[first].image) {
          continue;
        }
        drawn[i] = true;
        // An eye rendered smaller than its region (the immersive window's) fills the top-left part
        // the layer shows; one rendered larger while the resolution changes gives what fits.
        regions[regionCount] = BlitRegion{
            .source = &sources[i],
            .x = m_targets[i].x,
            .width = std::min(sources[i].size.width, m_targets[i].width),
            .height = std::min(sources[i].size.height, m_targets[i].height),
            .slot = i,
            .stamp = m_targets[i].stamp,
        };
        members[regionCount++] = i;
      }
      if (m_blitter.Encode(encoder, regions.data(), regionCount, m_activeViews[first],
                           to_wgpu_format(m_targets[first].vkFormat))) {
        continue;
      }
      // The blit could not be set up: copy instead.
      for (uint32_t r = 0; r < regionCount; ++r) {
        const uint32_t i = members[r];
        const auto& source = sources[i];
        const wgpu::TexelCopyTextureInfo from{
            .texture = *source.texture,
            .mipLevel = 0,
            // Multiview eye targets hold both eyes as layers of one texture.
            .origin = {0, 0, source.layer},
            .aspect = wgpu::TextureAspect::All,
        };
        const wgpu::TexelCopyTextureInfo to{
            .texture = m_active[i],
            .mipLevel = 0,
            .origin = {m_targets[i].x, 0, 0},
            .aspect = wgpu::TextureAspect::All,
        };
        const wgpu::Extent3D extent{regions[r].width, regions[r].height, 1};
        encoder.CopyTextureToTexture(&from, &to, &extent);
      }
    }
    m_encoded = true;
    return true;
  }

  void Submitted(const stereo::SinkFrame& frame) noexcept {
    std::lock_guard lock(m_mutex);
    if (m_token == 0 || !m_encoded || m_token != frame.frameToken) {
      return;
    }
    // Each image once: two targets may share one (packed eyes).
    std::array<void*, kMaxImages> textures{};
    uint32_t textureCount = 0;
    for (uint32_t i = 0; i < m_images; ++i) {
      void* texture = m_active[i].Get();
      if (std::find(textures.begin(), textures.begin() + textureCount, texture) == textures.begin() + textureCount) {
        textures[textureCount++] = texture;
      }
    }
    // The colour-attachment layout OpenXR expects at xrReleaseSwapchainImage, on Dawn's queue,
    // flushed before the XR thread may release.
    const bool success = dawn().release(webgpu::g_device.Get(), textures.data(), textureCount) != 0;
    const uint64_t token = m_token;
    m_token = 0;
    m_encoded = false;
    m_active = {};
    m_activeViews = {};
    m_callback(token, success, m_userdata);
  }

private:
  // Leaves the token pending: no GPU work references the images, and the XR thread cancels the
  // frame when no completion arrives.
  bool Decline(const char* reason) noexcept {
    if ((m_declines++ % 120) == 0) {
      Log.warn("Direct stereo frame {} not copied: {} ({} so far)", m_token, reason, m_declines);
    }
    return false;
  }

  std::mutex m_mutex;
  // SetTargets wakes an Encode waiting for its packet's targets.
  std::condition_variable m_targetsSet;
  std::vector<Wrap> m_wraps;
  std::array<AuroraVulkanDirectTarget, kMaxImages> m_targets{};
  std::array<wgpu::Texture, kMaxImages> m_active{};
  std::array<wgpu::TextureView, kMaxImages> m_activeViews{};
  Blitter m_blitter;
  AuroraVulkanDirectSubmittedCallback m_callback = nullptr;
  void* m_userdata = nullptr;
  uint64_t m_token = 0;
  // The last token withdrawn before its encode: a frame already carrying it waits for nothing.
  uint64_t m_cancelled = 0;
  uint32_t m_count = 0;
  uint32_t m_images = 0;
  uint32_t m_declines = 0;
  bool m_panel = false;
  bool m_encoded = false;
};

std::unique_ptr<Bridge> g_bridge;

} // namespace
} // namespace aurora::vulkan_direct

bool aurora_vulkan_direct_available() { return aurora::vulkan_direct::abi_matches(); }

bool aurora_vulkan_direct_configure(const AuroraDawnVulkanHooks* hooks) {
  return aurora::vulkan_direct::abi_matches() && aurora::vulkan_direct::dawn().configure(hooks) != 0;
}

bool aurora_vulkan_direct_get_handles(AuroraDawnVulkanHandles* handles, int64_t* colorVkFormat) {
  using namespace aurora;
  if (handles == nullptr || colorVkFormat == nullptr) {
    return false;
  }
  *handles = {};
  *colorVkFormat = vulkan_direct::to_vk_format(webgpu::g_graphicsConfig.surfaceConfiguration.format);
  if (!vulkan_direct::abi_matches() || !webgpu::g_device || webgpu::g_backendType != wgpu::BackendType::Vulkan ||
      *colorVkFormat == VK_FORMAT_UNDEFINED) {
    return false;
  }
  return vulkan_direct::dawn().handles(webgpu::g_device.Get(), handles) != 0;
}

bool aurora_vulkan_direct_enable(AuroraVulkanDirectSubmittedCallback submitted, void* userdata) {
  using namespace aurora::vulkan_direct;
  if (g_bridge || submitted == nullptr || !abi_matches() || !aurora::webgpu::g_device) {
    return false;
  }
  g_bridge = std::make_unique<Bridge>(submitted, userdata);
  aurora::stereo::set_sink(
      [](wgpu::CommandEncoder& encoder, const aurora::stereo::SinkFrame& frame, void* self) noexcept {
        return static_cast<Bridge*>(self)->Encode(encoder, frame);
      },
      [](const aurora::stereo::SinkFrame& frame, void* self) noexcept { static_cast<Bridge*>(self)->Submitted(frame); },
      g_bridge.get());
  return true;
}

bool aurora_vulkan_direct_set_targets(uint64_t frameToken, const AuroraVulkanDirectTarget* targets,
                                      uint32_t targetCount, const AuroraVulkanDirectTarget* panel) {
  using namespace aurora::vulkan_direct;
  return g_bridge && g_bridge->SetTargets(frameToken, targets, targetCount, panel);
}

bool aurora_vulkan_direct_cancel(uint64_t frameToken) {
  using namespace aurora::vulkan_direct;
  return g_bridge && g_bridge->Cancel(frameToken);
}

bool aurora_vulkan_direct_forget_targets(const uint64_t* images, uint32_t count) {
  using namespace aurora::vulkan_direct;
  if (!g_bridge) {
    return true;
  }
  return images != nullptr && g_bridge->Forget(images, count);
}

bool aurora_vulkan_direct_disable() {
  using namespace aurora::vulkan_direct;
  if (!g_bridge) {
    return true;
  }
  aurora::stereo::set_sink(nullptr, nullptr, nullptr);
  if (aurora::webgpu::g_device && dawn().drain(aurora::webgpu::g_device.Get()) == 0) {
    (void)g_bridge.release();
    return false;
  }
  g_bridge->Clear();
  g_bridge.reset();
  return true;
}

void* aurora_vulkan_direct_lock_queue() {
  using namespace aurora;
  if (!vulkan_direct::abi_matches() || !webgpu::g_device) {
    return nullptr;
  }
  return vulkan_direct::dawn().lock(webgpu::g_device.Get());
}

void aurora_vulkan_direct_unlock_queue(void* guard) {
  if (guard != nullptr) {
    aurora::vulkan_direct::dawn().unlock(guard);
  }
}

#else

bool aurora_vulkan_direct_available() { return false; }
bool aurora_vulkan_direct_configure(const AuroraDawnVulkanHooks*) { return false; }
bool aurora_vulkan_direct_get_handles(AuroraDawnVulkanHandles* handles, int64_t* colorVkFormat) {
  if (handles != nullptr) {
    *handles = {};
  }
  if (colorVkFormat != nullptr) {
    *colorVkFormat = 0;
  }
  return false;
}
bool aurora_vulkan_direct_enable(AuroraVulkanDirectSubmittedCallback, void*) { return false; }
bool aurora_vulkan_direct_set_targets(uint64_t, const AuroraVulkanDirectTarget*, uint32_t,
                                      const AuroraVulkanDirectTarget*) {
  return false;
}
bool aurora_vulkan_direct_cancel(uint64_t) { return false; }
bool aurora_vulkan_direct_forget_targets(const uint64_t*, uint32_t) { return true; }
bool aurora_vulkan_direct_disable() { return true; }
void* aurora_vulkan_direct_lock_queue() { return nullptr; }
void aurora_vulkan_direct_unlock_queue(void*) {}

#endif
