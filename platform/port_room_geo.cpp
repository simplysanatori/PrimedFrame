// Room geometry at run time: which areas have a file, their models, and the draw. See
// port_room_geo.h.
#include "port_room_geo.h"

#include "port_gci.h"
#include "port_log.h"
#include "port_mods.h"
#include "port_room_env.h"

#include "Kyoto/CResFactory.hpp"
#include "Kyoto/Graphics/CCubeMaterial.hpp"
#include "Kyoto/Graphics/CCubeModel.hpp"
#include "Kyoto/Graphics/CGraphics.hpp"
#include "Kyoto/Graphics/CModel.hpp"
#include "Kyoto/Graphics/CModelFlags.hpp"
#include "Kyoto/Math/CAABox.hpp"
#include "Kyoto/Math/CFrustumPlanes.hpp"
#include "MetroidPrime/CActor.hpp"
#include "MetroidPrime/CActorLights.hpp"
#include "MetroidPrime/Cameras/CCameraManager.hpp"
#include "MetroidPrime/Cameras/CGameCamera.hpp"
#include "MetroidPrime/CGameArea.hpp"
#include "MetroidPrime/CModelData.hpp"
#include "MetroidPrime/CScriptLayerManager.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/ScriptObjects/CScriptDamageableTrigger.hpp"
#include "MetroidPrime/TCastTo.hpp"
#include "MetaRender/CCubeRenderer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <unordered_map>

