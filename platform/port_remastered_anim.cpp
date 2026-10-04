// See platform/include/port_remastered_anim.h for what this is and why it exists.
// A port of build/mpr/anim/chpr_anim.py and chpr_skel.py. The arithmetic is done in
// doubles with the same float roundings the Python applies, so the output matches it.

#include "port_remastered_anim.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace PortRemasteredAnim {
namespace {

// A problem with the file or a layout the decoder does not know. Thrown inside this
// file only and turned into the error string at the API boundary.
struct Failure {
  std::string message;
};

[[noreturn]] void Fail(const std::string& message) { throw Failure{message}; }

uint16_t ReadLE16(const uint8_t* p) { return uint16_t(p[0]) | uint16_t(uint16_t(p[1]) << 8); }

uint32_t ReadLE32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

float ReadLEFloat(const uint8_t* p) {
  const uint32_t bits = ReadLE32(p);
  float value = 0.0f;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

// The Python rounds to single precision at a few points; this is that rounding.
double F32(double x) { return double(float(x)); }

// The file as a bounds-checked view.
struct Bytes {
  const uint8_t* data;
  size_t size;

  bool Has(size_t offset, size_t count) const { return offset <= size && count <= size - offset; }
  void Need(size_t offset, size_t count, const char* what) const {
    if (!Has(offset, count)) {
      Fail(std::string("truncated ") + what);
    }
  }
};

// CAnimBitStream: bits are read least significant first. Reading past the end gives
// zeros, as the Python does; callers that loop on the stream check Overrun().
class BitStream {
public:
  BitStream(const Bytes& bytes, uint64_t bitPos) : m_bytes(bytes), m_pos(bitPos) {}

  uint32_t Read(int count) {
    uint32_t value = 0;
    for (int i = 0; i < count; ++i) {
      const uint64_t byteIndex = m_pos >> 3;
      const uint32_t byte = byteIndex < m_bytes.size ? m_bytes.data[byteIndex] : 0;
      value |= ((byte >> (m_pos & 7)) & 1u) << i;
      ++m_pos;
    }
    return value;
  }

  bool Overrun() const { return m_pos > uint64_t(m_bytes.size) * 8 + 64; }

private:
  Bytes m_bytes;
  uint64_t m_pos;
};

int64_t SignExtend(int64_t v, int bits) {
  if (bits <= 0) {
    return 0;
  }
  return (v & (int64_t(1) << (bits - 1))) ? v - (int64_t(1) << bits) : v;
}

using Quat = std::array<double, 4>;  // w x y z while decoding

Quat Normalize(const Quat& q) {
  double n = 0.0;
  for (double c : q) {
    n += c * c;
  }
  const double r = n > 0.0 ? 1.0 / std::sqrt(n) : 1.0;
  return {q[0] * r, q[1] * r, q[2] * r, q[3] * r};
}

Quat Lerp(const Quat& a, const Quat& b, double t) {
  return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t,
          a[3] + (b[3] - a[3]) * t};
}

// anim_rot_lerp2 from the exe: a two-segment lerp through the normalized midpoint,
// taking the short way round.
Quat RotLerp2(const Quat& a, Quat b, double t) {
  double dot = 0.0;
  for (int i = 0; i < 4; ++i) {
    dot += a[size_t(i)] * b[size_t(i)];
  }
  if (dot < -0.001953125) {
    for (double& c : b) {
      c = -c;
    }
  }
  const Quat mid =
      Normalize({(a[0] + b[0]) * 0.5, (a[1] + b[1]) * 0.5, (a[2] + b[2]) * 0.5, (a[3] + b[3]) * 0.5});
  Quat r;
  if (t < 0.5) {
    r = Lerp(a, mid, 2 * t);
  } else if (t > 0.5) {
    r = Lerp(mid, b, 2 * (t - 0.5));
  } else {
    r = mid;
  }
  return Normalize(r);
}

// One rotation value of a track: how to dequantize it and the two keyframes it is
// currently interpolating between.
struct Value {
  int W = 0;  // bits per component
  int q = 0;  // fixed-point shift
  int isSigned = 0;
  int mask = 0;  // bit i set: component i (x, y, z) is stored
  int flag7 = 0;  // interpolate with RotLerp2 instead of lerp + normalize
  int sgnPrev = 0, sgnCur = 0;  // w is negative
  int64_t prev[3] = {0, 0, 0};
  int64_t cur[3] = {0, 0, 0};
  int64_t curFrame = 0, nextFrame = 0;
  double inv = 0.0;
};

struct Track {
  int type = 0;
  int flag = 0;
  std::vector<Value> values;
};

struct StreamInfo {
  Bytes blob;
  uint32_t offInit = 0;
  uint32_t w1 = 0;  // byte offset of the data bit stream
  uint32_t w2 = 0;  // byte offset of the op stream
  float headerFloat = 0.0f;
  uint32_t frames = 0;
  uint32_t v = 0;  // quantization steps per frame
  uint32_t u6 = 0;  // bits flagging a keyframe change per frame
  std::vector<Track> tracks;
};

// Blob layout: 24 byte header, header bits, init-dequant bit stream, data bit
// stream, op stream (see NOTES-anim.md).
StreamInfo ParseBlob(const Bytes& d, size_t base) {
  d.Need(base, 24, "animation header");
  const uint32_t size = ReadLE32(d.data + base);
  if (size < 24 || !d.Has(base, size)) {
    Fail("animation blob runs past the end of the file");
  }
  StreamInfo info;
  info.blob = Bytes{d.data + base, size};
  const uint16_t offBits = ReadLE16(d.data + base + 4);
  info.offInit = ReadLE16(d.data + base + 6);
  info.headerFloat = ReadLEFloat(d.data + base + 12);
  info.frames = ReadLE16(d.data + base + 16);

  BitStream bs(info.blob, uint64_t(offBits) * 8);
  const uint32_t streamInfos = bs.Read(4);
  bs.Read(10);  // total tracks
  bs.Read(4);
  if (streamInfos != 1) {
    Fail(std::to_string(streamInfos) + " stream infos are not supported");
  }
  info.w1 = bs.Read(20);
  info.w2 = bs.Read(20);
  const uint32_t trackCount = bs.Read(4);
  bs.Read(1);
  bs.Read(1);
  info.v = bs.Read(4);
  info.u6 = bs.Read(4);
  bs.Read(4);
  for (uint32_t i = 0; i < trackCount; ++i) {
    const uint32_t count = bs.Read(10);
    const uint32_t type = bs.Read(3);
    const uint32_t flag = bs.Read(1);
    if (type == 3) {
      bs.Read(6);
    }
    if (type != 0) {
      Fail("track type " + std::to_string(type) + " is not supported");
    }
    Track t;
    t.type = int(type);
    t.flag = int(flag);
    t.values.resize(count);
    info.tracks.push_back(std::move(t));
  }
  if (bs.Overrun()) {
    Fail("animation header bits run past the blob");
  }
  return info;
}

void InitDequant(StreamInfo& info) {
  BitStream bs(info.blob, uint64_t(info.offInit) * 8);
  for (;;) {
    if (bs.Overrun()) {
      Fail("init-dequant stream has no end marker");
    }
    const uint32_t op = bs.Read(4);
    if (op == 0) {
      bs.Read(4);
    } else if (op == 1) {
      break;
    } else if (op == 2) {
      const uint32_t index = bs.Read(4);
      if (index >= info.tracks.size()) {
        Fail("init-dequant names a track that does not exist");
      }
      for (Value& val : info.tracks[index].values) {
        val.W = int(bs.Read(5));
        val.q = int(bs.Read(5));
        val.isSigned = int(bs.Read(1));
        val.mask = int(bs.Read(3));
      }
    } else {
      Fail("init-dequant op " + std::to_string(op) + " is not supported");
    }
  }
}

// CAnimCompStreamInst: walks the data bit stream one frame at a time.
class Stream {
public:
  explicit Stream(StreamInfo& info)
      : m_info(info), m_bs(info.blob, uint64_t(info.w1) * 8) {}

