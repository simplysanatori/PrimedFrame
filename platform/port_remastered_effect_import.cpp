#include "port_remastered_effect_import.h"

#include "port_remastered_effect_convert.h"
#include "port_remastered_image.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>

namespace PortRemastered {
namespace {

constexpr uint32_t kGenp = EffectFourCC("GENP");
constexpr uint32_t kMati = EffectFourCC("MATI");
constexpr uint32_t kPart = EffectFourCC("PART");
constexpr uint32_t kTxtr = EffectFourCC("TXTR");

// Effect textures are drawn small; larger ones are scaled down to this side.
constexpr int kMaxTextureSide = 256;

// Between a pak's byte order and the order an effect stores an id in: the
// first three groups byte-swapped (the same swap both ways).
EffectGuid Swap(const EffectGuid& id) {
  EffectGuid out = id;
  std::swap(out[0], out[3]);
  std::swap(out[1], out[2]);
  std::swap(out[4], out[5]);
  std::swap(out[6], out[7]);
  return out;
}

uint32_t Hash(const EffectGuid& id, uint32_t salt) {
  uint32_t hash = 0x811C9DC5u ^ salt;  // FNV-1a
  for (uint8_t byte : id) {
    hash = (hash ^ byte) * 0x01000193u;
  }
  return hash;
}

std::string Hex(uint32_t id) {
  char text[16];
  std::snprintf(text, sizeof(text), "%08X", id);
  return text;
}

int RoundUp4(int side) { return std::max(8, (side + 3) / 4 * 4); }

bool IsLight(uint32_t fourcc) {
  for (uint32_t light : {EffectFourCC("LTYP"), EffectFourCC("LFOT"), EffectFourCC("LCLR"), EffectFourCC("LINT"),
                         EffectFourCC("LOFF"), EffectFourCC("LDIR"), EffectFourCC("LFOR"), EffectFourCC("LSLA")}) {
    if (fourcc == light) {
      return true;
    }
  }
  return false;
}

bool HasLight(const std::vector<RetailPartProperty>& properties) {
  return std::any_of(properties.begin(), properties.end(),
                     [](const RetailPartProperty& property) { return property.fourcc == EffectFourCC("LTYP"); });
}

void PutFourCC(std::vector<uint8_t>& out, uint32_t fourcc) {
  for (int shift = 24; shift >= 0; shift -= 8) {
    out.push_back(uint8_t(fourcc >> shift));
  }
}

// `converted` with its light replaced by the disc PART's. Remastered leaves
// out LOFF/LDIR/LFOR/LSLA (it has LIRD/LORD instead, which do not map onto
// them) and differs from retail in places, so the disc's light is the one
// retail's lighting was made for.
std::vector<uint8_t> WithDiscLight(const std::vector<RetailPartProperty>& converted,
                                   const std::vector<RetailPartProperty>& disc) {
  std::vector<uint8_t> out;
  PutFourCC(out, EffectFourCC("GPSM"));
  auto put = [&](const RetailPartProperty& property) {
    PutFourCC(out, property.fourcc);
    out.insert(out.end(), property.value.begin(), property.value.end());
  };
  for (const RetailPartProperty& property : converted) {
    if (!IsLight(property.fourcc)) {
      put(property);
    }
  }
  for (const RetailPartProperty& property : disc) {
    if (IsLight(property.fourcc)) {
      put(property);
    }
  }
  PutFourCC(out, EffectFourCC("_END"));
  return out;
}

class Importer {
public:
  explicit Importer(const EffectImportIO& io) : m_io(io) {}

  void Log(const std::string& line) const {
    if (m_io.log) {
      m_io.log(line);
    }
  }

  // The id a Remastered texture (pak order) is written under, converting it
  // the first time; 0 when it cannot be.
  uint32_t Texture(const EffectGuid& id) {
    const auto known = m_textures.find(id);
    if (known != m_textures.end()) {
      return known->second;
    }
    uint32_t out = 0;
    Image image;
    std::string error;
    if (!m_io.texture || !m_io.texture(id, image.width, image.height, image.rgba, error)) {
      Log("effect texture " + EffectGuidString(Swap(id)) + ": " + error);
    } else {
      int width = image.width;
      int height = image.height;
      while (width > kMaxTextureSide || height > kMaxTextureSide) {
        width = std::max(1, width / 2);
        height = std::max(1, height / 2);
      }
      width = RoundUp4(width);
      height = RoundUp4(height);
      if (width != image.width || height != image.height) {
        image = Resize(image, width, height);
      }
      out = m_io.freshId(Hash(id, kTxtr));
      if (m_io.write(Hex(out) + ".TXTR", EncodeTxtrRgba8(image))) {
        ++m_result.textures;
      } else {
        out = 0;
      }
    }
    m_textures.emplace(id, out);
    return out;
  }

