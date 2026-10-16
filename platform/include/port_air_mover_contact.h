#pragma once
// The airborne mover's contact decisions that do not need the game: 0xc6d460's working-normal
// selection and 0xf948d4's per-contact separation gate, so they can be regression-tested.
#include <cmath>

namespace PortAirMoverContact {

struct Vec3 {
  float x = 0.f, y = 0.f, z = 0.f;
};

// is_ceiling_normal (0xbecca0 / NCollisionUtils::is_ceiling_normal): up < -0.70710677.
inline bool IsCeilingNormal(Vec3 n) { return n.z < -0.70710677f; }

// The 0.866025 (0x3f5db3d0) up test of 0xc6d658-0xc6d67c.
inline bool UpExceeds(const Vec3& n) { return n.z > 0.866025f; }

// 0xc6d63c-0xc6d65c: the presence byte at contact+0x44 alone decides which normal the response
// uses. 0xc6d624-0xc6d630 copies the primary into the working slot; the presence path then stores
// the secondary over it, and neither 0xc6d69c nor 0xc6d6b4 restores the primary. A present but
// degenerate (zero) secondary stays the working normal: the callback never tests normalizability.
inline Vec3 WorkingNormal(Vec3 primary, bool hasSecondary, Vec3 secondary) {
  return hasSecondary ? secondary : primary;
}

// 0xc6d658-0xc6d698 -> 0xc6d6b4 (`mov w25,#1`) when no secondary is present, otherwise w25 = 1
// only while neither up exceeds 0.866025 and neither normal is a ceiling. w25 gates the
// kMS_ApplyJump gravity/force block at 0xc6d7f4 (`cbz w25`); it never changes the working normal.
inline bool W25(bool hasSecondary, Vec3 primary, Vec3 secondary) {
  if (!hasSecondary) {
    return true; // 0xc6d63c `cbz w8, 0xc6d6b4`
  }
  return !UpExceeds(primary) && !UpExceeds(secondary) && !IsCeilingNormal(primary) &&
         !IsCeilingNormal(secondary);
}

// 0xf94ff0-0xf95028: dot(displacement - riderOffset, working normal) > 0 skips the contact
// (`b.hi 0xf94f00`); otherwise it is responded to. The displacement is the tick's own motion and
// the normal is the working one, so a contact whose primary looks separating but whose secondary
// blocks is still responded to.
inline float SeparationDot(Vec3 displacement, Vec3 riderOffset, Vec3 working) {
  const float dx = displacement.x - riderOffset.x;
  const float dy = displacement.y - riderOffset.y;
  const float dz = displacement.z - riderOffset.z;
  return (dx * working.x + dy * working.y) + dz * working.z;
}

inline bool Acts(Vec3 displacement, Vec3 riderOffset, Vec3 working) {
  return !(SeparationDot(displacement, riderOffset, working) > 0.f);
}

} // namespace PortAirMoverContact
