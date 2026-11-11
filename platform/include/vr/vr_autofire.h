// SPDX-License-Identifier: GPL-3.0-or-later
//
// Auto Fire (Steam Frame's shoulder button): a toggle that turns a held fire
// trigger into repeated beam shots instead of a charge. No dependency on the
// game, so the tests drive it directly.
//
// The shots are the game's own: CPlayerGun::UpdateWeaponFire already synthesizes
// a press for a held trigger once the beam's cooldown (CTweakPlayerGun
// x0_coolDown, in game seconds) has run down, and the Auto Fire latch only
// routes that press to the quick-shot path (no charge) for the beam. The
// cooldown is summed from the simulation step, so it follows 60, 90 or 120
// steps a second alike.

#pragma once

#include <cmath>

namespace PortVr::AutoFire {

inline constexpr double kLabelSeconds = 1.5;

// The toggle's state machine, run once a frame. `down` is the shoulder, `allowed`
// whether the button may act now (setting on, first-person gameplay, no menu).
// Never saved: a new session starts off.
struct Toggle {
    bool on = false;
    bool was_down = false;

    // True when `on` changed this call (the label's cue). A press made while
    // not allowed does not count when it becomes allowed still held.
    bool Update(bool down, bool allowed) noexcept {
        const bool edge = down && !was_down;
        was_down = down;
        if (allowed && edge) {
            on = !on;
            return true;
        }
        return false;
    }
    // The setting turned the button off: the mode goes with it.
    bool Disable() noexcept {
        const bool changed = on;
        on = false;
        return changed;
    }
    void Reset() noexcept { *this = {}; }
    static const char* Label(bool on) noexcept { return on ? "Auto Fire On" : "Auto Fire Off"; }
};

// The hold's mode is decided at the trigger's press and kept until its release,
// so toggling in the middle of a hold never changes a charge already building
// (or an auto-fire run already going).
struct Latch {
    bool active = false;
    // `pressed`: the trigger went down this step; `held`: it is down.
    bool Update(bool pressed, bool held, bool mode_on) noexcept {
        if (pressed) {
            active = mode_on;
        }
        if (!held) {
            active = false;
        }
        return active;
    }
};

// The game's refire gate, in game time: a shot is due once the time since the
// last one has reached the beam's cooldown.
inline bool RefireDue(float elapsed, float cooldown) noexcept { return elapsed >= cooldown; }

// Shots a held trigger lands in `seconds` of game time at a simulation step of
// `dt`, as UpdateWeaponFire counts them (the first shot at the press). The
// elapsed time restarts at each shot and is advanced after the check.
inline int ShotsHeld(float cooldown, float dt, float seconds) noexcept {
    float elapsed = cooldown; // ready at the press
    int shots = 0;
    const int steps = static_cast<int>(std::lround(seconds / dt));
    for (int i = 0; i < steps; ++i) {
        if (RefireDue(elapsed, cooldown)) {
            ++shots;
            elapsed = 0.0f;
        }
        elapsed += dt;
    }
    return shots;
}

} // namespace PortVr::AutoFire
