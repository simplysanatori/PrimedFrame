#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "port_remastered_ground_policy.h"
#include "port_remastered_ground_world.h"

// Moving CScriptPlatforms under Remastered's grounded mover (issue #117, slice 1:
// platforms with a DCLN collision tree, translation only). The pure decisions; the
// game glue is CGameCollision::MoveGroundedRemastered and CScriptPlatform::MoveRiders.
//
// Sources (Remastered 1.0.0 offsets):
//  - CheckCollision_Boolean_NoPostFilter 0xc1f110 / CheckCollision 0xc1ec60 walk the near
//    list built by FindOverlap 0xc1a760: static tree first, then actors by ascending
//    `id & 0xffff`, so platform contacts follow terrain contacts.
//  - AABox_OBBoxTree 0xc0fc18, traversal 0xc0fce0: LIFO, 256-entry stack, the root OBB is
//    never tested, children pushed in index order (visited in reverse). Each leaf triangle
//    goes through the terrain producer 0xc0a180 with the surface material merged with the
//    object's (MergeSurfaceWithObjectMaterial). Boolean twin 0xbfd2f0 / 0xbfd360.
//  - MoveObject 0xf923d8: after Post (0xf92804) the scan 0xf92808-0xf92860 takes the first
//    contact whose owner is not static and can be ridden, and calls SetRiderController
//    0xc1631c with count 2. No normal test. DecayRiderFrameCount (PostSimulation) 2 -> 1 -> 0.
//  - PredictMotion 0xc23e40: the rider's pose delta (R+0x1100 - R+0x1128) is added to the
//    predicted translation, unscaled; the mode is cleared after the first MoveAlongGround
//    pass's ApplyMotionState (0xf9349c).
namespace PortRemasteredGroundRider {

using PortRemasteredGround::Vec3;

// Contact owner encoding (PortRemasteredGroundWorld::Contact::ownerUid): 0 is static
// terrain; an object is 0x10000 | its raw 16-bit TUniqueId, so a valid id never encodes as 0.
constexpr std::uint32_t kTerrainOwner = 0;
inline std::uint32_t OwnerFromRaw(std::uint16_t raw) { return 0x10000u | raw; }
inline std::uint16_t RawFromOwner(std::uint32_t owner) { return std::uint16_t(owner & 0xffffu); }
// The near-list order key. Remastered sorts actors by `id & 0xffff`, its object index; the
// port's index is the low 10 bits of TUniqueId (TUniqueId::Value), the version above it.
inline std::uint32_t NearListKey(std::uint32_t owner) { return owner & 0x3ffu; }
inline bool NearListBefore(std::uint32_t a, std::uint32_t b) {
  return NearListKey(a) < NearListKey(b);
}

// 0xc0fce0's traversal. `leaf(node)` is called for each overlapping leaf in visit order;
// returns false when a push was dropped at the 256-entry cap.
constexpr std::size_t kTreeStackDepth = 256;
template < class Node, class IsLeaf, class Left, class Right, class Overlaps, class Leaf >
bool WalkTreeLifo(const Node& root, IsLeaf isLeaf, Left left, Right right, Overlaps overlaps,
                  Leaf leaf) {
  if (isLeaf(root)) {
    leaf(root);
    return true;
  }
  const Node* stack[kTreeStackDepth];
  std::size_t top = 0;
  bool complete = true;
  auto push = [&](const Node* n) {
    if (top < kTreeStackDepth) {
      stack[top++] = n;
    } else {
      complete = false;
    }
  };
  push(&left(root));
  push(&right(root));
  while (top != 0) {
    const Node& n = *stack[--top];
    if (!overlaps(n)) {
      continue;
    }
    if (isLeaf(n)) {
      leaf(n);
    } else {
      push(&left(n));
      push(&right(n));
    }
  }
  return complete;
}

// One platform triangle as the terrain producer takes it: the surface material merged with
// the object's (MergeSurfaceWithObjectMaterial), tagged with its owner.
inline PortRemasteredGroundWorld::Triangle PlatformTriangle(
    const std::array< PortRemasteredGroundWorld::Vec3f, 3 >& verts,
    const PortRemasteredGroundWorld::Vec3f& normal, std::uint64_t surfaceBits,
    std::uint64_t objectBits, std::uint32_t owner) {
  PortRemasteredGroundWorld::Triangle tri;
  tri.verts = verts;
  tri.normal = normal;
  tri.materialBits = surfaceBits | objectBits;
  tri.ownerUid = owner;
  return tri;
}

// Admission of one near-list collidable that overlaps the tick's query region. Slice 1
// admits only an active CScriptPlatform with a DCLN tree whose orientation did not change
// this tick; anything else sends the whole tick to the retail grounded mover.
struct Candidate {
  bool platform = false;
  bool active = false;
  bool hasTree = false;
  bool rotated = false;
};
inline bool Admit(const Candidate& c) { return c.platform && c.active && c.hasTree && !c.rotated; }

// The rider controller (object +0x1220 side, SetRiderController 0xc1631c): set to 2 by a
// grounded tick that picked a rider, decayed once at the end of every movement tick.
struct RiderController {
  std::uint32_t owner = kTerrainOwner;
  int count = 0;

