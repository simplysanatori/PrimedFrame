#include "port_remastered_import.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <aurora/dvd.h>
#include <aurora/gfx.h>

#include "port_build_info.h"
#include "port_gallery.h"
#include "port_map_icons.h"
#include "port_model_variant.h"
#include "port_mods.h"
#include "port_remastered_cmdl.h"
#include "port_remastered_convert.h"
#include "port_remastered_effect_import.h"
#include "port_remastered_font.h"
#include "port_remastered_image.h"
#include "port_remastered_hud.h"
#include "port_remastered_map.h"
#include "port_remastered_movie.h"
#include "port_remastered_nsp.h"
#include "port_remastered_pak.h"
#include "port_remastered_room.h"
#include "port_remastered_table.h"
#include "port_remastered_text.h"
#include "port_remastered_txtr.h"
#include "port_room_geo.h"
#include "port_ws.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace PortRemastered {
namespace {

namespace fs = std::filesystem;

constexpr uint32_t kCMDL = 0x434D444C;
constexpr uint32_t kSMDL = 0x534D444C;
constexpr uint32_t kWMDL = 0x574D444C;  // a liquid's surface
constexpr uint32_t kCSKR = 0x43534B52;
constexpr uint32_t kANCS = 0x414E4353;
constexpr uint32_t kTXTR = 0x54585452;
constexpr uint32_t kMLVL = 0x4D4C564C;
constexpr uint32_t kMREA = 0x4D524541;
constexpr uint32_t kSTRG = 0x53545247;
constexpr uint32_t kMSBT = 0x4D534254;
constexpr uint32_t kFONT = 0x464F4E54;
constexpr uint32_t kGUIF = 0x47554946;
constexpr uint32_t kCMAP = 0x434D4150;
constexpr uint32_t kMAPA = 0x4D415041;
constexpr uint32_t kMAPW = 0x4D415057;
constexpr uint32_t kFRME = 0x46524D45;
constexpr uint32_t kFMV0 = 0x464D5630;
constexpr uint32_t kGENP = 0x47454E50;  // a particle effect
constexpr uint32_t kMATI = 0x4D415449;  // a material instance

constexpr const char* kStagingName = ".remastered-models.importing";
// Written last, so a staging folder without it is an import that was cut short.
constexpr const char* kMarkerName = "import-complete";
constexpr const char* kRoomFolder = "roomenv";
constexpr const char* kGeometryFolder = "roomgeo";
constexpr const char* kTextFolder = "text";
constexpr const char* kFontFolder = "font";
constexpr const char* kFontName = "deface.sdfont";
constexpr const char* kHudFolder = "hud";
constexpr const char* kMapFolder = "map";
// The disc's own folder: a mod's file there is opened in place of the disc's.
constexpr const char* kMovieFolder = "Video";
constexpr const char* kGalleryFolder = "gallery";
// Largest edge of a room geometry texture: there are thousands of them.
constexpr int kGeometryTexture = 1024;
// Texcoords a second a water surface's wave layers move by.
constexpr double kLiquidDrift = 0.02;
// A room model's coarser level of detail is written only when it has at most this share of
// the triangles of the level before it that was: one barely coarser costs a file and a
// switch for nothing.
constexpr double kLodShare = 0.75;

// The coarser levels of a room model worth converting, with the distance squared each
// starts at (Remastered's own rules).
std::vector<std::pair<int, float>> CoarserLevels(const Model& model) {
  std::vector<std::pair<int, float>> out;
  const size_t levels = std::min(model.lods.size() / 5, size_t(PortRoomGeo::kLodLevels));
  if (levels < 2 || model.lodRules.size() < levels) {
    return out;
  }
  auto triangles = [&](size_t level) {
    std::vector<bool> seen(model.meshes.size(), false);
    size_t count = 0;
    for (size_t r = level * 5; r < level * 5 + 5; ++r) {
      const ModelLod& range = model.lods[r];
      for (uint64_t i = range.indexOffset;
           i < uint64_t(range.indexOffset) + range.indexCount && i < model.lodMeshes.size(); ++i) {
        const uint16_t mesh = model.lodMeshes[i];
        if (mesh < seen.size() && !seen[mesh]) {
          seen[mesh] = true;
          count += model.meshes[mesh].indices.size() / 3;
        }
      }
    }
    return count;
  };
  double kept = double(triangles(0));
  float previous = 0.f;
  for (size_t level = 1; level < levels; ++level) {
    const float rule = model.lodRules[level];
    const size_t count = triangles(level);
    if (!std::isfinite(rule) || rule <= previous || count == 0) {
      break;
    }
    previous = rule;
    if (double(count) <= kLodShare * kept) {
      out.emplace_back(int(level), rule);
      kept = double(count);
    }
  }
  return out;
}

// The rooms whose geometry is imported, from MP_REMASTERED_GEOMETRY: "all", or
// room names (any part of one) separated by commas, or "none". Without it,
// what SetImportGeometry() last said: none, the port's drawing of room
// geometry being unfinished.
std::atomic<bool> sGeometry{false};

bool WantsGeometry(const std::string& room) {
  const char* env = std::getenv("MP_REMASTERED_GEOMETRY");
  if (env == nullptr || env[0] == '\0') {
    return sGeometry.load();
  }
  const std::string list = env;
  if (list == "none") {
    return false;
  }
  if (list == "all") {
    return true;
  }
  for (size_t at = 0; at <= list.size();) {
    const size_t comma = std::min(list.find(',', at), list.size());
    if (comma > at && room.find(list.substr(at, comma - at)) != std::string::npos) {
      return true;
    }
    at = comma + 1;
  }
  return false;
}

// The import runs beside the game on nearly every core: its threads only take
// the time the game leaves, or the game stutters for as long as it runs.
void YieldToGame() {
#if defined(_WIN32)
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_LOWEST);
#elif defined(__linux__)
  setpriority(PRIO_PROCESS, id_t(syscall(SYS_gettid)), 19);
#endif
}

std::mutex sStateMutex;
ImportState sState;
std::thread sThread;
std::atomic<bool> sCancel{false};

fs::path PathFromString(const std::string& text) { return fs::path(std::u8string(text.begin(), text.end())); }

void SetMessage(const std::string& message) {
  std::lock_guard<std::mutex> lock(sStateMutex);
  sState.message = message;
}

void AddLine(const std::string& line) {
  std::lock_guard<std::mutex> lock(sStateMutex);
  sState.lines.push_back(line);
  if (sState.lines.size() > kImportMaxLines) {
    sState.lines.erase(sState.lines.begin(), sState.lines.end() - kImportMaxLines);
  }
}

void Finish(bool ok, const std::string& message) {
  std::lock_guard<std::mutex> lock(sStateMutex);
  sState.running = false;
  sState.finished = true;
  sState.ok = ok;
  sState.cancelled = !ok && sCancel.load();
  sState.message = message;
}

// MP_REMASTERED_TEXT=0 leaves the disc's wording alone.
bool WantsText() {
  const char* env = std::getenv("MP_REMASTERED_TEXT");
  return env == nullptr || std::strcmp(env, "0") != 0;
}

// MP_REMASTERED_HUD=0 leaves the disc's HUD alone.
bool WantsHud() {
  const char* env = std::getenv("MP_REMASTERED_HUD");
  return env == nullptr || std::strcmp(env, "0") != 0;
}

// MP_REMASTERED_GALLERY=0 leaves the Extras gallery out.
bool WantsGallery() {
  const char* env = std::getenv("MP_REMASTERED_GALLERY");
  return env == nullptr || std::strcmp(env, "0") != 0;
}

// MP_REMASTERED_MOVIES=0 leaves the disc's movies alone; a size and rate
// ("1280x720@30") is what they are written as.
bool WantsMovies(MovieFormat& format) {
  const char* env = std::getenv("MP_REMASTERED_MOVIES");
  if (env == nullptr || env[0] == '\0' || std::strcmp(env, "1") == 0) {
    return true;
  }
  if (std::strcmp(env, "0") == 0) {
    return false;
  }
  if (!ParseMovieFormat(env, format)) {
    AddLine(std::string("MP_REMASTERED_MOVIES: \"") + env + "\" is not a size and rate like 1280x720@30");
  }
  return true;
}

// --- The retail disc ----------------------------------------------------------

// The CMDL, CSKR, TXTR, MLVL, MREA, STRG, FRME, MAPA and MAPW resources of the unmodded disc, and every id on it.
class Retail {
public:
  ~Retail() {
    for (auto& [entry, handle] : m_handles) {
      aurora_dvd_base_close(handle);
    }
  }

