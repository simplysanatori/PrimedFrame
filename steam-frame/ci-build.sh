#!/bin/bash
# Builds the PrimedFrame release on a GitHub Actions Arm64 runner (ubuntu-24.04-arm), the way
# .github/workflows/steam-frame-release.yml does it, one stage at a time:
#   steam-frame/ci-build.sh deps        apt packages: GCC 14, clang/lld for Dawn, the headers, the run-time libraries
#                                       the checks load (the release's allowlisted host libraries), patchelf
#   steam-frame/ci-build.sh qt          the official Qt (aqtinstall) into $PF_CI_WORK/qt (skipped when it is there)
#   steam-frame/ci-build.sh licenses    Qt's and ICU's licence texts (the Qt install carries none) into
#                                       $PF_CI_WORK/licenses-extra, from the sources of the exact versions bundled
#   steam-frame/ci-build.sh dawn        the patched Dawn package (tools/build-linux-dawn.sh, cached in $PF_CI_WORK/dawn)
#   steam-frame/ci-build.sh build       steam-frame/build.sh on this machine's own glibc 2.39 (PF_NATIVE_GLIBC=1)
#   steam-frame/ci-build.sh release     steam-frame/release.sh --no-build: strip, bundle, every check, pack
# Ubuntu 24.04 has glibc 2.39, the oldest SteamOS's, so the programs are built natively, without the glibc tree the
# Frame's own build links against; GCC 14's libstdc++ symbols (up to GLIBCXX_3.4.33) all exist on SteamOS.
# Environment: PF_CI_WORK (default <repo>/build-ci), PF_BUILD_DIR (default <repo>/build-portable), JOBS (default nproc).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/.." && pwd)
W=${PF_CI_WORK:-$SRC/build-ci}
QT_VERSION=6.10.3
QT=$W/qt/$QT_VERSION/gcc_arm64
ICU_TAG=release-73-2          # the ICU the Qt install bundles (lib/libicuuc.so.73.2); "qt" checks it
OUT=$W/out
export JOBS=${JOBS:-$(nproc)}
mkdir -p "$W"

case "${1:?usage: ci-build.sh deps|qt|licenses|dawn|build|release}" in
deps)
  sudo apt-get update -q
  sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -q --no-install-recommends \
    gcc-14 g++-14 clang lld cmake ninja-build ccache mold git curl file binutils patchelf python3 python3-venv \
    pkg-config zip unzip \
    libasound2-dev libpulse-dev libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxi-dev libxfixes-dev \
    libxss-dev libxtst-dev libxinerama-dev libx11-xcb-dev libwayland-dev wayland-protocols libxkbcommon-dev \
    libegl-dev libgl-dev libgles-dev libdrm-dev libgbm-dev mesa-common-dev libvulkan-dev \
    libfreetype-dev libpng-dev zlib1g-dev libbz2-dev libbrotli-dev libzstd-dev libssl-dev \
    libsystemd-dev libudev-dev libusb-1.0-0-dev libdbus-1-dev libglib2.0-dev libfontconfig-dev \
    libxcb1-dev libxcb-cursor0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 libxcb-randr0 libxcb-render0 \
    libxcb-render-util0 libxcb-shape0 libxcb-shm0 libxcb-sync1 libxcb-xfixes0 libxcb-xkb1 libxkbcommon-x11-0 \
    libsm6 libice6 libegl1 libgl1 libglx0 libopengl0 libwayland-client0 libwayland-cursor0 libwayland-egl1 \
    libvulkan1 libpulse0 libdbus-1-3 libfontconfig1 libfreetype6 libharfbuzz0b
  gcc-14 --version | head -n 1; clang --version | head -n 1; cmake --version | head -n 1
  ;;
qt)
  if [ -f "$QT/lib/libQt6Core.so.6" ]; then echo "Qt $QT_VERSION already in $QT"; else
    python3 -m venv "$W/aqt-venv"
    "$W/aqt-venv/bin/pip" install -q aqtinstall==3.3.0
    "$W/aqt-venv/bin/aqt" install-qt -O "$W/qt" linux_arm64 desktop "$QT_VERSION" linux_gcc_arm64 --archives qtbase qtwayland icu
  fi
  [ -f "$QT/lib/libQt6Core.so.6" ] || { echo "ci-build: no Qt at $QT"; exit 1; }
  [ -e "$QT/lib/libicuuc.so.73.2" ] || { echo "ci-build: the Qt install's ICU is not 73.2: update ICU_TAG"; ls "$QT"/lib/libicuuc.so*; exit 1; }
  ;;
licenses)
  L=$W/licenses-extra
  rm -rf "$L" "$W/lic-src"; mkdir -p "$L" "$W/lic-src"
  for repo in qtbase qtwayland; do
    git clone -q --depth 1 --branch "v$QT_VERSION" --filter=blob:none --sparse "https://github.com/qt/$repo.git" "$W/lic-src/$repo"
    git -C "$W/lic-src/$repo" sparse-checkout set LICENSES
    ls "$W/lic-src/$repo"/LICENSES/*.txt > /dev/null
  done
  mkdir -p "$L/qt6-base" "$L/qt6-wayland" "$L/icu"
  cp "$W"/lic-src/qtbase/LICENSES/* "$L/qt6-base/"
  cp "$W"/lic-src/qtwayland/LICENSES/* "$L/qt6-wayland/"
  curl -fsSL --retry 3 -o "$L/icu/LICENSE" "https://raw.githubusercontent.com/unicode-org/icu/$ICU_TAG/icu4c/LICENSE"
  [ -f "$L/qt6-base/LGPL-3.0-only.txt" ] || { echo "ci-build: qtbase v$QT_VERSION has no LICENSES/LGPL-3.0-only.txt"; exit 1; }
  echo "licence texts: $(find "$L" -type f | wc -l) files in $L"
  ;;
dawn)
  "$SRC/tools/build-linux-dawn.sh" "$W/dawn"
  ;;
build)
  m=$W/dawn/dawn-package.txt
  pkg=$W/dawn/$(sed -n 's/^package_file=//p' "$m")
  [ -f "$pkg" ] || { echo "ci-build: no Dawn package ($m): run the dawn stage"; exit 1; }
  PF_NATIVE_GLIBC=1 CC=gcc-14 CXX=g++-14 PF_BUILD_LOCK= PF_DAWN_PACKAGE="$pkg" CMAKE_PREFIX_PATH="$QT" \
    "$HERE/build.sh"
  ;;
release)
  PF_NATIVE_GLIBC=1 PF_BUILD_LOCK= PF_RELEASE_OUT="$OUT" \
    PF_LIBDIR="$QT/lib:/usr/lib/aarch64-linux-gnu" PF_QT_PLUGINS="$QT/plugins" PF_EXTRA_LICENSES="$W/licenses-extra" \
    "$HERE/release.sh" --no-build
  ;;
*) echo "ci-build: unknown stage $1"; exit 2 ;;
esac
