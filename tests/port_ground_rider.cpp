#include "port_remastered_ground_rider.h"

// These checks must also run when the enclosing build defines NDEBUG.
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <initializer_list>
#include <vector>

using namespace PortRemasteredGroundRider;
namespace W = PortRemasteredGroundWorld;

// A synthetic tree node: a leaf carries an id, an inner node two children.
struct Node {
  int id = -1;
  const Node* l = nullptr;
  const Node* r = nullptr;
  bool overlaps = true;
};

static std::vector< int > Walk(const Node& root) {
  std::vector< int > visited;
  WalkTreeLifo(
      root, [](const Node& n) { return n.l == nullptr; },
      [](const Node& n) -> const Node& { return *n.l; },
      [](const Node& n) -> const Node& { return *n.r; },
      [](const Node& n) { return n.overlaps; }, [&](const Node& n) { visited.push_back(n.id); });
  return visited;
}

static W::Triangle Flat(float z, std::uint32_t owner) {
  W::Triangle tri;
  tri.verts = {{{-.5f, -.5f, z}, {.5f, -.5f, z}, {0.f, .5f, z}}};
  tri.normal = {0.f, 0.f, 1.f};
  tri.ownerUid = owner;
  return tri;
}

static W::Contact Owned(std::uint32_t owner, bool self = false) {
  W::Contact c;
  c.ownerUid = owner;
  c.self = self;
  return c;
}

