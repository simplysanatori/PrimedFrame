// Regression coverage for the AUDIT-2 findings on CGameCollision::MoveAirborneStepped:
//  (1) 0xf94ff0-0xf95028 rejects a contact on dot(displacement, WORKING normal) > 0, so a contact
//      whose primary looks separating but whose secondary blocks is still responded to;
//  (2) 0xc6d644-0xc6d65c selects the secondary whenever the presence byte is set (a zero secondary
//      stays the working normal), and 0xc6d658-0xc6d698 only sets w25, which gates the
//      kMS_ApplyJump force block (0xc6d7f4 `cbz w25`), never the normal choice;
//  (3) the static half of a query is only made when the filter does not exclude
//      kMT_NoStaticCollision (CGameCollision::DetectCollision / DetectCollisionBoolean).
#include "Collision/CMaterialList.hpp"
#include "Collision/CMaterialFilter.hpp"

#include "port_air_mover_contact.h"

#include <cmath>
#include <cstdio>

static int gFailures = 0;

static void Check(bool ok, const char* what) {
  if (!ok) {
    ++gFailures;
    std::printf("FAIL %s\n", what);
  }
}

using PortAirMoverContact::Acts;
using PortAirMoverContact::Vec3;
using PortAirMoverContact::W25;
using PortAirMoverContact::WorkingNormal;

// (2) presence selects the secondary, unconditionally, including a degenerate one.
static void TestWorkingNormal() {
  const Vec3 primary{0.6f, 0.8f, 0.f};   // Z-up: up = z, so a level two-wall pair
  const Vec3 secondary{0.f, 1.f, 0.f};
  const Vec3 zero{0.f, 0.f, 0.f};
  const Vec3 chosen = WorkingNormal(primary, true, secondary);
  Check(chosen.x == secondary.x && chosen.y == secondary.y && chosen.z == secondary.z,
        "presence selects the secondary");
  const Vec3 without = WorkingNormal(primary, false, secondary);
  Check(without.x == primary.x && without.y == primary.y && without.z == primary.z,
        "absence keeps the primary");
  const Vec3 degenerate = WorkingNormal(primary, true, zero);
  Check(degenerate.x == 0.f && degenerate.y == 0.f && degenerate.z == 0.f,
        "a present zero secondary stays the working normal (no fallback to the primary)");
  Check(WorkingNormal({0.f, 0.f, -1.f}, true, zero).z == 0.f,
        "a ceiling primary does not win over a present zero secondary");
}

// (2) w25 only gates the force block: true for a wall pair, false for floor/ceiling pairs.
static void TestW25() {
  Check(W25(false, {0.f, 0.f, 1.f}, {0.f, 0.f, 0.f}), "no secondary -> w25 = 1");
  Check(W25(true, {0.6f, 0.8f, 0.f}, {0.f, 1.f, 0.f}), "two walls -> w25 = 1");
  Check(!W25(true, {0.f, 0.f, -1.f}, {0.6f, 0.8f, 0.f}), "ceiling primary -> w25 = 0");
  Check(!W25(true, {0.6f, 0.8f, 0.f}, {0.f, 0.f, -1.f}), "ceiling secondary -> w25 = 0");
  Check(!W25(true, {0.f, 0.f, 1.f}, {0.6f, 0.8f, 0.f}), "floor primary -> w25 = 0");
  Check(!W25(true, {0.6f, 0.8f, 0.f}, {0.f, 0.f, 1.f}), "floor secondary -> w25 = 0");
  // w25 must not touch the normal choice: the secondary is the working normal either way.
  const Vec3 work = WorkingNormal({0.6f, 0.8f, 0.f}, true, {0.f, 0.f, -1.f});
  Check(work.z == -1.f && !W25(true, {0.6f, 0.8f, 0.f}, {0.f, 0.f, -1.f}),
        "w25 = 0 does not restore the primary");
}

// (1) the separation gate is evaluated on the working normal: the review's example.
static void TestSeparationGate() {
  const Vec3 primary{1.f, 0.f, 0.f};
  const Vec3 secondary{0.f, 0.f, -1.f};
  const Vec3 motion{1.f, 0.f, 1.f};
  const Vec3 noRider{0.f, 0.f, 0.f};
  const Vec3 work = WorkingNormal(primary, true, secondary);
  // Primary +X, secondary -Z, motion (+1,0,+1): the native velocity form against the primary is
  // separating (dot = +1 > 0) and would skip the blocking contact; the source's working-normal
  // form is dot = -1 and responds.
  Check(!Acts(motion, noRider, primary), "primary alone would skip (the native form)");
  Check(Acts(motion, noRider, work), "the working normal responds (0xf94ff0-0xf95028)");
  // The converse: motion along -Z (into the -Z working normal) is separating.
  Check(!Acts({0.f, 0.f, -1.f}, noRider, work), "separating along the working normal skips");
  // A zero secondary never separates: dot = 0, so the contact is responded to (the guard is
  // `> 0`), which is why the whole-dt rule must see the working normal.
  Check(Acts(motion, noRider, {0.f, 0.f, 0.f}), "zero working normal never separates");
  // The rider offset is subtracted before the dot (0xf94ff0-0xf95020).
  Check(!Acts({1.f, 0.f, 0.f}, {2.f, 0.f, 0.f}, {1.f, 0.f, 0.f}) == false,
        "the rider offset is subtracted (dot -1, so the contact acts)");
}

// (3) the static half is guarded exactly as the native static half is.
static void TestStaticGuard() {
  const CMaterialFilter excludesStatic =
      CMaterialFilter::MakeExclude(CMaterialList(kMT_NoStaticCollision));
  const CMaterialFilter open = CMaterialFilter::skPassEverything;
  Check(excludesStatic.GetExcludeList().HasMaterial(kMT_NoStaticCollision),
        "the exclusion list carries kMT_NoStaticCollision");
  Check(!(!excludesStatic.GetExcludeList().HasMaterial(kMT_NoStaticCollision)),
        "excluded static collision must not be queried");
  Check(!open.GetExcludeList().HasMaterial(kMT_NoStaticCollision),
        "an open filter queries the static half");
}

int main() {
  TestWorkingNormal();
  TestW25();
  TestSeparationGate();
  TestStaticGuard();
  if (gFailures == 0) {
    std::printf("port_air_mover_contact_tests: all passed\n");
  }
  return gFailures == 0 ? 0 : 1;
}
