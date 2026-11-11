#!/bin/bash
# Lays out what the launcher carries so that PrimedFrame does not depend on the Qt SteamOS has: lib/ (Qt, ICU and
# every other library tools/host-libs.allow does not list), plugins/ (the few Qt plugins the launcher needs) and
# qt.conf (Qt's prefix is the app folder). Adapted from Azahar for Frame's stage-bundle.sh.
#   stage-bundle.sh <program> <out dir> [allowlist]
# <program> is the launcher binary (PrimedGun); the game itself needs only allowlisted libraries (OpenSSL is bundled
# below because the game links it). Needs patchelf, objdump, readelf.
#
# The libraries are copied from this machine's /usr/lib (SteamOS: glibc symbols up to 2.38, which any SteamOS
# runs; check-bundle.sh proves it). Qt, its plugins and ICU are the SteamOS ones, so they match each other.
set -euo pipefail
BIN="$(readlink -f "${1:?usage: stage-bundle.sh <program> <out dir> [allowlist]}")"
GAME="${PF_GAME:-}"
OUT="${2:?out dir}"
HERE="$(cd "$(dirname "$0")" && pwd)"
ALLOW="${3:-$HERE/../tools/host-libs.allow}"
LIBDIR=${PF_LIBDIR:-/usr/lib}
QTPLUG=${PF_QT_PLUGINS:-$LIBDIR/qt6/plugins}
for t in patchelf objdump readelf; do command -v "$t" > /dev/null || { echo "missing tool: $t"; exit 1; }; done
[ -f "$LIBDIR/libQt6Core.so.6" ] || { echo "no Qt in $LIBDIR"; exit 1; }
[ -f "$BIN" ] || { echo "no program at $BIN"; exit 1; }
mapfile -t PATS < <(sed -e 's/#.*//' -e 's/[[:space:]]//g' "$ALLOW" | grep .)
allowed() { local p; for p in "${PATS[@]}"; do [[ "$1" == $p ]] && return 0; done; return 1; }

rm -rf "$OUT"; mkdir -p "$OUT/lib" "$OUT/plugins"
cat > "$OUT/qt.conf" << 'CONF'
; PrimedFrame: its own Qt (steam-frame/portable), never the system's
[Paths]
Prefix = .
Libraries = lib
Plugins = plugins
CONF

# The plugins the launcher (a Widgets window) needs: the Wayland and X11 (Xwayland) platforms, offscreen, the
# Wayland window pieces, and the compose input context. No image-format, icon, theme, TLS or multimedia plugins.
echo "== plugins"
want="platforms/libqwayland.so platforms/libqxcb.so platforms/libqoffscreen.so wayland-shell-integration/libxdg-shell.so
      wayland-decoration-client/libbradient.so wayland-graphics-integration-client/libshm-emulation-server.so
      platforminputcontexts/libcomposeplatforminputcontextplugin.so"
for p in $want; do
    [ -f "$QTPLUG/$p" ] || { echo "missing Qt plugin $p in $QTPLUG"; exit 1; }
    mkdir -p "$OUT/plugins/$(dirname "$p")"; cp -L "$QTPLUG/$p" "$OUT/plugins/$p"
done
for d in "$OUT"/plugins/*/; do echo "  ${d#"$OUT"/}: $(cd "$d" && ls | tr '\n' ' ')"; done

echo "== libraries"
# every library the programs, the plugins and the bundled libraries need, until nothing new comes in
while :; do
    added=0
    while IFS= read -r -d '' f; do
        [ "$(head -c 4 "$f" | od -An -tx1 | tr -d ' \n')" = 7f454c46 ] || continue
        for s in $(objdump -p "$f" | awk '$1 == "NEEDED" { print $2 }'); do
            [ -e "$OUT/lib/$s" ] && continue
            allowed "$s" && continue
            [ -e "$LIBDIR/$s" ] || { echo "${f#"$OUT"/} needs $s: not in $LIBDIR, not allowlisted"; exit 1; }
            cp -L "$LIBDIR/$s" "$OUT/lib/$s"
            chmod 755 "$OUT/lib/$s"
            added=1
        done
    done < <({ printf '%s\0' "$BIN"; [ -z "$GAME" ] || printf '%s\0' "$GAME"; find "$OUT" -type f -print0; })
    [ "$added" = 1 ] || break
done
# libraries the programs open at run time (not in NEEDED): Qt's TLS and the like are not shipped; nothing else is known.

# RUNPATH: libraries find their neighbours in lib/, plugins two folders up. (Done last: strip after patchelf can damage a file.)
for f in "$OUT"/lib/*; do
    patchelf --remove-rpath "$f"; patchelf --set-rpath '$ORIGIN' "$f"
done
while IFS= read -r -d '' f; do
    patchelf --remove-rpath "$f"; patchelf --set-rpath '$ORIGIN/../../lib' "$f"
done < <(find "$OUT/plugins" -type f -name '*.so' -print0)

echo "  lib/: $(cd "$OUT/lib" && ls | wc -l) files"
echo "staged $OUT ($(du -sh "$OUT" | cut -f1))"
