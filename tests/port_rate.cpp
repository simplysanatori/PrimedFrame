// Per-tick logic at simulation rates other than 60 Hz (include/MetroidPrime/PortRate.hpp and the
// projectile frame stepper in src/Weapons/CProjectileWeapon.cpp): one second of game time gives
// the same result at 60, 72, 90 and 120 steps per second, and 60 Hz is retail's arithmetic exactly.
#include "MetroidPrime/PortRate.hpp"
#include "port_test.h"

#include <cmath>
#include <cstring>

namespace {

const float kRates[] = {60.f, 72.f, 90.f, 120.f};

float TickFrames(float rate) {
  // What PortDebug::TickFrames() returns: the float tick period times 60.
  const float period = static_cast< float >(1.0 / static_cast< double >(rate));
  return period * 60.f;
}

bool SameBits(float a, float b) { return std::memcmp(&a, &b, sizeof(float)) == 0; }

// CProjectileWeapon::Update's frame stepper with the port's interpolation, for a shot moving
// `perFrame` units per 60 Hz frame. Returns the drawn position after the tick.
struct Shot {
  double curTime = 0.0;
  int curFrame = 0;
  float local = 0.f;
  float prev = 0.f;
  bool interpolating = false;
  float offset = 0.f;

  float Tick(float dt, float perFrame) {
    double actualTime = curFrame * (1.0 / 60.0);
    const bool close = std::fabs(dt - 1.f / 60.f) <= 1.6666666851961054e-5f;
    double useDt = close ? (1.0 / 60.0) : dt;
    curTime += useDt;
    while (actualTime < curTime && !(std::fabs(actualTime - curTime) <= 1.6666666666666667e-5)) {
      prev = local;
      local += perFrame;
      actualTime += 1.f / 60.f;
      ++curFrame;
    }
    if (std::fabs(actualTime - curTime) <= 1.6666666666666667e-5) {
      curTime = actualTime;
    }
    const double remainder = (actualTime - curTime) / (1.f / 60.0);
    interpolating = remainder != 0.0;
    if (interpolating) {
      offset = static_cast< float >(remainder) * (prev - local);
    }
    return interpolating ? local + offset : local;
  }
};

} // namespace

int main() {
  // 60 Hz is retail's arithmetic.
  {
    const float f = TickFrames(60.f);
    CHECK(f == 1.f);
    CHECK(SameBits(PortRate::PerTickFactor(0.8f, f), 0.8f));
    CHECK(SameBits(PortRate::PerTickFactor(0.99f, f), 0.99f));
    CHECK(SameBits(PortRate::PerTickStep(0.03f, f), 0.03f));
    const float dt = static_cast< float >(1.0 / 60.0);
    CHECK(SameBits(PortRate::UnderwaterProjectileDt(dt), 37.5f * (dt * dt)));
    CHECK(PortRate::TicksReach(30, 30.f, f) && !PortRate::TicksReach(29, 30.f, f));
  }

  for (float rate : kRates) {
    const float f = TickFrames(rate);
    const float dt = static_cast< float >(1.0 / static_cast< double >(rate));
    const int ticks = static_cast< int >(std::lround(rate)); // one second

    // A per-tick damping (half-pipe drag, underwater slope damping): same decay per second.
    float v = 10.f;
    for (int i = 0; i < ticks; ++i) {
      v *= PortRate::PerTickFactor(0.99f, f);
    }
    CHECK_MSG(std::fabs(v - 10.f * std::pow(0.99f, 60.f)) < 1e-3f, std::to_string(rate));

    // A per-tick fade step (boost light, Phazon shimmer): same amount per second.
    float fade = 0.f;
    for (int i = 0; i < ticks; ++i) {
      fade += PortRate::PerTickStep(0.03f, f);
    }
    CHECK_MSG(std::fabs(fade - 1.8f) < 1e-4f, std::to_string(rate));

    // Underwater shots run at 0.625 of real time at every rate.
    CHECK_MSG(std::fabs(PortRate::UnderwaterProjectileDt(dt) / dt - 0.625f) < 1e-5f,
              std::to_string(rate));

    // A swarm boid off its surface: updated every other tick, it gives up after 30 updates at
    // 60 Hz (one second); the same time at any rate.
    unsigned updates = 0;
    int boidTicks = 0;
    while (!PortRate::TicksReach(updates, 30.f, f)) {
      ++updates;
      boidTicks += 2;
    }
    CHECK_MSG(std::fabs(boidTicks / rate - 1.f) < 2.f / rate, std::to_string(rate));

    // A shot at 0.5 units per 60 Hz frame is drawn where real time puts it on every tick (to
    // within the frame stepper's rounding), and at 60 Hz exactly on its frames.
    Shot shot;
    float worst = 0.f;
    for (int i = 1; i <= ticks; ++i) {
      const float drawn = shot.Tick(dt, 0.5f);
      const float ideal = 0.5f * 60.f * (static_cast< float >(i) / rate);
      worst = std::fmax(worst, std::fabs(drawn - ideal));
      if (rate == 60.f) {
        CHECK(!shot.interpolating);
      }
    }
    CHECK_MSG(worst < 1e-3f, std::to_string(rate) + " Hz shot off by " + std::to_string(worst));
  }

  // Without the interpolation a 90 Hz shot holds still on every third tick (the judder fixed).
  {
    Shot shot;
    const float dt = static_cast< float >(1.0 / 90.0);
    int held = 0;
    float last = 0.f;
    for (int i = 0; i < 90; ++i) {
      shot.Tick(dt, 0.5f);
      held += shot.local == last ? 1 : 0;
      last = shot.local;
    }
    CHECK(held == 30);
  }

  return PORT_TEST_RESULT();
}
