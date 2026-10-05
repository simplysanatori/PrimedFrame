// Dev tool for the Remastered particle reader (port_remastered_effect).
//
//   tool dump <file.GENP>            print the parsed effect
//   tool scan <romfs> [outdir]       parse every GENP in every pak under <romfs>
//   tool convert <romfs> <retail> <outdir>
//                                    convert every GENP to retail PART/SWHC/ELSC
//
// scan reports parse coverage (unique ids and every copy), lists the files that
// do not parse with the offset and FourCC the parse stopped at, and resolves the
// ids the effects refer to against every asset in the paks, by type. With
// <outdir> it also writes one dump per effect, <outdir>/<id>.txt, for diffing
// against retail PART dumps.
//
// convert writes <outdir>/<id>.PART for every effect that converts (the
// root under its retail id when it kept one, else under its own id; children
// as <root>-<child>.PART, .SWHC or .ELSC), and a line per effect of what was
// left out. It prints the embedded children found by form and the files
// written by type. With <retail> a folder of the disc's files named
// <8 hex digits>.PART, .SWHC and .ELSC ("-" for none), the splitters are run
// on each of them, and each converted file whose id is a retail one (the root,
// or a child that kept its retail id) is compared with the disc's file
// property by property; the totals per property are printed (prefixed with the
// type for SWHC and ELSC): identical, different, only on the disc, only
// converted.
//
// import runs the import's effect step (port_remastered_effect_import.h) on
// the paks under <romfs> into <outdir>, as the game's import would with
// MP_REMASTERED_EFFECTS=1. A retail id counts as on the disc when <retail>
// holds a file named <8 hex digits>.<type>.
//
// Not part of the build:
//   g++ -std=c++20 -O2 -Iplatform/include tests/port_remastered_effect_tool.cpp
//       platform/port_remastered_effect.cpp platform/port_remastered_effect_convert.cpp
//       platform/port_remastered_effect_import.cpp platform/port_remastered_image.cpp
//       platform/port_remastered_txtr.cpp platform/port_remastered_pak.cpp -lzstd -o effect_tool

#include "port_remastered_effect.h"
#include "port_remastered_effect_convert.h"
#include "port_remastered_effect_import.h"
#include "port_remastered_txtr.h"
#include "port_remastered_pak.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace {

class FileReader {
public:
  bool Open(const std::string& path, std::string& error) {
    m_stream.open(path, std::ios::binary);
    if (!m_stream.is_open()) {
      error = "could not open '" + path + "'";
      return false;
    }
    m_stream.seekg(0, std::ios::end);
    m_size = uint64_t(m_stream.tellg());
    m_stream.seekg(0, std::ios::beg);
    return true;
  }

  uint64_t Size() const { return m_size; }

  bool Read(uint64_t offset, void* out, size_t size) {
    m_stream.clear();
    m_stream.seekg(std::streamoff(offset));
    if (!m_stream) {
      return false;
    }
    m_stream.read(static_cast<char*>(out), std::streamsize(size));
    return m_stream.gcount() == std::streamsize(size);
  }

private:
  std::ifstream m_stream;
  uint64_t m_size = 0;
};

constexpr uint32_t kGenp = PortRemastered::EffectFourCC("GENP");
constexpr uint32_t kMati = PortRemastered::EffectFourCC("MATI");
constexpr uint32_t kTxtr = PortRemastered::EffectFourCC("TXTR");
constexpr uint32_t kPartType = PortRemastered::EffectFourCC("PART");

uint32_t FourCCOf(const std::string& text) {
  uint32_t fourcc = 0;
  for (size_t i = 0; i < 4; ++i) {
    fourcc = fourcc << 8 | uint8_t(i < text.size() ? text[i] : ' ');
  }
  return fourcc;
}

// How many embedded children of each form an effect has, at any depth.
void CountForms(const PortRemastered::EffectNode& node, std::map<uint32_t, size_t>& out) {
  for (const PortRemastered::EffectNode& child : node.children) {
    ++out[child.form];
    CountForms(child, out);
  }
}

// Ids inside an effect are stored as little-endian UUIDs; the pak reader keeps
// asset ids in printed order. Swap the first three groups to look one up.
PortRemastered::EffectGuid PakId(const PortRemastered::EffectGuid& guid) {
  PortRemastered::EffectGuid out = guid;
  std::swap(out[0], out[3]);
  std::swap(out[1], out[2]);
  std::swap(out[4], out[5]);
  std::swap(out[6], out[7]);
  return out;
}

