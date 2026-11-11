// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <string>

// Steam Frame headset switches (PrimedFrame 2.0), read once from the environment.
//
// On Linux (the Steam Frame build) the setup the headset tests settled on ("H5-5": packed stereo,
// release fence, ahead 1, the simulation locked to the display, the end at the compositor's phase)
// is the default with no variable set; each switch below overrides it, and 0 (or "off") turns it
// off. Elsewhere (Windows, the Quest) all are off when unset, as before.
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
//                         compositor frame (Linux default 1, else 2; "off": the built-in 2;
//                         pipelined packets n + 1), and the frame showing a fresh packet is ended
//                         with exactly the display time those poses were located for.
//   PF2_LATE_LOCATE=<ms>  in immersive play each packet locates (and publishes) its poses ms later
//                         (1..8; 0/unset: at once) for the same display slot, so they are that much
//                         fresher, taken from the idle time in xrWaitFrame after the eyes are done.
//                         Too much moves the eyes past their slot ([pf2] pose lead <=-1). The
//                         [pf2] line's "locate after wait" shows where the locate landed.
//   PF2_END_PHASE=after[:us] | before[:us] | off
//                         each eye set is released and its frame ended at a phase of SteamVR's
//                         compositor refresh instead of right after xrWaitFrame returns (KartFrame
//                         lane X): after = vsync + compositor update end p95 + us (default 300),
//                         before = vsync + update start p50 - us (default 1500). Linux default:
//                         after, when the runtime is SteamVR. The grid comes from the game's own
//                         SteamVR timing reader (steamvr_frame_timing.h), or from a 64-byte record
//                         at PF2_PHASE_FILE when that is set (dev/tools/vrtiming.py). Without a
//                         grid the end waits the offset the lock last used at this refresh rate
//                         (saved in the user folder), or half the display period. Log lines
//                         start "end phase:".
//   PF2_END_DELAY_US=<n>  release and end each eye set n us (1..12000) after xrWaitFrame returned
//                         (after the release fence; the layer keeps its display time and poses),
//                         bounded by the next refresh. Alone, or as PF2_END_PHASE's fallback.
//   PF2_TIMING_READER=0   no in-process SteamVR timing reader (the end phase then uses its fallback).
//   PF2_ALPHA_DISPLAY=1   the game's interpolation between simulation steps advances by the
//                         packets' display times instead of the wall clock at draw pickup, so each
//                         layer shows the game state for the moment it is seen.
//   PF2_SIM_LOCK=1        in the headset the game takes exactly one simulation step per new packet,
//                         of the display period (as 1.0 locked Dolphin to the display): no
//                         interpolation, no 0- or 2-step layers from wall-clock jitter. A packet
//                         whose display time moved k periods (a missed refresh) takes k steps
//                         (at most 0.25 s, and no more than the tick budget lets run), so the game
//                         keeps real time; without a new packet (a stall) the wall clock steps it
//                         at the display rate. Linux default: on.
//   PF2_SIM_LOCK_MAX_HZ=<hz>  the highest refresh rate the lock steps once per packet (default 96;
//                         the game's per-tick logic is checked up to 90 steps a second). Above it
//                         the lock takes one step of two periods per two packets (60 steps a second
//                         at 120 Hz); 120 (or more) allows one step per packet at 120 Hz (a test).
//   PF2_IDLE_AFTER_SEAL=0 in the headset the resource loader's per-frame idle runs before the frame
//                         is sealed (EndScene), as on a flat screen; default: after it, so a slow
//                         load step delays the next frame, not the one already drawn.
//   PF2_OVERLAP=<us> | 0  render-first pacing in immersive play: when the next packet is due before
//                         this frame's end (the recent CPU time from locate to Aurora's submission,
//                         plus the eye set's own GPU time, plus <us> of margin (default 1500), no
//                         longer fit between this frame's end and the next one's), it is located,
//                         armed and published while this frame still waits for its GPU work and
//                         its end phase, so the game's next step and draw run while the GPU
//                         finishes this one. Its poses are for the same display time as without
//                         it (PF2_AHEAD_PERIODS after the last xrWaitFrame), only located earlier,
//                         by as much as needed. Linux default: on; 0/off: the next packet only
//                         after the end (a CPU + GPU budget of one period). [pf2]: "early locate".
//   PF2_DEV_GPU_LOAD_MB=<MB>  tests only: each eye set also fills that many MB on the GPU.
//   PF2_HZ=<hz>           the refresh rate the game asks the headset for (72..144), over the
//                         vr_display_refresh_rate setting; that setting's 0 (runtime default) asks
//                         SteamVR for 90 on Linux.
//   PF2_ALPHA_TRACE=<f>   tests only: one line per headset draw (serial, display time, steps, alpha).
//   PF2_LABEL=<text>      shown for 3 s in the headset when the session starts (the toast panel).
//   PF2_FRAME_STAMP=1     tests only (xrmock MOCK_XR_STAMP): each eye image carries the display
//                         time its poses were located for in a 64x15 block in its top left corner.
namespace PortVr::pf2 {

// The Steam Frame build: the H5-5 setup is the default (see above).
#if defined(__linux__) && !defined(__ANDROID__)
inline constexpr bool kFrameDefaults = true;
#else
inline constexpr bool kFrameDefaults = false;
#endif

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
    // The end phase is the Linux default, not PF2_END_PHASE: it applies on SteamVR only.
    bool end_phase_default = false;
    // 0: off.
    long end_delay_us = 0;
    bool timing_reader = true;
    bool alpha_display = false;
    bool sim_lock = false;
    // PF2_SIM_LOCK_MAX_HZ: one step per packet up to this rate, one per two packets above.
    int sim_lock_max_hz = 96;
    bool idle_after_seal = false;
    // PF2_OVERLAP: the margin in us (0: off).
    long overlap_margin_us = 0;
    // PF2_HZ: 0 unset.
    int hz = 0;
    std::string alpha_trace;
    // The PF2_* variables that were set, as given ("PF2_X=v ...").
    std::string env;