int main() {
  // 1. Tree walk: LIFO, the root is never tested, children pushed left then right, so the
  // right subtree is visited first.
  {
    Node a{0}, b{1}, c{2}, d{3};
    Node left{-1, &a, &b}, right{-1, &c, &d};
    Node root{-1, &left, &right};
    root.overlaps = false; // never tested
    assert((Walk(root) == std::vector< int >{3, 2, 1, 0}));
    right.overlaps = false;
    assert((Walk(root) == std::vector< int >{1, 0}));
    b.overlaps = false;
    assert((Walk(root) == std::vector< int >{0}));
    // A leaf root is visited without a test.
    Node lone{7};
    lone.overlaps = false;
    assert((Walk(lone) == std::vector< int >{7}));
  }

  // 2. Material merge and owner.
  {
    const W::Triangle tri =
        PlatformTriangle({{{0.f, 0.f, 0.f}, {1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}}}, {0.f, 0.f, 1.f},
                         0x0000000000100001ull, 0x0000000200000000ull, OwnerFromRaw(0x0405));
    assert(tri.materialBits == 0x0000000200100001ull);
    assert(tri.ownerUid == 0x10405u);
    assert(RawFromOwner(tri.ownerUid) == 0x0405);
    assert(OwnerFromRaw(0) != kTerrainOwner);
  }

  // 3. Contact order: terrain first, then the platforms by `id & 0xffff` (the port's index).
  {
    W::Query q;
    q.box = {{-1.f, -1.f, -1.f}, {1.f, 1.f, 1.f}};
    const std::uint32_t p1 = OwnerFromRaw(std::uint16_t((5u << 10) | 9u));
    const std::uint32_t p2 = OwnerFromRaw(std::uint16_t((1u << 10) | 3u));
    assert(NearListBefore(p2, p1) && !NearListBefore(p1, p2));
    std::vector< std::uint32_t > platforms{p1, p2};
    std::stable_sort(platforms.begin(), platforms.end(), NearListBefore);
    assert(platforms[0] == p2 && platforms[1] == p1);
    std::vector< W::Triangle > tris{Flat(.9f, kTerrainOwner)};
    for (std::uint32_t p : platforms) {
      tris.push_back(Flat(.8f, p));
    }
    const W::TerrainResult r = W::QueryTerrain(q, tris.data(), tris.size(), true);
    std::vector< std::uint32_t > owners;
    for (const W::Contact& c : r.impacts.contacts) {
      if (!c.self) {
        owners.push_back(c.ownerUid);
      }
    }
    assert((owners == std::vector< std::uint32_t >{kTerrainOwner, p2, p1}));
  }

  // 4. Carry: whole (not scaled by t) until the first pass consumes it.
  {
    CarryTerm term{{0.f, 0.f, .02f}, true};
    const Vec3 p = term.Apply({1.f, 2.f, 3.f});
    assert(p.x == 1.f && p.y == 2.f && p.z == 3.f + .02f);
    const Vec3 half = term.Apply({.5f, 1.f, 1.5f});
    assert(half.z == 1.5f + .02f);
    term.ConsumeAfterPass();
    const Vec3 after = term.Apply({1.f, 2.f, 3.f});
    assert(after.x == 1.f && after.y == 2.f && after.z == 3.f);
    CarryTerm none;
    assert(none.Apply({1.f, 2.f, 3.f}).z == 3.f);
  }

  // 5. Rider choice over the one result list: the first non-self owned, rideable contact; no
  // normal test.
  {
    W::Result list;
    list.contacts = {Owned(kTerrainOwner), Owned(0x10011, true), Owned(0x10022), Owned(0x10033)};
    list.contacts[2].normalZ = -1.f; // a ceiling still counts
    auto all = [](std::uint32_t) { return true; };
    assert(PickRider(list, all) == 0x10022u);
    assert(PickRider(list, [](std::uint32_t o) { return o != 0x10022u; }) == 0x10033u);
    W::Result terrainOnly;
    terrainOnly.contacts = {Owned(kTerrainOwner)};
    assert(PickRider(terrainOnly, all) == kTerrainOwner);
    assert(PickRider(W::Result(), all) == kTerrainOwner);
  }

  // 5b. The list is appended, never replaced (copy_collision_info_list_into 0xc18c00): every
  // pass in order, then the snap, as 0xf923d8 builds it.
  {
    auto all = [](std::uint32_t) { return true; };
    auto build = [](std::initializer_list< W::Result > parts) {
      W::Result list;
      for (const W::Result& part : parts) {
        AppendContacts(list, part);
      }
      return list;
    };
    W::Result terrain, platA, platB;
    terrain.contacts = {Owned(kTerrainOwner)};
    platA.contacts = {Owned(kTerrainOwner), Owned(0x100AA)};
    platB.contacts = {Owned(0x100BB)};
    // The snap hit only terrain, but a pass touched platform A: A is the rider.
    assert(PickRider(build({platA, terrain}), all) == 0x100AAu);
    // Pass 1 terrain, pass 2 platform A, snap platform B: the first, A, wins.
    assert(PickRider(build({terrain, platA, platB}), all) == 0x100AAu);
    // Platforms across passes: the earlier pass's wins.
    assert(PickRider(build({platB, platA}), all) == 0x100BBu);
    // Standing on terrain and brushing a platform's wall in a pass: no normal test, it rides.
    W::Result wall;
    wall.contacts = {Owned(kTerrainOwner), Owned(0x100CC)};
    wall.contacts[1].normalZ = 0.f;
    wall.contacts[1].normalX = 1.f;
    assert(PickRider(build({wall, terrain}), all) == 0x100CCu);
    // Appending keeps each part's order and count.
    const W::Result joined = build({terrain, platA, platB});
    assert(joined.contacts.size() == 4 && joined.contacts[2].ownerUid == 0x100AAu &&
           joined.contacts[3].ownerUid == 0x100BBu);
  }

  // 6. Decay 2 -> 1 -> 0 across contactless ticks; re-contact resets to 2.
  {
    RiderController rider;
    assert(!rider.Active());
    rider.Set(0x10022);
    assert(rider.count == 2 && rider.Active());
    rider.Decay();
    assert(rider.count == 1 && rider.Active());
    rider.Set(0x10022);
    assert(rider.count == 2);
    rider.Decay();
    rider.Decay();
    assert(rider.count == 0 && !rider.Active() && rider.owner == kTerrainOwner);
    rider.Decay();
    assert(rider.count == 0);
  }

  // 7. Gate: only an active, tree-collided, unrotated platform is admitted.
  {
    assert(Admit({true, true, true, false}));
    assert(!Admit({false, true, true, false})); // not a platform (an actor, a door)
    assert(!Admit({true, true, false, false})); // AABox platform
    assert(!Admit({true, true, true, true}));   // rotated this tick
    assert(!Admit({true, false, true, false})); // inactive
  }

  // 8. A stale pose stamp (or no live rider) carries nothing.
  {
    const Vec3 c = PlatformCarry({0.f, 0.f, 1.f}, {0.f, 0.f, 1.25f}, 7, 7, true);
    assert(c.x == 0.f && c.y == 0.f && c.z == .25f);
    const Vec3 stale = PlatformCarry({0.f, 0.f, 1.f}, {0.f, 0.f, 1.25f}, 6, 7, true);
    assert(stale.x == 0.f && stale.y == 0.f && stale.z == 0.f);
    const Vec3 dead = PlatformCarry({0.f, 0.f, 1.f}, {0.f, 0.f, 1.25f}, 7, 7, false);
    assert(dead.z == 0.f);
  }

  // The stale carry after a deactivation: the last active step, start to end of its PreThink,
  // exactly once, never the pose now.
  {
    const Vec3 start = {0.f, 0.f, 1.f};
    const Vec3 end = {0.f, 0.f, 1.25f};
    const Vec3 warped = {5.f, 0.f, 9.f};
    const Vec3 live = RiderCarry(start, end, end, 7, 7, true, true);
    assert(live.z == .25f);
    const Vec3 stale = RiderCarry(start, warped, end, 7, 8, false, true);
    assert(stale.x == 0.f && stale.z == .25f);
    const Vec3 older = RiderCarry(start, warped, end, 7, 9, false, true);
    assert(older.x == 0.f && older.z == 0.f);
    const Vec3 notThought = RiderCarry(start, end, end, 7, 8, true, true);
    assert(notThought.z == 0.f);
    const Vec3 noRider = RiderCarry(start, warped, end, 7, 8, false, false);
    assert(noRider.z == 0.f);
    // Deactivated after its PreThink this tick: it did move this tick.
    const Vec3 sameTick = RiderCarry(start, end, end, 8, 8, false, true);
    assert(sameTick.z == .25f);
    // The rider set on the last active tick T is live at T+1 only: one stale carry.
    RiderController rider;
    rider.Set(OwnerFromRaw(0x0412));
    rider.Decay();
    int carries = 0;
    for (std::uint32_t tick = 8; tick < 12; ++tick) {
      if (RiderCarry(start, warped, end, 7, tick, false, rider.Active()).z != 0.f) {
        ++carries;
      }
      rider.Decay();
    }
    assert(carries == 1);
  }

  // Exactly one carry per tick, whichever mover runs. This models PlanCarry and
  // DeferRetailCarry over the tick order (MoveRiders in PreThink defers or moves, then the
  // tick's mover applies or drops); it re-implements the call sites' logic rather than
  // exercising them, so a glue site in MovePlayer, MoveGroundedRemastered or
  // MoveAirborneStepped that diverges from this model is not caught here (the rig traces
  // cover that). Both Remastered movers (admitted grounded, airborne) own the carry.
  {
    enum Mover { kAdmitted, kFallback, kAirborne, kMorph, kGateOff, kNoMove };
    const Mover script[] = {kGateOff,  kAdmitted, kAdmitted, kFallback, kAdmitted, kAirborne,
                            kAdmitted, kMorph,    kAdmitted, kNoMove,   kAdmitted, kAdmitted,
                            kFallback, kFallback, kAdmitted, kGateOff,  kAdmitted, kAirborne,
                            kAirborne, kFallback, kAirborne, kAdmitted, kAirborne, kMorph,
                            kAirborne, kNoMove,   kAirborne, kGateOff,  kAirborne};
    bool lastTickRemasteredCarry = false;
    for (Mover mover : script) {
      int carries = 0;
      bool deferred = false;
      bool retailCarried = false;
      // MoveRiders: the player is a retail rider every tick here.
      if (DeferRetailCarry(lastTickRemasteredCarry)) {
        deferred = true;
      } else {
        ++carries;
        retailCarried = true;
      }
      lastTickRemasteredCarry = false;
      if (mover == kAdmitted || mover == kAirborne) {
        const CarryPlan plan = PlanCarry(true, retailCarried);
        assert(!plan.applyDeferredRetail);
        deferred = false; // dropped
        if (plan.remasteredCarry) {
          ++carries;
        }
        lastTickRemasteredCarry = true;
      } else if (mover == kFallback) {
        const CarryPlan plan = PlanCarry(false, retailCarried);
        assert(plan.applyDeferredRetail && !plan.remasteredCarry);
      }
      // Every other mover applies the deferred carry; a tick whose mover did not run gets it
      // at PortEndMovementTick.
      if (deferred) {
        ++carries;
      }
      assert(carries == 1);
    }
  }

  // The airborne rider writer (0xf948d4 line 493, 0xf94720): only a landing step writes,
  // and it writes that step's candidate even when it is invalid.
  {
    AirborneRider none;
    assert(none.Controller() == kTerrainOwner);
    AirborneRider landed;
    landed.Landed(OwnerFromRaw(0x0412));
    assert(landed.Controller() == OwnerFromRaw(0x0412));
    AirborneRider terrain;
    terrain.Landed(kTerrainOwner);
    assert(terrain.Controller() == kTerrainOwner);
  }

  // Jumping off a ridden platform: the landing tick sets the rider (count 2), the
  // PostSimulation decay leaves it live for exactly one more tick, so the first airborne tick
  // is carried once and the second is not.
  {
    RiderController rider;
    rider.Set(OwnerFromRaw(0x0412)); // grounded tick T
    rider.Decay();                   // end of T
    assert(rider.Active());          // T+1: the jump tick, carried
    rider.Decay();
    assert(!rider.Active());         // T+2: airborne, not carried
  }

  // The airborne first step's carry: added unscaled to every prediction of that step, t = 0
  // included, and gone once the step is done.
  {
    CarryTerm carry;
    carry.carry = {0.f, 0.f, .5f};
    carry.pending = true;
    const Vec3 lo = carry.Apply({0.f, 0.f, 0.f});
    const Vec3 hi = carry.Apply({1.f, 0.f, -.25f});
    assert(lo.z == .5f && hi.x == 1.f && hi.z == .25f);
    carry.ConsumeAfterPass();
    const Vec3 retry = carry.Apply({1.f, 0.f, -.25f});
    assert(retry.z == -.25f);
  }
  return 0;
}
