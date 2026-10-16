#include "port_savestate.h"

#include <cstdio>
#include <cstdlib>
#include <limits>

namespace {
int sLine = 0;
void Check(bool condition) {
  if (!condition) {
    std::fprintf(stderr, "savestate regression failed at line %d\n", sLine);
    std::abort();
  }
}
#define CHECK(cond)                                                                                \
  do {                                                                                             \
    sLine = __LINE__;                                                                              \
    Check(cond);                                                                                   \
  } while (0)

using PortSaveState::Header;
} // namespace

int main() {
  Header in;
  in.worldId = 0x83F6FF6F;
  in.areaId = 17;
  in.position[0] = -120.5f;
  in.position[1] = 33.25f;
  in.position[2] = 9.f;
  in.forward[0] = 0.f;
  in.forward[1] = -1.f;
  in.forward[2] = 0.f;
  in.morphed = true;
  in.playTime = 3723.5;
  in.savedAt = 1790000000;
  in.world = "Chozo Ruins";
  in.room = "Main Plaza";
  std::vector< uint8_t > blob(4096, 0);
  for (size_t i = 0; i < 940; ++i)
    blob[i] = static_cast< uint8_t >(i * 7);

  const std::string data = PortSaveState::Encode(in, blob);

  // Round trip.
  {
    Header out;
    std::vector< uint8_t > outBlob;
    CHECK(PortSaveState::Decode(data, out, outBlob));
    CHECK(out.worldId == in.worldId);
    CHECK(out.areaId == in.areaId);
    for (int i = 0; i < 3; ++i) {
      CHECK(out.position[i] == in.position[i]);
      CHECK(out.forward[i] == in.forward[i]);
    }
    CHECK(out.morphed);
    CHECK(out.playTime == in.playTime);
    CHECK(out.savedAt == in.savedAt);
    CHECK(out.world == in.world);
    CHECK(out.room == in.room);
    CHECK(outBlob == blob);
  }

  // Every truncation fails and leaves the outputs alone.
  for (size_t len = 0; len < data.size(); ++len) {
    Header out;
    out.worldId = 1;
    std::vector< uint8_t > outBlob{9};
    CHECK(!PortSaveState::Decode(data.substr(0, len), out, outBlob));
    CHECK(out.worldId == 1);
    CHECK(outBlob.size() == 1);
  }

  // Trailing junk, a wrong magic, a wrong version and an empty blob fail.
  {
    Header out;
    std::vector< uint8_t > outBlob;
    CHECK(!PortSaveState::Decode(data + "x", out, outBlob));
    std::string bad = data;
    bad[0] = 'X';
    CHECK(!PortSaveState::Decode(bad, out, outBlob));
    bad = data;
    bad[4] = 2;
    CHECK(!PortSaveState::Decode(bad, out, outBlob));
    CHECK(!PortSaveState::Decode(PortSaveState::Encode(in, {}), out, outBlob));
  }

  // A NaN position is refused (it would teleport Samus into nowhere).
  {
    Header nan = in;
    nan.position[1] = std::numeric_limits< float >::quiet_NaN();
    Header out;
    std::vector< uint8_t > outBlob;
    CHECK(!PortSaveState::Decode(PortSaveState::Encode(nan, blob), out, outBlob));
  }

  // Long labels are cut, not rejected.
  {
    Header longLabel = in;
    longLabel.room.assign(1000, 'r');
    Header out;
    std::vector< uint8_t > outBlob;
    CHECK(PortSaveState::Decode(PortSaveState::Encode(longLabel, blob), out, outBlob));
    CHECK(out.room.size() == 256);
  }

  // Room state: round trip, skipping of unknown tags and versions, old files.
  {
    using namespace PortSaveState;
    Header withRoom = in;
    withRoom.roomState.areas = {2, 5};
    withRoom.roomState.present = {0x00020001, 0x00050007};
    RoomObject enemy;
    enemy.editorId = 0x00020001;
    enemy.active = true;
    BlockWriter hp;
    hp.F32(37.5f);
    hp.F32(0.25f);
    enemy.blocks.push_back({3, hp.bytes});
    BlockWriter ai;
    ai.U8(1);
    ai.Str("Patrol");
    ai.F32(4.5f);
    enemy.blocks.push_back({4, ai.bytes});
    enemy.blocks.push_back({0xBE, "future block"});
    RoomObject door;
    door.editorId = 0x00050007;
    withRoom.roomState.objects = {enemy, door};
    // Fingerprints: one entry per captured area, in the same order, with the
    // area the entry belongs to; an unreadable area records itself as unknown.
    withRoom.roomState.fingerprintsRecorded = true;
    withRoom.roomState.fingerprints = {{2, true, 0x1122334455667788ull}, {5, false, 0}};

    const std::string file = Encode(withRoom, blob);
    Header out;
    std::vector< uint8_t > outBlob;
    CHECK(Decode(file, out, outBlob));
    CHECK(outBlob == blob);
    CHECK(out.roomState.areas == withRoom.roomState.areas);
    CHECK(out.roomState.present == withRoom.roomState.present);
    CHECK(out.roomState.fingerprintsRecorded);
    CHECK(out.roomState.fingerprints.size() == 2);
    CHECK(out.roomState.fingerprints[0].area == 2 && out.roomState.fingerprints[0].known &&
          out.roomState.fingerprints[0].hash == 0x1122334455667788ull);
    CHECK(out.roomState.fingerprints[1].area == 5 && !out.roomState.fingerprints[1].known &&
          out.roomState.fingerprints[1].hash == 0);
    CHECK(out.roomState.objects.size() == 2);
    const RoomObject& got = out.roomState.objects[0];
    CHECK(got.editorId == 0x00020001 && got.active);
    CHECK(out.roomState.objects[1].editorId == 0x00050007 && !out.roomState.objects[1].active);
    const RoomBlock* b = FindBlock(got, 3);
    CHECK(b != nullptr);
    BlockReader hpIn(b->data);
    CHECK(hpIn.F32() == 37.5f && hpIn.F32() == 0.25f && hpIn.ok);
    b = FindBlock(got, 4);
    CHECK(b != nullptr);
    BlockReader aiIn(b->data);
    CHECK(aiIn.U8() == 1 && aiIn.Str() == "Patrol" && aiIn.F32() == 4.5f && aiIn.ok);
    CHECK(FindBlock(got, 0xBE) != nullptr && FindBlock(got, 9) == nullptr);
    BlockReader shortIn(b->data);
    shortIn.U32();
    shortIn.U32();
    shortIn.U32();
    shortIn.U32();
    CHECK(!shortIn.ok);

    // A file without the section (an old state) loads with an empty room state.
    CHECK(Decode(data, out, outBlob) && out.roomState.empty());

    // A newer version of the section is skipped, the state still loads.
    std::string newer = file;
    const size_t trailer = data.size();
    CHECK(newer.compare(trailer, 4, "RMST") == 0);
    newer[trailer + 4] = 9;
    CHECK(Decode(newer, out, outBlob) && out.roomState.empty() && outBlob == blob);

    // A damaged section is dropped; the rest of the state is kept.
    for (size_t len = trailer + 1; len < file.size(); ++len) {
      Header cut;
      CHECK(!Decode(file.substr(0, len), cut, outBlob) || cut.roomState.empty());
    }
    std::string bogus = file;
    bogus[trailer + 12] = static_cast< char >(0xFF);  // area count absurd
    bogus[trailer + 15] = static_cast< char >(0x7F);
    CHECK(Decode(bogus, out, outBlob) && out.roomState.empty());
    CHECK(!Decode(data + "RMS", out, outBlob));

    // The resume wrapper carries it too.
    ResumeMeta meta;
    meta.fileIdx = 0;
    std::string backState;
    ResumeMeta backMeta;
    CHECK(DecodeResume(EncodeResume(meta, file), backMeta, backState));
    CHECK(Decode(backState, out, outBlob) && out.roomState.objects.size() == 2);
  }

  // Room script fingerprints (RMST version 2): the hash and the per-area
  // decision the runtime makes, a v2 round trip, an explicit v1 file, an
  // unknown version and malformed fingerprint associations.
  {
    using namespace PortSaveState;

    std::vector< uint8_t > script(64);
    for (size_t i = 0; i < script.size(); ++i)
      script[i] = static_cast< uint8_t >(i * 3 + 1);
    const uint64_t hash = FingerprintScriptData(script.data(), script.size());
    CHECK(hash != 0);
    CHECK(FingerprintScriptData(script.data(), script.size()) == hash);
    CHECK(FingerprintScriptData(nullptr, 0) == 0);
    CHECK(FingerprintScriptData(script.data(), 0) == 0);
    std::vector< uint8_t > changed = script;
    changed[script.size() - 1] ^= 0x80;
    CHECK(FingerprintScriptData(changed.data(), changed.size()) != hash);
    CHECK(FingerprintScriptData(script.data(), script.size() - 1) != hash);

    // Nothing to read means no fingerprint: unknown, never a hash of its own.
    const AreaFingerprint none = MakeAreaFingerprint(5, nullptr, 0);
    CHECK(none.area == 5 && !none.known && none.hash == 0);
    const AreaFingerprint known = MakeAreaFingerprint(2, script.data(), script.size());
    CHECK(known.area == 2 && known.known && known.hash == hash);

    // What ApplyRoomState decides per area, through the same helper: matching
    // accepted, changed and unreadable rejected (they are skipped whole), and a
    // state without fingerprints restored as it was before.
    RoomState saved;
    saved.fingerprintsRecorded = true;
    saved.areas = {2, 5};
    saved.fingerprints = {known, none};
    CHECK(DecideAreaRestore(saved, 2, script.data(), script.size()) == RoomAreaRestore::kRestore);
    CHECK(DecideAreaRestore(saved, 2, changed.data(), changed.size()) == RoomAreaRestore::kSkipChanged);
    CHECK(DecideAreaRestore(saved, 5, script.data(), script.size()) == RoomAreaRestore::kSkipUnreadable);
    CHECK(DecideAreaRestore(saved, 9, script.data(), script.size()) == RoomAreaRestore::kSkipUnreadable);
    CHECK(DecideAreaRestore(saved, 2, nullptr, 0) == RoomAreaRestore::kSkipUnreadable);
    RoomState legacy;
    legacy.areas = {2};
    CHECK(DecideAreaRestore(legacy, 2, changed.data(), changed.size()) == RoomAreaRestore::kRestore);
    CHECK(DecideAreaRestore(legacy, 2, nullptr, 0) == RoomAreaRestore::kRestore);

    // v2 round trip through the encoder: both fingerprints come back attached
    // to their own area.
    Header withFp = in;
    withFp.roomState = saved;
    Header out;
    std::vector< uint8_t > outBlob;
    const std::string file = Encode(withFp, blob);
    CHECK(Decode(file, out, outBlob));
    CHECK(out.roomState.fingerprintsRecorded);
    CHECK(out.roomState.areas == saved.areas);
    CHECK(out.roomState.fingerprints.size() == 2);
    CHECK(out.roomState.fingerprints[0].area == 2 && out.roomState.fingerprints[0].known &&
          out.roomState.fingerprints[0].hash == hash);
    CHECK(out.roomState.fingerprints[1].area == 5 && !out.roomState.fingerprints[1].known &&
          out.roomState.fingerprints[1].hash == 0);
    CHECK(DecideAreaRestore(out.roomState, 2, script.data(), script.size()) == RoomAreaRestore::kRestore);
    CHECK(DecideAreaRestore(out.roomState, 5, script.data(), script.size()) ==
          RoomAreaRestore::kSkipUnreadable);

    // Fingerprints that do not line up one per area fail closed: the writer
    // leaves out the whole optional room-state trailer, so the base game-state
    // blob still loads and nothing can be restored unconditionally onto areas
    // the fingerprints do not describe.
    Header unaligned = in;
    unaligned.roomState.areas = {2, 5};
    unaligned.roomState.fingerprintsRecorded = true;
    unaligned.roomState.fingerprints = {known};  // one for two areas
    const std::string shortFile = Encode(unaligned, blob);
    CHECK(shortFile.size() == data.size());  // no trailer at all
    CHECK(Decode(shortFile, out, outBlob));
    CHECK(outBlob == blob && out.worldId == in.worldId && out.room == in.room);
    CHECK(out.roomState.empty() && !out.roomState.fingerprintsRecorded);
    // ... and the same for entries that line up in count but not in area.
    Header swapped = in;
    swapped.roomState.areas = {2, 5};
    swapped.roomState.fingerprintsRecorded = true;
    swapped.roomState.fingerprints = {MakeAreaFingerprint(5, script.data(), script.size()),
                                      MakeAreaFingerprint(2, script.data(), script.size())};
    const std::string swappedFile = Encode(swapped, blob);
    CHECK(swappedFile.size() == data.size());
    CHECK(Decode(swappedFile, out, outBlob));
    CHECK(out.roomState.empty() && !out.roomState.fingerprintsRecorded);
    // A fingerprinted snapshot that lines up is RMST version 2 (the encoder's
    // own file), and a legacy one below stays version 1.
    CHECK(static_cast< uint8_t >(file[data.size() + 4]) == 2);

    // A state written before fingerprints existed (fingerprintsRecorded false)
    // still writes the legacy body and restores as it always did.
    Header legacyState = in;
    legacyState.roomState.areas = {2};
    legacyState.roomState.present = {0x00020001};
    RoomObject only;
    only.editorId = 0x00020001;
    legacyState.roomState.objects = {only};
    const std::string legacyFile = Encode(legacyState, blob);
    CHECK(static_cast< uint8_t >(legacyFile[data.size() + 4]) == 1);
    CHECK(Decode(legacyFile, out, outBlob));
    CHECK(!out.roomState.fingerprintsRecorded && out.roomState.fingerprints.empty());
    CHECK(out.roomState.areas.size() == 1 && out.roomState.objects.size() == 1);
    CHECK(DecideAreaRestore(out.roomState, 2, changed.data(), changed.size()) == RoomAreaRestore::kRestore);

    // A hash of 0 is a value a fingerprint may legitimately carry (the known
    // bit is what says an entry was hashed): it survives the round trip and
    // still takes part in the decision, rather than being read as malformed.
    Header zeroHash = in;
    zeroHash.roomState.areas = {2};
    zeroHash.roomState.fingerprintsRecorded = true;
    zeroHash.roomState.fingerprints = {{2, true, 0}};
    CHECK(Decode(Encode(zeroHash, blob), out, outBlob));
    CHECK(out.roomState.fingerprintsRecorded && out.roomState.fingerprints.size() == 1);
    CHECK(out.roomState.fingerprints[0].area == 2 && out.roomState.fingerprints[0].known &&
          out.roomState.fingerprints[0].hash == 0);
    CHECK(DecideAreaRestore(out.roomState, 2, script.data(), script.size()) ==
          RoomAreaRestore::kSkipChanged);
    // An entry that was never hashed carries no hash, and stays unknown.
    Header unknown = in;
    unknown.roomState.areas = {2};
    unknown.roomState.fingerprintsRecorded = true;
    unknown.roomState.fingerprints = {MakeAreaFingerprint(2, nullptr, 0)};
    CHECK(Decode(Encode(unknown, blob), out, outBlob));
    CHECK(out.roomState.fingerprints[0].area == 2 && !out.roomState.fingerprints[0].known);
    CHECK(DecideAreaRestore(out.roomState, 2, script.data(), script.size()) ==
          RoomAreaRestore::kSkipUnreadable);

    // Little-endian builders, so a hand-written trailer can carry bodies the
    // writer never produces.
    const auto le32 = [](uint32_t v) {
      std::string s;
      for (int i = 0; i < 4; ++i)
        s += static_cast< char >((v >> (i * 8)) & 0xFF);
      return s;
    };
    const auto le64 = [&](uint64_t v) {
      return le32(static_cast< uint32_t >(v)) + le32(static_cast< uint32_t >(v >> 32));
    };
    const auto fpEntry = [&](uint32_t area, uint32_t flags, uint64_t h) {
      return le32(area) + std::string(1, static_cast< char >(flags)) + le64(h);
    };
    const auto trailer = [&](uint32_t version, const std::string& body) {
      std::string t("RMST");
      return data + t + le32(version) + le32(static_cast< uint32_t >(body.size())) + body;
    };
    // A v1 body: one area, one object (editor id 2 = zigzag delta varint 0x04),
    // no fingerprint section.
    const std::string v1body = le32(1) + le32(2) + le32(1) + std::string(1, 0x04) + std::string(1, 0x00);
    const std::string v1file = trailer(1, v1body);
    CHECK(Decode(v1file, out, outBlob));
    CHECK(out.roomState.fingerprintsRecorded == false && out.roomState.fingerprints.empty());
    CHECK(out.roomState.areas.size() == 1 && out.roomState.areas[0] == 2);
    CHECK(out.roomState.objects.size() == 1 && out.roomState.objects[0].editorId == 2);
    CHECK(DecideAreaRestore(out.roomState, 2, changed.data(), changed.size()) == RoomAreaRestore::kRestore);

    // A v2 body hand-written the way the writer writes it, next to the
    // encoder's own output: same fields, both areas with a fingerprint.
    const std::string v2body = le32(2) + le32(2) + le32(5) + std::string(1, 1) + le32(2) +
                               fpEntry(2, 1, 0x1111222233334444ull) + fpEntry(5, 1, 0x5555666677778888ull) +
                               le32(1) + std::string(1, 0x04) + std::string(1, 0x00);
    CHECK(Decode(trailer(2, v2body), out, outBlob));
    CHECK(out.roomState.fingerprintsRecorded && out.roomState.fingerprints.size() == 2);
    CHECK((out.roomState.areas == std::vector< int32_t >{2, 5}));
    CHECK(out.roomState.fingerprints[0].area == 2 && out.roomState.fingerprints[0].hash == 0x1111222233334444ull);
    CHECK(out.roomState.fingerprints[1].area == 5 && out.roomState.fingerprints[1].hash == 0x5555666677778888ull);
    CHECK(DecideAreaRestore(out.roomState, 5, script.data(), script.size()) ==
          RoomAreaRestore::kSkipChanged);

    // A version this build does not know is skipped, as before v2 existed.
    CHECK(Decode(trailer(3, v2body), out, outBlob));
    CHECK(out.roomState.empty() && !out.roomState.fingerprintsRecorded);
    CHECK(Decode(trailer(0, v1body), out, outBlob));
    CHECK(out.roomState.empty() && !out.roomState.fingerprintsRecorded);

    // Malformed fingerprint associations: the whole room state is dropped (the
    // state itself still loads) rather than applied to areas it does not
    // describe. Truncation is covered with it: every prefix of such a body is
    // dropped or refused too.
    const std::string bodies2[] = {
        le32(2) + le32(2) + le32(5) + std::string(1, 1) + le32(1) + fpEntry(2, 1, 0x1111) + le32(0),
        le32(2) + le32(2) + le32(5) + std::string(1, 1) + le32(2) + fpEntry(5, 1, 0x1111) +
            fpEntry(2, 1, 0x2222) + le32(0),
        le32(2) + le32(5) + le32(2) + std::string(1, 1) + le32(2) + fpEntry(2, 1, 0x1111) +
            fpEntry(5, 1, 0x2222) + le32(0),
        le32(2) + le32(2) + le32(5) + std::string(1, 2) + le32(2) + fpEntry(2, 1, 0x1111) +
            fpEntry(5, 1, 0x2222) + le32(0),
        le32(2) + le32(2) + le32(5) + std::string(1, 1) + le32(2) + fpEntry(2, 0, 0x1111) +
            fpEntry(5, 1, 0x2222) + le32(0),
        le32(2) + le32(2) + le32(5) + std::string(1, 1) + le32(3) + fpEntry(2, 1, 0x1111) +
            fpEntry(5, 1, 0x2222) + le32(0),
    };
    for (const std::string& body : bodies2) {
      const std::string bad = trailer(2, body);
      CHECK(Decode(bad, out, outBlob));
      CHECK(out.roomState.empty() && !out.roomState.fingerprintsRecorded);
      // ...and every truncation of one is dropped or refused too.
      for (size_t len = data.size() + 12; len < bad.size(); ++len) {
        Header cut;
        CHECK(!Decode(bad.substr(0, len), cut, outBlob) || cut.roomState.empty());
      }
    }
    // A hand-written v2 body whose hashed entry carries a 0 hash is accepted:
    // the known bit, not the value, says the entry was hashed.
    const std::string zeroBody = le32(1) + le32(2) + std::string(1, 1) + le32(1) + fpEntry(2, 1, 0) +
                                 le32(1) + std::string(1, 0x04) + std::string(1, 0x00);
    CHECK(Decode(trailer(2, zeroBody), out, outBlob));
    CHECK(out.roomState.fingerprintsRecorded && out.roomState.fingerprints.size() == 1);
    CHECK(out.roomState.fingerprints[0].known && out.roomState.fingerprints[0].hash == 0);
    // The v2 body cut short inside its last object, with the trailer length
    // still consistent: refused, not read past the end.
    CHECK(Decode(trailer(2, v2body.substr(0, v2body.size() - 4)), out, outBlob));
    CHECK(out.roomState.empty() && !out.roomState.fingerprintsRecorded);

    // A v2 body that says it carries no fingerprints restores as a legacy one
    // does (what an older writer produces, and the only remaining way to write
    // without fingerprints).
    const std::string v2NoFp =
        le32(1) + le32(2) + std::string(1, 0) + le32(1) + std::string(1, 0x04) + std::string(1, 0x00);
    CHECK(Decode(trailer(2, v2NoFp), out, outBlob));
    CHECK(!out.roomState.fingerprintsRecorded && out.roomState.areas.size() == 1);
    CHECK(DecideAreaRestore(out.roomState, 2, changed.data(), changed.size()) == RoomAreaRestore::kRestore);
  }

  // Quick resume wrapper.
  {
    PortSaveState::ResumeMeta meta;
    meta.fileIdx = 1;
    meta.cardSerial = 0x1122334455667788ull;
    meta.identity = "ap:seed42";
    const std::string state = "state-bytes";
    const std::string file = PortSaveState::EncodeResume(meta, state);
    PortSaveState::ResumeMeta back;
    std::string backState;
    CHECK(PortSaveState::DecodeResume(file, back, backState));
    CHECK(back.fileIdx == 1 && back.cardSerial == meta.cardSerial && back.identity == meta.identity);
    CHECK(backState == state);
    CHECK(!PortSaveState::DecodeResume(file.substr(0, file.size() - 1), back, backState));
    CHECK(!PortSaveState::DecodeResume(file + "x", back, backState));
    CHECK(!PortSaveState::DecodeResume("MPSS" + file.substr(4), back, backState));
    CHECK(!PortSaveState::DecodeResume("", back, backState));
    PortSaveState::ResumeMeta badSlot = meta;
    badSlot.fileIdx = 7;
    CHECK(!PortSaveState::DecodeResume(PortSaveState::EncodeResume(badSlot, state), back, backState));

    CHECK(PortSaveState::ResumeMatches(meta, meta));
    PortSaveState::ResumeMeta other = meta;
    other.identity = "ap:seed43";
    CHECK(!PortSaveState::ResumeMatches(meta, other));
    other = meta;
    other.cardSerial = 1;
    CHECK(!PortSaveState::ResumeMatches(meta, other));
    other = meta;
    other.identity.clear();
    CHECK(!PortSaveState::ResumeMatches(meta, other));
  }

  // Quick resume skip rules.
  {
    PortSaveState::ResumeContext ctx;
    CHECK(PortSaveState::ResumeSkipReason(ctx) != nullptr);  // not in game
    ctx.inGame = true;
    ctx.initialized = true;
    CHECK(PortSaveState::ResumeSkipReason(ctx) == nullptr);
    bool PortSaveState::ResumeContext::*flags[] = {
        &PortSaveState::ResumeContext::quitting, &PortSaveState::ResumeContext::dead,
        &PortSaveState::ResumeContext::cinematic, &PortSaveState::ResumeContext::transition,
        &PortSaveState::ResumeContext::saveUi};
    for (auto flag : flags) {
      PortSaveState::ResumeContext c = ctx;
      c.*flag = true;
      CHECK(PortSaveState::ResumeSkipReason(c) != nullptr);
    }
    ctx.initialized = false;
    CHECK(PortSaveState::ResumeSkipReason(ctx) != nullptr);
  }

  // Snapshot selection at quit/background.
  {
    using PortSaveState::ResumeWriteChoice;
    CHECK(PortSaveState::ChooseResumeWrite(true, false) == ResumeWriteChoice::kRefreshThenWrite);
    CHECK(PortSaveState::ChooseResumeWrite(true, true) == ResumeWriteChoice::kRefreshThenWrite);
    CHECK(PortSaveState::ChooseResumeWrite(false, true) == ResumeWriteChoice::kWriteSnapshot);
    CHECK(PortSaveState::ChooseResumeWrite(false, false) == ResumeWriteChoice::kNothing);
  }
  // Snapshot refresh schedule.
  {
    using PortSaveState::ResumeRefreshDue;
    const double stable = PortSaveState::kResumeStableSeconds;
    const double period = PortSaveState::kResumeRefreshSeconds;
    // Never while unsafe.
    CHECK(!ResumeRefreshDue(false, true, true, 1000.0, 1000.0));
    CHECK(!ResumeRefreshDue(false, false, false, 1000.0, 0.0));
    // First snapshot, or a new room, waits for the room to be stable.
    CHECK(!ResumeRefreshDue(true, false, false, stable - 0.1, 0.0));
    CHECK(ResumeRefreshDue(true, false, false, stable, 0.0));
    CHECK(!ResumeRefreshDue(true, true, false, stable - 0.1, 5.0));
    CHECK(ResumeRefreshDue(true, true, false, stable, 5.0));
    // The same room refreshes on the period only.
    CHECK(!ResumeRefreshDue(true, true, true, 100.0, period - 0.1));
    CHECK(ResumeRefreshDue(true, true, true, 100.0, period));
  }

  // A label length over the cap is rejected, not read past.
  {
    std::string crafted = data;
    const size_t at = crafted.find("Main Plaza");
    CHECK(at != std::string::npos && at >= 4);
    crafted[at - 4] = 0x01;  // 257, little-endian
    crafted[at - 3] = 0x01;
    Header out;
    std::vector< uint8_t > outBlob;
    CHECK(!PortSaveState::Decode(crafted, out, outBlob));
  }

  // An object with more blocks than the decoder accepts keeps the first kMaxBlocks.
  {
    using namespace PortSaveState;
    Header many = in;
    RoomObject obj;
    obj.editorId = 0x00020001;
    obj.active = true;
    for (int i = 0; i < 130; ++i)
      obj.blocks.push_back({static_cast< uint32_t >(i), std::string(1, static_cast< char >(i))});
    many.roomState.areas = {2};
    many.roomState.present = {obj.editorId};
    many.roomState.objects = {obj};
    Header out;
    std::vector< uint8_t > outBlob;
    CHECK(Decode(Encode(many, blob), out, outBlob));
    CHECK(out.roomState.objects.size() == 1);
    const RoomObject& got = out.roomState.objects[0];
    CHECK(got.active && got.blocks.size() == 64);
    CHECK(got.blocks.back().tag == 63 && got.blocks.back().data == std::string(1, '\x3f'));
  }

  std::puts("savestate tests passed");
  return 0;
}
