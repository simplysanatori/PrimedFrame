#pragma once

#include <cstddef>
#include <cstdint>

// Room marks on the map: the player flags a room to come back to (X on the
// world map screen), and the map and minimap tint it. They are kept per save
// as the "RMMK" trailer after the "PKSN" one (CGameState::PutTo/ctor): the
// magic, a 5-bit count and one 10-bit entry per mark, a 3-bit world index (in
// gpMemoryCard->GetMemoryWorlds() order; there are 7 worlds) and a 7-bit area
// index (the largest world has 64 rooms in tools' tables, so < 128). Written
// only when there is a mark and the whole trailer, with kSpareBits left over,
// fits the slot, so a full slot never gets a cut trailer.
//
// Slot budget (USA, 940 bytes = 7520 bits; measured with
// COutputStream::PortBitsWritten): the retail data is 7084 bits, the "APX4"
// trailer 416, "PKSN" 160. Without Archipelago that leaves 276 bits after
// PKSN: 276 - 32 spare - 37 header = 207 bits = 20 marks (kMaxMarks). With an
// Archipelago trailer only 20 bits are left, PKSN is already cut and no mark
// fits, so marks of such a game live for the session only. The PAL slot (2687
// bytes) holds every case.
namespace PortMapMarks {

constexpr uint32_t kMagic = 0x524D4D4B; // "RMMK"
constexpr unsigned kWorldBits = 3;
constexpr unsigned kAreaBits = 7;
constexpr unsigned kCountBits = 5;
constexpr size_t kMarkBits = kWorldBits + kAreaBits;
constexpr size_t kHeaderBits = 32 + kCountBits;
// Bits of the slot left unwritten after the trailer (4 bytes).
constexpr size_t kSpareBits = 32;
constexpr size_t kMaxMarks = 20;
static_assert(kMaxMarks < (1u << kCountBits), "the count field holds kMaxMarks");

constexpr uint16_t Pack(unsigned world, unsigned area) {
  return static_cast< uint16_t >((world << kAreaBits) | area);
}
constexpr unsigned WorldOf(uint16_t mark) { return mark >> kAreaBits; }
constexpr unsigned AreaOf(uint16_t mark) { return mark & ((1u << kAreaBits) - 1); }
constexpr bool Valid(unsigned world, unsigned area) {
  return world < (1u << kWorldBits) && area < (1u << kAreaBits);
}

// The marks of one save: count entries of Pack().
struct List {
  uint32_t count = 0;
  uint16_t packed[32] = {};

  bool Has(uint16_t mark) const {
    for (uint32_t i = 0; i < count; ++i) {
      if (packed[i] == mark) {
        return true;
      }
    }
    return false;
  }
  // +1 added, -1 removed, 0 the list is full (nothing changed).
  int Toggle(uint16_t mark) {
    for (uint32_t i = 0; i < count; ++i) {
      if (packed[i] == mark) {
        for (uint32_t j = i + 1; j < count; ++j) {
          packed[j - 1] = packed[j];
        }
        --count;
        return -1;
      }
    }
    if (count >= kMaxMarks) {
      return 0;
    }
    packed[count++] = mark;
    return 1;
  }
};

// How many of count marks the trailer can carry into a stream of capacityBits
// that has written usedBits, keeping kSpareBits free; 0 when even one does not
// fit (then nothing is written).
inline size_t Fit(size_t count, size_t capacityBits, size_t usedBits) {
  if (count == 0 || capacityBits < usedBits + kSpareBits + kHeaderBits + kMarkBits) {
    return 0;
  }
  const size_t room = (capacityBits - usedBits - kSpareBits - kHeaderBits) / kMarkBits;
  return count < room ? count : room;
}

// write(value, bits) puts bits of value, most significant first.
template < typename Write >
void Put(const List& list, size_t count, Write&& write) {
  write(kMagic, 32);
  write(static_cast< uint32_t >(count), kCountBits);
  for (size_t i = 0; i < count; ++i) {
    write(list.packed[i], kMarkBits);
  }
}

// After the magic matched: read(bits) returns the next bits. Entries out of
// range or repeated are dropped, and a count above kMaxMarks (a trailer cut
// short or garbage) yields an empty list.
template < typename Read >
List Get(Read&& read) {
  List list;
  const uint32_t count = read(kCountBits);
  if (count > kMaxMarks) {
    return list;
  }
  for (uint32_t i = 0; i < count; ++i) {
    const uint16_t mark = static_cast< uint16_t >(read(kMarkBits));
    if (!list.Has(mark)) {
      list.packed[list.count++] = mark;
    }
  }
  return list;
}

// The current game's list, by world asset id and area index; false (nothing
// marked) outside a game or for an unknown world.
bool IsMarked(uint32_t worldId, int area);
// +1 marked, -1 unmarked, 0 not possible (list full, unknown world or area).
int Toggle(uint32_t worldId, int area);
void Clear();
const List* Current();
// The world's index in gpMemoryCard->GetMemoryWorlds() order.
bool WorldIndex(uint32_t worldId, unsigned& index);

} // namespace PortMapMarks
