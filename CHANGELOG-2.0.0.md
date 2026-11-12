Metroid Prime in VR, running natively on the Valve Steam Frame, with no Dolphin underneath. PrimedFrame 2.0 is a port of PrimedGun 2.0 by Nobbie and iChris4, built on the PrimeDecomp decompilation, with frame pacing written for SteamVR. It replaces PrimedFrame 1.0, which ran PrimedGun's Dolphin build. You need your own Metroid Prime disc image; no game data is included.

### The game on the Frame

- A native Linux Arm64 build. The game code is the decompilation, drawn by Aurora instead of an emulator.
- The game starts straight into VR once your disc is set.
- A launcher for your disc image, with **OPEN LAUNCHER** on the VR menu's CONFIG tab to bring it back.
- Works on SteamOS Stable, Beta and Preview.

### Smoothness

- 90 Hz, where 1.0 ran 120 Hz with the game at 60 frames a second. The game now steps once per refresh, so the world and the arm cannon move at the display's rate and nothing you follow with your eyes doubles.
- Both eyes sit in one image, and each pair reaches SteamVR only once the GPU has finished it, so the two eyes never come from different frames.
- Each frame ends in step with SteamVR's compositor, so the pacing is the same on every launch.
- Both eyes are drawn in one pass instead of two.
- In heavy scenes the game slows down slightly instead of catching up in jumps.

### Performance

Both 2.0 builds used the same SteamVR settings. They ran in the Space Pirate Frigate (the first level, with some of the game's highest polygon counts: a good benchmark). PrimedGun 2.0 is unmodified upstream. PrimedFrame 1.0 ran in Dolphin.

| | PrimedFrame 2.0 | PrimedGun 2.0 | PrimedFrame 1.0 |
|---|---|---|---|
| Average | 89.9 of 90 fps | 80.4 of 90 fps | 59.9 of 60 fps |
| Time at full rate | 94% | 56% | 92% |
| Slowest 1% of seconds | 85 fps | 50 fps | 48 fps |
| Missed frames | 1 in 1,000 | 1 in 13 | 1 in 94 |

### The picture

- A crisp HUD. Valve's foveation layer, which blurred it at a distance, is off.
- Eyes of 1728 pixels, set for SteamVR at every launch along with 90 Hz and no motion smoothing.

### Controls

- A controller page for the Steam Frame controllers in the VR menu, with the D-pad, View, Menu and the shoulder buttons named as they are on the Frame.
- **Auto Fire**, new: press the right shoulder to switch it on, and a held right trigger fires the beam repeatedly at its own rate, with no charging. It starts off at every launch, and a message shows when you switch it. Set **AUTO FIRE BUTTON** on the VR menu's CONTROL tab to OFF to free the shoulder.
- The HUD offset settings in the VR menu move the HUD, and the scan window moves with it.

### Saves

- **Bring Over PrimedFrame 1.0 Save**: the launcher offers to bring over your 1.0 memory card and settings the first time it opens. The Setup tab's **Transfer PrimedGun Memory Card / Settings** does it later.

### Updates

- Install and update from the Releases page, or with Frametanium or FrameDrop. The launcher's **Check for Updates...** shows the newest release and opens the Releases page.

### From PrimedGun 2.0

The VR menu, the launcher tabs, calibration and the rest of the settings are PrimedGun 2.0's. See the Credits in the README.

## Download

- `primedframe-2.0.0-linux-arm64.tar.gz`
- `primedframe-2.0.0-framedrop-linux-arm64.zip` and `primedframe.framedrop.json`, for [FrameDrop](https://framedropvr.com)
- `SHA256SUMS`

## Install

Unpack the tarball on the Frame and run its `install.sh`, or install with Frametanium or FrameDrop. The [README](https://github.com/simplysanatori/PrimedFrame/blob/main/README.md) has the steps. Then launch **PrimedFrame** from your library and pick your disc image in the launcher.

## Upgrading from 1.0

Install 2.0 the same way. It reuses your **PrimedFrame** Steam entry and keeps the 1.0 program folder as `~/Games/PrimedFrame/app.bak-<date>`. Your 1.0 saves and settings in `~/.local/share/PrimedFrame/user` are not touched; 2.0 keeps its own in `~/.local/share/PrimedFrame/v2`.

1. Launch **PrimedFrame**. The launcher opens.
2. Press **Select Game...** and pick your disc image first, so the right region's card is used.
3. When it asks "A PrimedFrame 1.0 save was found.", press **Bring Over Save**.

If you skipped it, use **Transfer PrimedGun Memory Card / Settings** on the Setup tab. Save states are not transferred: 1.0 save states are Dolphin's. Save in the game before you move to 2.0. The Dolphin version stays available on the [`legacy` branch](https://github.com/simplysanatori/PrimedFrame/tree/legacy).

## Known

- In a few of the heaviest scenes the game can stutter for a moment.
- Some room loads still cause a short hitch.

## Reporting problems

Open an issue with what you did, what you expected and what happened, and the archive from `~/Games/PrimedFrame/app/collect-logs.sh`.

## Credits

Nobbie and iChris4 for [PrimedGun 2.0](https://github.com/iChris4/MetroidPrimePortVR), the [PrimeDecomp](https://github.com/PrimeDecomp/prime) project, [Odran's MetroidPrimePort](https://github.com/Odrannnn/MetroidPrimePort) and [Aurora](https://github.com/encounter/aurora). The README has the full list.
