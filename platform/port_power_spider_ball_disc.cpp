// The Power Suit spider ball's sources and cache (port_power_spider_ball.h):
// reads the retail models and textures from the disc, makes the look once,
// and keeps it in <user folder>/generated/power-spider/ under a stamp of the
// generator version and the disc, so a later boot only reads the files.

#include "port_power_spider_ball.h"

#include "port_disc.h"
#include "port_log.h"
#include "port_model_variant.h"
#include "port_paths.h"
#include "port_strings.h"
#include "port_bytes.h"
#include "port_custom_res.h"
#include "port_ws.h"

#include <aurora/dvd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <unordered_set>

namespace fs = std::filesystem;

namespace PortPowerSpiderBall {
namespace {

constexpr uint32_t kTXTR = 0x54585452;
constexpr uint32_t kCMDL = 0x434D444C;
constexpr uint32_t kANCS = 0x414E4353;
constexpr const char* kStampName = "stamp.txt";

std::string TypeName(uint32_t type) {
  return {char(type >> 24), char(type >> 16), char(type >> 8), char(type)};
}

std::string FileName(uint32_t type, uint32_t id) {
  char name[16];
  std::snprintf(name, sizeof(name), "%08X.", id);
  return name + TypeName(type);
}

// Every resource of the base disc's PAKs, read on demand.
class Disc {
public:
  ~Disc() {
    for (auto& [entry, handle] : m_handles) {
      aurora_dvd_base_close(handle);
    }
  }

  void Index() {
    const int32_t baseCount = aurora_dvd_base_entry_count();
    for (const auto& [entry, path] : PortMods::DiscPaks()) {
      if (entry < 0 || entry >= baseCount) {
        continue;  // a PAK only a mod brings
      }
      void* handle = aurora_dvd_base_open(entry);
      if (handle == nullptr) {
        continue;
      }
      m_handles[entry] = handle;
      std::vector<uint8_t> header;
      PortMods::PakTable table;
      size_t needed = 0x10000;
      bool parsed = false;
      while (needed <= (64u << 20)) {
        header.resize(needed);
        const size_t got = ReadAt(handle, 0, header.data(), header.size());
        if (PortMods::ParsePakTable(header.data(), got, table, needed)) {
          parsed = true;
          break;
        }
        if (got < header.size() || needed <= header.size()) {
          break;
        }
      }
      if (!parsed) {
        continue;
      }
      for (const PortMods::PakResource& res : table.resources) {
        m_ids.insert(res.id);
        m_where.emplace(Key(res.type, res.id), Where{entry, res.offset, res.size, res.compressed != 0});
      }
    }
  }

  bool HasId(uint32_t id) const { return m_ids.count(id) != 0; }

  bool Read(uint32_t type, uint32_t id, std::vector<uint8_t>& out) {
    const auto found = m_where.find(Key(type, id));
    if (found == m_where.end()) {
      return false;
    }
    const Where& where = found->second;
    std::vector<uint8_t> raw(where.size);
    if (ReadAt(m_handles[where.entry], where.offset, raw.data(), raw.size()) != raw.size()) {
      return false;
    }
    if (!where.compressed) {
      out = std::move(raw);
      return true;
    }
    // A big-endian length, then a zlib stream: two header bytes, the DEFLATE
    // data, and a checksum the decoder never reaches.
    if (raw.size() < 6) {
      return false;
    }
    const size_t length = port::ReadBE32(raw.data());
    const std::string deflated(raw.begin() + 6, raw.end());
    PortWs::Inflater inflater;
    inflater.SetKeepWindow(false);
    std::string inflated;
    if (!inflater.InflateMessage(deflated, inflated, length) || inflated.size() != length) {
      return false;
    }
    out.assign(inflated.begin(), inflated.end());
    return true;
  }

private:
  struct Where {
    int32_t entry = -1;
    uint32_t offset = 0;
    uint32_t size = 0;
    bool compressed = false;
  };