namespace PortRoomGeo {
namespace {

struct Model {
  uint32_t id = 0;
  std::unique_ptr< CModelData > data;
  bool loaded = false;
  bool hidden = false; // by the console, to find which model a surface belongs to
  CAABox bounds = CAABox::MakeMaxInvertedBox();
};

struct Area;

struct Placed {
  size_t model;
  Area* owner = nullptr; // the area it belongs to; areas stay put in Areas() until erased
  CTransform4f xf = CTransform4f::Identity(); // model -> world
  CAABox bounds = CAABox::MakeMaxInvertedBox();
  bool bounded = false;
  std::unique_ptr< CActorLights > lights;
  bool areaLit = false; // `lights` holds the area's lights
  uint32_t volume = 0;  // the area whose baked ambient lights it, 0 for none
  uint8_t layer = kEveryLayer; // drawn only while this script layer is on
  bool shown = true;           // by the area's scripts
  bool active = true;          // what the file starts it as
  // A platform's slave (0: none): the platform's editor id, where it stood when its area
  // was made, and how far from there it was when `xf` was last moved with it.
  uint32_t platform = 0;
  CVector3f platformStart = CVector3f::Zero();
  CVector3f dragged = CVector3f::Zero();
  // A DamageableTrigger's linked actor (kFollow; 0: none): the trigger's editor id, whether
  // it was last seen active, and the alpha it draws this instance at.
  uint32_t follow = 0;
  bool following = false;
  float alpha = 1.f;
};

// A script node now (see ScriptNode).
struct NodeState {
  bool active = true;
  bool inside = false; // a camera volume: the camera was in it when last looked at
  uint32_t count = 0;  // a counter's value
};

// An instance shown or hidden when a script object sends a state.
struct Trigger {
  uint32_t sender; // editor id without the layer bits (TEditorId::Value)
  uint8_t state;
  uint8_t action;
  size_t item;
};

struct Area {
  bool hasFile = false;
  bool placed = false; // the instances have their world transforms
  std::vector< Instance > instances;
  std::vector< Model > models;
  std::vector< Placed > items;
  std::vector< const Placed* > sorted; // this frame's, with blended surfaces still to draw
  std::vector< Trigger > triggers;
  bool gated = false; // some instance has a layer
  size_t loaded = 0;
  // Remastered's own script objects (Script), what each is now, the items of each group,
  // and each node's edges out.
  Script script;
  std::vector< NodeState > nodes;
  std::vector< std::vector< size_t > > groups;
  std::vector< std::vector< size_t > > edgesFrom;
  bool retailEdges = false; // some edge starts at a retail object
  float camera[3] = {};      // where Think last saw the camera, in area space
};

// Areas in memory; one without a file has no instances. Never destroyed: the models' tokens
// must not outlive the game's resource pool, which static destruction would not respect.
std::unordered_map< uint32_t, Area >& Areas() {
  static auto* const areas = new std::unordered_map< uint32_t, Area >();
  return *areas;
}

int sMode = -1;
int sAreaLights = -1;
// Counts the times areas left Areas(). The sorted pass hands DrawSorted only the item, so
// it draws while the areas are the ones they were when AddSorted queued it.
uint32_t sGeneration = 0;
uint32_t sQueuedGeneration = 0;
bool sQueued = false; // AddSorted has queued items since SetLoadedAreas
bool sQueueStale = false; // areas went between two AddSorted calls
bool sBuffersReady = false;
bool sWarned = false;
int sDrawn = 0;
int sDrawnLast = 0;
// Whether any area in memory has triggers, so the script hook costs nothing otherwise.
bool sTriggers = false;

// The console's material values, by model id; CCubeModel holds them by model, which is
// only good while the model is in memory, so they are handed over again every frame.
struct MaterialValue {
  uint32_t id;
  int material;
  int field;
  float value;
};
std::vector< MaterialValue > sMaterialValues;

const CCubeModel* CubeModel(const Model& model) {
  return model.loaded ? (**model.data->PickStaticModel(CModelData::kWM_Normal)).GetCubeModel() : nullptr;
}

void BindMaterialValues() {
  CCubeModel::PortClearPBROverrides();
  for (const MaterialValue& value : sMaterialValues) {
    for (const auto& [mrea, area] : Areas()) {
      for (const Model& model : area.models) {
        if (model.id == value.id) {
          CCubeModel::PortOverridePBR(CubeModel(model), value.material, value.field, value.value);
        }
      }
    }
  }
}

// The whole file, in one read when its size is known. The stream is left as a read
// through istreambuf_iterator leaves it: failed only when the file did not open.
std::vector< uint8_t > ReadAll(std::ifstream& in) {
  std::vector< uint8_t > data;
  if (!in) {
    return data;
  }
  in.seekg(0, std::ios::end);
  const std::streamoff size = in.tellg();
  in.seekg(0, std::ios::beg);
  if (in && size > 0) {
    data.resize(size_t(size));
    in.read(reinterpret_cast< char* >(data.data()), std::streamsize(size));
    data.resize(size_t(in.gcount()));
  } else {
    in.clear();
    data.assign(std::istreambuf_iterator< char >(in), std::istreambuf_iterator< char >());
  }
  in.clear();
  return data;
}

// The script objects as the file has them: a counter at 0, the camera in no volume.
void ResetNodes(Area& area) {
  area.nodes.assign(area.script.nodes.size(), NodeState());
  for (size_t i = 0; i < area.nodes.size(); ++i) {
    area.nodes[i].active = area.script.nodes[i].active;
  }
}

void Load(uint32_t mrea, Area& area) {
  const std::string path = PortMods::RoomGeoPath(mrea);
  if (path.empty() || gpResourceFactory == nullptr) {
    return;
  }
  std::ifstream in(PortGci::PathFromString(path), std::ios::binary);
  const std::vector< uint8_t > data = ReadAll(in);
  std::string error;
  if (!in || !Parse(data, area.instances, error, &area.script)) {
    PortLog::Write("room geo: %s: %s\n", path.c_str(), error.empty() ? "cannot read" : error.c_str());
    area.instances.clear();
    area.script = Script();
    return;
  }
  // A model the import could not convert leaves its instances behind.
  std::unordered_map< uint32_t, size_t > index;
  size_t missing = 0;
  for (const Instance& instance : area.instances) {
    auto found = index.find(instance.model);
    if (found == index.end()) {
      size_t slot = size_t(-1);
      if (gpResourceFactory->GetResourceTypeById(static_cast< CAssetId >(instance.model)) == 'CMDL') {
        slot = area.models.size();
        Model& model = area.models.emplace_back();
        model.id = instance.model;
        model.data.reset(new CModelData(
            CStaticRes(static_cast< CAssetId >(instance.model), CVector3f(1.f, 1.f, 1.f))));
      }
      found = index.emplace(instance.model, slot).first;
    }
    if (found->second == size_t(-1)) {
      ++missing;
      continue;
    }
    Placed& item = area.items.emplace_back();
    item.model = found->second;
    const float* const m = instance.transform;
    item.xf = CTransform4f(m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11]);
    item.layer = instance.layer;
    item.shown = item.active = instance.active;
    item.platform = instance.platform;
    item.platformStart =
        CVector3f(instance.platformStart[0], instance.platformStart[1], instance.platformStart[2]);
    area.gated = area.gated || item.layer != kEveryLayer;
    if (instance.group != kNoGroup) {
      if (area.groups.size() <= instance.group) {
        area.groups.resize(size_t(instance.group) + 1);
      }
      area.groups[instance.group].push_back(area.items.size() - 1);
    }
    for (const Link& link : instance.links) {
      if (link.action == kFollow) {
        // GetIdForScript wants the layer bits too.
        item.follow = link.sender;
        continue;
      }
      area.triggers.push_back({link.sender & 0x3ffffff, link.state, link.action, area.items.size() - 1});
    }
  }
  for (Placed& item : area.items) {
    item.owner = &area;
  }
  area.edgesFrom.assign(area.script.nodes.size(), {});
  for (size_t i = 0; i < area.script.edges.size(); ++i) {
    const ScriptEdge& edge = area.script.edges[i];
    if (edge.retail) {
      area.retailEdges = true;
    } else {
      area.edgesFrom[edge.from].push_back(i);
    }
  }
  ResetNodes(area);
  sTriggers = sTriggers || !area.triggers.empty() || area.retailEdges;
  area.instances.clear();
  area.instances.shrink_to_fit();
  area.hasFile = !area.items.empty();
  PortLog::Write("room geo: %08X: %zu instance(s) of %zu model(s), %zu without a model, %zu trigger(s), %zu script "
                 "node(s), %zu edge(s), %zu group(s)\n",
                 mrea, area.items.size(), area.models.size(), missing, area.triggers.size(),
                 area.script.nodes.size(), area.script.edges.size(), area.groups.size());
}

void Apply(Area& area, const ScriptEdge& edge, int depth);

// Edges one outside event may set off. A loop that fans out would otherwise take
// exponential time before the depth limit stops it.
constexpr int kApplyBudget = 4096;
int sApplyBudget = kApplyBudget;
bool sBudgetLogged = false;

// A node sends an event: every edge out of it with that event acts, in the file's order,
// each before the next as retail's SendScriptMsgs does.
void Send(Area& area, uint32_t node, uint8_t event, int depth) {
  // Remastered's objects can be wired in a loop; retail's would recurse until the stack
  // ran out, which no room depends on.
  if (depth > 32) {
    return;
  }
  if (sApplyBudget <= 0) {
    if (!sBudgetLogged) {
      PortLog::Write("room geo: script loop; stopped after %d edges\n", kApplyBudget);
      sBudgetLogged = true;
    }
    return;
  }
  for (const size_t i : area.edgesFrom[node]) {
    const ScriptEdge& edge = area.script.edges[i];
    if (edge.event == event) {
      Apply(area, edge, depth + 1);
    }
  }
}

void Apply(Area& area, const ScriptEdge& edge, int depth) {
  --sApplyBudget;
  if (edge.action == kGroupShow || edge.action == kGroupHide || edge.action == kGroupToggle) {
    if (edge.to >= area.groups.size()) {
      return;
    }
    size_t changed = 0;
    for (const size_t i : area.groups[edge.to]) {
      Placed& item = area.items[i];
      const bool shown = edge.action == kGroupShow ? true : edge.action == kGroupHide ? false : !item.shown;
      changed += shown != item.shown;
      item.shown = shown;
    }
    if (changed != 0) {
      PortLog::Write("room geo: group %u: %zu of %zu instance(s) %s\n", unsigned(edge.to), changed,
                     area.groups[edge.to].size(), edge.action == kGroupShow ? "shown" : edge.action == kGroupHide ? "hidden" : "toggled");
    }
    return;
  }
  NodeState& node = area.nodes[edge.to];
  const ScriptNode& kind = area.script.nodes[edge.to];
  switch (edge.action) {
  case kNodeActivate:
    node.active = true;
    return;
  case kNodeDeactivate:
    // A trigger told to stop forgets what is inside it (CScriptTrigger::AcceptScriptMsg),
    // so it sends Entered again once it is back on and the camera is still in it.
    node.active = false;
    node.inside = false;
    return;
  default:
    break;
  }
  if (!node.active) {
    return;
  }
  if (edge.action == kFire && kind.kind == kRelay) {
    Send(area, edge.to, 0, depth);
  } else if (edge.action == kIncrement && kind.kind == kCounter) {
    if (kind.max != 0 && node.count >= kind.max) {
      return;
    }
    ++node.count;
    if (node.count == kind.max) {
      Send(area, edge.to, 2, depth);
    }
    Send(area, edge.to, 0, depth);
  } else if (edge.action == kDecrement && kind.kind == kCounter) {
    if (node.count == 0) {
      return;
    }
    --node.count;
    Send(area, edge.to, node.count == 0 ? 1 : 0, depth);
  }
}

} // namespace

