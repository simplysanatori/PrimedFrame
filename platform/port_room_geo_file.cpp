// The .roomgeo file. See port_room_geo.h.
#include "port_room_geo.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace PortRoomGeo {
namespace {

constexpr uint32_t kMagic = 0x4752504D; // 'MPRG'
constexpr uint32_t kVersion = 5;
constexpr size_t kHeaderBytes = 12;
constexpr size_t kInstanceBytes = 4 + 12 * 4; // version 1; version 2 adds 4 + links
constexpr size_t kPlatformBytes = 4 + 3 * 4;   // version 3's, after version 2's 4
constexpr size_t kLinkBytes = 8;
constexpr uint32_t kScriptMagic = 0x50524353; // 'SCRP'
constexpr size_t kNodeBytes = 8 + 15 * 4;
constexpr size_t kEdgeBytes = 12;
constexpr uint32_t kGlowMagic = 0x574F4C47; // 'GLOW'
constexpr size_t kGlowBytes = 4 + 3 * 4;
constexpr uint32_t kAnimMagic = 0x4D494E41; // 'ANIM'
constexpr size_t kAnimHeadBytes = 3 * 4;
constexpr size_t kAnimKeyBytes = 7 * 4;
constexpr uint32_t kLodMagic = 0x444F4C52; // 'RLOD'
constexpr uint32_t kLodVersion = 1;

int HexDigit(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

uint32_t ReadU32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

void PutU32(std::vector<uint8_t>& out, uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(uint8_t(value >> (i * 8)));
  }
}

void PutF32(std::vector<uint8_t>& out, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, 4);
  PutU32(out, bits);
}

bool ReadF32(const uint8_t* p, float& out) {
  const uint32_t bits = ReadU32(p);
  std::memcpy(&out, &bits, 4);
  return std::isfinite(out);
}

// The script section at `at`, which is moved past it.
bool ParseScript(const std::vector<uint8_t>& data, size_t& at, std::vector<Instance>& instances, Script& script,
                 std::string& error) {
  if (data.size() - at < 12) {
    error = "truncated script";
    return false;
  }
  const uint32_t nodes = ReadU32(data.data() + at + 4), edges = ReadU32(data.data() + at + 8);
  at += 12;
  const size_t left = data.size() - at;
  if (nodes > left / kNodeBytes || edges > (left - nodes * kNodeBytes) / kEdgeBytes ||
      (left - nodes * kNodeBytes - edges * kEdgeBytes) / 4 < instances.size()) {
    error = "truncated script";
    return false;
  }
  script.nodes.resize(nodes);
  for (ScriptNode& node : script.nodes) {
    const uint8_t* const p = data.data() + at;
    node.kind = p[0];
    node.active = p[1] != 0;
    node.max = ReadU32(p + 4);
    float* const fields[] = {node.centre, node.half, node.axes};
    const int counts[] = {3, 3, 9};
    size_t o = 8;
    for (int f = 0; f < 3; ++f) {
      for (int j = 0; j < counts[f]; ++j, o += 4) {
        if (!ReadF32(p + o, fields[f][j])) {
          error = "bad script volume";
          return false;
        }
      }
    }
    if (node.kind < kCameraVolume || node.kind > kRelay) {
      error = "unknown script node";
      return false;
    }
    at += kNodeBytes;
  }
  script.edges.resize(edges);
  for (ScriptEdge& edge : script.edges) {
    const uint8_t* const p = data.data() + at;
    edge.retail = p[0] != 0;
    edge.event = p[1];
    edge.action = p[2];
    edge.from = ReadU32(p + 4);
    edge.to = ReadU32(p + 8);
    const bool toNode = edge.action != kGroupShow && edge.action != kGroupHide && edge.action != kGroupToggle;
    if ((!edge.retail && edge.from >= nodes) || edge.action < kIncrement || edge.action > kNodeDeactivate ||
        (toNode && edge.to >= nodes)) {
      error = "bad script edge";
      return false;
    }
    at += kEdgeBytes;
  }
  for (Instance& instance : instances) {
    instance.group = ReadU32(data.data() + at);
    at += 4;
  }
  return true;
}

