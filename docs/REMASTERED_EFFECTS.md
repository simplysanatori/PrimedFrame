# Remastered particle effects (GENP)

Metroid Prime Remastered stores its particle effects as `GENP` assets (1434
unique in the romfs). They replace retail's `PART` and fold its separate
swoosh, electric, weapon, collision and decal assets into the same file. This
page describes the format as far as `platform/port_remastered_effect.cpp`
reads it, how it maps onto retail, and what does not parse yet. Nothing in the
game loads these yet: the converter writes retail PART, but the import does
not call it yet.

## Container

- RFRM header (`RFRM`, then `GENP` at 0x14), with the root generator at 0x3c.
  A `FOOT` form follows the data and is ignored.
- FourCCs inside the form are stored byte-reversed (`CNST` is the bytes
  `TSNC`), so a little-endian u32 read gives the packed big-endian value.
- A generator is `GPSM` plus a 21-byte header. The u32 at +21 is 1 for the
  root. Then comes a flat list of properties: FourCC, a **tag byte**, value.
- **Tag byte**: when the value is evaluated. `00` once (static: flags, modes,
  textures), `01` per emitter (rates, counts), `03` per particle (colour,
  size, velocity, lifetime). `04` marks `_END`. It agrees with how retail
  evaluates each property; retail just did not store it.
- The root's `_END` is followed by a u32 count (at most 64), then that many
  children, each a 16-byte id and a form:
  - `GPSM`: a child generator. These replace retail's ICTS/IDTS/IITS/KSSM
    child PART ids. When the child was a retail PART, its id is that PART's id.
  - `SWSH` ≈ SWHC, `ELC2`/`ELSM` ≈ ELSC, `WPSM` ≈ WPSC, `CRSM` ≈ CRSC,
    `DPSM` ≈ DPSC, plus `EPSM` and `SPSM` (no retail equivalent found).
  - A child's own `_END` is a plain FourCC and tag.

## Ids

Ids inside effects are little-endian UUIDs. `EffectGuidString` prints them
that way. The pak reader prints ids in stored order (`IdToString`), so to look
one up in a pak, swap bytes 0↔3, 1↔2, 4↔5 and 6↔7. Assets carried over from
retail use `10000000-0000-f000-f000-0000XXXXXXXX` (in pak order), where
`XXXXXXXX` is the retail asset id. Some effects keep that id ("exact pairs",
80 GENPs). The rest got fresh UUIDs but often kept retail's name (the gun
effects: `PowerMuzzle`, `WaveCharge`...).

References from the 1306 parsed effects (5430 in all):

| Resolves to | Count | Notes |
|---|---|---|
| MATI | 2563 | Material instance. All 2317 distinct ones name an MTRL, and 2259 also bind a TXTR. |
| (no asset) | 2035 | Parameter/variable ids (VARF/DPVF/TPVF..., SEMR arguments) |
| CMDL | 665 | Particle models (PMDL) |
| VECF | 87 | Vector fields |
| TXTR | 79 | TEXR/TIND, mostly retail TXTR ids in the carried-over form |
| SWSH | 1 | |

## Grammar

Nothing records an element's arity or a value's length. One FourCC means
different things in different slots: `CNST` holds an int, a float, three or
four elements, a byte or an id. So the reader is a memoised backtracking parse
that keeps the reading that lets the file parse to its last `_END`. The
signature table (`kElementSigs`) is retail's element arities merged across
slots, plus Remastered's changes. Properties default to one of `e`, `b`,
`b g`, `g` or `b e`, with overrides for TEXR, SMVR, SORT, FRMD, PSPS, MTIN,
SNRD and SNRA. KSSM (spawn table), PVAR, TMTR, PMTR and SMTR have dedicated
readers. GRAD, ARRY and KEWS are read as raw blocks.

