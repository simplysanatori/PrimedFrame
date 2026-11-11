#!/bin/bash
# PrimedFrame's launch script: the one Steam entry "PrimedFrame" runs this. It points the game at its data
# folder, sets SteamVR's per-app keys for this entry (90 Hz, no motion smoothing, 1728 px eyes) and checks
# them by reading them back, starts the program (the launcher decides between its window and the game), and
# keeps a log of the session.
#
#   primedframe.sh [--dry-run] [program arguments]
#     --dry-run   print the environment, the SteamVR keys and the command; start nothing, change nothing
#
# Environment (all optional):
#   PRIMEDFRAME_HOME      data folder: saves, settings, mods, caches, sessions (default ~/.local/share/PrimedFrame/v2)
#   PRIMEDFRAME_HZ        SteamVR's refresh rate for this entry (72 80 90 96 108 120 144; default 90)
#   PRIMEDFRAME_EYE       SteamVR's render size per eye, pixels (default 1728)
#   PRIMEDFRAME_APPID     the Steam entry's appid when this is not started from Steam
#   PRIMEDFRAME_VR_WAIT   seconds to wait for SteamVR to accept the keys (default 20)
#   PRIMEDFRAME_STEAMVR_DIR  SteamVR's folder with vrcmd (default /opt/steamvr/bin/linuxarm64)
# Everything the session does is logged in $PRIMEDFRAME_HOME/sessions/<time>/ (launch.log, steamvr-keys.txt,
# stdout.log, stderr.log, the game's own log); sessions/latest points at the newest. steam-frame/collect-logs.sh
# packs them for a bug report.
HERE=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
DATA=${PRIMEDFRAME_HOME:-$HOME/.local/share/PrimedFrame/v2}
STEAMVR_DIR=${PRIMEDFRAME_STEAMVR_DIR:-/opt/steamvr/bin/linuxarm64}
VRCMD=${PRIMEDFRAME_VRCMD:-$STEAMVR_DIR/vrcmd}
GAME=$HERE/metroid_prime_port
MARKER=$HOME/.cache/headset-in-use

DRY=0
[ "$1" = --dry-run ] && { DRY=1; shift; }

# ---- the command this entry runs ---------------------------------------------------------------------------
# The launcher's start mode: with a saved, valid disc it execs the game in place (no window, VR); without one it
# opens its window, and Play execs the game in place. One process either way. PrimedGun must sit next to
# metroid_prime_port (the in-VR Open Launcher row only appears then).
START_CMD=("$HERE/PrimedGun" --frame-start)
CMD=("${START_CMD[@]}" "$@")

# ---- environment for the program -----------------------------------------------------------------------------
export MP_USER_PATH="$DATA/"                 # the game's data and its memory card (the program folder stays replaceable)
export MP_VR_LOG=1
export LD_LIBRARY_PATH="$HERE/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"   # the bundled Qt and libraries win over Steam's runtime
# Steam adds nothing to this program's environment that needs removing (native Vulkan and OpenXR).

HZ=${PRIMEDFRAME_HZ:-90}; EYE=${PRIMEDFRAME_EYE:-1728}
case "$HZ" in 72 | 80 | 90 | 96 | 108 | 120 | 144) ;; *) echo "primedframe: PRIMEDFRAME_HZ=$HZ is not a Frame mode (72 80 90 96 108 120 144), using 90" >&2; HZ=90 ;; esac
case "$EYE" in [0-9][0-9][0-9][0-9]) ;; *) echo "primedframe: PRIMEDFRAME_EYE=$EYE is not a pixel size, using 1728" >&2; EYE=1728 ;; esac
APPID=${PRIMEDFRAME_APPID:-$SteamAppId}
KEYS=("preferredRefreshRate=$HZ" "motionSmoothingOverride=2" "resolutionOverride=$EYE")   # 2 = motion smoothing off

# vrcmd read of one per-app key: prints the value, empty when unset or unreadable.
vr_get() { env -u LD_PRELOAD timeout 5 "$VRCMD" --settings-int "steam.app.$APPID.$1" 2> /dev/null | sed -n "s/^steam\.app\.$APPID\.$1=//p" | head -n 1; }

if [ "$DRY" = 1 ]; then
  echo "app:      $HERE"
  echo "env:      MP_USER_PATH=$MP_USER_PATH MP_VR_LOG=$MP_VR_LOG"
  echo "          LD_LIBRARY_PATH=$LD_LIBRARY_PATH"
  echo "steamvr:  per-app keys for steam.app.${APPID:-<no SteamAppId: would NOT be applied, logged loudly>}  (retry up to ${PRIMEDFRAME_VR_WAIT:-20} s, read back each)"
  for kv in "${KEYS[@]}"; do
    k=${kv%=*}; cur=; [ -n "$APPID" ] && [ -x "$VRCMD" ] && cur=$(vr_get "$k")
    echo "          vrcmd --set-settings-int steam.app.${APPID:-?}.$k ${kv#*=}   (read now: ${cur:-<unset/unreadable>})"
  done
  echo "touch:    $MARKER (removed on exit)"
  printf 'command:'; printf ' %q' "${CMD[@]}"; echo
  [ -x "$GAME" ] && echo "game: ok" || echo "game: MISSING"
  [ -x "${START_CMD[0]}" ] && echo "launcher: ok" || echo "launcher: MISSING"
  exit 0