void SetLoadedAreas(const uint32_t* mreas, size_t count) {
  auto& areas = Areas();
  if (!sBuffersReady && !sWarned) {
    for (size_t i = 0; i < count; ++i) {
      if (!PortMods::RoomGeoPath(mreas[i]).empty()) {
        PortLog::Write("room geo: not drawn; the game started without room geometry installed. Restart it.\n");
        sWarned = true;
        break;
      }
    }
  }
  if (GetMode() == Mode::Off) {
    CCubeModel::PortClearPBROverrides();
    areas.clear();
    ++sGeneration;
    return;
  }
  sTriggers = false;
  for (auto it = areas.begin(); it != areas.end();) {
    if (std::find(mreas, mreas + count, it->first) == mreas + count) {
      it = areas.erase(it);
      ++sGeneration;
    } else {
      sTriggers = sTriggers || !it->second.triggers.empty() || it->second.retailEdges;
      ++it;
    }
  }
  for (size_t i = 0; i < count; ++i) {
    if (areas.find(mreas[i]) == areas.end()) {
      Load(mreas[i], areas[mreas[i]]);
    }
  }
  // A frame Draw sits out (the thermal and X-ray visors) must not queue the last one's.
  for (auto& [mrea, area] : areas) {
    area.sorted.clear();
  }
  sQueued = false;
  sQueueStale = false;
  sDrawnLast = sDrawn;
  sDrawn = 0;
  if (!sMaterialValues.empty()) {
    BindMaterialValues();
  }
}