  static uint64_t Key(uint32_t type, uint32_t id) { return (uint64_t(type) << 32) | id; }

  static size_t ReadAt(void* handle, uint64_t offset, uint8_t* out, size_t size) {
    if (aurora_dvd_base_seek(handle, int64_t(offset), 0) != int64_t(offset)) {
      return 0;
    }
    size_t done = 0;
    while (done < size) {
      const int64_t got = aurora_dvd_base_read(handle, out + done, size - done);
      if (got <= 0) {
        break;
      }
      done += size_t(got);
    }
    return done;
  }

  std::map<int32_t, void*> m_handles;
  std::map<uint64_t, Where> m_where;
  std::unordered_set<uint32_t> m_ids;
};

struct Output {
  uint32_t type = 0;
  uint32_t id = 0;
  std::vector<uint8_t> data;
};

// Logs why, returns false.
bool Fail(const std::string& why) {
  PortLog::Write("power spider ball: %s; the Power Suit keeps the Gravity Suit's spider ball\n", why.c_str());
  return false;
}

bool ReadImage(Disc& disc, uint32_t id, PortRemastered::Image& image) {
  std::vector<uint8_t> txtr;
  return disc.Read(kTXTR, id, txtr) && DecodeCmprTxtr(txtr, image);
}

// The resources the cache holds, in a fixed order.
std::vector<std::pair<uint32_t, uint32_t>> OutputKeys() {
  return {{kTXTR, PortModelVariant::Id(kSpiderVaria, 0)}, {kTXTR, PortModelVariant::Id(kLowPolyVaria, 0)},
          {kCMDL, PortModelVariant::Id(kSpiderCmdl, 0)},  {kCMDL, PortModelVariant::Id(kLowPolyCmdl, 0)},
          {kCMDL, PortModelVariant::Id(kGlassCmdl, 0)},   {kANCS, PortModelVariant::Id(kSpiderAncs, 0)}};
}

bool Generate(std::vector<Output>& outputs) {
  Disc disc;
  disc.Index();

  // The new ids must be no retail resource's (PortModelVariant).
  const std::map<uint32_t, uint32_t> spiderSwap{{kSpiderVaria, PortModelVariant::Id(kSpiderVaria, 0)}};
  const std::map<uint32_t, uint32_t> lowPolySwap{{kLowPolyVaria, PortModelVariant::Id(kLowPolyVaria, 0)}};
  std::unordered_set<uint32_t> ids;
  for (const auto& [type, id] : OutputKeys()) {
    if (disc.HasId(id) || !ids.insert(id).second) {
      return Fail("id " + FileName(type, id) + " is taken");
    }
  }

  PortRemastered::Image ballPower, ballVaria;
  if (!ReadImage(disc, kBallPower, ballPower) || !ReadImage(disc, kBallVaria, ballVaria)) {
    return Fail("the normal ball's textures are not on the disc");
  }
  const Shift shift = FitShift(ballVaria, ballPower);
  if (shift.samples == 0) {
    return Fail("the Varia and Power balls do not differ");
  }
  PortLog::Write("power spider ball: Varia to Power is hue %+.1f, saturation %+.3f, value %+.3f (%zu texels)\n",
                 shift.hue, shift.saturation, shift.value, shift.samples);

  for (const auto& [varia, gravity] : {std::pair{kSpiderVaria, kSpiderGravity}, std::pair{kLowPolyVaria, kLowPolyGravity}}) {
    PortRemastered::Image look, other;
    if (!ReadImage(disc, varia, look) || !ReadImage(disc, gravity, other)) {
      return Fail("spider ball texture " + FileName(kTXTR, varia) + " or " + FileName(kTXTR, gravity) +
                  " is missing");
    }
    const size_t changed = Recolour(look, other, shift);
    PortLog::Write("power spider ball: %s: %zu of %d texels recoloured\n", FileName(kTXTR, varia).c_str(), changed,
                   look.width * look.height);
    outputs.push_back({kTXTR, PortModelVariant::Id(varia, 0), PortRemastered::EncodeTxtrCmpr(look, true)});
  }

  for (const auto& [cmdl, swap] : {std::pair{kSpiderCmdl, &spiderSwap}, std::pair{kLowPolyCmdl, &lowPolySwap},
                                   std::pair{kGlassCmdl, static_cast<const std::map<uint32_t, uint32_t>*>(nullptr)}}) {
    std::vector<uint8_t> model;
    if (!disc.Read(kCMDL, cmdl, model) || !CopyMaterialSet(model, 1, 0, swap != nullptr ? *swap : std::map<uint32_t, uint32_t>{})) {
      return Fail("model " + FileName(kCMDL, cmdl) + " is missing or not as expected");
    }
    outputs.push_back({kCMDL, PortModelVariant::Id(cmdl, 0), std::move(model)});
  }

  std::vector<uint8_t> ancs;
  if (!disc.Read(kANCS, kSpiderAncs, ancs) ||
      !PortCustomRes::SetAncsModel(ancs, kSpiderCmdl, PortModelVariant::Id(kSpiderCmdl, 0))) {
    return Fail("character " + FileName(kANCS, kSpiderAncs) + " is missing or not as expected");
  }
  outputs.push_back({kANCS, PortModelVariant::Id(kSpiderAncs, 0), std::move(ancs)});
  return true;
}

}  // namespace

std::vector<PortMods::LooseResource> Collect() {
  const std::string& user = PortPaths::UserFolder();
  if (user.empty()) {
    return {};
  }
  const fs::path dir = PortPaths::detail::FromUtf8(user) / "generated" / "power-spider";
  const std::string stamp = "power-spider " + std::to_string(kVersion) + " " +
                            PortDisc::Name(PortDisc::Current()) + "\n";
  std::error_code ec;
  const auto cached = [&]() {
    std::vector<PortMods::LooseResource> found;
    for (const auto& [type, id] : OutputKeys()) {
      const fs::path file = dir / FileName(type, id);
      const uintmax_t size = fs::file_size(file, ec);
      if (ec || size == 0) {
        return std::vector<PortMods::LooseResource>{};
      }
      found.push_back({type, id, PortPaths::detail::ToUtf8(file), size, "generated"});
    }
    return found;
  };
  {
    std::ifstream in(dir / kStampName);
    if (in.is_open() && port::ReadAllText(in) == stamp) {
      std::vector<PortMods::LooseResource> found = cached();
      if (!found.empty()) {
        PortLog::Write("power spider ball: from the cache in %s\n", PortPaths::detail::ToUtf8(dir).c_str());
        return found;
      }
    }
  }

  std::vector<Output> outputs;
  if (!Generate(outputs)) {
    return {};
  }
  fs::remove(dir / kStampName, ec);
  fs::create_directories(dir, ec);
  // Each file goes in whole (written aside, then renamed), and the stamp last,
  // so an interrupted write leaves no stamp and is made again.
  const auto put = [&](const std::string& name, const char* data, size_t size) {
    const fs::path file = dir / name;
    fs::path temporary = file;
    temporary += ".tmp";
    {
      std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
      out.write(data, std::streamsize(size));
      out.close();
      if (!out) {
        fs::remove(temporary, ec);
        return Fail("cannot write " + PortPaths::detail::ToUtf8(file));
      }
    }
    fs::rename(temporary, file, ec);
    if (ec) {
      fs::remove(temporary, ec);
      return Fail("cannot write " + PortPaths::detail::ToUtf8(file));
    }
    return true;
  };
  for (const Output& output : outputs) {
    if (!put(FileName(output.type, output.id), reinterpret_cast<const char*>(output.data.data()),
             output.data.size())) {
      return {};
    }
  }
  if (!put(kStampName, stamp.data(), stamp.size())) {
    return {};
  }
  PortLog::Write("power spider ball: made in %s\n", PortPaths::detail::ToUtf8(dir).c_str());
  return cached();
}

}  // namespace PortPowerSpiderBall
