// CHPR animation reader on small synthetic files: a blob with two stream infos (a bone
// stream and a bool-only stream whose op stream holds a sub-op 4), an init-dequant offset
// other than 0x28, and a record chain followed by a false record with a tiny length.
#include "port_remastered_anim.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
int sFailures = 0;

void Check(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++sFailures;
  }
}

// LSB-first bit writer, the order the game's bit streams are read in.
struct Bits {
  std::vector<uint8_t> bytes;
  size_t n = 0;
  void Put(uint32_t value, int count) {
    for (int i = 0; i < count; ++i, ++n) {
      if (n / 8 >= bytes.size()) {
        bytes.push_back(0);
      }
      bytes[n / 8] |= uint8_t(((value >> i) & 1u) << (n % 8));
    }
  }
};

void Le(std::vector<uint8_t>& v, uint32_t x, int bytes) {
  for (int i = 0; i < bytes; ++i) {
    v.push_back(uint8_t(x >> (8 * i)));
  }
}

// Stream 0: no tracks, one op stream group `sub-op 3, 1 entry` (a rotation and a translation
// pool reference). Stream 1: one type 3 track (bool words, extra 1) and an op stream with a
// sub-op 4 (a byte, the track index, one u16 word).
std::vector<uint8_t> Blob() {
  Bits h;  // header bits
  h.Put(2, 4);
  h.Put(1, 10);
  h.Put(0, 4);
  const uint32_t w1[2] = {0x34, 0x44};
  const uint32_t w2[2] = {0x38, 0x48};
  for (int s = 0; s < 2; ++s) {
    h.Put(w1[s], 20);
    h.Put(w2[s], 20);
    h.Put(s, 4);  // tracks
    h.Put(0, 1);
    h.Put(0, 1);
    h.Put(1, 4);  // v
    h.Put(0, 4);  // u6
    h.Put(0, 4);  // context
    if (s == 1) {
      h.Put(1, 10);  // count
      h.Put(3, 3);   // type: bool words
      h.Put(0, 1);
      h.Put(1, 6);  // word count
    }
  }
  Bits init;
  init.Put(0, 4);  // select stream
  init.Put(1, 4);
  init.Put(5, 4);  // bool values
  init.Put(0, 4);  // track 0
  init.Put(1, 12);
  init.Put(1, 1);
  init.Put(0x1234, 16);
  init.Put(1, 4);  // done
  std::vector<uint8_t> b;
  Le(b, 0x54, 4);
  Le(b, 0x18, 2);
  Le(b, 0x2c, 2);  // init-dequant offset: not the usual 0x28
  Le(b, 0, 4);
  Le(b, 0x3f800000, 4);  // rate 1.0
  Le(b, 1, 2);           // frames
  Le(b, 1, 4);
  Le(b, 0, 2);
  b.insert(b.end(), h.bytes.begin(), h.bytes.end());
  b.resize(0x2c);
  b.insert(b.end(), init.bytes.begin(), init.bytes.end());
  b.resize(0x34);
  b.resize(0x38, 0);  // stream 0 data
  const uint8_t ops0[] = {1, 8, 0, 3, 1, 0, 0, 0, 0, 1, 0, 0};
  b.insert(b.end(), ops0, ops0 + sizeof ops0);
  b.resize(0x48, 0);  // stream 1 data
  const uint8_t ops1[] = {1, 5, 0, 4, 0, 0, 0x34, 0x12, 0};
  b.insert(b.end(), ops1, ops1 + sizeof ops1);
  b.resize(0x54, 0);
  return b;
}

std::vector<uint8_t> File(int records, bool tinyTail) {
  std::vector<uint8_t> f(0x100, 0);
  std::memcpy(f.data(), "RFRM", 4);
  std::memcpy(f.data() + 0x14, "CHPR", 4);
  const std::vector<uint8_t> blob = Blob();
  for (int r = 0; r < records; ++r) {
    f.push_back(1);
    Le(f, uint32_t(r + 1), 4);
    Le(f, uint32_t(blob.size()), 4);
    // The blob's own size field is the record length: the length is at record + 5.
    f.resize(f.size() - 4);
    f.insert(f.end(), blob.begin(), blob.end());
    f.resize(f.size() + 7, 0);
  }
  if (tinyTail) {
    f.push_back(1);
    Le(f, 20, 4);
    Le(f, 2, 4);
    f.resize(f.size() + 16, 0);
  }
  return f;
}

void Run(const char* what, int records, bool tinyTail) {
  PortRemasteredAnim::Character c;
  std::string error;
  const bool ok = PortRemasteredAnim::ReadCharacter(File(records, tinyTail), c, error);
  if (!ok) {
    std::fprintf(stderr, "%s: %s\n", what, error.c_str());
  }
  Check(ok, what);
  if (ok) {
    Check(c.anims.size() == size_t(records), "one animation per record");
    Check(!c.anims.empty() && c.anims[0].frames == 1 && c.anims[0].fps == 30.0f, "frames and rate");
    Check(!c.anims.empty() && !c.anims[0].tracked.empty() && c.anims[0].tracked[0], "bone 0 is driven");
  }
}
}  // namespace

int main() {
  Run("single record, init-dequant offset 0x2c", 1, false);
  Run("two records and a tiny false record", 2, true);
  return sFailures == 0 ? 0 : 1;
}
