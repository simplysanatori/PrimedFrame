// Remastered collision failsafe decisions (platform/port_collision_failsafe.cpp) on
// hand-built contacts: core push selection 0xc1fc54, wrapper 0xc1fa68, tunneling
// 0xc20214, safe offset 0xc217a0, box-box impacts 0xbf61b4 and the AABox ray 0xbf7e50.
#include "port_collision_failsafe.h"

#include <cfloat>
#include <cmath>
#include <cstdio>

using namespace PortCollisionFailsafe;

static int gFailures = 0;

static void Check(bool ok, const char* what) {
  if (!ok) {
    ++gFailures;
    std::printf("FAIL %s\n", what);
  }
}

static bool Near(float a, float b, float e = 1e-5f) { return std::fabs(a - b) <= e; }
static bool Near(const Vec3& a, const Vec3& b, float e = 1e-5f) {
  return Near(a.x, b.x, e) && Near(a.y, b.y, e) && Near(a.z, b.z, e);
}

static Impact Terrain(float depth, Vec3 n) {
  Impact i;
  i.depth = depth;
  i.otherNormal = n;
  return i;
}

static void PushSelection() {
  // Within a contact the shallowest impact; across contacts the deepest of those.
  std::vector< Contact > list(2);
  list[0].impacts = {Terrain(.3f, {0, 0, 1}), Terrain(.1f, {1, 0, 0})};
  list[1].impacts = {Terrain(.2f, {0, 1, 0})};
  Vec3 push;
  Check(SelectPush(list, {}, 1.f / 60.f, push), "push: moved");
  Check(Near(push, {0, .2f + 1e-4f, 0}), "push: deepest contact's shallowest impact");

  // Ties keep the first (strict compares both ways).
  list[1].impacts = {Terrain(.1f, {0, -1, 0})};
  SelectPush(list, {}, 1.f / 60.f, push);
  Check(Near(push, {1e-4f + .1f, 0, 0}), "push: tie keeps the earlier contact");

  // Zero depth still pushes by EPS4 (best starts at -FLT_EPSILON).
  list.resize(1);
  list[0].impacts = {Terrain(0.f, {0, 0, 1})};
  SelectPush(list, {}, 1.f / 60.f, push);
  Check(Near(push, {0, 0, 1e-4f}, 1e-9f), "push: zero depth");

  // Character contact: horizontal, away from the other's centre seen from `from`,
  // capped at worldDt * 50.
  Impact c;
  c.depth = 2.f;
  c.otherCharacter = true;
  c.otherCenter = {3, 4, 10};
  list[0].impacts = {c};
  SelectPush(list, {0, 0, 0}, 1.f / 60.f, push);
  const float cap = 50.f / 60.f;
  Check(Near(push, {-.6f * cap, -.8f * cap, 0}), "push: character horizontal capped");
  list[0].impacts[0].depth = .1f;
  SelectPush(list, {0, 0, 0}, 1.f / 60.f, push);
  Check(Near(push, {-.6f * (.1f + 1e-4f), -.8f * (.1f + 1e-4f), 0}), "push: character depth");

  list.clear();
  Check(!SelectPush(list, {}, 1.f / 60.f, push), "push: empty");
}

static void CoreLoop() {
  // A floor plane at z = 0 overlapping a box whose bottom is at pos.z: one push.
  CoreHooks hooks;
  int calls = 0;
  hooks.collide = [&](const Vec3& pos, std::vector< Contact >& out) {
    ++calls;
    if (pos.z >= 0.f) return false;
    out.resize(1);
    out[0].impacts = {Terrain(-pos.z, {0, 0, 1})};
    return true;
  };
  bool tunnelCalled = false;
  hooks.tunnel = [&](const Vec3& from, const Vec3& to, Vec3&) {
    tunnelCalled = true;
    Check(Near(from, {0, 0, -.25f}) && Near(to, {0, 0, 1e-4f}), "core: tunnel from state to out");
    return false;
  };
  Vec3 out;
  Check(Core({0, 0, -.25f}, 1.f / 60.f, hooks, out), "core: moved");
  Check(Near(out, {0, 0, 1e-4f}), "core: depth + EPS4");
  Check(calls == 2 && tunnelCalled, "core: stops on the first free test");

  // Never resolves: five pushes at most.
  calls = 0;
  hooks.collide = [&](const Vec3&, std::vector< Contact >& out) {
    ++calls;
    out.resize(1);
    out[0].impacts = {Terrain(1.f, {1, 0, 0})};
    return true;
  };
  hooks.tunnel = [](const Vec3&, const Vec3&, Vec3&) { return false; };
  Core({}, 1.f / 60.f, hooks, out);
  Check(calls == 5 && Near(out, {5 * (1.f + 1e-4f), 0, 0}, 1e-4f), "core: five iterations");

  // A tunneling revert replaces the result and reports moved.
  calls = 0;
  hooks.collide = [&](const Vec3&, std::vector< Contact >&) { return false; };
  hooks.tunnel = [](const Vec3& from, const Vec3&, Vec3& o) {
    o = from;
    return true;
  };
  Check(Core({1, 2, 3}, 1.f / 60.f, hooks, out) && Near(out, {1, 2, 3}), "core: tunnel result");
}

