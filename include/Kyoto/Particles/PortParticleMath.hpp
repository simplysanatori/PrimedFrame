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

// FRECPE as the Switch runs it: ARM RecipEstimate's 8-fraction-bit integer construction (no
// FEAT_RPRES), round to nearest, denormals not flushed.
inline float ReciprocalEstimate(float v) {
  const uint32_t bits = FloatBits(v);
  const uint32_t sign = bits & 0x80000000;
  int exponent = int((bits >> 23) & 255);
  uint32_t fraction = bits & 0x7fffff;
  if (exponent == 255) {
    return fraction ? BitsFloat(bits | 0x400000) : BitsFloat(sign);
  }
  // Zero, and anything below 2^-128, overflows to infinity.
  if ((bits & 0x7fffffff) < 0x200000) {
    return BitsFloat(sign | 0x7f800000);
  }
  if (exponent == 0) {
    if ((fraction & 0x400000) == 0) {
      exponent = -1;
      fraction = (fraction << 2) & 0x7fffff;
    } else {
      fraction = (fraction << 1) & 0x7fffff;
    }
  }
  const uint32_t scaled = 256 | (fraction >> 15);
  const uint32_t estimate = ((1u << 19) / (scaled * 2 + 1) + 1) / 2;
  int resultExponent = 253 - exponent;
  uint32_t resultFraction = (estimate & 255) << 15;
  if (resultExponent == 0) {
    resultFraction = 0x400000 | (resultFraction >> 1);
  } else if (resultExponent == -1) {
    resultFraction = 0x200000 | (resultFraction >> 2);
    resultExponent = 0;
  }
  return BitsFloat(sign | (uint32_t(resultExponent) << 23) | resultFraction);
}

// FRECPS: fused 2 - a*b; zero times infinity gives 2.
inline float ReciprocalStep(float a, float b) {
  if ((a == 0.f && std::isinf(b)) || (b == 0.f && std::isinf(a))) {
    return 2.f;
  }
  return std::fma(-a, b, 2.f);
}

// One component of CCoordsExt::UpdateInverseScale 0x2a6390: FRECPE and two FRECPS refinements.
inline float InverseScale(float s) {
  float e = ReciprocalEstimate(s);
  e *= ReciprocalStep(e, s);
  return e * ReciprocalStep(e, s);
}

// CMath::SqrtF 0x65f70: 0 for v <= 0, else v times twice-refined FRSQRTE of max(v, 1e-35).
inline float SqrtF(float v) {
  if (!(v > 0.f)) {
    return 0.f;
  }
  const float m = std::fmax(v, BitsFloat(0x554ad2e));
  float e = ReciprocalSqrtEstimate(m);
  e *= ReciprocalSqrtStep(m, e * e);
  e *= ReciprocalSqrtStep(m, e * e);
  return v * e;
}

// NORM (CVENormalizeVector::GetValue 0x29e620): scaled by 1/|v| (CVector3f::Magnitude 0x6b218,
// (x*x + y*y) + z*z through SqrtF) when |v| > 2^-23, else unchanged.
inline CVector3f NormalizeVector(const CVector3f& v) {
  const float xx = v.GetX() * v.GetX();
  const float yy = v.GetY() * v.GetY();
  const float zz = v.GetZ() * v.GetZ();
  const float xy = xx + yy;
  const float magnitude = SqrtF(xy + zz);
  if (!(magnitude > BitsFloat(0x34000000))) {
    return v;
  }
  const float inverse = 1.f / magnitude;
  return CVector3f(inverse * v.GetX(), inverse * v.GetY(), inverse * v.GetZ());
}

// CMath::InvSqrtF 0x66090: twice-refined FRSQRTE of max(v, 1e-35).
inline float InvSqrtF(float v) {
  const float m = std::fmax(v, BitsFloat(0x554ad2e));
  float e = ReciprocalSqrtEstimate(m);
  e *= ReciprocalSqrtStep(m, e * e);
  return e * ReciprocalSqrtStep(m, e * e);
}

// CParticleValues::skMinNormalizableMagSqd (0x21109c8).
constexpr float kMinNormalizableMagSqd = 1.4210854715202004e-14f;

