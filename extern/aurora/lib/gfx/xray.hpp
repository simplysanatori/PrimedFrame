#pragma once

#include <cstdint>
#include <string>

#include <webgpu/webgpu_cpp.h>

// Port extension: Remastered's X-ray visor post passes (shader 00089f0, render_xray_effect), run on
// the finished EFB (see GXPortXRayPass).
namespace aurora::gfx::xray {
// The task's uniform, word for word.
struct Params {
  float tone[3][4];  // the tone curve the EFB was drawn through (as GXSetPBRTone); unused in HDR
  float p[8][4];     // cb0 p0..p7 of 00089f0; p0.z selects the pass (1: distortion, else ramp)
  float depth[4];    // x, y: the GX z range the world draws in (min, max); z: low-resolution target scale
};
static_assert(sizeof(Params) == 48 * 4);

// The scene the passes read and write (kb topic/visor-hdr-pipeline.md): the display-encoded EFB
// (Ldr), or the RGBA16F exposed linear HDR scene pass A's level is blended back into (Hdr). Pass B
// always runs after the post, so it is Ldr.
enum class Domain : uint32_t { Ldr, Hdr };
constexpr size_t DomainCount = 2;
// The task's WGSL for a domain and a sample count (the depth binding's type follows the count;
// exposed for the tests).
std::string source_for(Domain domain, uint32_t samples);

// Registers the task (game thread); false if it could not be.
bool ensure_task();
// Records a pass from the FIFO processor (GX_AURORA_PORT_XRAY), once ensure_task has returned true.
bool record(const Params& params);
void shutdown();
} // namespace aurora::gfx::xray
