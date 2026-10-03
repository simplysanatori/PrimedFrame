#include "port_room_env.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
int sFailures = 0;

void Check(bool condition, const char* what) {
  if (!condition) {
    std::fprintf(stderr, "FAIL: %s\n", what);
    ++sFailures;
  }
}

void Put32(std::vector<uint8_t>& out, uint32_t value) {
  for (int i = 0; i < 4; ++i) {
    out.push_back(uint8_t(value >> (i * 8)));
  }
}

void PutFloat(std::vector<uint8_t>& out, float value) {
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof(bits));
  Put32(out, bits);
}

// An axis-aligned box probe: centre and half extents.
void PutProbe(std::vector<uint8_t>& out, float cx, float cy, float cz, float hx, float hy, float hz, uint32_t cube) {
  const float centre[3] = {cx, cy, cz};
  const float half[3] = {hx, hy, hz};
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 3; ++col) {
      PutFloat(out, row == col ? 1.f / half[row] : 0.f);
    }
    PutFloat(out, -centre[row] / half[row]);
  }
  for (int i = 0; i < 9; ++i) {
    PutFloat(out, i % 4 == 0 ? 1.f : 0.f);
  }
  Put32(out, 0);
  Put32(out, cube);
  PutFloat(out, 1.f);
  PutFloat(out, 0.05f);
}

void PutCube(std::vector<uint8_t>& out, uint32_t size, uint32_t mips, uint8_t fill) {
  const size_t bytes = PortRoomEnv::CubeBytes(size, mips);
  Put32(out, size);
  Put32(out, mips);
  Put32(out, 0);
  Put32(out, uint32_t(bytes));
  out.insert(out.end(), bytes, fill);
}

std::vector<uint8_t> MakeFile(uint32_t version = 1) {
  std::vector<uint8_t> out = {'M', 'P', 'E', 'V'};
  Put32(out, version);
  PutFloat(out, 4.f);
  PutFloat(out, 0.18f);
  PutFloat(out, 0.6f);
  PutFloat(out, 0.15f);
  Put32(out, 3);
  Put32(out, 2);
  PutProbe(out, 0.f, 0.f, 0.f, 10.f, 10.f, 10.f, 0);  // the room
  PutProbe(out, 2.f, 0.f, 0.f, 1.f, 1.f, 1.f, 1);     // an alcove in it
  PutProbe(out, 40.f, 0.f, 0.f, 5.f, 5.f, 5.f, 0);    // next door
  PutCube(out, 8, 4, 0x11);
  PutCube(out, 4, 3, 0x22);
  return out;
}

// A grid point: grey light `level` (1.0 as a half is 0x3C00), with red arriving from +x of
// the grid, green from +y and blue from +z.
void PutPoint(std::vector<uint8_t>& out, uint16_t level) {
  for (int i = 0; i < 3; ++i) {
    out.push_back(uint8_t(level));
    out.push_back(uint8_t(level >> 8));
  }
  for (int i = 0; i < 3; ++i) {
    out.push_back(0x00);
    out.push_back(0x38); // 0.5
  }
  out.insert(out.end(), {255, 0, 51});
  for (int channel = 0; channel < 3; ++channel) {
    for (int axis = 0; axis < 3; ++axis) {
      out.push_back(channel == axis ? 255 : 128);
    }
  }
}

// Version 2: the same, then a 3 x 2 x 2 grid of 2 m cells whose point (0, 0, 0) is at world
// (10, 20, 30), with grid x along world y, y along world -x and z along world z. The
// points of the grid's x = 2 are empty, and those of x = 1 twice as bright as x = 0.
std::vector<uint8_t> MakeGridFile() {
  std::vector<uint8_t> out = MakeFile(2);
  Put32(out, 1);
  const float rows[12] = {0.f, 0.5f, 0.f, -10.f, -0.5f, 0.f, 0.f, 5.f, 0.f, 0.f, 0.5f, -15.f};
  for (float value : rows) {
    PutFloat(out, value);
  }
  Put32(out, 3);
  Put32(out, 2);
  Put32(out, 2);
  for (int i = 0; i < 12; ++i) {
    PutPoint(out, i % 3 == 0 ? 0x3C00 : i % 3 == 1 ? 0x4000 : 0);
  }
  return out;
}

