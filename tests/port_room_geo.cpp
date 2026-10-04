#include "port_room_geo.h"

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

PortRoomGeo::Instance MakeInstance(uint32_t model, float x) {
  PortRoomGeo::Instance instance;
  instance.model = model;
  instance.transform[0] = instance.transform[5] = instance.transform[10] = 1.f;
  instance.transform[3] = x;
  return instance;
}

void TestRoundTrip() {
  std::vector<PortRoomGeo::Instance> in;
  in.push_back(MakeInstance(0x11111111, 1.f));
  PortRoomGeo::Instance gated = MakeInstance(0x22222222, -2.5f);
  gated.layer = 3;
  gated.active = false;
  gated.links.push_back({0x0c1a0042, 9, PortRoomGeo::kShow});
  gated.links.push_back({0x001a0043, 10, PortRoomGeo::kToggle});
  gated.platform = 0x041a0044;
  gated.platformStart[0] = -142.f;
  gated.platformStart[2] = 3.25f;
  in.push_back(gated);
  in.push_back(MakeInstance(0x33333333, 0.f));

  std::vector<PortRoomGeo::Instance> out;
  std::string error;
  const std::vector<uint8_t> file = PortRoomGeo::Write(in);
  Check(PortRoomGeo::Parse(file, out, error), "v3 file parses");
  Check(out.size() == 3, "three instances");
  if (out.size() != 3) {
    return;
  }
  Check(out[0].layer == PortRoomGeo::kEveryLayer && out[0].active && out[0].links.empty(), "plain instance");
  Check(out[1].model == 0x22222222 && out[1].transform[3] == -2.5f, "model and transform");
  Check(out[1].layer == 3 && !out[1].active, "layer and active");
  Check(out[0].platform == 0 && out[1].platform == 0x041a0044 && out[1].platformStart[0] == -142.f &&
            out[1].platformStart[1] == 0.f && out[1].platformStart[2] == 3.25f,
        "platform");
  Check(out[1].links.size() == 2, "two links");
  if (out[1].links.size() == 2) {
    Check(out[1].links[0].sender == 0x0c1a0042 && out[1].links[0].state == 9 &&
              out[1].links[0].action == PortRoomGeo::kShow,
          "first link");
    Check(out[1].links[1].action == PortRoomGeo::kToggle, "second link");
  }
  Check(out[2].model == 0x33333333, "instance after links");

  for (size_t cut = 12; cut < file.size(); ++cut) {
    const std::vector<uint8_t> part(file.begin(), file.begin() + cut);
    if (PortRoomGeo::Parse(part, out, error)) {
      std::fprintf(stderr, "FAIL: truncated at %zu parses\n", cut);
      ++sFailures;
    }
  }
}

void TestVersion1() {
  std::vector<uint8_t> file;
  Put32(file, 0x4752504D);
  Put32(file, 1);
  Put32(file, 2);
  for (uint32_t i = 0; i < 2; ++i) {
    Put32(file, 0xabc00000 + i);
    for (int j = 0; j < 12; ++j) {
      Put32(file, 0);
    }
  }
  std::vector<PortRoomGeo::Instance> out;
  std::string error;
  Check(PortRoomGeo::Parse(file, out, error), "v1 file parses");
  Check(out.size() == 2 && out[1].model == 0xabc00001, "v1 instances");
  Check(out.size() == 2 && out[1].active && out[1].layer == PortRoomGeo::kEveryLayer, "v1 defaults");

  file[4] = 4;
  Check(!PortRoomGeo::Parse(file, out, error), "unknown version rejected");
}

