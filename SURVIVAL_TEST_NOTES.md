# Classic 0.30 Survival Test — Project Notes & Handoff

Branch: `claude/c030-s-gamemode-8fpmns`

This file is the living context/handoff for the c0.30-s survival gamemode recreation.
Goal: a **faithful from-scratch recreation** of Minecraft Classic Survival Test (0.30),
cross-referenced against the Minecraft Wiki, that does **not** disturb creative mode.

---

## AUDIT PASS (this session): bug fixes + explanatory comments

User asked for a self-directed audit of everything built so far: real bugs, bloat,
and missing WHY-comments. Found and fixed, all in `SurvivalTest.c`, all re-verified
against the decompiled Java sources (not guessed):

- **`Mob_DoAttack` had swapped `atan2` arguments** for both `Yaw` and `Pitch` —
  this codebase's own direction-vector convention (`Vec3_GetDirVector`/the
  commented-out inverse `Vec3_GetHeading` in `Vectors.c`) requires
  `Yaw = atan2(dx, -dz)`, `Pitch = atan2(-dy, horDist)`; the code had the
  arguments reversed on both, which is silently wrong by up to 90° except at
  exactly 45°-bearing targets. This affected both mob chase-movement direction
  (via `Mob_MoveRelative`'s sin/cos(Yaw) basis) and arrow-aim direction (via
  `Mob_ShootArrow`'s `Vec3_GetDirVector(Yaw,Pitch)`) simultaneously. It was never
  visually obvious before because melee damage is purely distance-gated (doesn't
  use Yaw/Pitch) and mob rendering never reads `.Pitch` — only the new arrow code
  actually exercised the bug's consequences. Fixed.
- **`Mob_ShootArrow`'s pitch-spread formula was wrong-shaped**: the comment (and
  code) claimed a symmetric `±22.5°` like yaw, but the actual decompiled
  `Skeleton.java` formula is `xRot - (random()*45 - 10.0)`, an asymmetric
  `(-35°, +10°]` spread. Fixed the formula and the comment.
- **`Mob_ShootArrow` and `SurvivalTest_TryShootArrow` both spawned arrows at eye
  height** (`Entity_GetEyePosition`), but `Skeleton.shootArrow()` and
  `Minecraft.java`'s Tab-fire handler both literally use the entity's base
  position (`this.x/y/z`). Fixed both to use `e->Position`.
- **Missing starting inventory**: `SurvivalGameMode.apply(Player)` gives every
  player **10 TNT in the last hotbar slot** on spawn — this was never ported, so
  survival mode silently started with a fully empty inventory. Restored in
  `SurvivalTest_ResetState`.
- **Missing mob despawn timer**: `BasicAI.tick()` removes a mob once it's gone
  600+ ticks without being hurt/landing a hit AND a 1/800 per-tick roll fires AND
  the player isn't within 32 blocks (otherwise the timer just resets) — ported as
  `Mob.noActionTime`, reset in `Mob_Hurt` and on a successful `Mob_DoAttack` hit.
  This supersedes the older "Despawn-at-distance ... (dropped, minor)" follow-up
  note further down in this file — it's now implemented.
- **Documented, not fixed**: `BasicAttackAI.attack()` also does a `level.clip()`
  line-of-sight check and aborts the attack (no damage either way) if a block is
  in the way; `Mob_DoAttack` has no such check, so a mob can land a melee hit
  through a sufficiently thin wall within its 2-block range. Left as a known gap
  (noted with a comment at the call site) rather than implemented, since it'd need
  a new arbitrary-point-to-point raycast this codebase doesn't currently expose.
- All fixes verified via `gcc -fsyntax-only` after each change; not yet re-verified
  with a full `make` build or in a running game this session.

---

## TNT FUSE SYSTEM (this session)

User asked: "in survival mode tnt doesn't explode instantly". Confirmed against
`PrimedTnt.java`/`TNTBlock.java`/`TNTPhysics.java` that this is correct — in real
Survival Test, **placing** TNT does nothing (`TNTPhysics.onPlace` is a no-op);
only **mining** an already-placed TNT block ignites a `PrimedTnt` with a 40-tick
(2 second @ 20 TPS) fuse, which then explodes (`level.explode`, radius 4, same
block-immunity rules used for normal TNT). `TNTBlock.getDropCount()==0`, so
mining TNT never yields an item either way.

ClassiCube already had a `Physics_HandleTnt` in `BlockPhysics.c` that
instant-explodes TNT **on placement** — that's a different, older
classic-multiplayer feature, not part of Survival Test. It had to be preserved
for creative/non-survival use, but suppressed while `SurvivalTest_Enabled`
(`BlockPhysics.c`: added `#include "SurvivalTest.h"` and an early-return guard
at the top of `Physics_HandleTnt`).

Implementation, all in `SurvivalTest.c`:

- Added a `case BLOCK_TNT:` branch to `SurvivalTest_SpawnDropsForBlock` (the
  mining-drops dispatcher) that calls a new `SurvivalTest_ArmTnt(coords)` and
  returns without spawning any drop item.