  bool Index(std::string& error) {
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
        if (res.type == kCMDL || res.type == kCSKR || res.type == kANCS || res.type == kTXTR || res.type == kMLVL ||
            res.type == kMREA || res.type == kSTRG || res.type == kFRME || res.type == kMAPA || res.type == kMAPW) {
          m_resources.emplace(Key(res.type, res.id), Where{entry, res.offset, res.size, res.compressed != 0});
        }
      }
    }
    if (m_resources.empty()) {
      error = "no models found on the disc";
      return false;
    }
    return true;
  }

  bool HasId(uint32_t id) const { return m_ids.count(id) != 0; }

  // The ids of every resource of `type` it reads, in no particular order.
  std::vector<uint32_t> Ids(uint32_t type) const {
    std::vector<uint32_t> ids;
    for (const auto& [key, where] : m_resources) {
      if (uint32_t(key >> 32) == type) {
        ids.push_back(uint32_t(key));
      }
    }
    return ids;
  }

  bool Read(uint32_t type, uint32_t id, std::vector<uint8_t>& out) {
    const auto found = m_resources.find(Key(type, id));
    if (found == m_resources.end()) {
      return false;
    }
    const Where& where = found->second;
    if (!where.compressed) {
      std::vector<uint8_t> raw(where.size);
      {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (ReadAt(m_handles[where.entry], where.offset, raw.data(), raw.size()) != raw.size()) {
          return false;
        }
      }
      out = std::move(raw);
      return true;
    }
    // Read straight into the string the inflater takes, so the blob is not copied into one.
    std::string raw(where.size, '\0');
    {
      std::lock_guard<std::mutex> lock(m_mutex);
      if (ReadAt(m_handles[where.entry], where.offset, reinterpret_cast<uint8_t*>(raw.data()), raw.size()) !=
          raw.size()) {
        return false;
      }
    }
    // A big-endian length, then a zlib stream: two header bytes, the DEFLATE
    // data, and a checksum the decoder never reaches.
    if (raw.size() < 6) {
      return false;
    }
    const auto byte = [&raw](size_t i) { return size_t(static_cast<uint8_t>(raw[i])); };
    const size_t length = byte(0) << 24 | byte(1) << 16 | byte(2) << 8 | byte(3);
    raw.erase(0, 6);
    PortWs::Inflater inflater;
    inflater.SetKeepWindow(false);
    std::string inflated;
    if (!inflater.InflateMessage(raw, inflated, length) || inflated.size() != length) {
      return false;
    }
    out.assign(inflated.begin(), inflated.end());
    return true;
  }

private:
  struct Where {
    int32_t entry;
    uint32_t offset;
    uint32_t size;
    bool compressed;
  };

  static uint64_t Key(uint32_t type, uint32_t id) { return uint64_t(type) << 32 | id; }

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

  std::mutex m_mutex;
  std::map<int32_t, void*> m_handles;
  std::unordered_map<uint64_t, Where> m_resources;
  std::unordered_set<uint32_t> m_ids;
};

// --- The Remastered image ------------------------------------------------------

// Every model and texture in the image's paks, by id.
class Remastered {
public:
  bool Open(const std::string& nspPath, const std::string& keysPath, std::string& error) {
    if (!m_nsp.Open(nspPath, keysPath, error)) {
      return false;
    }
    std::vector<const RomfsFile*> files;
    for (const RomfsFile& file : m_nsp.Files()) {
      if (file.path.size() > 4 && file.path.compare(file.path.size() - 4, 4, ".pak") == 0) {
        files.push_back(&file);
      }
    }
    for (size_t i = 0; i < files.size(); ++i) {
      if (sCancel) {
        error = "cancelled";
        return false;
      }
      SetMessage("Reading the paks (" + std::to_string(i + 1) + "/" + std::to_string(files.size()) + ")");
      const RomfsFile* file = files[i];
      auto pak = std::make_unique<Pak>();
      std::string pakError;
      // Nsp::Read is safe from the workers side by side.
      const ReadFn read = [this, file](uint64_t offset, void* out, size_t size) {
        std::string ignored;
        return m_nsp.Read(*file, offset, out, size, ignored);
      };
      if (!pak->Open(read, file->size, pakError)) {
        AddLine(file->path + ": " + pakError);
        continue;
      }
      const std::vector<PakAsset>& assets = pak->Assets();
      for (size_t a = 0; a < assets.size(); ++a) {
        const uint32_t type = assets[a].type;
        if (type == kCMDL || type == kSMDL || type == kWMDL) {
          m_models.emplace(assets[a].id, Where{m_paks.size(), a});
          for (const std::string& name : assets[a].names) {
            m_modelNames.emplace(FrameKey(name), Where{m_paks.size(), a});
          }
        } else if (type == kTXTR) {
          m_textures.emplace(assets[a].id, Where{m_paks.size(), a});
          for (const std::string& name : assets[a].names) {
            m_textureNames.emplace(FrameKey(name), Where{m_paks.size(), a});
          }
        } else if (type == kMSBT) {
          m_texts.emplace(assets[a].id, Where{m_paks.size(), a});
        } else if (type == kFONT) {
          m_fonts.emplace(assets[a].id, Where{m_paks.size(), a});
        } else if (type == kFMV0) {
          m_movies.emplace(IdToString(assets[a].id), Where{m_paks.size(), a});
        } else if (type == kGENP) {
          m_effects.emplace(assets[a].id, Where{m_paks.size(), a});
        } else if (type == kMATI) {
          m_materials.emplace(assets[a].id, Where{m_paks.size(), a});
        } else if (type == kGUIF) {
          for (const std::string& name : assets[a].names) {
            m_frames.emplace(FrameKey(name), Where{m_paks.size(), a});
          }
        } else if (type == kCMAP) {
          for (const std::string& name : assets[a].names) {
            m_maps.emplace(FrameKey(name), Where{m_paks.size(), a});
          }
        }
      }
      m_paks.push_back(std::move(pak));
      m_paths.push_back(file->path);
    }
    if (m_models.empty() && m_movies.empty()) {
      error = "no models in this image; is it Metroid Prime Remastered?";
      return false;
    }
    return true;
  }

  bool ReadModel(const ModelUuid& id, std::vector<uint8_t>& out, std::string& error) const {
    return Read(m_models, id, out, error);
  }
  bool ReadTexture(const ModelUuid& id, std::vector<uint8_t>& out, std::string& error) const {
    return Read(m_textures, id, out, error);
  }

