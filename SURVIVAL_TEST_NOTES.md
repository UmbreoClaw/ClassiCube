# Classic 0.30 Survival Test — Project Notes & Handoff

Branch: `claude/c030-s-gamemode-8fpmns`

This file is the living context/handoff for the c0.30-s survival gamemode recreation.
Goal: a **faithful from-scratch recreation** of Minecraft Classic Survival Test (0.30),
cross-referenced against the Minecraft Wiki, that does **not** disturb creative mode.

---

## NEXT TASK (agreed — start here next session)

**Physical dropped-item entities + drop table correction: DONE this session.**

Implemented in `src/SurvivalTest.c` (+ one hook in `src/Game.c`). Summary of what
landed, so the next session knows where things stand:

- **Dropped-item entity** (`struct DropItem`, fixed pool `st_drops[DROP_MAX=64]`).
  Each wraps a plain `struct Entity` using the engine's existing `Models.Block`
  model (the same "render entity as a floating block" model used elsewhere) —
  this gave faithful **full-size block pixels** for free, no custom mesh needed.
  Entities are **not** added to `Entities.List[]` (that array is player-shaped and
  network-synced); drops are simulated/rendered entirely inside SurvivalTest.c via
  manual calls to `Model_Render(Models.Block, &d->entity)`.
- **Physics**: simple custom gravity integrator (`SurvivalTest_DropPhysics`,
  `DROP_GRAVITY = 20 blocks/s²`, terminal velocity clamp), random scatter-pop
  velocity on spawn (`SurvivalTest_SpawnDrop`), settles via friction once it lands
  on a solid block top (`SurvivalTest_DropGroundY` samples `Blocks.Collide`/`MaxBB`).
  No horizontal wall collision (acceptable simplification — items can clip slightly
  into block faces, not noticeable in practice).
- **Pulsing white look**: custom `DropItem_GetCol` VTABLE callback returns
  `PackedCol_Scale(PACKEDCOL_WHITE, 0.7 + 0.3*sin(age*6))` instead of normal world
  lighting, so drops visibly pulse regardless of ambient light — matches the
  "items pulse white" research note.
- **Pickup**: `SurvivalTest_DropTryPickup` does a simple squared-distance check
  (`DROP_PICKUP_RADIUS = 1.0` block) against the player each tick once
  `pickupDelay` (0.5s) has elapsed; calls the existing `SurvivalTest_AddBlock`.
  Ticking happens inside the existing 20Hz `SurvivalTest_Tick` via
  `SurvivalTest_TickDrops`, no second `ScheduledTask` needed.
- **Wire-up**: `SurvivalTest_BlockChanged` now calls `SurvivalTest_SpawnDropsForBlock`
  on mining (was: direct `SurvivalTest_AddBlock`). `AddBlock` itself is unchanged
  and still used by the pickup path and by `SurvivalTest_TryEat`'s mushroom logic.
  Rendering hooked in once: `Game.c`'s `Render3DFrame` calls
  `SurvivalTest_RenderDrops(delta, t)` right after `Entities_RenderModels`.
  `st_drops[]` is cleared in `SurvivalTest_ResetState` (new level / fresh start).
- **Drop table corrected** (`SurvivalTest_SpawnDropsForBlock`): most blocks drop
  themselves (removed the old stone→cobble and ore→ingot mappings); grass→dirt;
  leaves→sapling only **1/10** of the time (9/10 nothing drops); logs→**3–5**
  planks (`BLOCK_WOOD`, multiple drop entities spawned with individual scatter).
- Verified: every `.c` file in `src/` (whole project, not just survival files)
  compiles clean with `make PLAT=linux` (`-Werror`) — only the known link failure
  from this container missing `-lXi`/`-lGL` remains, no new compiler warnings.
  Not yet tested in a running game (no display in this container) — worth an
  in-game pass next session: confirm visually the pulse/scatter/landing/pickup
  feel right, and that performance is fine when many blocks are mined quickly.