- New "TNT" section (between Health & damage and Mobs): a small fixed-size
  `struct TntFuse st_tnt[TNT_MAX]` pool (mirrors the existing drops/mobs/arrows
  pool pattern). `SurvivalTest_ArmTnt` restores the block to `BLOCK_TNT` via
  `Game_UpdateBlock` (NOT `Game_ChangeBlock` — using the latter, or manually
  raising `BlockChanged`, would make `SurvivalTest_BlockChanged`'s "placed"
  branch fire and incorrectly consume an inventory item the player never
  actually placed) and starts its fuse. `SurvivalTest_TickTnt` (wired into
  `SurvivalTest_Tick`) counts down every armed fuse and detonates it on expiry.
- `PrimedTnt.hurt()`: hitting an already-lit TNT destroys it without exploding
  and drops a normal pickup item instead — ported as `SurvivalTest_DefuseTnt`,
  checked first by `SurvivalTest_ArmTnt` (so re-mining an armed block defuses
  it rather than re-arming/extending its fuse). This gives mining TNT a real
  "punch it before it explodes" defuse mechanic, at the cost of the item
  reverting to a normal pickup instead of staying placed.
- Simplification: the real `PrimedTnt` is a separate falling/flashing entity,
  not the original world block (mining instantly clears the block to air, and
  the entity floats/bounces independently with its own gravity). Porting that
  would need a new entity-physics path, so instead the block itself is simply
  left in the world (clears back to air, then is immediately restored) ticking
  down before exploding — visually it just sits there normally for 2 seconds
  instead of vanishing/floating. No gravity/bounce physics were ported.
- `PrimedTnt.render()`'s flashing white overlay (additive-blended, pulsing
  faster as `life` approaches 0, almost solid white for the last 2 ticks) IS
  ported, as `SurvivalTest_RenderTnt`/`TntFuse_GlowAlpha` — reuses the exact
  technique the dropped-item twinkle already uses (`DropItem_BuildGlowCube`):
  an untextured white cube drawn over the block with additive alpha blending
  and face culling, just axis-aligned and full-block-sized instead of a small
  spinning item cube. New `st_tntGlowVB` vertex buffer, registered in
  `SurvivalTest_OnContextLost`/`SurvivalTest_Free` alongside the existing ones,
  and a new `SurvivalTest_RenderTnt(delta, t)` called from `Game.c`'s
  `Render3DFrame` alongside `RenderDrops`/`RenderMobs`/`RenderArrows`. The
  continuous `SmokeParticle`-every-tick from the original was NOT ported (no
  smoke particle type exists in this engine, and adding one felt out of
  proportion to the rest of this simplification - the flash carries the same
  "something is about to happen" cue on its own).
- Refactored the explosion math: pulled `Mob_ExplosionImmune` and the
  block-destruction-loop + linear player-damage-falloff body out of
  `Mob_CreeperExplode` into shared `SurvivalTest_ExplosionImmune`/
  `SurvivalTest_Explode(Vec3 center, int radius)` helpers (Health & damage
  section), since TNT and the creeper's death blast both derive from the same
  original `level.explode` code. `Mob_CreeperExplode` is now a one-line wrapper.
  Replaced the old `MOB_EXPLODE_RADIUS` define with a shared `EXPLOSION_RADIUS`.
- Verified via `gcc -fsyntax-only` on all touched files, then a full
  `make PLAT=linux -j$(nproc)` build — zero errors/warnings. The new glow-cube
  winding order was checked by direct comparison against the existing,
  already-working `DropItem_BuildGlowCube` face order rather than guessed, but
  the visual result (flash brightness/timing, face culling) is NOT yet
  confirmed in a running game this session (no display available).

---

## LAUNCHER UI: survival mode toggle (this session)

Exposed the previously hidden `OPT_SURVIVAL_MODE` option (`Options.h`), which
before this had zero UI anywhere and could only be set by hand-editing
`options.txt`. Added a `LCheckbox` bound to it in two places in `LScreens.c`,
both sharing one callback:

```c
static void SurvivalMode_Changed(struct LCheckbox* w) {
    Options_SetBool(OPT_SURVIVAL_MODE, w->value);
}
```

- **Launcher Settings screen** (`SettingsScreen`) — new "Survival mode"
  checkbox under "Use display scaling". Bumped
  `SETTINGS_SCREEN_MAX_WIDGETS` 9→10, added `set_cbSurvival` layout (y=164),
  pushed `set_btnBack` down (y=170→210) to make room.
- **Choose Mode screen** (`ChooseModeScreen`, Settings → "Mode", also shown
  on first launch) — new "Survival mode" checkbox + description label below
  the Enhanced/Classic+hax/Classic buttons. Deliberately added as an
  *independent* checkbox rather than a 4th mutually-exclusive button: those
  3 buttons pick the network protocol/feature mode (classic vs CPE/custom
  blocks), which is an orthogonal axis to gameplay mode (creative/survival)
  — a player should be able to combine e.g. "Enhanced" + "Survival".
  Bumped `CHOOSEMODE_SCREEN_MAX_WIDGETS` 12→14.