  // The FONT assets, in id order. There are several, with different sets of characters.
  std::vector<ModelUuid> Fonts() const {
    std::vector<ModelUuid> ids;
    for (const auto& [id, where] : m_fonts) {
      ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
  }
  bool ReadFont(const ModelUuid& id, std::vector<uint8_t>& out, std::string& error) const {
    return Read(m_fonts, id, out, error);
  }

  // A GUI frame by its asset name ("FRME_CombatHud"), whatever folder and case the pak has it under.
  bool ReadFrame(const std::string& name, std::vector<uint8_t>& out, std::string& error) const {
    const auto found = m_frames.find(FrameKey(name));
    if (found == m_frames.end()) {
      error = "not in the image";
      return false;
    }
    const Pak& pak = *m_paks[found->second.pak];
    return pak.ReadAsset(pak.Assets()[found->second.asset], out, error);
  }

  // A texture by its asset name ("TXTR_IconS"), as ReadFrame finds a frame.
  bool ReadTextureNamed(const std::string& name, std::vector<uint8_t>& out, std::string& error) const {
    const auto found = m_textureNames.find(FrameKey(name));
    if (found == m_textureNames.end()) {
      error = "not in the image";
      return false;
    }
    const Pak& pak = *m_paks[found->second.pak];
    return pak.ReadAsset(pak.Assets()[found->second.asset], out, error);
  }

  // The Extras gallery's pictures, in the pak's order: the textures of UI_FrontEnd at least 1000 texels high (the
  // concept art; the menu backdrops are 1600x900). `visit` gets each one's position and its TXTR file, and says
  // whether to go on.
  template <class Visit>
  void ForEachGalleryTexture(Visit&& visit) const {
    size_t position = 0;
    for (size_t i = 0; i < m_paks.size(); ++i) {
      const std::string& path = m_paths[i];
      if (path.substr(path.rfind('/') + 1) != "UI_FrontEnd.pak") {
        continue;
      }
      const Pak& pak = *m_paks[i];
      for (const PakAsset& asset : pak.Assets()) {
        std::vector<uint8_t> raw;
        std::string error;
        TxtrImage info;
        if (asset.type != kTXTR || !pak.ReadAsset(asset, raw, error) ||
            !ReadTxtrInfo(raw.data(), raw.size(), info, error) || info.height < 1000) {
          continue;
        }
        if (!visit(position++, raw)) {
          return;
        }
      }
    }
  }

  // A model by its asset name ("CMDL_MapCompass"), as ReadFrame finds a frame.
  bool ReadModelNamed(const std::string& name, std::vector<uint8_t>& out, std::string& error) const {
    const auto found = m_modelNames.find(FrameKey(name));
    if (found == m_modelNames.end()) {
      error = "not in the image";
      return false;
    }
    const Pak& pak = *m_paks[found->second.pak];
    return pak.ReadAsset(pak.Assets()[found->second.asset], out, error);
  }

  // A world's map by its asset name ("CMAP_IceLevel"), as ReadFrame finds a frame.
  bool ReadMap(const std::string& name, std::vector<uint8_t>& out, std::string& error) const {
    const auto found = m_maps.find(FrameKey(name));
    if (found == m_maps.end()) {
      error = "not in the image";
      return false;
    }
    const Pak& pak = *m_paks[found->second.pak];
    return pak.ReadAsset(pak.Assets()[found->second.asset], out, error);
  }

  // A movie by its id as IdToString prints it.
  bool ReadMovie(const std::string& id, std::vector<uint8_t>& out, std::string& error) const {
    const auto found = m_movies.find(id);
    if (found == m_movies.end()) {
      error = "not in the image";
      return false;
    }
    const Pak& pak = *m_paks[found->second.pak];
    return pak.ReadAsset(pak.Assets()[found->second.asset], out, error);
  }

  // Every text asset, each once however many paks carry it.
  std::vector<ModelUuid> Texts() const {
    std::vector<ModelUuid> ids;
    for (const auto& [id, where] : m_texts) {
      ids.push_back(id);
    }
    return ids;
  }
  bool ReadText(const ModelUuid& id, std::vector<uint8_t>& out, std::string& error) const {
    return Read(m_texts, id, out, error);
  }

  // The particle effects (GENP) and what they read: material instances and textures.
  std::vector<ModelUuid> Effects() const {
    std::vector<ModelUuid> ids;
    for (const auto& [id, where] : m_effects) {
      ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end());
    return ids;
  }
  bool ReadEffectAsset(uint32_t type, const ModelUuid& id, std::vector<uint8_t>& out, std::string& error) const {
    return Read(type == kGENP ? m_effects : type == kMATI ? m_materials : m_textures, id, out, error);
  }
  uint32_t EffectAssetType(const ModelUuid& id) const {
    return m_textures.count(id) != 0 ? kTXTR : m_materials.count(id) != 0 ? kMATI : m_effects.count(id) != 0 ? kGENP : 0;
  }

  // The paks of one world directory ("Intro_Master") as the room writer takes
  // them, and every pak of the image for the assets rooms share.
  void World(const std::string& dir, RoomPak& master, std::vector<RoomPak>& rooms) const {
    const std::string folder = "/!" + dir + "/";
    for (size_t i = 0; i < m_paks.size(); ++i) {
      const std::string& path = m_paths[i];
      const size_t at = path.find(folder);
      if (at == std::string::npos) {
        continue;
      }
      const std::string name = path.substr(at + folder.size(), path.size() - at - folder.size() - 4);
      if (name == "!" + dir) {
        master = RoomPak{dir, m_paks[i].get()};
      } else if (name.find('/') == std::string::npos) {
        rooms.push_back(RoomPak{name, m_paks[i].get()});
      }
    }
  }
  // The environment BRDF table out of the executable. Never throws.
  bool ExtractBrdf(std::vector<uint8_t>& out, std::string& error) const {
    try {
      return ExtractBrdfLut(m_nsp, out, error);
    } catch (const std::exception& e) {
      error = e.what();
      return false;
    }
  }

  std::vector<RoomPak> AllPaks() const {
    std::vector<RoomPak> all;
    for (size_t i = 0; i < m_paks.size(); ++i) {
      all.push_back(RoomPak{m_paths[i], m_paks[i].get()});
    }
    return all;
  }

private:
  struct Where {
    size_t pak;
    size_t asset;
  };
  using Index = std::unordered_map<std::array<uint8_t, 16>, Where, PakIdHash>;

  static std::string FrameKey(const std::string& name) {
    const size_t slash = name.find_last_of("/\\");
    std::string key = name.substr(slash == std::string::npos ? 0 : slash + 1);
    key = key.substr(0, key.find('.'));
    for (char& c : key) {
      c = char(std::tolower(static_cast<unsigned char>(c)));
    }
    return key;
  }

  bool Read(const Index& index, const ModelUuid& id, std::vector<uint8_t>& out, std::string& error) const {
    const auto found = index.find(id);
    if (found == index.end()) {
      error = "not in the image";
      return false;
    }
    const Pak& pak = *m_paks[found->second.pak];
    return pak.ReadAsset(pak.Assets()[found->second.asset], out, error);
  }

  Nsp m_nsp;
  std::vector<std::unique_ptr<Pak>> m_paks;
  std::vector<std::string> m_paths;  // of m_paks, in the image
  Index m_models;
  Index m_textures;
  Index m_texts;
  Index m_fonts;
  Index m_effects;
  Index m_materials;
  std::unordered_map<std::string, Where> m_frames;  // GUIF, by FrameKey
  std::unordered_map<std::string, Where> m_textureNames;  // the named TXTR, by FrameKey
  std::unordered_map<std::string, Where> m_modelNames;    // the named CMDL, by FrameKey
  std::unordered_map<std::string, Where> m_maps;  // CMAP, by FrameKey
  std::unordered_map<std::string, Where> m_movies;  // FMV0, by IdToString
};

// --- The import ------------------------------------------------------------------

fs::path StagingFolder() {
  const std::string mods = PortMods::Folder();
  return mods.empty() ? fs::path() : PathFromString(mods) / kStagingName;
}

ConvertOptions OptionsFor(const TableEntry& entry) {
  ConvertOptions options;
  options.retail = entry.retail;
  for (int i = 0; i < 9; ++i) {
    options.orient[i / 3][i % 3] = entry.orient[i];
  }
  for (int i = 0; i < 3; ++i) {
    options.offset[i] = entry.offset[i];
  }
  const uint32_t* skins = TableSkins(entry);
  options.skins.assign(skins, skins + entry.skinCount);
  options.pbr = entry.pbr;
  if (const TableOptions* extra = TableExtra(entry)) {
    options.material = extra->material;
    options.maxTexture = extra->maxTexture;
    if (extra->squeezeRole != nullptr) {
      options.squeeze = true;
      options.squeezeRole = extra->squeezeRole;
      options.squeezeFrom[0] = extra->squeeze[0];
      options.squeezeFrom[1] = extra->squeeze[1];
      options.squeezeTo[0] = extra->squeeze[2];
      options.squeezeTo[1] = extra->squeeze[3];
    }
  }
  return options;
}

// Remastered's menu movies, written into `folder` under the disc's names
// (port_remastered_movie.h). Returns how many of the disc's movies were replaced.
int ImportMovies(const Remastered& remastered, const fs::path& folder, const MovieFormat& format, bool& noFfmpeg) {
  SetMessage("Looking for ffmpeg");
  const std::string ffmpeg = FindFfmpeg();
  noFfmpeg = ffmpeg.empty();
  if (noFfmpeg) {
    AddLine("movies skipped: ffmpeg not found. Install ffmpeg (or put it next to the game), then use \"Import "
            "movies\".");
    return 0;
  }
  std::error_code ec;
  fs::create_directories(folder, ec);
  const auto text = [](const fs::path& path) {
    const std::u8string u8 = path.u8string();
    return std::string(u8.begin(), u8.end());
  };
  // ffmpeg reads the MP4 from a file: it has to seek in it.
  const fs::path source = folder / "import.tmp.mp4";
  const std::vector<Movie>& movies = Movies();
  int written = 0;
  for (size_t i = 0; i < movies.size() && !sCancel; ++i) {
    const Movie& movie = movies[i];
    SetMessage("Converting the movies (" + std::to_string(i + 1) + "/" + std::to_string(movies.size()) + ")");
    std::vector<uint8_t> raw;
    std::string error;
    size_t offset = 0;
    size_t length = 0;
    int frames = 0;
    const fs::path first = folder / (std::string(movie.names[0]) + ".thp");
    const fs::path tmp = folder / "import.tmp.thp";
    bool ok = remastered.ReadMovie(movie.id, raw, error) &&
              (MovieStream(raw.data(), raw.size(), offset, length) || (error = "not a movie", false));
    if (ok) {
      std::ofstream file(source, std::ios::binary | std::ios::trunc);
      file.write(reinterpret_cast<const char*>(raw.data() + offset), std::streamsize(length));
      file.close();
      ok = bool(file) || (error = "cannot write to the mod folder", false);
    }
    raw = {};
    ok = ok && ConvertMovie(ffmpeg, text(source), text(tmp), format, [] { return sCancel.load(); }, frames, error);
    if (ok) {
      // Written beside it and renamed, so a movie cut short never has the name.
      fs::rename(tmp, first, ec);
      ok = !ec || (error = "cannot replace the movie: " + ec.message(), false);
    }
    if (!ok) {
      fs::remove(tmp, ec);
      if (!sCancel) {
        AddLine(std::string(movie.names[0]) + ".thp: " + error);
      }
      continue;
    }
    ++written;
    // The disc's other takes of a transition are the same file again.
    for (size_t n = 1; n < movie.names.size(); ++n) {
      const fs::path other = folder / (std::string(movie.names[n]) + ".thp");
      fs::remove(other, ec);
      fs::create_hard_link(first, other, ec);
      if (ec) {
        fs::copy_file(first, other, fs::copy_options::overwrite_existing, ec);
      }
      if (ec) {
        AddLine(std::string(movie.names[n]) + ".thp: " + ec.message());
      } else {
        ++written;
      }
    }
  }
  fs::remove(source, ec);
  if (written == 0) {
    fs::remove(folder, ec); // only if nothing is in it
  }
  return written;
}

// The Extras gallery's concept art as gallery/NNN.jpg in `folder` (port_gallery.h). Returns how many were written.
int ImportGallery(const Remastered& remastered, const fs::path& target) {
  std::error_code ec;
  // Written beside the live folder and swapped in only if a picture came out, so a failed or
  // cancelled re-run keeps the old gallery.
  const fs::path folder = target.string() + ".new";
  fs::remove_all(folder, ec);
  fs::create_directories(folder, ec);
  int written = 0;
  remastered.ForEachGalleryTexture([&](size_t position, const std::vector<uint8_t>& raw) {
    SetMessage("Gallery picture " + std::to_string(position + 1));
    char name[16];
    std::snprintf(name, sizeof(name), "%03d.jpg", int(position));
    std::string error;
    TxtrImage image;
    std::vector<uint8_t> jpeg;
    bool ok = DecodeTxtr(raw.data(), raw.size(), image, error);
    ok = ok && PortGallery::EncodeGalleryJpeg(image.rgba.data(), int(image.width), int(image.height), jpeg);
    if (ok) {
      const fs::path tmp = folder / "import.tmp.jpg";
      {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(jpeg.data()), std::streamsize(jpeg.size()));
        file.close();
        ok = bool(file);
      }
      // Renamed, so a picture cut short never has the name.
      if (ok) {
        fs::rename(tmp, folder / name, ec);
        ok = !ec;
      }
      if (!ok) {
        fs::remove(tmp, ec);
        error = "cannot write to the mod folder";
      }
    } else if (error.empty()) {
      error = "cannot encode";
    }
    if (ok) {
      ++written;
    } else if (!sCancel) {
      AddLine(std::string("gallery/") + name + ": " + error);
    }
    return !sCancel;
  });
  if (written == 0) {
    fs::remove_all(folder, ec);
    return 0;
  }
  fs::remove_all(target, ec);
  fs::rename(folder, target, ec);
  if (ec) {
    std::fprintf(stderr, "gallery: cannot move the new folder into place: %s\n", ec.message().c_str());
    fs::remove_all(folder, ec);
    return 0;
  }
  return written;
}

