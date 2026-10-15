#include "port_electric2_math.h"

#include <cmath>
#include <cstdio>

using namespace PortElectric2;

namespace {
int failures = 0;
void Check(bool ok, const char* what) {
  if (!ok) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++failures;
  }
}

bool Same(const V3& a, const V3& b) { return a.x == b.x && a.y == b.y && a.z == b.z; }
} // namespace

int main() {
  BoltParams prm;
  prm.nodes = 8;
  const V3 start{0.f, 0.f, 0.f};
  const V3 end{4.f, 0.f, 0.f};

  // Same seed, same bolt; bit for bit.
  std::vector<V3> a, b;
  Rng r1, r2;
  CalculatePointsForBolt(r1, prm, start, end, {}, {}, a, 0.5f, 0.1f);
  CalculatePointsForBolt(r2, prm, start, end, {}, {}, b, 0.5f, 0.1f);
  Check(a.size() == 8 && b.size() == 8, "node count is SSEG");
  bool same = a.size() == b.size();
  for (size_t i = 0; same && i < a.size(); ++i) {
    same = Same(a[i], b[i]);
  }
  Check(same, "fixed seed is deterministic");
  Check(Same(a.front(), start) && Same(a.back(), end), "endpoints stay on start and end");
  Check(r1.seed == r2.seed && r1.seed != 99u, "rng advanced identically");

  // A fractal amplitude moves the inner nodes; zero amplitude leaves them near the line.
  std::vector<V3> flat;
  Rng r3;
  CalculatePointsForBolt(r3, prm, start, end, {}, {}, flat, 0.f, 0.f);
  float maxOff = 0.f, maxFlat = 0.f;
  for (size_t i = 1; i + 1 < a.size(); ++i) {
    maxOff = std::fmax(maxOff, std::sqrt(a[i].y * a[i].y + a[i].z * a[i].z));
    maxFlat = std::fmax(maxFlat, std::sqrt(flat[i].y * flat[i].y + flat[i].z * flat[i].z));
  }
  Check(maxFlat == 0.f, "no amplitude gives a straight bolt");
  Check(maxOff > 0.f, "amplitude bends the bolt");

  // A start direction takes the Bezier path and still pins both ends.
  std::vector<V3> bez;
  Rng r4;
  CalculatePointsForBolt(r4, prm, start, end, V3{0.f, 1.f, 0.f}, {}, bez, 0.f, 0.f);
  Check(bez.size() == 8 && Same(bez.front(), start) && Same(bez.back(), end), "Bezier ends pinned");
  Check(bez[3].y > 0.f, "Bezier bends toward the start direction");

  // One node count and one point per node for the minimum.
  BoltParams one;
  one.nodes = 2;
  std::vector<V3> two;
  Rng r5;
  CalculatePointsForBolt(r5, one, start, end, {}, {}, two, 0.5f, 0.1f);
  Check(two.size() == 2 && Same(two[1], end), "two-node bolt is its endpoints");

  if (failures == 0) {
    std::puts("port_electric2_bolt: ok");
  }
  return failures == 0 ? 0 : 1;
}