No "restart required" dialog needed (unlike DPI scaling): the Launcher and
the actual game are separate processes (`Process_StartGame2` in
`Launcher_StartGame`), and options are saved to disk *before* the new game
process is spawned. So toggling the checkbox takes effect the very next
time "Play"/Singleplayer is clicked — no extra messaging needed.

Verified via `gcc -fsyntax-only` and a full `make PLAT=linux -j$(nproc)`
build — zero errors/warnings. Not yet visually confirmed in a running
Launcher this session (no display available).

---

## TERRAIN GEN AUDIT (this session): confirmed already faithful, 2 RNG bugs fixed

User asked how hard it'd be to add c0.30-s terrain gen. Verified by reading
`mcraft_client`'s `LevelGenerator.java` (genuine c0.30 client — window title
literally "Minecraft 0.30") line-by-line against `NotchyGen` in
`Generator.c`: **it's already a faithful port**, not a different/later
generator. Same Perlin/octave/combined-noise stack and constants, same pass
order (heightmap → strata → caves → ore veins coal90/iron70/gold50 (no
diamond, correct for c0.30) → water/lava flood-fill → grass/sand/gravel
surface → flowers → mushrooms → trees), same world sizes (Small/Normal/Huge
= 128/256/512, height 64, waterLevel 32). Flowers/mushrooms are correctly
gated to `Game_Version.Version >= VERSION_0023` (0.30 qualifies). No new
code needed — survival worlds already get correct c0.30-s terrain via the
existing "Generate new level" flow, since `SurvivalTest.c` never touches
generation.

Found and fixed 2 real RNG divergences from the original Java while
cross-referencing (both in `Generator.c`):
- `NotchyGen_CarveCaves`: `caveLen` was computed as
  `Random_Float() * Random_Float() * 200` (product) instead of the
  original's `(nextFloat() + nextFloat()) * 200` (sum) — gave a skewed-short
  cave-length distribution instead of the original's triangular one.
- `NotchyGen_CarveOreVeins`: same product-vs-sum bug for `veinLen`, **plus**
  a second bug — the `theta` accumulation inside the per-vein wander loop
  was `theta = deltaTheta * 0.2f` (overwrite) instead of
  `theta = theta + deltaTheta * 0.2f` (increment, matching the sibling cave
  loop a few functions up and the original `var13 += var14 * 0.2F`). This
  meant ore veins weren't smoothly curving snake-shapes like caves/the
  original — they were re-randomizing direction every step.

Verified via `gcc -fsyntax-only` and a full `make PLAT=linux -j$(nproc)`
build — zero errors/warnings. Not yet visually confirmed in a running game
this session (no display available); this affects all world generation
(not survival-specific), so worth a visual sanity check of ore vein shapes
next time a display is available.

---

## INPUT BUG FIXES (this session): Tab-fire arrows + click-to-attack mobs

