#include "port_remastered_effect_import.h"

#include "port_remastered_effect_convert.h"
#include "port_remastered_image.h"

#include <algorithm>
#include <atomic>
#include <cctype>
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
constexpr uint32_t kCmdl = EffectFourCC("CMDL");

// Effect textures are drawn small; larger ones are scaled down to this side.
constexpr int kMaxTextureSide = 256;
// A flipbook atlas keeps its frames' size up to this edge.
constexpr int kMaxAtlasSide = 2048;

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

const EffectNode* FindNode(const EffectNode& node, const EffectGuid& id) {
  for (const EffectNode& child : node.children) {
    if (child.id == id) {
      return &child;
    }
    if (const EffectNode* found = FindNode(child, id)) {
      return found;
    }
  }
  return nullptr;
}

bool HasProperty(const EffectNode& node, uint32_t fourcc) {
  return std::any_of(node.properties.begin(), node.properties.end(),
                     [&](const EffectProperty& property) { return property.fourcc == fourcc; });
}

// A generator that drew a texture or a model whose converted PART draws
// neither: the conversion lost its look.
bool LostLook(const EffectNode& node, const std::vector<RetailPartProperty>& part) {
  const bool had = HasProperty(node, EffectFourCC("TEXR")) || HasProperty(node, EffectFourCC("MTIN")) ||
                    HasProperty(node, EffectFourCC("PMDL"));
  const bool has = std::any_of(part.begin(), part.end(), [](const RetailPartProperty& property) {
    return property.fourcc == EffectFourCC("TEXR") || property.fourcc == EffectFourCC("PMDL");
  });
  return had && !has;
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

  // An array texture's layers packed into one atlas TXTR, row-major from the
  // top: a power-of-two column count, and frames halved only while an edge is
  // over kMaxAtlasSide. Id 0 when it cannot be.
  FlipbookAtlas Flipbook(const EffectGuid& id) {
    const auto known = m_flipbooks.find(id);
    if (known != m_flipbooks.end()) {
      return known->second;
    }
    FlipbookAtlas out;
    int width = 0, height = 0, layers = 0;
    std::vector<uint8_t> rgba;
    std::string error;
    if (!m_io.layers || !m_io.layers(id, width, height, layers, rgba, error)) {
      Log("effect flipbook " + EffectGuidString(Swap(id)) + ": " + (m_io.layers ? error : "no layer reader"));
    } else if (layers < 1 || width < 1 || height < 1 || rgba.size() != size_t(width) * height * layers * 4) {
      Log("effect flipbook " + EffectGuidString(Swap(id)) + ": no layers");
    } else {
      int cols = 1;
      while (cols * cols < layers) {
        cols *= 2;
      }
      const int rows = (layers + cols - 1) / cols;
      int frameW = width;
      int frameH = height;
      while (cols * frameW > kMaxAtlasSide || rows * frameH > kMaxAtlasSide) {
        frameW = RoundUp4(frameW / 2);
        frameH = RoundUp4(frameH / 2);
      }
      Image atlas;
      atlas.width = cols * frameW;
      atlas.height = rows * frameH;
      atlas.rgba.assign(size_t(atlas.width) * atlas.height * 4, 0);
      for (int k = 0; k < layers; ++k) {
        Image frame;
        frame.width = width;
        frame.height = height;
        const uint8_t* src = rgba.data() + size_t(k) * width * height * 4;
        frame.rgba.assign(src, src + size_t(width) * height * 4);
        if (frameW != width || frameH != height) {
          frame = Resize(frame, frameW, frameH);
        }
        const int x0 = (k % cols) * frameW;
        const int y0 = (k / cols) * frameH;
        for (int y = 0; y < frameH; ++y) {
          std::memcpy(atlas.rgba.data() + (size_t(y0 + y) * atlas.width + x0) * 4,
                      frame.rgba.data() + size_t(y) * frameW * 4, size_t(frameW) * 4);
        }
      }
      const uint32_t fresh = m_io.freshId(Hash(id, kTxtr ^ 0xF11Bu));
      if (m_io.write(Hex(fresh) + ".TXTR", EncodeTxtrRgba8(atlas))) {
        ++m_result.textures;
        ++m_result.flipbooks;
        out = FlipbookAtlas{fresh, cols, rows, layers};
      }
    }
    m_flipbooks.emplace(id, out);
    return out;
  }

  // The id a Remastered-only model (pak order) is written under, converting
  // it the first time; 0 when it cannot be.
  uint32_t Model(const EffectGuid& id) {
    const auto known = m_models.find(id);
    if (known != m_models.end()) {
      return known->second;
    }
    uint32_t out = 0;
    std::string error;
    if (m_io.model) {
      out = m_io.freshId(Hash(id, kCmdl));
      if (m_io.model(id, out, error)) {
        ++m_result.models;
      } else {
        Log("effect model " + EffectGuidString(Swap(id)) + ": " + error);
        out = 0;
      }
    }
    m_models.emplace(id, out);
    return out;
  }

  // The retail id for an id as an effect stores it: the disc's own when it
  // was carried over from retail, else a converted texture's or model's.
  uint32_t Stored(const EffectGuid& stored, uint32_t type) {
    const std::optional<uint32_t> retail = EffectRetailId(stored);
    if (retail && m_io.retailId(*retail)) {
      return *retail;
    }
    const EffectGuid id = Swap(stored);
    if (type == kTxtr && m_io.typeOf(id) == kTxtr) {
      return Texture(id);
    }
    if (type == kCmdl && m_io.typeOf(id) == kCmdl) {
      return Model(id);
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

  // Each embedded child's fresh id, and the retail type it converts to (0 for
  // a form that is not converted, so nothing resolves to it).
  struct Child {
    uint32_t id;
    uint32_t type;
  };

  void Children(const EffectNode& node, uint32_t root, std::map<EffectGuid, Child>& out) {
    for (const EffectNode& child : node.children) {
      out.emplace(child.id, Child{m_io.freshId(Hash(child.id, root)), EffectRetailType(child.form)});
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
    std::map<EffectGuid, Child> children;
    Children(effect, *retail, children);
    EffectConvertIO io;
    io.assetId = [&](const EffectGuid& stored, uint32_t type) -> uint32_t {
      const auto child = children.find(stored);
      if (child != children.end() && child->second.type == type) {
        return child->second.id;
      }
      return Stored(stored, type);
    };
    io.materialTexture = [&](const EffectGuid& material) { return Material(material); };
    io.flipbook = [&](const EffectGuid& stored) { return Flipbook(Swap(stored)); };
    const std::vector<ConvertedPart> parts = ConvertEffect(effect, data.data(), io);
    std::vector<RetailPartProperty> check;
    if (parts.empty() || !SplitRetailPart(parts[0].part.data(), parts[0].part.size(), check, error)) {
      ++m_result.failed;
      Log(name + ": the converted effect does not read as a PART");
      return;
    }
    // A part that lost its texture or model would replace the disc's textured
    // effect with an invisible one: keep the disc's, before anything is written.
    for (const ConvertedPart& part : parts) {
      const EffectNode* node = part.root ? &effect : FindNode(effect, part.id);
      if (node != nullptr && SplitRetailPart(part.part.data(), part.part.size(), check, error) &&
          LostLook(*node, check)) {
        ++m_result.failed;
        Log(name + ": " + (part.root ? std::string("the root") : "child " + EffectGuidString(part.id)) +
            " has no texture, the disc's is kept");
        return;
      }
    }
    // Children first, so the root never names a child that was not written.
    for (size_t i = 1; i < parts.size(); ++i) {
      const auto child = children.find(parts[i].id);
      if (child == children.end() ||
          !SplitRetailEffect(parts[i].type, parts[i].part.data(), parts[i].part.size(), check, error) ||
          !m_io.write(Hex(child->second.id) + "." + EffectFourCCString(parts[i].type), parts[i].part)) {
        // The root would name a missing child: leave the disc's PART in place.
        ++m_result.failed;
        Log(name + ": child " + EffectGuidString(parts[i].id) + " not written");
        return;
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
  std::map<EffectGuid, FlipbookAtlas> m_flipbooks;
  std::map<EffectGuid, uint32_t> m_models;
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
  std::string value(env);
  for (char& c : value) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  if (value == "1" || value == "true" || value == "on" || value == "yes") {
    return true;
  }
  if (value == "0" || value == "false" || value == "off" || value == "no") {
    return false;
  }
  return sEffects.load();
}

void SetImportEffects(bool on) { sEffects = on; }

EffectImportResult ImportEffects(const EffectImportIO& io) { return Importer(io).Run(); }

}  // namespace PortRemastered