static void Wrapper() {
  const Vec3 cur{1, 1, 1};
  Vec3 out;
  int coreCalls = 0;
  auto core = [&](Vec3 a, Vec3 b, bool ra, bool rb) {
    return [=, &coreCalls](bool failsafe, Vec3& o) {
      ++coreCalls;
      o = failsafe ? b : a;
      return failsafe ? rb : ra;
    };
  };
  auto blocked = [](bool a, bool b) {
    return [=](bool failsafe, const Vec3&) { return failsafe ? b : a; };
  };
  // No failsafe filter: the core decides.
  Check(DoCollisionFailsafe(cur, false, core({2, 2, 2}, {}, true, false), blocked(true, true), out) &&
            Near(out, {2, 2, 2}),
        "wrapper: no F1290, core result");
  Check(!DoCollisionFailsafe(cur, false, core({2, 2, 2}, {}, false, false), blocked(true, true), out) &&
            Near(out, cur),
        "wrapper: no F1290, core failed");
  // First core free: kept.
  coreCalls = 0;
  Check(DoCollisionFailsafe(cur, true, core({2, 2, 2}, {3, 3, 3}, true, true), blocked(false, false), out) &&
            Near(out, {2, 2, 2}) && coreCalls == 1,
        "wrapper: first result free");
  // First core failed: not moved, false.
  Check(!DoCollisionFailsafe(cur, true, core({2, 2, 2}, {3, 3, 3}, false, true), blocked(false, false), out) &&
            Near(out, cur),
        "wrapper: first core failed");
  // First blocked, second free: the failsafe-filter result.
  Check(DoCollisionFailsafe(cur, true, core({2, 2, 2}, {3, 3, 3}, true, true), blocked(true, false), out) &&
            Near(out, {3, 3, 3}),
        "wrapper: failsafe filter result");
  // First blocked, second core failed: false, not moved.
  Check(!DoCollisionFailsafe(cur, true, core({2, 2, 2}, {3, 3, 3}, true, false), blocked(true, false), out) &&
            Near(out, cur),
        "wrapper: second core failed");
  // Both blocked: true, not moved.
  Check(DoCollisionFailsafe(cur, true, core({2, 2, 2}, {3, 3, 3}, true, true), blocked(true, true), out) &&
            Near(out, cur),
        "wrapper: still stuck reports resolved");
}

