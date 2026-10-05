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
#include <optional>

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

// Effects Remastered gave a fresh id, matched to the retail PART by the name
// both paks give it (the player's and global paks'). Only names that one
// retail PART has; the 8 effects that kept a retail id all match their names.
struct NamedEffect {
  const char* id;  // as IdToString prints it (EffectGuidString of the stored id)
  uint32_t retail;
};
constexpr NamedEffect kNamedEffects[] = {
    {"fb4d5181-cd7e-4c5e-8b11-afe35d30e231", 0x1EF973EA},  // BombExplo
    {"bbbb849a-d896-489f-83da-ef76c010888d", 0xC0E95E90},  // BombSet
    {"ca6fc8a1-30a2-441f-bf7a-12b6acd4e938", 0x39F0F5C6},  // BoostBallGlow
    {"ceaee14a-a690-44cc-a90b-0bd34935ae6c", 0x523048E0},  // BusterLight
    {"9c215d05-e582-4897-b958-85eb83c9f4f3", 0x9B564161},  // BusterMuzzle
    {"c1f18af2-c6e5-486b-86e0-0609316733f2", 0x04E29C5B},  // BusterSparks
    {"9499e42a-54bc-4402-8557-aa300f3db8de", 0xD8DB86CA},  // DirtWake
    {"97280bbe-5eef-42b6-b5bd-096b8c089c76", 0xF0C02F49},  // Effect_Ash
    {"70108314-fb42-4090-9ab4-b1c8c6570ee2", 0xA6B67F45},  // Effect_FirePop
    {"4dd6affe-ffd8-40ed-a056-1a77b9dcb9d6", 0xE6FC0230},  // Effect_IceBreak
    {"7bb7ac6f-16a2-45f0-a103-f634fe027768", 0x017DFFD5},  // Effect_IcePop
    {"103f6797-5ea0-4c2b-9c9b-eafdce3e48f4", 0xABE56164},  // Effect_MorphBallIceBreak
    {"9c56f6df-a9ae-403b-8da3-6a6ec6405971", 0x2D65511C},  // Effect_OnFire
    {"7cbb6382-2788-4225-b3a2-3430ba5558b7", 0xF42646D4},  // FlameMuzzle
    {"416c81e0-e3ab-4f72-b3f5-3aab7b851ba6", 0xD5A18910},  // grappleClaw
    {"9905ed28-e8ac-4231-9b05-ca49e338eb2d", 0xCFC222B0},  // grappleHit
    {"3727f6c9-0f4f-4369-ba8f-b22e71bfdd1d", 0x2CC7F7F5},  // grappleMuzzle
    {"14e59c07-ea96-4f88-ae84-a8762b7beab8", 0x87C0BDB2},  // grappleSegment
    {"f8502f32-276d-4db5-998c-f57c7a2cb00b", 0x7072A62D},  // holoTransition
    {"7114b32f-78b8-45fd-adc0-ab905c7671eb", 0x1BBFC5A6},  // Ice2nd_1
    {"c8aa99d2-f7a9-4bc0-bff1-1fb1b42c1990", 0xF97661F1},  // Ice2nd_2
    {"a6c1a9fb-e480-4b50-8e16-49ab849c86b7", 0x21F4D9AB},  // IceAuxMuzzle
    {"0649036e-0b4f-450f-b33d-01e9045a29c9", 0x6ECDC394},  // IceCharge
    {"91b571dc-1b15-4053-8e1a-8744abd2f2e1", 0x9ADE39C3},  // IceMuzzle
    {"b589f70b-1853-490e-bba8-a4f120563ddd", 0xC82F2028},  // IceSmoke
    {"42f41dcd-0da3-40e2-b4ec-d1c6096eda5b", 0xDE1A1140},  // IceSpread1
    {"b28d85f4-67a3-4664-981f-6fa9bf044f93", 0x045DDB2F},  // IceXfer
    {"ec64d113-7337-4ce0-bda7-0f885bfd0f50", 0x43A81EEC},  // MorphBallTransitionFlash
    {"56844d59-1a0f-4313-a6a8-533292dfd75c", 0x8B8CD2F6},  // MudWake
    {"2593f90a-21cf-4249-a3f2-64ef7fabe99b", 0xF639D24E},  // NFTMainFire
    {"369cc052-6d7b-44d0-8a33-e18f6fc67f85", 0xD67EE2D9},  // NFTMainSmoke
    {"d0766ccc-22d9-444f-8529-a8a6ada6cbdd", 0x1F4FD93A},  // NFTSecondaryFire
    {"bfe5b4c5-421f-41d1-9554-6d3e00f72066", 0xAD51661F},  // NFTSecondarySmoke
    {"93004165-738e-404b-ac29-1f1a059719ae", 0x7DA3DEE5},  // NFTSecondarySparks
    {"58175f6a-d712-4852-b350-13321240dc65", 0x7754967A},  // Phazon2nd_1
    {"c8536e8f-2e70-41b2-82b9-1c8410ac9c6f", 0x1C56F6B1},  // PhazonMuzzle
    {"4474842f-b051-45be-b72b-b7d03128d755", 0x18CB74EF},  // PhazonWake
    {"3210e9de-2f83-48f3-9168-d8a3587be355", 0x6C35D8FE},  // PhazonWakeOrange
    {"541f775f-c171-4b2e-a182-6ada992a21dc", 0xC0A88A87},  // Plasma2nd_1
    {"f494900e-6c34-41bb-a372-9f6f1fe6468a", 0xB0F9DBE6},  // PlasmaAuxMuzzle
    {"b2f2c408-d488-43ab-8562-e85bca2b654f", 0xD3053354},  // PlasmaCharge
    {"d682fa42-228c-445a-856f-18581ed7866d", 0x8D7BBFB2},  // PlasmaMuzzle
    {"6a83fb12-e81e-499a-9014-15a539f53ec9", 0x5721EE48},  // PlasmaXfer
    {"51d96194-91b7-416e-8c37-13031270163e", 0x3183F0A0},  // Power2nd_1
    {"4bc8b7ca-ce44-4e88-9e8f-a764138a9601", 0x7E8ADCBA},  // PowerBombExplo
    {"835d9ae7-2f75-48ab-ac9f-b9827b8740ad", 0x4CE91ECB},  // PowerCharge
    {"597c6fe6-6fe7-439b-956b-87728a5a067f", 0x0F21403B},  // PowerMuzzle
    {"411dae2f-d4be-4324-9433-7822f188928c", 0x3DD09610},  // PowerXfer
    {"d3abe44e-d8a8-473b-a60e-c2289eb6a74d", 0x8185DEB3},  // RainWake
    {"c82dd5d6-f6d9-4908-9794-f3fd62627c32", 0xF421ED31},  // SandWake
    {"5c2798a0-12e5-4153-b187-0594d7f8fa5d", 0x60817832},  // ShotSmoke
    {"81cb1d8a-4cb4-4469-8127-d6c761752e7c", 0xC9D4BA43},  // SnowWake
    {"026c1c9a-df22-4230-937b-29e785637252", 0x22B005A1},  // SpiderBallMagnetEffect
    {"7e1ca0b3-d242-4724-b1dc-ecb96e43bc73", 0xE1341D07},  // WallSpark
    {"1b44823d-c587-4b51-9c1d-65144c5fcac5", 0x629C848F},  // Wave2nd_3
    {"50b67be4-15f3-4230-9147-deb8d6a47248", 0x7E520CBC},  // WaveAuxMuzzle
    {"a0d802cb-6516-4a42-9c1d-7614e6122ca9", 0x2BC80C63},  // WaveCharge
    {"59dba49a-777f-42eb-ab47-c5dd91a28d3e", 0x0237C838},  // WaveXfer
};

// The retail PART an effect replaces: the id it carried over, else its name's.
std::optional<uint32_t> RetailEffect(const EffectGuid& id) {
  if (const std::optional<uint32_t> retail = EffectRetailId(Swap(id))) {
    return retail;
  }
  const std::string text = EffectGuidString(Swap(id));
  for (const NamedEffect& named : kNamedEffects) {
    if (text == named.id) {
      return named.retail;
    }
  }
  return std::nullopt;
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
    const std::optional<uint32_t> retail = RetailEffect(id);
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
