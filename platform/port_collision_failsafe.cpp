#include "port_collision_failsafe.h"

#include <cfloat>
#include <cmath>

namespace PortCollisionFailsafe {
namespace {

constexpr float kEps5 = 1e-5f; // 0x3727c5ac
constexpr float kEps4 = 1e-4f; // 0x38d1b717

Vec3 Add(const Vec3& a, const Vec3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 Sub(const Vec3& a, const Vec3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 Mul(const Vec3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }

// Remastered is Y-up: rem = (-port.x, port.z, port.y), its own inverse
// (platform/port_remastered_ground_world.cpp).
struct R3 {
  float v[3];
};
R3 ToRem(const Vec3& p) { return {{-p.x, p.z, p.y}}; }
Vec3 FromRem(const R3& r) { return {-r.v[0], r.v[2], r.v[1]}; }
void BoxToRem(const Box& b, R3& mn, R3& mx) {
  mn = {{-b.max.x, b.min.z, b.min.y}};
  mx = {{-b.min.x, b.max.z, b.max.y}};
}

Vec3 Center(const Box& b) {
  return {(b.min.x + b.max.x) * .5f, (b.min.y + b.max.y) * .5f, (b.min.z + b.max.z) * .5f};
}

float MinHalfExtent(const Box& b) {
  return std::fmin(std::fmin((b.max.x - b.min.x) * .5f, (b.max.y - b.min.y) * .5f),
                   (b.max.z - b.min.z) * .5f);
}

} // namespace

bool SelectPush(const std::vector< Contact >& contacts, const Vec3& from, float worldDt,
                Vec3& push) {
  float best = -FLT_EPSILON;
  bool moved = false;
  push = {};
  for (const Contact& c : contacts) {
    float curMin = FLT_MAX;
    bool has = false;
    Vec3 cand;
    for (const Impact& i : c.impacts) {
      // Behaviour bit 0x20 (+0x28) skips an impact; no MP1 material maps to it.
      if (!(i.depth < curMin)) continue;
      curMin = i.depth;
      if (!i.otherCharacter) {
        cand = Mul(i.otherNormal, i.depth + kEps4);
      } else {
        // Away from the other's bounds centre, horizontally, from the ORIGINAL
        // position, at most worldDt * 50.
        Vec3 d{from.x - i.otherCenter.x, from.y - i.otherCenter.y, 0.f};
        const float len = std::sqrt(d.x * d.x + d.y * d.y);
        d = len > FLT_EPSILON ? Mul(d, 1.f / len) : Vec3{}; // NormalizeOrZero
        cand = Mul(d, std::fmin(i.depth + kEps4, worldDt * 50.f));
      }
      has = true;
      moved = true;
    }
    if (has && curMin > best) {
      best = curMin;
      push = cand;
    }
  }
  return moved;
}

bool Core(const Vec3& from, float worldDt, const CoreHooks& hooks, Vec3& out) {
  out = from;
  bool moved = false;
  std::vector< Contact > list;
  for (int iter = 0; iter < 5; ++iter) {
    list.clear();
    // CheckCollision 0xc1ec60, then the pass-all result filter (vt[0x10], 0xc23a14),
    // which fails on an empty list.
    if (!hooks.collide(out, list) || list.empty()) break;
    Vec3 push;
    if (SelectPush(list, from, worldDt, push)) moved = true;
    out = Add(out, push);
  }
  Vec3 tmp = out;
  if (hooks.tunnel(from, out, tmp)) {
    out = tmp;
    return true;
  }
  return moved;
}

bool DoCollisionFailsafe(const Vec3& cur, bool hasFailsafeFilter,
                         const std::function< bool(bool failsafe, Vec3& out) >& core,
                         const std::function< bool(bool failsafe, const Vec3& pos) >& blocked,
                         Vec3& out) {
  out = cur;
  const bool r = core(false, out);
  if (!hasFailsafeFilter) {
    if (!r) out = cur;
    return r;
  }
  if (!r) {
    out = cur;
    return false;
  }
  if (!blocked(false, out)) return true;
  // Again from cur with the failsafe filter; the first result is discarded.
  if (!core(true, out)) {
    out = cur;
    return false;
  }
  if (!blocked(true, out)) return true;
  // Still stuck: not moved, but reported resolved.
  out = cur;
  return true;
}

bool IsFloor(float up, int orientation) {
  // 0xbecb40: up > 0.85 || orient == 0 || (up > 0.70710677 && (unsigned)(orient - 1) > 1).
  return up > 0.85f || orientation == 0 ||
         (up > 0.70710677f && unsigned(orientation - 1) > 1u);
}

bool TunnelingFailsafeApplies(const Box& bounds, const Vec3& from, const Vec3& to) {
  const Vec3 delta = Sub(to, from);
  const float len = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
  return len > MinHalfExtent(bounds);
}

bool DoTunnelingFailsafe(const Box& bounds, const Vec3& pos, const Vec3& from, const Vec3& to,
                         const TunnelHooks& hooks, Vec3& out) {
  out = to;
  const float cy = Center(bounds).z - pos.z;
  const float footZ = bounds.min.z;
  const float minHalf = MinHalfExtent(bounds);
  const Vec3 delta = Sub(to, from);
  const float len = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
  const Vec3 dir = len > FLT_EPSILON ? Mul(delta, 1.f / len) : Vec3{};
  if (len <= minHalf) return false;
  const Vec3 start{from.x, from.y, from.z + cy};
  const RayHit res = hooks.ray(start, dir, len);
  if (res.code == kRay_Inside) {
    out = from;
    return true;
  }
  if (res.code != kRay_Hit) return false;
  const float halfH =
      .5f * std::fmax(bounds.max.x - bounds.min.x, bounds.max.y - bounds.min.y);
  if (res.t < halfH) {
    out = from;
    return true;
  }
  // The box bottom lands on the ray point: pos ends cy above the swept path, as decoded.
  out = Add(start, Mul(dir, res.t - halfH));
  out.z -= footZ - pos.z;
  if (IsFloor(res.normal.z, res.orientation)) {
    Vec3 offset;
    if (hooks.safeOffset(out, minHalf + FLT_EPSILON, offset)) out = Add(out, offset);
  }
  return true;
}

namespace {
constexpr float kA = 0.70710677f;
constexpr float kB = 0.57735026f;
} // namespace

// 0x459c800 in port axes (SPEC table; Remastered (x,y,z) -> port (-x,z,y)).
const Vec3 kSafeDirections[26] = {
    {0, 1, 0},     {0, -1, 0},     {1, 0, 0},      {-1, 0, 0},     {0, 0, 1},
    {0, 0, -1},    {-kA, 0, kA},   {kA, 0, -kA},   {kA, 0, kA},    {-kA, 0, -kA},
    {-kA, kA, 0},  {kA, -kA, 0},   {kA, kA, 0},    {-kA, -kA, 0},  {0, kA, kA},
    {0, -kA, -kA}, {0, kA, -kA},   {0, -kA, kA},   {-kB, kB, kB},  {kB, kB, kB},
    {-kB, kB, -kB}, {kB, kB, -kB}, {-kB, -kB, kB}, {kB, -kB, kB},  {-kB, -kB, -kB},
    {kB, -kB, -kB},
};

const float kSafeRadii[21] = {0.01f,  0.015f, 0.02f,  0.03f,  0.045f, 0.065f, 0.095f,
                              0.14f,  0.21f,  0.315f, 0.47f,  0.705f, 1.055f, 1.58f,
                              2.37f,  3.f,    4.f,    5.f,    6.f,    7.f,    8.f};

bool CalculateOffsetToSafeCollisionPoint(const Box& box, const Vec3& pos, float maxDist,
                                         const SafeHooks& hooks, Vec3& offset) {
  offset = {};
  if (!hooks.collidesAt(pos)) return true;
  const Vec3 ctr = Center(box);
  const float minHalf = MinHalfExtent(box);
  for (int i = 0; i < 21; ++i) {
    const float d = kSafeRadii[i];
    if (d >= maxDist) break;
    for (int j = 0; j < 26; ++j) {
      const Vec3& dir = kSafeDirections[j];
      if (d >= minHalf && hooks.rayBlocked(ctr, dir, d)) continue;
      const Vec3 o = Mul(dir, d);
      if (hooks.collidesAt(Add(pos, o))) continue;
      offset = o;
      return true; // first hit wins
    }
  }
  return false;
}

bool BoxBoxImpacts(const Box& self, const Box& other, std::vector< Impact >& out) {
  // A = self, B = other; Remastered axes x, y (up), z.
  R3 aMin, aMax, bMin, bMax;
  BoxToRem(self, aMin, aMax);
  BoxToRem(other, bMin, bMax);
  // DoBoundsOverlap 0x98234 (inclusive) gates the whole routine.
  for (int k = 0; k < 3; ++k) {
    if (aMax.v[k] < bMin.v[k] || bMax.v[k] < aMin.v[k]) return false;
  }
  const std::size_t first = out.size();
  // 0xbf6650: an impact is added only with depth > 0; both halves carry the depth and
  // their owner's outward face normal: A's +axis*sign, B's -axis*sign.
  auto add = [&](int axis, float depth, float sign) {
    if (!(depth > 0.f)) return;
    R3 n{{0.f, 0.f, 0.f}};
    n.v[axis] = -sign; // the other (B) half's normal
    Impact i;
    i.depth = depth;
    i.otherNormal = FromRem(n);
    out.push_back(i);
  };
  for (int k = 0; k < 3; ++k) {
    const float extB = bMax.v[k] - bMin.v[k];
    const float extA = aMax.v[k] - aMin.v[k];
    const float s0 = aMax.v[k] - bMin.v[k];
    const float s8 = bMax.v[k] - aMin.v[k];
    if (s0 < extB && s0 < extA) add(k, s0, 1.f);
    if (s8 < extA && s8 < extB) add(k, s8, -1.f);
  }
  if (out.size() == first) {
    // No axis partly overlaps: the smallest non-negative difference, earlier axis on ties.
    const float d[6] = {aMax.v[0] - bMin.v[0], bMax.v[0] - aMin.v[0], aMax.v[1] - bMin.v[1],
                        bMax.v[1] - aMin.v[1], aMax.v[2] - bMin.v[2], bMax.v[2] - aMin.v[2]};
    float best = (d[0] < FLT_MAX && d[0] >= 0.f) ? d[0] : FLT_MAX;
    int axis = 0;
    for (int k = 1; k < 6; ++k) {
      if (d[k] < best && d[k] >= 0.f) {
        best = d[k];
        axis = k / 2;
      }
    }
    const float p = aMax.v[axis] - bMin.v[axis];
    const float n = bMax.v[axis] - aMin.v[axis];
    if (p < n) {
      add(axis, p, 1.f);
    } else {
      add(axis, n, -1.f);
    }
  }
  return out.size() != first;
}

RayHit RayAABox(const Box& box, const Vec3& start, const Vec3& dir, float length) {
  RayHit miss;
  R3 mn, mx;
  BoxToRem(box, mn, mx);
  const R3 p = ToRem(start);
  const R3 d = ToRem(dir);
  R3 normal{{0.f, 1.f, 0.f}}; // skUpVector until an entry face is taken
  float tEnter = 0.f;
  float tExit = FLT_MAX;
  for (int k = 0; k < 3; ++k) {
    if (std::fabs(d.v[k]) < kEps5) {
      if (p.v[k] < mn.v[k] || mx.v[k] < p.v[k]) return miss;
      continue;
    }
    const float inv = 1.f / d.v[k];
    const bool pos = d.v[k] >= 0.f;
    const float entry = ((pos ? mn.v[k] : mx.v[k]) - p.v[k]) * inv;
    const float exit = ((pos ? mx.v[k] : mn.v[k]) - p.v[k]) * inv;
    // Axis 0 takes its entry only when non-negative; later axes when entry >= tEnter.
    if (k == 0 ? entry >= 0.f : tEnter <= entry) {
      tEnter = entry;
      normal = {{0.f, 0.f, 0.f}};
      normal.v[k] = pos ? -1.f : 1.f;
    }
    if (tExit - exit >= 0.f) tExit = exit;
  }
  if (tExit <= 0.f || tExit < tEnter) return miss;
  if (length < tEnter) return miss;
  RayHit hit;
  if (tEnter == 0.f) {
    hit.code = kRay_Inside;
    hit.t = 0.f;
    return hit;
  }
  hit.code = kRay_Hit;
  hit.t = tEnter;
  hit.normal = FromRem(normal);
  return hit;
}

} // namespace PortCollisionFailsafe
