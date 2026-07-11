# The Indev world generator, reverse-engineered to bit-parity

This document describes the fork's port of **Minecraft Indev in-20100223's
world generator** (`LevelGenerator.java` + the generation half of
`World.java`) into C (`src/IndevGen.c`), and the verification harness that
proves the port is **byte-identical to the genuine Java generator** — for the
same seed, world type, theme, and size, the C port produces exactly the same
block array as the original game at every phase boundary.

The ground truth throughout is the deobfuscated in-20100223 source. Line
references in code comments point into it; class names below (`LevelGenerator`,
`NoiseGeneratorOctaves`, `World.generate`, …) are its names.

---

## 1. The fidelity standard

Most "faithful terrain" ports aim for statistical similarity: same biome
shapes, same cave density, roughly the same look. This port holds itself to a
much harder standard:

> For any `(seed, type, theme, width, length, height)`, the generated block
> array must equal the genuine Java generator's output **byte for byte**, at
> **every** intermediate phase, not just the final map.

That standard is what forces every decision documented below — the exact RNG,
the sine *table*, double-vs-float arithmetic, the order of side effects inside
`setBlockWithNotify`, even a macro-expansion bug class. Get any of them wrong
and the outputs diverge; conversely, achieving bit-parity is strong evidence
that no behavior was silently missed.

Verified bit-identical configurations (all 13+ phase snapshots each):

| Config | Seed |
|---|---|
| Island / Normal | 12345 |
| Inland / Normal | 777 |
| Floating / Normal | 4242 |
| Flat / Normal | 555 |
| Island / Hell | 666 |
| Inland / Paradise | 888 |
| Island / Woods (51 tree passes) | 999 |
| Floating / Deep 64×64×256 (5 stacked layers, 29 phase dumps) | 31337 |

## 2. The verification harness

Reproducing the proof requires two halves:

**The Java oracle.** The genuine `LevelGenerator`, `World`, `Light` and
`Block` classes compile standalone (entity/item classes stubbed out). Exactly
three patches were made, all seed-determinism plumbing, none behavioral:

1. `LevelGenerator` seeds its `Random` from the CLI seed instead of
   `System.nanoTime()`.
2. `World.load()`'s `new Random()` becomes `new Random(seed + 1)`.
3. `findSpawn()`'s `new Random()` becomes `new Random(seed + 2)`.

A `Dump` implementation of `IProgressUpdate` snapshots the raw
`blocksByteArray` every time the generator calls `displayLoadingString` —
13 phase dumps per run (more on multi-layer Floating maps). Invocation:
`java -cp out Main <seed> <type> <theme> <W> <L> <H> <outdir>`.

**The C side.** `IndevGen.c` has linux-only test hooks: the environment
variable `CC_INDEVGEN_SEED` forces the seed in `IndevGen_Prepare`, and
`CC_INDEVGEN_DUMP=<dir>` snapshots `Gen_Blocks` at every
`IndevGen_SetState` call using the same phase numbering. A script drives the
actual game UI on a virtual display and a diff script compares the dumps
pairwise (since the genuine-block-id migration, the id remap between the two
sides has shrunk to just the wall-torch metadata variants).

When the dumps match at phase N but differ at phase N+1, the divergence is
inside one pass, and the first differing byte's coordinates usually identify
the exact statement responsible.

## 3. Foundations the parity rests on

### 3.1 java.util.Random, exactly

The engine's `RNGState` (`ExtMath.c`) already implements Java's 48-bit LCG
(`seed * 0x5DEECE66D + 0xB`, high-bit extraction), so `Random_Next(n)` is
`java.util.Random.nextInt(n)` including the rejection loop and the
power-of-two shortcut, and `Random_Float` is `nextFloat()`. This is a
foundation, not a coincidence — everything else depends on consuming exactly
the same number of draws in exactly the same order as the Java.

The most instructive parity bug found lives here: the engine's `min()` is a
macro, and

```c
depth = min(Random_Next(&rnd, h), Random_Next(&rnd, h));  /* WRONG */
```

expands both arguments **twice**, consuming 6–7 draws where Java's
`Math.min(a.nextInt(h), b.nextInt(h))` consumes 4 in strict left-to-right
order. One such line in the lava pass silently desynced the RNG stream for
every pass after "Melting..". All min/max over RNG draws are now written with
explicit temporaries.

### 3.2 Three RNG streams, like genuine

Genuine generation involves **three** `Random` objects, and conflating them
was an early bug:

