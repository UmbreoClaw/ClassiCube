# Classic 0.30 Survival Test — Project Notes & Handoff

Branch: `claude/c030-s-gamemode-8fpmns`

This file is the living context/handoff for the c0.30-s survival gamemode recreation.
Goal: a **faithful from-scratch recreation** of Minecraft Classic Survival Test (0.30),
cross-referenced against the Minecraft Wiki, that does **not** disturb creative mode.

---

## NEXT TASK (agreed — start here next session)

**Build physical dropped-item entities + correct the drop table.**

This is the agreed next feature. The user explicitly chose to go ahead with it.

### Why
Research confirmed c0.30-s used **physical item drops**: breaking a block spawned a
collectable item on the ground (pulsing white, full-size block pixels) that the player
walks over to pick up — it did **not** go straight into the inventory. Our current
behaviour (drop goes directly to inventory via `SurvivalTest_AddBlock`) is therefore
**unfaithful** and this is the fix.

### Scope (introduces the world's first non-player entity)
`EntityType` is currently only `{ NONE, PLAYER }` (see `src/Entity.h:43`).
A dropped item is the first non-player world object. Implementation pieces:

1. **Dropped-item entity**: position + velocity, gravity so it falls and settles,
   a small random scatter-pop when spawned.
2. **Rendering**: draw as a small pulsing-white block. Reuse `IsometricDrawer_*`
   (already used by the hotbar / survival inventory) for the block picture.
3. **Pickup detection**: per-tick proximity check vs the player; on pickup add to
   inventory (reuse `SurvivalTest_AddBlock`). Add a short pickup delay (~0.5s) so a
   block you place-then-break doesn't instantly fly back.
4. **Wire-up**: change `SurvivalTest_BlockChanged` (in `src/SurvivalTest.c`) so mining
   **spawns a drop entity** instead of calling `SurvivalTest_AddBlock` directly.
   Keep `AddBlock` for the pickup path.

Self-contained: no AI, no combat needed. It IS the entity/physics groundwork that a
future mob system would reuse.

### Drop TABLE corrections (do alongside drops)
Current `SurvivalTest_DropFor()` in `src/SurvivalTest.c` is partly wrong for c0.30-s.
Faithful 0.24-s rules (from wiki): **most blocks drop themselves**, exceptions only:
- Leaves → sapling, **1/10 chance** (we currently give sapling 100%).
- Grass → dirt. (correct already)
- Logs → **3–5 planks**. (not handled yet)
- **Stone → stone**, **ores → themselves** — our current stone→cobble and
  ore→ingot mappings are NOT c0.30-s and should be removed.

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
- **Block handling**: mining gives the (mapped) drop to inventory [TO BE REPLACED by
  physical drops]; placing consumes one from the selected slot. Creative-safe:
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
