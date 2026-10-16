#pragma once

// Dynamic resolution: picks the EFB scale once a second so the frame rate holds
// its target. Kept apart from the overlay so it can be tested on recorded or
// made-up seconds (tests/port_dynamic_res.cpp); platform/debug_ui/options.inc
// feeds it and applies what it returns.
//
// With the GPU's own time per frame (timestamp queries, Aurora's gpu_prof) it
// knows whether the GPU is what holds the frame back, and by how much: GPU time
// is taken as a fixed part plus a part that grows with the pixel count (the
// scale squared), so it can go straight to the scale that fits the frame budget.
// Each change reallocates the EFB and stutters, so it moves only when the
// difference is worth it, down quickly and up slowly. Without GPU times it falls
// back to stepping on the frame rate alone.

namespace PortDynamicRes {

// One second of frames.
struct Sample {
  double fps = 0.0;       // presented frames per wall-clock second
  unsigned frames = 0;    // presented frames in it
  // GPU time per frame (gpu_prof's span: first pass start to last pass end); negative
  // when the GPU cannot time itself, 0 while the first average is still being taken.
  double gpuMs = -1.0;
  double targetFps = 60.0;
  float top = 1.f;        // the configured scale: the most it draws at
  float bottom = 1.f;     // the lowest scale allowed
};

enum class Reason {
  Off,          // nothing to do (no range, or just reset)
  Settling,     // the second after a change holds its stall
  Paused,       // too few frames to judge
  Collecting,   // GPU times are on but not published yet
  Steady,       // at the target, no change worth making
  NotGpuBound,  // below the target, but the GPU has time to spare
  GpuBound,     // lowered: the GPU could not fit the frame budget
  AtLowest,     // GPU-bound, but already at the lowest scale allowed
  Headroom,     // raised: the GPU has room for more pixels
  RaiseFailed,  // a raise turned out too slow and was undone
  NoGain,       // (frame rate only) a lowering gained nothing and was undone
};
const char* ReasonText(Reason reason);

struct Decision {
  float scale = 1.f;     // the scale to draw at
  bool changed = false;  // differs from the previous second's
  Reason reason = Reason::Off;
};

class Controller {
public:
  // Back to the top scale with nothing learnt.
  void Reset(float top);
  Decision Update(const Sample& sample);

  float Scale() const { return mScale; }
  Reason LastReason() const { return mReason; }
  // The share of GPU time found not to depend on the scale, in ms; -1 until two
  // scales have been measured.
  double FixedGpuMs() const { return mFixedMs; }

private:
  Decision UpdateFromGpu(const Sample& s, float top, float bottom);
  Decision UpdateFromFps(const Sample& s, float top, float bottom);
  Decision Change(float next, Reason reason, int settle);
  void Learn(float scale, double gpuMs);
  double Predict(float from, double gpuMs, float to) const;
  float FitBudget(float from, double gpuMs, double budgetMs, float top, float bottom) const;

  float mScale = 1.f;
  Reason mReason = Reason::Off;
  int mSettle = 0;
  int mSlow = 0;
  int mSteady = 0;
  int mRaiseAfter = 4;   // steady seconds before a raise; doubles when one fails
  int mProbation = -1;   // seconds since the last raise, while it may still be undone
  float mBeforeRaise = 0.f;
  // GPU model: the last measurement at a different scale, and the fixed part.
  float mMeasuredScale = 0.f;
  double mMeasuredMs = 0.0;
  double mFixedMs = -1.0;
  // Frame-rate-only fallback.
  double mFpsBefore = 0.0;
  int mHoldLow = 0;
  int mHoldFor = 30;
};

} // namespace PortDynamicRes
