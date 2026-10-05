#pragma once

// Runtime half of the native Remastered particle material (VMAT). The draw itself is
// aurora::gfx::vfx::draw_quads; this file evaluates the per-particle properties (SSZE, ITEN, VPMT)
// and the per-frame VSMT, and expands the quads. Contract: build/mpr/vfx/DESIGN.md.
// A PART without VMAT never reaches any of it.

#include "Kyoto/Particles/CElementGen.hpp"
#include "Kyoto/Particles/CGenDescription.hpp"

// True when the description carries a VMAT this runtime understands (version 2).
inline bool PortVfxActive(const CGenDescription& desc) {
  return desc.xPortVfx != nullptr && desc.xPortVfx->mat.version == 2;
}

// Evaluates SSZE (default: the particle's SIZE), ITEN (default 1) and the VPMT rows into the
// particle. The caller has set CParticleGlobals' particle context for `frame`.
void PortVfxEvalParticle(const CGenDescription& desc, CElementGen::CParticle& particle, int frame);

// Stores the unit launch direction (zero when the particle launched at rest); for VORN 1.
void PortVfxSetLaunchDir(CElementGen::CParticle& particle);
