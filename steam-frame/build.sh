#!/bin/bash
# Builds PrimedFrame's two programs (the game metroid_prime_port and the Qt launcher PrimedGun) for the Steam
# Frame the way a release needs them: linked against a glibc 2.39 tree so they start on every SteamOS
# (Stable has glibc 2.39, Beta and Preview 2.43), SDL 3 built from source and linked in, RUNPATH $ORIGIN/lib.
#   steam-frame/build.sh [ninja args]
# Environment:
#   PF_BUILD_DIR            build folder (default <repo>/build-portable)
#   PF_GLIBC239_SYSROOT     the glibc 2.39 tree (default ~/Games/frame-portable/sysroot-glibc2.39)
#   PF_DAWN_PACKAGE         the patched Dawn package (tools/build-linux-dawn.sh makes it; required, a .tar.gz)
#   PF_BUILD_TYPE           CMake build type (default RelWithDebInfo; release.sh strips the debug info into a symbols tarball)
#   PF_BUILD_LOCK           lock file to hold while building (default ~/Games/frame-portable/build.lock; empty = none)
#   JOBS                    ninja jobs (default 4)
#   CCACHE_DIR              default ~/.cache/ccache-pf2
#   PF_NATIVE_GLIBC=1       the build machine's own glibc is 2.39 or older (the CI runner, Ubuntu 24.04): no glibc
#                           tree and no tools/portable-bin wrappers, the compilers are $CC and $CXX (default gcc, g++)
#                           and CMake finds Qt through CMAKE_PREFIX_PATH; it refuses a machine with a newer glibc
#   PF_EXTRA_CMAKE_FLAGS    more configure flags (split on spaces)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/.." && pwd)
B=${PF_BUILD_DIR:-$SRC/build-portable}
export CCACHE_DIR=${CCACHE_DIR:-$HOME/.cache/ccache-pf2}
PB=$HERE/tools/portable-bin
if [ "${PF_NATIVE_GLIBC:-0}" = 1 ]; then
  g=$(getconf GNU_LIBC_VERSION | awk '{ print $2 }')
  [ "$(printf '%s\n2.39\n' "$g" | sort -V | tail -n 1)" = 2.39 ] || { echo "build: PF_NATIVE_GLIBC=1, but this machine has glibc $g (newer than 2.39, the oldest SteamOS)"; exit 1; }
  unset PF_GLIBC239_SYSROOT
  CC_=$(command -v "${CC:-gcc}"); CXX_=$(command -v "${CXX:-g++}")
  # Qt is not in a system folder here, so CMake would add its lib folder to the programs' RUNPATH: keep only
  # the $ORIGIN/lib the linker flags below set (check-bundle.sh insists on exactly that)
  NATIVE=(-DCMAKE_SKIP_BUILD_RPATH=ON)
else
  export PF_GLIBC239_SYSROOT=${PF_GLIBC239_SYSROOT:-$HOME/Games/frame-portable/sysroot-glibc2.39}
  [ -d "$PF_GLIBC239_SYSROOT/usr/lib" ] || { echo "build: no glibc 2.39 tree at $PF_GLIBC239_SYSROOT (PF_GLIBC239_SYSROOT)"; exit 1; }
  CC_=$PB/gcc; CXX_=$PB/g++
  export PATH=$PB:$PATH
  NATIVE=()
fi
DAWN=${PF_DAWN_PACKAGE:-}
[ -f "$DAWN" ] || { echo "build: PF_DAWN_PACKAGE must name the patched Dawn package (dawn-linux-aarch64-*.tar.gz from tools/build-linux-dawn.sh)"; exit 1; }
read -ra EXTRA <<< "${PF_EXTRA_CMAKE_FLAGS:-}"
# env -u LD_LIBRARY_PATH: Steam's runtime libraries must not reach the toolchain.
run() { if [ -n "${PF_BUILD_LOCK-x}" ] && [ "${PF_BUILD_LOCK-x}" != "" ]; then env -u LD_LIBRARY_PATH flock "${PF_BUILD_LOCK:-$HOME/Games/frame-portable/build.lock}" "$@"; else env -u LD_LIBRARY_PATH "$@"; fi; }
if [ ! -f "$B/build.ninja" ] || [ "$PF_RECONFIGURE" = 1 ]; then
  run cmake -S "$SRC" -B "$B" -G Ninja -DCMAKE_BUILD_TYPE="${PF_BUILD_TYPE:-RelWithDebInfo}" \
    -DCMAKE_C_COMPILER="$CC_" -DCMAKE_CXX_COMPILER="$CXX_" \
    "-DCMAKE_EXE_LINKER_FLAGS=-Wl,-rpath,'\$ORIGIN/lib' -Wl,--enable-new-dtags" \
    -DAURORA_DAWN_PROVIDER=package -DAURORA_DAWN_LINKAGE=static \
    -DAURORA_DAWN_PACKAGE_URL="$DAWN" -DAURORA_DAWN_PACKAGE_SHA256="$(sha256sum "$DAWN" | cut -d' ' -f1)" \
    -DAURORA_SDL3_PROVIDER=vendor -DAURORA_SDL3_LINKAGE=static \
    -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_KMSDRM=OFF -DSDL_IBUS=OFF -DSDL_HIDAPI_LIBUSB=OFF -DSDL_JACK=OFF -DSDL_SNDIO=OFF \
    -DSDL_LIBURING=OFF -DSDL_FRIBIDI=OFF -DSDL_LIBTHAI=OFF -DSDL_X11_XTEST=OFF -DSDL_X11_XSCRNSAVER=OFF \
    -DMP_ENABLE_OPENXR=ON -DMP_BUILD_LAUNCHER=ON -DMP_BUNDLED_DEPS=ON -DMP_EMBED_RESOURCES=ON \
    -DMP_USE_CCACHE=ON -DMP_USE_MOLD=ON -DMP_BUILD_TESTS=OFF "${NATIVE[@]}" "${EXTRA[@]}"
fi
run ninja -C "$B" -j"${JOBS:-4}" metroid_prime_port PrimedGun "$@"