| Stream | Genuine owner | Drives |
|---|---|---|
| seed | `LevelGenerator.rand` | terrain noise init, carve worms, ore veins, pass positions, plant counts |
| seed + 1 | `World.random`, created in `World.load()` at the end of Assembling (one `nextInt()` burned initializing `randId`) | tree *shapes*, `canBlockStay` pops, item-drop rolls during planting |
| seed + 2 | `findSpawn()`'s own fresh `Random` | spawn-point search only |

With a single stream, the spawn search perturbed the tree and flower
sequence — statistically invisible, fatal for parity. The three streams also
matter at runtime: the gameplay RNGs are time-seeded and must **never** touch
the generation streams.

### 3.3 MathHelper's sine table

Genuine Indev does not call `Math.sin` during generation. It reads a
65 536-entry `float` table:

```java
sin(f)  = SIN_TABLE[(int)(f * 10430.378F) & 0xFFFF];
cos(f)  = SIN_TABLE[(int)(f * 10430.378F + 16384.0F) & 0xFFFF];
```

The table quantizes angles to 1/65536 of a turn, so a cave worm's heading
drifts slightly differently than with libm `sinf` — enough to flip cells at
cave boundaries. The port builds the same table.

Two traps inside this one item:

- The table entries must be computed as
  `(float)StrictMath.sin(i * PI * 2 / 65536)` in **double**, with a true
  double π. The engine's `MATH_PI` is a *float* literal and its `Math_Sin`
  is a float-precision approximation — using either gives entries off by
  1 ulp, observed as a single flipped cave-boundary cell on seed 777.
- glibc's `sin` was verified to match OpenJDK's `StrictMath.sin`/`Math.sin`
  bit-exactly across all 65 536 inputs, so the table can be built with
  `__builtin_sin` at startup rather than shipping 256 KB of constants.

### 3.4 Double vs float discipline

The decompiled Java mixes `float` and `double` arithmetic in ways that look
accidental but are load-bearing once an `(int)` cast is nearby. Rules ported
verbatim:

- The island edge falloff and Floating cliff cut run `Math.abs`,
  `Math.sqrt`, `Math.signum` in **double**, including the quirky
  `(double)1.2F` constant (i.e. the double closest to the float 1.2, not
  1.2 itself).
- Wherever genuine writes `(float)Math.PI` the port uses `3.1415927f`, and
  wherever a chain stays float it must not be "improved" to double.
- Perlin noise is evaluated entirely in double, exactly like
  `NoiseGeneratorPerlin`.

Compiling with `-ffast-math` would break all of this; the build does not.

## 4. The noise stack

Three classes, ported one-to-one:

- **`NoiseGeneratorPerlin`** — classic improved Perlin with a permutation
  table shuffled by `rand.nextInt(256 - i) + i` swaps (the shuffle consumes
  RNG draws, so even *constructing* generators in the right order matters).
  Genuine samples 3D noise at z = 0 for the 2D uses.
- **`NoiseGeneratorOctaves`** — N stacked Perlins, amplitude and frequency
  doubling per octave.
- **`NoiseGeneratorDistort`** — a two-generator "domain distortion" wrapper:
  `source.generate(x + distort.generate(x, y), y)`.

The Raising pass alone constructs two Distorts (each wrapping two octave-8
generators) plus an octave-6 and an octave-2 — roughly 130 double-precision
Perlin evaluations per column, which is why Indev generation is (genuinely!)
slow compared to Classic's.

## 5. The pipeline, pass by pass

Passes run in genuine order under genuine progress-banner names. World types:
Island / Inland / Flat / Floating; shapes: Square / Long (w/2 × 2w) / Deep
(w/2 square × 256 tall); themes: Normal / Hell / Paradise / Woods.

For Floating worlds the terrain passes loop once per 48-block layer
(`layers = (height - 64) / 48 + 1`), with per-layer
`waterLevel = height - 32 - layer*48` and `groundLevel = waterLevel - 2`.

1. **Raising..** — the heightmap: distorted octave noise pairs blended by a
   third noise, island edge falloff in double precision, Flat worlds skip
   straight to an all-zero heightmap.
2. **Eroding..** — a second pair of distort noises re-carves the heightmap.
3. **Soiling..** — stone fill, dirt depth from noise, and the Floating
   cliff cut that hollows the underside of islands.
