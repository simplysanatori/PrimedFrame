#ifndef METROID_PRIME_PORT_PORT_SCENE_COPY_H
#define METROID_PRIME_PORT_PORT_SCENE_COPY_H

// The point of CStateManager::DrawWorld where Remastered copies the opaque scene
// (RenderPrimaryPass: CopyOpaqueDepthBuffer, CopyOpaqueColorBuffer): every visible area's
// opaque world and models, the sky and the decals have drawn, and no area's transparents or
// particles yet. The vfx draws after it read the copy (DepthSoften).
#include <aurora/vfx.hpp>

// A frame that shows only the headset's eyes skips it (PF2_SCENE_COPY_SKIP): only vfx draws, which
// the eyes do not replay, and a Phazon shell (port_shell.h) read it.
bool PortShellPending();
inline void PortSceneCopyPoint() {
  if (aurora::gfx::vfx::eyes_only_frame() && !PortShellPending()) {
    return;
  }
  aurora::gfx::vfx::capture_scene();
}

#endif