  int64_t Cur() const { return m_cur; }

  void InitFrames() {
    m_cur = 0;
    for (Track& tr : m_info.tracks) {
      for (Value& val : tr.values) {
        for (int i = 0; i < 3; ++i) {
          val.cur[i] = 0;
          val.prev[i] = 0;
        }
        val.curFrame = 0;
        val.nextFrame = 0;
        val.inv = 0.0;
        val.sgnCur = val.sgnPrev = 0;
      }
    }
    Load(SetupNext());
    Load(SetupStepped(0));
  }

  void DequantFrame() {
    ++m_cur;
    if (m_bs.Read(int(m_info.u6))) {
      if (m_cur == (int64_t(m_info.frames) - 1) * int64_t(m_info.v)) {
        Load(SetupStepped(1));
      } else {
        Load(SetupNext());
      }
    }
  }

private:
  std::vector<Value*> SetupNext() {
    const int width = int(m_bs.Read(4));
    std::vector<Value*> list;
    for (Track& tr : m_info.tracks) {
      for (Value& val : tr.values) {
        if (val.nextFrame == m_cur) {
          const int64_t delta = m_bs.Read(width);
          val.curFrame = val.nextFrame;
          val.nextFrame += delta;
          val.inv = delta ? F32(1.0 / double(delta)) : 0.0;
          list.push_back(&val);
        }
      }
    }
    return list;
  }