// Only the movies and the gallery, into the mod an earlier import made: for a player who had
// no ffmpeg at the time, or imported before the gallery.
void RunMovies(std::string nspPath, std::string keysPath, fs::path mod) {
  YieldToGame();
  SetMessage("Opening the image");
  std::string error;
  Remastered remastered;
  if (!remastered.Open(nspPath, keysPath, error)) {
    Finish(false, sCancel ? std::string("Cancelled.") : error);
    return;
  }
  MovieFormat format;
  WantsMovies(format);
  bool noFfmpeg = false;
  const int movies = ImportMovies(remastered, mod / kMovieFolder, format, noFfmpeg);
  const int gallery = WantsGallery() && !sCancel ? ImportGallery(remastered, mod / kGalleryFolder) : 0;
  if (sCancel) {
    Finish(false, "Cancelled.");
  } else if (noFfmpeg && gallery == 0) {
    Finish(false, "ffmpeg not found. Install it, or put it next to the game.");
  } else if (movies == 0 && gallery == 0) {
    Finish(false, "No movies converted.");
  } else {
    std::string message = std::to_string(movies) + " movies converted";
    if (gallery != 0) {
      message += ", " + std::to_string(gallery) + " gallery pictures";
    }
    Finish(true, message + (noFfmpeg ? ". Movies skipped: ffmpeg not found." : "."));
  }
}

