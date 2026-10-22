#include "port_savestate.h"

#include "port_entity_schema.h"

#include "MetroidPrime/CEntity.hpp"
#include "MetroidPrime/CStateManager.hpp"
#include "MetroidPrime/Enemies/CPatterned.hpp"
#include "MetroidPrime/TCastTo.hpp"

#include <cstdlib>
#include <cstring>
#include <vector>

// The kTagFields block: an enemy's own fields, read and written through the generated
// schema (port_entity_schema.h) instead of per-class code. Per class of the entity's chain,
// most derived first: its fingerprint, the payload length, then each saved field in table
// order. A reader skips a class whose fingerprint it does not have (the headers changed)
// and the whole block when the policy below differs, so an old save falls back to the
// restore the other tags do.
//
// Saved: the plain kinds (bool, int, float, bitfield, math), pod structs, and uids, which
// go by the editor id of the object they name. A uid naming an object without one (a
// collision actor or projectile the enemy made) is left as the rebuilt enemy has it.
// Not saved: pointers, smart pointers, containers and the kinds that need a codec of
// their own (state machine, body controller, animation; docs/SAVESTATE_SCHEMA.md),
// CEntity's fields (the room record's own), most of CActor's and CPhysicsActor's (pose,
// motion and health are restored by their tags; the rest is render and collision
// bookkeeping the engine keeps), and the port's interpolation fields.

