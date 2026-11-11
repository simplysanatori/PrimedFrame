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
//   PF2_LABEL=<text>      shown for 3 s in the headset when the session starts (the toast panel).
//   PF2_FRAME_STAMP=1     tests only (xrmock MOCK_XR_STAMP): each eye image carries the display
//                         time its poses were located for in a 64x15 block in its top left corner.
namespace PortVr::pf2 {

struct Switches {
    bool packed = false;
    bool fence = false;
    // -1: unset (the built-in lead).
    int ahead_periods = -1;
    bool stamp = false;
    std::string label;

    bool AnySet() const noexcept { return packed || fence || ahead_periods >= 0 || stamp || !label.empty(); }

    std::string Describe() const {
        if (!AnySet()) {
            return "PF2 switches: none set";
        }
        std::string text = "PF2 switches:";
        if (packed) text += " PF2_STEREO_PACKED=1";
        if (fence) text += " PF2_RELEASE_FENCE=1";
        if (ahead_periods >= 0) text += " PF2_AHEAD_PERIODS=" + std::to_string(ahead_periods);
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

// The widest packed swapchain image (two eyes): SteamVR downsamples images wider than 3458.
inline constexpr unsigned kPackedMaxWidth = 3456;

} // namespace PortVr::pf2
