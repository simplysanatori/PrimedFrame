#pragma once

#include <cstddef>
#include <cstdint>

#include "port_map_marks.h"

// "Unlock beam doors once opened" (setting beam_doors_stay_unlocked): a door
// locked to the Wave, Ice, Plasma or Power Beam stays a plain blue one, on both
// sides, once its force field was broken. Which doorways are open is one bit
// per beam doorway of the disc (PortApWorld::BeamDoorwayBit), kept per save in
// CGameState and written as the "BDUL" trailer, the last one after the "RMMK"
// room marks (CGameState::PutTo/ctor): the magic, an 8-bit length n and n
// bits, bit 0 first, n being the highest set bit + 1 (so a game with few open
// doors writes few bits). An older build reads past the trailer, as it does
// for any magic it doesn't know.
//
// Slot budget (USA, 940 bytes = 7520 bits; measured with
// COutputStream::PortBitsWritten): retail 7084 bits, "APX4" 416, "PKSN" 160. A
// game without Archipelago has 276 bits left after PKSN; the trailer takes
// 40 + n (at most 168), and the room marks, which keep what is left, give way:
// they are fitted into the slot minus this trailer. With an Archipelago
// trailer the slot is already full: nothing fits, the trailer is skipped and
// the unlocks are lost on a save to the card (they live on in the running
// game and in savestates). The PAL slot (2687 bytes) holds every case. A
// trailer that does not fit is skipped whole, never cut, and logged once.
class CStateManager;

namespace PortBeamDoors {

constexpr uint32_t kMagic = 0x4244554C; // "BDUL"
constexpr unsigned kLengthBits = 8;
constexpr int kWords = 4;
constexpr size_t kMaxBits = kWords * 32;
static_assert(kMaxBits < (1u << kLengthBits), "the length field holds kMaxBits");

inline bool Test(const uint32_t* words, int bit) {
  return bit >= 0 && static_cast< size_t >(bit) < kMaxBits && ((words[bit / 32] >> (bit % 32)) & 1);
}

inline void Set(uint32_t* words, int bit) {
  if (bit >= 0 && static_cast< size_t >(bit) < kMaxBits)
    words[bit / 32] |= 1u << (bit % 32);
}

// Highest set bit + 1; 0 when none is set.
inline size_t UsedBits(const uint32_t* words) {
  for (size_t bit = kMaxBits; bit > 0; --bit) {
    if (Test(words, static_cast< int >(bit - 1)))
      return bit;
  }
  return 0;
}

inline size_t TrailerBits(size_t used) { return 32 + kLengthBits + used; }

// Whether the trailer for `used` bits fits a stream of capacityBits that has
// written writtenBits. False for no bits (nothing to write).
inline bool Fits(size_t used, size_t capacityBits, size_t writtenBits) {
  return used != 0 && capacityBits >= writtenBits && capacityBits - writtenBits >= TrailerBits(used);
}

// write(value, bits) puts bits of value, most significant first.
template < typename Write >
void Put(const uint32_t* words, Write&& write) {
  const size_t used = UsedBits(words);
  write(kMagic, 32);
  write(static_cast< uint32_t >(used), kLengthBits);
  for (size_t bit = 0; bit < used; ++bit)
    write(Test(words, static_cast< int >(bit)) ? 1u : 0u, 1);
}

// After the magic matched: read(bits) returns the next bits. A length above
// kMaxBits (garbage) yields no bits at all.
template < typename Read >
void Get(Read&& read, uint32_t* words) {
  for (int i = 0; i < kWords; ++i)
    words[i] = 0;
  const uint32_t used = read(kLengthBits);
  if (used > kMaxBits)
    return;
  for (uint32_t bit = 0; bit < used; ++bit) {
    if (read(1) != 0)
      Set(words, static_cast< int >(bit));
  }
}

// What follows the "PKSN" trailer in a save slot, in one place for the game's
// save code and its tests: the "RMMK" room marks, then this trailer, each
// optional (an older save has neither, a full slot may have only one).
//
// PutTail: written(), the stream's bits so far, and write(value, bits). The
// beam doorways are placed first and the marks fitted into what they leave; a
// beam trailer that cannot fit is skipped whole (*skipped says so) and the
// marks keep their room. Returns whether the beam trailer was written.
template < typename Write, typename Written >
bool PutTail(const PortMapMarks::List& marks, const uint32_t* beam, size_t capacityBits,
             Written&& written, Write&& write, bool* skipped = nullptr) {
  const size_t used = UsedBits(beam);
  const bool fits = Fits(used, capacityBits, written());
  if (skipped != nullptr)
    *skipped = used != 0 && !fits;
  const size_t reserve = fits ? TrailerBits(used) : 0;
  const size_t fit =
      PortMapMarks::Fit(marks.count, capacityBits > reserve ? capacityBits - reserve : 0, written());
  if (fit != 0)
    PortMapMarks::Put(marks, fit, write);
  if (fits && Fits(used, capacityBits, written())) {
    Put(beam, write);
    return true;
  }
  return false;
}

// read(bits) returns the next bits. `marks` and `beam` are left as they are
// (empty) for a trailer the slot doesn't have.
template < typename Read >
void GetTail(Read&& read, PortMapMarks::List& marks, uint32_t* beam) {
  uint32_t next = read(32);
  if (next == PortMapMarks::kMagic) {
    marks = PortMapMarks::Get(read);
    next = read(32);
  }
  if (next == kMagic)
    Get(read, beam);
}

// ---- The running game (port_beam_doors.cpp; not in the unit tests) ----

// The setting.
bool Enabled();
// The current game's open doorways copied into `copy` (kWords words) when the
// setting is on and a game is loaded, else nullptr. For PortApWorld::Doors.
const uint32_t* Unlocked(uint32_t* copy);
// A damageable trigger of area `mrea` with this editor id went down: when the
// setting is on and it is a beam door's force field, record the doorway and
// restyle it everywhere it is loaded.
void OnForceBroken(CStateManager& mgr, uint32_t mrea, uint32_t editorId);
// Marks doorway `bit` open and restyles its loaded doors; false when it was
// already open, the bit is out of range or no game is loaded.
bool Unlock(CStateManager& mgr, int bit);
// Closes every doorway again (keeps nothing); false outside a game.
bool Reset();

} // namespace PortBeamDoors
