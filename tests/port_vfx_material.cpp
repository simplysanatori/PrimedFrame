// Real reader and evaluator/DrawDesc builder, without a renderer or asset pool.
#include "Kyoto/Particles/CPortVfxMat.hpp"
#include "port_vfx_particles.h"
#include "Kyoto/Graphics/CTexture.hpp"
#include "Kyoto/Streams/CMemoryInStream.hpp"

#include <bit>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <stdexcept>

// The fixtures have no textures. Fail hard if parsing/building unexpectedly reaches asset loading.
CTexture* CreateTexture(int) { std::abort(); }
CTexture::~CTexture() { std::abort(); }
CToken CSimplePool::GetObj(const SObjectTag&) { std::abort(); }
CToken::CToken(IObj*) { std::abort(); }
CToken::CToken(const CToken&) { std::abort(); }
CToken::~CToken() { std::abort(); }
void CTexture::PortLoad(GXTexMapID, EClampMode, EClampMode) const { std::abort(); }
void* IElement::operator new(size_t n, const char*, const char*) { return ::operator new(n); }
void IElement::operator delete(void* p, size_t) { ::operator delete(p); }
const CPortVarMemory* CPortVarGlobals::sCurrent = nullptr;

namespace {
int failures = 0;
void Check(bool ok, const char* what) {
  if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}
void Word(std::vector<u8>& b, u32 v) {
  for (int shift = 24; shift >= 0; shift -= 8) { b.push_back(u8(v >> shift)); }
}
void Float(std::vector<u8>& b, float v) { Word(b, std::bit_cast<u32>(v)); }
std::vector<u8> Blob(u32 version) {
  std::vector<u8> b;
  Word(b, version); Word(b, 0x838); Word(b, 2); Word(b, 0);
  for (int i = 0; i < 10; ++i) { Word(b, u32(-1)); } // slots, ramp rows, add row
  for (int i = 0; i < 11; ++i) { Word(b, u32(-1)); Word(b, 0); Float(b, float(i)); }
  Float(b, 1.f); Float(b, 0.f); Word(b, 4);
  if (version == 3) { Float(b, 2.f); Float(b, 0.2f); }
  return b;
}
void Parse(CPortVfxData& vfx, std::vector<u8> blob) {
  const auto size = blob.size();
  Word(blob, 0x12345678); // next property must remain unread, also after fallback
  CMemoryInStream in(blob.data(), blob.size());
  Check(PortReadVfxMat(vfx, in, u32(size), nullptr), "reader can resynchronise");
  Check(in.ReadLong() == 0x12345678, "reader consumes exactly the blob");
}
void TestParsing() {
  CPortVfxData vfx;
  Parse(vfx, Blob(3));
  Check(vfx.mat.version == 3 && vfx.mat.opacityGain == 2.f && vfx.mat.opacityBias == 0.2f,
        "v3 reads appended BE gain/bias");
  Parse(vfx, Blob(2));
  Check(vfx.mat.version == 2 && vfx.mat.opacityGain == 1.f && vfx.mat.opacityBias == 0.f,
        "v2 resets previous gain/bias to neutral");
  Check(vfx.mat.src[10].value == 10.f && vfx.mat.spriteCenter == 4, "original sources and tail unchanged");
  for (u32 version : {0u, 1u, 4u, 0xffffffffu}) {
    Parse(vfx, Blob(version));
    Check(vfx.mat.version == 0, "unknown version leaves retail fallback");
  }
  for (u32 version : {2u, 3u}) {
    for (int delta : {-8, -4, 4, 8}) {
      auto b = Blob(version);
      b.resize(b.size() + delta);
      Parse(vfx, b);
      Check(vfx.mat.version == 0, "known versions reject short/long tails");
    }
    for (int field : {11, 15}) { // invalid blend or texture count, before loading any texture
      auto b = Blob(version); b[field] = 6;
      Parse(vfx, b);
      Check(vfx.mat.version == 0, "bad headers leave fallback");
    }
  }
  Parse(vfx, std::vector<u8>(12));
  Check(vfx.mat.version == 0, "short header leaves fallback");
  for (u32 version : {2u, 3u, 0x12345678u}) {
    for (u32 length : {1u, 2u, 3u, 13u, 201u, 209u}) {
      auto b = Blob(version);
      b.resize(length);
      Parse(vfx, b);
      Check(vfx.mat.version == 0, "nonaligned malformed/unknown blob leaves fallback and consumes residual bytes");
    }
  }
}
void TestPhysicalTruncation() {
  for (u32 version : {2u, 3u, 0x12345678u}) {
    for (u32 length : {1u, 2u, 3u, 13u, 200u, 201u, 208u, 209u, 0xffffffffu}) {
      auto b = Blob(version);
      // Include valid-layout truncation, malformed residual-byte truncation and a
      // maximum-u32 declaration without allocating a huge blob or overflowing.
      b.resize(length == 0xffffffffu ? 16 : length - 1);
      CMemoryInStream in(b.data(), b.size());
      CPortVfxData vfx;
      bool threw = false;
      try {
        PortReadVfxMat(vfx, in, length, nullptr);
      } catch (const std::runtime_error&) {
        threw = true;
      }
      Check(threw, "physical truncation still raises the stream exception");
    }
  }
}
class FrameReal : public CRealElement {
  float scale;
public:
  explicit FrameReal(float s) : scale(s) {}
  bool GetValue(int frame, float& value) const override { value = scale * frame; return false; }
};
void TestDynamic() {
  CPortVfxData vfx;
  Parse(vfx, Blob(3));
  float values[kPortVfxVsmtCount + 1] = {};
  values[kPortVfxVsmtCount] = 123.f;
  aurora::gfx::vfx::DrawDesc desc;
  Check(PortVfxBuildDesc(vfx, values, 0, desc) && desc.opacityGain == 2.f && desc.opacityBias == 0.2f,
        "DrawDesc uses defaults without overrides");
  for (unsigned target : {0u, 18u, 19u, 20u, 21u, 31u}) {
    CPortVfxElem e; e.a = target; e.real = new FrameReal(float(target + 1) * 0.01f);
    vfx.vsmt.push_back(e);
  }
  for (int frame : {1, 2, 5}) {
    const auto mask = PortVfxEvalVsmt(vfx, frame, values);
    Check(mask == ((1u << 0) | (1u << 18) | (1u << 19) | (1u << 20)), "21-slot evaluation rejects out-of-range targets");
    Check(values[21] == 123.f, "evaluation does not overrun storage");
    Check(PortVfxBuildDesc(vfx, values, mask, desc) && desc.opacityGain == values[19] &&
              desc.opacityBias == values[20] && desc.erosion.value == values[0], "dynamic gain/bias and legacy source override");
    Check(std::fabs(values[19] - 0.2f * frame) < 1e-6f && std::fabs(values[20] - 0.21f * frame) < 1e-6f,
          "gain/bias evaluate anew each frame");
  }
  // Partial masks must not read a stale slot when only the other component is overridden.
  PortVfxBuildDesc(vfx, values, 1u << 19, desc);
  Check(desc.opacityGain == values[19] && desc.opacityBias == 0.2f, "gain-only override retains MATI bias");
  PortVfxBuildDesc(vfx, values, 1u << 20, desc);
  Check(desc.opacityGain == 2.f && desc.opacityBias == values[20], "bias-only override retains MATI gain");
}
} // namespace

int main() {
  TestParsing(); TestPhysicalTruncation(); TestDynamic();
  if (!failures) { std::puts("port_vfx_material: ok"); }
  return failures ? 1 : 0;
}
