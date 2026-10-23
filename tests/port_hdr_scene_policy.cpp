#include "MetroidPrime/CStateManager.hpp"

int main() {
  // Every visor draws inside the HDR scene (kb topic/visor-hdr-pipeline.md): thermal and X-ray push
  // their own reduced-size target over it and blend it back into it.
  if (!CStateManager::PortIsSceneHdrVisorSupported(true, false, false, false)) // Combat visor
    return 1;
  if (!CStateManager::PortIsSceneHdrVisorSupported(false, true, false, false)) // Scan visor
    return 2;
  if (!CStateManager::PortIsSceneHdrVisorSupported(false, false, true, false)) // Thermal visor
    return 3;
  if (!CStateManager::PortIsSceneHdrVisorSupported(false, false, false, true)) // X-ray visor
    return 4;
  if (CStateManager::PortIsSceneHdrVisorSupported(false, false, false, false)) // first person, no visor
    return 5;

  if (!CStateManager::PortShouldUseSceneHdr(true, true, true, true)) // enabled with Remastered tone
    return 6;
  if (CStateManager::PortShouldUseSceneHdr(false, true, true, true)) // MP_HDR_SCENE unset / 0
    return 7;
  if (CStateManager::PortShouldUseSceneHdr(true, false, false, true)) // room env mod disabled
    return 8;
  if (CStateManager::PortShouldUseSceneHdr(true, true, false, true)) // tone absent
    return 9;
  if (CStateManager::PortShouldUseSceneHdr(true, true, true, false)) // no supported visor
    return 10;
  // A visor transition no longer drops the scene: Remastered has no transition path, so nothing here
  // can express one (the parameter is gone rather than ignored).
}