### Possible follow-ups (not done, not asked for yet)
- No despawn timer for unpicked drops (original likely didn't have one either,
  low priority).
- `DROP_MAX = 64` pool: oldest-undropped silently skipped once full; fine for now.

### Block-breaking time (CLARIFIED — c0.30-s DID have a break timer)
Correction (per user, 2026-06): Survival Test blocks did **not** break instantly —
there was a **hold-to-break timer**, but it was a **single uniform duration for every
block** (no per-block hardness — dirt, stone, etc. all took the same time). What is
specifically *Indev* (Feb 2010) is **per-block hardness** (different blocks taking
different times). The cracking/destroy-stage overlay also belongs to that later lineage.

So a **uniform break time** is faithful to c0.30-s and is currently a **divergence**:
ClassiCube breaks instantly (creative behaviour). Building a single fixed hold-to-break
timer (same for all blocks, no crack overlay needed) would be the faithful fix.
Not yet implemented — not started without an explicit go-ahead.

---

## CURRENT STATE (what's implemented & pushed)

Files: `src/SurvivalTest.c`, `src/SurvivalTest.h`, plus hooks in `src/Game.c`,
`src/Options.h`, `src/InputHandler.c`, `src/Screens.c`, `src/Screens.h`,
`src/ClassiCube.vcxproj`, and CI in `.github/workflows/build_survival_ci.yml`.

- **Enable flag**: `survival-mode=True` in `options.txt` (must be `True`/`False`, NOT
  `1`/`0`; edit while the game is closed). Read once at init via
  `Options_GetBool(OPT_SURVIVAL_MODE, false)`.
- **Health**: 20 HP (10 hearts, half-heart granularity). No natural regen (faithful).
- **HUD**: hearts above the hotbar (icons.png), half-heart support, low-health shake
  at ≤4 HP, hotbar stack-count digits.
- **Inventory**: real 36-slot model (`st_inv[]`), hotbar mirrored into engine inventory.
  Survival inventory screen (`SurvivalInvScreen`) — solid dark panel, bordered slots,
  separator line, "Inventory" title; click to pick/swap stacks.
- **Block handling**: mining spawns physical dropped-item entity/entities on the
  ground (faithful drop table, see below); walking near one picks it up into
  inventory. Placing consumes one from the selected slot. Creative-safe:
  `SurvivalTest_CanPlace()` returns true when disabled.
- **Drop visuals**: drops render as a **small 0.25-block cube** (matching the
  decompiled `ItemModel` — see research note), textured with only the **middle 50%**
  (texels 4..12 of 16) of the block's tile on **every face**. They **spin** (3°/tick),
  **bob** (~1 Hz), are **world-lit**, and get a brief **white glint** (~1×/sec).
  All in `SurvivalTest.c`: the textured cube is built by hand in `DropItem_BuildItemCube`
  (rotated XZ corners via `DropItem_RotatedCorners`, UV cropped from `Atlas1D_TexRec`,
  drawn per-1D-atlas like the terrain particles into the single dynamic VB `st_itemVB`,
  recreated on `GfxEvents.ContextLost`). (Switched from the earlier full-size
  `Models.Block` approach.)
  - **Glint = single-pass colour lerp toward white** (`DropItem_GlowAmount`,
    `PackedCol_Lerp(litCol, WHITE, (sin(var3/10)*0.5+0.5)^4 * 0.5)`). The original did
    an *additive* second white pass; the engine exposes no additive blend, and the
    earlier attempt (a separate alpha-blended white *shell* cube) read as a boxy
    translucent overlay + double-blended its faces — visible artifact. The lerp is a
    clean approximation: no overlay, no z-fighting, flashes the item itself.
