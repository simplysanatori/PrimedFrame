#!/bin/bash
# Builds the PrimedFrame release for the Steam Frame, checks it, and packs it:
#   primedframe-<version>-linux-arm64.tar.gz           the app folder (install.sh at its top level: Frametanium runs it)
#   primedframe-<version>-linux-arm64-symbols.tar.gz   the debug symbols of the game and the launcher (for crash reports)
#   SHA256SUMS                                          of both
# It fails, and writes no release, unless every check passes: check-bundle.sh against all four SteamOS builds in
# tools/host-sonames, the audit (glibc symbols up to 2.39, nothing unbundled), and the glibc 2.39 loader tests.
#   steam-frame/release.sh [--no-build]
#   RELEASE_PREBUILT=<dir> steam-frame/release.sh   use the binaries already in <dir> (for example the desktop's
#                       pf-remote-build.sh --portable output): no compile, everything else (strip, bundle, checks) runs
# Environment:
#   PF_RELEASE_OUT       where everything goes (default ~/Games/PrimedFrame/v2/release-out)
#   PF_BUILD_DIR         build folder (default <repo>/build-portable; see build.sh for its other variables)
#   PF_KIT               the portable kit (default ~/Games/frame-portable: tools-audit.py and the glibc 2.39 tree)
#   PF_DAWN_PACKAGE      the patched Dawn package (required for a build)
# It never installs anything and never touches Steam.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$(cd "$HERE/.." && pwd)
VERSION=$(head -n 1 "$HERE/VERSION")
OUT=${PF_RELEASE_OUT:-$HOME/Games/PrimedFrame/v2/release-out}
B=${RELEASE_PREBUILT:-${PF_BUILD_DIR:-$SRC/build-portable}}
KIT=${PF_KIT:-$HOME/Games/frame-portable}
SYSROOT=${PF_GLIBC239_SYSROOT:-$KIT/sysroot-glibc2.39}
NAME=primedframe-$VERSION-linux-arm64
APPDIR=$OUT/work/$NAME
fail() { echo "release: $*" >&2; exit 1; }
[ -d "$SYSROOT/usr/lib" ] || fail "no glibc 2.39 tree at $SYSROOT"
[ -f "$KIT/tools-audit.py" ] || fail "no $KIT/tools-audit.py"
if [ -e "$HOME/.cache/headset-in-use" ]; then fail "the headset is in use ($HOME/.cache/headset-in-use): no builds now"; fi

# ---- 1. build (under the shared build lock; build.sh takes it) ----------------------------------------------------
if [ "${1:-}" != --no-build ] && [ -z "${RELEASE_PREBUILT:-}" ]; then
  mem=$(awk '/MemAvailable/ { print int($2 / 1024) }' /proc/meminfo)
  [ "$mem" -ge 2048 ] || fail "only $mem MB of memory available (need 2048): not building now"
  PF_BUILD_DIR=$B "$HERE/build.sh"
fi
GAME=$B/metroid_prime_port
LAUNCHER=$B/PrimedGun
[ -x "$GAME" ] || fail "no $GAME"
[ -x "$LAUNCHER" ] || fail "no $LAUNCHER"

# ---- 2. lay out the app folder ---------------------------------------------------------------------------------------
rm -rf "$OUT/work" "$OUT/$NAME.tar.gz" "$OUT/$NAME-symbols.tar.gz" "$OUT/SHA256SUMS"
mkdir -p "$APPDIR" "$OUT/work/symbols"
# programs: debug info goes to the symbols tarball, the symbol table stays (crash reports name functions)
for p in "$GAME" "$LAUNCHER"; do
  base=$(basename "$p")
  cp "$p" "$APPDIR/$base"
  objcopy --only-keep-debug "$p" "$OUT/work/symbols/$base.debug"
  strip --strip-debug "$APPDIR/$base"
  objcopy --add-gnu-debuglink="$OUT/work/symbols/$base.debug" "$APPDIR/$base"
