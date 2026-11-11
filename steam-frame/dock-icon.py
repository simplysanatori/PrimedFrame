#!/usr/bin/env python3
"""PrimedFrame: what makes its name and icon show in SteamVR's dock and dashboard.

    dock-icon.py install DEST --appid N [--art DIR] [--dry-run]
    dock-icon.py remove  DEST [--dry-run]

DEST is the program folder (~/Games/PrimedFrame/app); N is the appid of the Steam library
entry "PrimedFrame".  The icons are read from DEST/art, or from DIR (a dry run before
the files are in place).  --dry-run prints what would be done and changes nothing.  Two things draw the icon, and Steam's own entry does not cover them:

* The VR side runs as the SteamVR app "steam.app.<appid>".  SteamVR serves an app's picture
  from the image_path of an app manifest, and Steam's entry for a non-Steam shortcut has no
  usable one on the Frame, so the app showed only a generated "?" entry.  This writes
  DEST/primedframe.vrmanifest (is_internal: no extra "Non-Steam Games" entry, not in
  SteamVR's launch list) with the name and the full-bleed 512 px icon, and registers it with
  SteamVR.
* The flat Qt window (the dashboard window used for settings) has the window class
  "primedframe" (the program sets RESOURCE_NAME and the Qt desktop file name to it) or the
  program name "metroid_prime_port".  The dock looks that name up in the icon theme and cannot render
  SVG, so PNGs go to ~/.local/share/icons/hicolor/NxN/apps and a NoDisplay .desktop file
  ties the name to the Steam entry.

Everything is under $HOME, nothing needs root, and running it again changes nothing.
Environment: PRIMEDFRAME_VRCMD (default /opt/steamvr/bin/linuxarm64/vrcmd), HOME,
XDG_DATA_HOME, PRIMEDFRAME_VRSERVER_RUNNING (0/1, for tests).  GPLv3 or later.
"""
import json, os, subprocess, sys

ID = "primedframe"                  # window class / Qt desktop file name / icon name
ALIASES = ("metroid_prime_port", "PrimedGun")               # the program name, in case the class falls back to it
NAME = "PrimedFrame"
SIZES = (64, 128, 256, 512)
MANIFEST = "primedframe.vrmanifest"
DRY = False


def say(msg):
    print(("[dry run] would " if DRY else "") + msg)


def data_home():
    return os.environ.get("XDG_DATA_HOME") or os.path.join(os.path.expanduser("~"), ".local", "share")


def desktop_text(appid):
    gameid = (appid << 32) | 0x02000000
    return ("[Desktop Entry]\n"
            f"Name={NAME}\n"
            "Comment=Metroid Prime in VR, native on the Steam Frame, through its Steam entry\n"
            f"Exec=steam steam://rungameid/{gameid}\n"
            f"Icon={ID}\n"
            f"StartupWMClass={ID}\n"
            "Terminal=false\n"
            "Type=Application\n"
            "Categories=Game;\n"
            "NoDisplay=true\n")


def manifest_obj(dest, appid, art_src):
    launcher = dest + "/primedframe.sh"
    art = os.path.join(dest, "art")
    app = {
        "app_key": f"steam.app.{appid}",
        "launch_type": "binary",
        "binary_path_linux": launcher,
        "binary_path_linux_arm": launcher,
        "working_directory": dest,
        "image_path": os.path.join(art, "icon.png"),
        "is_internal": True,
        "strings": {"en_us": {"name": NAME}},
    }
    if os.path.exists(os.path.join(art_src, "portrait.png")):
        app["image_path_capsule"] = os.path.join(art, "portrait.png")
    return {"source": "builtin", "applications": [app]}


def write_if_changed(path, data):
    if isinstance(data, str):
        data = data.encode()
    try:
        if open(path, "rb").read() == data:
            return False
    except OSError:
        pass
    say(f"write {path} ({len(data)} bytes)")
    if not DRY:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "wb") as f:
            f.write(data)
    return True


def vrserver_running():
    if "PRIMEDFRAME_VRSERVER_RUNNING" in os.environ:      # for the tests
        return os.environ["PRIMEDFRAME_VRSERVER_RUNNING"] == "1"
    return subprocess.run(["pgrep", "-x", "vrserver"], capture_output=True).returncode == 0


def appconfig_path():
    return os.path.join(os.path.expanduser("~"), ".config/openvr/config/appconfig.json")