bool Near(float a, float b) { return a > b - 0.01f && a < b + 0.01f; }

void TestGrid() {
  PortRoomEnv::File file;
  std::string error;
  const std::vector<uint8_t> good = MakeGridFile();
  Check(PortRoomEnv::Parse(std::vector<uint8_t>(good), file, error), "grid: parse");
  Check(file.grids.size() == 1 && file.probes.size() == 3, "grid: counts");
  if (file.grids.size() != 1) {
    return;
  }
  const PortRoomEnv::Grid& grid = file.grids[0];
  Check(grid.size[0] == 3 && grid.size[1] == 2 && grid.size[2] == 2 && Near(grid.average, 1.414f), "grid: header");
  const size_t before = good.size() - 12 * 24 - 64;
  for (size_t length = before; length < good.size(); ++length) {
    Check(!PortRoomEnv::Parse(std::vector<uint8_t>(good.begin(), good.begin() + length), file, error), "grid: cut short");
  }
  std::vector<uint8_t> bad = good;
  bad[before + 4 + 48] = 0;
  Check(!PortRoomEnv::Parse(std::vector<uint8_t>(bad), file, error), "grid: size");
  bad = good;
  bad[before] = 200;
  Check(!PortRoomEnv::Parse(std::vector<uint8_t>(bad), file, error), "grid: count");
  Check(PortRoomEnv::Parse(MakeFile(), file, error) && file.grids.empty(), "grid: version 1 has none");
  Check(PortRoomEnv::Parse(std::vector<uint8_t>(good), file, error), "grid: parse again");
  Check(file.exposure[0] == 0.f && file.exposure[1] == 0.f, "grid: version 2 has no exposure range");

  // Version 3 ends with the exposure range.
  std::vector<uint8_t> v3 = good;
  v3[4] = 3;
  Check(!PortRoomEnv::Parse(std::vector<uint8_t>(v3), file, error), "exposure: cut short");
  PutFloat(v3, 5.5f);
  PutFloat(v3, 6.f);
  Check(PortRoomEnv::Parse(std::vector<uint8_t>(v3), file, error) && file.exposure[0] == 5.5f && file.exposure[1] == 6.f,
        "exposure: range");
  PutFloat(v3, 0.f);
  std::memcpy(v3.data() + v3.size() - 8, v3.data() + v3.size() - 4, 4); // highest below lowest
  v3.resize(v3.size() - 4);
  Check(PortRoomEnv::Parse(std::vector<uint8_t>(v3), file, error) && file.exposure[0] == 0.f && file.exposure[1] == 0.f,
        "exposure: a backwards range is none");

  // Version 4 follows the range with the exposure bias and the curve's contrast.
  std::vector<uint8_t> v4 = v3;
  v4[4] = 4;
  Check(!PortRoomEnv::Parse(std::vector<uint8_t>(v4), file, error), "tone: cut short");
  PutFloat(v4, 1.5f);
  PutFloat(v4, 0.4f);
  Check(PortRoomEnv::Parse(std::vector<uint8_t>(v4), file, error) && file.exposureBias == 1.5f && file.contrast == 0.4f,
        "tone: bias and contrast");
  v4.resize(v4.size() - 4);
  PutFloat(v4, 7.f);
  Check(PortRoomEnv::Parse(std::vector<uint8_t>(v4), file, error) && file.contrast == 0.f,
        "tone: a contrast out of range is none");

  // Version 5 adds the bloom: a threshold and a count of RGBA tints.
  std::vector<uint8_t> v5 = v4;
  v5[4] = 5;
  Check(!PortRoomEnv::Parse(std::vector<uint8_t>(v5), file, error), "bloom: cut short");
  PutFloat(v5, 0.5f);
  Put32(v5, 2);
  Check(!PortRoomEnv::Parse(std::vector<uint8_t>(v5), file, error), "bloom: tints cut short");
  for (int i = 0; i < 8; ++i) {
    PutFloat(v5, float(i));
  }
  Check(PortRoomEnv::Parse(std::vector<uint8_t>(v5), file, error) && file.bloomThreshold == 0.5f &&
            file.bloomTints.size() == 8 && file.bloomTints[6] == 6.f,
        "bloom: threshold and tints");
  Check(PortRoomEnv::Parse(std::vector<uint8_t>(v4), file, error) && file.bloomTints.empty(),
        "bloom: version 4 has none");

  // Version 6 adds the colour grades: layer, fades, a 33^3 RGBA8 LUT each.
  std::vector<uint8_t> v6 = v5;
  v6[4] = 6;
  Check(!PortRoomEnv::Parse(std::vector<uint8_t>(v6), file, error), "grade: cut short");
  Put32(v6, 2);
  for (int g = 0; g < 2; ++g) {
    Put32(v6, g == 0 ? uint32_t(-1) : 3u);
    PutFloat(v6, g == 0 ? 1.f : 1000.f);
    PutFloat(v6, 0.5f);
    for (uint32_t z = 0; z < 33; ++z) {
      for (uint32_t y = 0; y < 33; ++y) {
        for (uint32_t x = 0; x < 33; ++x) {
          // The first is the identity, the second warms it.
          const uint8_t px[4] = {uint8_t(std::min(255u, x * 255 / 32 + (g == 1 ? 20u : 0u))), uint8_t(y * 255 / 32),
                                 uint8_t(z * 255 / 32), 255};
          v6.insert(v6.end(), px, px + 4);
        }
      }
    }
  }
  Check(PortRoomEnv::Parse(std::vector<uint8_t>(v6), file, error) && file.grades.size() == 2 &&
            file.grades[0].layer == -1 && file.grades[0].fadeIn == 1.f && file.grades[0].id == 0 &&
            file.grades[1].layer == 3 && file.grades[1].fadeIn == 0.f && file.grades[1].fadeOut == 0.5f &&
            file.grades[1].id != 0,
        "grade: layers, fades, the identity's id is 0");
  Check(file.exposureSigma == 32.f && file.staticLerp == 0.5f, "convergence: version 6 has the defaults");

  // Version 7 adds the exposure's convergence sigma and the static exposure's lerp.
  std::vector<uint8_t> v7 = v6;
  v7[4] = 7;
  Check(!PortRoomEnv::Parse(std::vector<uint8_t>(v7), file, error), "convergence: cut short");
  PutFloat(v7, 12.f);
  PutFloat(v7, 0.25f);
  Check(PortRoomEnv::Parse(std::vector<uint8_t>(v7), file, error) && file.exposureSigma == 12.f &&
            file.staticLerp == 0.25f,
        "convergence: sigma and lerp");
  v7.resize(v7.size() - 8);
  PutFloat(v7, -1.f);
  PutFloat(v7, 2.f);
  Check(PortRoomEnv::Parse(std::vector<uint8_t>(v7), file, error) && file.exposureSigma == 32.f &&
            file.staticLerp == 0.5f,
        "convergence: out of range values fall back");

  // Version 8 adds a probe's priority and intensity range after its padding.
  std::vector<uint8_t> v8 = v7;
  v8[4] = 8;
  for (int i = 2; i >= 0; --i) {
    std::vector<uint8_t> extra;
    Put32(extra, uint32_t(10 + i));
    PutFloat(extra, 0.25f);
    PutFloat(extra, 2.f);
    v8.insert(v8.begin() + 32 + 100 * (i + 1), extra.begin(), extra.end());
  }
  Check(PortRoomEnv::Parse(std::vector<uint8_t>(v8), file, error) && file.probes.size() == 3 &&
            file.probes[1].padding == 0.05f && file.probes[1].priority == 11 && file.probes[2].intensityMin == 0.25f &&
            file.probes[0].intensityMax == 2.f && file.exposureSigma == 32.f && file.grids.size() == 1,
        "probe: version 8 record");
  Check(PortRoomEnv::Parse(std::vector<uint8_t>(v7), file, error) && file.probes[1].padding == 1.f &&
            file.probes[1].priority == 0 && file.probes[1].intensityMin == 0.f && file.probes[1].intensityMax == 1.f,
        "probe: version 7 has the defaults");

  {
    PortRoomEnv::Convergence c;
    c.SetSigma(32.f);
    c.SetValue(4.f);
    for (int i = 0; i < 100; ++i) {
      c.Step(4.f);
    }
    Check(Near(c.value, 4.f), "convergence: a constant stays put");
    // The recursive filter's step response is an S curve with a small ripple near the top
    // (~0.004 in 6, around frame 130), never past the target.
    float prev = c.value;
    bool smooth = true;
    float atSigma = 0.f;
    for (int i = 0; i < 600; ++i) {
      c.Step(10.f);
      smooth = smooth && c.value >= prev - 0.01f && c.value <= 10.f + 1e-3f;
      prev = c.value;
      if (i == 31) {
        atSigma = c.value;
      }
    }
    Check(smooth && atSigma > 5.f && atSigma < 9.f && Near(c.value, 10.f), "convergence: a step settles on the target");
    c.SetValue(-2.f);
    Check(c.value == -2.f, "convergence: SetValue snaps");
    c.Step(-2.f);
    Check(Near(c.value, -2.f), "convergence: snapped history");
  }
  v6.pop_back();
  Check(!PortRoomEnv::Parse(std::vector<uint8_t>(v6), file, error), "grade: LUT cut short");
  Check(PortRoomEnv::Parse(std::vector<uint8_t>(good), file, error), "grid: parse once more");

  PortRoomEnv::Ambient a;
  const float atPoint[3] = {10.f, 20.f, 30.f};
  Check(PortRoomEnv::SampleGrid(file, file.grids[0], atPoint, a), "grid: at a point");
  Check(Near(a.mean[0], 1.f) && Near(a.lobe[1], 0.5f) && Near(a.sharpness[0], 1.f) && Near(a.sharpness[1], 0.f) &&
            Near(a.sharpness[2], 0.2f),
        "grid: the point's values");
  // Red comes from grid +x, which is world +y; green from grid +y, world -x.
  Check(Near(a.direction[0][0], 0.f) && Near(a.direction[0][1], 1.f) && Near(a.direction[0][2], 0.f), "grid: red's direction");
  Check(Near(a.direction[1][0], -1.f) && Near(a.direction[1][1], 0.f), "grid: green's direction");
  Check(Near(a.direction[2][2], 1.f), "grid: blue's direction");
  const float halfway[3] = {9.f, 21.f, 31.f}; // grid (0.5, 0.5, 0.5)
  Check(PortRoomEnv::SampleGrid(file, file.grids[0], halfway, a) && Near(a.mean[0], 1.5f), "grid: between points");
  const float byEmpty[3] = {10.f, 23.5f, 30.f}; // grid x 1.75: the empty point does not darken it
  Check(PortRoomEnv::SampleGrid(file, file.grids[0], byEmpty, a) && Near(a.mean[0], 2.f), "grid: next to an empty point");
  const float inEmpty[3] = {10.f, 24.f, 30.f};
  Check(!PortRoomEnv::SampleGrid(file, file.grids[0], inEmpty, a), "grid: an empty point");
  const float edge[3] = {10.f, 19.5f, 29.5f}; // a quarter cell outside two faces
  Check(PortRoomEnv::SampleGrid(file, file.grids[0], edge, a) && Near(a.mean[0], 1.f), "grid: just outside");
  const float outside[3] = {10.f, 18.f, 30.f};
  Check(!PortRoomEnv::SampleGrid(file, file.grids[0], outside, a), "grid: outside");
}