done
# Qt, ICU, OpenSSL and the other libraries the allowlist does not cover, the Qt plugins, qt.conf
PF_GAME="$APPDIR/metroid_prime_port" "$HERE/portable/stage-bundle.sh" "$APPDIR/PrimedGun" "$OUT/work/bundle" > "$OUT/work/stage-bundle.log"
tail -n 3 "$OUT/work/stage-bundle.log"
cp -a "$OUT/work/bundle/lib" "$OUT/work/bundle/plugins" "$OUT/work/bundle/qt.conf" "$APPDIR/"
for f in primedframe.sh install.sh uninstall.sh collect-logs.sh steam-cef.py dock-icon.py VERSION; do cp "$HERE/$f" "$APPDIR/$f"; done
cp -a "$HERE/art" "$APPDIR/art"
chmod +x "$APPDIR"/*.sh "$APPDIR"/*.py
mkdir -p "$APPDIR/licenses"
"$SRC/tools/collect_licenses.sh" "$B" "$APPDIR/licenses" > /dev/null
cp "$SRC/LICENSE" "$APPDIR/LICENSE"
cp "$SRC/NOTICE" "$APPDIR/NOTICE" 2> /dev/null || true
cp "$HERE/THIRD_PARTY_NOTICES.md" "$APPDIR/THIRD_PARTY_NOTICES.md"
# the system libraries that were copied: their licences (the Qt LGPL texts first)
for d in qt6-base icu openssl systemd libcap pcre2 libb2 libxcb libxau libxdmcp md4c double-conversion graphite bzip2 libffi util-linux-libs gcc-libs; do
  [ -d "/usr/share/licenses/$d" ] && { mkdir -p "$APPDIR/licenses/system/$d"; cp -L /usr/share/licenses/$d/* "$APPDIR/licenses/system/$d/" 2> /dev/null || true; }
done

# ---- 3. checks ------------------------------------------------------------------------------------------------------------
echo "== check-bundle (all four SteamOS builds in tools/host-sonames)"
"$HERE/tools/check-bundle.sh" "$APPDIR" | tee "$OUT/work/check-bundle.log"
echo "== audit"
python3 -I "$KIT/tools-audit.py" "$APPDIR" | tee "$OUT/work/audit.log"
python3 -I - "$OUT/work/audit.log" << 'PY' || fail "the audit found glibc symbols newer than 2.39 or libraries that are neither bundled nor on every SteamOS"
import re, sys
text = open(sys.argv[1]).read()
for m in re.finditer(r'max GLIBC: (\d+)\.(\d+)', text):
    if (int(m.group(1)), int(m.group(2))) > (2, 39):
        sys.exit(1)
for m in re.finditer(r'max (GLIBCXX|CXXABI): ([\d.]+)', text):
    v = int(m.group(2).split('.')[-1])   # the audit prints the last number: GLIBCXX_3.4.<v>, CXXABI_1.3.<v>
    if (m.group(1) == 'GLIBCXX' and v > 34) or (m.group(1) == 'CXXABI' and v > 15):
        sys.exit(1)
if 'NOT ON ALL BUILDS' in text:
    sys.exit(1)
PY
echo "== loader tests with glibc 2.39"
LD=$SYSROOT/usr/lib/ld-linux-aarch64.so.1
LP=$APPDIR/lib:$SYSROOT/usr/lib:/usr/lib
for p in metroid_prime_port; do
  # --list maps every library and checks every symbol version it asks of the 2.39 glibc; the game is not started
  out=$(env -u LD_LIBRARY_PATH "$LD" --library-path "$LP" --list "$APPDIR/$p" 2>&1) || { echo "$out"; fail "$p: the glibc 2.39 loader cannot map it"; }
  echo "$out" | grep -E "not found|version .* not found|undefined" && fail "$p: the glibc 2.39 loader found problems"
  echo "  $p: all libraries found, all symbol versions satisfied by glibc 2.39 ($(echo "$out" | grep -c '=>') libraries)"
done
# the launcher: the 2.39 loader cannot start it on a machine whose own libraries (glib, ...) are built for glibc 2.43,
# because it then maps those host libraries too. So: (a) the 2.39 loader's complaints may name only host libraries
# (anything it names inside the app folder fails the release), (b) the launcher runs 4 s under this machine's own
# loader on Qt's offscreen platform with the bundled Qt and plugins, and must still be up.
out=$(env -u LD_LIBRARY_PATH "$LD" --library-path "$LP" --list "$APPDIR/PrimedGun" 2>&1 || true)
if ! echo "$out" | grep -q "not found"; then
  echo "  PrimedGun: the glibc 2.39 loader maps all of it"
else
  bad=$(echo "$out" | grep "not found" | grep -v -E "version .GLIBC_2\.4[0-9]. not found \(required by /usr/lib/" || true)
  [ -z "$bad" ] || { echo "$bad"; fail "PrimedGun: the glibc 2.39 loader found a problem that is not the host's own libraries"; }
  echo "  PrimedGun: the 2.39 loader objects only to this machine's own libraries (built for a newer glibc): $(echo "$out" | grep -c 'not found') messages, none about the app folder"
fi
tmp=$(mktemp -d)
set +e
env QT_QPA_PLATFORM=offscreen MP_USER_PATH="$tmp/" HOME="$tmp" LD_LIBRARY_PATH="$APPDIR/lib" timeout 4 "$APPDIR/PrimedGun" > "$tmp/launcher.log" 2>&1
rc=$?
set -e
[ "$rc" = 124 ] || { cat "$tmp/launcher.log"; fail "the launcher did not stay up on Qt's offscreen platform (exit $rc)"; }
echo "  PrimedGun: ran 4 s on Qt's offscreen platform with the bundled Qt and plugins"
rm -rf "$tmp"

# ---- 4. pack ------------------------------------------------------------------------------------------------------------------
tar --owner=0 --group=0 --numeric-owner --sort=name -cf - -C "$APPDIR" . | gzip -n > "$OUT/$NAME.tar.gz"
tar --owner=0 --group=0 --numeric-owner --sort=name -cf - -C "$OUT/work/symbols" . | gzip -n > "$OUT/$NAME-symbols.tar.gz"
(cd "$OUT" && sha256sum "$NAME.tar.gz" "$NAME-symbols.tar.gz" | tee SHA256SUMS)
echo "wrote $OUT/$NAME.tar.gz ($(du -h "$OUT/$NAME.tar.gz" | cut -f1)), the symbols tarball and SHA256SUMS; installed size $(du -sh "$APPDIR" | cut -f1)"