int Dump(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  const std::vector<uint8_t> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  PortRemastered::EffectNode effect;
  std::string error;
  if (!PortRemastered::ParseEffect(data.data(), data.size(), effect, error)) {
    std::cerr << path << ": " << error << "\n";
    return 1;
  }
  std::cout << PortRemastered::DumpEffect(effect, data.data());
  return 0;
}

int Scan(const std::string& romfs, const std::string& outDir, bool rawOut = false) {
  std::vector<std::filesystem::path> paks;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(romfs)) {
    if (entry.is_regular_file() && entry.path().extension() == ".pak") {
      paks.push_back(entry.path());
    }
  }
  std::sort(paks.begin(), paks.end());
  if (!outDir.empty()) {
    std::filesystem::create_directories(outDir);
  }
  if (rawOut) {
    // Every unique GENP as its raw bytes, <id>.GENP, for grepping ids the
    // dump prints as raw blocks (PVAR, the parameter tables).
    std::set<PortRemastered::EffectGuid> written;
    for (const std::filesystem::path& path : paks) {
      FileReader reader;
      std::string error;
      PortRemastered::Pak pak;
      if (!reader.Open(path.string(), error) ||
          !pak.Open([&reader](uint64_t offset, void* out, size_t size) { return reader.Read(offset, out, size); },
                    reader.Size(), error)) {
        std::cerr << path.string() << ": " << error << "\n";
        return 1;
      }
      for (const PortRemastered::PakAsset& asset : pak.Assets()) {
        std::vector<uint8_t> data;
        if (asset.type != kGenp || !written.insert(asset.id).second || !pak.ReadAsset(asset, data, error)) {
          continue;
        }
        std::ofstream out(std::filesystem::path(outDir) / (PortRemastered::IdToString(asset.id) + ".GENP"),
                          std::ios::binary);
        out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
      }
    }
    std::cout << written.size() << " GENPs written\n";
    return 0;
  }

  std::map<PortRemastered::EffectGuid, uint32_t> types; // every asset id -> type
  std::map<PortRemastered::EffectGuid, std::vector<PortRemastered::EffectGuid>> references;
  std::map<PortRemastered::EffectGuid, std::string> failures;
  std::set<PortRemastered::EffectGuid> seen;
  size_t copies = 0;
  size_t copiesParsed = 0;
  size_t parsed = 0;
  for (const std::filesystem::path& path : paks) {
    FileReader reader;
    std::string error;
    PortRemastered::Pak pak;
    if (!reader.Open(path.string(), error) ||
        !pak.Open([&reader](uint64_t offset, void* out, size_t size) { return reader.Read(offset, out, size); },
                  reader.Size(), error)) {
      std::cerr << path.string() << ": " << error << "\n";
      return 1;
    }
    for (const PortRemastered::PakAsset& asset : pak.Assets()) {
      types[asset.id] = asset.type;
      if (asset.type != kGenp) {
        continue;
      }
      copies += 1;
      const bool first = seen.insert(asset.id).second;
      if (!first) {
        copiesParsed += failures.count(asset.id) ? 0 : 1;
        continue;
      }
      std::vector<uint8_t> data;
      if (!pak.ReadAsset(asset, data, error)) {
        std::cerr << path.string() << ": " << error << "\n";
        return 1;
      }
      PortRemastered::EffectNode effect;
      size_t failOffset = 0;
      const std::string name = PortRemastered::IdToString(asset.id);
      if (!PortRemastered::ParseEffect(data.data(), data.size(), effect, error, &failOffset)) {
        char text[64];
        std::snprintf(text, sizeof(text), "0x%zx ", failOffset);
        std::string fourcc = failOffset + 4 <= data.size()
                                 ? std::string(data.begin() + long(failOffset), data.begin() + long(failOffset) + 4)
                                 : std::string("????");
        std::reverse(fourcc.begin(), fourcc.end());
        failures[asset.id] = text + fourcc + (asset.names.empty() ? "" : " " + asset.names.front());
        continue;
      }
      parsed += 1;
      copiesParsed += 1;
      references[asset.id] = PortRemastered::EffectReferences(effect);
      if (!outDir.empty()) {
        std::ofstream out(std::filesystem::path(outDir) / (name + ".txt"));
        if (!asset.names.empty()) {
          out << "# " << asset.names.front() << "\n";
        }
        out << PortRemastered::DumpEffect(effect, data.data());
      }
    }
  }

  std::map<std::string, size_t> referenceTypes;
  std::set<PortRemastered::EffectGuid> instances;
  size_t referenceCount = 0;
  for (const auto& [id, refs] : references) {
    for (const PortRemastered::EffectGuid& ref : refs) {
      auto it = types.find(PakId(ref));
      referenceTypes[it == types.end() ? "(not an asset)" : PortRemastered::EffectFourCCString(it->second)] += 1;
      referenceCount += 1;
      if (it != types.end() && it->second == kMati) {
        instances.insert(it->first);
      }
    }
  }

  // A MATI (material instance) names its MTRL and the textures bound to its
  // samplers. Resolve every 16-byte window of each referenced one against the
  // asset list to see what the effects' materials end up drawing with.
  std::map<std::string, size_t> instanceTypes;
  size_t instancesRead = 0;
  for (const std::filesystem::path& path : paks) {
    if (instances.empty()) {
      break;
    }
    FileReader reader;
    std::string error;
    PortRemastered::Pak pak;
    if (!reader.Open(path.string(), error) ||
        !pak.Open([&reader](uint64_t offset, void* out, size_t size) { return reader.Read(offset, out, size); },
                  reader.Size(), error)) {
      continue;
    }
    for (const PortRemastered::PakAsset& asset : pak.Assets()) {
      std::vector<uint8_t> data;
      if (instances.count(asset.id) == 0 || !pak.ReadAsset(asset, data, error)) {
        continue;
      }
      instances.erase(asset.id);
      instancesRead += 1;
      std::set<uint32_t> found;
      for (size_t at = 0; at + 16 <= data.size(); ++at) {
        PortRemastered::EffectGuid guid;
        std::copy(data.begin() + long(at), data.begin() + long(at) + 16, guid.begin());
        auto it = types.find(PakId(guid));
        if (it != types.end() && it->first != asset.id) {
          found.insert(it->second);
        }
      }
      for (uint32_t type : found) {
        instanceTypes[PortRemastered::EffectFourCCString(type)] += 1;
      }
    }
  }

  std::cout << paks.size() << " paks, " << types.size() << " unique assets\n";
  std::cout << "GENP: " << parsed << "/" << seen.size() << " unique parse, " << copiesParsed << "/" << copies
            << " counting every copy\n";
  std::cout << referenceCount << " ids referenced by parsed effects:\n";
  for (const auto& [type, count] : referenceTypes) {
    std::cout << "  " << type << " " << count << "\n";
  }
  std::cout << instancesRead << " referenced MATI, by the asset types they name:\n";
  for (const auto& [type, count] : instanceTypes) {
    std::cout << "  " << type << " " << count << "\n";
  }
  std::cout << failures.size() << " failures:\n";
  for (const auto& [id, text] : failures) {
    std::cout << "  " << PortRemastered::IdToString(id) << " " << text << "\n";
  }
  return 0;
}