namespace PortSaveState {
namespace {

// Bump when what SavedField picks or how a kind is written changes.
constexpr uint32_t kFieldPolicy = 1;

constexpr uint32_t kUidNone = 0xFFFFFFFFu;    // kInvalidUniqueId
constexpr uint32_t kUidDynamic = 0xFFFFFFFEu; // an object without an editor id: left alone

bool Named(const char* name, std::initializer_list< const char* > names) {
  for (const char* n : names)
    if (std::strcmp(name, n) == 0)
      return true;
  return false;
}

bool SavedField(const PortSchemaClass& cls, const PortSchemaField& f) {
  if ((f.flags & kPortSchemaConst) != 0)
    return false;
  if (std::strncmp(f.name, "xPort", 5) == 0 || std::strncmp(f.name, "xpc_", 4) == 0)
    return false;
  switch (f.kind) {
  case PortSchemaKind::bool_:
  case PortSchemaKind::int_:
  case PortSchemaKind::float_:
  case PortSchemaKind::bitfield:
  case PortSchemaKind::math:
  case PortSchemaKind::uid:
  case PortSchemaKind::pod:
    break;
  default:
    return false;
  }
  if (f.kind != PortSchemaKind::bitfield && f.addr == nullptr)
    return false;
  if (std::strcmp(cls.name, "CEntity") == 0)
    return false;
  if (std::strcmp(cls.name, "CActor") == 0)
    return Named(f.name, {"xb4_drawFlags", "xd0_damageMag", "xe5_26_muted", "xe5_28_callTouch",
                          "xe6_27_thermalVisorFlags", "xe6_29_renderParticleDBInside",
                          "xe6_31_targetableVisorFlags", "xe7_27_enableRender", "xe7_29_drawEnabled",
                          "xe7_30_doTargetDistanceTest", "xe7_31_targetable"});
  if (std::strcmp(cls.name, "CPhysicsActor") == 0)
    return Named(f.name, {"xf8_24_movable", "xf8_25_angularEnabled", "x108_angularMomentum",
                          "x144_angularVelocity", "x24c_numTicksStuck"});
  // CAi's health is the kTagHealth block's (Omega Pirate keeps its own elsewhere).
  if (std::strcmp(cls.name, "CAi") == 0)
    return std::strcmp(f.name, "x258_healthInfo") != 0;
  return true;
}

bool Enabled() {
  static const bool on = [] {
    const char* v = std::getenv("MP_SAVESTATE_FIELDS");
    return v == nullptr || std::strcmp(v, "0") != 0;
  }();
  return on;
}

// The schema classes of `ent`, most derived first.
std::vector< const PortSchemaClass* > Chain(const CEntity& ent) {
  std::vector< const PortSchemaClass* > chain;
  for (const PortSchemaClass* c = PortEntitySchema::For(ent); c != nullptr && chain.size() < 32;
       c = PortEntitySchema::Base(*c))
    chain.push_back(c);
  return chain;
}

void PutVar(BlockWriter& w, long long v) {
  uint64_t z = (static_cast< uint64_t >(v) << 1) ^ static_cast< uint64_t >(v >> 63);
  while (z >= 0x80) {
    w.U8(static_cast< uint8_t >(z | 0x80));
    z >>= 7;
  }
  w.U8(static_cast< uint8_t >(z));
}

long long GetVar(BlockReader& r) {
  uint64_t z = 0;
  for (int shift = 0; shift < 64; shift += 7) {
    const uint8_t b = r.U8();
    if (!r.ok)
      return 0;
    z |= static_cast< uint64_t >(b & 0x7F) << shift;
    if ((b & 0x80) == 0)
      return static_cast< long long >((z >> 1) ^ (~(z & 1) + 1));
  }
  r.ok = false;
  return 0;
}

void PutBytes(BlockWriter& w, const void* p, size_t n) {
  w.bytes.append(static_cast< const char* >(p), n);
}

bool GetBytes(BlockReader& r, void* p, size_t n) {
  if (!r.ok || r.bytes.size() - r.pos < n) {
    r.ok = false;
    return false;
  }
  std::memcpy(p, r.bytes.data() + r.pos, n);
  r.pos += n;
  return true;
}

void PutField(const CStateManager& mgr, CEntity& ent, const PortSchemaField& f, BlockWriter& w) {
  switch (f.kind) {
  case PortSchemaKind::bool_:
  case PortSchemaKind::int_:
  case PortSchemaKind::bitfield:
    PutVar(w, f.getInt != nullptr ? f.getInt(ent) : 0);
    return;
  case PortSchemaKind::uid: {
    const TUniqueId uid = *static_cast< const TUniqueId* >(f.addr(ent));
    uint32_t id = kUidNone;
    if (uid != kInvalidUniqueId) {
      // A script generator's copy carries its template's editor id: not a name for it.
      const CEntity* target = mgr.GetObjectById(uid);
      const TEditorId eid = mgr.GetEditorIdForUniqueId(uid);
      id = target != nullptr && eid != kInvalidEditorId &&
                   !IsScriptGeneratedCopy(mgr, const_cast< CEntity& >(*target))
               ? eid.Value()
               : kUidDynamic;
    }
    w.U32(id);
    return;
  }
  default:
    // float, math and pod: the bytes, after their size (a pod's differs between
    // compilers when its padding does; a reader skips one it cannot take).
    w.U32(f.size);
    PutBytes(w, f.addr(ent), f.size);
    return;
  }
}

// Returns whether the field was set; false with r.ok still true is a field skipped.
bool GetField(CStateManager& mgr, CEntity& ent, const PortSchemaField& f, BlockReader& r) {
  switch (f.kind) {
  case PortSchemaKind::bool_:
  case PortSchemaKind::int_:
  case PortSchemaKind::bitfield: {
    const long long v = GetVar(r);
    if (!r.ok || f.setInt == nullptr)
      return false;
    if (f.kind == PortSchemaKind::bool_ && v != 0 && v != 1)
      return false;
    f.setInt(ent, v);
    return true;
  }
  case PortSchemaKind::uid: {
    const uint32_t id = r.U32();
    if (!r.ok || id == kUidDynamic)
      return false;
    TUniqueId uid = kInvalidUniqueId;
    if (id != kUidNone) {
      uid = mgr.GetIdForScript(TEditorId(id));
      if (uid == kInvalidUniqueId)
        return false;
    }
    *static_cast< TUniqueId* >(f.addr(ent)) = uid;
    return true;
  }
  default: {
    const uint32_t size = r.U32();
    if (!r.ok || r.bytes.size() - r.pos < size) {
      r.ok = false;
      return false;
    }
    if (size != f.size) {
      r.pos += size;
      return false;
    }
    std::vector< unsigned char > buf(size);
    GetBytes(r, buf.data(), size);
    // A vector or transform that went NaN or huge is not put back.
    for (uint32_t i = 0; f.kind == PortSchemaKind::math && i + 4 <= size; i += 4) {
      float v;
      std::memcpy(&v, buf.data() + i, 4);
      if (!(v == v) || v > 1e9f || v < -1e9f)
        return false;
    }
    std::memcpy(f.addr(ent), buf.data(), size);
    return true;
  }
  }
}

} // namespace

bool CaptureEntityFields(const CStateManager& mgr, CEntity& ent, std::string& out) {
  if (!Enabled() || TCastToConstPtr< CPatterned >(&ent) == nullptr)
    return false;
  const std::vector< const PortSchemaClass* > chain = Chain(ent);
  BlockWriter w;
  w.U32(kFieldPolicy);
  w.U8(static_cast< uint8_t >(chain.size()));
  for (const PortSchemaClass* cls : chain) {
    BlockWriter body;
    for (int i = 0; i < cls->fieldCount; ++i)
      if (SavedField(*cls, cls->fields[i]))
        PutField(mgr, ent, cls->fields[i], body);
    w.U32(static_cast< uint32_t >(cls->fingerprint));
    w.U32(static_cast< uint32_t >(cls->fingerprint >> 32));
    w.U32(static_cast< uint32_t >(body.bytes.size()));
    w.bytes += body.bytes;
  }
  out = std::move(w.bytes);
  return true;
}

int ApplyEntityFields(CStateManager& mgr, CEntity& ent, const std::string& data) {
  if (!Enabled())
    return 0;
  BlockReader r(data);
  if (r.U32() != kFieldPolicy || !r.ok)
    return 0;
  const std::vector< const PortSchemaClass* > chain = Chain(ent);
  const int classes = r.U8();
  int set = 0;
  for (int c = 0; c < classes && r.ok; ++c) {
    const uint64_t lo = r.U32();
    const uint64_t fingerprint = lo | (static_cast< uint64_t >(r.U32()) << 32);
    const uint32_t len = r.U32();
    if (!r.ok || r.bytes.size() - r.pos < len)
      break;
    const PortSchemaClass* cls = nullptr;
    for (const PortSchemaClass* k : chain)
      if (k->fingerprint == fingerprint)
        cls = k;
    const size_t end = r.pos + len;
    if (cls != nullptr) {
      const std::string body = data.substr(r.pos, len);
      BlockReader br(body);
      for (int i = 0; i < cls->fieldCount && br.ok; ++i)
        if (SavedField(*cls, cls->fields[i]) && GetField(mgr, ent, cls->fields[i], br))
          ++set;
    }
    r.pos = end;
  }
  return set;
}

} // namespace PortSaveState
