#!/bin/bash
# Fails unless an unpacked release (the "app" folder) loads nothing from the system but the libraries on
# tools/host-libs.allow, on every SteamOS build listed in tools/host-sonames/.  The same idea as Moonlight XR
# for Frame's tools/check-bundle.sh.
#   steam-frame/tools/check-bundle.sh <app dir>
# For every ELF file under the app dir (metroid_prime_port or PrimedGun, lib/, the Qt plugins):
#   - each library it needs (NEEDED) is in app/lib, or on the allowlist and named in every
#     tools/host-sonames/*.txt (so it exists, under that soname, on every SteamOS build we know);
#   - it needs no glibc symbol newer than GLIBC_2.39 (the oldest SteamOS) and no libstdc++ or libgcc
#     symbol newer than the oldest SteamOS's (GLIBCXX_3.4.34, CXXABI_1.3.15, GCC_15.0.0);
#   - it needs no Qt symbol version (Qt_6.x) newer than the Qt in app/lib defines;
#   - it has no RPATH; every RUNPATH entry is relative ($ORIGIN), never a build machine's path; the
#     top-level binary's RUNPATH is exactly $ORIGIN/lib;
#   - resolved here (ldd, LD_LIBRARY_PATH unset), each library it needs comes from the app dir, or is
#     allowlisted.
# And app/lib holds nothing on the allowlist (the graphics stack and the C runtime always come from the
# system), and qt.conf points Qt at the app dir.
# What it cannot see: libraries opened at run time with dlopen (the Vulkan loader, the OpenXR loader, the
# audio, window and input backends of SDL 3 (linked in; it opens Wayland, X11, PipeWire, Pulse, ALSA and udev itself)
# and the runtime the OpenXR loader opens).
# ALLOW_MISSING_HOST=1: an allowlisted library this machine lacks is a warning, not a failure (a build
# container is not SteamOS); a library that is not allowlisted must still resolve inside the app dir.
# GLIBC_MAX, GLIBCXX_MAX, CXXABI_MAX, GCC_MAX override the limits (not for a release).
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
APP="$(cd "${1:?usage: check-bundle.sh <app dir>}" && pwd)"
ALLOW="${CHECK_BUNDLE_ALLOW:-$HERE/host-libs.allow}"
LISTS=("$HERE"/host-sonames/*.txt)
GLIBC_MAX="${GLIBC_MAX:-2.39}" GLIBCXX_MAX="${GLIBCXX_MAX:-3.4.34}" CXXABI_MAX="${CXXABI_MAX:-1.3.15}" GCC_MAX="${GCC_MAX:-15.0.0}"
for t in objdump readelf ldd; do command -v "$t" >/dev/null || { echo "check-bundle: missing tool $t"; exit 1; }; done
[ -f "${LISTS[0]}" ] || { echo "check-bundle: no tools/host-sonames/*.txt"; exit 1; }
mapfile -t PATS < <(sed -e 's/#.*//' -e 's/[[:space:]]//g' "$ALLOW" | grep .)
allowed() { local p; for p in "${PATS[@]}"; do [[ "$1" == $p ]] && return 0; done; return 1; }
newer() { [ "$1" != "$2" ] && [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | tail -n 1)" = "$1" ]; }   # newer A B: A > B
bad=0; warn=0; n=0
fail() { echo "  FAIL $*"; bad=$((bad+1)); }
declare -A EXT=()
L="$(readlink -f "$APP/lib" 2>/dev/null || echo "$APP/lib")"
if [ -d "$APP/lib" ]; then
    for f in "$APP"/lib/*; do [ -e "$f" ] && allowed "${f##*/}" && fail "app/lib/${f##*/} is on the allowlist: it must come from the system"; done
fi
# the newest Qt symbol version the bundled Qt defines
QTMAX=
if [ -f "$APP/lib/libQt6Core.so.6" ]; then
    QTMAX="$(objdump -p "$APP/lib/libQt6Core.so.6" 2>/dev/null | grep -oE 'Qt_6\.[0-9]+(\.[0-9]+)?' | sed 's/^Qt_//' | sort -uV | tail -n 1)"
fi
if [ -f "$APP/qt.conf" ]; then
    grep -qE '^[[:space:]]*Prefix[[:space:]]*=[[:space:]]*\.[[:space:]]*$' "$APP/qt.conf" || fail "qt.conf: Prefix is not ."
    grep -qE '^[[:space:]]*Plugins[[:space:]]*=[[:space:]]*plugins' "$APP/qt.conf" || fail "qt.conf: Plugins is not plugins"
elif [ -f "$APP/lib/libQt6Core.so.6" ]; then
    fail "qt.conf is missing (Qt would look for its plugins in the system)"
fi
while IFS= read -r -d '' f; do
    [ "$(head -c 4 "$f" | od -An -tx1 | tr -d ' \n')" = 7f454c46 ] || continue
    n=$((n+1)); rel="${f#"$APP"/}"
    dyn="$(readelf -d "$f" 2>/dev/null)"
    grep -q '(RPATH)' <<<"$dyn" && fail "$rel has an RPATH"
    rp="$(sed -n 's/.*(RUNPATH).*\[\(.*\)\]/\1/p' <<<"$dyn")"
    if [[ "$rel" != */* ]] && [ "$rp" != '$ORIGIN/lib' ]; then fail "$rel: RUNPATH '$rp', not \$ORIGIN/lib"; fi
    IFS=: read -ra rps <<<"$rp"
    for e in "${rps[@]}"; do [ -z "$e" ] || [[ "$e" == '$ORIGIN'* ]] || fail "$rel: RUNPATH entry '$e' is not relative to \$ORIGIN"; done
    # the symbol versions it needs from glibc, libstdc++, libgcc and Qt
    vers="$(objdump -p "$f" 2>/dev/null | sed -n '/^Version References:/,/^$/p')"
    for k in GLIBC:$GLIBC_MAX GLIBCXX:$GLIBCXX_MAX CXXABI:$CXXABI_MAX GCC:$GCC_MAX; do
        top="$(grep -oE "[[:space:]]${k%%:*}_[0-9][0-9.]*" <<<"$vers" | sed "s/^[[:space:]]*${k%%:*}_//" | sort -uV | tail -n 1)"
        [ -n "$top" ] && newer "$top" "${k#*:}" && fail "$rel needs ${k%%:*}_$top (the oldest SteamOS has up to ${k%%:*}_${k#*:})"
    done
    qtneed="$(grep -oE 'Qt_6(\.[0-9]+)*' <<<"$vers" | sed 's/^Qt_//' | sort -uV | tail -n 1)"
    if [ -n "$qtneed" ]; then
        if [ -z "$QTMAX" ]; then fail "$rel needs Qt_$qtneed but app/lib has no Qt"
        elif newer "$qtneed" "$QTMAX"; then fail "$rel needs Qt_$qtneed, the bundled Qt defines up to Qt_$QTMAX"; fi
    fi
    mapfile -t need < <(objdump -p "$f" 2>/dev/null | awk '$1 == "NEEDED" { print $2 }')
    [ "${#need[@]}" -gt 0 ] || continue
    for s in "${need[@]}"; do
        [ -f "$APP/lib/$s" ] && continue
        if ! allowed "$s"; then fail "$rel needs $s: not in app/lib and not on tools/host-libs.allow"; continue; fi
        EXT[$s]=1
        for l in "${LISTS[@]}"; do grep -qxF "$s" "$l" || fail "$rel needs $s, which SteamOS ${l##*/} does not have"; done
    done
    # resolved here, only the libraries it needs itself (the system's libraries' own needs are the system's)
    # (a bundled library without a RUNPATH of its own is resolved with app/lib on the path: in the app its
    # siblings are already loaded by the binary)
    if [[ "$rel" == lib/* ]]; then out="$(LD_LIBRARY_PATH="$L" ldd "$f" 2>&1)"; else out="$(env -u LD_LIBRARY_PATH ldd "$f" 2>&1)"; fi
    for s in "${need[@]}"; do
        line="$(grep -E "^[[:space:]]*$(sed 's/[.+]/\\&/g' <<<"$s") => " <<<"$out" | head -n 1)"
        [ -n "$line" ] || continue                     # ld-linux and the like: no "=>" line
        p="$(awk '{ print $3 }' <<<"$line")"
        if [ "$p" = not ]; then
            if allowed "$s" && [ "${ALLOW_MISSING_HOST:-0}" = 1 ]; then warn=$((warn+1))
            else fail "$rel: $s not found"; fi
            continue
        fi
        real="$(readlink -f "$p")"
        [[ "$real" == "$APP/"* || "$real" == "$L/"* ]] && continue
        if [ -f "$APP/lib/$s" ]; then fail "$rel loads $s from $real, not its own app/lib copy"
        elif ! allowed "$s"; then fail "$rel loads $s from $real, outside the app"; fi
    done
done < <(find "$APP" -type f -print0 | sort -z)
echo "check-bundle: $n ELF files; from the system: $(printf '%s\n' "${!EXT[@]}" | sort | tr '\n' ' ')"
[ -n "$QTMAX" ] && echo "check-bundle: the bundled Qt defines up to Qt_$QTMAX; limits: glibc $GLIBC_MAX, GLIBCXX $GLIBCXX_MAX"
[ "$warn" = 0 ] || echo "check-bundle: $warn allowlisted libraries are not installed on this machine (ALLOW_MISSING_HOST=1)"
[ "$bad" = 0 ] && { echo "check-bundle ok: $APP"; exit 0; }
echo "check-bundle FAILED: $bad problems in $APP"; exit 1
