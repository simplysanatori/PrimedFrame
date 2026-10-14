#include "port_map_pickups.h"

#include <cstdio>
#include <cstdlib>

namespace {
void Check(bool condition) {
  if (!condition) {
    std::abort();
  }
}
} // namespace

int main() {
  size_t count = 0;
  const PortMapPickups::Dot* dots = PortMapPickups::Dots(count);
  Check(count > 0 && count <= PortMapPickups::kMaxDots);

  // Every dot finds itself, and a point a little off it too.
  for (size_t i = 0; i < count; ++i) {
    const float exact[3] = {dots[i].pos[0], dots[i].pos[1], dots[i].pos[2]};
    Check(PortMapPickups::DotIndex(dots[i].world, dots[i].area, exact) == static_cast< int >(i));
    const float near[3] = {exact[0] + 0.5f, exact[1] - 0.5f, exact[2] + 0.5f};
    Check(PortMapPickups::DotIndex(dots[i].world, dots[i].area, near) == static_cast< int >(i));
  }
  const float far[3] = {dots[0].pos[0] + 50.f, dots[0].pos[1], dots[0].pos[2]};
  Check(PortMapPickups::DotIndex(dots[0].world, dots[0].area, far) == -1);
  Check(PortMapPickups::DotIndex(0x12345678u, dots[0].area, dots[0].pos) == -1);

  // The seen bitset: each dot has its own bit, setting is idempotent.
  uint32_t words[PortMapPickups::kSeenWords] = {};
  for (size_t i = 0; i < count; ++i) {
    Check(!PortMapPickups::IsSeen(words, i));
  }
  for (size_t i = 0; i < count; i += 3) {
    PortMapPickups::SetSeen(words, i);
    PortMapPickups::SetSeen(words, i);
  }
  for (size_t i = 0; i < count; ++i) {
    Check(PortMapPickups::IsSeen(words, i) == (i % 3 == 0));
  }
  PortMapPickups::SetSeen(words, PortMapPickups::kMaxDots); // out of range: ignored
  Check(!PortMapPickups::IsSeen(words, PortMapPickups::kMaxDots));
  std::puts("map pickup dot matching and seen bitset passed");
}
