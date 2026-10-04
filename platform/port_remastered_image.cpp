// Image resizing and texture encoders for the Remastered importer
// (port_remastered_image.h).

#include "port_remastered_image.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace PortRemastered {
namespace {

// --- Resize -------------------------------------------------------------------

double Lanczos3(double x) {
  if (x < 0.0) {
    x = -x;
  }
  if (x >= 3.0) {
    return 0.0;
  }
  if (x == 0.0) {
    return 1.0;
  }
  const double a = x * 3.14159265358979323846;
  return 3.0 * std::sin(a) * std::sin(a / 3.0) / (a * a);
}

// The source span and normalised weights for each output texel along one
// axis, as Pillow's precompute_coeffs: on a reduction the filter widens with
// the scale, and a span cut by the edge is renormalised.
struct Taps {
  int width = 0;  // most taps any output has
  std::vector<int> first, count;
  std::vector<float> weights;  // width per output
};

Taps MakeTaps(int in, int out) {
  Taps taps;
  const double scale = double(in) / double(out);
  const double filterScale = std::max(scale, 1.0);
  const double support = 3.0 * filterScale;
  taps.width = int(std::ceil(support)) * 2 + 1;
  taps.first.resize(size_t(out));
  taps.count.resize(size_t(out));
  taps.weights.assign(size_t(out) * size_t(taps.width), 0.0f);
  std::vector<double> w(size_t(taps.width));
  for (int x = 0; x < out; ++x) {
    const double center = (x + 0.5) * scale;
    const int lo = std::max(0, int(center - support + 0.5));
    const int hi = std::min(in, int(center + support + 0.5));
    const int n = hi - lo;
    double sum = 0.0;
    for (int i = 0; i < n; ++i) {
      w[size_t(i)] = Lanczos3((i + lo - center + 0.5) / filterScale);
      sum += w[size_t(i)];
    }
    for (int i = 0; i < n; ++i) {
      taps.weights[size_t(x) * size_t(taps.width) + size_t(i)] = float(sum != 0.0 ? w[size_t(i)] / sum : 0.0);
    }
    taps.first[size_t(x)] = lo;
    taps.count[size_t(x)] = n;
  }
  return taps;
}

uint8_t ToByte(float v) {
  return uint8_t(std::clamp(int(std::lround(v)), 0, 255));
}

// --- Mips ---------------------------------------------------------------------

// Each level is made from the one above, not from the top: a 2:1 Lanczos step
// costs a fraction of a reduction from full size and looks the same.
Image HalfOf(const Image& image) {
  return Resize(image, std::max(image.width / 2, 1), std::max(image.height / 2, 1));
}

void Put16(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(uint8_t(v >> 8));
  out.push_back(uint8_t(v));
}

void Put32(std::vector<uint8_t>& out, uint32_t v) {
  Put16(out, v >> 16);
  Put16(out, v & 0xFFFF);
}

void Put32LE(std::vector<uint8_t>& out, uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(uint8_t(v >> (8 * i)));
  }
}

// --- CMPR ---------------------------------------------------------------------

// A float colour (0..255) as packed RGB565 and its expansion as the GX
// decoder does it.
uint32_t To565(const float* c, int* e) {
  static const int kMax[3] = {31, 63, 31};
  int q[3];
  for (int i = 0; i < 3; ++i) {
    q[i] = std::clamp(int(std::nearbyint(c[i] * float(kMax[i]) / 255.0f)), 0, kMax[i]);
  }
  e[0] = (q[0] << 3) | (q[0] >> 2);
  e[1] = (q[1] << 2) | (q[1] >> 4);
  e[2] = (q[2] << 3) | (q[2] >> 2);
  return uint32_t((q[0] << 11) | (q[1] << 5) | q[2]);
}

