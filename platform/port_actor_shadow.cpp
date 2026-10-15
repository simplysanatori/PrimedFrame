#include "port_actor_shadow.h"

#include "MetroidPrime/CActor.hpp"
#include "MetroidPrime/CSimpleShadow.hpp"
#include "MetroidPrime/CStateManager.hpp"

#include "Kyoto/Math/CTransform4f.hpp"

#include "dolphin/gx/GXExtra.h"
#include "port_debug.h"
#include "port_room_geo.h"

#include <vector>

namespace PortActorShadow {
namespace {
std::vector<const CActor*> sActors;

Box ToBox(const float* v) { return {{v[0], v[1], v[2]}, {v[3], v[4], v[5]}}; }
} // namespace

bool Active() { return PortDebug::ProjectedShadows() && PortRoomGeo::GetMode() != PortRoomGeo::Mode::Off; }

void BeginFrame() { sActors.clear(); }

void Register(const CActor* actor) {
  if (sActors.size() < kMaxMaps) {
    sActors.push_back(actor);
  }
}

void RenderFrame(const CStateManager& mgr, const CTransform4f& view) {
  if (sActors.empty() || !Active()) {
    return;
  }
  // GX's view matrix (see PortSetupShadow): the camera's rows and their eye offset.
  const CVector3f eye = view.GetTranslation();
  const float axes[3][3] = {
      {view.Get00(), view.Get10(), view.Get20()},
      {view.Get02(), view.Get12(), view.Get22()},
      {-view.Get01(), -view.Get11(), -view.Get21()},
  };
  float w2v[3][4];
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      w2v[r][c] = axes[r][c];
    }
    w2v[r][3] = -(axes[r][0] * eye.GetX() + axes[r][1] * eye.GetY() + axes[r][2] * eye.GetZ());
  }
  for (size_t i = 0; i < sActors.size(); ++i) {
    const CActor& actor = *sActors[i];
    float clip[16];
    CasterClip(w2v, ToBox(actor.GetShadow()->PortMapBox()), clip);
    GXPortActorShadowCasters(static_cast<u32>(i), clip);
    CPortActorRenderScope presented(actor);
    actor.PortRenderActorShadowCaster(mgr);
  }
  GXPortActorShadowEnd();
  GXPortActorShadowRender();
  for (size_t i = 0; i < sActors.size(); ++i) {
    const CSimpleShadow& s = *sActors[i]->GetShadow();
    const Box rb = ToBox(s.PortReceiverBox());
    float params[kParamFloats];
    DecalParams(w2v, ToBox(s.PortMapBox()), rb, s.PortOpacity(), s.PortFadeStart(), static_cast<unsigned>(i), params);
    GXPortActorShadowDecal(static_cast<u32>(i), params);
    const float lo[3] = {rb.centre[0] - rb.half[0], rb.centre[1] - rb.half[1], rb.centre[2] - rb.half[2]};
    const float hi[3] = {rb.centre[0] + rb.half[0], rb.centre[1] + rb.half[1], rb.centre[2] + rb.half[2]};
    PortRoomGeo::DrawShadowReceivers(lo, hi);
  }
  GXPortActorShadowEnd();
}

} // namespace PortActorShadow
