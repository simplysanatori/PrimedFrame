#include "port_remastered_effect_convert.h"

#include <cstring>
#include <unordered_map>

namespace PortRemastered {
namespace {

constexpr uint32_t F(const char (&text)[5]) { return EffectFourCC(text); }

// The types CParticleDataFactory reads a value as.
enum class Type : uint8_t { Int, Real, Vector, ModVector, Color, Emitter, Texture, Bool, Asset };

// Retail's properties (CParticleDataFactory::CreateGPSM) and the type each is
// read as. An Asset property also names the type of asset it refers to.
struct PropertyType {
  Type type;
  uint32_t asset = 0;
};

const std::unordered_map<uint32_t, PropertyType>& RetailProperties() {
  static const std::unordered_map<uint32_t, PropertyType> table = [] {
    std::unordered_map<uint32_t, PropertyType> t;
    auto add = [&t](const char* names, Type type, uint32_t asset = 0) {
      for (const char* c = names; *c != 0;) {
        const uint32_t fourcc = uint32_t(uint8_t(c[0])) << 24 | uint32_t(uint8_t(c[1])) << 16 |
                                uint32_t(uint8_t(c[2])) << 8 | uint32_t(uint8_t(c[3]));
        t[fourcc] = {type, asset};
        c += 4;
        while (*c == ' ') {
          ++c;
        }
      }
    };
    add("PSLT PSWT MBSP MAXP LTME SEED NCSY CSSD NDSY PISY SISY SSSD SESD LTYP LFOT", Type::Int);
    add("PSTS GRTE SIZE ROTA LENG WIDT LINT LFOR LSLA ADV1 ADV2 ADV3 ADV4 ADV5 ADV6 ADV7 ADV8", Type::Real);
    add("PSIV PSOV ILOC IVEC POFS PMOP PMRT PMSC SSPO SEPO LOFF LDIR", Type::Vector);
    add("PSVM VEL1 VEL2 VEL3 VEL4", Type::ModVector);
    add("COLR PMCL LCLR", Type::Color);
    add("EMTR", Type::Emitter);
    add("TEXR TIND", Type::Texture);
    add("LIT_ ORNT RSOP AAPH ZBUF SORT MBLR VMD1 VMD2 VMD3 VMD4 CIND PMAB PMUS PMOO LINE FXLL OPTS", Type::Bool);
    add("PMDL", Type::Asset, F("CMDL"));
    add("ICTS IDTS IITS", Type::Asset, F("PART"));
    add("SSWH", Type::Asset, F("SWHC"));
    add("SELC", Type::Asset, F("ELSC"));
    return t;
  }();
  return table;
}

// Each retail element per type, and its arguments: i int, r real, v vector,
// m mod vector, c colour, B bool (CNST and a byte), k a keyframe block, # a
// literal of the type (CNST's int or float). Empty is no arguments.
using Signatures = std::unordered_map<uint32_t, const char*>;

const Signatures& ElementsOf(Type type) {
  static const Signatures intElements = {
      {F("CNST"), "#"},  {F("KEYE"), "k"},  {F("KEYP"), "k"},   {F("TSCL"), "r"},  {F("DETH"), "ii"},
      {F("CHAN"), "iii"}, {F("ADD_"), "ii"}, {F("MULT"), "ii"},  {F("MODU"), "ii"}, {F("RAND"), "ii"},
      {F("IMPL"), "i"},  {F("ILPT"), "i"},  {F("SPAH"), "iii"}, {F("IRND"), "ii"}, {F("CLMP"), "iii"},
      {F("PULS"), "iiii"}, {F("NONE"), ""}, {F("RTOI"), "rr"},  {F("SUB_"), "ii"}, {F("GTCP"), ""},
      {F("GAPC"), ""},   {F("GEMT"), ""},
  };
  static const Signatures realElements = {
      {F("CNST"), "#"},   {F("NONE"), ""},    {F("KEYE"), "k"},    {F("KEYP"), "k"},   {F("SCAL"), "r"},
      {F("SINE"), "rrr"}, {F("ADD_"), "rr"},  {F("MULT"), "rr"},   {F("DOTP"), "vv"},  {F("RAND"), "rr"},
      {F("IRND"), "rr"},  {F("CHAN"), "rri"}, {F("CLMP"), "rrr"},  {F("PULS"), "iirr"}, {F("RLPT"), "r"},
      {F("LFTW"), "rr"},  {F("PRLW"), ""},    {F("PSLL"), ""},     {F("PAP1"), ""},    {F("PAP2"), ""},
      {F("PAP3"), ""},    {F("PAP4"), ""},    {F("PAP5"), ""},     {F("PAP6"), ""},    {F("PAP7"), ""},
      {F("PAP8"), ""},    {F("VXTR"), "v"},   {F("VYTR"), "v"},    {F("VZTR"), "v"},   {F("VMAG"), "v"},
      {F("ISWT"), "rr"},  {F("CLTN"), "rrrr"}, {F("CEQL"), "rrrr"}, {F("CRNG"), "rrrrr"}, {F("CEXT"), "i"},
      {F("ITRL"), "ir"},  {F("SUB_"), "rr"},  {F("GTCR"), "c"},    {F("GTCG"), "c"},   {F("GTCB"), "c"},
      {F("GTCA"), "c"},
  };
  static const Signatures vectorElements = {
      {F("NONE"), ""},      {F("CNST"), "rrr"}, {F("KEYE"), "k"},  {F("KEYP"), "k"},   {F("ANGC"), "rrrrr"},
      {F("CONE"), "vr"},    {F("CIRC"), "vvrrr"}, {F("CCLU"), "vvir"}, {F("ADD_"), "vv"}, {F("MULT"), "vv"},
      {F("CHAN"), "vvi"},   {F("PULS"), "iivv"}, {F("RTOV"), "r"},  {F("PLOC"), ""},    {F("PLCO"), ""},
      {F("PVEL"), ""},      {F("PSOF"), ""},    {F("PSOU"), ""},   {F("PSOR"), ""},    {F("PSTR"), ""},
      {F("SUB_"), "vv"},    {F("CTVC"), "c"},
  };
  static const Signatures modVectorElements = {
      {F("NONE"), ""},     {F("CNST"), "rrr"},  {F("GRAV"), "v"},    {F("WIND"), "vr"},  {F("EXPL"), "rr"},
      {F("CHAN"), "mmi"},  {F("PULS"), "iimm"}, {F("IMPL"), "vrrrB"}, {F("LMPL"), "vrrrB"}, {F("EMPL"), "vrrrB"},
      {F("SWRL"), "vvrr"}, {F("BNCE"), "vvrrB"}, {F("SPOS"), "v"},
  };
  static const Signatures colorElements = {
      {F("CNST"), "rrrr"}, {F("KEYE"), "k"},   {F("KEYP"), "k"},    {F("FADE"), "ccr"}, {F("CFDE"), "ccrr"},
      {F("CHAN"), "cci"},  {F("PULS"), "iicc"}, {F("PCOL"), ""},    {F("NONE"), ""},
  };
  // SETR is left out: Remastered writes SEMR, which retail reads too.
  static const Signatures emitterElements = {
      {F("NONE"), ""}, {F("SEMR"), "vv"}, {F("SPHE"), "vrr"}, {F("ASPH"), "vrrrrrr"},
  };
  static const Signatures none;
  switch (type) {
  case Type::Int:
    return intElements;
  case Type::Real:
    return realElements;
  case Type::Vector:
    return vectorElements;
  case Type::ModVector:
    return modVectorElements;
  case Type::Color:
    return colorElements;
  case Type::Emitter:
    return emitterElements;
  default:
    return none;
  }
}

Type TypeOfLetter(char letter) {
  switch (letter) {
  case 'i':
    return Type::Int;
  case 'r':
    return Type::Real;
  case 'v':
    return Type::Vector;
  case 'm':
    return Type::ModVector;
  case 'c':
    return Type::Color;
  default:
    return Type::Bool;
  }
}

uint32_t Le32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }

void PutBe32(std::vector<uint8_t>& out, uint32_t value) {
  out.push_back(uint8_t(value >> 24));
  out.push_back(uint8_t(value >> 16));
  out.push_back(uint8_t(value >> 8));
  out.push_back(uint8_t(value));
}

uint32_t FloatBits(float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  return bits;
}

float HalfToFloat(uint16_t half) {
  const uint32_t sign = uint32_t(half >> 15) << 31;
  const int exponent = (half >> 10) & 0x1f;
  const uint32_t mantissa = half & 0x3ff;
  uint32_t bits;
  if (exponent == 0) {
    // Zero or subnormal: value = mantissa * 2^-24.
    float value = float(mantissa) * (1.0f / 16777216.0f);
    std::memcpy(&bits, &value, 4);
    bits |= sign;
  } else if (exponent == 31) {
    bits = sign | 0x7f800000u | mantissa << 13;
  } else {
    bits = sign | uint32_t(exponent - 15 + 127) << 23 | mantissa << 13;
  }
  float out;
  std::memcpy(&out, &bits, 4);
  return out;
}

bool IsElement(const EffectValue& value, uint32_t fourcc) {
  return value.kind == EffectValue::Kind::Element && value.fourcc == fourcc;
}

class Writer {
public:
  Writer(const uint8_t* data, const EffectConvertIO& io) : m_data(data), m_io(io) {}