void TestNames() {
  uint32_t id = 0;
  Check(PortRoomEnv::ParseFileName("d1241219.RoomEnv", id) && id == 0xD1241219, "file name");
  Check(!PortRoomEnv::ParseFileName("D1241219.roomenvs", id), "file name: longer suffix");
  Check(!PortRoomEnv::ParseFileName("D124121G.roomenv", id), "file name: bad hex");
  Check(!PortRoomEnv::ParseFileName("D1241219.TXTR", id), "file name: a resource");
  Check(!PortRoomEnv::ParseFileName("", id), "file name: empty");
}

void TestParse() {
  Check(PortRoomEnv::CubeBytes(8, 4) == size_t(4 + 1 + 1 + 1) * 16 * 6, "cube bytes");
  PortRoomEnv::File file;
  std::string error;
  const std::vector<uint8_t> good = MakeFile();
  Check(PortRoomEnv::Parse(std::vector<uint8_t>(good), file, error), "parse");
  Check(file.probes.size() == 3 && file.cubes.size() == 2, "parse: counts");
  Check(file.tonemap[1] == 0.18f, "parse: tonemap");
  Check(file.probes[1].cube == 1 && file.probes[1].padding == 1.f, "parse: probe");
  Check(file.cubes[1].size == 4 && file.cubes[1].mipCount == 3 && file.data[file.cubes[1].offset] == 0x22 &&
            file.data[file.cubes[0].offset + file.cubes[0].length - 1] == 0x11,
        "parse: cubes");

  // Every way of cutting the file short fails, and none reads past the end.
  for (size_t length = 0; length < good.size(); length += length < 400 ? 1 : 97) {
    Check(!PortRoomEnv::Parse(std::vector<uint8_t>(good.begin(), good.begin() + length), file, error), "parse: cut short");
  }
  std::vector<uint8_t> bad = good;
  bad[4] = 6;
  Check(!PortRoomEnv::Parse(std::vector<uint8_t>(bad), file, error), "parse: version");
  bad = good;
  bad[32 + 88] = 2;
  Check(!PortRoomEnv::Parse(std::vector<uint8_t>(bad), file, error), "parse: cube index");
  bad = good;
  bad[32 + 300] = 6; // the first cube's size
  Check(!PortRoomEnv::Parse(std::vector<uint8_t>(bad), file, error), "parse: cube size");
  bad = good;
  bad[32 + 2] = 0xC0;
  bad[32 + 3] = 0x7F; // NaN
  Check(!PortRoomEnv::Parse(std::vector<uint8_t>(bad), file, error), "parse: not finite");
  bad = good;
  bad[24] = 0xFF;
  bad[25] = 0xFF;
  bad[26] = 0xFF;
  bad[27] = 0xFF;
  Check(!PortRoomEnv::Parse(std::vector<uint8_t>(bad), file, error), "parse: probe count");
}