// Version 2: the platform fields are absent, the links follow the fixed part.
void TestVersion2() {
  std::vector<uint8_t> file;
  Put32(file, 0x4752504D);
  Put32(file, 2);
  Put32(file, 2);
  for (uint32_t i = 0; i < 2; ++i) {
    Put32(file, 0xabc00000 + i);
    for (int j = 0; j < 12; ++j) {
      Put32(file, 0);
    }
    file.push_back(1);
    file.push_back(0);
    file.push_back(1);
    file.push_back(0);
    Put32(file, 0x00100020 + i);
    file.push_back(9);
    file.push_back(PortRoomGeo::kShow);
    file.push_back(0);
    file.push_back(0);
  }
  std::vector<PortRoomGeo::Instance> out;
  std::string error;
  Check(PortRoomGeo::Parse(file, out, error), "v2 file parses");
  Check(out.size() == 2 && out[1].model == 0xabc00001 && out[1].layer == 1 && !out[1].active, "v2 instances");
  Check(out.size() == 2 && out[1].links.size() == 1 && out[1].links[0].sender == 0x00100021, "v2 links");
  Check(out.size() == 2 && out[1].platform == 0, "v2 has no platform");
}
// The script section: nodes, edges and each instance's group.
void TestScript() {
  std::vector<PortRoomGeo::Instance> in;
  in.push_back(MakeInstance(0x11111111, 1.f));
  in.push_back(MakeInstance(0x22222222, 2.f));
  in[1].group = 7;
  in[1].active = false;

  PortRoomGeo::Script script;
  PortRoomGeo::ScriptNode volume;
  volume.kind = PortRoomGeo::kCameraVolume;
  volume.centre[0] = 10.f;
  volume.half[1] = 2.5f;
  volume.axes[0] = 0.f;
  volume.axes[1] = 1.f;
  PortRoomGeo::ScriptNode counter;
  counter.kind = PortRoomGeo::kCounter;
  counter.max = 50;
  counter.active = false;
  script.nodes = {volume, counter};
  script.edges.push_back({false, 0, PortRoomGeo::kIncrement, 0, 1});
  script.edges.push_back({false, 1, PortRoomGeo::kGroupHide, 1, 7});
  script.edges.push_back({true, 9, PortRoomGeo::kNodeActivate, 0x0c1a0042, 1});

  const std::vector<uint8_t> file = PortRoomGeo::Write(in, &script);
  std::vector<PortRoomGeo::Instance> out;
  PortRoomGeo::Script back;
  std::string error;
  Check(PortRoomGeo::Parse(file, out, error, &back), "script file parses");
  Check(out.size() == 2 && out[0].group == PortRoomGeo::kNoGroup && out[1].group == 7, "groups");
  Check(back.nodes.size() == 2 && back.edges.size() == 3, "script sizes");
  if (back.nodes.size() == 2 && back.edges.size() == 3) {
    Check(back.nodes[0].kind == PortRoomGeo::kCameraVolume && back.nodes[0].centre[0] == 10.f &&
              back.nodes[0].half[1] == 2.5f && back.nodes[0].axes[0] == 0.f && back.nodes[0].axes[1] == 1.f &&
              back.nodes[0].active,
          "volume node");
    Check(back.nodes[1].kind == PortRoomGeo::kCounter && back.nodes[1].max == 50 && !back.nodes[1].active,
          "counter node");
    Check(!back.edges[1].retail && back.edges[1].event == 1 && back.edges[1].action == PortRoomGeo::kGroupHide &&
              back.edges[1].from == 1 && back.edges[1].to == 7,
          "group edge");
    Check(back.edges[2].retail && back.edges[2].event == 9 && back.edges[2].from == 0x0c1a0042, "retail edge");
  }

  // Without groups or a script the section is left out.
  in[1].group = PortRoomGeo::kNoGroup;
  Check(PortRoomGeo::Write(in).size() + 12 + 2 * 68 + 3 * 12 + 2 * 4 == PortRoomGeo::Write(in, &back).size(),
        "no empty section");

  for (size_t cut = 13; cut < file.size(); ++cut) {
    const std::vector<uint8_t> part(file.begin(), file.begin() + cut);
    if (PortRoomGeo::Parse(part, out, error, &back) && cut != PortRoomGeo::Write(in).size()) {
      std::fprintf(stderr, "FAIL: script truncated at %zu parses\n", cut);
      ++sFailures;
    }
  }

  std::vector<uint8_t> bad = file;
  bad[bad.size() - 8 - 3 * 12 + 4] = 9; // the first edge's from: no such node
  Check(!PortRoomGeo::Parse(bad, out, error, &back), "edge from a missing node rejected");
  bad = PortRoomGeo::Write(in);
  bad.push_back(1);
  Check(!PortRoomGeo::Parse(bad, out, error, &back), "trailing bytes rejected");
}

void TestLods() {
  std::vector<PortRoomGeo::Lods> in(2);
  in[0].model = 0x11111111;
  in[0].levels = {{100.f, 0x11111112}, {400.f, 0x11111113}};
  in[1].model = 0x22222222;
  in[1].levels = {{2500.f, 0x22222223}};
  const std::vector<uint8_t> file = PortRoomGeo::WriteLods(in);
  Check(file.size() == 12 + 2 * 8 + 3 * 8, "lods size");
  std::vector<PortRoomGeo::Lods> out;
  std::string error;
  Check(PortRoomGeo::ParseLods(file, out, error), "lods parse");
  Check(out.size() == 2 && out[0].model == 0x11111111 && out[0].levels.size() == 2 &&
            out[0].levels[1].distanceSq == 400.f && out[0].levels[1].model == 0x11111113,
        "first model's levels");
  Check(out.size() == 2 && out[1].levels.size() == 1 && out[1].levels[0].model == 0x22222223, "second model");
  for (size_t cut = 0; cut < file.size(); ++cut) {
    const std::vector<uint8_t> part(file.begin(), file.begin() + cut);
    if (PortRoomGeo::ParseLods(part, out, error)) {
      std::fprintf(stderr, "FAIL: lods truncated at %zu parses\n", cut);
      ++sFailures;
    }
  }
  std::vector<PortRoomGeo::Lods> bad = in;
  std::swap(bad[0].levels[0], bad[0].levels[1]);
  Check(!PortRoomGeo::ParseLods(PortRoomGeo::WriteLods(bad), out, error), "out-of-order distances rejected");
  bad = in;
  bad[1].levels.clear();
  Check(!PortRoomGeo::ParseLods(PortRoomGeo::WriteLods(bad), out, error), "model without levels rejected");
}
} // namespace

int main() {
  TestRoundTrip();
  TestVersion1();
  TestVersion2();
  TestScript();
  TestLods();
  uint32_t id = 0;
  Check(PortRoomGeo::ParseFileName("1a2B3c4D.ROOMGEO", id) && id == 0x1A2B3C4D, "file name");
  if (sFailures == 0) {
    std::printf("port_room_geo_tests: ok\n");
  }
  return sFailures == 0 ? 0 : 1;
}