  // The retail id for an id as an effect stores it: the disc's own when it
  // was carried over from retail, else a converted texture's.
  uint32_t Stored(const EffectGuid& stored, uint32_t type) {
    const std::optional<uint32_t> retail = EffectRetailId(stored);
    if (retail && m_io.retailId(*retail)) {
      return *retail;
    }
    const EffectGuid id = Swap(stored);
    if (type == kTxtr && m_io.typeOf(id) == kTxtr) {
      return Texture(id);
    }
    return 0;
  }

  // The first texture a material instance names that converts.
  uint32_t Material(const EffectGuid& stored) {
    std::vector<uint8_t> data;
    std::string error;
    if (!m_io.read(kMati, Swap(stored), data, error)) {
      return 0;
    }
    for (size_t at = 0; at + 16 <= data.size(); ++at) {
      EffectGuid id;
      std::memcpy(id.data(), data.data() + at, 16);
      if (m_io.typeOf(Swap(id)) == kTxtr) {
        if (const uint32_t texture = Stored(id, kTxtr)) {
          return texture;
        }
      }
    }
    return 0;
  }

  void Children(const EffectNode& node, uint32_t root, std::map<EffectGuid, uint32_t>& out) {
    for (const EffectNode& child : node.children) {
      out.emplace(child.id, m_io.freshId(Hash(child.id, root)));
      Children(child, root, out);
    }
  }

  void Effect(const EffectGuid& id) {
    const std::optional<uint32_t> retail = EffectRetailId(Swap(id));
    if (!retail || !m_io.retailId(*retail)) {
      return;
    }
    ++m_result.candidates;
    const std::string name = Hex(*retail) + ".PART";
    std::vector<uint8_t> data;
    std::string error;
    EffectNode effect;
    if (!m_io.read(kGenp, id, data, error) || !ParseEffect(data.data(), data.size(), effect, error)) {
      ++m_result.failed;
      Log(name + ": " + error);
      return;
    }
    std::map<EffectGuid, uint32_t> children;
    Children(effect, *retail, children);
    EffectConvertIO io;
    io.assetId = [&](const EffectGuid& stored, uint32_t type) -> uint32_t {
      if (type == kPart) {
        const auto child = children.find(stored);
        if (child != children.end()) {
          return child->second;
        }
      }
      return Stored(stored, type);
    };
    io.materialTexture = [&](const EffectGuid& material) { return Material(material); };
    const std::vector<ConvertedPart> parts = ConvertEffect(effect, data.data(), io);
    std::vector<RetailPartProperty> check;
    if (parts.empty() || !SplitRetailPart(parts[0].part.data(), parts[0].part.size(), check, error)) {
      ++m_result.failed;
      Log(name + ": the converted effect does not read as a PART");
      return;
    }
    // Children first, so the root never names a child that was not written.
    for (size_t i = 1; i < parts.size(); ++i) {
      const auto child = children.find(parts[i].id);
      if (child == children.end() ||
          !SplitRetailPart(parts[i].part.data(), parts[i].part.size(), check, error) ||
          !m_io.write(Hex(child->second) + ".PART", parts[i].part)) {
        Log(name + ": child " + EffectGuidString(parts[i].id) + " not written");
        continue;
      }
      ++m_result.parts;
      m_result.dropped += parts[i].droppedRetail;
    }
    std::vector<uint8_t> root = parts[0].part;
    std::vector<RetailPartProperty> converted;
    std::vector<RetailPartProperty> disc;
    std::vector<uint8_t> discData;
    if (m_io.retail && SplitRetailPart(root.data(), root.size(), converted, error) && HasLight(converted) &&
        m_io.retail(kPart, *retail, discData) &&
        SplitRetailPart(discData.data(), discData.size(), disc, error) && HasLight(disc)) {
      root = WithDiscLight(converted, disc);
      Log(name + ": light from the disc");
    }
    if (!m_io.write(name, root)) {
      ++m_result.failed;
      Log(name + ": could not write it");
      return;
    }
    ++m_result.parts;
    ++m_result.written;
    m_result.dropped += parts[0].droppedRetail;
  }

  EffectImportResult Run() {
    for (const EffectGuid& id : m_io.effects) {
      Effect(id);
    }
    return m_result;
  }

private:
  const EffectImportIO& m_io;
  std::map<EffectGuid, uint32_t> m_textures;  // by Remastered id, 0 for one that failed
  EffectImportResult m_result;
};

}  // namespace

namespace {
std::atomic<bool> sEffects{false};
}  // namespace

bool WantsRemasteredEffects() {
  const char* env = std::getenv("MP_REMASTERED_EFFECTS");
  if (env == nullptr || env[0] == '\0') {
    return sEffects.load();
  }
  return std::strcmp(env, "1") == 0;
}

void SetImportEffects(bool on) { sEffects = on; }

EffectImportResult ImportEffects(const EffectImportIO& io) { return Importer(io).Run(); }

}  // namespace PortRemastered
