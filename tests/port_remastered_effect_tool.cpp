// Dev tool for the Remastered particle reader (port_remastered_effect).
//
//   tool dump <file.GENP>            print the parsed effect
//   tool scan <romfs> [outdir]       parse every GENP in every pak under <romfs>
//
// scan reports parse coverage (unique ids and every copy), lists the files that
// do not parse with the offset and FourCC the parse stopped at, and resolves the
// ids the effects refer to against every asset in the paks, by type. With
// <outdir> it also writes one dump per effect, <outdir>/<id>.txt, for diffing
// against retail PART dumps.
//
// Not part of the build:
//   g++ -std=c++20 -O2 -Iplatform/include tests/port_remastered_effect_tool.cpp
//       platform/port_remastered_effect.cpp platform/port_remastered_pak.cpp -lzstd -o effect_tool

#include "port_remastered_effect.h"
#include "port_remastered_pak.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
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

int Scan(const std::string& romfs, const std::string& outDir) {
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

} // namespace

int main(int argc, char** argv) {
  const std::string mode = argc > 1 ? argv[1] : "";
  if (mode == "dump" && argc == 3) {
    return Dump(argv[2]);
  }
  if (mode == "scan" && (argc == 3 || argc == 4)) {
    return Scan(argv[2], argc == 4 ? argv[3] : "");
  }
  std::cerr << "usage: " << argv[0] << " dump <file.GENP> | scan <romfs> [outdir]\n";
  return 2;
}
