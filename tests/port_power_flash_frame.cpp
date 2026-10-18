#include "port_power_flash_frame.h"
#include "port_test.h"

#include <cstdio>
#include <cstring>

namespace {
bool Near(float a, float b) { return a > b - 0.00001f && a < b + 0.00001f; }

struct GeneratorRecorder {
  bool converted;
  int calls = 0;
  float frame[12] = {};

  bool PortIsRemastered() const { return converted; }
  void SetGlobalOrientAndTrans(const CTransform4f& xf) {
    ++calls;
    const float values[12] = {xf.Get00(), xf.Get01(), xf.Get02(), xf.Get03(),
                              xf.Get10(), xf.Get11(), xf.Get12(), xf.Get13(),
                              xf.Get20(), xf.Get21(), xf.Get22(), xf.Get23()};
    std::memcpy(frame, values, sizeof(frame));
  }
};

bool SameBits(float a, float b) { return std::memcmp(&a, &b, sizeof(a)) == 0; }

float Determinant(const float* m) {
  return m[0] * (m[5] * m[10] - m[6] * m[9]) -
         m[1] * (m[4] * m[10] - m[6] * m[8]) +
         m[2] * (m[4] * m[9] - m[5] * m[8]);
}

void CheckLocator(const CTransform4f& locator) {
  GeneratorRecorder raw{false};
  raw.SetGlobalOrientAndTrans(locator);
  for (int slot = 0; slot < 8; ++slot) {
    for (bool converted : {false, true}) {
      GeneratorRecorder generator{converted};
      NPortPowerFlashFrame::SetGlobalLocator(generator, locator, slot);
      CHECK_MSG(generator.calls == 1, "all paths set one full global frame");
      const bool corrected = converted && slot == NPortPowerFlashFrame::kPowerChargedFlashSlot;
      for (int row = 0; row < 3; ++row) {
        const int base = row * 4;
        CHECK_MSG(SameBits(generator.frame[base + 3], raw.frame[base + 3]),
                  "sampled translation is byte-for-byte unchanged (including signed zero)");
        if (corrected) {
          CHECK_MSG(SameBits(generator.frame[base], -raw.frame[base]) &&
                        SameBits(generator.frame[base + 1], raw.frame[base + 2]) &&
                        SameBits(generator.frame[base + 2], raw.frame[base + 1]),
                    "right composition has all signed columns (-col0,col2,col1)");
        }
      }
      if (!corrected) {
        CHECK_MSG(std::memcmp(generator.frame, raw.frame, sizeof(raw.frame)) == 0,
                  "other slots and nonconverted generators retain the full raw transform");
        continue;
      }
      CHECK_MSG(Near(Determinant(generator.frame), 1.f), "proper B preserves determinant +1");
      for (int a = 0; a < 3; ++a) {
        for (int b = 0; b < 3; ++b) {
          float dot = 0.f;
          for (int row = 0; row < 3; ++row) {
            dot += generator.frame[row * 4 + a] * generator.frame[row * 4 + b];
          }
          CHECK_MSG(Near(dot, a == b ? 1.f : 0.f), "basis columns remain orthonormal");
        }
      }
      // Actual flash child A authors position +Z .05 and zero velocity. At scale 2
      // its displacement is .10 along the sampled +Y axis; no velocity is invented.
      float forwardProjection = 0.f;
      float upProjection = 0.f;
      for (int row = 0; row < 3; ++row) {
        const int base = row * 4;
        const float delta = generator.frame[base + 2] * (0.05f * 2.f);
        CHECK_MSG(Near(delta, raw.frame[base + 1] * 0.10f),
                  ".05 position at scale 2 follows sampled forward, not sampled up");
        forwardProjection += delta * raw.frame[base + 1];
        upProjection += delta * raw.frame[base + 2];
        const float center = generator.frame[base + 3] + generator.frame[base] * 0.f +
                             generator.frame[base + 1] * 0.f + generator.frame[base + 2] * 0.f;
        CHECK_MSG(center == raw.frame[base + 3], "zero-position child stays at the sampled center");
        const float velocity = generator.frame[base] * 0.f + generator.frame[base + 1] * 0.f +
                               generator.frame[base + 2] * 0.f;
        CHECK_MSG(velocity == 0.f, "zero authored velocity remains zero");
      }
      CHECK_MSG(Near(forwardProjection, 0.10f) && Near(upProjection, 0.f),
                "displacement is positive .10 forward and has no sampled-up component");
    }
  }
}

void CheckNoncommutingOrder() {
  // Rz(+90) * B != B * Rz(+90). Explicit expected rows catch left composition,
  // inverse, or constant substitution, independently of the column-loop checks.
  const CTransform4f locator(0.f, -1.f, 0.f, 0.5f,
                             1.f, 0.f, 0.f, -0.25f,
                             0.f, 0.f, 1.f, -0.f);
  GeneratorRecorder generator{true};
  NPortPowerFlashFrame::SetGlobalLocator(generator, locator, 3);
  const float expected[12] = {0.f, 0.f, -1.f, 0.5f,
                              -1.f, 0.f, 0.f, -0.25f,
                              0.f, 1.f, 0.f, -0.f};
  for (int i = 0; i < 12; ++i) {
    CHECK_MSG(generator.frame[i] == expected[i], "noncommuting sample proves R_sampled * B order");
  }
  // A second noncommuting Rz * Ry sample exercises every signed column.
  const CTransform4f general(0.48f, -0.8f, 0.36f, -2.5f,
                             0.64f, 0.6f, 0.48f, 3.75f,
                             -0.6f, 0.f, 0.8f, -0.125f);
  CheckLocator(locator);
  CheckLocator(general);
}
} // namespace

int main() {
  static_assert(NPortPowerFlashFrame::kPowerChargedFlashSlot == 3);
  const CTransform4f identity(1.f, 0.f, 0.f, -0.f,
                              0.f, 1.f, 0.f, 1.28631f,
                              0.f, 0.f, 1.f, 0.00166f);
  CheckLocator(identity);
  CheckNoncommutingOrder();
  if (port_test::Failures() == 0) {
    std::puts("Power charged-release flash frame checks passed (8 slots x 2 flags x 3 frames)");
  }
  return PORT_TEST_RESULT();
}
