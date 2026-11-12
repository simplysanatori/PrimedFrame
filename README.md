# PrimedFrame

Metroid Prime in VR, running natively on the Valve Steam Frame (SteamOS, Arm64).
Based on [PrimedGun 2.0](https://github.com/iChris4/MetroidPrimePortVR) by Nobbie and iChris4.

## Features

- Runs natively on the Frame, built from the Metroid Prime decomp project instead of Dolphin
- Smooth 90 Hz, with no double vision
- Sharp HUD: Valve's foveation layer is off
- Steam Frame controller layout, with Auto Fire on the right shoulder
- A launcher for your disc image, and for bringing over a 1.0 save

## How it works

The game runs standalone on the Frame and draws both eyes through SteamVR. You need to supply your own disc image.

## Performance

2.0 is PrimedFrame 2.0, upstream is unmodified PrimedGun 2.0, 1.0 is PrimedFrame 1.0 in Dolphin. Both 2.0 builds used the same SteamVR settings. They ran in the Space Pirate Frigate (the first level, with some of the game's highest polygon counts: a good benchmark).

- Average: 89.9 of 90 fps (2.0), 80.4 of 90 (upstream), 59.9 of 60 (1.0)
- Time at full rate: 94% (2.0), 56% (upstream), 92% (1.0)
- Slowest 1% of seconds: 85 fps (2.0), 50 (upstream), 48 (1.0)
- Missed frames: 1 in 1,000 (2.0), 1 in 13 (upstream), 1 in 94 (1.0)

Optimizations for the Steam Frame:

- Both eyes go out as one finished image
- Both eyes are drawn in one pass
- Each frame is timed to SteamVR's compositor
- The game's next step runs while the GPU finishes the last frame
- In heavy scenes the game slows slightly instead of skipping ahead
- Room loads keep showing the last frame

Details: [docs/TECHNICAL.md](docs/TECHNICAL.md#performance).

## Requirements

- A Steam Frame on SteamOS Stable, Beta or Preview, with SteamVR
- About 500 MB of free space during install
- A terminal: SSH, or Konsole on the desktop
- Steam running during install
- Your own Metroid Prime GameCube disc image: USA 1.00, 1.01 or 1.02 (PAL is experimental), as `.iso`, `.gcm`, `.nkit`, `.rvz`, `.ciso` or `.wbfs`

## Install

1. Download `primedframe-2.0.0-linux-arm64.tar.gz` and `SHA256SUMS` from [Releases](https://github.com/simplysanatori/PrimedFrame/releases).
2. In a terminal, in the download folder:

   ```sh
   sha256sum -c --ignore-missing SHA256SUMS
   mkdir -p ~/Downloads/primedframe && tar xf primedframe-2.0.0-linux-arm64.tar.gz -C ~/Downloads/primedframe
   ~/Downloads/primedframe/install.sh
   ```

It adds the **PrimedFrame** entry to your Steam library, with its art.

Other installers:

- [Frametanium](https://github.com/Cogential/frametanium): add `simplysanatori/PrimedFrame` there.
- [FrameDrop](https://framedropvr.com): [Install with FrameDrop](https://framedropvr.com/install?manifest=https%3A%2F%2Fgithub.com%2Fsimplysanatori%2FPrimedFrame%2Freleases%2Flatest%2Fdownload%2Fprimedframe.framedrop.json). Its first start installs the game.

Coming from 1.0? See [Upgrading from 1.0](CHANGELOG-2.0.0.md#upgrading-from-10). The Dolphin version stays on the [`legacy` branch](https://github.com/simplysanatori/PrimedFrame/tree/legacy).

## Controls

| Action | Control |
|---|---|
| Move | Left stick |
| Look, jump (stick up) | Right stick |
| Shoot | Right trigger |
| Lock on | Left trigger |
| Missile | Right grip |
| Map | Left grip |
| Beam select | Hold B, swing the cannon |
| Morph Ball | Left D-pad down |
| Pause | Left D-pad up |
| Auto Fire on/off | Right shoulder |
| VR menu | Left stick click or View |

Auto Fire, visors and the Morph Ball: see [docs/CONTROLS.md](docs/CONTROLS.md).

## Update

Repeat Install with the new release. **Check for Updates...** in the launcher opens the Releases page.

## Uninstall

```sh
~/Games/PrimedFrame/app/uninstall.sh
```

Add `--purge` to also delete your saves and settings in `~/.local/share/PrimedFrame/v2`.

## Troubleshooting

Run `install.sh` from `~/Downloads/primedframe`.

- "Steam was not reachable": start Steam, run `install.sh` again.
- "PrimedFrame is running": quit it, run `install.sh` again.
- The dock icon is a question mark: restart Steam once.
- The launcher opens every time: select your game and press **Play**.

## Reporting a problem

```sh
~/Games/PrimedFrame/app/collect-logs.sh
```

It writes `~/primedframe-logs-<date-time>.tar.gz`. Personal details are masked on a best-effort basis. Check it before sharing, then attach it to an [issue](https://github.com/simplysanatori/PrimedFrame/issues).

## More

- [Controls](docs/CONTROLS.md)
- [How the port works](docs/TECHNICAL.md)
- [Changelog](CHANGELOG-2.0.0.md)
- [PrimedGun 2.0's own README](README.upstream.md)

## Support

Support upstream: star [MetroidPrimePortVR](https://github.com/iChris4/MetroidPrimePortVR) and [sponsor iChris4 on GitHub](https://github.com/sponsors/iChris4).
For more Steam Frame ports, you can [buy me a coffee](https://ko-fi.com/sana748653).

## Credits

- Built by [simplysanatori](https://github.com/simplysanatori)
- Nobbie and iChris4, [PrimedGun](https://github.com/iChris4/MetroidPrimePortVR): the VR port this is built on
- [PrimeDecomp](https://github.com/PrimeDecomp/prime): the decompilation of the game
- [Odran's MetroidPrimePort](https://github.com/Odrannnn/MetroidPrimePort): the native port under it
- [Aurora](https://github.com/encounter/aurora): the graphics and platform layer
- Valve: the Steam Frame and SteamVR

Full list: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Unofficial fan project, not affiliated with Nintendo, Valve, PrimeDecomp or the PrimedGun authors. Metroid Prime is a Nintendo trademark.

## Licence

Each part keeps its own licence (CC0, GPL-3.0-or-later, MIT). See `LICENSE` and `NOTICE`.

## How this was made

I started this project and used AI (Claude Code with Fable and Opus 5.5) to speed up development. I write, review and fix code by hand all the way through, keep the AI's work checked and reined in, and aim to keep the port as lightweight as possible. I test it on my own Steam Frame. PrimedGun's README says Codex assisted its development.