void Run(std::string nspPath, std::string keysPath, int threads, fs::path staging) {
  YieldToGame();
  std::error_code ec;
  fs::remove_all(staging, ec);
  fs::create_directories(staging, ec);
  if (ec) {
    Finish(false, "Cannot create the mod folder: " + ec.message());
    return;
  }
  auto fail = [&](const std::string& message) {
    fs::remove_all(staging, ec);
    Finish(false, sCancel ? std::string("Cancelled.") : message);
  };

  SetMessage("Opening the image");
  std::string error;
  Remastered remastered;
  if (!remastered.Open(nspPath, keysPath, error)) {
    fail(error);
    return;
  }
  SetMessage("Reading the disc");
  Retail retail;
  if (!retail.Index(error)) {
    fail(error);
    return;
  }

  size_t count = 0;
  const TableEntry* table = Table(count);
  {
    std::lock_guard<std::mutex> lock(sStateMutex);
    sState.total = int(count);
  }
  std::atomic<size_t> next{0};
  std::atomic<int> converted{0};
  // Ids the import has given out: no two resources of it share one, whatever their types.
  std::mutex takenMutex;
  std::unordered_set<uint32_t> taken;
  // The PBR maps a model worker has taken on, so that a map several models
  // share is converted once rather than once per worker that meets it.
  std::mutex modelClaimMutex;
  std::unordered_set<uint32_t> modelClaimed;
  auto makeIO = [&](int worker, const fs::path& folder) {
    ConvertIO io;
    io.retail = [&](uint32_t type, uint32_t id, std::vector<uint8_t>& out) { return retail.Read(type, id, out); };
    io.retailId = [&](uint32_t id) { return retail.HasId(id); };
    io.texture = [&](const ModelUuid& id, Image& out, std::string& textureError) {
      std::vector<uint8_t> raw;
      TxtrImage image;
      if (!remastered.ReadTexture(id, raw, textureError) ||
          !DecodeTxtr(raw.data(), raw.size(), image, textureError)) {
        return false;
      }
      out.width = int(image.width);
      out.height = int(image.height);
      out.rgba = std::move(image.rgba);
      return true;
    };
    io.cube = [&](const ModelUuid& id, uint32_t& edge, std::vector<uint8_t>& rgba, std::string& cubeError) {
      std::vector<uint8_t> raw;
      return remastered.ReadTexture(id, raw, cubeError) &&
             DecodeTxtrCubeRgba8(raw.data(), raw.size(), edge, rgba, cubeError);
    };
    // Workers can still meet the same texture at once (a TEV slot's, a solid
    // colour); each writes its own temporary file and the rename decides, the
    // content being the same either way.
    io.write = [&, worker, folder](const std::string& name, const std::vector<uint8_t>& data) {
      {
        std::lock_guard<std::mutex> lock(takenMutex);
        taken.insert(uint32_t(std::strtoul(name.substr(0, 8).c_str(), nullptr, 16)));
      }
      const fs::path path = folder / PathFromString(name);
      const fs::path tmp = folder / PathFromString(name + ".tmp" + std::to_string(worker));
      {
        std::ofstream file(tmp, std::ios::binary);
        file.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        file.close();
        if (!file) {
          return false;
        }
      }
      std::error_code renameError;
      fs::rename(tmp, path, renameError);
      return !renameError;
    };
    return io;
  };
  // Second looks of a retail model (TableEntry::ancs, key) are written under
  // ids of their own, given out here before anything is written so that they
  // come out the same in every import.
  struct Look {
    bool ok = true;
    std::string error;
    uint32_t model = 0;  // the CMDL's id, 0 for the retail one
    std::vector<uint32_t> skins;  // the CSKRs' ids, empty for the retail ones
    uint32_t ancs = 0;            // the ANCS copy's id, 0 for none
  };
  std::vector<Look> looks(count);
  // The characters written once the models are in: retail ANCS with some of
  // their (model, skin[, skeleton]) ids, big-endian and side by side, swapped
  // for new ones. A swap is made only if its model and skin were written.
  struct Rebind {
    size_t entry;
    std::vector<uint8_t> from, to;
    uint32_t skin;  // the CSKR `to` names
  };
  struct Character {
    uint32_t source = 0;  // the retail ANCS it is a copy of
    std::vector<uint8_t> data;
    std::vector<Rebind> rebinds;  // its own looks'
  };
  std::map<uint32_t, Character> characters;
  std::vector<Rebind> everywhere;  // swapped in every character that binds them
  auto bytesOf = [](std::initializer_list<uint32_t> ids) {
    std::vector<uint8_t> out;
    for (uint32_t id : ids) {
      for (int b = 0; b < 4; ++b) {
        out.push_back(uint8_t(id >> (24 - b * 8)));
      }
    }
    return out;
  };
  auto readU32 = [](const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; };
  auto hex = [](uint32_t id) {
    char name[16];
    std::snprintf(name, sizeof(name), "%08X", id);
    return std::string(name);
  };
  {
    auto variant = [&](Look& look, uint32_t id, int key) {
      const uint32_t out = PortModelVariant::Id(id, key);
      if (retail.HasId(out) || taken.count(out) != 0) {
        look.ok = false;
        look.error = "the id for look " + std::to_string(key) + " of " + hex(id) + " is taken";
      }
      taken.insert(out);
      return out;
    };
    for (size_t i = 0; i < count; ++i) {
      const TableEntry& entry = table[i];
      Look& look = looks[i];
      if (entry.ancs == kEveryCharacter) {
        if (entry.key >= 0) {
          look.ok = false;
          look.error = "a model bound in every character has no second look";
        }
      } else if (entry.ancs == 0 && entry.key >= 0) {
        look.model = variant(look, entry.retail, entry.key);
      } else if (entry.ancs != 0) {
        look.ancs = entry.key >= 0 ? variant(look, entry.ancs, entry.key) : entry.ancs;
        if (entry.skinCount != 1) {
          look.ok = false;
          look.error = "a character's look needs exactly one skin";
        }
      }
    }
    // The new models and skins of the looks: hashed, then moved past every id
    // the disc or this import has.
    auto fresh = [&](uint32_t seed) {
      uint32_t id = 0x811C9DC5u;  // FNV-1a
      for (int i = 0; i < 4; ++i) {
        id = (id ^ ((seed >> (i * 8)) & 0xFFu)) * 0x01000193u;
      }
      while (id == 0 || id == 0xFFFFFFFFu || retail.HasId(id) || taken.count(id) != 0) {
        ++id;
      }
      taken.insert(id);
      return id;
    };
    for (size_t i = 0; i < count; ++i) {
      const TableEntry& entry = table[i];
      Look& look = looks[i];
      if (entry.ancs != 0 || entry.key < 0 || !look.ok) {
        continue;
      }
      // A static look of a skinned model (the low-poly and glass balls) is
      // drawn without its skin, but the converter still writes one; it must
      // not land on the retail model's.
      const uint32_t* skins = TableSkins(entry);
      for (int s = 0; s < entry.skinCount; ++s) {
        look.skins.push_back(fresh(look.model ^ skins[s] * 0x9E3779B1u));
      }
    }
    for (size_t i = 0; i < count; ++i) {
      const TableEntry& entry = table[i];
      Look& look = looks[i];
      if (look.ancs == 0 || !look.ok) {
        continue;
      }
      // The copy binds the new pair where the retail one binds the old.
      Character& character = characters[look.ancs];
      character.source = entry.ancs;
      if (character.data.empty() && !retail.Read(kANCS, entry.ancs, character.data)) {
        look.ok = false;
        look.error = "character " + hex(entry.ancs) + " is not on the disc";
        continue;
      }
      const std::vector<uint8_t> pair = bytesOf({entry.retail, TableSkins(entry)[0]});
      const std::vector<uint8_t>& data = character.data;
      int found = 0;
      for (auto it = std::search(data.begin(), data.end(), pair.begin(), pair.end()); it != data.end();
           it = std::search(it + 1, data.end(), pair.begin(), pair.end())) {
        ++found;
      }
      if (found != 1) {
        look.ok = false;
        look.error = "character " + hex(entry.ancs) + " binds " + hex(entry.retail) + " " + std::to_string(found) +
                     " times";
        continue;
      }
      const uint32_t seed = entry.ancs * 0x9E3779B1u ^ entry.retail ^ uint32_t(entry.key + 1) * 0x85EBCA6Bu;
      look.model = fresh(seed);
      look.skins = {fresh(seed ^ 0x534B494Eu)};
      character.rebinds.push_back({i, pair, bytesOf({look.model, look.skins[0]}), look.skins[0]});
    }
    // A body whose skin another model shares keeps its id but takes skins of
    // its own, and every character that binds it is rebound to them. Its
    // first skin's CSKR serves every skin it is bound with on the same
    // skeleton: the weights name the skeleton's bones.
    std::vector<size_t> bodies;
    for (size_t i = 0; i < count; ++i) {
      const TableEntry& entry = table[i];
      Look& look = looks[i];
      if (entry.ancs != kEveryCharacter || !look.ok) {
        continue;
      }
      bodies.push_back(i);
      const uint32_t* skins = TableSkins(entry);
      for (int s = 0; s < entry.skinCount; ++s) {
        look.skins.push_back(fresh(entry.retail * 0x85EBCA6Bu ^ skins[s]));
      }
    }
    if (!bodies.empty()) {
      // The skeletons each (body, skin) is bound with.
      std::map<std::pair<size_t, int>, std::set<uint32_t>> skeletons;
      for (uint32_t id : retail.Ids(kANCS)) {
        const auto known = characters.find(id);
        std::vector<uint8_t> read;
        if (known == characters.end() && !retail.Read(kANCS, id, read)) {
          continue;
        }
        const std::vector<uint8_t>& data = known != characters.end() ? known->second.data : read;
        bool binds = false;
        for (size_t i : bodies) {
          const TableEntry& entry = table[i];
          for (int s = 0; s < entry.skinCount; ++s) {
            const std::vector<uint8_t> pair = bytesOf({entry.retail, TableSkins(entry)[s]});
            for (auto it = std::search(data.begin(), data.end(), pair.begin(), pair.end()); data.end() - it >= 12;
                 it = std::search(it + 1, data.end(), pair.begin(), pair.end())) {
              skeletons[{i, s}].insert(readU32(&*(it + 8)));
              binds = true;
            }
          }
        }
        if (binds && known == characters.end()) {
          characters[id] = Character{id, std::move(read), {}};
        }
      }
      for (const auto& [key, bound] : skeletons) {
        const auto [i, s] = key;
        const TableEntry& entry = table[i];
        const Look& look = looks[i];
        const auto first = skeletons.find({i, 0});
        for (uint32_t skeleton : bound) {
          const bool shared = first != skeletons.end() && first->second.count(skeleton) != 0;
          const uint32_t skin = shared ? look.skins[0] : look.skins[s];
          everywhere.push_back({i, bytesOf({entry.retail, TableSkins(entry)[s], skeleton}),
                                bytesOf({entry.retail, skin, skeleton}), skin});
        }
      }
    }
  }
  std::vector<uint8_t> modelOk(count, 0);
  auto work = [&](int worker) {
    YieldToGame();
    ConvertIO io = makeIO(worker, staging);
    io.claim = [&](uint32_t id) {
      std::lock_guard<std::mutex> lock(modelClaimMutex);
      return modelClaimed.insert(id).second;
    };
    Converter converter(std::move(io));
    for (size_t i = next++; i < count && !sCancel; i = next++) {
      const TableEntry& entry = table[i];
      const Look& look = looks[i];
      ModelUuid id;
      std::memcpy(id.data(), entry.rem, 16);
      std::string modelError = look.error;
      bool ok = false;
      // A worker thread that lets an exception out (bad_alloc on a phone)
      // terminates the game, so a model that throws only fails itself.
      try {
        std::vector<uint8_t> raw;
        Model model;
        ConvertOptions options = OptionsFor(entry);
        options.outputModel = look.model;
        options.outputSkins = look.skins;
        ok = look.ok && remastered.ReadModel(id, raw, modelError) &&
             ParseModel(raw.data(), raw.size(), model, modelError) && converter.Convert(model, options, modelError);
      } catch (const std::exception& e) {
        ok = false;
        modelError = e.what();
      }
      if (ok) {
        modelOk[i] = 1;
        ++converted;
      } else {
        char name[16];
        std::snprintf(name, sizeof(name), "%08X", entry.retail);
        AddLine(std::string(name) + ": " + modelError);
      }
      std::lock_guard<std::mutex> lock(sStateMutex);
      ++sState.done;
      if (!ok) {
        ++sState.failed;
      }
      sState.message = "Converting models (" + std::to_string(sState.done) + "/" + std::to_string(sState.total) + ")";
    }
  };
  SetMessage("Converting models");
  std::vector<std::thread> workers;
  for (int i = 1; i < threads; ++i) {
    workers.emplace_back(work, i);
  }
  work(0);
  for (std::thread& worker : workers) {
    worker.join();
  }

  if (sCancel) {
    fail("Cancelled.");
    return;
  }
  if (converted == 0) {
    fail("No model could be converted.");
    return;
  }

  // The characters last, so that none names a model that failed. A copy is
  // written only if one of its own looks made it.
  {
    const auto write = makeIO(0, staging).write;
    for (auto& [id, character] : characters) {
      std::vector<uint8_t>& data = character.data;
      bool own = false;
      bool changed = false;
      auto apply = [&](const Rebind& rebind) {
        bool swapped = false;
        std::error_code skinError;
        if (modelOk[rebind.entry] != 0 && fs::exists(staging / (hex(rebind.skin) + ".CSKR"), skinError)) {
          for (auto it = std::search(data.begin(), data.end(), rebind.from.begin(), rebind.from.end());
                it != data.end();
               it = std::search(it + rebind.from.size(), data.end(), rebind.from.begin(), rebind.from.end())) {
            std::copy(rebind.to.begin(), rebind.to.end(), it);
            swapped = true;
          }
        }
        return swapped;
      };
      for (const Rebind& rebind : character.rebinds) {
        own = apply(rebind) || own;
      }
      for (const Rebind& rebind : everywhere) {
        changed = apply(rebind) || changed;
      }
      if (!(own || (changed && id == character.source))) {
        continue;
      }
      if (!write(hex(id) + ".ANCS", data)) {
        AddLine("could not write " + hex(id) + ".ANCS");
      }
    }
  }

  // Remastered's particle effects in place of the disc's, when asked for.
  if (WantsRemasteredEffects()) {
    SetMessage("Converting effects");
    EffectImportIO effectIO;
    effectIO.effects = remastered.Effects();
    effectIO.read = [&](uint32_t type, const EffectGuid& id, std::vector<uint8_t>& out, std::string& effectError) {
      return remastered.ReadEffectAsset(type, id, out, effectError);
    };
    effectIO.typeOf = [&](const EffectGuid& id) { return remastered.EffectAssetType(id); };
    effectIO.retailId = [&](uint32_t id) { return retail.HasId(id); };
    effectIO.retail = [&](uint32_t type, uint32_t id, std::vector<uint8_t>& out) { return retail.Read(type, id, out); };
    effectIO.freshId = [&](uint32_t seed) {
      std::lock_guard<std::mutex> lock(takenMutex);
      uint32_t id = seed;
      while (id == 0 || id == 0xFFFFFFFFu || retail.HasId(id) || taken.count(id) != 0) {
        ++id;
      }
      taken.insert(id);
      return id;
    };
    effectIO.texture = [&](const EffectGuid& id, int& width, int& height, std::vector<uint8_t>& rgba,
                           std::string& effectError) {
      Image image;
      if (!makeIO(0, staging).texture(id, image, effectError)) {
        return false;
      }
      width = image.width;
      height = image.height;
      rgba = std::move(image.rgba);
      return true;
    };
    effectIO.layers = [&](const EffectGuid& id, int& width, int& height, int& layers, std::vector<uint8_t>& rgba,
                          std::string& effectError) {
      std::vector<uint8_t> raw;
      uint32_t w = 0;
      uint32_t h = 0;
      uint32_t n = 0;
      if (!remastered.ReadTexture(id, raw, effectError) ||
          !DecodeTxtrLayersRgba8(raw.data(), raw.size(), w, h, n, rgba, effectError)) {
        return false;
      }
      width = int(w);
      height = int(h);
      layers = int(n);
      return true;
    };
    // A model of Remastered's own, as a standalone CMDL under the id the effect
    // was given. One converter serves them all: the effects run on one thread.
    std::unique_ptr<Converter> effectModels;
    std::unordered_set<uint32_t> effectClaimed;
    effectIO.model = [&](const EffectGuid& id, uint32_t retailId, std::string& effectError) {
      if (!effectModels) {
        ConvertIO io = makeIO(0, staging);
        io.retailId = [&](uint32_t other) { return retail.HasId(other); };
        io.claim = [&](uint32_t other) { return effectClaimed.insert(other).second; };
        effectModels = std::make_unique<Converter>(std::move(io));
      }
      ConvertOptions options;
      options.retail = retailId;
      options.standalone = true;
      options.skip.clear();
      options.nativeMax = kGeometryTexture;
      try {
        std::vector<uint8_t> raw;
        Model model;
        return remastered.ReadModel(id, raw, effectError) && ParseModel(raw.data(), raw.size(), model, effectError) &&
               effectModels->Convert(model, options, effectError);
      } catch (const std::exception& e) {
        effectError = e.what();
        return false;
      }
    };
    effectIO.write = makeIO(0, staging).write;
    effectIO.log = [](const std::string& line) { AddLine(line); };
    const EffectImportResult effects = ImportEffects(effectIO);
    AddLine("effects: " + std::to_string(effects.written) + " of " + std::to_string(effects.candidates) + " written (" +
            std::to_string(effects.parts) + " PARTs, " + std::to_string(effects.textures) + " textures, " +
            std::to_string(effects.flipbooks) + " flipbooks, " + std::to_string(effects.models) + " models, " +
            std::to_string(effects.dropped) + " properties left out)");
  }

  // The rooms' reflection cubes and baked ambient light, a file per area. A
  // world that cannot be read costs its rooms their environment, not the import.
  SetMessage("Writing the room environments");
  const fs::path roomFolder = staging / kRoomFolder;
  fs::create_directories(roomFolder, ec);
  // Remastered's environment BRDF table, from the user's own executable. Nothing
  // here may fail the import: without it the port keeps its own fit.
  {
    std::vector<uint8_t> brdf;
    std::string brdfError;
    if (!remastered.ExtractBrdf(brdf, brdfError)) {
      AddLine("brdf.lut: left out (" + brdfError + ")");
    } else {
      std::ofstream file(roomFolder / "brdf.lut", std::ios::binary);
      file.write(reinterpret_cast<const char*>(brdf.data()), std::streamsize(brdf.size()));
      file.close();
      if (!file) {
        AddLine("brdf.lut: cannot be written");
      }
    }
  }
  fs::create_directories(staging / kGeometryFolder, ec);
  const std::vector<RoomPak> allPaks = remastered.AllPaks();
  const std::vector<RoomWorld>& worlds = RoomWorlds();
  std::atomic<size_t> nextWorld{0};
  std::atomic<int> roomFiles{0};
  // A room's geometry names its models; they are given ids here and converted
  // afterwards, on every thread. A model that then fails leaves its id naming
  // nothing, which the port skips.
  const fs::path geometryFolder = staging / kGeometryFolder;
  struct GeometryModel {
    ModelUuid uuid;
    uint32_t id;
    int liquid = -1;  // index into `liquids` when it is a liquid's surface
    int joint = -1;   // the joint whose rigid piece of a skinned model it is (ConvertOptions::joint)
    // Ids set aside for its coarser levels of detail (index 0 unused); a level it
    // turns out not to have, or that is not worth its file, leaves its id unused.
    std::array<uint32_t, PortRoomGeo::kLodLevels> lods{};
  };
  std::vector<RoomLiquid> liquids;
  std::vector<GeometryModel> geometry;
  std::unordered_map<ModelUuid, uint32_t, PakIdHash> geometryIds;
  auto geometryId = [&](const ModelUuid& uuid, uint32_t& id) {
    std::lock_guard<std::mutex> lock(takenMutex);
    const auto known = geometryIds.find(uuid);
    if (known != geometryIds.end()) {
      id = known->second;
      return true;
    }
    id = 0x811C9DC5u;  // FNV-1a
    for (const uint8_t byte : uuid) {
      id = (id ^ byte) * 0x01000193u;
    }
    while (id == 0 || id == 0xFFFFFFFFu || retail.HasId(id) || taken.count(id) != 0) {
      ++id;
    }
    taken.insert(id);
    geometryIds.emplace(uuid, id);
    geometry.push_back({uuid, id});
    return true;
  };
  std::map<std::pair<ModelUuid, int>, uint32_t> pieceIds;
  auto pieceId = [&](const ModelUuid& uuid, int joint, uint32_t& id) {
    std::lock_guard<std::mutex> lock(takenMutex);
    const auto known = pieceIds.find({uuid, joint});
    if (known != pieceIds.end()) {
      id = known->second;
      return true;
    }
    id = 0x811C9DC5u ^ uint32_t(joint + 1) * 0x85EBCA77u;
    for (const uint8_t byte : uuid) {
      id = (id ^ byte) * 0x01000193u;
    }
    while (id == 0 || id == 0xFFFFFFFFu || retail.HasId(id) || taken.count(id) != 0) {
      ++id;
    }
    taken.insert(id);
    pieceIds.emplace(std::make_pair(uuid, joint), id);
    GeometryModel& g = geometry.emplace_back();
    g.uuid = uuid;
    g.id = id;
    g.joint = joint;
    return true;
  };
  // A liquid's surface is converted with what its room says of it, so it is a model of its
  // own even where two rooms share the sheet.
  auto liquidId = [&](const RoomLiquid& liquid, uint32_t& id) {
    std::lock_guard<std::mutex> lock(takenMutex);
    id = 0x811C9DC5u ^ uint32_t(liquids.size() + 1) * 0x9E3779B1u;
    for (const uint8_t byte : liquid.model) {
      id = (id ^ byte) * 0x01000193u;
    }
    while (id == 0 || id == 0xFFFFFFFFu || retail.HasId(id) || taken.count(id) != 0) {
      ++id;
    }
    taken.insert(id);
    geometry.push_back({liquid.model, id, int(liquids.size())});
    liquids.push_back(liquid);
    return true;
  };
  auto roomWork = [&] {
    YieldToGame();
    for (size_t i = nextWorld++; i < worlds.size() && !sCancel; i = nextWorld++) {
      RoomPak master;
      std::vector<RoomPak> rooms;
      remastered.World(worlds[i].dir, master, rooms);
      RoomIO io;
      io.retail = [&](uint32_t type, uint32_t id, std::vector<uint8_t>& out) { return retail.Read(type, id, out); };
      io.write = [&](const std::string& name, const std::vector<uint8_t>& data) {
        const bool isGeometry = (name.size() > 8 && name.compare(name.size() - 8, 8, ".roomgeo") == 0) ||
                                (name.size() > 11 && name.compare(name.size() - 11, 11, ".roomliquid") == 0);
        std::ofstream file((isGeometry ? geometryFolder : roomFolder) / PathFromString(name), std::ios::binary);
        file.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        file.close();
        return bool(file);
      };
      io.model = geometryId;
      io.piece = pieceId;
      io.liquid = liquidId;
      io.wantsGeometry = WantsGeometry;
      io.cancelled = [] { return sCancel.load(); };
      // One line a room (what was left out and why): too many for the panel, so the log.
      io.log = [](const std::string& line) {
        static std::mutex logMutex;
        std::lock_guard<std::mutex> lock(logMutex);
        std::printf("remastered import: %s\n", line.c_str());
      };
      int written = 0;
      std::string worldError;
      bool ok = false;
      // As for the models: an exception here would terminate the game.
      try {
        ok = WriteWorldRoomEnvs(worlds[i].mlvl, master, rooms, allPaks, io, written, worldError);
      } catch (const std::exception& e) {
        worldError = e.what();
      }
      if (!ok && !sCancel) {
        AddLine(std::string(worlds[i].dir) + ": " + worldError);
      }
      roomFiles += written;
    }
  };
  workers.clear();
  for (int i = 1; i < threads; ++i) {
    workers.emplace_back(roomWork);
  }
  roomWork();
  for (std::thread& worker : workers) {
    worker.join();
  }
  if (sCancel) {
    fail("Cancelled.");
    return;
  }

  std::atomic<int> geometryDone{0};
  std::atomic<int> lodsDone{0};
  if (!geometry.empty()) {
    // Before any texture takes an id, so none takes a level's.
    for (GeometryModel& g : geometry) {
      if (g.liquid >= 0 || g.joint >= 0) {
        continue;
      }
      for (int level = 1; level < PortRoomGeo::kLodLevels; ++level) {
        uint32_t id = 0x811C9DC5u ^ uint32_t(level) * 0x9E3779B1u;
        for (const uint8_t byte : g.uuid) {
          id = (id ^ byte) * 0x01000193u;
        }
        while (id == 0 || id == 0xFFFFFFFFu || retail.HasId(id) || taken.count(id) != 0) {
          ++id;
        }
        taken.insert(id);
        g.lods[size_t(level)] = id;
      }
    }
    std::unordered_set<uint32_t> modelIds;
    for (const GeometryModel& g : geometry) {
      modelIds.insert(g.id);
      for (int level = 1; level < PortRoomGeo::kLodLevels; ++level) {
        if (g.lods[size_t(level)] != 0) {
          modelIds.insert(g.lods[size_t(level)]);
        }
      }
    }
    std::mutex lodMutex;
    std::vector<PortRoomGeo::Lods> lodTable;
    std::atomic<size_t> nextModel{0};
    std::atomic<int> seen{0};
    std::mutex claimMutex;
    std::unordered_set<uint32_t> claimed;
    auto geometryWork = [&](int worker) {
      YieldToGame();
      ConvertIO io = makeIO(worker, geometryFolder);
      // A texture never takes a geometry model's id either.
      io.retailId = [&](uint32_t id) { return retail.HasId(id) || modelIds.count(id) != 0; };
      io.claim = [&](uint32_t id) {
        std::lock_guard<std::mutex> lock(claimMutex);
        return claimed.insert(id).second;
      };
      Converter converter(std::move(io));
      for (size_t i = nextModel++; i < geometry.size() && !sCancel; i = nextModel++) {
        ConvertOptions options;
        options.retail = geometry[i].id;
        options.standalone = true;
        // The list drops a character's simplified meshes by name; a room has none, and its
        // stone is named "simple".
        options.skip.clear();
        options.nativeMax = kGeometryTexture;
        options.joint = geometry[i].joint;
        if (geometry[i].liquid >= 0 && liquids[size_t(geometry[i].liquid)].type != RoomLiquid::kLava) {
          const RoomLiquid& liquid = liquids[size_t(geometry[i].liquid)];
          options.water = true;
          options.waterHasNormal = liquid.hasNormal;
          options.waterNormal = liquid.normal;
          for (int k = 0; k < 4; ++k) {
            options.waterTint[k] = liquid.tint[k];
          }
          // Each wave layer drifts the way it faces; Remastered's speeds are not read.
          for (int k = 0; k < 2; ++k) {
            const double angle = double(liquid.waveAngle[k]) * (3.14159265358979323846 / 180.0);
            options.waterScale[k] = liquid.normalScale[k];
            options.waterFlow[k * 2] = kLiquidDrift * std::cos(angle);
            options.waterFlow[k * 2 + 1] = kLiquidDrift * std::sin(angle);
          }
        }
        std::string modelError;
        bool ok = false;
        // As for the models: an exception here would terminate the game.
        try {
          std::vector<uint8_t> raw;
          Model model;
          ok = remastered.ReadModel(geometry[i].uuid, raw, modelError) &&
               ParseModel(raw.data(), raw.size(), model, modelError) && converter.Convert(model, options, modelError);
          // Its coarser levels, for the distance; a level that fails ends the list there.
          PortRoomGeo::Lods lods;
          lods.model = geometry[i].id;
          for (const auto& [level, distanceSq] : ok && geometry[i].liquid < 0 && geometry[i].joint < 0
                                                     ? CoarserLevels(model)
                                                                              : std::vector<std::pair<int, float>>()) {
            options.lod = level;
            options.retail = geometry[i].lods[size_t(level)];
            std::string levelError;
            if (!converter.Convert(model, options, levelError)) {
              char name[16];
              std::snprintf(name, sizeof(name), "%08X", geometry[i].id);
              AddLine(std::string("room model ") + name + " level " + std::to_string(level) + ": " + levelError);
              break;
            }
            lods.levels.push_back({distanceSq, options.retail});
          }
          if (!lods.levels.empty()) {
            lodsDone += int(lods.levels.size());
            std::lock_guard<std::mutex> lock(lodMutex);
            lodTable.push_back(std::move(lods));
          }
        } catch (const std::exception& e) {
          modelError = e.what();
        }
        if (ok) {
          ++geometryDone;
        } else {
          char name[16];
          std::snprintf(name, sizeof(name), "%08X", geometry[i].id);
          AddLine(std::string("room model ") + name + ": " + modelError);
        }
        SetMessage("Converting room models (" + std::to_string(++seen) + "/" + std::to_string(geometry.size()) + ")");
      }
    };
    workers.clear();
    for (int i = 1; i < threads; ++i) {
      workers.emplace_back(geometryWork, i);
    }
    geometryWork(0);
    for (std::thread& worker : workers) {
      worker.join();
    }
    if (sCancel) {
      fail("Cancelled.");
      return;
    }
    if (!lodTable.empty()) {
      std::sort(lodTable.begin(), lodTable.end(),
                [](const PortRoomGeo::Lods& a, const PortRoomGeo::Lods& b) { return a.model < b.model; });
      const std::vector<uint8_t> data = PortRoomGeo::WriteLods(lodTable);
      std::ofstream file(geometryFolder / PortRoomGeo::kLodFileName, std::ios::binary);
      file.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
      file.close();
      if (!file) {
        AddLine(std::string(PortRoomGeo::kLodFileName) + ": cannot be written");
      }
    }
  }
  if (geometry.empty()) {
    fs::remove(geometryFolder, ec);
  }

  // The strings Remastered reworded, and its European translations, as the
  // disc's tables with those strings changed and the languages added.
  int textTables = 0;
  int textStrings = 0;
  int textTranslated = 0;
  if (WantsText()) {
    SetMessage("Writing the text");
    std::vector<const char*> languages{kRemasteredEnglish};
    for (size_t k = 0; k < kTextLanguageCount; ++k) {
      languages.push_back(kTextLanguages[k].code);
    }
    std::map<uint32_t, TableText> tables;
    for (const ModelUuid& id : remastered.Texts()) {
      std::vector<uint8_t> raw;
      std::string textError;
      if (!remastered.ReadText(id, raw, textError)) {
        AddLine("text " + IdToString(id) + ": " + textError);
        continue;
      }
      for (const char* language : languages) {
        std::vector<TextEntry> entries;
        if (!ParseMsbt(raw.data(), raw.size(), language, entries, textError)) {
          if (language == kRemasteredEnglish) {
            AddLine("text " + IdToString(id) + ": " + textError);
            break;
          }
          continue;
        }
        for (TextEntry& entry : entries) {
          uint32_t strg = 0;
          uint32_t index = 0;
          std::string name;
          if (SplitTextLabel(entry.label, strg, index)) {
            tables[strg].byIndex[index][language] = std::move(entry.text);
          } else if (SplitNamedLabel(entry.label, strg, name)) {
            tables[strg].byName[name][language] = std::move(entry.text);
          }
        }
      }
    }
    const fs::path textFolder = staging / kTextFolder;
    fs::create_directories(textFolder, ec);
    for (const auto& [strg, strings] : tables) {
      std::vector<uint8_t> original;
      std::vector<uint8_t> merged;
      int reworded = 0;
      int translated = 0;
      if (!retail.Read(kSTRG, strg, original) ||
          !MergeStringTable(original.data(), original.size(), strings, merged, reworded, translated)) {
        continue;  // not on this disc, or worded as it was
      }
      char name[16];
      std::snprintf(name, sizeof(name), "%08X.STRG", strg);
      std::ofstream file(textFolder / name, std::ios::binary);
      file.write(reinterpret_cast<const char*>(merged.data()), std::streamsize(merged.size()));
      file.close();
      if (!file) {
        AddLine(std::string(name) + ": cannot write");
        continue;
      }
      ++textTables;
      textStrings += reworded;
      textTranslated += translated;
    }
    if (textTables == 0) {
      fs::remove(textFolder, ec);
    }
  }
  // Remastered's typeface, which the port draws the disc's text with.
  bool fontWritten = false;
  {
    std::vector<uint8_t> raw;
    std::vector<uint8_t> out;
    ModelUuid atlas{};
    PortHdFont::Font font;
    TxtrImage image;
    std::string fontError = "not in the image";
    // The one with the most characters, so that every language's text is covered.
    for (const ModelUuid& id : remastered.Fonts()) {
      ModelUuid candidateAtlas{};
      PortHdFont::Font candidate;
      std::string candidateError;
      if (!remastered.ReadFont(id, raw, candidateError) ||
          !ParseFont(raw.data(), raw.size(), candidateAtlas, candidate, candidateError)) {
        fontError = candidateError;
      } else if (candidate.glyphs.size() > font.glyphs.size()) {
        font = std::move(candidate);
        atlas = candidateAtlas;
      }
    }
    if (font.glyphs.empty() || !remastered.ReadTexture(atlas, raw, fontError) ||
        !DecodeTxtr(raw.data(), raw.size(), image, fontError)) {
      AddLine("font: " + fontError);
    } else if (!SetFontAtlas(font, image.width, image.height, image.rgba.data(), image.rgba.size()) ||
               !PortHdFont::WriteFont(font, out)) {
      AddLine("font: its texture is not usable");
    } else {
      const fs::path fontFolder = staging / kFontFolder;
      fs::create_directories(fontFolder, ec);
      std::ofstream file(fontFolder / kFontName, std::ios::binary);
      file.write(reinterpret_cast<const char*>(out.data()), std::streamsize(out.size()));
      file.close();
      fontWritten = bool(file);
      if (!fontWritten) {
        AddLine("font: cannot write");
      }
    }
  }
  // Remastered's HUD: the disc's frames laid out and drawn as its own.
  int hudFrames = 0;
  if (WantsHud() && !sCancel) {
    SetMessage("Converting the HUD");
    const fs::path hudFolder = staging / kHudFolder;
    fs::create_directories(hudFolder, ec);
    HudConverter converter(makeIO(0, hudFolder));
    HudCounts counts;
    for (const HudFrame& frame : HudFrames()) {
      std::vector<uint8_t> raw;
      std::vector<uint8_t> rawModel;
      ModelUuid modelId{};
      Model model;
      std::string hudError;
      if (!remastered.ReadFrame(frame.name, raw, hudError) ||
          !(HudFrameModel(raw.data(), raw.size(), modelId) || (hudError = "not a frame", false)) ||
          !remastered.ReadModel(modelId, rawModel, hudError) ||
          !ParseModel(rawModel.data(), rawModel.size(), model, hudError) ||
          !converter.Convert(frame.retail, raw.data(), raw.size(), model, counts, hudError)) {
        AddLine(std::string(frame.name) + ": " + hudError);
        continue;
      }
      ++hudFrames;
    }
    // The map screen's compass, which the game draws itself (port_map_icons.h).
    int compassModels = 0;
    for (const auto& [name, id] : {std::pair<const char*, uint32_t>{"CMDL_MapCompassShell", PortMapIcons::kCompassShell},
                                   std::pair<const char*, uint32_t>{"CMDL_MapCompass", PortMapIcons::kCompassNeedle}}) {
      std::vector<uint8_t> raw;
      Model model;
      std::string compassError;
      if (!remastered.ReadModelNamed(name, raw, compassError) ||
          !ParseModel(raw.data(), raw.size(), model, compassError) ||
          !converter.ConvertModel(model, id, counts, compassError)) {
        AddLine(std::string(name) + ": " + compassError);
        continue;
      }
      ++compassModels;
    }
    if (hudFrames == 0 && compassModels == 0) {
      fs::remove_all(hudFolder, ec);
    }
  }
  // Remastered's map icons, where the game looks for the disc's, and the rooms
  // whose map it reshaped.
  if (WantsHud() && !sCancel) {
    const fs::path mapFolder = staging / kMapFolder;
    fs::create_directories(mapFolder, ec);
    ConvertIO io = makeIO(0, mapFolder);
    int icons = 0;
    for (const MapIcon& icon : MapIcons()) {
      std::vector<uint8_t> raw;
      TxtrImage decoded;
      std::string iconError;
      if (!remastered.ReadTextureNamed(icon.name, raw, iconError) ||
          !DecodeTxtr(raw.data(), raw.size(), decoded, iconError)) {
        AddLine(std::string(icon.name) + ": " + iconError);
        continue;
      }
      Image image;
      image.width = int(decoded.width);
      image.height = int(decoded.height);
      image.rgba = std::move(decoded.rgba);
      char name[32];
      std::snprintf(name, sizeof(name), "%08X.TXTR", icon.id);
      const std::vector<uint8_t> txtr = EncodeMapIcon(image);
      if (txtr.empty() || !io.write(name, txtr)) {
        AddLine(std::string(icon.name) + ": cannot write");
        continue;
      }
      ++icons;
    }
    MapIO mapIO;
    mapIO.retail = io.retail;
    mapIO.write = io.write;
    mapIO.log = [](const std::string& line) { AddLine(line); };
    for (const MapWorld& world : MapWorlds()) {
      if (sCancel) {
        break;
      }
      std::vector<uint8_t> raw;
      std::string mapError;
      if (!remastered.ReadMap(world.name, raw, mapError) ||
          !WriteWorldMapAreas(world.mlvl, raw.data(), raw.size(), mapIO, icons, mapError)) {
        AddLine(std::string(world.name) + ": " + mapError);
      }
    }
    if (icons == 0) {
      fs::remove_all(mapFolder, ec);
    }
  }
  // Remastered's menu movies.
  int movies = 0;
  bool noFfmpeg = false;
  if (MovieFormat format; WantsMovies(format) && !sCancel) {
    movies = ImportMovies(remastered, staging / kMovieFolder, format, noFfmpeg);
  }
  // The Extras gallery's concept art.
  int gallery = 0;
  if (WantsGallery() && !sCancel) {
    gallery = ImportGallery(remastered, staging / kGalleryFolder);
  }
  if (sCancel) {
    fail("Cancelled.");
    return;
  }
  {
    // Only a full import stamps: the movies-only run leaves an older mod's stamp as it was.
    std::ofstream stamp(staging / kImportStampName);
    stamp << kImportVersion << '\n' << MP_BUILD_REVISION << '\n';
    stamp.close();
    if (!stamp) {
      fail("Cannot write to the mod folder.");
      return;
    }
  }
  {
    std::ofstream marker(staging / kMarkerName);
    marker.close();
    if (!marker) {
      fail("Cannot write to the mod folder.");
      return;
    }
  }
  const int failed = int(count) - converted.load();
  std::string message = std::to_string(converted.load()) + " models converted";
  if (failed != 0) {
    message += ", " + std::to_string(failed) + " failed";
  }
  message += ", " + std::to_string(roomFiles.load()) + " room environments";
  if (!geometry.empty()) {
    message += ", " + std::to_string(geometryDone.load()) + " of " + std::to_string(geometry.size()) + " room models (" +
               std::to_string(lodsDone.load()) + " coarser levels)";
  }
  if (textTables != 0) {
    message += ", " + std::to_string(textStrings) + " strings in " + std::to_string(textTables) + " text tables";
    if (textTranslated != 0) {
      message += " (" + std::to_string(textTranslated) + " translated)";
    }
  }
  if (fontWritten) {
    message += ", the font";
  }
  if (hudFrames != 0) {
    message += ", " + std::to_string(hudFrames) + " HUD frames";
  }
  if (movies != 0) {
    message += ", " + std::to_string(movies) + " movies";
  }
  if (gallery != 0) {
    message += ", " + std::to_string(gallery) + " gallery pictures";
  }
  Finish(true, message + (noFfmpeg ? ". Movies skipped: ffmpeg not found." : "."));
}

}  // namespace