// One DXT1 block the GX way: big-endian colours, the first texel in the top
// bits. The endpoints are the ends of the block's principal colour axis. A
// block with a texel under alpha 128 uses the 3-colour mode, whose index 3 is
// transparent.
void CmprBlock(const uint8_t px[16][4], uint8_t* out) {
  bool opaque[16];
  bool punch = false;
  float mean[3] = {0, 0, 0};
  float n = 0;
  for (int i = 0; i < 16; ++i) {
    opaque[i] = px[i][3] >= 128;
    punch = punch || !opaque[i];
    if (opaque[i]) {
      for (int c = 0; c < 3; ++c) {
        mean[c] += float(px[i][c]);
      }
      n += 1;
    }
  }
  n = std::max(n, 1.0f);
  for (float& m : mean) {
    m /= n;
  }
  float cov[3][3] = {};
  for (int i = 0; i < 16; ++i) {
    if (opaque[i]) {
      float d[3];
      for (int c = 0; c < 3; ++c) {
        d[c] = float(px[i][c]) - mean[c];
      }
      for (int a = 0; a < 3; ++a) {
        for (int b = 0; b < 3; ++b) {
          cov[a][b] += d[a] * d[b];
        }
      }
    }
  }
  float ax[3] = {1, 1, 1};
  for (int it = 0; it < 8; ++it) {  // power iteration
    float v[3];
    for (int a = 0; a < 3; ++a) {
      v[a] = cov[a][0] * ax[0] + cov[a][1] * ax[1] + cov[a][2] * ax[2];
    }
    const float len = std::max(std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]), 1e-6f);
    for (int a = 0; a < 3; ++a) {
      ax[a] = v[a] / len;
    }
  }
  float tmin = 0, tmax = 0;
  bool any = false;
  for (int i = 0; i < 16; ++i) {
    if (opaque[i]) {
      float t = 0;
      for (int c = 0; c < 3; ++c) {
        t += (float(px[i][c]) - mean[c]) * ax[c];
      }
      tmin = any ? std::min(tmin, t) : t;
      tmax = any ? std::max(tmax, t) : t;
      any = true;
    }
  }
  float c0[3], c1[3];
  for (int c = 0; c < 3; ++c) {
    c0[c] = mean[c] + ax[c] * tmax;
    c1[c] = mean[c] + ax[c] * tmin;
  }
  int e0[3], e1[3];
  uint32_t v0 = To565(c0, e0), v1 = To565(c1, e1);
  // 4-colour mode needs c0 > c1, the 3-colour (punch-through) mode c0 <= c1
  if (punch ? v0 > v1 : v0 < v1) {
    std::swap(v0, v1);
    std::swap(e0, e1);
  }
  int pal[4][3];
  for (int c = 0; c < 3; ++c) {
    pal[0][c] = e0[c];
    pal[1][c] = e1[c];
    if (punch) {
      pal[2][c] = pal[3][c] = (e0[c] + e1[c]) >> 1;
    } else {
      pal[2][c] = (5 * e0[c] + 3 * e1[c]) >> 3;
      pal[3][c] = (3 * e0[c] + 5 * e1[c]) >> 3;
    }
  }
  uint8_t idx[16];
  for (int i = 0; i < 16; ++i) {
    int best = 0, bestDist = 1 << 30;
    for (int k = 0; k < (punch ? 3 : 4); ++k) {
      int dist = 0;
      for (int c = 0; c < 3; ++c) {
        const int d = int(px[i][c]) - pal[k][c];
        dist += d * d;
      }
      if (dist < bestDist) {
        bestDist = dist;
        best = k;
      }
    }
    if (punch && !opaque[i]) {
      best = 3;
    } else if (v0 == v1) {  // equal ends read as 3-colour
      best = 0;
    }
    idx[i] = uint8_t(best);
  }
  out[0] = uint8_t(v0 >> 8);
  out[1] = uint8_t(v0);
  out[2] = uint8_t(v1 >> 8);
  out[3] = uint8_t(v1);
  for (int r = 0; r < 4; ++r) {
    out[4 + r] = uint8_t((idx[r * 4] << 6) | (idx[r * 4 + 1] << 4) | (idx[r * 4 + 2] << 2) | idx[r * 4 + 3]);
  }
}

// --- BC4 (one channel of BC5) -------------------------------------------------

void Bc4Block(const uint8_t* rgba, int channel, uint8_t* out) {
  int lo = 255, hi = 0;
  for (int i = 0; i < 16; ++i) {
    const int v = rgba[i * 4 + channel];
    lo = std::min(lo, v);
    hi = std::max(hi, v);
  }
  out[0] = uint8_t(hi);
  out[1] = uint8_t(lo);
  uint64_t bits = 0;
  if (hi != lo) {
    // red0 > red1: six values between them. Palette order is 0 = hi, 1 = lo,
    // then 2..7 stepping from hi down to lo.
    const int range = hi - lo;
    for (int i = 0; i < 16; ++i) {
      const int v = rgba[i * 4 + channel];
      const int step = ((hi - v) * 14 + range) / (2 * range);  // nearest of 0..7 from hi
      const int index = step == 0 ? 0 : step == 7 ? 1 : step + 1;
      bits |= uint64_t(index) << (3 * i);
    }
  }
  for (int i = 0; i < 6; ++i) {
    out[2 + i] = uint8_t(bits >> (8 * i));
  }
}

// --- BC7, mode 6 --------------------------------------------------------------

const int kWeights4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

struct Mode6 {
  int e[2][4];  // 8-bit endpoints (7 bits and the shared p-bit)
  uint8_t idx[16];
  int error = 0;
};

// The nearest endpoint a p-bit allows: every channel has that low bit.
void Quantise(const float* c, int p, int* e) {
  for (int k = 0; k < 4; ++k) {
    const int q = std::clamp(int(std::lround((c[k] - float(p)) / 2.0f)), 0, 127);
    e[k] = (q << 1) | p;
  }
}

float QuantError(const float* c, const int* e) {
  float err = 0;
  for (int k = 0; k < 4; ++k) {
    const float d = c[k] - float(e[k]);
    err += d * d;
  }
  return err;
}

void Endpoint(const float* c, bool opaque, int* e) {
  if (opaque) {
    Quantise(c, 1, e);  // only p = 1 reaches alpha 255
    return;
  }
  int a[4], b[4];
  Quantise(c, 0, a);
  Quantise(c, 1, b);
  std::memcpy(e, QuantError(c, a) <= QuantError(c, b) ? a : b, sizeof(a));
}

