# PrimedFrame: notes for developers and AI agents

This file holds the technical detail moved out of the README.

Related documents:

- [docs/TECHNICAL_NOTES.md](docs/TECHNICAL_NOTES.md): how the build and the wrapper work, what was learned about SteamVR on the Frame, pacing, build from source, release.
- [frame/DEV.md](frame/DEV.md): build flags, the headless test setup, the statistics line.
- [frame/PACING.md](frame/PACING.md): the full pacing record with headset sessions and numbers.
- [frame/UNTESTED.md](frame/UNTESTED.md): what is built but not tested.
- [BUILDING-STEAMOS.md](BUILDING-STEAMOS.md): upstream PrimedGun's SteamOS (x86, desktop Qt) build workflow, contributed by josethevrtech. Not the PrimedFrame build.
- [docs/USER_GUIDE.md](docs/USER_GUIDE.md): user-facing settings, test switches, performance numbers, files, troubleshooting.
- [docs/CHANGELOG.md](docs/CHANGELOG.md).

Most other files in `docs/` and the `Building-*` files are Dolphin's or PrimedGun's upstream docs. `frame/release.sh` packs `README.md` into the release tarball and `frame/install.sh` copies it into the app folder, so the README must stay valid as a standalone file.

## Background

PrimedFrame is a fork of Nobbie248's [PrimedGun](https://github.com/Nobbie248/PrimedGun), the Dolphin based build that plays Metroid Prime in VR. PrimedGun has no Linux Arm64 build, so before this I ran its PC build on the Frame inside a Linux container with the Frame's graphics driver mounted in. PrimedFrame is the same game code built as a plain Linux Arm64 program with no window of its own, tuned to run in the Frame's own SteamVR. You do not need a container, Lepton, an APK or a PC.

I wanted PrimedGun on my Frame without a container, so I took Nobbie's code and built it natively for SteamVR. The container setup worked, but big rooms ran at 20 to 30 frames a second and the right eye ghosted. The work after the native build was pacing written for how SteamVR actually behaves, and finding that the Frame's GPU driver was doing the stereo drawing once per tile. It started as a project for my own headset. It ended up good enough to share.

Developer notes and what was learned about SteamVR on the Frame are in [frame/DEV.md](frame/DEV.md), [frame/PACING.md](frame/PACING.md) and the [technical notes](docs/TECHNICAL_NOTES.md).

## Status

Version 1.0.0 is the first release. It has been played on one Steam Frame (SteamOS 0.4.5, build 20261007.6125817) with the US disc.

Seen working in the headset: starting from the Steam library into the title screen, loading and saving the memory card save, PrimedGun's controls under SteamVR's automatic binding, the in-VR menu and its settings kept between launches, 120 Hz with the game at 60, 3x internal resolution with 2160 pixel eye images, heavy rooms at 60 frames a second, no doubled images, a steady helmet HUD. [frame/UNTESTED.md](frame/UNTESTED.md) lists what is built but not tested.

