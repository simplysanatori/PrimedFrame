#!/bin/bash
# Removes PrimedFrame's Steam library entry and program folder. The data folder (~/.local/share/PrimedFrame/v2:
# your saves, settings, mods, caches and logs) stays unless --purge. Older app folders kept by install.sh
# (app.bak-<date>) and the PrimedFrame 1.x data are never touched. Your disc image is never touched.
#   uninstall.sh [--purge]
# Environment: PRIMEDFRAME_APP, PRIMEDFRAME_HOME, PRIMEDFRAME_SKIP_STEAM and PRIMEDFRAME_CEF_PORT as in install.sh.
# If Steam is not running the entry stays; run this again with Steam up, or remove it in Steam.
# It never asks a question, so Frametanium can run it.
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
DEST=${PRIMEDFRAME_APP:-$HOME/Games/PrimedFrame/app}
DATA=${PRIMEDFRAME_HOME:-$HOME/.local/share/PrimedFrame/v2}
NAME="PrimedFrame"
PURGE=0
case "$1" in --purge) PURGE=1 ;; "") ;; *) echo "usage: $0 [--purge]" >&2; exit 2 ;; esac
if pgrep -f "^$DEST/(metroid_prime_port|PrimedGun)( |\$)" > /dev/null 2>&1; then echo "PrimedFrame is running; quit it first" >&2; exit 1; fi

# steam-cef.py and dock-icon.py are next to this script, or in the program folder when this is a loose copy.
CEFPY=$HERE/steam-cef.py; [ -f "$CEFPY" ] || CEFPY=$DEST/steam-cef.py
DOCKPY=$HERE/dock-icon.py; [ -f "$DOCKPY" ] || DOCKPY=$DEST/dock-icon.py

if [ -n "$PRIMEDFRAME_SKIP_STEAM" ]; then
  echo "PRIMEDFRAME_SKIP_STEAM set: Steam left alone"
elif curl -s --max-time 3 "127.0.0.1:${PRIMEDFRAME_CEF_PORT:-8080}/json" > /dev/null; then
  CEF="python3 $CEFPY"
  ID=$($CEF "(() => { for (const a of (appStore.allApps || [])) { if (a.display_name === '$NAME' && a.app_type === 1073741824) return a.appid; } return 0; })()" 2> /dev/null || echo 0)
  if [ -n "$ID" ] && [ "$ID" != 0 ]; then
    $CEF "(() => { SteamClient.Apps.RemoveShortcut($ID); return 1; })()" > /dev/null
    echo "removed the '$NAME' entry (appid $ID)"
  else
    echo "Steam has no '$NAME' entry"
  fi
  [ -f "$DOCKPY" ] && { python3 -I "$DOCKPY" remove "$DEST" || echo "note: could not remove the dock icon files (see above)"; }
else
  echo "Steam is not reachable, so the '$NAME' entry stays; remove it in Steam, or run this again with Steam up."
fi

# The program folder goes last: this script may be running from it.
if [ -d "$DEST" ]; then rm -rf "$DEST"; echo "removed $DEST"; fi
if [ "$PURGE" = 1 ]; then
  case "$DATA" in */v2 | */v2/) ;; *) [ -n "$PRIMEDFRAME_HOME" ] || { echo "refusing to purge $DATA" >&2; exit 1; } ;; esac
  echo "--purge: removing $DATA (saves, settings, mods, caches, logs)"
  rm -rf "$DATA"
else
  echo "kept your data in $DATA; --purge removes it"
fi
