#include "gx.hpp"
#include "__gx.h"
#include "dolphin/gx/GXAurora.h"
#include "../../gfx/bloom.hpp"
#include "../../gfx/probe.hpp"

#include <bit>
#include <cstring>
#include <vector>

namespace {
// Port: a PBR draw sets all of its probe, cube, ambient, volume, tone and material state
// per surface, and neighbouring surfaces almost always repeat it. The processor already
// ignores a repeat, so a repeat is not written at all; that saves a few hundred FIFO bytes
// per surface. These are only ever written from the game thread, and never recorded into a
// display list (which a skipped write would leave out).
template <typename T>
struct LastPBRWrite {
  T value{};
  bool valid = false;

  // True when `now` is what was written last; otherwise remembers it.
  bool repeats(const T& now) {
    if (valid && std::memcmp(&value, &now, sizeof(T)) == 0) {
      return true;
    }
    std::memcpy(&value, &now, sizeof(T));
    valid = true;
    return false;
  }
};

struct PBRProbeWrite {
  f32 rows[3][3];
  f32 weight;
  f32 occlusionMin;
  f32 occlusionInvMax;
};
struct PBRCubeWrite {
  u32 id;
  f32 params[4];
};
struct PBRAmbientWrite {
  f32 rows[6][3];
  f32 mode;
  u32 present;
};
struct PBRVolumeWrite {
  u32 id;
  f32 rows[6][4];
};
struct PBRToneWrite {
  f32 rows[3][4];
};
struct PBRLightScaleWrite {
  f32 diffuse;
  f32 f0;
  f32 alpha;
  u32 alphaReplaces;
};
struct PBRLightHdrWrite {
  f32 color[3];
  f32 pos[3];
  f32 r0;
  f32 r1;
  u32 falloff;
};
struct PBRMaterialWrite {
  f32 emissive[3];
  f32 backlight[3];
  f32 heightBlend;
  f32 mode;
  f32 layer[5];
  f32 kind[6];
  f32 up[3];
  u32 debugView;
};
} // namespace

