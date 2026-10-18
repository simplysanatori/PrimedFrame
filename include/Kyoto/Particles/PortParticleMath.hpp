#pragma once

// Port-only particle element math that needs no engine state, so it can be tested alone.
// All follow Remastered (Switch 1.0.0): DistanceFromCameraBlend::CalculateBlendValue 0x298f44,
// CCEGradient::GetValue 0x2c8d48 and CVERandomVector::GetValue 0x29e954.
#ifdef TARGET_PC

#include <cmath>
#include <cstdint>
#include <cstring>

#include "Kyoto/Math/CTransform4f.hpp"
#include "Kyoto/Math/CVector3f.hpp"

namespace PortParticleMath {

// XFMD3 particles already carry the emitter's global orientation in their positions.
// Their center/sort transform therefore removes only the camera rotation; the legacy
// system-camera transform remains in use for XFMD2/default positions and directions.
inline CVector3f ParticlePositionForSort(const CTransform4f& cameraRotationInverse,
                                         const CTransform4f& systemCameraCopy,
                                         bool followsEmitter, const CVector3f& current,
                                         const CVector3f& previous, float deltaScale) {
  const CVector3f pos = (current - previous) * deltaScale + previous;
  const CTransform4f& positionCameraCopy = followsEmitter ? cameraRotationInverse : systemCameraCopy;
  return positionCameraCopy * pos;
}

inline CVector3f ParticleDirectionInCamera(const CTransform4f& systemCameraCopy,
                                           const CVector3f& direction) {
  return systemCameraCopy.Rotate(direction);
}

// DFCS/DFCP: 1 at distance a, 0 at distance b, clamped; 0 when a and b coincide.
inline float DistanceBlend(float d, float a, float b) {
  if (std::fabs(a - b) < 1e-5f) {
    return 0.f;
  }
  const float t = (d - b) / (a - b);
  return t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
}

struct GradientStop {
  float color[4];
  float position;
};

// GRAD: `frame` is the integer age in frames, `period` the int element's value (below 2 it is
// 1). The position is min(frame, period) / period, or (frame mod period) / period when it
// repeats; stops hold at both ends and every channel (alpha too) lerps linearly. Returns false
// with out untouched-to-zero when no stop brackets the position (Remastered returns 0 then).
inline bool EvalGradient(const GradientStop* stops, int count, int period, bool repeat, int frame, float out[4]) {
  if (period < 2) {
    period = 1;
  }
  float t;
  if (repeat) {
    int m = frame % period;
    if (m < 0) {
      m += period;
    }
    t = float(m) / float(period);
  } else {
    t = float(frame < period ? frame : period) / float(period);
  }
  for (int c = 0; c < 4; ++c) {
    out[c] = 0.f;
  }
  if (count <= 0) {
    return false;
  }
  if (t <= stops[0].position) {
    for (int c = 0; c < 4; ++c) {
      out[c] = stops[0].color[c];
    }
    return true;
  }
  for (int i = 1; i < count; ++i) {
    if (t <= stops[i].position) {
      const float a = stops[i - 1].position;
      const float b = stops[i].position;
      const float f = b > a ? (t - a) / (b - a) : 1.f;
      for (int c = 0; c < 4; ++c) {
        out[c] = stops[i - 1].color[c] + (stops[i].color[c] - stops[i - 1].color[c]) * f;
      }
      return true;
    }
  }
  for (int c = 0; c < 4; ++c) {
    out[c] = stops[count - 1].color[c];
  }
  return true;
}

inline uint32_t FloatBits(float f) {
  uint32_t b;
  std::memcpy(&b, &f, 4);
  return b;
}

inline float BitsFloat(uint32_t b) {
  float f;
  std::memcpy(&f, &b, 4);
  return f;
}

// FRSQRTE as the Switch runs it: ARM RecipSqrtEstimate's 8-fraction-bit integer construction
// (no FEAT_RPRES), not the host's rsqrt.
inline float ReciprocalSqrtEstimate(float v) {
  const uint32_t bits = FloatBits(v);
  int exponent = int((bits >> 23) & 255);
  uint32_t fraction = bits & 0x7fffff;
  if (exponent == 255) {
    return fraction ? BitsFloat(bits | 0x400000) : 0.f;
  }
  if ((bits & 0x7fffffff) == 0) {
    return BitsFloat((bits & 0x80000000) | 0x7f800000);
  }
  if (bits >> 31) {
    return BitsFloat(0x7fc00000);
  }
  if (exponent == 0) {
    while ((fraction & 0x400000) == 0) {
      fraction <<= 1;
      --exponent;
    }
    fraction = (fraction & 0x3fffff) << 1;
  }
  uint32_t scaled = (exponent & 1) ? (128 | (fraction >> 16)) : (256 | (fraction >> 15));
  scaled = scaled < 256 ? scaled * 2 + 1 : ((scaled & ~1u) + 1) * 2;
  uint32_t b = 512;
  while (scaled * (b + 1) * (b + 1) < (1u << 28)) {
    ++b;
  }
  return BitsFloat((uint32_t((380 - exponent) / 2) << 23) | (((b + 1) / 2 & 255) << 15));
}

// FRSQRTS: fused (3 - a*b) / 2; zero times infinity gives 1.5.
inline float ReciprocalSqrtStep(float a, float b) {
  if ((a == 0.f && std::isinf(b)) || (b == 0.f && std::isinf(a))) {
    return 1.5f;
  }
  return std::fma(-a, b, 3.f) * .5f;
}

// RNDV (CVERandomVector::GetValue 0x29e954): three draws in [0, 1) (CRandom16::GetVector3f
// 0xba108) mapped to the cube [-1, 1)^3, scaled to `magnitude` by one refined FRSQRTE of
// (x*x + y*y) + z*z. A cube point, so the directions are not uniform over the sphere.
inline CVector3f RandomVector(float u0, float u1, float u2, float magnitude) {
  const float x = (u0 + u0) + -1.f;
  const float y = (u1 + u1) + -1.f;
  const float z = (u2 + u2) + -1.f;
  // Separate statements: Remastered uses unfused fmul/fadd, and clang contracts
  // within one expression on arm64.
  const float xx = x * x;
  const float yy = y * y;
  const float zz = z * z;
  const float xy = xx + yy;
  const float squared = xy + zz;
  float reciprocal = ReciprocalSqrtEstimate(squared);
  reciprocal *= ReciprocalSqrtStep(squared, reciprocal * reciprocal);
  const float scale = magnitude * reciprocal;
  return CVector3f(x * scale, y * scale, z * scale);
}

} // namespace PortParticleMath

#endif
