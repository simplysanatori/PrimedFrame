#!/bin/bash
# Installs PrimedFrame on the Steam Frame: the game, its launcher and the libraries they bring go to
# ~/Games/PrimedFrame/app, and the Steam library gets one entry, "PrimedFrame" (VR, with its art). An
# earlier PrimedFrame entry is reused, not duplicated. An older install of the app folder (PrimedFrame 1.x)
# is kept beside it as app.bak-<date>. Safe to run again: it replaces the program folder and leaves your
# saves, settings and mods alone (they live in ~/.local/share/PrimedFrame/v2; the 1.x data in
# ~/.local/share/PrimedFrame/user is never touched).
#   install.sh [--dry-run] [--remove-dev] [primedframe-*-linux-arm64.tar.gz]
# Without an argument it installs the folder it is in when that is an unpacked release (the tarball carries
# this script at its top level, which is how Frametanium runs it), else the newest tarball in steam-frame/release/.
#   --dry-run     print every file it would copy and every Steam call it would make; change nothing
#   --remove-dev  also remove the temporary "PrimedFrame 2.0 (dev)" Steam entry
# It never asks a question. When Steam cannot be reached it installs the files, says that the Steam entry
# was skipped and exits 0; run it again with Steam up.
#
# Environment (for trial installs): PRIMEDFRAME_APP (program folder), PRIMEDFRAME_HOME (data folder),
# PRIMEDFRAME_SKIP_STEAM=1 (leave Steam and SteamVR alone), PRIMEDFRAME_CEF_PORT (Steam's debugging port,
# default 8080), PRIMEDFRAME_VRCMD (SteamVR's vrcmd), PRIMEDFRAME_BACKUP_STAMP (the backup folder's date part).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
DEST=${PRIMEDFRAME_APP:-$HOME/Games/PrimedFrame/app}
DATA=${PRIMEDFRAME_HOME:-$HOME/.local/share/PrimedFrame/v2}
SKIP_STEAM=${PRIMEDFRAME_SKIP_STEAM:-}
NAME="PrimedFrame"
DEV_NAME="PrimedFrame 2.0 (dev)"
DRY=; REMOVE_DEV=; TARBALL=
while [ $# -gt 0 ]; do
  case "$1" in
    -h | --help) sed -n '2,/^set -e/p' "$0" | sed '$d; s/^# \{0,1\}//'; exit 0 ;;
    --dry-run) DRY=1 ;;
    --remove-dev) REMOVE_DEV=1 ;;
    -*) echo "unknown option $1" >&2; exit 2 ;;
    *) TARBALL=$1 ;;
  esac
  shift
done

act() { if [ -n "$DRY" ]; then echo "[dry run] would run: $*"; else "$@"; fi; }

UNPACKED=
if [ -z "$TARBALL" ]; then
  if [ -x "$HERE/metroid_prime_port" ] && [ -f "$HERE/primedframe.sh" ]; then
    UNPACKED=$HERE                                   # an unpacked release (Frametanium)
  else
    TARBALL=$(ls "$HERE"/release/primedframe-*-linux-arm64.tar.gz 2> /dev/null | sort -V | tail -1)
    [ -n "$TARBALL" ] || { echo "usage: $0 primedframe-*-linux-arm64.tar.gz (from the Releases page)" >&2; exit 1; }
  fi
fi
[ -z "$TARBALL" ] || [ -f "$TARBALL" ] || { echo "no such file: $TARBALL" >&2; exit 1; }

if pgrep -f "^$DEST/(metroid_prime_port|PrimedGun)( |\$)" > /dev/null 2>&1; then
  echo "PrimedFrame is running. Quit it and run install.sh again." >&2
  exit 1
fi

SRC=$UNPACKED
if [ -n "$TARBALL" ]; then
  echo "installing $TARBALL"
  STAGE=$(mktemp -d)
  trap 'rm -rf "$STAGE"' EXIT
  tar -xzf "$TARBALL" -C "$STAGE"
  SRC=$STAGE
  [ -x "$SRC/metroid_prime_port" ] || SRC=$(dirname "$(find "$STAGE" -maxdepth 2 -name primedframe.sh | head -1)")
  [ -x "$SRC/metroid_prime_port" ] || { echo "$TARBALL has no metroid_prime_port program" >&2; exit 1; }