  uint32_t AssetId(const EffectGuid& id, uint32_t type) const {
    if (m_io.assetId) {
      return m_io.assetId(id, type);
    }
    return EffectRetailId(id).value_or(0);
  }

  // One element of `type` into `out`, or false with `why` set.
  bool Element(const EffectValue& value, Type type, std::vector<uint8_t>& out, std::string& why) const {
    if (value.kind != EffectValue::Kind::Element) {
      why = "a bare value where retail reads an element";
      return false;
    }
    // MPCB wraps Remastered's vectors; its cartesian form is the vector itself,
    // and MPCB(MPAC(x bias, y bias, x range, y range), magnitude) is retail's
    // ANGC with the same five arguments (checked against the disc's pairs).
    if (value.fourcc == F("MPCB") && type == Type::Vector) {
      if (value.args.size() == 1) {
        return Element(value.args[0], type, out, why);
      }
      if (value.args.size() == 2 && IsElement(value.args[0], F("MPAC")) && value.args[0].args.size() == 4) {
        PutBe32(out, F("ANGC"));
        for (const EffectValue& arg : value.args[0].args) {
          if (!Element(arg, Type::Real, out, why)) {
            return false;
          }
        }
        return Element(value.args[1], Type::Real, out, why);
      }
      why = "MPCB in a form retail has no element for";
      return false;
    }
    if (value.fourcc == F("MPCB") && type == Type::ModVector) {
      if (value.args.size() != 1) {
        why = "MPCB in its angle form where retail reads a mod vector";
        return false;
      }
      return Element(value.args[0], type, out, why);
    }
    // ANCR(REUL(0, 0, 0, #00), x range, y range, magnitude) is retail's ANGC
    // with no bias (the disc writes the biases as -0). A rotated cone is not
    // converted: what REUL's angles do to the cone is not pinned down yet.
    if (value.fourcc == F("ANCR") && type == Type::Vector && value.args.size() == 4 &&
        IsElement(value.args[0], F("REUL")) && value.args[0].args.size() == 4) {
      for (size_t i = 0; i < 3; ++i) {
        const EffectValue& angle = value.args[0].args[i];
        if (!IsElement(angle, F("CNST")) || angle.args.size() != 1 || (angle.args[0].word & 0x7fffffffu) != 0) {
          why = "ANCR with a rotated cone";
          return false;
        }
      }
      PutBe32(out, F("ANGC"));
      PutBe32(out, F("CNST"));
      PutBe32(out, 0x80000000u);
      PutBe32(out, F("CNST"));
      PutBe32(out, 0x80000000u);
      for (size_t i = 1; i < 4; ++i) {
        if (!Element(value.args[i], Type::Real, out, why)) {
          return false;
        }
      }
      return true;
    }
    // MPRD(a, b): a random int between two, as RAND.
    if (value.fourcc == F("MPRD") && value.args.size() == 2 && (type == Type::Int || type == Type::Real)) {
      PutBe32(out, F("RAND"));
      return Element(value.args[0], type, out, why) && Element(value.args[1], type, out, why);
    }
    // DFCP and DFCS scale a real by something retail cannot compute (they look
    // like fades with the camera's distance); they are taken as 1.
    if ((value.fourcc == F("DFCP") || value.fourcc == F("DFCS")) && type == Type::Real) {
      m_approximated.push_back(EffectFourCCString(value.fourcc) + " taken as 1");
      PutBe32(out, F("CNST"));
      PutBe32(out, FloatBits(1.0f));
      return true;
    }
    const Signatures& elements = ElementsOf(type);
    const auto found = elements.find(value.fourcc);
    if (found == elements.end()) {
      why = "retail has no element " + EffectFourCCString(value.fourcc) + " there";
      return false;
    }
    const std::string sig = found->second;
    PutBe32(out, value.fourcc);
    if (sig == "#") {
      return Literal(value, type, out, why);
    }
    if (sig == "k") {
      return value.args.size() == 1 && Keyframes(value.args[0], type, out, why);
    }
    if (value.args.size() != sig.size()) {
      why = EffectFourCCString(value.fourcc) + " with " + std::to_string(value.args.size()) + " arguments, retail reads " +
            std::to_string(sig.size());
      return false;
    }
    for (size_t i = 0; i < sig.size(); ++i) {
      const bool ok = sig[i] == 'B' ? Bool(value.args[i], out, why) : Element(value.args[i], TypeOfLetter(sig[i]), out, why);
      if (!ok) {
        return false;
      }
    }
    return true;
  }