bool Draw(const CStateManager& mgr, const CGameArea& gameArea, const CFrustumPlanes& frustum) {
  if (GetMode() == Mode::Off) {
    return false;
  }
  auto& areas = Areas();
  const auto found = areas.find(gameArea.GetAreaAssetId());
  if (found == areas.end() || !found->second.hasFile) {
    return false;
  }
  Area& area = found->second;
  area.sorted.clear();
  if (!area.placed) {
    for (Placed& item : area.items) {
      item.xf = gameArea.GetTM() * item.xf;
    }
    area.placed = true;
  }
  // Slaves go where the platform has taken them (CScriptPlatform::DragSlaves adds each
  // translation the platform makes, never its turns). One whose platform is gone stays
  // where it was last taken.
  for (Placed& item : area.items) {
    if (item.platform == 0) {
      continue;
    }
    const CActor* const platform =
        TCastToConstPtr< CActor >(mgr.GetObjectById(mgr.GetIdForScript(TEditorId(item.platform))));
    if (platform == nullptr) {
      continue;
    }
    const CVector3f dragged = platform->GetTranslation() - item.platformStart;
    if (dragged != item.dragged) {
      item.xf.AddTranslation(dragged - item.dragged);
      item.dragged = dragged;
      item.bounded = false;
    }
  }
  if (area.loaded != area.models.size()) {
    area.loaded = 0;
    for (Model& model : area.models) {
      if (!model.loaded) {
        if (!model.data->IsLoaded(0)) {
          model.data->Touch(CModelData::kWM_Normal, 0);
          continue;
        }
        model.bounds = model.data->GetBounds();
        model.loaded = true;
      }
      ++area.loaded;
    }
    // The area's own geometry stays until the last model is in, so no frame has holes.
    if (area.loaded != area.models.size() && GetMode() == Mode::Replace) {
      return false;
    }
  }
  // A trigger's linked actors are made active and given its alpha every frame it is active
  // (CScriptDamageableTrigger::Think); when it goes inactive, after its death fade or by a
  // Deactivate at alpha 0, they are not seen again until it is.
  for (Placed& item : area.items) {
    if (item.follow == 0) {
      continue;
    }
    const CScriptDamageableTrigger* const trigger = dynamic_cast< const CScriptDamageableTrigger* >(
        mgr.GetObjectById(mgr.GetIdForScript(TEditorId(item.follow))));
    if (trigger != nullptr && trigger->GetActive()) {
      item.shown = true;
      item.following = true;
      item.alpha = trigger->PortLinkedAlpha();
    } else if (item.following) {
      item.shown = false;
      item.following = false;
    }
  }
  const bool baked = !AreaLights() && PortRoomEnv::HasVolume(gameArea.GetAreaAssetId());
  CScriptLayerManager* const layers =
      area.gated ? const_cast< CStateManager& >(mgr).WorldLayerState().GetPtr() : nullptr;
  for (Placed& item : area.items) {
    const Model& model = area.models[item.model];
    if (!model.loaded || model.hidden || !item.shown || item.alpha <= 0.f) {
      continue;
    }
    if (item.layer != kEveryLayer && layers != nullptr &&
        !layers->IsLayerActive(gameArea.GetAreaId(), item.layer)) {
      continue;
    }
    if (!item.bounded) {
      item.bounds = model.bounds.GetTransformedAABox(item.xf);
      item.bounded = true;
    }
    if (!frustum.BoxInFrustumPlanes(item.bounds)) {
      continue;
    }
    if (item.lights == nullptr || item.areaLit == baked) {
      // The baked ambient already holds the area's lights, so that set has room for none:
      // one that expects area lights and has none drops its dynamic lights too. Its
      // ambient is what a material outside PBR is drawn at.
      item.lights.reset(new CActorLights(8, CVector3f(0.f, 0.f, 0.f), 4, baked ? 0 : 4));
      if (baked) {
        item.lights->SetAmbientColor(CColor::White());
      }
      item.areaLit = !baked;
    }
    if (baked) {
      const CVector3f centre = item.bounds.GetCenterPoint();
      const float at[3] = {centre.GetX(), centre.GetY(), centre.GetZ()};
      PortRoomEnv::SetVolumeHint(gameArea.GetAreaAssetId(), at);
    } else {
      item.lights->BuildAreaLightList(mgr, gameArea, item.bounds);
    }
    item.lights->BuildDynamicLightList(mgr, item.bounds);
    item.volume = baked ? gameArea.GetAreaAssetId() : 0;
    // Blended surfaces (glass, decals) wait for the sorted pass, where they are drawn
    // back to front among the actors.
    // A faded one is drawn whole among them, as retail's CActor with blend flags is.
    const CModel& cmodel = **model.data->PickStaticModel(CModelData::kWM_Normal);
    if (item.alpha < 1.f) {
      area.sorted.push_back(&item);
      ++sDrawn;
      continue;
    }
    gpRender->SetModelMatrix(item.xf);
    item.lights->ActivateLights();
    cmodel.DrawUnsortedParts(CModelFlags::Normal());
    if (!cmodel.IsDefinitelyOpaque()) {
      area.sorted.push_back(&item);
    }
    ++sDrawn;
  }
  gpRender->SetAmbientColor(CColor::White());
  CGraphics::DisableAllLights();
  if (baked) {
    PortRoomEnv::ClearVolumeHint();
  }
  return GetMode() == Mode::Replace;
}

