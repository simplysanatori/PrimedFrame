#pragma once

#include <cstddef>
#include <functional>
#include <vector>

// Remastered's collision failsafe for the MP1 stepped mover (issue #117):
// CPhysicsCollisionFailsafe::DoCollisionFailsafe 0xc1fa68 (core 0xc1fc54),
// DoTunnelingFailsafe 0xc20214 and NPhysicsCollision::CalculateOffsetToSafeCollisionPoint
// 0xc21740/0xc217a0, plus the two primitive routines they need for box actors:
// CCollidableAABox::AABox_AABox 0xbf61b4 (impacts via 0xbf6650) and
// NCollisionRayCast::AABox 0xbf7e50. Game-independent: the caller supplies the
// collision queries as hooks. Port frame (Z up) throughout; the box routines
// convert to Remastered's Y-up frame internally so axis order and ties match.
// Spec: build/s-failsafe-20261016/spec/SPEC.md.
namespace PortCollisionFailsafe {

struct Vec3 {
  float x = 0.f, y = 0.f, z = 0.f;
};
struct Box {
  Vec3 min, max;
};

// One impact of a contact, as the core reads it: the self half's +0x14 depth and the
// other half's normal (+0x8) and filter flags (+0x2c & 0x41).
struct Impact {
  float depth = 0.f;
  Vec3 otherNormal;
  bool otherCharacter = false; // Character or CollisionActor: horizontal push
  Vec3 otherCenter;            // other object's collision bounds centre
};
// One CCollisionInfo {first,last} range: one per collided object (a whole area tree
// for terrain).
struct Contact {
  std::vector< Impact > impacts;
};

// 0xc1fc54 inner loop, one iteration: the deepest contact's shallowest impact.
// Returns true when any impact was considered ("moved").
bool SelectPush(const std::vector< Contact >& contacts, const Vec3& from, float worldDt,
                Vec3& push);

struct CoreHooks {
  // CheckCollision 0xc1ec60 at `pos` with the core's filter, through the pass-all result
  // filter: false (nothing collided) ends the loop.
  std::function< bool(const Vec3& pos, std::vector< Contact >& out) > collide;
  // DoTunnelingFailsafe(from, to, out).
  std::function< bool(const Vec3& from, const Vec3& to, Vec3& out) > tunnel;
};
// 0xc1fc54: up to five pushes from `from`, then the tunneling check from the state
// (`from`) to the result.
bool Core(const Vec3& from, float worldDt, const CoreHooks& hooks, Vec3& out);

// 0xc1fa68. `core(failsafe, out)` runs Core from `cur` with the collision filter
// (0x1270) or the failsafe filter (0x1290); `blocked(failsafe, pos)` is
// CheckGlobalCollision_Boolean_NoPostFilter 0xc1f240 with that filter. `out` is the
// translation to set (== cur when the object is not moved); the return value is the
// failsafe's result.
bool DoCollisionFailsafe(const Vec3& cur, bool hasFailsafeFilter,
                         const std::function< bool(bool failsafe, Vec3& out) >& core,
                         const std::function< bool(bool failsafe, const Vec3& pos) >& blocked,
                         Vec3& out);

// ERayResult.
enum RayCode { kRay_Miss = 0, kRay_Inside = 1, kRay_Hit = 2 };
struct RayHit {
  int code = kRay_Miss;
  float t = 0.f;
  Vec3 normal;
  int orientation = -1; // ECollisionOrientation of the hit material
};

// NCollisionUtils::is_floor 0xbecb40.
bool IsFloor(float up, int orientation);

struct TunnelHooks {
  // RayIntersection 0xc20810 over the swept near list (no Character/CollisionActor).
  std::function< RayHit(const Vec3& start, const Vec3& dir, float length) > ray;
  // CalculateOffsetToSafeCollisionPoint at `pos` with `maxDist`; false = no valid offset.
  std::function< bool(const Vec3& pos, float maxDist, Vec3& offset) > safeOffset;
};
// 0xc20214. `bounds`/`pos` are the object's CURRENT collision bounds and translation.
bool DoTunnelingFailsafe(const Box& bounds, const Vec3& pos, const Vec3& from, const Vec3& to,
                         const TunnelHooks& hooks, Vec3& out);

// 0x459c800 (26, port frame) and 0x1d1f9d8 (21).
extern const Vec3 kSafeDirections[26];
extern const float kSafeRadii[21];

struct SafeHooks {
  // GetCollision_Boolean of the object's box placed at `pos` against the near list.
  std::function< bool(const Vec3& pos) > collidesAt;
  // A ray from `start` along `dir` hits a near object with t < length.
  std::function< bool(const Vec3& start, const Vec3& dir, float length) > rayBlocked;
};
// 0xc217a0. `box` is the object's box at `pos`. Returns valid; `offset` is the push.
bool CalculateOffsetToSafeCollisionPoint(const Box& box, const Vec3& pos, float maxDist,
                                         const SafeHooks& hooks, Vec3& offset);

// 0xbf61b4: impacts of `self` against `other`, normals the OTHER side's (push self
// along them). Returns false when the boxes do not overlap.
bool BoxBoxImpacts(const Box& self, const Box& other, std::vector< Impact >& out);

// 0xbf7e50.
RayHit RayAABox(const Box& box, const Vec3& start, const Vec3& dir, float length);

} // namespace PortCollisionFailsafe
