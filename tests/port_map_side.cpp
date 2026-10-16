// The seen-pickup / room-mark side file: its format, its file layer and the
// staging the memory card driver goes through (see port_map_side.h). The
// engine's own wiring - the four call sites in CMemoryCardDriver.cpp and the
// directory the files are keyed on - is checked by
// tests/port_map_side_wiring.py, so this test needs no card stack: it provides
// PortMapSide::Location itself.
#include "port_map_side.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace {

namespace fs = std::filesystem;

void Check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    std::abort();
  }
}

fs::path sDirectory;

std::string ReadRaw(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator< char >(in)), std::istreambuf_iterator< char >());
}

bool FileExists(const fs::path& path) {
  std::error_code ec;
  return fs::exists(path, ec);
}

int CountFiles(const fs::path& dir) {
  int count = 0;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(dir, ec)) {
    count += fs::is_regular_file(entry.path(), ec) ? 1 : 0;
  }
  return count;
}

} // namespace

// What platform/port_map_side_paths.cpp does in the game.
namespace PortMapSide {
void Location(std::string& userFolder, std::string& cardBase) {
  userFolder = sDirectory.string() + "/";
  cardBase = "/cards/mine/";
}
} // namespace PortMapSide

int main() {
  using namespace PortMapSide;
  using PortMapMarks::Pack;

  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path root = fs::temp_directory_path(ec) / "port_map_side_tests";
  fs::remove_all(root, ec);
  sDirectory = root;

  uint8_t slot[940];
  for (size_t i = 0; i < sizeof(slot); ++i) {
    slot[i] = static_cast< uint8_t >(i * 7 + 3);
  }
  const uint64_t fp = Fingerprint(slot, sizeof(slot));

  Data in;
  in.seen[0] = 0x80000001u;
  in.seen[3] = 0xDEADBEEFu;
  // More marks than the in-save trailer carries.
  for (unsigned i = 0; i < PortMapMarks::kMaxMarks; ++i) {
    Check(in.marks.Toggle(Pack(i % 7, i)) == 1, "mark added");
  }
  Check(in.marks.Toggle(Pack(7, 127)) == 0 && in.marks.count == 64, "cap of 64");

  // --- the codec -------------------------------------------------------------
  const std::vector< uint8_t > bytes = Encode(fp, in);
  Check(bytes.size() <= kMaxBytes && bytes.size() == kMaxBytes, "bounded");
  Data out;
  Check(Decode(bytes.data(), bytes.size(), fp, out), "round trip");
  Check(out.seen[0] == in.seen[0] && out.seen[3] == in.seen[3] && out.seen[1] == 0, "seen words");
  Check(out.marks.count == 64, "all 64 marks");
  for (uint32_t i = 0; i < 64; ++i) {
    Check(out.marks.packed[i] == in.marks.packed[i], "mark order");
  }

  // The slot changed anywhere: the fingerprint no longer matches.
  slot[500] ^= 1;
  Data stale;
  Check(!Decode(bytes.data(), bytes.size(), Fingerprint(slot, sizeof(slot)), stale) &&
            stale.marks.count == 0,
        "stale fingerprint rejected, data untouched");
  slot[500] ^= 1;

  // Empty data is valid.
  const std::vector< uint8_t > empty = Encode(fp, Data());
  Data e;
  e.marks.Toggle(Pack(1, 1));
  Check(Decode(empty.data(), empty.size(), fp, e) && e.marks.count == 0, "empty replaces");

  // Damage: truncation, extra bytes, bad magic/version, a count above the cap.
  Data d;
  for (size_t cut = 0; cut < bytes.size(); cut += 7) {
    Check(!Decode(bytes.data(), cut, fp, d), "truncated rejected");
  }
  std::vector< uint8_t > longer = bytes;
  longer.push_back(0);
  Check(!Decode(longer.data(), longer.size(), fp, d), "trailing byte rejected");
  std::vector< uint8_t > bad = bytes;
  bad[0] ^= 1;
  Check(!Decode(bad.data(), bad.size(), fp, d), "bad magic");
  bad = bytes;
  bad[4] = 2;
  Check(!Decode(bad.data(), bad.size(), fp, d), "bad version");
  bad = bytes;
  bad[16] = 8; // seen word count
  Check(!Decode(bad.data(), bad.size(), fp, d), "different seen size");
  std::vector< uint8_t > over = Encode(fp, Data());
  over[20] = 65; // mark count word
  over.resize(over.size() + 130, 0);
  Check(!Decode(over.data(), over.size(), fp, d), "count above cap");

  // Marks out of range drop, repeats drop.
  Data dup;
  dup.marks.count = 4;
  dup.marks.packed[0] = dup.marks.packed[1] = Pack(2, 9);
  dup.marks.packed[2] = Pack(3, 1);
  dup.marks.packed[3] = Pack(9, 1); // world 9 does not exist
  const std::vector< uint8_t > dupBytes = Encode(fp, dup);
  Check(Decode(dupBytes.data(), dupBytes.size(), fp, d) && d.marks.count == 2 &&
            d.marks.Has(Pack(3, 1)),
        "repeat and out of range dropped");

  // Cards: the same directory gives the same key, another does not.
  Check(CardKey("/a/card") == CardKey("/a/card") && CardKey("/a/card") != CardKey("/a/card2"),
        "card key");

  // --- the file layer --------------------------------------------------------
  const fs::path file = root / "mapdata" / "file.bin";
  Check(!ReadFile(file, fp, d), "missing file reads false");
  Check(WriteFile(file, fp, in), "write makes the file");
  Check(FileExists(file) && !FileExists(file.string() + ".tmp"), "no temporary left");
  Check(ReadRaw(file).size() == bytes.size(), "written whole");
  Data back;
  Check(ReadFile(file, fp, back) && back.marks.count == 64 && back.seen[3] == in.seen[3],
        "read round trip");
  Check(!ReadFile(file, fp ^ 1, back), "another fingerprint reads false");
  // The second write replaces the first, and the first is not read back.
  Data other;
  other.seen[1] = 7;
  other.marks.Toggle(Pack(6, 2));
  Check(WriteFile(file, fp, other), "write replaces");
  Check(ReadFile(file, fp, back) && back.marks.count == 1 && back.seen[0] == 0, "new content");
  // Every malformed shape the decoder rejects is rejected through the file too.
  {
    std::ofstream garbage(file, std::ios::binary | std::ios::trunc);
    garbage << ReadRaw(file).substr(0, 30) << "extra";
  }
  Check(!ReadFile(file, fp, back), "damaged file reads false");
  Check(back.marks.count == 1, "damaged file leaves the caller's data");
  // A file far longer than the format allows is refused without being read.
  {
    std::vector< char > huge(kMaxBytes * 4, 0);
    std::ofstream big(file, std::ios::binary | std::ios::trunc);
    big.write(huge.data(), static_cast< std::streamsize >(huge.size()));
  }
  Check(!ReadFile(file, fp, back), "oversized file reads false");
  // Errors: a path whose parent is a file cannot be written or removed.
  const fs::path blocked = file / "inside.bin";
  Check(!WriteFile(blocked, fp, in) && !FileExists(blocked), "unwritable path refuses");
  Check(RemoveFile(file) && !FileExists(file), "removed");
  Check(RemoveFile(file), "removing a missing file is fine");
  // A directory that still holds something is not removed.
  const fs::path full = root / "full";
  fs::create_directories(full, ec);
  { std::ofstream keep(full / "keep.bin"); keep << "x"; }
  Check(!RemoveFile(full), "a non-empty directory reports failure");
  fs::remove_all(full, ec);

  // --- the staging the driver goes through ----------------------------------
  Check(Flush(nullptr, 0) == 0, "nothing staged, nothing written");
  const PortMapSide::SlotBytes slots[3] = {{slot, sizeof(slot)}, {nullptr, 0}, {nullptr, 0}};

  // A rebuilt slot, then the card write finished: the file lands under mapdata,
  // keyed on the card (FNV-1a of "/cards/mine/") and the slot.
  const fs::path slot0 = root / "mapdata" / "38fb4584ba12400e-0.bin";
  Stage(0, slot, sizeof(slot), in);
  // The slot the file does not hold writes nothing, and is dropped rather than
  // written by a later write.
  Check(Flush(slots + 1, 2) == 0 && CountFiles(root / "mapdata") == 0,
        "a slot the written file does not hold writes nothing");
  Check(Flush(slots, 3) == 0, "and is dropped, not written later");
  Stage(0, slot, sizeof(slot), in);
  // Bytes that are not the staged ones are refused: the file on the card and
  // the side file would not agree.
  uint8_t otherSlot[940];
  for (size_t i = 0; i < sizeof(otherSlot); ++i) {
    otherSlot[i] = static_cast< uint8_t >(i);
  }
  const PortMapSide::SlotBytes staleSlots[3] = {{otherSlot, sizeof(otherSlot)}, {nullptr, 0},
                                                {nullptr, 0}};
  Check(Flush(staleSlots, 3) == 0 && CountFiles(root / "mapdata") == 0, "stale slot refused");
  Stage(0, slot, sizeof(slot), in);
  Check(Flush(slots, 3) == 1, "staged slot written");
  Check(CountFiles(root / "mapdata") == 1 && !FileExists(slot0.string() + ".tmp"),
        "one file, no temporary");
  Check(FileExists(slot0), "named for the card and the slot");
  Check(Flush(slots, 3) == 0, "flushing again writes nothing");

  // Loading that slot: the side file replaces the game's seen set and marks.
  uint32_t seen[kSeenWords] = {1, 2, 3, 4};
  PortMapMarks::List marks;
  marks.Toggle(Pack(0, 0));
  Check(Apply(0, slot, sizeof(slot), seen, marks), "apply matches");
  Check(seen[0] == in.seen[0] && seen[3] == in.seen[3] && seen[1] == in.seen[1], "seen restored");
  Check(marks.count == 64 && marks.Has(Pack(1, 1)), "marks restored");
  // A slot the file was not written for, or one that changed since: the caller
  // keeps what the save's own trailers gave it.
  uint32_t untouched[kSeenWords] = {5, 6, 7, 8};
  PortMapMarks::List untouchedMarks;
  untouchedMarks.Toggle(Pack(4, 4));
  Check(!Apply(0, otherSlot, sizeof(otherSlot), untouched, untouchedMarks), "stale apply false");
  Check(untouched[0] == 5 && untouchedMarks.count == 1, "a stale file changes nothing");
  Check(!Apply(1, slot, sizeof(slot), untouched, untouchedMarks), "another slot has no file");
  Check(!Apply(2, slot, sizeof(slot), untouched, untouchedMarks), "nor the third");
  // Two slots of one card keep their own files.
  Data second;
  second.seen[2] = 0x1234;
  second.marks.Toggle(Pack(3, 3));
  Stage(1, slot, sizeof(slot), second);
  const PortMapSide::SlotBytes two[3] = {{slot, sizeof(slot)}, {slot, sizeof(slot)}, {nullptr, 0}};
  Check(Flush(two, 3) == 1 && CountFiles(root / "mapdata") == 2, "one file per slot");
  Check(Apply(0, slot, sizeof(slot), seen, marks) && marks.count == 64, "slot 0 unchanged");
  Check(Apply(1, slot, sizeof(slot), seen, marks) && marks.count == 1 && seen[2] == 0x1234,
        "slot 1 own data");

  // Erase: the file goes with the slot.
  StageErase(0);
  Check(Flush(two, 3) == 1 && !FileExists(slot0) && CountFiles(root / "mapdata") == 1,
        "erase deletes the file");
  Check(!Apply(0, slot, sizeof(slot), seen, marks), "erased slot falls back to the save");
  StageErase(0);
  Check(Flush(two, 3) == 1, "erasing a missing file still counts as gone");
  // Written files whose directory cannot be made are not fatal: the save on the
  // card is what matters.
  fs::remove_all(root / "mapdata", ec);
  {
    std::ofstream obstacle(root / "mapdata", std::ios::binary | std::ios::trunc);
    obstacle << "not a directory";
  }
  Stage(0, slot, sizeof(slot), in);
  Check(Flush(slots, 3) == 0, "an unwritable directory reports nothing written");
  Check(!Apply(0, slot, sizeof(slot), seen, marks), "and nothing is readable there");
  fs::remove(root / "mapdata", ec);
  fs::remove_all(root, ec);

  std::puts("port_map_side_tests ok");
  return 0;
}