void AddSorted(const CGameArea& gameArea) {
  const auto found = Areas().find(gameArea.GetAreaAssetId());
  if (found == Areas().end()) {
    return;
  }
  if (sQueued && sQueuedGeneration != sGeneration) {
    sQueueStale = true;
  }
  sQueued = true;
  sQueuedGeneration = sGeneration;
  const CVector3f forward = CGraphics::GetViewMatrix().GetForward();
  for (const Placed* item : found->second.sorted) {
    gpRender->AddDrawable(item, item->bounds.ClosestPointAlongVector(forward), item->bounds, kDrawableType,
                          IRenderer::kDS_SortedCallback);
  }
}

void DrawSorted(const void* drawable) {
  // The models of the area it was added for are still there: the list is rebuilt by Draw
  // every frame, and an area's models only go between frames. Should one go mid-frame
  // all the same (the console turning room geometry off), the item is not touched.
  if (sQueueStale || sQueuedGeneration != sGeneration) {
    return;
  }
  const Placed& item = *static_cast< const Placed* >(drawable);
  if (item.owner == nullptr) {
    return;
  }
  const Model& model = item.owner->models[item.model];
  if (!model.loaded) {
    return;
  }
  if (item.volume != 0) {
    const CVector3f centre = item.bounds.GetCenterPoint();
    const float at[3] = {centre.GetX(), centre.GetY(), centre.GetZ()};
    PortRoomEnv::SetVolumeHint(item.volume, at);
  }
  gpRender->SetModelMatrix(item.xf);
  item.lights->ActivateLights();
  const CModel& cmodel = **model.data->PickStaticModel(CModelData::kWM_Normal);
  if (item.alpha < 1.f) {
    cmodel.Draw(CModelFlags(CModelFlags::kT_Blend, item.alpha));
  } else {
    cmodel.DrawSortedParts(CModelFlags::Normal());
  }
  gpRender->SetAmbientColor(CColor::White());
  CGraphics::DisableAllLights();
  if (item.volume != 0) {
    PortRoomEnv::ClearVolumeHint();
  }
}

bool sReplacingArea = false;

void OnScriptState(CStateManager& mgr, uint32_t editorId, int state) {
  const TAreaId areaId(int((editorId >> 16) & 0x3ff));
  const CWorld* const world = mgr.GetWorld();
  // An area's objects are made after it counts as loaded (CGameArea::PostConstructArea), so
  // a state from elsewhere (editor id 0 reads as area 0) loads nothing.
  if (GetMode() == Mode::Off || world == nullptr || areaId.Value() >= world->GetNumAreas() ||
      !world->GetArea(areaId)->IsLoaded()) {
    return;
  }
  const uint32_t mrea = world->GetArea(areaId)->GetAreaAssetId();
  auto& areas = Areas();
  auto found = areas.find(mrea);
  if (found == areas.end()) {
    // Script objects are made, and some send states, before the next SetLoadedAreas, which
    // keeps the area (an area without a file stays in as an empty one).
    found = areas.emplace(mrea, Area()).first;
    Load(mrea, found->second);
  }
  if (!sTriggers) {
    return;
  }
  Area& area = found->second;
  const uint32_t sender = editorId & 0x3ffffff;
  for (const Trigger& trigger : area.triggers) {
    if (trigger.sender != sender || trigger.state != state) {
      continue;
    }
    Placed& item = area.items[trigger.item];
    const bool shown = trigger.action == kShow ? true : trigger.action == kHide ? false : !item.shown;
    if (shown != item.shown) {
      char line[96];
      std::snprintf(line, sizeof(line), "room geo: %08X: instance %u %s by %08X\n", mrea,
                    unsigned(trigger.item), shown ? "shown" : "hidden", sender);
      PortLog::Write(line);
    }
    item.shown = shown;
  }
  if (area.retailEdges) {
    for (const ScriptEdge& edge : area.script.edges) {
      if (edge.retail && (edge.from & 0x3ffffff) == sender && edge.event == state) {
        sApplyBudget = kApplyBudget;
        Apply(area, edge, 0);
      }
    }
  }
}

