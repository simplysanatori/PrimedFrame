"""Production wiring guards for the map side file (port_map_side.h).

The seen pickups and the room marks of a save live in the card slot's own
"PKSN"/"RMMK" trailers where they fit, and in a small side file where they do
not: a USA slot with an Archipelago trailer holds room for PKSN alone, and the
"RMMK" trailer carries 20 marks of the 64 the save's list keeps. The side file
is keyed on the card's directory and holds a fingerprint of the exact slot
bytes, so it is only used for the save it was written for.

What this checks, none of which any unit test can see:

  * the four call sites in CMemoryCardDriver.cpp - a slot's buffer being rebuilt
    from the live game (Stage), a finished card write (Flush), a slot being read
    back and loaded (Apply) and a slot being erased (StageErase);
  * Flush is called from the success path alone: never from a write that failed,
    which would leave the side file ahead of the card;
  * the trailer keeps the fields the first build wrote (5-bit count, cap 20) so
    an older build reads what this one writes, and the side file carries 64;
  * the directory is the port's user folder and the card in use - never the
    emulator's folder or the card itself - and the save state carries the same
    data in the blob's tail, so a state round trip does not shorten the list.

It is a source check: no engine code runs and no card is touched.
"""
import pathlib
import sys
import unittest

root = pathlib.Path(sys.argv.pop())
DRIVER = root / "src/MetroidPrime/CMemoryCardDriver.cpp"
PATHS = root / "platform/port_map_side_paths.cpp"
SIDE = root / "platform/port_map_side.cpp"
HEADER = root / "platform/include/port_map_side.h"
MARKS = root / "platform/include/port_map_marks.h"
SAVESTATE = root / "platform/port_savestate.cpp"
CMAKE = root / "CMakeLists.txt"


def between(text, start, end, path):
    assert start in text, f"{path}: no {start!r}"
    body = text.split(start, 1)[1]
    assert end in body, f"{path}: no {end!r} after {start!r}"
    return body.split(end, 1)[0]


