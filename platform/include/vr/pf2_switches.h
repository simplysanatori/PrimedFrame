// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <string>

// Steam Frame headset A/B switches (PrimedFrame 2.0), read once from the environment. All are off
// when unset, and the frame loop then behaves exactly as without them.
//
//   PF2_STEREO_PACKED=1   both eyes side by side in ONE swapchain per eye set (one acquire, one
//                         release; projection views pick their halves by imageRect), at most 3456
//                         wide (SteamVR downsamples wider images): eyes wider than 1728 are scaled
//                         down to fit. SteamVR on the Frame takes each eye's latest released image
//                         when it reaches that eye, so per-eye swapchains can pair eyes of
//                         different sets (KartFrame's double image, fixed only by packing).
//   PF2_RELEASE_FENCE=1   an eye set's images are released only after the GPU finished the work
//                         that wrote them (an empty submission signals a timeline semaphore after
//                         Aurora's, under Dawn's queue lock; the release waits for it). SteamVR
//                         shows a released image at once, finished or not.
//   PF2_AHEAD_PERIODS=<n> render-first packets locate their poses n display periods past the last
//                         compositor frame (default 2; pipelined packets n + 1, default 3), and the
//                         frame showing a fresh packet is ended with exactly the display time those
//                         poses were located for.
//   PF2_LATE_LOCATE=<ms>  in immersive play each packet locates (and publishes) its poses ms later
//                         (1..8; 0/unset: at once) for the same display slot, so they are that much
//                         fresher, taken from the idle time in xrWaitFrame after the eyes are done.
//                         Too much moves the eyes past their slot ([pf2] pose lead <=-1). The
//                         [pf2] line's "locate after wait" shows where the locate landed.
//   PF2_END_PHASE=after[:us] | before[:us]
//                         each eye set is released and its frame ended at a phase of SteamVR's
//                         compositor refresh instead of right after xrWaitFrame returns (KartFrame
//                         lane X): after = vsync + compositor update end p95 + us (default 300),
//                         before = vsync + update start p50 - us (default 1500). The grid comes
//                         from dev/tools/vrtiming.py (PF2_END_PHASE set in its environment), a
//                         64-byte record at PF2_PHASE_FILE (default
//                         $XDG_RUNTIME_DIR/primedframe2-phase.bin); without a fresh grid the end
//                         waits PF2_END_DELAY_US, or 4000 us. Log lines start "end phase:".
//   PF2_END_DELAY_US=<n>  release and end each eye set n us (1..12000) after xrWaitFrame returned
//                         (after the release fence; the layer keeps its display time and poses),
//                         bounded by the next refresh. Alone, or as PF2_END_PHASE's fallback.
//   PF2_ALPHA_DISPLAY=1   the game's interpolation between simulation steps advances by the
//                         packets' display times instead of the wall clock at draw pickup, so each
//                         layer shows the game state for the moment it is seen.
//   PF2_SIM_LOCK=1        in the headset the game takes exactly one simulation step per new packet,
//                         of the display period (as 1.0 locked Dolphin to the display): no
//                         interpolation, no 0- or 2-step layers from wall-clock jitter. A packet
//                         whose display time moved k periods (a missed refresh) takes k steps
//                         (at most 0.25 s), so the game keeps real time; without a new packet
//                         (a stall) the wall clock steps it as before.
//   PF2_ALPHA_TRACE=<f>   tests only: one line per headset draw (serial, display time, steps, alpha).
//   PF2_LABEL=<text>      shown for 3 s in the headset when the session starts (the toast panel).
//   PF2_FRAME_STAMP=1     tests only (xrmock MOCK_XR_STAMP): each eye image carries the display
//                         time its poses were located for in a 64x15 block in its top left corner.
namespace PortVr::pf2 {

struct Switches {
    bool packed = false;
    bool fence = false;
    // -1: unset (the built-in lead).
    int ahead_periods = -1;
    // 0: off.
    int late_locate_ms = 0;
    bool stamp = false;
    std::string label;
    // "" (off), "after" or "before", and its offset in us (-1: the mode's default).
    std::string end_phase;
    long end_phase_us = -1;
    // 0: off.
    long end_delay_us = 0;
    bool alpha_display = false;
    bool sim_lock = false;
    std::string alpha_trace;

    bool AnySet() const noexcept {
        return packed || fence || ahead_periods >= 0 || late_locate_ms > 0 || stamp || !label.empty() ||
               !end_phase.empty() || end_delay_us > 0 || alpha_display || sim_lock ||
               !alpha_trace.empty();
    }