void Think(CStateManager& mgr) {
  const CWorld* const world = mgr.GetWorld();
  if (GetMode() == Mode::Off || world == nullptr) {
    return;
  }
  auto& areas = Areas();
  bool any = false;
  for (const auto& [mrea, area] : areas) {
    any = any || !area.script.nodes.empty();
  }
  if (!any) {
    return;
  }
  const CVector3f camera = mgr.CameraManager()->CurrentCamera(mgr).GetTranslation();
  for (int i = 0; i < world->GetNumAreas(); ++i) {
    const CGameArea* const gameArea = world->GetArea(TAreaId(i));
    if (!gameArea->IsLoaded()) {
      continue;
    }
    const auto found = areas.find(gameArea->GetAreaAssetId());
    if (found == areas.end() || found->second.script.nodes.empty()) {
      continue;
    }
    Area& area = found->second;
    // Volumes are in the area's own space, as its script objects are.
    const CVector3f local = gameArea->GetInverseTransform() * camera;
    const float at[3] = {local.GetX(), local.GetY(), local.GetZ()};
    std::memcpy(area.camera, at, sizeof(at));
    for (size_t n = 0; n < area.nodes.size(); ++n) {
      const ScriptNode& node = area.script.nodes[n];
      NodeState& state = area.nodes[n];
      // An inactive trigger does not think (CScriptTrigger::Think), so it sees nothing.
      if (node.kind != kCameraVolume || !state.active) {
        continue;
      }
      const float d[3] = {at[0] - node.centre[0], at[1] - node.centre[1], at[2] - node.centre[2]};
      bool inside = true;
      for (int axis = 0; axis < 3 && inside; ++axis) {
        const float* const a = node.axes + 3 * axis;
        inside = std::fabs(d[0] * a[0] + d[1] * a[1] + d[2] * a[2]) <= node.half[axis];
      }
      if (inside != state.inside) {
        state.inside = inside;
        sApplyBudget = kApplyBudget;
        Send(area, uint32_t(n), inside ? 0 : 1, 0);
      }
    }
  }
}

std::string ScriptInfo() {
  std::string out;
  char line[200];
  for (const auto& [mrea, area] : Areas()) {
    if (area.script.nodes.empty()) {
      continue;
    }
    std::snprintf(line, sizeof(line), "%08X: camera %.1f %.1f %.1f, %zu node(s), %zu group(s)\n", mrea,
                  area.camera[0], area.camera[1], area.camera[2], area.nodes.size(), area.groups.size());
    out += line;
    for (size_t n = 0; n < area.nodes.size(); ++n) {
      const ScriptNode& node = area.script.nodes[n];
      const NodeState& state = area.nodes[n];
      if (node.kind == kCameraVolume) {
        std::snprintf(line, sizeof(line), "  %zu volume%s%s at %.1f %.1f %.1f, half %.1f %.1f %.1f\n", n,
                      state.active ? "" : " (off)", state.inside ? " CAMERA IN" : "", node.centre[0],
                      node.centre[1], node.centre[2], node.half[0], node.half[1], node.half[2]);
      } else {
        std::snprintf(line, sizeof(line), "  %zu %s%s %u/%u\n", n, node.kind == kCounter ? "counter" : "relay",
                      state.active ? "" : " (off)", state.count, node.max);
      }
      out += line;
    }
    for (size_t g = 0; g < area.groups.size(); ++g) {
      size_t shown = 0;
      for (const size_t i : area.groups[g]) {
        shown += area.items[i].shown;
      }
      std::snprintf(line, sizeof(line), "  group %zu: %zu of %zu shown\n", g, shown, area.groups[g].size());
      out += line;
    }
  }
  return out;
}

int SetGroupShown(uint32_t group, bool shown) {
  int count = 0;
  for (auto& [mrea, area] : Areas()) {
    if (group < area.groups.size()) {
      for (const size_t i : area.groups[group]) {
        area.items[i].shown = shown;
        ++count;
      }
    }
  }
  return count;
}

void ResetScriptState() {
  for (auto& [mrea, area] : Areas()) {
    ResetNodes(area);
    for (Placed& item : area.items) {
      item.shown = item.active;
      item.following = false;
      item.alpha = 1.f;
    }
  }
}

