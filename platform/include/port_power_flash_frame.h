#ifndef PORT_POWER_FLASH_FRAME_H
#define PORT_POWER_FLASH_FRAME_H

#ifdef TARGET_PC
#include "Kyoto/Math/CTransform4f.hpp"

namespace NPortPowerFlashFrame {
// PortInitFlashes: Power charged-release, not CGunWeapon's charge-effect slot 1.
inline constexpr int kPowerChargedFlashSlot = 3;

// Native flash vectors use X left, Y up, Z forward; port locators use X right,
// Y forward, Z up. Right-compose B(x,y,z)=(-x,z,y), preserving sampled T exactly.
// This maps source axes only; it does not retarget the native animation/pose.
inline CTransform4f ConvertedFrame(const CTransform4f& locator) {
  return CTransform4f(-locator.Get00(), locator.Get02(), locator.Get01(), locator.Get03(),
                      -locator.Get10(), locator.Get12(), locator.Get11(), locator.Get13(),
                      -locator.Get20(), locator.Get22(), locator.Get21(), locator.Get23());
}

template < typename Generator >
void SetGlobalLocator(Generator& generator, const CTransform4f& locator, int slot) {
  if (slot == kPowerChargedFlashSlot && generator.PortIsRemastered()) {
    generator.SetGlobalOrientAndTrans(ConvertedFrame(locator));
  } else {
    generator.SetGlobalOrientAndTrans(locator);
  }
}
} // namespace NPortPowerFlashFrame
#endif

#endif
