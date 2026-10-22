#include "port_statedump.h"

#include "port_savestate.h"
#include "port_savestate_boss.h"
#include "MetroidPrime/BodyState/CBodyController.hpp"
#include "MetroidPrime/CGameArea.hpp"
#include "MetroidPrime/CHealthInfo.hpp"
#include "MetroidPrime/CObjectList.hpp"
#include "MetroidPrime/CPhysicsActor.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/CWorld.hpp"
#include "MetroidPrime/Enemies/CPatterned.hpp"
#include "MetroidPrime/Player/CPlayer.hpp"
#include "MetroidPrime/Player/CPlayerState.hpp"
#include "MetroidPrime/ScriptObjects/CScriptDoor.hpp"
#include "MetroidPrime/TCastTo.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <typeinfo>
#if defined(__GNUC__)
#include <cxxabi.h>
#endif

namespace PortStateDump {
namespace {
// The class naming of the `objs` console command.
std::string ClassName(const CEntity& ent) {
  const char* raw = typeid(ent).name();
#if defined(__GNUC__)
  int status = 0;
  char* demangled = abi::__cxa_demangle(raw, nullptr, nullptr, &status);
  if (status == 0 && demangled != nullptr) {
    std::string name = demangled;
    std::free(demangled);
    return name;
  }
#endif
  return raw;
}

std::string Esc(const char* s) {
  std::string out;
  for (; s != nullptr && *s != '\0'; ++s) {
    const unsigned char c = static_cast< unsigned char >(*s);
    if (c == '"' || c == '\\') {
      out += '\\';
      out += static_cast< char >(c);
    } else if (c < 0x20 || c >= 0x7f) {
      out += '?';
    } else {
      out += static_cast< char >(c);
    }
  }
  return out;
}

std::string Fmt(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
std::string Fmt(const char* fmt, ...) {
  char buf[512];
  va_list ap;
  va_start(ap, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  return buf;
}

std::string Vec(const CVector3f& v) { return Fmt("[%.4f,%.4f,%.4f]", v.GetX(), v.GetY(), v.GetZ()); }

std::string Hex(const std::string& bytes) {
  static const char digits[] = "0123456789abcdef";
  std::string out;
  for (char c : bytes) {
    out += digits[(static_cast< unsigned char >(c) >> 4) & 15];
    out += digits[static_cast< unsigned char >(c) & 15];
  }
  return out;
}

std::string ObjectLine(CStateManager& mgr, CEntity& ent) {
  const int area = ent.GetAreaId().Value();
  uint32_t mrea = 0;
  const CWorld* world = mgr.GetWorld();
  if (world != nullptr && area >= 0 && area < world->GetNumAreas())
    mrea = static_cast< uint32_t >(world->GetAreaAlways(TAreaId(area)).GetAreaAssetId());
  std::string line = Fmt("{\"t\":\"obj\",\"uid\":%u,\"eid\":\"%08X\",\"area\":%d,\"mrea\":\"%08X\","
                         "\"class\":\"%s\",\"name\":\"%s\",\"active\":%d,\"generated\":%d",
                         ent.GetUniqueId().Value(), ent.GetEditorId().Value(), area, mrea,
                         Esc(ClassName(ent).c_str()).c_str(), Esc(ent.GetDebugName().data()).c_str(),
                         ent.GetActive() ? 1 : 0,
                         PortSaveState::IsScriptGeneratedCopy(mgr, ent) ? 1 : 0);
  if (CActor* actor = TCastToPtr< CActor >(&ent)) {
    if (const CHealthInfo* health = actor->GetHealthInfo(mgr))
      line += Fmt(",\"hp\":%.4f,\"kbres\":%.4f", health->GetHP(), health->GetKnockBackResistance());
    line += ",\"pos\":" + Vec(actor->GetTranslation());
    line += ",\"fwd\":" + Vec(actor->GetTransform().GetForward());
  }
  if (const CPhysicsActor* physics = TCastToConstPtr< CPhysicsActor >(&ent)) {
    line += ",\"vel\":" + Vec(physics->GetVelocityWR());
    line += ",\"mom\":" + Vec(physics->GetMomentumWR());
  }
  if (const CScriptDoor* door = TCastToConstPtr< CScriptDoor >(&ent))
    line += Fmt(",\"door_open\":%d", door->IsOpen() ? 1 : 0);
  float time = 0.f;
  bool timing = false;
  if (PortSaveState::ScriptTimerState(ent, time, timing))
    line += Fmt(",\"timer\":%.4f,\"timing\":%d", time, timing ? 1 : 0);
  int current = 0;
  if (PortSaveState::ScriptCounterState(ent, current))
    line += Fmt(",\"counter\":%d", current);
  if (CPatterned* patterned = TCastToPtr< CPatterned >(&ent)) {
    const CAiState* state = patterned->GetStateMachineState().GetActorState();
    line += Fmt(",\"ai\":\"%s\",\"ai_time\":%.4f", state != nullptr ? Esc(state->GetName()).c_str() : "",
                patterned->GetStateMachineTime());
    if (const CBodyController* body = patterned->GetBodyCtrl()) {
      line += Fmt(",\"body\":%d,\"loco\":%d,\"anim\":%d", static_cast< int >(body->GetCurrentStateId()),
                  static_cast< int >(body->GetLocomotionType()), body->GetCurrentAnimId());
    }
  }
  std::string boss;
  if (PortSaveState::CaptureBossState(mgr, ent, boss))
    line += ",\"boss\":\"" + Hex(boss) + "\"";
  return line + "}\n";
}

std::string PlayerLine(CStateManager& mgr) {
  CPlayer& player = *mgr.Player();
  const CPlayerState& ps = *mgr.GetPlayerState();
  return Fmt("{\"t\":\"player\",\"pos\":%s,\"fwd\":%s,\"morph\":%d,\"hp\":%.4f,\"area\":%d}\n",
             Vec(player.GetTranslation()).c_str(), Vec(player.GetTransform().GetForward()).c_str(),
             static_cast< int >(player.GetMorphballTransitionState()), ps.GetHealthInfo().GetHP(),
             player.GetAreaId().Value());
}
} // namespace

int Write(CStateManager& mgr, const std::string& path, std::string& error) {
  std::string text = PlayerLine(mgr);
  int count = 0;
  const CObjectList& list = mgr.GetObjectListById(kOL_All);
  for (int i = list.GetFirstObjectIndex(); i != -1; i = list.GetNextObjectIndex(i)) {
    CEntity* ent = const_cast< CEntity* >(list[i]);
    if (ent == nullptr)
      continue;
    text += ObjectLine(mgr, *ent);
    ++count;
  }
  FILE* f = std::fopen(path.c_str(), "wb");
  if (f == nullptr || std::fwrite(text.data(), 1, text.size(), f) != text.size()) {
    error = "can't write " + path;
    if (f != nullptr)
      std::fclose(f);
    return -1;
  }
  std::fclose(f);
  return count;
}
} // namespace PortStateDump
