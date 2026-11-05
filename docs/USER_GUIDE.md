# PrimedFrame user guide

The full reference for PrimedFrame: disc image, install options, controls, settings, test switches, performance, files and troubleshooting. The short version is the [README](../README.md).

## Contents

- [What it does](#what-it-does)
- [Your disc image](#your-disc-image)
- [Install options](#install-options)
- [Playing](#playing)
- [Controls](#controls)
- [Settings](#settings)
- [One launch of test settings](#one-launch-of-test-settings)
- [Performance](#performance)
- [Files](#files)
- [Known issues](#known-issues)
- [Troubleshooting](#troubleshooting)
- [Reporting a problem](#reporting-a-problem)
- [Support and credits in full](#support-and-credits-in-full)

## What it does

- Plays Metroid Prime with PrimedGun's VR controls, visor and arm cannon, launched from the Steam library into the Frame's SteamVR.
- Renders at 3x internal resolution (1920x1584 per eye) into eye images of 2160 pixels per eye, the size of the Frame's panel.
- Runs the 120 Hz display with SteamVR pacing the game at its own 60 frames a second, two refreshes for each game frame.
- Holds 60 frames a second in heavy rooms.
- Keeps your settings, the in-VR menu's settings, your memory card and save states in one folder under your home.

You do not need a container, Lepton, an APK or a PC.

## Your disc image

You need your own Metroid Prime disc image, the US release (game ID GM8E01, revision 0, the version PrimedGun supports), as `.rvz`, `.iso` or `.gcm`. No disc image is provided. No game data is in this repository or its releases.

1. **Dump your own disc.** Follow Dolphin's guide, [Ripping Games](https://dolphin-emu.org/docs/guides/ripping-games/). A Wii with CleanRip reads GameCube discs. A GameCube-compatible disc drive on a PC does not read them.
2. **Shrink it (optional).** Open the image in Dolphin on a PC, right click the game and choose Convert File to make an `.rvz`. It is about 1 GB instead of 1.4.
3. **Check it is the right one.** In Dolphin, the game's properties show the ID and the revision. They must read GM8E01 and revision 0. A different region or revision does not work with PrimedGun.
4. **Put it on the Frame.** Copy it with a USB drive opened from the desktop, or with `scp` over SSH from a PC, into `~/Documents/Roms/gc`. The installer searches that folder (and one level of subfolders) for a `.rvz`, `.iso` or `.gcm` with "Metroid Prime" in its name. If it finds several, it asks which one, or without a terminal asks you to run it with `--rom`. Anywhere else works too: give the path with `--rom`.

## Install options

### Change the disc image

To change the disc image later, run `install.sh --rom PATH` again, or edit the one line in `~/.local/share/PrimedFrame/rom`.

### Update

To update, download the new tarball and run step 2 of the README's Install again with the new file name. Your settings, memory card and save states stay where they are; the installer only adds files that are missing.

### Bring over an existing save

To bring over an existing save, copy your memory card and save states into `~/.local/share/PrimedFrame/user` (`GC/` and `StateSaves/`) from a PC Dolphin or PrimedGun install, before the first launch. PrimedGun advises against carrying save states from one build to another. To bring over the in-VR menu settings of a PrimedGun desktop build, paste the `[primedgun]` section of its `Qt.ini` into `user/Config/PrimedGun.ini`.

### Install without Steam

With `PRIMEDFRAME_SKIP_STEAM=1` the installer installs without the library entry.

### Remove

To remove it, run `~/Games/PrimedFrame/app/tools/uninstall.sh` with Steam running. It removes the Steam entry, its library art and the program; your data folder stays unless you add `--purge`, which deletes `~/.local/share/PrimedFrame` (saves, states, settings, logs) with no undo.

### Install with Frametanium

If you use [Frametanium](https://github.com/Cogential/frametanium), add `simplysanatori/PrimedFrame` there. It downloads the release, runs its `install.sh` and updates it when I publish a new version. Put your Metroid Prime (USA) disc image in `~/Documents/Roms/gc` before the first install and the installer finds it, or write its path as the only line of `~/.local/share/PrimedFrame/rom`. Later installs keep the path.

### Install with FrameDrop

[FrameDrop](https://framedropvr.com) can install this too: [Install with FrameDrop](https://framedropvr.com/install?manifest=https%3A%2F%2Fgithub.com%2Fsimplysanatori%2FPrimedFrame%2Freleases%2Flatest%2Fdownload%2Fprimedframe.framedrop.json). FrameDrop adds its own library entry, so you get no second one from the installer. The installer finds your Metroid Prime disc image in `~/Documents/Roms/gc`, or you can write its path into `~/.local/share/PrimedFrame/rom` before the first start.

## Playing

Start SteamVR (put the headset on), open the Steam library and launch **PrimedFrame**. The entry has the VR flag, so SteamVR starts it as a VR app. There is no desktop window: the game starts straight into the headset, on the title screen, after a few seconds of black while SteamVR hands it the display. The first launch stutters while the shaders compile; later launches do not.

In the game, PrimedGun's own notes apply: click the right stick to set your height, click the left thumbstick to open or close the in-headset settings menu, stay in the centre of your play space and face forward, and use Exit Game at the top right of the menu (press it twice) to leave the game cleanly. Exit Game writes the shader caches and the menu's settings. Quitting from the SteamVR dashboard also works; the launcher still writes the session report.

Save in the game as usual. The save goes to the memory card in `~/.local/share/PrimedFrame/user/GC`.

## Controls

The controls are PrimedGun's, unchanged. It reads the controllers through OpenXR and SteamVR binds its actions to the Frame controllers automatically; I have not added a binding of my own. The buttons land on the Frame controllers as PrimedGun's layout picture shows: it is [`assets/controller layout.png`](../assets/controller%20layout.png), drawn for Touch controllers. Click the right stick to set your height; click the left thumbstick for the in-VR menu.

The in-VR menu has a Save button and an Exit Game button (top right, press it twice).

## Settings

Settings are the ini files in `~/.local/share/PrimedFrame/user`, and PrimedGun's in-VR menu (click the left thumbstick):

- `Config/GFX.ini`: graphics, and the `[VR]` section with the PrimedGun options. The values that matter on the Frame: `ImmediateXFBEnable = True` (off, the eye images are a frame older than their poses), `LockHeadPosePerFrame = False` (PrimedGun's default for Vulkan), `DisableCPUCull = True`, `OpcodeReplay = 0`, `UnitsPerMeter = 1.5`, hi-res textures on.
- `GameSettings/GM8E01.ini`: overrides for this game: `InternalResolution = 3`, background shader compilation, and `EnableGPUTextureDecoding = False`. Leave that last one off: with it on, the Frame's GPU reads bad memory and takes SteamVR down.
- `Config/Dolphin.ini`: the Vulkan backend, the DSP thread and the 60 Hz video timing (`VIOverclock = 1.001001`).
- `Config/Logger.ini`: turns on the log file for the OpenXR and video categories, which carries the once a second statistics line.
- `Config/PrimedGun.ini`: the in-VR menu's settings, in a `[primedgun]` section. They are saved when the menu closes, when you press Save in it, and at exit. It ships with my HUD placement for the Frame (distance 1.15, size 1.0, 0.08 to the right and 0.02 up); everything else is PrimedGun's default until you change it in the menu.

## One launch of test settings

Put `NAME=value` lines in `~/.local/share/PrimedFrame/session.env`. The launcher reads it for the next launch only and renames it afterwards; `session.env.next`, if present, becomes the launch after's `session.env`, for two-launch comparisons. These are the switches, with the defaults the game ships with:

| Switch | Default | What it does |
|---|---|---|
| `PRIMEDFRAME_STATE` | unset | Start from a save state: a slot number 1 to 10, or a file path. Useful to test a given room. |
| `PRIMEDFRAME_EYE` | `2160` | Eye image size asked of SteamVR. `1728` is SteamVR's 100 % on the Frame. |
| `PRIMEDFRAME_REFRESH_HZ` | `120` | Refresh rate asked of SteamVR, 90 or 120. |
| `PRIMEDFRAME_THROTTLE` | `1` | SteamVR's frames to throttle for the game. |
| `PRIMEDFRAME_MOTION_SMOOTHING` | `2` | SteamVR's motion smoothing for the game: 0 global setting, 1 on, 2 off, 3 always on. |
| `PRIMEDFRAME_SYSMEM` | `1` | `0` leaves the Turnip driver in its tiled mode (slow here). |
| `PRIMEDFRAME_PACING` | `cycle` | The frame loop: `cycle` (one SteamVR cycle per game frame), `pipelined` (a pacing thread; doubled images in the headset), `drain` (the GPU drained every frame, the container setup's loop), `inline` (as drain, fence waited for before the release). |
| `PRIMEDFRAME_CYCLE_END` | `plan` | `plan`: one landing per room. `asap`: each frame ends as soon as its GPU work is done, for an A/B. |
| `PRIMEDFRAME_CYCLE_OVERLAP` | `1` | `0`: the next frame's `xrWaitFrame` waits for the last frame's end. |
| `PRIMEDFRAME_PREDICT_EXTRA_MS` | `auto` | How far past SteamVR's display time the poses are predicted: `auto` is one period for each period the frame lands late, a number is fixed milliseconds, `0` is off. |
| `PRIMEDFRAME_PREDICT_ONTIME_MS` | `19.5` | With `auto`, how long after SteamVR's wake-up a frame still counts as on time. |
| `PRIMEDFRAME_KICK_DRAWS` | `100` | The GPU gets the frame's drawing every this many draws; `0` only at the present. |
| `PRIMEDFRAME_EYE_CLEAR` | `0` | `1` clears the eye images before the blit, as upstream. |
| `PRIMEDFRAME_XR_CLOCK` | `raw` | The clock SteamVR's times are read on, `raw` or `steady`. |
| `PRIMEDFRAME_FDM_LAYER` | `1` | `0` turns off Valve's `fdm_injection` Vulkan layer for an A/B. |
| `PRIMEDFRAME_HUD_TEST` | `0` | `1` shows a white test ring on a composition layer of its own, head-locked, to compare with the HUD. |
| `PRIMEDFRAME_FRAME_LOG` | `1` | `0` turns off the per-frame line in the game's log (about 20 MB an hour). |
| `PRIMEDFRAME_VRTIMING` | `1` | `0`: no compositor timing log. |
| `PRIMEDFRAME_KEEP_SESSIONS` | `30` | Session folders kept. |
| `PRIMEDFRAME_WINDOW_PRESENT` | `0` | `1` presents the desktop window during the VR session, as upstream. |

PrimedGun's own `PRIMEDGUN_*` switches pass through too. The switches other than the defaults are built but not confirmed in the headset; [frame/UNTESTED.md](../frame/UNTESTED.md) says which.

## Performance

Numbers from my Frame, from the game's own log and SteamVR's compositor counters, at 120 Hz with 3x internal resolution and 2160 pixel eye images, unless stated. "Headless" means the mock OpenXR runtime on the Frame itself, with SteamVR idle, which gives the GPU and the cadence without a headset session.

| | PrimedFrame |
|---|---|
| Game frames a second, the heaviest room I know (the landing site with the HUD up) | 59.6 on average over a launch |
| Game frames a second, a light room | 59.2 on average over a launch |
| Refreshes per game frame | 2, constant: the compositor reused 93 to 97 % of its refreshes, which is each frame shown twice |
| Frames SteamVR had to reproject | 0 |
| Frames ended with poses other than the ones drawn | 0 |
| GPU per game frame, averaged over a session | about 5 ms, against the 16.7 ms a game frame has |
| GPU per game frame, the heaviest room, headless | 9 ms at 3x and 2160; 6 ms at 2x and 1728; 12 ms at 4x and 2160 |
| The same room, headless, in the driver's tiled mode at 2x and 1728 | 19 ms, 52 frames a second |
| Compositor GPU per refresh | about 1 ms |
| Head pose to display, light room, headless | 41 to 48 ms, depending on where in the period the frame starts |
| Head pose to display, the heaviest room, headless | about 50 ms, one period later than a light room |
| Late frames after the 60 Hz timing, headless | 0.04 a second, from 21.7 |
| First launch | stutters while the shaders compile; later launches start from the cache |

The resolution table (headless, the heaviest room, direct rendering): 2x and 1728 cost 6 ms of GPU per frame, 3x and 2160 cost 9 ms with the GPU at 77 C flat over six minutes, 4x and 2160 cost 12 ms with the GPU at 87 C and rising. 3x and 2160 is the default; 4x is about the panel's own resolution and works headless, but I have not run it in the headset for long.

Each session writes `sessions/latest/report.txt`, a table of the game's once a second statistics line and SteamVR's compositor counters, and the per-frame log holds every frame's timing. If your Frame is too slow for a heavy room, lower `InternalResolution` in `GameSettings/GM8E01.ini` to 2.

Where the time went, and what was done about it, is in [AGENTS.md](../AGENTS.md#performance-internals).

## Files

| What | Where |
|---|---|
| Program, launcher, tools, the seed settings, the library art | `~/Games/PrimedFrame/app` |
| Settings, memory card, save states, the in-VR menu's settings, shader caches | `~/.local/share/PrimedFrame/user` (`Config/`, `GameSettings/`, `GC/`, `StateSaves/`, `Cache/`, `Logs/`) |
| Path of your disc image | `~/.local/share/PrimedFrame/rom` (a text file, one line) |
| Logs | `~/.local/share/PrimedFrame/sessions/latest/` (`launch.log`, `dolphin.log`, `vrtiming.log`, `report.txt`, `stdout.log`) |
| One launch of test settings | `~/.local/share/PrimedFrame/session.env` and `session.env.next`, `NAME=value` lines |

Your memory card is `user/GC/MemoryCardA.USA.raw`. Save normally in the game.

## Known issues

- The game's world, enemies and the arm cannon move at 60 frames a second. On the 120 Hz display an object your eye follows can look doubled along its movement. Head movement is corrected at 120.
- The helmet HUD has a very light jitter left on fast head turns. The HUD is drawn head-locked into the same image as the world, and SteamVR turns that whole image to your head at display time; any prediction error moves the HUD. A HUD on a layer of its own would fix it; only a test ring exists.
- In the heaviest rooms the picture is one refresh period behind the light rooms, by the landing plan. It is constant, not a flicker.
- The first launch stutters while the shaders compile.
- There is a 75 ms hitch at the step from the ship flight intro into play. It is the game's own.

## Troubleshooting

- **`install.sh` says Steam's debugging port is not reachable.** Start Steam and run it again. With `PRIMEDFRAME_SKIP_STEAM=1` it installs without the library entry.
- **The game closes at once.** Read `sessions/latest/launch.log`. A missing disc image gives `REFUSING TO START` and exit status 66. Fix it with `install.sh --rom PATH`.
- **The game starts flat, on the desktop, not in the headset.** The Steam entry lost its VR flag. Run `install.sh` again; it sets the flag on the existing entry.
- **Motion is uneven, or the picture ghosts when you turn your head.** Check `launch.log` for the read-back line: the per-app SteamVR settings should be as asked (120 Hz, one frame to throttle, motion smoothing off). `report.txt` shows what the game and SteamVR's compositor did; `late` and `plan_breaks` in it should be near 0 a second.
- **Doubled images or uneven timing in a room.** Put `PRIMEDFRAME_PACING=drain` in `~/.local/share/PrimedFrame/session.env` for one launch. That is the older loop that drains the GPU every frame; it is slower but a useful comparison. `PRIMEDFRAME_CYCLE_END=asap` is the other comparison.
- **Slow or glitchy on direct rendering.** Put `PRIMEDFRAME_SYSMEM=0` in `session.env` for one launch to leave the driver tiled.
- **Too slow in a heavy room.** `InternalResolution = 2` in `GameSettings/GM8E01.ini`, or `PRIMEDFRAME_EYE=1728` in `session.env` for one launch.
- **The HUD shakes.** `wake_lead_ms` in `report.txt` should sit around 36 ms and `extra_frames` should be low in a light room. If not, send me the report.
- **The game froze.** If it wrote `PrimedGunFrameStall.txt`, the session folder has a copy.
- **The Steam overlay does not appear in the game.** Steam's overlay does not inject into this program. The `LD_PRELOAD` lines in `launch.log` come from that and are harmless.
- **A bad mapping on the Frame controllers.** Tell me which button and what it did; see [Reporting a problem](#reporting-a-problem).

## Reporting a problem

Run `~/Games/PrimedFrame/app/tools/collect-logs.sh`. It writes an archive of the latest session with addresses, user names and IDs masked. Read it before you attach it to an [issue](https://github.com/simplysanatori/PrimedFrame/issues) of the PrimedFrame repository, together with what you did, what you expected and what happened. Masking is best effort.

The archive is `~/primedframe-logs-<date-time>.tar.gz`. It holds the session's `launch.log`, `stdout.log`, `dolphin.log`, the stall file, `vrtiming.log`, `report.txt` and `session.env`, your `Dolphin.ini`, `GFX.ini` and `GM8E01.ini`, and a summary with the SteamOS version and the program's hash. Masked: IP and MAC addresses, UUIDs, hostnames, Wi-Fi network names, serial numbers, Steam IDs, email addresses, tokens, certificate and key blocks, and user names in `/home` paths.

## Support and credits in full

PrimedFrame exists because of the people who made PrimedGun and the VR builds of Dolphin it grew from. If you want to support someone, support them first:

- Nobbie made [PrimedGun](https://github.com/Nobbie248/PrimedGun). I could not find a donation page, so star and follow the project, report what you find there, and send improvements upstream.
- iChris4 made Dolphin ReduX, the VR build of Dolphin that PrimedGun is based on, and ported PrimedGun to the Quest. You can [sponsor iChris4 on GitHub](https://github.com/sponsors/iChris4).
- The Dolphin team builds the emulator all of this runs on. Their site, [dolphin-emu.org](https://dolphin-emu.org), is the place to follow and help them.

If you would like more Steam Frame ports, you can also [buy me a coffee](https://ko-fi.com/sana748653). That link is only for my Steam Frame work.

Credits:

- Built by [simplysanatori](https://github.com/simplysanatori).
- [PrimedGun](https://github.com/Nobbie248/PrimedGun) by Nobbie, and everyone PrimedGun thanks: iChris4, who made Dolphin ReduX for PC and Quest and ported PrimedGun to the Quest, the Metroid Prime modding community, the Metroid Prime decompilation project, Metaforce and PrimeHack, and the early testers. Its own README, kept as [README.upstream.md](../README.upstream.md), has the full list.
- iChris4's Dolphin ReduX and the old Dolphin Hydra build, whose Metroid Prime behaviour PrimedGun carries forward.
- Carl Kenner and the contributors of Dolphin VR, the first VR build of Dolphin, which started the line of VR work that later builds grew from.
- josethevrtech and [PrimedSteam](https://github.com/josethevrtech/PrimedSteam), who contributed PrimedGun's SteamOS build workflow.
- The Dolphin team, whose emulator all of this is built on.
- Valve, for the Steam Frame, SteamVR and the OpenXR runtime in it.

[THIRD-PARTY-NOTICES.md](../THIRD-PARTY-NOTICES.md) lists what my additions use and where it came from.

Metroid Prime and its logos are trademarks of Nintendo. This is an unofficial fan project. It is not affiliated with or endorsed by Nintendo, Valve, the Dolphin team or the PrimedGun author.

Dolphin, PrimedGun and my changes are free software under the GNU General Public License version 2 or later, as Dolphin is: see `COPYING` and the licence texts in `LICENSES/`. Files that say otherwise in their header keep their own licence. No Nintendo game data is included.