// The glow section at `at`, which is moved past it.
bool ParseGlow(const std::vector<uint8_t>& data, size_t& at, std::vector<Instance>& instances, std::string& error) {
  if (data.size() - at < 8 || ReadU32(data.data() + at + 4) > (data.size() - at - 8) / kGlowBytes) {
    error = "truncated glow";
    return false;
  }
  const uint32_t count = ReadU32(data.data() + at + 4);
  at += 8;
  for (uint32_t i = 0; i < count; ++i, at += kGlowBytes) {
    const uint32_t index = ReadU32(data.data() + at);
    if (index >= instances.size() || instances[index].glows) {
      error = "bad glow instance";
      return false;
    }
    Instance& instance = instances[index];
    for (int j = 0; j < 3; ++j) {
      if (!ReadF32(data.data() + at + 4 + 4 * j, instance.glow[j]) || instance.glow[j] < 0.f) {
        error = "bad glow";
        return false;
      }
    }
    instance.glows = true;
  }
  return true;
}

// The animation section at `at`, which is moved past it.
bool ParseAnim(const std::vector<uint8_t>& data, size_t& at, std::vector<Instance>& instances, std::string& error) {
  if (data.size() - at < 8 || ReadU32(data.data() + at + 4) > (data.size() - at - 8) / kAnimHeadBytes) {
    error = "truncated animation";
    return false;
  }
  const uint32_t count = ReadU32(data.data() + at + 4);
  at += 8;
  for (uint32_t i = 0; i < count; ++i) {
    if (data.size() - at < kAnimHeadBytes) {
      error = "truncated animation";
      return false;
    }
    const uint32_t index = ReadU32(data.data() + at);
    const uint32_t frames = ReadU32(data.data() + at + 8);
    float fps;
    if (index >= instances.size() || !instances[index].animKeys.empty()) {
      error = "bad animation instance";
      return false;
    }
    if (!ReadF32(data.data() + at + 4, fps) || fps <= 0.f || frames < 2) {
      error = "bad animation";
      return false;
    }
    at += kAnimHeadBytes;
    if (frames > (data.size() - at) / kAnimKeyBytes) {
      error = "truncated animation";
      return false;
    }
    Instance& instance = instances[index];
    instance.animFps = fps;
    instance.animKeys.resize(size_t(frames) * 7);
    for (size_t f = 0; f < frames; ++f, at += kAnimKeyBytes) {
      float* const key = instance.animKeys.data() + f * 7;
      for (int j = 0; j < 7; ++j) {
        if (!ReadF32(data.data() + at + 4 * j, key[j])) {
          error = "bad animation";
          return false;
        }
      }
      const float length =
          std::sqrt(key[0] * key[0] + key[1] * key[1] + key[2] * key[2] + key[3] * key[3]);
      if (!(std::fabs(length - 1.f) <= 1e-3f)) {
        error = "bad animation";
        return false;
      }
    }
  }
  return true;
}

void WriteScript(std::vector<uint8_t>& out, const std::vector<Instance>& instances, const Script* script) {
  static const Script kNone;
  const Script& s = script != nullptr ? *script : kNone;
  PutU32(out, kScriptMagic);
  PutU32(out, uint32_t(s.nodes.size()));
  PutU32(out, uint32_t(s.edges.size()));
  for (const ScriptNode& node : s.nodes) {
    out.push_back(node.kind);
    out.push_back(node.active ? 1 : 0);
    out.push_back(0);
    out.push_back(0);
    PutU32(out, node.max);
    for (float v : node.centre) {
      PutF32(out, v);
    }
    for (float v : node.half) {
      PutF32(out, v);
    }
    for (float v : node.axes) {
      PutF32(out, v);
    }
  }
  for (const ScriptEdge& edge : s.edges) {
    out.push_back(edge.retail ? 1 : 0);
    out.push_back(edge.event);
    out.push_back(edge.action);
    out.push_back(0);
    PutU32(out, edge.from);
    PutU32(out, edge.to);
  }
  for (const Instance& instance : instances) {
    PutU32(out, instance.group);
  }
}

} // namespace

bool ParseFileName(const std::string& fileName, uint32_t& id) {
  static const char kSuffix[] = ".roomgeo";
  if (fileName.size() != 8 + sizeof(kSuffix) - 1) {
    return false;
  }
  for (size_t i = 0; i + 1 < sizeof(kSuffix); ++i) {
    const char c = fileName[8 + i];
    if ((c >= 'A' && c <= 'Z' ? char(c | 0x20) : c) != kSuffix[i]) {
      return false;
    }
  }
  id = 0;
  for (size_t i = 0; i < 8; ++i) {
    const int digit = HexDigit(fileName[i]);
    if (digit < 0) {
      return false;
    }
    id = (id << 4) | uint32_t(digit);
  }
  return true;
}

