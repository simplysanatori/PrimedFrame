#include "port_map_side.h"

#include <cstdio>
#include <string>

#include "port_log.h"

namespace PortMapSide {
namespace {

namespace fs = std::filesystem;

// The port's own text is UTF-8 and std::filesystem's narrow form is not, so a
// folder goes through the u8 form (what PortGci::PathFromString does); keeping
// it here leaves this unit free of the gci code, and so of the card stack.
fs::path FromUtf8(const std::string& text) {
  return fs::path(std::u8string(text.begin(), text.end()));
}

// What one card write should leave behind, held from the moment the slot's
// buffer is rebuilt until the write finished.
struct Staged {
  bool erase = false;
  uint64_t fingerprint = 0;
  Data data;
};

std::map< int, Staged > sStaged;

fs::path SlotPath(int slot) {
  std::string user;
  std::string card;
  Location(user, card);
  if (user.empty()) {
    return {};
  }
  char name[48];
  std::snprintf(name, sizeof(name), "%016llx-%d.bin",
                static_cast< unsigned long long >(CardKey(card.c_str())), slot);
  return FromUtf8(user + "mapdata") / name;
}

} // namespace

bool ReadFile(const fs::path& path, uint64_t fingerprint, Data& data) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return false;
  }
  std::vector< uint8_t > bytes;
  bytes.reserve(kMaxBytes);
  // One byte more than a file of this version can be, so a longer one is
  // rejected without reading all of it.
  char buffer[kMaxBytes + 1];
  while (bytes.size() <= kMaxBytes && in.read(buffer, sizeof(buffer)).gcount() != 0) {
    const std::streamsize got = in.gcount();
    bytes.insert(bytes.end(), buffer, buffer + got);
  }
  Data decoded;
  if (bytes.size() > kMaxBytes || !Decode(bytes.data(), bytes.size(), fingerprint, decoded)) {
    return false;
  }
  data = decoded;
  return true;
}

bool WriteFile(const fs::path& path, uint64_t fingerprint, const Data& data) {
  std::error_code ec;
  if (!path.parent_path().empty()) {
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
      PortLog::Write("map data: couldn't make %s: %s\n", path.parent_path().string().c_str(),
                     ec.message().c_str());
      return false;
    }
  }
  const std::vector< uint8_t > bytes = Encode(fingerprint, data);
  fs::path temp = path;
  temp += ".tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast< const char* >(bytes.data()),
              static_cast< std::streamsize >(bytes.size()));
    // A full disk shows up here or at the close, so flush before checking.
    out.flush();
    if (!out) {
      PortLog::Write("map data: couldn't write %s\n", temp.string().c_str());
      out.close();
      fs::remove(temp, ec);
      return false;
    }
  }
  fs::rename(temp, path, ec);
  if (ec) {
    PortLog::Write("map data: couldn't rename %s: %s\n", temp.string().c_str(),
                   ec.message().c_str());
    fs::remove(temp, ec);
    return false;
  }
  return true;
}

bool RemoveFile(const fs::path& path) {
  std::error_code ec;
  fs::remove(path, ec);
  if (ec) {
    PortLog::Write("map data: couldn't delete %s: %s\n", path.string().c_str(),
                   ec.message().c_str());
    return false;
  }
  return true;
}

#ifdef TARGET_PC
void Stage(int slot, const uint8_t* slotBytes, size_t size, const Data& data) {
  Staged staged;
  staged.fingerprint = Fingerprint(slotBytes, size);
  staged.data = data;
  sStaged[slot] = staged;
}

void StageErase(int slot) {
  Staged staged;
  staged.erase = true;
  sStaged[slot] = staged;
}

int Flush(const SlotBytes* slots, int slotCount) {
  int written = 0;
  for (const auto& entry : sStaged) {
    const int slot = entry.first;
    const fs::path path = SlotPath(slot);
    if (path.empty()) {
      continue;
    }
    if (entry.second.erase) {
      written += RemoveFile(path) ? 1 : 0;
      continue;
    }
    const SlotBytes* inFile = nullptr;
    for (int i = 0; i < slotCount; ++i) {
      if (i == slot && slots[i].bytes != nullptr) {
        inFile = &slots[i];
        break;
      }
    }
    if (inFile == nullptr) {
      // The file written has no such slot (a save that was built and then
      // abandoned, or an erase), so the card does not hold these bytes.
      continue;
    }
    if (Fingerprint(inFile->bytes, inFile->size) != entry.second.fingerprint) {
      PortLog::Write("map data: slot %d was not the save that was staged; leaving it\n", slot);
      continue;
    }
    written += WriteFile(path, entry.second.fingerprint, entry.second.data) ? 1 : 0;
  }
  sStaged.clear();
  return written;
}

bool Apply(int slot, const uint8_t* slotBytes, size_t size, uint32_t* seen,
           PortMapMarks::List& marks) {
  const fs::path path = SlotPath(slot);
  if (path.empty()) {
    return false;
  }
  Data data;
  if (!ReadFile(path, Fingerprint(slotBytes, size), data)) {
    return false;
  }
  for (size_t i = 0; i < kSeenWords; ++i) {
    seen[i] = data.seen[i];
  }
  marks = data.marks;
  return true;
}
#endif

} // namespace PortMapSide