std::string DefaultKeysPath() {
  const char* home = std::getenv("HOME");
  if (home == nullptr || home[0] == '\0') {
    home = std::getenv("USERPROFILE");
  }
  if (home == nullptr || home[0] == '\0') {
    return {};
  }
  const fs::path path = PathFromString(home) / ".switch" / "prod.keys";
  std::error_code ec;
  if (!fs::exists(path, ec)) {
    return {};
  }
  const std::u8string text = path.u8string();
  return std::string(text.begin(), text.end());
}

bool StartMovieImport(const std::string& nspPath, const std::string& keysPath) {
  {
    std::lock_guard<std::mutex> lock(sStateMutex);
    if (sState.running) {
      return false;
    }
  }
  if (sThread.joinable()) {
    sThread.join();
  }
  // A finished import that has not been moved into place yet is the newer mod.
  const fs::path staging = StagingFolder();
  std::error_code ec;
  fs::path mod;
  if (!staging.empty()) {
    mod = fs::exists(staging / kMarkerName, ec) ? staging : staging.parent_path() / kImportModName;
  }
  std::lock_guard<std::mutex> lock(sStateMutex);
  sState = {};
  if (mod.empty() || !fs::is_directory(mod, ec)) {
    sState.finished = true;
    sState.message = "Import the models first: the movies go into that mod.";
    return false;
  }
  sCancel = false;
  sState.running = true;
  sState.message = "Starting";
  sThread = std::thread(RunMovies, nspPath, keysPath, mod);
  return true;
}

