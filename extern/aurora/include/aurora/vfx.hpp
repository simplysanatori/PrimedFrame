#pragma once

#include <cstdint>

#include <dolphin/gx.h>

// Port extension: a native draw for Remastered's VFX particle materials. The shader is not the
// GX TEV pipeline: it computes colour and opacity from up to two textures, a ramp, an erosion
// threshold and an indirect warp, then tone maps and fogs the result like the PBR path does.
namespace aurora::gfx::vfx {
enum Feature : uint32_t {
  ColorTex = 1,    // slot 0 is the colour map (BCLR)
  OpacityTex = 2,  // slot 1 is the opacity map (slot 0 if there is no colour map and slot 0 is empty)
  Erosion = 4,
  Ramp = 8,        // slot 0 is the ramp
  Indirect = 16,   // slot 1 is the warp map
  DepthSoften = 32 // accepted and ignored: the scene depth is the pass's attachment and can't be sampled
};
enum class Blend : uint32_t { Alpha = 0, Premultiplied = 1, Additive = 2, Opaque = 3 };

struct Vertex {
  float pos[3];      // PNMTX0 space
  float uv[2][3];    // (u, v, layer) per UV set
  float color[4];    // rgb is HDR and unclamped
  float extra[3][4]; // PMTR rows 0..2
};
static_assert(sizeof(Vertex) == 100);

struct Texture {
  const GXTexObj* obj = nullptr;
  uint32_t uvSet = 0;
  uint32_t cols = 1, rows = 1, layers = 1; // layers > 1: the texture is a cols x rows atlas
  GXTexWrapMode wrapS = GX_CLAMP, wrapT = GX_CLAMP;
  bool linear = true;
};

struct DrawDesc {
  uint32_t features = 0;
  Blend blend = Blend::Alpha;
  Texture tex[2];
  float params0[4] = {};
  float modulate = 1.f;
  float depthSoften = 0.f;
  int rampRow[2] = {0, 1};
  int erosionRow = 0, erosionComp = 0;
};

// Records quadCount quads (4 vertices each, drawn as two triangles) into the current pass with the
// projection, PNMTX0, depth state, fog and tone curve the GX state holds at this call. Waits for the
// FIFO to be processed first. The first use of a texture layout builds its array texture in an
// encoder task, and draws that need one that isn't built yet are skipped until it is.
void draw_quads(const DrawDesc& desc, const Vertex* verts, uint32_t quadCount);
void shutdown();
} // namespace aurora::gfx::vfx