// Picks each texel's index for the endpoints in `m` and totals the error.
void Assign(const uint8_t* rgba, Mode6& m) {
  int pal[16][4];
  for (int i = 0; i < 16; ++i) {
    for (int k = 0; k < 4; ++k) {
      pal[i][k] = ((64 - kWeights4[i]) * m.e[0][k] + kWeights4[i] * m.e[1][k] + 32) >> 6;
    }
  }
  // The palette is a line, so the nearest entry is found from the projection
  // onto it and its two neighbours.
  float dir[4], len2 = 0;
  for (int k = 0; k < 4; ++k) {
    dir[k] = float(m.e[1][k] - m.e[0][k]);
    len2 += dir[k] * dir[k];
  }
  m.error = 0;
  for (int i = 0; i < 16; ++i) {
    const uint8_t* p = rgba + i * 4;
    int guess = 0;
    if (len2 > 0) {
      float t = 0;
      for (int k = 0; k < 4; ++k) {
        t += float(int(p[k]) - m.e[0][k]) * dir[k];
      }
      guess = std::clamp(int(t / len2 * 15.0f + 0.5f), 0, 15);
    }
    int best = guess, bestErr = 1 << 30;
    for (int j = std::max(guess - 1, 0); j <= std::min(guess + 1, 15); ++j) {
      int err = 0;
      for (int k = 0; k < 4; ++k) {
        const int d = int(p[k]) - pal[j][k];
        err += d * d;
      }
      if (err < bestErr) {
        bestErr = err;
        best = j;
      }
    }
    m.idx[i] = uint8_t(best);
    m.error += bestErr;
  }
}

// --- BC7, mode 1 --------------------------------------------------------------
//
// Two subsets of texels, each with its own pair of opaque colours: what a block
// needs when it holds two unrelated gradients, which one line cannot follow.

const int kWeights3[8] = {0, 9, 18, 27, 37, 46, 55, 64};

// The texels of the second subset, one bit each, for the 64 partitions.
const uint16_t kPartitions[64] = {
    0xCCCC, 0x8888, 0xEEEE, 0xECC8, 0xC880, 0xFEEC, 0xFEC8, 0xEC80, 0xC800, 0xFFEC, 0xFE80,
    0xE800, 0xFFE8, 0xFF00, 0xFFF0, 0xF000, 0xF710, 0x008E, 0x7100, 0x08CE, 0x008C, 0x7310,
    0x3100, 0x8CCE, 0x088C, 0x3110, 0x6666, 0x366C, 0x17E8, 0x0FF0, 0x718E, 0x399C, 0xAAAA,
    0xF0F0, 0x5A5A, 0x33CC, 0x3C3C, 0x55AA, 0x9696, 0xA55A, 0x73CE, 0x13C8, 0x324C, 0x3BDC,
    0x6996, 0xC33C, 0x9966, 0x0660, 0x0272, 0x04E4, 0x4E40, 0x2720, 0xC936, 0x936C, 0x39C6,
    0x639C, 0x9336, 0x9CC6, 0x817E, 0xE718, 0xCCF0, 0x0FCC, 0x7744, 0xEE22};

// The second subset's texel whose index is stored a bit short.
const uint8_t kAnchors[64] = {15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
                              15, 2,  8,  2,  2,  8,  8,  15, 2,  8,  2,  2,  8,  8,  2,  2,
                              15, 15, 6,  8,  2,  8,  15, 15, 2,  8,  2,  2,  2,  15, 15, 6,
                              6,  2,  6,  8,  15, 15, 2,  2,  15, 15, 15, 15, 15, 2,  2,  15};

struct Mode1 {
  int part = 0;
  int e[2][2][3];  // per subset: 8-bit endpoints (6 bits, a p-bit, the top bit again)
  uint8_t idx[16];
  int error = 0;
};

// The largest eigenvector of a symmetric 3x3 matrix (xx xy xz yy yz zz) and its
// eigenvalue, by power iteration.
float Axis3(const float* c, float* ax) {
  ax[0] = ax[1] = ax[2] = 0.577f;
  float value = 0;
  for (int it = 0; it < 4; ++it) {
    const float v[3] = {c[0] * ax[0] + c[1] * ax[1] + c[2] * ax[2],
                        c[1] * ax[0] + c[3] * ax[1] + c[4] * ax[2],
                        c[2] * ax[0] + c[4] * ax[1] + c[5] * ax[2]};
    value = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (value < 1e-6f) {
      return 0;
    }
    ax[0] = v[0] / value;
    ax[1] = v[1] / value;
    ax[2] = v[2] / value;
  }
  return value;
}

// The pair of endpoints nearest c0/c1 that share a p-bit.
void Quantise1(const float* c0, const float* c1, int e[2][3]) {
  float bestErr = 1e30f;
  for (int p = 0; p < 2; ++p) {
    int trial[2][3];
    float err = 0;
    for (int s = 0; s < 2; ++s) {
      const float* c = s ? c1 : c0;
      for (int k = 0; k < 3; ++k) {
        const int base = int(c[k] * (127.0f / 255.0f)) >> 1;
        float chErr = 1e30f;
        for (int q = std::max(base - 1, 0); q <= std::min(base + 1, 63); ++q) {
          const int v7 = (q << 1) | p, v = (v7 << 1) | (v7 >> 6);
          const float d = (c[k] - float(v)) * (c[k] - float(v));
          if (d < chErr) {
            chErr = d;
            trial[s][k] = v;
          }
        }
        err += chErr;
      }
    }
    if (err < bestErr) {
      bestErr = err;
      std::memcpy(e, trial, sizeof(trial));
    }
  }
}

