// SPDX-License-Identifier: GPL-3.0-or-later

#include "frame_start.h"

#include "port_settings_file.h"
#include "primedgun_import.h"

#include <system_error>

namespace fs = std::filesystem;

namespace PrimedGunLauncher {
namespace {

bool IsFile(const std::string& utf8) {
  if (utf8.empty()) {
    return false;
  }
  std::error_code ec;
  return fs::is_regular_file(fs::path(std::u8string(utf8.begin(), utf8.end())), ec);
}

} // namespace

FrameStart DecideFrameStart(const fs::path& settingsFile, const char* envDisc) {
  FrameStart result;
  if (envDisc != nullptr && IsFile(envDisc)) {
    result.startGame = true;
    result.disc = envDisc;
    return result;
  }
  PortSettingsFile file;
  if (file.Load(settingsFile)) {
    if (const auto disc = file.Get("disc_path"); disc && IsFile(*disc)) {
      result.startGame = true;
      result.disc = *disc;
    }
  }
  return result;
}

fs::path FindLegacyFrameFolder(const char* xdgDataHome, const char* home) {
  fs::path base;
  if (xdgDataHome != nullptr && *xdgDataHome != '\0') {
    base = fs::path(xdgDataHome);
  } else if (home != nullptr && *home != '\0') {
    base = fs::path(home) / ".local" / "share";
  } else {
    return {};
  }
  const fs::path folder = base / "PrimedFrame" / "user";
  std::error_code ec;
  if (FindOldCardUnder(folder, "USA").Found() || fs::is_regular_file(folder / "Config" / "PrimedGun.ini", ec)) {
    return folder;
  }
  return {};
}

} // namespace PrimedGunLauncher
