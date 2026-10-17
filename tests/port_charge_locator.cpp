#include "MetroidPrime/Weapons/CGunWeapon.hpp"
#include "port_test.h"

#include <cstdio>

namespace {

bool Near(float a, float b) { return a > b - 0.0001f && a < b + 0.0001f; }

struct CGeneratorRecorder {
  explicit CGeneratorRecorder(bool remastered)
  : remastered(remastered)
  , orientationCalls(0)
  , translationCalls(0)
  , rotation{7.f, 8.f, 9.f, 10.f, 11.f, 12.f, 13.f, 14.f, 15.f}
  , translation{-1.f, -2.f, -3.f} {}

  bool PortIsRemastered() const { return remastered; }

  void SetGlobalOrientAndTrans(const CTransform4f& xf) {
    ++orientationCalls;
    rotation[0] = xf.Get00();
    rotation[1] = xf.Get01();
    rotation[2] = xf.Get02();
    rotation[3] = xf.Get10();
    rotation[4] = xf.Get11();
    rotation[5] = xf.Get12();
    rotation[6] = xf.Get20();
    rotation[7] = xf.Get21();
    rotation[8] = xf.Get22();
    SetGlobalTranslation(xf.GetTranslation());
  }

  void SetGlobalTranslation(const CVector3f& pos) {
    ++translationCalls;
    translation[0] = pos.GetX();
    translation[1] = pos.GetY();
    translation[2] = pos.GetZ();
  }

  // The generator's frame applied to a local offset, as CElementGen places its children.
  void ChildOffset(float x, float y, float z, float out[3]) const {
    out[0] = translation[0] + rotation[0] * x + rotation[1] * y + rotation[2] * z;
    out[1] = translation[1] + rotation[3] * x + rotation[4] * y + rotation[5] * z;
    out[2] = translation[2] + rotation[6] * x + rotation[7] * y + rotation[8] * z;
  }

  float Determinant() const {
    return rotation[0] * (rotation[4] * rotation[8] - rotation[5] * rotation[7]) -
           rotation[1] * (rotation[3] * rotation[8] - rotation[5] * rotation[6]) +
           rotation[2] * (rotation[3] * rotation[7] - rotation[4] * rotation[6]);
  }