int Assign1(const uint8_t* rgba, const int* texels, int n, const int e[2][3], uint8_t* idx) {
  int pal[8][3];
  for (int i = 0; i < 8; ++i) {
    for (int k = 0; k < 3; ++k) {
      pal[i][k] = ((64 - kWeights3[i]) * e[0][k] + kWeights3[i] * e[1][k] + 32) >> 6;
    }
  }
  int total = 0;
  for (int t = 0; t < n; ++t) {
    const uint8_t* p = rgba + texels[t] * 4;
    int best = 0, bestErr = 1 << 30;
    for (int j = 0; j < 8; ++j) {
      const int dr = int(p[0]) - pal[j][0], dg = int(p[1]) - pal[j][1], db = int(p[2]) - pal[j][2];
      const int err = dr * dr + dg * dg + db * db;
      if (err < bestErr) {
        bestErr = err;
        best = j;
      }
    }
    idx[texels[t]] = uint8_t(best);
    total += bestErr;
  }
  return total;
}

// Fits one subset: endpoints along its principal axis, then one least-squares
// pass over the indices that gives.
int FitSubset1(const uint8_t* rgba, const int* texels, int n, int e[2][3], uint8_t* idx) {
  float mean[3] = {0, 0, 0};
  for (int t = 0; t < n; ++t) {
    for (int k = 0; k < 3; ++k) {
      mean[k] += float(rgba[texels[t] * 4 + k]);
    }
  }
  for (float& m : mean) {
    m /= float(n);
  }
  float cov[6] = {};
  for (int t = 0; t < n; ++t) {
    const uint8_t* p = rgba + texels[t] * 4;
    const float d[3] = {float(p[0]) - mean[0], float(p[1]) - mean[1], float(p[2]) - mean[2]};
    cov[0] += d[0] * d[0];
    cov[1] += d[0] * d[1];
    cov[2] += d[0] * d[2];
    cov[3] += d[1] * d[1];
    cov[4] += d[1] * d[2];
    cov[5] += d[2] * d[2];
  }
  float ax[3];
  Axis3(cov, ax);
  float tmin = 1e30f, tmax = -1e30f;
  for (int t = 0; t < n; ++t) {
    const uint8_t* p = rgba + texels[t] * 4;
    const float v = (float(p[0]) - mean[0]) * ax[0] + (float(p[1]) - mean[1]) * ax[1] +
                    (float(p[2]) - mean[2]) * ax[2];
    tmin = std::min(tmin, v);
    tmax = std::max(tmax, v);
  }
  float c0[3], c1[3];
  for (int k = 0; k < 3; ++k) {
    c0[k] = std::clamp(mean[k] + ax[k] * tmin, 0.0f, 255.0f);
    c1[k] = std::clamp(mean[k] + ax[k] * tmax, 0.0f, 255.0f);
  }
  Quantise1(c0, c1, e);
  int error = Assign1(rgba, texels, n, e, idx);
  for (int pass = 0; pass < 2 && error > 0; ++pass) {
    float aa = 0, ab = 0, bb = 0, ra[3] = {0, 0, 0}, rb[3] = {0, 0, 0};
    for (int t = 0; t < n; ++t) {
      const float b = float(kWeights3[idx[texels[t]]]) / 64.0f, a = 1.0f - b;
      aa += a * a;
      ab += a * b;
      bb += b * b;
      for (int k = 0; k < 3; ++k) {
        ra[k] += a * float(rgba[texels[t] * 4 + k]);
        rb[k] += b * float(rgba[texels[t] * 4 + k]);
      }
    }
    const float det = aa * bb - ab * ab;
    if (std::fabs(det) < 1e-4f) {
      break;
    }
    for (int k = 0; k < 3; ++k) {
      c0[k] = std::clamp((ra[k] * bb - rb[k] * ab) / det, 0.0f, 255.0f);
      c1[k] = std::clamp((rb[k] * aa - ra[k] * ab) / det, 0.0f, 255.0f);
    }
    int trialE[2][3];
    uint8_t trialIdx[16];
    Quantise1(c0, c1, trialE);
    const int trial = Assign1(rgba, texels, n, trialE, trialIdx);
    if (trial >= error) {
      break;
    }
    error = trial;
    std::memcpy(e, trialE, sizeof(trialE));
    for (int t = 0; t < n; ++t) {
      idx[texels[t]] = trialIdx[texels[t]];
    }
  }
  return error;
}

