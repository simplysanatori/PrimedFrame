// "Unlock beam doors once opened": the doorway numbering, the door patch it
// drives and the save trailer (platform/include/port_beam_doors.h).
#include "port_ap_world.h"
#include "port_beam_doors.h"
#include "port_json.h"
#include "port_test.h"

#include <cstdio>
#include <set>
#include <string>
#include <vector>

namespace {

struct Bits {
  std::vector< bool > bits;
  size_t pos = 0;
  void Write(uint32_t value, unsigned n) {
    for (int i = static_cast< int >(n) - 1; i >= 0; --i)
      bits.push_back(((value >> i) & 1) != 0);
  }
  uint32_t Read(unsigned n) {
    uint32_t v = 0;
    for (unsigned i = 0; i < n; ++i) {
      v = (v << 1) | (pos < bits.size() && bits[pos] ? 1u : 0u);
      ++pos;
    }
    return v;
  }
};

// The slot as the game's save code fills it: `before` bits already written (retail
// data, "APX4", "PKSN") out of capacity, then the marks and the beam trailer.
struct Slot {
  Bits bits;
  size_t capacity;
  bool beamWritten = false;
  Slot(size_t before, size_t cap) : capacity(cap) {
    for (size_t i = 0; i < before; ++i)
      bits.bits.push_back(false);
  }
  void Put(const PortMapMarks::List& marks, const uint32_t* beam, bool* skipped = nullptr) {
    beamWritten = PortBeamDoors::PutTail(
        marks, beam, capacity, [this]() { return bits.bits.size(); },
        [this](uint32_t v, unsigned n) { bits.Write(v, n); }, skipped);
  }
  void Get(size_t before, PortMapMarks::List& marks, uint32_t* beam) {
    bits.pos = before;
    PortBeamDoors::GetTail([this](unsigned n) { return bits.Read(n); }, marks, beam);
  }
};

constexpr size_t kUsaSlot = 940 * 8;
constexpr size_t kPalSlot = 0xA7F * 8;
constexpr size_t kRetail = 7084;
constexpr size_t kApx4 = 416;
constexpr size_t kPksn = 160;

PortMapMarks::List Marks(int count) {
  PortMapMarks::List list;
  for (int i = 0; i < count; ++i)
    list.packed[list.count++] = PortMapMarks::Pack(i % 7, i);
  return list;
}

bool SameMarks(const PortMapMarks::List& a, const PortMapMarks::List& b) {
  if (a.count != b.count)
    return false;
  for (uint32_t i = 0; i < a.count; ++i)
    if (a.packed[i] != b.packed[i])
      return false;
  return true;
}

void TestTrailer() {
  using namespace PortBeamDoors;
  uint32_t none[kWords] = {};
  uint32_t some[kWords] = {};
  Set(some, 0);
  Set(some, 37);
  Set(some, 104);
  CHECK_EQ(UsedBits(none), size_t(0));
  CHECK_EQ(UsedBits(some), size_t(105));
  CHECK_MSG(!Test(some, 1) && Test(some, 37) && !Test(some, -1) && !Test(some, 128),
            "bit access stays in range");

  // A game without Archipelago: 276 bits after PKSN.
  const size_t before = kRetail + kPksn;
  for (int marks : {0, 3, 20}) {
    Slot slot(before, kUsaSlot);
    slot.Put(Marks(marks), some);
    CHECK_MSG(slot.beamWritten, "the beam trailer fits a USA slot without Archipelago");
    CHECK_MSG(slot.bits.bits.size() <= kUsaSlot, "nothing is written past the slot");
    PortMapMarks::List read;
    uint32_t back[kWords] = {};
    slot.Get(before, read, back);
    CHECK_MSG(back[0] == some[0] && back[1] == some[1] && back[2] == some[2] && back[3] == some[3],
              "the doorways read back");
    // The marks give way to the trailer, in order, but keep what fits.
    CHECK_MSG(read.count <= size_t(marks) && (marks == 0 || read.count >= 1), "marks keep some room");
    PortMapMarks::List want = Marks(marks);
    want.count = read.count;
    CHECK_MSG(SameMarks(read, want), "the marks that fit read back");
    std::printf("budget: %d marks asked -> %u written, %zu bits used of %zu\n", marks, read.count,
                slot.bits.bits.size(), kUsaSlot);
  }
  // 40 + 105 bits of trailer and 7 marks (37 + 70 + the 32 spare) fill the 276 left.
  {
    Slot slot(before, kUsaSlot);
    slot.Put(Marks(20), some);
    PortMapMarks::List read;
    uint32_t back[kWords] = {};
    slot.Get(before, read, back);
    CHECK_EQ(read.count, 6u);
  }

  // The worst case, every bit up to the last one, still fits without Archipelago.
  uint32_t full[kWords] = {~0u, ~0u, ~0u, ~0u};
  {
    Slot slot(before, kUsaSlot);
    slot.Put(PortMapMarks::List(), full);
    CHECK_MSG(slot.beamWritten, "128 bits fit too");
    CHECK_EQ(slot.bits.bits.size(), before + TrailerBits(128));
  }

  // With the Archipelago trailer the slot is full: skipped whole, marks untouched.
  {
    const size_t apBefore = kRetail + kApx4 + kPksn;
    Slot slot(apBefore, kUsaSlot);
    bool skipped = false;
    slot.Put(Marks(3), some, &skipped);
    CHECK_MSG(!slot.beamWritten && skipped, "a trailer that does not fit is skipped and reported");
    CHECK_EQ(slot.bits.bits.size(), apBefore);
    // Even with room for marks, the skip must not take theirs.
    Slot roomy(before, kUsaSlot);
    uint32_t ap[kWords] = {};
    Set(ap, 120);
    Slot tight(kUsaSlot - 120, kUsaSlot);
    tight.Put(Marks(2), ap, &skipped);
    CHECK_MSG(skipped && !tight.beamWritten, "128 bits do not fit a nearly full slot");
    PortMapMarks::List read;
    uint32_t back[kWords] = {};
    tight.Get(kUsaSlot - 120, read, back);
    CHECK_MSG(read.count == PortMapMarks::Fit(2, kUsaSlot, kUsaSlot - 120) && back[0] == 0 && back[3] == 0,
              "the marks keep their room when the doorways are skipped");
  }

  // PAL: everything fits.
  {
    Slot slot(kRetail + kApx4 + kPksn, kPalSlot);
    slot.Put(Marks(20), some);
    PortMapMarks::List read;
    uint32_t back[kWords] = {};
    slot.Get(kRetail + kApx4 + kPksn, read, back);
    CHECK_MSG(slot.beamWritten && read.count == 20 && back[2] == some[2],
              "a PAL slot holds marks, doorways and the Archipelago trailer");
  }

  // No doorway open: nothing is written, and no skip is reported.
  {
    Slot slot(before, kUsaSlot);
    bool skipped = true;
    slot.Put(Marks(2), none, &skipped);
    CHECK_MSG(!slot.beamWritten && !skipped, "no doorway, no trailer");
  }

  // An old save: the zeros after PKSN, or marks without a beam trailer, read as none.
  {
    Slot old(before, kUsaSlot);
    PortMapMarks::List read;
    uint32_t back[kWords] = {1, 2, 3, 4};
    old.Get(before, read, back);
    CHECK_MSG(read.count == 0 && back[0] == 1, "zeros leave the doorways as they were");
    uint32_t zero[kWords] = {};
    Slot marksOnly(before, kUsaSlot);
    marksOnly.Put(Marks(4), zero);
    uint32_t after[kWords] = {};
    marksOnly.Get(before, read, after);
    CHECK_MSG(read.count == 4 && UsedBits(after) == 0, "marks alone read back, no doorways");
  }
  // The doorways without marks (none fit, or none made).
  {
    Slot slot(before, kUsaSlot);
    slot.Put(PortMapMarks::List(), some);
    PortMapMarks::List read;
    uint32_t back[kWords] = {};
    slot.Get(before, read, back);
    CHECK_MSG(read.count == 0 && back[1] == some[1], "the trailer follows PKSN when there are no marks");
  }
  // Garbage length: nothing.
  {
    Bits bits;
    bits.Write(kMagic, 32);
    bits.Write(255, 8);
    for (int i = 0; i < 300; ++i)
      bits.Write(1, 1);
    PortMapMarks::List read;
    uint32_t back[kWords] = {9, 9, 9, 9};
    GetTail([&bits](unsigned n) { return bits.Read(n); }, read, back);
    CHECK_MSG(UsedBits(back) == 0, "a bad length reads as no doorways");
  }
  // An older build ignores it: its reader stops after the marks.
  {
    Slot slot(before, kUsaSlot);
    slot.Put(Marks(3), some);
    slot.bits.pos = before;
    CHECK_EQ(slot.bits.Read(32), PortMapMarks::kMagic);
    const PortMapMarks::List read = PortMapMarks::Get([&slot](unsigned n) { return slot.bits.Read(n); });
    CHECK_MSG(read.count <= 3, "the old reader reads its marks");
  }
}

void TestNumbering() {
  using namespace PortApWorld;
  const int count = BeamDoorwayCount();
  std::printf("beam doorways: %d\n", count);
  CHECK_MSG(count > 90 && count <= kBeamDoorBits, "about 111 beam doorways, within the bitset");
  CHECK_EQ(count, 111);
  std::set< int > seen;
  int next = 0;
  bool paired = true, inOrder = true;
  int doors = 0;
  for (int bit = 0; bit < count; ++bit) {
    const std::vector< DoorChange > way = BeamDoorway(bit);
    for (const DoorChange& door : way) {
      ++doors;
      CHECK_EQ(BeamDoorwayBit(door.index), bit);
      CHECK_MSG(door.type.empty(), "BeamDoorway leaves the type empty");
      if (door.pair >= 0 && BeamDoorwayBit(door.pair) >= 0)
        paired = paired && BeamDoorwayBit(door.pair) == bit;
    }
    seen.insert(bit);
  }
  CHECK_MSG(paired, "both sides of a doorway share a bit");
  for (int i = 0; i < 600; ++i) {
    const int bit = BeamDoorwayBit(i);
    if (bit < 0)
      continue;
    // A number appears first at its lowest door, so numbers count up in table order.
    if (bit > next)
      inOrder = false;
    if (bit == next)
      ++next;
  }
  CHECK_MSG(inOrder && next == count, "bits count up in table order");
  CHECK_MSG(BeamDoorwayBit(-1) < 0 && BeamDoorwayBit(100000) < 0, "out-of-range doors have none");
  CHECK_MSG(BeamDoorway(-1).empty() && BeamDoorway(count).empty(), "out-of-range doorways are empty");
  CHECK_MSG(doors > count, "most doorways have two doors");
}

PortApWorld::DoorChange* Find(std::vector< PortApWorld::DoorChange >& list, int index) {
  for (PortApWorld::DoorChange& change : list)
    if (change.index == index)
      return &change;
  return nullptr;
}

void TestDoors() {
  using namespace PortApWorld;
  const int count = BeamDoorwayCount();
  uint32_t all[kBeamDoorWords] = {~0u, ~0u, ~0u, ~0u};
  uint32_t none[kBeamDoorWords] = {};
  const Layout disc;

  // Setting off (no bitset), or nothing unlocked: the disc's rooms are untouched.
  int checked = 0;
  for (int bit = 0; bit < count; ++bit) {
    for (const DoorChange& door : BeamDoorway(bit)) {
      CHECK_MSG(Doors(disc, door.mrea).empty(), "no seed, no unlocks: no change");
      CHECK_MSG(Doors(disc, door.mrea, nullptr, none).empty(), "an empty set changes nothing");
      // One doorway unlocked: both sides, and only that doorway.
      uint32_t one[kBeamDoorWords] = {};
      one[bit / 32] |= 1u << (bit % 32);
      std::vector< DoorChange > changes = Doors(disc, door.mrea, nullptr, one);
      DoorChange* mine = Find(changes, door.index);
      CHECK_MSG(mine != nullptr && mine->type == "Blue" && mine->shield.empty() &&
                    mine->doorId == door.doorId && mine->forces[0] == door.forces[0],
                "an unlocked beam door is a plain blue one");
      for (const DoorChange& other : changes)
        CHECK_MSG(BeamDoorwayBit(other.index) == bit && other.type == "Blue",
                  "only the unlocked doorway changes");
      for (const DoorChange& side : BeamDoorway(bit)) {
        std::vector< DoorChange > there = Doors(disc, side.mrea, nullptr, one);
        CHECK_MSG(Find(there, side.index) != nullptr, "both sides turn blue");
      }
      ++checked;
    }
  }
  CHECK_MSG(checked > count, "every beam door was tried");

  // A seed's colours: the unlocked door is blue whatever the seed made it, but a
  // door the seed made Bomb or Missile (not a beam lock) is left alone.
  PortJson::Value data;
  size_t offset = 0;
  const char* reason = nullptr;
  CHECK_MSG(PortJson::Parse(R"({"door_color_randomization":1,"door_color_mapping":{
      "Chozo Ruins":{"area":"Chozo Ruins","type_mapping":{"Blue":"Ice Beam","Wave Beam":"Bomb",
      "Ice Beam":"Missile","Plasma Beam":"Wave Beam","Power Beam Only":"Plasma Beam"}}}})",
                            data, offset, &reason),
            "the colour fixture parses");
  Layout colours;
  Parse(data, colours);
  int bombed = 0, recoloured = 0, blueNow = 0;
  for (int bit = 0; bit < count; ++bit) {
    for (const DoorChange& door : BeamDoorway(bit)) {
      std::vector< DoorChange > plain = Doors(colours, door.mrea);
      std::vector< DoorChange > open = Doors(colours, door.mrea, nullptr, all);
      DoorChange* before = Find(plain, door.index);
      DoorChange* after = Find(open, door.index);
      CHECK_MSG(after != nullptr, "an unlocked door is listed");
      if (after == nullptr)
        continue;
      if (before != nullptr && before->type == "Blue")
        ++blueNow;
      if (before != nullptr && (before->type == "Bomb" || before->type == "Missile")) {
        CHECK_MSG(after->type == before->type, "a Bomb or Missile door stays as the seed made it");
        ++bombed;
      } else {
        CHECK_MSG(after->type == "Blue", "an unlocked beam door is blue under a seed too");
        if (before != nullptr && before->type != "Blue")
          ++recoloured;
      }
    }
  }
  CHECK_MSG(bombed > 0, "the seed fixture reaches a Bomb or Missile door");
  // Unlockable: the seed's lock decides.
  int unlockable = 0, refused = 0;
  for (int bit = 0; bit < count; ++bit) {
    for (const DoorChange& door : BeamDoorway(bit)) {
      CHECK_MSG(BeamDoorUnlockable(disc, door.mrea, door.index), "a disc beam door can be unlocked");
      const bool ok = BeamDoorUnlockable(colours, door.mrea, door.index);
      (ok ? unlockable : refused) += 1;
    }
  }
  CHECK_MSG(unlockable > 0 && refused == bombed + blueNow, "a Bomb, Missile or already-blue door is refused");
  CHECK_MSG(!BeamDoorUnlockable(disc, 0x18AB6106u, -1) && !BeamDoorUnlockable(disc, 0, 100000),
            "a door without a doorway is refused");

  CHECK_MSG(MapDoors(disc, 0x12345678u, nullptr, all).empty(), "no map area, no icons");

  DoorStyle style;
  CHECK_MSG(BlueDoorStyle(style) && style.cmdl == 0x0734977Au && style.cmdlVertical == 0x18D0AEE6u &&
                style.pattern0 == 0x544A9892u && style.color == 0x8A7F3683u,
            "the blue style is the disc's blue door");
  CHECK_MSG(BlueDoorVulnerability().size() == 4 + 15 * 4 + 4 + 4 + 5 * 4 + 4 + 5 * 4,
            "the blue weaknesses are a whole vulnerability");
}

} // namespace

int main() {
  TestTrailer();
  TestNumbering();
  TestDoors();
  return PORT_TEST_RESULT();
}
