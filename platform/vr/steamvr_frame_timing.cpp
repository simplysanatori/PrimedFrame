// SPDX-License-Identifier: GPL-3.0-or-later
//
// SteamVR's compositor frame timing, read inside the game (see steamvr_frame_timing.h for why
// it attaches to the OpenXR session's own vrclient instead of calling VR_Init).

#include "vr/steamvr_frame_timing.h"

#include "port_paths.h"

#include <dlfcn.h>
#include <pthread.h>
#include <link.h>
#include <time.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace PortVr::steamvr_timing {
namespace {

// openvr.h's structs, packed to 4 on Linux as there (#pragma pack(push, 4)); 184 bytes, the
// size vrtiming.py passes on the Frame.
#pragma pack(push, 4)
struct HmdMatrix34 { float m[12]; };
struct HmdVector3 { float v[3]; };
struct TrackedDevicePose {
    HmdMatrix34 device_to_absolute;
    HmdVector3 velocity, angular_velocity;
    int32_t tracking_result;
    bool pose_valid, connected;
};
struct FrameTiming {
    uint32_t size, frame_index, presents, mispresented, dropped, reprojection_flags;
    double system_time_s;
    float pre_submit_gpu, post_submit_gpu, total_render_gpu, compositor_render_gpu, compositor_render_cpu,
        compositor_idle_cpu, client_frame_interval, present_call_cpu, wait_for_present_cpu, submit_frame,
        wait_get_poses_called, new_poses_ready, new_frame_ready, compositor_update_start, compositor_update_end,
        compositor_render_start;
    TrackedDevicePose hmd_pose;
    uint32_t vsyncs_ready_for_use, vsyncs_to_first_view;
};
#pragma pack(pop)
static_assert(sizeof(FrameTiming) == 184, "Compositor_FrameTiming layout");

// The C function tables vrclient hands out for "FnTable:<interface>" (openvr_capi.h order).
using GetFrameTimingsFn = uint32_t (*)(FrameTiming*, uint32_t);       // IVRCompositor_027 slot 9
using TimeSinceLastVsyncFn = bool (*)(float*, uint64_t*);             // IVRSystem_022 slot 5
// IVRClientCore (openvr src/ivrclientcore.h): Init, Cleanup, IsInterfaceVersionValid,
// GetGenericInterface, ... No virtual destructor: GetGenericInterface is the fourth slot.
using GetGenericInterfaceFn = void* (*)(void* self, const char* name, int* error);
using ClientCoreFactoryFn = void* (*)(const char* version, int* return_code);

constexpr uint32_t kFrames = 128;
constexpr auto kPoll = std::chrono::milliseconds(100);
constexpr auto kAttachFor = std::chrono::seconds(10);

std::mutex g_mutex;               // the thread, the state, the log function
std::condition_variable g_cv;
std::thread g_thread;
bool g_stop = false;
State g_state = State::Off;
std::string g_why;
LogFn g_log;

std::mutex g_grid_mutex;
Grid g_grid;
bool g_have_grid = false;

std::mutex g_learned_mutex;
std::map<int, long> g_learned;
bool g_learned_loaded = false;
bool g_learned_dirty = false;

void Log(const std::string& line) {
    LogFn log;
    {
        std::lock_guard lock(g_mutex);
        log = g_log;
    }
    if (log) log("SteamVR timing: " + line);
}

int64_t MonoNs() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return static_cast<int64_t>(t.tv_sec) * 1000000000 + t.tv_nsec;
}

struct FindData {
    const char* name;
    bool exact;
    std::string path;
};

int FindCallback(dl_phdr_info* info, size_t, void* data) {
    auto* find = static_cast<FindData*>(data);
    if (info->dlpi_name == nullptr || info->dlpi_name[0] == '\0') return 0;
    const char* base = std::strrchr(info->dlpi_name, '/');
    base = base != nullptr ? base + 1 : info->dlpi_name;
    if (find->exact ? std::strcmp(base, find->name) == 0 : std::strstr(base, find->name) != nullptr) {
        find->path = info->dlpi_name;
        return 1;
    }
    return 0;
}

std::string FindLoaded(const char* name, bool exact) {
    FindData data{name, exact, {}};
    dl_iterate_phdr(FindCallback, &data);
    return data.path;
}

