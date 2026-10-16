#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <system_error>
#include <vector>

#include "port_map_marks.h"
#include "port_map_pickups.h"

// The seen pickups and room marks of a save slot, kept in a small file in the
// port's user folder instead of only in the slot: a USA slot has no room for
// them next to an Archipelago trailer (the in-save "PKSN"/"RMMK" trailers are
// cut or dropped there), and even without one the "RMMK" trailer carries at
// most PortMapMarks::kTrailerMarks marks. One file per slot of the memory card
// in use: <user folder>/mapdata/<card hash>-<slot>.bin, the hash being of the
// card's directory (the port's own card, a shared emulator card, or an
// Archipelago game's own card - each gets theirs, and the data never goes into
// the emulator's folder). The two come from port_map_side_paths.cpp.
//
// Layout, little endian: "MPMD", version 1, a 64-bit fingerprint, the seen
// word count and the words, the mark count and the 16-bit packed marks. The
// whole file is at most kMaxBytes.
//
// The fingerprint is FNV-1a over the whole slot buffer as written to the card
// (the bytes SGameFileSlot::PutTo writes, trailers included): the file is only
// used when the slot read back hashes the same, so a save replaced elsewhere
// (Dolphin, import, conversion, an older port build) ignores stale data and
// falls back to the slot's own trailers. The trailers are inside it because
// those are exactly the bytes between write and read; the in-save bytes change
// whenever the game state does, so the hash also follows the progress.
namespace PortMapSide {

constexpr uint32_t kMagic = 0x444D504D; // "MPMD" read as a little endian word
constexpr uint32_t kVersion = 1;
constexpr size_t kSeenWords = PortMapPickups::kSeenWords;
// The header (magic, version, fingerprint, seen count, mark count) and the
// longest body the version can hold; a file longer than this is not ours.
constexpr size_t kHeaderBytes = 24;
constexpr size_t kMaxBytes = kHeaderBytes + kSeenWords * 4 + PortMapMarks::kMaxMarks * 2;

struct Data {
  uint32_t seen[kSeenWords] = {};
  PortMapMarks::List marks;
};

inline uint64_t Fingerprint(const uint8_t* bytes, size_t size) {
  uint64_t hash = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < size; ++i) {
    hash = (hash ^ bytes[i]) * 0x100000001b3ull;
  }
  return hash;
}

inline void PutU32(std::vector< uint8_t >& out, uint32_t v) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(static_cast< uint8_t >(v >> (8 * i)));
  }
}

inline std::vector< uint8_t > Encode(uint64_t fingerprint, const Data& data) {
  std::vector< uint8_t > out;
  out.reserve(kMaxBytes);
  PutU32(out, kMagic);
  PutU32(out, kVersion);
  PutU32(out, static_cast< uint32_t >(fingerprint));
  PutU32(out, static_cast< uint32_t >(fingerprint >> 32));
  PutU32(out, static_cast< uint32_t >(kSeenWords));
  for (size_t i = 0; i < kSeenWords; ++i) {
    PutU32(out, data.seen[i]);
  }
  PutU32(out, data.marks.count);
  for (uint32_t i = 0; i < data.marks.count; ++i) {
    out.push_back(static_cast< uint8_t >(data.marks.packed[i]));
    out.push_back(static_cast< uint8_t >(data.marks.packed[i] >> 8));
  }
  return out;
}

// False (and data untouched) for a wrong magic/version, another fingerprint,
// a different seen size, a mark count above the cap or a truncated or longer
// file. Marks out of range or repeated are dropped.
inline bool Decode(const uint8_t* bytes, size_t size, uint64_t fingerprint, Data& data) {
  size_t pos = 0;
  bool ok = true;
  auto u32 = [&]() -> uint32_t {
    if (pos + 4 > size) {
      ok = false;
      return 0;
    }
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
      v |= static_cast< uint32_t >(bytes[pos + i]) << (8 * i);
    }
    pos += 4;
    return v;
  };
  Data result;
  if (u32() != kMagic || u32() != kVersion) {
    return false;
  }
  const uint64_t lo = u32();
  const uint64_t hi = u32();
  if (!ok || (lo | (hi << 32)) != fingerprint || u32() != kSeenWords) {
    return false;
  }
  for (size_t i = 0; i < kSeenWords; ++i) {
    result.seen[i] = u32();
  }
  const uint32_t count = u32();
  if (!ok || count > PortMapMarks::kMaxMarks || pos + size_t(count) * 2 != size) {
    return false;
  }
  for (uint32_t i = 0; i < count; ++i) {
    const uint16_t mark = static_cast< uint16_t >(bytes[pos] | (bytes[pos + 1] << 8));
    pos += 2;
    if (PortMapMarks::Valid(PortMapMarks::WorldOf(mark), PortMapMarks::AreaOf(mark)) &&
        !result.marks.Has(mark)) {
      result.marks.packed[result.marks.count++] = mark;
    }
  }
  data = result;
  return true;
}

// The file name part for a card: the hash of its directory, 16 hex digits.
inline uint64_t CardKey(const char* directory) {
  size_t n = 0;
  while (directory[n] != '\0') {
    ++n;
  }
  return Fingerprint(reinterpret_cast< const uint8_t* >(directory), n);
}

// The file of one slot, read whole: false (and data untouched) when it is
// missing, unreadable, longer than kMaxBytes or does not match the fingerprint.
bool ReadFile(const std::filesystem::path& path, uint64_t fingerprint, Data& data);

// Writes one file whole, through a temporary beside it and a rename, so a
// crash or a full disk leaves the previous file (or none) in place. False
// (and no file) when the directory or the file could not be written; the
// temporary is removed either way.
bool WriteFile(const std::filesystem::path& path, uint64_t fingerprint, const Data& data);

// Removes the file of one slot; true when it is gone (also when it never was).
bool RemoveFile(const std::filesystem::path& path);

// Where the files go: the port's user folder and the card directory whose path
// keys them, under <user folder>/mapdata. Resolved in
// platform/port_map_side_paths.cpp from PortPaths::UserFolder() and
// CMemoryCardSys::PortCardBase() (nothing else implements this, so the unit
// test resolves its own directory without the engine).
void Location(std::string& userFolder, std::string& cardBase);

#ifdef TARGET_PC
// A slot of the file the memory card driver just wrote, as SGameFileSlot holds
// it: bytes null when the file has no such slot.
struct SlotBytes {
  const uint8_t* bytes = nullptr;
  size_t size = 0;
};

// Called where a slot's buffer is rebuilt from the live game (not yet on the
// card): remembers what the file should say for that slot.
void Stage(int slot, const uint8_t* slotBytes, size_t size, const Data& data);
// The slot is being erased from the card.
void StageErase(int slot);
// After the card write finished and the file was renamed into place: writes (or
// deletes) the file of each staged slot and drops the staging. A slot whose
// bytes are not the ones that were staged is left alone (an erase staged for it
// still deletes the file); a slot the written file does not hold is dropped, so
// a save that was built and then abandoned writes nothing. Returns how many
// files were written or removed. Never call this for a write that failed.
int Flush(const SlotBytes* slots, int slotCount);
// After a slot was read back from the card and loaded into the live game:
// replaces the game's seen set and marks with the file's when it matches.
bool Apply(int slot, const uint8_t* slotBytes, size_t size, uint32_t* seen,
           PortMapMarks::List& marks);
#endif

} // namespace PortMapSide
