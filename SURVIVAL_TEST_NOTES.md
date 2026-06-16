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

### DON'T build: block-breaking time / hardness
Research verdict: c0.30-s broke blocks **instantly** (one click). Per-block hardness,
hold-to-break, and the cracking overlay are **Indev** features (Feb 2010), not Survival
Test. Instant breaking (ClassiCube's current behaviour) is already faithful. Skip it.

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
- **Instant breaking** (no hardness/cracks — those are Indev).
- **Physical item drops** (added in 0.24-s; 0.30 keeps them). Items pulse white,
  full-block-pixel size; picked up by walking over them.
- Drop rules: most blocks drop themselves; **leaves→sapling (1/10)**, **grass→dirt**,
  **logs→3–5 planks**.

Sources: minecraft.wiki — Survival Test, Classic 0.24/0.27/0.30 SURVIVAL_TEST,
Item (entity), Breaking, Damage, Pig, Skeleton, Indev 0.31.

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
