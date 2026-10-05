#!/usr/bin/python3
"""mprig: isolated headless runs of the port for debugging (Xvfb + game + debug console).

    mprig.py start t1 --room 83F6FF6F:492CBF4A     # own Xvfb + game; prints "name display port log"
    mprig.py cmd t1 status 'roomenv info'          # one console command per argument
    mprig.py shot t1 /tmp/a.png 'roomenv bloom off'
    mprig.py ab t1 /tmp/x --a 'roomenv bloom off' --b 'roomenv bloom on'
    mprig.py stop --all

State lives in build/rig/ (git-ignored): build/rig/<name>/{run.json,game.log,user/,shots/}.
Defaults (disc, real user dir, build) come from build/rig.ini [rig], overridden by env
MPRIG_DISC / MPRIG_USER / MPRIG_BUILD (disc also $MP_DISC) and by flags. Every command exits
non-zero with a one-line "mprig: <reason>" on stderr when it fails. Processes are only ever
signalled by the pids recorded in run.json (never by pattern); captures never touch :0.
"""
import argparse
import atexit
import configparser
import fcntl
import json
import os
import re
import shutil
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RIG = ROOT / "build" / "rig"
LOCKS = RIG / "locks"
CACHE = RIG / "cache"
NOISE = ["MP frame", "prompt ", "mpstream"]  # lines `log` hides unless --raw
FIRST_DISPLAY, FIRST_PORT = 200, 5100
CRASH_RE = re.compile(r"port:\s+(?:#(\d+)\s+)?(?:at\s+)?(\S+)\+0x([0-9a-fA-F]+)")


def die(msg, code=1):
    print(f"mprig: {msg}", file=sys.stderr)
    sys.exit(code)


# ---------------------------------------------------------------- config

def config():
    cp = configparser.ConfigParser()
    cp.read(ROOT / "build" / "rig.ini")
    sec = cp["rig"] if cp.has_section("rig") else {}
    disc = os.environ.get("MPRIG_DISC") or sec.get("disc") or os.environ.get("MP_DISC") or ""
    user = os.environ.get("MPRIG_USER") or sec.get("user") or str(Path.home() / ".local/share/Metroid Prime")
    build = os.environ.get("MPRIG_BUILD") or sec.get("build") or "port-gcc"
    return {"disc": disc, "user": user, "build": build}


def resolve_binary(build):
    p = Path(build)
    if "/" not in build:
        p = ROOT / "build" / build
    p = p.resolve()
    return p / "metroid_prime_port" if p.is_dir() else p


# ---------------------------------------------------------------- process helpers

def proc_start(pid):
    try:
        with open(f"/proc/{pid}/stat") as f:
            s = f.read()
        rest = s[s.rindex(")") + 2:].split()
        return rest[19] if rest[0] != "Z" else None
    except (OSError, ValueError, IndexError):
        return None


def alive(entry):
    """entry = {'pid':, 'st':}: the recorded process still runs (and the pid was not reused)."""
    return bool(entry) and proc_start(entry["pid"]) == entry["st"]


def ident(pid):
    return {"pid": pid, "st": proc_start(pid)}


def kill_entry(entry, group=False, wait=3.0):
    if not alive(entry):
        return
    for sig in (signal.SIGTERM, signal.SIGKILL):
        try:
            (os.killpg if group else os.kill)(entry["pid"], sig)
        except OSError:
            return
        end = time.monotonic() + wait
        while time.monotonic() < end:
            if not alive(entry):
                return
            time.sleep(0.1)


# ---------------------------------------------------------------- registry

def run_dir(name):
    if not re.fullmatch(r"[A-Za-z0-9_.-]+", name):
        die(f"bad run name '{name}'")
    return RIG / name


def load_run(name):
    f = run_dir(name) / "run.json"
    if not f.exists():
        die(f"no run '{name}' (see: mprig.py ls)")
    return json.loads(f.read_text())


def save_run(run):
    f = Path(run["dir"]) / "run.json"
    tmp = f.with_suffix(".tmp")
    tmp.write_text(json.dumps(run, indent=1))
    tmp.replace(f)


def all_runs():
    out = []
    if RIG.is_dir():
        for d in sorted(RIG.iterdir()):
            f = d / "run.json"
            if f.is_file():
                try:
                    out.append(json.loads(f.read_text()))
                except ValueError:
                    pass
    return out


def game_alive(run):
    return alive(run.get("game"))


