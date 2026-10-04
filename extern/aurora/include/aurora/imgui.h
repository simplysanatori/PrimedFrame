#ifndef AURORA_IMGUI_H
#define AURORA_IMGUI_H

#include <imgui.h>

#ifdef __cplusplus
#include <cstdint>

extern "C" {
#else
#include "stdint.h"
#endif

ImTextureID aurora_imgui_add_texture(uint32_t width, uint32_t height, const void* rgba8);
// Frees a texture from aurora_imgui_add_texture. Not while a frame that draws it is still in flight: call it
// the frame after the last one that used it.
void aurora_imgui_remove_texture(ImTextureID texture);

#ifdef __cplusplus
}
#endif

#endif