struct PropertyTally {
  size_t same = 0;
  size_t different = 0;
  size_t discOnly = 0;
  size_t convertedOnly = 0;
};

// Error text can carry the raw bytes of a FourCC that is not one.
std::string Printable(std::string text) {
  for (char& c : text) {
    if (c < 0x20 || c > 0x7e) {
      c = '?';
    }
  }
  return text;
}

std::vector<uint8_t> ReadFile(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

int Convert(const std::string& romfs, const std::string& retailDir, const std::string& outDir) {
  using namespace PortRemastered;
  std::vector<std::filesystem::path> paks;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(romfs)) {
    if (entry.is_regular_file() && entry.path().extension() == ".pak") {
      paks.push_back(entry.path());
    }
  }
  std::sort(paks.begin(), paks.end());
  std::filesystem::create_directories(outDir);

  // Every effect and material instance, read once.
  std::map<EffectGuid, uint32_t> types;
  std::map<EffectGuid, std::vector<uint8_t>> effects;
  std::map<EffectGuid, std::vector<uint8_t>> materials;
  std::map<EffectGuid, std::string> names;
  for (const std::filesystem::path& path : paks) {
    FileReader reader;
    std::string error;
    Pak pak;
    if (!reader.Open(path.string(), error) ||
        !pak.Open([&reader](uint64_t offset, void* out, size_t size) { return reader.Read(offset, out, size); },
                  reader.Size(), error)) {
      std::cerr << path.string() << ": " << error << "\n";
      return 1;
    }
    for (const PakAsset& asset : pak.Assets()) {
      types[asset.id] = asset.type;
      auto& store = asset.type == kGenp ? effects : materials;
      if ((asset.type != kGenp && asset.type != kMati) || store.count(asset.id) != 0) {
        continue;
      }
      if (!pak.ReadAsset(asset, store[asset.id], error)) {
        std::cerr << path.string() << ": " << error << "\n";
        return 1;
      }
      if (!asset.names.empty()) {
        names[asset.id] = asset.names.front();
      }
    }
  }

  EffectConvertIO io;
  // The first TXTR a material instance names, when it is one carried over from retail.
  io.materialTexture = [&](const EffectGuid& material) -> uint32_t {
    auto it = materials.find(PakId(material));
    if (it == materials.end()) {
      return 0;
    }
    const std::vector<uint8_t>& data = it->second;
    for (size_t at = 0; at + 16 <= data.size(); ++at) {
      EffectGuid guid;
      std::copy(data.begin() + long(at), data.begin() + long(at) + 16, guid.begin());
      // Ids inside a material instance are stored as they are in an effect.
      auto type = types.find(PakId(guid));
      if (type != types.end() && type->second == kTxtr) {
        return EffectRetailId(guid).value_or(0);
      }
    }
    return 0;
  };

  std::map<std::string, PropertyTally> tally;
  std::map<std::string, size_t> dropReasons;
  std::map<uint32_t, size_t> found;  // embedded children by form
  std::map<std::string, size_t> writtenByType, droppedByType;
  size_t parsed = 0, written = 0, clean = 0, compared = 0, identical = 0, invalid = 0;
  std::ofstream log(std::filesystem::path(outDir) / "convert.txt");
  for (const auto& [id, data] : effects) {
    EffectNode effect;
    std::string error;
    if (!ParseEffect(data.data(), data.size(), effect, error)) {
      continue;
    }
    ++parsed;
    // An asset id in pak order is an effect id with its first groups swapped.
    const EffectGuid effectId = PakId(id);
    const std::optional<uint32_t> retailId = EffectRetailId(effectId);
    char rootName[16];
    std::snprintf(rootName, sizeof(rootName), "%08X", retailId.value_or(0));
    const std::string root = retailId ? std::string(rootName) : IdToString(id);
    CountForms(effect, found);
    const std::vector<ConvertedPart> parts = ConvertEffect(effect, data.data(), io);
    bool allClean = true;
    for (const ConvertedPart& part : parts) {
      const std::string type = EffectFourCCString(part.type);
      std::vector<RetailPartProperty> check;
      if (!SplitRetailEffect(part.type, part.part.data(), part.part.size(), check, error)) {
        ++invalid;
        log << root << " writes a " << type << " retail does not read: " << Printable(error) << "\n";
        continue;
      }
      const std::string file = part.root ? root : root + "-" + EffectGuidString(part.id);
      std::ofstream(std::filesystem::path(outDir) / (file + "." + type), std::ios::binary)
          .write(reinterpret_cast<const char*>(part.part.data()), std::streamsize(part.part.size()));
      ++written;
      ++writtenByType[type];
      droppedByType[type] += size_t(part.droppedRetail);
      allClean = allClean && part.droppedRetail == 0;
      for (const std::string& dropped : part.dropped) {
        log << file << (names.count(id) ? " " + names[id] : "") << " dropped " << Printable(dropped) << "\n";
        dropReasons[(part.type == kPartType ? "" : type + " ") + dropped.substr(0, 4)] += 1;
      }
    }
    clean += allClean ? 1 : 0;

    if (retailDir == "-") {
      continue;
    }
    // Each converted file whose id is a retail one, against the disc's file of
    // that id: the effect's own PART, and children that kept their retail id.
    for (const ConvertedPart& part : parts) {
      const std::optional<uint32_t> discId = part.root ? retailId : EffectRetailId(part.id);
      if (!discId) {
        continue;
      }
      const std::string type = EffectFourCCString(part.type);
      char discName[24];
      std::snprintf(discName, sizeof(discName), "%08X.%s", *discId, type.c_str());
      const std::filesystem::path disc = std::filesystem::path(retailDir) / discName;
      if (!std::filesystem::exists(disc)) {
        continue;
      }
      const std::vector<uint8_t> retail = ReadFile(disc);
      std::vector<RetailPartProperty> want, got;
      if (!SplitRetailEffect(part.type, retail.data(), retail.size(), want, error)) {
        log << discName << " on the disc does not split: " << error << "\n";
        continue;
      }
      // A converted file that does not read is counted above, not compared.
      if (!SplitRetailEffect(part.type, part.part.data(), part.part.size(), got, error)) {
        continue;
      }
      if (!part.root) {
        log << discName << " compared with the child of " << root << "\n";
      }
      ++compared;
      const std::string prefix = part.type == kPartType ? "" : type + " ";
      bool same = want.size() == got.size();
      for (const RetailPartProperty& w : want) {
        const std::string name = prefix + EffectFourCCString(w.fourcc);
        auto g = std::find_if(got.begin(), got.end(), [&](const RetailPartProperty& p) { return p.fourcc == w.fourcc; });
        if (g == got.end()) {
          tally[name].discOnly += 1;
          same = false;
        } else if (g->value == w.value) {
          tally[name].same += 1;
        } else {
          tally[name].different += 1;
          same = false;
        }
      }
      for (const RetailPartProperty& g : got) {
        if (std::none_of(want.begin(), want.end(), [&](const RetailPartProperty& p) { return p.fourcc == g.fourcc; })) {
          tally[prefix + EffectFourCCString(g.fourcc)].convertedOnly += 1;
        }
      }
      identical += same ? 1 : 0;
    }
  }

  std::cout << parsed << " effects parse, " << written << " files written, " << clean
            << " effects with no retail property left out, " << invalid << " files retail would not read\n";
  std::cout << "embedded children, by form (found / written as):\n";
  for (const auto& [form, count] : found) {
    const uint32_t type = EffectRetailType(form);
    std::cout << "  " << EffectFourCCString(form) << " " << count;
    if (type != 0) {
      std::cout << " / " << EffectFourCCString(type);
    }
    std::cout << "\n";
  }
  std::cout << "written, by type (files / retail properties left out):\n";
  for (const auto& [type, count] : writtenByType) {
    std::cout << "  " << type << " " << count << " / " << droppedByType[type] << "\n";
  }
  if (retailDir != "-") {
    // The splitters against every file of their types on the disc.
    std::map<std::string, std::pair<size_t, size_t>> splits;
    for (const auto& entry : std::filesystem::directory_iterator(retailDir)) {
      const std::string ext = entry.path().extension().string();
      if (ext != ".PART" && ext != ".SWHC" && ext != ".ELSC") {
        continue;
      }
      const std::vector<uint8_t> file = ReadFile(entry.path());
      std::vector<RetailPartProperty> split;
      std::string error;
      auto& [all, ok] = splits[ext.substr(1)];
      ++all;
      if (SplitRetailEffect(FourCCOf(ext.substr(1)), file.data(), file.size(), split, error)) {
        ++ok;
      } else {
        log << entry.path().filename().string() << " on the disc does not split: " << error << "\n";
      }
    }
    for (const auto& [type, counts] : splits) {
      std::cout << counts.second << " of the disc's " << counts.first << " " << type << " split\n";
    }
  }
  std::cout << "left out, by property:\n";
  for (const auto& [name, count] : dropReasons) {
    std::cout << "  " << name << " " << count << "\n";
  }
  if (compared != 0) {
    std::cout << compared << " compared with the disc, " << identical << " identical; per property"
              << " (same / different / disc only / converted only):\n";
    for (const auto& [name, t] : tally) {
      std::cout << "  " << name << " " << t.same << " / " << t.different << " / " << t.discOnly << " / "
                << t.convertedOnly << "\n";
    }
  }
  return 0;
}