def xvfb_alive(run):
    return alive(run.get("xvfb"))


class Lock:
    def __init__(self, name):
        LOCKS.mkdir(parents=True, exist_ok=True)
        self.f = open(LOCKS / name, "w")

    def __enter__(self):
        fcntl.flock(self.f, fcntl.LOCK_EX)
        return self

    def __exit__(self, *a):
        fcntl.flock(self.f, fcntl.LOCK_UN)
        self.f.close()


def port_free(port):
    s = socket.socket()
    try:
        s.bind(("127.0.0.1", port))
        return True
    except OSError:
        return False
    finally:
        s.close()


def allocate(taken_displays, taken_ports):
    d = FIRST_DISPLAY
    while d in taken_displays or Path(f"/tmp/.X{d}-lock").exists() or Path(f"/tmp/.X11-unix/X{d}").exists():
        d += 1
    p = FIRST_PORT
    while p in taken_ports or not port_free(p):
        p += 1
    return d, p


# ---------------------------------------------------------------- console

def console(port, commands, wait=0.0, timeout=600.0):
    """Send commands; return [(cmd, ok, reply_lines)]. Raises OSError/ConnectionError if unreachable."""
    deadline = time.monotonic() + wait
    while True:
        try:
            sock = socket.create_connection(("127.0.0.1", port), timeout=5)
            break
        except OSError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.5)
    sock.settimeout(timeout)
    reader = sock.makefile("r", encoding="utf-8", errors="replace")
    out = []
    try:
        for c in commands:
            sock.sendall((c + "\n").encode())
            lines = []
            while True:
                r = reader.readline()
                if not r:
                    lines.append("=> connection closed")
                    break
                r = r.rstrip("\n")
                lines.append(r)
                if r.startswith("=> "):
                    break
            out.append((c, lines[-1] == "=> ok", lines))
            if lines[-1] == "=> connection closed":
                break
    finally:
        sock.close()
    return out


def run_console(run, commands, echo=True, timeout=600.0):
    """Print replies; return True when every command succeeded. Exits 2 if unreachable."""
    if not game_alive(run):
        die(f"run '{run['name']}' is not running (see: mprig.py log {run['name']}; crash {run['name']})", 2)
    try:
        res = console(run["port"], commands, timeout=timeout)
    except (OSError, ConnectionError) as e:
        die(f"console of '{run['name']}' unreachable on port {run['port']}: {e}", 2)
    ok = True
    for c, good, lines in res:
        if echo:
            if len(commands) > 1:
                print(f"> {c}")
            print("\n".join(lines))
        if not good:
            ok = False
            print(f"mprig: command '{c}' failed: {lines[-1]}", file=sys.stderr)
    if len(res) < len(commands):
        ok = False
        print("mprig: the game closed the connection (crashed? see: mprig.py crash)", file=sys.stderr)
    return ok


def read_cmds(args):
    cmds = list(args.commands)
    if getattr(args, "file", None):
        with open(args.file, encoding="utf-8") as f:
            cmds += [x.strip() for x in f if x.strip() and not x.lstrip().startswith("#")]
    return cmds


def take_shot(run, cmds, settle=0):
    """Run cmds, optionally wait frames, `shot`; return the BMP path."""
    seq = list(cmds) + ([f"wait {settle}"] if settle else []) + ["shot"]
    if not game_alive(run):
        die(f"run '{run['name']}' is not running", 2)
    try:
        res = console(run["port"], seq)
    except (OSError, ConnectionError) as e:
        die(f"console of '{run['name']}' unreachable: {e}", 2)
    for c, good, lines in res:
        if not good:
            die(f"command '{c}' failed: {lines[-1]}")
    if len(res) < len(seq):
        die("the game closed the connection while taking the shot (crashed? see: mprig.py crash)")
    for line in reversed(res[-1][2]):
        m = re.search(r"(\S+\.bmp)", line)
        if m:
            p = Path(m.group(1))
            if not p.is_absolute():
                p = Path(run["dir"]) / p
            if p.exists():
                return p
    die("`shot` printed no screenshot path: " + " | ".join(res[-1][2])[:200])


def bmp_to_png(bmp, out, crop=None):
    from PIL import Image
    img = Image.open(bmp).convert("RGB")
    if crop:
        x, y, w, h = crop
        img = img.crop((x, y, x + w, y + h))
    out = Path(out).resolve()
    out.parent.mkdir(parents=True, exist_ok=True)
    img.save(out)
    bmp.unlink()
    return out


