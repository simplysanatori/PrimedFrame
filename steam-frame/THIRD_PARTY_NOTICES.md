# Third-party notices

PrimedFrame is the Steam Frame build of PrimedGun, a native port of Metroid Prime with VR support. The release
folder carries the full licence and notice texts in `licenses/`; this page says what is in it and where it
comes from. The port's own licence and notice are in `LICENSE` and `NOTICE`. The game's data is not included:
you supply your own disc image.

## Program

| Component | Licence | Where |
| --- | --- | --- |
| PrimedGun / MetroidPrimePortVR (VR, launcher) | GPL-3.0-or-later | `licenses/GPL-3.0.txt`, `NOTICE` |
| Metroid Prime Port and PrimeDecomp (native port) | MIT and CC0, as `NOTICE` explains | `licenses/native-port-MIT.txt`, `LICENSE` |
| Aurora (GX and platform layer), MusyX | MIT | `licenses/aurora.txt`, `licenses/musyx.txt` |
| nod (disc reading) | Apache-2.0 and MIT | `licenses/nod-Apache-2.0.txt`, `licenses/nod-MIT.txt` |
| Dawn (WebGPU), patched, linked in | BSD-3-Clause | `licenses/dawn.txt` |
| SDL 3, built from source and linked in | zlib | `licenses/sdl-src.txt` |
| OpenXR loader and headers, linked in | Apache-2.0 | `licenses/openxr-src.txt` |
| Abseil, fmt, SQLite, libpng, FreeType, Dear ImGui, xxHash, Tracy, astcenc | their own licences | `licenses/` (one file each) |
| Texture importer pieces (tegra_swizzle, bcdec, astc-decode, retrotool, Ryujinx) | MIT and Apache-2.0 | `licenses/remastered-*` |

## Libraries packed in `lib/` and `plugins/`

The launcher is a Qt 6 program. So that it runs on every SteamOS build, Qt and the libraries SteamOS does not
promise to keep are packed with it, unchanged, copied from SteamOS itself:

| Library | Licence | Text |
| --- | --- | --- |
| Qt 6 (Core, Gui, Widgets, DBus, WaylandClient, XcbQpa) and its platform plugins | LGPL-3.0 (with parts under other free licences) | `licenses/system/qt6-base/` |
| ICU | Unicode licence | `licenses/system/icu/` |
| OpenSSL (libssl, libcrypto) | Apache-2.0 | `licenses/system/openssl/` |
| libsystemd, libcap, libuuid | LGPL-2.1-or-later | `licenses/system/systemd/`, `licenses/system/libcap/`, `licenses/system/util-linux-libs/` |
| PCRE2, libb2, libffi, md4c, double-conversion, Graphite2, bzip2, libxcb and friends, libgomp | BSD, MIT, CC0, Apache-2.0 and similar | `licenses/system/` |

Qt is used under the LGPL version 3. You may replace the Qt libraries in `lib/` and `plugins/` with your own
build of the same version: the launcher links to them dynamically and nothing else is changed. The Qt source is
at https://download.qt.io/official_releases/qt/ and SteamOS's package sources are published by Valve.
