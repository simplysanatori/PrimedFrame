# How the port works

Notes for people working on the port. For playing, see the [README](../README.md) and [CONTROLS.md](CONTROLS.md).

## What it is

PrimedFrame 2.0 is [PrimedGun 2.0](https://github.com/iChris4/MetroidPrimePortVR) (Nobbie and iChris4) built for Linux Arm64 and the Steam Frame. PrimedGun 2.0 is a native port of Metroid Prime: the PrimeDecomp decompilation of the game, Odran's MetroidPrimePort, and Aurora as the GX, input and platform layer over SDL3 and WebGPU (Dawn on Vulkan). PrimedFrame adds the Frame's graphics path, its frame pacing, its controllers and the packaging. The game code reads your disc image at run time; no game data is shipped.

Two programs run:

- `metroid_prime_port`: the game.
- `PrimedGun`: the Qt launcher. With `--frame-start` it starts the game in place when a valid disc is saved, and opens its window otherwise. Play starts the game in place too, so there is one process either way.

`primedframe.sh` is what the Steam entry runs. It sets the data folder, SteamVR's per-app keys and the environment, starts `PrimedGun --frame-start` and writes a session log.

## Folders

| Path | Holds |
|---|---|
| `~/Games/PrimedFrame/app` | The program folder: both programs, `lib/` and `plugins/` (Qt and the libraries SteamOS does not promise to keep), `art/`, `licenses/` and the scripts. Replaced as a whole on update |
| `~/.local/share/PrimedFrame/v2` | Your data: `port_settings.ini`, `USA/Card A` (memory card), `savestates`, `mods`, `user_textures`, the shader and Dawn caches and `sessions/` |
| `~/.local/share/PrimedFrame/user` | PrimedFrame 1.0's data. Never touched |
| `~/Games/PrimedFrame/app.bak-<date>` | The 1.0 program folder, kept by `install.sh` |

Sessions: `sessions/<time>/` holds `launch.log`, `steamvr-keys.txt`, `stdout.log`, `stderr.log`, `gpu-mhz.tsv` and the game's log. The 30 newest are kept. `collect-logs.sh` packs the newest one with personal details masked.

## Stereo and frame pacing

On the Steam Frame, SteamVR shows each eye's latest released image when it reaches that eye. With one swapchain per eye, the two eyes can come from different frames, which shows as double vision. The port avoids that:

- **Packed stereo.** Both eyes go side by side into one swapchain image, at most 3456 pixels wide (SteamVR downsamples wider images): 1728 per eye. One acquire and one release per frame.
- **Release fence.** An eye set is released only after the GPU has finished the work that wrote it. SteamVR shows a released image at once, finished or not.
- **One pass.** Both eyes are drawn with Vulkan multiview. Frames that show only the eyes leave out the flat image and the scene copy.
- **Poses for the refresh the frame lands on.** The poses are located one display period past the last compositor frame, and the frame is ended with exactly the display time they were located for.
- **End phase.** Each eye set is released and its frame ended at a phase of SteamVR's compositor refresh, not right after `xrWaitFrame` returns. The game reads the compositor's frame timings itself, in process, with no helper program. Without timings it falls back to the delay it last used at this refresh rate.
- **Simulation locked to the display.** In the headset the game takes one simulation step per new frame at up to 96 Hz, as 1.0 locked Dolphin to the display. A frame that misses its refresh still takes one step: the game slows down slightly instead of catching up with double steps, which cost CPU time when it is already short and show as jumps. It never runs slower than 75% of real time in any second; past that, late frames catch up as before. At 120 Hz and above it takes one step of two periods per two frames.
- **Overlap.** When the next frame is due before this one finishes, its poses are located and the game's next step runs while the GPU finishes this one.
- **Stalls.** While the game hands over no eyes for two display periods (a room load), the last layer is repeated at every refresh.
- **Async seal.** The game thread ends a headset frame without waiting for the GX FIFO processor to work through it. The processor finishes the frame (recording and queueing it) on its own thread while the game starts its next step.
- **Loading budget.** Background loading after a frame is sealed gets at most 2 ms per frame.
- **Foveation off.** Valve's foveation layer (`VK_LAYER_VALVE_fdm_injection`) is not loaded: `primedframe.sh` removes it from `VK_INSTANCE_LAYERS`, where Steam lists it explicitly, and the game sets `DISABLE_VULKAN_FDM_INJECTION_LAYER` before the Vulkan loader starts.

## Performance

All three runs are from the same Steam Frame. The first 10 seconds and the last 2 of each are left out.

- **PrimedFrame 2.0 and PrimedGun 2.0:** the Space Pirate Frigate, 90 Hz, 1728 pixels per eye, motion smoothing off, Valve's foveation layer off. PrimedGun 2.0 is the unmodified upstream build (2cdb27e75, the commit this port is based on). Both write the same `[xr-diag]` line each second: a new frame is an image the game handed to SteamVR, and a missed frame is a display refresh the compositor skipped.
- **PrimedFrame 1.0:** PrimedGun 1.0's Dolphin build, the game at 60 fps on a 120 Hz display, 2160 pixels per eye, in normal play (the area is not logged). The numbers come from its `XRSTATS` line each second: frames shown out of the game's 60.

| | PrimedFrame 2.0 | PrimedGun 2.0 | PrimedFrame 1.0 |
|---|---|---|---|
| Time measured | 635 s | 327 s | 644 s |
| Frame rate | 90 fps | 90 fps | 60 fps |
| Average | 89.9 | 80.4 | 59.9 |
| Seconds at full rate (no missed frame) | 94.1% | 56.0% | 91.6% |
| Slowest 1% of seconds | 85 | 50 | 48 |
| Slowest second | 79 | 43 | 0 (a load) |
| Missed frames | 55 (0.10%, 5 a minute) | 2189 (7.4%, 401 a minute) | 412 (1.1%, 38 a minute) |

SteamVR's compositor timings for the PrimedGun 2.0 run add 6215 mispresented frames and 5.7 refreshes on average from a frame's poses to its photons. A PrimedFrame 2.0 run in Tallon Overworld and the Chozo Ruins averaged 89.7 fps with 0.14% missed frames.

## SteamVR per-app keys

`primedframe.sh` sets these for the entry's `steam.app.<appid>` at every launch and reads each back, retrying for up to 20 seconds while SteamVR starts:

| Key | Value |
|---|---|
| `preferredRefreshRate` | 90 |
| `motionSmoothingOverride` | 2 (off) |
| `resolutionOverride` | 1728 |
| `framesToThrottle` | 0 |
| `additionalFramesToPredict` | 0 |

The result is in `steamvr-keys.txt` and `launch.log`.

## Environment

For `primedframe.sh`:

| Variable | Effect |
|---|---|
| `PRIMEDFRAME_HOME` | Data folder |
| `PRIMEDFRAME_HZ` | Refresh rate: 72, 80, 90, 96, 108, 120 or 144. Default 90 |
| `PRIMEDFRAME_EYE` | Render size per eye in pixels. Default 1728 |
| `PRIMEDFRAME_SMOOTHING=1` | SteamVR motion smoothing on |
| `PRIMEDFRAME_FDM=1` | Keep Valve's foveation layer |
| `PRIMEDFRAME_APPID` | The entry's appid, when not started from Steam |
| `PRIMEDFRAME_VR_WAIT` | Seconds to wait for SteamVR to take the keys. Default 20 |

`primedframe.sh --dry-run` prints the environment, the keys and the command, and starts nothing.

For `install.sh` and `uninstall.sh`: `PRIMEDFRAME_APP`, `PRIMEDFRAME_HOME`, `PRIMEDFRAME_SKIP_STEAM=1`, `PRIMEDFRAME_CEF_PORT`. `install.sh --dry-run` lists every file and Steam call without making them.

The game reads `PF2_*` switches for the pacing described above. On Linux the setup is the default; `0` turns a switch off: `PF2_STEREO_PACKED`, `PF2_RELEASE_FENCE`, `PF2_MULTIVIEW`, `PF2_MONO_SKIP`, `PF2_SCENE_COPY_SKIP`, `PF2_SIM_LOCK`, `PF2_SIM_SLOWDOWN` (or the lowest game speed in percent, 50 to 99; default 75), `PF2_OVERLAP`, `PF2_STALL_REPEAT`, `PF2_TIMING_READER`, `PF2_ASYNC_SEAL`, `PF2_IDLE_CAP_US` (or the loading budget in microseconds; default 2000). `PF2_MODEL_GEOMETRY_CACHE=1` turns on a per-model geometry cache that is off by default: explosions that spawn many new models stalled while each built its cache. `PF2_HZ` overrides the refresh rate the game asks for. The full list is in `platform/include/vr/pf2_switches.h`.

## Install

`install.sh` copies the unpacked release (or a tarball given as its argument) to `~/Games/PrimedFrame/app`, keeps a non-2.x app folder as `app.bak-<date>`, and talks to Steam through its debugging port (8080): it finds or adds the **PrimedFrame** entry, sets its VR flag, start folder and launch options, sets the library art and writes the SteamVR dock icon files. It never asks a question, so Frametanium can run it. If Steam is not reachable it installs the files and says the entry was skipped. `install.sh --remove-dev` also removes a leftover "PrimedFrame 2.0 (dev)" entry.

`uninstall.sh` removes the entry and the program folder. `--purge` also removes `~/.local/share/PrimedFrame/v2`.

## Portable build

SteamOS Stable has glibc 2.39; Beta and Preview have 2.43. The release is built against a glibc 2.39 tree, so one build starts on all three. SDL3 is built from source and linked in, a patched Dawn is linked in, and Qt 6, ICU, OpenSSL and the other libraries SteamOS does not promise to keep are packed in `lib/`. `steam-frame/release.sh` builds, strips, bundles and refuses to write a release unless the bundle checks pass against all four SteamOS builds' library lists and the audit shows no glibc symbol newer than 2.39.

## Build from source

The Frame build lives in `steam-frame/`: `build.sh` (needs the glibc 2.39 tree and the patched Dawn package that `tools/build-linux-dawn.sh` makes) and `release.sh`. For the rest, PrimedGun's `docs/NATIVE_PORT.md` is the reference.