def parse_crop(s):
    try:
        v = [int(x) for x in s.split(",")]
        assert len(v) == 4
        return v
    except (ValueError, AssertionError):
        die(f"bad --crop '{s}' (want x,y,w,h)")


# ---------------------------------------------------------------- crash symbolizing

def find_crash(text):
    """Frames of the last crash block: [(label, module, offset)], plus the header and build lines."""
    lines = text.splitlines()
    idx = [i for i, l in enumerate(lines) if "port: crashed:" in l]
    if not idx:
        return None
    block = lines[idx[-1]:idx[-1] + 80]
    head = block[0].strip()
    rev = None
    frames = []
    for l in block[1:]:
        m = re.search(r"port: build (\S+)", l)
        if m:
            rev = m.group(1)
            continue
        m = CRASH_RE.search(l)
        if m and l.lstrip().startswith("port:"):
            frames.append(("#" + m.group(1) if m.group(1) else "at", m.group(2), int(m.group(3), 16)))
    return {"head": head, "rev": rev, "frames": frames}


def short_path(p):
    p = p.replace(str(ROOT) + "/", "")
    return re.sub(r"^build/[^/]+/\.\./\.\./", "", p)


def symbolize(crash, binary, extra_binary=None):
    """Return printable lines; the binary's own module is resolved with one addr2line call."""
    bins = {Path(binary).name: str(binary)}
    if extra_binary:
        bins[Path(extra_binary).name] = str(extra_binary)
    out = [crash["head"]]
    if crash["rev"] and Path(binary).exists():
        try:
            ver = subprocess.run([str(binary), "--version"], capture_output=True, text=True, timeout=15).stdout
            if crash["rev"][:8] not in ver:
                out.append(f"WARN log build {crash['rev']} differs from {Path(binary).name} --version "
                           f"({ver.strip()[:60]}): symbols may be wrong")
        except (OSError, subprocess.TimeoutExpired):
            pass
    resolved = {}
    for mod, path in bins.items():
        offs = sorted({o for _, m, o in crash["frames"] if m == mod})
        if not offs or not Path(path).exists():
            continue
        r = subprocess.run(["addr2line", "-a", "-f", "-C", "-i", "-e", path] + [hex(o) for o in offs],
                           capture_output=True, text=True)
        cur, pending = None, []
        for l in r.stdout.splitlines():
            if l.startswith("0x") and re.fullmatch(r"0x[0-9a-fA-F]+", l):
                cur = int(l, 16)
                resolved[(mod, cur)] = []
                pending = []
            elif cur is not None:
                if not pending:
                    pending.append(l)
                else:
                    resolved[(mod, cur)].append(f"{pending.pop()} {short_path(l)}")
    for label, mod, off in crash["frames"]:
        syms = resolved.get((mod, off))
        if syms:
            out.append(f"{label} {syms[0]}" + "".join(f"  [inl {s}]" for s in syms[1:]))
        else:
            out.append(f"{label} {mod}+{off:#x}")
    return out


def crash_report(text, binary, extra=None):
    c = find_crash(text)
    if not c:
        return None
    return symbolize(c, binary, extra)


# ---------------------------------------------------------------- subcommands