int Import(const std::string& romfs, const std::string& retailDir, const std::string& outDir) {
  using namespace PortRemastered;
  std::filesystem::create_directories(outDir);
  std::set<uint32_t> disc;
  for (const auto& entry : std::filesystem::directory_iterator(retailDir)) {
    const std::string name = entry.path().filename().string();
    if (name.size() > 9 && name[8] == '.') {
      disc.insert(uint32_t(std::strtoul(name.substr(0, 8).c_str(), nullptr, 16)));
    }
  }
  struct Open {
    FileReader reader;
    Pak pak;
  };
  std::vector<std::unique_ptr<Open>> paks;
  std::map<EffectGuid, std::pair<size_t, size_t>> where;  // pak, asset
  std::map<EffectGuid, uint32_t> types;
  std::vector<std::filesystem::path> paths;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(romfs)) {
    if (entry.is_regular_file() && entry.path().extension() == ".pak") {
      paths.push_back(entry.path());
    }
  }
  std::sort(paths.begin(), paths.end());
  for (const std::filesystem::path& path : paths) {
    auto open = std::make_unique<Open>();
    std::string error;
    FileReader& reader = open->reader;
    if (!reader.Open(path.string(), error) ||
        !open->pak.Open([&reader](uint64_t offset, void* out, size_t size) { return reader.Read(offset, out, size); },
                        reader.Size(), error)) {
      std::cerr << path.string() << ": " << error << "\n";
      return 1;
    }
    const std::vector<PakAsset>& assets = open->pak.Assets();
    for (size_t a = 0; a < assets.size(); ++a) {
      if (types.emplace(assets[a].id, assets[a].type).second) {
        where[assets[a].id] = {paks.size(), a};
      }
    }
    paks.push_back(std::move(open));
  }
  EffectImportIO io;
  for (const auto& [id, type] : types) {
    if (type == kGenp) {
      io.effects.push_back(id);
    }
  }
  auto read = [&](const EffectGuid& id, std::vector<uint8_t>& out, std::string& error) {
    const auto found = where.find(id);
    if (found == where.end()) {
      error = "not in the image";
      return false;
    }
    const Pak& pak = paks[found->second.first]->pak;
    return pak.ReadAsset(pak.Assets()[found->second.second], out, error);
  };
  io.read = [&](uint32_t type, const EffectGuid& id, std::vector<uint8_t>& out, std::string& error) {
    const auto found = types.find(id);
    if (found == types.end() || found->second != type) {
      error = "not in the image";
      return false;
    }
    return read(id, out, error);
  };
  io.typeOf = [&](const EffectGuid& id) -> uint32_t {
    const auto found = types.find(id);
    return found == types.end() ? 0 : found->second;
  };
  io.retailId = [&](uint32_t id) { return disc.count(id) != 0; };
  io.retail = [&](uint32_t type, uint32_t id, std::vector<uint8_t>& out) {
    char name[16];
    std::snprintf(name, sizeof(name), "%08X.%c%c%c%c", id, char(type >> 24), char(type >> 16), char(type >> 8),
                  char(type));
    std::ifstream file(std::filesystem::path(retailDir) / name, std::ios::binary);
    if (!file) {
      return false;
    }
    out.assign(std::istreambuf_iterator<char>(file), {});
    return !out.empty();
  };
  std::set<uint32_t> taken;
  io.freshId = [&](uint32_t seed) {
    uint32_t id = seed;
    while (id == 0 || id == 0xFFFFFFFFu || disc.count(id) != 0 || taken.count(id) != 0) {
      ++id;
    }
    taken.insert(id);
    return id;
  };
  io.texture = [&](const EffectGuid& id, int& width, int& height, std::vector<uint8_t>& rgba, std::string& error) {
    std::vector<uint8_t> raw;
    TxtrImage image;
    if (!read(id, raw, error) || !DecodeTxtr(raw.data(), raw.size(), image, error)) {
      return false;
    }
    width = int(image.width);
    height = int(image.height);
    rgba = std::move(image.rgba);
    return true;
  };
  io.write = [&](const std::string& name, const std::vector<uint8_t>& data) {
    std::ofstream file(std::filesystem::path(outDir) / name, std::ios::binary);
    file.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    return bool(file);
  };
  io.log = [](const std::string& line) { std::cout << "  " << line << "\n"; };
  const EffectImportResult result = ImportEffects(io);
  std::cout << result.written << " of " << result.candidates << " effects written, " << result.failed << " failed, "
            << result.parts << " PARTs, " << result.textures << " textures, " << result.dropped
            << " retail properties left out\n";
  return 0;
}

} // namespace

int main(int argc, char** argv) {
  const std::string mode = argc > 1 ? argv[1] : "";
  if (mode == "dump" && argc == 3) {
    return Dump(argv[2]);
  }
  if (mode == "scan" && (argc == 3 || argc == 4)) {
    return Scan(argv[2], argc == 4 ? argv[3] : "");
  }
  if (mode == "extract" && argc == 4) {
    return Scan(argv[2], argv[3], true);
  }
  if (mode == "convert" && argc == 5) {
    return Convert(argv[2], argv[3], argv[4]);
  }
  if (mode == "import" && argc == 5) {
    return Import(argv[2], argv[3], argv[4]);
  }
  std::cerr << "usage: " << argv[0]
            << " dump <file.GENP> | scan <romfs> [outdir] | extract <romfs> <outdir>"
               " | convert <romfs> <retail|-> <outdir>"
               " | import <romfs> <retail> <outdir>\n";
  return 2;
}
