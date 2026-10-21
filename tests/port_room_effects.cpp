#include "port_room_effects.h"
#include "port_test.h"

#include <cstring>
#include <string>
#include <vector>

namespace {

using PortRoomEffects::Effect;
using PortRoomEffects::File;

Effect MakeEffect(uint32_t layer, uint32_t index) {
  Effect e;
  e.id = PortRoomEffects::AddOnId(layer, 0x1A, index);
  e.name = "Remastered effect " + std::to_string(index);
  e.pos[0] = -120.5f;
  e.pos[2] = 33.f;
  e.rot[1] = 90.f;
  e.scale[0] = 2.f;
  e.part = 0xFE123456;
  e.active = index % 2 == 0;
  e.noTimerUnlessAreaOccluded = true;
  e.thermalVisorVisible = false;
  e.rateCamDistRangeFarRate = 0.25f;
  return e;
}

void TestIds() {
  CHECK(PortRoomEffects::AddOnId(1, 0x1A, 0) == 0x041AF000);
  CHECK(PortRoomEffects::AddOnId(63, 0x3FF, PortRoomEffects::kMaxAddOns - 1) == 0xFFFFFFFF);
  CHECK(PortRoomEffects::IsAddOnId(0x041AF000));
  CHECK(PortRoomEffects::IsAddOnId(0x001AFFFE));
  CHECK(!PortRoomEffects::IsAddOnId(0x041AEFFF));
  CHECK(!PortRoomEffects::IsAddOnId(0x000001CA));
  CHECK(PortRoomEffects::kMaxAddOns == 0x1000);
}

void TestEffect() {
  const Effect in = MakeEffect(2, 7);
  std::vector<uint8_t> bytes;
  PortRoomEffects::AppendEffect(in, bytes);
  // Type, length, then the body (id, no connections, 24 properties).
  REQUIRE(bytes.size() > 17);
  CHECK(bytes[0] == 7);
  const uint32_t length = uint32_t(bytes[1]) << 24 | uint32_t(bytes[2]) << 16 | uint32_t(bytes[3]) << 8 | bytes[4];
  CHECK(length == bytes.size() - 5);
  CHECK(bytes[5] == 0x08 && bytes[6] == 0x1A && bytes[7] == 0xF0 && bytes[8] == 0x07);
  CHECK(bytes[16] == 24);
  Effect out;
  size_t used = 0;
  REQUIRE(PortRoomEffects::ReadEffect(bytes.data(), bytes.size(), out, used));
  CHECK(used == bytes.size());
  CHECK(out.id == in.id && out.name == in.name && out.part == in.part);
  CHECK(out.pos[0] == -120.5f && out.pos[2] == 33.f && out.rot[1] == 90.f && out.scale[0] == 2.f);
  CHECK(out.active == in.active && out.noTimerUnlessAreaOccluded && !out.thermalVisorVisible && out.xrayVisorVisible);
  CHECK(out.rateCamDistRangeFarRate == 0.25f && out.duration == 0.2f && out.rateCamDistRangeMax == 30.f);
  // 4 + 4 + 4 + name + 36 + 8 + 4 + 1 + 16 + 1 + 12 + 4 + 71 light bytes.
  CHECK(length == 12 + in.name.size() + 1 + 36 + 8 + 4 + 1 + 16 + 1 + 12 + 4 + 71);
  CHECK(!PortRoomEffects::ReadEffect(bytes.data(), bytes.size() - 1, out, used));
}

void TestFile() {
  File in;
  in.skips = {0x040001CA, 0x040001CB, 0x040001CC};
  in.connected = 5;
  for (uint32_t l : {0u, 1u}) {
    PortRoomEffects::Layer layer;
    layer.layer = l;
    for (uint32_t i = 0; i < 3; ++i) {
      PortRoomEffects::AppendEffect(MakeEffect(l, l * 3 + i), layer.objects);
      ++layer.count;
    }
    in.layers.push_back(layer);
  }
  const std::vector<uint8_t> data = PortRoomEffects::Write(in);
  File out;
  std::string error;
  REQUIRE_MSG(PortRoomEffects::Parse(data, out, error), error.c_str());
  CHECK(out.skips == in.skips && out.connected == 5 && out.layers.size() == 2);
  CHECK(out.layers[1].layer == 1 && out.layers[1].count == 3 && out.layers[1].objects == in.layers[1].objects);

  std::vector<uint8_t> bad = data;
  bad.pop_back();
  CHECK_MSG(!PortRoomEffects::Parse(bad, out, error), "truncated");
  bad = data;
  bad.push_back(0);
  CHECK_MSG(!PortRoomEffects::Parse(bad, out, error), "trailing bytes");
  bad = data;
  bad[4] = 2;
  CHECK_MSG(!PortRoomEffects::Parse(bad, out, error), "version");

  // An object outside the add-on range, or on another layer than its own, is refused.
  File wrong;
  PortRoomEffects::Layer layer;
  Effect retail = MakeEffect(0, 0);
  retail.id = 0x001A01CA;
  PortRoomEffects::AppendEffect(retail, layer.objects);
  layer.count = 1;
  wrong.layers.push_back(layer);
  CHECK_MSG(!PortRoomEffects::Parse(PortRoomEffects::Write(wrong), out, error), "retail id refused");
  wrong.layers[0].objects.clear();
  PortRoomEffects::AppendEffect(MakeEffect(3, 0), wrong.layers[0].objects);
  CHECK_MSG(!PortRoomEffects::Parse(PortRoomEffects::Write(wrong), out, error), "layer mismatch refused");

  File empty;
  CHECK(PortRoomEffects::Parse(PortRoomEffects::Write(empty), out, error) && out.layers.empty());
}

}  // namespace

int main() {
  TestIds();
  TestEffect();
  TestFile();
  uint32_t id = 0;
  CHECK_MSG(PortRoomEffects::ParseFileName("1a2B3c4D.ROOMFX", id) && id == 0x1A2B3C4D, "file name");
  CHECK(!PortRoomEffects::ParseFileName("1A2B3C4D.roomgeo", id));
  CHECK(!PortRoomEffects::ParseFileName("1A2B3C4.roomfx", id));
  return PORT_TEST_RESULT();
}
