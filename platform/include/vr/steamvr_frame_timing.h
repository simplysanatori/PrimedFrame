// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <functional>
#include <string>

// SteamVR's compositor frame timing, read inside the game (PrimedFrame 2.0), for the end-phase
// lock (PF2_END_PHASE, openxr_vulkan_direct.cpp): no helper program, no second Steam entry.
//
// SteamVR's OpenXR runtime IS its OpenVR client: the OpenXR loader loads
// <SteamVR>/bin/linuxarm64/vrclient.so (it exports both xrNegotiateLoaderRuntimeInterface and
// VRClientCoreFactory), and xrCreateInstance initializes the same client core VR_Init would.
// A second VR_Init in this process (what dev/tools/vrtiming.py does from its own process, as a
// background app) is what vrclient itself calls undefined ("Process called VR_Init or
// xrCreateInstance %u times without a matching VR_Shutdown or xrDestroyInstance. Behavior is
// undefined.") and could change the process's application type. So this reader never calls Init
// or Cleanup: it finds the vrclient.so the loader already loaded (dl_iterate_phdr, then
// dlopen RTLD_NOLOAD, which only adds a reference), takes the client core singleton from
// VRClientCoreFactory("IVRClientCore_003") and asks it for the C function tables of
// IVRCompositor_027 (GetFrameTimings) and IVRSystem_022 (GetTimeSinceLastVsync), which use the
// connection xrCreateInstance made.
//
// A background thread polls every 100 ms and fills the grid the lock aims at, the same model
// vrtiming.py published to a file: the newest vsync (CLOCK_MONOTONIC = steady_clock), the refresh
// period, and the compositor's update window (start p5/p50, end p50/p95, ns after the vsync, over
// the newest 120 frames). Without vrclient.so in the process (another runtime, the mock without
// its stub), or when the core gives no compositor table, the reader says why once and stops;
// the lock then uses its fallback. Stop() runs before the session or instance is destroyed.
namespace PortVr::steamvr_timing {

struct Grid {
    int64_t vsync_ns = 0;    // newest vsync, steady_clock ns
    int64_t period_ns = 0;   // refresh period
    int32_t update_start_p5 = 0, update_start_p50 = 0, update_end_p50 = 0, update_end_p95 = 0;
    int64_t written_ns = 0;  // when this grid was made, steady_clock ns
    uint32_t frames = 0;     // compositor frames the window percentiles came from
    uint32_t count = 0;      // grids made so far
};

enum class State { Off, Attaching, Running, Unavailable };

using LogFn = std::function<void(const std::string&)>;

// Starts the reader thread once per session (no-op while one runs). Logs through `log`.
void Start(LogFn log);
// Stops and joins the thread; safe to call any time, from any thread but the reader's.
void Stop();
// The newest grid, false before the first.
bool Latest(Grid& out);
State CurrentState();
// One line: what the reader is doing or why it is unavailable.
std::string Describe();
// Whether this process has SteamVR's client (vrclient.so) loaded: the OpenXR runtime is SteamVR.
bool SteamVrClientLoaded();
// Whether a shared object whose file name contains `name` is loaded in this process.
bool LibraryLoaded(const char* name);

// The end-phase lock's learned offset (end after xrWaitFrame's return, us) per refresh rate,
// kept in the user folder (vr_end_phase.txt) for runs without a grid. Load reads the file
// once; Note records the offset the lock used (saved from the reader thread at most every 30 s
// and at Stop).
long LearnedOffsetUs(int hz);
void NoteLockedOffset(int hz, long offset_us);

} // namespace PortVr::steamvr_timing