std::string LearnedPath() {
    const std::string& folder = PortPaths::UserFolder();
    return folder.empty() ? std::string() : folder + "vr_end_phase.txt";
}

void LoadLearnedLocked() {
    if (g_learned_loaded) return;
    g_learned_loaded = true;
    const std::string path = LearnedPath();
    if (path.empty()) return;
    std::FILE* file = std::fopen(path.c_str(), "r");
    if (file == nullptr) return;
    char line[128];
    while (std::fgets(line, sizeof line, file) != nullptr) {
        int hz = 0;
        long us = 0;
        if (line[0] != '#' && std::sscanf(line, "%d %ld", &hz, &us) == 2 && hz >= 60 && hz <= 144 && us > 0 &&
            us < 1000000 / hz) {
            g_learned[hz] = us;
        }
    }
    std::fclose(file);
}

void SaveLearned() {
    std::map<int, long> copy;
    {
        std::lock_guard lock(g_learned_mutex);
        if (!g_learned_dirty) return;
        g_learned_dirty = false;
        copy = g_learned;
    }
    const std::string path = LearnedPath();
    if (path.empty()) return;
    const std::string tmp = path + ".tmp";
    std::FILE* file = std::fopen(tmp.c_str(), "w");
    if (file == nullptr) return;
    std::fprintf(file, "# PrimedFrame: the headset's end-phase offset (us after xrWaitFrame returns) per refresh "
                       "rate, learned from SteamVR's compositor timing; used when that timing is unavailable.\n");
    for (const auto& [hz, us] : copy) std::fprintf(file, "%d %ld\n", hz, us);
    const bool ok = std::fclose(file) == 0;
    if (ok) std::rename(tmp.c_str(), path.c_str());
}

// Sleeps up to `d`; true when Stop() asked the thread to leave.
bool WaitStop(std::chrono::milliseconds d) {
    std::unique_lock lock(g_mutex);
    return g_cv.wait_for(lock, d, [] { return g_stop; });
}

void SetState(State state, const std::string& why) {
    std::lock_guard lock(g_mutex);
    g_state = state;
    g_why = why;
}

// One poll's grid, as vrtiming.py's publish_phase made it.
struct GridMaker {
    int64_t vsync = 0;
    std::vector<double> periods;
    uint32_t count = 0;

    bool Make(const FrameTiming* buf, uint32_t n, TimeSinceLastVsyncFn since_vsync, Grid& out) {
        if (n < 8) return false;
        float since_s = 0.f;
        uint64_t counter = 0;
        const int64_t m0 = MonoNs();
        const bool ok = since_vsync(&since_s, &counter);
        const int64_t m1 = MonoNs();
        // The newest two frames may still be filling in.
        const uint32_t first = n > 122 ? n - 122 : 0;
        std::vector<double> steps;
        for (uint32_t i = first + 1; i + 2 < n; ++i) {
            if (buf[i].frame_index == buf[i - 1].frame_index + 1) {
                steps.push_back(buf[i].system_time_s - buf[i - 1].system_time_s);
            }
        }
        if (!ok || steps.empty()) return false;
        std::sort(steps.begin(), steps.end());
        const double p0 = steps[steps.size() / 2] * 1e9;
        const double since = static_cast<double>(since_s) * 1e9;
        if (!(since >= 0.0 && since < 2.0 * p0) || m1 - m0 > 200000) return false;
        const int64_t vs = (m0 + m1) / 2 - static_cast<int64_t>(since);
        if (vsync != 0) {
            const double k = std::round(static_cast<double>(vs - vsync) / p0);
            if (k > 0) {
                periods.push_back(static_cast<double>(vs - vsync) / k);
                if (periods.size() > 20) periods.erase(periods.begin());
            }
        }
        vsync = vs;
        double per = p0;
        if (periods.size() >= 3) {
            std::vector<double> sorted = periods;
            std::sort(sorted.begin(), sorted.end());
            per = sorted[sorted.size() / 2];
        }
        std::vector<float> us, ue;
        for (uint32_t i = first; i + 2 < n; ++i) {
            const FrameTiming& f = buf[i];
            if (f.compositor_update_start == 0.f && f.compositor_update_end == 0.f) continue;
            if (!(f.compositor_update_end >= 0.f && f.compositor_update_end < per / 1e6)) continue;
            us.push_back(f.compositor_update_start);
            ue.push_back(f.compositor_update_end);
        }
        if (us.size() < 30) return false;
        std::sort(us.begin(), us.end());
        std::sort(ue.begin(), ue.end());
        const auto q = [](const std::vector<float>& v, double p) {
            return static_cast<int32_t>(
                v[std::min(v.size() - 1, static_cast<size_t>(p / 100.0 * static_cast<double>(v.size())))] * 1e6);
        };
        out.vsync_ns = vs;
        out.period_ns = static_cast<int64_t>(std::llround(per));
        out.update_start_p5 = q(us, 5);
        out.update_start_p50 = q(us, 50);
        out.update_end_p50 = q(ue, 50);
        out.update_end_p95 = q(ue, 95);
        out.written_ns = MonoNs();
        out.frames = static_cast<uint32_t>(us.size());
        out.count = ++count;
        return true;
    }
};