Known issues are listed in [docs/USER_GUIDE.md](docs/USER_GUIDE.md#known-issues).

## What this adds to PrimedGun

PrimedGun runs on Windows and on the Meta Quest. PrimedFrame changes the platform side and the frame pacing, and leaves the game side as it is.

Against the container setup I ran before:

| | Container setup | PrimedFrame |
|---|---|---|
| How it runs | PC build in a Linux container with the Frame's graphics driver mounted in | native Linux Arm64 build, started from the Steam library |
| Internal resolution | 2x (1280x1056 per eye) | 3x (1920x1584 per eye) |
| Eye images | 1728 per eye | 2160 per eye, the panel's native size |
| Display | 90 Hz with SteamVR's defaults | 120 Hz, SteamVR paces the game at 60 |
| Big rooms | 20 to 30 frames a second | 60 frames a second |
| Right eye | ghosted unless the GPU was drained every frame, which cost more frame rate | no doubled images |
| GPU texture decoding | off, after Adreno GPU faults | off, kept |
| In-VR menu settings | saved by the desktop window | saved by the game itself, there is no window |

The heaviest room I know averaged 59.6 frames a second in the headset session logs. The container setup ran it at 20 to 30.

Everything that was changed or added for the Frame, by area:

**The build**

- A native Linux Arm64 build of Dolphin's NoGUI program with PrimedGun's VR code: no Qt, no desktop window, Vulkan on SteamVR's own device, the OpenXR loader built in. The rest are stock SteamOS libraries.
- A relocatable layout: `Sys/` and `assets/` are looked up next to the program, the user folder is given with `-u`, so an install is a folder copy and nothing is written outside the folders under [Files](docs/USER_GUIDE.md#files).
- The desktop window is not drawn or presented while the VR session runs. Under the X11 platform Steam gives the game, presenting it cost GPU time and hid pacing errors.
- The shader cache works. The no-window build never created its cache folder, so every launch recompiled every shader. Now only the first launch stutters.
- The in-VR menu's settings are saved. The no-window build had no way to keep them (PrimedGun keeps them through the desktop window's settings), so every launch reset the menu. They now live in `Config/PrimedGun.ini` and are written when the menu closes, when you press Save in it, and at exit. The Save and Exit Game buttons work.

**The GPU**

- Direct rendering on the Frame's GPU. The launcher sets `TU_DEBUG=sysmem` for the Turnip driver. In the driver's default tiled mode every draw runs once per tile, and PrimedGun's stereo geometry shader draws both eyes in one pass, so the heaviest room's 1250-draw pass ran nine times. Rendering straight to memory cut the GPU time per frame in that room from 19 ms to 6 ms at the old settings (measured headless on the Frame). That is what paid for 3x and 2160.
- 3x internal resolution (1920x1584 per eye) and 2160 pixel eye images as the defaults, chosen from a resolution table measured headless (see [Performance](docs/USER_GUIDE.md#performance)).
- The eye images are no longer cleared before the game's image is blitted into them. The blit covers the whole image; the clear was 0.6 ms per frame at 1728 and more at 2160.
- The GPU gets the game's drawing every 100 draws instead of all at the end of the frame, so it works while the game is still emulating the frame instead of starting at the present.
- GPU texture decoding stays off. With it on, the Frame's GPU reads freed memory and the kernel logs a GPU fault that takes SteamVR down.
- Hi-res textures, background shader compilation, immediate XFB and the rest of the graphics settings are in the shipped ini files, with the values I run.

**The pacing**

- No GPU drain. Eye images go to SteamVR only after a fence shows the GPU has finished them. SteamVR shows a Vulkan image the moment it is released, finished or not, which is where the container setup's ghosting in the right eye came from; the container setup fixed it by draining the GPU every frame, which serialised the CPU, the GPU and the compositor. A releaser thread waits for the fence, so the emulation thread keeps going.
- One SteamVR frame cycle per game frame. The game runs at 60 frames a second and the display at 120. Each game frame runs in its own `xrWaitFrame` cycle: wait, begin, locate the head once, draw both eyes from exactly those poses, release after the fence, end the frame with the same display time and poses. SteamVR shows the frame on two refreshes itself. The variant with a pacing thread that ended a frame on every refresh, repeating the last pair, showed doubled images in the headset in every form I tried and is kept as a switch only.
- One landing per room. The only thing that decides whether motion is even is which refresh each frame is shown on. Each frame is planned at its `xrWaitFrame` to land on the refresh after the previous frame's, whatever refresh SteamVR hands it. A frame handed an earlier refresh is held just past the on-time limit, with its poses located for the refresh it lands on; a frame lands later only when it cannot make it. The light rooms settle on their shortest landing, the heaviest rooms land every frame one period late, constant, instead of flipping between the two.
- Prediction for the refresh the frame really appears on. Head and controller poses are located for that refresh, not for the display time `xrWaitFrame` first returned. In the headset this was less lag than without, in an A/B of two launches.
- The next frame's `xrWaitFrame` overlaps the last frame's end, so the game does not wait for SteamVR's hand-out of the next refresh before it starts emulating.
- SteamVR's own wake-up timing is measured from its blocking `xrWaitFrame` calls only, on `CLOCK_MONOTONIC_RAW`, which is the clock SteamVR's OpenXR times are on. An estimate that included the early hand-outs was a period too long and shook the HUD.
- Exactly 60 Hz. The GameCube's video timing is 59.94 Hz. With `VIOverclock = 1.001001` the game runs at exactly 60, 0.1 % faster than the original, so frames stop drifting against SteamVR's 60 Hz slots, which cost a late frame every 16 seconds.
- SteamVR's per-app settings for the game, set at every launch because SteamVR drops them now and then: 120 Hz, one frame to throttle (SteamVR paces the game at 60 and shows each frame twice), no extra prediction, motion smoothing off, 2160 pixel eye images.

**The launcher, the installer and the tools**

- A launcher for the Frame. The Steam entry runs the game through it. It keeps one folder per session with the launch log, the game's log, SteamVR's compositor timing and a report, applies the per-app settings and reads them back, refuses to start with a clear line in the log when the disc image is missing (there is no window to ask in), and takes one launch of test settings from `session.env`.
- An installer that unpacks the release, seeds the settings where files are missing, finds your disc image, adds the Steam entry with the VR flag and the library art (capsule, cover, hero, logo and icon), or updates the entry if it is there.
- A statistics line once a second in the game's log (`XRSTATS`, with the frame rate, GPU time, late frames, the landing plan's state and more), a per-frame line if asked, a logger for SteamVR's compositor counters, and a script that puts both into one table per session.
- Tools to collect masked logs for a bug report and to remove the program again.
- A privacy check that runs on my additions, on the history after upstream and on the release tarball before anything is published.
- A headless test setup: a mock OpenXR runtime with SteamVR's hand-out behaviour, a stamp check that proves each frame's poses match its display time, and a positive control that fails when an image is released before the GPU finished it. Everything in the pacing was proven there before a headset session.

## Installer internals

The installer copies the program to `~/Games/PrimedFrame/app`, puts default settings in `~/.local/share/PrimedFrame` where files are missing, remembers the path of your disc image, adds **PrimedFrame** to your Steam library as a VR shortcut with its library art, or updates the entry if it is there, and sets SteamVR's per-app settings for it. It talks to Steam through its local debugging port, which SteamOS has on by default. It changes nothing outside the folders under [Files](docs/USER_GUIDE.md#files), the shortcut and those settings. It never writes into a SteamVR file; the launcher sets the per-app settings through SteamVR's own `vrcmd`.

Steam must be running on the Frame while you install, so the installer can add the library entry.

### FrameDrop

The release has a `primedframe-<version>-framedrop-linux-arm64.zip` and a `primedframe.framedrop.json` for it. FrameDrop unpacks the zip on the Frame and adds its own library entry, which runs the `run.sh` in the zip. The first time it runs, and again after an update, `run.sh` runs `install.sh` with the Steam shortcut step turned off, so you get FrameDrop's entry and no second one from the installer, and then starts PrimedFrame through its launch wrapper.

## The launcher at each launch

At every launch the launcher does this, and `sessions/latest/launch.log` shows each step:

1. Makes a session folder under `~/.local/share/PrimedFrame/sessions/` and points `sessions/latest` at it.
2. Reads `session.env` if it exists, for one launch of test settings, and renames it `session.env.used`.
3. Reads the path of your disc image from the `rom` file and refuses to start, with `REFUSING TO START` in the log and exit status 66, if the file is not there.
4. Sets SteamVR's per-app settings for the Steam entry through `vrcmd` and reads them back from SteamVR's settings file:

   | SteamVR setting | Value | Why |
   |---|---|---|
   | Refresh rate | 120 Hz | The game runs at 60 frames a second. On 120 Hz each frame is shown exactly twice. On 90 Hz, 60 frames a second cannot be shown evenly. |
   | Frames to throttle | 1 | SteamVR paces the game at half the refresh, 60, and shows each frame on two refreshes. Without it SteamVR expects a new frame every refresh and the in-between refresh is mishandled. |
   | Additional frames to predict | 0 | The game predicts the poses for the right refresh itself. |
   | Motion smoothing | off | SteamVR did not apply it to the game in my sessions even when it was allowed; off keeps the cadence in the game's hands. |
   | Resolution | 2160 pixels per eye | The panel's native size. SteamVR's 100 % on the Frame is 1728. |

5. Starts a logger for SteamVR's compositor timing, once a second.
6. Sets `TU_DEBUG=sysmem` for the Turnip driver (direct rendering) and runs the game with the user folder, the X11 platform, Vulkan and your disc image.
7. When the game exits, stops the logger, copies the game's log and a stall file if the game wrote one, writes `report.txt`, and keeps the last 30 session folders.

## Performance internals

The measured numbers are in [docs/USER_GUIDE.md](docs/USER_GUIDE.md#performance). Where the time went, and what was done about it:

- **The GPU.** The whole frame was one render pass of about 1250 draws, and Turnip's tiled mode ran it once per tile, nine times, because PrimedGun's stereo geometry shader puts both eyes into one pass. Direct rendering (`TU_DEBUG=sysmem`) runs it once: 19 ms became 6 ms at 2x and 1728. The eye clears were pure waste and are gone, and the GPU gets the frame's draws in slices of 100 instead of all at the present, so it is busy while the CPU still emulates. The heavy-room GPU tail that used to stall the next frame is overlapped with it.
- **The pacing.** SteamVR shows an image the moment it is released. The container setup drained the GPU every frame to keep the right eye clean, which serialised everything; a fence does the same without the stall. Then the cadence: a loop that ended a frame on every refresh, repeating the last pair for the in-between refresh, doubled the picture in the headset, because SteamVR took each repeat as a new frame and the repeats carried poses a refresh old. One SteamVR cycle per game frame, with SteamVR told to pace the game at 60, is clean. The last piece was the landing plan: left to itself, SteamVR's hand-out of refreshes depended on how long the game held each frame, and the cadence flipped between on-time and a period late several times a launch, which was felt as lag spikes. Planning each frame's landing refresh from the last one's gives one steady cadence per room.
- **Prediction.** With each frame landing on a known refresh, the head and controller poses are predicted for that refresh. Before, they were predicted for the display time SteamVR first offered, which for a frame landing a period later was a period short, felt as lag, and when the estimate was wrong by a period, as a shaking HUD.
- **60 Hz.** The GameCube's 59.94 Hz drifted a frame against SteamVR's 60 Hz slots every 16 seconds, a late frame each time. The video timing at exactly 60 removed them.
- **Shaders.** The no-window build never created its shader cache folder. With it created, the first launch compiles and later launches load.

What did not help, so you know: Valve's `fdm_injection` Vulkan layer is in the chain either way and was not the difference in any comparison; SteamVR's motion smoothing was never applied to the game in my sessions, allowed or not; PrimedGun's opcode replay (drawing each refresh again from a fresh pose) is not wired in this tree and does nothing here.

## Build from source

See the [technical notes](docs/TECHNICAL_NOTES.md) and [frame/DEV.md](frame/DEV.md). `frame/build.sh` builds on the Frame itself with the tools SteamOS has, in about 16 minutes the first time. `frame/install.sh --dev` installs a build as a separate **PrimedFrame (dev)** entry. `frame/tools/mockrun.sh` runs the game headless under the mock runtime. `frame/tools/privacy-check.sh` checks my own additions, the history after upstream and a release tarball for personal data before publishing.

## How this was made

I started this project and used AI (Claude Code with Fable 5.1, Opus 5.5 and Sonnet 5.5) to speed up development. I write, review and fix code by hand all the way through, keep the AI's work checked and reined in, and aim to keep the port as lightweight as possible. I test it on my own Steam Frame. PrimedGun's README says Codex was used to assist its development, so the code underneath is AI assisted too.
