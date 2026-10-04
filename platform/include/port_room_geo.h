#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class CFrustumPlanes;
class CGameArea;
class CStateManager;
class CVector3f;

// A room's static geometry as a list of models and where each stands. A mod supplies one
// per area as <MREA id>.roomgeo, with the models as ordinary CMDL files; the port draws
// them in place of the area's own world geometry.
//
// The file is little endian:
//   'MPRG', u32 version (1 to 4), u32 instances
//   instance: u32 CMDL id, f32 transform[12] (rows of model -> area)
//     version 2 adds: u8 layer, u8 active, u16 links,
//     version 3 (and 4) then: u32 platform, f32 platformStart[3],
//     then (2 to 4) per link: u32 sender, u8 state, u8 action, u16 0
// An instance whose CMDL does not exist is skipped.
//
// Version 2 is for scenery Remastered added as actors, which its scripts show and hide:
// such an instance is drawn only while the area's script layer `layer` is on (kEveryLayer:
// always), starts shown or hidden by `active`, and changes when the retail object with
// editor id `sender` sends `state` (an EScriptObjectState).
//
// Version 3 adds the actors a platform carries (its Play -> Activate connections, which
// retail's CScriptPlatform::BuildSlaveList takes as slaves): `platform` is the retail
// platform's editor id (0: none) and `platformStart` where it stands in the world when its
// area is made. Such an instance moves by what the platform has moved since, as a slave
// is dragged by the platform's translation alone.
//
// Version 3 may end with Remastered's own script objects that show and hide geometry, which
// retail has no object for (Script):
//   'SCRP', u32 nodes, u32 edges
//   node: u8 kind, u8 active, u16 0, u32 counter max, f32 centre[3], half[3], axes[9]
//   edge: u8 retail, u8 event, u8 action, u8 0, u32 from, u32 to
//   then u32 group per instance (kNoGroup: none)
//
// Version 4 may then end with the instances that glow in a colour of their own:
//   'GLOW', u32 count, per instance: u32 index, f32 glow[3]
// Remastered colours its door frames' lights with a ColorModulateMP1 in its "incandescence"
// mode, which stands its colour B (times its intensity) in for the strength of every
// material's emissive map. `glow` is that colour.
namespace PortRoomGeo {

enum : uint8_t { kEveryLayer = 0xff };
enum : uint32_t { kNoGroup = 0xffffffff };
// kFollow (with state MaxReached): `sender` is a DamageableTrigger, and the instance is one
// of the actors retail's trigger shows and fades with itself (SetLinkedObjectAlpha): drawn
// at the trigger's puddle alpha while it is active, hidden once it goes inactive.
enum LinkAction : uint8_t { kShow = 1, kHide = 2, kToggle = 3, kFollow = 4 };

struct Link {
  uint32_t sender; // retail editor id, layer bits included
  uint8_t state;
  uint8_t action; // LinkAction
};

struct Instance {
  uint32_t model = 0;
  float transform[12] = {};
  uint8_t layer = kEveryLayer;
  bool active = true;
  std::vector<Link> links;
  uint32_t platform = 0; // retail editor id, layer bits included
  float platformStart[3] = {};
  uint32_t group = kNoGroup; // the Remastered entity the scripts show and hide it by
  bool glows = false;        // whether `glow` replaces its materials' emissive strength
  float glow[3] = {};
};

// Remastered's script objects between what happens in game and a group of instances.
enum NodeKind : uint8_t {
  // A TriggerMP1 that detects the camera: sends Entered (event 0) when the camera comes into
  // its box and Exited (1) when it leaves. The box is in area space: a point p is inside when
  // |dot(p - centre, axis i)| <= half[i] for each axis (axes[3i..3i+2], unit length).
  kCameraVolume = 1,
  // A Counter: kIncrement/kDecrement move it within 0..max; a change to 1 or more sends
  // MaxReached (event 2, at max) and then NonZero (0), a change to 0 sends Zero (1).
  kCounter = 2,
  // Remastered's Relay: kFire sends Fired (event 0).
  kRelay = 3,
};
enum ScriptAction : uint8_t {
  kIncrement = 1,
  kDecrement = 2,
  kFire = 3,
  kGroupShow = 4, // `to` is a group
  kGroupHide = 5,
  kGroupToggle = 6,
  kNodeActivate = 7, // `to` is a node; an inactive node takes no action and sends nothing
  kNodeDeactivate = 8,
};

struct ScriptNode {
  uint8_t kind = 0;
  bool active = true;
  uint32_t max = 0;
  float centre[3] = {};
  float half[3] = {};
  float axes[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
};

// `from` is a node index, or for `retail` a retail editor id (layer bits included) whose
// state `event` (an EScriptObjectState) sets it off.
struct ScriptEdge {
  bool retail = false;
  uint8_t event = 0;
  uint8_t action = 0;
  uint32_t from = 0;
  uint32_t to = 0;
};

struct Script {
  std::vector<ScriptNode> nodes;
  std::vector<ScriptEdge> edges;
  bool Empty() const { return nodes.empty() && edges.empty(); }
};

// --- The file (port_room_geo_file.cpp; no game or GX dependencies) -------------

// "1A2B3C4D.roomgeo" (any case) -> 0x1A2B3C4D.
bool ParseFileName(const std::string& fileName, uint32_t& id);
bool Parse(const std::vector<uint8_t>& data, std::vector<Instance>& out, std::string& error,
           Script* script = nullptr);
// The file for these instances (version 4), with the script section when there is a script
// or a group and the glow section when an instance glows.
std::vector<uint8_t> Write(const std::vector<Instance>& instances, const Script* script = nullptr);

// The coarser levels of detail of the models, one table for the whole mod (kLodFileName in
// the folder of the .roomgeo files), little endian:
//   'RLOD', u32 version (1), u32 models
//   model: u32 CMDL id, u32 levels (1 to kLodLevels - 1),
//     per level, coarsest last: f32 distance squared it starts at, u32 CMDL id
// The distances are in model space, from the eye to the model's bounds.
constexpr const char* kLodFileName = "lods.bin";
constexpr int kLodLevels = 5; // the model itself and up to four coarser ones
struct LodLevel {
  float distanceSq = 0.f;
  uint32_t model = 0;
};
struct Lods {
  uint32_t model = 0;
  std::vector<LodLevel> levels;
};
bool ParseLods(const std::vector<uint8_t>& data, std::vector<Lods>& out, std::string& error);
std::vector<uint8_t> WriteLods(const std::vector<Lods>& models);

// --- The game side (port_room_geo.cpp) -----------------------------------------

// The areas in memory now. Loads the files of new ones and frees those of areas that left.
void SetLoadedAreas(const uint32_t* mreas, size_t count);
// Draws the area's room geometry. True when it stands in for the area's own, which is
// when the area has a file, the mode says so and every model of it has loaded.
bool Draw(const CStateManager& mgr, const CGameArea& area, const CFrustumPlanes& frustum);
// Queues the blended surfaces of what Draw drew for the area this frame in the renderer's
// sorted pass; the renderer hands each back to DrawSorted through the state manager's
// drawable callback, as type kDrawableType.
enum { kDrawableType = 3 };
void AddSorted(const CGameArea& area);
void DrawSorted(const void* drawable);
// Set around the renderer's sorted pass for an area Draw stood in for, so that pass
// draws the actors and leaves the area's own surfaces out.
extern bool sReplacingArea;
// Lets go of every model (the mods folder is about to change).
void Reset();
// A script object sent a state (CEntity::SendScriptMsgs): shows or hides the instances
// linked to it. Loads the area's file if the area is newer than the last SetLoadedAreas,
// since objects send states while their area is still being set up.
void OnScriptState(CStateManager& mgr, uint32_t editorId, int state);
// Once a frame, where the game's objects think (CStateManager::Update): runs the camera
// volumes of every loaded area's Script against the current camera, as CScriptTrigger
// does for its own.
void Think(CStateManager& mgr);
// The console's `roomgeo script`: each loaded area's Script now (camera, nodes, groups).
std::string ScriptInfo();
// The console's `roomgeo group <n> show|hide`: sets every loaded area's group n until its
// script next changes it; how many instances it set.
int SetGroupShown(uint32_t group, bool shown);
// A new game, a death or a save state builds a new CStateManager: every instance goes back
// to how the file starts it, since the scripts will not resend what already happened.
void ResetScriptState();

// Whether the frame's buffers were sized for room geometry at startup (main.cpp). Until
// they are, nothing is drawn: a room of it overflows the default ones.
void SetBuffersReady(bool ready);
bool BuffersReady();
// Whether Aurora set room aside at startup to keep models on the GPU (main.cpp): each model's
// vertex arrays and display lists are then retained when it loads (GXPortRetainResident)
// and released before it goes, so its draws no longer copy them into the frame's buffers.
void SetResident(bool resident);
bool Resident();

enum class Mode {
  Off,
  Replace, // in place of the area's world geometry
  Overlay, // on top of it
};
// MP_ROOM_GEO=0|1|overlay, the console's `roomgeo`.
void SetMode(Mode mode);
Mode GetMode();
// Whether room geometry takes the area's lights where the room has baked light too
// (MP_ROOM_GEO_AREA_LIGHTS, the console's `roomgeo lights`).
void SetAreaLights(bool on);
bool AreaLights();
// Instances whose bounds span fewer pixels than this, in the game's own resolution (its
// 640x480-ish viewport, whatever the render scale), are left out. 0 draws every one.
// A room is thousands of small props, and on a phone the draws cost more than the
// triangles do.
void SetMinPixels(float pixels);
float MinPixels();
// Off draws merged copies one by one again, each with its own lights (an A/B check; not saved).
void SetMergedDraws(bool on);
bool MergedDraws();
// Draws the opaque room models nearest first, the cut-out ones after them, so the GPU skips
// the shading of what is already covered. Off goes model by model, to keep pipelines bound
// (an A/B check; not saved).
void SetFrontToBack(bool on);
bool FrontToBack();
// With FrontToBack, draws the cut-out models (grass, leaves) once for depth only, then shaded
// where the depth is equal, so what they hide of each other isn't shaded. Looks the same
// (an A/B check; not saved).
void SetDepthPrepass(bool on);
bool DepthPrepass();
// Scales the distances where an instance switches to a coarser level of detail (the
// import's lods.bin): 1 is Remastered's own, 2 keeps the full model twice as far, 0 never
// switches (MP_ROOM_GEO_LOD, the console's `roomgeo lod`). Merged copies keep the full one.
void SetLodDistance(float scale);
float LodDistance();
// Coarser levels in the loaded areas, how many have loaded, and the instances drawn with
// one in the last frame.
void LodStats(int& levels, int& loaded, int& drawnCoarse);
// Areas with a file, their instances, the distinct models and how many have loaded, and
// the instances drawn in the last frame.
void Stats(int& areas, int& instances, int& models, int& loaded, int& drawn);


// For finding which model a surface belongs to (the console's `roomgeo at|hide|show`).
// One line per drawn instance whose box holds the point, give or take the margin.
std::string At(const CVector3f& point, float margin);
// Stops or resumes drawing a model, or every model for id 0; how many it matched.
int SetHidden(uint32_t id, bool hidden);
// What a ray meets (the console's `roomgeo pick`): one line per instance whose box it
// passes through, nearest first, then the boxes the origin is already inside, smallest
// first. Boxes, not triangles, so the surface looked at is among the first few. Returns
// the first one's model, 0 for none.
uint32_t Pick(const CVector3f& origin, const CVector3f& direction, std::string& out);
// One line per material of a loaded model: flags, whether it is drawn through PBR, and
// its record (see CCubeModel::PortSetPBRMaterial). Empty when no loaded model has the id.
std::string Materials(uint32_t id);
// Draws a model's material with one value of its record replaced (index 0 to 18), until
// cleared; kept across room loads. False when no loaded model has that material.
bool SetMaterialValue(uint32_t id, int material, int field, float value);
int ClearMaterialValues();

} // namespace PortRoomGeo