else
  echo "installing the unpacked release in $SRC"
fi
VERSION=$(head -n 1 "$SRC/VERSION" 2> /dev/null || echo unknown)

[ -z "$DRY" ] || echo "[dry run] nothing below is done; HOME=$HOME DEST=$DEST DATA=$DATA"
act mkdir -p "$(dirname "$DEST")" "$DATA"

# ---- the program folder ---------------------------------------------------------------------------------------
if [ "$(readlink -f "$SRC")" = "$(readlink -f "$DEST")" ]; then
  echo "the release is already in $DEST"
else
  # An app folder that is not a PrimedFrame 2 one (1.x) is kept as app.bak-<date> (never overwritten).
  if [ -d "$DEST" ] && ! head -n 1 "$DEST/VERSION" 2> /dev/null | grep -q '^2\.'; then
    BAK=$DEST.bak-${PRIMEDFRAME_BACKUP_STAMP:-$(date +%Y%m%d)}
    n=1; while [ -e "$BAK" ]; do n=$((n + 1)); BAK=$DEST.bak-${PRIMEDFRAME_BACKUP_STAMP:-$(date +%Y%m%d)}-$n; done
    echo "the existing app folder is not a PrimedFrame 2 one: keeping it as $BAK"
    act mv "$DEST" "$BAK"
  fi
  if [ -n "$DRY" ]; then
    echo "[dry run] would copy $SRC/ -> $DEST/ (replacing a PrimedFrame 2 folder as a whole)"
  else
    rm -rf "$DEST.new"
    cp -a "$SRC" "$DEST.new"
    if [ -d "$DEST" ]; then mv "$DEST" "$DEST.old.$$"; fi
    mv "$DEST.new" "$DEST"
    rm -rf "$DEST.old.$$"
  fi
fi
act chmod +x "$DEST/primedframe.sh" "$DEST/metroid_prime_port" "$DEST/PrimedGun" "$DEST/install.sh" "$DEST/uninstall.sh" "$DEST/collect-logs.sh" "$DEST/dock-icon.py" "$DEST/steam-cef.py"
ART_SRC=$SRC/art
CEFPY=$DEST/steam-cef.py; DOCK_PY=$DEST/dock-icon.py
[ -z "$DRY" ] || { CEFPY=$SRC/steam-cef.py; DOCK_PY=$SRC/dock-icon.py; }

# ---- the Steam library entry ------------------------------------------------------------------------------------
APPID=none
STEAM_NOTE=
CEF="python3 $CEFPY"
find_js() { echo "(() => { for (const a of (appStore.allApps || [])) { if (a.display_name === '$1' && a.app_type === 1073741824) return a.appid; } return 0; })()"; }
# SetShortcutExe / SetShortcutStartDir store the text as given (quotes included); AddShortcut quotes it itself.
UPDATE_JS() { echo "(() => { SteamClient.Apps.SetShortcutExe($1, '\"$DEST/primedframe.sh\"'); SteamClient.Apps.SetShortcutStartDir($1, '\"$DEST\"'); SteamClient.Apps.SetShortcutLaunchOptions($1, ''); SteamClient.Apps.SetShortcutIsVR($1, true); return 1; })()"; }
ADD_JS="(async () => { const id = await SteamClient.Apps.AddShortcut('$NAME', '$DEST/primedframe.sh', '$DEST', ''); SteamClient.Apps.SetShortcutName(id, '$NAME'); return id; })()"
if [ -n "$SKIP_STEAM" ]; then
  STEAM_NOTE="PRIMEDFRAME_SKIP_STEAM is set: Steam and SteamVR were left alone."