  // CNST's literal: retail reads an s32 for an int and a float for a real.
  bool Literal(const EffectValue& value, Type type, std::vector<uint8_t>& out, std::string& why) const {
    if (value.args.size() != 1) {
      why = "CNST with " + std::to_string(value.args.size()) + " arguments";
      return false;
    }
    const EffectValue& arg = value.args[0];
    if (arg.kind == EffectValue::Kind::Word) {
      PutBe32(out, arg.word);
      return true;
    }
    if (arg.kind == EffectValue::Kind::Byte) {
      PutBe32(out, type == Type::Int ? arg.word : FloatBits(float(arg.word)));
      return true;
    }
    why = "CNST holding neither a number nor a byte";
    return false;
  }

  // GetBool: a class id (CNST) and a byte.
  bool Bool(const EffectValue& value, std::vector<uint8_t>& out, std::string& why) const {
    const EffectValue* byte = &value;
    if (IsElement(value, F("CNST")) && value.args.size() == 1) {
      byte = &value.args[0];
    }
    if (byte->kind != EffectValue::Kind::Byte && byte->kind != EffectValue::Kind::Word) {
      why = "not a flag";
      return false;
    }
    PutBe32(out, F("CNST"));
    out.push_back(byte->word != 0 ? 1 : 0);
    return true;
  }

