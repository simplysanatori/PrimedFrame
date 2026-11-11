#ifndef _PORTRATE_HPP
#define _PORTRATE_HPP

// Port: per-tick logic that retail wrote for 60 Hz ticks, kept at the same real-time behaviour at
// any simulation rate. `frames` is the tick's length in 60 Hz frames (PortDebug::TickFrames():
// 1 at 60 Hz, 2/3 at 90 Hz, 1/2 at 120 Hz). Every helper returns retail's value at 60 Hz exactly.

#include <math.h>

namespace PortRate {

// A multiplier applied once per tick (damping, decay): the same decay per second at any rate.
inline float PerTickFactor(float factor, float frames) {
  return frames == 1.f ? factor : powf(factor, frames);
}

// An amount added once per tick (a fade step, an angle step): the same amount per second.
inline float PerTickStep(float step, float frames) { return step * frames; }

// A count of ticks compared against a threshold authored in 60 Hz frames.
inline bool TicksReach(unsigned ticks, float thresholdFrames, float frames) {
  return static_cast< float >(ticks) * frames >= thresholdFrames;
}

// Underwater projectiles run at 0.625 of real time. Retail writes the step as 37.5 * dt^2, which is
// 0.625 * dt only at dt = 1/60 (at 90 Hz it gave 0.42, at 120 Hz 0.31: slower shots).
inline float UnderwaterProjectileDt(float dt) { return 37.5f * (dt * (1.f / 60.f)); }

} // namespace PortRate

#endif // _PORTRATE_HPP