static void Tunneling() {
  // Player-like box: 1 x 1 x 2.7, position at the feet.
  const Vec3 pos{0, 0, 0};
  const Box bounds{{-.5f, -.5f, 0}, {.5f, .5f, 2.7f}};
  TunnelHooks hooks;
  int rays = 0;
  RayHit hit;
  Vec3 rayStart, rayDir;
  float rayLen = 0.f;
  hooks.ray = [&](const Vec3& s, const Vec3& d, float l) {
    ++rays;
    rayStart = s;
    rayDir = d;
    rayLen = l;
    return hit;
  };
  int safeCalls = 0;
  hooks.safeOffset = [&](const Vec3&, float maxDist, Vec3& o) {
    ++safeCalls;
    Check(Near(maxDist, .5f + FLT_EPSILON), "tunnel: safe radius = min half + eps");
    o = {0, 0, .01f};
    return true;
  };
  Vec3 out;
  // Short move (<= min half extent): never checked.
  Check(!DoTunnelingFailsafe(bounds, pos, {0, 0, 0}, {.5f, 0, 0}, hooks, out) && rays == 0 &&
            Near(out, {.5f, 0, 0}),
        "tunnel: short move");
  // Miss.
  hit = RayHit{};
  Check(!DoTunnelingFailsafe(bounds, pos, {0, 0, 0}, {2, 0, 0}, hooks, out) && rays == 1,
        "tunnel: miss");
  Check(Near(rayStart, {0, 0, 1.35f}) && Near(rayDir, {1, 0, 0}) && Near(rayLen, 2.f),
        "tunnel: ray from the bounds-centre height");
  // Code 1: revert.
  hit.code = kRay_Inside;
  Check(DoTunnelingFailsafe(bounds, pos, {0, 0, 0}, {2, 0, 0}, hooks, out) && Near(out, {0, 0, 0}),
        "tunnel: start inside reverts");
  // Hit closer than the half width: revert.
  hit.code = kRay_Hit;
  hit.t = .49f;
  hit.normal = {-1, 0, 0};
  hit.orientation = 2;
  Check(DoTunnelingFailsafe(bounds, pos, {0, 0, 0}, {2, 0, 0}, hooks, out) && Near(out, {0, 0, 0}),
        "tunnel: near hit reverts");
  // Wall hit: placed half a width short, raised by cy, no safe offset.
  hit.t = 1.5f;
  safeCalls = 0;
  Check(DoTunnelingFailsafe(bounds, pos, {0, 0, 0}, {2, 0, 0}, hooks, out) &&
            Near(out, {1.f, 0, 1.35f}) && safeCalls == 0,
        "tunnel: wall placement");
  // Floor hit: the safe offset is added.
  hit.normal = {0, 0, 1};
  hit.orientation = -1;
  Check(DoTunnelingFailsafe(bounds, pos, {0, 0, 0}, {2, 0, 0}, hooks, out) &&
            Near(out, {1.f, 0, 1.36f}) && safeCalls == 1,
        "tunnel: floor placement + safe offset");
  // Feet below the position shift the result by the same amount.
  const Box low{{-.5f, -.5f, -.2f}, {.5f, .5f, 2.5f}};
  hit.normal = {-1, 0, 0};
  hit.orientation = 2;
  DoTunnelingFailsafe(low, pos, {0, 0, 0}, {2, 0, 0}, hooks, out);
  Check(Near(out, {1.f, 0, 1.15f + .2f}), "tunnel: foot offset");
}

static void Floor() {
  Check(IsFloor(.86f, 2), "is_floor: steep up");
  Check(IsFloor(-1.f, 0), "is_floor: orientation 0");
  Check(IsFloor(.8f, -1), "is_floor: unknown orientation passes 0.7071");
  Check(IsFloor(.8f, 3), "is_floor: orientation 3");
  Check(!IsFloor(.8f, 1) && !IsFloor(.8f, 2), "is_floor: ceiling/wall need 0.85");
  Check(!IsFloor(.7f, -1), "is_floor: below 0.7071");
}

static void SafeOffset() {
  const Box box{{-.5f, -.5f, 0}, {.5f, .5f, 2.7f}};
  SafeHooks hooks;
  int rays = 0;
  hooks.rayBlocked = [&](const Vec3&, const Vec3&, float) {
    ++rays;
    return false;
  };
  Vec3 off;
  hooks.collidesAt = [](const Vec3&) { return false; };
  Check(CalculateOffsetToSafeCollisionPoint(box, {}, .5f, hooks, off) && Near(off, {}),
        "safe: already free");
  // Free only above z >= 0.03: radius order first, then direction order (+Z is #4).
  hooks.collidesAt = [](const Vec3& p) { return p.z < .03f - 1e-6f; };
  Check(CalculateOffsetToSafeCollisionPoint(box, {}, .5f + FLT_EPSILON, hooks, off) &&
            Near(off, {0, 0, .03f}) && rays == 0,
        "safe: smallest radius, first direction");
  // Free only along +Y (#0): wins over every later direction at the same radius.
  hooks.collidesAt = [](const Vec3& p) { return !(p.y > 0.f); };
  CalculateOffsetToSafeCollisionPoint(box, {}, .5f, hooks, off);
  Check(Near(off, {0, .01f, 0}), "safe: direction order");
  // Never free: no valid offset; radii stop below maxDist.
  hooks.collidesAt = [](const Vec3&) { return true; };
  Check(!CalculateOffsetToSafeCollisionPoint(box, {}, .5f, hooks, off), "safe: none");
  // Rays only from the min half extent on.
  rays = 0;
  CalculateOffsetToSafeCollisionPoint(box, {}, .8f, hooks, off);
  Check(rays == 26, "safe: ray branch at d >= min half (only 0.705)");
  // Direction table: unit vectors, port = (-remX, remZ, remY) of the SPEC order.
  for (const Vec3& d : kSafeDirections) {
    Check(Near(d.x * d.x + d.y * d.y + d.z * d.z, 1.f, 1e-5f), "safe: unit directions");
  }
  Check(Near(kSafeDirections[18], {-.57735026f, .57735026f, .57735026f}), "safe: dir 18");
  Check(kSafeRadii[0] == .01f && kSafeRadii[20] == 8.f, "safe: radii");
}