  // A keyframe block: percent, unknown, loop, unknown (u32 u32 u8 u8), loop
  // end and start (u32 u32), a count and that many keys of the type's size:
  // 4 bytes for an int or real, 12 for a vector, 16 for a colour. Remastered
  // may store a colour's key as four halves; those are widened.
  bool Keyframes(const EffectValue& value, Type type, std::vector<uint8_t>& out, std::string& why) const {
    if (value.kind != EffectValue::Kind::Keys || value.size < 22) {
      why = "not a keyframe block";
      return false;
    }
    const uint8_t* p = m_data + value.offset;
    const uint32_t count = Le32(p + 18);
    const size_t keys = value.size - 22;
    const size_t want = type == Type::Color ? 16 : type == Type::Vector ? 12 : 4;
    const size_t keySize = count == 0 ? want : keys / count;
    const bool halves = type == Type::Color && keySize == 8;
    if ((count == 0 && keys != 0) || (count != 0 && keys % count != 0) || (keySize != want && !halves)) {
      why = "keyframes of " + std::to_string(keySize) + " bytes where retail reads " + std::to_string(want);
      return false;
    }
    PutBe32(out, Le32(p));
    PutBe32(out, Le32(p + 4));
    out.push_back(p[8]);
    out.push_back(p[9]);
    PutBe32(out, Le32(p + 10));
    PutBe32(out, Le32(p + 14));
    PutBe32(out, count);
    if (halves) {
      for (size_t at = 22; at < value.size; at += 2) {
        PutBe32(out, FloatBits(HalfToFloat(uint16_t(p[at] | p[at + 1] << 8))));
      }
      return true;
    }
    for (size_t at = 22; at < value.size; at += 4) {
      PutBe32(out, Le32(p + at));
    }
    return true;
  }

