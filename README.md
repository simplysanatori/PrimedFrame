# PrimedFrame

Metroid Prime in VR, running natively on the Valve Steam Frame.
It is a Linux Arm64 build of Nobbie248's [PrimedGun](https://github.com/Nobbie248/PrimedGun), the Dolphin based VR build.

## Features

- PrimedGun's VR controls, visor and arm cannon
- Starts from your Steam library into the headset
- 3x internal resolution with 2160 pixel eye images
- 120 Hz display, 60 frames a second even in heavy rooms

## How it works

The game runs on the Frame itself as a Linux program. It draws both eyes through the Frame's own SteamVR. You supply the disc image; no PC is needed.

## Requirements

- A Steam Frame with SteamOS and SteamVR, and a few hundred MB free
- A terminal: SSH, or Konsole on the desktop (SteamVR dashboard, Launch a program, Desktop)
- Steam running during install
- Your own Metroid Prime disc image

### Your disc image

- US release only: game ID GM8E01, revision 0
- `.rvz`, `.iso` or `.gcm`
- Dump your own disc: Dolphin's [Ripping Games](https://dolphin-emu.org/docs/guides/ripping-games/) guide
- Copy it to `~/Documents/Roms/gc`, with "Metroid Prime" in the file name

Full steps: [user guide](docs/USER_GUIDE.md#your-disc-image).

## Install

1. Download `primedframe-1.0.0-linux-arm64.tar.gz` and `SHA256SUMS` from [Releases](https://github.com/simplysanatori/PrimedFrame/releases) onto the Frame.
2. In a terminal, in the download folder:

   ```sh
   sha256sum -c --ignore-missing SHA256SUMS
   mkdir -p ~/Downloads/primedframe && tar xf primedframe-1.0.0-linux-arm64.tar.gz -C ~/Downloads/primedframe
   ~/Downloads/primedframe/install.sh --rom "/path/to/Metroid Prime (USA).rvz"
   ```

   Leave out `--rom` if the image is in `~/Documents/Roms/gc`.

Or with [Frametanium](https://github.com/Cogential/frametanium): add `simplysanatori/PrimedFrame` there.

Or with [FrameDrop](https://framedropvr.com): [Install with FrameDrop](https://framedropvr.com/install?manifest=https%3A%2F%2Fgithub.com%2Fsimplysanatori%2FPrimedFrame%2Freleases%2Flatest%2Fdownload%2Fprimedframe.framedrop.json).

## First run

1. Start SteamVR and launch **PrimedFrame** from your library.
2. Expect a few seconds of black. The first launch stutters.
3. Stand in the centre of your play space, face forward, and click the right stick to set your height.

## Controls

| Action | Control |
|---|---|
| Set your height | Click the right stick |
| Open or close the in-VR menu | Click the left thumbstick |
| Quit | **Exit Game** (menu, top right), pressed twice |

The rest are PrimedGun's: see its [controller layout](assets/controller%20layout.png) and the [user guide](docs/USER_GUIDE.md#controls).

## Update

Download the new release and repeat Install step 2 with its file name.

## Uninstall

With Steam running:

```sh
~/Games/PrimedFrame/app/tools/uninstall.sh
```

Add `--purge` to also delete your saves and settings.

## Troubleshooting

Run `install.sh` from `~/Downloads/primedframe`.

- **Steam's debugging port is not reachable:** start Steam, run `install.sh` again.
- **The game closes at once:** set your disc image with `install.sh --rom PATH`.
- **The game starts flat on the desktop:** run `install.sh` again.
- **Too slow in a heavy room:** set `InternalResolution = 2` in `~/.local/share/PrimedFrame/user/GameSettings/GM8E01.ini`.
- **No Steam overlay in the game:** expected.

More fixes: [full troubleshooting list](docs/USER_GUIDE.md#troubleshooting).

## Reporting a problem

```sh
~/Games/PrimedFrame/app/tools/collect-logs.sh
```

It writes `~/primedframe-logs-<date-time>.tar.gz`. Personal details are masked, as a best effort. Check it before sharing, then attach it to an [issue](https://github.com/simplysanatori/PrimedFrame/issues) and say what happened.

## More

- [User guide](docs/USER_GUIDE.md): settings, files, troubleshooting
- [AGENTS.md](AGENTS.md): for developers and AI agents
- [Building from source](docs/TECHNICAL_NOTES.md#build-from-source)
- [Changelog](docs/CHANGELOG.md)

## Support

Please support upstream first: star [PrimedGun](https://github.com/Nobbie248/PrimedGun) and send improvements there, [sponsor iChris4 on GitHub](https://github.com/sponsors/iChris4), and follow [dolphin-emu.org](https://dolphin-emu.org).

For more Steam Frame ports, [buy me a coffee](https://ko-fi.com/sana748653).

## Credits

- [simplysanatori](https://github.com/simplysanatori): this port
- Nobbie, [PrimedGun](https://github.com/Nobbie248/PrimedGun): the VR game code
- iChris4: Dolphin ReduX, PrimedGun's base
- Carl Kenner and Dolphin VR contributors: the first VR Dolphin
- josethevrtech, [PrimedSteam](https://github.com/josethevrtech/PrimedSteam): PrimedGun's SteamOS build
- The Dolphin team: the emulator
- Valve: the Steam Frame and SteamVR

Full credits: [user guide](docs/USER_GUIDE.md#support-and-credits-in-full), [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md). Unofficial, not affiliated with Nintendo, Valve, Dolphin or PrimedGun. Metroid Prime is a Nintendo trademark.

## Licence

GPL version 2 or later, as Dolphin: see `COPYING` and `LICENSES/`.

## How this was made

I started this project and used AI (Claude Code with Fable 5.1, Opus 5.5 and Sonnet 5.5) to speed up development. I write, review and fix code by hand all the way through, keep the AI's work checked and reined in, and aim to keep the port as lightweight as possible. I test it on my own Steam Frame. PrimedGun's README says Codex assisted its development.
