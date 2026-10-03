#include "port_remastered_effect_convert.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace PortRemastered;

namespace {
int sFailures = 0;

void Check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++sFailures;
  }
}

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

// Retail side: big-endian.
struct Retail {
  std::vector<uint8_t> bytes;
  Retail& f(const char* text) {
    bytes.insert(bytes.end(), text, text + 4);
    return *this;
  }
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

// A root using most of what the converter undoes, with one embedded child.
std::vector<uint8_t> Effect() {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, true);
  PutProperty(out, "DVVN", 1);  // Remastered only
  out.push_back(4);
  PutProperty(out, "MAXP", 1);
  PutConstant(out, 20);
  PutProperty(out, "LTM2", 1);
  PutConstant(out, 30);
  PutProperty(out, "SIZE", 3);
  PutFourCC(out, "MULT");
  PutConstant(out, Bits(2.0f));
  PutConstant(out, Bits(3.0f));
  PutProperty(out, "ROTA", 3);
  PutConstant(out, Bits(5.0f));
  PutProperty(out, "ZBUF", 0);
  out.push_back(1);
  PutProperty(out, "COLR", 3);
  PutFourCC(out, "KEYE");
  Put32(out, 1);   // percent
  Put32(out, 0);   // unknown
  out.push_back(0);  // loop
  out.push_back(0);  // unknown
  Put32(out, 9);   // loop end
  Put32(out, 0);   // loop start
  Put32(out, 2);   // keys
  for (float c : {1.0f, 0.5f, 0.25f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f}) {
    Put32(out, Bits(c));
  }
  PutProperty(out, "VEL1", 3);
  PutFourCC(out, "MPCB");
  PutFourCC(out, "CNST");
  PutConstant(out, Bits(0.0f));
  PutConstant(out, Bits(1.0f));
  PutConstant(out, Bits(0.0f));
  PutProperty(out, "TEXR", 0);
  PutFourCC(out, "CNST");
  PutGuid(out, Legacy(0x1234ABCD));
  PutFourCC(out, "NONE");
  PutProperty(out, "PMDL", 0);
  PutGuid(out, Fresh(0));  // no retail id: dropped
  PutProperty(out, "ICTS", 0);
  PutGuid(out, Legacy(0x0BADF00D));
  PutProperty(out, "_END", 4);
  Put32(out, 1);
  PutGuid(out, Legacy(0x0BADF00D));
  PutGenerator(out, false);
  PutProperty(out, "MAXP", 1);
  PutConstant(out, 5);
  PutProperty(out, "MTIN", 0);
  out.push_back(1);
  PutGuid(out, Fresh(1));
  PutProperty(out, "_END", 4);
  out.insert(out.end(), {'F', 'O', 'O', 'T'});
  return out;
}

void TestRetailId() {
  Check(EffectRetailId(Legacy(0x0034CE07)) == 0x0034CE07u, "carried-over id resolves");
  Check(!EffectRetailId(Fresh(0)).has_value(), "a fresh id has no retail id");
}

void TestConvert() {
  const std::vector<uint8_t> data = Effect();
  EffectNode effect;
  std::string error;
  Check(ParseEffect(data.data(), data.size(), effect, error), "effect parses");
  if (!error.empty()) {
    std::fprintf(stderr, "  %s\n", error.c_str());
  }

  EffectConvertIO io;
  io.materialTexture = [](const EffectGuid& material) { return material == Fresh(1) ? 0x5EED0001u : 0u; };
  const std::vector<ConvertedPart> parts = ConvertEffect(effect, data.data(), io);
  Check(parts.size() == 2, "root and child");
  if (parts.size() != 2) {
    return;
  }

  Retail root;
  root.f("GPSM");
  root.f("MAXP").f("CNST").w(20);
  root.f("LTME").f("CNST").w(29);  // LTM2 is one frame longer
  root.f("SIZE").f("MULT").f("CNST").w(Bits(2.0f)).f("CNST").w(Bits(3.0f));
  root.f("ROTA").f("CNST").w(Bits(-5.0f));
  root.f("ZBUF").f("CNST").b(1);
  root.f("COLR").f("KEYE").w(1).w(0).b(0).b(0).w(9).w(0).w(2);
  for (float c : {1.0f, 0.5f, 0.25f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f}) {
    root.w(Bits(c));
  }
  root.f("VEL1").f("CNST").f("CNST").w(Bits(0.0f)).f("CNST").w(Bits(1.0f)).f("CNST").w(Bits(0.0f));
  root.f("TEXR").f("CNST").f("CNST").w(0x1234ABCD);
  root.f("ICTS").f("CNST").w(0x0BADF00D);
  root.f("_END");
  Check(parts[0].root && parts[0].part == root.bytes, "root converts to retail's bytes");
  Check(parts[0].droppedRetail == 1, "PMDL with no retail id is the one retail property dropped");
  Check(parts[0].dropped.size() == 2, "DVVN and PMDL dropped");

  Retail child;
  child.f("GPSM");
  child.f("MAXP").f("CNST").w(5);
  child.f("TEXR").f("CNST").f("CNST").w(0x5EED0001);
  child.f("_END");
  Check(!parts[1].root && parts[1].id == Legacy(0x0BADF00D), "child keeps its id");
  Check(parts[1].part == child.bytes, "MTIN becomes the child's TEXR");
  Check(parts[1].droppedRetail == 0, "child drops nothing");

  // What the converter writes reads as retail reads it.
  for (const ConvertedPart& part : parts) {
    std::vector<RetailPartProperty> properties;
    Check(SplitRetailPart(part.part.data(), part.part.size(), properties, error), "converted PART reads as retail");
  }
  std::vector<RetailPartProperty> properties;
  Check(SplitRetailPart(root.bytes.data(), root.bytes.size(), properties, error) && properties.size() == 9 &&
            properties[5].fourcc == EffectFourCC("COLR") && properties[5].value.size() == 4 + 22 + 32,
        "retail PART splits into its properties");
}

