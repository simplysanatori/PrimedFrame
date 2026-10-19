#pragma once

#include <cstdint>

#include <dolphin/gx.h>

// Port extension: a native draw for Remastered's VFX particle materials. The shader is not the
// GX TEV pipeline: it is one composable shader (build/mpr/vfx/SPEC.md section 2, spec2/{a,b}.md
// "Feature model") that computes colour and opacity from up to four textures, a ramp (or two), an
// indirect warp, a threshold mask, a fresnel/fade factor and an erosion, then tone maps and fogs
// the result like the PBR path does.
//
// Order of operations: uv chain (Indirect warps the slots flagged `warped`) -> texture reads
// (Ramp/DualMod, ColorIndexing or ColorTex) -> alpha factors (OpacityTex, Thresholding,
// OpacityFresnel, OpacityFade) -> Erosion (max(0, a - e), the only subtraction) -> x vc.w ->
// colour x vc.rgb x modulate (AddColor: x row.w + row.xyz).
namespace aurora::gfx::vfx {
enum Feature : uint32_t {
  ColorTex = 1,         // colorSlot is the colour map (BCLR): rgb = t.rgb, a = t.w
  OpacityTex = 2,       // opacitySlot: a *= t.x (with no other colour source, rgb is vc only)
  Erosion = 4,          // a = max(0, a - erosion)
  Ramp = 8,             // rampSlot: t3 = r.x^3, rgb = mix(c0, c1, t3), a = r.y * mix(c0.w, c1.w, t3)
  Indirect = 16,        // indirectSlot: w = d.xy * 0.99609375 - 0.5; a `warped` slot's uv += w * warpScale
  DepthSoften = 32,     // fades the alpha where the particle nears the opaque scene's depth (capture_scene)
  Thresholding = 64,    // thresholdSlot (xy), thrX/thrY, thrW: a *= S(t1) * S(t2)
  DualMod = 128,        // with Ramp: ramp2Slot too, t3 = (r.x * r2.x)^3, a = r.y * r2.y * mix(c0.w, c1.w, t3)
  OpacityFresnel = 256, // a *= S(sat((|vec.z|/|vec| - F.x) / (F.y - F.x)))
  ColorIndexing = 512,  // colorSlot = lookup (x index, y alpha), paletteSlot read at (i.x*s + o, z), layer 0
  AddColor = 1024,      // extra[addRow] = (add.rgb, scale): rgb = rgb * scale + add
  OpacityFade = 2048,   // a *= S(sat(-p.x / (p.y - p.x))), p = fadeX/fadeY
  ColorRgbOnly = 4096,  // with ColorTex: the map's alpha is not opacity (a stays 1)
  Pyro = 8192,          // VFX_Pyro: colorSlot = P, rampSlot = gradient LUT read at (P.r, 0), (P.g, 0); e = extra[1]:
                        // rgb = P.b * (P.r*e.x*G(P.r) + P.g*e.y*G(P.g)) / exposure, a = S(sat((P.a - e.w)/(1 - e.w))) * e.z
  Disintegration = 16384, // VFX_Disintegration: colorSlot = mask, s = t.x^2, extra[0..2] = A, B, edge E, extra[3] = (thr,
                          // edge width, alpha lo, alpha hi): m = sat(s - thr), q = sat(1 - m - width), e = q - q^2,
                          // rgb = mix(A, B, s) * vc.rgb + e * E, a = sat(m * (e > 0 ? hi : lo))
  SceneWarp = 32768,    // VFX_FrameBuffer_Indirect_*: rgb *= the scene copy (capture_scene) at the fragment, offset by
                        // the indirectSlot map d: uv = frag / size + s * k * (d.x * 0.99609375 - 0.5, 0.5 - d.y * 0.99609375),
                        // k = the indirect slot's warpScale (CCH0.xy), s = near / view depth (per vertex)
  VisorSplat = 65536,   // VFX_VisorSplat: the indirectSlot map read at the world position (extra[2].xy) x PMTR row 1.xy
                        // + row 0.zw gives w = (d * 0.99609375 - 0.5) * row 0.xy, a view-space offset taken to the quad's
                        // own axes by extra[3] = the inverse basis's rows 0, 1 (xy each): t = colorSlot at uv + that,
                        // rgb = t.rgb * vc.rgb, a = max(0, t.w + row 1.z - 1). The runtime fills extra[2..3] (sprites)
  // VFX_IceChargeBeam (9e0605cd, perms 000_1/000_0): a model particle whose CPU feed (CPortVfxMeshBatch::Add) has
  // turned and shaken the mesh by the shader's hash and filled tan. colorSlot = BCLR, normalSlot = NMAP, params =
  // CCH0..2, uv[1].x = the mesh's COLOR.z, extra rows 0..2 = PMTR A, B, C. Unfogged (DFOG 1: perm 000).
  IceChargeBeam = 1u << 20,
  // Not a shader feature: a VMAT blob's exposure policy, carried in its features word. The loader moves
  // it to DrawDesc::alreadyExposed; a draw ignores it in `features` (no shader or pipeline key sees it).
  AlreadyExposed = 1u << 31
};
// Multiply is port-only: the stand-in for the FrameBuffer_* shaders without a SceneWarp recipe
// (3db95827). The warp is dropped and the target is multiplied by mix(1, rgb, a) instead.
// AdditiveOne is the wave orb's model blend: RGB One/One, alpha Zero/One, drawn with depth write off.
// The values are serialised in VMAT blobs (0..4 older, 5 new) and form part of the pipeline key.
enum class Blend : uint32_t { Alpha = 0, Premultiplied = 1, Additive = 2, Opaque = 3, Multiply = 4, AdditiveOne = 5 };

// The highest Blend value a VMAT blob may carry.
inline constexpr uint32_t MaxBlend = uint32_t(Blend::AdditiveOne);

// A model draw of this blend never writes depth and always depth-tests (LEqual), whatever its generator asked for.
inline constexpr bool forces_depth_write_off(Blend blend) { return blend == Blend::AdditiveOne; }

// The depth state of a model draw: the generator's own (test = zTest, write = !AAPH && ZBUF) for every blend
// but AdditiveOne, which tests always and never writes.
struct DepthSelection {
  bool compare;
  bool write;
  constexpr bool operator==(const DepthSelection& o) const { return compare == o.compare && write == o.write; }
};
inline constexpr DepthSelection select_model_depth(Blend blend, bool zTest, bool aaph, bool zbuf) {
  if (forces_depth_write_off(blend))
    return {true, false};
  return {zTest, !aaph && zbuf};
}

struct Vertex {
  float pos[3];      // PNMTX0 space
  float uv[3][3];    // (u, v, layer) per UV set
  float color[4];    // rgb is HDR and unclamped
  float extra[4][4]; // PMTR rows 0..3
  float vec[3];      // the vector whose |z|/|v| feeds the fresnel term (view-space quad normal for sprites; the runtime fills it)
  // IceCharge*: the camera-local tangent in vec's (x, z, y) order (the GX view tangent is (x, y, -z)), w the
  // handedness with the model matrix's mirror folded in. Unused (zero) elsewhere.
  float tan[4];
};
static_assert(sizeof(Vertex) == 156);

struct Texture {
  const GXTexObj* obj = nullptr;
  uint32_t uvSet = 0;       // 0..2: which Vertex.uv set
  uint32_t cols = 1, rows = 1, layers = 1; // layers > 1: the texture is a cols x rows atlas
  GXTexWrapMode wrapS = GX_CLAMP, wrapT = GX_CLAMP;
  bool linear = true;
  bool warped = false;      // the Indirect warp distorts this slot's uv
  float warpScale[2] = {};  // the warp's scale for this slot (from params0: .xy, or .zw for the second ramp)
};

// A per-particle scalar: row >= 0 reads Vertex.extra[row][comp], row = -1 uses value.
struct Src {
  int8_t row = -1;
  int8_t comp = 0;
  float value = 0;
};

struct DrawDesc {
  uint32_t features = 0;
  Blend blend = Blend::Alpha;
  Texture tex[4];
  // Which tex[] a feature reads, -1 = none. colorSlot is also the ColorIndexing lookup.
  int8_t colorSlot = -1, opacitySlot = -1, rampSlot = -1, ramp2Slot = -1, thresholdSlot = -1, indirectSlot = -1,
         paletteSlot = -1;
  float modulate = 1.f;
  float depthSoften = 0.f; // DepthSoften's softness (SFTD): the view-space distance the fade spans; <= 0: none
  float opacityGain = 1.f, opacityBias = 0.f; // gain * clamp(smooth3(f) - bias, 0, 1)
  int8_t rampRow[2] = {0, 1}; // extra[] rows read whole as the ramp colours c0, c1
  int8_t addRow = 0;          // AddColor: the extra[] row (add.rgb, scale)
  Src erosion;
  Src thrX, thrY, thrW;       // Thresholding: p, q and the softness w
  Src fresnelX, fresnelY;     // OpacityFresnel: F.x, F.y
  Src fadeX, fadeY;           // OpacityFade: p.x, p.y
  Src indexScale, indexOffset, indexRow; // ColorIndexing: palette coordinate (i.x*s + o, z)
  // The colour is already exposed light: into the HDR scene it is written at exposure 1 instead of the
  // room's (the GX tone row's exposure, as every other draw takes it). An LDR draw ignores it.
  // Set only for the material the converter validated (the wave orb's MATI), not for VFX in general.
  bool alreadyExposed = false;
  // IceCharge*: the NMAP slot, Remastered's CCHn uniforms (MATI with the SMTR overrides) and its modular
  // simulation time (CScene::GetModularSimulationTime, the port's CGraphics::GetSimTime).
  int8_t normalSlot = -1;
  float params[7][4] = {};
  float time = 0.f;
};

// Records quadCount quads (4 vertices each, drawn as two triangles) into the current pass with the
// projection, PNMTX0, depth state, fog and tone curve the GX state holds at this call. Waits for the
// FIFO to be processed first. The first use of a texture layout builds its array texture in an
// encoder task, and draws that need one that isn't built yet are skipped until it is.
void draw_quads(const DrawDesc& desc, const Vertex* verts, uint32_t quadCount);
// The same for triangles: 3 vertices each, drawn in order (no index sharing). Large batches are split
// into draws of MaxTrianglesPerDraw (a draw holds at most 65536 vertices; this is a multiple of 3).
constexpr uint32_t MaxTrianglesPerDraw = (1u << 14) * 4 / 3;
void draw_triangles(const DrawDesc& desc, const Vertex* verts, uint32_t triCount);
// Remastered's CopyOpaqueColorBuffer point, between the opaque and the sorted geometry: copies the
// EFB's depth (raw, full size) and its colour (half size with a mip chain; linear RGBA16F in the HDR
// scene, else the RGBA8 display copy) for the later draws of this frame to read, at one pass break.
// Call once per frame from the game thread. Draws of a frame without a copy, before it, or into
// another target (an offscreen pass) bind 1x1 dummies, do no DepthSoften and draw no SceneWarp.
void capture_scene();
void shutdown();
} // namespace aurora::gfx::vfx