    bool AnySet() const noexcept {
        return packed || fence || ahead_periods >= 0 || late_locate_ms > 0 || stamp || !label.empty() ||
               !end_phase.empty() || end_delay_us > 0 || alpha_display || sim_lock ||
               !alpha_trace.empty();
    }

    std::string Describe() const {
        std::string text = "PF2 switches:";
        text += packed ? " packed" : " per-eye";
        text += fence ? " fence" : " no-fence";
        text += " ahead=" + (ahead_periods >= 0 ? std::to_string(ahead_periods) : std::string("built-in"));
        if (late_locate_ms > 0) text += " late-locate=" + std::to_string(late_locate_ms) + "ms";
        text += sim_lock ? " sim-lock(1 step/packet to " + std::to_string(sim_lock_max_hz) + " Hz)" : " sim-wallclock";
        if (alpha_display) text += " alpha-display";
        if (!end_phase.empty()) {
            text += " end-phase=" + end_phase + (end_phase_us >= 0 ? ":" + std::to_string(end_phase_us) : "") +
                    (end_phase_default ? "(SteamVR)" : "");
        } else {
            text += " end-phase=off";
        }
        if (end_delay_us > 0) text += " end-delay=" + std::to_string(end_delay_us) + "us";
        if (!timing_reader) text += " no-timing-reader";
        text += idle_after_seal ? " idle-after-seal" : " idle-before-seal";
        text += overlap_margin_us > 0 ? " overlap(margin " + std::to_string(overlap_margin_us) + " us)" : " no-overlap";
        if (hz > 0) text += " hz=" + std::to_string(hz);
        if (stamp) text += " stamp";
        if (!alpha_trace.empty()) text += " alpha-trace";
        text += kFrameDefaults ? " (Frame defaults" : " (defaults off";
        text += env.empty() ? "; no PF2_* set)" : "; set: " + env + ")";
        return text;
    }
};