void Run() {
    const std::string path = FindLoaded("vrclient.so", true);
    if (path.empty()) {
        SetState(State::Unavailable, "SteamVR's vrclient.so is not loaded in this process (the OpenXR runtime is "
                                     "not SteamVR)");
        Log("unavailable: " + Describe());
        return;
    }
    // Only a reference to the copy the OpenXR loader loaded: never a load of our own.
    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_NOLOAD);
    const auto factory =
        handle != nullptr ? reinterpret_cast<ClientCoreFactoryFn>(dlsym(handle, "VRClientCoreFactory")) : nullptr;
    int rc = 0;
    void* core = factory != nullptr ? factory("IVRClientCore_003", &rc) : nullptr;
    if (core == nullptr) {
        SetState(State::Unavailable, path + (handle == nullptr ? ": dlopen(RTLD_NOLOAD) failed"
                                             : factory == nullptr ? ": no VRClientCoreFactory"
                                                                  : ": no IVRClientCore_003 (" + std::to_string(rc) + ")"));
        Log("unavailable: " + Describe());
        if (handle != nullptr) dlclose(handle);
        return;
    }
    const auto get = reinterpret_cast<GetGenericInterfaceFn>((*reinterpret_cast<void***>(core))[3]);
    void** compositor = nullptr;
    void** system = nullptr;
    int compositor_error = 0, system_error = 0;
    const auto deadline = std::chrono::steady_clock::now() + kAttachFor;
    // The session made the client core's connection; give it a moment if it is not up yet.
    for (;;) {
        compositor = static_cast<void**>(get(core, "FnTable:IVRCompositor_027", &compositor_error));
        system = static_cast<void**>(get(core, "FnTable:IVRSystem_022", &system_error));
        if ((compositor != nullptr && system != nullptr) || std::chrono::steady_clock::now() >= deadline) break;
        if (WaitStop(std::chrono::milliseconds(1000))) {
            dlclose(handle);
            return;
        }
    }
    if (compositor == nullptr || system == nullptr || compositor[9] == nullptr || system[5] == nullptr) {
        SetState(State::Unavailable, path + ": the client core gave no " +
                                         (compositor == nullptr ? "IVRCompositor_027 (VRInitError " +
                                                                      std::to_string(compositor_error) + ")"
                                                                : "IVRSystem_022 (VRInitError " +
                                                                      std::to_string(system_error) + ")") +
                                         " without an init of our own");
        Log("unavailable: " + Describe());
        dlclose(handle);
        return;
    }
    const auto timings = reinterpret_cast<GetFrameTimingsFn>(compositor[9]);
    const auto since_vsync = reinterpret_cast<TimeSinceLastVsyncFn>(system[5]);
    SetState(State::Running, "attached to " + path + " (the OpenXR session's own client: no VR_Init), "
                                                     "IVRCompositor_027 + IVRSystem_022, polled every 100 ms");
    Log("running: " + Describe());

    std::vector<FrameTiming> buf(kFrames);
    GridMaker maker;
    auto saved_at = std::chrono::steady_clock::now();
    bool first = true;
    uint32_t empty_polls = 0;
    while (!WaitStop(kPoll)) {
        std::memset(buf.data(), 0, sizeof(FrameTiming) * buf.size());
        buf[0].size = sizeof(FrameTiming);
        const uint32_t n = timings(buf.data(), kFrames);
        Grid grid;
        if (maker.Make(buf.data(), std::min(n, kFrames), since_vsync, grid)) {
            {
                std::lock_guard lock(g_grid_mutex);
                g_grid = grid;
                g_have_grid = true;
            }
            if (first) {
                first = false;
                char line[256];
                std::snprintf(line, sizeof line,
                              "first grid: period %.3f ms (%.1f Hz), compositor update %.2f-%.2f ms after vsync "
                              "(start p50, end p95; %u frames)",
                              grid.period_ns / 1e6, 1e9 / static_cast<double>(grid.period_ns),
                              grid.update_start_p50 / 1e6, grid.update_end_p95 / 1e6, grid.frames);
                Log(line);
            }
            empty_polls = 0;
        } else if (++empty_polls == 50) {
            Log("no grid for 5 s (" + std::to_string(n) + " frames from GetFrameTimings)");
        }
        if (std::chrono::steady_clock::now() - saved_at >= std::chrono::seconds(30)) {
            saved_at = std::chrono::steady_clock::now();
            SaveLearned();
        }
    }
    dlclose(handle);
}

} // namespace

