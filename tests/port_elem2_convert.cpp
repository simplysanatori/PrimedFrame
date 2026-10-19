#include "port_remastered_effect_convert.h"
#include "port_vfx_opacity.h"
#include "Kyoto/Math/CMatrix3f.hpp"
#include "Kyoto/Math/CQuaternion.hpp"
#include "Kyoto/Particles/CVectorElement.hpp"
#include "Kyoto/Particles/IElement.hpp"
#include "port_test.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace PortRemastered;

namespace {

// Remastered side: little-endian, FourCCs byte-reversed.
void Put32(std::vector<uint8_t>& out, uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(uint8_t(value >> (i * 8)));
  }
}

uint32_t Bits(float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  return bits;
}

void PutFourCC(std::vector<uint8_t>& out, const char* text) {
  for (int i = 3; i >= 0; --i) {
    out.push_back(uint8_t(text[i]));
  }
}

void PutProperty(std::vector<uint8_t>& out, const char* name, uint8_t tag) {
  PutFourCC(out, name);
  out.push_back(tag);
}

void PutConstant(std::vector<uint8_t>& out, uint32_t word) {
  PutFourCC(out, "CNST");
  Put32(out, word);
}

void PutGenerator(std::vector<uint8_t>& out, bool root) {
  PutFourCC(out, "GPSM");
  out.resize(out.size() + 17);
  Put32(out, root ? 1 : 0);
}

void PutGuid(std::vector<uint8_t>& out, const EffectGuid& guid) { out.insert(out.end(), guid.begin(), guid.end()); }

// An id carried over from retail, as an effect stores it.
EffectGuid Legacy(uint32_t retail) {
  EffectGuid guid = {0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0xf0, 0xf0, 0x00, 0x00, 0x00};
  guid[12] = uint8_t(retail >> 24);
  guid[13] = uint8_t(retail >> 16);
  guid[14] = uint8_t(retail >> 8);
  guid[15] = uint8_t(retail);
  return guid;
}

EffectGuid Fresh(uint8_t seed) {
  EffectGuid guid;
  for (size_t i = 0; i < guid.size(); ++i) {
    guid[i] = uint8_t(0xa0 + seed + i);
  }
  return guid;
}

// A texture carried over from retail, so the generator draws something.
void PutTexr(std::vector<uint8_t>& out, uint32_t retail) {
  PutProperty(out, "TEXR", 0);
  PutFourCC(out, "CNST");
  PutGuid(out, Legacy(retail));
  PutFourCC(out, "NONE");
}

// Retail side: big-endian.
struct Retail {
  std::vector<uint8_t> bytes;
  Retail& f(const char* text) {
    bytes.insert(bytes.end(), text, text + 4);
    return *this;
  }
  // The end of a converted PART: Remastered's default PSLT, the port-only PIRN marker, then _END.
  Retail& end() { return pslt().f("PIRN").f("CNST").w(1).f("_END"); }
  // An embedded child's end: PSLT, PIRN, then PGID with its GUID's first word read little-endian.
  Retail& end(const EffectGuid& id) {
    return pslt().f("PIRN").f("CNST").w(1).f("PGID").f("CNST")
        .w(uint32_t(id[0]) | uint32_t(id[1]) << 8 | uint32_t(id[2]) << 16 | uint32_t(id[3]) << 24)
        .f("_END");
  }
  // Remastered's system lifetime without a PSLT (CElementGen ctor 0x244cfc).
  Retail& pslt() { return f("PSLT").f("CNST").w(0x1000000u); }
  Retail& w(uint32_t value) {
    for (int i = 3; i >= 0; --i) {
      bytes.push_back(uint8_t(value >> (i * 8)));
    }
    return *this;
  }
  Retail& b(uint8_t value) {
    bytes.push_back(value);
    return *this;
  }
};

Retail& RetailVector(Retail& out, float x, float y, float z) {
  out.f("CNST").f("CNST").w(Bits(x)).f("CNST").w(Bits(y)).f("CNST").w(Bits(z));
  return out;
}


// Element batch 2 converter output: NEGT, then the PLNV count rule, CRCV, SPEV and ELPS.

// A generator with the given properties written by `props`, one TEXR so it draws something.
std::vector<ConvertedPart> ConvertGen(const std::function<void(std::vector<uint8_t>&)>& props, EffectNode& effect,
                                      std::vector<uint8_t>& out) {
  out.assign(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, false);
  props(out);
  PutTexr(out, 0x1234ABCD);
  PutProperty(out, "_END", 4);
  std::string error;
  CHECK_MSG(ParseEffect(out.data(), out.size(), effect, error), "effect parses");
  return ConvertEffect(effect, out.data(), {});
}

bool Contains(const std::vector<uint8_t>& bytes, const std::vector<uint8_t>& what) {
  return std::search(bytes.begin(), bytes.end(), what.begin(), what.end()) != bytes.end();
}

void Show(const ConvertedPart& part) {
  for (const std::string& d : part.dropped) {
    std::fprintf(stderr, "  dropped %s\n", d.c_str());
  }
}

// NEGT(x) is MULT(x, CNST -1).
void TestNegate() {
  EffectNode effect;
  std::vector<uint8_t> out;
  const auto parts = ConvertGen(
      [](std::vector<uint8_t>& o) {
        PutProperty(o, "SIZE", 3);
        PutFourCC(o, "RAND");
        PutFourCC(o, "NEGT");
        PutConstant(o, Bits(2.0f));
        PutConstant(o, Bits(2.0f));
      },
      effect, out);
  Retail want;
  want.f("GPSM");
  want.f("SIZE").f("RAND").f("MULT").f("CNST").w(Bits(2.0f)).f("CNST").w(Bits(-1.0f)).f("CNST").w(Bits(2.0f));
  want.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD).end();
  CHECK_MSG(parts.size() == 1 && parts[0].part == want.bytes, "RAND(NEGT(a), a) is RAND(MULT(a, CNST -1), a)");
  CHECK_MSG(parts.size() == 1 && parts[0].dropped.empty(), "NEGT drops nothing");
  if (parts.size() == 1) {
    Show(parts[0]);
  }
}

}  // namespace

int main() {
  TestNegate();
  if (port_test::Failures() == 0) {
    std::printf("port_elem2_convert: ok\n");
  }
  return PORT_TEST_RESULT();
}

void* IElement::CElementAllocator::Alloc(size_t size, const char*, const char*) { return ::operator new(size); }
void IElement::CElementAllocator::Free(void* ptr, size_t) { ::operator delete(ptr); }
void* IElement::operator new(size_t size, const char* file, const char* type) {
  return CElementAllocator::Alloc(size, file, type);
}
void IElement::operator delete(void* ptr, size_t size) { CElementAllocator::Free(ptr, size); }