static void BoxBox() {
  std::vector< Impact > imp;
  // Self sinks 0.1 into the top of a wide slab: smallest impact pushes up.
  const Box slab{{-5, -5, -1}, {5, 5, 0}};
  Check(BoxBoxImpacts({{-.5f, -.5f, -.1f}, {.5f, .5f, 2.6f}}, slab, imp), "boxbox: hit");
  float best = FLT_MAX;
  Vec3 n;
  for (const Impact& i : imp) {
    if (i.depth < best) {
      best = i.depth;
      n = i.otherNormal;
    }
  }
  Check(Near(best, .1f) && Near(n, {0, 0, 1}), "boxbox: vertical push out of the top");
  // Side overlap: +X push from a box on the -X side.
  imp.clear();
  BoxBoxImpacts({{.8f, -.5f, 0}, {1.8f, .5f, 2.7f}}, {{-1, -1, -1}, {1, 1, 5}}, imp);
  Check(imp.size() == 1 && Near(imp[0].depth, .2f) && Near(imp[0].otherNormal, {1, 0, 0}),
        "boxbox: side impact");
  // Self inside other on every axis: fallback picks the smallest difference.
  imp.clear();
  BoxBoxImpacts({{-.5f, -.5f, 0}, {.5f, .5f, 1}}, {{-3, -3, -.25f}, {3, 3, 4}}, imp);
  Check(imp.size() == 1 && Near(imp[0].depth, 1.25f) && Near(imp[0].otherNormal, {0, 0, -1}),
        "boxbox: contained fallback");
  // Touching: no positive depth, no contact.
  imp.clear();
  Check(!BoxBoxImpacts({{1, 0, 0}, {2, 1, 1}}, {{0, 0, 0}, {1, 1, 1}}, imp), "boxbox: touching");
  // Apart.
  Check(!BoxBoxImpacts({{2, 0, 0}, {3, 1, 1}}, {{0, 0, 0}, {1, 1, 1}}, imp), "boxbox: apart");
}

static void Ray() {
  const Box box{{1, -1, -1}, {3, 1, 1}};
  RayHit r = RayAABox(box, {0, 0, 0}, {1, 0, 0}, 5.f);
  Check(r.code == kRay_Hit && Near(r.t, 1.f) && Near(r.normal, {-1, 0, 0}), "ray: hit");
  r = RayAABox(box, {0, 0, 0}, {1, 0, 0}, .5f);
  Check(r.code == kRay_Miss, "ray: too short");
  r = RayAABox(box, {2, 0, 0}, {1, 0, 0}, 5.f);
  Check(r.code == kRay_Inside && r.t == 0.f, "ray: start inside");
  r = RayAABox(box, {0, 2, 0}, {1, 0, 0}, 5.f);
  Check(r.code == kRay_Miss, "ray: parallel outside");
  r = RayAABox(box, {2, 0, 5}, {0, 0, -1}, 10.f);
  Check(r.code == kRay_Hit && Near(r.t, 4.f) && Near(r.normal, {0, 0, 1}), "ray: down onto top");
  r = RayAABox(box, {5, 0, 0}, {1, 0, 0}, 5.f);
  Check(r.code == kRay_Miss, "ray: behind");
}

int main() {
  PushSelection();
  CoreLoop();
  Wrapper();
  Tunneling();
  Floor();
  SafeOffset();
  BoxBox();
  Ray();
  if (gFailures != 0) {
    std::printf("%d failure(s)\n", gFailures);
    return 1;
  }
  std::printf("port_collision_failsafe: ok\n");
  return 0;
}