class Wiring(unittest.TestCase):
    def test_slot_stages_on_rebuild_and_applies_on_load(self):
        driver = DRIVER.read_text()
        stage = between(driver, "void SGameFileSlot::InitializeFromGameState() {",
                        "\nvoid ", DRIVER)
        self.assertIn("PortMapSide::Stage(", stage)
        # The fingerprint must be over the buffer SGameFileSlot::PutTo writes,
        # not a copy of it: the read side hashes what came back from the card.
        self.assertIn("x0_saveBuffer.data(), x0_saveBuffer.size(), data", stage)
        apply = between(driver, "void SGameFileSlot::LoadGameState(int idx) {",
                        "\nconst CGameState::GameFileStateInfo*", DRIVER)
        self.assertIn("gpMain->StreamNewGameState(r, idx);", apply)
        self.assertIn("PortMapSide::Apply(idx, x0_saveBuffer.data(), x0_saveBuffer.size()", apply)

    def test_erase_and_flush_are_the_side_of_the_card_write(self):
        driver = DRIVER.read_text()
        erase = between(driver, "void CMemoryCardDriver::EraseFileSlot(int saveIdx) {", "\nvoid ",
                        DRIVER)
        self.assertIn("xe4_fileSlots[saveIdx] = nullptr;", erase)
        self.assertIn("PortMapSide::StageErase(saveIdx);", erase)
        write = between(driver, "void CMemoryCardDriver::WriteBackupBuf() {", "\nvoid ", DRIVER)
        self.assertIn("PortMapSide::Flush(", write)
        # Every slot the file just written holds, so a save that was built and
        # then abandoned leaves nothing behind.
        self.assertIn("x0_saveBuffer.data()", write)
        # Flush is only reached once the write finished: WriteBackupBuf is
        # called from the two completion paths, and from no failure handler.
        self.assertEqual(driver.count("\n    WriteBackupBuf();"), 2, "one call per completion path")
        for failed in ("UpdateFileWrite(ECardResult result)", "UpdateFileWriteTransactional(ECardResult result)"):
            body = between(driver, failed + " {", "\nvoid ", DRIVER)
            self.assertNotIn("WriteBackupBuf", body, failed)
            self.assertIn("Failed", body, failed)
        self.assertEqual(driver.count("PortMapSide::Flush("), 1)
        # And flush is a no-op for a card whose directory could not be resolved.
        self.assertIn("if (path.empty())", SIDE.read_text())

    def test_directory_is_the_port_folder_and_the_card_in_use(self):
        paths = PATHS.read_text()
        self.assertIn("PortPaths::UserFolder()", paths)
        self.assertIn("CMemoryCardSys::PortCardBase()", paths)
        # The data never goes into the card or the emulator's folder: nothing
        # here builds an output path from anything but the user folder.
        self.assertNotIn("CardTargetDirectory", paths)
        self.assertNotIn("CardFolder", paths)
        side = SIDE.read_text()
        self.assertIn('user + "mapdata"', side)
        self.assertIn("CardKey(card.c_str())", side)
        # One file per card and slot, written through a temporary and a rename.
        self.assertIn('"%016llx-%d.bin"', side)
        self.assertIn("fs::rename(temp, path, ec)", side)
        self.assertIn("fs::remove(temp, ec)", side)

    def test_record_is_fingerprinted_and_the_load_falls_back(self):
        header = HEADER.read_text()
        self.assertIn("Fingerprint", header)
        # A wrong fingerprint, a longer or shorter file and a count above the
        # cap are all refused, so the caller keeps what the save's trailers gave.
        self.assertIn("count > PortMapMarks::kMaxMarks", header)
        self.assertIn("bytes.size() > kMaxBytes", SIDE.read_text())
        self.assertIn("(lo | (hi << 32)) != fingerprint", header)
        # Reading is the only thing Apply does; nothing is written on a load.
        apply = between(SIDE.read_text(), "bool Apply(int slot", "\n#endif", SIDE)
        self.assertNotIn("WriteFile", apply)
        self.assertNotIn("RemoveFile", apply)

    def test_trailer_keeps_its_first_shape_and_the_side_file_holds_more(self):
        marks = MARKS.read_text()
        self.assertIn("constexpr unsigned kCountBits = 5;", marks)
        self.assertIn("constexpr size_t kTrailerMarks = 20;", marks)
        self.assertIn("constexpr size_t kMaxMarks = 64;", marks)
        self.assertIn("count > kTrailerMarks", marks)
        # Fit never hands the trailer more than its count field holds, and the
        # writer still writes only what fits.
        fit = between(marks, "inline size_t Fit(", "\n}", MARKS)
        self.assertIn("fit < kTrailerMarks ? fit : kTrailerMarks", fit)
        self.assertIn("PortMapMarks::Fit(", (root / "src/MetroidPrime/Player/CGameState.cpp").read_text())

    def test_save_state_carries_the_whole_list(self):
        state = SAVESTATE.read_text()
        capture = between(state, "bool Capture(CStateManager& mgr, Header& header,", "\nvoid ",
                          SAVESTATE)
        self.assertIn("gpGameState->PutTo(out);", capture)
        self.assertIn("gameBits = out.PortBitsWritten();", capture)
        # The record is written after the stream is gone: COutputStream flushes
        # its last byte in the destructor, so a fingerprint taken inside the
        # scope covers bytes that change at once and never matches on load.
        self.assertIn("StoreMapData(blob, gameBits);", capture)
        self.assertIn("}\n", between(capture, "gpGameState->PutTo(out);",
                                     "StoreMapData(blob, gameBits);", SAVESTATE))
        install = between(state, "void InstallPending() {", "\n}\n", SAVESTATE)
        self.assertIn("gpMain->PortLoadGameState(in);", install)
        self.assertIn("LoadMapData(sPendingBlob);", install)
        # The record sits in the blob's tail, past the game state, and only its
        # own fingerprint makes it ours: an older state keeps its trailers.
        self.assertIn("constexpr size_t kMapOffset = kBlobSize - 256;", state)
        load = between(state, "bool LoadMapData(const std::vector< uint8_t >& blob) {", "\n}", state)
        self.assertIn("PortMapSide::Decode(", load)
        self.assertIn("PortMapSide::Fingerprint(blob.data(), kMapOffset)", load)

    def test_sources_are_built(self):
        cmake = CMAKE.read_text()
        for source in ("platform/port_map_side.cpp", "platform/port_map_side_paths.cpp"):
            self.assertIn(source, cmake)
        self.assertIn("port_map_side_tests", cmake)
        self.assertIn("port_map_side_wiring", cmake)


if __name__ == "__main__":
    unittest.main(verbosity=2)