The untyped table alone lets nested values be read across their neighbours (a
vector's `CNST` taking four arguments and eating the next one, or three nested
`CNST`s read as an id). So a property retail knows is read first as the type
retail reads it as (int, real, vector, mod vector, colour, emitter), from
per-type tables (`kIntSigs` and the rest: retail's elements and arguments plus
Remastered's MPCB, MPAC, ANCR, ASPR, RNDV, DFCP/DFCS, MPRD and the parameter
reads). Inside a typed value every argument is typed in turn, so a vector's
`CNST` is exactly three reals and a real's `CNST` one word. An element the type
does not list is read from the untyped table; one it lists is only read typed.
The whole property falls back to the untyped defaults when the typed reading
does not parse, so typing only ever chooses between readings that already
parsed. Remastered's flags are a `CNST` and a byte (IMPL/LMPL/EMPL/BNCE's
last argument, ELPS, ATEX). Emitter shapes pinned from the shipped bytes:
`PLNE(v, v, v, r, r, r)`, `ELPS(v, v, v, r, flag)`,
`PLNV(v, v, r x8, byte)`; colour `MDAO(c, r)` and `SLCT(r, ARRY)`;
`SMOV(EXTT(v), EXTR(NONE, NONE))`. A 16-byte id holding an element FourCC
is not read as an id.
Unknown elements try their larger arities first when building the tree.

New elements, with arities known:

- **SEMR** replaces SETR as the emitter.
- **MPCB** wraps every vector: either a cartesian vector, or `MPAC` angles
  plus a radius.
- **MPRD** (random int), **RADD** and **REUL** (rotation), **PEOD**, **SPAx**,
  and the **DPVx/TPVx/VARx** parameter reads.

Elements whose arity is only inferred are listed in `kLooseElements`.

## Mapping to retail PART

Compared property by property over the 67 exact pairs that have a retail
PART on the disc:

- **Direct**: value identical, or the same shape with different numbers.
  - 176 property values were byte-identical, 20 kept their shape but changed numbers.
  - TEXR/TIND: retail `CNST(CNST(id))` becomes `CNST(id), NONE`, with the
    same TXTR id.
  - LTME becomes **LTM2**, one frame longer: across the 47 exact pairs that
    convert, LTM2 is always the disc's LTME + 1.
- **Approximate**: same meaning, re-encoded.
  - EMTR: SETR becomes SEMR. VEL1-3, PMOP, PMRT and the emitter's vectors
    are wrapped in MPCB; stripping it gives retail's vector.
  - ROTA is negated: `SCAL(5)` becomes `MULT(SCAL(5), -1)`, and 180 becomes -180.
  - LFOT and LTYP: int CNST becomes an enum byte, shifted by one (LTYP 1 →
    `#02`, LFOT 2 → `#03`).
  - PSLT loses its inline PSTS, which becomes its own property.
  - Child PART ids (ICTS/IDTS/IITS/KSSM) become embedded children.
- **Dropped**: properties retail stored at their default. Of the omissions,
  983 were `CNST #00` and 391 were `NONE`. Remastered omits defaults; nothing
  is lost.
- **No retail equivalent**:
  - On every effect: **DVVN** (byte, always `#04`), **IEXP** (`#01`),
    **XFMD** (int, e.g. 5).
  - On about half: **PBDM** (int, blend mode?).
  - Rarer: PSTS, INTT, ITEN, INTR, LIRD, LORD, SSZE, SMVR, SCTR, FXBR, DBIS,
    SBIS, VGD2/VGD3, MTIN (byte plus MATI id: a material instead of a bare
    texture), PVAR/TMTR/PMTR/SMTR (parameter tables).
  - These need a renderer feature or a default when converting.

## Coverage

1306 of 1434 unique GENPs parse (91.1%); 1309 of 1437 counting duplicate
copies. On the 904-file reference set, the C++ reader and the Python prototype it
was ported from agree on every one of the 842 both parse. The 128 failures, by the property where the parse stops:

| SMVR | EMTR | TEXR | ZBUF | SMTR | KSSM | VEL2/3 | SIZE/COLR | other |
|---|---|---|---|---|---|---|---|---|
| 39 | 35 | 10 | 7 | 7 | 7 | 6 | 6 | 11 |

The `scan` command prints each failure with its offset and the bytes there;
those are the grammar gaps to close next.

11 of the failures are exact pairs. The gun effects parse except `IceCharge`,
`PlasmaCharge`, `Plasma2nd_1` and `PowerBombExplo`.

## Converting to retail PART

`platform/port_remastered_effect_convert.cpp` writes a parsed effect as retail
PART. It is driven by retail's reader (`CParticleDataFactory`): each property
retail knows is written as the type retail reads it as, and each element in it
must be one retail has for that type, with retail's arguments. On the way it
undoes the re-encodings above: LTM2 is written as LTME less one, LFOT/LTYP go back down
by one, MPCB is stripped (cartesian form only), ROTA is negated back (a
`MULT(x, -1)` becomes `x`, anything else is wrapped in one), TEXR's
`CNST(id), NONE` becomes `CNST CNST id`, an MTIN stands in for a missing TEXR
through its material's texture, words and keyframe blocks are byte-swapped,
and ids become retail ids through a callback (by default only the ids carried
over from retail resolve).