def cmd_start(a):
    cfg = config()
    name = a.name
    d = run_dir(name)
    RIG.mkdir(parents=True, exist_ok=True)
    CACHE.mkdir(exist_ok=True)
    disc = a.disc or cfg["disc"]
    if not disc or not Path(disc).is_file():
        die(f"disc image not found ('{disc}'): set disc in build/rig.ini, MPRIG_DISC or --disc")
    binary = resolve_binary(a.build or cfg["build"])
    if not binary.is_file():
        die(f"binary not found: {binary}")
    for kv in a.env:
        if "=" not in kv:
            die(f"bad --env '{kv}' (want K=V)")
    m = re.fullmatch(r"(\d+)x(\d+)", a.size)
    if not m:
        die(f"bad --size '{a.size}' (want WxH)")
    if a.room and not re.fullmatch(r"[0-9A-Fa-f]{1,8}(:[0-9A-Fa-f]{1,8})?", a.room):
        die(f"bad --room '{a.room}' (want MLVL[:MREA] in hex)")

    with Lock("alloc.lock"):
        old = d / "run.json"
        if old.exists():
            prev = json.loads(old.read_text())
            if game_alive(prev):
                if not a.replace:
                    die(f"run '{name}' is already running (use --replace or stop it)")
                stop_run(prev, quiet=True)
        if d.exists():
            shutil.rmtree(d)
        live = [r for r in all_runs() if game_alive(r) or xvfb_alive(r) or r.get("state") == "starting"]
        display, port = allocate({r["display"] for r in live}, {r["port"] for r in live})
        (d / "user").mkdir(parents=True)
        (d / "shots").mkdir()
        run = {"name": name, "dir": str(d), "display": display, "port": port, "binary": str(binary),
               "build": binary.parent.name, "room": a.room, "disc": disc, "user": str(d / "user"),
               "log": str(d / "game.log"), "started": time.time(), "state": "starting",
               "xvfb": None, "game": None, "env": a.env, "owner": os.getpid()}
        save_run(run)

    ok = False

    def cleanup():
        if not ok:
            kill_entry(run.get("game"), group=True)
            kill_entry(run.get("xvfb"))
            run["state"] = "failed"
            save_run(run)
    atexit.register(cleanup)
    for s in (signal.SIGINT, signal.SIGTERM, signal.SIGHUP):
        signal.signal(s, lambda *_: sys.exit(130))

    # user dir: settings + imgui only (the real one is GBs)
    real = Path(cfg["user"])
    user = d / "user"
    if a.settings == "none":
        pass
    else:
        src = Path(a.settings) if a.settings else real / "port_settings.ini"
        if a.settings and not src.is_file():
            die(f"--settings file not found: {src}")
        if src.is_file():
            shutil.copy(src, user / "port_settings.ini")
    if (real / "imgui.ini").is_file():
        shutil.copy(real / "imgui.ini", user / "imgui.ini")
    if a.saves:
        for sub in ("USA", "savestates"):
            if (real / sub).is_dir():
                shutil.copytree(real / sub, user / sub)
    if a.mods == "none":
        mods = d / "emptymods"
        mods.mkdir()
    else:
        mods = Path(a.mods) if a.mods else real / "mods"
    mods = mods.resolve() if mods.exists() else mods

    xlog = open(d / "xvfb.log", "w")
    xv = subprocess.Popen(["Xvfb", f":{display}", "-screen", "0", f"{a.size}x24", "-ac"],
                          stdout=xlog, stderr=xlog, start_new_session=True)
    run["xvfb"] = ident(xv.pid)
    save_run(run)
    end = time.monotonic() + 15
    while not Path(f"/tmp/.X11-unix/X{display}").exists():
        if xv.poll() is not None or time.monotonic() > end:
            die(f"Xvfb :{display} failed to start: {(d / 'xvfb.log').read_text()[-200:].strip()}")
        time.sleep(0.1)

    env = {k: v for k, v in os.environ.items() if not k.startswith("MP_") and k not in ("WAYLAND_DISPLAY",)}
    env.update({
        "DISPLAY": f":{display}", "XDG_RUNTIME_DIR": f"/run/user/{os.getuid()}", "SDL_AUDIO_DRIVER": "dummy",
        "MESA_VK_WSI_DEBUG": "sw", "SDL_VIDEODRIVER": "x11",  # RADV on Xvfb has no DRI3
        "MP_USER_PATH": str(user), "MP_CACHE_PATH": str(CACHE), "MP_MODS": str(mods),
        "MP_FAST_BOOT": "1", "MP_DISABLE_AI_AUDIO": "1", "MP_ASPECT": "16:9", "MP_CONSOLE": str(port),
        "ASAN_OPTIONS": "detect_leaks=0:abort_on_error=1",
    })
    if a.room:
        env["MP_BOOT_WORLD"] = a.room
    for kv in a.env:
        k, v = kv.split("=", 1)
        env[k] = v
    argv = [str(binary), disc]
    if a.gdb:
        argv = ["gdb", "-batch", "-ex", "run", "-ex", "bt", "-ex", "thread apply all bt 8", "--args"] + argv
    log = open(d / "game.log", "w")
    game = subprocess.Popen(argv, cwd=d, env=env, stdout=log, stderr=subprocess.STDOUT,
                            stdin=subprocess.DEVNULL, start_new_session=True)
    run["game"] = ident(game.pid)
    run["state"] = "running"
    save_run(run)

    end = time.monotonic() + a.wait
    ready = False
    while time.monotonic() < end:
        if game.poll() is not None:
            break
        try:
            res = console(port, ["status"], timeout=10)
            if res and res[0][1]:
                ready = True
                break
        except (OSError, ConnectionError):
            pass
        time.sleep(1)
    if not ready:
        text = (d / "game.log").read_text(errors="replace")
        died = game.poll() is not None
        print("\n".join(text.splitlines()[-15:]), file=sys.stderr)
        rep = crash_report(text, binary)
        if rep:
            print("\n".join(rep), file=sys.stderr)
        die(f"'{name}' " + (f"died during startup (exit {game.returncode})" if died
                             else f"console not ready after {a.wait:.0f}s"))
    ok = True
    print(f"{name} :{display} {port} {d / 'game.log'}")


