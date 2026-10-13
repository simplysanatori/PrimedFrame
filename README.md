# Metroid Prime — native port

A native build of **Metroid Prime** for Linux, Windows and Android, compiled
from the [PrimeDecomp](https://github.com/PrimeDecomp/prime) decompilation and
rendered through Aurora (SDL3/WebGPU). There's no emulator involved. Supported
discs: USA 1.00, 1.01 and 1.02 (`GM8E01`), and PAL (`GM8P01`, experimental).

**No game content is included.** You need your own copy of the game: the port
reads it from a disc image and never ships any of its assets. This repository
ships no disc image, and neither does any package or release built from it.

## What it adds over the console release

- **Remastered assets** — import models and effects from your own Metroid Prime
  Remastered dump (F1 > Remastered; see `docs/MODS.md`)
- **Widescreen** with an edge-to-edge HUD, and an **uncapped frame rate** over
  the original fixed-rate simulation
- **Graphics:** FOV, MSAA, anisotropic filtering, HUD scale, arm cannon offset,
  and toggles to hide the helmet and visor effects
- **Controls:** mouse and twin-stick aim, gyro, full rebinding with presets
  (GameCube, Remastered, Modern, Southpaw), and button prompts that match your
  bindings
- **Optional tweaks**, all off by default: Fast Morph, Spring Ball, Toggle
  Lock-On, Sticky Charge
- **Skippable cutscenes**
- **Save states** (eight slots plus undo) and **quick resume** after quitting
- **Saves:** import from and export to USA/PAL GameCube, Wii Trilogy and Dolphin
- **Photo mode** with a free camera and high-resolution capture
- **Speedrunning:** in-game timer and LiveSplit auto-splitting
- **Map and tracker:** reveal the map, plus item, scan and room tracking
- **Mods and texture packs**, including HD texture replacements
- **Unlocks:** Hard mode, the Fusion Suit and the galleries, without finishing
  the game first
- **1.00 compatibility mode** for later disc revisions
- **Discord Rich Presence** on desktop, and a **touch overlay** on Android
- The **F1 overlay** has every setting, with search; the common ones are also in
  pause > Options

## Randomizer and Archipelago

Both are supported, on all three platforms.

- **Randomizer** — item placement is driven by a seed file. `tools/rando_seed.py`
  generates one and `docs/RANDOMIZER.md` covers the format.
- **Archipelago multiworld** — the Metroid Prime AP world's item and location
  tables are built in. Open F1 > Archipelago, enter the server (`host:port`), your
  slot name and the password if any, and connect; no seed file is needed. The
  seed's options are applied and a new game starts at the Landing Site with
  the intro skipped. Each seed and slot gets its own memory card, checks made
  offline are sent on the next connect, Recent games resumes an earlier seed,
  and the Chat sub-tab shows the server log and sends messages such as `!hint`.
  DeathLink is supported.

  `wss://` works out of the box on every platform. See `docs/ARCHIPELAGO.md`.

Cutscenes are always skippable in randomizer and Archipelago games.

## Downloads

Releases are published on the
[GitHub releases page](https://github.com/Odrannnn/MetroidPrimePort/releases).
Every build needs your own disc image; none of them contains one.

| Platform | Artefact | Notes |
| --- | --- | --- |
| Linux | AppImage | Self-contained, no install. `chmod +x` and run. |
| Linux | Flatpak | `io.github.odrannnn.metroidprimeport` |
| Linux | `.tar.gz` | The bare binary, its textures and the licences |
| Windows | `.zip` | Unzip anywhere; needs a Visual C++ redistributable |
| Android | `.apk` | Sideload; enable install from unknown sources |

Android APKs are signed with a project release key. An app signed with one key
cannot be updated by one signed with another, so keep the key — see
`docs/RELEASING.md`.

## Building

Needs CMake 3.25+, Ninja and a C++20 toolchain. Aurora and MusyX are vendored, so
a normal clone is enough; Aurora fetches its pinned dependencies on the first
configure.

```sh
cmake -S . -B build/native -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DAURORA_ENABLE_TESTS=OFF -DMP_BUILD_TESTS=ON
cmake --build build/native -j 4
ctest --test-dir build/native -L port --output-on-failure
```

On Windows, run from an MSVC developer shell and add
`-DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl`. CI builds and tests
Linux (GCC 14 and Clang 18) and Windows (clang-cl); see
`.github/workflows/`.

### Android

```sh
tools/make_android_keystore.sh          # once; writes android/keystore.properties
tools/android_apk.sh :app:assembleRelease
```

Both files it writes name a private key and are deliberately untracked. The
build needs a Rust toolchain on `PATH`, because the disc layer is `nod`
cross-compiled for `aarch64-linux-android`. Without one the build still
succeeds but links a stub that cannot open a disc — see
`docs/ANDROID_BUILD_PROBE.md`.

## Running

```sh
./build/native/metroid_prime_port "/path/to/Metroid Prime (USA) (v1.00).iso"
```

The disc can also be set with `MP_DISC`, kept beside the executable, or picked
through a file dialog the first time you launch without one — the choice is
remembered in the settings. The image must be `GM8E01`, disc 0, revision 0, 1 or 2 (USA 1.00, 1.01 or 1.02), or
`GM8P01` (PAL, experimental, which also brings its French, German, Spanish and Italian text;
a USA player can add those from a PAL image in F1 > Game > Language). If a
remembered disc stops opening — the file moved, or on Android the system revoked
its access grant — the port asks again rather than exiting.

On Android the first pick is copied into app storage, so later launches need no
permission and there is nothing to grant again.

`--version` prints the source revision without starting graphics; include it in
bug reports. The F1 overlay holds the rest of the settings, and the same flags
are available as `MP_*` environment variables.

## Packaging

`tools/make_appimage.sh` builds a self-contained AppImage and
`tools/make_flatpak.sh` a Flatpak. Neither bundles the disc. See
`docs/NATIVE_PORT.md` for what each one expects from the host.

## Credits and licensing

This is a fork of the [PrimeDecomp/prime](https://github.com/PrimeDecomp/prime)
decompilation, which the game logic is built from. Aurora (`extern/aurora`) and
MusyX (`extern/musyx`) are MIT-licensed vendored snapshots, and the button
prompt icons come from Kenney's Input Prompts pack (CC0), vendored under
`tools/prompt_icons`. Their licences are kept alongside them and are copied into
release packages.

`LICENSE` covers this project's own work — the platform layer, the build and
packaging scripts, and the Android and Flatpak integration. It does **not** cover
the game code under `src/` and `include/`, which comes from the decompilation
and remains Nintendo's. See `NOTICE`.

Metroid Prime is a trademark of Nintendo. This project is unaffiliated with
Nintendo and Retro Studios, and ships no game data.

`docs/NATIVE_PORT.md` is the detailed reference: build options, the smoke-test hooks,
and the platform notes. Every setting and environment variable lives in
`docs/SETTINGS.md` (debug console: `docs/CONSOLE.md`, mods: `docs/MODS.md`).

## AI assistance

Parts of this port were written with AI coding assistants, under my oversight.