void TestPick() {
  PortRoomEnv::File file;
  std::string error;
  Check(PortRoomEnv::Parse(MakeFile(), file, error), "pick: parse");
  const float inRoom[3] = {-5.f, 0.f, 0.f};
  const float inAlcove[3] = {2.5f, 0.5f, 0.f};
  const float between[3] = {30.f, 0.f, 0.f}; // 20 from the room, 5 from next door
  const float nearRoom[3] = {12.f, 0.f, 0.f};
  PortRoomEnv::Pick pick = PortRoomEnv::PickProbe(file, inRoom);
  Check(pick.probe == 0 && pick.inside, "pick: inside");
  pick = PortRoomEnv::PickProbe(file, inAlcove);
  Check(pick.probe == 1 && pick.inside && pick.score > 7.99f && pick.score < 8.01f, "pick: the smaller of two boxes");
  pick = PortRoomEnv::PickProbe(file, between);
  Check(pick.probe == 2 && !pick.inside && pick.score > 4.99f && pick.score < 5.01f, "pick: the nearest box");
  pick = PortRoomEnv::PickProbe(file, nearRoom);
  Check(pick.probe == 0 && !pick.inside, "pick: outside, nearest");

  PortRoomEnv::Pick none;
  Check(!none.Better(pick) && pick.Better(none), "pick: against none");
  Check(PortRoomEnv::PickProbe(PortRoomEnv::File(), inRoom).probe < 0, "pick: no probes");
}