def stop_run(run, quiet=False):
    name = run["name"]
    if game_alive(run):
        try:
            console(run["port"], ["quit"], timeout=3)
        except (OSError, ConnectionError, socket.timeout):
            pass
        end = time.monotonic() + 5
        while game_alive(run) and time.monotonic() < end:
            time.sleep(0.2)
        kill_entry(run["game"], group=True)
    kill_entry(run.get("xvfb"))
    run["state"] = "stopped"
    save_run(run)
    log = Path(run["log"])
    if log.exists():
        rep = crash_report(log.read_text(errors="replace"), run["binary"])
        if rep:
            print(f"{name}: crash found in log:\n" + "\n".join(rep))
    if not quiet:
        print(f"{name}: stopped")


def cmd_stop(a):
    if a.all:
        runs = [r for r in all_runs() if game_alive(r) or xvfb_alive(r)]
        if not runs:
            print("no live runs")
        for r in runs:
            stop_run(r)
        return
    if not a.name:
        die("give a run name or --all")
    stop_run(load_run(a.name))


def cmd_cmd(a):
    cmds = read_cmds(a)
    if not cmds:
        die("no commands (give them as arguments or -f FILE)")
    sys.exit(0 if run_console(load_run(a.name), cmds) else 1)


def cmd_shot(a):
    run = load_run(a.name)
    crop = parse_crop(a.crop) if a.crop else None
    bmp = take_shot(run, a.commands, a.settle)
    print(bmp_to_png(bmp, a.out, crop))


def cmd_pick(a):
    run = load_run(a.name)
    ok = run_console(run, [f"pick {a.x} {a.y}"])
    if a.shot:
        # A normal frame (pick put the view back) with a crosshair where it looked.
        from PIL import Image, ImageDraw
        bmp = take_shot(run, [])
        img = Image.open(bmp).convert("RGB")
        bmp.unlink()
        d = ImageDraw.Draw(img)
        for c, w in (((0, 0, 0), 3), ((255, 0, 255), 1)):
            d.line([(a.x - 12, a.y), (a.x + 12, a.y)], fill=c, width=w)
            d.line([(a.x, a.y - 12), (a.x, a.y + 12)], fill=c, width=w)
        out = Path(a.shot).resolve()
        out.parent.mkdir(parents=True, exist_ok=True)
        img.save(out)
        print(out)
    sys.exit(0 if ok else 1)


def image_stats(pa, pb):
    import numpy as np
    from PIL import Image
    A = Image.open(pa).convert("RGB")
    B = Image.open(pb).convert("RGB")
    if A.size != B.size:
        die(f"size mismatch: {A.size} vs {B.size}")
    x = np.asarray(A).astype(np.int16)
    y = np.asarray(B).astype(np.int16)
    d = np.abs(x - y)
    mad = float(d.mean())
    mse = float((d.astype(np.float64) ** 2).mean())
    psnr = 99.0 if mse == 0 else 10 * np.log10(255.0 ** 2 / mse)
    mx = d.max(axis=2)
    ch = mx > 8
    if ch.any():
        ys, xs = np.nonzero(ch)
        bbox = f"{xs.min()},{ys.min()},{xs.max() - xs.min() + 1},{ys.max() - ys.min() + 1}"
    else:
        bbox = "0,0,0,0"
    w = np.array([0.2126, 0.7152, 0.0722])
    la = float((x * w).sum(axis=2).mean())
    lb = float((y * w).sum(axis=2).mean())
    line = (f"mad={mad:.3f} psnr={psnr:.2f} changed={100 * ch.mean():.2f}% bbox={bbox} "
            f"luma_a={la:.1f} luma_b={lb:.1f}")
    return mad, line, mx