bool StartImport(const std::string& nspPath, const std::string& keysPath, int threads) {
  {
    std::lock_guard<std::mutex> lock(sStateMutex);
    if (sState.running) {
      return false;
    }
  }
  if (sThread.joinable()) {
    sThread.join();
  }
  const fs::path staging = StagingFolder();
  std::lock_guard<std::mutex> lock(sStateMutex);
  sState = {};
  if (staging.empty()) {
    sState.finished = true;
    sState.message = "There is no mods folder to write to.";
    return false;
  }
  // Each worker holds a whole model, its decoded buffers and a few RGBA
  // textures at once, so the count is bounded by memory as much as by cores.
  if (threads <= 0) {
    threads = std::clamp(int(std::thread::hardware_concurrency()) - 2, 1, 8);
  }
#if defined(__ANDROID__)
  threads = std::min(threads, 3);
#endif
  // The device exists in the game and not on the command line, where this
  // stays at its default of BC.
  bool bc = false, astc = false;
  aurora_get_texture_support(&bc, &astc);
  SetGpuTextureSupport(bc, astc);
  std::fprintf(stderr, "remastered: writing %s textures\n", TextureFormatName());
  sCancel = false;
  sState.running = true;
  sState.message = "Starting";
  sThread = std::thread(Run, nspPath, keysPath, threads, staging);
  return true;
}

