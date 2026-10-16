#include "port_dynamic_res.h"

#include <algorithm>
#include <cmath>

namespace PortDynamicRes {
namespace {

// Below this share of the target the second counts as slow; at or above the
// other it counts as on target.
constexpr double kSlow = 0.93;
constexpr double kOnTarget = 0.97;
// The GPU is what holds the frame back only when it is busy for at least this
// share of the frame budget; otherwise the CPU (or a stall) is.
constexpr double kGpuBusy = 0.8;
// A new scale is aimed at this share of the budget, leaving room for heavier
// moments without dropping again.
constexpr double kHeadroom = 0.85;
// Scales are picked on this grid, and a change smaller than kMinStep is not
// worth the reallocation stutter (except to reach the top or the bottom).
constexpr float kGrid = 0.05f;
constexpr float kMinStep = 0.1f;
// Seconds skipped after a change: the stall itself, then one more so the GPU times
// (an average published every 60 frames or half a second) hold only the new scale.
constexpr int kSettleFps = 1;
constexpr int kSettleGpu = 2;
constexpr float kMaxRaise = 0.5f;

bool Worth(float from, float to, float top, float bottom) {
  const float d = std::fabs(to - from);
  if (d >= kMinStep - 1e-4f) {
    return true;
  }
  return d > 1e-3f && (to == top || to == bottom);
}

} // namespace

const char* ReasonText(Reason reason) {
  switch (reason) {
  case Reason::Off: return "off";
  case Reason::Settling: return "settling after a change";
  case Reason::Paused: return "paused or loading";
  case Reason::Collecting: return "waiting for GPU times";
  case Reason::Steady: return "steady";
  case Reason::NotGpuBound: return "below target, but not GPU-bound";
  case Reason::GpuBound: return "lowered: GPU-bound";
  case Reason::AtLowest: return "GPU-bound at the lowest scale";
  case Reason::Headroom: return "raised: GPU has headroom";
  case Reason::RaiseFailed: return "raise was too slow, undone";
  case Reason::NoGain: return "lowering gained nothing, undone";
  }
  return "?";
}

void Controller::Reset(float top) {
  *this = Controller();
  mScale = top;
}

Decision Controller::Change(float next, Reason reason, int settle) {
  mScale = next;
  mReason = reason;
  mSettle = settle;
  mSlow = 0;
  return {mScale, true, reason};
}

void Controller::Learn(float scale, double gpuMs) {
  // Two seconds at scales far enough apart give the fixed part:
  // gpu = fixed + perPixel * scale^2.
  if (mMeasuredScale > 0.f && std::fabs(scale - mMeasuredScale) >= kMinStep - 1e-4f) {
    const double s1 = double(mMeasuredScale) * mMeasuredScale;
    const double s2 = double(scale) * scale;
    const double perPixel = (mMeasuredMs - gpuMs) / (s1 - s2);
    if (perPixel > 0.0) {
      const double fixed = gpuMs - perPixel * s2;
      // A noisy pair can put it outside what one frame holds; keep what is plausible.
      if (fixed >= 0.0 && fixed < std::min(gpuMs, mMeasuredMs)) {
        mFixedMs = fixed;
      }
    }
  }
  mMeasuredScale = scale;
  mMeasuredMs = gpuMs;
}

double Controller::Predict(float from, double gpuMs, float to) const {
  const double fixed = mFixedMs >= 0.0 ? std::min(mFixedMs, gpuMs * 0.9) : 0.0;
  const double perPixel = (gpuMs - fixed) / (double(from) * from);
  return fixed + perPixel * double(to) * to;
}

float Controller::FitBudget(float from, double gpuMs, double budgetMs, float top, float bottom) const {
  // The largest scale on the grid (or the top itself) predicted to fit.
  const double allowed = budgetMs * kHeadroom;
  if (Predict(from, gpuMs, top) <= allowed) {
    return top;
  }
  for (int k = int(std::floor((top - 1e-4f) / kGrid)); k * kGrid >= bottom - 1e-4f; --k) {
    const float s = k * kGrid;
    if (Predict(from, gpuMs, s) <= allowed) {
      return std::max(s, bottom);
    }
  }
  return bottom;
}

Decision Controller::Update(const Sample& s) {
  const float top = s.top;
  const float bottom = std::min(s.bottom, top);
  if (top <= bottom) {
    if (mScale != top) {
      return Change(top, Reason::Off, 0);
    }
    mReason = Reason::Off;
    return {mScale, false, mReason};
  }
  // The range moved under it (the render scale was changed).
  if (mScale > top || mScale < bottom) {
    return Change(std::clamp(mScale, bottom, top), Reason::Off, kSettleFps);
  }
  if (mSettle > 0) {
    --mSettle;
    mReason = Reason::Settling;
    return {mScale, false, mReason};
  }
  // GPU times are coming but none is published yet: wait rather than guess.
  if (s.gpuMs == 0.0) {
    mReason = Reason::Collecting;
    return {mScale, false, mReason};
  }
  // Few frames is a pause or a load, unless the GPU was busy for most of the second:
  // then it is a scene too heavy for it, which is exactly when to lower.
  const bool gpuFull = s.gpuMs > 0.0 && s.gpuMs * s.frames >= 700.0;
  if ((s.frames < 10 && !gpuFull) || s.targetFps <= 0.0) {
    mReason = Reason::Paused;
    return {mScale, false, mReason};
  }
  return s.gpuMs > 0.0 ? UpdateFromGpu(s, top, bottom) : UpdateFromFps(s, top, bottom);
}

Decision Controller::UpdateFromGpu(const Sample& s, float top, float bottom) {
  Learn(mScale, s.gpuMs);
  const double budget = 1000.0 / s.targetFps;
  const bool slow = s.fps < s.targetFps * kSlow;
  const float fit = FitBudget(mScale, s.gpuMs, budget, top, bottom);

  if (mProbation >= 0) {
    if (slow && s.gpuMs >= budget * kGpuBusy) {
      // The raise was too much after all: back to where it was, and wait longer next time.
      mRaiseAfter = std::min(mRaiseAfter * 2, 64);
      mProbation = -1;
      mSteady = 0;
      return Change(mBeforeRaise, Reason::RaiseFailed, kSettleGpu);
    }
    if (++mProbation >= 3) {
      mProbation = -1;
    }
  }

  if (slow) {
    mSteady = 0;
    if (s.gpuMs < budget * kGpuBusy) {
      // The GPU had time to spare: fewer pixels would not bring the frames back.
      mSlow = 0;
      mReason = Reason::NotGpuBound;
      return {mScale, false, mReason};
    }
    // Two slow seconds in a row, so a single hitch does not cost a stutter.
    if (++mSlow >= 2 && fit < mScale && Worth(mScale, fit, top, bottom)) {
      return Change(fit, Reason::GpuBound, kSettleGpu);
    }
    mReason = mScale <= bottom ? Reason::AtLowest : fit < mScale ? Reason::GpuBound : Reason::NotGpuBound;
    return {mScale, false, mReason};
  }

  mSlow = 0;
  if (mProbation < 0 && s.fps >= s.targetFps * kOnTarget && mScale < top && ++mSteady >= mRaiseAfter) {
    mSteady = 0;
    const float next = std::min(fit, mScale + kMaxRaise);
    if (next > mScale && Worth(mScale, next, top, bottom)) {
      mBeforeRaise = mScale;
      mProbation = 0;
      return Change(next, Reason::Headroom, kSettleGpu);
    }
  }
  mReason = Reason::Steady;
  return {mScale, false, mReason};
}

// Without GPU times: the frame rate alone. Down a step after two slow seconds;
// a step down that gains nothing means the GPU was not the limit, so it goes
// back up and holds there; up a step after a steady stretch that doubles each
// time a raise turns out too slow.
Decision Controller::UpdateFromFps(const Sample& s, float top, float bottom) {
  const float scale = mScale;
  // Half steps above 2x, where a quarter step changes the pixel count by little.
  const float stepUp = scale >= 2.f ? 0.5f : 0.25f;
  const float stepDown = scale > 2.f ? 0.5f : 0.25f;
  const double target = s.targetFps;
  if (mHoldLow > 0) {
    --mHoldLow;
  }
  float next = scale;
  Reason reason = Reason::Steady;
  if (mFpsBefore > 0.0) {
    if (s.fps < mFpsBefore * 1.03) {
      next = scale + stepUp;
      reason = Reason::NoGain;
      mHoldLow = mHoldFor;
      mHoldFor = std::min(mHoldFor * 2, 240);
    }
    mFpsBefore = 0.0;
  } else if (s.fps < target * kSlow) {
    mSteady = 0;
    if (mProbation >= 0) {
      next = scale - stepDown;
      reason = Reason::RaiseFailed;
      mRaiseAfter = std::min(mRaiseAfter * 2, 64);
      mProbation = -1;
    } else if (++mSlow >= 2 && mHoldLow == 0 && scale > bottom) {
      next = scale - stepDown;
      reason = Reason::GpuBound;
      mFpsBefore = s.fps;
    }
  } else {
    mSlow = 0;
    if (mProbation >= 0 && ++mProbation >= 3) {
      mProbation = -1;
    }
    if (mProbation < 0 && s.fps >= target * kOnTarget && scale < top && ++mSteady >= mRaiseAfter) {
      next = scale + stepUp;
      reason = Reason::Headroom;
      mSteady = 0;
      mProbation = 0;
    }
  }
  next = std::clamp(next, bottom, top);
  if (next != scale) {
    return Change(next, reason, kSettleFps);
  }
  mReason = reason;
  return {mScale, false, mReason};
}

} // namespace PortDynamicRes