  // An id, bare or as CNST(id); NONE is no asset.
  bool Asset(const std::vector<EffectValue>& value, uint32_t type, std::vector<uint8_t>& out, std::string& why) const {
    if (value.size() == 1 && IsElement(value[0], F("NONE"))) {
      PutBe32(out, F("NONE"));
      return true;
    }
    const EffectValue* guid = nullptr;
    if (value.size() == 1 && value[0].kind == EffectValue::Kind::Guid) {
      guid = &value[0];
    } else if (value.size() == 1 && IsElement(value[0], F("CNST")) && value[0].args.size() == 1 &&
               value[0].args[0].kind == EffectValue::Kind::Guid) {
      guid = &value[0].args[0];
    } else if (value.size() == 2 && value[0].kind == EffectValue::Kind::Byte && value[1].kind == EffectValue::Kind::Guid) {
      guid = &value[1];
    }
    if (guid == nullptr) {
      why = "not an asset id";
      return false;
    }
    const uint32_t id = AssetId(guid->guid, type);
    if (id == 0) {
      why = EffectFourCCString(type) + " " + EffectGuidString(guid->guid) + " has no retail id";
      return false;
    }
    PutBe32(out, F("CNST"));
    PutBe32(out, id);
    return true;
  }

  // TEXR/TIND: Remastered's `CNST(id), NONE` or `ATEX(id, ...)`, retail's
  // `CNST CNST id` and `ATEX CNST id ...`.
  bool Texture(const std::vector<EffectValue>& value, std::vector<uint8_t>& out, std::string& why) const {
    if (value.empty() || value[0].kind != EffectValue::Kind::Element) {
      why = "not a texture";
      return false;
    }
    const EffectValue& head = value[0];
    if (head.fourcc == F("NONE")) {
      PutBe32(out, F("NONE"));
      return true;
    }
    const bool animated = head.fourcc == F("ATEX");
    if ((head.fourcc != F("CNST") && !animated) || head.args.empty() || head.args[0].kind != EffectValue::Kind::Guid) {
      why = "texture element " + EffectFourCCString(head.fourcc) + " retail does not have";
      return false;
    }
    const uint32_t id = AssetId(head.args[0].guid, F("TXTR"));
    if (id == 0) {
      why = "TXTR " + EffectGuidString(head.args[0].guid) + " has no retail id";
      return false;
    }
    PutBe32(out, head.fourcc);
    PutBe32(out, F("CNST"));
    PutBe32(out, id);
    if (!animated) {
      return true;
    }
    // ATEX: tile width and height, stride width and height, cycle frames, loop.
    const char* sig = "iiiiiB";
    if (head.args.size() != 7) {
      why = "ATEX with " + std::to_string(head.args.size()) + " arguments";
      return false;
    }
    for (size_t i = 0; i < 6; ++i) {
      const bool ok = sig[i] == 'B' ? Bool(head.args[i + 1], out, why) : Element(head.args[i + 1], Type::Int, out, why);
      if (!ok) {
        return false;
      }
    }
    return true;
  }