def heatmap(mx):
    import numpy as np
    from PIL import Image
    v = np.clip(mx.astype(np.int32) * 4, 0, 255)
    r = v
    g = np.clip(v * 2 - 255, 0, 255)
    b = np.clip(v * 2 - 400, 0, 255)
    return Image.fromarray(np.stack([r, g, b], axis=2).astype(np.uint8))


def cmd_diff(a):
    for p in (a.a, a.b):
        if not Path(p).is_file():
            die(f"no such image: {p}")
    mad, line, mx = image_stats(a.a, a.b)
    if a.out:
        heatmap(mx).save(a.out)
    print(line)
    if a.fail_above is not None and mad > a.fail_above:
        die(f"mad {mad:.3f} above {a.fail_above}", 3)


def captioned(imgs, labels, cols, width):
    from PIL import Image, ImageDraw
    thumbs = []
    for p, lab in zip(imgs, labels):
        im = Image.open(p).convert("RGB")
        h = max(1, round(im.height * width / im.width))
        thumbs.append((im.resize((width, h)), lab))
    cap = 16
    rows = [thumbs[i:i + cols] for i in range(0, len(thumbs), cols)]
    rh = [max(t.height for t, _ in r) + cap for r in rows]
    sheet = Image.new("RGB", (cols * width, sum(rh)), (24, 24, 24))
    dr = ImageDraw.Draw(sheet)
    y = 0
    for r, h in zip(rows, rh):
        for i, (t, lab) in enumerate(r):
            sheet.paste(t, (i * width, y + cap))
            dr.text((i * width + 4, y + 3), lab, fill=(255, 255, 255))
        y += h
    return sheet


def cmd_sheet(a):
    for p in a.images:
        if not Path(p).is_file():
            die(f"no such image: {p}")
    labels = a.labels.split(",") if a.labels else [Path(p).name for p in a.images]
    if len(labels) != len(a.images):
        die("--labels count differs from the image count")
    cols = a.cols or min(len(a.images), 3)
    captioned(a.images, labels, cols, a.width).save(a.out)
    print(Path(a.out).resolve())


def cmd_ab(a):
    run = load_run(a.name)
    split = lambda s: [c.strip() for c in s.split(";") if c.strip()]
    pre = Path(a.prefix).resolve()
    pre.parent.mkdir(parents=True, exist_ok=True)
    hold = [] if a.no_hold else ["hold 1"]  # freeze game ticks so only the toggle differs
    pa = bmp_to_png(take_shot(run, hold + split(a.a), a.settle), f"{pre}-a.png")
    pb = bmp_to_png(take_shot(run, split(a.b), a.settle), f"{pre}-b.png")
    _, line, mx = image_stats(pa, pb)
    heatmap(mx).save(f"{pre}-diff.png")
    captioned([pa, pb], [f"A: {a.a}", f"B: {a.b}"], 2, 640).save(f"{pre}-ab.png")
    print(line)
    print(f"{pre}-a.png {pre}-b.png {pre}-diff.png {pre}-ab.png")


def fmt_age(s):
    s = int(s)
    return f"{s // 3600}h{s % 3600 // 60:02d}m" if s >= 3600 else f"{s // 60}m{s % 60:02d}s"


def cmd_ls(a):
    with Lock("alloc.lock"):
        runs = all_runs()
        for r in runs:
            if not game_alive(r) and xvfb_alive(r):
                kill_entry(r["xvfb"])  # stale: game gone, its recorded Xvfb is not
            if r.get("state") in ("running", "starting") and not game_alive(r):
                r["state"] = "dead"
                save_run(r)
    if not runs:
        print("no runs")
    for r in runs:
        st = "alive" if game_alive(r) else "dead"
        print(f"{r['name']} {st} :{r['display']} {r['port']} {r['build']} room={r.get('room') or '-'} "
              f"up={fmt_age(time.time() - r['started'])}")


def cmd_log(a):
    run = load_run(a.name)
    log = Path(run["log"])
    if not log.exists():
        die(f"no log at {log}")
    lines = log.read_text(errors="replace").splitlines()
    if not a.raw:
        lines = [l for l in lines if not any(n in l for n in NOISE)]
    if a.grep:
        rx = re.compile(a.grep)
        lines = [l for l in lines if rx.search(l)]
    print("\n".join(l[:300] for l in lines[-a.n:]))