- **HUD hearts**: left-aligned to the hotbar's left edge (matches c0.30-s), not centred.
- **HUD stack counts**: digits drawn at the natural font size (like the inventory
  screen) but anchored to each slot's block-icon **bottom-right**
  (`HUDScreen_BuildCountsMesh`). The original fullscreen bug was *positioning*
  (left-aligned/detached), not size — manual glyph scaling overshot and was reverted.
- **Damage**: fall (peak-tracking, `floor(dist)-3`, ~1 HP/block past 3 safe blocks),
  lava (4 HP / 0.5s), drowning (2 HP/s after 15s air), 0.5s invincibility frames.
- **Mushrooms**: right-click to eat — brown +5 HP, red −3 HP poison (`SurvivalTest_TryEat`).
- **Death**: faithful **"Game over!"** screen (permadeath, no respawn) with
  "Generate new level..." and "Quit game". `GameOverScreen` in `src/Screens.c`.
- **Hacks**: fly/noclip/speed disabled in survival via `OnNewMapLoaded` +
  `HacksComp_Update` (faithful — c0.30-s had no hacks).
- **CI**: `.github/workflows/build_survival_ci.yml` cross-compiles Win32/Win64
  (D3D9/OpenGL/D3D11) with `-Werror` on push and uploads `.exe` artifacts
  (ClassiCube-SurvivalTest-Win32/Win64, 14-day retention).

### Decisions made
- Combat/mobs: **deferred**. ClassiCube has mob *models* but no mob system (no spawn/AI,
  no mob entity type). Focus on simpler mechanics first.
- Death: **faithful Game Over / permadeath** (chosen over keeping respawn).

---

## RESEARCHED c0.30-s FACTS (reference)

Confidence noted; Survival Test is lightly documented, much reconstructed from wiki
per-version pages.

### Health & damage
- 20 HP = 10 hearts, half-heart units. Heart bar **shakes at ≤4 HP**.
- Fall: safe ≤3 blocks; **~1 HP per block beyond 3** (`floor(dist) - 3`).
- **No natural regen.** Heal only via **brown mushroom +5 HP**; **red mushroom −3 HP** (poison).
- Drowning: **2 HP/s** after air runs out (air duration ~ a few s, uncertain).
- Lava: deals damage; exact rate undocumented (we use 4 HP / 0.5s — reconstruction).
- Invincibility frames: exist; ~0.5s / 10 ticks likely (exact uncertain).
- Knockback + white hurt-flash + death animation: yes (not yet implemented).
- **Death = permadeath "Game over!"**, world ends; only option generate a new level.

### Combat (DEFERRED — for when mobs are built)
- Player fist: flat **4 HP/hit**. All mobs have **20 HP** (5 punches to kill).
- Zombie/Skeleton melee: **1–6 HP random**. Creeper melee 2–6 HP.
- Creeper **explodes only on death**: up to **12 HP (6 hearts)**, distance-scaled.
- TNT: up to 12 HP, ~4-block radius, stone immune. (after 0.26, player starts with 10 TNT)
- Skeleton arrow dmg & spider dmg: undocumented.
- Mobs present: zombie, skeleton, creeper, spider (hostile); pig, sheep (passive).
- Mob drops are **physical**: skeleton 4–9 arrows, pig/sheep mushrooms.

### Mining & drops
- **Uniform hold-to-break timer** — every block takes the *same* time to break;
  **per-block hardness** and the crack overlay are Indev, not Survival Test.
  (ClassiCube currently breaks instantly = a divergence; see the break-time section above.)
- **Physical item drops** (added in 0.24-s; 0.30 keeps them).
- Drop rules: most blocks drop themselves; **leaves→sapling (1/10)**, **grass→dirt**,
  **logs→3–5 planks**.