extern "C" {
GXBool GXPortPostProcess(GXBool bloom, f32 threshold, const f32 tints[5][3], const f32 tone[3][4], u32 gradeA,
                         u32 gradeB, f32 gradeWeight, f32 exposure) {
  if (GXGetPBRCostTest() == 10) {
    return true;
  }
  aurora::gfx::bloom::Params params{};
  params.bloom = bloom ? 1 : 0;
  params.threshold = threshold;
  if (tints != nullptr) {
    std::memcpy(params.tints, tints, sizeof(params.tints));
  }
  if (tone != nullptr) {
    std::memcpy(params.tone, tone, sizeof(params.tone));
  }
  params.gradeA = gradeA;
  params.gradeB = gradeB;
  params.gradeWeight = gradeWeight;
  // The curve's toe has no use for row 0's w; it carries the exposure (see bloom.hpp).
  params.tone[0][3] = tone != nullptr && exposure > 0.f ? exposure : 0.f;
  if (!params.bloom && gradeA == 0 && gradeB == 0 && params.tone[0][3] == 0.f) {
    return true;
  }
  if (!aurora::gfx::bloom::ensure_task()) {
    return false;
  }
  // Through the FIFO: recording it directly would first wait for the FIFO thread to finish
  // everything the frame has drawn so far.
  static_assert(sizeof(params) == 32 * sizeof(u32));
  u32 words[32];
  std::memcpy(words, &params, sizeof(words));
  GX_WRITE_AURORA(GX_AURORA_PORT_POST_PROCESS);
  for (const u32 word : words) {
    GX_WRITE_U32(word);
  }
  return true;
}

void GXPortColorGradeLut(u32 id, const u8* rgba) { aurora::gfx::bloom::set_grade_lut(id, rgba); }

GXBool GXPortFrameRadiance(f32 out[3], u32* serial) {
  uint32_t count = 0;
  const bool ok = aurora::gfx::bloom::frame_radiance(out, count);
  if (serial != nullptr) {
    *serial = count;
  }
  return ok;
}

void GXDestroyTexObj(GXTexObj* obj_) {
  auto* obj = reinterpret_cast<GXTexObj_*>(obj_);
  if (obj->texObjId != 0) {
    GX_WRITE_AURORA(GX_AURORA_DESTROY_TEXOBJ);
    GX_WRITE_U32(obj->texObjId);
  }
  obj->texObjId = 0;
}

void GXDestroyTlutObj(GXTlutObj* obj_) {
  auto* obj = reinterpret_cast<GXTlutObj_*>(obj_);
  if (obj->tlutObjId != 0) {
    GX_WRITE_AURORA(GX_AURORA_DESTROY_TLUT);
    GX_WRITE_U32(obj->tlutObjId);
  }
  obj->tlutObjId = 0;
}

void GXDestroyCopyTex(void* dest) {
  if (dest != nullptr) {
    GX_WRITE_AURORA(GX_AURORA_DESTROY_COPY_TEX);
    GX_WRITE_U64(reinterpret_cast<u64>(dest));
  }
}

void GXSetArrayBaseIndex(GXAttr attr, u32 base) {
  if (attr == GX_VA_NBT) {
    attr = GX_VA_NRM;
  }
  const u32 cpIdx = attr - GX_VA_POS;
  assert((cpIdx & ~0xF) == 0);
  GX_WRITE_AURORA(GX_AURORA_LOAD_ARRAY_BASE_INDEX);
  GX_WRITE_U8(static_cast<u8>(cpIdx));
  GX_WRITE_U32(base);
}

static u32 sPBRCostTest = 0;

void GXSetPBRCostTest(u32 test) { sPBRCostTest = test <= 11 ? test : 0; }

u32 GXGetPBRCostTest() { return sPBRCostTest; }

void GXSetPBR(GXBool enable) {
  GX_WRITE_AURORA(GX_AURORA_SET_PBR);
  // Tests 10 and 11 are outside the shading.
  GX_WRITE_U8(enable ? static_cast<u8>(1 + (sPBRCostTest <= 9 ? sPBRCostTest : 0)) : 0);
}

void GXSetSDF(u8 edge) {
  GX_WRITE_AURORA(GX_AURORA_SET_SDF);
  GX_WRITE_U8(edge);
}

void GXPortSetDepthPrepass(u8 pass) {
  GX_WRITE_AURORA(GX_AURORA_PORT_DEPTH_PREPASS);
  GX_WRITE_U8(pass);
}

void GXSetDrawTag(u32 asset, u32 model, u32 material) {
  GX_WRITE_AURORA(GX_AURORA_SET_DRAW_TAG);
  GX_WRITE_U32(asset);
  GX_WRITE_U32(model);
  GX_WRITE_U32(material);
}

void GXCopyProbeFace(u32 face) {
  GX_WRITE_AURORA(GX_AURORA_COPY_PROBE_FACE);
  GX_WRITE_U8(static_cast<u8>(face));
  aurora::gx::fifo::publish();
}

void GXSetPBRProbe(const f32 viewToProbe[3][3], f32 weight) { GXSetPBRProbeEx(viewToProbe, weight, 0.f, 0.f); }

void GXSetPBRProbeEx(const f32 viewToProbe[3][3], f32 weight, f32 occlusionMin, f32 occlusionInvMax) {
  static LastPBRWrite<PBRProbeWrite> sLast;
  PBRProbeWrite now{};
  std::memcpy(now.rows, viewToProbe, sizeof(now.rows));
  now.weight = weight;
  now.occlusionMin = occlusionMin;
  now.occlusionInvMax = occlusionInvMax;
  if (sLast.repeats(now)) {
    return;
  }
  GX_WRITE_AURORA(GX_AURORA_SET_PBR_PROBE);
  const f32 w[3]{weight, occlusionMin, occlusionInvMax};
  for (int col = 0; col < 3; ++col) {
    GX_WRITE_F32(viewToProbe[0][col]);
    GX_WRITE_F32(viewToProbe[1][col]);
    GX_WRITE_F32(viewToProbe[2][col]);
    GX_WRITE_F32(w[col]);
  }
}

GXBool GXPortBlendPBRCube(u32 dst, const u32* src, const f32* weights, u32 count) {
  return aurora::gfx::probe::blend_cubes(dst, src, weights, count);
}

static u32 sPBRDebugView = 0;

void GXSetPBRDebugView(u32 view) { sPBRDebugView = view; }

void GXSetPBRMaterial(const f32 emissive[3], const f32 backlight[3], f32 heightBlend, f32 mode, const f32 layer[5],
                      const f32 kind[6], const f32 up[3]) {
  static LastPBRWrite<PBRMaterialWrite> sLast;
  PBRMaterialWrite now{};
  std::memcpy(now.emissive, emissive, sizeof(now.emissive));
  std::memcpy(now.backlight, backlight, sizeof(now.backlight));
  now.heightBlend = heightBlend;
  now.mode = mode;
  std::memcpy(now.layer, layer, sizeof(now.layer));
  std::memcpy(now.kind, kind, sizeof(now.kind));
  std::memcpy(now.up, up, sizeof(now.up));
  now.debugView = sPBRDebugView;
  if (sLast.repeats(now)) {
    return;
  }
  GX_WRITE_AURORA(GX_AURORA_SET_PBR_MATERIAL);
  GX_WRITE_F32(emissive[0]);
  GX_WRITE_F32(emissive[1]);
  GX_WRITE_F32(emissive[2]);
  GX_WRITE_F32(heightBlend);
  GX_WRITE_F32(backlight[0]);
  GX_WRITE_F32(backlight[1]);
  GX_WRITE_F32(backlight[2]);
  GX_WRITE_F32(mode);
  GX_WRITE_F32(layer[0]);
  GX_WRITE_F32(kind[0]);
  GX_WRITE_F32(kind[1]);
  GX_WRITE_F32(static_cast<f32>(sPBRDebugView));
  for (int i = 1; i < 5; ++i) {
    GX_WRITE_F32(layer[i]);
  }
  for (int i = 2; i < 6; ++i) {
    GX_WRITE_F32(kind[i]);
  }
  GX_WRITE_F32(up[0]);
  GX_WRITE_F32(up[1]);
  GX_WRITE_F32(up[2]);
  GX_WRITE_F32(0.f);
}

void GXCreatePBRCube(u32 id, u32 size, u32 mipCount, const void* texels, u32 length) {
  // The processor frees the copy.
  auto* copy = new std::vector<u8>(static_cast<const u8*>(texels), static_cast<const u8*>(texels) + length);
  GX_WRITE_AURORA(GX_AURORA_CREATE_PBR_CUBE);
  GX_WRITE_U32(id);
  GX_WRITE_U32(size);
  GX_WRITE_U32(mipCount);
  GX_WRITE_U64(reinterpret_cast<u64>(copy));
}

void GXDestroyPBRCube(u32 id) {
  GX_WRITE_AURORA(GX_AURORA_DESTROY_PBR_CUBE);
  GX_WRITE_U32(id);
}

void GXSetPBRCube(u32 id, const f32 params[4]) {
  static LastPBRWrite<PBRCubeWrite> sLast;
  PBRCubeWrite now{};
  now.id = id;
  std::memcpy(now.params, params, sizeof(now.params));
  if (sLast.repeats(now)) {
    return;
  }
  GX_WRITE_AURORA(GX_AURORA_SET_PBR_CUBE);
  GX_WRITE_U32(id);
  for (int i = 0; i < 4; ++i) {
    GX_WRITE_F32(params[i]);
  }
}

void GXSetPBRAmbient(const f32 rows[6][3], f32 mode) {
  static LastPBRWrite<PBRAmbientWrite> sLast;
  PBRAmbientWrite now{};
  if (rows != nullptr) {
    std::memcpy(now.rows, rows, sizeof(now.rows));
    now.mode = mode;
    now.present = 1;
  }
  if (sLast.repeats(now)) {
    return;
  }
  GX_WRITE_AURORA(GX_AURORA_SET_PBR_AMBIENT);
  for (int row = 0; row < 6; ++row) {
    for (int i = 0; i < 3; ++i) {
      GX_WRITE_F32(rows != nullptr ? rows[row][i] : 0.f);
    }
    GX_WRITE_F32(rows != nullptr && row == 0 ? mode : 0.f);
  }
}

void GXCreatePBRVolume(u32 id, u32 sizeX, u32 sizeY, u32 sizeZ, const void* texels, u32 length) {
  // The processor frees the copy.
  auto* copy = new std::vector<u8>(static_cast<const u8*>(texels), static_cast<const u8*>(texels) + length);
  GX_WRITE_AURORA(GX_AURORA_CREATE_PBR_VOLUME);
  GX_WRITE_U32(id);
  GX_WRITE_U32(sizeX);
  GX_WRITE_U32(sizeY);
  GX_WRITE_U32(sizeZ);
  GX_WRITE_U64(reinterpret_cast<u64>(copy));
}

void GXDestroyPBRVolume(u32 id) {
  GX_WRITE_AURORA(GX_AURORA_DESTROY_PBR_VOLUME);
  GX_WRITE_U32(id);
}

void GXSetPBRVolume(u32 id, const f32 rows[6][4]) {
  static LastPBRWrite<PBRVolumeWrite> sLast;
  PBRVolumeWrite now{};
  if (rows != nullptr) {
    now.id = id;
    std::memcpy(now.rows, rows, sizeof(now.rows));
  }
  if (sLast.repeats(now)) {
    return;
  }
  GX_WRITE_AURORA(GX_AURORA_SET_PBR_VOLUME);
  GX_WRITE_U32(rows != nullptr ? id : 0);
  for (int row = 0; row < 6; ++row) {
    for (int i = 0; i < 4; ++i) {
      GX_WRITE_F32(rows != nullptr ? rows[row][i] : 0.f);
    }
  }
}

void GXSetPBRBrdfLut(const void* texels, u32 length) {
  // The processor frees the copy.
  auto* copy = new std::vector<u8>;
  if (texels != nullptr && length == 256) {
    copy->assign(static_cast<const u8*>(texels), static_cast<const u8*>(texels) + length);
  }
  GX_WRITE_AURORA(GX_AURORA_SET_PBR_BRDF_LUT);
  GX_WRITE_U64(reinterpret_cast<u64>(copy));
}

void GXSetPBRTone(const f32 rows[3][4]) {
  static LastPBRWrite<PBRToneWrite> sLast;
  PBRToneWrite now{};
  if (rows != nullptr) {
    std::memcpy(now.rows, rows, sizeof(now.rows));
  }
  if (sLast.repeats(now)) {
    return;
  }
  GX_WRITE_AURORA(GX_AURORA_SET_PBR_TONE);
  for (int row = 0; row < 3; ++row) {
    for (int i = 0; i < 4; ++i) {
      GX_WRITE_F32(rows != nullptr ? rows[row][i] : 0.f);
    }
  }
}

void GXSetPBRLightSkip(u32 mask) {
  static LastPBRWrite<u32> sLast;
  if (sLast.repeats(mask)) {
    return;
  }
  GX_WRITE_AURORA(GX_AURORA_SET_PBR_LIGHT_SKIP);
  GX_WRITE_U32(mask);
}

void GXSetPBRLightHdr(GXLightID light, const f32 color[3], const f32 viewPos[3], f32 r0, f32 r1, u32 falloff) {
  const u32 bit = static_cast<u32>(light) & 0xFF;
  if (bit == 0) {
    return;
  }
  const u32 idx = static_cast<u32>(std::countr_zero(bit));
  PBRLightHdrWrite now{};
  if (color != nullptr && viewPos != nullptr && r1 > 0.f) {
    now = {{color[0], color[1], color[2]}, {viewPos[0], viewPos[1], viewPos[2]}, r0, r1, falloff};
  }
  static LastPBRWrite<PBRLightHdrWrite> sLast[8];
  if (sLast[idx].repeats(now)) {
    return;
  }
  GX_WRITE_AURORA(GX_AURORA_SET_PBR_LIGHT_HDR);
  GX_WRITE_U32(bit);
  for (f32 v : now.color) {
    GX_WRITE_F32(v);
  }
  for (f32 v : now.pos) {
    GX_WRITE_F32(v);
  }
  GX_WRITE_F32(now.r0);
  GX_WRITE_F32(now.r1);
  GX_WRITE_U32(now.falloff);
}

void GXSetPBRLightScale(f32 diffuse, f32 f0, f32 alpha, GXBool alphaReplaces) {
  static LastPBRWrite<PBRLightScaleWrite> sLast;
  const PBRLightScaleWrite now{diffuse, f0, alpha, alphaReplaces ? 1u : 0u};
  if (sLast.repeats(now)) {
    return;
  }
  GX_WRITE_AURORA(GX_AURORA_SET_PBR_LIGHT_SCALE);
  GX_WRITE_F32(diffuse);
  GX_WRITE_F32(f0);
  GX_WRITE_F32(alpha);
  GX_WRITE_U32(now.alphaReplaces);
}
}
