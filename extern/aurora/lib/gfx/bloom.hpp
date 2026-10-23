#pragma once

#include <cstdint>

// Port extension: Remastered's frame bloom (CRenderPass_Bloom) and colour grade, run on the
// finished HDR scene as it is converted to the display EFB (see GXPortPostProcessScene).
namespace aurora::gfx::bloom {
constexpr uint32_t GradeLutSize = 33;

struct Params {
  float threshold;
  float tints[5][3];
  float tone[3][4];   // [0][3]: unused (0)
  uint32_t bloom;     // bit 0: the bloom (else grade only); the Cost bits below
  uint32_t gradeA;    // LUT ids from set_grade_lut; 0 is the identity
  uint32_t gradeB;
  float gradeWeight;  // 0 draws A, 1 draws B
  float add[4];       // the HDR composite's added term, linear r, g, b, a (a 0: none); see CompositeAddWgsl
};
static_assert(sizeof(Params) <= 144);
// Params::bloom bits for speed tests (GXSetPBRCostTest), which leave out one part of the work and
// look wrong: the pass after the bloom clearing depth instead of loading it. CostKeepDepth looks
// right: it always loads depth after the bloom, as before that pass learned to clear it when the HUD
// doesn't test against it.
constexpr uint32_t CostNoDepthReload = 8;
constexpr uint32_t CostKeepDepth = 16;

static_assert(sizeof(Params) % 4 == 0);

// The HDR composite's damage-tint term, Remastered's UberPost material 0007259 (the HUD camera-filter
// quad folded into the composite): the tonemapped + graded + bloomed frame clamped to [0, 1], plus the
// colour times its alpha (c clamped to [0, 1] first), all in linear light before the sRGB encode. The
// frame's alpha is kept by the caller. Shared with the composite's WGSL so a test can run the same code.
inline constexpr const char* CompositeAddWgsl = R"(
fn composite_add(frame: vec3f, c: vec4f) -> vec3f {
  let t = clamp(c, vec4f(0.0), vec4f(1.0));
  return clamp(frame, vec3f(0.0), vec3f(1.0)) + t.rgb * t.a;
}
)";

// Registers the bloom's encoder task (game thread); false if it could not be.
bool ensure_task();
// Read-only FIFO preflight for entering the HDR scene: the task and composite draw must still be
// registered, and this frame must have room for another post. Does not reserve a post slot.
bool scene_post_available();
// Records the bloom from the FIFO processor (GX_AURORA_PORT_POST_PROCESS), once ensure_task
// has returned true. In the HDR scene's pass it reads that scene, and its composite (always drawn:
// the scene's one tone, the grade, the bloom) is the first draw of the display EFB's pass, which the
// FIFO goes on recording in (the HDR scene is left). At most two posts a frame. Outside the HDR
// scene's pass there is no post: false, as whenever nothing was recorded.
bool record(const Params& params);
// Stores a 33^3 RGBA8 LUT (red fastest) under id (non-zero), uploaded before its next use.
void set_grade_lut(uint32_t id, const uint8_t* rgba);
// Records the auto-exposure measure of the HDR scene as it is now (Remastered's opaque colour copy:
// a pow2 half-size mip chain to 1x1, copied to a staging buffer). `exposure` is what the scene is
// drawn at. Once a frame; false outside the HDR scene's pass, for exposure <= 0, or when the task
// could not be registered. Waits for the FIFO first, and ends the current pass.
bool measure_scene(float exposure);
// Maps the measure readbacks that the last submit carried.
void after_submit() noexcept;
// The radiance (the 1x1 undone by its exposure) of the newest measure the GPU has finished, and
// a count that goes up with every new one; false before the first.
bool frame_radiance(float out[3], uint32_t& serial);
void shutdown();
} // namespace aurora::gfx::bloom
