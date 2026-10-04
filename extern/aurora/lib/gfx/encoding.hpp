#pragma once

#include "frame_packet.hpp"

namespace aurora::gfx {

bool bind_pipeline(PipelineRef ref, const wgpu::RenderPassEncoder& pass);
// Sets GX texture bind group 2 unless it is already bound in this pass. Dawn tracks every
// texture in a bind group each time it is set, which dominates encoding with many draws.
void bind_texture_group(BindGroupRef ref, const wgpu::RenderPassEncoder& pass);
// Call after setting bind group 2 directly, so the next bind_texture_group sets it again.
void forget_texture_group();

namespace detail {
void encode_op(wgpu::CommandEncoder& encoder, FramePacket& frame, const FrameOp& op);
}

} // namespace aurora::gfx
