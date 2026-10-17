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

// The mover's result list (0xf923d8 +0x12a0): cleared once when the tick starts, then every
// MoveAlongGround pass's list and finally the snap's are appended to it, in that order
// (copy_collision_info_list_into 0xc18c00 appends and never clears its destination).
inline void AppendContacts(PortRemasteredGroundWorld::Result& into,
                           const PortRemasteredGroundWorld::Result& from) {
  into.contacts.insert(into.contacts.end(), from.contacts.begin(), from.contacts.end());
}

// The scan after Post (0xf92808-0xf92860) over that combined list: the first non-static
// contact (the side whose owner is not the mover) that can be ridden wins, whichever pass or
// the snap produced it; no normal or floor test, so a wall contact counts.
template < class Rideable >
std::uint32_t PickRider(const PortRemasteredGroundWorld::Result& list, Rideable rideable) {
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

// The rider carry for any platform state. A platform that thought this tick carries its pose
// delta since that PreThink (PlatformCarry). One deactivated since its last PreThink (it was
// active in the previous tick's, not in this one's) carries that last step's delta, start to
// end of that PreThink, once: an inactive actor is not simulated, so PredictMotion's
// R+0x1100 - R+0x1128 still holds its last step, and the rider set in that step decays at the
// end of this one (platform-lifecycle ADDENDUM §1, §3). Never the pose now: a warp while
// inactive is not carried. Any older stamp, or an active platform that did not think this
// tick, carries nothing.
inline Vec3 RiderCarry(const Vec3& start, const Vec3& now, const Vec3& end,
                       std::uint32_t stampTick, std::uint32_t tick, bool platformActive,
                       bool riderActive) {
  if (!riderActive) {
    return {};
  }
  if (stampTick == tick) {
    return PlatformCarry(start, now, stampTick, tick, true);
  }
  if (!platformActive && stampTick + 1u == tick) {
    return {end.x - start.x, end.y - start.y, end.z - start.z};
  }
  return {};
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
//    previous movement tick ran a Remastered mover that owns the carry: an admitted grounded
//    tick (MoveObject 0xf923d8) or an airborne one (MoveObject 0xf943b4, slice 2); otherwise
//    it moves the player as retail does and the tick is marked as already carried.
//  - Then, once the tick's mover is known: a carry-owning Remastered tick drops the deferred
//    retail carry and adds its own carry unless a retail carry already moved the player this
//    tick; every other tick (retail grounded fallback, morph ball, gate off, a tick whose
//    mover never runs) applies the deferred retail carry and no Remastered one.
// Both Remastered movers own the carry because both predict through PredictMotion 0xc23e40,
// which adds the rider's pose delta whenever the rider id is valid (state +0x128): nothing
// in it tests the object state, so an airborne tick with a live rider is carried too.
inline bool DeferRetailCarry(bool lastTickRemasteredCarry) { return lastTickRemasteredCarry; }

struct CarryPlan {
  bool applyDeferredRetail = false;
  bool remasteredCarry = false;
};
inline CarryPlan PlanCarry(bool remasteredCarryMover, bool retailCarriedThisTick) {
  CarryPlan plan;
  if (!remasteredCarryMover) {
    plan.applyDeferredRetail = true;
    return plan;
  }
  plan.remasteredCarry = !retailCarriedThisTick;
  return plan;
}

// The airborne mover's rider writer. DoMovementStep 0xf948d4 resets its candidate to the
// invalid id at the top of each step's impact loop (local_1108, line 331), takes the owner of
// every is_floor impact that is not flagged (contact+0x29 bit 0), not static
// (`(type & ~1) != 2`) and can be ridden (+0x1220) (lines 395-412), and writes it out only on
// the landing branch (`*param_7 = local_1108`, line 493), invalid or not. The post-step probe
// landing (lines 511-533) writes nothing. MoveObject 0xf943b4 then calls SetRiderController
// with it only when valid (0xf94720); a tick that does not land leaves the rider to decay.
struct AirborneRider {
  std::uint32_t written = kTerrainOwner;
  bool any = false;

  void Landed(std::uint32_t stepCandidate) {
    written = stepCandidate;
    any = true;
  }
  // The owner to pass to SetRiderController, or kTerrainOwner for none.
  std::uint32_t Controller() const { return any ? written : kTerrainOwner; }
};

} // namespace PortRemasteredGroundRider