std::string At(const CVector3f& point, float margin) {
  std::string out;
  char line[160];
  for (const auto& [mrea, area] : Areas()) {
    for (const Placed& item : area.items) {
      const Model& model = area.models[item.model];
      if (!item.bounded) {
        continue;
      }
      const CVector3f lo = item.bounds.GetMinPoint();
      const CVector3f hi = item.bounds.GetMaxPoint();
      if (point.GetX() < lo.GetX() - margin || point.GetX() > hi.GetX() + margin ||
          point.GetY() < lo.GetY() - margin || point.GetY() > hi.GetY() + margin ||
          point.GetZ() < lo.GetZ() - margin || point.GetZ() > hi.GetZ() + margin) {
        continue;
      }
      std::snprintf(line, sizeof(line), "%08X in %08X: (%.1f, %.1f, %.1f) to (%.1f, %.1f, %.1f)%s\n", model.id,
                    mrea, lo.GetX(), lo.GetY(), lo.GetZ(), hi.GetX(), hi.GetY(), hi.GetZ(),
                    model.hidden ? " hidden" : "");
      out += line;
    }
  }
  return out;
}

int SetHidden(uint32_t id, bool hidden) {
  int count = 0;
  for (auto& [mrea, area] : Areas()) {
    for (Model& model : area.models) {
      if (id == 0 || model.id == id) {
        model.hidden = hidden;
        ++count;
      }
    }
  }
  return count;
}

uint32_t Pick(const CVector3f& origin, const CVector3f& direction, std::string& out) {
  struct Hit {
    float key; // distance to the box, or its volume when the origin is inside
    bool inside;
    uint32_t mrea;
    const Placed* item;
  };
  std::vector< Hit > hits;
  const float o[3] = {origin.GetX(), origin.GetY(), origin.GetZ()};
  const float d[3] = {direction.GetX(), direction.GetY(), direction.GetZ()};
  for (const auto& [mrea, area] : Areas()) {
    for (const Placed& item : area.items) {
      if (!item.bounded || area.models[item.model].hidden || !item.shown) {
        continue;
      }
      const CVector3f lo = item.bounds.GetMinPoint();
      const CVector3f hi = item.bounds.GetMaxPoint();
      const float l[3] = {lo.GetX(), lo.GetY(), lo.GetZ()};
      const float h[3] = {hi.GetX(), hi.GetY(), hi.GetZ()};
      float enter = -3.4e38f;
      float leave = 3.4e38f;
      bool miss = false;
      for (int axis = 0; axis < 3 && !miss; ++axis) {
        if (std::fabs(d[axis]) < 1e-8f) {
          miss = o[axis] < l[axis] || o[axis] > h[axis];
          continue;
        }
        float t0 = (l[axis] - o[axis]) / d[axis];
        float t1 = (h[axis] - o[axis]) / d[axis];
        if (t0 > t1) {
          std::swap(t0, t1);
        }
        enter = std::max(enter, t0);
        leave = std::min(leave, t1);
      }
      if (miss || enter > leave || leave < 0.f) {
        continue;
      }
      const bool inside = enter <= 0.f;
      hits.push_back({inside ? (h[0] - l[0]) * (h[1] - l[1]) * (h[2] - l[2]) : enter, inside, mrea, &item});
    }
  }
  std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
    return a.inside != b.inside ? !a.inside : a.key < b.key;
  });
  constexpr size_t kLines = 12;
  char line[200];
  for (size_t i = 0; i < hits.size() && i < kLines; ++i) {
    const Hit& hit = hits[i];
    const CVector3f lo = hit.item->bounds.GetMinPoint();
    const CVector3f hi = hit.item->bounds.GetMaxPoint();
    const uint32_t id = Areas()[hit.mrea].models[hit.item->model].id;
    if (hit.inside) {
      std::snprintf(line, sizeof(line), "%08X in %08X: around the camera, (%.1f, %.1f, %.1f) to (%.1f, %.1f, %.1f)\n",
                    id, hit.mrea, lo.GetX(), lo.GetY(), lo.GetZ(), hi.GetX(), hi.GetY(), hi.GetZ());
    } else {
      std::snprintf(line, sizeof(line), "%08X in %08X: %.1f m, (%.1f, %.1f, %.1f) to (%.1f, %.1f, %.1f)\n", id,
                    hit.mrea, hit.key, lo.GetX(), lo.GetY(), lo.GetZ(), hi.GetX(), hi.GetY(), hi.GetZ());
    }
    out += line;
  }
  if (hits.size() > kLines) {
    std::snprintf(line, sizeof(line), "and %zu more\n", hits.size() - kLines);
    out += line;
  }
  return hits.empty() ? 0 : Areas()[hits[0].mrea].models[hits[0].item->model].id;
}