void Start(LogFn log) {
    std::lock_guard lock(g_mutex);
    g_log = std::move(log);
    if (g_thread.joinable()) return;
    {
        std::lock_guard learned(g_learned_mutex);
        LoadLearnedLocked();
    }
    {
        std::lock_guard grid(g_grid_mutex);
        g_have_grid = false;
        g_grid = {};
    }
    g_stop = false;
    g_state = State::Attaching;
    g_why = "looking for SteamVR's client in this process";
    g_thread = std::thread([] {
        pthread_setname_np(pthread_self(), "pf2-vrtiming");
        Run();
    });
    // A process that exits without destroying its session still stops the thread before
    // vrclient's own exit handlers (registered when it was loaded, before this) run.
    static std::once_flag once;
    std::call_once(once, [] { std::atexit([] { Stop(); }); });
}

void Stop() {
    std::thread thread;
    {
        std::lock_guard lock(g_mutex);
        // Nothing running: still keep what the lock learned.
        if (g_thread.joinable()) {
            if (g_thread.get_id() == std::this_thread::get_id()) return;
            g_stop = true;
            thread = std::move(g_thread);
        }
    }
    g_cv.notify_all();
    if (thread.joinable()) thread.join();
    {
        std::lock_guard lock(g_mutex);
        if (g_state == State::Running || g_state == State::Attaching) g_state = State::Off;
    }
    {
        std::lock_guard grid(g_grid_mutex);
        g_have_grid = false;
    }
    SaveLearned();
}

bool Latest(Grid& out) {
    std::lock_guard lock(g_grid_mutex);
    if (!g_have_grid) return false;
    out = g_grid;
    return true;
}

State CurrentState() {
    std::lock_guard lock(g_mutex);
    return g_state;
}

std::string Describe() {
    std::lock_guard lock(g_mutex);
    const char* state = g_state == State::Off         ? "off"
                        : g_state == State::Attaching ? "attaching"
                        : g_state == State::Running   ? "in-process reader"
                                                      : "unavailable";
    return std::string(state) + (g_why.empty() ? "" : ": " + g_why);
}

bool SteamVrClientLoaded() { return !FindLoaded("vrclient.so", true).empty(); }

bool LibraryLoaded(const char* name) { return !FindLoaded(name, false).empty(); }

long LearnedOffsetUs(int hz) {
    std::lock_guard lock(g_learned_mutex);
    LoadLearnedLocked();
    const auto it = g_learned.find(hz);
    return it != g_learned.end() ? it->second : 0;
}

void NoteLockedOffset(int hz, long offset_us) {
    if (hz < 60 || hz > 144 || offset_us <= 0 || offset_us >= 1000000 / hz) return;
    std::lock_guard lock(g_learned_mutex);
    LoadLearnedLocked();
    long& slot = g_learned[hz];
    if (std::labs(slot - offset_us) >= 50) {
        slot = offset_us;
        g_learned_dirty = true;
    }
}

} // namespace PortVr::steamvr_timing