// The best mode 1 encoding of an opaque block. The partition is chosen by how
// little each subset strays from its own principal axis, which costs a few sums
// per candidate; only the winner is fitted in full.
void FitMode1(const uint8_t* rgba, Mode1& m) {
  float px[16][9];
  float all[9] = {};
  for (int i = 0; i < 16; ++i) {
    const float r = float(rgba[i * 4]), g = float(rgba[i * 4 + 1]), b = float(rgba[i * 4 + 2]);
    const float v[9] = {r, g, b, r * r, r * g, r * b, g * g, g * b, b * b};
    for (int k = 0; k < 9; ++k) {
      px[i][k] = v[k];
      all[k] += v[k];
    }
  }
  float bestScore = 1e30f;
  for (int part = 0; part < 64; ++part) {
    float sum[2][9] = {};
    int count = 0;
    for (int i = 0; i < 16; ++i) {
      if (kPartitions[part] >> i & 1) {
        for (int k = 0; k < 9; ++k) {
          sum[1][k] += px[i][k];
        }
        ++count;
      }
    }
    for (int k = 0; k < 9; ++k) {
      sum[0][k] = all[k] - sum[1][k];
    }
    float score = 0;
    for (int s = 0; s < 2; ++s) {
      const float n = float(s ? count : 16 - count);
      const float* t = sum[s];
      const float cov[6] = {t[3] - t[0] * t[0] / n, t[4] - t[0] * t[1] / n, t[5] - t[0] * t[2] / n,
                            t[6] - t[1] * t[1] / n, t[7] - t[1] * t[2] / n, t[8] - t[2] * t[2] / n};
      float ax[3];
      score += cov[0] + cov[3] + cov[5] - Axis3(cov, ax);
    }
    if (score < bestScore) {
      bestScore = score;
      m.part = part;
    }
  }
  int texels[2][16], n[2] = {0, 0};
  for (int i = 0; i < 16; ++i) {
    const int s = kPartitions[m.part] >> i & 1;
    texels[s][n[s]++] = i;
  }
  m.error = FitSubset1(rgba, texels[0], n[0], m.e[0], m.idx) +
            FitSubset1(rgba, texels[1], n[1], m.e[1], m.idx);
  // Each subset's anchor index is stored without its top bit.
  const int anchor[2] = {0, kAnchors[m.part]};
  for (int s = 0; s < 2; ++s) {
    if (m.idx[anchor[s]] & 4) {
      std::swap(m.e[s][0], m.e[s][1]);
      for (int t = 0; t < n[s]; ++t) {
        m.idx[texels[s][t]] = uint8_t(7 - m.idx[texels[s][t]]);
      }
    }
  }
}

struct BitWriter {
  uint64_t lo = 0, hi = 0;
  int at = 0;

  void Put(uint32_t value, int bits) {
    if (at < 64) {
      lo |= uint64_t(value) << at;
      if (at + bits > 64) {
        hi |= uint64_t(value) >> (64 - at);
      }
    } else {
      hi |= uint64_t(value) << (at - 64);
    }
    at += bits;
  }

  void Store(uint8_t* out) const {
    for (int i = 0; i < 8; ++i) {
      out[i] = uint8_t(lo >> (8 * i));
      out[8 + i] = uint8_t(hi >> (8 * i));
    }
  }
};

// Mode 6 already this close (summed squared error) is not worth a second fit.
const int kMode1Threshold = 48;

// Fills the colour of a cut-out texture's transparent texels (black in
// Remastered's grass) from the nearest opaque ones, then their mean. Filtering
// and the smaller levels mix that colour into the edges, and black there shows
// as dark specks across distant grass.
void BleedColour(Image& image) {
  const int w = image.width, h = image.height;
  const size_t count = size_t(w) * size_t(h);
  std::vector<uint8_t> filled(count);
  double sum[3] = {0, 0, 0};
  size_t opaque = 0;
  for (size_t i = 0; i < count; ++i) {
    if (image.rgba[i * 4 + 3] >= 128) {
      filled[i] = 1;
      ++opaque;
      for (int c = 0; c < 3; ++c) {
        sum[c] += image.rgba[i * 4 + c];
      }
    }
  }
  if (opaque == 0 || opaque == count) {
    return;
  }
  // Up to 16 rings outwards, each from the four neighbours (wrapping, as the
  // textures tile) filled before it.
  std::vector<uint8_t> next;
  for (int ring = 0; ring < 16; ++ring) {
    next = filled;
    bool grew = false;
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        const size_t i = size_t(y) * size_t(w) + size_t(x);
        if (filled[i]) {
          continue;
        }
        const size_t around[4] = {size_t(y) * size_t(w) + size_t((x + 1) % w),
                                  size_t(y) * size_t(w) + size_t((x + w - 1) % w),
                                  size_t((y + 1) % h) * size_t(w) + size_t(x),
                                  size_t((y + h - 1) % h) * size_t(w) + size_t(x)};
        int acc[3] = {0, 0, 0}, n = 0;
        for (const size_t j : around) {
          if (filled[j]) {
            ++n;
            for (int c = 0; c < 3; ++c) {
              acc[c] += image.rgba[j * 4 + c];
            }
          }
        }
        if (n > 0) {
          for (int c = 0; c < 3; ++c) {
            image.rgba[i * 4 + c] = uint8_t((acc[c] + n / 2) / n);
          }
          next[i] = 1;
          grew = true;
        }
      }
    }
    if (!grew) {
      break;
    }
    filled.swap(next);
  }
  for (size_t i = 0; i < count; ++i) {
    if (!filled[i]) {
      for (int c = 0; c < 3; ++c) {
        image.rgba[i * 4 + c] = uint8_t(std::lround(sum[c] / double(opaque)));
      }
    }
  }
}

