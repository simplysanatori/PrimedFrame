#ifndef AURORA_GFX_H
#define AURORA_GFX_H

#ifdef __cplusplus
#include <cstdint>

extern "C" {
#else
#include "stdint.h"
#endif

#if !defined(NDEBUG) && !defined(AURORA_GFX_DEBUG_GROUPS)
#define AURORA_GFX_DEBUG_GROUPS
#endif

void push_debug_group(const char* label);
void pop_debug_group();

typedef struct {
  uint32_t queuedPipelines;
  uint32_t createdPipelines;
  uint32_t drawCallCount;
  uint32_t mergedDrawCallCount;
  uint32_t renderPassCount; // render passes the frame's EFB recording encoded
  uint32_t lastVertSize;
  uint32_t lastUniformSize;
  uint32_t lastIndexSize;
  uint32_t lastStorageSize;
  uint32_t lastTextureUploadSize;
} AuroraStats;

const AuroraStats* aurora_get_stats();
// Live GPU textures: [0] sampled only, [1] render targets/copies. Bytes are the mip chain's size.
typedef struct {
  uint32_t count[2];
  uint64_t bytes[2];
} AuroraTextureStats;
void aurora_get_texture_stats(AuroraTextureStats* out);
// The AuroraConfig::frameBufferScale in use: the device may allow less than was asked for.
uint32_t aurora_get_frame_buffer_scale();
// The MiB set aside for AuroraConfig::residentGeometryMiB: the device may allow less.
uint32_t aurora_get_resident_geometry_mib();
// The bytes of it in use, as of the last frame processed.
uint64_t aurora_get_resident_geometry_used();
float aurora_get_fps();

void aurora_enable_vsync(bool enabled);

#ifdef __cplusplus
}
#endif

#endif