  bool remastered;
  int orientationCalls;
  int translationCalls;
  float rotation[9];
  float translation[3];
};

const CTransform4f IdentityLocator(1.f, 0.f, 0.f, 0.f,
                                   0.f, 1.f, 0.f, 0.f,
                                   0.f, 0.f, 1.f, 0.f);

// The retail LBEAM locator at full Power charge, measured in game (gun-local).
const CTransform4f SampledPowerLocator(0.999993f, 0.003689f, -0.000819f, 0.00705f,
                                       -0.003690f, 0.999992f, -0.001245f, 1.28631f,
                                       0.000815f, 0.001248f, 0.999999f, 0.00166f);

// An exact, trig-free rotated sample, a quarter turn about the third axis, with a
// translation: distinct from the muzzle basis, so composing it and passing it through give
// visibly different matrices.
const CTransform4f QuarterTurnLocator(0.f, -1.f, 0.f, 0.5f,
                                      1.f, 0.f, 0.f, -0.25f,
                                      0.f, 0.f, 1.f, 0.f);

void TestConvertedChargeEffectGetsMuzzleBasis() {
  CGeneratorRecorder generator(true);
  NPortChargeFxDetail::SetLocalLocator(generator, IdentityLocator, true);

  CHECK_MSG(generator.orientationCalls == 1, "converted charge effect receives an orientation");
  CHECK_MSG(generator.translationCalls == 1, "converted charge effect receives a translation");
  // Rx(+90): columns (1,0,0), (0,0,1), (0,-1,0), i.e. rows (1,0,0), (0,0,-1), (0,1,0).
  const float expected[9] = {1.f, 0.f, 0.f, 0.f, 0.f, -1.f, 0.f, 1.f, 0.f};
  for (int i = 0; i < 9; ++i) {
    CHECK_MSG(Near(generator.rotation[i], expected[i]), "muzzle basis entry matches Rx(+90)");
  }
  CHECK_MSG(Near(generator.Determinant(), 1.f), "muzzle basis determinant is +1 (a rotation)");
  const float column1[3] = {expected[1], expected[4], expected[7]};
  const float column2[3] = {expected[2], expected[5], expected[8]};
  CHECK_MSG(Near(expected[0], 1.f) && Near(expected[3], 0.f) && Near(expected[6], 0.f),
            "local +X stays the gun's X (right)");
  CHECK_MSG(Near(column1[0], 0.f) && Near(column1[1], 0.f) && Near(column1[2], 1.f),
            "local +Y becomes the gun's +Z (up)");
  CHECK_MSG(Near(column2[0], 0.f) && Near(column2[1], -1.f) && Near(column2[2], 0.f),
            "local +Z becomes the gun's -Y (back along the barrel)");
}

void TestSampledTranslationIsUnchanged() {
  CGeneratorRecorder generator(true);
  NPortChargeFxDetail::SetLocalLocator(generator, SampledPowerLocator, true);
  CHECK_MSG(Near(generator.translation[0], 0.00705f) && Near(generator.translation[1], 1.28631f) &&
                Near(generator.translation[2], 0.00166f),
            "muzzle-frame compose keeps the sampled locator translation");
  CHECK_MSG(Near(generator.Determinant(), 1.f), "composed sampled rotation stays a +1 rotation");
}

void TestAuthoredSourceOffsetMapsBackAlongBarrel() {
  // The Power charge tree's authored EMTR (0, 0, 0.15) at global scale 2 = (0, 0, 0.30) in
  // the generator's local frame; the fix must put it 0.30 back down the barrel (gun-local -Y).
  CGeneratorRecorder generator(true);
  NPortChargeFxDetail::SetLocalLocator(generator, SampledPowerLocator, true);
  float out[3];
  generator.ChildOffset(0.f, 0.f, 0.15f * 2.f, out);
  // The offset from the locator, so the pose-dependent translation cancels; the sampled
  // rotation's ~0.2 deg tilt shows up only as a sub-millimetre lateral term.
  const float delta[3] = {out[0] - generator.translation[0], out[1] - generator.translation[1],
                          out[2] - generator.translation[2]};
  CHECK_MSG(delta[0] > -0.002f && delta[0] < 0.002f && Near(delta[1], -0.30f) && delta[2] > -0.002f &&
                delta[2] < 0.002f,
            "authored (0,0,.15)*2 offset maps to gun-local (0,-0.30,0)");
  CHECK_MSG(Near(out[1], 1.28631f - 0.30f), "the offset lands 0.30 back down the barrel, not up");
}

void TestRotatedSampleLocatorComposes() {
  // With a rotated sample the basis is composed on the locator's own side: the result's
  // columns are the locator's (col0, col2, -col1).
  CGeneratorRecorder generator(true);
  NPortChargeFxDetail::SetLocalLocator(generator, QuarterTurnLocator, true);
  const float expected[9] = {0.f, 0.f, 1.f, 1.f, 0.f, 0.f, 0.f, 1.f, 0.f};
  for (int i = 0; i < 9; ++i) {
    CHECK_MSG(Near(generator.rotation[i], expected[i]),
              "composed rotated sample matches locator x Rx(+90)");
  }
  CHECK_MSG(Near(generator.Determinant(), 1.f), "composed rotated sample keeps determinant +1");
  CHECK_MSG(Near(generator.translation[0], 0.5f) && Near(generator.translation[1], -0.25f) &&
                Near(generator.translation[2], 0.f),
            "composed rotated sample keeps its translation");
  float out[3];
  generator.ChildOffset(0.f, 0.f, 0.30f, out);
  CHECK_MSG(Near(out[0], 0.5f + 0.30f) && Near(out[1], -0.25f) && Near(out[2], 0.f),
            "the authored offset follows the sample (its back is the sample's +X)");
}

void TestConvertedNonChargeEffectKeepsTheSampledLocator() {
  // The normal muzzle flash shares this call site but is out of scope: it must receive the
  // locator exactly as sampled, not composed with the muzzle basis.
  CGeneratorRecorder generator(true);
  NPortChargeFxDetail::SetLocalLocator(generator, QuarterTurnLocator, false);
  CHECK_MSG(generator.orientationCalls == 1, "converted non-charge effect receives an orientation");
  const float sampled[9] = {0.f, -1.f, 0.f, 1.f, 0.f, 0.f, 0.f, 0.f, 1.f};
  for (int i = 0; i < 9; ++i) {
    CHECK_MSG(Near(generator.rotation[i], sampled[i]),
              "converted non-charge effect keeps the sampled locator rotation");
  }
  CHECK_MSG(Near(generator.translation[0], 0.5f) && Near(generator.translation[1], -0.25f) &&
                Near(generator.translation[2], 0.f),
            "converted non-charge effect keeps the locator translation");

  float out[3];
  generator.ChildOffset(0.f, 0.f, 0.25f, out);
  CHECK_MSG(Near(out[0], 0.5f) && Near(out[1], -0.25f) && Near(out[2], 0.25f),
            "converted non-charge child +Z offset follows the sampled locator frame numerically");
}

void TestRetailKeepsLegacyTranslationOnly() {
  CGeneratorRecorder generator(false);
  NPortChargeFxDetail::SetLocalLocator(generator, QuarterTurnLocator, false);

  CHECK_MSG(generator.orientationCalls == 0, "retail effect does not receive locator orientation");
  CHECK_MSG(generator.translationCalls == 1, "retail effect keeps the legacy translation setter");
  CHECK_MSG(Near(generator.rotation[0], 7.f) && Near(generator.rotation[5], 12.f) &&
                Near(generator.rotation[8], 15.f),
            "retail effect orientation is left untouched");
  CHECK_MSG(Near(generator.translation[0], 0.5f) && Near(generator.translation[1], -0.25f) &&
                Near(generator.translation[2], 0.f),
            "retail effect retains locator translation");
}

void TestRetailChargeEffectIsUnchangedByTheFix() {
  // The retail control: the charge slot on a retail effect behaves exactly as before,
  // whatever the muzzle-basis flag says.
  CGeneratorRecorder generator(false);
  NPortChargeFxDetail::SetLocalLocator(generator, SampledPowerLocator, true);
  CHECK_MSG(generator.orientationCalls == 0, "retail charge effect still gets no orientation");
  CHECK_MSG(generator.translationCalls == 1, "retail charge effect still gets the translation only");
  CHECK_MSG(Near(generator.rotation[0], 7.f) && Near(generator.rotation[5], 12.f) &&
                Near(generator.rotation[8], 15.f),
            "retail charge effect orientation is left untouched");
  CHECK_MSG(Near(generator.translation[1], 1.28631f), "retail charge effect keeps its translation");
}
} // namespace

int main() {
  TestConvertedChargeEffectGetsMuzzleBasis();
  TestSampledTranslationIsUnchanged();
  TestAuthoredSourceOffsetMapsBackAlongBarrel();
  TestRotatedSampleLocatorComposes();
  TestConvertedNonChargeEffectKeepsTheSampledLocator();
  TestRetailKeepsLegacyTranslationOnly();
  TestRetailChargeEffectIsUnchangedByTheFix();
  if (port_test::Failures() == 0) {
    std::puts("charge locator checks passed");
  }
  return PORT_TEST_RESULT();
}
