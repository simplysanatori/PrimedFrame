// SPDX-License-Identifier: GPL-3.0-or-later
//
// PrimedGun.exe: the PrimedGun launcher for the native Metroid Prime port.
//
// PrimedGun --frame-start (Steam Frame build, Linux): with a valid disc in
// port_settings.ini, the launcher's own selected disc (primedgun_launcher.ini) or MP_DISC
// the game is started at once, straight into VR, in place of this process; without one
// the launcher window opens.

#include "ui/launcher_window.h"
#include "ui/style.h"

#include <QApplication>
#include <QFileInfo>
#include <QIcon>
#include <QSettings>
#include <QStyleFactory>

#if defined(__linux__)
#include "frame_start.h"
#include "port_paths.h"

#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

namespace {
// Replaces this process with the game; only returns when that is not possible.
void StartGameInPlace() {
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path exe = fs::read_symlink("/proc/self/exe", ec);
  if (ec) {
    return;
  }
  const fs::path folder = exe.parent_path();
  const fs::path game = folder / "metroid_prime_port";
  const std::string& user = PortPaths::UserFolder();
  PrimedGunLauncher::FrameStart start = PrimedGunLauncher::DecideFrameStart(
      fs::path(std::u8string(user.begin(), user.end())) / "port_settings.ini", std::getenv("MP_DISC"));
  if (!start.startGame) {
    // Play keeps the disc in the launcher's own settings (the game saves disc_path only
    // when its own picker chose it), so a disc picked in the launcher counts too.
    const QSettings launcherIni(QString::fromStdString(user) + QStringLiteral("/primedgun_launcher.ini"),
                                QSettings::IniFormat);
    const QString disc = launcherIni.value(QStringLiteral("selected_disc")).toString();
    if (!disc.isEmpty() && QFileInfo(disc).isFile()) {
      start.startGame = true;
      start.disc = disc.toStdString();
    }
  }
  if (!start.startGame || !fs::exists(game, ec)) {
    return;
  }
  // The game keeps the user folder the launcher resolved.
  setenv("MP_USER_PATH", user.c_str(), 1);
  if (chdir(folder.c_str()) != 0) {
    return;
  }
  const std::string gamePath = game.string();
  const std::string disc = start.disc;
  char* args[] = {const_cast<char*>(gamePath.c_str()), const_cast<char*>(disc.c_str()), nullptr};
  execv(gamePath.c_str(), args);
}
} // namespace
#endif

int main(int argc, char** argv) {
#if defined(__linux__)
  if (argc >= 2 && std::strcmp(argv[1], "--frame-start") == 0) {
    StartGameInPlace();
  }
#endif
  QApplication app(argc, argv);
  QApplication::setApplicationName(QStringLiteral("PrimedGun"));
  QApplication::setApplicationVersion(QStringLiteral(PRIMEDGUN_LAUNCHER_VERSION));
  QApplication::setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
  QApplication::setPalette(PrimedGunLauncher::Style::DarkPalette(QApplication::palette()));
  QApplication::setWindowIcon(QIcon(QStringLiteral(":/PrimedGun.png")));

  PrimedGunLauncher::LauncherWindow window;
  window.show();
  return QApplication::exec();
}