  // One property's value, as the type retail reads it.
  bool Property(uint32_t fourcc, const PropertyType& type, const std::vector<EffectValue>& value,
                std::vector<uint8_t>& out, std::string& why) const {
    switch (type.type) {
    case Type::Bool:
      if (value.size() != 1) {
        why = "not a flag";
        return false;
      }
      return Bool(value[0], out, why);
    case Type::Texture:
      return Texture(value, out, why);
    case Type::Asset:
      return Asset(value, type.asset, out, why);
    default:
      break;
    }
    // LFOT and LTYP: Remastered writes the enum as a byte, one above retail's int.
    if (fourcc == F("LFOT") || fourcc == F("LTYP")) {
      const EffectValue* byte = value.size() == 1 ? &value[0] : nullptr;
      if (byte != nullptr && IsElement(*byte, F("CNST")) && byte->args.size() == 1) {
        byte = &byte->args[0];
      }
      if (byte != nullptr && byte->kind == EffectValue::Kind::Byte) {
        if (byte->word == 0) {
          why = "enum byte 0";
          return false;
        }
        PutBe32(out, F("CNST"));
        PutBe32(out, byte->word - 1);
        return true;
      }
    }
    if (value.size() != 1) {
      why = "a value of " + std::to_string(value.size()) + " parts where retail reads one element";
      return false;
    }
    // ROTA: Remastered turns the other way. MULT(x, -1) is x in retail; anything
    // else is wrapped in MULT(..., CNST -1).
    // LTM2 is one frame longer than retail's LTME: constants, and the bounds
    // of a random lifetime, come down by one.
    if (fourcc == F("LTME")) {
      const EffectValue& v = value[0];
      auto constant = [](const EffectValue& c) {
        return IsElement(c, F("CNST")) && c.args.size() == 1 &&
               (c.args[0].kind == EffectValue::Kind::Word || c.args[0].kind == EffectValue::Kind::Byte);
      };
      auto putLess = [&out](const EffectValue& c) {
        PutBe32(out, F("CNST"));
        PutBe32(out, c.args[0].word == 0 ? 0 : c.args[0].word - 1);
      };
      if (constant(v)) {
        putLess(v);
        return true;
      }
      if ((IsElement(v, F("IRND")) || IsElement(v, F("RAND")) || IsElement(v, F("MPRD"))) && v.args.size() == 2 &&
          constant(v.args[0]) && constant(v.args[1])) {
        PutBe32(out, v.fourcc == F("MPRD") ? F("RAND") : v.fourcc);
        putLess(v.args[0]);
        putLess(v.args[1]);
        return true;
      }
      PutBe32(out, F("SUB_"));
      if (!Element(v, Type::Int, out, why)) {
        return false;
      }
      PutBe32(out, F("CNST"));
      PutBe32(out, 1);
      return true;
    }
    if (fourcc == F("ROTA")) {
      const EffectValue& v = value[0];
      if (IsElement(v, F("CNST")) && v.args.size() == 1 && v.args[0].kind == EffectValue::Kind::Word) {
        PutBe32(out, F("CNST"));
        PutBe32(out, v.args[0].word ^ 0x80000000u);
        return true;
      }
      if (IsElement(v, F("MULT")) && v.args.size() == 2 && IsElement(v.args[1], F("CNST")) && v.args[1].args.size() == 1 &&
          v.args[1].args[0].kind == EffectValue::Kind::Word && v.args[1].args[0].word == FloatBits(-1.0f)) {
        return Element(v.args[0], Type::Real, out, why);
      }
      PutBe32(out, F("MULT"));
      if (!Element(v, Type::Real, out, why)) {
        return false;
      }
      PutBe32(out, F("CNST"));
      PutBe32(out, FloatBits(-1.0f));
      return true;
    }
    return Element(value[0], type.type, out, why);
  }