// Gives each smaller level of a cut-out (alpha tested at 128) texture a 1-bit
// alpha with the same share of opaque texels as the top level: opaque where its
// alpha is in that top share. Halving averages a thin stem's alpha under 128 a
// few levels down, so a fixed cut thins distant grass and fences to shimmering
// dashes. Levels are halved from the raw level above, so call this last.
void KeepCoverage(std::vector<Image>& levels) {
  const auto& top = levels[0].rgba;
  size_t opaque = 0;
  for (size_t i = 3; i < top.size(); i += 4) {
    opaque += top[i] >= 128;
  }
  const size_t texels = top.size() / 4;
  if (opaque == 0 || opaque == texels) {
    return;
  }
  for (size_t l = 1; l < levels.size(); ++l) {
    auto& a = levels[l].rgba;
    size_t histogram[256] = {};
    for (size_t i = 3; i < a.size(); i += 4) {
      ++histogram[a[i]];
    }
    const double target = double(opaque) / double(texels) * double(a.size() / 4);
    // The cut whose count of texels at or over it is closest to the target
    // (many texels can share one value, so none may hit it).
    int cut = 255;
    double best = target - double(histogram[255]);
    for (size_t above = histogram[255], c = 254; c >= 1; --c) {
      above += histogram[c];
      if (std::abs(double(above) - target) < std::abs(best)) {
        best = double(above) - target;
        cut = int(c);
      }
    }
    for (size_t i = 3; i < a.size(); i += 4) {
      a[i] = a[i] >= cut ? 255 : 0;
    }
  }
}

}  // namespace

Image Resize(const Image& image, int width, int height) {
  Image out;
  out.width = width;
  out.height = height;
  if (image.width == width && image.height == height) {
    out.rgba = image.rgba;
    return out;
  }
  // Across first, then down; the intermediate is 8-bit, as Pillow's is.
  const Taps tx = MakeTaps(image.width, width);
  std::vector<uint8_t> mid(size_t(width) * size_t(image.height) * 4);
  for (int y = 0; y < image.height; ++y) {
    const uint8_t* src = &image.rgba[size_t(y) * size_t(image.width) * 4];
    uint8_t* dst = &mid[size_t(y) * size_t(width) * 4];
    for (int x = 0; x < width; ++x) {
      const float* w = &tx.weights[size_t(x) * size_t(tx.width)];
      const uint8_t* s = src + size_t(tx.first[size_t(x)]) * 4;
      float acc[4] = {0, 0, 0, 0};
      for (int i = 0, n = tx.count[size_t(x)]; i < n; ++i, s += 4) {
        acc[0] += w[i] * float(s[0]);
        acc[1] += w[i] * float(s[1]);
        acc[2] += w[i] * float(s[2]);
        acc[3] += w[i] * float(s[3]);
      }
      for (int c = 0; c < 4; ++c) {
        dst[x * 4 + c] = ToByte(acc[c]);
      }
    }
  }
  const Taps ty = MakeTaps(image.height, height);
  out.rgba.resize(size_t(width) * size_t(height) * 4);
  std::vector<float> row(size_t(width) * 4);
  for (int y = 0; y < height; ++y) {
    std::fill(row.begin(), row.end(), 0.0f);
    const float* w = &ty.weights[size_t(y) * size_t(ty.width)];
    for (int i = 0, n = ty.count[size_t(y)]; i < n; ++i) {
      const uint8_t* s = &mid[size_t(ty.first[size_t(y)] + i) * size_t(width) * 4];
      const float wi = w[i];
      for (size_t k = 0; k < row.size(); ++k) {
        row[k] += wi * float(s[k]);
      }
    }
    uint8_t* dst = &out.rgba[size_t(y) * size_t(width) * 4];
    for (size_t k = 0; k < row.size(); ++k) {
      dst[k] = ToByte(row[k]);
    }
  }
  return out;
}

std::vector<uint8_t> EncodeTxtrRgba8(const Image& image, int minSize) {
  std::vector<Image> levels{image};
  while (levels.back().width > minSize && levels.back().height > minSize) {
    levels.push_back(HalfOf(levels.back()));
  }
  std::vector<uint8_t> out;
  Put32(out, 9);
  Put16(out, uint32_t(image.width));
  Put16(out, uint32_t(image.height));
  Put32(out, uint32_t(levels.size()));
  for (const Image& lv : levels) {
    // 4x4 blocks: sixteen (alpha, red) pairs, then sixteen (green, blue).
    for (int by = 0; by < lv.height; by += 4) {
      for (int bx = 0; bx < lv.width; bx += 4) {
        for (int pass = 0; pass < 2; ++pass) {
          for (int y = 0; y < 4; ++y) {
            // A level that is not a multiple of 4 repeats its last row and column.
            const size_t row = size_t(std::min(by + y, lv.height - 1)) * size_t(lv.width);
            for (int x = 0; x < 4; ++x) {
              const uint8_t* p = &lv.rgba[(row + size_t(std::min(bx + x, lv.width - 1))) * 4];
              out.push_back(pass == 0 ? p[3] : p[1]);
              out.push_back(pass == 0 ? p[0] : p[2]);
            }
          }
        }
      }
    }
  }
  return out;
}