What does not convert is left out and listed: Remastered-only properties, an
element retail does not have in that slot (RADD, REUL, MPRD, the parameter
reads...), MPCB's angle form, an id with no retail id, and KSSM (Remastered's
spawn table is a different layout). Retail then uses its default for the
property. `droppedRetail` counts the left-out properties retail does read, so
a caller can skip effects that lose something that matters.

Embedded GPSM children come out as PARTs of their own under their child ids.
The swoosh, electric, weapon, collision and decal children are not converted
yet, so SSWH/SELC on a converted effect only survive when their id is a retail
one.

Keyframe blocks keep retail's layout; a colour's keys may be four halves
(8 bytes), which are widened to floats. KSSM is retail's too, but its spawn
table is laid out differently and is not converted yet.

`SplitRetailPart` reads a retail PART back the same way, property by property.
The tests use it to check every converted PART is one retail's reader takes,
and `effect_tool convert` uses it to compare converted effects with the disc's.
Run on the disc's own PARTs it also checks the type tables against real files:
all 3202 PARTs on the US disc split.

First run on the shipped files (before the LTME, ROTA, KSSM and colour-key
fixes): of 1306 effects, 100 converted with no retail property left out. Most
of what is left out is Remastered-only. Of what retail reads, the emitter is
the big loss (EMTR left out of 1520 generators), from elements retail does not
have there: DFCP (1238 times), MPCB's angle form (485), ASPR (401), GRAD
(378), ANCR (249) and MPRD (202), and CNST with one argument where retail
reads a vector (378) or three where it reads a colour (292). Those are the
next things to map. Some values differ from the disc on purpose: MAXP and
SIZE were changed in a few effects.

From the second run's samples:

- The "CNST with one argument" and "three arguments in a colour" cases were
  the reader taking two nested constants (`CNST(CNST(0), ...)`) as a 16-byte
  id, the shorter reading. An id no longer starts with an element's FourCC.
- `MPCB(MPAC(xb, yb, xr, yr), m)` is retail's `ANGC(xb, yb, xr, yr, m)`
  (the disc writes the same values under `IVEC(ANGC(...))`). MPCB takes one
  or two elements, never none: with none allowed it read as a bare MPCB
  followed by a four-element CNST.
- `ANCR(REUL(0, 0, 0, #00), xr, yr, m)` is `ANGC(-0, -0, xr, yr, m)` on the
  disc. ANCR with a rotated cone is not converted yet, nor is ASPR.
- MPRD (2 or 4 elements) is a random int; its two-element form is RAND. In
  LTM2, the bounds of RAND/IRND/MPRD come down by one each, as the disc's
  IRND shows.
- DFCP (2 or 3 elements) and DFCS (3) scale a size, colour or speed by
  something retail has no element for; they are written as 1 and listed as
  approximated.
- Colour keyframes are four halves per key in Remastered (8 bytes; scalars
  stay 4-byte floats). Widened, 7 of the 17 compared COLR curves match the
  disc to half precision; the other 10 were retuned. The disc also sets the
  header's second flag byte where Remastered leaves it 0 in 8 of them;
  retail does not use it.
- GRAD (colour gradients, children only) is not converted yet.

## Tools

`tests/port_remastered_effect_tool.cpp` is a dev tool, not built by CMake:

```
g++ -std=c++20 -O2 -Iplatform/include tests/port_remastered_effect_tool.cpp \
    platform/port_remastered_effect.cpp platform/port_remastered_effect_convert.cpp platform/port_remastered_pak.cpp -lzstd -o effect_tool
./effect_tool dump <file.GENP>             # one effect as text
./effect_tool scan <romfs> [outdir]        # coverage, references, failures;
                                           # outdir gets one dump per effect
./effect_tool convert <romfs> <retail|-> <outdir>
                                           # every effect as retail PART, what
                                           # was left out, and (with a folder of
                                           # the disc's <id>.PART) a comparison
```


`tests/port_remastered_effect.cpp` (`port_remastered_effect_tests`) checks
the reader on a synthetic effect, and `tests/port_remastered_effect_convert.cpp`
(`port_remastered_effect_convert_tests`) checks the converter against
hand-written retail bytes.
