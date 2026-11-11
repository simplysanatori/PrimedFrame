// SPDX-License-Identifier: GPL-3.0-or-later
//
// The Steam Frame build's start path (PrimedGun --frame-start): the launcher
// binary decides whether the game can start straight into VR or the launcher
// window has to open first, so the "PrimedFrame" Steam entry needs no second
// program.

#pragma once

#include <filesystem>
#include <string>

namespace PrimedGunLauncher {

struct FrameStart {
  bool startGame = false; // a valid disc is configured: exec the game, no window
  std::string disc;       // that disc (UTF-8), empty when startGame is false
};

// A disc is valid when MP_DISC (`envDisc`, may be null) or port_settings.ini's
// disc_path names a file that exists. Nothing is written.
FrameStart DecideFrameStart(const std::filesystem::path& settingsFile, const char* envDisc);

// PrimedFrame 1.0 (Dolphin) kept its data in <XDG data home>/PrimedFrame/user
// (GC/MemoryCardA.USA.raw, Config/PrimedGun.ini). Returns that folder when it
// holds a card or a settings file, else an empty path.
std::filesystem::path FindLegacyFrameFolder(const char* xdgDataHome, const char* home);

} // namespace PrimedGunLauncher
