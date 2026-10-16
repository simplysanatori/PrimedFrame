#pragma once

#include "types.hpp"

namespace aurora::gfx {

BindGroupRef bind_group_ref(const WGPUBindGroupDescriptor& descriptor);
// Same, with an id the caller derived from a key that identifies the descriptor (cheaper than hashing every
// entry); the descriptor is only read when the bind group isn't cached yet.
BindGroupRef bind_group_ref(BindGroupRef id, const WGPUBindGroupDescriptor& descriptor);
wgpu::BindGroup find_bind_group(BindGroupRef id);
wgpu::Sampler sampler_ref(const wgpu::SamplerDescriptor& descriptor);

namespace detail {
void clear_bind_group_cache();
void expire_cached_bind_groups();
// After an EFB resize that kept the cache: two frames on, drop what those frames did not use.
void expire_unused_bind_groups_soon();
void shutdown_resource_cache();
} // namespace detail

} // namespace aurora::gfx