void SetImportGeometry(bool on) { sGeometry = on; }

ImportState ImportStatus() {
  std::lock_guard<std::mutex> lock(sStateMutex);
  return sState;
}

void CancelImport() { sCancel = true; }

void StopImport() {
  sCancel = true;
  if (sThread.joinable()) {
    sThread.join();
  }
}

bool ApplyPendingImport() {
  const fs::path staging = StagingFolder();
  std::error_code ec;
  // A running import is still writing there.
  if (staging.empty() || ImportStatus().running || !fs::is_directory(staging, ec)) {
    return false;
  }
  if (!fs::exists(staging / kMarkerName, ec)) {
    fs::remove_all(staging, ec);
    return false;
  }
  const fs::path target = staging.parent_path() / kImportModName;
  // The working mod is kept as a backup until the new one is in place. Hidden, so the mod loader
  // never picks it up as a second mod if a crash leaves it behind.
  const fs::path backup = staging.parent_path() / (std::string(".") + kImportModName + ".old");
  const bool hadTarget = fs::exists(target, ec);
  if (hadTarget) {
    fs::remove_all(backup, ec);
    fs::rename(target, backup, ec);
    if (ec) {
      std::fprintf(stderr, "metroid_prime_port: could not set aside %s: %s\n", target.string().c_str(),
                   ec.message().c_str());
      return false;
    }
  }
  fs::rename(staging, target, ec);
  if (ec) {
    std::fprintf(stderr, "metroid_prime_port: could not move the imported mod to %s: %s\n",
                 target.string().c_str(), ec.message().c_str());
    if (hadTarget) {
      std::error_code restoreError;
      fs::rename(backup, target, restoreError);
      if (restoreError) {
        std::fprintf(stderr, "metroid_prime_port: could not restore %s: %s\n", target.string().c_str(),
                     restoreError.message().c_str());
      }
    }
    return false;
  }
  if (hadTarget) {
    fs::remove_all(backup, ec);
  }
  fs::remove(target / kMarkerName, ec);
  return true;
}

int RunImportFromCommandLine(const std::string& nspPath, const std::string& keysPath, bool moviesOnly) {
  std::string keys = keysPath.empty() ? DefaultKeysPath() : keysPath;
  if (keys.empty()) {
    std::fprintf(stderr, "no key file given and no ~/.switch/prod.keys\n");
    return 2;
  }
  if (moviesOnly ? !StartMovieImport(nspPath, keys)
                 : !StartImport(nspPath, keys, int(std::max(1u, std::thread::hardware_concurrency())))) {
    std::fprintf(stderr, "%s\n", ImportStatus().message.c_str());
    return 1;
  }
  std::string shown;
  for (;;) {
    const ImportState state = ImportStatus();
    if (state.message != shown) {
      shown = state.message;
      std::printf("%s\n", shown.c_str());
      std::fflush(stdout);
    }
    if (!state.running) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  StopImport();
  const ImportState state = ImportStatus();
  for (const std::string& line : state.lines) {
    std::printf("  %s\n", line.c_str());
  }
  if (!state.ok) {
    return 1;
  }
  if (moviesOnly) {
    return 0;
  }
  if (!ApplyPendingImport()) {
    std::fprintf(stderr, "could not move the mod into %s\n", PortMods::Folder().c_str());
    return 1;
  }
  std::printf("installed as %s/%s\n", PortMods::Folder().c_str(), kImportModName);
  return 0;
}

}  // namespace PortRemastered