  std::vector<Value*> SetupStepped(int flag) {
    std::vector<Value*> list;
    for (Track& tr : m_info.tracks) {
      if (tr.flag == flag) {
        for (Value& val : tr.values) {
          list.push_back(&val);
        }
      }
    }
    return list;
  }

  void Load(const std::vector<Value*>& list) {
    for (Value* val : list) {
      for (int i = 0; i < 3; ++i) {
        val->prev[i] = val->cur[i];
        val->cur[i] = 0;
      }
      val->sgnPrev = val->sgnCur;
      val->flag7 = int(m_bs.Read(1));
      val->sgnCur = int(m_bs.Read(1));
      for (int c = 0; c < 3; ++c) {
        if (val->mask & (1 << c)) {
          int64_t x = m_bs.Read(val->W);
          if (val->isSigned) {
            x = SignExtend(x, val->W);
          }
          val->cur[c] = x;
        }
      }
    }
  }

  StreamInfo& m_info;
  BitStream m_bs;
  int64_t m_cur = 0;
};

// The rotation of one value at frame time t, as (w, x, y, z).
Quat TweenRot(const Value& val, int64_t t) {
  auto make = [&](const int64_t* ints, int negative) {
    const double scale = std::ldexp(1.0, -val.q);
    double c[3];
    double sum = 0.0;
    for (int i = 0; i < 3; ++i) {
      c[i] = F32(double(ints[i]) * scale);
      sum += c[i] * c[i];
    }
    const double w = std::sqrt(std::max(0.0, 1.0 - sum));
    return Quat{negative ? -w : w, c[0], c[1], c[2]};
  };
  const Quat prev = make(val.prev, val.sgnPrev);
  if (t == val.curFrame) {
    return prev;
  }
  const Quat cur = make(val.cur, val.sgnCur);
  const double frac = double(t - val.curFrame) * val.inv;
  if (val.flag7) {
    return RotLerp2(prev, cur, frac);
  }
  return Normalize(Lerp(prev, cur, frac));
}

// One entry of the op stream: which bone, and where its rotation, scale and
// translation come from (a track value, or an index into the constant pool).
struct Op {
  uint32_t bone;
  uint16_t rot, scale, trans;
};

std::vector<Op> ParseOps(const StreamInfo& info) {
  const Bytes& b = info.blob;
  size_t p = info.w2;
  std::vector<Op> out;
  auto byteAt = [&](size_t at) -> uint8_t {
    b.Need(at, 1, "op stream");
    return b.data[at];
  };
  for (;;) {
    const uint8_t op = byteAt(p++);
    if (op != 1) {
      break;  // 0 ends the list; the static animation ends in padding instead
    }
    p += 2;
    for (;;) {
      const uint8_t sub = byteAt(p++);
      if (sub == 0) {
        break;
      }
      if (sub != 2) {
        Fail("op stream sub-op " + std::to_string(sub) + " is not supported");
      }
      const uint32_t count = byteAt(p);
      b.Need(p + 1, 2, "op stream");
      const uint32_t hdr = ReadLE16(b.data + p + 1);
      p += 3;
      const uint32_t bone = (hdr >> 5) & 0x7ff;
      for (uint32_t k = 0; k < count; ++k) {
        b.Need(p, 6, "op stream");
        out.push_back({bone + k, ReadLE16(b.data + p), ReadLE16(b.data + p + 2), ReadLE16(b.data + p + 4)});
        p += 6;
      }
    }
  }
  return out;
}

// The pooled names: i32 size at 0x25, then NUL separated strings.
std::vector<std::string> ReadNames(const Bytes& d) {
  d.Need(0x25, 4, "name pool");
  const int32_t n = int32_t(ReadLE32(d.data + 0x25));
  if (n < 0 || !d.Has(0x29, size_t(n))) {
    Fail("name pool runs past the end of the file");
  }
  std::vector<std::string> names;
  std::string cur;
  for (int32_t i = 0; i < n; ++i) {
    const char c = char(d.data[0x29 + size_t(i)]);
    if (c == 0) {
      if (!cur.empty()) {
        names.push_back(cur);
      }
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if (!cur.empty()) {
    names.push_back(cur);
  }
  return names;
}

struct Record {
  size_t offset;
  uint32_t id;
  size_t end;
};

// Compressed animation records: 01 <id u32> <len u32> payload, the next record 12 + len on.
std::vector<Record> ReadRecords(const Bytes& d, size_t start) {
  std::vector<Record> recs;
  size_t o = start;
  while (d.Has(o, 9) && d.data[o] == 1) {
    const uint32_t id = ReadLE32(d.data + o + 1);
    const uint32_t len = ReadLE32(d.data + o + 5);
    if (id > 0xff || len == 0 || !d.Has(o, size_t(12) + len)) {
      break;
    }
    recs.push_back({o, id, o + 12 + len});
    o += 12 + len;
  }
  return recs;
}

// The longest chain of at least two records found by trying every start.
std::vector<Record> FindRecords(const Bytes& d) {
  std::vector<Record> best;
  for (size_t o = 0x100; d.Has(o, 9); ++o) {
    if (d.data[o] == 1 && d.data[o + 2] == 0 && d.data[o + 3] == 0 && d.data[o + 4] == 0) {
      std::vector<Record> r = ReadRecords(d, o);
      if (r.size() >= 2 && r.size() > best.size()) {
        best = std::move(r);
      }
    }
  }
  return best;
}

// The constant pool: u16 a, u16 n, n floats, ending shortly before the first record.
std::vector<double> FindPool(const Bytes& d, size_t first) {
  bool found = false;
  size_t bestEnd = 0;
  std::vector<double> pool;
  const size_t from = first >= 0x40 ? first - 0x40 : 0;
  for (size_t o = from; o + 6 < first; ++o) {
    const uint32_t a = ReadLE16(d.data + o);
    const uint32_t n = ReadLE16(d.data + o + 2);
    if (n == 0 || n > 64 || o + 4 + 4 * size_t(n) > first || a >= 16) {
      continue;
    }
    bool ok = true;
    for (uint32_t i = 0; i < n; ++i) {
      if (!(std::fabs(ReadLEFloat(d.data + o + 4 + 4 * size_t(i))) < 1e6f)) {
        ok = false;
        break;
      }
    }
    const size_t end = o + 4 + 4 * size_t(n);
    if (ok && (!found || end > bestEnd)) {
      if (n >= 4 && ReadLEFloat(d.data + o + 4) == 1.0f) {
        found = true;
        bestEnd = end;
        pool.clear();
        for (uint32_t i = 0; i < n; ++i) {
          pool.push_back(double(ReadLEFloat(d.data + o + 4 + 4 * size_t(i))));
        }
      }
    }
  }
  if (!found) {
    pool = {1.0, 0.0, 0.0, 0.0};
  }
  return pool;
}

void Evaluate(const StreamInfo& info, const std::vector<Op>& ops, const std::vector<double>& pool,
              int64_t t, std::vector<Key>& frameKeys, std::vector<bool>& has) {
  auto poolAt = [&](size_t index, size_t count) -> const double* {
    if (index > pool.size() || count > pool.size() - index) {
      Fail("constant pool index out of range");
    }
    return pool.data() + index;
  };
  for (const Op& op : ops) {
    Quat q;  // w x y z
    if (op.rot & 0x8000) {
      const size_t track = (op.rot >> 10) & 0xf;
      const size_t index = op.rot & 0x3ff;
      if (track >= info.tracks.size() || index >= info.tracks[track].values.size()) {
        Fail("rotation names a track value that does not exist");
      }
      q = TweenRot(info.tracks[track].values[index], t);
    } else {
      const double* p = poolAt(op.rot & 0x3fff, 4);
      q = {p[0], p[1], p[2], p[3]};
    }
    if (op.scale & 0x8000) {
      Fail("scale tracks are not supported");
    }
    double sc[3];
    if (op.scale & 0x4000) {
      sc[0] = sc[1] = sc[2] = *poolAt(op.scale & 0x3fff, 1);
    } else {
      const double* p = poolAt(op.scale & 0x3fff, 3);
      sc[0] = p[0];
      sc[1] = p[1];
      sc[2] = p[2];
    }
    if (op.trans & 0x8000) {
      Fail("translation tracks are not supported");
    }
    const double* tr = poolAt(op.trans & 0x3fff, 3);
    Key key;
    key.rotation[0] = float(q[1]);
    key.rotation[1] = float(q[2]);
    key.rotation[2] = float(q[3]);
    key.rotation[3] = float(q[0]);
    for (int i = 0; i < 3; ++i) {
      key.scale[i] = float(F32(sc[i] + 1.0));  // stored as a delta from one
      key.translation[i] = float(tr[i]);
    }
    frameKeys[op.bone] = key;
    has[op.bone] = true;
  }
}

Anim DecodeAnim(const Bytes& d, const Record& rec, const std::vector<double>& pool) {
  StreamInfo info = ParseBlob(d, rec.offset + 5);
  InitDequant(info);
  Stream stream(info);
  const std::vector<Op> ops = ParseOps(info);
  stream.InitFrames();

  Anim anim;
  anim.fps = info.headerFloat;
  anim.frames = info.frames;
  uint32_t boneCount = 0;
  for (const Op& op : ops) {
    boneCount = std::max(boneCount, op.bone + 1);
  }
  anim.bones.assign(boneCount, std::vector<Key>());
  for (std::vector<Key>& keys : anim.bones) {
    keys.reserve(info.frames);
  }
  std::vector<Key> frameKeys(boneCount);
  std::vector<bool> has(boneCount);
  for (uint32_t f = 0; f < info.frames; ++f) {
    const int64_t t = int64_t(f) * int64_t(info.v);
    while (stream.Cur() < t) {
      stream.DequantFrame();
    }
    std::fill(has.begin(), has.end(), false);
    Evaluate(info, ops, pool, t, frameKeys, has);
    for (uint32_t b = 0; b < boneCount; ++b) {
      anim.bones[b].push_back(has[b] ? frameKeys[b] : Key());
    }
  }
  return anim;
}

bool StartsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

}  // namespace

bool ReadCharacter(const std::vector<uint8_t>& chpr, Character& out, std::string& error) {
  out = Character();
  try {
    const Bytes d{chpr.data(), chpr.size()};
    if (!d.Has(0, 0x29) || std::memcmp(d.data, "RFRM", 4) != 0 || std::memcmp(d.data + 0x14, "CHPR", 4) != 0) {
      Fail("not a CHPR file");
    }
    // Animation names are the pool strings with the usual prefixes, in record order. Unless
    // there is one per record, which is which is not known: the animations go unnamed, so
    // Find matches none of them.
    std::vector<std::string> animNames;
    for (const std::string& n : ReadNames(d)) {
      if (StartsWith(n, "spin_") || StartsWith(n, "static") || StartsWith(n, "idle") || StartsWith(n, "anim")) {
        animNames.push_back(n);
      }
    }
    const std::vector<Record> recs = FindRecords(d);
    if (recs.empty()) {
      Fail("no animation records found");
    }
    const std::vector<double> pool = FindPool(d, recs[0].offset);
    for (size_t i = 0; i < recs.size(); ++i) {
      Anim anim = DecodeAnim(d, recs[i], pool);
      anim.name = animNames.size() == recs.size() ? animNames[i] : std::string();
      out.anims.push_back(std::move(anim));
    }
    // After the last record come a few fixed fields, a 16 byte reference (not the
    // skinned model) and then the SMDL reference, 0x22 bytes past the last record.
    const size_t smdl = recs.back().end + 0x22;
    if (d.Has(smdl, 16)) {
      std::copy(d.data + smdl, d.data + smdl + 16, out.skinnedModel.begin());
    }
  } catch (const Failure& failure) {
    error = failure.message;
    out = Character();
    return false;
  }
  return true;
}

const Anim* Find(const Character& c, std::string_view name) {
  for (const Anim& a : c.anims) {
    if (!a.name.empty() && a.name == name) {
      return &a;
    }
  }
  return nullptr;
}

}  // namespace PortRemasteredAnim
