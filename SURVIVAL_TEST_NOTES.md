# Classic 0.30 Survival Test — Project Notes & Handoff

## SESSION LOG - Indev leaf decay (latest)

User: do leaves decay when a tree is broken, dropping saplings? Verified vs
source: Indev YES, c0.30 NO.
- c0.30 LeavesBlock has getDrop=sapling / getDropCount=1-in-10 but NO update()
  method - leaves are permanent, only a mined leaf drops a sapling. (Ours
  already dropped a sapling 1-in-10 on mining - correct, unchanged.)
- Indev BlockLeaves.updateTick DOES decay: a leaf whose block directly below is
  non-solid (Material.isSolid, so air/plants/liquids but not leaves/log/stone)
  and that has no log (Block.wood == id 17 == BLOCK_LOG) within x+-2, y-1..y,
  z+-2 drops a sapling (same 1-in-10 quantityDropped) and is removed. The
  non-solid-below gate makes a chopped canopy peel from the bottom up over
  random ticks.

Our port had NO leaf random-tick at all (BLOCK_LEAVES had no OnRandomTick
handler and wasn't dispatched in IndevTest_TickRandomBlocks), so Indev leaves
never decayed. Added IndevTest_TickLeaves (faithful port) dispatched alongside
IndevTest_TickGrass; leaves are Indev-only, c0.30's classic path is untouched.

gdb-verified (30 ticks each): a leaf beside a log stays (18); an isolated leaf
with air below decays to air (0).

## SESSION LOG - Indev grass decay: covered vs merely shadowed

User (floating world screenshot): should grass beneath a floating island grow
back or stay dirt? Answer from the source: grass with open AIR above it should
STAY GRASS even in the island's shadow; dirt there stays dirt (won't spread
into low light). Our port got the grass-death half wrong.

Genuine BlockGrass.updateTick decay gate is
`getBlockLightValue(x,y+1,z) < 4 && canBlockGrass(x,y+1,z)` - grass reverts to
dirt ONLY when a grass-blocking (opaque) block sits DIRECTLY on top. canBlockGrass
is false for air/glass/sprites (Material Transparent/Logic override it), true for
opaque blocks. c0.30's GrassBlock is different (keys on `!isLit(x,y,z)`), so this
is an Indev-specific rule.

Our IndevTest_TickGrass used `!Lighting.IsLit(x,y,z)` for decay. IsLit is
`y > lightHeight`, i.e. sky-exposure of the grass block itself - which is FALSE
for anything shadowed by a distant block (a floating island), even with open air
directly above. So we were stripping every island's underside back to dirt.

Fix: decay now gates on `Blocks.BlocksLight[block directly above]` (a faithful
stand-in for canBlockGrass - opaque light-blockers are exactly the grass-blocking
materials, and they also make it dark below). Air/glass above -> no decay. The
spread branch (sky-lit grass seeds nearby sky-lit dirt) is unchanged. Indev-only;
c0.30's classic Physics_HandleGrass path is untouched.

gdb-verified (80 ticks each): grass shadowed-with-air-above stays grass (2);
grass with stone directly on top decays to dirt (3).

NOTE (separate, not changed): c0.30's classic Physics_HandleGrass reverts
unlit grass instantly and never spreads, whereas genuine c0.30 GrassBlock uses a
1-in-4 decay roll + 4 spread attempts. A real but pre-existing c0.30-only
deviation, out of scope for this floating-world (Indev) question.

## SESSION LOG - Deep spawning/world-fidelity audit

User asked for a deeper sweep of spawning + world deviations, verified before
acting. Read the full genuine spawn stack (MobSpawner both modes, the
getCanSpawnHere chain, World.findSpawn, the day/night light path) against ours.

**Deviation found + FIXED - Indev spawn ground must be a NORMAL (opaque) cube.**
The two modes gate the "ground below" on DIFFERENT predicates:
- c0.30 MobSpawner uses `isSolidTile` (any solid) -> our `Mob_BlockIsSolid`
  (COLLIDE_SOLID) is correct, unchanged.
- Indev MobSpawner.performSpawning gates on `isBlockNormalCube(x,y-1,z)` =
  `Block.isOpaqueCube()`. Leaves/glass/slabs are solid-collidable but NOT
  opaque cubes, so genuine Indev never spawns on tree canopies / glass. We
  were using COLLIDE_SOLID there too, so mobs could perch on leaves.
Added `Mob_BlockIsNormalCube` (Blocks.FullOpaque == DRAW_OPAQUE full cube =
isOpaqueCube, clamps OOB like getBlockId) and used it for the Indev ground
check only. Cell/head stay COLLIDE_SOLID pre-filters (net unchanged - the real
box clearance is SpawnMobAt's isFree). gdb-verified: NormalCube gives
dirt=1/stone=1, leaves=0/glass=0; Solid gave leaves=1/glass=1 (the old bug).

**Everything else verified FAITHFUL (no change):**
- Initial population: Indev 1000 performSpawning passes at gen (LevelGenerator);
  c0.30 vol/800 at prepareLevel. Ours matches; Indev correctly skips a
  prepareLevel bulk-pop.
- Per-tick cadence: Indev runs every tick (4 monster + 4 animal attempts);
  c0.30 gate `rand(100) < area && count < area*20`, spawn `area` passes. Match.
- Caps: Indev monsterCap = vol*20/64^3 /2 (Normal), animalCap = w*l/4000,
  counted separately (Mob_IndevCountKinds). c0.30 area*20. Match.
- Type rolls: Indev monster nextInt(5) with index 4 = no-spawn, animal
  nextInt(2); c0.30 nextInt(6). Jitter nextInt(6)-nextInt(6) horizontal, 0
  vertical. Y depth-bias (min of two floats) for monsters only. Match.
- Distance gates: Indev >= 32 (1024 sq), c0.30 >= 16 (256 sq). Match.
- Light rules: monster `light <= rand(8)`, animal `light > 8`; c0.30
  `!isLit || rand(5)==0`. Match. getBlockPathWeight>=0 (EntityCreature) is
  NOT ported but is redundant with these (dark => weight>0, bright animal =>
  weight>0) - verified no behavioural difference, so intentionally skipped.
- Box/liquid clearance: genuine getCanSpawnHere's checkIfAABBIsClear +
  getCollidingBoundingBoxes + !getIsAnyLiquid == our SpawnMobAt isFree
  (Entity_TouchesAny over solid+liquid). Match.
- Player findSpawn: genuine World.findSpawn (central-half pick, first
  uncovered+1, > waterLevel & >= 4, 7x4x8 clear volume, opaque footprint) is
  ported faithfully in IndevGen_FindSpawn.
- Day/night: genuine DOES darken skylight over time (World.skylightSubtracted,
  eased by Light.updateDaylightCycle toward getSkyBrightness) - our
  indev_lastSkyLight one-step easing models this. getBlockLightValue reads the
  stored (eased) nibble and CLAMPS OOB - matching our IndevTest_LightLevel.
  So night spawns are light-driven (not a separate time factor), faithfully.

## SESSION LOG - Indev mob AI: c0.30 chase logic leaked into the fallback

User: Indev zombies "fighting its own AI to attack players" and "semi wander
off even a few blocks away".

Root cause: `Mob_BasicAIUpdate` doubles as BOTH c0.30's `BasicAI.update()`
AND (in Indev) the pathless fallback that stands in for `EntityLiving.
updatePlayerActionState`. It carried c0.30 `BasicAI.update`'s target branch:
```
if (attackTarget != null) { yya = runSpeed; jumping = rand<0.04; }
```
In c0.30 that's fine - `BasicAttackAI.doAttack()` runs immediately after and
turns `yRot` to face the target, so the forced-forward and the facing agree.
But Indev's `EntityLiving.updatePlayerActionState` (in-20100223) has NO target
branch - it's pure random wander; the target/attack is fully handled up in
`EntityCreature.updatePlayerActionState` (our `Mob_IndevCreatureAI`). So when
an Indev monster fell into this fallback (its A* path exhausted or unreachable),
it got `moveForward = moveSpeed` forced while its yaw was the RANDOM wander
heading - i.e. it barrelled off full-speed in a random direction while still
"targeting" the player. That is the wandering-off / fighting-its-own-AI.

Fix (SurvivalTest.c): gate that block `if (m->hasTarget && !IndevTest_Enabled)`.
Indev's fallback is now the faithful pure-wander `EntityLiving` (7% random
move impulse, 1% jump, 4% yaw-velocity, pitch 0) with no forced forward;
c0.30 is byte-for-byte unchanged.

Rig note (gdb-driven, floating jf0 world): a stable single zombie acquired the
player (hasTarget 1), walked in from 5-10 blocks and held engaged ~3 blocks
away instead of fleeing. (The floating world constantly recycles mob slots -
mobs walk off the islands and drop to the world's y=0 collision boundary -
which made longer observations noisy.)

### Spawn-on-floating-floor bug (Mob_BlockIsSolid out-of-bounds)
Follow-up: the user still saw tons of mobs on the void floor. Root cause was
a real deviation. `Mob_BlockIsSolid` (the spawner's ground/clearance test)
returned `true` for out-of-bounds cells. Genuine `World.getBlockId` instead
CLAMPS out-of-bounds coords to the edge block (`y<0 -> y=0`, etc.), and the
spawn gate is `!isBlockNormalCube(x,y,z) && isBlockNormalCube(x,y-1,z) &&
getCanSpawnHere` (MobSpawner.performSpawning line 125). So at the bottom the
"ground below" test at y-1 = -1 genuinely reads the y=0 block - which in a
floating world is AIR, so monsters do NOT spawn in the void.

Our `return true` faked solid bedrock at y=-1, so the pitch-black void column
passed both the ground check AND the monster light check, carpeting the y=0
floor with endless spawns. Fixed `Mob_BlockIsSolid` to clamp coordinates like
getBlockId instead of returning true. (Only used by the two spawner passes -
no AI/collision impact. Normal worlds are unaffected: their y=0 is bedrock,
still solid.)

Rig-verified: on a floating world, `Mob_BlockIsSolid(x,-1,z)` now returns
false (reads the y=0 air); after clearing all mobs and running the per-tick
spawner 45s, the void floor stayed at 0 mobs while islands filled to 11.

Note: mobs cannot spawn mid-air either - both passes still require solid
ground below. The few that reach the bottom now are only ones that walked off
an island edge (much reduced by the AI fix above).

## SESSION LOG - Large (double) chest: InventoryLargeChest 54-slot GUI

User: "work on the doublechest ... look at how such interactions work with
other blocks around it as well, port the gui/textures and make sure it's
faithful."

Ground truth (in-20100223): `BlockChest.blockActivated` combines two
adjacent `TileEntityChest`s into an `InventoryLargeChest` (27+27 = 54).
`GuiChest` lays it out as `rows = size/9` (3 single / 6 large), ySize =
114+rows*18, player rows offset by var3=(rows-4)*18; the background is the
single 6-row `container.png` sampled as a top strip (0,0)-(176,rows*18+17)
plus a fixed player-inventory strip (0,126)-(176,222). The single chest is
just the top 3 rows of that same texture. Adjacency rules already ported in
a prior pass (`canPlaceBlockAt` caps doubles; a normal cube above EITHER
half keeps the lid shut).

What was missing: the two chests were never actually combined - opening a
paired chest showed a lone 27-slot view. Added:
- **IndevTest.c**: `indev_openTE2` (the lower half). `IndevTest_OpenContainer`
  now detects a neighbour chest in the genuine priority (-X/+X/-Z/+Z), lazily
  creates its tile entity, and assigns upper/lower exactly as
  `BlockChest.blockActivated` does (the -X/-Z neighbour is the UPPER half).
  `IndevTest_ContainerSlot(i)` routes 0..26 -> upper TE, 27..53 -> lower TE
  (InventoryLargeChest.getStackInSlot). New `IndevTest_ContainerSlotCount()`
  (3 / 27 / 54). Breaking either open half, or a new map, drops the whole
  view to the discard slot.
- **Screens.c**: chest rows are now dynamic (`SurvivalInv_ChestRows` = count/9).
  Layout uses the genuine `114+rows*18` ySize and `103/161 + var3` player
  offsets; the background top strip is `rows*18+17` tall; a new "Large chest"
  label (InventoryLargeChest.getInvName) replaces "Chest" and the "Inventory"
  section label follows the panel down (`rows*18+20`).
- **SurvivalTest.h**: added `SURVIVAL_CONTAINER_MAX` (54) and moved
  `SURVIVAL_ARMOR_BASE` above it, so the widened container address span
  (45..98) can't collide with the armor slots in the slot-dispatch order.

Rig-verified (gdb-driven, Indev world): two Z-adjacent chests open one tall
"Large chest" GUI - 6x9 slots + player inventory, ContainerSlotCount 54,
slot 0 and slot 27 resolve to two different tile entities (indev_tes+16 vs
+208). A lone chest still opens the 3-row "Chest" (27). A chest under a plank
ceiling on its far half faithfully refuses to open.

## SESSION LOG - in-20100201 "Human" mob (debug-only)

User: "find the source of in-20100201-0025 and port the human mob but only
as debug spawns."

- **Source traced**: cloned `pythonengineer/minecraft-python` branch
  `0.31.20100201-2`. There is NO distinct Human class - `MobSpawner.spawnMob`
  just does `mob = EntityLiving(world); mob.setEntityAI(AILiving())`. So the
  "Human" is the generic base `EntityLiving`: `HEALTH = 20`, skin
  `char.png`, passive wander AI (AILiving), default size 0.6x1.8.
- **Port (SurvivalTest.c / .h)**: the mob-type enum used `MOB_TYPE_COUNT`
  for BOTH the natural-spawner `nextInt(6)` range AND the info-table size.
  Split them: `MOB_SPAWN_COUNT` (=6) bounds the spawner roll; `MOB_TYPE_HUMAN`
  (=6) sits above it so `Mob_SpawnerRun` can never roll it. `MOB_TYPE_COUNT`
  is now 7 (table size only).
- Human `mobTypeInfo` row: model `"humanoid"` (its defaultTex IS `char.png`,
  so an un-skinned mob renders as Steve), `MOB_AI_PASSIVE`, damage 0,
  deathScore 0, size 0.6x1.8 both modes, heightOff 1.62. Health resolves to
  20 (only Indev pig/sheep drop to 10). Not zombie/skeleton, so it never
  gets daylight-burned or armored; not pig/sheep, so its hurt sound is the
  generic `random.hurt` and it has no ambient voice - all correct for a bare
  EntityLiving.
- **Debug-only wiring**: added `SURVIVAL_DEBUG_MOB_HUMAN` (last, value lines
  up with MOB_TYPE_HUMAN), `"human"` in `debugMobNames[]`, and to the
  `/client spawn` help line. Reachable ONLY via `/client spawn human [count]`.
- **Rig-verified**: `/client spawn human 3` on a loaded world - full-size
  Steve-skinned humanoid renders and stands passively, no crash. Natural
  spawner still rolls only the six real types.

## SESSION LOG - Paperdoll lean + spawn fallback + feedback triage

Playtest feedback batch (Indev-vs-CC side-by-sides). Fixed this pass:
- **Paperdoll lean too much**: the vertical mouse tilt was applied as a
  BODY pitch (doll.RotX) stacked on top of the head pitch - double lean.
  Genuine drawGuiContainerBackgroundLayer applies it as a whole-SCENE
  camera rotation (glRotatef(-atan(dy/40)*20, 1,0,0) before rendering
  the entity) PLUS the head pitch; the body only ever tracks
  horizontally (renderYawOffset yaw). Now: body RotX=0, head pitch kept,
  and a dollCamPitch rides the GUI view matrix as the scene tilt. Rig-
  verified - body stays upright, legs clean.
- **Startup air-spawn**: findSpawn is a byte-faithful transcription
  including genuine's 1,000,000-attempt fallback of ySpawn = height+100
  (genuine drops you and you fall). On our floating/uneven maps that
  reads as "spawned in the sky"/void fall. Graceful deviation: the
  fallback now drops to the sampled surface column (FirstUncovered+2)
  instead of +100. Only triggers on the rare search-failure seed.

Confirmed WORKING (not bugs), documented for the record:
- **Sun/moon DO render** - the RenderSky port draws them (stars visible
  at midnight, the sun is the soft additive glow above the horizon at
  dawn - screenshotted). It's a soft Indev-era glow, not a hard disc,
  so it's easy to miss as a time indicator but it IS there and faithful.

Still open (need the user's exact view - couldn't reproduce cleanly on
the flaky headless rig):
- **Armor paperdoll "light bugs"**: our armor box defs match the engine
  human legs + genuine ModelBiped inflation, so the remaining issue is
  subtle (likely leggings/boots z-overlap). Needs the user's specific
  armored-doll frame to pin down.
- **Fire "placed as a block next to the wood"**: genuine fire keeps a
  full selection cube (only entity collision is off), and our
  Builder_DrawFire already leans against flammable neighbours when the
  cell has no floor - so this is likely the grounded-render case (floor
  below -> 8 upright sheets, genuine behaviour) reading as detached.
  Needs the user's exact placement to confirm vs. genuine.

## SESSION LOG - Genuine block-id migration

User (after /give fire handed out the inert CPE Fire 54): asked for
per-mode block identity so Indev uses the REAL Indev ids. Done - the
1:1 blocks now live AT their genuine ids, shadowing CPE decoration in
Indev mode only (Game_Reset restores CPE defs when a non-Indev map
loads; classic/creative untouched):
  torch 70->50, fire 98->51, chest 67->54, diamond ore 93->56,
  workbench 66->58, furnace 68/69->61/62.
- Items were ALREADY at genuine ids (256+local == shiftedIndex).
- Metadata-simulating variants keep internal ids (chest/furnace facings
  71-82, farmland 83/84, crop stages 85-92, wall torches 94-97) and
  still collapse to genuine id + Data nibble on save.
- BlockToIndev/FromIndev are now identity for the migrated ids; the
  leftover CPE-slot remaps stay as lossy fallbacks for stray ids in old
  worlds. parity_diff.py remap shrank to just the wall torches.
- /client give checks Indev block names before the engine lookup
  (IndevTest_FindBlockByName, canonicalised - so "torch" gives 50, not
  a wall variant), and genuine NUMERIC ids now work: give 51 = fire.
- Also: diamond ore smelts to a diamond (genuine FurnaceRecipes entry
  that previously had no source block), and fire is directly placeable
  (genuine has no fire item at all, so give+place is our extension -
  it behaves exactly like flint-and-steel fire).
- Existing .mclevel saves are unaffected: the disk format always stored
  genuine ids; only the runtime ids moved.

### Rig verification (finally including the flames!)
- give fire/torch/workbench by name -> correct genuine blocks.
- Placed fire on the spawn house wall: Builder_DrawFire renders the
  animated leaning wall-flame perfectly, and within seconds the house
  was engulfed - wall sheets, ceiling flames, a hole burned through.
  The fire render was never broken; the earlier invisible-fire hunts
  were placement/framing failures on the rig.

## SESSION LOG - Startup Indev world + diamond mining chain

- **Startup world**: launching in Indev mode now generates a genuine
  Indev world (IndevGen, Generate-menu defaults Inland/Normal 128x128x64)
  instead of the classic NotchyGen one - the game boots into the spawn
  house. Rig-verified.
- **Diamond chain**: diamond ore (93) now drops the diamond ITEM (256+8,
  qty 1 - BlockOre.idDropped) and requires an iron-or-better pickaxe
  (genuine ItemPickaxe.canHarvestBlock: oreDiamond/blockDiamond need
  harvestLevel >= 2). Diamond tools/sword/armor recipes already existed,
  so mining a diamond with an iron pick completes the progression loop.

## SESSION LOG - Paperdoll genuine rewrite + painting wall-break pop

User: paperdoll placement/armor "a little misdone" - head should track,
body lightly track while staying front-facing with a tilt, legs looked
misshaped; also paintings must pop when the block behind breaks.

### Paperdoll (SurvivalInv_RenderDoll rewritten from genuine GuiInventory)
- The old doll used homebrew atan2 tracking (written before we had the
  in-20100223 source) and a PERSPECTIVE camera - the wide-angle
  distortion near the frame edge is what misshaped the legs.
- Now transcribed from genuine drawGuiContainerBackgroundLayer:
  ORTHOGRAPHIC GUI projection (glTranslate + glScale 30, no perspective),
  x+y-mirrored view (genuine's glScalef(-30,30,30) + RotZ 180 pair -
  preserves winding, mirrors the doll horizontally, maps y-up into the
  GUI's y-down; it IS a 180-degree spin, so the face-camera base yaw is
  0 in that frame, not 180 - first attempt showed the doll's back).
- Tracking math verbatim: dx/dy measured from the doll anchor (window
  centre, eye 50px above base in the 70px window) in genuine GUI px,
  body RotY = atan(dx/40)*20, head Yaw = atan(dx/40)*40 (double), head
  Pitch = -atan(dy/40)*20, whole-body lean RotX = -atan(dy/40)*20
  (lean and pitch stack, like genuine). Genuine multiplies raw radians
  by 20/40 and calls them degrees - transcribed as-is.
- Engine gotcha: Math_Atan2f(x, y) computes atan(y/x) - args look
  swapped vs libc atan2.
- Rig-verified both cursor directions: head+body turn toward the
  cursor, low cursor pitches the head down with the body tipping.
  Armor rides the same entity transform. User to confirm feel + armor.

### Painting pops when the wall behind breaks
- Genuine in-20100223 only re-checks onValidSurface at tickCounter==100
  (the counter resets ONLY when that check fails), so a wall broken
  later leaves the painting hanging forever - reads as a genuine bug
  (b1.x made it periodic). Per user request: every block change now
  re-validates all active paintings (SurvivalTest_BlockChanged), so
  breaking the backing wall pops the painting immediately. The
  tick-100 check stays.

## SESSION LOG - Fire + flint & steel (BlockFire port)

User: "finish the fire and flint and steel". Roadmap stage 4. New module
src/IndevFire.c/.h (picked up automatically - Makefile globs src/*.c).

### What landed (all Indev-gated)
- **Fire block 98** (genuine 51): DRAW_SPRITE custom mesh, full-bright
  (light 15), no collision, CanPlace=false (only flint & steel starts it),
  0 hardness (punching extinguishes instantly, drops nothing). Two
  animated flame tiles: 118 (existing) + new 120 (INDEV_FIRE_TEX_LOC2),
  each its own TextureFlamesFX instance (Animations.c refactored to a
  2-element sim array - genuine registers two).
- **Burn tables** (BlockFire ctor setBurnRate): planks 5/20, log 5/5,
  leaves 30/60, bookshelf 30/20, tnt 15/100, all 16 cloths 30/60
  (chance = encourages neighbours, ability = catches when consumed).
- **Scheduled-update queue** (World.java tickList port): entries wait
  tickRate=20 game ticks then run updateTick; <=200 processed per game
  tick. IndevFire_Tick() runs each 20Hz tick before the random-block
  pass. Ring buffer 8192 (drops new entries when full - a fire that big
  self-heals by constant rescheduling).
- **updateTick** verbatim: ages 0->15 in a per-map nibble store
  (freed on OnNewMap), retires when no flammable neighbour (or floor gone
  + age>3), else consumes neighbours (down 100 / up 200 / sides 300) and
  jumps to air cells in the 3x3 column up to y+4 with the genuine
  encourage-chance vs rand(bound) roll. Consumed TNT is armed
  (SurvivalTest_IgniteTnt -> ArmTnt), not deleted.
- **Flint & steel** (ItemFlintAndSteel.onItemUse): steps out of the
  clicked face, interior cells only, places fire in an air cell, wears the
  item by 1 (maxDamage 64) whether or not fire landed. Hooked in
  IndevTest_UseHeldItem before the hoe/seed branch.
- **Lava ignition** (BlockFlowing/BlockFluid fireSpread): lava flowing
  into a flammable block lights the first free spot around it
  (up/-x/+x/-z/+z/below) or the block itself. Hooked in
  Physics_PropagateLava before the normal flow.
- **Entity burning**: player takes 1 contact damage per tick standing in
  fire and is set alight (st_playerFire=300); alight burns 1 HP/sec,
  water fizzes it out (random.fizz), lava re-arms to 600 - genuine
  Entity.onEntityUpdate. Mobs get the same. First-person flame overlay
  (ItemRenderer.renderOverlays): two additive flame sheets at the bottom
  of the view while alight.
- **Chain armor recipe unlocked**: RecipesArmor's genuine material row is
  {clothGray, FIRE, ingotIron, diamond, ingotGold} - chain armor is
  literally crafted from fire blocks (a genuine, normally-unobtainable
  quirk). The armorSet loop now runs all 5 materials incl. chain (46).
- **.mclevel**: fire saves as genuine id 51 + age in the Data nibble
  (new position-aware IndevTest_BlockDataMetaAt/ApplyDataMetaAt so age
  comes from the side store, not the block id). setTickOnLoad reschedules
  every fire block on load.

### Rig verification
- **Mechanics CONFIRMED**: a save patched with a wood box wrapped in fire
  around spawn - loading it, the player spawned inside, took fire damage,
  and DIED (Game Over with the burning red overlay). Fire spread
  confirmed via live gdb block-count dumps growing 59 -> 85 fire blocks
  across ticks as walls caught. flint & steel placement, lava ignition,
  and a 5-fire-block save round-trip all confirmed via memory dumps.
- **Flame SPRITE beauty-shot NOT cleanly captured** on the rig: the
  test-world spawn is a lake (every fire ends up in/under water), the
  headless xdotool session started firing spurious Escape events on large
  mouse moves, and gdb position writes get overwritten by the physics
  tick each frame - so I could never frame a fire block against open sky.
  The burning-overlay red and the lethal box prove fire renders as a
  visible hazard, but the exact animated flame sheets should be
  eyeballed in a normal Indev world during playtest (same
  ship-then-confirm arrangement as paintings). If they look wrong,
  Builder_DrawFire is the single renderer to tune (genuine
  renderType-3 tessellation; grounded = 8 slanted sheets alternating the
  two flame tiles, wall/ceiling = leaning sheets per flammable
  neighbour, padded to 12 quads for the banked sprite layout).

### Traps hit
- The fire vertex macro captured `v->U` when its param was named U/V
  (the documented member-capture trap) - renamed to FU/FVV.
- A stale `case 51:` in BlockFromIndev (old CPE-fire fallback) became a
  duplicate once the real fire mapping was added - removed.

## SESSION LOG - Held items: genuine ItemRenderer chain, re-derived

User (after the u-unflip round): "the axe shaft is facing leftwards rather
than in the hand... would you be willing to reread how indev handles it".
Correct call - the sign-patching approach was chasing symptoms.

### What was actually wrong, in full
The first-person item was rendered by BAKING genuine's item-local
transforms into the mesh and then pushing it through the ENGINE's
held-block entity pipeline (Entity_GetTransform on held_entity with
Yaw/RotY -45). That pipeline's model-yaw convention is mirrored relative
to genuine's plain GL chain, so every rotation came out sign-flipped and
the sprite plane appeared mirrored. Each patch (-50 yaw, u-unflip, +eu
strips) fixed one symptom and exposed the next: after the unflip the
plane read unmirrored but the ROLL (RotZ 335) still leaned the wrong way
-> "shaft facing leftwards".

### The fix: run genuine's chain in genuine's frame
Numerically simulated the genuine GL chain (T(0.56,-0.52,-0.72) - RotY 45
- [swing rots] - S(0.4) - T(0,-0.3,0) - S(1.5) - RotY 50 - RotZ 335 -
T(-15/16,-1/16,0), column convention) to get the expected screen layout
of the mesh corners, then reimplemented it verbatim:
- The engine held pass ALREADY renders in genuine's frame: SetMatrix uses
  an identity-orientation camera at the eye, projection is the fixed 70
  degrees, SetBaseOffset's itemOffset IS genuine's hand anchor, and
  DoAnimation's dig translate / equip dip match genuine's swing translate
  exactly. Only Entity_GetTransform was the alien piece.
- HeldItem_Render now composes (row-major, reverse GL order):
  mesh chain [T(-15/16,-1/16,0), RotZ 335, RotY +50, S(1.5), T(0,-0.3,0),
  S(0.4)] then [genuine dig rotations RotX(-80 s5), RotZ(-20 s5),
  RotY(-20 sin(t^2 pi)) while breaking] then RotY(45) then
  T(held_entity.Position) then Gfx.View. No Entity_GetTransform.
- Mesh restored to VERBATIM genuine: u-mirrored plane (x=0 samples u2),
  v-flipped, edge strips -eu/-ev. All previous sign adaptations deleted.
- ClassiCube Matrix_RotateY/Z row-major matrices are exact transposes of
  glRotatef's column-major ones, so same-sign angles behave identically
  once Entity_GetTransform is out of the loop.
- The dig swing is genuine's chop again - through the correct pipeline
  this time (the earlier "fucked mining up massively" was these same
  rotations running on top of the mirrored entity transform).

### Rig verification (zoomed crops vs the simulated corner layout)
- Iron axe: fills the lower right, head upper-left with the blade facing
  LEFT into the scene, handle descending into the hand anchor - matches
  the simulation and genuine screenshots.
- Torch: single solid stick bottom-right -> upper-left, flame cap on the
  top end, clean extrusion shading, no missing pixels.
- Mid-dig frame: the item arcs down-forward (edge-on at the swing
  bottom) - the genuine chop.
- Held BLOCKS (non-extruded) untouched - still the engine block path.

## SESSION LOG - Wall torches + held-item mirror fix

User: "now we should work on torch hanging to walls" + spotted that held
items rendered left-right mirrored ("items in hand are mirrored lol").

### Wall torches (genuine BlockTorch metadata port)
- 4 new wall-variant block ids 94-97 = genuine torch (50) metadata 1-4
  (hanging on the solid block at -X/+X/-Z/+Z; standing torch 70 = meta 5).
  Same tile/brightness as the torch; MinBB/MaxBB = the genuine
  collisionRayTrace per-metadata pick bounds (the thin wall-hugging
  selection outline verified on the rig).
- **Placement** (BlockChanged placed-hook): genuine ItemBlock.onItemUse
  order - onBlockAdded's auto wall-pick (-X,+X,-Z,+Z walls then floor),
  overridden by onBlockPlaced's clicked-face mounting when that backing
  is a normal cube, then dropTorchIfCantStay (no support anywhere = the
  block pops straight back off as an item). Face comes from
  Game_SelectedPos (valid in the same input frame). Conversion via
  Game_UpdateBlock (no event, same pattern as container rotation).
- **Pop-off** (onNeighborBlockChange): every block change re-checks the
  six neighbouring torches; a wall torch pops (as an item) when ITS wall
  goes - it never re-mounts elsewhere - and a standing torch when its
  floor goes. Both genuine. Rig-verified: torch mounted on a plank
  pillar's side popped to a pickup-able item when the pillar was chopped.
- **Rendering** (Builder_DrawWallTorch): the genuine renderBlockTorch
  geometry - base shifted 0.1 into the wall and raised 0.2, bottom verts
  displaced a further 0.4 toward the wall (top stays -> the lean),
  full-tile side quads, 2x2px tip cap at 10/16 height interpolated along
  the lean line. Routed through the builder sprite path; the banked
  sprite vertex layout needs quad counts uniform per bank, so the 5 real
  quads are padded to 8 (2 per bank) with degenerate quads.
- **Spawn house**: genuine setBlockWithNotify runs onBlockAdded, so the
  genuine house torches HANG on the side walls (meta 1/2) - our gen now
  applies the same auto-mount after the notify pass (light unaffected:
  same emission, applied post-light). Rig-verified both house torches
  wall-mounted. scratchpad/parity_diff.py remap extended (94-97 -> 50).
- **.mclevel**: wall variants save as torch 50 + Data nibble meta 1-4
  (BlockDataMeta/ApplyDataMeta/BlockToIndev all extended) - round-trips
  with genuine Indev saves.
- isBlockNormalCube approximated as Blocks.FullOpaque (leaves/glass/
  slabs correctly rejected as torch supports, matching genuine).

### Held-item mirror fix (user-spotted on the iron axe)
- Genuine ItemRenderer builds the extruded plane u-mirrored because ITS
  camera views the plane's BACK; our -50 yaw (the opposite-apparent-sign
  workaround, see the pickaxe session) shows the camera the FRONT - so
  every held item read left-right flipped. Dropped the u mirror
  (u1=rec.u1 now); the v mapping stays flipped (model y=0 = sprite
  bottom row in both engines). Rig-verified zoomed: axe blade now faces
  left into the scene like genuine, torch flame still up.
- Full first-person orientation lineage for future reference:
  engine yaw -50 (not +50)  +  UNMIRRORED u  +  flipped v. If the yaw
  workaround is ever revisited, the u mirror flips with it.
- ROUND 2 (user: "still both broken... missing pixels"): the u unflip
  fixed the front/back faces but the per-pixel EDGE extrusion strips
  kept genuine's -eu half-texel nudge, which belongs to the old
  DECREASING u mapping - with the mapping now increasing, -eu sampled
  the NEIGHBOURING texel column, so outline columns hit transparent
  texels and alpha-tested away (the missing pixels; the held torch even
  showed a detached second stick from the offset strips). X strips now
  use +eu; the v mapping still decreases so Y strips keep -ev.
  Rig-verified zoomed: axe head solid with clean outline, torch a
  single stick. LESSON: the u1/u2 plane mapping and the strip
  half-texel nudges are one coupled system - flip them together.

### Rig verification summary
- House torches wall-mounted on both side walls, leaning correctly.
- /client give resolved "torch", "wood", "iron axe" ("planks" is not a
  name - the classic block 5 name is "Wood").
- Hand placement on a wall face mounts correctly (count consumed);
  clicking onto a torch's own pick box places nothing (genuine-ish).
- Standing placement, side placement on a free-standing block, pop-off
  with item return all verified.

## SESSION LOG - Huge-map lighting crash fix

User: reproducible ACCESS_VIOLATION crash right after generating a HUGE
(512x512) Floating Woods world (client.log supplied; also reproduced on
the Linux rig - the in-game handler stack showed IndevTest_LightLevel ->
ClassicLighting_IsLit -> ClassicLighting_GetLightHeight).

### Root cause (proven from the user's crash registers)
- IndevTest_LightLevel passed x/y/z STRAIGHT into Lighting.IsLit; the
  engine's ClassicLighting_GetLightHeight indexes
  classic_heightmap[z * Width + x] with NO bounds check (engine callers
  always pre-check World_Contains - see ClassicLighting_Color).
- Entities go out of bounds routinely: mobs wander off floating-island
  edges, and Mob AI path-weight sampling probes blocks AROUND a mob, so
  a mob standing at the border samples z = -1 immediately.
- The user's crash 6 registers nail it: rdx = 0xFFFFFFFFFFFFFF64 = -156
  = Lighting_Pack(356, -1) on a 512-wide map (-512 + 356), and the
  faulting address was heightmap base - 0x138 (= -156 shorts). QED.
- Why only HUGE maps crash: a 512x512 heightmap (512KB) crosses glibc's
  mmap threshold, so it gets its own mapping with unmapped guard space
  right before it -> negative index = instant segfault. Small maps'
  heightmaps live mid-heap, so the same bug silently read garbage there
  ("seemed to be a one time thing" on smaller maps).

### Fixes
- IndevTest_LightLevel now CLAMPS each coordinate to the world bounds -
  which is exactly what genuine World.getBlockLightValue does (it clamps
  var1/var2/var3 rather than rejecting), so this is parity, not just
  safety. Plus a !World.Blocks early-out (returns full light 15).
  One choke point covers every caller: Mob_Brightness, AI path weights,
  RenderMobs light, painting per-cell light, grass/zombie-burn checks.
- The c0.30 zombie/skeleton daylight-burn check called Lighting.IsLit
  directly with raw mob coords - same latent crash in classic mode.
  Guarded with World_Contains; out-of-bounds counts as lit, matching
  classic Level.isLit's out-of-bounds-returns-true.
- Audited every other Lighting.* call in our files: the Lighting.Color
  variants bounds-check internally (safe), the mob-spawn and grass-tick
  IsLit calls use already-validated in-bounds coords (safe).

### The user's client.log also contains an older crash family
- 4x "Textures must have power of two dimensions" aborts (a different
  build/exe base). All our embedded/created textures are pow2 (arrows
  32x32, cracks padded 160->256x16, plate 64x32, kz 256x256), so that
  signature likely predates the current build or involves their local
  texture pack - if it recurs on this build, get a fresh client.log.

### Rig verification
- Fixed binary: generated Huge/Floating/Woods (the exact repro), spawned
  in the wooden house, ran a 5-minute survival watch with initial mobs
  active - no crash (unfixed binary died to the same recipe within
  seconds on the first rig repro).

## SESSION LOG - Debug menu -> /client commands + swing revert

User: revamp the F9 debug menu (buttons outgrew the grid) into client chat
commands, delete the GUI entirely, make noai/armor per-spawn modifiers of
the spawn command, and check how Indev handles mob armor at spawn. ALSO:
the Indev mining swing animation read as broken in play - revert to the
original (pre-"genuine chop") animation.

### Swing animation revert (HeldBlockRenderer.c)
- The genuine-Indev X-chop dig branch (RotY -20 / RotZ -20 / RotX -80,
  ported from ItemRenderer's glRotatef chain) is GONE - user verdict after
  playing with it: "fucked mining up massively". Every mode now uses the
  original engine swing again (RotY/Yaw -= sin(sqrtLerpPI)*80, RotX +=
  sin(t*t*pi)*20), i.e. the exact pre-session code.
- KEPT: the held sprite-plane yaw (-50) fix and the extruded-item UV
  mapping - those addressed the static "face toward player" orientation
  complaint, not the swing. If the hand still reads wrong, that yaw
  constant in HeldBlockRenderer_RenderModel is the next knob.
- Lesson: on-paper genuine (transcribed glRotatef axes) lost to how it
  actually FEELS in this engine's held-item transform space; the engine
  applies its own base transforms, so genuine local axes don't land the
  same. Faithfulness calls here go to the user's hands-on verdict.

### Indev mob-armor parity fix (found per user's hunch)
- Genuine check: in-20100223 EntityZombie/EntitySkeleton have NO
  helmet/armor fields, MobSpawner.java assigns nothing, and the only
  armor rendering in the whole client is RenderPlayer (RenderLiving has
  no plate pass). Armored zombies/skeletons are a c0.30 Survival Test
  thing (HumanoidMob's 20% field-initialiser rolls).
- SurvivalTest_SpawnMobAt was rolling those 20% chances in BOTH modes ->
  natural Indev spawns could wear plate. Now gated !IndevTest_Enabled.
- The /client spawn "armor" modifier still force-equips them in either
  mode (debug-only visual, never natural).

### F9 menu -> /client commands
- Menus.c's whole SurvivalDebugScreen section (2 pages x 18 buttons),
  its Menus.h decl and the F9 InputHandler hook are DELETED. F9 is free.
- New command section at the bottom of SurvivalTest.c (registered in
  SurvivalTest_Init only while survival mode is on - normal ClassiCube's
  /client help never sees them; unregistered in Free):
  - /client spawn <zombie|skeleton|spider|creeper|pig|sheep|tnt|drops|
    arrow> [count<=10] [noai] [armor] - modifiers in any order
  - /client give <name|id> [count<=99] - names via the Indev item table
    (spaces/underscores/case all ignored: "iron_pickaxe"/"Iron Pickaxe"/
    "ironpickaxe"), then Block_Parse fallback; raw ids too (256+ ids
    require Indev + a known item). Reports "inventory full" on overflow.
  - /client time [dawn|noon|dusk|midnight|0-23999] - Indev only; no arg
    prints the current time
  - /client god (toggle, prints state) / heal [n] / hurt [n]
  - /client arrows [n] - c0.30 quiver only; Indev points at give arrow
  - /client mobs [kill] - census (alive/cap/roll) or kill-all
- IndevTest gained the name helpers: IndevTest_ItemName(id) and
  IndevTest_FindItemByName (both NULL/-1 outside Indev, which auto-gates
  giving items in classic mode).
- SurvivalTest_DebugSpawnMob signature: (type) -> (type, noAI, forceArmor);
  the global st_debugNoAI/st_debugForceArmor toggles are gone (they were
  only ever F9-menu state). God mode stays a toggle.
- Menus.c gotcha: the file is CRLF in most sections but the include block
  is LF - and IndevTest.h is still needed there (IndevGenScreen uses
  IndevTest_Enabled at ~1392), only the GUI section was deletable.

## SESSION LOG - Paintings (EntityPainting port)

User: "work on paintings next, try to remember box bounds and such as well
as like with farmland" - i.e. the bounding-box math must be EXACT (the
farmland-height lesson). Everything Indev-gated; c0.30 untouched.

### What landed
- **Painting item** (id 321 = 256+65, "Painting", icon 26) + the genuine
  Indev RecipesArt recipe: 8 planks ringing a cloth block -> 1 painting.
- **19 EnumArt entries** (Kebab..Fighters/Skeleton/DonkeyKong etc) with the
  genuine sizeX/sizeY/offsetX/offsetY atlas table from EnumArt.java.
- **Genuine EntityPainting geometry, verbatim** (Painting_SetDirection):
  yaw = dir*90; center starts at tile center, pushed 0.5 - 1/16 toward the
  wall along the facing axis; half-extents sizeX/32 x sizeY/32 x 0.5/32
  (a 1/16-thick slab); arts with a dimension >= 32px get the genuine +0.5
  recentring - and because `func_190_a(px)` returns 0.5 for BOTH 32 and
  64, the 64px-wide arts (Fighters, Pointer, Pigscene, BurningSkull era
  sheet has Fighters only) sit genuinely OFF-CENTRE. Kept, it's source
  behaviour. Finally the bbox MAX corner is shrunk by 0.1/16 on ALL axes
  (genuine asymmetric shrink - min corner untouched). None of this is
  "cleaned up"; it is a line-for-line transcription.
- **onValidSurface, all 3 genuine checks**: (1) no solid-block collision
  inside the painting bbox, (2) EVERY 16px cell must be backed by a solid
  block in the wall behind, (3) no overlap with another painting.
- **Placement** (ItemPainting.onItemUse): right-click a wall face with the
  painting held; Y faces rejected; face->dir map ZMIN->0 XMIN->1 ZMAX->2
  XMAX->3; tries every art, picks a RANDOM valid one (genuine random
  choice among the arts that fit that wall spot); consumes the item.
  Interior (in-map) blocks only. Hooked before block placement.
- **The genuine tick-100 quirk**: EntityPainting.onUpdate re-checks
  onValidSurface ONCE, at tickCounter == 100 exactly (then never again -
  the counter keeps incrementing past it). If invalid, pops off as an
  item. Kept as-is: break the wall behind a painting and it hangs there
  until its own tick counter happens to cross 100, exactly like genuine.
- **Any hit pops it off** as a painting item drop: melee punch (ray-vs-
  slab test capped by the mob/TNT pick distance so you can't punch a
  painting through a zombie) and arrows (arrow is consumed, genuine
  attackEntityFrom path).
- **Renderer** (RenderPainting port): per-16px-cell quads - front face
  samples the art region RIGHT-TO-LEFT (genuine u-mirror), back face is
  the canvas-back cell (u 192..208 px), 1px edge strips around the rim,
  each cell lit individually with the block light AT that cell's world
  position (genuine getEntityBrightness-per-cell look). /256-atlas UVs.
  Basis vectors per dir: 0 along +X normal -Z; 1 along -Z normal -X;
  2 along -X normal +Z; 3 along +Z normal +X.
- **art/kz.png shipping** (Resources.c): pulled from the b1.7.3 jar like
  the other assets, BUT b1.7.3 redrew exactly two regions vs authentic
  in-20100223 (Sea 64,32 32x16 - later split into Sea+Plant; Stage 64,128
  32x32 - the Graham cell art changed). The authentic cells are embedded
  as PNG byte arrays and composited over the beta sheet at decode time,
  so the shipped kz.png is pixel-identical to in-20100223's. New
  TextureEntry in IndevTest.c (IndevTest_KzTex). Users must delete
  texpacks/default.zip once to pick the new asset up.
- **.mclevel persistence**: paintings save as genuine "Painting" entity
  compounds (Dir byte, Motive string, TileX/TileY/TileZ ints + the
  standard Pos/Rotation/Motion entity fields, Rotation = dir*90) and load
  back through the same parser as mobs/items; unknown Motive strings fall
  back to Kebab. Round-trips with genuine Indev saves' painting format.

### Verification status
- Compiles clean (-Werror). Armor/bow/generator regression: untouched
  paths, same build. NOT yet rig-tested in-game (user asked to ship
  without the headless pass this session - usage constraints); next
  session: inject painting items into a save, hang several art sizes,
  verify orientation/lighting/pop-off/save round-trip on the rig.

### Traps hit (for future reference)
- The renderer's per-vertex macro originally named its params U/V - and
  `v->U` expanded the MEMBER access too (macro capture). Params renamed.
- The lazily-created painting dynamic VB must be registered in BOTH
  SurvivalTest_OnContextLost and SurvivalTest_Free like every other
  dynamic VB (caught in the pre-commit audit - a context loss would have
  left a stale handle).
- Painting helpers are used by earlier code in SurvivalTest.c (RenderMobs
  tail, TryAttackMob, the arrow loop) - forward decls live next to the
  SurvivalTest_AddItem decl block.

## SESSION LOG - Bow item + Indev HUD cleanup (roadmap stage 3)

User: implement the bow, remove Tab-firing and the score count (Indev).

### What landed (all Indev-gated; c0.30 keeps its Tab-fire/score/arrow kit)
- **ItemBow.onItemRightClick** (SurvivalTest_TryUseBow): right-click while
  holding the bow consumes ONE arrow item - consumeInventoryItem semantics,
  the FIRST slot holding arrows in inventory order - plays random.bow at
  the genuine 1/(rand*0.4+0.8) pitch and looses an arrow from eye height at
  the genuine 1.5 speed / 4 damage (genuine EntityArrow hits for a flat 4).
  A dry bow handles the click but does nothing. The bow has NO durability
  in in-20100223. Hooked into InputHandler_PlaceBlock after TryUseBlock, so
  chests/workbenches still open and it fires with or without a block
  targeted. (Bow + arrow crafting recipes already existed.)
- **Tab-fire disabled in Indev** (gate inside SurvivalTest_TryShootArrow -
  covers every caller); arrows are items fired only by the bow.
- **HUD**: the "Score: &eN" top-right label and the "Arrows: N" counter are
  c0.30-only now - genuine in-20100223 GuiIngame draws neither. The death
  screen keeps its score line (genuine GuiGameOver has it).
- **Start kit**: the 10 TNT + 20 arrows are SurvivalGameMode.apply (c0.30);
  genuine Indev starts with an EMPTY inventory - both gated to classic.
- **Arrow pickup**: stuck player arrows return to the inventory as arrow
  ITEMS (left stuck when the inventory is full, like genuine playerTouch).
- **Stuck-arrow lifetime**: genuine EntityArrow dies at EXACTLY
  ticksInGround == 1200 (player and mob arrows alike) - replaces c0.30's
  1%-per-tick-after-300 roll in Indev mode.
- Skeleton deaths already dropped arrow ITEMS in Indev (earlier session).

### Rig verification (injected bow + 5 arrows save)
- No Score label, no Arrows counter, empty start inventory on a fresh
  Indev world; Tab does nothing; three right-clicks fired three arrows
  (count 5 -> 2, arrows visibly stuck in the wall at the crosshair);
  fired one more and walked over it - count restored (pickup as item).

## SESSION LOG - Held pickaxe swing + held torch fixes

User reports: the first-person pickaxe looked flipped while MINING (the
swing angle read wrong), and a held torch floated at arm's length.

- **The real pickaxe bug was the SWING, not the sprite.** The extruded
  mesh's original u-mirrored + v-flipped sampling IS the correct genuine
  idle pose (head top-left, tip toward the screen centre, handle into the
  fist) - an intermediate "fix" that unflipped the UVs rendered it upside
  down and was reverted (lesson relearned: judge orientation from ZOOMED
  crops, 1x rig screenshots misread easily; see icon98.png vs tl*/tm*
  zooms). The actual fix: the engine's dig animation uses the CLASSIC
  c0.30 swing - translate identical to genuine, but the dominant 80-degree
  rotation around Y/yaw (sideways sweep, fine for a cube in hand). Genuine
  Indev ItemRenderer chops 80 degrees around X (pitch-down), with
  20-degree yaw/roll accents (glRotatef -sin(t^2 pi)*20 y,
  -sin(sqrt(t) pi)*20 z, -sin(sqrt(t) pi)*80 x). HeldBlockRenderer's
  DigAnimation now applies the genuine axes in Indev mode (classic mode
  keeps the classic sweep) - the tool now chops DOWN head-first into the
  block.
- **Held torch** (HeldBlockRenderer.c): genuine ItemRenderer holds only
  renderType 0 blocks as 3D blocks; torches (renderType 2) and flowers/
  mushrooms/saplings (renderType 1) use the SAME extruded sprite path as
  items, drawn from their terrain tile. Added the
  IndevTest_HeldIsExtruded(block) branch to the held renderer (the block
  model path had drawn the torch at the block-in-hand anchor - the
  floating look). With the genuine v-flipped mesh mapping the terrain
  TexRec needs NO extra flip (flame up verified zoomed).
- Verified on the rig with an injected pickaxe/torch save: genuine idle
  pose, the chop swing arcing head-first into the crack, torch flame-up
  in the fist. Equip-dip frames right after slot switch look odd in
  stills - wait for the dip to settle before judging screenshots.
- **Round 2 (user: "the pickaxe FACE is facing you rather than mining")**:
  the sprite PLANE's yaw was wrong - genuine stacks rotY 45 (base) + 50
  (sprite chain) so the plane slices INTO the scene at ~95 degrees (near
  edge-on; the 1/16 extrusion is what you actually see - that IS the b1.x
  3D-item look). The engine's held-entity chain contributes its 45 with
  the opposite apparent sign in this pipeline, so the literal +50 nearly
  cancelled and the item faced the camera flat-on. Empirically swept the
  yaw via a temporary env knob: -50 (net ~-95) reproduces genuine's
  oblique; +140 and +50 do not. Hardcoded -50 with a comment. All
  extruded held items (tools + torch + flowers) get the genuine slice.

## SESSION LOG - Armor part 2: worn-armor rendering + fuzz-verified math

### Damage/wear calcs FORMALLY VERIFIED (user asked for this explicitly)
scratchpad/ArmorFuzz.java pits a verbatim port of the genuine decompiled
logic (EntityPlayer.attackEntityFrom + InventoryPlayer.getPlayerArmorValue +
ItemStack.damageItem) against an exact transcription of our C implementation:
random loadouts over all 5 sets with random pre-wear and missing pieces,
random 1-30 hit sequences of 1-20 damage, comparing health, damageRemainder,
per-piece wear and breakage after every hit. **6,000,000 trials across 3
seeds: zero mismatches.** (Breakage semantics included: a piece survives at
wear == maxDamage exactly and breaks only when EXCEEDED, like damageItem.)
Plus the earlier live test: 8-damage TNT vs full pristine iron = 1 HP lost,
remainder 15, +8 wear on all four pieces.

### Worn-armor renderer (src/IndevArmor.c, new)
Genuine RenderPlayer.shouldRenderPass: up to four overlay passes per player -
pass 0 helmet = head + headwear, 1 chest = torso + both arms, 2 legs = torso
+ legs on the _2 texture at HALF inflation (ModelBiped(0.5F)), 3 boots =
legs at full inflation; armorInventory indexed [3 - pass]; texture picked by
ItemArmor.renderIndex (cloth/chain/iron/diamond/gold).
- Implemented as two engine models ("indev_armor" 1.0-inflated,
  "indev_armor2" 0.5-inflated) whose boxes are the ENGINE humanoid's boxes
  (same pivots, same swapped-coordinate UV mirroring for left limbs)
  expanded via the Dims/Bounds pattern the engine's hat layer uses - UVs
  stay the unexpanded skin cells. Headwear box gets f+0.5 like genuine.
- Draw binds the armor texture itself and forces uScale/vScale to the 64x32
  layout (the wearer's skin may be 64x64); Models.Active->index reset like
  the c0.30 MobArmor overlay.
- **TRAP: Model MakeParts only runs lazily via Model_Get(name)** - a model
  rendered directly with Model_Render never inits, and draws stale VB
  garbage that can look deceptively like armor (re-textured copies of the
  previous model's vertices). IndevArmor_Register Model_Gets both models
  once to force part building.
- Textures: armor_{cloth,chain,iron,diamond,gold}_{1,2}.png in the texture
  pack, registered as ModelTex entries. Resources.c BetaPatcher pulls them
  from the beta jar's /armor/*.png - VERIFIED byte-identical to the
  authentic in-20100223 assets in the EaglerPorts resources tree.
- Hooks: third-person local player at the end of SurvivalTest_RenderMobs
  (never in first person, like genuine), and the inventory paperdoll right
  after its Model_Render.

### Rig verification
- Full iron set renders on the third-person player (helmet enclosing the
  head, chestplate with shoulder-pad arms, leggings, chunky boots), poses
  tracking the body. Swapped the diamond chestplate in through the GUI:
  mixed set renders each piece with its own texture (gdb-confirmed slot
  state through the swap).
- KNOWN RIG ARTIFACTS (not code bugs): the paperdoll window renders black
  on the headless software-GL rig (predates the armor work - the doll armor
  hook is in place and shows wherever the doll shows); the rig texpack's
  stale terrain.png had a magenta placeholder in the torch cell - patched
  the rig copy (real installs build default.zip through Resources.c which
  patches it properly).

## SESSION LOG - Armor system part 1: mechanics + slots + GUI + persistence

Roadmap stage 2. Everything below is Indev-gated; c0.30 mode untouched.

### What landed
- **ItemArmor helpers** (IndevTest.c): the 20 armor items already existed in
  the item table (local ids 42-61, param = piece 0 helmet..3 boots).
  IndevTest_ArmorPiece/ArmorMaxDamage/ArmorReduce implement genuine
  ItemArmor: reduce {3,8,6,3} by piece, maxDamage {11,16,15,13}[piece]*3<<tier
  with set tiers cloth 0 / chain 1 / iron 2 / diamond 3 / GOLD 1 (gold armor
  genuinely has chain-tier durability).
- **Armor slots**: st_armor[4] in genuine armorInventory order ([0] boots ..
  [3] helmet, piece = 3-index), addressed as extended slots 72-75
  (SURVIVAL_ARMOR_BASE). SlotPtr/SlotClick route them; SlotArmor.isItemValid
  is enforced in SlotClick (placement/swap needs the matching piece; taking
  out is always allowed). ResetState/Respawn clear them; death drops them
  (DELIBERATE DEVIATION - see the death-scatter entry below; there is no
  dropAllItems in either ground truth).
- **Damage absorption** (SurvivalTest_Damage, Indev branch): genuine
  EntityPlayer.attackEntityFrom - NO delta damage during the invulnerability
  window (c0.30's ghost-heart delta stays c0.30-only), armorValue =
  (sumReduce-1)*remainingDurability/totalMax+1 (getPlayerArmorValue), scaled
  = dmg*(25-armorValue)+remainder, applied dmg = scaled/25 with scaled%25
  carried in damageRemainder. Every worn piece takes the RAW damage as wear
  (breaking at maxDamage), even when the final result rounds to 0.
  Difficulty scaling is skipped (fixed normal difficulty).
- **Recipes**: RecipesArmor generated family - helmet "XXX/X X", chest
  "X X/XXX/XXX", legs "XXX/X X/X X", boots "X X/X X" from gray cloth/iron
  ingot/diamond/gold ingot. CHAIN armor (genuinely crafted from FIRE blocks)
  is deferred until BlockFire exists.
- **GUI** (SurvivalInvScreen): 4 armor slots at genuine GuiInventory
  positions (x=8, y=8+row*18, helmet on top) on the POCKET inventory only
  (workbench/chest/furnace GUIs have none, like genuine); empty slots draw
  the items.png piece silhouettes (Slot.getBackgroundIconIndex = 15+(piece
  <<4)); durability bars work on armor; full drag/drop with the type gate.
- **.mclevel persistence**: armor saved/loaded as the genuine Slot 100+index
  numbering under the player's Inventory list, both directions.

### Rig verification
- NBT-injected a full iron set + spare diamond chestplate into a save:
  loaded correctly into the slots (gdb-verified st_armor contents), sprites
  + silhouettes render at genuine positions, helmet picked up onto cursor,
  dropping it on the chest slot REJECTED (state unchanged), resave
  round-trips Slot 100-103 byte-correctly.
- Absorption: TNT blast dealing 8 raw damage against full pristine iron
  (armorValue 20) cost exactly 1 HP with damageRemainder 15 and +8 wear on
  all four pieces - matching the genuine 25ths math to the digit.
- Rig texpack default.zip was missing items.png/inventory.png etc (why item
  sprites looked absent in earlier screenshots) - now added to the rig copy.

### Still to do (armor part 2)
- Worn-armor RENDERING: paperdoll + third-person overlay boxes with the
  armor textures (armor/{cloth,chain,iron,diamond,gold}_1/_2.png from the
  b1.7.3 jar, same 64x32 layout Indev uses), helmet=head, chest=body+arms,
  legs=legs (layer _2), boots=inflated legs. Needs texpack asset patches +
  a per-piece humanoid overlay model.
- Genuine chain-armor recipe once BlockFire lands (stage 4).
- HUD: genuine in-20100223 GuiIngame has NO armor bar - nothing to add.

## SESSION LOG - Generator round 3: BIT-EXACT parity with genuine Java

User asked to perfect the generator "to a tee". The standard adopted: for the
same seed, the C port must produce a BYTE-IDENTICAL block array to the genuine
in-20100223 LevelGenerator at every phase boundary. It now does.

### Verification harness (how to reproduce)
- **Java oracle**: the genuine LevelGenerator/World/Light/Block classes from
  /tmp/indev_eagler compiled standalone with entity/item stubs, at
  scratchpad/oracle (`java -cp out Main <seed> <type> <theme> <W> <L> <H> <dir>`).
  Three patches only, all seed-determinism: LevelGenerator.setSeed(seed),
  World.load()'s `new Random()` -> seed+1, findSpawn's `new Random()` -> seed+2.
  A Dump IProgressUpdate snapshots blocksByteArray at every
  displayLoadingString - 13 phase dumps per run (more on multi-layer floating).
- **C side**: linux-only env hooks in IndevGen.c - CC_INDEVGEN_SEED forces the
  seed in Prepare, CC_INDEVGEN_DUMP=dir snapshots Gen_Blocks at every
  IndevGen_SetState in the same numbering. scratchpad/parity_run.sh drives the
  rig (xdotool) per config; parity_diff.py compares (remapping engine torch
  70->genuine 50, diamond 93->56).
- **Results**: ALL PHASES IDENTICAL for island/12345, inland/777,
  floating/4242, flat/555, hell-island/666, paradise-inland/888,
  woods-island/999 (51 tree passes), deep-floating 64x64x256/31337
  (5 stacked layers, 29 phase dumps). Raising through Spawning, byte for byte.

### Bugs found and fixed to get there
1. **min() macro re-evaluation in LavaGen** - `min(Random_Next(..),
   Random_Next(..))` expands both args twice, drawing 6-7 ints instead of
   genuine's strict left-to-right 4. Desynced the stream from Melting onward.
   Now draws d1..d4 explicitly.
2. **MathHelper sine table** - carve/ore worms now use the genuine 65536-entry
   float table (sin(f)=table[(int)(f*10430.378F)&0xFFFF], cos +16384) instead
   of libm sinf/cosf. TRAPS: ExtMath's Math_Sin is a float-precision fake
   (wraps Math_SinF), and MATH_PI is a FLOAT literal - the table init needs
   __builtin_sin with a true double pi or entries differ by 1 ulp (seen as a
   single flipped cave-boundary cell on seed 777). Verified glibc sin ==
   OpenJDK StrictMath.sin == Math.sin bit-exactly across all 65536 entries.
3. **Double vs float precision** in Raising/Soiling: genuine Math.abs/
   Math.sqrt/Math.signum run in DOUBLE (island edge falloff, floating
   cliff cut) with the quirky `(double)1.2F` constant; float versions shifted
   (int) casts off by one occasionally. Also all pi-in-float-chains use
   (float)Math.PI = 3.1415927f, not double MATH_PI.
4. **defaultFluid is the MOVING liquid** - Assembling's border fluid band is
   waterMoving(8)/lavaMoving(10), not the still ids.
5. **Three RNG streams like genuine**: the seeded generator rand (terrain,
   positions); World.random, recreated in load() at the END of Assembling
   (one nextInt burned for randId) - drives tree SHAPES and item-drop rolls;
   findSpawn's own fresh Random. Previously one stream did everything, so
   findSpawn perturbed the tree/flower sequence.
6. **Genuine World-semantics replica (WR_* in IndevGen.c)** for Building/
   Planting: World.generate()'s one-time light init (heightMap by
   lightOpacity scan - water 3, lava 255, leaves 1, opaque 255; light nibble
   = y>=heightMap ? skylightSubtracted : 0, max blockLightValue; theme
   skylight hell 7 / woods 12 / else 15) - light updates after that are only
   QUEUED until the Lighting phase, so every planting pass reads this static
   snapshot. setBlock refuses the outermost map shell; setBlockWithNotify
   fires the neighbour reactions that matter during gen: sand/gravel
   tryToFall (with genuine's coordinate-CLAMPING getBlockId reads and the
   fall-out-of-world vanish), flowers/mushrooms popping when canBlockStay
   fails (burning 4 World.random floats each), still liquid waking to MOVING
   liquid (canFlow + sponge scan) and water<->lava contact turning to stone.
   growGrassOnDirt converts EVERY lit dirt in the volume (not just surface).
   Flowers use canBlockStay (light>=8, or >=4 with sky; mushrooms light<=13
   on opaque) - a fresh tree canopy does NOT block them because the light
   snapshot predates the trees, a genuine quirk faithfully preserved.
7. **findSpawn clearance/foundation** use genuine Material.isSolid /
   opaqueCubeLookup with CLAMPED out-of-range reads (not skip-as-air).
8. Ores run under the "Carving.." banner (no separate "Mining.." phase);
   Lighting/Spawning announced for phase parity.

### Still intentionally different from genuine (documented, gameplay-level)
- Engine block ids: torch 70 (genuine 50), diamond ore 93 (genuine 56) -
  remapped at .mclevel save/load; block ARRAYS otherwise identical.
- Classic cross-mode substitutions remain: coal for diamond ore, no house
  torches (c0.30 purity).
- House torches are floor-model; genuine metadata would wall-mount them
  (onBlockPlaced picks metadata 1/2 against the house wall) - cosmetic,
  metadata is outside the block array.
- Mob spawning (1000 MobSpawner passes) is entities only, runs post-load.

## SESSION LOG - Generator round 2: Assembling pass, floating basin, spawn height

User reports after testing the generator: floating maps still had "dirt at the
bottom" instead of an empty basin with bedrock, and the spawn put your head
inside the house ceiling.

### Fixes
- **Missing "Assembling.." pass ported** (`IndevGen_Assemble`). Genuine
  LevelGenerator line 425 calls `World.generate()` AFTER the edge flood and
  before findSpawn - a pass my first port skipped entirely. It rebuilds the
  bottom/borders: interior columns only touch y=0, y=1 and the very top layer
  (the y-skip quirk at World.java:118 - `if (var7 == 1 && interior) var7 =
  height - 2`), border columns are rebuilt in full: bedrock below
  groundLevel-1 (still lava at y<=1 where the block above is air), a
  grass/dirt cap at groundLevel-1 (grass iff groundLevel > waterLevel and the
  default fluid is water, so inland=grass, island=submerged dirt), then
  defaultFluid up to waterLevel. On Floating maps groundLevel=-128 makes every
  branch miss, so everything the pass touches becomes AIR - that is what
  hollows out the basin (borders + map floor) under the islands. Writes are
  UNCONDITIONAL (overwrites terrain), exactly like genuine.
- **Env sides are now bedrock, not dirt** (`IndevGen_ApplyPostLoad`). The
  engine always draws a SidesBlock-textured plane covering the map footprint
  at y=0 (EnvRenderer.c BuildMapSides), plus side walls from Env_SidesHeight
  up. With SidesBlock=DIRT that plane is what the user saw as "dirt at the
  bottom" of floating maps. Bedrock matches both the genuine border shell and
  the classic default, and gives floating maps their empty bedrock basin
  (EdgeHeight=-127 hides the water plane, Env_SidesHeight=-128 sinks the
  walls, but the y=0 floor plane is unconditional).
- **Spawn height fixed**: genuine `preparePlayerToSpawn` puts the bounding box
  CENTRE at ySpawn (posY is the bbox centre in Indev - Entity.setPosition), so
  feet sit at ySpawn-0.9, i.e. 0.1 above the house floor (house interior air
  is ySpawn-1..ySpawn+1, ceiling at +2). My port used feet = ySpawn+1.0, two
  blocks too high - head inside the ceiling. Now `pos.y = spawnY - 0.9f`.
- **Indev random ticks no longer run the engine's classic sand/gravel/
  dirt/still-liquid handlers** (IndevTest_TickRandomBlocks skip list). Genuine
  in-20100223 coverage: BlockSand/BlockGravel/dirt have NO updateTick (sand
  falls only on place/neighbour change) and BlockStationary.updateTick is
  EMPTY. The engine registers Physics_DoFalling on OnRandomTick for
  sand/gravel (a classic-mode behaviour), which made floating islands rain
  their sand/gravel down within seconds and (via activation cascades and
  liberated water pockets) flood the entire basin with water in minutes.
- **Genuine Indev grass tick implemented** (`IndevTest_TickGrass`, replaces
  the classic HandleGrass/HandleDirt pair in Indev mode): covered grass decays
  to dirt on a 1-in-4 roll; lit grass spreads to one random nearby dirt block
  (+-1 x/z, y-3..y+1). Dirt itself never ticks - it only becomes grass by
  spreading (the classic behaviour of dirt spontaneously regrowing grass is
  NOT Indev). Genuine light thresholds (<4 decay, >=9/4 spread) approximated
  with the engine's binary sky lighting - documented deviation.

### Rig verification (saved worlds parsed offline)
- Indev mode, Floating/Square/Small: two .mclevel saves 40s apart were
  IDENTICAL (0 block diffs) - no sand rain, no flood, still-water pockets
  intact (92 still water, 24 still lava), house torches present (2x id 50),
  y=1 fully empty, borders fully air, no physical bedrock (basin is env
  rendering). Spawn inside the house with head clear of the ceiling.
- c0.30 mode cross-gen (same floating map under classic physics): classic
  random-tick sand DOES rain and liberated pockets DO flood the basin over
  minutes - that is classic physics faithfully applied to a foreign map type,
  left as-is by the mode-purity mandate.
- FOLLOW-UP RESOLVED: the "~500 sand/gravel drop once at load" artifact was
  an artifact of the rig itself - the Indev-mode test ran a STALE binary (a
  failed `sed && grep && cp` chain skipped the copy), confirmed by gdb:
  Physics_DoFalling was reached from IndevTest_TickRandomBlocks, i.e. the
  skip-list fix wasn't in the running build. Re-run with the checksummed
  current binary: y=0 and y=1 completely EMPTY immediately after generation
  and still empty ~2 minutes later; the only diffs in that window were 7
  surface mushrooms dying in sunlight (authentic - mushrooms need darkness).
  Nothing falls at load at all.
- Genuine-authentic oddity seen while retesting: a Floating seed exhausted
  findSpawn's 1,000,000 attempts (no 7x9 opaque foundation + clear interior
  anywhere), so it used the genuine fallback ySpawn = height+100 - you
  skydive onto the islands (and can die of fall damage or miss entirely).
  Genuine does exactly this; generateHouse is bounds-checked/skipped for the
  sky spawn, so nothing corrupts.
- The earlier "255/garbage block ids in saves" scare was a parser off-by-one
  in my own analysis script (+14 instead of +13 after the 13-byte
  '\x07\x00\nBlockArray' tag), reading NBT metadata as blocks. Saves clean.

## SESSION LOG - Indev world generator (roadmap stage 1, WORKING)

### What landed
- **src/IndevGen.c/.h**: full port of LevelGenerator.java + the noise stack
  (NoiseGeneratorPerlin/Octaves/Distort), registered as a third engine
  MapGenerator alongside FlatgrassGen/NotchyGen. Passes, in genuine order:
  Raising (distorted-octave heightmap, island edge falloff), Eroding,
  Soiling (dirt/stone fill, floating-layer carve-out; loops per 48-block
  layer on Floating worlds), Growing (gravel/sand beaches; hell's odd
  grass-beach quirk), Carving (worm caves, r up to ~4.7, y-squashed
  ellipsoid), ore veins (coal 1000/10, iron 800/8, gold 500/6, diamond
  800/2 with per-ore height caps), Melting (lava pockets, quad-min depth
  bias), Watering (spring pockets < 640 cells via probe flood; edge ocean
  flood at waterLevel-1; hell floods lava), theme colours/brightness
  (hell 7 / paradise 16 / woods 12), findSpawn (mid-map, above water,
  house-clearance + opaque-foundation checks, 1M-attempt sky fallback),
  generateHouse (7x5x7 stone/plank shell, obsidian floor slab, doorway on
  -Z, two torches), grass/trees/flowers/mushrooms, then post-load the
  1000 MobSpawner passes (SurvivalTest_IndevInitialSpawn).
- **UI**: IndevGenScreen = genuine GuiNewLevel (World type/Shape/Size/
  Theme cyclers + Generate). Cross-linked per user request: the classic
  gen screen has an "Indev..." button and the Indev screen a "Classic..."
  button, so EITHER mode can use EITHER generator. Indev mode opens the
  Indev screen first. Sizes 128<<n; Long = w/2 x 2w; Deep = w/2 sq x 256.
- **Post-load hook**: GeneratingScreen_EndGeneration calls
  IndevGen_ApplyPostLoad - spawn-in-house (player Spawn + yaw 180), theme
  env via new IndevTest_SetBaseEnvColors (live + day/night baseline
  together), EdgeHeight/SidesOffset from water/groundLevel, hell lava
  edge fluid, initial mobs. Falls back to default spawn for classic gens.
- **New block 93 Diamond Ore** (genuine 56, tile 119 patched from b173
  tile (2,3)); .mclevel maps 56 <-> 93 exactly now (was coal-visual).
  Cross-mode: classic-mode Indev generations substitute coal ore for
  diamond and skip house torches (blocks 70/93 undefined there).
- **Engine additions**: Gen_SetDone() exported (gen_done was a
  Generator.c static only internal gens could set - without it the
  "Generating level" screen never ended: THE hang symptom, main thread
  fine at 600fps, gen thread exited, gen_done false forever).

### Deviations from genuine (documented, low-impact)
- MathHelper's 65536-entry sine TABLE -> libm sinf/cosf (identical
  distribution; caves/veins differ per-seed but not statistically).
- growGrassOnDirt light>=4 and flower canBlockStay -> "sky-exposed"
  approximation (generation precedes engine lighting; loses only grass
  just inside cave mouths).
- Engine RNGState IS java.util.Random-compatible, so nextInt/nextFloat
  match exactly. floodFill's segmented 1M-int stack -> growable stack.
- COOPTHREADED platforms (web): IndevGen_Generate runs monolithically
  in one call (no yield points) - fine on desktop, would hitch on web.

### Performance notes (user asked: why does classic gen feel instant?)
Measured on the rig (llvmpipe VM, slow CPU): Small inland ~2s, Normal
island ~10-15s of real work. Genuine Indev was similarly slow in 2010
(it even ran 10000 updateLighting rounds we skip). Why Indev >> classic:
- NotchyGen evaluates ~10-20 noise samples per column; Indev's pipeline
  is ~130+ DOUBLE-precision Perlin evaluations per column: Raising alone
  is two Distorts (each = 2x octaves8 = 16 perlins, and the distort input
  doubles it) + octaves6 + octaves2, then Eroding re-creates two more
  distorts, Soiling 2x octaves8, Growing 2x octaves8.
- Future optimization directions (fidelity-safe):
  1. Specialize Perlin to true 2D (genuine samples 3D at z=0; the w/fz
     lerp arm is dead weight - ~40% fewer ops, bit-identical results).
  2. Evaluate each octave layer for the whole heightmap in one cache-
     friendly sweep (array of doubles) instead of per-column call trees.
  3. Multithread the per-column loops (columns are independent; only the
     shared RNG for octave INIT needs ordering - noise evaluation itself
     is pure table lookups).
  4. Precompute the two erosion noises into arrays (they're sampled at
     (x<<1, z<<1) - a quarter of the samples repeat).
  5. floodFill edge-ocean pass scans full borders even over solid rock -
     early-out when the start cell isn't air.
  The 1000 post-load mob passes and the flood fills are NOT hot; the
  Perlin tree is ~90% of the time.

### Verified in rig / still to verify
- Verified: gen screen cyclers (note: xdotool needs mousemove THEN click
  as separate calls), progress phases display, Island-Normal generates,
  player spawns INSIDE the genuine plank/stone house, no crashes.
- Still to eyeball (user/next session): island coastline shape, cave/ore
  density, hell lava sea + dark sky, paradise/woods themes, floating
  layers, Deep worlds (256 tall), house torches, initial animal spread,
  first-gen "screen reopened" oddity (likely a stacked-pause-menu quirk,
  reproduce before chasing).

## ROADMAP (agreed 2026-07, in priority order)
1. **Indev world generator** (IN PROGRESS): full LevelGenerator.java port -
   island/inland/floating/flat shapes, themes (normal/hell/paradise/woods),
   soil layering, cave carving, ore veins, water/lava springs + edge
   flooding, beaches, trees; genuine noise stack (Perlin/Octaves/Distort);
   wired into Generate New Level with type/theme/size pickers in Indev mode.
2. **Armor system**: ItemArmor tiers, inventory armor slots (GUI spots
   exist; .mclevel slots 100-103 currently skipped), damage absorption,
   player armor overlay rendering (mob flags already exist).
3. **Bow + arrows as items**: ItemBow right-click fire consuming arrow
   items (262) in Indev mode, replacing the c0.30 arrow counter there.
4. **Paintings**: ItemPainting/EntityPainting/EnumArt - placement,
   knock-off rule, rendering, .mclevel entity round-trip.
5. **Flint & steel + BlockFire spread** (fire block + flame animation
   already exist from the burning-mob work).
6. **Polish batch**: wall torches, creeper swell white-flash overlay,
   equip-dip on item switch, exact first-person swing curves, double-chest
   verification, splash/fizz sound gaps, third-person held item decision.

## SESSION LOG - Pre-test fixes: lit furnace drop + inventory count shadow (latest)

### Growth stall: random ticks ran at 1/6.8 the genuine rate (user report)
The per-tick math (BlockCrops growth rate, farmland moisture) was already
genuine, but the DISPATCH was starved: genuine World.tick pays out
volume/200 random block updates per game tick (updateLCG accumulator +
the randId*3+1013904223 LCG for coordinates), while the engine's
Physics_TickRandomBlocks does 3 per 16^3 chunk = volume/1365. Crops that
genuinely take ~8 min/stage on dry soil took ~57 min, and ~50s farmland
hydration took ~6 min - "sat a while, nothing grew".
Fix: IndevTest_TickRandomBlocks ports the genuine loop exactly (volume/
200 with remainder carry, genuine LCG coordinate unpack, power-of-two
masks with a bounds skip for odd imports) and dispatches through the
same Physics.OnRandomTick table - so classic grass/sapling/flower ticks
also run at the genuine Indev rate. Physics_Tick branches on
IndevTest_Enabled; c0.30/creative keep the engine loop untouched.
Rig-verified with a farmland patch + water trench: soil went wet and
crops advanced a stage inside 90 seconds, matching genuine expectations.

### Bounds audit: no other block shares the farmland fate
Swept every Indev-layer block against genuine Block.java bounds:
- farmland 15/16 (fixed previous entry), crops 1x0.25x1 (fixed) - done.
- torch: genuine floor torch box is 0.4-0.6 x/z, 0-0.6 y; ours uses the
  engine's texture-derived sprite box which lands within a texel of
  that. Fine.
- slabs: engine classic half-block, matches genuine 44. Chest/workbench/
  furnaces/ores/etc: full cubes in genuine too. Flowers/mushrooms/
  saplings: engine texture-derived sprite boxes (long-standing classic
  behaviour, close to genuine's 0.3-0.7 boxes). Nothing else non-full.

### SOLVED: farmland was full-height, burying the crop planes' bottom row
The user called it: genuine BlockFarmland is setBlockBounds(0,0,0, 1,
15/16, 1) - the block sits 1/16 LOW, and the crop planes sink that same
1/16 to rest flush on it. Our farmland was a full cube, so the bottom
1/16 of every crop plane - the texture row holding stage 0's tiny
sprout dots - rendered INSIDE the farmland block. That's exactly why
freshly planted seeds showed ~5 centre marks instead of genuine's ~12
spread ones (only the taller centre cluster cleared the surface).
Fix: farmland (83/84) MaxBB.y = 15/16 (re-registered after define).
Bonus fidelity: farmland now renders recessed with the neighbour lip,
and walking on it sits you 1/16 lower, like genuine.
Rig-verified with a 3x3 stage-0 patch: dense even sprout grid across
every block, matching the genuine reference screenshot; a solid
red/yellow tracer tile also confirmed all four "#" planes render
full-span from every angle (the earlier "half-width single wall"
reads were corner-view foreshortening of the symmetric planes).

### Crop render deep-verification (round 4 - renderer confirmed correct)
Full instrumented investigation after the user still saw sparse stage-0
sprouts on the stamped build. Method: debug prints in AddSpriteVertices/
Builder_DrawSprite + a full vertex dump from Builder_DrawCrops, plus a
minimal all-stages test world (scratchpad gen_croptest3.py). Findings:
- Planted crops DO reach Builder_DrawCrops (count + draw both isCrop=1);
  the emitted vertices are exact: 4 full-span planes at +-0.25, both
  windings, u 0..1, y sunk 1/16, banks laid out to spec.
- The view-bank conditions split at the chunk CENTRE (drawXMin = camera
  west of centre etc, MapRenderer.c:707), not the chunk bounds - the
  complementary bank pairing still guarantees every plane renders from
  every camera position.
- MATURE stages (3-7) visually verified: dense parallel wheat rows
  spanning the block, matching genuine.
- Stage 0's texture (identical in indev and b1.7.3 terrain.png -
  compared pixel-level) is just 3-4 tiny 1-2px dots at the tile bottom;
  on the sunken (-1/16) planes those dots sit below the neighbouring
  grass block's lip, so edge dots hide and only the centre cluster
  reads - a faithful consequence of the genuine geometry, not a bug we
  could find. If genuine side-by-side at the SAME stage/surroundings
  still shows more, revisit with that exact A/B.
Debug instrumentation removed; tree matches the verified commit.

### Build stamp + crop outline actually-fix (user report round 3)
User still saw sparse sprouts on their Windows build after the bank fix.
Re-derived the whole chain against the engine: chunk faces render with
culling ON on both backends (DrawNormalFaces draws min+max together and
lets the GPU cull), so GL and D3D9 winding conventions ARE consistent,
and the crop quad winding matches Drawer_XMax's convention - the code is
correct on both. Prime suspect is a stale exe (the artifact was still
building when they tested). Two changes to close the loop:
- "Minecraft Indev (<compile date time>)" corner stamp so any build is
  self-identifying - no more guessing which artifact is running.
- The 0.25-tall crop outline was being STOMPED: the engine recalculates
  every DRAW_SPRITE block's bounds from texture alpha on each atlas
  change (Block_RecalculateAllSpriteBB). Crops are now skipped there, so
  BlockCrops' fixed 1 x 0.25 x 1 box survives.

### Crops render fix round 2: view-bank culling + short outline (user report)
The first "#" port scattered quads across the sprite banks arbitrarily -
but the sprite region renders with FACE CULLING ON and its four banks
are VIEW-DIRECTION groups (MapRenderer draws bank 0 when the camera is
past XMax or ZMin, bank 1 for XMin/ZMax, 2 for XMin/ZMin, 3 for
XMax/ZMax). From most angles the crop quads were in undrawn banks or
backface-culled, leaving a few floating sprouts. Each quad now sits in
a bank guaranteed drawn whenever it is front-facing (+X faces in bank
0/3, -X in 1/2, +Z in 1/3, -Z in 0/2) with the engine sprite winding
((v1-v0)x(v2-v1) = outward normal). Also BlockCrops.setBlockBounds(0,
0, 0, 1, 0.25, 1): the pick/outline box is now 4/16 tall, not a full
cube. Rig-verified visually this time (patched the crop tiles from
b173.jar into the rig pack, probe world with obsidian marker ring +
all-stage crop ring): dense wheat rows from every angle, short outline.

### Mobs staring at / shooting at feet + zombie arm flail (user report)
Three Java-position-convention and animation fixes, all Indev-gated:
- **Head pitch**: the pathless BasicAI fallback applied c0.30's per-type
  defaultLookAngle (zombie 30 degrees DOWN) - genuine Indev
  EntityLiving.updatePlayerActionState pins rotationPitch to 0, so Indev
  mobs now hold their heads level instead of staring at your feet.
- **Skeleton aim**: genuine aims at target.posY - 0.2 where Java posY is
  eye-anchored; we fed it CC's feet-anchored Position.y, so arrows dove
  at ankles. Now aims at the target's eye point - 0.2.
- **Zombie arm flail**: genuine melee re-arms its 20-tick swing timer
  every tick in range (reads as a held pose with its 20-tick swing
  model); our 5-tick c0.30 swing restarting every tick flailed. Melee is
  now gated on a 10-tick attackDelay - same landed-damage cadence as the
  victim's invuln half-window, one clean swing per attempt. (Documented
  deviation from the literal every-tick attackEntity, matching its
  effective behaviour instead.)
Lock-on jitter should be mostly the moveSpeed fix (previous entry);
point-blank orbiting/wandering underfoot is genuine EntityCreature
behaviour (adjacent paths finish instantly and fall back to the random
action state).

### Crops render as the genuine "#" row pattern (user report)
Crops were engine DRAW_SPRITE (a single diagonal X-cross clustered at the
block centre); genuine BlockCrops is render type 6 - four double-sided
planes at +-0.25 from centre (two spanning the full Z extent, two the
full X), sunk 1/16 into the farmland, so sprouts spread across the whole
tilled block. Ported as `Builder_DrawCrops` in Builder.c, emitted as two
of the engine's banked 4-quad sprite units so the sprite vertex layout/
counting stays intact (`AddSpriteVertices` counts crops as 8 quads, and
`Builder_DrawSprite` dispatches via the new `IndevTest_IsCropBlock`).
c0.30/creative sprites are untouched. Rig verified crash-free chunk
builds with a generated all-stages crop world (scratchpad
gen_croptest.py); the VISUAL check needs the user's textured build - the
rig's texture pack predates the crop tile patches so crops sample blank
atlas space there.

### Mob movement jitter / constant 180 flips (user report)
The Indev waypoint steering was fed c0.30's runSpeed table (zombie 1.0,
skeleton 0.3) instead of the genuine Indev EntityLiving.moveSpeed values
(0.7 default, zombie 0.5, spider 0.8 - EntityZombie/EntitySpider are the
only overrides). At double speed the 20Hz steering overshot the current
waypoint every tick and snapped yaw 180 degrees back, reading as jittery
"front/back" indecision. New `Mob_IndevMoveSpeed()` feeds both the
pathfollow and the pathless BasicAI fallback (Indev-gated - c0.30 keeps
its own runSpeed everywhere). Needs a feel pass by the user.

### Menu dim losing its top while holding an item (user report)
Symptom: pause-menu background dim only covered the lower ~60% of the
screen, triggered by holding any ITEM id (mining with a pickaxe, "Give
Hoe, Seeds"). Cause: `HeldItem_Render` (the extruded first-person item)
enables alpha test for its cutout mesh and never turned it back off -
the bare-arm and held-block branches both restore it, the item branch
didn't. With alpha test leaked into the 2D pass, every GUI pixel below
the 0.5 threshold is discarded; the dim gradient runs alpha 105 (top)
-> 162 (bottom) and crosses 127 about 40% down, which is exactly the
cutoff line seen. One-line fix: `Gfx_SetAlphaTest(false)` after
`HeldItem_Render`. Rig-verified holding an iron pick: extruded item
still renders, pause dim covers the whole screen again.

### Lit furnace mined -> idle furnace (user report)
`IndevTest_CanonicalBlock` mapped the lit-furnace directional variants back
to the lit BASE furnace, and the base lit furnace passed through unchanged -
so mining a burning furnace put a lit one in the inventory. Genuine
BlockFurnace.idDropped is Block.stoneOvenIdle for BOTH states; every lit
form (base + variants 79-82) now canonicalises to the idle furnace. Both
mining-drop call sites already route through CanonicalBlock, so this one
mapping fixes them all (Formats.c's chest scan is unaffected).

### Inventory stack counts get their drop shadow (user report)
The SurvivalInvScreen count atlas was built from the size-14 label font,
whose baked shadow is size/8 = 1px - effectively invisible. Now built like
the hotbar's counts: dedicated unpadded size-16 digit font (2px shadow,
exact glyph metrics for the right-aligned layout), freed after rasterising.
Rig-verified: counts in the inventory show the dark backdrop shadow.


## SESSION LOG - Phase 2 finish: per-mob attack AI, fire, mob sounds (latest)

### Per-mob attackEntity ports (all Indev-gated, c0.30 AI untouched)
`Mob_IndevCreatureUpdate` now runs the genuine `updatePlayerActionState`
order: resolve/drop target -> LOS raytrace -> `Mob_IndevAttackEntity` ->
if `hasAttacked` halt movement, else path/wander. Per-type attacks:
- **Creeper**: fuse state machine (EntityCreeper). Ignites within 3 blocks
  (7 once lit), `random.fuse` at 0.5 pitch on ignition, blows at 30 ticks
  via `Mob_IndevCreeperBlast` (radius-3 explosion + instant removal, NO
  gunpowder - drops only come from killing it first). `creeperState`
  idles at -1, winds down while not-attacking, re-armed to 1 each tick it
  attacks. Swell rendered in `SurvivalTest_RenderMobs` via `ModelScale`
  (RenderCreeper.preRenderCallback: s^4 fattens x/z up to 40%, y +10%,
  sin(s*100) shiver), interpolated by `fuseLast`->`fuseTicks`.
- **Skeleton**: bow fire < 10 blocks, 30-tick `attackDelay` cooldown,
  faces victim + stands still. `Mob_IndevShootArrow`: lob += horiz*0.2,
  speed 0.6, damage 4, `random.bow` at genuine pitch. (The old c0.30
  1/30 arrow roll + melee are now skipped in Indev mode.)
- **Spider**: darkness-only aggro (brightness < 0.5 to acquire, 1/100 to
  give up in light), 1/10 pounce from 2-6 blocks (grounded lunge), else
  shared melee.
- **Zombie/melee**: dist < 2.5 + vertical bbox overlap, flat strength
  (zombie 5, else EntityMob default 2), no attacker cooldown.
- **Sheep** (Indev) is a plain EntityAnimal wanderer (no c0.30 grass-eat);
  ANY living attacker shears **gray** cloth (block 35, 1+rand(3)) and the
  hit still lands (falls through to super), unlike the c0.30 white-wool
  player-only shear which is preserved on its own branch.

### Fire / daylight burning (corrects an earlier wrong assumption)
in-20100223 zombies AND skeletons **do** catch fire in daylight (this was
mis-noted before as "monsters don't burn until Alpha"). Ported from
Entity/EntityZombie/EntitySkeleton.onLivingUpdate: sky light > 7 + entity
brightness > 0.5 + open sky + `rand*30 < (bright-0.4)*2` roll -> `fire=300`;
fire deals 1 HP / 20 ticks; water extinguishes with a `random.fizz`; lava
sets `fire=600`. New `struct Mob.fire`.

### Flame visuals (follow-up commit, no longer a gap)
- **Fire texture**: TextureFlamesFX ported into Animations.c
  (`FireAnimation_Tick`, alongside the classic lava/water generators) - the
  genuine 16x20 heat buffer (bottom 4 rows are random fuel, each cell pulls
  18x the cell above + its 3x2 neighbourhood / (denom*1.06)) mapped through
  the genuine palette (r=155b+100, g=b^2, b=b^10, alpha cut below 0.5) into
  spare terrain tile 118 (`INDEV_FIRE_TEX_LOC`) every tick. Only runs when
  IndevTest_Enabled; skips HD packs (sim is fixed 16x16 like the original).
- **Billboard**: Render.java's burning pass as `SurvivalTest_RenderMobFires`
  (end of RenderMobs): per burning mob, ceil(height/width) stacked strips
  (cap 4), each 1.4 units tall / 10% narrower than the last, scaled by
  width*1.4, offset 0.4 toward the viewer (-0.04/layer), yaw-only camera
  facing (same convention as the item-drop sprites), full-bright white,
  batched into one dynamic VB + one atlas draw.
- Ignition moved ABOVE the AI dispatch (genuine onLivingUpdate order), so
  even a debug No-AI-frozen zombie catches sun - this was also what made
  it rig-testable.
- Rig-verified: frozen zombie at noon ignites within ~a second, flame
  column animates per-tick, burn damage hit-flash visible, .mclevel world
  load regression-checked in the same run.
Also fixed the light-aging threshold: was `light > 8`, genuine is entity
brightness > 0.5 which the `lightBrightnessTable` curve only reaches at
light 12+ (`Indev_LightBrightness` helper + `IndevTest_CurSkyLight`).

### Mob / entity sounds (new Audio mob soundboard)
- Resources.c: 14 genuine classic-era oggs added to the asset fetcher
  (mob_pig/pigdeath/sheep, mob_hurt/bow/fuse/drr/pop/explode/fizz), SHA1s
  verified live on mojang's asset host, +154 KB to the download.
- Audio.c/h: `MobSoundType` enum + `mobSnd_groups` board loaded from
  `mob_*` zip entries (`MobSounds_Load`), and `Audio_PlayMobSound(type,
  vol, pitch, dist)` mirroring World.playSoundAtEntity's 16-block cutoff
  (16*vol when louder) with linear distance falloff (non-positional engine).
- SurvivalTest.c funnels every entity sound through `Indev_PlaySoundAt`
  (no-op in c0.30 mode): hurt/death (pig/pigdeath, sheep, else random.hurt
  - monsters were still voiceless in Indev), pig/sheep ambient livingSound
  roll (rand(1000) < livingSoundTime++), player random.hurt, skeleton
  random.bow, creeper random.fuse, arrow random.drr on block hit,
  random.pop on item + arrow pickup, random.explode on any blast,
  random.fizz on extinguish. Existing installs: delete audio/default*.zip
  once to refetch with the mob sounds.

### Rig-verified
Build clean. Headless run at noon: 47 mobs pathing at 80-90 fps, skeleton
arrows seen in flight, invincibility + kill-all + spawn all work, no crash,
clean log across every new attack/fire/sound path. (Headless has no audio
device so sounds are silent - Audio_SoundsVolume gates them off safely.)
Creeper swell + burning visuals best confirmed on the user's audio build.


## SESSION LOG - Entity rewrite phase 2: Indev A* pathfinding (latest)

### Pathfinder.java port (level/path/, in-20100223)
- `PF_*` block in SurvivalTest.c: A* over walkable columns, binary heap +
  open-point hash (900 node cap like the genuine 4096-region working sets,
  scaled to our 64-waypoint paths), 4 horizontal neighbours per node with
  step-up (+1) and drop-down (up to 3, matching getSafePoint's 4-deep scan)
  handling, nodes capped at 16 blocks from the target, best-effort partial
  path (closest-to-target node) when the goal is unreachable.
- Faithfully preserves the genuine passability BUG: getVerticalOffset's
  box scan reads the LOOP START coordinates, so only the corner block is
  tested regardless of entity size (decompile-visible in Pathfinder.java).
  Mobs therefore path like 1x1 entities - spiders squeeze, exactly like
  genuine Indev.
- Vertical offset classes: solid -> 0 (blocked), liquid -> -1 (rejected
  below feet: mobs won't path onto water), else 1 (walkable air).

### EntityCreature.updatePlayerActionState port
- `Mob_IndevCreatureUpdate` drives every non-sheep mob in Indev mode:
  target acquire when the player is < 16 blocks (distSq < 256), re-path
  every rand(20) ticks toward the target, otherwise a 200-point weighted
  wander scan (monsters weight by 0.5 - brightness, so they wander toward
  dark spots) picking a random nearby ground column.
- Waypoint follow: advance when within width*2 of the waypoint, yaw =
  atan2(-dz, dx) toward it, moveForward = runSpeed, jump when the next
  waypoint is higher (or randomly when in liquid, 0.8 chance).
- No path / path exhausted -> falls back to Mob_BasicAIUpdate (the c0.30
  wander), matching EntityLiving's default action state.
- Path storage lives on struct Mob (64 waypoints, pathIndex cursor);
  c0.30 mode is untouched - dispatch is `IndevTest_Enabled` gated, sheep
  keep Mob_SheepUpdate everywhere.

### Rig-verified
Spawned zombies via F9 acquire the player, face them and close in over
terrain/water; several minutes without crash, clean log. Full behaviour
testing (step-up paths, dark-seeking wander, creeper approach+swell)
still on the user checklist.

### Phase 2 remainder (backlog)
- Creeper swell timing vs ours (verify against EntityCreeper).
- Body/head yaw separation on the models (renderer refinement).


## SESSION LOG - GUI item handling fixes + workbench notes (latest)

### Fixed (user report on the crafting GUI)
- Cursor-held stack showed NO count - now drawn (right-aligned at the mouse,
  blocks and items alike) via the count mesh (PointerMove already dirties it).
- Items/blocks were edge-to-edge in slots ("too big") - iso blocks shrank
  texF*8 -> texF*7, item sprites texF*16 -> texF*14 with a ~1px inset, cursor
  sprite now matches slot items (was slotSize*0.75).

### Workbench right-click - logic verified correct; likely-caused-by items
The TryUseBlock hook is correct end to end: block 66 is defined/placeable/
picked as solid, TryUseBlock runs before placement, opens the 3x3. The
broken item GUI (above) made crafting+placing a workbench nearly impossible,
so "doesn't open" was most likely "never had one placed". Re-test with the
fixed GUI.
IMPORTANT interaction: IndevTest_Enabled is now
`OPT_INDEV_MODE && !OPT_SURVIVAL_MODE`. A stale options.txt with BOTH set now
resolves to Survival Test (no Indev features). If Indev "stops working",
re-pick "Indev (WIP)" from the launcher (it clears survival-mode).
Also: delete default.zip once so crafting.png is fetched, else the 3x3 opens
with the flat fallback panel.

### Paperdoll: still the deferred D3D issue
The 2x2 doll box is set correctly; the blank/garbled doll on the user's build
is the known Direct3D 3D-in-2D-pass problem (renders on GL, not D3D). Real
fix = render the doll in the 3D frame, still deferred.


## SESSION LOG - Workbench 3x3 crafting + mode-exclusivity fix (latest)

### Mode leak fix (user: string/feathers in survival test)
Indev mob/block drops are gated by IndevTest_Enabled, but if BOTH
indev-mode AND survival-mode options were set at once, Indev won and its
drops appeared in "survival test". IndevTest_Enabled is now
`OPT_INDEV_MODE && !OPT_SURVIVAL_MODE` - Survival Test wins any both-set
edge, so no Indev behaviour leaks into c0.30. (Launcher already keeps
them exclusive; this is the runtime backstop.)

### Workbench 3x3 crafting grid
- Craft grid expanded to 9 slots with a runtime CraftDim (2 pocket / 3
  workbench). CraftResult/Take/ReturnAll are dim-aware; the recipe matcher
  already slides patterns in an arbitrary gw x gh grid, so 3-wide recipes
  (tools, chest, furnace, bread, TNT) only match in the 3x3 - genuine.
- gui/crafting.png extracted from the b1.7.3 jar (like inventory.png) via
  BetaPatcher; IndevTest_CraftGuiTex() exposes it. The screen renders it
  as the panel for the workbench (176x166, 3x3 grid at 30,17, result 124,35,
  no paperdoll window) and inventory.png for the pocket - genuine
  GuiCrafting/GuiInventory coords.
- Screen made dim-aware throughout: CraftXY (i%dim), HitSlot, Display
  count/slot iterate CraftCells() = dim*dim; layout picks coords by dim;
  the doll is skipped for the workbench.
- Right-clicking a placed workbench (SurvivalTest_TryUseBlock, wired into
  InputHandler_PlaceBlock) sets dim 3 and opens the screen; closing resets
  to pocket 2x2. E-inventory stays the 2x2 pocket.
- Existing installs: delete default.zip once to fetch crafting.png.


## SESSION LOG - Indev inventory GUI now uses the genuine texture (latest)
### Paperdoll black-box fix RESOLVED on OpenGL by the default-skin fallback (the local player's
TextureId is 0 in singleplayer with no ClassiCube skin; the doll now forces
the model's char.png defaultTex). Remaining GL tweaks: pitch tracking was
inverted (cursor down tilted the head up) - fixed the sign + use the tall
box's vertical centre; and the model was ~96% of the box height (too big) -
DOLL_DIST 3.4 -> 4.7 for margin. STILL BLANK ON DIRECT3D: rendering a 3D
model mid-2D-UI-pass behaves differently on D3D (depth-stencil state objects,
no depth buffer bound). Proper fix = render the doll during the 3D frame,
not the 2D overlay - deferred as its own task.
The doll rendered as just the texture's black window - Model_Render relies on
backface culling but the panel/item-sprite draws before it left culling OFF,
so every model face was culled. Now sets Gfx_SetFaceCulling(true) around the
doll's Model_Render (restored after). Block-in-GUI sizing/centering pending
user recheck on this build.

The flat grey survival panel is replaced, in Indev mode, by the real
gui/inventory.png from the b1.7.3 jar - so the screen matches Indev's look.
- BetaPatcher (Resources.c) now also extracts gui/inventory.png into
  default.zip; IndevTest exposes it via IndevTest_InvGuiTex() + the
  inventory.png TextureEntry (texpack-overridable).
- Layout: Indev branch uses GuiInventory's exact 176x166 geometry - the
  whole panel is one textured quad (0,0)-(176,166); slots on the genuine
  18px grid at craft (88,26), result (144,36), storage (8,84), hotbar
  (8,142). texF = slotSize/18 scales it all. Classic/Enhanced c0.30 keeps
  the old flat panel unchanged (else branch).
- Paperdoll renders in the genuine window: a TALL region (x 26..74, ~68
  high, not square) so legs aren't clipped - added dollBoxH + viewport
  aspect = boxW/boxH so the model isn't stretched. Feet land at the
  window's genuine spot.
- Items/blocks now centre in the genuine 16px item area (slotX = item
  origin), block iso halfSize = 8*texF, sprites = 16*texF at slotX,
  count digits at the 16px item bottom. Mouse-over draws GuiContainer's
  translucent-white 16px highlight. Flat panel/dollbox/title/arrow all
  suppressed for Indev (the texture supplies them).
- Existing installs: delete default.zip once to re-extract inventory.png
  (added as a required entry, so a fresh generate picks it up too).


Branch: `survival-test` (renamed from `claude/c030-s-gamemode-8fpmns`)

This file is the living context/handoff for the c0.30-s survival gamemode recreation.
Goal: a **faithful from-scratch recreation** of Minecraft Classic Survival Test (0.30),
cross-referenced against the decompiled source tree at `/tmp/good2000mo_oc/`
(primary ground truth) and the Minecraft Wiki, that does **not** disturb creative mode.

---

## SESSION LOG - Indev entity/spawn rewrite (phase 1) + third-person finding

### Third-person held items (user question) - SOURCE VERDICT
in-20100223 does NOT render held items or blocks on the third-person
player model AT ALL: RenderLiving has zero item code, RenderPlayer's
render passes are armor-only (equipped-item rendering arrived in Alpha).
So F5 with empty-looking hands is FAITHFUL. Engine quirk: ClassiCube
natively shows held BLOCKS in third person (kept - a nicety beyond
genuine); items show nothing (faithful). User decides after testing:
add items for consistency, or suppress blocks for strict parity.

### Indev mob spawning (MobSpawner.performSpawning port, Indev-only)
- Runs EVERY tick (replaces the classic c0.30 spawn gate in Indev mode).
- Monster pass: cap = volume*20/64^3 / 2 (difficulty Normal), 4 attempts,
  type = nextInt(5) with index 4 spawning NOTHING (genuine quirk: 0 skel,
  1 creeper, 2 spider, 3 zombie), Y biased to the depths (min of two
  uniforms), 2 clusters x 3 jitter steps (+-6 xz, y jitter is genuinely
  always 0), needs solid-below + 2 air, no liquid, >= 32 blocks from the
  player, and the DARKNESS rule: light <= nextInt(8).
- Animal pass: pigs/sheep, cap = width*length/4000, light > 8.
- Light source for both rules = IndevTest_LightLevel (sky-lit day/night
  level vs fancy-lighting block light) - so torches near a dark cave
  mouth genuinely suppress monster spawns.
- Initial-population flood REMOVED in Indev mode (genuine prepareLevel
  doesn't exist there; the spawner fills gradually - this also fixes the
  spawn-camping deaths seen in rig testing).
- Monsters in bright light (level > 8) age +2 per tick toward the
  despawn roll (EntityMob.onLivingUpdate) - in-20100223 monsters DO NOT
  burn in sunlight (Alpha behaviour); they just despawn faster by day.

### Entity rewrite phase 2 (NEXT): EntityCreature pathfinding (the
level/path Pathfinder + path-following movement), spider day-neutrality,
creeper swell timing vs ours, mob body/head yaw separation.

---

## SESSION LOG - .mclevel entity parity + Indev block hardness overrides

### .mclevel format parity (the remaining Entities-list gap)
- SAVE now writes the FULL genuine Entities list: LocalPlayer + every live
  mob (Zombie/Skeleton/Pig/Creeper/Spider/Sheep with Pos/Rotation/Health,
  the genuine writeToNBT field set) + every physical item drop ("Item"
  entities with the ItemStack payload, block ids remapped to Indev space).
- LOAD restores them: mobs respawn via SurvivalTest_RestoreMob with saved
  health (genuine default 10 when absent), item drops via SpawnDropWorld
  (ids remapped back). Arrows/PrimedTnt/Paintings still skipped - genuine
  Indev also skips Arrow/PrimedTnt on load ("Skipping unknown entity id"),
  so only Painting remains a real gap (no painting entity system yet).
- SkyBrightness was SAVED as a constant 15 - now round-trips the real
  value (paradise maps stay paradise).
- VERIFIED in the rig: saved a mob-heavy world -> 58 entities parsed out
  of the file with correct id strings; loaded back with inventory, score,
  drops and mobs restored, no crash.
- Remaining known deltas: armor slots 100-103 (needs the armor system),
  Data light nibble written as full-light (genuine relights anyway),
  drop-entity Damage always 0 (our drops don't carry tool wear).

### Indev block hardness overrides (classic untouched)
Indev rebalanced classic blocks' hardness; applied via SetHardness inside
IndevBlocks_Define - which ONLY runs in Indev mode, so c0.30 keeps its
faithful table. Changes vs c0.30 (x20 tick units): stone 20->30, cobble/
planks 30->40, brick 0->40 (classic's brick was a genuine 0-hardness
quirk!), mossy 20->40, slabs 20->40, dirt/sand 12->10, log 50->40,
flowing lava 2000->0 (genuine Indev quirk: setHardness(0)), wool 16,
bookshelf 30, obsidian 200, ores 60, gold block 60, iron block 100.

---

## SESSION LOG - first-person extruded held item (ItemRenderer port)

The camera-anchored billboard held-item hack is GONE. Items (tools/food/
materials) and sprite-type blocks (flowers/saplings/torches) in hand now
render as ItemRenderer.renderItemInFirstPerson's extruded sprite: front +
back quads plus 16 strip quads per edge (66 quads), baked with the genuine
local chain translate(-15/16,-1/16,0) -> rotZ 335 -> rotY 50 -> scale 1.5
-> translate(0,-0.3,0), textured from items.png (items) or the terrain
tile (sprite blocks), mirrored like genuine (x=0 samples u2).

Integration: rendered INSIDE HeldBlockRenderer's model path, replacing the
old skip-arm branch - so it inherits the engine's held-entity transform
(scale 0.4, rotY -45), its projection/view (70 FOV, tilt, bob), and ALL
its animations (click/dig swing, equip dip on block change). The hand
anchor uses ItemRenderer's (0.56, -0.52, -0.72). Normal BLOCKS in hand
keep the engine's held block (classic pose = Indev's inherited look).
Rotation signs verified by rig iteration (first attempt was mirrored -
ClassiCube Matrix_Rotate* handedness vs GL glRotatef).

Known deltas (documented, revisit on user feedback): the equip-change dip
only triggers on BLOCK id changes (engine tracks Inventory_SelectedBlock;
item ids all map to AIR there); genuine equip curve is +-0.4/tick toward
target with item swap below 0.1 - engine's dip differs slightly; exact
swing curves are the engine's classic ones, not ItemRenderer's sqrt-sin
trio. Pose verified in the rig with the iron pickaxe (blade centre-left,
handle to the bottom-right corner, visible 1/16 extrusion depth).

---

## SESSION LOG - per-block light query (torch-lit night farms)

The crop-growth light approximation is gone. FancyLighting already caches a
per-block byte (lamp nibble | lava nibble) per chunk - the new
FancyLighting_BlockLightLevel(x,y,z) just exposes max(lamp, lava) from that
cache (CalcForChunkIfNeeded + two masks; ~20 lines). Indev_GrowLightOk now
takes max(sky-lit ? day/night sky level : 0, block light) >= 9 - matching
World.getBlockLightValue, so TORCH-LIT FARMS GROW AT NIGHT like genuine.
Falls back to the sky-only check if the user forces classic lighting mode.

---

## SESSION LOG - farming + growth pipeline (in-20100223 ports)

New blocks: farmland dry 83 / wet 84 (tiles 116/115, dirt sides, not
placeable) and crop stages 85-92 (X-sprites of tiles 107-114, instant
break, planted only via seeds).

Mechanics (exact ports, hooked into the ENGINE's random-tick system via
Physics.OnRandomTick - so BlockPhysics' volume-scaled ticking drives them,
and its existing sapling handler already grows trees):
- ItemHoe.onItemUse: grass (non-solid above) / dirt -> dry farmland, tool
  wear 1, 1-in-8 seed drop from hoed GRASS. Wired into TryUseBlock after
  the container checks (genuine order: blockActivated then item use).
- ItemSeeds.onItemUse: plants stage-0 crops on farmland, consumes 1.
- BlockFarmland.updateTick (1-in-5 gate): water within x/z +-4 at y/y+1
  hydrates; else wet dries to dry; dry with no crops above reverts to
  dirt; solid cover reverts (genuine does that on neighbour change).
  Moisture is binary wet/dry (genuine has 0-7; only >0 matters for crops
  rate 3.0 and the top texture).
- BlockCrops.updateTick: light >= 9 above (approximated: column sky-lit
  AND sky light >= 9 - torch-grown night farms need a per-block light
  query, noted), growth rate 1 + farmland below (1 dry/3 wet, neighbours
  quarter-weighted), halved when crowded (diag or both-axis row crops),
  then 1-in-(100/rate) advances the stage.
- Breaking crops: wheat at stage 7 only + up to 3 seed rolls weighted by
  stage (nextInt(15) <= stage). Farmland drops dirt. Crops pop off when
  their farmland vanishes.
- Saplings: the engine's classic Physics sapling handler (TreeGen, light
  gated) covers tree growth; genuine Indev's staged metadata counter is
  approximated by it (rate differs slightly - acceptable, noted).
- .mclevel: farmland <-> 60 (moisture nibble), crops <-> 59 (stage
  nibble) via new generic IndevTest_BlockDataMeta/ApplyDataMeta - REAL
  Indev farms round-trip with stages and moisture intact.
- Debug page 2: "Give Hoe, Seeds" button (replaced Give Bread - bread is
  craftable from wheat anyway).

NEEDS LIVE RETEST (rig spawn was a sand pit; code paths compile + mirror
verified infrastructure): hoe tilling + seed drop, planting, growth over
a day, wheat/seed harvest at stage 7, farmland hydration visual, and an
mclevel round-trip of a farm to genuine Indev.

---

## SESSION LOG - sun, moon and stars (renderSky port)

User asked whether in-20100223 had night stars + the sun texture: YES -
RenderGlobal.renderSky draws /terrain/sun.png and /terrain/moon.png quads
plus a 500-star field; World.getStarBrightness drives star alpha. Ported:
- Sun: +-30 quad at y=+100; moon: +-20 quad at y=-100 with flipped UVs;
  both rotate around X by celestialAngle*360, centred on the eye (view
  translation stripped), additive blending, fog off, no depth writes.
- Stars: 500 quads baked once from java-Random(10842) - ClassiCube's RNG
  is java.util.Random-compatible so sizes/angles match genuine; the
  genuine display list never resets its matrix so rotations accumulate
  star-to-star (composition handedness may mirror the field - visually
  indistinguishable for a random sky). Colour = getStarBrightness
  (clamp01(1 - (cos*2 + 12/16))^2 * 0.5), drawn only when > 0.
- Hooked in Game.c between EnvRenderer_RenderSky and RenderClouds (the
  genuine draw order). Dynamic VBs freed on context loss.
- sun.png/moon.png extracted from the beta jar (terrain/*.png) as required
  default.zip entries - which ALSO forces existing installs to regenerate
  default.zip automatically, delivering the torch-top tile 117 without the
  manual delete the previous entry asked for.
- VERIFIED in the rig: star field + moon overhead at midnight, bright sun
  overhead at noon (and night->noon colour recovery via the time switcher).

---

## SESSION LOG - paged debug menu + time switcher, torch top texture fix

### Debug menu refactor (user: nestle options under pages)
F9 menu is now TWO pages sharing one 18-button grid - the flip button
relabels the widgets and re-hooks their MenuClick handlers in place (no
widget rebuild). Page persists across open/close.
- Page 1 "Mobs + Combat": the original spawn/heal/toggles/census set.
- Page 2 "Items + Time": give shortcuts (iron pick/axe/sword, workbench,
  chest, furnace, coal x10, iron ore x10, logs x10, torches x8, planks x32,
  string x8, bread x5, arrows x8) and the TIME SWITCHER - Dawn 21600 /
  Noon 3600 / Dusk 9600 / Midnight 15600 (celestial angle = t/24000 - 0.15,
  so noon sits at t=3600). All verified in the rig: page flip, give
  handlers, and Time: Midnight -> instant black-sky night on a freshly
  generated world (confirming the day/night env path works for generated
  worlds too, not just .mclevel loads).

### Torch top texture (user report, verified against source)
renderBlockTorch samples the top face at tile pixels x 7-9, y 6-8 (the
ember), but the engine's bounds-crop reads y 7-9 - one pixel low, smearing
flame+stick. Fix: BetaPatcher now writes tile 117 = the torch tile shifted
DOWN 1px (PatchTerrainTileShifted), and the torch's FACE_YMAX uses 117, so
the crop lands exactly on the genuine ember pixels.
NOTE: existing installs need default.zip deleted once to regenerate the
atlas with tile 117 (the required-entries check can't detect tile-level
changes).

---

## SESSION LOG — 2x2 crafting GUI (latest)

The crafting loop is now playable end to end in Indev mode.

### Model (SurvivalTest.c)
- 2x2 grid `st_craft[4]`, addressed as extended slots 36..39. `SwapSlots`
  gained `SlotPtr` so it moves stacks between inventory and grid uniformly.
- `SurvivalTest_CraftResult(&count)` builds the 2x2 full-id grid and queries
  `IndevTest_MatchRecipe`. `CraftTake` yields the output + consumes one of
  each ingredient (SlotCrafting.onPickupFromSlot). `CraftReturnAll` refunds
  the grid to the inventory on close (never lose materials).

### GUI (Screens.c, SurvivalInvScreen - the survival inventory screen)
- Extended the existing paperdoll inventory: the top row now shows the 2x2
  grid + an arrow + the result slot beside the doll; panel widens and the
  storage grid recentres. **All of this is gated on IndevTest_Enabled** -
  plain c0.30-s renders the byte-identical old screen (DisplayCount returns
  storage-only, HitSlot ignores the craft area, no arrow, panel not widened).
- **Item sprites in slots**: this screen only ever drew ISO block pictures;
  item ids (tools/food/materials) were invisible in the inventory. Added an
  immediate `Texture_Render` pass from items.png for item-id slots (storage,
  grid, and result), so items finally show in the inventory too - not just
  crafting. Blocks still use the ISO batch; counts overlay both.
- Click model reuses the swap-based `heldSlot`: click to pick up (block OR
  item now - the pickup gate was block-only before), click again to place/
  swap; clicking the result slot crafts (CraftTake). Refund on E/Esc/outside/
  Free.

### Known simplifications (documented, not blocking)
- No item-follows-cursor drag; the held slot just highlights yellow (existing
  screen behaviour). 2x2 only - the 3x3 workbench needs the workbench block,
  which needs the block-additions phase. Result recomputes per render (cheap).

### NEXT: block additions (workbench, crate/chest, furnace, torch, painting)
unlock the deferred recipes + the 3x3 grid; then day/night + lighting.

---

## SESSION LOG — day/night cycle, torch model + real torch light

### Day/night (exact World.java ports, verified at night in the rig)
- `worldTime` ticks at 20Hz in IndevTest_Tick, wraps at 24000 (20 min/day).
- `getCelestialAngle` = (worldTime/24000) - 0.15; "paradise" maps
  (SkyBrightness > 15) pin it to 0 = always noon.
- Sky colour: base * clamp01(cos(a*2PI)*2 + 0.5); fog floors
  (f*0.94+0.06, f*0.94+0.06, f*0.91+0.09); clouds (0.9+0.1 / 0.85+0.15).
  Base colours snapshotted at OnNewMapLoaded (after .mclevel env applied) -
  the SAVER writes the base colours, not the live scaled ones, else saving
  at night would bake a black sky into the file.
- `getSkyBrightness`: clamp01(cos*1.5 + 0.5) -> light level 15 (noon) to 4
  (night). Sun/shadow env colours scaled by light/15 - only applied when
  the level steps (sun colour changes trigger a world relight; 11 steps
  per dawn/dusk transition is cheap).
- .mclevel TimeOfDay + SkyBrightness now round-trip (load was TODO before;
  save previously wrote 0). VERIFIED: hand-patched a save to TimeOfDay
  15500, loaded -> black sky, dim level-4 terrain, exact night look.

### Torch fixes (user: "not a true torch model - it's the flower render")
- Was DRAW_SPRITE (flower X-cross). Now a genuine thin column: MinBB/MaxBB
  (7/16, 0, 7/16)-(9/16, 10/16, 9/16), DRAW_TRANSPARENT - the engine crops
  the tile UVs to the bounds per face, matching BlockTorch's stick look
  (minus the wall-mount tilt; torches are floor-standing only for now).
- Torches now EMIT light: Brightness = 14 << FANCY_LIGHTING_LAMP_SHIFT
  (white lamp light, Indev setLightValue(14/16)), and Indev mode switches
  to LIGHTING_MODE_FANCY at map load (unless a server locked the mode) so
  the light actually propagates. Lit furnaces (all facings) also emit 14.
- NEEDS LIVE RETEST: torch column look + cast light in a dark area.

---

## SESSION LOG — GUI parity with genuine Indev (side-by-side screenshots)

User compared our chest GUI against real Indev's side by side. Fixed:
- **Placement facing was 180 degrees flipped** (fronts faced away) -
  ClassiCube yaw != Beta yaw convention; metadata quadrant picks swapped
  (2/5/3/4 -> 3/4/2/5). Verified direction still needs one more live test.
- **Foreground labels added**: genuine GuiContainer draws dark-gray
  (0x404040 = &8) unshadowed text - "Chest"(8,6)/"Furnace"(60,6)/
  "Crafting"(28,6) + "Inventory"(8,72/74); pocket GuiInventory has
  "Crafting"(86,16). Rendered as prebuilt text textures at texF-scaled
  genuine coords. Verified in the rig (pocket Crafting label correct).
- **Stack counts hung off the slot's right edge**: the width estimate used
  TextAtlas.offset - which is the PREFIX width (0 for the digits atlas!),
  not a per-digit advance - so textW was always 0. Now sums the atlas'
  per-glyph widths[] and right-aligns at x+17 (renderItemOverlayIntoGUI's
  x + 19 - 2 - stringWidth). Cursor count fixed the same way.
- **Durability bars in every slot**: genuine draws the damage bar in ALL
  GUI slots (chest/storage/hotbar rows), ours only did the HUD hotbar.
  Same formulas as the HUD implementation, scaled by texF; container slot
  damage read from the tile entity, craft-grid cells skipped.

---

## SESSION LOG — real-Indev interop fixes + directional chests/furnaces

User LIVE-TESTED our .mclevel files in genuine Indev: terrain loads, tools
keep durability - but clicking chests/crafting tables crashed Indev, and
reloading in ClassiCube showed containers as undefined "green blocks".
All root-caused and fixed, plus the review-confirmed explosion bug:

1. **Indev crash on chest click**: tile entities are created lazily on first
   open, so never-opened chests saved as a chest BLOCK with no TileEntity -
   genuine BlockChest.blockActivated casts getBlockTileEntity() and NPEs.
   Fix: MCLevel_Save scans the world and emits an empty TileEntity entry for
   every container block not in the pool.
2. **Indev crash on crafting table click** (and chest GUI): item stacks were
   saved with OUR raw block ids - a workbench (66) or chest (67) in the
   inventory is a null entry in Indev's item table, crashing GUI rendering
   the moment any container screen draws the player inventory. Fix: item ids
   < 256 are remapped to the Indev id space on save and back on load
   (MCLevel_WriteItem / MCLevel_CommitItem).
3. **"Green blocks" after ClassiCube reload**: Map_LoadFrom runs Game_Reset,
   which wipes ALL custom block definitions; ids 66+ survive in the map but
   render undefined (the ids still FUNCTIONED - container logic is id-based).
   Fix: IndevTest registers OnNewMapLoaded -> IndevBlocks_Define() again.
4. **Explosions bypassed the TE lifecycle** (adversarial review, CONFIRMED):
   SurvivalTest_Explode removes blocks via bare Game_UpdateBlock (no
   BlockChanged event), so TNT/creeper-blasted chests lost contents with no
   scatter and leaked their TE (contents resurrected by placing a chest at
   the crater coords; wrong-kind GUI possible). Fix: the removal lifecycle
   is now public (IndevTest_NotifyBlockRemoved) and the explosion path calls
   it explicitly. (Review's other critical - map-change TE leak - was
   already fixed in 65e1ff7; furnace blocks are explosion-immune SOUND_STONE
   so the furnace-conjuring scenario was refuted.)

### Directional chests + furnaces (user request)
Block ids 71-82: chest/furnace-idle/furnace-lit x 4 facings, front texture
on the face matching Indev metadata 2/3/4/5 (-Z/+Z/-X/+X). Canonical 66-70
remain the inventory/recipe/drop form. Placing a canonical chest/furnace
rotates it to face the player (Beta onBlockPlacedBy yaw-quadrant formula -
NEEDS LIVE RETEST, the yaw convention may be 180 degrees off). The furnace
lit/unlit tick swap preserves facing. Drops canonicalise (variants drop the
canonical block, both mining and explosion paths). .mclevel round-trips
facing through the Data array metadata nibble both ways (torches write
meta 5 = standing); genuine Indev now sees properly-faced furnaces/chests.

---

## SESSION LOG — world persistence via genuine Indev .mclevel format

User request: save inventory + chest/furnace contents with worlds, using the
"backwards approach" - the genuine Indev format itself, rather than bolting
survival data onto .cw. Schema ported exactly from the in-20100223 decompile
(LevelLoader.java, EntityPlayerSP.writeEntityToNBT, ItemStack.writeToNBT,
TileEntityChest/Furnace NBT).

### What was added
- **MCLevel_Save** (Formats.c): full gzipped "MinecraftLevel" NBT writer -
  About / Environment / Map (Width/Length/Height, Blocks remapped to genuine
  Indev block ids, Data array = full-light nibbles, Spawn) / Entities
  [LocalPlayer: Pos(+1.62 eye)/Motion/Rotation floats, Health, Score,
  Inventory list of {Slot, id, Count, Damage}] / TileEntities [{Pos packed
  x+(y<<10)+(z<<20), id "Chest"/"Furnace", Items, BurnTime, CookTime}].
- **MCLevel_Load extensions**: the existing blocks/spawn/env loader now also
  parses Entities (LocalPlayer -> inventory slots 0-35, health, score, exact
  saved position/rotation; armor slots 100-103 skipped - no armor system yet)
  and TileEntities (chests/furnaces recreated with contents; currentBurn
  recomputed from the fuel slot like readFromNBT). Accumulate-and-commit
  parser - the NBT walker fires compound callbacks AFTER children, and field
  order is never assumed (genuine files order by Java HashMap).
- **Block id mapping** (IndevTest.c): bidirectional 50+ tables. Exact pairs:
  torch 50<->70, chest 54<->67, workbench 58<->66, furnace 61/62<->68/69,
  fire 51<->CPE fire 54. Lossy: crops->air, farmland->dirt, gear->air,
  diamond ore->coal ore, diamond block->iron block; our CPE 50-65 -> nearest
  Indev equivalent (slab->stairSingle, ice->glass, etc). Load remap only
  applies when Indev mode is on (blocks 66-70 undefined otherwise).
- **Menus.c**: save screen writes maps/<name>.mclevel when the Indev gamemode
  is active (else .cw as before); .mclevel added to the save-file dialog.
  Loading needed no wiring - the .mclevel importer was already registered.

### Compatibility notes (user question: genuine Indev interop)
- Genuine Indev saves .mclevel (NOT .dat - that's Classic's serialized-Java
  format, which ClassiCube already imports terrain-only via Dat_Load).
- Real Indev world -> us: terrain (remapped), spawn, env colors, player
  position/health/score/inventory, chest+furnace contents all restore. Mobs/
  dropped items/paintings in Entities are skipped (mobs respawn); Data
  light/metadata discarded (furnace facing lost - we don't track facing).
- Us -> real Indev: full genuine schema written; item ids already mirror
  shiftedIndex 1:1. UNTESTED in a real Indev client so far; creative-mode
  saves write an empty Entities list (no player) - unknown how genuine Indev
  reacts to that; Indev-mode saves always include the player.
- KNOWN GAP CLOSED: chest/furnace contents + inventory now survive save/load.

### Status
Compiles + links clean. Live headless round-trip verification pending (was
interrupted); retest checklist: save Indev world with items + placed chest/
furnace, reload, verify inventory/containers/furnace-burn restore, and blocks
66-70 survive the id round-trip.

---

## SESSION LOG — chests + furnaces (tile entities), instant-break fix, recipe fixes

Big feature drop, all mechanics ported from the in-20100223 decompile and
live-verified in the headless rig (screenshots of every stage).

### Instant-break root cause (user report: new blocks broke instantly)
`SurvivalTest_SetHardness` wrote into `st_hardness[]` WITHOUT seeding, and
the table's lazy first-read seed (inside `SurvivalTest_Hardness`) rewrote ALL
entries from the c0.30 defaults - whose `default: return 0` covers ids 66-70.
Sequence: IndevTest OnInit sets 66-70 hardness -> first left-click triggers
`CanInstaBreak` -> seed loop clobbers them to 0 -> instant break. Fixed by
seed-before-override (`SurvivalTest_SeedHardness()` called from both paths).
**Identical latent bug found & fixed in `st_maxStack`** - IndevItems_Seed's
64-max/tools-stack-1 values were clobbered back to 99 on first read.
Hardness values also corrected to the decompile: workbench/chest 2.5F -> 50
ticks (was 30), furnace 3.5F -> 70 (already right), torch 0.

### Tile entities (IndevTest.c)
Flat pool (`INDEV_TE_MAX` 192) keyed by block position; created lazily on
first open (covers pre-existing/loaded blocks), destroyed via a BlockChanged
hook. Slots use the shared `struct SurvivalSlot` (moved to SurvivalTest.h).
- **Chest break**: contents scatter as drops - faithful BlockChest.onBlockRemoval
  port (0.1-0.9 offset per slot, stacks split into 10-30 chunks).
- **Furnace break**: contents silently destroyed - faithful (BlockContainer
  only removes the TE; Indev really did eat your ore).
- **Lit<->unlit furnace swap keeps the TE** (hook ignores furnace->furnace).
- KNOWN GAP: TE contents are NOT saved with the map (needs sidecar format).

### Furnace (TileEntityFurnace port, verified live: 5 ore -> 5 ingots)
- Slots 0 input / 1 fuel / 2 output. Smelts iron ore->iron ingot, gold
  ore->gold ingot, sand->glass, cobblestone->stone, raw->cooked porkchop
  (diamond ore pair omitted - no diamond ore block in the classic set).
- Fuel: coal 1600 ticks, stick 100, wood-material blocks 300 (approximated
  by SOUND_WOOD dig sound, torch excluded - its material is circuits).
- 200 ticks/item; fuel consumed only when burnt out AND smeltable; ALL
  partial progress lost when it can't smelt (faithful, no decay).
- Lit block swap driven by the tick, exactly like updateEntity.

### Container GUIs (same SurvivalInvScreen, new modes)
Extended slot addressing grows: 0-35 inv, 36-44 craft, 45-71 open container
(SURVIVAL_CONTAINER_BASE). GuiContainer click semantics work unchanged.
- **Furnace** (gui/furnace.png 176x166): input (56,17), fuel (56,53), output
  (116,35); flame overlay src(176,0) dest(56,36+12-h) h=burn*12/currentBurn;
  arrow src(176,14) dest(79,34) w=cook*24/200. All era-faithful coords; the
  furnace slots are plain slots (no fuel/output restrictions - genuine).
- **Chest** (gui/container.png, 176x168 = 114+3*18): grid at (8,18), player
  rows at 85/143 (one pixel lower than furnace's 84/142 - genuine quirk).
  Panel composed of the two texture strips exactly like GuiChest (0,0)-(176,71)
  + (0,126)-(176,222). Chest refuses to open with an opaque block above it
  (faithful); right-click on any container is consumed even when it refuses.
- Both GUIs verified rendering + interacting in the rig (store/retrieve,
  smelt with live flame/arrow overlays).
- Double chests: NOT implemented (single 27-slot chests only for now).

### Resources
gui/furnace.png + gui/container.png added to the beta-jar extraction
(defaultZipEntries + SelectEntry + ProcessEntry). Growing the required-entry
table makes an OLD default.zip fail the existence check, so the launcher
re-downloads and rebuilds it automatically - no manual deletion needed.

### Recipe fixes (user report: axe uncraftable)
**Root cause**: the generated tool patterns stored ALL shapes 3-wide, but
`Recipe_MatchesAt` reads `cells[ry*w + rx]` at the pattern's own width - so
the 2-wide axe/hoe patterns were scrambled (axe read as XX/.X/S. garbage) and
NEVER matched. Shovel/sword had an explicit repack; axe/hoe didn't. Fixed by
tightly packing every pattern at its true width.
**Missing recipes added** (vs the complete CraftingManager list):
- bow (" #X/# X/ #X", stick+string), arrows x4 ("X/#/Y" iron/stick/feather)
- gold block <-> 9 gold ingots, iron block <-> 9 iron ingots (RecipesIngots;
  diamond pair omitted - no diamond block id in the classic set)
- painting (planks ring around gray cloth)
Verified with a 13-case matcher self-test battery in the live game - all
PASS (axe at two grid offsets, hoe, stone pick, iron sword, shovel, bow,
arrows, gold block both directions, painting, sticks, workbench).

### Verification method
Headless rig again (Xvfb + xdotool + real default.zip with the b1.7.3 GUI
textures injected): placed containers via a temporary debug-menu harness
(removed before commit), loaded coal+ore by real GUI clicks, watched the
flame light, the arrow fill, 5 ingots accumulate, the TE survive lit-swap
and close/reopen; stored sand in a chest, broke it (proper 2.5s mining -
hardness fix confirmed live), saw the scatter drops.

---

## SESSION LOG — workbench crafting table finally opens (root cause found) + E key

The workbench right-click "not opening" bug that survived several sessions is
FIXED, and the root cause was found by reproducing it live (headless Xvfb +
xdotool, driving the real game and reading stderr diagnostics), not by static
review — because on paper every link was already correct.

### The real bug (not what we thought)
Prior guesses (both-flags state, stale crafting.png) were all wrong. Live trace
of a genuine right-click on a placed workbench:

    [WB] tryuse valid=1
    [WB] pos=64,34,61 block=66      <- aiming at the workbench
    [WB] Show: Gui_Add done         <- screen DID open
    [WB] Click mx=400 my=300 hit=-1 <- the SAME right-click hit the new screen
    [WB] Click: hit<0 -> CLOSING    <- and closed it instantly

Right mouse is delivered as a `CCMOUSE_R` key event. When you right-click a
workbench with no screen open, the event falls through to BIND_PLACE_BLOCK →
`SurvivalTest_TryUseBlock` → `SurvivalInvScreen_Show()`. The screen opens and
grabs input, the cursor re-centres (a PointerMove to screen centre), and then
that *same* `CCMOUSE_R` reaches the freshly-opened screen's KeyDown, which
routed it into `SurvivalInv_Click` at the crosshair/centre. That point is the
panel background (`hit == -1`), and the old code closed the screen on ANY
`hit < 0`. So it opened and shut in one input tick — invisible to the player.
(The E/B pocket inventory never hit this because it opens from a KEY event, not
a mouse button that also lands on the panel.)

### The fix (also more faithful)
`SurvivalInv_Click`, on `hit < 0`, now distinguishes clicking the panel
BACKGROUND (inside panelX/Y/W/H → do nothing, as genuine Minecraft does) from
clicking fully OUTSIDE the window (→ refund cursor + grid and close). The stray
opening click lands on the panel centre = inside → no-op → screen stays open.
Verified live: the 3×3 workbench grid opens and stays up.

### Bonus confirmations from the same live session
- The **paperdoll renders correctly** now (default Steve skin, upright, framed
  in its box) on OpenGL — the viewport-origin + ortho-restore fixes from the
  previous session work. (Direct3D still needs a real Windows/GPU retest.)
- The pocket inventory (2×2 craft + storage + doll) and workbench (3×3) both
  open and stay open.

### E key opens the survival inventory (user request)
BIND_INVENTORY defaults to **B** in ClassiCube; the user wanted **E** (modern
Minecraft muscle memory). `ChatScreen_KeyDown` now also opens the survival
inventory on `E` when `SurvivalTest_Enabled`. E is `BIND_FLY_DOWN` by default,
which is inert in survival (flying disabled), so there's no conflict; B still
works too.

Testing method note: headless repro rig lives in the scratchpad — Xvfb :99 +
the linux GL build with `survival-gamemode=2`, driven by xdotool, screenshots
via `import -window root`. Invaluable for GUI/input bugs that read as correct
on paper.

---

## SESSION LOG — paperdoll Direct3D fix (viewport origin) + GUI-corruption fix

User: "work on the paper doll fixes and the d3dx fix as well, it seems like the
geometry of the model was fucked up as well." Two real, backend-level bugs found
in `SurvivalInv_RenderDoll` — the previous "3D-in-2D-pass depth-stencil state
object" theory was a red herring (D3D11's `Gfx_SetDepthTest/Write` apply
immediately via `OM_UpdateDepthState`; D3D9 uses plain SetRenderState). The
actual culprits:

### Bug 1 — viewport Y origin mismatch (the D3D "fucked-up geometry")
`Gfx_SetScissor` takes **top-left**-origin window coords on every backend
(it flips internally for GL's bottom-left `glScissor`). But GL's
`Gfx_SetViewport` was passing its argument **straight** to `glViewport`
(bottom-left) with no flip — so the doll code had to pre-flip the Y itself
(`Game.Height - boxY - boxH + 1`) to make GL correct. That pre-flipped,
bottom-left Y was then *also* handed to the Direct3D backends, whose
`Gfx_SetViewport` treats Y as **top-left** → the D3D viewport landed in the
wrong vertical half of the screen, so only a clipped sliver of the model fell
inside the (correctly-placed, top-left) scissor rect = "geometry fucked up".

Fix: make GL's `Gfx_SetViewport` flip Y internally
(`_glViewport(x, Game.Height - h - y, w, h)`), exactly mirroring
`Gfx_SetScissor` right above it, so **viewport and scissor now take the same
top-left coords on all backends** (which Graphics.h already documents: "This
region should normally be the same as the scissor region"). The doll then
passes plain top-left `boxY+1` to both. GL output is byte-identical to before
(the flip reproduces the old manual math); D3D now gets the correct region.
- Safe globally: the only non-`(0,0,W,H)` viewport caller besides the doll is
  console splitscreen (`Game.c`), and every splitscreen platform uses its own
  console graphics backend — none include `_GLShared.h`. All the
  `(0,0,Width,Height)` callers are flip-invariant.

### Bug 2 — world projection matrix leaked into the rest of the 2D pass
On exit the doll restored `Gfx_LoadMatrix(MATRIX_PROJ, &Gfx.Projection)` —
but `Gfx.Projection` holds the **3D world perspective** matrix, not the 2D
ortho that `Gfx_Begin2D` had set. Any GUI drawn *after* the doll in the same
2D pass (other screens, Draw2D hooks, the pause/options overlay) then projected
through perspective → garbled/invisible widgets. This matches earlier live
reports of "hides buttons when hitting escape" and "blocks floating in the
hotbar". Fix: rebuild the exact ortho (`Gfx_CalcOrthoMatrix(..-100,1000)`) +
identity view that `Gfx_Begin2D` uses, instead of reloading the world matrix.

Depth handling verified consistent per-backend: `Gfx_ClearBuffers(DEPTH)`
clears to each backend's "far" value (GL 1.0 + LEQUAL + standard proj; D3D
0.0 + GREATEREQUAL + reversed-Z proj), and `Gfx_CalcPerspectiveMatrix`
emits the matching convention, so the cleared doll depth range is correct
everywhere. Depth func is set once at init and never changed, so it stays
right through the model draw.

Linux/GL build clean. **User retest**: the doll should now render inside its
box on Direct3D9/11 (not just OpenGL), and opening then Esc-ing the survival
inventory should no longer corrupt other GUI.

---

## SESSION LOG — single gamemode enum replaces the two mode booleans

User asked for a better scheme than "a bunch of flags that can unfortunately
interfere with each other", then approved: one authoritative option key,
`survival-gamemode` (`OPT_SURVIVAL_GAMEMODE`), an int enum:

- `SURVIVAL_GAMEMODE_OFF   = 0` — plain creative ClassiCube
- `SURVIVAL_GAMEMODE_C030  = 1` — faithful c0.30 Survival Test
- `SURVIVAL_GAMEMODE_INDEV = 2` — Indev (in-20100223) layer over the core

### Why (previous bugs this class-of-design caused)
1. **Feathers/string/porkchops leaking into c0.30 survival**: both
   `survival-mode` AND `indev-mode` could be true at once (the old Indev
   launcher button set indev without clearing survival for a while), and the
   Indev drop tables keyed only off `IndevTest_Enabled`. Order-dependent
   precedence hacks (`indev && !survival`) papered over it.
2. **Dead "Indev (WIP)" launcher button**: `UseModeIndev` set the flag, then
   `ChooseMode_Click` unconditionally cleared it — two writers of the same
   pair of booleans disagreeing about invariants.
3. **Workbench right-click doing nothing**: the both-flags-set state from (1)
   made the precedence hack disable the Indev layer entirely, so
   `IndevTest_IsWorkbench` never matched. (Logic itself verified correct.)

With a single enum the three states are mutually exclusive *by construction* —
no writer can produce "both on".

### Implementation (everywhere)
- `Options.h`: `OPT_SURVIVAL_GAMEMODE "survival-gamemode"` — the authoritative
  key; the two legacy booleans remain only for migration/downgrade compat.
- `SurvivalTest.h`: `enum SurvivalGamemode` + `int SurvivalTest_Gamemode(void)`.
- `SurvivalTest.c`: resolver reads the int option (missing-key sentinel -1);
  falls back to the legacy booleans (survival wins a tie, matching the old
  runtime backstop so upgraded installs keep their behaviour). OnInit:
  `SurvivalTest_Enabled = Gamemode() != OFF`.
- `IndevTest.c`: `IndevTest_Enabled = Gamemode() == INDEV` — the init-order
  dependency between the two components' OnInit hooks is gone.
- `Audio.c`: music-gap defaults keyed off the resolver (order-independent).
- `LScreens.c` (CRLF!): new `SetSurvivalGamemode(mode)` helper writes the enum
  AND keeps both legacy booleans in sync (downgrade compat). `SurvivalMode_Click`
  toggles C030<->OFF via the resolver; `ChooseMode_Click` gained the rule
  "Indev button selects INDEV; plain mode buttons only turn Indev off,
  preserving an existing c0.30 choice" (matches the old intent, now explicit);
  the "Survival: ON/OFF" caption reads `Gamemode() == C030`.
- Remaining legacy-flag references verified down to exactly two sites:
  the resolver's migration fallback and the launcher's compat writers.

### Migration behaviour
- Fresh installs / new picks: only the enum matters.
- Old options.txt (booleans only): resolver maps survival->1, indev->2,
  both->1 (survival wins, same as the old backstop), none->0. First launcher
  mode pick writes the enum and the state is canonical from then on.

Full clean build verified. **User retest needed**: re-pick Indev in the
launcher (writes the enum, clears any stale both-flags state), delete
default.zip once (for gui/crafting.png), then retry workbench right-click.

---

## SESSION LOG — Indev mode plumbing + modularity groundwork

Next major goal: an **Indev (in-20100223) gamemode** layered on the survival
core. Ground truth: the deobfuscated EaglerPorts/in-20100223 tree (fetched to
/tmp/indev_eagler; public repo, re-fetchable any time).

### Plumbing added
- `src/IndevTest.c/h` - new IGameComponent + `IndevTest_Enabled` flag from the
  new `OPT_INDEV_MODE` ("indev-mode") option. Registered BEFORE SurvivalTest
  in Game.c so `SurvivalTest_Enabled = survival option || IndevTest_Enabled`
  can key the shared survival core. No launcher UI yet (option-file only).
- Architecture: SurvivalTest.c = shared survival core (entities, combat,
  drops, HUD); IndevTest.c grows the version layer (ItemStacks, tools,
  crafting, day/night...). c0.30-s behaviour stays frozen behind its flag.

### Modularity groundwork (multiplayer/MCGalaxy-proofing)
- Hardness converted from a hardcoded switch to a runtime per-block table
  (`st_hardness[BLOCK_COUNT]`, lazily seeded from the faithful c0.30
  defaults) with `SurvivalTest_SetHardness(block, hardness)` exposed - so
  CPE BlockDefs custom blocks or a future MCGalaxy plugin (over a CPE
  PluginMessages channel) can override per-block hardness without code
  changes. Same treatment planned for drop tables and the mob registry
  (data-driven, runtime-extendable) as the Indev work touches them.
- Multiplayer note: server-authoritative survival would live in an MCGalaxy
  plugin; the client keeps HUD/rendering/break-progress hooks reusable.

### Phase plan (agreed)
mode plumbing (DONE) -> ItemStack refactor (IN PROGRESS) -> tools/durability/
mob item drops -> crafting + GUIs -> day/night + lighting -> chests/furnaces
-> fire/farming/bow/armor -> polish.

### Drag-and-drop stack handling (GuiContainer semantics) - user request
- Cursor-held stack backend (InventoryPlayer.itemStack): st_cursor +
  SurvivalTest_SlotClick implementing the genuine rules - empty cursor: left
  takes all / RIGHT TAKES THE UPPER HALF; same id: left merges up to max
  stack / right places exactly ONE; different: swap. ResultClick crafts once
  ONTO THE CURSOR (stacks when same id fits). CursorReturn empties the
  cursor back to inventory on any close path (Esc, E, click-outside) so
  stacks are never eaten.
- Screen: PointerDown = left click; right mouse arrives as a KEY event
  (CCMOUSE_R) and routes through the same click path with the tracked mouse
  position. The held stack renders following the mouse: blocks join the iso
  mesh (rebuilt while carrying), items draw as sprites; old heldSlot swap UI
  retired (field kept, always -1).
- So half-stack splitting / one-by-one placement for crafting works:
  right-click a stack to halve, right-click cells to distribute singles.

### BLOCK ADDITIONS LANDED: workbench/chest/furnace/torch defined
- BetaPatcher now also extracts terrain.png from the b1.7.3 jar and patches
  the Indev tiles into our reserved free cells (sources verified VISUALLY
  against the rendered atlas): wb top(11,2)/side(12,3)/front(11,3), furnace
  front(12,2)/lit(13,3)/side(13,2)/top(14,3), chest top(9,1)/side(10,1)/
  front(11,1), torch(0,5), crops (8..15,5), farmland wet/dry (6,5)/(7,5).
  Crops need 8 stages so the reservation extends into row 7: crops 107-114,
  farmland 115/116 (both rows fully free per the derivation).
- Blocks defined at ids 66-70 via Block_SetName/Block_Tex/Block_DefineCustom:
  Workbench(66, wood 30 hits), Chest(67), Furnace(68, stone 70),
  Furnace-lit(69), Torch(70: fullbright walk-through sprite, instant break).
  Front textures on FACE_ZMIN; drops = self via the default drop path;
  placement/consumption flow through the existing survival gates unchanged.
- Recipes added: 2x2 planks -> WORKBENCH (craftable in the pocket grid!),
  coal-over-stick -> 4 torches (also 2x2-able), planks ring -> chest and
  cobble ring -> furnace (registered now, craftable once the 3x3 exists).
- NEXT: right-clicking a placed workbench opens the crafting screen with a
  3x3 grid (parameterise SURVIVAL_CRAFT grid size); furnace smelting +
  chest storage are their own later phases (need tile-entity state).
- Existing installs: default.zip lacks a version stamp for the terrain
  patch - delete default.zip (or texpacks/default.zip) once to re-generate
  with the new tiles; fresh installs get them automatically.

### BLOCK ADDITIONS: atlas + id reservations DERIVED (blocker cleared)
Computed from core_blockDefs + both jar patchers + the fire animation cell:
the engine uses only 74/256 terrain cells - ROWS 6-15 (tile indices 96-255)
ARE ENTIRELY FREE, plus scattered cells in rows 1-5. Block ids 66+ are free
(engine defs end at 65/stone brick).
RESERVED LAYOUT (canonical from here on):
- ids: 66 workbench, 67 chest, 68 furnace, 69 furnace-lit, 70 torch,
  71+ farmland/crops when farming lands.
- tiles (row 6, indices 96+): 96 workbench top, 97 wb side, 98 wb front,
  99 furnace front, 100 furnace-lit front, 101 furnace side, 102 furnace
  top, 103 chest front, 104 chest side, 105 chest top, 106 torch,
  107-111 crops stages.
Tiles extracted from the b1.7.3 jar's terrain.png by extending BetaPatcher
(b1.7.3 source cells to be read off its atlas layout next session).
CAVEAT: correct for the stock auto-downloaded terrain.png; custom texture
packs show whatever their rows 6-15 contain unless they adopt this layout.

### NEXT SESSION: block additions plan (workbench first)
Approach verified this session: extend BetaPatcher (Resources.c) to also
extract terrain.png from the already-downloaded b1.7.3 jar and PatchTerrainTile
the workbench top/side/front, furnace front/side, chest tiles into FREE cells
of the terrain atlas (must first verify which cells are unused against the
actual default terrain.png - not safely determinable blind, hence deferred).
Then define blocks via the engine block-defs (id 66+), give them the Indev
hardness/sounds, add the 2x2->workbench recipe (already encodable), and a
3x3 variant of the crafting screen (grid size is the only difference).

### Inventory screen: in-screen hotbar row added (user report)
Hotbar slots (0-8) weren't clickable inside the inventory screen - HitSlot
started at slot 9, so stacks couldn't move between hotbar and storage/craft.
GuiInventory-style fix: the hotbar now renders as its own row below the
storage grid (double gap separating them, panel grown/centred accordingly),
and every pass (slot boxes, iso blocks, item sprites, count digits, hit
tests) picks it up through the shared DisplayCount/DisplaySlot iteration.
SwapSlots already handled indices 0-8, so click-to-pick/click-to-swap works
across all three regions with no backend change.

### CRAFTING NOW PLAYABLE + critical HUD regression + Indev drops
- **HUD corruption fixed**: HeldBlockRenderer_RenderModel's arm-skip early
  return bypassed the depth/cull teardown, leaving depth test off + culling
  on -> corrupted all 2D UI after (vanishing hotbar, floating blocks, hidden
  pause buttons). Now skips only the arm draw, keeps the teardown.
- **Indev block drops** (canHarvestBlock gate + BlockX.idDropped): rock/iron
  blocks (stone dig-sound) drop NOTHING without a pickaxe of sufficient
  harvest level (obsidian 3, gold ore/block 2, iron ore/block 1, other rock
  any); log drops the LOG block not planks; coal ore -> coal item; gravel
  1/10 flint; glass/bookshelf nothing. c0.30 drops unchanged. SpawnDrop
  widened to the full id space so item drops (coal/flint) work.
- **Crafting is now PLAYABLE in Indev**: the GUI (SurvivalInvScreen: 2x2
  grid + result slot + item-sprite slots + result-take) and backend
  (st_craft, CraftResult/Take/ReturnAll) already existed; this session added
  the recipe engine they matmch against (IndevTest_MatchRecipe) and opened
  the screen for Indev mode (was Enhanced-only) - so E/B now opens the
  crafting inventory. Plank->stick->wooden pickaxe is fully craftable.
- KNOWN: the storage/paperdoll screen isn't visually Indev-faithful yet
  (cosmetic, deferred); 3x3 workbench needs the workbench block (deferred
  with the block additions).

### CRAFTING PHASE OPENED: melee damage + full recipe engine
- **Melee damage** (Minecraft.java:352): damage = held Item.getDamageVsEntity;
  bare fist 1 (Indev nerf vs c0.30's flat 4 - c0.30 mode keeps 4), shovel/
  pick/axe = 1/2/3 + tier, swords = 4 + tier*2 (ItemSword.java:12), hoes 1.
- **Recipe engine** (CraftingManager + Recipes* in-20100223): shaped w*h
  patterns in full-space ids matched at any offset in a gw*gh grid
  (IndevTest_MatchRecipe). Explicit table: planks x4, sticks x4, slabs x3,
  bread, gray cloth from 9 string, TNT checkerboard, bowls x4, mushroom soup
  (both orders), flint&steel. Generated: 25 tool/weapon recipes (5 materials
  x pickaxe/shovel/axe/hoe/sword) exactly as RecipesTools/Weapons compose
  them. DEFERRED (need blocks the classic set lacks): torches, workbench,
  crate/chest, furnace, painting, armor plates.
- NEXT: the 2x2 pocket crafting GUI (then 3x3 via workbench once the block
  exists), consuming grid items on take, exactly per GuiCrafting/
  SlotCrafting semantics.

### Held item CONFIRMED WORKING via the drop pass (user screenshot)
The camera-anchored sprite renders. Bare arm is now suppressed while the
sprite shows (it previously rendered beside it). BACK-BURNERED by agreement:
the genuine ItemRenderer look - an EXTRUDED 3D mesh built from the sprite's
pixels, held angled in the swing transform chain - needs a proper renderer
port (mesh extrusion + swing animation) and replaces this approximation
wholesale when done.

### Held item sprite REWRITTEN onto the proven drop-sprite pipeline
Two fixes to the bespoke HeldBlockRenderer quad (view matrix load, face
culling) still left it invisible on the user's machine - rather than a third
round of blind matrix archaeology, the held sprite now renders through the
drop-sprite pass, which is proven working there (porkchop drops visible).
SurvivalTest_HeldSpriteState anchors the quad at camera + look*0.55 +
right*0.28 - 0.40 up (first-person hand position); drawn only in first
person with an item id selected. The HeldBlockRenderer branch is reverted
to the plain bare-arm path. Trade-offs: world-pass depth testing (sprite can
clip into point-blank walls) and no swing animation yet - both acceptable
until the proper extruded ItemRenderer port.

### Held item sprite: second blocker found (still-invisible after view fix)
RenderModel turns face culling ON for the model paths; the billboard quad's
winding is back-facing in the held view, so it was culled even once the view
matrix was loaded. Culling is now disabled around the quad draw (and restored
after). Two stacked bugs total: missing MATRIX_VIEW load + culling.

### Item ids could be "placed" as garbage blocks (user report)
Right-clicking with a non-block item (string etc) selected could leak the
item id into the engine's block placement truncated to 8 bits (string 287 ->
wool 31), placing garbage blocks whose mining then produced the reported
"invalid block drops". SurvivalTest_CanPlace now refuses outright when the
selected slot holds an item id (genuine right-click with items does the
item's own action or nothing), and the place-consume hook only consumes when
the placed block actually matches the held slot's block - so no path can eat
the wrong stack or place an item id again.

### Two regressions from the visuals batch fixed (user reports)
- **Stack counts vanished**: Gfx_Draw2DFlat (durability bar) and
  Texture_Render switch the pipeline to COLOURED format / another VB, and
  the icon block only re-bound the VB - every later HUD mesh draw (counts,
  bubbles) read TEXTURED-format vertices with the wrong stride. Both the
  icon/bar block and the corner-label block now restore
  Gfx_SetVertexFormat(TEXTURED) + rebind s->vb.
- **Held pickaxe sprite invisible** (dirt block fine): Model_Render loads
  the view matrix itself for the block/arm paths, but the raw item quad
  never did - it rendered with whatever transform the last entity left.
  Now loads MATRIX_VIEW from Gfx.View before drawing.

### Indev visual feedback: mini-block drops, durability bar, held item
- **Block drops in Indev render as miniature FULL blocks** (whole tile per
  face, RenderItem's renderBlockOnInventory at 0.25 scale) instead of
  classic's middle-50%-cropped ItemModel cube - c0.30 mode keeps the crop.
  (Per-face top/bottom textures still TODO; all faces use the side tile.)
- **Durability bar** under hotbar tool icons - RenderItem.renderItemOverlay-
  IntoGUI exact: 13x2 black backing at (x+2, y+13) of the 16px icon space, a
  12x1 dark track, and (13 - dmg*13/max) of the red->green gradient
  ((255-v)<<16 | v<<8, v = 255 - dmg*255/max), all scaled to the icon size.
  New SurvivalTest_SlotDamage accessor.
- **Held item shows in hand**: holding an item id draws its sprite quad at
  the held-block position (approximates ItemRenderer's extruded sprite -
  full 3D extrusion + swing animation still TODO). Falls back to the bare
  arm as before when no items.png/UV. VB freed on context loss.

### Tools/durability phase started + drop rendering regression fixed
- **REGRESSION FIX (user report: block drops rendered as full blocks)**: the
  cube/glow BUILD loops didn't skip item-id drops even though the 1D batch
  COUNTS did - item drops spilled into other drops' vertex ranges and
  corrupted the cube geometry. Both loops now skip !ST_ID_IS_BLOCK drops.
- **All-mob Indev death drops** (EntityLiving.onDeath rand(3) of scoreValue):
  zombie feather(32), skeleton arrow(6), spider string(31), creeper
  gunpowder(33), pig raw porkchop(63); sheep drop nothing on death in Indev
  (no scoreValue override) - c0.30 mode keeps mushrooms/wool.
- **Mining speed**: st_breakHits advances by IndevTest_MiningSpeed(held id,
  block) = (tier+1)*2 when the tool class matches the block's dig-sound
  material (pickaxe:stone/metal, shovel:grass/gravel/sand/snow, axe:wood),
  else 1. NOTE: gold tools are tier 0 in Indev = wood speed, genuine quirk.
- **Durability** (ItemTool.maxDamage = 32 << tier): held tool wears 1 per
  block broken, 2 per landed melee hit (ItemStack.hitEntity), shatters and
  clears the slot at max. Sword damage bonus vs mobs still TODO.
- **Q drops the held item** (EntityPlayer.dropPlayerItem): one of the stack,
  spawned at eye-0.3 with look-direction*0.3 velocity (+0.1 up bias, slight
  jitter) and the genuine 40-tick self-pickup delay (pickupDelay field
  reintroduced, Indev-toss only; c0.30 drops keep instant pickup).
- **F9 "Give Iron Pick"** button (SurvivalTest_DebugGiveItem) so tools are
  testable before crafting exists.

### Sprite fidelity pass (RenderItem.doRender exact)
- Drop sprites now 0.5 world units (were 0.25 - user spotted the difference)
  and stacks draw the genuine jumbled copies: 1 / 2 (count>1) / 3 (count>5) /
  4 (count>20), offset (rand*2-1)*0.3 per axis from RenderItem's FIXED seed
  187 - reproduced as a precomputed offset table (same deterministic jumble
  every frame, matching genuine behaviour without per-frame RNG).
- Still simplified vs genuine: our billboard faces the camera fully (genuine
  yaw-billboards only), no drop shadow (shadowSize 0.15 - engine has no
  entity shadow hook for hand-simulated drops). Documented, low priority.
- NEXT PHASE (agreed roadmap): tools & durability + more Indev mob drops
  (zombies drop feathers, skeletons arrows, sheep wool blocks) -> crafting
  + GUIs -> day/night + lighting -> chests/furnaces.

### BUG FIX: Indev launcher button never actually enabled the mode
UseModeIndev set indev-mode=true and then routed through ChooseMode_Click,
which unconditionally cleared it - so the button was a no-op and no feature
ever saw the flag. ChooseMode_Click now takes an `indev` parameter and all
four mode buttons are exclusive through the ONE code path (Indev also forces
survival-mode off; the Survival toggle still clears indev-mode).
Verified flag chain: launcher writes indev-mode -> IndevTest_Component
(registered before SurvivalTest) reads it -> SurvivalTest core activates on
survival||indev -> gated features: item defs seeding + items.png entry
(IndevTest.c), pig porkchop drops + item eating (SurvivalTest.c), hotbar
icons + "Minecraft Indev" corner label (Screens.c).

### items.png auto-download LANDED (no texture pack needed anymore)
- The in-20100223 jar keeps its atlas at /gui/items.png INSIDE the jar (user
  correctly found no loose file). Neither already-downloaded jar has it
  (classic predates items; 1.6.2 is post-1.5-texture-split), so Resources.c
  now fetches Mojang's OFFICIAL b1.7.3 client jar (launcher.mojang.com,
  sha1 43db9b49... verified against piston-meta's manifest; 1431 KB) and a
  new BetaPatcher extracts gui/items.png into default.zip as items.png.
  Early items.png cell layout is identical Indev->beta (icons appended only),
  so every icon index in the defs table lines up.
- items.png added to the required default.zip entries - EXISTING installs
  detect it missing and re-download automatically on next launch.
- Texture packs can still override it via the items_entry TextureEntry.

### FIRST TESTABLE INDEV FEATURE: pig -> porkchop, end-to-end item pipeline
- **Pig death in Indev mode** drops 0-2 RAW PORKCHOPS (EntityLiving.onDeath:
  rand(3) of scoreValue-as-item-id; item 63) instead of c0.30 mushrooms.
- **Item-id drop entities**: DropItem.block widened to the full id space;
  item drops render as billboard SPRITES from items.png (Indev EntityItem
  style, bobbing like the cubes) via a new pass modeled on the smoke
  renderer. Block drops unchanged. Sprites bail without an items.png
  (supply one via texture pack until the Resources patcher lands).
- **Pickup** generalised (AddItem over the id space; AddBlock is a macro
  wrapper); death scatter now includes item stacks.
- **Eating**: TryEat checks IndevTest_ItemFoodHeal first - raw porkchop
  heals 3, cooked 8 (soup bowl-return TODO).
- **Hotbar item icons**: immediate Texture_Render quads over slots holding
  item ids (slots are engine-AIR there, so no cube behind).
- **"Minecraft Indev" corner label** top-left whenever the F3/FPS line is
  hidden (body font, x/y = 2).
- **Launcher**: Indev is a separate MODE button ("Indev (WIP)", next to the
  Survival toggle on the Choose Mode screen; sets indev-mode on + survival
  off + non-classic engine mode). Survival toggle clears indev-mode;
  Enhanced/Classic modes clear it too. The survival-row description now
  covers both.
- TEST PATH: launcher -> Choose Mode -> Indev (WIP); needs a texture pack
  with items.png for sprites; kill a pig, walk over the porkchop, watch the
  hotbar icon, right-click to eat (heal 3).

### ItemStack refactor - step 3b COMPLETE (rendering foundation)
- **Full iconIndex mapping recovered** from Item.java's init, including the
  tail the first pass missed: flint (62, icon 6), RAW/COOKED PORKCHOP (63/64,
  icons 87/88, heal 3/8 - the earlier "no porkchop in this version" note was
  WRONG), painting (65, icon 26). Hoes assign icons via a separate variable
  (128-132, row 8 of the atlas) which is why the first extraction missed
  them. Roster now 66 items, each with kind/param/icon.
- `IndevTest_ItemSpriteUV(id, &u1,&v1,&u2,&v2)`: atlas UVs on the 16x16-cell
  items.png grid; false for unknown/block ids. `SurvivalTest_SlotId(slot)`
  exposes the raw id so HUD/drop renderers can branch item-vs-block.
- Visible wiring (hotbar quads, drop sprite entities, held item) lands
  together with the first mechanic that actually PUTS an item id in a slot
  (mob porkchop drops / tool crafting) so it can be tested for real rather
  than dead code. items.png auto-provision patcher still planned per below.

### ItemStack refactor - step 3b started: items.png plumbing
- `items.png` registered as a TextureEntry in the Indev layer (texture packs
  can supply it now); `IndevTest_ItemsTex()` exposes the texture id and all
  sprite rendering must bail gracefully while it is 0.
- **Auto-download answer**: YES, feasible - Resources.c already downloads
  Mojang's classic 0.30 jar AND a 1.6.2 jar at first launch and extracts
  assets into default.zip via patcher callbacks (that's where char.png,
  arrows.png etc. come from). Plan: add a patcher step composing an Indev-
  layout items.png atlas from the already-downloaded jar assets (1.6.2 has
  per-item textures post-texture-split; most classic-era item pixels are
  unchanged). Needs the iconIndex -> atlas cell mapping from Item.java's
  init first. Until then: texture pack supplies items.png.
- REMAINING for 3b: iconIndex mapping per item in the defs table, hotbar/
  hand/drop sprite rendering keyed on ST_ID_IS_BLOCK.

### ItemStack refactor - step 3a landed + hotbar bug fix
- **Hotbar-cycling bug (user report)**: pressing G switched to the engine's
  alternate hotbar row, desyncing it from the survival inventory (blocks
  seemed to vanish until a pickup resynced). Inventory_SetHotbarIndex now
  no-ops while SurvivalTest_Enabled - survival owns its single 9-slot hotbar.
- **Complete item definitions table** (IndevTest.c): all 62 in-20100223
  items from Item.java's static init - tools/swords/hoes in 5 tiers (param =
  tier; maxDamage = 32 << tier per ItemTool.java:14), flint&steel, bow/arrow,
  materials, foods (apple heals 4, bread 5, soup 10 - NO porkchop in this
  version), seeds/wheat, 5 armor sets (param = piece). Single-stack kinds
  seed max stack 1, everything else 64. Data-driven for later steps
  (durability, eating, sprites) and eventual server overrides.
- NEXT (step 3b): items.png sprite rendering in hotbar/hand/drops, then
  drops carrying item ids.

### ItemStack refactor - step 2 landed
- Every block-consuming site now routes through ST_ID_BLOCK (SlotBlock,
  hotbar sync, TryEat, death drops - the last skips item-id stacks until
  sprite drop entities exist in step 3), so a future item id can never
  masquerade as a block.
- Runtime per-id max-stack table (ST_MAX_IDS=1024 covering Item.itemsList)
  with SurvivalTest_SetMaxStack; c0.30 default 99 everywhere (unchanged).
  IndevItems_Seed sets the Indev values when the mode is on: 64 default for
  item ids, 1 for the tool/sword/bow ids confirmed so far (roster to be
  completed against Item.java's full static init with the defs table).
- NEXT (step 3): item definitions table (name, tool tier, food value,
  maxDamage - ItemTool sets maxStackSize=1 and per-tier maxDamage), then
  items.png sprites in hotbar/hand/drops.

### ItemStack refactor - design (step 1 landed)
Indev inventories hold ItemStack(id, count, damage) where id spans blocks
(0..255) AND items (256+, e.g. 256+16=porkchop style shifted ids). Step 1:
`struct SurvivalSlot` becomes `{ cc_uint16 id; cc_int16 count, damage }` with
ST_ID_IS_BLOCK/ST_ID_BLOCK helpers; behaviour unchanged (ids are always
blocks in c0.30 mode). Later steps: item definitions table (max stack,
tool tier, food value...), items.png sprite rendering in hotbar/hand/drops,
drops carrying full stacks, tool damage on use.

---

## SESSION LOG — Entity pool overflow fixes (drops vanishing on littered maps)

User report confirmed NOT faithful: with 64+ drops in the world, breaking a
block yielded nothing - the fixed pools silently discarded the NEW entity
when full, while genuine Level.addEntity is an unbounded ArrayList.add (no
entity cap exists anywhere in c0.30).

- **Drops**: DROP_MAX 64 -> 256; on overflow the OLDEST drop (largest age,
  never one mid-pickup) is evicted so fresh drops always spawn - it was
  nearest its 5-minute despawn anyway.
- **Arrows**: on overflow evict the longest-STUCK arrow first (inert
  scenery closest to despawn), else the oldest in flight - firing never
  silently fails.
- **TNT**: TNT_MAX 8 -> 64 fuses; if a 64+ chain reaction still overflows,
  the overflow TNT drops as a pickup item instead of vanishing (no material
  loss; can't recursively explode from inside the chain loop).

---

## SESSION LOG — Sound randomization, music gap, spawn-scaling verification

### Added
- **Per-play sound randomization** (StepSound.getVolume/getPitch): every dig/
  step/place sound now divides its pitch by (rand*0.2+0.9) (~91-111%) and its
  volume by (rand*0.4+1) (~71-100%), survival-only, in Audio.c's Sounds_Play.
- **Music gap**: survival DEFAULTS are now 300-1200s between calm tracks
  (Minecraft.tick's `lastBGM = now + 300000 + rand(900000)`); a user-set
  music-delay option still wins. Engine default (120-420s) unchanged outside
  survival.

### Mob spawn scaling - VERIFIED ALREADY FAITHFUL (no change)
User suspected spawns were static across world sizes. Checked against
SurvivalGameMode.java: periodic gate is `rand(100) < area` with
`area = w*h*d / 64^3` and live cap `area*20`; initial population is
`volume/800` spawner attempts. Our port implements exactly these formulas
(SurvivalTest_TrySpawnMobs / SpawnInitialMobs), so spawn pressure DOES scale:
128x64x128 -> area 4 (4%/tick gate, cap 80); 256x64x256 -> area 16 (16%/tick,
cap 320, clamped by the 256 mob slots); 512x64x512 -> area 64 (64%/tick).
Only caveat: MOB_MAX=256 slots clamp the cap on very large maps.

### Engine plumbing done in the follow-up commit (backlog cleared)
- **Sheep grazing head dip**: Sheep.renderModel moves the head part's render
  origin down 8/16 and forward 1/16 blocks * graze. Since Model_DrawRotate
  emits R*(v-p)+p, translating the origin equals translating the emitted
  vertices - Model.c's SheepModel_DipHead shifts the just-drawn head (and
  fur head) verts in place, driven by a new Anim.Graze field. graze/grazeO
  ease at 0.2/tick in Mob_SheepUpdate (Sheep.aiStep) and lerp at render.
- **Underwater/lava ambient tint** (GL_LIGHT_MODEL_AMBIENT): while the
  camera block is water, survival-rendered entity colours are multiplied by
  (0.4, 0.4, 0.9); in lava by (0.4, 0.3, 0.3). One helper
  (SurvivalTest_AmbientTint) applied in Mob_GetColor and DropItem_WorldColor
  (which drops/arrows/TNT/smoke all share). Engine entities (other players)
  are untouched.
- **Debug**: F9 menu gained "Mob census" - prints live count / area*20 cap /
  per-tick spawn roll so world-size spawn scaling can be observed in-game.

---

## SESSION LOG — Death camera fix + mob infighting + sound quirks

### Death camera was invisible - root cause found
`GameOverScreen_Show` set `blocksWorld = true`, which makes the engine skip
`Render3DFrame` entirely - the roll/FOV math ran but the world was never
drawn behind the screen. Now `blocksWorld = false`: the world keeps rendering
behind the translucent red gradient while the camera keels and zooms, as
genuine. (Note the genuine ease is slow: ~4 degrees after 1s, ~13 after 5s.)

### Mob infighting (BasicAttackAI.hurt attackTarget = cause)
- `struct Mob.targetSlot` (-1 = player, else st_mobs index) alongside
  hasTarget; `st_hurtCauseSlot` side-channel identifies a mob attacker.
- Aggro on hurt skips same-species causes; arrows resolve to their OWNER,
  so a stray skeleton arrow turns the victim against the skeleton.
- `Mob_DoAttack` chases/faces/attacks the resolved target: mob-vs-mob melee
  goes through Mob_Hurt (knockback, no score credit); LOS clip and blast
  anchor use the target's own heightOffset. Proximity acquisition remains
  player-only, exactly as doAttack's null-target branch. Creeper self-hurt
  cause is its actual victim (score credited only for the player).

### Sound quirks
- Breaking SAND plays the GRAVEL sound; glass breaks with its METAL step
  sound (no shatter in c0.30). Overridden in the survival init only.

---

## SESSION LOG — Polish batch: sounds, HUD flash, combat feedback, death camera

Implemented the prioritized gaps from the three fidelity surveys (sounds /
models+HUD / environment). Environment survey found NO gaps - the engine's
BlockPhysics.c already covers Level.tick's random-tick behaviours (saplings,
grass, flowers/mushrooms, liquids, sponge) and is on by default.

Key finding from the sound survey: **genuine c0.30 has only FOUR sound
events** - footsteps, block break, block place, background music. No mob
voices, no hurt/TNT/explosion/arrow/pickup/splash sounds exist. Do not add
them.

### Added this session
- **Mob footsteps** (`Entity.move`): per-mob `walkDist += horizDist*0.6`,
  step sound of the block under the feet each time it passes `nextStep`,
  gated to 32 blocks from the player (engine audio is non-positional).
- **Block place sound**: removed `!Game_ClassicMode` gate in Audio.c -
  genuine classic plays the placed block's step sound.
- **Sheared sheep switch to the engine's `sheep_nofur` model** (and back on
  regrow) - previously fur was visually permanent. Plus the grazing head
  pitch nod (40-50 degrees alternating, `SheepAI.update`).
- **Heart-row invulnerability flash** (`HUDScreen.render`): while
  `invuln/3 % 2 == 1` (fresh half of the window) the heart backgrounds swap
  to the white-flash sprite and ghost `lastHealth` hearts (U=70/79) draw on
  top. New accessors `SurvivalTest_InvulnTicks/LastHealth`.
- **Per-heart jitter** seeded `ticks*312871` (stable within a tick), and
  hearts/bubbles now pack 8px apart (9px sprites overlap 1px) as genuine.
- **Mob hurt wobble**: `sin((hurtTime/10)^4*PI)*14` degrees of roll while
  hurtTime decays; death keel-over ADDS on top, sum capped at 90. (Rolled
  about model Z, not the genuine hurtDir frame - same simplification the
  death roll already used.)
- **Mob white hit flash**: second additive render pass (white, 75% alpha)
  while `invulnerableTime > duration - 10`, via a `Mob_GetColor` flag +
  `Gfx_SetAlphaBlendingAdditive`.
- **Death camera**: `st_deathTicks` keeps counting while dead; camera rolls
  `40 - 8000/(deathTime+t+200)` degrees and the projection FOV divides by
  `(1 - 500/(deathTime+500))*2 + 1` (1x->3x zoom) via
  `SurvivalTest_DeathFovZoom` hooked into `PerspectiveCamera_GetProjection`.
- **Arrow pickup fly-in**: collected arrows zip to the player over 3 ticks
  (TakeEntityAnim), like item drops, instead of vanishing.
- **Eating feedback**: successful mushroom eat triggers the held-block dip
  (`HeldBlockRenderer_ClickAnim(false)`).
- **Lava fog density 2.0** (was ClassiCube's stock 1.8) - overridden in
  the survival init path only.

### Known remaining polish (documented, not yet done)
- Random per-play volume/pitch on sounds (`vol/(rand*0.4+1)*0.5` etc);
  sand-breaks-as-gravel and glass-breaks-as-stone dig-sound quirks;
  music gap 300-1200s (engine default 2-7 min, user-configurable).
- Sheep grazing head Y-dip (needs model plumbing; pitch nod is done).
- Underwater/lava ambient light-model tint on entities.
- Mob hurtDir frame for the wobble; mob infighting (see prior session log).

---

## SESSION LOG — Full source audit + faithfulness fixes

Four parallel audits cross-referenced every survival mechanic against the
decompiled Java (`Item`, `PrimedTnt`, `Level.explode`, `Mob`, `BasicAI`,
`BasicAttackAI`, `Creeper`, `Sheep`, `Pig`, `MobSpawner`, `Player`,
`Inventory`, `SurvivalGameMode`, `BlockUtils`, `Particle`/`SmokeParticle`,
`Arrow`, `GameOverScreen`). Everything found was fixed across four commits:

### Items (`Item drops: match Item.java spawn, physics and pickup exactly`)
- Spawn scatter `rand*0.7+0.15` per axis per item; ctor velocity ±0.1/tick
  horizontal + fixed 0.2/tick vertical; gravity 0.04/tick (16 blocks/s²);
  the missing *0.98 drag added; invented terminal-velocity clamp, zero-snap
  and 0.5s pickup delay removed; despawn at age 6000 ticks; double slab
  drops 2; full inventory leaves the drop on the ground (`AddBlock` returns
  acceptance); pickup anim lands fully on the player (target y+0.62).

### TNT/explosions (`TNT/explosions: match PrimedTnt, Level.explode...`)
- **Liquids explode** — `canExplode`'s immunity list is exactly the solid
  stone/metal-sound blocks; water/lava are destroyed (and flood back in).
- Genuine blast box `(int)(c-r-1)..(int)(c+r+1)` with +0.5 block-centre
  distances; entity damage measured from `entity.y = feet + heightOffset`
  (1.62 humanoids/player, 1.72 sheep/pig, 0.72 spider — added to the mob
  type table), NOT the bbox centre.
- Primed TNT lands dead/stops at walls (move() zeroes clipped axes; the
  `yd*=-0.5` bounce is dead code); fuse post-decrement (smokes 40 ticks,
  explodes on the 41st); smoke lifetime post-increment (+1 move tick).
- Skeleton aim reproduces the genuine pitch quirk `-atan2(dy, sqrt(dist3d))`.
- Debris: `Particles_BreakBlockEffect` burst at detonation (approximation of
  the genuine 100 TNT / 500 LEAVES gaussian TerrainParticles).

### Player (`Player & mobs: port genuine hurt/invuln/spawn/drop semantics`)
- **Dual-threshold invulnerability** ported (was a flat 0.5s block-all):
  20-tick window; fresher than half → only excess over the opening hit lands;
  staler → full hit + re-arm. Lava/drowning are now genuine per-tick
  `hurt(null,10)`/`hurt(null,2)` calls shaped by that window — the separate
  cadence timers were deleted outright.
- Knockback on entity hits; fall damage `ceil(dist-3)`; lava doesn't cushion
  falls; air refills instantly on surfacing; red mushroom = ordinary
  `hurt(null,3)`; `heal()` grants half-window invuln.
- Hardness: dirt/sand 12, slab/double-slab 20, **brick 0** (absent from
  `getHardness`'s switch → instant break; looks like an upstream omission but
  it IS the ground truth); cracks = `(hits-1)/hardness`.
- **CORRECTED by the systematic audit**: the claim here that
  "GameOverScreen.java has the [Respawn] button" was WRONG - neither
  ground truth has one (both death screens offer only Generate new
  level... / Load level..), and the genuine death screen was restored in
  audit batch 1 (0876ebd). SurvivalTest_Respawn survives as dead code for
  future use. **Death inventory scatter is a DELIBERATE DEVIATION** (user
  decision 2026-07-11): neither c0.30 Player.die nor Indev
  EntityPlayer.onDeath drops any items (no dropAllItems exists), but the
  scatter is kept intentionally because multiplayer support is planned -
  where corpse drops matter. Revisit the gating (option key) when
  multiplayer work starts.
- Arrow pickup uses the item pickup's `bb.grow(1,0,1)` reach.

### Mobs (same commit)
- Water/lava 80% bob-jump applies to EVERY mob (was chase-only).
- Spawner: min-of-two-uniforms Y bias restored (the `min()` macro was
  re-evaluating its RNG-call args → uniform, no bias); `level.isFree` bb
  check before adding (no wall-clipped wide mobs); initial population passes
  a NULL avoid-position (`prepareLevel` uses null, not the spawn point).
- Liquid tests use the genuine `bb.grow(0,-0.4,0)` shrunk box (`ST_InLiquid`).
- Sheep death drops 1-2 WHITE wool (prior "identical to Pig" note was wrong).
- Creeper self-hurt passes the player as cause → knockback + 200 pts credit;
  genuine damage-scaled `sin(tickCount)` brightness pulse in `Mob_GetColor`.
- `MOB_MAX` 32 → 256 (genuine caps: `area*20` live, `volume/800` initial).

### Audit findings NOT acted on (verified false or documented gaps)
- "Arrows defuse primed TNT" — **false**: `Arrow.tick` hits via
  `isShootable()` (PrimedTnt doesn't override it) and passes the ARROW as
  cause, while `PrimedTnt.hurt` only defuses for `cause instanceof Player`.
  Melee-only defuse is faithful as-is.
- Mob infighting (`attackTarget = cause` on being hurt, e.g. a skeleton
  arrow aggroing a zombie onto the skeleton) — NOT ported; our target is a
  player-only boolean. Known gap, single-target simplification.
- The 36-slot inventory (vs genuine 9) is a deliberate extension; genuine
  overflow behaviour (drop on the ground when 9 slots full) applies only
  after 36 here.
- Debris particle bursts approximate the gaussian scatter with the engine's
  block-break burst (public particle API has no arbitrary-velocity spawn).

### Condensing done alongside
- `Explosion_Damage` helper deduplicates the player/mob blast falloff.
- `Mob_PushAgainst` helper deduplicates mob-mob/mob-player push.
- `Mob_SpawnDeathDrops(m, block)` replaces the mushroom-only spawner.
- Lava/drown cadence timers, pickup-delay field and terminal-velocity clamp
  deleted (superseded by faithful mechanics above).

---

## SESSION LOG — Mob-player push + hotbar slot pop animation

### Mob-player push (`SurvivalTest.c` / `Mob_PushApart`)

`BasicAI.tick()` calls `level.findEntities(mob, mob.bb.grow(0.2,0,0.2))` which
returns both other mobs AND the player. For each pushable result it calls
`result.push(mob)` — `Entity.push(Entity)` normalises the horizontal
centre-to-centre vector, divides by distance again, scales by 0.05, then applies
equal-and-opposite impulses (pushthrough=0 for all mobs and the player).

`Mob_PushApart` previously only looped over `st_mobs[]`. The player push block
was added immediately after the mob-mob loop:
- Expanded AABB (±0.2 on X/Z, same as mob-mob) is intersected with the player's AABB
- If overlapping and `sq >= 0.01`: `fx = dx/sq*0.05`, `fz = dz/sq*0.05`
- Mob velocity += (fx, fz); player velocity -= (fx, fz)

This fires from each mob's tick, so every mob independently shoves the player
(and the player shoves back). Frozen `noAI` mobs are skipped.

### Hotbar slot pop animation (`Widgets.h/c` + `Screens.c` + `SurvivalTest.c`)

Original: `Inventory.addResource()` sets `popTime[slot]=5` (integer ticks);
`Inventory.tick()` decrements it once per game tick; `HUDScreen.render()` drives
a pop-and-scale per slot: `t=popTime/5` ∈ [0,1]; `sinT2=sin(t²π)`;
Y-shift = -`sinT2*8` (slots briefly jump upward); block scale *= `sinT2+1`.

Implementation:
- **`Widgets.h`** — `float slotPopTime[INVENTORY_BLOCKS_PER_HOTBAR]` added to
  `HotbarWidget`. Zero-init; set externally, decremented internally.
- **`Widgets.c` `HotbarWidget_Update`** — decrements each `slotPopTime[i]` by
  `delta*20` (20 ticks/sec matches original 1/tick), clamped ≥0.
- **`Widgets.c` `HotbarWidget_BuildEntriesMesh`** — for slots with `slotPopTime>0`:
  `t=slotPopTime/5`; `sinT2=Math_SinF(t²*π)`; `yOff=-sinT2*8*(height/22)`;
  `slotScale=scale*(sinT2+1)`. Passes adjusted float coords to `IsometricDrawer_AddBatch`.
- **`Screens.h/c`** — `HUDScreen_SetSlotPop(slot, time)` sets the pop time on the
  active HUD's hotbar widget. In `HUDScreen_Update`, if any `slotPopTime[i]>0`,
  sets `s->dirty=true` so the mesh rebuilds every frame while animating.
- **`SurvivalTest.c` `SurvivalTest_AddBlock`** — calls `HUDScreen_SetSlotPop(i,5.0f)`
  whenever a block lands in a hotbar slot (both stack-onto-existing and new-slot paths).

---

## SESSION LOG — TNT explosion item drops

User reported that blocks destroyed by a TNT explosion never drop any
items, whereas genuine Survival Test pops a scatter of items out of the
blast. Root cause: `SurvivalTest_Explode` cleared every destroyed block
straight to air via `Game_UpdateBlock`, which (unlike `InputHandler.c`'s
mining path) never raises `UserEvents.BlockChanged` - so the existing
mining-drop handler (`SurvivalTest_SpawnDropsForBlock`, wired to that
event) was never reached for exploded blocks at all.

Cross-referenced against `/tmp/good2000mo_oc`'s decompiled
`Level.explode()`, `BlockUtils.dropItems()`/`getDrop()`/`getDropCount()`,
and `PrimedTnt.java`:

- `Level.explode()` calls `BlockUtils.dropItems(block, level, x, y, z,
  0.3F)` for every destroyed block **before** clearing it to air - each
  potential item only has a 30% chance of actually spawning (vs. mining's
  implicit 100% chance). This is the "drops explode into existence"
  sparse/scattered look the user described.
- Refactored the drop-type/count mapping out of `SurvivalTest_
  SpawnDropsForBlock` into a new shared `SurvivalTest_GetBlockDrop()`
  (mirrors `getDrop()`/`getDropCount()`, returns false for water/lava/
  bookshelf/TNT - none of which yield a plain item). Mining
  (`SurvivalTest_SpawnDropsForBlock`, chance implicitly 1.0) and the new
  `SurvivalTest_ExplodeDropsForBlock` (chance 0.3 per item, via
  `Random_Float(&st_dropRng) <= 0.3f`) both call into it.
- Leaves keep their existing 1/10 sapling roll *inside*
  `SurvivalTest_GetBlockDrop` (mirrors `getDropCount()`'s own RNG call) -
  that roll is separate from, and on top of, the explosion's 0.3 chance
  gate, exactly like the Java does two independent `rand` calls.
- TNT destroyed by an explosion chain-reacts instead of dropping an item:
  `PrimedTnt.tick()`'s expiry spawns a fresh `PrimedTnt` with
  `life = rand.nextInt(life/4) + life/8` (a **partial randomized fuse**,
  5-14 ticks for the default life=40) rather than mining's full 40-tick
  fuse. `SurvivalTest_ArmTnt` gained a `fuseTicks` parameter so both
  paths (full fuse for mining, partial randomized fuse for the chain
  reaction) share the same arming code; all three call sites
  (`SurvivalTest_SpawnDropsForBlock`, `SurvivalTest_Explode`,
  `SurvivalTest_DebugSpawnTnt`) updated.
- `SurvivalTest_Explode`'s block-destruction loop now rolls the drop (or
  arms the chain-reaction fuse) immediately before clearing each block to
  air, matching `Level.explode()`'s exact order of operations.
- Did **not** touch `SurvivalTest_ExplosionImmune` - it currently also
  treats liquids as blast-immune, which genuine `BlockUtils.canExplode()`
  does not (only STONE/COBBLESTONE/BEDROCK/ORES/GOLD_BLOCK/IRON_BLOCK/
  SLAB/DOUBLE_SLAB/BRICK_BLOCK/MOSSY_COBBLESTONE/OBSIDIAN are immune).
  Flagged as a separate, related discrepancy - out of scope of this
  fix since the user only asked about missing drops.

---

## SESSION LOG — launcher "Choose mode" survival toggle

User asked for the "Survival mode" checkbox on the Launcher's Choose Mode
screen (`LScreens.c`'s `ChooseModeScreen`) to become a button like the three
mode buttons above it, plus a clearer description than the old "Hearts,
hunger, mobs and dropped items".

- `cbSurvival` (`LCheckbox`) → `btnSurvival` (`LButton`, 145x35, matching
  `btnEnhanced`/`btnClassicHax`/`btnClassic`). Click handler
  `SurvivalMode_Click` reads+flips `OPT_SURVIVAL_MODE` and relabels itself
  via `LButton_SetConst` to `"Survival: ON"`/`"Survival: OFF"` - same
  toggle-caption pattern as the F9 debug menu's `SetToggleLabels`.
- Caption was originally `"Survival mode: ON"`/`"Survival mode: OFF"` but
  that overflowed the fixed 145px button width (longer than the other
  buttons' captions, e.g. "Classic +hax" at 12 chars) - `LButton` doesn't
  auto-size to text. Shortened to `"Survival: ON"`/`"Survival: OFF"`
  (12/13 chars) to match.
- `lblSurvival` widened from 1 line to 2 (`lblSurvival[2]`) to match the
  other three buttons' two-line descriptions: "Based on Classic Survival
  Test - adds hearts, hunger, mobs, and mining".
- `CHOOSEMODE_SCREEN_MAX_WIDGETS` bumped 14→15 (net +1 widget: checkbox+1
  label → button+2 labels).
- Cosmetic/UI only - no gameplay logic touched, `OPT_SURVIVAL_MODE` plumbing
  unchanged.

---

## SESSION LOG — arrows burying into blocks

User reported arrows sink almost flush into blocks here, whereas in genuine
c0.30-s they stick out — and crucially "it depends on the angle shot at"
(side-by-side screenshot: c0.30 arrow protruding from the ground at an angle
vs ours buried nearly flush).

- Root cause: `Arrow_BoxAt` built the collision AABB with `AABB_Make`, which
  uses ClassiCube's standard *feet-at-position* convention (`Min.y = pos.y`).
  But `Entity.setPos` (Entity.java:127) centres the bb on the position on
  ALL THREE axes: `bb.y0 = y - bbHeight/2`. So the arrow's tracked position
  is the box CENTRE, and our box sat 0.25 (half of the 0.5 height) too high.
- Effect: on a downward/angled shot the arrow's position sank ~0.25 deeper
  into the ground before the box BOTTOM hit the block, so it buried nearly
  flush. A purely horizontal shot into a vertical wall was unaffected (a
  vertical box offset doesn't move the horizontal stop point) — which is
  exactly why the user saw it "depend on the angle".
- Fix: `Arrow_BoxAt` now centres the box on the position vertically by hand
  (`Min.y = pos.y - ARROW_HEIGHT*0.5f`, `Max.y = pos.y + ARROW_HEIGHT*0.5f`),
  matching the original. The renderer was already correct — it offsets
  `center.y -= 0.125` (= Java's `- heightOffset/2`) from the tracked
  position, consistent with the centred box.
- The collision/tick sweep itself (stop-before-move on `expand`+`getCubes`
  overlap, so the arrow never enters the block) was already a faithful port
  of `Arrow.tick`; only the box's vertical centring was wrong.

---

## SESSION LOG — "chestplate might be too small" audit

User audited the now-working armor render (post VB-index fix) and reported the
chestplate looks visually too small. Re-derived the box geometry from the
ground-truth decompile (`/tmp/good2000mo_oc/.../model/HumanoidModel.java` +
`ModelPart.java`'s `setBounds`) rather than guessing — **verdict: not a bug**.

- `HumanoidMob.renderModel` renders armor via `modelCache.getModel("humanoid.armor")`
  = `ModelManager`'s `new HumanoidModel(1.0F)`. `ModelPart.setBounds(x1,y1,z1,w,h,d,var7)`
  inflates every face by `var7`: `x1 -= var7; y1 -= var7; z1 -= var7;` and the max
  corner gets `+= var7` on each axis. So `var1=1.0F` is a **flat 1-pixel (1/16 block)
  inflate on every face**, nothing more.
- Checked this bit-for-bit against `Model.c`'s `armorTorso`/`armorLArm`/`armorRArm`/
  `armorHead` `BoxDesc_Bounds` values (`HumanModel_MakeParts`, ~line 1154):
  torso base dims `-4,12,-2 to 4,24,2` → bounds `-5,11,-3 to 5,25,3` is exactly
  ±1 on every face; same for both arms and the head box. The C port matches the
  Java inflate **exactly**, not approximately.
- Also confirmed `armored.rightLeg.render = false; armored.leftLeg.render = false;`
  in `HumanoidMob.renderModel` — genuine c0.30 armor never covers the legs, which
  `Model.c`'s comment already noted and `MobArmor_Draw` already respects.
- Conclusion: the genuine c0.30-s "chestplate" is just the bare torso box wrapped
  in a shell exactly **1 pixel** larger on every side — a subtle outline, not a
  bulky modern-Minecraft chestplate. The "too small" look the user is seeing is
  therefore a **faithful reproduction** of how thin the original overlay actually
  was, not a geometry/UV bug. No code change made.
- Open option (NOT implemented, needs user sign-off since it'd be non-authentic):
  could gate a chunkier inflate amount behind `SurvivalTest_Enhanced` if the user
  wants a more visually distinct chestplate, leaving classic mode byte-faithful.

---

## SESSION LOG — mob-mob pushing

### Mobs shove each other apart — AUTHENTIC c0.30, ported

User asked to close the last gameplay gap from the status audit: mob-mob
push-apart physics. Ported from the ground-truth tree (`/tmp/good2000mo_oc`),
not guessed:

- Source: `BasicAI.tick()` ends (right after `mob.travel(...)`) with
  `level.findEntities(mob, mob.bb.grow(0.2F,0,0.2F))` then `e.push(mob)` for
  every neighbour where `e.isPushable()` (`Mob.isPushable() == !removed`).
- `Entity.push(Entity)`: takes the horizontal centre-to-centre delta, normalises
  it, divides by the distance **again**, scales by `0.05`, multiplies by
  `1 - pushthrough`, then applies `-delta` to itself and `+delta` to the other —
  an equal-and-opposite shove with a soft `1/dist` falloff (so each axis term is
  `0.05*delta/dist^2`). Guarded by `sqXZDiff >= 0.01`. `pushthrough` is `0.0` for
  every mob (only `NetworkPlayer` sets it `0.8`), so the `(1-pushthrough)` factor
  is always 1 here.
- Ported as `Mob_PushApart(m)` (`SurvivalTest.c`), called in
  `SurvivalTest_TickOneMob` immediately after `Mob_Travel` (matching the
  original's travel-then-push order). Uses `Entity_GetBounds` + the grown-0.2
  AABB + `AABB_Intersects` to replicate `findEntities`, then writes the shove
  straight into both mobs' `Base.Velocity` (= Java's `xd/zd`). Because the pass
  runs from both mobs' ticks, each pair is processed twice per tick — exactly as
  the original does (every mob's `BasicAI.tick` runs its own scan).
- The `dist/dist/×0.05` chain simplifies to `delta/sq*0.05` (no sqrt needed),
  since normalise(/dist) then /dist = /dist² = /sq. Done that way for speed; the
  result is byte-identical to the Java order.
- **Debug frozen (noAI) mobs are exempted on both sides** (skipped as both pusher
  and pushee), so an F9-spawned "No-AI" inspection mob can't be nudged out of
  place by its neighbours.
- Built clean with `-Werror`; headless Xvfb smoke ran with no crash.
- **Deliberately scoped to mob-mob only.** In the original, the same loop also
  pushes the *player* (Player `extends Mob`, so `isPushable()` is true and mobs
  shove the player too). NOT ported here: that would mean a SurvivalTest mob tick
  reaching in to perturb the carefully-tuned `LocalPlayer` velocity, which is a
  different integration and risk profile than mob-on-mob. Left as a documented,
  faithful follow-up if the user wants mobs to physically jostle the player.

---

## SESSION LOG — armor render fix

### Broken armor overlay — `MobArmor_Draw` VB index bug, FIXED

User play-tested the new zombie/skeleton plate armor and reported it "seems to
be broken" (screenshot showed an armored zombie rendering wrong). Root-caused
by reading the model VB plumbing, not guessed:

- `Model_DrawPart`/`Model_DrawRotate` emit each vertex at
  `Models.Vertices[model->index]` and bump `model->index`. That index is zeroed
  **once per entity** by `Model_SetupState` (right before `model->Draw(e)`), and
  every model's single `Model_LockVB`→draw→`Model_UnlockVB` batch relies on it
  being 0 at lock time.
- `MobArmor_Draw` does a **second** `Model_LockVB(e, count)` *after*
  `HumanModel_DrawCore` already ran the body batch — which left `model->index`
  sitting at the body's vertex count (~252+). Nothing resets it between the two
  locks, so the armor parts were written **past the end** of the freshly-locked,
  much smaller (`count` ≤ 144) armor region, while `Gfx_DrawVb_IndexedTris(count)`
  drew indices `[0,count)` that were never written — i.e. uninitialised/stale GPU
  buffer contents. That garbage geometry is exactly the "broken armor" the user
  saw.
- The Sheep two-texture model (`SheepModel_Draw`) avoids this by doing body+fur
  in **one** lock (index flows 0→body→fur continuously) and drawing sub-ranges
  with `Gfx_DrawVb_IndexedTris_Range`. Armor is kept as a separate second lock
  (so it stays isolated to zombie/skeleton instead of being threaded through the
  shared `HumanModel_DrawCore`), so it just needs to restart the index.
- **Fix:** `Models.Active->index = 0;` immediately after `Model_LockVB(e, count)`
  in `MobArmor_Draw` (`Model.c`), with a comment explaining why. Armor verts now
  fill `[0,count)`, matching what gets drawn. Built clean with `-Werror`; headless
  Xvfb smoke ran with no crash. Visual correctness to be confirmed on the user's
  machine (no real display/default.zip here).
- NOTE: this bug only ever affected armored zombies/skeletons (the ~20%/20%
  rolls). Un-armored mobs never enter `MobArmor_Draw` past its early-out, which
  is why most mobs looked fine and only some looked broken.

---

## SESSION LOG — zombie/skeleton plate armor

### Mob armor (helmet + body plate) — AUTHENTIC c0.30, fully ported

**Research findings (confirmed against `/tmp/good2000mo_oc` decompiled source,
the primary ground-truth tree):**
- `Mob` → `HumanoidMob` (adds `boolean helmet, armor`, each
  `Math.random() < 0.2`, rolled ONCE in the constructor) → `Zombie extends
  HumanoidMob` → `Skeleton extends Zombie`. So **both zombies and skeletons**
  get independent ~20%/20% helmet/armor rolls; no other mob (pig/sheep/
  creeper/spider) extends `HumanoidMob`, so none of them can ever have armor.
- **Purely cosmetic — confirmed no damage-mitigation path exists anywhere.**
  Grepped `Mob.hurt()` and `NetworkPlayer.java`; the only other `armor`/
  `helmet` reference in the whole tree is `NetworkPlayer`'s
  `this.armor = this.helmet = false;` field init, never read for defense.
- Render mechanism: `HumanoidMob.renderModel` does a SECOND draw pass bound to
  `/armor/plate.png`, using a single shared cached model `"humanoid.armor"`
  (= `new HumanoidModel(1.0F)`, i.e. the same humanoid geometry with every box
  outset/inflated by 1 unit on each face — same trick as the player's
  hat/2nd-layer skin parts). It renders `head` if `helmet`, and
  `body+rightArm+leftArm` if `armor` (legs are hardcoded to NEVER render,
  in the original too). Pose is copied live from the mob's own current model
  pose each frame.
- **Faithful quirk preserved, not "fixed":** `SkeletonModel extends
  ZombieModel extends HumanoidModel`, so the cast in `renderModel` succeeds
  for skeletons too — but the armor overlay always uses the oversized/thick
  `HumanoidModel` arm geometry, never `SkeletonModel`'s own thinner arms. So
  an armored skeleton's plate overlay is visibly chunkier than its own arms
  in genuine c0.30. Ported as-is (both `human_armor*` and `skeleton_armor*`
  parts use byte-identical `BoxDesc` box dimensions).
- `/armor/plate.png` (64x32 RGBA, 742 bytes) is a real bundled c0.30 asset,
  byte-identical across all three decompiled trees and an extracted built
  jar. Embedded directly into `Model.c` as a fallback (same pattern as
  `arrows.png`/`cracks.png` in `SurvivalTest.c`), **and** added to
  `Resources.c`'s `defaultZipEntries[]` (`"classic jar files"` group, next to
  `arrows.png`/`zombie.png`/etc.) so the real asset auto-downloads from the
  genuine c0.30 client jar into `texpacks/default.zip` like every other mob
  skin — no `ClassicPatcher_SelectEntry`/`ProcessEntry` changes needed, since
  that pipeline already matches purely by basename regardless of the jar's
  internal `armor/` folder path. The embedded copy only matters before that
  resource exists (e.g. very first launch); whichever one loads first wins,
  and a texture pack can still override either via the `armor_entry`
  `TextureEntry`. A sibling `/armor/chain.png` exists in the same folders but
  is referenced by **zero** code anywhere — dead/unused planned-but-never-
  wired chainmail tier, deliberately excluded (and NOT added to
  `defaultZipEntries`).
- `arrows.png` already had this same dual-source treatment from an earlier
  session (`dceeebc`/`2e60f87`) — fixed its stale comment in
  `SurvivalTest.c` while making this change, since it still claimed "no
  texture pack ships it" despite `Resources.c` fetching the real one.

**Implementation (`src/EntityComponents.h`, `src/SurvivalTest.c`, `src/Model.c`):**
- `AnimatedComp` gained `cc_bool HasHelmet, HasArmor` — copied from the mob's
  own `hasHelmet`/`hasArmor` every render frame in `SurvivalTest_RenderMobs`,
  always false for the player/anything else.
- `struct Mob` gained `cc_bool hasHelmet, hasArmor`, rolled independently in
  `SurvivalTest_SpawnMobAt` only `if (type == MOB_TYPE_ZOMBIE ||
  type == MOB_TYPE_SKELETON)` — there's no `HumanoidMob` class in ClassiCube
  (flat `enum MobType` + `struct MobTypeInfo` table design), so the Java
  class-hierarchy check just becomes this one `if`.
- `Model.c`: added `human_armorHead/Torso/LeftArm/RightArm` parts (built in
  `HumanModel_MakeParts`, sized via `BoxDesc_Dims` + `BoxDesc_Bounds` with a
  +1 unit inflate, mirroring the existing hat/2nd-layer convention) and an
  identical set of `skeleton_armor*` parts (built in
  `SkeletonModel_MakeParts` — duplicated, not shared, because
  `BoxDesc_BuildBox` writes into whichever `struct Model` is `Models.Active`,
  and zombie/skeleton are separate `Model`s with separate vertex arrays).
  Bumped both `human_vertices[]` and `skeleton_vertices[]` sizes accordingly.
- New `MobArmor_Draw()` helper: bails immediately if neither flag is set;
  lazily decodes the embedded `plate_png[]` into `armor_texId` on first use
  (also registered as a `TextureEntry` so a real texture pack can still
  override it); computes a vertex count dynamic on which of helmet/armor are
  set (engine has no "skip draw but reserve VB slot" mechanism), does its own
  `Model_LockVB`/`Model_UnlockVB` pass (separate from the body's own, since
  armor is conditional per-instance), and binds `plate.png` for its own
  `Gfx_DrawVb_IndexedTris` call. Called from both `ZombieModel_Draw` and
  `SkeletonModel_Draw` right after their normal body draw.
- Built clean with `-Werror`; headless Xvfb smoke test ran 25s with no crash
  (exit 124 = timeout, expected — this environment has no real display, so
  this only verifies crash-safety, not the actual visual look of the armor).

---

## SESSION LOG — combat/mob fixes, render smoothing, TNT entity, inventory direction

Catch-up entry covering the work between the "stuck drops" fix (last commit that
touched this file, `3394a81`) and `0b1df4e`. All in `src/SurvivalTest.c` unless
noted, all cross-referenced to the decompiled Java, all built with `-Werror`.
Newest first.

### Mob attack arm swing — AUTHENTIC c0.30, fully ported (zombie + skeleton)
- **What it is:** the genuine c0.30 humanoid-mob melee swing. The user was right
  that "zombies have it when attacking" — it lives in `ZombieModel.setRotationAngles`
  (and `SkeletonModel extends ZombieModel`, so skeletons inherit it verbatim). This
  *fully replaces* `HumanoidModel`'s walk-cycle arm swing for these two mobs (it
  calls `super.setRotationAngles` then immediately overwrites every arm angle) with
  three independent, additive effects:
  1. **Attack chop (pitch):** driven by `Mob.attackTime`. `BasicAttackAI.attack()`
     sets `attackTime = 5` on a landed hit, `Mob.tick()` decrements it every tick,
     and `Mob.render()` feeds `grounded = (attackTime - partialTick)/5` (clamped
     >= 0) into the model. Both arms pitch together by `v1*1.2 - v2*0.4` where
     `v1 = sin(g*PI)`, `v2 = sin((1-(1-g)^2)*PI)`, `g = grounded` — a single
     up-then-down chop over 5 ticks (0.25s) right after a hit connects.
  2. **Outward yaw splay**, also `grounded`-driven: `±(0.1 - v1*0.6)` per arm — a
     small ~5.7° constant splay at rest that widens to ~28° mid-chop.
  3. **Slow always-on idle sway** in roll and pitch, using `Mob.tickCount` (ticks
     since spawn, NOT partial-tick-only): `roll = ±(cos(age*0.09)*0.05 + 0.05)`,
     `pitch += ±sin(age*0.067)*0.05`. Independent of attacking — present even
     while idle/walking.
- **How it's ported here (all three effects, not just the chop):**
  - `struct Mob` (`src/SurvivalTest.c`) gained `int attackTime;` (set to 5 on a
    landed hit in `Mob_DoAttack`, decremented every tick before the AI runs,
    matching Java's order) and `int ticksAlive;` (`Mob.tickCount`, incremented
    once per tick alongside it).
  - `AnimatedComp` (`src/EntityComponents.h`) gained `float AttackSwing;` (the
    per-frame `grounded`) and `float Age;` (`ticksAlive + partialTick`), both set
    by `SurvivalTest_RenderMobs` right before `Model_Render`, and `float LeftArmY,
    RightArmY;` (arm yaw — previously every arm draw hardcoded Y=0; the player/
    `CalcHumanAnim` path never touches these new fields, so they stay 0 there).
  - `HumanModel_DrawCore` (`src/Model.c`) now passes `e->Anim.LeftArmY`/`RightArmY`
    instead of a literal `0` to the arm `Model_DrawRotate` calls.
  - `ZombieModel_SetArmPose(e)` (`src/Model.c`) computes all three effects from
    `AttackSwing`/`Age` and writes the full `LeftArmX/Y/Z`/`RightArmX/Y/Z` sextet;
    called by both `ZombieModel_Draw` and `SkeletonModel_Draw` (the latter via a
    forward declaration, since skeleton's section comes first in the file).
  - **No-op guarantee unchanged:** at `AttackSwing == Age == 0` (never reached in
    practice for the player, since neither field is ever touched outside
    `RenderMobs`) the formula reduces to the original static `+90deg` forward
    pose with zero yaw/roll — so the player, creative-mode zombies/skeletons, and
    every other model are completely unaffected.
- **Sign/axis caveats (unverified — no display in this dev environment):**
  - **Pitch (X):** confident. ClassiCube's static pose is `+90deg` where Java's is
    `-90deg` (mirrored), so Java's `pitch -= X` consistently becomes `+= X` here
    for both the chop and the idle-pitch term.
  - **Yaw (Y) and roll→Z:** ported as a direct, unmirrored read of Java's value —
    there was no prior CC precedent for arm yaw to anchor a mirroring rule against
    (it was always 0 before this change), and the GL rotation *order* ClassiCube's
    `ROTATE_ORDER_XZY` macro applies (X then Z then Y) doesn't textually match the
    order Java's `ModelPart.render()` issues its `glRotatef` calls in (roll/Z,
    yaw/Y, pitch/X — which composes to vertex-order pitch→yaw→roll). For the small
    angles here (yaw ≤ ~28°, idle roll ≤ ~5.7°) any composition-order mismatch is a
    minor secondary skew, not a broken pose, but it's unverified. If the splay/sway
    looks wrong or fights the attack chop, the first things to try are flipping
    `LeftArmY`/`RightArmY` and/or `LeftArmZ`/`RightArmZ`'s signs in
    `ZombieModel_SetArmPose`.
- **Passive mobs (pig, sheep):** never attack at all (`MOB_AI_PASSIVE`, no
  `BasicAttackAI`) — confirmed nothing to port, no change made or needed.
- **Spiders:** DO attack (jump-attack AI) but `SpiderModel.render()` in the
  decompiled source never reads `grounded` — it only animates the legs from the
  walk cycle. The genuine client gives spiders **zero** visual attack animation,
  so ClassiCube's current spider (no swing) already matches; nothing to port.
- **Creepers:** same story — `CreeperModel` doesn't reference `grounded` (and has
  no arms regardless). `grep -l grounded` across every decompiled `*Model.java`
  matches only `Model.java` (the field declaration) and `ZombieModel.java` —
  confirming zombie+skeleton are the *only* mobs with any `grounded`-driven
  animation in genuine c0.30. Creeper/spider still set `m->attackTime` in
  `Mob_DoAttack` (harmless, just unread by their models) since that's shared code.

### Player model attack/punch swing (non-authentic cosmetic) — DISABLED, deferred
- **NOTE:** the bullet below from the previous session wrongly concluded c0.30 has
  no mob attack animation. It does (see the section just above) — that was a gap in
  ClassiCube's port, now filled. The player-side punch infra remains disabled as
  described, awaiting the future beta-humanoid-animation work.
- **Status: disabled by user request.** The trigger call
  `AnimatedComp_StartPunch(&Entities.CurPlayer->Base.Anim)` in
  `HeldBlockRenderer_ClickAnim` (`src/HeldBlockRenderer.c`) has been removed (and
  the now-unused `#include "SurvivalTest.h"` in that file removed with it), so the
  player model no longer swings its arm in third person on mine/attack/place.
  User said "we will fix it later sometime" — i.e. revisit, not abandon.
- **The shared `AnimatedComp` infrastructure is left in place, inert**, for that
  future follow-up: fields `Punching`/`PunchO`/`PunchN` in `EntityComponents.h`,
  the per-tick advance in `AnimatedComp_Update`, the render-time layering at the
  end of `AnimatedComp_GetCurrent`, and `AnimatedComp_StartPunch` itself
  (`EntityComponents.c`) are all still there and compile clean, but nothing in the
  codebase calls `AnimatedComp_StartPunch` anymore, so `PunchN` never leaves 0 and
  the GetCurrent block is permanently a no-op until something calls it again.
  Original design notes (sign convention, timing, why it was layered after
  `CalcHumanAnim`) are preserved in git history (commit `aa87aae`) for when this
  is picked back up.
- **Checked the "zombies have it when attacking" claim**: there is no separate
  attack/punch-swing mechanism for mobs anywhere in `SurvivalTest.c`. Mobs only
  ever drive `AnimatedComp` through the normal walk-cycle path (`Mob_Tick` calls
  `AnimatedComp_Update`/`AnimatedComp_GetCurrent` just like players, just the
  movement-distance-based `Swing`, not a discrete punch), and `Mob_DoAttack`
  (line ~1783) deals damage on contact with no extra animation call — it doesn't
  even call `AnimatedComp_StartPunch` (nothing does, post-removal). What likely
  looks like an "attack swing" is zombies' arms naturally swinging from the walk
  cycle as they lunge/close distance to hit the player, not a dedicated punch
  animation. Worth keeping in mind for the future redo: if a real mob punch is
  wanted too, `Mob_DoAttack`'s hit branch (where it currently just calls
  `Mob_Hurt`/damages the player) is the right place to also call
  `AnimatedComp_StartPunch(&m->Base.Anim)`.

### HUD
- **Score/Arrows labels now scale with the hotbar** (`0b1df4e`, `src/Screens.c`).
  They were `TextWidget`s rasterised at a fixed 16px font and drawn at native
  pixel height, so at large window/GUI scales they looked tiny next to the
  scaling hotbar/hearts. `HUDScreen_BuildMesh` now builds each label's quad as a
  **scaled copy** of its text texture (not the persistent widget tex — that would
  compound every frame), stretched to `hotbar.height * 8/22` — the same on-screen
  height the original HUDScreen draws its 8px font at, identical to the stack-count
  digits. Arrows is vertically centred on the heart row; margins scale too. NOTE:
  the top-left FPS/position text is stock ClassiCube (intentionally fixed-size,
  not GUI-scaled) and was left alone — revisit only if the user asks.

### Combat & damage fidelity
- **`Mob.hurt()` dual-threshold invulnerability** (`919e57b`). The old code used a
  flat 20-tick (1s) window that blocked *all* damage, so rapid click-attacks only
  landed once per second. Genuine `Mob.hurt()` instead tests against the same
  20-tick `invulnerableDuration` with two thresholds: while `invulnerableTime` is
  in the **first half** of the window a follow-up hit is ignored *unless it's
  strictly stronger* than the hit that opened the window (and then only the extra
  damage lands, `health = lastHealth - damage`); once **past the halfway point** a
  fresh full hit lands and re-arms the window. Net: equal-damage hits register
  every **10 ticks (0.5s)**, matching the real game. Added `Mob.lastHealth`, and
  moved the aggro / despawn-timer reset (`ai.hurt`) *ahead* of the window check
  since the original applies it on every hit. "Stronger" = larger raw `damage`
  int than the window's opening hit (no armor/mitigation exists in c0.30-s; damage
  is a flat per-source int — fist 4, player arrow 7, creeper headbutt 6, explosion
  `(1-d/r)*15+1`, lava 10, drown/suffocate 2, fall `floor(dist)-3`).
- **Explosion falloff + mob damage** (`1a66378`). `SurvivalTest_Explode` now uses
  the genuine `(1 - dist/radius)*15 + 1` falloff (16 point-blank → 1 at the rim)
  and damages **mobs as well as the player** (was player-only with a guessed
  `*MAXHP*0.6` curve); distance measured to each entity's bbox centre to match
  `Entity.distanceTo`.
- **Mob melee line-of-sight** (`1a66378`). `Mob_DoAttack` now does a
  `Level.clip`-equivalent raycast (`Mob_SightBlocked`) before landing a hit, so
  mobs can't hit through thin walls — closes the gap that was previously only
  documented, not implemented.
- **Air / drowning fixed + HUD** (`1a66378`). The underwater check read
  `Blocks.Collide` (which collapses water/lava to `COLLIDE_LIQUID`) instead of
  `Blocks.ExtendedCollide`, silently breaking drowning damage and the underwater
  state. Fixed, exposed via `SurvivalTest_HeadUnderwater/_AirSupply`, and added the
  depleting air-bubble HUD row (icons.png) plus live Score/Arrows `TextWidget`
  labels (replacing the old digit-atlas arrow count).
- **Lava damage 4 → 10** per half-second tick (`1864995`), matching
  `Mob.tick()`'s `hurt(null, 10)` (~20 HP/s, a full player in ~1s).

### Mob facing / AI
- **`Math_Atan2f` arguments were swapped** (`e94a908`). CC's `Math_Atan2f(x, y)`
  returns `atan2(y, x)` (first arg = cosine/x axis). The mob aiming code called it
  as `(y, x)`, so every facing computation was ~90–180° off — attack mobs faced
  *away* from the player (head at the ground, body running the wrong way) instead
  of chasing. Corrected all four sites to `Math_Atan2f(-dz, dx)` /
  `Math_Atan2f(horDist, -dy)`, matching `Vec3_GetDirVector`'s basis: `Mob_DoAttack`
  yaw+pitch, `Mob_UpdateBodyYaw`, and `SurvivalTest_CalcHurtDir`. (This supersedes
  the older "swapped atan2" note in the AUDIT PASS section — that fix had the right
  diagnosis but the wrong convention; this is the corrected one, verified in-game.)

### Dropped items
- **Pickup uses AABB overlap, not a sphere** (`0fece5d`). `Player.tick()` collects
  items via `this.bb.grow(1, 0, 1)` — an AABB widened a full block *horizontally
  but not vertically*. The port used a 1-block Euclidean sphere from the feet, too
  narrow horizontally and penalizing any vertical offset, so items resting one
  block over and up were often just outside the radius. Now an AABB-overlap test.
- **Death-drops** (`1864995`). `Player.die()` scatters one drop per non-empty
  inventory slot at the death position. Refactored `SpawnDrop` into a position-based
  core (`SpawnDropAt`) shared by block-mining and death drops.

### Render smoothing (tick-rate → frame-rate interpolation pass)
This is a recurring theme: survival entities simulate at the fixed 20 Hz tick but
render every frame, so anything that read raw tick state "stepped" visibly. The
fix pattern throughout: store a `prev*` snapshot each tick and blend `prev → cur`
by the partial-tick `t` already threaded into the render calls.
- **Arrows** (`a5979f2`): added `ArrowEntity.prevPos`, blended by `t` in
  `SurvivalTest_RenderArrows`.
- **Item pickup fly-in** (`a5979f2`, smoothed `f45281f`): ported `TakeEntityAnim`
  (eases a collected item toward the player over 3 ticks, `(time/3)^2`, before
  removing it) via `pickingUp/pickupTime/pickupFrom`; then added `DropItem.prevPos`
  so the fly-in (and all drop motion) interpolates per-frame instead of stepping
  ~3 times over its 0.15s.
- **Drop spin/bob/glow** (`9a4f880`): `DropItem_Phase` read raw `d->age` (20 Hz
  steps). Added `DropItem.prevAge`, threaded an interpolated age through
  `DropItem_Phase/_ComputeGeometry/_GlowAmount`.
- **Mob death roll** (`a5979f2`): dying mobs now roll onto their side over the
  20-tick death window (`(deathTicks/20)^2 * 800°`, capped 90°) via `e->next.rotZ`,
  picked up by the existing `Entity_LerpAngles`.

### D3D11 vertex-stride desync (`37a77c9`)
`Gfx_UnlockDynamicVb` on D3D11 implicitly rebinds the VB using the stride of
whatever format was last set via `Gfx_SetVertexFormat`. Several survival paths set
the format only *after* lock/fill/unlock, so the unlock used a stale stride left
by an earlier draw (e.g. `SelOutlineRenderer`'s `VERTEX_FORMAT_COLOURED`),
scrambling every vertex past the first — D3D11-only, since GL applies stride at
draw time. Fixed by moving `Gfx_SetVertexFormat` to immediately before each
`Gfx_LockDynamicVb` in cracks, drop items+glow, TNT glow, and arrows. **This is the
load-bearing fix that made the crack overlay / items / TNT glow / arrows actually
render correctly on D3D11**; subsequent visual fixes assume it.

### Block-breaking crack overlay darkening (`ea1a914`)
The embedded crack texture's alpha was baked as `(255 - grayscale)`, the inverse of
a plain `dst*src` multiply (white-neutral). But genuine c0.30 draws cracks with
`glBlendFunc(GL_DST_COLOR, GL_SRC_COLOR)` = `2*src*dst`, whose neutral point is
**50% grey** — so the tiles use a grey background that vanishes under the 2× blend.
Under the white-neutral inverse that grey became alpha 128, washing the whole face
half-black. Re-derived the correct black-source alpha at upload: `out = dst*(1-a)`
must equal `2*src*dst`, so `a = 2*aOld - 255` (clamped ≥ 0). Grey background → alpha
~1 (block keeps its colour); only the dark crack lines stay visible. (Root-caused by
decoding the embedded PNG bytes in Python and simulating the histogram before
touching code.)

### TNT: smoke, then a real PrimedTnt entity
- **Smoke puffs** (`c3c19a7`). Ported `SmokeParticle` as a small self-contained pool:
  one puff per fuse tick, the exact `Particle.java` velocity/grey/lifetime math,
  rendered as alpha-tested billboards off the shared `particles.png` atlas (8 smoke
  frames), greyed by world light and tick-interpolated. Added a `Particles_TexId()`
  getter to `Particle.c/.h` to reach the atlas. Smoke renders independently of live
  fuses so puffs linger briefly after the blast.
- **Physical PrimedTnt entity** (`bed39c6`). Replaced the static "block left in the
  world ticking down" approach (see the now-superseded TNT FUSE SYSTEM section
  below) with a real entity, matching `TNTPhysics.onBreak`: `ArmTnt` clears the
  block to air and spawns an entity that pops up (`yd=0.2` + the faithful Notch
  double-radians-convert drift), then `TntPhysics` runs gravity + a swept block
  collision (reusing the drops' `Collisions_MoveAndWallSlide`) + the damped landing
  bounce (keeping the *intended* velocity for the bounce as Java's `move()` does).
  Drawn as a real textured cube (per-face TNT tiles, 1D-atlas batched, uniform
  world-light brightness like `model.renderAll`), with the flash + smoke now
  tracking the moving, interpolated entity. **Defuse** moved from "mine the block
  again" (there's no block now) to the melee ray-cast: a swing landing on a lit TNT
  *closer than any mob* removes it and drops a TNT item (`PrimedTnt.isPickable/hurt`).

### Inventory screen — direction reversed (faithful = none)
A paperdoll inventory was built then deliberately gated/removed for faithful mode:
- `364f770` redesigned the inventory as an Indev/Beta-style light-grey panel with a
  3D skin **paperdoll**; `705dae2`/`1f0d371`/`eaa01b2`/`39d649f` fixed its facing
  (RotY 180 to face camera, body fixed forward + only head tracks the mouse,
  proportions/scissor, and the atan2 black-box-on-first-frame).
- `3316dfb` then gated all non-authentic extras behind a new **`SurvivalTest_Enhanced`**
  flag (`OPT_SURVIVAL_ENHANCED`, off by default; "Enhanced survival" checkbox in
  Misc options, shown only in survival). The paperdoll is the first thing it gates.
- `6f7db8a` made **faithful survival (`Enabled && !Enhanced`) open no inventory at
  all** — matching c0.30-s, which had only the fixed hotbar. (Previously it fell
  through to the creative block-grid picker, which can't move survival items.)
  Non-survival keeps the normal creative inventory; Enhanced keeps the paperdoll.

### Debug aid (temporary)
- **F9 debug menu** (`c7234e6`): `SurvivalDebugScreen` (Menus.c), survival-only —
  spawn each mob type, heal/hurt, kill all mobs, refill arrows. Explicitly *not*
  c0.30-s parity; isolated (one screen + the `SurvivalTest_Debug*` fns + one
  InputHandler hook) so it's easy to strip out later.
- **Expanded (latest session)** at user request, for testing the armor fix and
  the entity systems: now also has **Spawn drops** (a spread of stone/log/red-
  mushroom/TNT items), **Spawn TNT** (a primed fused entity in front of you),
  **Shoot arrow** (a free player arrow that doesn't spend the count), and three
  *persistent* toggles whose button captions show ON/OFF live: **Invincible**
  (`st_godMode` - blocks ALL player damage at the top of `SurvivalTest_Damage`),
  **No-AI** (`st_debugNoAI` - subsequently debug-spawned mobs stand frozen for
  inspection: no wander/chase/attack and held out of the despawn roll, but
  gravity/hurt still apply), and **Armor** (`st_debugForceArmor` - forces
  helmet+armor on every debug-spawned zombie/skeleton instead of the ~20% roll,
  so the plate overlay is easy to eyeball). The two spawn toggles affect only
  debug-menu spawns, never natural ones. `SurvivalTest_SpawnMobAt` now returns
  the `struct Mob*` so `SurvivalTest_DebugSpawnMob` can post-apply those flags;
  the F9 menu went 2 columns / 10 buttons -> 3 columns / 16 buttons.

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

> **SUPERSEDED (see SESSION LOG above, `bed39c6`/`c3c19a7`):** the "block left in
> the world ticking down in place" approach described below was replaced with a
> real physical `PrimedTnt` entity (pops up, falls/bounces, smokes, flashes,
> defused by meleeing it). The fuse timing, drop table, and explosion rules below
> are still accurate; only the "it stays a static block" rendering/representation
> changed.

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

- **Arm doesn't swing when attacking a mob (later session, FIXED)**: user
  noticed the held-item/hand stays still when hitting a mob, unlike the
  visible swing when mining a block. Ground truth: `Minecraft.onMouseClick(0)`
  (Minecraft.java:1224-1228) starts the held-block swing at the very *top* of
  the left-click handler, unconditionally, *before* it branches into
  `entity.hurt(player, 4)` (mob), `gamemode.hitBlock(...)` (block), or the
  air-miss case — so every left click swings the arm. In our port the swing
  is played by `InputHandler_DeleteBlock` (`HeldBlockRenderer_ClickAnim(true)`,
  "always play delete animations, even if we aren't deleting a block"), but
  the left-click routing is `if (!SurvivalTest_TryAttackMob()) DeleteBlock();`
  — so when a mob is hit, DeleteBlock (and the swing) is skipped entirely.
  Fix: `SurvivalTest_TryAttackMob` now calls `HeldBlockRenderer_ClickAnim(true)`
  on a successful hit, so hit-mob / hit-block / hit-air all play exactly one
  swing, matching Java. (Included `HeldBlockRenderer.h` in SurvivalTest.c.)

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

### 2. Arrows — no texture (arrows invisible) — FIXED
- ROOT CAUSE: ClassiCube's default texture pack has no `arrows.png` (Classic
  loaded it from `/item/arrows.png` in the jar; CC packs are flat and don't
  ship it). So the `arrows_entry` TextureEntry callback never fired,
  `st_arrowsTexId` stayed 0, and `SurvivalTest_RenderArrows` bailed at
  `if (!any || !st_arrowsTexId) return;` — arrows were not just untextured but
  not rendered at all.
- FIX: embedded the original 32x32 RGBA `arrows.png` (322 bytes, byte-for-byte
  from the decompiled jar) as a static array in SurvivalTest.c, plus
  `SurvivalTest_EnsureArrowTexture()` which decodes it (Stream_ReadonlyMemory
  + Png_Decode + Gfx_CreateTexture) the first time arrows render and after any
  context loss. The arrows_entry TextureEntry is kept so a custom pack can
  still override (Game_UpdateTexture frees the embedded one first). Verified
  the embedded bytes match the original and decode to the expected 32x32.

### 3. Mob models render "broken" — LIKELY FIXED (body-rotation bug; verify)
- The mob TEXTURES themselves are fine: zombie.png/skeleton.png/etc. are part
  of ClassiCube's default.zip (see textureResources[] in Resources.c) and are
  loaded via Model_RegisterTexture; our mob models use those defaultTex's
  (usesHumanSkin=false, NonHumanSkin=false), so Model_ApplyTexture binds the
  right texture. Could not render-test here (this container has no real
  default.zip and no display), but there's no texture-assignment bug in code.
- The real defect that made mobs look broken: our mobs set `e->Yaw` (head)
  but NEVER set `e->RotY` (body). `Entity_GetTransform` rotates the body by
  RotY only, and `Model_SetupState` rotates the head by `Yaw - RotY`. With
  RotY stuck at 0, every mob's body/legs were frozen facing north while the
  head swivelled and the legs walk-animated sideways relative to travel —
  exactly the "mangled / tripod" look in the screenshots.
- FIX: sync `e->RotY = e->Yaw` each tick (after the AI updates Yaw) and at
  spawn, so Classic mobs turn as a whole. **User to verify** whether mobs now
  look correct; if a specific texture is still wrong, revisit per-mob.

### 4. Mob behaviour — look down = FAITHFUL; clumping = spawn/chase + #3 fix
- "Look down" is NOT a bug: Zombie.java sets `defaultLookAngle = 30` and
  Creeper.java sets `= 45` in the decompiled c0.30 source, and BasicAI.update
  does `mob.xRot = defaultLookAngle`. So zombies/creepers genuinely tilt their
  heads down 30/45 degrees in Survival Test - the port is faithful. (In CC
  e->Pitch only rotates the head, not the body, so it's just the head tilt.)
  Left as-is intentionally.
- "Gravitate toward a point": mobs spawn in clusters (MobSpawner scatters ~9
  around a point) and hostile mobs chase the player once within 16 blocks -
  both faithful. The unnatural part was really the frozen-body bug in #3
  (fixed), which made their movement look wrong. Re-evaluate after the RotY
  fix; if they still unnaturally converge, dig into wander RNG next.

<!-- (superseded note kept for history)
- Mobs tilt their heads/bodies **down** instead of looking ahead, and they
  all **gravitate toward a single point** rather than wandering. Likely the
  AI look-target/heading is defaulting to something like origin or (0,0,0),
  and pitch isn't being clamped/zeroed. Review BasicAI/wander port in the
  Mobs section (yaw/pitch assignment + target selection per tick).
-->

### 5. Block breaking not implemented — FIXED
- Survival now uses **progressive, per-block-hardness breaking** with a
  crack overlay, ported directly from the genuine c0.30 decompile
  (`SurvivalGameMode.hitBlock(x,y,z,side)` + `Block.java`'s hardness table +
  `Minecraft.java`'s crack-overlay render, all in `/tmp/mcraft_client`). This
  also corrects the earlier "uniform break timer" note below, which turned
  out to be wrong — see that entry.
- `SurvivalTest_Hardness()` (`src/SurvivalTest.c`) ports the full per-block
  hardness table (in ticks, 20/sec): e.g. dirt 10, grass 12, stone 20,
  cobble/wood 30, log 50, ore 60, iron 100, obsidian 200, bedrock ~19980
  (unbreakable), flowers/mushrooms/saplings/TNT 0 (instant). 0-hardness
  blocks still insta-break through the old click path
  (`SurvivalTest_CanInstaBreak`); everything else only breaks through the
  new continuous per-tick system.
- `SurvivalTest_TickBreaking()` is the continuous 20Hz hits/cooldown state
  machine (mirrors `hitBlock`/`resetHits()`), hooked into `SurvivalTest_Tick`.
  Driven by `Input.Pressed[CCMOUSE_L]` + `Game_SelectedPos`, so it reuses the
  engine's existing mouse/picking state rather than needing new plumbing.
  `InputHandler_DeleteBlock`'s old 4Hz instant-delete path is now gated by
  `SurvivalTest_CanInstaBreak()` so it only fires for 0-hardness blocks in
  survival; creative is untouched.
- Reach distance corrected to 4 blocks for survival (`LocalPlayer.ReachDistance`
  in `SurvivalTest_OnNewMapLoaded`), vs creative's 5 — matches
  `SurvivalGameMode.getReachDistance()`.
- Crack overlay (`SurvivalTest_RenderCracks`) renders the 10-stage crack
  texture over whatever block is being mined, scaled 1.01x around its center
  to avoid z-fighting (matching `Minecraft.java`'s `glScalef`). The genuine
  client multiply-blends the crack tile (`glBlendFunc(GL_DST_COLOR,
  GL_SRC_COLOR)`); ClassiCube has no multiply-blend mode, so the crack
  texture was re-derived as black RGB + alpha = `(255-v)/255` (where `v` is
  the original grayscale value) — this is mathematically identical to the
  multiply blend when used with standard `Gfx_SetAlphaBlending(true)`, so no
  engine/graphics-backend changes were needed.
- ClassiCube's bundled default texture pack's `terrain.png` is only 256x128
  and has no crack tiles at all (the genuine c0.30 `terrain.png` is 256x256
  and has them at indices 240-249). Rather than depend on a texture pack
  that may not have them, the 10 crack-stage tiles were extracted from the
  genuine asset and embedded as a small standalone texture (`cracks_png[]`),
  following the same embedding pattern already used for `arrows_png`
  (lazy-decoded via `Png_Decode`, overridable by `cracks.png` in a custom
  texture pack via `TextureEntry_Register`).
- **Black-sliver artifacts outside the block — FIXED (this session).** The
  overlay quad was inflated 1.01x around the block centre (0.005-block
  overhang past every face). The genuine client uses 1.01 too, but its
  multiply blend makes the overhang invisible; our black+alpha approximation
  rendered that overhang as dark slivers against the air/adjacent blocks.
  Reduced to 1.002 (0.001-block overhang) — still enough to win the depth
  test against the block face without z-fighting (cracks only ever draw on
  the block you're right next to), but the overhang is no longer visible.
- **Crash fix (NPOT cracks texture)**: the embedded crack strip is 160×16
  (10 stages × 16px). 160 isn't a power of two, so `Gfx_CreateTexture` aborts
  on backends that reject non-power-of-two textures ("Textures must have power
  of two dimensions" — hit on D3D11 the instant a block started cracking, and
  also when meleeing a mob, since holding left-click cracks the block behind
  it). Fixed in `SurvivalTest_EnsureCracksTexture` by padding the decoded
  bitmap out to a 256-wide power-of-two texture (transparent filler on the
  right); the crack UVs now address the real 160px via per-stage pixel maths
  (`stage*16/256`) instead of `stage/10` over the full width.

### 6. Drop tables wrong — FIXED
- Found the genuine c0.30 client's `level/tile/` package in `/tmp/mcraft_client`
  (Block.java + every Block subclass: StoneBlock, OreBlock, WoodBlock,
  LeavesBlock, GrassBlock, SlabBlock, BookshelfBlock, TNTBlock, LiquidBlock,
  etc.) - this is the actual `getDrop()`/`getDropCount()` override table, not
  a guess. Cross-checked every finding against the Wiki (Java_Edition_Survival_Test)
  before changing anything; both sources agreed in every case.
- **Real bugs found in `SurvivalTest_SpawnDropsForBlock`** (an earlier session
  had wrongly "corrected" these away, thinking they were guesses - they were not):
  - `BLOCK_STONE` and `BLOCK_OBSIDIAN` were dropping themselves. Both should
    drop **cobblestone** - `StoneBlock.getDrop()` always returns
    `COBBLESTONE.id`, and Obsidian is literally constructed as
    `new StoneBlock(49, 37)`, so it goes through the exact same override.
    (Wiki confirms: "breaking stone/obsidian yields cobblestone".)
  - `BLOCK_COAL_ORE`/`BLOCK_GOLD_ORE`/`BLOCK_IRON_ORE` were dropping
    themselves. `OreBlock.getDrop()`: gold ore -> gold block, iron ore ->
    iron block (no separate ingot item exists yet), and **coal ore -> a
    stone SLAB** (not coal - there's no coal item either). This slab quirk
    is genuinely correct, not a misread: Wiki explicitly confirms "stone
    slabs were obtained by mining coal ore" in Survival Test.
    `getDropCount()` is `random.nextInt(3)+1` = **1-3** for all three ores.
  - `BLOCK_DOUBLE_SLAB` was dropping itself; should drop a single `SLAB`
    (`SlabBlock.getDrop()` always returns `SLAB.id` regardless of instance).
  - `BLOCK_BOOKSHELF` was dropping itself; `BookshelfBlock.getDropCount()==0`
    - it should drop nothing.
  - `BLOCK_WATER`/`STILL_WATER`/`LAVA`/`STILL_LAVA` had no case (would fall
    through to "drop itself" if ever minable) - `LiquidBlock` overrides
    `dropItems()`/`onBreak()` to no-ops, `getDropCount()==0`. Added an
    explicit no-drop case for safety even though these likely aren't
    minable through normal play.
  - Everything else genuinely does drop itself by default (`Block.getDrop()`
    returns `this.id`) - grass->dirt, leaves->sapling 1/10, logs->3-5 planks,
    and TNT's no-drop-arms-a-fuse were already correct from earlier sessions.
- **Mob death drops** - found a second real bug while reading `Mob.java`'s
  subclasses (`/tmp/mcraft_client/.../mob/`): **`Sheep.die()` is byte-for-byte
  identical to `Pig.die()`** - both drop 1-2 brown mushrooms
  (`(int)(rand+rand+1.0)`). The old notes/code had concluded "Sheep has no
  drop" from a different decompile pass that apparently missed this override.
  Wiki corroborates directly: "pigs and sheep would drop mushrooms, which was
  the only food item at the time". Fixed: `Mob_Die` now calls the (renamed)
  `Mob_SpawnMushroomDrops` for both `MOB_TYPE_PIG` and `MOB_TYPE_SHEEP`.
- **New mechanic found and ported**: `Sheep.hurt()` - a **player punch**
  (not an arrow/other source) against a still-furred sheep **shears** it
  instead of dealing any damage: drops 1-3 white wool, clears `hasFur`, and
  returns before the normal damage/knockback/invincibility logic runs at all.
  Subsequent punches (once `hasFur` is false) behave as normal combat.
  Added a `hasFur` field to `struct Mob` (spawned `true`, irrelevant for
  non-sheep) and a special-cased branch at the top of `Mob_Hurt` that checks
  `attacker == &Entities.CurPlayer->Base` (matches Java's
  `attacker instanceof Player` - excludes arrows, which use a separate local
  `fakeAttacker` struct, not the real player entity pointer).
  - **Not ported** (deliberate simplification, out of scope for a drop-table
    fix): wool **regrowth** is tied to a full sheep-specific grazing AI
    (`Sheep$1.update()` in the decompiled source) that replaces the normal
    wander behaviour entirely - the sheep detects grass beneath it, "eats"
    it (converts to dirt) over 60 ticks, with a 1-in-5 chance to regrow fur
    on completion. Porting that is a real AI feature, not a drop-table
    correction, so sheared sheep currently stay sheared forever. Worth a
    follow-up if the user wants full fidelity there.
- Verified via `gcc -fsyntax-only` and a full `make PLAT=linux -j$(nproc)`
  build - zero errors/warnings. Not yet confirmed in a running game this
  session (no display available) - worth a test pass: mine stone/obsidian/
  each ore type/a double slab/a bookshelf, and punch a sheep once vs. twice.

### 7. Launcher — redundant survival toggle + cut-off Back button — FIXED
- DONE (this session). Removed the "Survival mode" checkbox from the Settings
  screen, keeping only the one on the Choose Mode screen (user confirmed
  "keep Choose Mode only"). Reverted `SettingsScreen`'s struct field,
  `SETTINGS_SCREEN_MAX_WIDGETS` (10→9), `set_btnBack` (y 210→170) and the
  layout list back to their pre-aa377d5 state — which also fixes the Back
  button being cut off in windowed mode (it was only cut off because the
  4th checkbox had pushed it down). `SurvivalMode_Changed` stays (still used
  by the Choose Mode checkbox). Verified with a clean build.

### 8. Mob AI/texture fidelity pass (this session) — skeleton transparency FIXED, body-yaw decoupling FIXED, spider bobbing FIXED, look-angle + knockback CONFIRMED already correct
User reported from a screenshot: skeletons looked "stiff" with heads cocked
down, mobs didn't seem to bob much while walking, and asked about
knockback-on-hurt fidelity. Also reported separately: skeletons have no
transparency and their texture alignment looks "lightly fucked up".

- **Skeleton transparency — FIXED, real bug.** `SurvivalTest_RenderMobs`
  (the function that draws every mob model) never enabled alpha testing.
  Compare `Entities_RenderModels` (`Entity.c`) which wraps its entity-model
  loop in `Gfx_SetAlphaTest(true)` / `(false)` - mob rendering had no such
  wrapper. Worse, `SurvivalTest_RenderDrops` (called right before
  `SurvivalTest_RenderMobs` every frame, in `Render3DFrame`) explicitly
  leaves alpha test **disabled** after its own draw calls, so mobs were
  reliably drawn with alpha test off. Skeleton's model has real cutout
  regions (gaps between its thin 2px arms/legs and the torso), so those
  regions rendered as solid texture garbage instead of being clipped.
  Fix: `SurvivalTest_RenderMobs` now does `Gfx_SetAlphaTest(true)` before its
  loop and `(false)` after, matching the real entity path.
- **Skeleton "texture alignment lightly fucked up" — investigated, found NOT
  a UV/box bug.** Read the genuine decompiled `SkeletonModel.java` (extends
  `ZombieModel` extends `HumanoidModel`) and compared every box size/texture
  origin against `SkeletonModel_MakeParts` in `Model.c`: head 8x8x8 @ (0,0),
  torso 8x12x4 @ (16,16), legs 2x12x2 @ (0,16), arms 2x12x2 @ (40,16) - all
  match exactly, byte-for-byte. The model geometry was never wrong. Strong
  suspicion (not separately provable without a display) is that this
  was the *same* alpha-test bug above: with cutout regions rendering as
  solid garbage instead of transparent, the silhouette looks like the
  texture doesn't line up with the model. **User: please re-check after the
  alpha-test fix** - if it still looks misaligned once transparency works,
  it's a separate issue and worth a fresh look with an actual screenshot.
- **Mob look-angle context-switching — confirmed ALREADY correct, no change
  needed.** Was worried `Mob_DoAttack` might never override pitch, but it
  already does: `Mob_BasicAIUpdate` sets the idle/wander tilt
  (`e->Pitch = info->defaultLookAngle`) every tick first, then
  `Mob_DoAttack` (called right after, only for non-passive mobs) overwrites
  both `e->Yaw` and `e->Pitch` with a real look-at-target calculation once
  `m->hasTarget` is set - faithfully porting `BasicAttackAI.doAttack()`.
  Skeleton's own `defaultLookAngle` is correctly `0` (Skeleton's AI is a
  fresh `Skeleton$1 extends BasicAttackAI` that never sets it, and `AI`'s
  base default is `0`) - so an idle/non-aggroed skeleton looks straight
  ahead, not down; only Zombie (30) and Creeper (45) tilt down while idle.
  If skeletons still look like they're staring down at the player while
  approaching, that's very likely just `Mob_DoAttack`'s look-at-target pitch
  pointing slightly downward because of head-height/eye-height differences
  versus the player's eye position - which is correct per the source, not a
  bug.
- **Body/head yaw decoupling — FIXED, real bug (refines #3's earlier
  simpler fix).** The #3 fix above (`e->RotY = e->Yaw` every tick) made the
  body stop being frozen, but it also made the *entire* model (body, legs,
  AND head) snap instantly to face the target every tick, since
  `Model_SetupState`'s headDelta (`Yaw - RotY`) was always exactly 0 - i.e.
  no actual head/body decoupling ever happened. The real
  `Mob.java tick()` keeps `yBodyRot` as separate persistent state that:
  (a) eases toward the actual movement direction (`atan2(dz,dx)-90`) at
  `+= delta*0.1`/tick rather than snapping, and (b) is independently
  clamped to stay within +-75 degrees of wherever the head (`yRot`) is
  currently looking. This is what makes a real c0.30 mob's head swivel
  ahead to track the player while the body/legs visibly catch up a moment
  later, instead of rigidly snapping. Ported as `Mob_UpdateBodyYaw`
  (`SurvivalTest.c`), called after `Mob_Travel` each tick using the real
  position delta for the movement-direction target; `e->RotY` is reused
  directly as the persistent `yBodyRot` state (nothing else needs Entity's
  RotY semantics for mobs).
- **Body-yaw convention bug — FIXED (this session, live-test feedback).**
  `Mob_UpdateBodyYaw` computed its body-target yaw with Java's `yRot`
  formula `atan2(dz,dx)-90`, but `e->Yaw`/`e->RotY` are in ClassiCube's
  convention (`atan2(dx,-dz)`, matching `Mob_DoAttack` and
  `Vec3_GetDirVector`). Those two conventions are 180 apart, so the body
  eased toward the **opposite** of the travel direction and got clamped 75
  off the head — a mob that was actually walking toward the player rendered
  with its body/legs facing away, reading as "won't chase / runs away".
  Java doesn't hit this because its head `yRot` uses the *same* convention
  as the body target; our port mixed the two. Fixed to `atan2(dx,-dz)` so
  the body target matches the head/movement convention. NOTE: the chase
  *velocity* was always correct (it's driven by `e->Yaw` via
  `Mob_MoveRelative`); this was purely the visible model orientation.
- **Walking bob — confirmed ALREADY implemented generically, one real bug
  found (spiders).** ClassiCube's model system already has a universal
  walk-bob: `model->bobbing` defaults to `true` for every `Model`
  (`Model_Init`), and `Model_GetEntityTransform` adds
  `e->Anim.BobbingModel` (`= |cos(WalkTime)| * Swing * 4/16`, driven by
  actual distance moved via `AnimatedComp_Update`) to the model's Y
  position - this already runs for mobs since `SurvivalTest_RenderMobs`
  calls `AnimatedComp_GetCurrent`/`Model_Render` same as real entities. So
  the "mobs should bob a lot while walking" behaviour was already faithful
  and working for every mob *except* spiders: `Spider.java` is the only mob
  that sets `bobStrength = 0.0F` (explicitly no bob), but
  `SpiderModel_Register` (`Model.c`) never overrode the `bobbing` default of
  `true`. Fixed: `spider_model.bobbing = false` now set in
  `SpiderModel_Register`.
- **Knockback-on-hurt — confirmed ALREADY correct, no change needed.**
  Compared `Mob_Hurt`'s knockback math against the real `Mob.knockback()`
  line-by-line: `xd/=2; xd -= dx/dist*0.4; zd/=2; zd -= dz/dist*0.4;
  yd/=2; yd += 0.4; if (yd>0.4) yd=0.4;` - our existing code already does
  exactly this (halve current velocity, then push away from the attacker
  on X/Z and up on Y, capped at 0.4). No discrepancy found.
- **Mob movement jitter ("looked like lower fps") — FIXED (this session,
  live-test feedback).** Root cause: mobs were the only entity-like things
  in the game with no prev/next double-buffered position/orientation.
  `SurvivalTest_TickOneMob` mutated `Base.Position`/`Yaw`/`Pitch`/`RotY`
  directly once per game tick (default 20/sec), and `SurvivalTest_RenderMobs`
  rendered straight from those fields every render frame with no
  interpolation - so a mob's visible position only changed 20 times a
  second no matter the framerate, while everything else in the game (the
  local player via `LocalPlayer_SetInterpPosition`, and `NetPlayer` via
  `NetPlayer_RenderModel`) blends `Base.prev`/`Base.next` by the partial-tick
  `t` every frame for buttery movement between ticks. Not a client/engine
  limitation - the engine already has exactly the machinery needed
  (`Entity.prev`/`next`, `Entity_LerpAngles`, `Vec3_Lerp`), mobs just never
  hooked into it. Fixed by giving mobs the same treatment as `NetPlayer`:
  `SurvivalTest_TickOneMob` now starts each tick with
  `e->prev = e->next; e->Position = e->prev.pos;` (plus yaw/pitch/rotY),
  runs AI/movement exactly as before, then ends by snapshotting the result
  into `e->next`. `SurvivalTest_RenderMobs` now calls
  `Vec3_Lerp(&e->Position, &e->prev.pos, &e->next.pos, t)` and
  `Entity_LerpAngles(e, t)` before `Model_Render`, mirroring
  `NetPlayer_RenderModel` exactly. `SurvivalTest_SpawnMobAt` also seeds
  `prev`/`next` to the spawn position/yaw/rotY so a freshly-spawned mob's
  first tick interpolates from its real spawn point instead of warping in
  from a zeroed-out `prev`. All other per-tick readers of mob `Base.Position`
  (AI distance checks, arrow-hit checks, despawn roll, fall-damage tracking)
  are unaffected since they all run during `SurvivalTest_TickMobs` /
  `SurvivalTest_TickArrows` (which runs right after `TickMobs` in
  `SurvivalTest_Tick`), by which point every mob's `Base.Position` for that
  tick has already been reset to its fresh, fully-resolved value - only the
  *render-frame* reads (between ticks) ever see the interpolated value.
- Verified via `gcc -fsyntax-only` and a full `make PLAT=linux -j$(nproc)`
  build (`Model.c` + `SurvivalTest.c`) - zero errors/warnings. Not render-
  tested here (no display in this container) - **user: please re-check
  skeleton transparency/alignment, and watch for the head-leads/body-catches-
  up effect on an approaching zombie/skeleton.**

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
  (see `Mob.noActionTime`). Mob-mob push-apart physics: **implemented** (see the
  "mob-mob pushing" session-log entry at the top).
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

### Block-breaking time — CORRECTED (the "CLARIFIED" note below was wrong)
The note that used to be here (attributed to a prior user statement, 2026-06)
claimed c0.30-s used a single uniform break duration for every block, with no
per-block hardness and no crack overlay, and that those were later (Indev-era)
additions. **This is incorrect** — direct inspection of the genuine decompiled
c0.30 source (`/tmp/mcraft_client/.../level/tile/Block.java`,
`SurvivalGameMode.java`, `Minecraft.java`) this session shows c0.30-s already had:
- **Per-block hardness**, set in `Block.java`'s static init (e.g. dirt 10 ticks,
  stone 20, cobble 30, obsidian 200, bedrock effectively unbreakable) — not a
  uniform timer.
- A real **10-stage crack overlay**, rendered in `Minecraft.java` (multiply-blended,
  texture indices 240-249), driven by `SurvivalGameMode.hitBlock(x,y,z,side)`'s
  hits/hardness state machine.

This has now been implemented faithfully per the genuine source — see "5. Block
breaking" above. Flagging this correction explicitly since it reverses something
previously written down as user-clarified; the decompiled source is unambiguous
on this point across all three independent decompiles checked.

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
  **Inventory SCREEN (updated, see SESSION LOG):** faithful survival
  (`Enabled && !Enhanced`) opens **no inventory screen** — hotbar only, matching
  c0.30-s. The Indev/Beta-style 3D **paperdoll** storage screen (`SurvivalInvScreen`)
  is now gated behind the `SurvivalTest_Enhanced` toggle (`OPT_SURVIVAL_ENHANCED`,
  off by default; "Enhanced survival" checkbox in Misc options). Non-survival uses
  the normal creative inventory. (The earlier "solid dark panel / click to pick-swap"
  description is obsolete.)
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
- **HUD fullscreen/DPI scaling**: the hearts and arrow count previously sized
  themselves with the bare `Gui_GetHotbarScale()`, which has DPI factored *out*
  (`GetWindowScale` divides by `DisplayInfo.ScaleX/Y`); the hotbar widget then
  multiplies it back in (`scaleY = hotbarScale * DisplayInfo.ScaleY`). So on a
  HiDPI display in fullscreen the hearts/arrow count rendered smaller than the
  hotbar they sit on. Both now use `Gui_GetHotbarScale() * DisplayInfo.ScaleY`
  to match the hotbar's true on-screen scale (a no-op when `ScaleY == 1`, i.e.
  ordinary non-HiDPI displays, so existing setups are unchanged). The stack
  counts were already correct here since they derive from `w->height`/
  `w->slotWidth`, which already bake in DPI + GUI scale and reflow on every
  resize / fullscreen toggle (`HUDScreen_Layout` → `LayoutHotbar` →
  `Widget_Layout` → `HotbarWidget_Reposition`).
- **HUD stack counts** (`HUDScreen_BuildCountsMesh`) — re-audited & RE-FIXED
  against the genuine `HUDScreen.java`. The original draws counts with the
  8px-tall GUI font inside its fixed 240-unit-tall virtual screen, where the
  hotbar is 22 units tall and each slot cell is 20 wide, right-aligning the
  count to the cell's right edge (`var26 + 19`) with its top 10 units above the
  hotbar's bottom (`slotY + 6`). Our `HotbarWidget` bakes that exact scale into
  its pixels (`height = 22 * hotbarScale * ScaleY`, `slotWidth = 20 * hotbarScale
  * ScaleX`), so the faithful digit height is `w->height * 8/22` and the right
  edge is `w->x + slotWidth*(i+1)`, top `(w->y+w->height) - w->height*10/22`.
  The previous code was wrong on both axes: it sized digits as `slotWidth*0.34`
  (= `6.8*scaleX`, ~15% too small *and* tied to the X scale, so they came out
  the wrong size and stretched on non-square DPI), and applied a fabricated
  `slotWidth*0.1` inset that shoved the text up and to the left, detaching it
  from the cell edge. Now tied to the hotbar's own height (which carries the
  same 22-unit scale the original font lives in), so the digits track the
  hotbar at any GUI scale / fullscreen / DPI. (Digits are still rasterised from
  CC's TrueType atlas rather than the bitmap font, and have no drop shadow — a
  possible future fidelity touch, but it'd need the counts vertex budget
  doubled.)
- **Dropped item physics ("stuck in blocks", reported this session) — FIXED.**
  Checked against the genuine `Item.java`/`Entity.java`: confirmed c0.30-s has
  **no pickup magnetism at all** — `playerTouch()` only fires from `Entity.move()`'s
  plain AABB-touch test, there's no pull-toward-player anywhere in the original.
  The actual bug was in `SurvivalTest_DropPhysics` (`SurvivalTest.c`): X/Z position
  was updated every tick with **zero horizontal collision** — the only "collision"
  was `SurvivalTest_DropGroundY` re-checking a single block directly below the
  *new* (x,z) column every tick. So a drop drifting sideways into a taller
  neighbouring block wasn't stopped by it like a wall; instead it got vertically
  warped straight up onto that block's top the instant the ground check passed —
  looking like it clipped into terrain, and landing elevated just enough that the
  1-block pickup radius mostly got eaten by the vertical offset, forcing the
  player to stand almost on top of it. Fixed by replacing the single-block
  vertical-only check with a real swept-AABB collision: a throwaway scratch
  `struct Entity` (`Position`/`Size`/`Velocity`/`OnGround` only) run through the
  same `Collisions_MoveAndWallSlide` the player and mobs already use
  (`Mob_TravelGround`'s exact apply-velocity → collide → `Vec3_AddBy` pattern),
  with `StepSize = 0` to match `Item.java`'s `footSize` (defaults to 0 — genuine
  items get no auto step-up and stop dead against obstacles, never climb them).
  `d->velocity` is a blocks/sec rate (pre-existing design, unlike mobs' native
  blocks/tick), so it's scaled by `delta` into a displacement going into the
  collision call and back out of it afterwards; `SurvivalTest_DropGroundY` was
  removed (subsumed by the real collision). Ground damping (`Item.tick()`'s
  `xd*=0.7; zd*=0.7` while `onGround`) now keys off the collision's own
  `OnGround` flag (new `struct DropItem.onGround` field) instead of the deleted
  single-block check. The 1-block-radius pickup test itself (`Euclidean distSq`,
  more forgiving than genuine's plain touch-test) was left as-is — it wasn't the
  root cause, and already approximates the "doesn't need to be pixel-perfect"
  feel the user expected without inventing actual magnetism.
- **Damage**: fall (peak-tracking, `floor(dist)-3`, ~1 HP/block past 3 safe blocks),
  lava (4 HP / 0.5s), drowning (2 HP/s after 15s air), 0.5s invincibility frames.
- **Damage tilt**: every successful hit briefly rolls the camera up to 14°, eased via
  `sin(t^4*pi)` over a fixed 10-tick window (ported from `Renderer.hurtEffect` -
  always a flat 10 ticks regardless of damage dealt, never scales). Rolls away from
  the hit direction for melee/arrow hits (`SurvivalTest_HurtFrom`, attacker position
  known); random left/right for environmental damage - fall/lava/drowning/poison/
  explosion (`SurvivalTest_Hurt`, no attacker, matching the original's
  `hurt(null, damage)` call sites). Applied directly to the view matrix in
  `Render3DFrame` (`SurvivalTest_ApplyHurtTilt`), right after `Camera.Active->GetView`,
  using the same `t` partial-tick fraction other survival renderers already get.
  The original's separate death-only "keel over" roll (up to 40°, grows with
  `deathTime`) was **not** ported - `GameOverScreen` sets `blocksWorld = true` and
  takes over the instant health hits 0, so the 3D scene (and thus any camera roll)
  stops rendering at the same moment, making it permanently invisible in this engine.
- **Mushrooms**: right-click to eat — brown +5 HP, red −3 HP poison (`SurvivalTest_TryEat`).
- **Death**: faithful **"Game over!"** screen (permadeath, no respawn) with
  "Generate new level..." and "Quit game". `GameOverScreen` in `src/Screens.c`.
  Matches the decompiled `GameOverScreen.java`: title rendered at 2x font size
  (32 vs the usual 16, mirroring `glScalef(2,2,2)`), background is the genuine
  dark-red-to-maroon fading gradient (`PackedCol_Make(80,0,0,96)` top to
  `(128,48,48,160)` bottom — decoded from the original's literal
  `drawFadingBox(.., 1615855616, -1602211792)` ARGB ints, not a neutral gray
  like earlier), and shows **"Score: {points}"** below the title in place of
  a fabricated "You ran out of health" line that was never in the original.
  "Quit game" is a ClassiCube-specific stand-in for the original's
  session-gated "Load level.." button (no login-session concept here).
- **Score**: `Player.score`/`awardKillScore` ported as `st_score`
  (`SurvivalTest_Score()`). Credited only on player-attributable kills —
  direct melee always credits, arrow kills credit iff `ArrowEntity.ownerIsPlayer`
  (mirrors `Arrow.awardKillScore` forwarding to its `owner`), and all
  environmental/self-damage `Mob_Hurt` calls (drowning, lava, fall, creeper
  self-damage) never credit, matching `die(Entity)`'s `attacker != null` gate.
  Per-kill values from `Mob.deathScore`/`Pig.die()`/`Sheep.die()`: zombie 80,
  skeleton 120, creeper 200, spider 105, pig/sheep 10 (the latter two bypass
  the `deathScore` field in the original in favour of a hardcoded flat-10
  `awardKillScore` call, but the net point value is identical either way).
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

## SESSION LOG — doll armor culling fix + fire interaction batch

### Paperdoll armor partially invisible — face culling, FIXED (2b4122a)
Armor pieces rendered broken ONLY on the GUI paperdoll (helmet rim only,
torso missing, one leg bare) while the identical armor drew perfectly in
the world view (proved by gdb-equipping a diamond set and comparing views).
Root cause: the doll pass enabled `Gfx_SetFaceCulling(true)`, but ClassiCube
draws entity models with culling OFF everywhere (Model.c only enables it
for sprite billboards), so model box geometry carries NO winding guarantee —
mirrored parts built from corner-swapped bounds (armor limbs, the skin's
left leg) wind backwards and culling silently eats their faces. Fix: leave
culling off for the doll like every other model draw. Rig-verified: full
diamond set renders on the doll.
- Lesson recorded: any state the engine never exercises against model
  geometry (winding, two-sidedness) must not be enabled in custom passes.

### Sun/moon missing on the user's machine (Direct3D?) — diagnostic added
Code audit found no GL-only calls in `IndevTest_RenderSky`; D3D9 implements
`Gfx_SetAlphaBlendingAdditive` with the same (SRCALPHA, ONE) formula as GL.
Textures are the likelier failure: sun.png/moon.png only exist inside
default.zip (resource fetcher packs them from the beta jar) — a CUSTOM
texture pack never has them and the sky quads silently skip. Added a
one-time chat warning when either texture is missing. Discriminator for
the user: stars visible at night = pass runs, textures missing; nothing
at all = pass broken on D3D (escalate).

### Fire interaction batch (task 38) — genuine BlockFire click/ambience layer
All from in-20100223 decompiled source:
- **Fire is unpickable** (`BlockFire.isCollidable()` = false → the ray in
  `World.rayTraceBlocks` skips it): `Game_CanPick` now returns false for
  fire in Indev mode. No selection wireframe, can't be mined/punched, the
  pick ray hits whatever is behind the flames. Placing a block into the
  fire cell is also blocked (genuine ItemBlock only places into id 0).
- **Punch-extinguish** (`World.extinguishFire`, called by Minecraft.clickMouse
  on every left click that lands on a block, BEFORE the dig): step one cell
  out of the clicked face; if it holds fire, play "random.fizz" (vol 0.5,
  pitch 2.6 + (r−r)*0.8) and remove it. The click still punches the block
  afterwards. Hooked at the top of `InputHandler_DeleteBlock`.
- **Flint & steel now plays "fire.ignite"** (vol 1.0, pitch r*0.4 + 0.8) when
  it actually places fire, per ItemFlintAndSteel.onItemUse.
- **randomDisplayUpdates port** (`IndevTest_RandomDisplayTicks`, 20 Hz from
  SurvivalTest_Tick): 1000 random cells in the ±16 cube around the player,
  dispatching the visual-only randomDisplayTick of whatever it lands on:
  - fire: 1/24 chance of the ambient "fire.fire" crackle (vol 1+r, pitch
    r*0.7+0.3) + "largesmoke" plumes — 2 per burnable neighbour face
    (hugging that face) when not sitting on solid/burnable ground, else
    3 pouring out the top (y + 0.5 + r*0.5).
  - torches (standing + wall metas): 1 smoke wisp + 1 flame fleck at the
    head (+0.7 y, ±0.27 toward the wall for hanging metas, +0.22 y).
  - lit furnaces: 1 smoke + 1 flame licking out of the front face at a
    random mouth height (x/z ±0.52, y + r*6/16, lateral r*0.6−0.3).
  Separate client-side RNG — zero effect on the parity-matched world
  RNG streams.
- **Particle pool generalised** (the TNT smoke pool in SurvivalTest.c):
  slots now carry kind + scale. Three kinds, each with its genuine class
  constants: c0.30 SmokeParticle (TNT, unchanged), Indev EntitySmokeFX
  ("smoke" ×1 / "largesmoke" ×2.5: scale (r*0.5+0.5)*2 * 12/16 * mul,
  lifetime ×mul, grow-in clamp01(ageT*32), collides — blocked vertical
  motion crawls sideways ×1.1, ground friction 0.7), and Indev
  EntityFlameFX ("flame": velocity ×0.01, atlas index 48, shrinks
  1 − t²·0.5, brightness lerps fullbright→world light, noClip). Pool
  raised 96 → 192 for ambience volume.
- **Sound assets**: newsound/fire/fire.ogg (8b260108…) and ignite.ogg
  (19c729c3…) added to the fetcher as mob_fire1/mob_ignite1 (sha1s
  verified live on Mojang's CDN), MOBSND_FIRE/MOBSND_IGNITE groups.
  Existing installs auto-refetch the sounds zip (entry-count check).
- **`IndevFire_IsFire` is now gated on `IndevTest_Enabled`**: in c0.30 or
  CPE-server maps, block id 51 is whatever the server defines and must
  keep engine behaviour (Builder.c's fire renderer + SurvivalTest's fire
  contact damage previously matched bare id 51 in every mode).

### Rig-verified (Xvfb)
Fire placed on stone: burns with rising dark largesmoke, NO selection
wireframe with the crosshair inside the flames; one left-click through
the fire onto its supporting stone extinguishes it instantly (stone
intact, wireframe then lands on the stone). Fire against the spawn-house
wall spread through the planks and burnt the house down with smoke
everywhere (genuine spread pace). Torch on the floor emits rising smoke
wisps above the head. Sounds untestable headless (no OpenAL on the rig) —
constants transcribed verbatim; user to verify crackle/fizz/ignite by ear.

### Known gaps / follow-ups
- Fluid randomDisplayTick ambience (water splash edges, lava pops + the
  dead-code liquid.lava/water sound roll) left for the polish batch.
- Genuine clickMouse re-fires every 250 ms while the button is held;
  our extinguish fires on each press event only (idempotent — re-press
  for fire that spawned after the click).
- Indev smoke collision approximated by point-solidity per axis (genuine
  moveEntity sweeps a 0.2-wide AABB); visually indistinguishable at puff
  sizes.

---

## SESSION LOG — sky blend fix + polish batch (equip-dip, splashes, chest rules)

### Sun/moon invisible on the user's machine - SOLVED, was never Direct3D
User confirmed: stars render, sun/moon don't, textures present in their
CUSTOM texture pack. That isolated it to the textured draws - and the
genuine source held the answer: renderSky draws sun/moon with
glBlendFunc(GL_ONE, GL_ONE) (alpha IGNORED), while our port used the
engine's Gfx_SetAlphaBlendingAdditive = (SRC_ALPHA, ONE) on every
backend. A pack whose sun.png has a real alpha channel gets multiplied
into invisibility; the stock beta-jar textures are alpha-255 everywhere,
which is why the rig (default pack) always showed it. Stars were immune
(drawn with vertex alpha 255). Fix: force alpha 255 on the sun/moon
bitmaps at texture load (CelestialPngProcess), making the two formulas
identical for these textures on any backend/pack - keeps the engine's
additive blend untouched for its other users (TNT flash, fire overlay).
Also fixed the moon quad UVs: genuine mirrors U ONLY (u=1 at -x, v=0 at
-z); we flipped both, so the moon was vertically mirrored. (02d09cb)

### Polish batch (user-ordered phase 3)
- **Equip-dip** (ItemRenderer.updateEquippedItem): switching the selected
  stack dips the held item 0.6 down and back at 0.4/tick; the OLD item
  keeps rendering until progress < 0.1, then the hand swaps. 20Hz
  fixed-step accumulator + render interpolation in HeldBlockRenderer;
  keyed on (hotbar index << 16 | slot id) standing in for genuine's
  ItemStack identity. Engine's sine switch anim suppressed in Indev
  (c0.30 keeps it). Rig-verified frame sequence. (64315c4)
- **Water-entry splash** (Entity.onEntityUpdate): player, mobs and drops
  splash on the air->water edge - random.splash volume
  sqrt(vx^2*0.2 + vy^2 + vz^2*0.2)*0.2 (wading whispers, dives are
  loud), pitch 1 +- 0.4, plus 1+width*20 bubbles AND as many droplets
  at floor(feet)+1. wasInWater latches per entity, init true = genuine
  isFirstUpdate suppression. Drop velocities are per-second - scaled
  /20 for the genuine per-tick volume formula. Rig-verified via gdb
  pool state after a teleport-drop into a pond. (64315c4)
- **New particle kinds** (all genuine constants): EntityBubbleFX (texIdx
  32, rises 0.002/tick, x0.85 damping, dies leaving water),
  EntitySplashFX (EntityRainFX with 0.04 gravity, texIdx 17, half-dies
  on landing, dies in liquid/solid), EntityLavaFX (texIdx 49,
  fullbright, 1-t^2 shrink, smoke trail while rand > age/life, 0.03
  gravity). (64315c4)
- **Fluid randomDisplayTick** joins the ambience driver: lava under open
  air spits an ember 1/100 at y+0.91 (the fluid's real maxY); water at
  an exposed ledge edge (liquidAirCheck: side not solid/liquid, below-
  side solid or liquid) throws 4 droplets off each open face at +-2/16
  outside the block. The liquid.lava/liquid.water sound roll in genuine
  is DEAD CODE (nextInt(128) == -1 never true) - faithfully omitted.
- **Chest placement rules** (BlockChest.canPlaceBlockAt): a chest may
  touch at most one other chest and never one that's already paired -
  doubles are the cap, triples/L-shapes refuse. Hooked as
  IndevTest_CanPlaceBlockAt in InputHandler_PlaceBlock. The lid rule
  now also checks above the OTHER half of a double (genuine
  blockActivated), and uses isBlockNormalCube (a chest stacked on a
  chest seals it - genuine, chests are opaque cubes). Rig-verified:
  third-in-row refused, lid-blocked double won't open.
- **Third-person held items: faithfully SKIPPED** - in-20100223's
  RenderPlayer has NO held-item rendering (only armor passes +
  drawFirstPersonHand); held-on-model arrived in later versions.
  Implementing it would be a deviation.
- **Per-play pitch variance: audit says already complete** - engine
  Sounds_Play randomizes every dig/step play (pitch / (rand*0.2+0.9),
  volume / (rand*0.4+1), survival-gated) from an earlier session, and
  every mob/entity call site carries its genuine per-call jitter.

### Known gaps / follow-ups
- Large chest (task 40): adjacent chests must open as the genuine
  54-slot InventoryLargeChest (-X/-Z half first). TEs already store
  27 slots each and genuine pairs only at open time, so .mclevel is
  unaffected - the work is the 6-row container GUI + slot-space
  plumbing (19 SURVIVAL_CONTAINER_* uses in Screens.c).
- random.splash / fire sounds inaudible on the headless rig - user to
  verify by ear (sounds zip auto-refetches, ~46 KB total).

### HUD armor bar (user request, same polish phase)
GuiIngame's armor row: 10 icons on the hearts row, flush with the
hotbar's RIGHT edge, filling right-to-left - icon i covers protection
points 2i+1/2i+2 (full below the armor value, half at it, empty above),
visible only while armor is worn, and never shaking with the low-health
hearts (genuine adds the heart jitter after the armor draw). The value
is the wear-weighted InventoryPlayer.getPlayerArmorValue - the exact
function absorption already used (now exported as
SurvivalTest_PlayerArmorValue), so icons drain as pieces wear down.

Two extra finds while implementing:
- **The classic jar's icons.png has the armor sprites MIRRORED** vs the
  Indev/beta sheet: classic row 9 is (16,9) full .. (34,9) empty, while
  Indev/b1.7.3 (which the genuine HUD coordinates expect) is (16,9)
  empty .. (34,9) full. The resource fetcher now PNG-decodes icons.png
  and the beta patcher overwrites the 27x9 armor strip from the beta
  jar's icons.png, so the stock default.zip renders genuinely. (Match
  by FILENAME, not path - the jar stores it at gui/icons.png.)
  Existing default.zip installs keep the mirrored sprites until
  re-fetched (no new entry names = no auto-refetch); Indev-style custom
  packs are already correct.
- **Latent hearts-mesh overflow fixed**: SURVIVAL_HEARTS_MAX_VERTICES
  was 80 (20 quads), but the invuln glow can draw 10 backgrounds + 10
  ghosts + 10 filled = 30 quads, silently overrunning into the counts
  region. Now 160 (40 quads, armor row included).

Rig-verified: fresh diamond set = 10 full icons; set worn to ~47%
durability (armorValue 11 via gdb) = 5 full from the right + 1 half +
4 empty, matching the genuine right-to-left fill. Refetched default.zip
confirmed to carry the beta armor strip.

---

## Round 3: genuine-model ports (dig time, arrows, environment ticks)

One spec at a time (session-limit workflow): extract the genuine model
from the Java, then port + rig-verify + commit before the next.

### Indev dig-time model (commit f326483)
Block.blockStrength is a PER-TICK float progress accumulator, not the
c0.30 integer hit counter:
- `IndevTest_StrVsBlock` (replaces the MiningSpeed dig-sound proxy):
  genuine blocksEffectiveAgainst id lists - pickaxe {4,43,44,1,48,15,42,
  16,41,14,56}, axe {5,47,17,54 - Block.crate IS the chest}, spade
  {2,3,12,13}; (tier+1)*2 on match (gold = tier 0 = wood speed), sword
  flat 1.5f vs everything, hoes/others 1.0f. Chest/furnace directional
  variants fold to canonical first. Workbench/brick/obsidian/furnace in
  NO list = genuine quirk (75s/105s digs).
- `Indev_BlockStrength`: bedrock -> 0 (genuine -1 sentinel; our uint16
  table can't store it), hardness 0 -> instant, non-harvestable ->
  1/hardness/100 with NO tool speed and NO penalties, else
  strVsBlock (/5 head-in-water via the eye-column sample, /5 airborne)
  / hardness / 30. Table stores genuine seconds*20, so hardness =
  st_hardness/20.0f (exact for every table value).
- TickBreaking splits by mode: Indev st_breakDamage += strength, break
  >= 1.0; c0.30 restored to pure hits++/hardness+1. blockHitWait 5 both.
- BreakProgress feeds the crack overlay the raw 0..1 damage in Indev.
gdb-verified per-tick values exact: stone bare hand 0.0066667
(1/1.5/100), iron pick 0.133333 (6/1.5/30), obsidian w/o diamond pick
0.001, leaves 0.166667; bare-hand planks broke in the genuine 3s.

### Indev arrow physics (commit e5f082f)
Full EntityArrow port behind IndevTest_Enabled (c0.30 byte-kept):
- setArrowHeading: normalize raw aim, +gaussian*0.0075*spreadFactor per
  velocity axis (player bow speed 1.5/spread 1.0; skeleton 0.6/12.0
  fed the raw unnormalized dx,dy+lob,dz), scale by speed, NO renorm.
  New Box-Muller ST_NextGaussian on st_arrowRng (distribution parity -
  sequence parity impossible vs a time-seeded Marsaglia polar).
- Flight order: move FIRST, then drag 0.99 (0.8 when the centre block
  is water) and flat 0.03 gravity (no 1/force scale). Facing lags the
  velocity by the genuine 0.2 lerp (vector-space stand-in).
- Flat damage 4 for every Indev arrow; landed hit plays random.drr;
  ABSORBED hit (invuln window / armor zero-round) bounces at -0.1x with
  ticksInAir reset. Mob_Hurt + SurvivalTest_Damage now return cc_bool
  "landed" (attackEntityFrom's boolean) - public Hurt/HurtFrom wrappers
  stay void.
- Stick records xTile/yTile/zTile + inTile + remnant motion; mining the
  block re-loosens with the nextFloat()*0.2 kick the same tick;
  arrowShake=7 decays 1/tick and gates pickup. Owner grace airTicks>=5.
  Entity intercept grows the TARGET box 0.3/side.
- Player bow applies the ctor hand offset (0.16 sideways, 0.1 down).
rig: spawn |v| = 1.509, one tick = *0.99 - 0.03 exact, stick recorded.

### Environment tick batch (commit e941a27)
- **setBlockWithNotify hook**: Game_UpdateBlock -> IndevTest_BlockUpdated
  on every mutation (depth-capped 8). Centralizes: torch support pops,
  crop pops (stage 7 -> 1 wheat via Indev_PopCrop), farmland cover
  revert (now immediate like onNeighborBlockChange), fire lifecycle,
  chest scatter/TE removal (same-container-kind swaps skip - furnace
  lit/unlit + facing rotation keep the TE). UserEvents handler keeps
  only player-intent logic. IndevFire's explicit Fire_Schedule-after-set
  calls removed (the hook schedules onBlockAdded now).
- **Trampling**: IndevTest_TrampleStep (1-in-4 -> dirt) on step events -
  mobs from their walkDist trigger, player from a new 0.6x horizontal
  accumulator (foot block at feetY-0.2, not onGround-gated, skipped
  flying/noclip). Sneaking does NOT prevent it (no such mechanic).
- **Crops canBlockStay**: checkFlowerChange first on every random tick:
  (light>=8 || light>=4+sky) && farmland below, else pop.
- **Torch placement**: CanPlaceBlockAt refuses no-support torches before
  placement (nothing consumed/played); place-then-pop arm deleted.
- **Night brightness + easing**: sun/shadow through the genuine
  lightBrightnessTable curve ((1-v)/(3v+1)*0.95+0.05 - night 0.129 not
  linear 0.267) via public IndevTest_BrightnessOfLight; sky level eases
  1 step/tick (11-step dawn/dusk fade, gradual /time catch-up); crops/
  zombie-burn/spawn reads use the eased value like getBlockLightValue.
- **c0.30 random ticks**: Physics_TickRandomBlocksC030 = Level.tick's
  volume/200 loop (randId*3+1013904223 LCG) for c0.30 survival; the
  engine 3-per-chunk loop (volume/1365, ~6.8x sparser) stays for
  creative. Both mode randIds now seeded per map (were 0 forever).
rig: curve values exact, night sun colour 0x202020, torch mid-air
refused, gdb trample -> dirt + crop popped through the hook, raw
Game_UpdateBlock support removal pops a standing torch.

### Entity polish batch (commit c9d2494)
Eight findings from the re-run spec agent, each re-verified against the
Java before porting (one agent claim was wrong and is documented):
- **EntityItem lava/fire** (Indev): health 5; isBoundingBoxBurning (fire
  + both lava ids, UNSHRUNK box) deals 1/tick - silent 5-tick death.
  The handleLavaMovement 10-damage hit NEVER fires for items: the -0.4
  Y shrink is degenerate for a 0.25-tall box (int-cast loop bounds come
  out empty - verified in World.java:697). Centre-cell lava fizz-bounce
  (motionY 0.2, x/z (r-r)*0.2, random.fizz 0.4 / 2.0+r*0.4). Rig: drop
  died in a walled lava pocket; open-pool tests are invalid (the bounce
  kicks items out, ours got bounced into pickup range).
- **pushOutOfBlocks** (Indev): centre cell FullOpaque -> pick the
  nearest open face of six, overwrite that ONE axis velocity with
  (rand*0.2+0.1) blocks/tick (x20 per-second at the drop layer).
- **Drop spin/bob** (Indev): spin (age_ticks/20 + hoverStart) rad
  (57.3 deg/s vs c0.30's 60), bob sin(age_ticks/10 + hoverStart) - a
  THIRD of c0.30's frequency; rot0 deg folds to hoverStart rad. Sprite
  drops share the bob, never spin. Glow stays c0.30-only. Audit's
  "Indev pickup is instant" was WRONG - genuine block drops carry
  delayBeforeCanPickup=10 (already implemented); c0.30 is the instant
  one.
- **TNT render** (Indev): swell (1-(fuse-t+1)/10 clamp01)^4*0.3+1 over
  the last 10 ticks applied to cube AND flash shell (render-only; the
  defuse/pick box stays 1x1); flash only while fuse/5 % 2 == 0, alpha
  (1-(fuse-t+1)/100)*0.8; smoke spawns at y+0.5 (c0.30 keeps +0.6 and
  its own flash). gdb: swell(5,0)=1.00768, swell(0,0.5)=1.24435 exact.
  Genuine blends SRC_ALPHA/DST_ALPHA; our additive shell is the
  established stand-in.
- **Drowning** (Indev, player + mobs): genuine underflow model - --air,
  at exactly -20: 8 bubbles at (r-r) offsets around the eye + 2 damage
  + reset to 0. First hit 320 ticks after submerging, then 1/s. c0.30
  keeps every-tick-hurt + invuln shaping. st_airTicks (300; respawn 20)
  mirrors into st_airTimer for the HUD.
- **Lit furnace drops LIT (62)**: BlockFurnace has NO idDropped
  override in in-20100223 (grep-verified) - the lit->idle remap is a
  later-version myth. IndevTest_DropFormBlock (drop paths only);
  CanonicalBlock keeps folding for recipes/naming. Placed 62 rotates
  like 61 and genuinely stays lit until the furnace is actually used
  (updateBlockState only flips on burn-state CHANGE - the Indev
  "furnace lamp" trick).
- **Insta-break wear**: sendBlockRemoved runs Item.onBlockDestroyed
  unconditionally, so the discrete click path wears tools too - new
  SurvivalTest_WearHeldToolForBlockBreak before Game_ChangeBlock in
  InputHandler_DeleteBlock.
- **Mob boxes**: genuine setSize per mode in mobTypeInfo (c0.30:
  humanoids 0.6x1.8, pig 1.4x1.2, sheep 1.4x1.72, spider 1.4x0.9;
  Indev shrinks pig 0.9x0.9, sheep 0.9x1.3). Mob_ApplySize re-applied
  after EVERY Entity_SetModel (spawn + 3 shear/regrow swaps) since
  SetModel resets Size from the model's GetCollisionSize. LOS/eye
  anchors (heightOff) left as-is deliberately.

### GUI leftovers batch (commit 843508a) - round 3 complete
- **Hotbar pop curves**: IsometricDrawer_AddBatchScaled (asymmetric X/Y
  about the centre; plain AddBatch resets the statics so nothing leaks
  into TableWidget; the Flat path scales too for low-FPU builds).
  c0.30 HUDScreen: X = sin(t^2*pi)+1, Y = sin(t*pi)+1 (DIFFERENT
  curves) + the sin(t^2*pi)*8px rise. Indev GuiIngame: squash
  scaleX = 1/k, scaleY = (k+1)/2 (k = 1+t/5), pivot 4 GUI px below the
  icon centre (reproduced by shifting the centre), NO bounce. Style via
  hotbar.popSquash set in HUDScreen_SetSlotPop. Counts never scale.
- **Paperdoll**: genuine GuiInventory constants (the old "no decompiled
  source" note was stale) - feet (guiLeft+51, guiTop+75), mouse anchor
  (51,25), scale 30*texF. ALSO fixed a PRE-EXISTING 180-degree facing
  bug: the ortho x+y mirror = a Z-axis spin, which cannot turn the
  face toward the camera (facing is a Z direction) - the old comment's
  assumption was geometrically wrong; base yaw is now 180 and the
  genuine dx sign cancels the mirror's left/right flip (rig-verified
  tracking both ways). Confirmed present in the pre-change build too.
- **Container layer order** (GuiContainer.drawScreen): slot items ->
  counts -> hover highlight -> held stack -> labels LAST. The iso and
  count meshes are split (isoSlotVerts/countSlotVerts) so the cursor-
  held block + count draw as batch tails above the highlight (genuine
  z+32); labels moved to the function tail. contKind/workbench/guiTex
  hoisted to function scope. Classic flat branch untouched; paperdoll
  stays last (viewport deviation, documented).
- **Indev initial spawn**: audited conformant, no change (findSpawn
  ranges/1e6 attempts/asymmetric z-5..z+3 volume + opaque floor all
  match; sky-spawn failure fallback stays a documented deviation).

Round 3 status: all six spec domains DONE (dig-time f326483, explosion
c0812dd, arrows e5f082f, env ticks e941a27, entity polish c9d2494,
GUI leftovers 843508a).

### User-reported fixes after round 3 (commit 5d98a00)
- **Paperdoll facing is BACKEND-DEPENDENT**: D3D9/D3D11 use a reversed
  depth buffer (ZFUNC GREATEREQUAL; GL is LEQUAL) with the same ortho z
  row, so which side of the doll wins the depth test flips per backend.
  The GL-side 180-yaw fix flipped the user's D3D9 doll. Now
  DOLL_BASE_YAW is 0 on CC_BUILD_D3D9/D3D11, 180 elsewhere. Cursor
  tracking is backend-independent (same screen x/y, only occlusion
  differs). LESSON: anything that relies on depth ordering inside our
  custom GUI 3D passes must account for the reversed-Z backends.
- **OOB horizon planes are genuine**: the earlier "genuine draws no
  border walls or horizon plane" note was HALF wrong - no walls, but
  RenderGlobal.oobGroundRenderer/oobWaterRenderer draw infinite planes
  outside the map: ground at World.groundLevel (grass.png when
  groundLevel > waterLevel && defaultFluid == water, else dirt.png) and
  the fluid at waterLevel. Air-walls-only left Flat/Inland ringed by
  void with the sun visible UNDER the world near dawn/dusk (user
  report). Now IndevTest_SetSurroundings pins groundLevel/waterLevel/
  defaultFluid (IndevGen post-gen values incl. the genuine per-type
  adjustments; .mclevel Surrounding* tags on load - GroundHeight read
  SIGNED, floating maps store -128) and maps them onto the engine
  planes: dry -> grass/dirt edge plane at groundLevel + sides AIR;
  water -> still fluid at waterLevel + dirt sides skirt to groundLevel
  (approximates the submerged OOB ground plane); floating -> void.
  The .mclevel saver writes the pinned values back (the live env is
  the remapped form - deriving from Env.EdgeHeight post-apply would
  corrupt round-trips).

### User feedback round: HUD/GUI/sky fixes (commits 05b11da, d9b4287)
- **Stars through mobs**: the sky fix's depth-test-off was wrong; now the
  celestial pass keeps depth TESTING (entities occlude it) and the ENGINE
  sky ceiling stops writing depth in Indev (EnvRenderer_RenderSky,
  genuine glDepthMask(false) sky) - that's what un-hides the upper
  hemisphere. Sun re-verified overhead at noon.
- **Chat over hearts**: chat stack now anchors 10 GUI px (hearts row)
  above the hotbar in survival.
- **HUD item sprites 1 GUI px right**: genuine GuiIngame icon x is
  cell + 2 (dead centre), not +3. Most visible on coal/diamond.
- **"plumbing active" banner removed.**
- **Death screen sizing**: fonts/buttons now built in genuine GUI px x
  the survival GUI scale (title 8px-font at 2x, buttons 200x20 GUI px),
  rebuilt on scale change - offsets used to scale while widgets stayed
  tiny. Matches the user's genuine Indev reference screenshot.
- **Stale death FOV**: loading a world from Game Over kept the ~3x
  death zoom (projection never rebuilt after st_isDead cleared);
  SurvivalTest_ResetState now calls Camera_UpdateProjection.
- **Paperdoll saga concluded** (313b004 + 2549d0c): CC_BUILD_D3D9 is NOT
  a macro - backend checks must compare CC_GFX_BACKEND against
  CC_GFX_BACKEND_D3D9/D3D11. Reversed-depth backends need the view Z
  negated (yaw swaps leave a mirror); yaw tracking sign is global (the
  x-mirror inverts it everywhere); scene tilt sign follows DOLL_Z.

---

## SESSION LOG — networking client handshake foundation

### Survival multiplayer client scaffold (client-first reference for MCGalaxy)
The planning phase (`doc/networking-plan.md`, §0–§31) is done; this lays the
FIRST client-side stone so the server session can build its handshake to match
ours. Scope is deliberately **minimal foundation** — negotiate the extension,
receive/log the handshake, expose the send path — with **no simulation
mode-flip** (server-authoritative ownership handover is deferred to the
integrated server session where sim/entity/inventory handoff is designed as one
piece).

New files:
- `src/SurvivalNet.h` — the wire contract. `#define SURVNET_CHANNEL 0xB0`,
  `enum SurvNetMsg` (server→client 0x01–0x50, client→server 0x80–0x87),
  `SurvivalNet_Component`, `SurvivalNet_Send`. Matches `doc/networking-plan.md`
  §25 byte layouts.
- `src/SurvivalNet.c` — receive dispatch + send wrapper. Gated on
  `!Server.IsSinglePlayer && Server.SupportsSurvival` (`SurvivalNet_Active`);
  switches on `data[0]`; SURV_HELLO / SURV_WORLDINFO parse+chat-log stubs.
  `SurvivalNet_Send` = thin wrapper over `CPE_SendPluginMessage(0xB0, ...)`,
  no-op unless active. Registers on `NetEvents.PluginMessageReceived` in Init.

CPE negotiation (the gate that keeps Classic untouched):
- `src/Protocol.c`: added `survival_Ext = { "SurvivalTest", 1 }`, appended
  `&survival_Ext` to `cpe_clientExtensions[]`, and set
  `Server.SupportsSurvival = true` in the ExtEntry handler (alongside
  `notifyAction_Ext`). We advertise the ext; it only goes live if the server
  echoes it back.
- `src/Server.h`: added `cc_bool SupportsSurvival;` to the Server struct.
- `src/Game.c`: `Game_AddComponent(&SurvivalNet_Component);` + include.

**Why this is safe for Classic:** a stock Classic/CPE server never sends
ExtEntry for "SurvivalTest", so `SupportsSurvival` stays false, so both the
receive handler and `SurvivalNet_Send` early-out — zero behavior change. In
singleplayer `IsSinglePlayer` is true, which also fails the gate. Verified: the
whole game builds+links clean; `nm` confirms `SurvivalNet_Component`,
`SurvivalNet_Send`, `SurvivalNet_OnPluginMessage`, and `survival_Ext` are in the
binary.

**Deferred (next / server session):** SURV_HELLO mode-flip into a
server-authoritative sim, and handlers for the remaining server→client messages
(mobs 0x10–0x13, inventory 0x20–0x25, drops 0x30–0x32, blockmeta 0x40, equip
0x50) + client→server intent senders. All ids are already reserved in the enum.

## SESSION LOG — Indev creative mode (Option A: blocks-only picker)

### Non-genuine convenience creative for the Indev gamemode
A user-requested Beta-1.8-style creative: a scrolling block-picker GUI you click
to deposit stacks, plus creative behaviors (no damage, instant no-drop building
with infinite blocks, no survival HUD, flight). **Explicitly non-genuine** —
genuine Indev's last build shipped creative *disabled* (private, never-constructed
`PlayerControllerCreative`; only `instanceof` checks + commented-out keybinds
remain — see `/tmp/indev_eagler`). Kept strictly off the faithful survival path.

**Toggle & resolver (the structural decision):**
- `OPT_INDEV_CREATIVE` + `SurvivalTest_Creative` global, read in `SurvivalTest_Init`.
- `SurvivalTest_CreativeActive()` = `SurvivalTest_Creative && IndevTest_Enabled`.
  **All** creative checks route through this, never the raw flag. In MP the
  server will set the effective state from `SURV_HELLO` and downstream code needs
  no change (SP toggle vs server-dictated — doc/networking-plan §14).
- In-game **Misc options → "Indev creative"** bool toggle (mirrors "Enhanced
  survival"; `MenuOptions.c`), `Menu_Remove`'d unless the gamemode is Indev.
  Applies on next map load (hacks/reach set in `OnNewMapLoaded`).

**Behaviors (all gated on `SurvivalTest_CreativeActive()`):**
- No damage: early-out in `SurvivalTest_Damage` (the `survivalWorld=false`
  equivalent — player takes none; combat is inert).
- Instant break: `SurvivalTest_CanInstaBreak` returns true for every block.
- No drops on break + no consume on place (infinite blocks): both guarded in
  `SurvivalTest_BlockChanged`.
- No survival HUD: hearts+armor builder (`HUDScreen_BuildHeartsMesh`) and the
  air-bubbles builder early-out. Score/arrows already Indev-hidden.
- Flight/speed + 5-block creative reach in `SurvivalTest_OnNewMapLoaded`
  (overrides the survival fly-off/reach-4 defaults).

**The picker (reuses the stock scrolling block table — zero new scroll code):**
- The survival-inventory bind (default **I**) routes through `SurvivalInvScreen_Show`,
  which now opens the stock `InventoryScreen` block grid when creative is active.
- `InventoryScreen` gained a `creative` flag (set from `CreativeActive()` in Show).
  In creative, a committed cell click calls `SurvivalTest_CreativeGive(block)`
  (deposits one full `ST_MaxStack` stack via `AddItem`) and **keeps the picker
  open** (Beta-style) instead of selecting-and-closing; clicking off the grid
  still closes. Same branch added to the Enter-key path.

**Rig-verified (Xvfb):** creative ON → no hearts/armor/air HUD, empty hotbar,
picker opens on I with its scrollbar, clicking deposits a 99 stack into the next
free slot and stays open, hover shows block names. Creative OFF → 10 hearts
return (faithful survival untouched). Full `make PLAT=linux` builds+links clean.

**Deferred:** Option B (items in the picker, not just blocks); MP server-dictated
creative (the resolver is already shaped for it).

## SESSION LOG — creative made faithful + Indev block-ID cleanup

### Fully-faithful Indev creative (replaces the bare block-picker)
User steered creative back toward faithfulness (the Beta-1.8 tabbed picker was
reverted). Genuine Indev creative (`PlayerControllerCreative`) had **no** item
picker - it filled the 9 hotbar slots from `Session.registeredBlocksList` and
opened the normal `GuiInventory`. Matched that exactly:
- **Inventory GUI**: creative now opens the Indev-textured `SurvivalInvScreen`
  (armor/paperdoll/2x2 craft/storage/hotbar), same as survival - not the stock
  floating ClassiCube block grid. (`SurvivalInvScreen_Show`: dropped the
  `|| CreativeActive()` bare-grid route; the now-dead `InventoryScreen.creative`
  deposit hooks were removed.)
- **Palette hotbar**: `SurvivalTest_CreativeFillPalette` fills each empty hotbar
  slot on creative map-load with the genuine list - stone, cobblestone, brick,
  dirt, planks, log, leaves, torch, slab. Creative never depletes them, so it's
  a genuine infinite palette. Gated on `SurvivalTest_CreativeActive()`.
- Rig-verified (Xvfb): hotbar shows the 9 genuine palette blocks, `I` opens the
  textured Indev inventory panel (not the bare grid), no survival HUD.

### Indev block-ID faithfulness (user-flagged pillar/crate)
Genuine Indev ends at block 62. Nine ClassiCube CPE default blocks were leaking
into Indev at ids 52,53,55,57,59,60,63,64,65 (sandstone/snow/extra wools/ice/
pillar/crate/stone brick). `IndevBlocks_Define` now hides all nine (CanPlace=
false + `Inventory_Remove`), leaving only genuine Indev blocks. Indev-only, so
c0.30-s and plain creative keep their full block sets. gdb-verified. Full detail
+ the crops(59)/farmland(60) internal-id relocation are in AUDIT_FINDINGS.md #20.

### Fidelity note: flight is the ONE invented bit
Everything creative does maps to the dead `PlayerControllerCreative` - no damage
(`survivalWorld=false`), no HUD (`shouldDrawHUD()=false`), instant no-drop build,
9-slot palette hotbar (`onRespawn`), mobs still spawn (`onUpdate` spawner) - AND
reach 5 is genuine too (base `getBlockReachDistance`, SP overrides to 4). The
sole non-genuine addition is **flight/speed**: Indev had no fly anywhere
(grep-confirmed across the player + controller code). Kept by user choice as a
deliberate convenience; `SurvivalTest_CreativeUpdateHacks` is where fly/speed are
granted, so removing them later is a one-function change.

### Creative-never-bleeds-into-survival audit (user-requested)
Every creative behavior routes through `SurvivalTest_CreativeActive()` =
`Creative && IndevTest_Enabled`. So the toggle is inert in c0.30-s (IndevTest
off) and in Indev *survival* (Creative off). Verified gate sites: damage
(`SurvivalTest_Damage`), instant break (`CanInstaBreak`), no drops / no consume
(`BlockChanged`), flight+reach+palette (`OnNewMapLoaded`), no HUD (hearts/air
builders). The block-hiding lives in `IndevBlocks_Define` (Indev-only, applies
to Indev survival too - correct, since those blocks aren't genuine in either).
No creative code path is reachable from c0.30-s.

## SESSION LOG — genuine blocks 52/53/55/57 (definitions; textures follow)

Implemented the four genuine Indev blocks that were leaking through as ClassiCube
CPE defaults, at their genuine ids (`IndevBlocks_Define`), gdb-verified:
- **52 water / 53 lava source** (`BlockSource`): render like the still fluid,
  hardness 0, and refill their 4 horizontal air neighbours with flowing fluid
  each random tick (`Indev_TickSource` → `Physics.OnRandomTick`).
- **55 gears/cog**: flat 1px walk-through decorative (Collide none, Draw transp),
  hardness 0.5, drops self.
- **57 diamond block**: opaque cube, hardness 5.0, drops self.

No recipes (in-20100223's CraftingManager has none for these — creative/technical
blocks). `BlockTo/FromIndev` map all four 1:1; the hidden list is trimmed to
59/60/63/64/65. Also confirmed the deobfuscator's "crate" = the **chest (54)**, so
ClassiCube's block-64 crate really is non-genuine (correctly still hidden).

**Textures pending** (user: "blocks now, textures follow"): diamond block + gears
use tile 119 as a temporary stand-in — their genuine tiles (teal diamond tex 40,
gears tex 62) are Indev-only, not in the patcher's b1.7.3 source, so they need
embedding from Indev's `terrain.png` (kz_sea_png-style). Sources reuse the live
water/lava tiles (already correct). Full detail: AUDIT_FINDINGS.md #21.

**Crops/farmland** stay at 85-92 / 83-84 — a hard engine limit (no runtime block
metadata, so the 8 crop stages can't share one id and there's no free 8-run at
genuine 59). The `.mclevel` save/load already encodes them as genuine 59/60 +
metadata, so this is invisible outside the runtime ids.

## ENGINE NOTES (useful pointers)

### Indev block metadata vs our id-multiplexing (why crops/farmland use many ids)
Genuine Indev keeps TWO byte-arrays per world (`World.java`): `blocks[]` (the id)
and `data[]` (one companion byte per block), where `data` is split into nibbles:
low 4 bits = **light** (`data & 15`), high 4 bits = **metadata** (`getBlockMetadata`
returns `data >>> 4 & 15`; `setBlockMetadata` writes `(data & 15) + (val << 4)`).
So every block carries a 4-bit metadata (0-15). Uses: farmland moisture (0 dry /
1-7 wet), chest+furnace facing (2-5), crops growth (0-7 → all 8 stages are ONE id
59), torch direction (1-4 wall / 5 floor), fire age (0-15).

**ClassiCube has NO per-position metadata array** — `struct _WorldData` (`World.h`)
is just `Blocks` (+ `Blocks2` = the *upper 8 bits of the id* for >255-block worlds,
NOT metadata). Each position stores only an id. So we can't pack `id+meta`. Two
workarounds:
- **A distinct id per rendered state** (id space is cheap — EXTENDED_BLOCKS gives
  up to 65535): crops 85-92, farmland 83/84, chest facing 71-74, furnace 75-82,
  wall torch 94-97. One genuine `(id, nibble)` → one of our ids.
- **A side per-position store** for dynamic metadata that would need too many ids:
  fire age via `IndevTest_BlockDataMetaAt` (keyed by block index).

**The `.mclevel` boundary rebuilds Indev's exact format** so saves stay byte-genuine:
save = `IndevTest_BlockToIndev` + `IndevTest_BlockDataMeta` (our id → genuine id +
high-nibble); load = `IndevTest_BlockFromIndev` (genuine id + nibble → our id). We
do NOT round-trip the light nibble — ClassiCube recomputes lighting on load.

Adding a REAL metadata array would be an engine-core rework (every id-keyed
consumer — the chunk builder's `Block_Tex`/draw paths, `Physics.OnRandomTick[]`,
`Blocks.MinBB[]`/collide/pick — assumes id→fixed), for no visible or save-format
gain over the multi-id approach. Not worth it; multi-id is the idiomatic solution.

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

## SESSION LOG - Floating-world void death (fall through the map)

User report + screenshot: on a floating world you "die too close to the islands"
when you fall - it doesn't look genuine. Position readout showed (x, **0**, z):
the player was standing on an invisible floor right under the islands, not
falling into the open void.

### Root cause
Two engine facts collided:
- Genuine `World.getBlockId` CLAMPS y<0 -> y=0 (World.java). On a floating map
  the Assembling pass hollows y=0/y=1 to air, so "below the world" reads air ->
  you fall through into a bottomless void. (Normal/hell maps have solid or lava
  at y=0, so the clamp still gives a floor / lava sink there.)
- ClassiCube's `World_GetPhysicsBlock` returns `BLOCK_BEDROCK` for ANY y<0 - a
  global "you can't fall out of the map" convention. So on our floating maps you
  dropped a couple blocks and landed on invisible bedrock at y=0, took the
  genuine fall damage on that landing, and died right under the islands.

Also confirmed while investigating: genuine in-20100223 has NO explicit void
death opcode anywhere (Entity/EntityLiving/EntityPlayer/World all checked). A
genuine floating map is simply bottomless - you fall forever. The "you die when
you fall through" the user (correctly) observed is fall damage on our bedrock
floor. So the FEATURE is real/desired; the mechanism just needed to move from
"invisible floor next to the islands" to "open void + a deep hard kill."

### Fix
- **`World_FallThroughFloor`** (new global, `World.c`/`.h`): when set, y<0 reads
  `World_GetBlock(x, 0, z)` - the genuine getBlockId clamp - instead of bedrock.
  Reset to false in `World_Reset` and at the top of `World_SetNewMap`; the Indev
  component sets it true in `OnNewMapLoaded`. So classic/c0.30 maps keep the
  bedrock floor (mode purity), and non-floating Indev maps are unchanged (their
  y=0 is solid, so the clamp still returns a floor). Floating maps become
  genuinely bottomless.
- **`ST_VOID_KILL_Y = -64`** (`SurvivalTest.c` tick): Indev-gated. Fall below it
  and it's a hard kill (force Health=0 -> Game Over + drop inventory), exempt
  while flying/noclip. Creative can't take damage, so instead it snap-teleports
  back to spawn (LocationUpdate) to avoid an endless fall. This is the ONE
  non-genuine bit (genuine = fall forever), added because our engine can't
  express an infinite drop cleanly and the user wants the void to be fatal.
- **Mob void cleanup** (`SurvivalTest_TickOneMob`): a mob that wanders off a
  floating island past `ST_VOID_KILL_Y` gets its slot freed (`m->active=false`)
  instead of falling forever and leaking the slot.

Builds + links clean (`make PLAT=linux`). c0.30-s untouched (`IndevTest_Enabled`
false there, and the bedrock floor stays since the flag is never set).

### Follow-up: void death should land near terrain (zoom/shake visible)
User compared genuine Indev vs ours on a floating world: genuine "stops after a
while" and the death screen zooms + camera keels over; ours plunged into a deep
empty void and appeared to do neither.

Diagnosis: the FOV zoom (Camera.c:53 <- SurvivalTest_DeathFovZoom) and death
keel-over roll (Game.c:511 <- SurvivalTest_ApplyHurtTilt, st_deathTicks-driven)
are both already wired and DO fire on the void death - you just couldn't see
them because you died at y=-64 in featureless void. Genuine has no void-death
plane at all: you land on a lower floating-island LAYER (our generator ports
these - IndevGen.c:1294 layers=(height-64)/48+1) and die from fall damage near
terrain, so the animation plays against visible ground.

Fix: ST_VOID_KILL_Y -64 -> -16 (die just under the world, terrain still in view
instead of an empty-void plunge), and the kill now mirrors a genuine fatal
landing - freeze velocity (steady death cam), set the impact hurt-wobble
(st_hurtTicks), and reset st_deathTicks so the zoom/roll start clean. Common
falls still die on a lower island from fall damage (genuine); -16 is just the
backstop for falling clean through a gap.

### Fix: perpetual death camera wobble (frozen hurtTime) + floating-depth audit
User (comparing genuine Indev side-by-side): our Indev death has a "massive
wobble" where genuine's is subtle; suspected a c0.30 Survival Test effect leaking
into Indev, and separately that our floating worlds might be "lower" so you fall
deep. Ran a 7-agent workflow (genuine Indev EntityRenderer vs genuine c0.30
Renderer.hurtEffect vs our SurvivalTest_ApplyHurtTilt; genuine LevelGenerator vs
our IndevGen dimensions).

WOBBLE - confirmed real bug, NOT a leak. Genuine Indev and c0.30 camera math is
byte-for-byte identical: hurt tilt sin((hurtTime/10)^4*pi)*14deg, death keel-roll
40-8000/(deathTime+200), FOV zoom, and the same no-else STACKING (both stack the
hurt wobble on the keel-roll for the first ~10 ticks - that part is genuine).
Root cause was ours-only: SurvivalTest_Tick's `if (st_isDead) {...return;}`
branch returned BEFORE the `if (st_hurtTicks>0) st_hurtTicks--` line, which is the
only place the player's hurt timer decrements. So after death st_hurtTicks froze
at 10, GetHurtTilt never returned false, and the per-hit tilt recomputed every
frame as the render partial-tick swept 0..1 - a ~20Hz sawtooth ~12deg roll
forever, on top of the keel-roll = the "massive wobble." Genuine's
EntityLiving.onEntityUpdate keeps decrementing hurtTime while dead, so the wobble
decays over ~0.5s and only the slow keel-roll + FOV zoom remain. This affected
EVERY death (mob kills set st_hurtTicks=10 on the killing blow too), pre-existing;
the recent void-death st_hurtTicks arming just made it obvious. Fix: decrement
st_hurtTicks inside the dead branch (one line). Constants/stacking/keel-roll left
exactly as genuine - no Indev-specific change, since the two versions are identical.

DEPTH - no differential; ours matches genuine exactly. World height is the genuine
Indev preset (64 default; 256 only for the Deep shape), NOT ClassiCube cube sizes
and NOT taller/lower (Menus.c:1296-1305 -> Generator.c:90-93). Floating layer
count (height-64)/48+1 and per-layer waterLevel=height-32-layer*48 are
byte-identical (IndevGen.c:1294,1299), lowest island y~32 in both, spawn genuine.
The "fall deep" is GENUINE single-layer behavior: the default Square/Normal
floating preset is ONE island at y~32 with open void below - the stacked
catch-islands (every 48 blocks) only exist on the Deep/256 shape (5 layers). Our
only deviation, ST_VOID_KILL_Y=-16, makes the fatal fall SHORTER than genuine
(which has no void death and falls indefinitely), never deeper. No change made.

### Fix: death screen bypass via Generate-new-level -> escape
User: you can bypass the Game Over screen by clicking "Generate new level..." and
then pressing escape. Cause: GameOverScreen_OnGen (Screens.c:3810) removes the
Game Over screen and opens GenLevelScreen, whose cancel/escape routes to
Menu_SwitchPause (Menus.c:1235) -> pause menu -> "Back to game" -> live gameplay
while st_isDead is still true (dead, no screen, playable). Same for Load level.

Fix (SurvivalTest.c, st_isDead branch of SurvivalTest_Tick): death is modal - the
only exits are generating/loading a world (both clear st_isDead on map load via
ResetState) or respawning. If we ever end up dead with NO screen grabbing input
(Gui_GetInputGrab()==NULL, i.e. back in live gameplay), the screen was bypassed,
so re-assert GameOverScreen_Show(). It grabs input, so the check is a no-op while
it or any menu is already up; and it can't misfire during generation because the
new map clears st_isDead before gameplay resumes. Catch-all - also covers pressing
escape directly on the death screen. Applies to both c0.30-s and Indev survival.

## SESSION LOG - MP mode-flip: server-driven survival (client side complete)

User: implement the client's server-handling packets now so the mcgalaxy Claude
session can build against them. The fork (github.com/UmbreoClaw/mcgalaxy,
survival-support) turned out to be AHEAD of us: phases 0+2 done server-side
(HELLO/WORLDINFO/TIME/HEALTH sent, RESPAWN handled). Cloned it read-only to
/tmp/mcgalaxy_ro and matched every byte against Network/SurvivalNet.cs.

NOTE: the container was recycled mid-session (fresh clone on the default
branch, /tmp ground truths lost). Recovered via git fetch origin survival-test;
all work had been pushed. /tmp/indev_eagler + /tmp/mcraft_client need re-cloning
when next needed. libgl1-mesa-dev/libxi-dev reinstalled for linking.

### Wire-contract finding (drift caught by reading the real server)
- SURV_HEALTH score is i32 BE (server SendHealth bytes 2-5); our SS25 draft said
  i16. Client implements i32; SS25 fixed; handoff tells the server session to
  re-snapshot its reference/ copies of our docs.
- SURV_WORLDINFO v1 = single-byte heights [ground][water][fluid][theme][flags]
  [sides][edge] (matches survival-handshake.md SS5); SS25's int16 layout is the
  planned fuller revision, now marked as such.

### What landed (client)
- SurvivalNet.c: per-map activation (net_mode/net_flags; cleared OnNewMap and by
  a mode-0 HELLO = live /Survival refresh; unknown mode -> off). Appliers:
  HELLO -> SurvivalTest_NetworkModeChanged; WORLDINFO -> Indev OOB horizon
  planes; HEALTH -> SurvivalTest_ApplyNetHealth (i32 score); TIME ->
  IndevTest_SetWorldTime (server owns the CLOCK, client renders the genuine
  celestial light from it - the server's ramp byte is deliberately unused).
  SurvivalNet_ServerDriven() = capability + activation. Senders for all of
  0x80-0x87; HELD_SLOT auto-sent on HeldBlockChanged; RESPAWN + DROP_ITEM wired.
- Mode plumbing: SurvivalTest_EffectiveGamemode() = SP ? options : network.
  Both components re-derive per map (OnNewMapLoaded) AND on the HELLO flip
  (IndevTest_NetworkModeChanged -> SurvivalTest_ApplyMode/MapActivate; Indev
  first, matching component order). Registrations made unconditional at Init
  (self-guarding hooks/texture entries/data tables) so a server can flip Indev
  on at runtime; classic-visible things stay gated: IndevBlocks_Define +
  Indev_RegisterFarmTicks only run while Indev is on. Commands register/
  unregister on the transition. MP flip-off mid-map leaves block DEFINITIONS
  visually until next map (documented v1 limitation).
- MP sim handover, one predicate: local damage OFF via a central gate in
  SurvivalTest_Damage; mob spawner/ticks, drops, arrows, paintings, TNT,
  furnace tick, day/night ADVANCE (apply still runs), eating, bow/arrows, tool
  wear, container GUIs, survival inventory UI (falls back to classic picker),
  drops+consume on BlockChanged, void death, trampling, burning overlay - all
  gated. Break timing stays (presentation; server validates the SetBlock).
  Hacks: in MP CreativeUpdateHacks only sets reach (4/5); permissions belong to
  the server's HackControl. MiO_SetIndevCreative only mutates runtime in SP.
- Death UX in MP: ApplyNetHealth 0 -> death camera + Game Over with a single
  "Respawn" button (sends SURV_RESPAWN; screen stays until the authoritative
  revive); health rise while dead -> revive (GameOverScreen_Hide, new export).
  No local inventory drop (server owns drops).
- Docs: SS4.2 status box rewritten (mode-flip landed); P-list items ticked; SS25
  HEALTH i32 + WORLDINFO v1-vs-planned layouts; survival-handshake.md status,
  SS1 table (both layers implemented), SS5 HEALTH/TIME layouts, SS6.4 resolved,
  SS8 rewritten. NEW doc/server-session-handoff.md - the brief for the mcgalaxy
  Claude session (setup, what's consumed, corrections, server TODOs incl. death
  dwell + per-map time + BlockDefinitions, build order for phases 3-5).

### Verification
- Full make PLAT=linux builds + links clean; symbols present.
- Server.IsSinglePlayer ordering verified: Server_Component (Game.c:443) inits
  before IndevTest/SurvivalTest/SurvivalNet (456-458), so EffectiveGamemode is
  valid at component Init. SP path byte-identical behaviour (Effective==options).
- Stock-server safety: ActiveMode()==0 without the negotiated ext, so MP on a
  normal server = everything off (and BlockPhysics random ticks/fire were
  already SP-only).
- End-to-end vs the real fork server: PREPARED but not run - built their CLI
  (dotnet-sdk-8.0 + make cli, 0 errors) but the sandbox policy (correctly)
  declined executing an external repo's binary in this session. The handoff doc
  gives the server session the exact test to run; user can also run it locally.

### Follow-up: MP implementation PARKED at ed604b6 (user decision)
User: revert all the network code for now (a combined session with BOTH repos is
the right place to land it, where client+server can be integration-tested
together), but keep the docs. Done via `git checkout a97647d -- src/`:
- The tree is back to the pre-MP state: SurvivalNet foundation (CPE ext +
  HELLO/WORLDINFO parse/log) stays; everything else from this session
  (creative+fly, void death, wobble fix, death-screen bypass guard, ...) is
  untouched - only the ed604b6 MP code (mode flip, appliers, ServerDriven
  gates, respawn button) came out, and it remains cherry-pickable at ed604b6.
- Docs kept, with statuses adjusted to "implemented + verified, PARKED at
  ed604b6": networking-plan SS4.2 + P-list, survival-handshake status/SS1/
  SS6.4/SS8, and a prominent restore note atop server-session-handoff.md. The
  SS25 wire corrections (HEALTH score i32 BE, WORLDINFO v1 layout) are contract
  truth and stand regardless.
- Ground truths re-fetched after the container recycle and landmark-verified:
  /tmp/indev_eagler = github.com/EaglerPorts/in-20100223 (EntityRenderer.java:88
  death roll, LevelGenerator.java:356 groundLevel=-128 confirmed);
  /tmp/mcraft_client = github.com/ManiaDevelopment/MCraft-Client
  (Minecraft.java:323 "Minecraft 0.30", Renderer.hurtEffect:48, Mob.java:342
  hurtTime=hurtDuration=10 confirmed). Recorded here so future sessions can
  re-clone without hunting: EaglerPorts/in-20100223 + ManiaDevelopment/MCraft-Client.

## SESSION LOG - Genuine Indev sky lighting (flooded 0-15 channel, caves get dark)

User: how does Indev compute lighting, do caves get dark, don't we light
everywhere? Right on all counts - our fancy-lighting sky was ClassiCube's BINARY
column bit (sun vs a flat ~61% "shadow" shade), so caves only got tree-shade
dark, never the genuine near-black, and the cave mouth had a hard edge instead
of a fade. Genuine Light.java is a real flooded 0-15 channel.

### Genuine model (Light.java, verified against EaglerPorts/in-20100223)
- One 0-15 light value per block in data[]'s low nibble; heightmap-exposed cells
  are sky sources at the eased day level, flood-filling 6-way losing >=1 per
  block (>=lightOpacity; water=3). Emitters set a floor at Block.lightValue
  (lava/fire 15, torch/lit-furnace 14, brown mushroom 1). Fully-opaque=0.
- Day/night doesn't re-flood: updateDaylightCycle walks the map nudging every
  sky cell +-1 as the level eases. Render = table[L], L15->1.0, L0->0.05 (the
  (1-v)/(3v+1)*0.95+0.05 curve, World.java:1657). So caves bottom out near-black.

### Port (FancyLighting.c, Indev-gated via indevSky = IndevTest_Enabled)
- Sky reuses the LAVA nibble (genuine Indev has no tinted lava light - moved
  every emitter to the LAMP nibble at its genuine lightValue in IndevBlocks_
  Define, freeing the low nibble). Above-heightmap cells stay IMPLICIT (no
  storage; the sun palette group), so only cave light costs memory.
- CalculateChunkLightingSelf seeds each column's sky boundary cells (Indev_
  SeedSkyColumn: column-bottom + cells at/under a neighbour's heightmap) at 15
  and floods. Water attenuates the extra 2 in FlushLightQueue (opacity 3).
- Day/night = palette rebuild only, NO re-flood: effSky = flood - (15 - k),
  which is mathematically identical to genuine's incremental walk for the
  max(lamp, sky) query. InitPaletteIndev builds table[max(lamp, effSky)] * face
  shade; the skyLit (sun) group ignores the stored nibble and uses k directly.
- Block changes: OnBlockChanged diffs the classic heightmap and Indev_SkyBlock
  Changed unlights cells that lost sky + re-seeds the 5 affected columns.
- Gameplay light query rerouted: IndevTest_LightLevel -> FancyLighting_IndevLight
  (combined max of lamp + effSky), so spawn darkness rule / crop growth / grass
  decay read the genuine value, not the old binary approximation.
- Day/night tick: FancyLighting_SetIndevSky(k) before the Env sun-colour change
  (which rebuilds palettes); stopped needing the SunCol/ShadowCol SCALE to carry
  cave darkness (palette does it now) but kept it for the OOB horizon + non-fancy.

### Rig verification (gdb, live singleplayer Indev world 128x64x128, fancy mode)
Controlled sealed stone box in open air, then a 1-block roof skylight:
- SEALED interior centre + corner = 0  (pitch black - CAVES GET DARK) [PASS]
- directly under the skylight, y+1 and y+2 = 15 (full vertical sun)   [PASS]
- 1 block off the shaft = 14, diagonal corner = 13 (-1/block flood)   [PASS]
- torch bubble = 14/13/12                                             [PASS]
- surface (open sky) = 15; buried solid = 0                           [PASS]
In-world screenshot: real gradient inside the generated house (bright at the
door, dark ceiling/corners) - previously one flat shade. (An earlier synthetic
horizontal-tunnel dig read non-monotonic, but that was light leaking through
real generated terrain around the bore, not the algorithm - the sealed box is
the clean proof.)
Non-Indev untouched: indevSky=false keeps the stock sun/shadow palettes, so
c0.30-s and plain creative ClassiCube are byte-identical to before.

### Rig/env notes
- Fresh container had no default.zip -> "resources missing" dialog blocks world
  load; seeded texpacks/default.zip from misc/ps1/classicube.zip to test.
- Pre-existing latent risk observed: IndevTest_LightLevel can be reached from
  the gen thread's MobSpawner before classic_heightmap exists on some paths
  (NULL deref in ClassicLighting_GetLightHeight). Guarded reads already clamp
  coords; the FancyLighting_IndevLight path early-outs on !chunkLightingData.
  Not introduced here, but flagged for the full-fidelity audit (task #42).

### Follow-up: gen-time lighting NULL guard (audit finding #22)
The intermittent first-boot crash flagged after the sky-lighting work is fixed.
Traced it: not the common path (lighting IS allocated during World_SetNewMap's
MapLoaded event, before ApplyPostLoad's initial MobSpawner passes), but a light
query CAN reach ClassicLighting_GetLightHeight while classic_heightmap is NULL on
a broken-init boot, and that read segfaults. The rig2/rig3 "crashes" while chasing
it turned out to be a long-lived Xvfb :99 dying ("Failed to open X11 display",
empty backtrace) - a red herring; a fresh Xvfb boots clean. The real one was the
early NULL_POINTER_DEREF in GetLightHeight+57.
Fix: one guard at the deref site (Lighting.c) returning the -10 all-sky sentinel
= full daylight (genuine: light is computed by gen time, so uncomputed defaults
to lit, never accidental black). Covers every gameplay caller in both lighting
modes; the _Fast render variants are builder-only (post-alloc). gdb-verified by
forcing the NULL and confirming safe returns, plus a clean fresh generate.

## SESSION LOG - Fidelity sweep round 1 (items/crafting + player combat)

First two domain audits of the systematic sweep (task #42) returned; every
claimed divergence was re-verified against the decompiled Java before fixing.
Fixed: diamond block recipes + pickaxe effective/harvest rules (finding #23 -
and corrected #21's wrong "no diamond recipes" claim), player fire ignition
ramp fireResistance=20 (#24), soup returns bowl (#25), fire block excluded
from furnace fuel (#26). Queued: difficulty setting (#27 - current behaviour
== genuine default Normal). The audits also positively verified ~20 subsystems
as byte-exact (see AUDIT_FINDINGS Domain 6 list). Blocks + mob audits still
running; drops/HUD queued.

Process note: an errant conditional in a python edit truncated
AUDIT_FINDINGS.md to 0 bytes mid-session (open('w') before a failed write);
restored via git checkout. Notes edits are append-only heredocs from now on.

## SESSION LOG - Fidelity sweep round 2 (blocks + mob AI)

Second audit batch returned; all claims re-verified against the Java before
fixing. Fixed: genuine plant ticks (#28 - sapling 16-stage metadata growth with
a runtime World.growTrees port + side store round-tripped through .mclevel,
flower/mushroom genuine stay rules + self-drops; torch-lit flowers no longer
die), gears any-item harvest + 0.5 resistance (#29), diamond block 6.0
resistance (#30), sand/gravel falling through fire + void destruction on
floating maps (#31), animal spawn path-weight gate (#33), Indev creeper no
longer dimmed by the c0.30 damage pulse (#34), death/shear drops at the mob
position with genuine velocities (#35). QUEUED HIGH: #32 finite BlockFlowing
fluids (the classic infinite flood is the biggest remaining divergence -
dedicated session). Rig smoke test: 75s live fresh-gen Indev world with the
new plant handlers ticking, no crash (two earlier "crashes" were the flaky
Xvfb dying again - X11-display failures, not code).

## SESSION LOG - Genuine finite fluids (BlockFlowing/BlockStationary port, #32)

User asked for the queued HIGH fluids finding with the remaining budget. Done:
the full finite-fluid port now lives in IndevTest.c (see AUDIT_FINDINGS #32 for
the complete mechanics). Key discovery while decoding fluidFlowCheck: the
genuine SOURCE blocks (52/53) integrate there - a fluid body touching one
reports infinite supply and never donates, which is what makes springs work.
Our sources previously only had the updateTick side-fill approximation; they
now also fill on placement (onBlockAdded) and feed bodies through the genuine
donor mechanism. Verified live in the rig: a sealed 7x7 basin with ONE flowing
water block held exactly 1 water block after 200 ticks (the classic flood
would have filled ~49 cells instantly). Rig lesson: stale ClassiCube/Xvfb
processes + X locks from killed runs must be pkill'd + /tmp/.X*-lock cleaned
before each pass, or attaches target dead pids.

## SESSION LOG - Indev-scaled menu screens (gui-indevscale toggle)

User: match the remaining GUI scales to the Indev sizing the HUD/Game Over
screen already use, switched by the same toggle. The gap was the MENU system
(pause/options/gen/load/save/hotkeys): menus scaled by raw display DPI
(Display_ScaleX/Y) while the HUD and Game Over screen used the genuine
ScaledResolution step (GetWindowScale's Indev branch, gui-indevscale option).

Fix, centralized at the three funnels every menu goes through:
- Gui_GetIndevMenuScale() (Gui.c): the shared factor - same formula as the
  survival HUD/GameOverScreen (hotbar scale x display scale), 0 when not
  Indev / toggle off / touch UI.
- Gui_MakeTitleFont/Gui_MakeBodyFont: the genuine 8-GUI-px menu font x scale
  (menus previously used a fixed 16pt).
- ButtonWidget_Init: genuine 200x20 GUI px x scale (ClassiCube authors menu
  widths at 2x classic GUI px, so genuine = minWidth/2), and sets a new
  WIDGET_FLAG_INDEV_SCALE so Widget_SetLocation scales THOSE widgets' offsets
  by scale/2 - row spacing stays proportional to the resized buttons. HUD/
  chat/hotbar widgets don't carry the flag, so their layout is untouched.
- GuO_SetIndevScale now calls Gui_RefreshAll() so flipping the toggle rebuilds
  open screens at the new scale immediately.
Rig-verified: pause menu screenshot at the Indev step - chunky genuine-
proportioned buttons, 8px font, correct spacing, Quit/Back anchored fine.
c0.30/plain creative unaffected (factor is 0 outside Indev+toggle).
Known minor: unflagged menu TextWidget labels (options descriptions) keep DPI
offsets - cosmetic misalignment only on dense options screens; buttons and
titles read correctly.

### Follow-up: gen-level + input/label widgets on the Indev menu scale
User: what about the generate-level screens? Covered the remaining widget
types the button pass missed:
- TextInputWidget (seed/dimension boxes - a menu-only class; chat input is a
  separate widget) now sizes at the genuine scale ((width/2) x scale, 20-GUI-px
  box) and auto-flags its offsets in TextInputWidget_Add/Create.
- All menu TextWidget labels/titles in Menus.c + MenuOptions.c are flagged via
  Widget_SetIndevScaled (new tiny export) so their offsets track the scale -
  this also closes the "labels keep DPI offsets" cosmetic note from the
  previous commit. HUD/chat TextWidgets are untouched (flag is opt-in).
Rig-verified: the Indev Generate-new-level screen (GuiNewLevel port) renders
at the Indev step - title above the four cyclers, genuine button proportions,
correct alignment; the classic gen screen's inputs/labels follow too.

## SESSION LOG - Audio bugs (creeper hurt sound genuine; mining thunk added)

The batch-3 audit agents died to the session limit twice, so the two USER-
REPORTED bugs were verified by hand in the main loop instead:
- Creeper "Steve hurt sound": GENUINE. in-20100223 EntityLiving defaults hurt/
  death to "random.hurt" with null ambient; only pig/sheep override. Our mob
  sound mapping already matches exactly (finding #37, no action).
- Silent mining: REAL BUG (#38, fixed). Genuine Indev plays the block's step
  sound every 4th dig tick at quarter volume/half pitch (sendBlockRemoving);
  our TickBreaking played nothing. New Audio_PlayDigHitSound + the %4 cadence
  in the Indev breaking branch. c0.30 mining is genuinely silent (particles
  only) and stays that way.
The full audio + drops/HUD sweeps remain queued for after the quota reset
(prompts preserved; findings #37/#38 already cover the user-visible items).

## SESSION LOG - Final sweep round: audio + drops/HUD (findings #39-#45)

Both relaunched audits completed. Audio: metal blocks now sound metallic
(#39), the c0.30 sound-type table corrected (dirt=grass, silent plants,
cloth=pitched grass, metal pitch 2.0, silent placing, per-play randomization
c0.30-only, Indev steps at genuine 0.15 volume) (#40/#41), Indev music gap
600-1200s (#42), fall landing thud added (#43). Drops/HUD: four micro-fixes
(#44: bubbles flush, lava-kick order, Indev fly-in target, skeleton arrow ctor
offsets). Queued: GUI click sound needs the random.click asset (#45). The
audits also verified the entire remaining sound/drops/arrow/HUD constant space
as exact - see the Domain 8 verified list. 60s rig smoke test clean.
This completes the systematic fidelity sweep (task #42 of the original list):
all five planned domains + the audio domain audited, 20 fixes landed across
the rounds, remaining queue = difficulty option (#27), click asset (#45),
fluids rig follow-ups, and the MP/server-session work.

## 2026-07-17: Indev menu-scale regressions (user reports)

Two regressions from the `gui-indevscale` menu work, both fixed in `75265e6`:

1. **Game Over buttons missing** ("im missing my buttons :sob:"): the new
   auto-set `WIDGET_FLAG_INDEV_SCALE` in `ButtonWidget_Init` made
   `Widget_SetLocation` rescale offsets by scale/2 — but `GameOverScreen`
   already bakes its own genuine survival GUI scale into its offsets
   (`Game.Height/4 + 72*scale`), so the two buttons were double-scaled off
   the bottom of the screen while the unflagged title/score text stayed
   put. Fix: `GameOverScreen_Init` clears the flag on its buttons; the
   screen keeps its own genuine ScaledResolution layout.

2. **"Soiling.." progress bar too small**: the loading/generating screen's
   text followed the Indev 8px*scale font but the bar stayed at fixed
   `Display_Scale(200x4)`. Ported the genuine geometry from Indev's
   `LoadingScreenRenderer.setLoadingProgress`: bar is 100x2 GUI px at
   `(w/2-50, h/2+16)`, title top at `h/2-20`, message top at `h/2+4` —
   now all multiplied by `Gui_GetIndevMenuScale()`. The title/message
   widgets take the INDEV_SCALE flag path (offsets authored 2x: -32/+16).
   Gated on the toggle + Indev mode; c0.30/vanilla paths byte-identical.

## 2026-07-17: washed-out sky after .mclevel save+load (user report)

Symptom: after saving and reloading a world, the sky turns milky white and
the additive sun quad blooms enormously. Root cause: Floating maps store
`cloudHeight = -16` (clouds below the islands; `LevelGenerator.java:358`),
and genuine `LevelLoader` round-trips it as a SIGNED short. Our saver wrote
the correct two's-complement bytes, but the loader read them UNSIGNED
(`NbtTag_U16` -> 65520), so `EnvRenderer`'s sky ceiling
(`max(World.Height+2, CloudsHeight)+6`) jumped to ~y=65526 - the whole view
became the white horizon-fog gradient, over which the additive sun.png glow
saturates much further out. Fix in Formats.c: cast `(cc_int16)` like the
SurroundingGround/WaterHeight tags already do. Saved files were always
byte-correct; genuine Indev reads them fine.

## 2026-07-17: leaves shadows (user report) - lighting re-audit

Genuine Indev `Block.java:496` registers leaves with `setLightOpacity(1)`;
the heightmap top-scan stops at ANY nonzero opacity, so canopies shadow the
ground (World.java:138, Light.java updateLists). Flood attenuation through
a leaf is max(1, opacity)=1 - identical to air - so the shadow comes ONLY
from losing the direct sky injection. c0.30's `isLightBlocker()` is
`isOpaque()`, which LeavesBaseBlock overrides to false: NO leaf shadows in
Survival Test, so the fix is Indev-gated (IndevBlocks_Define).

Fix: `Blocks.BlocksLight[BLOCK_LEAVES] = true` in Indev mode, plus clear
LIGHT_FLAG_SHADES_FROM_BELOW on leaves/water so ClassicLighting's
heightmap sits AT the top blocker cell (genuine heightMap = blockerY+1 =
first fully-lit cell). The blocker cell itself is now flood-lit, never a
direct seed: top leaf = 14, water surface = 15-3 = 12 (was seeded 15 and
only paid opacity deeper down). Verified opacity table against Block.java
static init: water 3 (ours: -1 spread -2 entry = 3 total), lava 255
(BlocksLight default true), farmland/step 255, everything else nonopaque 0.

## 2026-07-17: mob hurt flash fullbright red (user report)

Neither ground truth tints the model itself. Our Mob_GetColor blended the
lit colour toward pure (255,0,0) scaled by hurtTicks/10 - fullbright red,
fading out, in BOTH modes. Genuine:

- c0.30 Mob.render: NO red at all. Hurt feedback = the sin(t^4*pi)*14 body
  roll (already ported) + the additive white pass (glColor4f(1,1,1,0.75),
  SRC_ALPHA/ONE, textured) while invulnerableTime is within 10 of a fresh
  hit. That pass is now gated !IndevTest_Enabled.
- Indev RenderLiving.java:67-82: while hurtTime > 0 OR deathTime > 0, the
  model is redrawn UNTEXTURED with glColor4f(brightness, 0, 0, 0.4F) and
  SRC_ALPHA/ONE_MINUS_SRC_ALPHA - the red channel is getEntityBrightness
  (the genuine lightBrightnessTable curve), so mobs flash DARK red in caves
  and never fullbright; alpha is a constant 0.4 with no fade, and the
  overlay stays on through the 20-tick death fall.

Port: removed the colour blend; the Indev overlay is a second Model_Render
pass with a 4x4 white substitute skin (e->TextureId + NonHumanSkin swap)
standing in for glDisable(GL_TEXTURE_2D), flat colour from
IndevTest_BrightnessOfLight(IndevTest_LightLevel(eye)) in the red channel,
alpha 102. White tex freed on context loss. (Genuine also red-flashes the
PLAYER via RenderPlayer in third person - ours is mob-only for now.)

## 2026-07-17: world time persisting across worlds (user report)

Genuine Indev constructs a brand-new World object per level: worldTime
defaults to 0 for generated worlds, and LevelLoader's getShort("TimeOfDay")
also yields 0 when the tag is missing. Our indev_worldTime is a static only
ever written by the loader tag or /time, so the clock leaked from world to
world. Fix: the IndevTest component's OnNewMap hook (fires on
World_NewMap - generation start, map-load Game_Reset, and MP map start,
all BEFORE the .mclevel Environment tags are parsed) now resets
worldTime=0, skyBrightness=15 (World.java field default), and the eased
skylight (-1 -> first tick snaps to target, mirroring LevelLoader:83's
immediate skylightSubtracted sync).

Also ported LevelLoader:69-75's SkyBrightness load clamp: getByte is
SIGNED (negative -> 0) and values > 16 are legacy percentages (* 15/100).

## 2026-07-18: COMBINED TWO-REPO SESSION - MP implementation restored + live integration test

Both repos in one session at last (branch claude/mock-survival-server-33jx1q
on each, containing survival-test / survival-support respectively).

### Restore of the parked MP implementation (ed604b6)
`git cherry-pick ed604b6` onto the tip; conflicts resolved deliberately per the
handoff's restore caveat (17 later fidelity commits touched the same files):
- SURVIVAL_TEST_NOTES.md / doc/*: kept HEAD (the park kept docs; HEAD carries
  all later corrections). server-session-handoff.md add/add -> HEAD.
- Screens.c GameOverScreen_Init: merged BOTH sides - ed604b6's MP-conditional
  Load button (single Respawn button when ServerDriven) UNDER HEAD's
  WIDGET_FLAG_INDEV_SCALE clears (the double-scale missing-buttons fix).
- SurvivalTest.c fire block: HEAD's later genuine fire-resistance ramp port
  (st_playerFire counting up from -PLAYER_FIRE_RESIST) kept, wrapped inside
  ed604b6's !SurvivalNet_ServerDriven() gate.
- SurvivalTest.c Init: took ed604b6's EnableMode split + unconditional hook
  registration (needed for runtime HELLO flips), with HEAD's later c0.30
  Tile$SoundType table block moved inside EnableMode.
Builds + links clean (needed apt libxi-dev + libgl1-mesa-dev in this container).

### Server side landed this session (mcgalaxy)
Death-screen dwell (handoff SS3.1): OnPlayerDied now HOLDS health at 0 (no
back-to-back revive), Player.HandleDeath skips its auto-respawn while
SurvivalNet.HoldsDeathScreen, repeat hazard deaths are suppressed via
OnPlayerDying cancel, and the revive comes from the client's SURV_RESPAWN
intent (validated: dead players only - a stray intent gets an authoritative
health echo instead of a free spawn teleport) or a 30 s safety timeout ticked
by the TIME scheduler. Map change while dead auto-restores full health
(OnJoinedLevel) since the client tears down per-map death state. Plus the SS1
HackControl note: Hacks.MakeHackControl now overrides fly/speed from the
level's SurvivalCreative (same decision as HELLO bit1) for survival-active
sessions, respawn hack off (server-owned), Referee keeps its escape hatch;
/Survival re-sends motd+hacks on live changes. No wire change - ext stays v1.

### Live integration test (MCGalaxy CLI + this client, Xvfb rig)
Setup notes for future sessions: the CLI anchors its CWD to the exe dir
(CLI/bin/Release/net8.0 holds properties/ + levels/); verify-names=false lets
the direct-connect client in; console via a FIFO. The misc/ps1 texpack has NO
icons.png/gui.png (no hearts/crosshair!), and classicube.net's default.zip has
a DIFFERENT icons.png layout (touch buttons where c0.30 hearts live -> solid
blue bar). Rig pack = classicube.net default.zip + genuine icons.png/sun.png/
moon.png overlaid from /tmp/indev_eagler resources (engine keeps only the top
quarter of icons.png, so the genuine 256x256 lands exactly on our /64 UVs).

Verified live against the fork server (main level SurvivalMode=Indev):
- HELLO (mode=2 flags=8 proto=1) + WORLDINFO (ground=30 water=32) chat lines;
  "Connected via the survival client (handshake verified)" both sides.
- SURV_HEALTH -> 10 genuine hearts above the hotbar (rebuilt on change).
- SURV_TIME -> genuine celestial day/night: scene went day->night with stars;
  IndevTest_WorldTime() advanced 15580->15680 over 5 s = the server's 20
  ticks/s with the local advance gated off.
- /kill -> Game Over + death camera + red tint + single Respawn button HELD
  by the server (health 0, empty heart backgrounds). Dwell verified >10 s.
- Respawn button path (gdb call SurvivalNet_SendRespawn, the exact call the
  button makes) -> server log "revived (respawn intent)", teleport to spawn,
  hearts refill, screen down. Round-trip ~1 s.
- Safety timeout path: no intent -> "revived (safety timeout)" at exactly +30 s.
- Stray respawn while alive -> "ignored respawn intent (not dead)" + echo.
- HELD_SLOT (0x85) intents logged server-side on hotbar changes.
- /Survival off mid-map -> mode-0 HELLO -> SurvivalTest/IndevTest_Enabled
  flip 1->0 live; /Survival indev flips them back. No rejoin needed.
- Map spawn caveat found: MCGalaxy default spawn (y=48 over ground 32) is a
  16-block drop - with /map death on + default fall 9 every (re)spawn was
  lethal (an infinite death loop, stock MCGalaxy behaves the same). Test rig
  uses /map main fall 20. Real survival maps must place spawn on the ground
  (or the phase-1 generator must set it) - noted for the server session.

## 2026-07-18: Phase 3 - mob streaming, both sides, live-tested

### Client: st_mobs becomes a network puppet (networking-plan 15.1/17.5)
- struct Mob grew netId (server u16 key), netState (last STATE flags for
  fire/fuse/graze pinning) and a latched netPos/netYaw/netPitch move target.
- SurvivalTest_NetMobSpawn/Move/State/Despawn appliers + a puppet tick that
  runs ONLY presentation: prev=next interpolation advance, cosmetic timers
  (hurtTicks/invincTicks/attackTime), fuse swell ramp (FUSE bit -> fuseTicks
  toward 30, fuseLast interp), fire overlay pinned while ONFIRE, graze easing,
  death keel-over + local corpse free at 40 ticks (DESPAWN belt-and-braces),
  ambient pig/sheep voices, walk anim + body-yaw easing + step sounds against
  the latched move, water-entry splash. No Mob_Travel/BasicAI/Mob_Hurt runs
  on puppets - the appliers + tick are the entire surface.
- MOB_STATE grew b5 noFur (visible mid-life shear; same bytes, no ext bump -
  25 updated). SPAWN flags: b0 helmet, b1 armor, b2 fur.
- TryAttackMob in MP: raycast picks the puppet, swing anim plays, and the hit
  becomes SurvivalNet_SendAttack(0, netId) - no local Mob_Hurt/tool wear.

### Server: SurvivalMobs.cs - the simulation (mcgalaxy)
Port of this client's verified c0.30/Indev mob sim to C#: 20 TPS dedicated
scheduler, per-level registries, BasicAI wander (7%/1%/4% impulse rolls, 80%
liquid bob), BasicAttackAI chase (16-block player acquisition, 2x-range 1%
give-up, genuine yaw/pitch facing math), c0.30 melee roll + creeper headbutt
self-damage, Indev per-type attacks (creeper 3/7-block fuse -> 30-tick blast,
spider light-flee + 2-6 block pounce, zombie 5 / default 2 melee), Mob.hurt
dual-threshold invuln + knockback, Mob.travel physics (0.91/0.98 drag, 0.08
gravity, 0.6 ground friction, water/lava 0.8/0.5 + paddle assist), axis-
clipped AABB collision vs level blocks (no step assist - genuine c0.30 mobs
jump instead), fall damage ceil(dist-3), drowning air ticks, lava/fire burn,
sheep graze (grass->dirt via normal SetBlock + 1/5 fur regrow), c0.30
initial population + capped topup spawner with min-of-two-uniforms Y bias,
600-tick + 1/800 despawn roll, undead sunburn. Player side: graduated
DamagePlayer with the same dual-threshold window (ticked at 20 TPS), lethal
damage routed through HandleDeath so the death-screen dwell applies, score
credit c0.30-only. /Survival spawn [type] + /Survival mobs test aids.

V1 deviations (documented in mcgalaxy session-notes + handoff): no Indev A*
pathing (c0.30 direct-steer in both modes), skeletons melee (arrows need the
phase-5 wire), brightness approximated as sky-exposure x day/night (no server
light engine), explosions damage players but never blocks, no drops, mobs
freeze on empty maps, no persistence across restarts, cluster spawn trimmed
to 1-3 (vs genuine up-to-9) to tame MP populations.

### Live integration test (same rig as the phase-2 session)
- /Survival spawn pig at the elevated level spawn: SPAWNed on the wire, fell
  under SERVER physics with MOVE streaming (client showed y 48->32.06), took
  the genuine 13-damage spawn fall (10 HP Indev pig), died, DESPAWNed. The
  whole pipeline proved itself by accident before the first posed screenshot.
- Ground-snapped test spawns: pig + sheep visibly wandering on the grass
  (positions moving through fractional lerp values client-side), genuine
  pig/sheep models + animation.
- Attack round-trip: SendAttack out of reach -> server "rejected attack (out
  of reach)"; in reach -> pig health 10->9->8->7 streamed back via STATE
  (client-visible), knockback pushed it out of range and the remaining spam
  was correctly rejected (13 rejections logged).
- Zombie: chased and melee'd the player dead in the genuine ~1.5 s point-
  blank cadence (4x5 HP, victim invuln absorbing the rest) -> "UserC was
  slain by a zombie" + death-screen dwell held. Mob targets are dropped for
  dead players (no corpse camping between revives).
- Creeper: spawned point-blank, fused, exploded - player hearts 20 -> 5
  (falloff damage), no corpse (fuse blast leaves none).
- Natural spawner verified ticking (population 0 in daylight is correct for
  monsters; animal spawn odds are conservative - raised Indev attempts to
  match c0.30 area count before landing).

## 2026-07-18 (cont): Phase 4 first slice - server-owned inventory, live-tested

### Client: st_inv/st_craft/st_armor + st_cursor become a server-driven view
- SurvivalTest_NetInvSlot/NetCursor appliers (SurvivalNet 0x20/0x21/0x25
  handlers, INV_FULL chunked 12 slots x 5 bytes) write echoed state via
  SlotPtr + invVersion++ + SyncHotbar - the HUD and the survival inventory
  screen already redraw on invVersion, so echoes appear immediately.
- The MP gate that forced the classic inventory screen is gone: the survival
  screen opens in MP and renders the streamed slots. Clicks are INTENTS now
  (echo-only v1, networking-plan 27.2): slot -> SendSlotClick, craft result ->
  SendResultClick, close/outside-click -> SendContClose (the SERVER refunds
  cursor + grid and echoes). SP paths unchanged.
- MP hotbar consequence: SyncHotbar now feeds from the streamed inventory, so
  the classic creative palette is gone on survival maps - slots start empty
  and fill by mining. (Creative-flag maps keep free placement server-side.)

### Server: SurvivalInventory.cs
Per-player 103-slot layout mirroring SurvivalTest.h exactly (36 main + 9
craft + 54 container reserved + 4 armor) + cursor + held slot, stored in
Player.Extras (follows /goto within a session). SendAll streams main+craft,
armor, cursor at handshake. HandleSlotClick is the 1:1 GuiContainer port
(pickup-all/half, merge-with-max-stack, right-place-one, swap) run on server
state with SLOT+CURSOR echoes; container range rejected until streamed;
armor accepts nothing yet (no armor items exist). HandleContClose refunds
cursor + craft grid (AddOne = storePartialItemStack order: merge first -
hotbar wins - then first empty) and resyncs. OnBlockChangingEvent bridge:
mining adds the broken classic block (raw <= 49) directly to the inventory
(drop-entity hop is phase 5; liquids yield nothing), placing consumes one
(held-slot preferred) or cancels + RevertBlock + full resync. Dead players'
block edits are cancelled. Max stacks v1: 99 c0.30 / 64 Indev flat (per-id
tables land with item definitions).

### Live test (gdb-driven, same rig)
- Mine grass -> INV_SLOT echo -> st_inv[0] = {2, 1}; second mine merged to
  count 2; hotbar rendered the stack (survival view, not the classic palette).
- Place -> consumed back to 1 -> 0; place with an EMPTY inventory -> server
  cancelled + reverted (the target cell came back as the authoritative dirt).
- SendSlotClick(0) -> cursor {2,1} + slot empty (echoed); SendSlotClick(5) ->
  stack moved to slot 5; SendContClose -> cursor refunded into slot 0.
- Rig notes: server/client must be started with setsid (a Bash-tool timeout
  killed the whole process group mid-test); survival digging is hold-to-break,
  so scripted mining calls Game_ChangeBlock (the dig-finish call) directly.

Remaining phase 4: containers + furnace streaming (0x22-0x24), crafting
recipes server-side, USE_ITEM (eat/containers), per-id max-stack + item
tables, PLAYER_EQUIP, optimistic click prediction.

## 2026-07-18: crash placing first block over the void (user report + crash log)

ACCESS_VIOLATION in the Indev sky maintenance: on floating maps an all-air
column's heightmap is the -10 sentinel, and Indev_SkyBlockChanged's
"cells that lost their sun" walk ran `for (yy = newH; yy > oldH; yy--)`
with oldH = -10 - GetBrightness at y = -1, -2, ... indexed the chunk
lighting array negatively (user's crash registers showed rsi/r12/r13 =
-1/-2/-3 mid-walk and a heap read ~width*length bytes before the array).
Trigger: placing the FIRST block into an open-void column ("placing a
block on a lower platform" on a deep floating map). Fix: clamp the walk
at y >= 0. Audited the siblings: Indev_SeedSkyColumn already clamps
(yMin >= h+1 never lowers a 0 floor, empty-column top walk produces no
iterations), breaking the last block gives newH = -10 which skips the
loop, and ClassicLighting_UpdateLighting only writes heightmap entries.

## 2026-07-18: pause menu scrambled on servers (user report)

PauseScreen_Init disables Generate/Load in MP with a RAW flags assignment
(`btns[n].flags = WIDGET_FLAG_DISABLED`), which wiped the auto-set
WIDGET_FLAG_INDEV_SCALE off exactly those buttons - they laid out with
plain Display_Scale offsets while the rest of the menu used the Indev
(offset*s)/2 transform, producing the overlapping half-grid (only the two
dark disabled buttons sat wrong in the screenshot). Same latent pattern in
ClassicPauseScreen_Init and DisconnectScreen (reconnect). All three now go
through Widget_SetDisabled, which sets/clears only the DISABLED bit - the
same helper the texture-pack button already used, so click/selection
handling is unchanged.

## 2026-07-20: phase 1 step 1 — the Indev block set on survival maps (server session)

No client code changed. The server (mcgalaxy `SurvivalBlocks.cs`) now applies
the full Indev block set to Indev-mode maps as level-scoped BlockDefinitions —
a 1:1 port of IndevBlocks_Define (src/IndevTest.c): the same names, tiles
(96-123), per-face container fronts on the -Z/+Z/-X/+X faces for views 71-82,
farmland 15/16 height, sprite crops/torches, lamp brightness 14/15, classic
fallback ids for pre-BlockDefs visitors. No new 0xB0 messages and no ext bump
(stock CPE DefineBlock/DefineBlockExt v2/UndefineBlock), so the wire contract
is untouched. The IndevTest_BlockToIndev/_FromIndev/BlockDataMeta bijection is
ported as SurvivalBlocks.ToIndev/FromIndev/DataMeta/ApplyDataMeta — the
authoritative view-id <-> (id, Data nibble) encoding for the upcoming server
generator + .mclevel steps, exactly the §18.2 model.

Confirmed during integration: on SURV_HELLO(mode=Indev) this client runs
IndevTest_NetworkModeChanged -> IndevTest_MapActivate -> IndevBlocks_Define,
i.e. the fork DOES locally (re)define the genuine models over the server defs
in MP (torch stick, fire mesh, wall-torch tilt, and the SetHardness table ride
along). The server-session-handoff claim that the client never self-defines on
server maps was stale and has been corrected. Server defs are what stock CPE
spectators render; the ones this client briefly shows between map load and
HELLO are visually identical tiles.

Server-side verified live (fork client + synthetic BlockDefs CPE client):
37 defs stream on join; /Survival off -> 37 undefines, /Survival indev ->
re-apply; /Survival give torch|chest|workbench|furnace -> INV_FULL echo with
genuine item icons + held torch model; in-reach place consumed (x4->x3),
break picked back up (x3->x4); un-owned lit-furnace place reverted without
consuming; reach-rejected far place consumed nothing; classic dirt mining
still yields its block. Mining view ids normalizes (facing views -> canonical,
lit furnace -> idle, wall torch -> torch, farmland -> dirt); crops/fire/
sources yield nothing until items/drops exist.

Test-rig note (not a code issue): this environment's hand-assembled
default.zip lacked the beta-jar terrain tiles 96+, so hotbar icons rendered
as the blue placeholder cells until the rig's terrain.png was patched with
the b1.7.3 jar tiles per the Resources.c beta_tiles table (a normal install
patches these on first-run asset download). Also: the test map's spawn was
buried 2 blocks under the surface, which at night produced a mob-attack /
suffocation death loop — the death dwell + 30 s safety revive cycled exactly
as designed throughout; grounding only fixes FLOATING spawns, so a buried
spawn may deserve the same auto-fix treatment some session.

## 2026-07-20: phase 1 step 2 — the Indev world generator on the server (server session)

No client code changed. mcgalaxy Generator/IndevGenerator.cs is now a C# port
of this repo's src/IndevGen.c (the oracle-verified LevelGenerator.java port),
registered as the "indev" /NewLvl theme: themes normal/hell/paradise/woods,
types inland/island/floating/flat, power-of-two width/length, height >= 64.
The parity-critical machinery came along: the three java.util.Random streams,
the double-built MathHelper sine table, float/double expression precision,
and the WR_* World replica (clamped reads, interior-only setBlock, falling
sand, liquid wake-ups, flower pops burning World.random draws, the
Assembling y-skip). Same seed/type/theme/size should reproduce this client's
generator block-for-block; the server output was verified by theme-signature
histograms + in-client inspection, not by re-running the Java oracle.

Generated maps come out survival-ready (SurvivalMode=Indev, hazards, the
block set, spawn house with genuine wall-torch mounting stored as extended
blocks) and their env config now feeds SURV_WORLDINFO genuine per-map
ground/water levels + edge fluid, replacing the placeholder defaults this
client had been receiving. Live-checked from this client: normal inland
terrain + house interior spawn, floating multi-layer islands, hell's lava
flood + dark red ambience, with the mob sim populating everything.

Server-side deviations to remember: per-theme sky brightness (hell 7, woods
12, paradise always-day 16) exists only in the generation light snapshot and
env colours - the live light model is still the shared clock; a floating
hell map's SurvivalTheme config byte reads Floating (type folded into the
enum). GitHub note: today's queued CI runs were a github-wide Actions
partial outage (incident since Jul 19 23:34 UTC), not a workflow problem.

## 2026-07-20: SURV_WORLDINFO v2 - i16 levels (floating dirt-plane fix)

User-diagnosed on the generated floating world: a dirt ground plane rendered
under the islands. Cause: SURV_WORLDINFO v1 carries ground/water as u8, but
floating maps genuinely use groundLevel -128 / waterLevel -127 (hell -16) -
the server clamped them to 0, so the OOB ground plane sat at y=0 instead of
far below the void. Fix: SurvivalTest CPE ext bumped to version 2 on BOTH
sides; WORLDINFO now carries the levels as i16 BE at offsets 1-4 (fields
after shift by 2). Both sides branch on the NEGOTIATED version, so v1 peers
still exchange the old layout. Client stores the negotiated version in
Server.SurvivalExtVersion (Server.h). Verified live: floating map renders
open sky with the genuine below-island clouds (cloud height -16), no plane.

Also fixed: Server_ResetState never cleared Server.SupportsSurvival on
reconnect (the SurvivalNet.c comment claimed the net layer did) - a survival
flag could leak from a survival server into a following non-survival
connection. Both flags now reset. Docs: networking-plan section 25 updated to
the v2 layout, survival-handshake table updated, mcgalaxy reference
snapshots refreshed. NEXT SESSION: user wants an in-depth bug + quality pass
across the whole stack; candidates listed in mcgalaxy session-notes.

## 2026-07-20: map-change leaks - night lighting, forced fancy mode, death screen

Three user-reported leaks when leaving a survival map (all fixed, verified
live against a midnight Indev map -> plain flatgrass map move):

1. Night lighting bled into the next map: FancyLighting's eased Indev sky
   level (indevSkyLevel, e.g. 4 at midnight) is a static that only the Indev
   day/night tick writes - on a non-Indev map that tick never runs, so the
   next map rendered permanently at night while its env colours reset to
   daylight ("grass weirdly brighter" mismatch). IndevTest's OnNewMap now
   resets it to 15 and clears the stale base-colour snapshot.
2. The lighting mode itself: MapActivate force-switches to FANCY and never
   restored the player's previous mode; OnNewMap now restores it (unless the
   server locked the mode or the player changed it again mid-map). The
   mid-map mode-0 HELLO path (/Survival off) got the same cleanup plus an
   immediate env colour/sun/shadow restore.
3. The Game Over screen survived a server-initiated map change while dead:
   the new map may not even be survival, so no revive SURV_HEALTH ever
   arrives to dismiss it - the screen sat over the new map with full hearts.
   SurvivalTest_OnNewMap now tears down the death presentation (screen +
   death-zoom FOV) unconditionally, before its enabled check.

Server side (same session): the mob sim now ticks whenever ANY player is on
the level, so mobs keep roaming for classic spectators after the last
survival client leaves (they only freeze on truly empty maps), the spawner
rings around any viewer, and the ChangeModel mirror syncs at 10 Hz (was 5) -
the cadence MCGalaxy relays player positions at, so stock-client entity
interpolation smooths mobs exactly like players. AI targeting, hazards and
puppet streams remain survival-client-only.

## 2026-07-20: MP Indev creative uses the Indev creative inventory (user request)

On creative survival maps (HELLO flags bit1) the fork client now behaves like
SP Indev creative instead of falling back to the classic picker flow:

- New SurvivalTest_ServerOwnsInventory() = ServerDriven && !CreativeActive.
  In MP creative the inventory is the genuine CLIENT-side palette - the
  server tracks no inventory on creative maps (free build, no consume) - so
  the Indev GuiInventory clicks stay local (Screens.c routes through the new
  helper), the palette hotbar fills at MapActivate in MP too, drops stay
  local, and the INV_FULL/INV_SLOT/CURSOR appliers ignore stray streams.
- The classic block picker (B) becomes the creative palette browser: picking
  a block deposits a full stack via SurvivalTest_CreativeGive (finally wired
  - it was dead code from the section-14 research) into the Indev inventory
  instead of poking the classic hotbar, in SP and MP alike. Stack size 99 =
  the genuine Indev block stack limit.
- Server (mcgalaxy): SendHandshake skips the inventory streams on creative
  maps (they would wipe the palette the HELLO just filled) and
  HandleSlotClick/HandleContClose reject stray intents there.

Verified live on a creative Indev map: palette hotbar fills on join, the
Indev inventory screen opens with local clicks, CreativeGive(chest 54 /
workbench 58) deposits 99-stacks, torch placement sticks with no revert and
no consume, and /Survival inv shows the server tracking nothing. Flipping
creative off live resyncs the real server inventory via the fresh handshake.

## 2026-07-20: the rest of the phase-4 GUI intents - containers over the wire

USE_ITEM 0x81 + CONT_OPEN 0x22 + CONT_SLOT 0x23 + FURN_PROG 0x24 are live on
both sides (no ext bump needed - all four ids were reserved; an old server
logs the intent, an old client never sends it).

Client: right-clicking a workbench/chest/furnace on a server map now sends
SURV_USE_ITEM (consuming the click like genuine blockActivated) instead of
no-opping; the reply CONT_OPEN routes into a new NET CONTAINER VIEW in
IndevTest (IndevTest_NetContOpen/NetContSlot/NetFurnProg) that the existing
chest/large-chest/furnace screens render unchanged - OpenKind/SlotCount/
ContainerSlot and the furnace flame/arrow getters branch to it when set.
Kind 4 (workbench) just sets CraftDim(3) over the streamed craft slots.
Kind 0 force-closes via SurvivalInvScreen_ForceClose (new; removes the
screen without echoing CONT_CLOSE - Gui_Remove no-ops when absent, an
InputGrab guard turned out to skip removal and was dropped). Container
clicks ride the existing SLOT_CLICK intents at slots 45..98.

Server (SurvivalInventory.cs): session-scoped tile entities keyed by level+
position (chest 27, furnace 3), lazily created on open; the genuine large
chest pairing (-X/-Z neighbour = upper half) and the BlockChest lid-block
rule (solid above either half refuses to open); reach-validated. The
GuiContainer click model now resolves the 45..98 range through the player's
open view, echoing CONT_SLOT to every viewer of the same entity. Mining a
container discards the tile entity (scatter needs phase-5 drops) and
force-closes any open screens. Not opened on creative maps (local palette).

Verified live: chest open -> deposit a torch stack via container clicks ->
close -> reopen persists -> full reconnect persists (level-keyed state);
workbench opens the 3x3 grid; furnace opens its 3-slot GUI; mining the
furnace under the open screen force-closed the GUI and yielded the pickup.
Untested live (single-client rig, verified by inspection): large-chest
pairing, multi-viewer CONT_SLOT echo. V1 deviations: no smelting or recipes
until items land, container contents vanish on destruction, contents do not
persist across server restarts.

Rig note: repeated rapid gdb attach/call cycles can wedge the client
(SIGSTOP-like freeze with identical HUD frames; kill -CONT sometimes
insufficient) - space attaches by a few seconds and use timeout'd gdb.
Also: a heredoc that dies with a timed-out command silently leaves the
NEXT run reading a nonexistent script - check for "No such file".

## 2026-07-20: ITEMS land server-side (SurvivalItems.cs) - no client changes

The mcgalaxy port of this client's item machinery: the full indevItems table
(ids 256+, per-id max stacks 99/64/1), IndevTest_CanHarvest (pickaxe-tier
gating), SpawnIndevDrops as the authoritative mining-yield table (stone->
cobble, coal ore->coal ITEM, diamond ore->diamond, gravel flint roll, crop
wheat+seed rolls; v1 yields go straight to the inventory until phase-5
drops), the complete CraftingManager recipe set (fixed recipes + the
generated 5x5 tool matrix + armor incl. the genuine chain-from-FIRE quirk,
offset+mirror matching) behind a real RESULT_CLICK, and TileEntityFurnace
smelting on the 20 TPS survival tick with the lit/unlit block flip and
FURN_PROG/CONT_SLOT streaming. Recipe/smelt/fuel tables are byte-identical
ports of this client's - REQUIRED, because this client renders the craft
result PREVIEW locally from the streamed grid while the server crafts
authoritatively on the click.

Zero client changes for items themselves (u16 ids already ride every slot
message and the icon table exists) - ONE client fix: the net-container
appliers (CONT_SLOT/FURN_PROG/CONT_OPEN) now bump the inventory version
(new SurvivalTest_MarkInvDirty) so an open furnace/chest GUI repaints when
the server streams changes; before, live smelting only showed after a click.

Verified live: /Survival give by item name (coal, iron_pickaxe); log ->
planks x4 (client preview + server craft onto cursor, log consumed);
cobble x3 + coal smelted to stone x3 with the furnace block flipping lit
(61->62) and flame/arrow streaming at 4 Hz. Not yet: eating/tools/durability
(USE_ITEM on items), PLAYER_EQUIP/armor absorption, drop entities.

## 2026-07-20 (later): furnace output take-only + server .mclevel I/O

User-reported: the furnace GUI accepted placing items into the output
(product) slot; genuine SlotFurnace is take-only. Fixed on both sides with
the same one-line rule - a click on furnace container slot 2 with a
non-empty cursor is refused (covers plain places AND merge-onto-stack;
taking stays the cursor-empty pickup path). Client: SurvivalTest_SlotClick
(the singleplayer click model). Server: HandleSlotClick, so multiplayer is
guarded regardless of client. The crafting result slot was already fine -
it goes through RESULT_CLICK, which only ever moves the preview onto the
cursor.

Server side (mcgalaxy, same session): .mclevel import/export landed -
McLevelImporter now expands the Data metadata nibble through the
SurvivalBlocks bijection (facings/stages/moisture/orientation) and marks
imports survival-ready; new McLevelExporter writes this client's
MCLevel_Save schema (LocalPlayer stub, chest/furnace TileEntities with live
contents, one per container block). /Survival export <name> <level> ->
extra/import/, /Import round trip verified cell-identical (128x64x128);
networking-plan 18 updated with the as-implemented status. Files exported
by the server open in this client's singleplayer loader (same schema).

Verified live (gdb-driven intents against the rig server): open furnace ->
SLOT_CLICK on output with coal held = slot stays empty; next click on fuel
slot lands the stack (streamed CONT_SLOT echoes confirm). Exported the
level after: the furnace TE carries Slot 1 = coal, output untouched.

## 2026-07-20 (later still): server-side placement shaping + block-set finish

User request: finish the blocks, drop the non-Indev leftovers from Indev
levels, audit def shapes, and give placed furnaces their facing (MP had
none - the client's SP rotation was local-only, so the server kept the
canonical id).

All landed server-side in mcgalaxy; NO client code changes were needed -
the client was already the oracle for every rule and its local placement
guess is now confirmed (or corrected) by the server's authoritative echo:

- MP furnace/chest placement now rotates to face the placer using this
  client's exact yaw-quadrant formula (IndevTest_BlockChanged); torches
  wall-mount server-side via the onBlockAdded -X/+X/-Z/+Z/floor auto pick,
  unsupported torches and chest triples/L-shapes are refused before any
  inventory consume. Deviation: the clicked-face torch override
  (onBlockPlaced) can't ride the classic place packet (no face byte) -
  when several supports exist the server's auto pick wins.
- The nonGenuine leftovers (59/60/63/64/65) are now hidden AND unplaceable
  on Indev maps server-side too (they placed free before); stock clients
  get proper thin-column torch defs (bounds-cropped tile) instead of
  full-size X sprites.

Verified live on the rig: gdb-driven Game_ChangeBlock placements through a
real MP session - faced furnace (76) / chest (72), double chest ok, triple
refused with the original wall block restored, wall torch mounts on the
placed furnace (94), floating torch and pillar(63) refused, consumption
counts exact, def stream audited via the synthetic CPE client.

## 2026-07-21: MP right-click item intents (hoe / seeds / food)

The client now forwards held-item right-clicks to the server as SURV_USE_ITEM,
where before only container blocks did:

- SurvivalTest_TryUseBlock (ServerDriven branch): sends USE_ITEM when the held
  item is a hoe or seeds (targeted at the clicked block), in addition to the
  existing workbench/chest/furnace container opens.
- SurvivalTest_TryEat (ServerDriven branch): a held food is now sent as a
  TARGETLESS USE_ITEM - sentinel coords (-1,-1,-1, face 0xFF) - so the server
  takes the eat path. Previously eating was a no-op in MP.

No new wire message: USE_ITEM's existing [heldSlot][x i16][y i16][z i16][face]
layout carries it, with x=-1 signalling "no target block" (eat / use-in-air).
The server applies the effect and streams back the block change + SURV_HEALTH +
the affected inventory slot, so the client just renders the authoritative
result (it runs none of the hoe/seed/eat logic locally when ServerDriven).

Server side (MCGalaxy): hoe -> farmland (+1 durability, grass 1/8 seed), seeds
-> crop + consume, food -> heal + consume (soup -> bowl), damageItem tool wear.
Flint&steel -> fire is deferred with the phase-5 fire spread/burnout ticks.

Verified live server-authoritative (via /Export + /SurvInv): grass tilled to
genuine farmland (60), hoe took 1 durability, seeds planted a genuine crop (59)
and consumed one, bread was eaten out of the stack.

## Handoff note (branch)

Continue this work on the EXISTING feature branch
`claude/mock-survival-server-33jx1q` on BOTH repos (UmbreoClaw/ClassiCube and
UmbreoClaw/mcgalaxy) - do NOT create a new branch. The full next-session plan
(pending live-verification items, the /inventory GUI design, spectate, block
drops) lives in mcgalaxy `doc/survival-support/session-notes.md` under
"HANDOFF - next session pickup".

## /Inventory dedicated panel (SurvivalTest v3, CONT_OPEN kind 5)

The server's /Inventory viewer now opens a bespoke player-inventory panel instead
of reusing the chest GUI. New internal kind INDEV_CONTAINER_PLAYERINV (IndevTest.h),
mapped from wire kind 5 in SurvivalNet_HandleContOpen. The ext version bumped to 3
(Protocol.c) - the server (mcgalaxy) sends kind 5 / 40 cells to v3 clients and the
chest fallback (kind 1 / 36) to older ones.

Screens.c renders it as a DUAL-INVENTORY window: inventory.png on top shows the
TARGET's 40 slots laid out pocket-style (27 storage + 9 hotbar + 4 armor column,
helmet on top), and the container.png player strip below shows the VIEWER's own
36 slots for drag-transfer. Cell layout in SurvivalInv_ContainerSlotXY: 0..26
storage grid (8,84), 27..35 hotbar (8,142), 36..39 armor (8, 8/26/44/62). The rest
of the survival-inv screen (hit-test, content, damage overlays, DisplaySlot split
of own-strip vs container) already generalised to a 40-cell net container. The
target paperdoll window is left empty (the client isn't told whose inventory it is).

Verified in the graphical rig via gdb-injected IndevTest_NetContOpen(3,40) +
NetContSlot samples: armor column, storage grid, hotbar and the viewer's own strip
all render in the right cells.

### Revision: side-by-side panels (not stacked)

The first cut stacked inventory.png over the container.png player strip, which read
as broken (tall, seamed, empty middle). Replaced with TWO inventory.png panels side
by side: LEFT = the target's 40 cells (via the container cells), RIGHT = the
viewer's own inventory. Layout: panelW = 176+8+176, both panels 166 tall; the
viewer's own gridX/gridY/hotY point at the right panel (panelX + 184 + 8). The
paperdoll (which renders the LOCAL player) moved to the RIGHT panel's doll window
(dollBoxX = panelX + 184 + 26); RenderDoll is enabled for PLAYERINV, and its two
hardcoded `panelX + 51*texF` model-anchor references became `dollBoxX + 25*texF`
(identical for the normal single panel, correct for the right panel here). The
LEFT/target doll window stays empty (the client isn't told the target's entity).
Verified via gdb-injected NetContOpen(3,40) + NetContSlot/NetInvSlot samples: two
filled inventories side by side with the viewer's doll in the right one.
