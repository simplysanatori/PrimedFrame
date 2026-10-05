#pragma once

// Runtime half of the native Remastered particle material (VMAT). The draw itself is
// aurora::gfx::vfx::draw_quads; this file evaluates the per-particle properties (SSZE, ITEN, VPMT)
// and the per-frame VSMT, and expands the quads. Contract: build/mpr/vfx/DESIGN.md.
// A PART without VMAT never reaches any of it.

#include "Kyoto/Particles/CElementGen.hpp"
#include <vector>

#include "Kyoto/Particles/CGenDescription.hpp"
#include "Kyoto/Math/CTransform4f.hpp"
#include "Kyoto/Graphics/CColor.hpp"
#include "aurora/vfx.hpp"

// True when the description carries a VMAT this runtime understands (version 2).
inline bool PortVfxActive(const CGenDescription& desc) {
  return desc.xPortVfx != nullptr && desc.xPortVfx->mat.version == 2;
}

// Evaluates SSZE (default: the particle's SIZE), ITEN (default 1) and the VPMT rows into the
// particle. The caller has set CParticleGlobals' particle context for `frame`.
void PortVfxEvalParticle(const CGenDescription& desc, CElementGen::CParticle& particle, int frame);

// Stores the unit launch direction (zero when the particle launched at rest); for VORN 1.
void PortVfxSetLaunchDir(CElementGen::CParticle& particle);

// Model particles with a VMSH mesh: RenderModels adds each live particle's CPU-transformed
// triangles (world space) and PortRenderMeshesVfx draws them all in one aurora draw with the
// PART's VMAT. A PART with PMDV variants keeps the retail model path (VMSH holds one model).
struct CPortVfxMeshBatch {
  std::vector< aurora::gfx::vfx::Vertex > verts;

  // True when adding `vfx`'s mesh would take the batch past one aurora draw (the size
  // draw_triangles splits at); the caller draws and clears it first, so it stays bounded.
  bool WouldOverflow(const CPortVfxData& vfx) const {
    return !verts.empty() &&
           verts.size() + size_t(vfx.meshTris) * 3 > size_t(aurora::gfx::vfx::MaxTrianglesPerDraw) * 3;
  }

  // `model` is the particle's full model matrix; the particle's UV context must be set.
  void Add(const CPortVfxData& vfx, const CTransform4f& model, const CElementGen::CParticle& particle,
           int partFrame, const CColor& modulate);
};

inline bool PortVfxHasMesh(const CGenDescription& desc) {
  return PortVfxActive(desc) && desc.xPortVfx->meshTris != 0 && desc.xPortPMDV.empty();
}