  ConvertedPart Generator(const EffectNode& node) const {
    ConvertedPart result;
    result.id = node.id;
    result.root = node.root;
    std::vector<uint8_t>& out = result.part;
    PutBe32(out, F("GPSM"));
    const auto& retail = RetailProperties();
    bool texture = false;
    const EffectProperty* material = nullptr;
    for (const EffectProperty& property : node.properties) {
      const uint32_t fourcc = property.fourcc == F("LTM2") ? F("LTME") : property.fourcc;
      if (fourcc == F("MTIN")) {
        material = &property;
        continue;
      }
      // Retail reads KSSM, but Remastered's spawn table is laid out differently.
      if (fourcc == F("KSSM")) {
        // The reader keeps a spawn table as raw bytes: NONE is the FourCC alone.
        const EffectValue& table = property.value.empty() ? EffectValue() : property.value[0];
        const bool none = IsElement(table, F("NONE")) ||
                          (table.kind == EffectValue::Kind::Raw && table.size == 4 && Le32(m_data + table.offset) == F("NONE"));
        if (!none) {
          result.dropped.push_back("KSSM: spawn table not converted yet");
          ++result.droppedRetail;
        }
        continue;
      }
      const auto found = retail.find(fourcc);
      if (found == retail.end()) {
        result.dropped.push_back(EffectFourCCString(property.fourcc) + ": Remastered only");
        continue;
      }
      std::vector<uint8_t> bytes;
      std::string why;
      if (!Property(fourcc, found->second, property.value, bytes, why)) {
        result.dropped.push_back(EffectFourCCString(property.fourcc) + ": " + why);
        ++result.droppedRetail;
        continue;
      }
      if (fourcc == F("TEXR")) {
        texture = true;
      }
      PutBe32(out, fourcc);
      out.insert(out.end(), bytes.begin(), bytes.end());
    }
    // A material instance draws with its texture where there is no TEXR.
    if (material != nullptr && !texture) {
      const EffectValue* guid = nullptr;
      for (const EffectValue& value : material->value) {
        if (value.kind == EffectValue::Kind::Guid) {
          guid = &value;
        }
      }
      const uint32_t id = guid != nullptr && m_io.materialTexture ? m_io.materialTexture(guid->guid) : 0;
      if (id != 0) {
        PutBe32(out, F("TEXR"));
        PutBe32(out, F("CNST"));
        PutBe32(out, F("CNST"));
        PutBe32(out, id);
      } else {
        result.dropped.push_back("MTIN: no texture for its material");
        ++result.droppedRetail;
      }
    } else if (material != nullptr) {
      result.dropped.push_back("MTIN: the effect has a TEXR");
    }
    PutBe32(out, F("_END"));
    result.approximated = std::move(m_approximated);
    m_approximated.clear();
    return result;
  }

  // The top node is the effect's own PART whatever its root flag says
  // (Remastered sets it only on effects with embedded children).
  void Collect(const EffectNode& node, bool top, std::vector<ConvertedPart>& out) const {
    if (node.form == F("GPSM")) {
      out.push_back(Generator(node));
      out.back().root = top;
    }
    for (const EffectNode& child : node.children) {
      Collect(child, false, out);
    }
  }

private:
  const uint8_t* m_data;
  const EffectConvertIO& m_io;
  mutable std::vector<std::string> m_approximated;  // for the generator being written
};


uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]); }

// Walks a retail PART the way CParticleDataFactory reads it.
class RetailReader {
public:
  RetailReader(const uint8_t* data, size_t size) : m_data(data), m_size(size) {}

  size_t At() const { return m_at; }

  bool Word(uint32_t& out) {
    if (m_at + 4 > m_size) {
      return false;
    }
    out = Be32(m_data + m_at);
    m_at += 4;
    return true;
  }

  bool Skip(size_t bytes) {
    if (m_at + bytes > m_size) {
      return false;
    }
    m_at += bytes;
    return true;
  }

  bool Bool() {
    uint32_t fourcc;
    return Word(fourcc) && Skip(1);
  }

  // A keyframe block of `keySize`-byte keys.
  bool Keyframes(size_t keySize) {
    uint32_t count;
    return Skip(18) && Word(count) && Skip(size_t(count) * keySize);
  }

  bool Element(Type type) {
    uint32_t fourcc;
    if (!Word(fourcc)) {
      return false;
    }
    if (type == Type::Emitter && fourcc == F("SETR")) {
      uint32_t name;
      return Word(name) && Element(Type::Vector) && Word(name) && Element(Type::Vector);
    }
    const Signatures& elements = ElementsOf(type);
    const auto found = elements.find(fourcc);
    if (found == elements.end()) {
      m_error = "no " + EffectFourCCString(fourcc) + " element of that type";
      return false;
    }
    for (const char* c = found->second; *c != 0; ++c) {
      bool ok;
      switch (*c) {
      case '#':
        ok = Skip(4);
        break;
      case 'k': {
        const size_t sizes[] = {4, 4, 12, 12, 16, 0, 0, 0, 0};
        ok = Keyframes(sizes[size_t(type)]);
        break;
      }
      case 'B':
        ok = Bool();
        break;
      default:
        ok = Element(TypeOfLetter(*c));
        break;
      }
      if (!ok) {
        return false;
      }
    }
    return true;
  }