inline const Switches& Get() {
    static const Switches switches = [] {
        Switches s;
        const auto note = [&s](const char* name, const char* value) {
            if (!s.env.empty()) s.env += ' ';
            s.env += std::string(name) + "=" + value;
        };
        const auto get = [&note](const char* name) -> const char* {
            const char* value = std::getenv(name);
            if (value == nullptr || *value == '\0') return nullptr;
            note(name, value);
            return value;
        };
        // Unset: the default; 1 on; 0/off (anything else) off.
        const auto flag = [&](const char* name, bool fallback) {
            const char* value = get(name);
            return value == nullptr ? fallback : value[0] == '1' || std::string(value) == "on";
        };
        s.packed = flag("PF2_STEREO_PACKED", kFrameDefaults);
        s.fence = flag("PF2_RELEASE_FENCE", kFrameDefaults);
        s.stamp = flag("PF2_FRAME_STAMP", false);
        s.ahead_periods = kFrameDefaults ? 1 : -1;
        if (const char* ahead = get("PF2_AHEAD_PERIODS")) {
            char* end = nullptr;
            const long n = std::strtol(ahead, &end, 10);
            if (end != ahead && n >= 0 && n <= 8) {
                s.ahead_periods = static_cast<int>(n);
            } else if (std::string(ahead) == "off") {
                s.ahead_periods = -1;
            }
        }
        if (const char* late = get("PF2_LATE_LOCATE")) {
            char* end = nullptr;
            const long ms = std::strtol(late, &end, 10);
            if (end != late && ms >= 1 && ms <= 8) {
                s.late_locate_ms = static_cast<int>(ms);
            }
        }
        if (const char* phase = get("PF2_END_PHASE")) {
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
        } else if (kFrameDefaults) {
            s.end_phase = "after";
            s.end_phase_default = true;
        }
        if (const char* delay = get("PF2_END_DELAY_US")) {
            char* end = nullptr;
            const long us = std::strtol(delay, &end, 10);
            if (end != delay && us >= 1 && us <= 12000) {
                s.end_delay_us = us;
            }
        }
        s.timing_reader = flag("PF2_TIMING_READER", true);
        s.alpha_display = flag("PF2_ALPHA_DISPLAY", false);
        s.sim_lock = flag("PF2_SIM_LOCK", kFrameDefaults);
        if (const char* max = get("PF2_SIM_LOCK_MAX_HZ")) {
            const long hz = std::strtol(max, nullptr, 10);
            if (hz >= 30 && hz <= 240) {
                s.sim_lock_max_hz = static_cast<int>(hz);
            }
        }
        s.idle_after_seal = flag("PF2_IDLE_AFTER_SEAL", kFrameDefaults);
        s.overlap_margin_us = kFrameDefaults ? 1500 : 0;
        if (const char* overlap = get("PF2_OVERLAP")) {
            char* end = nullptr;
            const long us = std::strtol(overlap, &end, 10);
            s.overlap_margin_us = std::string(overlap) == "on"                   ? 1500
                                  : end != overlap && us >= 0 && us <= 8000 ? us
                                                                            : 0;
        }
        if (const char* hz = get("PF2_HZ")) {
            const long v = std::strtol(hz, nullptr, 10);
            if (v >= 60 && v <= 144) {
                s.hz = static_cast<int>(v);
            }
        }
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

// PF2_SIM_LOCK's rate guard: the packets per simulation step at a display rate, 1 up to
// PF2_SIM_LOCK_MAX_HZ (default 96), else 2 (each step then covers two periods).
inline int SimLockStride(double display_hz) {
    return display_hz <= static_cast<double>(Get().sim_lock_max_hz) + 0.5 ? 1 : 2;
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
