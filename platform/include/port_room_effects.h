#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class CStateManager;

// Remastered's own particle effect actors (EffectMP1 entities the disc has no object for),
// added to an area as retail Effect objects. A mod supplies them per area as
// <MREA id>.roomfx (the Remastered import writes it); while the area loads its script
// objects, each active layer's add-ons are created after the layer's own, and the retail
// objects the file names are not created (the effects Remastered replaced).
//
// The add-ons live outside the area's layer buffers, so nothing saves them: a save, a save
// state, a memory relay and the randomizer's dumps see only the retail objects. Only effects
// no script object talks to are added (the import counts the others).
//
// Their editor ids are the area's, on the retail layer they load with, with instance numbers
// from kFirstInstance up: no retail area uses those (the import checks).
//
// The file is little endian:
//   'MPRF', u32 version (1)
//   u32 skips, u32 editor id[skips] (retail objects not to create, layer bits included)
//   u32 connected (effects left out because something connects to them; for the log)
//   u32 layers, then per layer: u32 retail layer, u32 objects, u32 bytes, then that many
//   bytes of retail SCLY objects (u8 type, then big endian: u32 length, u32 editor id,
//   u32 connections (0), u32 property count, properties), all of type Effect (7).
namespace PortRoomEffects {

constexpr uint32_t kFileMagic = 0x4652504D;  // 'MPRF'
constexpr uint32_t kFileVersion = 1;
constexpr uint8_t kEffectType = 7;
constexpr uint32_t kEffectProps = 24;
constexpr uint32_t kFirstInstance = 0xF000;
constexpr uint32_t kMaxAddOns = 0x10000 - kFirstInstance;

// Whether an editor id is in the add-ons' range.
constexpr bool IsAddOnId(uint32_t eid) { return (eid & 0xFFFF) >= kFirstInstance; }
// The editor id of an area's `index`th add-on on `layer`; `area` is the area bits of the
// area's own editor ids.
constexpr uint32_t AddOnId(uint32_t layer, uint32_t area, uint32_t index) {
  return (layer & 0x3F) << 26 | (area & 0x3FF) << 16 | (kFirstInstance + index);
}

// One retail Effect object's properties (ScriptLoader::LoadEffect, in its order).
struct Effect {
  uint32_t id = 0;
  std::string name;
  float pos[3] = {0, 0, 0};
  float rot[3] = {0, 0, 0};  // degrees, as retail's editor transform takes them
  float scale[3] = {1, 1, 1};
  uint32_t part = 0xFFFFFFFF;
  bool hotInThermal = false;
  bool noTimerUnlessAreaOccluded = false;
  bool rebuildSystemsOnActivate = false;
  bool active = true;
  bool useRateInverseCamDist = false;
  float rateInverseCamDist = 5.0f;
  float rateInverseCamDistRate = 0.5f;
  float duration = 0.2f;
  float durationResetWhileVisible = 0.1f;
  bool useRateCamDistRange = false;
  float rateCamDistRangeMin = 20.0f;
  float rateCamDistRangeMax = 30.0f;
  float rateCamDistRangeFarRate = 0.0f;
  bool combatVisorVisible = true;
  bool thermalVisorVisible = true;
  bool xrayVisorVisible = true;
  bool dieWhenSystemsDone = false;
};

struct Layer {
  uint32_t layer = 0;
  uint32_t count = 0;
  std::vector<uint8_t> objects;  // `count` SCLY objects
};

struct File {
  std::vector<uint32_t> skips;
  uint32_t connected = 0;
  std::vector<Layer> layers;
};

// Appends the effect as one SCLY object. Its light parameters are retail's most common
// Effect lighting.
void AppendEffect(const Effect& effect, std::vector<uint8_t>& out);
// Reads one SCLY object written by AppendEffect at `data`; `used` is its size.
bool ReadEffect(const uint8_t* data, size_t size, Effect& out, size_t& used);
std::vector<uint8_t> Write(const File& file);
// Checks every object's framing, type and id range too.
bool Parse(const std::vector<uint8_t>& data, File& out, std::string& error);
// "1A2B3C4D.roomfx" (any case) -> 0x1A2B3C4D.
bool ParseFileName(const std::string& fileName, uint32_t& id);

// MP_REMASTERED_EFFECT_ACTORS=0 turns the add-ons (and the skips) off.
bool Enabled();
// Around CGameArea::LoadScriptObjects: reads the area's file.
void BeginArea(uint32_t mrea);
void EndArea();
// Whether the retail object `eid` is one the area being loaded leaves out.
bool Skips(uint32_t eid);
// Creates the add-ons of `layer` in area `area` (TAreaId) and adds their editor ids to `ids`,
// for InitializedInArea.
void LoadLayer(CStateManager& mgr, int area, uint32_t layer, std::vector<uint32_t>& ids);

struct Spawned {
  uint32_t mrea;
  uint32_t eid;
  uint16_t uid;
  uint32_t part;
  std::string name;
};
// The add-ons created for an area, as loaded (the console checks they still live).
std::vector<Spawned> List(uint32_t mrea);
// The retail objects left out when the area last loaded.
size_t SkippedCount(uint32_t mrea);

}  // namespace PortRoomEffects