def appconfig_edit(add=None, drop=None):
    """Add or drop one manifest path in SteamVR's appconfig.json (only when SteamVR is not running)."""
    cfg_path = appconfig_path()
    try:
        cfg = json.load(open(cfg_path))
    except (OSError, ValueError):
        cfg = {}
    paths = [p for p in cfg.get("manifest_paths", []) if p != drop]
    if add and add not in paths:
        paths.append(add)
    if paths == cfg.get("manifest_paths", []):
        return False
    say(f"set manifest_paths in {cfg_path} to {paths}")
    if not DRY:
        cfg["manifest_paths"] = paths
        os.makedirs(os.path.dirname(cfg_path), exist_ok=True)
        json.dump(cfg, open(cfg_path, "w"), indent=3)
    return True


def install(dest, appid, art_src):
    n = 0
    # flat Qt window: icon theme + desktop file
    for s in SIZES:
        src = os.path.join(art_src, "icon.png" if s == 512 else f"icon-{s}.png")
        if not os.path.exists(src):
            print(f"dock icon: {src} is missing, skipped")
            continue
        data = open(src, "rb").read()
        for name in (ID,) + ALIASES:
            n += write_if_changed(os.path.join(data_home(), "icons/hicolor", f"{s}x{s}", "apps", name + ".png"), data)
    n += write_if_changed(os.path.join(data_home(), "applications", ID + ".desktop"), desktop_text(appid))
    out = [f"{n} icon/desktop file(s) {'to update' if DRY else 'updated'} ({ID}.png and "
           f"{', '.join(a + '.png' for a in ALIASES)} in hicolor, {ID}.desktop)"]
    # VR side: SteamVR app manifest
    man = os.path.join(dest, MANIFEST)
    changed = write_if_changed(man, json.dumps(manifest_obj(dest, appid, art_src), indent=2) + "\n")
    vrcmd = os.environ.get("PRIMEDFRAME_VRCMD", "/opt/steamvr/bin/linuxarm64/vrcmd")
    if os.path.exists(vrcmd) and vrserver_running():
        say(f"run {vrcmd} --appmanifest {man}")
        if not DRY:
            env = {k: v for k, v in os.environ.items() if k != "LD_LIBRARY_PATH"}
            r = subprocess.run([vrcmd, "--appmanifest", man], env=env, capture_output=True, timeout=30)
            out.append("SteamVR app manifest registered with the running SteamVR" if r.returncode == 0
                       else "SteamVR app manifest written; vrcmd could not register it (it loads at the next SteamVR start)")
            if r.returncode != 0:
                appconfig_edit(add=man)
    else:
        added = appconfig_edit(add=man)
        out.append("SteamVR app manifest " + ("written" if changed else "unchanged") +
                   ("; added to SteamVR's appconfig.json" if added else "") +
                   " (SteamVR is not running: it is read at the next SteamVR start)")
    print(f"dock icon (steam.app.{appid}): " + "; ".join(out))
    return 0


def remove(dest):
    for s in SIZES:
        for name in (ID,) + ALIASES:
            p = os.path.join(data_home(), "icons/hicolor", f"{s}x{s}", "apps", name + ".png")
            if os.path.exists(p):
                say(f"remove {p}")
                if not DRY:
                    os.remove(p)
    p = os.path.join(data_home(), "applications", ID + ".desktop")
    if os.path.exists(p):
        say(f"remove {p}")
        if not DRY:
            os.remove(p)
    man = os.path.join(dest, MANIFEST)
    if not vrserver_running():
        appconfig_edit(drop=man)
    if os.path.exists(man):
        say(f"remove {man}")
        if not DRY:
            os.remove(man)
    print("dock icon: files removed (SteamVR forgets the manifest at its next start)")
    return 0


def main(argv):
    global DRY
    args = [a for a in argv[1:] if a != "--dry-run"]
    DRY = len(args) != len(argv) - 1
    if len(args) < 2 or args[0] not in ("install", "remove"):
        print(__doc__)
        return 2
    cmd, dest = args[0], args[1].rstrip("/")
    appid = None
    art_src = os.path.join(dest, "art")
    if "--appid" in args:
        appid = int(args[args.index("--appid") + 1])
    if "--art" in args:
        art_src = args[args.index("--art") + 1]
    if cmd == "install":
        if appid is None:
            print("dock icon: install needs --appid N (the Steam entry's appid)")
            return 2
        return install(dest, appid, art_src)
    return remove(dest)


if __name__ == "__main__":
    sys.exit(main(sys.argv))