bool Parse(const std::vector<uint8_t>& data, std::vector<Instance>& out, std::string& error, Script* script) {
  out.clear();
  Script scratch;
  Script& parsed = script != nullptr ? *script : scratch;
  parsed = {};
  if (data.size() < kHeaderBytes || ReadU32(data.data()) != kMagic) {
    error = "not a room geometry file";
    return false;
  }
  const uint32_t version = ReadU32(data.data() + 4);
  if (version < 1 || version > kVersion) {
    error = "unknown version";
    return false;
  }
  const uint32_t count = ReadU32(data.data() + 8);
  const size_t instanceBytes =
      version == 1 ? kInstanceBytes : kInstanceBytes + 4 + (version >= 3 ? kPlatformBytes : 0);
  if (count > (data.size() - kHeaderBytes) / instanceBytes) {
    error = "truncated";
    return false;
  }
  out.clear();
  out.resize(count);
  size_t at = kHeaderBytes;
  for (uint32_t i = 0; i < count; ++i) {
    if (data.size() - at < instanceBytes) {
      error = "truncated";
      out.clear();
      return false;
    }
    const uint8_t* const p = data.data() + at;
    Instance& instance = out[i];
    instance.model = ReadU32(p);
    for (int j = 0; j < 12; ++j) {
      const uint32_t bits = ReadU32(p + 4 + j * 4);
      std::memcpy(&instance.transform[j], &bits, 4);
      if (!std::isfinite(instance.transform[j])) {
        error = "bad transform";
        out.clear();
        return false;
      }
    }
    at += instanceBytes;
    if (version == 1) {
      continue;
    }
    instance.layer = p[kInstanceBytes];
    instance.active = p[kInstanceBytes + 1] != 0;
    const size_t links = size_t(p[kInstanceBytes + 2]) | size_t(p[kInstanceBytes + 3]) << 8;
    if (version >= 3) {
      const uint8_t* const q = p + kInstanceBytes + 4;
      instance.platform = ReadU32(q);
      for (int j = 0; j < 3; ++j) {
        const uint32_t bits = ReadU32(q + 4 + j * 4);
        std::memcpy(&instance.platformStart[j], &bits, 4);
        if (!std::isfinite(instance.platformStart[j])) {
          error = "bad platform position";
          out.clear();
          return false;
        }
      }
    }
    if ((data.size() - at) / kLinkBytes < links) {
      error = "truncated";
      out.clear();
      return false;
    }
    instance.links.resize(links);
    for (Link& link : instance.links) {
      link.sender = ReadU32(data.data() + at);
      link.state = data[at + 4];
      link.action = data[at + 5];
      at += kLinkBytes;
    }
  }
  // Version 3 may end with the script section, version 4 then with the glow section and
  // version 5 then with the animation section.
  bool ok = true;
  if (version >= 3 && data.size() - at >= 4 && ReadU32(data.data() + at) == kScriptMagic) {
    ok = ParseScript(data, at, out, parsed, error);
  }
  if (ok && version >= 4 && data.size() - at >= 4 && ReadU32(data.data() + at) == kGlowMagic) {
    ok = ParseGlow(data, at, out, error);
  }
  if (ok && version >= 5 && data.size() - at >= 4 && ReadU32(data.data() + at) == kAnimMagic) {
    ok = ParseAnim(data, at, out, error);
  }
  if (ok && at != data.size()) {
    error = "unknown data after the instances";
    ok = false;
  }
  if (!ok) {
    out.clear();
    parsed = {};
    return false;
  }
  return true;
}