std::vector<uint8_t> EncodeTxtrCmpr(const Image& image, bool alpha) {
  std::vector<Image> levels{image};
  if (!alpha) {
    for (size_t i = 3; i < levels[0].rgba.size(); i += 4) {
      levels[0].rgba[i] = 255;
    }
  } else {
    BleedColour(levels[0]);
  }
  while (levels.back().width > 8 && levels.back().height > 8) {
    levels.push_back(HalfOf(levels.back()));
  }
  if (alpha) {
    KeepCoverage(levels);
  }
  std::vector<uint8_t> out;
  Put32(out, 10);
  Put16(out, uint32_t(image.width));
  Put16(out, uint32_t(image.height));
  Put32(out, uint32_t(levels.size()));
  for (const Image& lv : levels) {
    // 8x8 tiles of four blocks each.
    for (int ty = 0; ty < lv.height; ty += 8) {
      for (int tx = 0; tx < lv.width; tx += 8) {
        for (int sub = 0; sub < 4; ++sub) {
          const int bx = tx + (sub & 1) * 4, by = ty + (sub >> 1) * 4;
          uint8_t px[16][4];
          for (int y = 0; y < 4; ++y) {
            const size_t row = size_t(std::min(by + y, lv.height - 1)) * size_t(lv.width);
            for (int x = 0; x < 4; ++x) {
              std::memcpy(px[y * 4 + x], &lv.rgba[(row + size_t(std::min(bx + x, lv.width - 1))) * 4], 4);
            }
          }
          uint8_t block[8];
          CmprBlock(px, block);
          out.insert(out.end(), block, block + 8);
        }
      }
    }
  }
  return out;
}

void EncodeBc5Block(const uint8_t* rgba, uint8_t* out) {
  Bc4Block(rgba, 0, out);
  Bc4Block(rgba, 1, out + 8);
}

void EncodeBc7Block(const uint8_t* rgba, uint8_t* out) {
  bool opaque = true;
  float mean[4] = {0, 0, 0, 0};
  for (int i = 0; i < 16; ++i) {
    for (int k = 0; k < 4; ++k) {
      mean[k] += float(rgba[i * 4 + k]);
    }
    opaque = opaque && rgba[i * 4 + 3] == 255;
  }
  for (float& m : mean) {
    m /= 16.0f;
  }
  // The block's principal axis in RGBA, by power iteration.
  float cov[4][4] = {};
  for (int i = 0; i < 16; ++i) {
    float d[4];
    for (int k = 0; k < 4; ++k) {
      d[k] = float(rgba[i * 4 + k]) - mean[k];
    }
    for (int a = 0; a < 4; ++a) {
      for (int b = a; b < 4; ++b) {
        cov[a][b] += d[a] * d[b];
      }
    }
  }
  for (int a = 0; a < 4; ++a) {
    for (int b = 0; b < a; ++b) {
      cov[a][b] = cov[b][a];
    }
  }
  float ax[4] = {1, 1, 1, 1};
  for (int it = 0; it < 6; ++it) {
    float v[4];
    float len = 0;
    for (int a = 0; a < 4; ++a) {
      v[a] = cov[a][0] * ax[0] + cov[a][1] * ax[1] + cov[a][2] * ax[2] + cov[a][3] * ax[3];
      len += v[a] * v[a];
    }
    if (len < 1e-8f) {
      break;  // a flat block, or an axis the start vector is blind to
    }
    len = std::sqrt(len);
    for (int a = 0; a < 4; ++a) {
      ax[a] = v[a] / len;
    }
  }
  float tmin = 1e30f, tmax = -1e30f;
  for (int i = 0; i < 16; ++i) {
    float t = 0;
    for (int k = 0; k < 4; ++k) {
      t += (float(rgba[i * 4 + k]) - mean[k]) * ax[k];
    }
    tmin = std::min(tmin, t);
    tmax = std::max(tmax, t);
  }
  float c0[4], c1[4];
  for (int k = 0; k < 4; ++k) {
    c0[k] = std::clamp(mean[k] + ax[k] * tmin, 0.0f, 255.0f);
    c1[k] = std::clamp(mean[k] + ax[k] * tmax, 0.0f, 255.0f);
  }
  Mode6 best;
  Endpoint(c0, opaque, best.e[0]);
  Endpoint(c1, opaque, best.e[1]);
  Assign(rgba, best);
  // Least squares: with the indices fixed, the endpoints that fit them best.
  for (int pass = 0; pass < 2 && best.error > 0; ++pass) {
    float aa = 0, ab = 0, bb = 0, ra[4] = {0, 0, 0, 0}, rb[4] = {0, 0, 0, 0};
    for (int i = 0; i < 16; ++i) {
      const float b = float(kWeights4[best.idx[i]]) / 64.0f, a = 1.0f - b;
      aa += a * a;
      ab += a * b;
      bb += b * b;
      for (int k = 0; k < 4; ++k) {
        ra[k] += a * float(rgba[i * 4 + k]);
        rb[k] += b * float(rgba[i * 4 + k]);
      }
    }
    const float det = aa * bb - ab * ab;
    if (std::fabs(det) < 1e-4f) {
      break;
    }
    Mode6 trial;
    for (int k = 0; k < 4; ++k) {
      c0[k] = std::clamp((ra[k] * bb - rb[k] * ab) / det, 0.0f, 255.0f);
      c1[k] = std::clamp((rb[k] * aa - ra[k] * ab) / det, 0.0f, 255.0f);
    }
    Endpoint(c0, opaque, trial.e[0]);
    Endpoint(c1, opaque, trial.e[1]);
    Assign(rgba, trial);
    if (trial.error >= best.error) {
      break;
    }
    best = trial;
  }
  if (opaque && best.error > kMode1Threshold) {
    Mode1 two;
    FitMode1(rgba, two);
    if (two.error < best.error) {
      // Mode 1: two mode bits, the partition, R G B for the four endpoints at
      // 6 bits, a p-bit per subset, and 3-bit indices (2 for the anchors).
      BitWriter w;
      w.Put(2, 2);
      w.Put(uint32_t(two.part), 6);
      for (int k = 0; k < 3; ++k) {
        for (int s = 0; s < 2; ++s) {
          w.Put(uint32_t(two.e[s][0][k] >> 2), 6);
          w.Put(uint32_t(two.e[s][1][k] >> 2), 6);
        }
      }
      w.Put(uint32_t(two.e[0][0][0] >> 1 & 1), 1);
      w.Put(uint32_t(two.e[1][0][0] >> 1 & 1), 1);
      for (int i = 0; i < 16; ++i) {
        w.Put(two.idx[i], i == 0 || i == kAnchors[two.part] ? 2 : 3);
      }
      w.Store(out);
      return;
    }
  }
  // The first index is stored without its top bit, so it must be clear.
  if (best.idx[0] & 8) {
    std::swap(best.e[0], best.e[1]);
    for (uint8_t& i : best.idx) {
      i = uint8_t(15 - i);
    }
  }
  // Mode 6: seven mode bits, then R0 R1 G0 G1 B0 B1 A0 A1 at 7 bits, the two
  // p-bits, and the indices (3 bits for the first, 4 for the rest).
  BitWriter w;
  w.Put(0x40, 7);
  for (int k = 0; k < 4; ++k) {
    w.Put(uint32_t(best.e[0][k] >> 1), 7);
    w.Put(uint32_t(best.e[1][k] >> 1), 7);
  }
  w.Put(uint32_t(best.e[0][0] & 1), 1);
  w.Put(uint32_t(best.e[1][0] & 1), 1);
  w.Put(best.idx[0], 3);
  for (int i = 1; i < 16; ++i) {
    w.Put(best.idx[i], 4);
  }
  w.Store(out);
}

