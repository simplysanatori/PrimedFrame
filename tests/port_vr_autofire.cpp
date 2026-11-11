// SPDX-License-Identifier: GPL-3.0-or-later
//
// Auto Fire: the toggle's state machine, the press-time latch and the refire
// timing in game time at 60, 90 and 120 simulation steps a second.

#include "vr/vr_autofire.h"
#include "vr/vr_settings.h"
#include "vr/vr_settings.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

using namespace PortVr::AutoFire;

namespace {
void Check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        std::exit(1);
    }
}

void TestToggle() {
    Toggle t;
    Check(!t.on, "starts off");
    Check(!t.Update(false, true) && !t.on, "no press, no change");
    Check(t.Update(true, true) && t.on, "press turns on");
    Check(!t.Update(true, true) && t.on, "a held press does not repeat");
    Check(!t.Update(false, true) && t.on, "release keeps it");
    Check(t.Update(true, true) && !t.on, "next press turns off");
    t.Update(false, true);
    // Not allowed (menu, morph ball, setting off): presses do nothing.
    Check(!t.Update(true, false) && !t.on, "ignored while not allowed");
    // Held into an allowed state: not an edge.
    Check(!t.Update(true, true) && !t.on, "a press held through the gate does not count");
    t.Update(false, true);
    t.Update(true, true);
    t.Update(false, true);
    Check(t.on, "on again");
    Check(t.Disable() && !t.on, "the setting turning off clears the mode");
    Check(!t.Disable(), "disabling twice reports no change");
    t.Update(true, true);
    t.Reset();
    Check(!t.on && !t.was_down, "reset: off at every launch");
    Check(std::strcmp(Toggle::Label(true), "Auto Fire On") == 0 &&
              std::strcmp(Toggle::Label(false), "Auto Fire Off") == 0,
          "labels");
}

void TestLatch() {
    Latch l;
    Check(!l.Update(true, true, false) && !l.active, "off at the press: charge as usual");
    Check(!l.Update(false, true, true) && !l.active, "toggled on mid-hold: this hold still charges");
    Check(!l.Update(false, false, true), "release");
    Check(l.Update(true, true, true) && l.active, "on at the press: auto fire");
    Check(l.Update(false, true, false) && l.active, "toggled off mid-hold: this run keeps firing");
    Check(!l.Update(false, false, false) && !l.active, "release stops");
}

// Intervals between shots never undercut the beam's cooldown, and the rate is the
// cooldown rounded up to a whole step, whatever the simulation rate.
void TestRefire() {
    for (const float cooldown : {0.1f, 0.2f, 0.25f, 0.5f}) {
        for (const float hz : {60.0f, 90.0f, 120.0f}) {
            const float dt = 1.0f / hz;
            const int steps_per_shot = static_cast<int>(std::ceil(cooldown / dt - 1e-4f));
            const int shots = ShotsHeld(cooldown, dt, 10.0f);
            const int expect = static_cast<int>(std::lround(10.0f * hz)) / steps_per_shot;
            Check(std::abs(shots - expect) <= 1, "shots in ten seconds match the cooldown rounded to a step");
            const float interval = static_cast<float>(steps_per_shot) * dt;
            Check(interval >= cooldown - 1e-4f, "never faster than the cooldown");
            Check(interval < cooldown + dt + 1e-4f, "never slower than one step over");
            // In game time: the rate in shots per second barely depends on the step.
            Check(std::abs(static_cast<float>(shots) / 10.0f - 1.0f / cooldown) <= 1.0f / cooldown * (dt / cooldown) + 0.15f,
                  "rate within a step of 1 / cooldown");
        }
    }
    Check(ShotsHeld(0.0f, 1.0f / 90.0f, 1.0f) == 90, "a zero cooldown fires every step");
    Check(!RefireDue(0.09f, 0.1f) && RefireDue(0.1f, 0.1f) && RefireDue(0.2f, 0.1f), "refire gate");
    // The same hold at two rates lands within a shot per second of each other.
    Check(std::abs(ShotsHeld(0.25f, 1.0f / 60.0f, 5.0f) - ShotsHeld(0.25f, 1.0f / 120.0f, 5.0f)) <= 2,
          "60 and 120 steps a second agree");
}

void TestSettingDefault() {
    PortVr::PortVrSettings v{};
    Check(!v.auto_fire_button, "the Auto Fire Button setting is off by default");
    v.auto_fire_button = true;
    Check(v.auto_fire_button, "the Auto Fire Button setting can be turned on");
}
} // namespace

int main() {
    TestSettingDefault();
    TestToggle();
    TestLatch();
    TestRefire();
    std::puts("auto fire tests passed");
    return 0;
}