// CParticleMath::CrossIntoMatrixRow 0x286d70: a x b scaled by InvSqrtF of its squared length;
// false (out untouched) when that is below kMinNormalizableMagSqd.
inline bool NormalizedCross(const CVector3f& a, const CVector3f& b, CVector3f& out) {
  const float a1b2 = a.GetY() * b.GetZ(), a2b1 = a.GetZ() * b.GetY();
  const float a2b0 = a.GetZ() * b.GetX(), a0b2 = a.GetX() * b.GetZ();
  const float a0b1 = a.GetX() * b.GetY(), a1b0 = a.GetY() * b.GetX();
  const float cx = a1b2 - a2b1, cy = a2b0 - a0b2, cz = a0b1 - a1b0;
  const float xx = cx * cx, yy = cy * cy, zz = cz * cz;
  const float xy = xx + yy;
  const float squared = zz + xy;
  if (squared < kMinNormalizableMagSqd) {
    return false;
  }
  const float s = InvSqrtF(squared);
  out = CVector3f(cx * s, cy * s, cz * s);
  return true;
}

// RAZY (CERotationFromAxis::GetValue 0x29d298 as GetRotationElement 0x25e418 builds it: axis pair
// 1, normalised row 2, second pair 1): Z = the first vector normalised, X = normalize(second x Z),
// Y = normalize(Z x X), each under kMinNormalizableMagSqd or false (the caller's fallback). The
// rotation is conj(FromMatrix(rows X, Y, Z)), the one whose columns are X, Y, Z.
inline bool RotationFromAxisZY(const CVector3f& first, const CVector3f& second, CVector3f& x,
                               CVector3f& y, CVector3f& z) {
  const float xx = first.GetX() * first.GetX();
  const float yy = first.GetY() * first.GetY();
  const float zz = first.GetZ() * first.GetZ();
  const float xy = xx + yy;
  const float squared = xy + zz;
  if (squared < kMinNormalizableMagSqd) {
    return false;
  }
  const float s = InvSqrtF(squared);
  z = CVector3f(s * first.GetX(), s * first.GetY(), s * first.GetZ());
  return NormalizedCross(second, z, x) && NormalizedCross(z, x, y);
}

// CPSS (CVECameraPositionInSystemSpace::GetValue 0x2a24fc): the camera position in the system's
// global frame, conj(q) * (camera - t) * q with q = (w, x, y, z) unnormalised, then times
// InverseScale(s) per axis. Separate statements: Remastered uses unfused fmul/fadd/fsub.
inline CVector3f CameraPositionInSystemSpace(const CVector3f& camera, const CVector3f& t,
                                             float w, float x, float y, float z,
                                             const CVector3f& s) {
  const float d0 = camera.GetX() - t.GetX();
  const float d1 = camera.GetY() - t.GetY();
  const float d2 = camera.GetZ() - t.GetZ();
  const float d0x = d0 * x, d1y = d1 * y, d2z = d2 * z;
  const float dot = (d0x + d1y) + d2z;
  const float d0w = d0 * w, d2y = d2 * y, d1z = d1 * z;
  const float b = d1z + (d0w - d2y);
  const float d1w = d1 * w, d0z = d0 * z, d2x = d2 * x;
  const float c = d2x + (d1w - d0z);
  const float d2w = d2 * w, d1x = d1 * x, d0y = d0 * y;
  const float e = d0y + (d2w - d1x);
  const float wb = w * b, xa = x * dot, zc = z * c, ye = y * e;
  const float ya = y * dot, za = z * dot, zb = z * b, yb = y * b;
  const float we = w * e, wc = w * c, xe = x * e, xc = x * c;
  const float outX = (zc + (xa + wb)) - ye;
  const float outY = (xe + (ya + wc)) - zb;
  const float outZ = (yb + (za + we)) - xc;
  return CVector3f(InverseScale(s.GetX()) * outX, InverseScale(s.GetY()) * outY,
                   InverseScale(s.GetZ()) * outZ);
}

} // namespace PortParticleMath

#endif