std::vector<uint8_t> EncodeDds(const Image& image, DdsFormat format, bool punch) {
  std::vector<Image> levels{image};
  if (punch) {
    BleedColour(levels[0]);
  }
  while (levels.back().width > 1 || levels.back().height > 1) {
    levels.push_back(HalfOf(levels.back()));
  }
  if (punch) {
    KeepCoverage(levels);
  }
  const auto blocks = [](int side) { return size_t(std::max(side, 4) / 4); };
  std::vector<uint8_t> out;
  out.reserve(148 + blocks(image.width) * blocks(image.height) * 16 * 4 / 3 + 64);
  // DDSD_CAPS|HEIGHT|WIDTH|PIXELFORMAT|MIPMAPCOUNT|LINEARSIZE; DDPF_FOURCC
  // 'DX10'; COMPLEX|TEXTURE|MIPMAP.
  out.insert(out.end(), {'D', 'D', 'S', ' '});
  Put32LE(out, 124);
  Put32LE(out, 0x000A1007);
  Put32LE(out, uint32_t(image.height));
  Put32LE(out, uint32_t(image.width));
  Put32LE(out, uint32_t(blocks(image.width) * blocks(image.height) * 16));
  Put32LE(out, 0);
  Put32LE(out, uint32_t(levels.size()));
  out.resize(out.size() + 44);
  Put32LE(out, 32);
  Put32LE(out, 4);
  out.insert(out.end(), {'D', 'X', '1', '0'});
  out.resize(out.size() + 20);
  Put32LE(out, 0x00401008);
  out.resize(out.size() + 16);
  Put32LE(out, format == DdsFormat::BC7 ? 98 : 83);  // DXGI format
  Put32LE(out, 3);                                   // TEXTURE2D
  Put32LE(out, 0);
  Put32LE(out, 1);  // array size
  Put32LE(out, 0);
  for (const Image& lv : levels) {
    // A level under 4 texels a side is stretched to one block, which is what
    // its block holds anyway.
    const int bw = std::max(lv.width, 4), bh = std::max(lv.height, 4);
    for (int by = 0; by < bh; by += 4) {
      for (int bx = 0; bx < bw; bx += 4) {
        uint8_t px[64];
        for (int y = 0; y < 4; ++y) {
          const int sy = (by + y) * lv.height / bh;
          for (int x = 0; x < 4; ++x) {
            const int sx = (bx + x) * lv.width / bw;
            std::memcpy(px + (y * 4 + x) * 4, &lv.rgba[(size_t(sy) * size_t(lv.width) + size_t(sx)) * 4], 4);
          }
        }
        uint8_t block[16];
        if (format == DdsFormat::BC7) {
          EncodeBc7Block(px, block);
        } else {
          EncodeBc5Block(px, block);
        }
        out.insert(out.end(), block, block + 16);
      }
    }
  }
  return out;
}

}  // namespace PortRemastered