4. **Growing..** — surface capping: sand/gravel beaches from two octave-8
   noises (including Hell's odd grass-beach quirk), grass on top.
5. **Carving..** — cave worms: random-walk ellipsoid tunnels (radius up to
   ~4.7, y-squashed), headings driven by the sine table. **Ore veins run
   under this same banner** (genuine has no "Mining.." phase): coal
   1000/10, iron 800/8, gold 500/6, diamond 800/2, each with per-ore height
   caps (4h/5, 3h/5, 2h/5, h/5).
6. **Melting..** — lava pockets; the pocket depth uses a min of two draws
   biased toward the floor (the `min()` macro war story above).
7. **Watering..** — two flood stages: theme springs (closed air pockets
   under 640 cells, probed by flood-filling to a sentinel and reverting if
   too large) and the edge ocean flood at `waterLevel - 1` from every border
   column (lava instead of water on Hell).
8. **Theme environment** — sky/fog/cloud colors and sky brightness
   (Hell 7, Woods 12, Paradise 16, Normal 15), cloud height (height + 2;
   −16 on Floating).
9. **Post-terrain level adjustment** (genuine, easy to miss): Floating sets
   `groundLevel = -128, waterLevel = -127`; non-Island sets
   `groundLevel = waterLevel + 1` then `waterLevel = groundLevel - 16`;
   Island sets `groundLevel = waterLevel - 9`. These adjusted values drive
   everything after — the border shell, the spawn search, and the
   out-of-bounds horizon planes.
10. **Assembling..** — `World.generate()`: rebuilds the floor and border
    shell. Interior columns only touch y=0, y=1 and the top layer — via the
    genuine **y-skip quirk** (`if (var7 == 1 && interior) var7 = height-2`),
    a loop-variable jump the port reproduces exactly. Border columns are
    rebuilt in full: bedrock below `groundLevel-1` (still lava at y≤1 under
    air), a **grass/dirt cap at `groundLevel-1`** (grass iff
    `groundLevel > waterLevel` and the default fluid is water — so Inland
    borders are grass-capped, Island borders submerged dirt), then the
    default fluid up to `waterLevel`. The fluid band uses the **moving**
    liquid ids (8/10), not the still ones — another parity bug found the
    hard way. On Floating maps `groundLevel = -128` makes every branch
    miss, which is precisely what hollows the empty basin under the
    islands. Writes are unconditional, overwriting terrain, like genuine.
11. **Building/Planting** — grass, trees, flowers, mushrooms. This stage
    runs through a **replica of `World`'s runtime semantics** (`WR_*` in
    `IndevGen.c`) rather than raw array writes, because genuine plants via
    `setBlockWithNotify` on a live `World`:
    - **The light snapshot.** `World.generate()` performs a one-time light
      init: a heightmap scan by `lightOpacity` (water 3, lava 255, leaves 1,
      opaque 255), then per-cell light = theme skylight if `y ≥ heightMap`
      else 0, maxed with the block's own `blockLightValue`. Crucially, light
      *updates* after this init are only queued (the queue is not pumped
      until the Lighting phase) — so every planting decision reads this
      static snapshot. A fresh tree canopy therefore does **not** darken
      the ground under it during planting, and flowers happily generate
      under brand-new trees. A genuine quirk, faithfully preserved.
    - **`setBlock` refuses the outermost shell** (border columns are
      immutable during planting).
    - **`setBlockWithNotify` side effects** that fire during generation are
      replicated: sand/gravel `tryToFall` (with genuine's coordinate-
      *clamping* out-of-range reads, and blocks that fall out of the world
      vanishing), flowers/mushrooms popping when `canBlockStay` fails
      (burning 4 `World.random` floats each — the draws matter), still
      liquids waking to moving liquids (canFlow + sponge scan), and
      water/lava contact turning to stone.
    - `growGrassOnDirt` converts **every** lit dirt cell in the volume, not
      just surface cells. Flowers use genuine `canBlockStay` (light ≥ 8, or
      ≥ 4 with sky access); mushrooms need light ≤ 13 on an opaque block.
    - Tree *positions* come from the generator stream; tree *shapes* (trunk
      height, canopy rolls) come from `World.random` — the stream split
      of §3.2.
12. **findSpawn** — up to 1 000 000 attempts from its own RNG stream:
    `x ∈ [w/4, 3w/4)`, `z ∈ [l/4, 3l/4)`, `y = firstUncovered + 1`;
    rejects `y < 4` and `y ≤ waterLevel`; requires a clear 7×4×9 volume
    (x±3, y−1..y+2, and the genuinely *asymmetric* z−5..z+3 window) and a
    fully opaque 7×9 floor layer at y−2, both tested with genuine's
    clamped out-of-range reads. On exhaustion, genuine sky-spawns at
    `height + 100` (you skydive onto Floating islands — authentic; the
    port's default lands on the sampled column instead, a documented
    deviation).