def cmd_crash(a):
    binary = Path(a.binary) if a.binary else None
    target = a.target
    if target and Path(target).is_file():
        log = Path(target)
    elif target:
        run = load_run(target)
        log = Path(run["log"])
        binary = binary or Path(run["binary"])
    else:
        runs = [r for r in all_runs() if Path(r["log"]).exists()]
        if not runs:
            die("no runs with a log")
        run = max(runs, key=lambda r: r["started"])
        log = Path(run["log"])
        binary = binary or Path(run["binary"])
    binary = binary or resolve_binary(config()["build"])
    text = log.read_text(errors="replace")
    c = find_crash(text)
    if not c:
        die(f"no 'port: crashed:' block in {log}")
    extra = Path(a.binary) if a.binary else None
    main = binary if not (extra and extra.name.startswith("lib")) else resolve_binary(config()["build"])
    print("\n".join(symbolize(c, main, extra)))


def cmd_doctor(a):
    cfg = config()
    bad = 0

    def rep(level, msg):
        nonlocal bad
        bad += level == "FAIL"
        print(f"{level} {msg}")
    for tool in ("Xvfb", "addr2line", "gdb"):
        p = shutil.which(tool)
        rep("OK" if p else ("WARN" if tool == "gdb" else "FAIL"), f"{tool}: {p or 'missing'}")
    binary = resolve_binary(cfg["build"])
    if binary.is_file():
        try:
            v = subprocess.run([str(binary), "--version"], capture_output=True, text=True, timeout=15)
            rep("OK", f"binary {binary}: {(v.stdout or v.stderr).strip()[:80]}")
        except (OSError, subprocess.TimeoutExpired) as e:
            rep("WARN", f"binary {binary}: --version failed ({e})")
    else:
        rep("FAIL", f"default binary missing: {binary}")
    rep("OK" if Path(cfg["disc"]).is_file() else "FAIL", f"disc: {cfg['disc'] or 'unset'}")
    user = Path(cfg["user"])
    rep("OK" if user.is_dir() else "FAIL", f"real user dir: {user}")
    mod = user / "mods" / "remastered-models"
    cur = current_import_version()
    if not mod.is_dir():
        rep("WARN", "no remastered-models mod installed")
    else:
        st = mod / ".import-version"
        have = st.read_text().strip() if st.is_file() else None
        if have is None:
            rep("WARN", f"remastered-models has no .import-version (current kImportVersion={cur})")
        elif cur is not None and str(cur) != have:
            rep("WARN", f"remastered-models .import-version={have}, current kImportVersion={cur}: re-import")
        else:
            rep("OK", f"remastered-models .import-version={have} (kImportVersion={cur})")
    st = os.statvfs("/tmp")
    free = st.f_bavail * st.f_frsize / 2 ** 30
    rep("OK" if free > 5 else "WARN", f"/tmp free: {free:.1f} GiB")
    runs = all_runs()
    n = sum(game_alive(r) for r in runs)
    stale = [r["name"] for r in runs if not game_alive(r)]
    rep("OK", f"live rig runs: {n}")
    rep("OK" if len(stale) < 10 else "WARN", f"dead rig runs: {len(stale)} (mprig.py clean)")
    sys.exit(1 if bad else 0)


def current_import_version():
    h = ROOT / "platform/include/port_remastered_import.h"
    try:
        t = h.read_text()
        stages = dict(re.findall(r"inline constexpr int (k\w+) = (\d+);", t.split("namespace ImportStage {")[1]
                                 .split("}  // namespace ImportStage")[0]))
        m = re.search(r"kImportVersion =\s*(\d+)\s*((?:\+\s*ImportStage::\w+\s*)*);", t)
        return int(m.group(1)) + sum(int(stages[k]) for k in re.findall(r"ImportStage::(\w+)", m.group(2)))
    except (OSError, IndexError, KeyError, AttributeError):
        return None


def cmd_clean(a):
    with Lock("alloc.lock"):
        dead = [r for r in all_runs() if not game_alive(r)]
        dead.sort(key=lambda r: r["started"], reverse=True)
        n = 0
        for r in dead[a.keep:]:
            d = Path(r["dir"]).resolve()
            if d.parent != RIG.resolve():
                continue
            kill_entry(r.get("xvfb"))
            shutil.rmtree(d)
            n += 1
    print(f"removed {n} dead runs, kept {min(len(dead), a.keep)}")