// PickProbe as it was before the half extents were worked out at parse time: the same pick,
// bit for bit, is what the stored extents must give.
PortRoomEnv::Pick PickProbeReference(const PortRoomEnv::File& file, const float pos[3]) {
  PortRoomEnv::Pick best;
  for (size_t i = 0; i < file.probes.size(); ++i) {
    const float* m = file.probes[i].worldToBox;
    PortRoomEnv::Pick pick;
    pick.probe = int(i);
    pick.inside = true;
    float volume = 8.f;
    float distance2 = 0.f;
    for (int row = 0; row < 3; ++row) {
      const float* r = m + row * 4;
      const float u = r[0] * pos[0] + r[1] * pos[1] + r[2] * pos[2] + r[3];
      const float scale = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
      const float half = scale > 1e-12f ? 1.f / scale : 0.f;
      volume *= half;
      const float over = std::fabs(u) - 1.f;
      if (!(over <= 0.f)) {
        pick.inside = false;
        distance2 += over * half * over * half;
      }
    }
    pick.score = pick.inside ? volume : std::sqrt(distance2);
    if (pick.Better(best)) {
      best = pick;
    }
  }
  return best;
}

void TestPickExact() {
  // Turned, stretched and overlapping boxes (and one squashed to nothing along a row), each
  // written out as a file and parsed, then picked from at points in and around them.
  uint32_t seed = 12345;
  const auto random = [&seed](float lo, float hi) {
    seed = seed * 1664525u + 1013904223u;
    return lo + (hi - lo) * float(seed >> 8) / float(1u << 24);
  };
  constexpr uint32_t kProbes = 24;
  std::vector<uint8_t> data = {'M', 'P', 'E', 'V'};
  Put32(data, 1);
  for (int i = 0; i < 4; ++i) {
    PutFloat(data, 0.5f);
  }
  Put32(data, kProbes);
  Put32(data, 1);
  for (uint32_t probe = 0; probe < kProbes; ++probe) {
    const float angle = random(0.f, 6.2831853f);
    const float c = std::cos(angle), s = std::sin(angle);
    const float half[3] = {random(0.3f, 30.f), random(0.3f, 30.f), probe == 5 ? 0.f : random(0.3f, 30.f)};
    const float centre[3] = {random(-50.f, 50.f), random(-50.f, 50.f), random(-20.f, 20.f)};
    const float axes[3][3] = {{c, s, 0.f}, {-s, c, 0.f}, {0.f, 0.f, 1.f}};
    for (int row = 0; row < 3; ++row) {
      const float inverse = half[row] > 0.f ? 1.f / half[row] : 0.f;
      float offset = 0.f;
      for (int col = 0; col < 3; ++col) {
        PutFloat(data, axes[row][col] * inverse);
        offset -= axes[row][col] * inverse * centre[col];
      }
      PutFloat(data, offset);
    }
    for (int i = 0; i < 9; ++i) {
      PutFloat(data, i % 4 == 0 ? 1.f : 0.f);
    }
    Put32(data, 0);
    Put32(data, 0);
    PutFloat(data, 1.f);
    PutFloat(data, 0.05f);
  }
  PutCube(data, 4, 3, 0x11);
  PortRoomEnv::File file;
  std::string error;
  Check(PortRoomEnv::Parse(std::move(data), file, error), "pick exact: parse");
  int mismatches = 0;
  int inside = 0;
  for (int i = 0; i < 20000; ++i) {
    const float pos[3] = {random(-90.f, 90.f), random(-90.f, 90.f), random(-40.f, 40.f)};
    const PortRoomEnv::Pick a = PortRoomEnv::PickProbe(file, pos);
    const PortRoomEnv::Pick b = PickProbeReference(file, pos);
    if (a.probe != b.probe || a.inside != b.inside || std::memcmp(&a.score, &b.score, sizeof(a.score)) != 0) {
      ++mismatches;
    }
    inside += a.inside ? 1 : 0;
  }
  Check(mismatches == 0, "pick exact: same picks as before");
  Check(inside > 100 && inside < 19900, "pick exact: points both in and out of boxes");
  Check(file.probes[5].half[2] == 0.f && file.probes[5].volume == 0.f, "pick exact: a flat box has no volume");
}