  void Set(std::uint32_t o) {
    owner = o;
    count = 2;
  }
  void Decay() {
    if (count > 0 && --count == 0) {
      owner = kTerrainOwner;
    }
  }
  bool Active() const { return count > 0 && owner != kTerrainOwner; }
};

// The scan after Post: the snap's list when the snap ran its CheckCollision, else the last
// MoveAlongGround pass's. The first non-static contact (the side whose owner is not the
// mover) that can be ridden wins; no normal or floor test.
template < class Rideable >
std::uint32_t PickRider(bool snapRan, const PortRemasteredGroundWorld::Result& snapList,
                        const PortRemasteredGroundWorld::Result& passList, Rideable rideable) {
  const PortRemasteredGroundWorld::Result& list = snapRan ? snapList : passList;
  for (const PortRemasteredGroundWorld::Contact& c : list.contacts) {
    if (!c.self && c.ownerUid != kTerrainOwner && rideable(c.ownerUid)) {
      return c.ownerUid;
    }
  }
  return kTerrainOwner;
}

// The rider's pose delta for this tick: the platform's translation now minus its
// translation at the start of its PreThink, only when that snapshot was taken this tick and
// the rider is live. A stale stamp (the platform did not think this tick) carries nothing.
inline Vec3 PlatformCarry(const Vec3& previous, const Vec3& current, std::uint32_t stampTick,
                          std::uint32_t tick, bool riderActive) {
  if (!riderActive || stampTick != tick) {
    return {};
  }
  return {current.x - previous.x, current.y - previous.y, current.z - previous.z};
}

// The carry term of PredictMotion: added whole (not scaled by t) while pending, and
// consumed by the first MoveAlongGround pass's ApplyMotionState.
struct CarryTerm {
  Vec3 carry;
  bool pending = false;

  Vec3 Apply(const Vec3& predicted) const {
    if (!pending) {
      return predicted;
    }
    return {predicted.x + carry.x, predicted.y + carry.y, predicted.z + carry.z};
  }
  void ConsumeAfterPass() { pending = false; }
};

// Exactly one carry per tick, whichever mover runs. Order inside a tick:
// CStateManager::Update runs PreThinkObjects (CScriptPlatform::PreThink: pose snapshot,
// MovePlatform, MoveRiders) BEFORE the player's CGameCollision::Move, so retail MoveRiders
// has to act before the player's mover is chosen.
//  - MoveRiders defers the player's retail carry (instead of moving it) when the player's
//    previous movement tick ran the Remastered grounded mover; otherwise it moves the player
//    as retail does and the tick is marked as already carried.
//  - Then, once the tick's mover is known: an admitted Remastered grounded tick drops the
//    deferred retail carry and adds its own carry unless a retail carry already moved the
//    player this tick; every other tick (retail fallback, airborne, morph ball, gate off,
//    a tick whose mover never runs) applies the deferred retail carry and no Remastered one.
inline bool DeferRetailCarry(bool lastTickRemasteredGround) { return lastTickRemasteredGround; }

struct CarryPlan {
  bool applyDeferredRetail = false;
  bool remasteredCarry = false;
};
inline CarryPlan PlanCarry(bool admittedRemasteredGround, bool retailCarriedThisTick) {
  CarryPlan plan;
  if (!admittedRemasteredGround) {
    plan.applyDeferredRetail = true;
    return plan;
  }
  plan.remasteredCarry = !retailCarriedThisTick;
  return plan;
}

} // namespace PortRemasteredGroundRider