std::string Materials(uint32_t id) {
  static const char* const kTags[] = {"PBRM", "PBR2", "PBR3", "PBR4"};
  for (const auto& [mrea, area] : Areas()) {
    for (const Model& model : area.models) {
      const CCubeModel* const cube = model.id == id ? CubeModel(model) : nullptr;
      if (cube == nullptr) {
        continue;
      }
      std::string out;
      char line[320];
      const int count = int(cube->PortMaterialCount());
      for (int i = 0; i < count; ++i) {
        const uint flags = cube->GetMaterialByIndex(i).GetFlags();
        float v[19];
        const int floats = cube->PortReadPBRMaterial(i, v);
        const char* const tag =
            floats == 19 ? kTags[3] : floats == 13 ? kTags[2] : floats == 8 ? kTags[1] : floats == 6 ? kTags[0] : "none";
        // What the console's `roomgeo mat` put in place is what gets drawn, so show that.
        int shown = floats;
        bool overridden = false;
        for (const MaterialValue& value : sMaterialValues) {
          if (value.id == id && value.material == i) {
            v[value.field] = value.value;
            overridden = true;
            shown = std::max(shown, value.field < 6 ? 6 : value.field < 8 ? 8 : 19);
          }
        }
        int used = std::snprintf(line, sizeof(line), "%d: flags %08X %s%s%s, record %s", i, flags,
                                 (flags & kStateFlag_PortPBR) != 0 ? "PBR" : "TEV",
                                 (flags & kStateFlag_DepthSorting) != 0 ? " blended" : "",
                                 (flags & kStateFlag_AlphaTest) != 0 ? " cutout" : "", tag);
        if (shown > 0 && used < int(sizeof(line))) {
          used += std::snprintf(line + used, sizeof(line) - used,
                                ", emissive %g %g %g, backlight %g %g %g", v[0], v[1], v[2], v[3], v[4], v[5]);
        }
        if (shown >= 8 && used < int(sizeof(line))) {
          used += std::snprintf(line + used, sizeof(line) - used, ", height %g, mode %g", v[6], v[7]);
        }
        if (shown >= 19 && used < int(sizeof(line))) {
          used += std::snprintf(line + used, sizeof(line) - used, ", kind %g, strength %g, params %g %g %g %g",
                                v[13], v[14], v[15], v[16], v[17], v[18]);
        }
        if (overridden && used < int(sizeof(line))) {
          std::snprintf(line + used, sizeof(line) - used, " (overridden)");
        }
        out += line;
        out += '\n';
      }
      return out;
    }
  }
  return {};
}

bool SetMaterialValue(uint32_t id, int material, int field, float value) {
  if (material < 0 || field < 0 || field >= 19) {
    return false;
  }
  bool found = false;
  for (const auto& [mrea, area] : Areas()) {
    for (const Model& model : area.models) {
      const CCubeModel* const cube = model.id == id ? CubeModel(model) : nullptr;
      found = found || (cube != nullptr && uint(material) < cube->PortMaterialCount());
    }
  }
  if (!found) {
    return false;
  }
  for (MaterialValue& entry : sMaterialValues) {
    if (entry.id == id && entry.material == material && entry.field == field) {
      entry.value = value;
      BindMaterialValues();
      return true;
    }
  }
  sMaterialValues.push_back({id, material, field, value});
  BindMaterialValues();
  return true;
}

int ClearMaterialValues() {
  const int count = int(sMaterialValues.size());
  sMaterialValues.clear();
  CCubeModel::PortClearPBROverrides();
  return count;
}

void Reset() {
  CCubeModel::PortClearPBROverrides();
  Areas().clear();
  ++sGeneration;
}

bool AreaLights() {
  if (sAreaLights < 0) {
    sAreaLights = std::getenv("MP_ROOM_GEO_AREA_LIGHTS") != nullptr ? 1 : 0;
  }
  return sAreaLights != 0;
}

void SetAreaLights(bool on) { sAreaLights = on ? 1 : 0; }

void SetMode(Mode mode) {
  sMode = int(mode);
  if (mode == Mode::Off) {
    Reset();
  }
}

void SetBuffersReady(bool ready) { sBuffersReady = ready; }

Mode GetMode() {
  if (!sBuffersReady) {
    return Mode::Off;
  }
  if (sMode < 0) {
    const char* const env = std::getenv("MP_ROOM_GEO");
    sMode = int(env == nullptr || env[0] == '\0' ? Mode::Replace
                : env[0] == '0'                  ? Mode::Off
                : env[0] == 'o'                  ? Mode::Overlay
                                                 : Mode::Replace);
  }
  return Mode(sMode);
}

void Stats(int& areaCount, int& instances, int& models, int& loaded, int& drawn) {
  areaCount = instances = models = loaded = 0;
  for (const auto& [mrea, area] : Areas()) {
    if (!area.hasFile) {
      continue;
    }
    ++areaCount;
    instances += int(area.items.size());
    models += int(area.models.size());
    for (const Model& model : area.models) {
      loaded += model.loaded ? 1 : 0;
    }
  }
  drawn = sDrawnLast;
}

} // namespace PortRoomGeo