### Item-drop visuals — from the DECOMPILED `Item.render()` (authoritative)
Cross-checked across three independent decompilations (zhuowei/OpenClassic,
good2000mo/OpenClassic, ManiaDevelopment/MCraft-Client 0.30-s). The real method:
```
var5 = level.getBrightness(x,y,z);          // world lighting (1.0 sky / 0.6 shade)
var3 = rot + (tickCount+partial)*3.0;       // spin angle, 3 deg/tick = 60 deg/s
glColor4f(var5,var5,var5,1);                // base = world lit
bob  = sin(var3/10)*0.1 + 0.1;              // render-Y bob, ~1 Hz, range 0..0.2
glTranslatef(.., y+bob, ..); glRotatef(var3,0,1,0);
model.render();                             // PASS 1: lit textured block
g = (sin(var3/10)*0.5+0.5);  g = g*g*g*g;   // glow curve, ^4 -> brief sharp peak
glColor4f(1,1,1, g*0.4);                    // white, max 40% alpha
glDisable(TEXTURE_2D); glBlendFunc(SRC_ALPHA, ONE);  // ADDITIVE solid white
model.render();                             // PASS 2: white glow overlay
```
- **Spin: YES**, 3°/tick (60°/s), random initial angle. (Indev 0.31 changelog
  "items don't spin/glow anymore" confirms ST did both.)
- **Glow: additive solid-white second pass**, alpha = (sin(var3/10)*0.5+0.5)^4 * 0.4,
  ~1 Hz, brief sharp peak. NOT a texture dim/brighten — a real white flash.
- **Bob: YES**, sin(var3/10)*0.1+0.1, phase-locked to spin/glow (~1 Hz).
- **Size: small center-cropped cube** (terrain.png middle 8 of 16 px), NOT a full
  block. Full-size / uniformly "shrunken down blocks" is the *Indev 0.31* lineage.
  Decompiled `ItemModel`: a cube of model-units -2..2 rendered at 1/16 scale =
  a **0.25-block cube**, with UV cropped to u/v 0.25..0.75 on all 6 faces.
  ✅ IMPLEMENTED (user chose fidelity over the full-size look they'd first liked) —
  `DropItem_BuildItemCube` builds exactly this (`DROP_ITEM_HALF = 0.125`).
- **No shadow** (entity shadow stub is empty in this engine era).
- **Pickup: 3-tick (~0.15s) fly-to-player animation** (eased t², toward player feet),
  still spinning/glowing during flight, then removed. (Not yet implemented — current
  pickup is instant.)
- **Despawn: age >= 6000 ticks (5 min)**. (Not yet implemented.)

Sources: minecraft.wiki — Survival Test, Classic 0.24/0.27/0.30 SURVIVAL_TEST,
Item (entity), Breaking, Damage, Pig, Skeleton, Indev 0.31; decompiled `Item.java`
(zhuowei/OpenClassic, good2000mo/OpenClassic, ManiaDevelopment/MCraft-Client).

---

## ENGINE NOTES (useful pointers)
- Component pattern: `IGameComponent` with Init/Free/Reset/OnNewMap/OnNewMapLoaded.
  `SurvivalTest_Component` registered in `src/Game.c`.
- 20 Hz tick: `ScheduledTask_Add(GAME_DEF_TICKS, SurvivalTest_Tick)`.
- Block changes: `UserEvents.BlockChanged` event `(coords, oldBlock, newBlock)`.
- Mushroom block IDs: `BLOCK_BROWN_SHROOM = 39`, `BLOCK_RED_SHROOM = 40`.
- World gen entry for "new level": `GenLevelScreen_Show()` (declared in `Menus.h`).
- Isometric block drawing: `IsometricDrawer_BeginBatch/AddBatch/EndBatch/Render`.
- Entities: `Entities.List[]`, `Entities.CurPlayer`; `Entity_GetBounds` /
  `Entity_GetPickingBounds` for AABBs; `Entity_TouchesAny(bb, cond)` for block tests.
- Local build note: container is missing `-lXi`/`-lGL` so the final *link* fails, but
  all `.c` files compile clean under `-Werror`. Windows CI is the real compile gate.
