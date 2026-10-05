#pragma once

#include <cstdint>

// Port extension: Remastered's volumetric fog (CRenderPass_VolumetricFog), run on the finished
// EFB between two passes (see GXPortVolumetricFog).
namespace aurora::gfx::volfog {
constexpr uint32_t LutSize = 64;

// The task's uniform, word for word. View space is GX's: x right, y up, z towards the camera.
struct Params {
  float viewToWorld[3][4];   // rows: world = row . (view, 1)
  float worldToVolume[3][4]; // rows of world -> the ambient volume's texture coordinates
  float frustum[4];          // left, right, bottom, top at a view depth of 1
  float depth[4];            // near, far, the depth range the world draws in (GX z, min, max)
  float fog[4];              // range (the fog's far), scatter, absorb, density
  float shape[4];            // height slope, height bias (over world z), noise frequency, noise strength
  float noise[4];            // xyz: the noise's offset (wind), w: the light's largest channel
  float colorB[4];           // the light's multiplier; w: the volume's level (0: no volume light)
  float colorA[4];           // the light added; w: the exposure the EFB was drawn at
  float tone[3][4];          // the tone curve the EFB was drawn through (as GXSetPBRTone)
  float lut[LutSize];        // density over distance: entry i at (i / 63)^2 * range
  uint32_t volume;           // the ambient volume (probe::create_volume id), 0 for none
  uint32_t flags;
  uint32_t grid[2];          // set by the task: the froxel grid's width and height
};
static_assert(sizeof(Params) == 132 * 4);

// Registers the fog's encoder task (game thread); false if it could not be.
bool ensure_task();
// Records the fog from the FIFO processor (GX_AURORA_PORT_VOLUMETRIC_FOG), once ensure_task has
// returned true.
void record(const Params& params);
void shutdown();
} // namespace aurora::gfx::volfog
