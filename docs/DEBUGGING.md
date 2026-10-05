# Debugging the port headless: `tools/mprig.py`

One CLI replaces the per-task shell rigs: it starts its own Xvfb and game with the debug
console, drives it, screenshots, diffs and tears down. Parallel sessions never collide
(displays from `:200`, console ports from 5100, allocated under a lock). Every command exits
non-zero with a one-line `mprig: <reason>` on stderr when it fails. State is in `build/rig/`
(git-ignored): `build/rig/<name>/{run.json,game.log,user/,shots/}`.

The Xvfb/RADV environment (`MESA_VK_WSI_DEBUG=sw`, `SDL_VIDEODRIVER=x11`, dummy audio,
`MP_FAST_BOOT`, `MP_CONSOLE`, ...) is set automatically. **Never capture on `:0`**: GPU-heavy
runs on the real display have taken the desktop session down.

## Setup

Defaults come from `build/rig.ini` (`[rig]`: `disc`, `user`, `build`; local only), overridable
by env `MPRIG_DISC` / `MPRIG_USER` / `MPRIG_BUILD` (disc also `$MP_DISC`) and by flags.
`mprig.py doctor` checks it all (tools, binary, disc, user dir, Remastered `.import-version`
vs the current `kImportVersion`, `/tmp` space, stale runs).

## Quick start

    M="python3 tools/mprig.py"
    $M start t1 --room 83F6FF6F:D5CDB809    # prints: t1 :200 5100 <log>
    $M cmd t1 status 'roomenv info'          # one console command per argument
    $M shot t1 /tmp/a.png 'view albedo'      # run commands, screenshot -> PNG
    $M stop --all                            # always finish with this

Boot takes about 10 s with `--room`. Use your own run names so sessions stay apart.

## Subcommands

- `start <name> [--build B] [--room MLVL[:MREA]] [--env K=V]... [--mods DIR|none] [--settings FILE|none] [--saves] [--size WxH] [--wait S] [--gdb] [--replace]`:
  `--build` is a dir under `build/` or a path (default `port-gcc`). The user dir is fresh: only
  `port_settings.ini` and `imgui.ini` are copied (`--saves` adds `USA/`, `savestates/`). Mods
  default to the real user's (read-only); `--mods none` is an empty folder. On a startup crash
  it prints the log tail and the symbolized crash, and leaves nothing running.
- `cmd <name> <cmd>... | -f script.txt`: replies printed, exit 1 if any command failed, 2 if the game is gone.
- `shot <name> <out.png> [cmd...] [--crop x,y,w,h] [--settle FRAMES]`
- `ab <name> <prefix> --a "cmd;cmd" --b "cmd;cmd" [--settle 30] [--no-hold]`: writes
  `<prefix>-{a,b,diff,ab}.png` and the diff line. Sends `hold 1` first (ticks frozen) so only
  the toggle differs; run `cmd <name> 'hold 0'` afterwards to resume.
- `diff a.png b.png [--out heat.png] [--fail-above MAD]`: `mad= psnr= changed=% bbox=x,y,w,h luma_a= luma_b=`; exit 3 above the threshold.
- `sheet out.png img... [--cols N] [--labels a,b] [--width 480]`
- `log <name> [-n 40] [--grep RE] [--raw]`: tail of `game.log`, dropping `MP frame|prompt |mpstream` lines (constant `NOISE`).
- `crash [<name>|<logfile>] [--binary PATH]`: symbolizes the last `port: crashed:` block with one `addr2line` call (`#N func file:line`); warns if the log's `port: build` differs from `<binary> --version`. For Android pass the matching `libmain.so` with `--binary`.
- `ls` (alive/dead, display, port, build, room, uptime; reaps Xvfb of dead runs), `stop <name>|--all` (sends `quit`, then SIGTERM/SIGKILL on the recorded pids only; reports a crash found in the log), `clean [--keep 5]` (deletes dead run dirs under `build/rig/` only).

## Recipes

**A/B a render toggle**

    $M start r --room <MLVL:MREA>
    $M ab r /tmp/bloom --a 'roomenv bloom off' --b 'roomenv bloom on'
    $M sheet /tmp/s.png /tmp/bloom-a.png /tmp/bloom-b.png --labels off,on

Read `-ab.png` and the diff line; `changed=0.00%` means the toggle did nothing in this view.
Move the camera first with `freecam pos|look` or `warp` for a view where it matters.

**Bisect a crash**

    $M start c --build old-build --env MP_CRASH_TEST=segv   # forced crash, to check the symbols
    $M start c --room <id> --gdb --replace                  # real backtraces land in game.log
    $M crash c

Run the same `--room` against two builds (`--build port-gcc` vs `--build wt-x`) and compare.
Symbols only resolve against the binary that crashed: check the build warning.

**Check a Remastered room**: `$M start r --room <MLVL:MREA>`, then
`$M cmd r roomgeo 'roomenv info'` and `$M shot r /tmp/n.png 'view normal'` (also `albedo`, `rough`,
`metal`, `ao`, `glow`; `view off` resets). `doctor` first: a stale `.import-version` means
the install needs a re-import. Use `--mods DIR` to test a mod build without touching the real one.

**Two builds side by side**: start `a --build port-gcc` and `b --build <other>` with the same
`--room`, then `shot` both with the same commands and `diff` / `sheet` them.

## Limits

- Boot is `MP_BOOT_WORLD` only (works on any build); `MP_SMOKE_*` needs `build/smoke-gcc`
  (`--build smoke-gcc --env MP_SMOKE_WORLD=...`).
- `ab` freezes ticks, so animated effects stay put; use `--no-hold` to compare live frames
  (expect motion noise in `diff`).
- No built-in way to wait for "room loaded": use `--room` (which waits for the console) plus
  `cmd r 'wait 120'`.

## See also

`tools/mpcon.py` (the plain console client, also usable against a real game),
`docs/NATIVE_PORT.md` (full console command list, env variables),
`build/mpr/re.sh` + `build/mpr/TOOLS.md` (local Remastered reverse-engineering toolkit).