// An axis-aligned probe along x: centre, half extent (all three axes).
PortRoomEnv::Probe BoxProbe(float cx, float half, int32_t priority, float scale, float padding) {
  PortRoomEnv::Probe p{};
  for (int row = 0; row < 3; ++row) {
    for (int col = 0; col < 4; ++col) {
      p.worldToBox[row * 4 + col] = row == col ? 1.f / half : 0.f;
    }
  }
  p.worldToBox[3] = -cx / half;
  p.scale = scale;
  p.padding = padding;
  p.priority = priority;
  p.intensityMin = 0.1f * scale;
  p.intensityMax = 2.f * scale;
  return p;
}

void TestBlend() {
  bool inside = false;
  const PortRoomEnv::Probe room = BoxProbe(0.f, 10.f, 0, 2.f, 4.f);
  const float centre[3] = {0.f, 0.f, 0.f};
  const float out1[3] = {11.f, 0.f, 0.f};
  const float corner[3] = {11.f, 13.f, 0.f}; // 1 m out in x, 3 m in y: the farthest counts
  const float far[3] = {20.f, 0.f, 0.f};
  Check(PortRoomEnv::ProbeFade(room, centre, inside) == 1.f && inside, "fade: inside");
  Check(Near(PortRoomEnv::ProbeFade(room, out1, inside), 0.75f) && !inside, "fade: in the padding");
  Check(Near(PortRoomEnv::ProbeFade(room, corner, inside), 0.25f), "fade: farthest axis");
  Check(PortRoomEnv::ProbeFade(room, far, inside) == 0.f, "fade: past the padding");
  PortRoomEnv::Probe tight = room;
  tight.padding = 0.f;
  Check(PortRoomEnv::ProbeFade(tight, out1, inside) == 0.f && PortRoomEnv::ProbeFade(tight, centre, inside) == 1.f,
        "fade: no padding");

  // A small high-priority probe inside the room, and a room next door.
  const PortRoomEnv::Probe alcove = BoxProbe(5.f, 1.f, 1, 1.f, 2.f);
  const PortRoomEnv::Probe nextDoor = BoxProbe(22.f, 10.f, 0, 4.f, 4.f);
  const PortRoomEnv::BlendCandidate all[3] = {{1, &room}, {2, &alcove}, {3, &nextDoor}};
  PortRoomEnv::Blend blend;
  PortRoomEnv::UpdateBlend(all, 3, centre, blend);
  Check(blend.entries.size() == 1 && blend.entries[0].key == 1 && blend.entries[0].weight == 1.f &&
            blend.intensity == 2.f && Near(blend.min, 0.2f) && blend.max == 4.f,
        "blend: one probe keeps its own values");

  // 1 m outside the alcove: it takes half, the room (which holds the point) the rest.
  const float byAlcove[3] = {7.f, 0.f, 0.f};
  PortRoomEnv::UpdateBlend(all, 3, byAlcove, blend);
  Check(blend.entries.size() == 2 && blend.entries[0].key == 2 && blend.entries[1].key == 1, "blend: priority first");
  // Raw weights 0.5 and 0.5; intensity 0.5 * 1 + 0.5 * 2.
  Check(Near(blend.intensity, 1.5f) && Near(blend.entries[0].weight, 1.f / 3.f) &&
            Near(blend.entries[1].weight, 2.f / 3.f) && Near(blend.min, 0.15f) && Near(blend.max, 3.f),
        "blend: weights by share and intensity");

  // Between the rooms (both 1 m outside, padding 4): 0.75 of the first, 0.25 * 0.75 of the
  // second. The room was listed last frame, so it stays first.
  const float between[3] = {11.f, 0.f, 0.f};
  PortRoomEnv::UpdateBlend(all, 3, between, blend);
  Check(blend.entries.size() == 2 && blend.entries[0].key == 1 && blend.entries[1].key == 3, "blend: old first on ties");
  const float sum = 0.75f * 2.f + 0.1875f * 4.f;
  Check(Near(blend.intensity, sum) && Near(blend.entries[0].weight, 1.5f / sum), "blend: fading out of two rooms");

  // Swapped order with no history: the candidates' order decides.
  PortRoomEnv::Blend fresh;
  const PortRoomEnv::BlendCandidate swapped[2] = {{3, &nextDoor}, {1, &room}};
  PortRoomEnv::UpdateBlend(swapped, 2, between, fresh);
  Check(fresh.entries.size() == 2 && fresh.entries[0].key == 3, "blend: new ones in the candidates' order");

  // A probe that left the candidates (its area unloaded) drops out.
  PortRoomEnv::UpdateBlend(all + 2, 1, between, blend);
  Check(blend.entries.size() == 1 && blend.entries[0].key == 3 && blend.intensity == 4.f, "blend: unloaded probe");
  PortRoomEnv::UpdateBlend(all, 3, far, blend);
  Check(blend.entries.size() == 1 && blend.entries[0].key == 3, "blend: inside next door");
  const float nowhere[3] = {100.f, 0.f, 0.f};
  PortRoomEnv::UpdateBlend(all, 3, nowhere, blend);
  Check(blend.entries.empty() && blend.intensity == 1.f && blend.min == 0.f && blend.max == 1.f, "blend: none");

  // At most four, by priority.
  std::vector<PortRoomEnv::Probe> many;
  for (int i = 0; i < 6; ++i) {
    many.push_back(BoxProbe(0.f, 10.f + float(i), i, 1.f, 10.f));
  }
  std::vector<PortRoomEnv::BlendCandidate> manyCandidates;
  for (int i = 0; i < 6; ++i) {
    manyCandidates.push_back({uint64_t(100 + i), &many[i]});
  }
  PortRoomEnv::Blend capped;
  // Outside every box; the smallest is out of reach, the next four of five fade 0.5..0.2.
  const float edge[3] = {20.f, 0.f, 0.f};
  PortRoomEnv::UpdateBlend(manyCandidates.data(), manyCandidates.size(), edge, capped);
  Check(capped.entries.size() == 4 && capped.entries[0].key == 105 && capped.entries[3].key == 102,
        "blend: four, by priority");
  // The point inside the top probe: it takes everything.
  const float edgeIn[3] = {14.f, 0.f, 0.f};
  PortRoomEnv::UpdateBlend(manyCandidates.data(), manyCandidates.size(), edgeIn, capped);
  Check(capped.entries.size() == 1 && capped.entries[0].key == 105, "blend: the top probe holds the point");
}
} // namespace

int main() {
  TestNames();
  TestParse();
  TestPick();
  TestPickExact();
  TestBlend();
  TestGrid();
  if (sFailures != 0) {
    std::fprintf(stderr, "%d failure(s)\n", sFailures);
    return 1;
  }
  std::puts("port_room_env tests passed");
  return 0;
}