fi

[ -x "${START_CMD[0]}" ] || { echo "primedframe: missing ${START_CMD[0]}" >&2; exit 1; }
STAMP=$(date +%Y%m%d-%H%M%S)
SESS=$DATA/sessions/$STAMP
mkdir -p "$SESS" "$HOME/.cache"
LOG=$SESS/launch.log
log() { echo "$(date +%T) $*" >> "$LOG"; }
touch "$MARKER"
{ echo "launch $STAMP"; env | grep -E '^(MP_|XR_|VK_|MESA|SteamGameId|SteamAppId|QT_QPA|DISABLE_VULKAN)' | sort
  printf 'cmd:'; printf ' %q' "${CMD[@]}"; echo; } > "$LOG"

GAME_PID=
cleanup() {
  rm -f "$MARKER"
  [ -f "$DATA/metroid_prime_port.log" ] && cp -f "$DATA/metroid_prime_port.log" "$SESS/" 2> /dev/null
  ln -sfn "$SESS" "$DATA/sessions/latest"
  # keep the 30 newest sessions
  ls -1d "$DATA"/sessions/2* 2> /dev/null | sort | head -n -30 | while read -r d; do rm -rf "$d"; done
}
trap cleanup EXIT
trap '[ -n "$GAME_PID" ] && kill -TERM "$GAME_PID" 2> /dev/null; exit 143' TERM INT HUP

log "PrimedFrame launch script, session $STAMP, SteamAppId '${SteamAppId:-unset}'"

# ---- SteamVR per-app keys, with read-back ------------------------------------------------------------------------
# Shows nothing on screen; every outcome is in launch.log and steamvr-keys.txt. SteamVR may still be starting
# when this runs (it takes the keys only once vrserver is up), so it retries.
RB=$SESS/steamvr-keys.txt
if [ -z "$APPID" ]; then
  log "!!! LOUD: no SteamAppId (not a Steam library launch) and no PRIMEDFRAME_APPID: SteamVR per-app keys NOT applied; SteamVR will pick its own refresh rate"
  echo "NOT APPLIED: no SteamAppId" > "$RB"
elif [ ! -x "$VRCMD" ]; then
  log "!!! LOUD: no vrcmd at $VRCMD: SteamVR per-app keys NOT applied"
  echo "NOT APPLIED: no vrcmd" > "$RB"
else
  DEADLINE=$((SECONDS + ${PRIMEDFRAME_VR_WAIT:-20})); TRIES=0; ALLOK=0
  while :; do
    TRIES=$((TRIES + 1)); ALLOK=1; : > "$RB"
    if pgrep -x vrserver > /dev/null 2>&1; then
      for kv in "${KEYS[@]}"; do
        k=${kv%=*}; want=${kv#*=}; got=$(vr_get "$k")
        if [ "$got" != "$want" ]; then
          env -u LD_PRELOAD timeout 5 "$VRCMD" --set-settings-int "steam.app.$APPID.$k" "$want" > /dev/null 2>&1
          sleep 0.3; got=$(vr_get "$k")
        fi
        echo "$k wanted=$want got=${got:-<unreadable>}" >> "$RB"
        [ "$got" = "$want" ] || ALLOK=0
      done
    else
      ALLOK=0; echo "vrserver not running yet" > "$RB"
    fi
    [ "$ALLOK" = 1 ] && break
    [ "$SECONDS" -ge "$DEADLINE" ] && break
    sleep 1
  done
  if [ "$ALLOK" = 1 ]; then log "steamvr keys for steam.app.$APPID applied and read back (try $TRIES): $(tr '\n' ' ' < "$RB")"
  else log "!!! LOUD: steamvr keys for steam.app.$APPID NOT confirmed after $TRIES tries / ${PRIMEDFRAME_VR_WAIT:-20} s: $(tr '\n' ' ' < "$RB"); expect SteamVR's own refresh rate and smoothing"; fi
fi

# ---- the program ----------------------------------------------------------------------------------------------
cd "$HERE" || exit 1
log "starting ${CMD[*]}"
"${CMD[@]}" > "$SESS/stdout.log" 2> "$SESS/stderr.log" &
GAME_PID=$!
wait "$GAME_PID"
RC=$?
GAME_PID=
# the launcher may start the game and leave: stay (and keep the headset marker) while the game runs
while pgrep -f "^$GAME( |\$)" > /dev/null 2>&1; do sleep 1; done
echo "exit $RC" >> "$LOG"
exit $RC