def build_parser():
    p = argparse.ArgumentParser(prog="mprig.py", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="sub", required=True)

    def add(name, fn, help_):
        sp = sub.add_parser(name, help=help_, description=help_)
        sp.set_defaults(fn=fn)
        return sp
    s = add("start", cmd_start, "start Xvfb + game with a console; prints 'name display port log'")
    s.add_argument("name")
    s.add_argument("--build", help="build dir name under build/ or a path (default: rig.ini build)")
    s.add_argument("--disc")
    s.add_argument("--room", help="MLVL[:MREA] hex: boot straight into a room")
    s.add_argument("--env", action="append", default=[], metavar="K=V")
    s.add_argument("--mods", help="mods dir, or 'none' for an empty one (default: the real user's)")
    s.add_argument("--settings", help="port_settings.ini to use, or 'none' (default: the real user's)")
    s.add_argument("--saves", action="store_true", help="copy USA/ and savestates/ from the real user dir")
    s.add_argument("--size", default="1280x720", help="Xvfb screen size WxH")
    s.add_argument("--wait", type=float, default=120, help="seconds to wait for the console")
    s.add_argument("--gdb", action="store_true", help="run under gdb so a crash leaves a backtrace in game.log")
    s.add_argument("--replace", action="store_true", help="stop a running run of this name first")
    s = add("cmd", cmd_cmd, "send console commands (one per argument, or -f FILE); exit 1 if one fails")
    s.add_argument("name")
    s.add_argument("-f", "--file")
    s.add_argument("commands", nargs="*")
    s = add("shot", cmd_shot, "run commands, take a screenshot, write it as PNG")
    s.add_argument("name")
    s.add_argument("out")
    s.add_argument("commands", nargs="*", help="console commands to run first (one per argument)")
    s.add_argument("--crop", help="x,y,w,h")
    s.add_argument("--settle", type=int, default=0, help="frames to wait before the shot")
    s = add("pick", cmd_pick, "which draw is at a window pixel: owner, CMDL, material, record, shader hash")
    s.add_argument("name")
    s.add_argument("x", type=int)
    s.add_argument("y", type=int)
    s.add_argument("--shot", metavar="OUT.png", help="also save a frame with a crosshair at x,y")
    s = add("ab", cmd_ab, "A/B two command sets: <prefix>-{a,b,diff,ab}.png + diff stats")
    s.add_argument("name")
    s.add_argument("prefix")
    s.add_argument("--a", required=True, help="';'-separated console commands for A")
    s.add_argument("--b", required=True)
    s.add_argument("--settle", type=int, default=30, help="frames to wait before each shot")
    s.add_argument("--no-hold", action="store_true", help="do not send 'hold 1' (freeze ticks) first")
    s = add("diff", cmd_diff, "image difference stats (mad psnr changed bbox luma)")
    s.add_argument("a")
    s.add_argument("b")
    s.add_argument("--out", help="write an amplified heatmap here")
    s.add_argument("--fail-above", type=float, metavar="MAD", help="exit 3 if mad exceeds this")
    s = add("sheet", cmd_sheet, "contact sheet with captions")
    s.add_argument("out")
    s.add_argument("images", nargs="+")
    s.add_argument("--cols", type=int)
    s.add_argument("--labels", help="comma-separated captions (default: file names)")
    s.add_argument("--width", type=int, default=480, help="thumbnail width")
    s = add("stop", cmd_stop, "quit the game, then kill only the recorded processes")
    s.add_argument("name", nargs="?")
    s.add_argument("--all", action="store_true")
    add("ls", cmd_ls, "list runs; reaps the Xvfb of dead runs")
    s = add("log", cmd_log, "tail game.log without the noisy lines")
    s.add_argument("name")
    s.add_argument("-n", type=int, default=40)
    s.add_argument("--grep")
    s.add_argument("--raw", action="store_true", help="do not filter noise (" + "|".join(NOISE) + ")")
    s = add("crash", cmd_crash, "symbolize the last 'port: crashed:' block of a run or log file")
    s.add_argument("target", nargs="?", help="run name or log file (default: newest run)")
    s.add_argument("--binary", help="binary to resolve against (also for libmain.so)")
    add("doctor", cmd_doctor, "check tools, binary, disc, user dir, Remastered import stamp, disk")
    s = add("clean", cmd_clean, "delete dead runs under build/rig/")
    s.add_argument("--keep", type=int, default=5)
    return p


def main():
    a = build_parser().parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