// The splitter refuses what retail's reader would not take.
void TestSplitRejects() {
  Retail bad;
  bad.f("GPSM").f("SIZE").f("RADD").f("_END");
  std::vector<RetailPartProperty> properties;
  std::string error;
  Check(!SplitRetailPart(bad.bytes.data(), bad.bytes.size(), properties, error) && !error.empty(),
        "unknown element refused");
  Retail spawn;
  spawn.f("GPSM").f("KSSM").f("CNST").w(0).w(0).w(10).w(0).w(1).w(3).w(1).w(0xAABBCCDD).w(0).w(0).w(0).f("_END");
  Check(SplitRetailPart(spawn.bytes.data(), spawn.bytes.size(), properties, error) && properties.size() == 1,
        "KSSM spawn table reads");
}

// An effect with no children has its root flag clear but is still the
// effect's own PART; a colour's keys stored as halves come out as floats; a
// spawn table counts as a retail property left out.
void TestSingleNode() {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, false);
  PutProperty(out, "COLR", 3);
  PutFourCC(out, "KEYE");
  Put32(out, 1);
  Put32(out, 0);
  out.push_back(0);
  out.push_back(0);
  Put32(out, 9);
  Put32(out, 0);
  Put32(out, 1);
  for (uint16_t half : {0x3c00, 0x3800, 0x0000, 0xbc00}) {  // 1, 0.5, 0, -1
    out.push_back(uint8_t(half));
    out.push_back(uint8_t(half >> 8));
  }
  PutProperty(out, "KSSM", 0);
  PutFourCC(out, "NONE");
  PutProperty(out, "_END", 4);
  EffectNode effect;
  std::string error;
  Check(ParseEffect(out.data(), out.size(), effect, error), "single-node effect parses");
  const std::vector<ConvertedPart> parts = ConvertEffect(effect, out.data(), {});
  Check(parts.size() == 1 && parts[0].root, "the only GPSM is the root");
  Retail want;
  want.f("GPSM").f("COLR").f("KEYE").w(1).w(0).b(0).b(0).w(9).w(0).w(1);
  want.w(Bits(1.0f)).w(Bits(0.5f)).w(Bits(0.0f)).w(Bits(-1.0f)).f("_END");
  Check(parts.size() == 1 && parts[0].part == want.bytes, "half colour keys widen to floats");
  Check(parts.size() == 1 && parts[0].dropped.empty(), "an empty KSSM is nothing left out");
}

// Properties retail reads as something else than Remastered wrote are left out.
void TestRejects() {
  std::vector<uint8_t> out(0x3c, 0);
  std::memcpy(out.data(), "RFRM", 4);
  std::memcpy(out.data() + 0x14, "GENP", 4);
  PutGenerator(out, true);
  PutProperty(out, "SIZE", 3);
  PutFourCC(out, "RADD");  // Remastered's, not retail's
  PutConstant(out, Bits(1.0f));
  PutConstant(out, Bits(2.0f));
  PutProperty(out, "LFOT", 0);
  out.push_back(3);
  PutProperty(out, "_END", 4);
  Put32(out, 0);
  EffectNode effect;
  std::string error;
  Check(ParseEffect(out.data(), out.size(), effect, error), "reject effect parses");
  const std::vector<ConvertedPart> parts = ConvertEffect(effect, out.data(), {});
  Check(parts.size() == 1 && parts[0].droppedRetail == 1, "RADD in SIZE is dropped");
  Retail root;
  root.f("GPSM").f("LFOT").f("CNST").w(2).f("_END");
  Check(parts.size() == 1 && parts[0].part == root.bytes, "LFOT byte 3 is retail 2");
}
}  // namespace

int main() {
  TestRetailId();
  TestConvert();
  TestRejects();
  TestSingleNode();
  TestSplitRejects();
  if (sFailures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", sFailures);
    return 1;
  }
  std::printf("port_remastered_effect_convert: ok\n");
  return 0;
}