std::vector<uint8_t> Write(const std::vector<Instance>& instances, const Script* script) {
  std::vector<uint8_t> out;
  out.reserve(kHeaderBytes + instances.size() * kInstanceBytes);
  PutU32(out, kMagic);
  PutU32(out, kVersion);
  PutU32(out, uint32_t(instances.size()));
  for (const Instance& instance : instances) {
    PutU32(out, instance.model);
    for (int j = 0; j < 12; ++j) {
      uint32_t bits;
      std::memcpy(&bits, &instance.transform[j], 4);
      PutU32(out, bits);
    }
    const size_t links = instance.links.size() < 0xffff ? instance.links.size() : 0xffff;
    out.push_back(instance.layer);
    out.push_back(instance.active ? 1 : 0);
    out.push_back(uint8_t(links));
    out.push_back(uint8_t(links >> 8));
    PutU32(out, instance.platform);
    for (int j = 0; j < 3; ++j) {
      uint32_t bits;
      std::memcpy(&bits, &instance.platformStart[j], 4);
      PutU32(out, bits);
    }
    for (size_t j = 0; j < links; ++j) {
      PutU32(out, instance.links[j].sender);
      out.push_back(instance.links[j].state);
      out.push_back(instance.links[j].action);
      out.push_back(0);
      out.push_back(0);
    }
  }
  const bool grouped =
      std::any_of(instances.begin(), instances.end(), [](const Instance& i) { return i.group != kNoGroup; });
  if ((script != nullptr && !script->Empty()) || grouped) {
    WriteScript(out, instances, script);
  }
  const size_t glows =
      size_t(std::count_if(instances.begin(), instances.end(), [](const Instance& i) { return i.glows; }));
  if (glows != 0) {
    PutU32(out, kGlowMagic);
    PutU32(out, uint32_t(glows));
    for (size_t i = 0; i < instances.size(); ++i) {
      if (instances[i].glows) {
        PutU32(out, uint32_t(i));
        for (float v : instances[i].glow) {
          PutF32(out, v);
        }
      }
    }
  }
  const size_t anims = size_t(
      std::count_if(instances.begin(), instances.end(), [](const Instance& i) { return !i.animKeys.empty(); }));
  if (anims != 0) {
    PutU32(out, kAnimMagic);
    PutU32(out, uint32_t(anims));
    for (size_t i = 0; i < instances.size(); ++i) {
      const Instance& instance = instances[i];
      if (!instance.animKeys.empty()) {
        PutU32(out, uint32_t(i));
        PutF32(out, instance.animFps);
        PutU32(out, uint32_t(instance.animKeys.size() / 7));
        for (size_t k = 0; k < instance.animKeys.size() / 7 * 7; ++k) {
          PutF32(out, instance.animKeys[k]);
        }
      }
    }
  }
  return out;
}

bool ParseLods(const std::vector<uint8_t>& data, std::vector<Lods>& out, std::string& error) {
  out.clear();
  if (data.size() < kHeaderBytes || ReadU32(data.data()) != kLodMagic) {
    error = "not a level of detail table";
    return false;
  }
  if (ReadU32(data.data() + 4) != kLodVersion) {
    error = "unknown version " + std::to_string(ReadU32(data.data() + 4));
    return false;
  }
  const uint32_t count = ReadU32(data.data() + 8);
  size_t at = kHeaderBytes;
  for (uint32_t i = 0; i < count; ++i) {
    if (data.size() - at < 8) {
      error = "cut short";
      return false;
    }
    Lods& lods = out.emplace_back();
    lods.model = ReadU32(data.data() + at);
    const uint32_t levels = ReadU32(data.data() + at + 4);
    at += 8;
    if (levels == 0 || levels >= uint32_t(kLodLevels) || data.size() - at < size_t(levels) * 8) {
      error = "a model with " + std::to_string(levels) + " levels";
      return false;
    }
    for (uint32_t l = 0; l < levels; ++l) {
      LodLevel& level = lods.levels.emplace_back();
      level.model = ReadU32(data.data() + at + 4);
      if (!ReadF32(data.data() + at, level.distanceSq) || level.distanceSq <= 0.f ||
          (l > 0 && level.distanceSq <= lods.levels[l - 1].distanceSq)) {
        error = "a level's distance is out of order";
        return false;
      }
      at += 8;
    }
  }
  return true;
}

std::vector<uint8_t> WriteLods(const std::vector<Lods>& models) {
  std::vector<uint8_t> out;
  PutU32(out, kLodMagic);
  PutU32(out, kLodVersion);
  PutU32(out, uint32_t(models.size()));
  for (const Lods& lods : models) {
    PutU32(out, lods.model);
    PutU32(out, uint32_t(lods.levels.size()));
    for (const LodLevel& level : lods.levels) {
      PutF32(out, level.distanceSq);
      PutU32(out, level.model);
    }
  }
  return out;
}

} // namespace PortRoomGeo