  bool Texture() {
    uint32_t fourcc;
    if (!Word(fourcc)) {
      return false;
    }
    if (fourcc == F("NONE")) {
      return true;
    }
    uint32_t sub;
    if ((fourcc != F("CNST") && fourcc != F("ATEX")) || !Word(sub) || (sub != F("NONE") && !Skip(4))) {
      return false;
    }
    if (fourcc == F("CNST")) {
      return true;
    }
    for (int i = 0; i < 5; ++i) {
      if (!Element(Type::Int)) {
        return false;
      }
    }
    return Bool();
  }

  bool Asset() {
    uint32_t fourcc;
    return Word(fourcc) && (fourcc == F("NONE") || Skip(4));
  }

  // KSSM: NONE, or CNST, four ints and a frame table (frame, count, 16 bytes each).
  bool SpawnTable() {
    uint32_t fourcc;
    if (!Word(fourcc)) {
      return false;
    }
    if (fourcc != F("CNST")) {
      return true;
    }
    uint32_t frames;
    if (!Skip(16) || !Word(frames)) {
      return false;
    }
    for (uint32_t i = 0; i < frames; ++i) {
      uint32_t count;
      if (!Skip(4) || !Word(count) || !Skip(size_t(count) * 16)) {
        return false;
      }
    }
    return true;
  }

  const std::string& Error() const { return m_error; }

private:
  const uint8_t* m_data;
  size_t m_size;
  size_t m_at = 0;
  std::string m_error;
};

}  // namespace

bool SplitRetailPart(const uint8_t* data, size_t size, std::vector<RetailPartProperty>& out, std::string& error) {
  out.clear();
  RetailReader reader(data, size);
  uint32_t fourcc;
  if (!reader.Word(fourcc) || fourcc != F("GPSM")) {
    error = "not a GPSM";
    return false;
  }
  const auto& retail = RetailProperties();
  for (;;) {
    const size_t start = reader.At();
    if (!reader.Word(fourcc)) {
      error = "no _END";
      return false;
    }
    if (fourcc == F("_END")) {
      return true;
    }
    bool ok;
    if (fourcc == F("KSSM")) {
      ok = reader.SpawnTable();
    } else if (const auto found = retail.find(fourcc); found == retail.end()) {
      error = "property " + EffectFourCCString(fourcc) + " retail does not read";
      return false;
    } else {
      switch (found->second.type) {
      case Type::Bool:
        ok = reader.Bool();
        break;
      case Type::Texture:
        ok = reader.Texture();
        break;
      case Type::Asset:
        ok = reader.Asset();
        break;
      default:
        ok = reader.Element(found->second.type);
        break;
      }
    }
    if (!ok) {
      error = EffectFourCCString(fourcc) + " does not read" + (reader.Error().empty() ? "" : ": " + reader.Error());
      return false;
    }
    out.push_back({fourcc, std::vector<uint8_t>(data + start + 4, data + reader.At())});
  }
}

std::optional<uint32_t> EffectRetailId(const EffectGuid& id) {
  // 10000000-0000-f000-f000-0000XXXXXXXX in pak order is stored here with its
  // first three groups byte-swapped.
  static const uint8_t kPrefix[12] = {0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0xf0, 0xf0, 0x00, 0x00, 0x00};
  if (std::memcmp(id.data(), kPrefix, sizeof(kPrefix)) != 0) {
    return std::nullopt;
  }
  return uint32_t(id[12]) << 24 | uint32_t(id[13]) << 16 | uint32_t(id[14]) << 8 | uint32_t(id[15]);
}

std::vector<ConvertedPart> ConvertEffect(const EffectNode& effect, const uint8_t* data, const EffectConvertIO& io) {
  std::vector<ConvertedPart> out;
  Writer(data, io).Collect(effect, true, out);
  return out;
}

}  // namespace PortRemastered
