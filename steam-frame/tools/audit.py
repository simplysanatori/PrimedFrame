#!/usr/bin/env python3
# Audits an unpacked release (the "app" folder) for release.sh, alongside check-bundle.sh:
#   - the newest glibc, libstdc++ (GLIBCXX) and C++ ABI (CXXABI) symbol version any ELF file in it asks for;
#   - every library an ELF file needs that the app does not carry itself and that some SteamOS build listed in
#     tools/host-sonames/ lacks ("NOT ON ALL BUILDS").
# release.sh reads the "max ..." and "NOT ON ALL BUILDS" lines.
#   steam-frame/tools/audit.py <app dir> [host-sonames dir]
import glob
import os
import re
import subprocess
import sys

root = sys.argv[1]
lists_dir = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), 'host-sonames')
snaps = sorted(glob.glob(os.path.join(lists_dir, '*.txt')))
if not snaps:
    sys.exit(f'audit: no SteamOS soname lists in {lists_dir}')
sets = {os.path.basename(s): {l.strip() for l in open(s) if l.strip() and not l.startswith('#')} for s in snaps}


def readelf(*args):
    return subprocess.run(['readelf', *args], capture_output=True, text=True).stdout


elfs, bundled = [], set()
for dp, dn, fn in os.walk(root):
    for f in fn:
        p = os.path.join(dp, f)
        if os.path.islink(p):
            bundled.add(f)
            continue
        if not os.path.isfile(p):
            continue
        with open(p, 'rb') as h:
            if h.read(4) != b'\x7fELF':
                continue
        elfs.append(p)
        bundled.add(f)
        m = re.search(r'\(SONAME\).*\[(.*)\]', readelf('-d', p))
        if m:
            bundled.add(m.group(1))

maxv, missing = {}, {}
for p in elfs:
    need = re.findall(r'\(NEEDED\).*\[(.*)\]', readelf('-d', p))
    v = readelf('-V', '-W', p)
    for lib, pat in (('GLIBC', r'GLIBC_(\d+)\.(\d+)'), ('GLIBCXX', r'GLIBCXX_3\.4\.(\d+)'), ('CXXABI', r'CXXABI_1\.3\.(\d+)')):
        vs = [tuple(map(int, x if isinstance(x, tuple) else (x,))) for x in re.findall(pat, v)]
        if vs:
            mv = max(vs)
            if lib not in maxv or mv > maxv[lib][0]:
                maxv[lib] = (mv, os.path.relpath(p, root))
    for n in need:
        if n in bundled:
            continue
        if any(n not in s for s in sets.values()):
            missing.setdefault(n, set()).add(os.path.relpath(p, root))

print('ELF files:', len(elfs))
for k, (v, p) in maxv.items():
    print(f'  max {k}: {".".join(map(str, v))}  ({p})')
for n, ps in sorted(missing.items()):
    absent = [k for k, s in sets.items() if n not in s]
    print(f'  NOT ON ALL BUILDS: {n} (absent {absent}) <- {sorted(ps)[:3]}')