    std::string Describe() const {
        if (!AnySet()) {
            return "PF2 switches: none set";
        }
        std::string text = "PF2 switches:";
        if (packed) text += " PF2_STEREO_PACKED=1";
        if (fence) text += " PF2_RELEASE_FENCE=1";
        if (ahead_periods >= 0) text += " PF2_AHEAD_PERIODS=" + std::to_string(ahead_periods);
        if (late_locate_ms > 0) text += " PF2_LATE_LOCATE=" + std::to_string(late_locate_ms);
        if (!end_phase.empty())
            text += " PF2_END_PHASE=" + end_phase + (end_phase_us >= 0 ? ":" + std::to_string(end_phase_us) : "");
        if (end_delay_us > 0) text += " PF2_END_DELAY_US=" + std::to_string(end_delay_us);
        if (alpha_display) text += " PF2_ALPHA_DISPLAY=1";
        if (sim_lock) text += " PF2_SIM_LOCK=1";
        if (!alpha_trace.empty()) text += " PF2_ALPHA_TRACE=" + alpha_trace;
        if (stamp) text += " PF2_FRAME_STAMP=1";
        if (!label.empty()) text += " PF2_LABEL=\"" + label + "\"";
        return text;
    }
};

inline const Switches& Get() {
    static const Switches switches = [] {
        Switches s;
        const auto flag = [](const char* name) {
            const char* value = std::getenv(name);
            return value != nullptr && value[0] == '1';
        };
        s.packed = flag("PF2_STEREO_PACKED");
        s.fence = flag("PF2_RELEASE_FENCE");
        s.stamp = flag("PF2_FRAME_STAMP");
        if (const char* ahead = std::getenv("PF2_AHEAD_PERIODS"); ahead != nullptr && *ahead != '\0') {
            char* end = nullptr;
            const long n = std::strtol(ahead, &end, 10);
            if (end != ahead && n >= 0 && n <= 8) {
                s.ahead_periods = static_cast<int>(n);
            }
        }
        if (const char* late = std::getenv("PF2_LATE_LOCATE"); late != nullptr && *late != '\0') {
            char* end = nullptr;
            const long ms = std::strtol(late, &end, 10);
            if (end != late && ms >= 1 && ms <= 8) {
                s.late_locate_ms = static_cast<int>(ms);
            }
        }
        if (const char* phase = std::getenv("PF2_END_PHASE"); phase != nullptr && *phase != '\0') {
            const std::string v(phase);
            const std::string word = v.substr(0, v.find(':'));
            if (word == "after" || word == "before") {
                s.end_phase = word;
                if (const auto colon = v.find(':'); colon != std::string::npos) {
                    char* end = nullptr;
                    const long us = std::strtol(v.c_str() + colon + 1, &end, 10);
                    if (end != v.c_str() + colon + 1 && us >= 0 && us <= 4000) {
                        s.end_phase_us = us;
                    }
                }
            }
        }
        if (const char* delay = std::getenv("PF2_END_DELAY_US"); delay != nullptr && *delay != '\0') {
            char* end = nullptr;
            const long us = std::strtol(delay, &end, 10);
            if (end != delay && us >= 1 && us <= 12000) {
                s.end_delay_us = us;
            }
        }
        s.alpha_display = flag("PF2_ALPHA_DISPLAY");
        s.sim_lock = flag("PF2_SIM_LOCK");
        if (const char* trace = std::getenv("PF2_ALPHA_TRACE"); trace != nullptr) {
            s.alpha_trace = trace;
        }
        if (const char* label = std::getenv("PF2_LABEL"); label != nullptr) {
            s.label = label;
            if (s.label.size() > 48) {
                s.label.resize(48);
            }
        }
        return s;
    }();
    return switches;
}

// The game's simulation steps (CGraphics::TickSimTime), counted for the [pf2] line: steps and
// 1/600 s ticks since the last line, against the fresh layers shown meanwhile.
inline std::atomic<uint32_t> g_sim_steps{0};
inline std::atomic<uint32_t> g_sim_ticks{0};
inline void NoteSimStep(uint32_t ticks) noexcept {
    g_sim_steps.fetch_add(1, std::memory_order_relaxed);
    g_sim_ticks.fetch_add(ticks, std::memory_order_relaxed);
}

// PF2_ALPHA_DISPLAY and the [pf2] line: headset draws whose interpolation advanced by display
// time or (no new packet, a jump) by the wall clock, and true repeats: draws with no simulation
// step since the previous draw and the same interpolation alpha (the same image of the game).
inline std::atomic<uint32_t> g_draws{0};
inline std::atomic<uint32_t> g_true_repeats{0};
inline std::atomic<uint32_t> g_alpha_display{0};
inline std::atomic<uint32_t> g_alpha_wall{0};
// Headset draws by the simulation steps taken since the previous draw (0, 1, 2 or more), and
// draws PF2_SIM_LOCK stepped (the rest: no new packet, or the lock off).
inline std::atomic<uint32_t> g_draw_steps[3] = {};
inline std::atomic<uint32_t> g_locked_draws{0};

// The widest packed swapchain image (two eyes): SteamVR downsamples images wider than 3458.
inline constexpr unsigned kPackedMaxWidth = 3456;

} // namespace PortVr::pf2