13. **generateHouse** — the 7×5×7 "house of paradise": stone shell, plank
    roof/floor bands, obsidian centre slab, doorway on −Z, two wall
    torches (mounted with genuine `onBlockPlaced` metadata).
14. **Lighting.. / Spawning..** — announced for phase parity (the engine
    relights with its own system; genuine's 10 000 `updateLighting` rounds
    are unnecessary here). Post-load, the 1000-pass `MobSpawner` population
    fills the map with mobs, and the player spawn applies genuine
    `preparePlayerToSpawn` semantics: Indev `posY` is the bounding-box
    centre, so feet = `ySpawn − 0.9` (0.1 above the house floor).

### After generation: the surroundings

Genuine Indev renders no border *walls*, but `RenderGlobal` draws infinite
**out-of-bounds horizon planes**: ground at `groundLevel` (grass when the
world is dry land with water as its fluid, dirt otherwise) and the default
fluid at `waterLevel`. The generator records
`groundLevel/waterLevel/defaultFluid` ("the surroundings"), which are mapped
onto the engine's edge/sides planes at map load, and round-trip through
`.mclevel`'s `SurroundingGroundHeight/SurroundingWaterHeight/
SurroundingWaterType` tags (`SurroundingGroundHeight` is a **signed** short —
genuine Floating maps store −128). Result: Flat/Inland worlds are ringed by
an endless grass plane, Islands by ocean, Floating by void.

## 6. Genuine quirks preserved on purpose

These look like bugs. They are genuine, verified in the Java, and kept:

- The Assembling y-skip (`var7 = height - 2` mid-loop) leaving interior
  columns untouched between y=2 and the top layer.
- Flowers generating in the dark under fresh tree canopies (stale light
  snapshot).
- Hell's grass beaches.
- The asymmetric z−5..z+3 spawn clearance window.
- The `(double)1.2F` constant and every other float/double oddity.
- `findSpawn` exhaustion → skydive from height+100 (genuine behavior;
  see deviations for what the port does by default).
- The ore passes running under the "Carving.." banner.

## 7. Documented deviations

Everything the port *knowingly* does differently:

- **Spawn-search exhaustion** lands the player on the sampled column
  instead of genuine's `height + 100` skydive (graceful; the genuine
  behavior can kill you on arrival).
- **floodFill's stack**: genuine uses segmented 1M-int arrays; the port
  uses a growable stack (identical fill order and results).
- **Lighting**: the engine's lighting system replaces genuine's
  10 000-round update pump; block arrays are unaffected.
- **Mob population** is entities-only and runs post-load (entities are not
  part of the block-array parity domain).
- On cooperative-threading platforms (web), generation runs monolithically
  in one call — fine on desktop, would hitch in a browser.

## 8. Performance notes

Genuine Indev generation was slow in 2010 and the faithful port is slow for
the same reason: the Raising/Eroding/Soiling/Growing stack evaluates on the
order of 130 double-precision Perlin samples per column (Classic's NotchyGen
does 10–20). Measured on a slow software-rendered VM: Small Inland ≈ 2 s,
Normal Island ≈ 10–15 s.

Fidelity-safe optimization directions, none applied yet:

1. Specialize Perlin to true 2D — genuine samples 3D at z = 0, so one lerp
   arm is dead weight (~40% fewer ops, bit-identical results).
2. Evaluate each octave layer across the whole heightmap in one
   cache-friendly sweep instead of per-column call trees.
3. Multithread the per-column loops (noise evaluation is pure table
   lookups; only generator *construction* consumes RNG and needs ordering).
4. Precompute the erosion noises (sampled at (x≪1, z≪1) — a quarter of the
   samples repeat).
5. Early-out the edge-ocean flood when the start cell isn't air.

## 9. File map

| File | Role |
|---|---|
| `src/IndevGen.c` | The entire generator: noise stack, passes, `WR_*` world replica, spawn/house, test hooks |
| `src/IndevGen.h` | Setup entry points (type/theme) used by the generation UI |
| `src/IndevTest.c` | Surroundings storage/apply, theme env colors, genuine block definitions |
| `src/Formats.c` | `.mclevel` load/save incl. the Surrounding* env tags |
| `SURVIVAL_TEST_NOTES.md` | The session-by-session engineering log (search "Generator round") |