elif [ -n "$DRY" ]; then
  echo "[dry run] Steam is not contacted; its debugging port (127.0.0.1:${PRIMEDFRAME_CEF_PORT:-8080}) is assumed reachable"
  echo "[dry run] would look for an existing '$NAME' entry: $(find_js "$NAME")"
  echo "[dry run] if it exists it would be reused: $(UPDATE_JS '<appid>')"
  echo "[dry run] otherwise it would be added: $ADD_JS, then the same update (VR flag, no launch options)"
  [ -z "$REMOVE_DEV" ] || echo "[dry run] --remove-dev: would remove the '$DEV_NAME' entry (RemoveShortcut on the appid found by name)"
  APPID=2147483648
  echo "[dry run] the entry's appid would be whatever Steam assigns or already has (placeholder $APPID below)"
elif ! curl -s --max-time 3 "127.0.0.1:${PRIMEDFRAME_CEF_PORT:-8080}/json" > /dev/null; then
  STEAM_NOTE="Steam was not reachable, so the library entry and its art were skipped. Run install.sh again with Steam up."
  echo "note: $STEAM_NOTE"
else
  EXISTING=$($CEF "$(find_js "$NAME")" 2> /dev/null || echo 0)
  if [ -n "$EXISTING" ] && [ "$EXISTING" != 0 ]; then
    APPID=$EXISTING
    echo "Steam already has a '$NAME' entry (appid $APPID): reusing it"
  else
    APPID=$($CEF "$ADD_JS" 2> /dev/null || echo 0)
    echo "added the '$NAME' entry (appid $APPID)"
  fi
  case "$APPID" in '' | 0 | *[!0-9]*)
    echo "note: Steam did not return an appid for the entry, so the library entry and its art were skipped; run install.sh again with Steam up"
    APPID=none
    STEAM_NOTE="Steam did not return an appid, so the library entry and its art were skipped. Run install.sh again with Steam up."
    ;;
  *) $CEF "$(UPDATE_JS "$APPID")" > /dev/null ;;
  esac
  if [ -n "$REMOVE_DEV" ]; then
    DEVID=$($CEF "$(find_js "$DEV_NAME")" 2> /dev/null || echo 0)
    if [ -n "$DEVID" ] && [ "$DEVID" != 0 ]; then
      $CEF "(() => { SteamClient.Apps.RemoveShortcut($DEVID); return 1; })()" > /dev/null
      echo "removed the '$DEV_NAME' entry (appid $DEVID)"
    else
      echo "Steam has no '$DEV_NAME' entry"
    fi
  fi
fi

if [ "$APPID" != none ] && [ -z "$SKIP_STEAM" ]; then
  # Library art (portrait, wide, hero, logo, icon), set live through Steam; run again to set it again.
  if [ -f "$ART_SRC/portrait.png" ]; then
    if [ -n "$DRY" ]; then python3 -I "$CEFPY" --art "$APPID" "$ART_SRC" --dry-run
    else python3 "$CEFPY" --art "$APPID" "$DEST/art" || echo "note: could not set the library art; run install.sh again with Steam up"; fi
  else
    echo "warning: no art in the release ($ART_SRC/portrait.png), the library entry has no artwork"
  fi
  # SteamVR's dock and dashboard: the VR app's name and icon (an app manifest for steam.app.<appid>).
  if [ -f "$DOCK_PY" ]; then
    python3 -I "$DOCK_PY" install "$DEST" --appid "$APPID" --art "$ART_SRC" ${DRY:+--dry-run} ||
      echo "note: the dock icon was not set up (see above); run install.sh again"
  fi
  STEAM_NOTE="Steam library entry \"$NAME\" (appid $APPID) runs $DEST/primedframe.sh.
Its artwork and icon are set. SteamVR's dock reads the icon when Steam starts: if it still shows
a question mark, restart Steam once. primedframe.sh sets SteamVR's refresh rate (90 Hz) for the
entry at every launch."
fi

if [ -n "$DRY" ]; then
  echo
  echo "[dry run] done: nothing was changed."
  exit 0
fi
cat << MSG

PrimedFrame $VERSION installed in $DEST.
Your saves, settings and mods are in $DATA (kept on every update).
$STEAM_NOTE
MSG