User reported two gameplay bugs from live testing: Tab didn't fire arrows,
and mobs "had no hitboxes" (couldn't be attacked). Both were input-routing
bugs, not gameplay-logic bugs — the underlying `SurvivalTest_TryShootArrow`
and `SurvivalTest_TryAttackMob` were correct.

- **Tab arrows**: the old `SurvivalTest_TryShootArrow()` call in
  `OnInputDown` (InputHandler.c) sat *after* the per-screen
  `HandlesInputDown` loop. The chat HUD (`ChatScreen_KeyDown`, Screens.c)
  claims `BIND_TABLIST` (Tab by default) and `return true`s, so the screen
  loop returned early and the arrow code was dead. Moved the handler to
  *before* the screen loop, guarded by `!was` (one arrow per discrete press,
  not per key-repeat), `!Gui.InputGrab` (don't fire while typing chat so Tab
  autocomplete still works), and `InputBind_Claims(BIND_TABLIST, …)` (respects
  key rebinding, instead of the old hardcoded `key == CCKEY_TAB`).
- **Mob melee**: `SurvivalTest_TryAttackMob()` was only called from
  `InputHandler_Tick` — the held-down auto-repeat path that runs 4×/sec after
  ~0.25s. A normal quick left *click* goes through `BindTriggered_DeleteBlock`
  (bound to `BIND_DELETE_BLOCK`), which called `InputHandler_DeleteBlock()`
  directly and never tried to attack a mob. Added the same
  `if (!SurvivalTest_TryAttackMob()) InputHandler_DeleteBlock();` guard there.
  Confirmed the ray/hitbox path itself is correct: `SurvivalTest_TryAttackMob`
  uses the exact same `Entity_GetEyePosition` + `Vec3_GetDirVector` +
  `Intersection_RayIntersectsRotatedBox` pattern as the engine's own
  `Entities_GetClosest` (Entity.c), and mob `ModelAABB` is populated by the
  standard `Entity_SetModel` call at spawn.

Verified via `gcc -fsyntax-only` and a full `make PLAT=linux -j$(nproc)`
build — zero errors/warnings. Not yet confirmed in a running game this
session (no display available).

---

## OPEN BUGS — RESUME HERE NEXT SESSION (live-test feedback, not yet fixed)

User play-tested everything and provided a full bug list + screenshots
(transcript has the images). None of the below are fixed yet — this session
was just enumeration to save their usage limit. Suggested priority order is
the numbering. Verify each against a running build; don't assume root cause.

### 1. Arrows — don't fire / count stuck at 20 — FIXED
- ROOT CAUSE (found by static analysis, not the input path at all): arrows
  were spawned at `e->Position`, which in ClassiCube is the player's **feet**.
  `AABB_Make` puts the arrow box bottom at that y, and `AABB_Intersects` treats
  touching edges as overlapping, so an arrow born at feet level sits exactly on
  the block the player is standing on → instant block collision on tick 1 →
  `hasHit=true`, velocity zeroed, stuck at the feet → `Arrow_TryPickup`
  re-collects it the same tick (`st_playerArrows++`). Net: count went
  20→19→20 in one tick and the arrow existed for <1 tick inside the player, so
  firing looked like a total no-op with the count frozen at 20.
- The InputHandler routing fix (915b6a6) was fine and necessary — it just
  wasn't the (only) bug. The misleading part was the old comment claiming
  "Minecraft.java spawns at this.player.y (base position), not eye height": in
  Minecraft Classic the entity `y` IS the eye/camera position (bbox hangs
  below it), so that maps to ClassiCube's `Entity_GetEyePosition`, NOT
  `Entity.Position` (feet).
- FIX: `SurvivalTest_TryShootArrow` now spawns at `Entity_GetEyePosition(e)`.
  Same fix applied to `Mob_ShootArrow` (skeletons were spawning arrows at
  their own feet too, so skeleton shots stuck at the skeleton and never
  reached the player). Verified with a clean build.

### 2. Arrows — no texture
- Even if/when they fire, arrows have **no texture** (the `arrows_entry` /
  `st_arrowsTexId` registration isn't resolving). User confirmed via the
  "Texture ID reference sheet" debug overlay that the arrow texture slot is
  missing/blank. Check `TextureEntry_Register(&arrows_entry)` and how
  `st_arrowsTexId` is looked up vs. the actual texture pack contents.

### 3. Mob models render broken (zombie & skeleton especially)
- Screenshots show zombie/skeleton rendering malformed/grayscale with wrong
  or missing textures (one looks like a bare tripod of model parts). Pig
  appears OK-ish. Likely our hand-spawned mob entities aren't getting their
  mob texture/skin assigned, so the model draws with a default/missing tex.
  Check how `SurvivalTest_SpawnMobAt` sets up textures vs. how ClassiCube's
  normal mob models obtain `/mob/zombie.png` etc.

### 4. Mob behaviour — look down + clump toward a point
- Mobs tilt their heads/bodies **down** instead of looking ahead, and they
  all **gravitate toward a single point** rather than wandering. Likely the
  AI look-target/heading is defaulting to something like origin or (0,0,0),
  and pitch isn't being clamped/zeroed. Review BasicAI/wander port in the
  Mobs section (yaw/pitch assignment + target selection per tick).

### 5. Block breaking not implemented (has textures in code already)
- Survival should use **progressive block breaking** (multi-stage crack
  overlay) instead of instant deletion. The destroy-stage/crack textures
  already exist in the code/texture atlas. Need to implement the dig-timer +
  crack overlay render and gate instant-break behind it in survival.

### 6. Drop tables wrong
- The block/mob drop tables need correcting against the decompiled source
  (`SurvivalTest_SpawnDropsForBlock` and mob death drops). Cross-check each
  block's drop + count vs. c0.30-s.

### 7. Launcher — redundant survival toggle + cut-off Back button — FIXED
- DONE (this session). Removed the "Survival mode" checkbox from the Settings
  screen, keeping only the one on the Choose Mode screen (user confirmed
  "keep Choose Mode only"). Reverted `SettingsScreen`'s struct field,
  `SETTINGS_SCREEN_MAX_WIDGETS` (10→9), `set_btnBack` (y 210→170) and the
  layout list back to their pre-aa377d5 state — which also fixes the Back
  button being cut off in windowed mode (it was only cut off because the
  4th checkbox had pushed it down). `SurvivalMode_Changed` stays (still used
  by the Choose Mode checkbox). Verified with a clean build.

### Misc observed in screenshots (confirm whether intended)
- A "Texture ID reference sheet" debug overlay is present — confirm if that's
  one of ours/a dev tool and whether it should stay.

---

## NEXT TASK (agreed — start here next session)

**Arrow projectile system (bow-less Tab-fire, skeleton shooting, render, pickup):
DONE.** User confirmed: humans start with **20 arrows** (`MAX_ARROWS=99`),
sourced from decompiled `Player.java`. Researched (decompiled `Arrow.java`/
`Skeleton.java`/`Player.java`/`Minecraft.java` + Wiki, not guessed):
- No bow item in Survival Test — player fires directly via Tab key, force 1.2F,
  dead straight along look direction, instant (no charge/cooldown beyond key repeat).
- Arrow bbox 0.3w x 0.5h, heightOffset 0.25. Per-tick: velocity *= 0.998 (drag,
  all axes) then yd -= 0.02/force (gravity). Damage fixed: 7 if player-fired,
  3 if mob-fired (not knockback). Sticks into blocks by zeroing velocity (no
  shake/inTile in this era). Player arrows despawn after stickTime>=300 ticks
  with 1%/tick roll; mob arrows always despawn at exactly 20 ticks (not
  pickupable). Player can pick up own stuck arrows, capped at 99 total. Arrow
  can't hit its firing owner until time>5 ticks after firing.
- Skeleton `shootArrow()`: +-22.5 deg yaw spread + pitch spread, force 1.0F,
  damage 3 (mob value, not its melee 8). Fires ~1/30 chance per tick once it
  has a target, using the same aggro range constants already in
  `SurvivalTest.c` (acquire <256, give up >1024 w/ 1% roll). Skeleton death
  bursts 4-9 pickupable arrows (owner=player) at force 0.4F.
- Render: 2-quad head plane + 4-quad cross shaft (each rotated 90 deg about X),
  yaw/pitch/45-deg-roll rotation sequence, 0.05625 uniform scale.
- **Texture, corrected this session**: authentic texture is `item/arrows.png`
  (plural), **32x32**, two 10px-tall row-bands (type 0 = player rows 0-10,
  type 1 = mob/"purple" rows 10-20, selected via `type*10` Y offset). It lives
  in the **classic c0.30 jar**, not the modern 1.6.2 jar. An earlier pass in
  this session had wrongly wired up the modern jar's single 16x16
  `entity/arrow.png` (no player/mob variant) — reverted. Fixed in
  `Resources.c`: `defaultZipEntries[]` entry renamed `arrow.png` -> `arrows.png`
  and moved into the classic-jar-files block; the two `ModernPatcher_SelectEntry`/
  `ModernPatcher_ProcessEntry` checks for `entity/arrow.png` were removed.
  `ClassicPatcher_SelectEntry`/`ProcessEntry` needed no changes — they already
  auto-extract any jar entry whose basename matches `defaultZipEntries[]`
  regardless of subfolder, so this alone makes `arrows.png` get pulled from the
  classic jar. Adding this entry invalidates existing users' cached
  `default.zip` (one-time re-download+rebuild), which is expected/correct.
- **Implemented** (`SurvivalTest.c`, new "Arrows" section, all behaviour
  re-derived directly from decompiled `Arrow.java`/`Skeleton.java`/
  `Minecraft.java`, not guessed):
  - Fixed `st_arrows[ARROW_MAX]` pool (mirrors the drops/mobs pattern), each
    with its own `gravity = 1/force` since force varies per source (1.2
    player Tab-fire, 1.0 skeleton `shootArrow`, 0.4 skeleton death-burst).
  - `Arrow_Tick`: exact drag/gravity/substep collision sweep from
    `Arrow.tick()` — block hits zero velocity and stick; entity hits
    (respecting the 5-tick owner-immunity window) call `Mob_Hurt`/
    `SurvivalTest_Hurt` and always despawn (no sticking on entity hit).
    Player-fired stuck arrows are pickupable and despawn after stickTime>=300
    with a 1%/tick roll; mob-fired stuck arrows always despawn at tick 20.
  - `SurvivalTest_TryShootArrow` wired to a discrete Tab key-down hook in
    `InputHandler.c`'s `OnInputDown` (covers both the legacy and `Down2`
    input dispatch paths, which both route through that one function).
  - Skeleton AI (`SurvivalTest.c` Mobs section): 1/30 per-tick chance to call
    `Mob_ShootArrow` (±22.5° yaw/pitch spread, force 1.0, damage 3) while it
    has a target, on top of (not instead of) its existing melee attack;
    `Mob_SkeletonDeathBurst` fires 4-9 arrows (force 0.4, owner=player so
    they're pickupable) at the 20-deathTick removal mark.
  - Rendering (`SurvivalTest_RenderArrows`, called from `Game.c`'s 3D
    render-frame alongside `SurvivalTest_RenderDrops`/`RenderMobs`): exact
    2-quad head + 4-quad cross-shaft geometry and UVs from `Arrow.render()`,
    rebuilt each frame into a dynamic VB via an orthonormal basis derived
    from the arrow's stored unit facing vector (with a degenerate straight
    up/down fallback), using the `arrows.png` texture registered via the
    same `TextureEntry` pattern as `particles.png`.
  - HUD: arrow count digit display added to `Screens.c`'s `HUDScreen`
    (mirrors the existing hotbar stack-count digit rendering, drawn
    right-aligned above the hotbar's right edge, alongside the heart row),
    following the fixed-vertex-budget VB pattern (`SURVIVAL_ARROWS_MAX_VERTICES`
    folded into `HUD_MAX_VERTICES`, dirty-checked via a `lastArrows` field).
  - Deliberate simplification: no player-side knockback from arrow hits —
    the existing mob-melee-vs-player damage path also has none, so adding it
    only for arrows would've been an inconsistent, out-of-scope addition.

---

**Fall damage bug fix (player + new mob fall damage): DONE (earlier session).**

User reported player fall damage wasn't registering for falls just past the
3-block safe threshold. Root cause found in `SurvivalTest_UpdateFall`: it read
`e->Position.y` directly, but `LocalPlayer_Tick` (which runs *before*
`SurvivalTest_Tick` every tick — `Entities_Component` is registered before
`SurvivalTest_Component` in `Game.c`, and both append to the same scheduled-task
list in registration order) ends by doing
`e->next.pos = e->Position; e->Position = e->prev.pos;` (`Entity.c:752`) —
stashing the just-computed position for interpolation and resetting
`e->Position` back to the *previous* tick's value. So every read of
`e->Position.y` from our tick was one tick stale, which silently dropped the
final (fastest, due to gravity) tick of every fall from the measured distance —
under-counting borderline falls (a fall just over 3 blocks could measure as
exactly 3.0 and deal 0 damage instead of 1).
**Fix**: read `e->next.pos.y` (this tick's true, freshly-computed height)
instead of `e->Position.y`, and seed the fall's start height from
`e->prev.pos.y` (the resting height before this tick's movement) when first
detecting airborne — `SurvivalTest.c`'s `SurvivalTest_UpdateFall`.
Mobs were *not* affected by this specific bug (they have no prev/next
double-buffering — `Mob.Base.Position` is mutated directly and never reset),
but auditing mob damage surfaced a real gap: **mobs had no fall damage at
all** (`Mob.java`'s `causeFallDamage()` was never ported). Added it: new
`falling`/`fallPeakY` fields on `struct Mob`, same peak-tracking approach as
the player, applied via the existing `Mob_Hurt(m, NULL, damage)` path right
after `Mob_Travel` in `SurvivalTest_TickOneMob` (post-move `Position`/`OnGround`
are already fresh for mobs, no staleness concern there).
Verified: `gcc -fsyntax-only` clean, full `make -j$(nproc)` compiles and links
(`ClassiCube` executable produced) with zero errors. Not yet tested in a
running game — next session should specifically test: falling exactly 3
blocks (should be safe, 0 damage), falling 4+ blocks (should now reliably
deal `floor(dist)-3` damage), and a mob (e.g. a zombie) falling off a ledge.

---

**Mob entity system (spawning/AI/combat/death/render): DONE (earlier session).**

Implemented entirely in `src/SurvivalTest.c` (+ hooks in `src/SurvivalTest.h`,
`src/Game.c`, `src/InputHandler.c`), based on decompiled `Mob.java`/`AI.java`/
`BasicAI.java`/`BasicAttackAI.java`/`JumpAttackAI.java`/`Zombie.java`/
`Skeleton.java`/`Spider.java`/`Creeper.java`/`Pig.java`/`MobSpawner.java`/
`SurvivalGameMode.java` (not guessed). Summary of what landed:

- **6 species**, fixed pool `st_mobs[MOB_MAX=32]`, each wrapping a plain
  `struct Entity` (`Mob.Base`) the same way drops do — simulated/rendered
  entirely inside `SurvivalTest.c`, never added to the networked `Entities.List[]`.
  Per-species table (`mobTypeInfo[]`): Zombie (AI=attack, runSpeed 1.0,
  damage 6, lookAngle 30°), Skeleton (attack, 0.3, damage 8, melee-only —
  no projectile system), Pig (passive, 0.7), Creeper (attack, 0.7, damage 6,
  lookAngle 45°, self-damaging/explodes), Spider (jump-attack lunge, 0.56),
  Sheep (passive, 0.7).
- **Physics**: faithful port of `Mob.travel()`/`moveRelative()` — gravity
  0.08, drag (.91,.98,.91), ground friction (.6,1,.6), jump velocity 0.42 —
  reusing the engine's `CollisionsComp`/`Collisions_MoveAndWallSlide` for
  wall/ground collision (same as `LocalPlayer`). Water/lava use the
  original's drag constants (0.8/0.5) with a simplified upward-nudge paddle
  assist instead of the exact `isFree` port.
- **AI**: wander (7%/tick new direction, 1%/tick jump, 4%/tick ±30° turn
  impulse), chase (full speed toward target, 4%/tick hop / 80%/tick while
  submerged), attack (aggro range 16 blocks, gives up at 32 blocks with a 1%
  roll/tick, attacks within 2 blocks, delay 10+rand(20) ticks, damage
  `(int)((rand+rand)/2*damage+1)`), Spider's jump-attack lunges at the
  target when it has one. Facing uses `Math_Atan2f` on the CC-native yaw
  convention (re-derived from `Vec3_RotateY3`, not a literal port of Java's
  raw yRot formula).
- **Combat**: flat 20-tick invincibility window (simplification of Java's
  dual-threshold `invulnerableTime`), knockback away from attacker, aggro-on-hit
  for non-passive mobs, player fist deals flat 4 HP (`SurvivalTest_TryAttackMob`,
  wired into `InputHandler_Tick`'s left-click so it's tried before block-breaking).
  Picking uses `Intersection_RayIntersectsRotatedBox` against each mob's AABB,
  gated by `ReachDistance`, same pattern as block picking.
- **Death/drops**: Pig spawns exactly 1–2 brown mushrooms (`(int)(rand+rand+1)`,
  not 1–3) via the existing drop-entity system; Sheep has no drop (passive,
  no `die()` override in source). Creeper explodes ~20 ticks after death,
  radius-4 sphere block destruction (TNT-immune blocks skipped, faithful to
  `BlocksTNT`), player damage falls off linearly with distance, capped at
  `MOB_MAX_HEALTH*0.6` (~12 HP) at point-blank — exact Java falloff curve
  unknown, this is an approximation. Creeper also self-damages 6 HP per
  successful attack (matches `Creeper$1.attack()`), dying after ~4 hits.
- **Spawning**: faithful port of `MobSpawner.spawn()`'s cluster-jitter
  algorithm (3 outer x 3 inner jitter, vertical jitter always 0 — a quirk
  preserved from the original, not a bug), including the
  `distSq < 256` avoid-skip-but-still-consume-jitter behaviour. Periodic gate
  (`SurvivalTest_TrySpawnMobs`, called every tick): `area = volume/64³`,
  spawns if `rand(100) < area && mobCount < area*20`. Initial population on
  map load (`SurvivalTest_SpawnInitialMobs`): `area = volume/800`, avoiding
  the map spawn point.
- **Rendering**: `SurvivalTest_RenderMobs` reuses `Model_Render`/
  `AnimatedComp_GetCurrent`/`Model_ShouldRender`, hooked into `Game.c`'s
  `Render3DFrame` right after `SurvivalTest_RenderDrops`. Needed a real
  `EntityVTABLE` (`mob_VTABLE`) with a working `GetCol` since `Model_SetupState`
  calls `e->VTABLE->GetCol(e)` directly — all other slots are NULL since
  mobs are ticked/rendered by hand, never through generic Entity dispatch.
  `Mob_GetColor` blends in a red hit-flash based on `hurtTicks`.
- Verified: `gcc -fsyntax-only` clean on all three touched files, then a full
  `make -j$(nproc)` build compiles **and links** with zero errors/warnings
  (the earlier `-lXi`/`-lGL` link failure was just missing system dev
  packages in this container — resolved by installing `libgl1-mesa-dev` +
  `libxi-dev` after an `apt-get update`; not a code issue). Not yet tested
  in a running game (no display in this container) — worth an in-game pass
  next session: spawn rates, wander/chase/attack feel, knockback, creeper
  explosion radius/damage, pig drops.

### Possible follow-ups (not done, not asked for yet)
- Skeleton arrow-shooting (needs a projectile system — out of scope here).
- Despawn-at-distance: **implemented** in the audit pass at the top of this file
  (see `Mob.noActionTime`). Mob-mob push-apart physics is still not ported.
- Mob death animation / fall-over before removal (currently mobs just
  freeze in place during `deathTicks` then vanish).
- Mob names/render distance culling tuning, sound effects on hurt/death.

---

**Physical dropped-item entities + drop table correction: DONE (earlier session).**

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
  - **Glint = a second translucent white "shell" pass**, `DropItem_BuildGlowCube`,
    same geometry as the item cube but `VERTEX_FORMAT_COLOURED` (flat colour, no
    texture), alpha = `(sin(var3/10)*0.5+0.5)^4 * 0.4` (`DropItem_GlowAmount`) —
    matches the decompiled curve and 0.4 max alpha exactly. Drawn with
    `Gfx_SetFaceCulling(true)` + `Gfx_SetAlphaBlendingAdditive(true)` + `Gfx_SetDepthWrite(false)`.
    Two earlier attempts both failed: (1) the first shell attempt had no face culling,
    so its own back faces blended in too, doubling up into a boxy/flashing artifact;
    (2) lerping the item's *own* lit colour toward white was a no-op in full daylight,
    since `Lighting.Color` is already pure white there — the glint was invisible
    outdoors, exactly where it was tested. Face culling works because the hand-built
    cube's vertex winding is consistent (verified: `cross(p1-p0, p2-p1)` gives the
    correct outward normal for all 6 faces), so culling back faces leaves exactly the
    visible front shell, matching the original's literal two-pass solid+glow render
    (confirmed via decompiled `Item.render()` calling `model.render()` twice).
    A third issue: even after the above two fixes, the flash still looked "wrong"/
    flatter than the reference client. Root cause: the shell pass used standard
    interpolative alpha blending (`dst = dst*(1-a) + src*a`), but the decompiled
    `Item.render()` explicitly does `glBlendFunc(SRC_ALPHA, ONE)` — genuine **additive**
    blending (`dst = dst + src*a`), a different curve entirely (not reproducible via
    repeated standard-blend passes). Added a new cross-platform primitive,
    `Gfx_SetAlphaBlendingAdditive(cc_bool)` (`Graphics.h`), with real implementations
    for GL1/GL11/GL2 (`_GLShared.h`, toggling `glBlendFunc` between
    `SRC_ALPHA,ONE_MINUS_SRC_ALPHA` and `SRC_ALPHA,ONE`), D3D9 (`Graphics_D3D9.c`,
    same toggle via `D3DRS_DESTBLEND`), and D3D11 (`Graphics_D3D11.c`, widened the
    precomputed `om_blendStates` lookup table with an extra "additive" bit folded into
    `DestBlend`/`DestBlendAlpha`). All other backends fall back to regular
    `Gfx_SetAlphaBlending` via a generic default in `_GraphicsBase.h` (slightly less
    punchy glow, but no breakage) since none of those platforms build from this branch.
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
- Combat/mobs: **implemented this session** (see NEXT TASK above) — full spawn/AI/
  combat/death/render system, client-simulated, not networked.
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

### Combat (IMPLEMENTED — see decompiled-source figures below)
Figures below are from the recovered decompiled `Mob.java`/`Zombie.java`/
`Skeleton.java`/`Spider.java`/`Creeper.java`/`Pig.java`/`BasicAttackAI.java`
(ground truth, supersedes the earlier wiki-reconstructed guesses).
- Player fist: flat **4 HP/hit**. All mobs have **20 HP** (5 punches to kill).
- Melee damage formula: `(int)((rand+rand)/2*damage+1)` where `damage` is the
  mob's base figure below (so actual hit range is roughly 1..damage, weighted
  toward the middle, not uniform).
  - **Zombie**: base damage **6** → ~1–6 HP/hit.
  - **Skeleton**: base damage **8** → ~1–8 HP/hit. Melee-only in this
    implementation (no projectile system for its real ranged attack).
  - **Spider**: base damage **6** → ~1–6 HP/hit (jump-attack lunge, not a
    bigger hit).
  - **Creeper**: base damage **6** → ~1–6 HP/hit (same as Zombie, NOT 2–6 as
    previously guessed). Also **self-damages 6 HP per successful attack**
    (`Creeper$1.attack()`), dying after ~4 successful hits even without being
    fought back.
- Creeper **explodes ~20 ticks after death** (not on death instantly): up to
  ~**12 HP (6 hearts)** (`MOB_MAX_HEALTH*0.6`, our linear-falloff approximation
  — exact Java damage-falloff curve vs. distance is unknown), **4-block radius**,
  TNT-immune blocks (stone etc.) survive.
- TNT: up to 12 HP, ~4-block radius, stone immune. (after 0.26, player starts with 10 TNT)
- Mobs present: zombie, skeleton, creeper, spider (hostile); pig, sheep (passive).
- Mob drops are **physical**: **Pig drops exactly 1–2 brown mushrooms**
  (`(int)(rand+rand+1)`, not a 1–3 range as previously guessed). **Sheep has
  no drop** (passive `QuadrupedMob`, no `die()` override in source). Skeleton
  arrow-drops are out of scope (no projectile/arrow system).

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
- **Same generic cube for every block, no sprite exception** — confirmed by reading
  `Item.initModels()`: it builds one `ItemModel` per block ID unconditionally
  (`models[id] = new ItemModel(block.textureId)`), and `render()` always calls
  `models[resource].render()`. `ItemModel`'s constructor only special-cases UV
  nudges for wool/cobblestone colour variants — never sprite/cross blocks. Roses,
  dandelions, saplings and both mushrooms all predate c0.30 (added Classic 0.0.20a,
  June 2009), so this isn't a "didn't exist yet" gap — dropped flowers/saplings/
  mushrooms in real Survival Test really did look like the generic cropped cube
  (blob-of-the-texture's-center-pixels), NOT a flower-shaped sprite. So the current
  ClassiCube behaviour (drops always use `DropItem_BuildItemCube`, never a sprite
  quad) is period-accurate, even though it looks rougher for plants than a cross
  sprite would. **Confirmed keep-as-is** — user chose fidelity over a nicer-looking
  but non-source cross-sprite deviation, after being told this matches the
  decompiled source exactly.
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
- Local build note: this container originally lacked `libgl1-mesa-dev`/`libxi-dev`
  (so the final link failed with `-lXi`/`-lGL` not found, even though all `.c` files
  compiled clean). Fixed by `apt-get update` then installing both packages — full
  `make PLAT=linux` now compiles **and links** successfully. Windows CI remains the
  authoritative compile gate either way.
