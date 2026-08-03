# Systematic Fidelity Audit — Findings Queue

User delegated systematic auditing of everything (GUI placement/scale,
gameplay, mob mechanics, all between) against the decompiled ground truths.
All the domain audits below are complete.

### Getting the ground truths

Both are reobtainable in minutes; do not trust a `/tmp` path from an older
session, they do not survive.

- **in-20100223** (Indev, the version this fork targets):
  `git clone --depth 1 https://github.com/EaglerPorts/in-20100223` — readable
  Java, the primary source.
- **c0.30_01c** (Survival Test): pull the client jar from Mojang's
  `piston-meta` version manifest and read it with `javap -p -c`. It is
  obfuscated, but the classes that matter deobfuscate by inspection
  (`com.mojang.minecraft.Entity`, `mob.Mob`, `level.Level`, `level.b` =
  MobSpawner, `mob.Skeleton`, `item.Arrow`). Set `JAVA_TOOL_OPTIONS=` first
  to silence the picked-up-options noise.

Status legend: [V] = independently verified against the Java by the main
session; [P] = pending verification (audit finding, not yet re-checked).
Every [P] item must be verified against the genuine source before fixing.

VERIFICATION RULE: subagent findings are leads, not verdicts.

This is not a formality. A later mob audit reported that the client was the
unfaithful side for giving mobs `StepSize = 0.5` and recommended deleting it.
The jar says `Mob.<init>` sets `footSize = 0.5F`, and every mob — and the
player — descends from Mob. The finding was exactly backwards: the client was
right and the SERVER was missing the step-up entirely. Acting on it unchecked
would have broken the faithful side to match the broken one. Verify first,
every time.

---

## Domain 1: GUI placement & scale (audit complete)

MATCHES confirmed for: hotbar rects/highlight, heart UVs/fill/jitter/glow,
armor row, bubble count formula, stack-count placement, score/arrows labels
(absent in Indev, correct), all container slot grids (inventory/workbench/
furnace/chest), label positions/colors, empty-armor silhouettes, hover
highlight, death tint, held-stack centering.

FIXED in batch 1 (0876ebd): 1, 2, 3, 5, 7, 14a.
FIXED in batch 2 (06ba2c9): 9, 10, 11, 12, 14b.
FIXED in batch 3: mobs 1, 2, 3, 6, 7, 8, 11 + c0.30 half of 5 (initial
population avoids the level spawn point; the INDEV initial 1000-pass
avoid still needs the gen spawn threaded through - open); items 1, 3,
4, 5, 6, 8, 14 (+ bonus: Indev sheep shear verified to drop GRAY cloth,
already correct). Item 2 (sword 1.5x dig speed) folded into the open
dig-model overhaul (item 9).
FIXED in batch 6: 4 (Indev GUI scale - genuine integer ScaledResolution
via the new "Indev GUI scale" toggle in GUI options, default ON, applies
only while the Indev gamemode is active; c0.30/creative keep the engine
formula).
FIXED in batch 7: 8 (labels/counts rasterised at 8 GUI px * texF and
rebuilt on scale change) + the invert-blend regression (batch 1's
crosshair blend toggled the ENABLE state off, blacking out transparency
for everything drawn after it - now swaps the blend function only).
STILL OPEN: 6 (pop anim curves - needs IsometricDrawer asymmetric scale
support), 13 (doll anchor constants), 14c (label draw order over held
stack).

1. [V] **Container GUI background dim missing** (HIGH).
   Genuine GuiContainer.drawScreen -> drawDefaultBackground() ->
   drawGradientRect(0,0,w,h, 0x60050500, 0xA0303060) behind EVERY container
   screen (GuiContainer.java:22, GuiScreen.java:94-97). Ours draws the panel
   with no dim (Screens.c SurvivalInvScreen_Render). Engine helper exists:
   Gfx_Draw2DGradient.
2. [V] **Death screen: Respawn button is NOT genuine** (MEDIUM, gameplay).
   BOTH ground truths have exactly two 200-wide buttons: "Generate new
   level..." (w/2-100, h/4+72) and "Load level.." (w/2-100, h/4+96) — no
   respawn (GameOverScreen.java:7-14; GuiGameOver.java:10-16). Title 2x at
   on-screen y=60 top-anchored, "Score: &e<n>" at y=100 (Indev shows score
   on death too). Our earlier notes claim of a genuine respawn button is
   WRONG — fix notes too. Our layout is centre-anchored: change to genuine
   top-anchored offsets. Keep our "Quit game" only if replacing nothing —
   genuine has no quit button on this screen.
3. [V] **Indev crosshair invert blend missing + 1px anchor** (MEDIUM).
   GuiIngame.java:44-46: crosshair at (w/2-7, h/2-7), 16x16, drawn with
   glBlendFunc(GL_ONE_MINUS_DST_COLOR, GL_ONE_MINUS_SRC_COLOR) — colour-
   inverting. Ours: exactly centred, 15px UV sample, plain blending.
   Needs a Gfx invert-blend mode (GL: glBlendFunc; D3D9: D3DBLEND_INVDESTCOLOR/
   INVSRCCOLOR; D3D11: D3D11_BLEND_INV_DEST_COLOR/INV_SRC_COLOR) + default
   fallback in _GraphicsBase.h. Indev only; c0.30 has no invert.
4. [P] **GUI scale quantization** (MEDIUM). Genuine Indev ScaledResolution:
   largest integer scale keeping >=320x240 (1080p -> 4x); c0.30 HUD:
   continuous h/240 (1080p -> 4.5x). Engine: 1+min(w/640,h/480) (1080p -> 3x)
   so the whole survival GUI is a step smaller than genuine. Plan: in Indev
   mode make Gui window scale = max(1, min(w/320, h/240)) integer; leave
   c0.30 on engine behaviour (conservative).
5. [P] Hearts/bubbles row 1 GUI px too high: our gap 2*scale above hotbar,
   genuine y = height-32 = gap 1 (HUDScreen.java:69; Screens.c:548).
6. [V] Hotbar slot pop animation: FIXED in round 3 (GUI batch) - new
   IsometricDrawer_AddBatchScaled (asymmetric X/Y about the centre; plain
   AddBatch resets the scale). c0.30: X=sin(t^2*pi)+1, Y=sin(t*pi)+1 +
   the sin(t^2*pi)*8px rise; Indev: squash 1/k x (k+1)/2 (k=1+t/5) with
   the low (y+12) pivot reproduced by shifting the centre. Rig-verified
   (squashed tall-narrow icon caught mid-anim, count unscaled).
7. [P] HUD hotbar item sprites: genuine 16x16 GUI px at (cell+3, height-19)
   (GuiIngame.java:133-134); ours 0.72*slotW centred (Screens.c:862-875).
8. [P] Container label/count text rasterised at fixed pt (14/16pt), doesn't
   track panel scale (Screens.c:3314-3334); genuine 8px GUI font scales with
   panel. Medium effort (re-rasterise on scale change).
9. [P] Chest bottom strip stretched 96 texels over 97 units — chest panel
   should be 71+96=167 units tall, ours 168 (Screens.c:3037-3046).
10. [P] Slot hit region: genuine xPos-1..xPos+17 (Slot.java:22-28); ours
    xPos..xPos+18 (Screens.c:2590-2592) — shifted +1.
11. [P] Craft-grid slots suppress durability bars (Screens.c:2748-2750);
    genuine renders overlays in every slot.
12. [P] Furnace flame stub: genuine draws flame while isBurning() even at
    h==0 (2px stub); ours gates h > 0 (Screens.c:3064). Needs an is-burning
    accessor.
13. [V] Paperdoll anchors: FIXED in round 3 (GUI batch) - Indev path uses
    panel-relative (51,75) feet anchor at scale 30*texF, mouse deltas from
    (panel+51, panel+25) in genuine GUI px. ALSO fixed a pre-existing
    180-degree facing bug (the ortho rewrite's x+y mirror is a Z-spin,
    which never turns the face; base yaw is now 180). Cursor tracking
    rig-verified both directions. Classic doll box untouched.
14. [P->partial] Minor: (a) heart shake gated health>0, genuine shakes at
    <=4 incl 0 - OPEN; (b) held-stack count anchor +8*texF vs genuine +9 -
    OPEN; (c) [V] FIXED in round 3 (GUI batch): labels now draw LAST
    (genuine drawGuiContainerForegroundLayer order), hover highlight after
    the slot item+count, and the cursor-held block/count draw as the iso/
    count batch tails above the highlight (genuine z+32 held stack).

## Domain 2: Mob mechanics (audit complete)

MATCHES confirmed for: species stat table, c0.30 wander/attack AI constants,
spider lunge, skeleton fire/death-burst, creeper, sheep grazing/shear, hurt/
knockback/invuln, despawn, fall/air/lava, Indev fire ignition, Indev creature
AI + pathfinder, spawner rules (except 5 below), death drops (except 2),
mob push (player push superseded note confirmed implemented).

1. [P] **c0.30 look-pitch double sqrt** (MEDIUM): BasicAttackAI.java:51-53
   pitch = -atan2(dy, dist3d); ours uses sqrt(dist3d) as adjacent
   (SurvivalTest.c:3518-3520) -> steep over-pitch, affects skeleton arrows.
   Notes:3080 repeats the wrong claim — fix notes.
2. [P] **c0.30 sheep death drop** (MEDIUM): Sheep.die() drops 1-2 BROWN
   MUSHROOMS like Pig (Sheep.java:53-61); ours drops 1-2 white wool
   (SurvivalTest.c:2554). Notes claim wool — contradiction; fix notes.
3. [P] **Indev animal health 10** (MEDIUM): EntityLiving default 10, only
   EntityMob raises to 20. Ours: 20 for all (SurvivalTest.c:4027).
4. [P] **Indev explosion curve** (MEDIUM): World.createExplosion uses ray
   destruction + entity damage to 2x radius, ((d^2+d)/2*8*2r+1) (~49 HP
   point-blank r=3) + velocity knockback. Ours reuses c0.30 sphere+15 HP
   falloff, no knockback (SurvivalTest.c:2813-2874).
5. [P] **Initial-population spawn-point avoidance** (MEDIUM): both genuine
   spawners, when avoid entity is null, still reject candidates within 16
   blocks of xSpawn/ySpawn/zSpawn (MobSpawner.java:53-65 c0.30; Indev same).
   Ours skips the check when avoidPos NULL (SurvivalTest.c:4095-4101,
   4184, 4237-4245). Fix notes claim too.
6. [P] Indev animal spawn Y: uniform nextInt(height) for animals; only
   monsters use min-of-two bias. Ours biases both (SurvivalTest.c:4152-4154).
7. [P] Indev sheep shear trigger: genuine only if attacker instanceof
   EntityLiving — arrows never shear (EntitySheep.java:20). Ours shears on
   any attacker incl. arrow fakeAttacker (SurvivalTest.c:2702, 4672-4681).
8. [P] Indev aggro-on-hit: no same-species exemption in Indev
   (EntityMob.java:38-42); ours applies c0.30 exclusion in both modes
   (SurvivalTest.c:2737-2742).
9. [V] FIXED in round 3 (entity polish): Indev drowning: 2 HP every 20 ticks (air==-20 reset) + 8-bubble
   burst (EntityLiving.java:85-100); ours c0.30 cadence (~2 HP/10 ticks),
   no bubbles (SurvivalTest.c:3706-3711).
10. [V] FIXED in round 3 (entity polish - per-mode setSize tables +
    Mob_ApplySize re-applied on every model swap): Mob bbox sizes come from engine models, differ from genuine
    (c0.30 pig/sheep/spider 1.4 wide; ours ~0.875-0.94). Wide impact but
    engine-model constraint — decide: override Size per mode?
11. [P] Indev kill score: genuine awards none; ours adds c0.30 deathScore
    in both modes (SurvivalTest.c:2531).
12. [P] c0.30 explosion knockback: genuine hurt() passes the TNT/creeper as
    attacker -> knockback; ours passes NULL (SurvivalTest.c:2865-2873).

## Domain 3: Items/blocks/crafting (audit complete)

MATCHES confirmed for: full item table (ids/stack/durability/heal), armor
tables + absorption, melee damage, canHarvest rules, every crafting recipe
shape, furnace smelts/burn times/cook order, drops tables (except below),
Indev hardness overrides, hoe till, bow behaviour.

1. [V] Indev food maxStackSize 1: FIXED - ItemFood foods are single-stack
   (IndevTest.c IndevItems_Seed, verified apple/bread/porkchops).
2. [V] Sword dig speed 1.5x vs everything: FIXED in round 3 (dig-time) -
   IndevTest_StrVsBlock returns float, swords a flat 1.5f.
3. [V] Sword wear: FIXED - IndevTest_ToolUseWear returns sword 1/hit 2/block,
   tools 2/hit 1/block (genuine ItemSword/ItemTool split).
4. [V] Hoe/flint&steel wear: FIXED - ToolUseWear returns 0 for hoe; flint&
   steel only wears on ignite (IndevFire, genuine ItemFlintAndSteel.onItemUse).
5. [V] Tool breakage: FIXED - DamageHeldTool uses strict > maxDamage.
6. [V] Indev obsidian drop: FIXED - SpawnIndevDrops drops BLOCK_COBBLE.
7. [V] c0.30 hardness: FIXED + re-verified vs Block.java setData - dirt 10,
   sand 10, slab/double 40, brick 40 (2.0F), grass 12, gravel 12. All match.
8. [V] c0.30 double slab drops 1: FIXED - GetBlockDrop count=1 for DOUBLE_SLAB.
9. [V] **Indev dig-time model** (MEDIUM): FIXED in round 3 (dig-time) -
   Indev_BlockStrength ports Block.blockStrength exactly (float accumulator
   st_breakDamage, break at >= 1.0, /5 head-in-water, /5 airborne,
   non-harvest 1/hardness/100, bedrock 0, hardness-0 instant); c0.30 keeps
   the integer hits/hardness+1 model. gdb-verified per-tick values exact.
10. [V] Tool effectiveness lists: FIXED in round 3 (dig-time) - dig-sound
    proxy replaced by IndevTest_StrVsBlock with the genuine ItemPickaxe/
    ItemAxe/ItemSpade id arrays (+ sword flat 1.5, chest/furnace variant
    fold). Workbench/brick/obsidian/furnace correctly revert to 1.0.
11. [V] Mirrored recipe matching: FIXED - IndevTest_MatchRecipe tries every
    offset unmirrored AND horizontally mirrored (mirror flag in the scan).
12. [V] Indev arrow physics: FIXED in round 3 (arrows) - full EntityArrow
    port: setArrowHeading gaussian spread (0.0075/axis, player 1.0 /
    skeleton 12.0), move-then-drag order, drag 0.99 air / 0.8 water, flat
    0.03 gravity, flat damage 4, bounce on absorbed hits (-0.1/axis),
    stuck-block re-loosen kick, arrowShake pickup gate, 0.3/side target
    grow, airTicks owner grace, facing 0.2 lerp. c0.30 paths byte-kept.
    Rig-verified: |v|=1.509 at spawn, one tick = *0.99 - 0.03 exactly.
13. [V] Lit furnace drop: FIXED in round 3 (entity polish) - new
    IndevTest_DropFormBlock keeps lit 62 in the drop paths (mining +
    explosion); CanonicalBlock untouched for recipes/naming; placed lit
    furnaces rotate like idle ones and genuinely stay lit until used.
14. [V] Mushroom eating c0.30-only: FIXED - TryEat returns false for Indev
    before the mushroom branch (Indev shrooms are soup ingredients).
15. [V] Insta-break tool wear: FIXED in round 3 (entity polish) -
    SurvivalTest_WearHeldToolForBlockBreak in InputHandler_DeleteBlock
    (wear before removal, like sendBlockRemoved's onBlockDestroyed).

FIXED in batch 5: domain-4 items 1, 2, 3, 4, 7, 14 (TNT fuse 80 +
chain 10..29 + fuse sound + no Indev defuse, pickup delay 10, lava
tickRate 25). Item 6 was already fixed in batch 3.
STILL OPEN (next session): 5 (Indev explosion drop table), 8 (item
lava pop/burn/push-out), 9 (drop spin/bob rates + instant pickup),
10 (TNT render swell/flash), 11+mobs-4 (Indev explosion ray-march +
damage curve + knockback, one combined fix), 12 (farmland trampling),
13 (crops canBlockStay), 15 (c0.30 random-tick rate), 16 (night
brightness curve), 17 (physics change notifications), 18 (torch
placement fail), 19 (death-scatter notes decision - ASK USER),
plus mobs 9/10/12, items 7-partial/9/10/12/13/15, GUI 4/6/8/13/14c.

## Domain 4: Entities + environment (audit complete)

MATCHES confirmed for: EntityItem core physics/pickup/toss/despawn, render
copy thresholds + glow, c0.30 TNT entirely, day/night formulas, Indev
random-tick dispatch, crops growth math, farmland moisture, paintings
(entire port), torch placement, fire spread verbatim.

1. [P] **Indev TNT fuse 80 ticks** (HIGH): EntityTNTPrimed fuse=80 (4s);
   ours TNT_FUSE_TICKS 40 both modes (40 is c0.30-correct). Mined + fire
   paths + debug all pass 40 in Indev.
2. [P] Indev chain-reaction fuse rand(20)+10 = 10..29; ours 5..14 both
   modes (c0.30-correct).
3. [P] No random.fuse sound when Indev TNT primes (BlockTNT.java:31).
4. [P] Indev primed TNT must NOT be melee-defusable (no attackEntityFrom
   override); ours defuses in both modes.
5. [P] Indev explosion drops must use the INDEV drop table
   (dropBlockAsItemWithChance 0.3): ours routes explosions through the
   c0.30 table in both modes (log->planks etc).
6. [P] Indev obsidian mined drop -> cobble. FIXED in batch 3 (overlap).
7. [P] Mined drops need delayBeforeCanPickup=10 (Block.java:287); ours 0
   (only Q-toss gets 40) - items vacuum instantly.
8. [V] Indev EntityItem lava/fire: FIXED in round 3 (entity polish) -
   health 5, isBoundingBoxBurning (fire/lava ids, UNSHRUNK box; the agent's
   handleLavaMovement claim was wrong - the -0.4 shrink degenerates for a
   0.25 box) deals 1/tick, silent death; centre-cell lava fizz-bounce
   (motionY 0.2, x/z (r-r)*0.2, random.fizz 0.4/2.0+r*0.4); pushOutOfBlocks
   six-face minimum-exit port. Rig-verified death in a walled lava pocket.
9. [V] Indev drop spin/bob: FIXED in round 3 (entity polish) - Drop_SpinDeg/
   Drop_Bob mode split: Indev (age_ticks/20 + hoverStart) rad spin + 
   sin(age_ticks/10 + hoverStart) bob (1/3 the c0.30 frequency), rot0
   folded to hoverStart. NOTE pickup delay was ALREADY correct (10-tick
   block drops - genuine is NOT instant; c0.30 is the instant one).
10. [V] Indev TNT render: FIXED in round 3 (entity polish) - swell
    (1-(fuse-t+1)/10 clamped)^4*0.3+1 over the last 10 ticks on cube+glow
    (render-only, pick box unswollen), flash only while fuse/5%2==0 at
    alpha (1-(fuse-t+1)/100)*0.8, smoke at y+0.5 (c0.30 keeps +0.6).
    gdb-verified: swell(5)=1.00768, swell(0,t=.5)=1.24435 exact.
11. [P] Indev explosion ray-march block destruction + entity velocity
    knockback (overlaps mobs finding 4 - one combined fix).
12. [V] **Farmland trampling**: FIXED in round 3 (env batch) -
    IndevTest_TrampleStep (1-in-4 -> dirt with notify) on genuine step
    events: mob walkDist trigger + new player distanceWalkedModified
    accumulator (0.6x horizontal, foot block at feetY-0.2). gdb-verified
    incl. crop pop via the notify hook.
13. [V] Crops canBlockStay: FIXED in round 3 (env batch) - checkFlowerChange
    runs first on every crop random tick ((light>=8 || light>=4+sky) &&
    farmland below); pop drops 1 wheat at stage 7, nothing earlier
    (Indev_PopCrop, shared with the farmland-removed neighbour pop).
14. [P] Indev lava flow tickRate 25 (ours 30 = c0.30-correct).
15. [V] c0.30 random-tick rate: FIXED in round 3 (env batch) -
    Physics_TickRandomBlocksC030 ports Level.tick's volume/200 loop
    (randId*3+1013904223 LCG, seeded random.nextInt() style per map) for
    c0.30 survival; creative keeps the engine loop. indev_randId now
    seeded per map too (was always 0).
16. [V] Night terrain brightness: FIXED in round 3 (env batch) - public
    IndevTest_BrightnessOfLight curve drives sun/shadow (night 0x20 =
    0.1255 ~ genuine 0.129, rig-verified), eased at most 1 level/tick
    (World.tick + updateDaylightCycle); LightLevel/CurSkyLight gameplay
    reads (crops, zombie burn, spawns) use the eased value.
17. [V] Physics-change notifications: FIXED in round 3 (env batch) -
    Game_UpdateBlock now calls IndevTest_BlockUpdated (setBlockWithNotify
    fan-out) for EVERY mutation: torch pops, crop pops (+wheat), farmland
    cover revert (now immediate), fire lifecycle, chest scatter/TE removal
    (same-container-kind swaps preserved). Validators moved out of the
    UserEvents handler; fire spread's explicit schedules deduped.
    gdb-verified: raw Game_UpdateBlock support removal pops the torch.
18. [V] Torch placement fail: FIXED in round 3 (env batch) -
    IndevTest_CanPlaceBlockAt refuses torches with no normal-cube support
    (genuine 4-walls-or-floor check pre-placement, nothing consumed);
    dead place-then-pop arm deleted. gdb-verified mid-air refusal.
19. RESOLVED (user decision 2026-07-11): death inventory scatter is NOT
    genuine (no dropAllItems in either ground truth) but is KEPT as a
    deliberate deviation because multiplayer support is planned; may
    later be gated behind a multiplayer option when that work starts.
    Notes + code comments corrected.

20. [V] Indev block-ID faithfulness (user-flagged: "pillar and crate present").
    Genuine Indev's block registry (Block.java) ends at 62 (furnace lit).
    ClassiCube's CPE defaults occupy 50-65; we correctly redefine the
    genuine-matching ids - torch(50), fire(51), chest(54), diamond ore(56),
    workbench(58), furnace(61/62) - but 9 ids carried NON-genuine ClassiCube
    blocks into Indev:
      52 sandstone / 53 snow   (genuine = infinite water/lava springs, unimpl.)
      55 light-pink wool       (genuine = gears, unimpl.)
      57 brown wool            (genuine = diamond block, unimpl.)
      59 turquoise wool        (genuine = crops - we host at 85-92)
      60 ice                   (genuine = farmland - we host at 83-84)
      63 pillar / 64 crate / 65 stone brick  (genuine = nothing; Indev stops at 62)
    FIXED: IndevBlocks_Define now sets CanPlace=false + Inventory_Remove for all
    nine, so only genuine Indev blocks are placeable / in the inventory map.
    Indev-only (c0.30-s and plain creative untouched). gdb-verified:
    CanPlace[52,53,55,57,59,60,63,64,65]=0, faithful 50/54/56/58/61=1,
    Inventory.Map[62..65]=0.
    OPEN (internal-id deviation, not user-visible): crops/farmland run at our
    relocated ids 85-92/83-84 instead of genuine 59/60 (the genuine ids were
    taken by ClassiCube CPE wool/ice); .mclevel load remaps genuine->ours. Gears
    (55), diamond block (57) and the water/lava spring blocks (52/53) are simply
    not implemented. Relocating crops/farmland to 59/60 + implementing the rest
    would be full id parity but is a larger, save-format-touching change - left
    as a deliberate, transparent deviation for now.
    CONSISTENCY (user asked "update the generator too?"): no other update needed.
    IndevGen.c places only stone/dirt/grass/sand/gravel/water/lava/ores/plants -
    never a hidden id (verified). The .mclevel remap handles them both ways:
    IndevTest_BlockFromIndev (load) turns genuine 52->water/53->lava/55->air/
    57->iron block/59->crops/60->farmland; _BlockToIndev (save) writes our
    relocated farmland(83)/crops(85+) back as genuine 60/59. So nothing generates
    or deserializes a hidden block - the picker + manual place were the only
    exposures, and both are now closed.

21. [~] Genuine blocks 52/53/55/57 implemented (was: hidden as CPE defaults).
    Confirmed the deobfuscator's "crate" is just the CHEST(54) - so ClassiCube's
    block-64 crate really is non-genuine. Implemented the four real ones at their
    genuine ids (definitions gdb-verified):
      52 waterSource / 53 lavaSource: BlockSource - render like the still fluid,
         hardness 0, and each random tick refill the 4 horizontal air neighbours
         with flowing fluid (Indev_TickSource on Physics.OnRandomTick).
      55 gears/cog: flat 1px walk-through decorative (Collide none, Draw transp),
         hardness 0.5, drops self.
      57 diamond block: opaque cube, hardness 5.0, drops self.
    None are craftable in in-20100223 (verified CraftingManager - no diamond/gear
    recipes), so no recipes added. BlockTo/FromIndev now map all four 1:1; hidden
    list trimmed to 59/60/63/64/65. Un-hidden in-inventory.
    TEXTURES DONE: diamond block's teal tile IS in the beta jar (fetched to atlas
    120->moved to 122 after finding 120 == INDEV_FIRE_TEX_LOC2). Gears' tile is
    Indev-only and Mojang hosts no Indev jar (verified: 901-version manifest, zero
    "in-" ids), so it's EMBEDDED in Resources.c (kz_sea_png-style) at atlas tile
    123. Sources reuse the live water/lava tiles. Gears render is a sprite stand-in
    for the genuine wall-mounted-quad renderType 5. FOLLOW-UP DONE: Builder_DrawGears
    now ports renderType 5 (gear quad flush on each adjacent solid wall, full-cube
    pick bounds, no collision); rig-verified against a wall.
    CROPS/FARMLAND: stay at 85-92 / 83-84 (engine has no runtime block metadata,
    so 8 crop stages can't share one id; no free 8-run at 59). Save format is
    already genuine 59/60+metadata, so this is invisible outside the runtime ids.

---

## Domain 5: Engine robustness (lighting)

### #22 Light query before heightmap allocation -> NULL deref [FIXED]
STATUS: fixed. SEVERITY: crash (intermittent, first-boot).
While rig-verifying the genuine flooded-sky lighting, fresh --singleplayer boots
occasionally aborted with NULL_POINTER_DEREF in ClassicLighting_GetLightHeight+57
(the `classic_heightmap[hIndex]` read). Reached from an Indev light query
(IndevTest_LightLevel -> Lighting.IsLit / FancyLighting_IndevLight -> GetLight
Height) racing the lighting AllocState around the post-load MobSpawner darkness
check. Normal flow allocs lighting during the World_SetNewMap MapLoaded event,
before IndevGen_ApplyPostLoad's initial spawn, so it's a broken-init edge (e.g.
missing-resources first boot) rather than the common path - but a real crash.

FIX (src/Lighting.c): guard ClassicLighting_GetLightHeight - `if (!classic_
heightmap) return -10;` (the empty-all-sky-column sentinel). Genuine-faithful:
Indev has light computed by gen time, so "not computed yet" = FULL DAYLIGHT, not
accidental black - surface spawns behave and monsters don't flood a lit map.
This is the single gameplay entry every light-height query funnels through in
BOTH modes (FancyLighting's IsLit delegates to the classic one), so the one
guard covers BlockPhysics grass, IndevTest grass/leaf/farm ticks, and Survival
mob spawning. The direct classic_heightmap[] derefs that remain are the _Fast
render variants (Color_*_Fast / IsLit_Fast), called only from the chunk builder
- structurally post-AllocState, not gameplay-reachable pre-alloc.

VERIFIED (gdb, live world): forcing classic_heightmap = NULL, GetLightHeight ->
-10, IsLit -> 1 (daylight), IndevTest_LightLevel -> 15, all without a deref;
restoring the pointer resumes normal values (29). Fresh clean generate loads
128x64x128 fancy, surface light 15, no crash.

---

## Domain 6: Fidelity sweep round 1 (items/crafting + player combat)

### #23 Diamond block: missing genuine recipes + pickaxe rules [FIXED]
RecipesIngots.recipeItems is {gold, steel, DIAMOND} - 9 diamonds <-> diamond
block both directions - so the earlier note (finding #21) claiming "no diamond
block recipes in in-20100223" was WRONG (gears/sources part stands). Added the
pair to indevRecipes[] (57 <-> 9x R_ITEM(8)). Also ItemPickaxe lists
blockDiamond in blocksEffectiveAgainst and canHarvestBlock gates it at
harvestLevel >= 2 exactly like oreDiamond - added 57 to pickaxeBlocks and a
case 57 in IndevTest_CanHarvest (it previously dug at 1.0x and dropped for ANY
pickaxe). IndevTest.c.

### #24 Player fire ignition ramp (fireResistance = 20) [FIXED]
Genuine Entity.move: fire contact deals 1/tick and INCREMENTS the fire counter
from -fireResistance; only at 0 does the entity catch alight (fire = 300).
EntityPlayer.fireResistance = 20, so the player needs 20 consecutive burning
ticks to ignite; leaving fire un-ignited (and the water fizz) resets to
-fireResistance. Ours ignited on the FIRST contact tick. Fixed with the signed
counter + PLAYER_FIRE_RESIST 20 in SurvivalTest_Tick; mob insta-ignition kept
(Entity default fireResistance = 1 - genuine). SurvivalTest.c.

### #25 Mushroom soup doesn't return the bowl [FIXED]
ItemSoup.onItemRightClick heals 10 then returns new ItemStack(bowlEmpty); ours
consumed the soup outright (long-standing TODO at IndevTest.c:257). Eating soup
now leaves Bowl (256+25) in the emptied slot. SurvivalTest_TryEat.

### #26 Fire block burned as furnace fuel [FIXED]
getItemBurnTime gives 300 only to Material.wood blocks; BlockFire is
Material.fire. Our SOUND_WOOD proxy excluded only the torch, so the obtainable
fire block (chain-armor ingredient) burned as fuel. Excluded INDEV_BLOCK_FIRE
in Furnace_FuelTime. IndevTest.c.

### #27 Difficulty setting absent [QUEUED]
Genuine EntityPlayer scales mob/arrow damage by difficulty BEFORE the armor
25ths (Peaceful 0, Easy dmg/3+1, Hard dmg*3/2; World default = 2 Normal) and
heals 1 HP per 20 ticks on Peaceful (the ticksExisted % 20 << 2 == 0 precedence
quirk reduces to t%20==0). We have no difficulty concept - current behaviour
exactly matches the default Normal, so nothing is WRONG today; adding the
option (+ the two rules at the SurvivalTest_Damage entry) is queued work.

### Sweep round 1 verified-exact list (no action)
Tool durabilities (32<<tier, golden-hoe 512 quirk), armor durability/reduction
tables, stack sizes, the entire remaining recipe table (shapes, counts, mirror/
offset matching), smelting (6 recipes, 200 ticks, fuel 300/100/1600, ignition-
consumes-fuel), tool speed tiers + drop gating on canHarvestBlock, armor 25ths
with remainder, food heals, attack damage tables (sword 4+2*tier etc., c0.30
flat 4), knockback vectors, drowning/air both modes, lava/fire damage cadence,
invuln windows both modes (c0.30 delta-damage, Indev miss-entirely), player
movement (stock engine classic physics, no overrides).

---

## Domain 7: Fidelity sweep round 2 (blocks + mob AI)

### #28 Genuine plant ticks: sapling staging, flower/mushroom stay + self-drop [FIXED]
BlockSapling.updateTick: light(x,y+1,z) >= 9 AND nextInt(5)==0 per tick climbs
metadata 0-15 (16 successful rolls), THEN tries growTrees, restoring the
sapling on failure; lives on grass/dirt/farmland like all BlockFlower plants.
BlockFlower.canBlockStay: light >= 8 OR (light >= 4 && sky-visible), and pops
by DROPPING ITSELF first (dropBlockAsItem). BlockMushroom.canBlockStay: light
<= 13 + ANY opaque cube below. Our classic handlers grew saplings instantly on
lit grass only, popped flowers whenever un-sunlit (torch-lit flowers died!),
kept mushrooms only on stone/cobble, and never dropped anything. Fixed with
Indev-only handlers (Indev_TickSapling/Flower/Mushroom) registered over the
classic ones per Indev map: genuine light thresholds via the flooded-light
query, farmland soil, self-drops with the genuine 0.7+0.15 scatter, a per-map
sapling-stage side store (round-tripped through the .mclevel Data nibble like
fire age), and a runtime port of World.growTrees (trunk rand(3)+4, clearance
envelope, grass/dirt->dirt below, corner-trimmed diamond canopy). Residual
gap: genuine also re-checks on neighbour change; engine plants are random-tick
only, so a soil-removed plant pops on its next tick instead of instantly.
c0.30 keeps the classic handlers. IndevTest.c.

### #29 Gears: any-item harvest + 0.5 explosion resistance [FIXED]
BlockGears is Material.circuits (never gated by the rock/iron harvest rule)
despite stone sounds - our sound-proxy demanded a pickaxe. setHardness(0.5)
-> effective explosion resistance 0.5; our resistance switch had no case (0).
IndevTest_CanHarvest early-return + resistance case added.

### #30 Diamond block explosion resistance 6.0 [FIXED]
setHardness(5).setResistance(10) -> resistance 30 -> effective 6.0 (the same
group as stone/obsidian). Was falling to the 0 default. (Harvest gating and
dig speed were fixed in #23.) SurvivalTest.c Indev_ExplosionResistance.

### #31 Sand/gravel: fall through fire + void destruction [FIXED]
BlockSand.tryToFall passes through air/fire/water/lava, EXTINGUISHING fire en
route, and a faller that runs past the world bottom is destroyed (the y<0
branch - reachable on floating maps via the getBlockId clamp). Ours stopped on
fire and rested on the invisible floor. Physics_DoFalling now clears fire
cells as it passes (Indev only) and deletes the faller at the bottom of a
floating map instead of resting it. BlockPhysics.c.

### #32 Indev fluids are classic infinite-flood, not finite BlockFlowing [FIXED]
Genuine BlockFlowing: finite fluid - spreading REMOVES a donor cell from the
connected body (World.floodFill/fluidFlowCheck volume conservation), at most
one random-direction spread per update, stagnation rolls (nextInt(3)==0 keeps
trying, else lava petrifies to stone / water evaporates), water converts
adjacent lava to stone + extinguishes fire, still+opposite fluid -> stone
(BlockStationary.onNeighborBlockChange). Ours: the engine's classic infinite
duplication flood (only the 25-tick lava rate and ignition were Indev-ized).
FIXED same session: full port in IndevTest.c - Fluid_FloodFill +
Fluid_FlowCheck (the per-layer scanline walks with the genuine 30000-generation
stamp counter and the x+z<<10 packing), BlockFlowing.update verbatim (downhill
liquidSpread2 with donor removal + the donor sanity test, one shuffled
horizontal spread per update, 1/3+1/3 stagnation with evaporate/petrify, water
extinguish/petrify contacts, lava fireSpread via IndevFire_LavaFlowInto,
settle-to-still), BlockStationary wake/petrify on activation, a scheduled-
update queue at the genuine tickRates (water 5 / lava 25) run from
Physics_Tick, setTickOnLoad scheduling of moving cells at map load, and
canFlow = non-solid non-liquid target + the 5x5x5 sponge veto. BONUS
discovered in the port: fluidFlowCheck is where the SOURCE blocks 52/53 hook
in - a body touching one returns -9999 (infinite supply, no donor) - so
springs now work through the genuine mechanism, and BlockSource.onBlockAdded
now fills its sides on placement too. Classic/c0.30 keep the engine flood
(handlers registered per Indev map only).
RIG-VERIFIED: sealed 7x7 basin, one flowing water block, 200 live ticks ->
still exactly 1 water block (classic flood = ~49 instantly); no crash; map
load with the fluid scan clean. Deviations documented in-code: still-
conversion raises neighbour notifies (genuine setTileNoUpdate doesn't; the
still-wake handler ignores them unless flow is possible, so bodies converge),
still-wake petrify scans the 6 neighbours (engine activations don't carry the
changed block id), the schedule queue dedupes and caps at 4096 entries.
Residual to rig-test next pass: source-spring growth + lava stagnation
petrify (code paths shared with the verified flow-check/update).

### #33 Animal spawns skip the getBlockPathWeight gate [FIXED]
EntityAnimal.getCanSpawnHere ALSO requires getBlockPathWeight >= 0: weight 10
over grass, else lightBrightness - 0.5 - so animals need grass below OR
brightness >= 0.5 (light >= 12). Ours allowed pigs/sheep on any opaque block
at light 9-11. Gate added to Mob_IndevSpawnPass (monsters unaffected).

### #34 c0.30 creeper dim pulse leaked into Indev [FIXED]
Indev's EntityCreeper has no getBrightness override - it renders at normal
mob brightness (fuse blink is a render overlay, still a notes TODO). Our
Mob_GetColor applied the c0.30 damage-dim pulse in both modes; now gated
!IndevTest_Enabled.

### #35 Indev death/shear drops spawned block-scattered instead of at the mob [FIXED]
Entity.entityDropItem spawns AT posX/Y/Z (randomness is velocity-only);
sheep shear spawns at +1.0 Y with extra motion jitter on top. Ours used the
block-mining scatter (floor + rand*0.7 + 0.15) and floored shear coords.
Death drops now spawn at the mob position; shear wool at +1.0 Y with the
genuine extra jitter (SurvivalTest_SpawnDropAtEx exposes the drop for the
velocity add-on).

### Sweep round 2 verified-exact list (no action)
Hardness table (x20 conversions, bedrock sentinel, flowing-lava-0, cloth 16),
all other explosion resistances (obsidian 6.0 via setResistance, flowing lava
1.2 ctor quirk), the full drop table (leaves 1/10 sapling, gravel 1/10 flint,
stone->cobble, no clay in in-20100223), TNT (fuse 80, radius 4, chain 10+r20,
0.3 drop chance, ray-march constants), the entire IndevFire constant set, slab
combining, sponge, mob stat table (health/size/speed/damage/score), wander AI
thresholds, Indev creature AI (256 acquire, 1/20 re-path, 200-sample wander),
creeper fuse/radius both modes, skeleton fire rates both modes, spider
brightness<0.5 aggro + lunge, sheep shear/graze rules, zombie+skeleton
daylight burning formula, spawner caps/light gates/distances/1000-pass init,
death-drop contents, despawn/hurt/knockback/push.

### #36 Genuine runtime ids for visible-metadata blocks [QUEUED - future refactor]
User decision: someday collapse the multi-id workaround (crops 85-92, farmland
83/84, chest/furnace facing 71-82, wall torches 94-97) into genuine single ids
+ a per-position metadata store, like fire age / sapling stage already use.
Storage is trivial (the side-store pattern exists); the real work is renderer
surgery - ClassiCube resolves texture/model/draw-mode purely from block id in
the hot meshing path, so visible metadata needs a position-aware texture hook
in Builder.c plus mesh invalidation on metadata-only changes, and MP would then
need SURV_BLOCKMETA for every visual state change (multi-id streams over plain
SetBlock today - that advantage disappears). Save format is ALREADY genuine
either way. Do this only when something depends on runtime id genuineness.

### #36 addendum: MP resolves the runtime-id question (user design)
Decision: on servers, genuine `id + metadata` is the DATA MODEL (storage, sim,
saves) and the multi-id table is the VIEW/WIRE ENCODING - the server translates
(id, meta) <-> view id at the packet boundary using the IndevTest_BlockToIndev
bijection. Stock CPE clients then see every visible state change over plain
SetBlock + BlockDefinitions, classic clients get per-stage fallbacks, and the
CLIENT runtime ids never need to change - so the #36 refactor is only ever
needed if something starts depending on single-player runtime-id genuineness.
SURV_BLOCKMETA stays reserved for invisible nibbles (sapling stage, fire age)
to make fork-client local saves of server worlds byte-perfect. Recorded in
doc/server-session-handoff.md SS3.5 for the server session.

---

## Domain 8: Audio (user reports, verified by hand after audit agents hit limits)

### #37 Creeper plays the "Steve" hurt sound [VERIFIED GENUINE - no action]
User report investigated: in genuine in-20100223, EntityLiving's defaults are
getHurtSound = getDeathSound = "random.hurt" and getLivingSound = null, and
ONLY EntityPig (mob.pig / mob.pigdeath) and EntitySheep (mob.sheep) override
them. Zombies, skeletons, CREEPERS and spiders have no sound overrides - the
famous mob voices came in Alpha. Our mapping (SurvivalTest.c:3007-3010 hurt/
death, :4333-4334 ambient pig/sheep only) is already byte-genuine, so the
creeper using the player hurt sound is faithful behaviour, kept as-is.

### #38 Missing while-mining hit sound (Indev) [FIXED]
User report confirmed: PlayerControllerSP.sendBlockRemoving:70-80 plays the
target block's STEP sound every 4th digging tick at (soundVolume + 1) / 8
volume and soundPitch * 0.5 - the low mining "thunk". Our TickBreaking had no
sound at all, so timed mining (most obvious on wood's long dig) was silent.
Added Audio_PlayDigHitSound (step board, rate 50, quarter volume) called on
the genuine %4 cadence in the Indev branch of SurvivalTest_TickBreaking.
c0.30 intentionally stays silent while mining - genuine SurvivalGameMode.
hitBlock spawns particles only, no sound.

### Remaining audio + drops/HUD audit [QUEUED - relaunch after quota reset]
The two audit agents (full audio sweep: block sound table, break transforms,
walk cadence, player sounds, music delays; and drops/arrows/HUD formulas)
died to session limits before reporting, twice. Relaunch the two prompts
(recorded in the session transcript) when the limit resets; the user-facing
bugs from the batch are already resolved above.

---

## Domain 8 continued: full audio sweep + drops/arrows/HUD (final sweep round)

### #39 Metal blocks demoted to stone sounds (Indev) [FIXED]
Gold/iron/diamond blocks + gears use soundMetalFootstep ("stone" samples at
pitch 1.5) in Block.java; ours had them SOUND_STONE (pitch 1.0). The engine's
SOUND_METAL is exactly the genuine transform (stone samples, dig rate 120 =
1.5*0.8), so all four now use SOUND_METAL. Gears' any-item harvest exemption
is id-based and unaffected.

### #40 c0.30 sound-type table corrections [FIXED]
Per Tile$SoundType + Block.java: DIRT is grass (was engine gravel), SAND is
gravel for FOOTSTEPS too (dig was already fixed), sapling/flowers/mushrooms
are SoundType.none (silent break), sponge/TNT are cloth = grass samples at
pitch 1.2 (Sounds_PlayScaled remaps SOUND_CLOTH->grass*1.2 in c0.30), and the
metal pitch base is 2.0 (c0.30 dig/step rates now 160/200). All c0.30-only;
Indev keeps its verified table.

### #41 Indev plays sounds EXACTLY; randomization is c0.30-only [FIXED]
Indev SoundManager.playSound uses the given volume/pitch verbatim; the
per-play rolls (getPitch /(rand*0.2+0.9), getVolume /(rand*0.4+1)) are c0.30's
Tile$SoundType. Our roll was gated on SurvivalTest_Enabled (both modes) - now
c0.30-only. Also Indev walk steps now play at the genuine soundVolume * 0.15
(was the engine's /2), and c0.30 block PLACING is silent (the place sound is
Indev's ItemBlock.onItemUse; c0.30's only sound sites are break + walk).

### #42 Indev music gap 600-1200 s [FIXED]
SoundManager: rand(12000) + 12000 ticks after track end. Default min delay now
600 s in Indev (c0.30 keeps its genuine 300 + rand(900)); user-set values win.

### #43 Indev fall-damage landing thud [FIXED]
EntityLiving.fall plays the block-under's step sound at volume*0.5, pitch*0.75
on damaging landings. Added Audio_PlayFallSound (step board, rate 75, half
volume) for player and mob falls, Indev only.

### #44 Drops/arrows/HUD micro-fixes [FIXED]
(a) air-bubble row now flush atop the hearts (1px gap, was 2) - the unfixed
half of the earlier hearts alignment; (b) drop lava fizz-bounce/push-out now
run AFTER the tick's gravity like genuine (kick was netting 0.16/tick, now
0.2); (c) Indev pickup fly-in eases to eye-0.5 (EntityPickupFX) while c0.30
keeps y-1.0 (TakeEntityAnim); (d) skeleton arrows get the EntityArrow ctor
offsets (-0.1 Y, 0.16 sideways) the player bow already applied.

### #45 Indev GUI button click sound [QUEUED - needs asset]
GuiScreen plays "random.click" at 0.25 volume on button press; our soundboard
has no click group. Needs the click sample sourced/embedded before wiring.

### Final sweep verified-exact list (audio + drops/HUD)
Rest of the Indev block sound table, break/place transforms, mining-hit #38,
bow/drr/pop/fuse/explode/splash/all-three-fizz/ignite/fire-ambient constants,
hurt pitch, absent eating/container sounds (genuine), liquid ambient dead code,
c0.30 music timer, 16-block range; EntityItem physics/pickup/render constants,
EntityArrow both-mode ports line-by-line, all HUD layout/flash/armor/bubble/
jitter formulas, held-block transforms, c0.30 arrows counter.

---

## Domain 9: Physics fidelity sweep — explosions/TNT, fluids, fire, growth

Run against the deobfuscated in-20100223 tree AND the real c0.30 jar (see the
header for how to obtain both). Four domain finders completed; the adversarial
verify pass got through the first five findings (all CONFIRMED, all fixed the
same session) before hitting a usage limit — every remaining [P] item below is
a LEAD that must be independently verified against the genuine source before
acting on it. The verified-exact counts are the negative space: everything
those finders checked that matched.

Severity/side legend as reported by the finder: side = which port diverges
(sp-mp-split = the two ports disagree with each other).

### explosions (9 findings, 24 verified-exact)

- **[V] FIXED** (critical, sp-mp-split) Server c0.30 explosions destroy blast-proof (non-explodable) blocks
  - genuine: c0.30 Level.explode only destroys a tile if its explodable flag is set: bytecode `206: getstatic tile/a.b ... 212: invokevirtual com/mojang/minecraft/level/tile/a.i:()Z; 215: ifeq 264` (skip block). Tile.i() returns field `ap`, and tile/a's static initializer sets ap=false (`icon
  - ours: The server's classic-mode sphere path marks EVERY in-radius cell and DestroyMarked clears every non-air marked block with no explodable filter - stone, cobblestone, ores, metal blocks, slabs, brick, obsidian and even bedrock/admincrete map
  - at: c030 Level.class explode (offsets 190-264) + tile/a.class static{} ap=false sites (offsets 77-82, 154-159, 233-238, 416-421, 449-454, 482-487, 1091-1096, 1125-1130, 1156-1161, 1187-1192, 1219-1224, 1298-1303, 1331-1336) vs server /home/user/mcgalaxy/MCGalaxy/Network/SurvivalExplosions.cs:105-118 (sphere marks all) + 161-184 (DestroyMarked clears all non-air); client correct at /home/user/ClassiCube/src/SurvivalTest.c:16
- **[V] FIXED** (medium, sp-mp-split) Server c0.30 explosion drops use the Indev drop table, not the c0.30 per-tile drops
  - genuine: c0.30 Level.explode calls each tile's own drop routine with 0.3 chance: bytecode `231: ldc 0.3f; 233: invokevirtual tile/a.a:(L...Level;IIIF)V`. Per-tile c0.30 counts/ids differ from Indev, e.g. log tile e: `f(): nextInt(3)+3` (3-5 drops) and `g(): planks id` - an exploded log yi
  - ours: SurvivalExplosions.DestroyMarked routes BOTH modes through the Indev-style ExplodeDrops table: log drops 1 LOG (not 3-5 planks), gravel rolls 1/10 for an Indev flint ITEM (id 256+62, nonexistent in c0.30), coal ore would drop the Indev coal
  - at: c030 Level.class explode offsets 218-235; tile/e.class f()/g(); tile/g.class f() vs server /home/user/mcgalaxy/MCGalaxy/Network/SurvivalExplosions.cs:186-225 (mode-blind Indev table); client correct at /home/user/ClassiCube/src/SurvivalTest.c:716-785 + 867-875
- **[V] FIXED** (medium, sp-mp-split) MP explosions never knock players back (Indev velocity kick and c0.30 hurt knockback both missing)
  - genuine: Indev World.java:1185-1188: `var36.attackEntityFrom(var1, (int) ((var43 * var43 + var43) / 2.0F * 8.0F * var5 + 1.0F)); var36.motionX += var26 * var43; var36.motionY += var39 * var43; var36.motionZ += var40 * var43;` - every entity in range, the player included, gets the dir*f ve
  - ours: The client SP applies the kick faithfully (e->Velocity += d/dist * f). The server's ExplodeAt builds the kick only for MOBS (BlastHit.KX/KY/KZ, applied at line 969); players just get SurvivalNet.DamagePlayer - KnockbackPlayer (which already
  - at: /tmp/claude-0/-home-user/ebc9ea10-f533-5652-9e7f-ec7fb09f7000/scratchpad/indev/src/game/java/net/minecraft/game/level/World.java:1185-1188 vs server /home/user/mcgalaxy/MCGalaxy/Network/SurvivalMobs.cs:907-930 + 963-964 (players: damage only) vs 956/969 (mobs get KX/KY/KZ); KnockbackPlayer unused for blasts (/home/user/mcgalaxy/MCGalaxy/Net
- **[V] FIXED** (medium, both) Indev explosions never damage item drops or pop paintings (both ports)
  - genuine: World.createExplosion damages EVERY entity within 2r (World.java:1162-1189), and EntityItem.java:149-156 reads `public final boolean attackEntityFrom(Entity var1, int var2) { this.health -= var2; if(this.health <= 0) { this.setEntityDead(); } return false; }` (health 5, so any ne
  - ours: Both ports damage only the player and mobs: client Indev_CreateExplosion iterates Entities.CurPlayer + st_mobs only; server ExplodeAt iterates online players + lm.Mobs only. Drop entities (SurvivalDrops / st_drops) and paintings are untouch
  - at: /tmp/claude-0/-home-user/ebc9ea10-f533-5652-9e7f-ec7fb09f7000/scratchpad/indev/src/game/java/net/minecraft/game/entity/misc/EntityItem.java:149-156; .../entity/EntityPainting.java:192-196; .../level/World.java:1162-1189 vs client /home/user/ClassiCube/src/SurvivalTest.c:3589-3641; server /home/user/mcgalaxy/MCGalaxy/Network/SurvivalMobs.cs:910-958
- **[V] FIXED** (medium, sp-mp-split) MP explosions are silent - random.explode never reaches multiplayer clients
  - genuine: Indev World.java:1103-1104: `this.playSoundAtPlayer(var2, var3, var4, "random.explode", 4.0F, (1.0F + (this.random.nextFloat() - this.random.nextFloat()) * 0.2F) * 0.7F);` - volume 4 makes the blast audible out to ~64 blocks, before any block is touched.
  - ours: The client plays the sound only in its LOCAL sim paths (Indev_CreateExplosion and the c0.30 SurvivalTest_Explode), which are gated off in MP. The server sends no sound message for detonations, and the client's SURV_TNT_REMOVE handler (Survi
  - at: /tmp/claude-0/-home-user/ebc9ea10-f533-5652-9e7f-ec7fb09f7000/scratchpad/indev/src/game/java/net/minecraft/game/level/World.java:1103-1104 vs client /home/user/ClassiCube/src/SurvivalTest.c:2210-2223 (NetTntRemove: particles only) vs 3544-3545/3714-3715 (SP only); server: no sound sender anywhere in /home/user/mcgalaxy/MCGalaxy/Network/Surv
- **[P]** (low, sp-mp-split) Server primed-TNT hop velocity is a corrected uniform direction, not the genuine double-converted drift
  - genuine: EntityTNTPrimed.java:17-20: `float var5 = (float)(Math.random() * (double)((float)Math.PI) * 2.0D); this.motionX = -MathHelper.sin(var5 * (float)Math.PI / 180.0F) * 0.02F; this.motionY = 0.2F; this.motionZ = -MathHelper.cos(var5 * (float)Math.PI / 180.0F) * 0.02F;` - the radians
  - ours: The client reproduces the bug verbatim (`-Math_SinF(ang * MATH_DEG2RAD) * 0.02f`). The server 'fixes' it: `VX = -Math.Sin(ang) * 0.02, VZ = -Math.Cos(ang) * 0.02` with ang in [0,2pi) - a uniformly random horizontal hop of magnitude 0.02 - a
  - at: /tmp/claude-0/-home-user/ebc9ea10-f533-5652-9e7f-ec7fb09f7000/scratchpad/indev/src/game/java/net/minecraft/game/entity/misc/EntityTNTPrimed.java:17-20 (c0.30: PrimedTnt.class ctor offsets 36-88) vs server /home/user/mcgalaxy/MCGalaxy/Network/SurvivalTnt.cs:128,134; client (correct) /home/user/ClassiCube/src/SurvivalTest.c:2026-2029
- **[P]** (low, sp-mp-split) Server fire-ignited TNT always leaves air; genuine leaves FIRE in the cell 50% of the time
  - genuine: BlockFire.java:114-127 tryToCatchBlockOnFire: `boolean var8 = var1.getBlockId(...) == Block.tnt.blockID; if (var6.nextInt(2) == 0) { var1.setBlockWithNotify(var2, var3, var4, this.blockID); } else { var1.setBlockWithNotify(var2, var3, var4, 0); } if (var8) { Block.tnt.onBlockDest
  - ours: The client is faithful (Fire_TryCatch keeps the 50% fire-vs-air roll, then arms the TNT). The server special-cases TNT before the roll: `if (b == Block.TNT) { SetFire(lvl, x, y, z, Block.Air); SurvivalTnt.Ignite(lvl, x, y, z, DefaultFuse(lv
  - at: /tmp/claude-0/-home-user/ebc9ea10-f533-5652-9e7f-ec7fb09f7000/scratchpad/indev/src/game/java/net/minecraft/game/level/block/BlockFire.java:114-127 vs server /home/user/mcgalaxy/MCGalaxy/Network/SurvivalPhysics.cs:302-306; client (correct) /home/user/ClassiCube/src/IndevFire.c:166-185
- **[P]** (low, client) Client c0.30 explosions play the Indev explosion sound; genuine c0.30 blasts are silent
  - genuine: c0.30 Level.explode's full bytecode contains no playSound invocation (Level has playSound methods, but neither explode, PrimedTnt.tick's detonation branch, nor Creeper$1.beforeRemove calls one) - Survival Test explosions produce particles only, no sound.
  - ours: SurvivalTest_Explode (the c0.30 path) plays MOBSND_EXPLODE at volume 4 with the INDEV pitch formula before the block loop - an extra rule imported from Indev's World.createExplosion into c0.30 mode.
  - at: c030 Level.class explode (offsets 0-384, no audio call); PrimedTnt.class tick offsets 146-337; Creeper$1.class beforeRemove offsets 0-203 vs client /home/user/ClassiCube/src/SurvivalTest.c:3712-3715
- **[P]** (low, both) Primed-TNT pool caps disagree between sides and change overflow outcomes
  - genuine: Neither Indev nor c0.30 caps the number of primed TNT entities: BlockTNT.onBlockDestroyedByExplosion (Indev) / tile j.f (c0.30) unconditionally `spawnEntityInWorld`/`addEntity` a new PrimedTnt for every TNT block consumed by a blast.
  - ours: The client pool is 64: overflow converts the TNT into a pickup item instead of priming it (documented fallback). The server cap is 128: overflow silently returns from Ignite AFTER the block was already cleared, so the TNT vanishes without e
  - at: /tmp/claude-0/-home-user/ebc9ea10-f533-5652-9e7f-ec7fb09f7000/scratchpad/indev/src/game/java/net/minecraft/game/level/block/BlockTNT.java:22-26; c030 tile/j.class f(Level,III) vs client /home/user/ClassiCube/src/SurvivalTest.c:1998-2006 and 2193; server /home/user/mcgalaxy/MCGalaxy/Network/SurvivalTnt.cs:57,130

### fluids (11 findings, 22 verified-exact)

- **[V] FIXED** (critical, client) Client: physics-driven block changes never wake adjacent still fluids (no setBlockWithNotify equivalent for fluids)
  - genuine: World.java:335-342 'public final boolean setBlockWithNotify(...) { if (this.setBlock(var1, var2, var3, var4)) { this.notifyBlocksOfNeighborChange(var1, var2, var3, var4); return true; }' — every fluid/fire/explosion write notifies all 6 neighbours, and BlockStationary.java:18-56
  - ours: The client's notify hook IndevTest_BlockUpdated (IndevTest.c:3106-3153, run for every Game_UpdateBlock) handles crops/farmland/containers/fire/torches but never activates still fluids. IndevFluid_ActivateStill (IndevTest.c:2230) only runs v
  - at: World.java:335-351 + BlockStationary.java:18-58 vs client /home/user/ClassiCube/src/IndevTest.c:3106-3153 (missing wake) vs server /home/user/mcgalaxy/MCGalaxy/Network/SurvivalPhysics.cs:154-160 (has it)
- **[V] FIXED** (critical, client) Client: mining a sponge on an Indev map triggers the CLASSIC infinite water flood (waterQ leak)
  - genuine: BlockSponge.java:25-34 'public final void onBlockRemoval(World var1, ...) { for(int var5 = var2 - 2; var5 <= var2 + 2; ++var5) { ... var1.notifyBlocksOfNeighborChange(var5, var6, var7, var1.getBlockId(var5, var6, var7)); } }' — removal just notifies the ±2 cube so the FINITE flui
  - ours: Physics.OnDelete[BLOCK_SPONGE] = Physics_DeleteSponge stays registered on Indev maps (BlockPhysics.c:632; Indev_RegisterFarmTicks never overrides it). Physics_DeleteSponge (BlockPhysics.c:521-540) enqueues the ±3 shell's water into the clas
  - at: BlockSponge.java:25-34 vs client /home/user/ClassiCube/src/BlockPhysics.c:632,658-659,521-540,454-477
- **[V] FIXED** (medium, sp-mp-split) Server: sponge does not absorb water on placement and removal only wakes 6 direct neighbours (genuine notifies the ±2 cube)
  - genuine: BlockSponge.java:12-23 'public final void onBlockAdded(World var1, ...) { for(int var5 = var2 - 2; var5 <= var2 + 2; ++var5) { ... if(var1.isWater(var5, var6, var7)) { var1.setBlock(var5, var6, var7, 0); } } }' absorbs all water-material blocks in the 5x5x5 cube on placement; onB
  - ours: SurvivalPhysics has no sponge handling at all beyond the canFlow veto (SurvivalPhysics.cs:431-436): placing a sponge next to MP Indev water removes nothing (the water just sits inside the exclusion zone), and mining a sponge only fires Noti
  - at: BlockSponge.java:12-34 vs server /home/user/mcgalaxy/MCGalaxy/Network/SurvivalPhysics.cs:431-436 (only the canFlow veto; no absorb) vs client /home/user/ClassiCube/src/BlockPhysics.c:503-519 (absorbs)
- **[V] FIXED** (medium, both) Both: map-border shell is writable, so the edge ocean drains instead of being infinite and fluid spreads into cells genuine cannot touch
  - genuine: World.java:297-298 'public final boolean setBlock(int var1, int var2, int var3, int var4) { if (var1 > 0 && var2 > 0 && var3 > 0 && var1 < this.width - 1 && var2 < this.height - 1 && var3 < this.length - 1) {' — every runtime write to the outer shell silently fails. Consequently
  - ours: Server SurvivalGrowth.SetView (SurvivalGrowth.cs:117) accepts the full 0..dim-1 range, and client Game_UpdateBlock has no shell guard, so FluidSpread2/FlowCheck donor removal (server SurvivalPhysics.cs:592,626; client IndevTest.c:2091,2137)
  - at: World.java:297-298 + BlockFlowing.java:173-178 vs server /home/user/mcgalaxy/MCGalaxy/Network/SurvivalGrowth.cs:117 + SurvivalPhysics.cs:588-592; client /home/user/ClassiCube/src/IndevTest.c:2087-2091 + BlockPhysics.c:151-167
- **[V] FIXED** (medium, both) Both: still-fluid petrify direction inverted for pre-existing water/lava contact (self petrifies instead of waking and petrifying the lava)
  - genuine: BlockStationary.java:40-47 'if (var5 != 0) { Material var7 = Block.blocksList[var5].material; if (this.material == Material.water && var7 == Material.lava || var7 == Material.water && this.material == Material.lava) { var1.setBlockWithNotify(var2, var3, var4, Block.stone.blockID)
  - ours: ActivateStill scans all 6 CURRENT neighbours and petrifies ITSELF when any is the opposite material (server SurvivalPhysics.cs:701-707; client IndevTest.c:2241-2249). With map-gen still water adjacent to still lava, mining an unrelated ston
  - at: BlockStationary.java:40-47 vs server /home/user/mcgalaxy/MCGalaxy/Network/SurvivalPhysics.cs:701-707; client /home/user/ClassiCube/src/IndevTest.c:2241-2249
- **[V] FIXED** (medium, server) Server: placed source block does not fill its 4 sides immediately (waits for a random tick, ~10 s average); client fills instantly
  - genuine: BlockSource.java:16-33 'public final void onBlockAdded(World var1, ...) { super.onBlockAdded(...); if (var1.getBlockId(var2 - 1, var3, var4) == 0) { var1.setBlockWithNotify(var2 - 1, var3, var4, this.fluid); } ... }' — placing a spring floods the 4 horizontal air neighbours immed
  - ours: Server Notify (SurvivalPhysics.cs:148-152) handles only FIRE/Water/Lava for newV; WATER_SRC/LAVA_SRC placement schedules nothing, so the source sits dry until the volume/200 random pass hits it (SurvivalGrowth.cs:378 → RandomTickSource). Th
  - at: BlockSource.java:16-33 vs server /home/user/mcgalaxy/MCGalaxy/Network/SurvivalPhysics.cs:148-152 (missing) vs client /home/user/ClassiCube/src/IndevTest.c:2442-2443 (present)
- **[P]** (low, both) Both: flammable-neighbour wake of still fluids restricted to lava (genuine applies it to still WATER too, and tests the changed block id, not a neighbour scan)
  - genuine: BlockStationary.java:49-51 'if (Block.fire.getChanceOfNeighborsEncouragingFire(var5)) { var6 = true; }' — unconditional on material: a still WATER with no flow targets also wakes when a flammable block (planks, logs, leaves, wool, TNT, bookshelf) changes beside it, and its moving
  - ours: Both ports gate the flammable check on lava only: server SurvivalPhysics.cs:712 'if (!wake && !water)' and client IndevTest.c:2254 'if (!wake && !water)'. Still water never wakes for a flammable placement, so e.g. an enclosed still-water po
  - at: BlockStationary.java:49-51 vs server /home/user/mcgalaxy/MCGalaxy/Network/SurvivalPhysics.cs:712-718; client /home/user/ClassiCube/src/IndevTest.c:2254-2260
- **[P]** (low, both) Both: fluid update period is one tick shorter for cascade-scheduled cells (5/25 vs genuine 6/26), and the genuine shared 200-entries-per-tick cap is absent for fluids
  - genuine: World.java:553-556 'int var6 = this.tickList.size(); if (var6 > 200) { var6 = 200; }' snapshots the list size before processing, so entries scheduled during a tick get their first decrement the NEXT tick — with World.java:562-564 '--var8.scheduledTime; this.tickList.add(var8);' a
  - ours: Both drivers iterate a growing list ('for (int i = 0; i < lp.Fluid.Count; )' SurvivalPhysics.cs:404; 'for (i = 0; i < indev_fluidSchedCount; )' IndevTest.c:2207), so entries appended mid-pass (the dominant flow-cascade case, since spread sc
  - at: World.java:553-577 + 719-727 vs server /home/user/mcgalaxy/MCGalaxy/Network/SurvivalPhysics.cs:403-421; client /home/user/ClassiCube/src/IndevTest.c:2202-2216
- **[P]** (low, both) Both: schedule dedup by cell index only, dropping genuine's per-blockID stale-entry semantics
  - genuine: World.java:719-726 adds duplicate NextTickListEntry freely and World.java:571-573 'byte var9 = this.blocks[...]; if (var9 == var8.blockID && var9 > 0) { Block.blocksList[var9].updateTick(...); }' skips entries whose recorded block id no longer matches — a stale water entry never
  - ours: Server dedups via FluidPending HashSet on index (SurvivalPhysics.cs:380) and client via a linear index scan (IndevTest.c:2054-2055), then both run whatever fluid currently sits there (SurvivalPhysics.cs:417-418; IndevTest.c:2213-2214). Cons
  - at: World.java:571-573 + 719-727 vs server /home/user/mcgalaxy/MCGalaxy/Network/SurvivalPhysics.cs:379-385,415-419; client /home/user/ClassiCube/src/IndevTest.c:2051-2062,2213-2214
- **[P]** (low, both) Both: map load schedules every moving-fluid cell in one burst (genuine has no load-time scan; setTickOnLoad only enables random ticks)
  - genuine: World.java:589-592 'byte var15 = this.blocks[...]; if (Block.tickOnLoad[var15]) { Block.blocksList[var15].updateTick(this, var14, var13, var10, this.random); }' — tickOnLoad is consulted only by the random-update pass; there is no whole-map scheduling scan at load (pending update
  - ours: Server EnsureLoaded (SurvivalPhysics.cs:193-206) and client IndevTest_FluidsOnMapLoaded (IndevTest.c:2274-2282) scan the whole volume and schedule every Water/Lava cell, so a freshly loaded map with many suspended moving cells (e.g. an in-p
  - at: World.java:39,579-593 vs server /home/user/mcgalaxy/MCGalaxy/Network/SurvivalPhysics.cs:193-206; client /home/user/ClassiCube/src/IndevTest.c:2274-2282
- **[P]** (low, both) Both: source blocks classified by their own fluid; genuine gives BOTH sources Material.water, changing several corner-case gates
  - genuine: BlockSource.java:10-11 'protected BlockSource(int var1, int var2) { super(var1, Block.blocksList[var2].blockIndexInTexture, Material.water);' — the LAVA source is Material.water too. Hence (a) BlockFlowing.update's equalize gate 'var1.getBlockMaterial(var2, var3 - 1, var4) == thi
  - ours: Both ports put each source in its own fluid class: server IsWaterMat/IsLavaMat (SurvivalPhysics.cs:52-53) and client Fluid_IsWaterMat/Fluid_IsLavaMat (IndevTest.c:1884-1889), used in the equalize gate (SurvivalPhysics.cs:618; IndevTest.c:21
  - at: BlockSource.java:10-11 + BlockFlowing.java:43 vs server /home/user/mcgalaxy/MCGalaxy/Network/SurvivalPhysics.cs:52-53,618; client /home/user/ClassiCube/src/IndevTest.c:1884-1889,2126-2127

### fire (9 findings, 29 verified-exact)

- **[V] FIXED** (medium, server) Server: TNT consumed by fire never leaves a fire block behind (genuine does 50% of the time)
  - genuine: BlockFire.tryToCatchBlockOnFire: "boolean var8 = var1.getBlockId(var2, var3, var4) == Block.tnt.blockID; if (var6.nextInt(2) == 0) { var1.setBlockWithNotify(var2, var3, var4, this.blockID); } else { var1.setBlockWithNotify(var2, var3, var4, 0); } if (var8) { Block.tnt.onBlockDest
  - ours: Server FireTryCatch special-cases TNT before the coin flip: "if (b == Block.TNT) { SetFire(lvl, x, y, z, Block.Air); SurvivalTnt.Ignite(...); return; }" - the cell is always set to air, never fire. The client is correct (IndevFire.c:173-184
  - at: BlockFire.java:114-129 (tryToCatchBlockOnFire) vs server /home/user/mcgalaxy/MCGalaxy/Network/SurvivalPhysics.cs:302-305 (wrong); client /home/user/ClassiCube/src/IndevFire.c:171-184 (correct)
- **[V] FIXED** (medium, server) Server: fire-support test uses IsSolid instead of isOpaqueCube - fire can rest on glass/slabs
  - genuine: World.isBlockNormalCube: "Block var4 = Block.blocksList[this.getBlockId(var1, var2, var3)]; return var4 == null ? false : var4.isOpaqueCube();" (World.java:399-402). Fire's support/burnout tests use it (BlockFire.java:64 "if (!var1.isBlockNormalCube(var2, var3 - 1, var4) || var6
  - ours: Server NormalCube: "return CollideType.IsSolid(lvl.CollideType(lvl.GetBlock(...)))" (SurvivalPhysics.cs:129-132) - glass and slabs ARE solid-collide, so server-side fire on a glass/slab top with no flammable neighbour survives ~4-5 schedule
  - at: World.java:399-402; BlockFire.java:64,154,158 vs server /home/user/mcgalaxy/MCGalaxy/Network/SurvivalPhysics.cs:129-132 (wrong); client /home/user/ClassiCube/src/IndevFire.c:136-139 (correct)
- **[P]** (low, server) Server: onBlockAdded/onNeighborBlockChange fire validation is deferred ~1s instead of inline
  - genuine: BlockFire.onNeighborBlockChange: "if (!var1.isBlockNormalCube(var2, var3 - 1, var4) && !this.canNeighborCatchFire(var1, var2, var3, var4)) { var1.setBlockWithNotify(var2, var3, var4, 0); }" (BlockFire.java:157-161), and onBlockAdded identically at 163-169 - both run synchronously
  - ours: Server Notify only ENQUEUES a scheduled fire update (SurvivalPhysics.cs:140-162 NotifyNeighbour -> ScheduleFire with Time=20, :235-238), so server-authoritatively the fire persists ~21 ticks (~1.05s) before FireUpdate's support check remove
  - at: BlockFire.java:157-169; World.java:310-316,335-342 vs server /home/user/mcgalaxy/MCGalaxy/Network/SurvivalPhysics.cs:140-162,235-238; client /home/user/ClassiCube/src/IndevFire.c:286-316 (correct)
- **[P]** (low, both) Both: fire can spread into / burn blocks in the outermost boundary shell (genuine setBlock is interior-only)
  - genuine: World.setBlock: "if (var1 > 0 && var2 > 0 && var3 > 0 && var1 < this.width - 1 && var2 < this.height - 1 && var3 < this.length - 1) {...} else return false" (World.java:297-298) - every fire write (spread, burn-to-air, tryToCatchBlockOnFire) silently fails on cells with any coord
  - ours: Both ports allow the full 0..dim-1 range: client Fire_Set gates on World_Contains only (IndevFire.c:157-161) and Fire_SpreadCheck returns false for out-of-range (IndevFire.c:387-394); server SetFire/In likewise (SurvivalPhysics.cs:117-122,
  - at: World.java:297-298; BlockFire.java:271-281 vs client /home/user/ClassiCube/src/IndevFire.c:157-161,387-394; server /home/user/mcgalaxy/MCGalaxy/Network/SurvivalPhysics.cs:117-122,355-362
- **[P]** (low, both) Both: flint & steel loses durability on world-boundary clicks (genuine does not)
  - genuine: ItemFlintAndSteel.onItemUse: "if(var3 > 0 && var4 > 0 && var5 > 0 && var3 < var2.width - 1 && ...) { ... var1.damageItem(1); return true; } else { return false; }" (ItemFlintAndSteel.java:38-49) - damageItem(1) is INSIDE the interior-bounds branch; a click whose face-adjusted tar
  - ours: Client: "SurvivalTest_DamageHeldItem(1); return true;" runs unconditionally after the bounds check (IndevFire.c:349-350); the comment at IndevFire.c:338-340 claims genuine "damages the item unconditionally", which is incorrect. Server match
  - at: ItemFlintAndSteel.java:38-49 vs client /home/user/ClassiCube/src/IndevFire.c:336-350; server /home/user/mcgalaxy/MCGalaxy/Network/SurvivalInventory.cs:597-618
- **[P]** (low, sp-mp-split) MP only: no "fire.ignite" sound when flint & steel places fire
  - genuine: ItemFlintAndSteel.onItemUse: "var2.playSoundAtPlayer((float)var3 + 0.5F, ..., \"fire.ignite\", 1.0F, rand.nextFloat() * 0.4F + 0.8F);" (ItemFlintAndSteel.java:41) before setting the fire block.
  - ours: SP client plays it (IndevFire.c:344-346). In MP the right-click leaves as SURV_USE_ITEM (SurvivalTest.c:7781-7790) and the server's UseFlintSteel places the fire with no sound packet (SurvivalInventory.cs:597-619), so nobody hears the ignit
  - at: ItemFlintAndSteel.java:41 vs server /home/user/mcgalaxy/MCGalaxy/Network/SurvivalInventory.cs:597-619; client MP path /home/user/ClassiCube/src/SurvivalTest.c:7781-7790
- **[P]** (low, server) Server: primed-TNT kick velocity skips Notch's double angle conversion (affects fire-ignited TNT)
  - genuine: EntityTNTPrimed ctor: "float var5 = (float)(Math.random() * Math.PI * 2.0D); this.motionX = -MathHelper.sin(var5 * (float)Math.PI / 180.0F) * 0.02F; ... this.motionZ = -MathHelper.cos(var5 * (float)Math.PI / 180.0F) * 0.02F;" (EntityTNTPrimed.java:17-20) - the radians value is co
  - ours: Server Ignite: "VX = -Math.Sin(ang) * 0.02, VY = 0.2, VZ = -Math.Cos(ang) * 0.02" (SurvivalTnt.cs:128-134) - a uniformly random horizontal direction at full 0.02 magnitude; its comment says the magnitude is what matters, but genuine's direc
  - at: EntityTNTPrimed.java:17-20 vs server /home/user/mcgalaxy/MCGalaxy/Network/SurvivalTnt.cs:124-134 (wrong); client /home/user/ClassiCube/src/SurvivalTest.c:2022-2029 (correct)
- **[P]** (low, server) Server: fire age (metadata nibble) is not persisted across map unload/save
  - genuine: Fire age lives in the world's data nibble: "byte var6 = var1.getBlockMetadata(var2, var3, var4); if (var6 < 15) { var1.setBlockMetadata(var2, var3, var4, var6 + 1); ... }" (BlockFire.java:57-61), and the data array is part of the saved level, so a mid-burn fire resumes at its sav
  - ours: Server FireAge is a plain in-memory byte[] referenced only inside SurvivalPhysics.cs (lines 62, 222-233); no sidecar/persistence code touches it, so unloading/reloading a level resets every burning fire to age 0 - it restarts its full 16-st
  - at: BlockFire.java:57-61; World.java:810-858 (data nibble store) vs server /home/user/mcgalaxy/MCGalaxy/Network/SurvivalPhysics.cs:62,222-233 (memory only); client /home/user/ClassiCube/src/IndevTest.c:727,732 (persisted)
- **[P]** (low, both) Both: scheduled-update queue is bounded at 8192 entries (genuine list is unbounded)
  - genuine: World.scheduleBlockUpdate: "NextTickListEntry var5 = new NextTickListEntry(...); ... this.tickList.add(var5);" (World.java:719-727) - an unconditional add to an unbounded List; only PROCESSING is capped at 200/tick (World.java:553-556).
  - ours: Both ports drop new schedules when 8192 entries are queued: client Fire_Schedule "if (fire_qCount >= FIRE_QUEUE_LEN) return;" (IndevFire.c:98-100) and the requeue guard at IndevFire.c:260; server ScheduleFire "if (lp.FireQueue.Count >= FIRE
  - at: World.java:719-727,553-556 vs client /home/user/ClassiCube/src/IndevFire.c:93-105,258-264; server /home/user/mcgalaxy/MCGalaxy/Network/SurvivalPhysics.cs:82,235-238,245-247

### growth (21 findings, 32 verified-exact)

- **[V] FIXED** (critical, both) Grass spread gated on binary sky-exposure instead of light >= 9 source / >= 4 target: spreads at night, never by torchlight
  - genuine: BlockGrass.java:25-33: "if (var1.getBlockLightValue(var2, var3 + 1, var4) >= 9) { var2 = var2 + var5.nextInt(3) - 1; ... if (var1.getBlockId(var2, var3, var4) == Block.dirt.blockID && var1.getBlockLightValue(var2, var3 + 1, var4) >= 4 && !var1.getBlockMaterial(var2, var3 + 1, var
  - ours: Server SurvivalGrowth.cs TickGrass: 'if (!IsLit(lvl, x, y, z)) return;' for the source and 'if (!IsLit(lvl, tx, ty, tz)) return;' for the target - pure time-independent sky exposure, no LightLevel/CurrentSkyLight call at all. Client IndevTe
  - at: BlockGrass.java:25-33, World.java:512-519, Light.java:251, Material.java:33-35 vs mcgalaxy/MCGalaxy/Network/SurvivalGrowth.cs:393-401; ClassiCube/src/IndevTest.c:2506-2515
- **[V] FIXED** (critical, client) c0.30 grass handler: missing 1-in-4 gate, missing the 4-attempt spread, plus an invented spontaneous dirt-to-grass random tick
  - genuine: c0.30 tile/q.a (GrassTile.tick, verified via javap): "0: aload 5; 2: iconst_4; 3: invokevirtual Random.nextInt; 6: ifeq 10; 9: return" (a 1-in-4 gate on the WHOLE tick), then unlit -> setTile dirt, else a loop "39: iload_0; 40: iconst_4; if_icmpge 136" doing FOUR spread attempts
  - ours: Client BlockPhysics.c Physics_HandleGrass (356-363): unlit grass -> dirt on EVERY random tick (no 1/4 gate, ~4x faster die-back) and NO spread attempts at all; instead Physics_HandleDirt (347-354) makes any lit dirt anywhere spontaneously b
  - at: c030 jar com/mojang/minecraft/level/tile/q.class method a(Level,int,int,int,Random); tile/c.class (no tick) vs ClassiCube/src/BlockPhysics.c:347-363, 620-622, 228-257
- **[V] FIXED** (medium, sp-mp-split) Server grass die-back trigger conflates cover with darkness: grass under leaves dies (client keeps it, genuine keeps it)
  - genuine: BlockGrass.java:19-23: "if (var1.getBlockLightValue(var2, var3 + 1, var4) < 4 && var1.getBlockMaterial(var2, var3 + 1, var4).getCanBlockGrass()) { if (var5.nextInt(4) == 0) { var1.setBlockWithNotify(var2, var3, var4, Block.dirt.blockID); } }". Leaves have lightOpacity 1 (Block.ja
  - ours: Server SurvivalGrowth.cs TickGrass:388-391 rolls the 1/4 die-back whenever BlocksSky(above) is true, and BlocksSky (157-169) counts leaves (and water, farmland, containers) as blockers - so grass touching a leaf block above (sloped forest f
  - at: BlockGrass.java:19-23; Block.java:496; Material.java:33 vs mcgalaxy/MCGalaxy/Network/SurvivalGrowth.cs:388-391,157-174; ClassiCube/src/IndevTest.c:2500-2504
- **[V] FIXED** (medium, both) Grass under shallow water dies in both ports; genuine water opacity 3 keeps it alive under up to ~3 layers
  - genuine: Block.java:462-463: "waterMoving = (new BlockFlowing(8, Material.water)).setHardness(100.0F).setLightOpacity(3); waterStill = ... .setLightOpacity(3);" - the Indev flood light engine (Light.java:253-304) attenuates by max(1, opacity) per cell, so the water cell above submerged gr
  - ours: Both ports treat any water directly above as full darkness: server BlocksSky includes water (SurvivalGrowth.cs:157-174, only ShadesFromBelow special-cases it) so TickGrass:389 rolls the 1/4 revert; client Blocks.BlocksLight[water]=true (Blo
  - at: Block.java:462-463; Light.java:253-304; BlockGrass.java:19-23 vs mcgalaxy/MCGalaxy/Network/SurvivalGrowth.cs:388-391; ClassiCube/src/IndevTest.c:2500-2504 with src/Block.c:52
- **[V] FIXED** (medium, server) Server has no random-tick handler for flowers or mushrooms: no dark-pop, no bright-light mushroom pop, no soil check on MP
  - genuine: BlockFlower.java:12 "this.setTickOnLoad(true)" with updateTick calling checkFlowerChange (BlockFlower.java:29-39): a flower pops (dropping itself) unless "(var1.getBlockLightValue(var2, var3, var4) >= 8 || var1.getBlockLightValue(var2, var3, var4) >= 4 && var1.canBlockSeeTheSky(v
  - ours: SurvivalGrowth.Tick's dispatch (SurvivalGrowth.cs:364-379) has cases for grass/leaves/crops/farmland/sapling/fire/fluids only - Block.Rose, Block.Dandelion, Block.Mushroom, Block.RedMushroom never receive a handler, and SurvivalPhysics.Noti
  - at: BlockFlower.java:12,29-45; BlockMushroom.java:15-24 vs mcgalaxy/MCGalaxy/Network/SurvivalGrowth.cs:364-379 (missing cases); ClassiCube/src/IndevTest.c:2339-2352 (present)
- **[V] FIXED** (medium, server) Farmland trample missing entirely in multiplayer
  - genuine: BlockFarmland.java:103-108: "public final void onEntityWalking(World var1, int var2, int var3, int var4) { if (var1.random.nextInt(4) == 0) { var1.setBlockWithNotify(var2, var3, var4, Block.dirt.blockID); } }" - fired from Entity.java:338-351 for every walking entity (players and
  - ours: The client implements it (IndevTest_TrampleStep, IndevTest.c:1738-1750, fed by SurvivalTest.c:8472-8486 for the player and 4782-4797 for mobs) but the player feed is explicitly gated '!SurvivalNet_ServerDriven()' and the local mob sim is of
  - at: BlockFarmland.java:103-108; Entity.java:336-351 vs ClassiCube/src/SurvivalTest.c:8472-8486; mcgalaxy/MCGalaxy/Network/SurvivalGrowth.cs (absent)
- **[P]** (medium, both) Farmland moisture has one wet state instead of 8: dries ~7x too fast after water is removed
  - genuine: BlockFarmland.java:63-72: with water in range "var1.setBlockMetadata(var2, var3, var4, 7)"; without, "byte var13 = var1.getBlockMetadata(var2, var3, var4); if (var13 > 0) { var1.setBlockMetadata(var2, var3, var4, var13 - 1); return; }" - moisture counts down 7,6,...,0 through SEV
  - ours: Both ports model farmland as exactly two blocks (FARMLAND/FARMLAND_WET): server TickFarmland (SurvivalGrowth.cs:497-500) and client Indev_TickFarmland (IndevTest.c:1723-1726) drop from wet to fully dry in ONE successful 1/5 gate (~1000 game
  - at: BlockFarmland.java:63-72; BlockCrops.java:47-51 vs mcgalaxy/MCGalaxy/Network/SurvivalGrowth.cs:493-501; ClassiCube/src/IndevTest.c:1719-1729
- **[V] FIXED** (medium, both) Server sapling collapses the 16-stage counter into a memoryless 1/80 roll; failed-tree retry also 16x slower than genuine on both sides
  - genuine: BlockSapling.java:14-24: "if (var1.getBlockLightValue(var2, var3 + 1, var4) >= 9 && var5.nextInt(5) == 0) { byte var6 = var1.getBlockMetadata(var2, var3, var4); if (var6 < 15) { var1.setBlockMetadata(var2, var3, var4, var6 + 1); return; } var1.setTileNoUpdate(var2, var3, var4, 0)
  - ours: Server TickSapling (SurvivalGrowth.cs:522-530): 'if (g.Rng.Next(5) != 0) return; if (g.Rng.Next(16) != 0) return;' - same 80-roll mean but memoryless: a just-planted sapling can grow on its first random tick (genuine: impossible before 16),
  - at: BlockSapling.java:12-27; World.java:353-365 vs mcgalaxy/MCGalaxy/Network/SurvivalGrowth.cs:517-530; ClassiCube/src/IndevTest.c:2422-2429
- **[V] FIXED** (medium, client) c0.30 sapling handler wrong on every axis: inert on dirt, no 1-in-5 gate, tree height 5-7 vs 4-6, no restore on failed growth
  - genuine: c0.30 tile/n.a (SaplingTile.tick via javap): stay-check pops the sapling unless isLit AND below is dirt(a.g) or grass(a.f); then "56: aload 5; 58: iconst_5; 59: invokevirtual Random.nextInt; 62: ifne 99" (1-in-5 gate) -> setTileNoUpdate(0) -> Level.maybeGrowTree -> restore saplin
  - ours: Client Physics_HandleSapling (BlockPhysics.c:317-345), used by c0.30 survival at the genuine tick rate: 'if (below == BLOCK_DIRT) return;' (sapling on dirt never grows AND never pops even unlit); on grass+lit it removes the sapling and atte
  - at: c030 jar com/mojang/minecraft/level/tile/n.class method a(...); Level.class maybeGrowTree vs ClassiCube/src/BlockPhysics.c:317-345
- **[V] FIXED** (medium, client) c0.30 Survival Test flowers never pop in genuine (growTrees gate), but ours pops them
  - genuine: c0.30 tile/r.a (Bush.tick via javap): "0: aload_1; 1: getfield Level.growTrees; 4: ifeq 8; 7: return" - when Level.growTrees is TRUE the whole stay-check is skipped. The survival gamemode class com/mojang/minecraft/d/b sets "1: iconst_0; putfield creativeMode; 6: iconst_1; putfie
  - ours: Client Physics_HandleFlower (BlockPhysics.c:365-382) pops dandelion/rose when unlit or when below is not dirt/grass, and runs in c0.30 survival via the OnRandomTick table at the genuine volume/200 rate - flowers in player-built dark rooms o
  - at: c030 jar com/mojang/minecraft/level/tile/r.class method a(...); com/mojang/minecraft/d/b.class vs ClassiCube/src/BlockPhysics.c:365-382,624-625
- **[P]** (medium, server) c0.30 multiplayer survival maps get MCGalaxy classic physics, not genuine c0.30 growth
  - genuine: c0.30 Level.tick (javap, offsets 246-291): "unprocessed += width*height*depth; var6 = unprocessed / 200; unprocessed -= var6 * 200" then the c*3+1013904223 LCG picks driving GrassTile/Bush/Sapling/Mushroom ticks - the same volume/200 growth engine as Indev, which a faithful c0.30
  - ours: SurvivalGrowth.Tick is invoked only for Indev ('if (indev) SurvivalGrowth.Tick(lvl);' SurvivalMobs.cs:2255) and SurvivalGrowth.cs's header states 'Runs on Indev maps only - c0.30 keeps the classic engine physics'. So MP c0.30 survival growt
  - at: c030 jar com/mojang/minecraft/level/Level.class tick() offsets 246-410 vs mcgalaxy/MCGalaxy/Network/SurvivalMobs.cs:2255; mcgalaxy/MCGalaxy/Network/SurvivalGrowth.cs:25-34
- **[P]** (low, both) Neighbor-change plant reactions deferred to random ticks: farmland solid-cover revert and soil-removal pops are delayed instead of instant
  - genuine: BlockFarmland.java:110-117: "public final void onNeighborBlockChange(...) { Material var6 = var1.getBlockMaterial(var2, var3 + 1, var4); if (var6.isSolid()) { var1.setBlockWithNotify(var2, var3, var4, Block.dirt.blockID); } }" and BlockFlower.java:24-27 onNeighborBlockChange -> c
  - ours: Neither port wires plant/farmland reactions to block-change notifications. Server: SurvivalPhysics.Notify (SurvivalPhysics.cs:140-162) schedules only fire/fluids; the solid-above farmland check was moved INSIDE the 1/5-gated random tick (Su
  - at: BlockFarmland.java:110-117; BlockFlower.java:24-27; World.java:344-351 vs mcgalaxy/MCGalaxy/Network/SurvivalGrowth.cs:484-492; ClassiCube/src/IndevTest.c:1712-1717,2432-2465
- **[P]** (low, both) Farmland hydration ignores spring blocks (52/53) - genuine BlockSource has Material.water for BOTH water and lava springs
  - genuine: BlockFarmland.java:53 scans "if (var12.getBlockMaterial(var9, var10, var11) == Material.water)"; BlockSource.java:10-11: "protected BlockSource(int var1, int var2) { super(var1, Block.blocksList[var2].blockIndexInTexture, Material.water); }" - both the water spring (52) and even
  - ours: Server WaterNear (SurvivalGrowth.cs:511-512) matches only 'b == Block.Water || b == Block.StillWater'; client Indev_WaterNear (IndevTest.c:1696-1697) only BLOCK_WATER/BLOCK_STILL_WATER. A farm whose only in-range water is the spring block i
  - at: BlockFarmland.java:40-66; BlockSource.java:10-14 vs mcgalaxy/MCGalaxy/Network/SurvivalGrowth.cs:505-515; ClassiCube/src/IndevTest.c:1688-1702
- **[P]** (low, sp-mp-split) Server tree canopy refuses to overwrite water/farmland; genuine (and client) replace any non-opaque-cube cell
  - genuine: World.java:1055-1057: "if ((Math.abs(var12) != var9 || Math.abs(var11) != var9 || this.random.nextInt(2) != 0 && var8 != 0) && !Block.opaqueCubeLookup[this.getBlockId(var10, var13, var6)]) { this.setBlockWithNotify(var10, var13, var6, Block.leaves.blockID); }" - opaqueCubeLookup
  - ours: Server GrowTree (SurvivalGrowth.cs:583, 597-601) gates on IsFullOpaque, which is BlocksSky-based and thus counts water and farmland as opaque - those cells are skipped. Client Indev_GrowTree (IndevTest.c:2394) uses Blocks.FullOpaque, false
  - at: World.java:1050-1061; BlockFarmland.java:22-24 vs mcgalaxy/MCGalaxy/Network/SurvivalGrowth.cs:583,597-601; ClassiCube/src/IndevTest.c:2394
- **[P]** (low, both) Server growth drops spawn at cell center instead of the genuine 0.15-0.85 scatter
  - genuine: Block.java:282-284 (dropBlockAsItemWithChance): "float var10 = var1.random.nextFloat() * 0.7F + 0.15F;" on all three axes - popped plants, decayed-leaf saplings and popped mature crops spawn scattered inside the cell.
  - ours: Server spawns at exact centers: SurvivalGrowth.cs:425-426 and 473-475 use 'x + 0.5, y + 0.5, z + 0.5'. Client is faithful (IndevTest.c:2316-2318, 2545-2547) except PopCrop's wheat uses y + 0.5f (IndevTest.c:1759). (The server's clear-before
  - at: Block.java:275-292 vs mcgalaxy/MCGalaxy/Network/SurvivalGrowth.cs:423-426,472-477; ClassiCube/src/IndevTest.c:1755-1762
- **[P]** (low, client) Client sapling stage side-store never cleared on pop/break: replanted sapling inherits the old stage
  - genuine: World.java:297-309 setBlock: "this.blocks[...] = (byte) var4; this.setBlockMetadata(var1, var2, var3, 0);" - every block change zeroes metadata, so a freshly placed sapling always starts at stage 0.
  - ours: The client's per-cell stage store (indev_saplingStage, IndevTest.c:2288-2304) is only written by metadata import and Indev_TickSapling; Indev_PopPlant (2314-2321) and player breaks never reset it. Replanting a sapling in a cell whose previo
  - at: World.java:297-309 vs ClassiCube/src/IndevTest.c:2288-2304,2314-2321
- **[P]** (low, client) c0.30 mushroom soil set missing gravel
  - genuine: c0.30 tile/t.a (Mushroom.tick via javap): stays only when NOT lit and below is a.e, a.q or a.h - static init shows field e = tile id 1 (rock), field h = tile id 4 (stoneBrick/cobblestone), field q = tile id 13 (GRAVEL): mushrooms survive on stone, cobblestone or gravel.
  - ours: Client Physics_HandleMushroom (BlockPhysics.c:395-397): 'if (!(below == BLOCK_STONE || below == BLOCK_COBBLE))' pops it - a c0.30 mushroom sitting on gravel pops in ours, stays in genuine. (The lit->pop half matches.)
  - at: c030 jar com/mojang/minecraft/level/tile/t.class method a(...); tile/a.class static init offsets 59-88, 134-163, 370-392 vs ClassiCube/src/BlockPhysics.c:384-401
- **[P]** (low, client) c0.30 sand/gravel random-tick falling is an extra rule (genuine c0.30 never random-ticks them)
  - genuine: c0.30 tile/l (sand/gravel) constructor: "0: aload_0; 1: iload_1; 2: iload_2; 3: invokespecial a.<init>(II); 6: return" - it never calls the shouldTick setter a(Z) and has no tick override; sand/gravel fall only from the neighbor-change hooks b(...). Level.tick's random loop gates
  - ours: BlockPhysics.c:617-618 registers 'Physics.OnRandomTick[BLOCK_SAND] = Physics_DoFalling' (and GRAVEL), and Physics_TickRandomBlocksC030 dispatches purely on that table - so floating sand left without any neighbor update eventually falls in o
  - at: c030 jar com/mojang/minecraft/level/tile/l.class; Level.class tick() offsets 375-401 vs ClassiCube/src/BlockPhysics.c:613-618,244-256
- **[P]** (low, sp-mp-split) Random-tick coordinate masks disagree between client and server on non-power-of-two maps
  - genuine: World.java:583-588: "this.randId = this.randId * 3 + 1013904223; int var13 = this.randId >> 2; int var14 = var13 & var4;" with var4 = width-1 etc. (World.java:550-552) - genuine masks with dim-1, which is only uniform because genuine Indev/c0.30 dimensions are always powers of tw
  - ours: Client copies genuine literally (IndevTest.c:2567 'maskX = World.Width - 1' - on an odd-sized import the AND knocks holes in the pattern so many cells are NEVER picked, acknowledged in its comment); server instead uses next-power-of-two mas
  - at: World.java:540-552,583-593 vs ClassiCube/src/IndevTest.c:2565-2579; mcgalaxy/MCGalaxy/Network/SurvivalGrowth.cs:339-362
- **[P]** (low, both) Genuine border-shell setBlock immutability not replicated: growth can alter the outermost columns
  - genuine: World.java:297-298: "public final boolean setBlock(int var1, int var2, int var3, int var4) { if (var1 > 0 && var2 > 0 && var3 > 0 && var1 < this.width - 1 && var2 < this.height - 1 && var3 < this.length - 1) {" - every growth write (setBlockWithNotify) silently fails on the 1-blo
  - ours: Server SetView (SurvivalGrowth.cs:116-123) and client Game_UpdateBlock write anywhere in bounds, so border-column grass spread/decay, leaf decay and farmland reverts all proceed normally. Edge-only cosmetic difference (and it 'fixes' the ge
  - at: World.java:297-333; BlockLeaves.java:25-26 vs mcgalaxy/MCGalaxy/Network/SurvivalGrowth.cs:116-123; ClassiCube/src/IndevTest.c:2550
- **[P]** (low, both) Crop/flower stay-check sky test approximated by heightmap IsLit instead of canBlockSeeTheSky's opaque-scan
  - genuine: World.java:1638-1649 canBlockSeeTheSky: "if (this.heightMap[var1 + var3 * this.width] <= var2) { return true; } else { while (var2 < this.height) { if (Block.opaqueCubeLookup[this.getBlockId(var1, var2, var3)]) { return false; } ++var2; } }" - a plant under only NON-opaque cover
  - ours: Both ports substitute binary sky exposure: server FlowerStayCheck/TickCrops use IsLit (SurvivalGrowth.cs:434,538) where leaves/water shadow their own cell; client uses Lighting.IsLit (IndevTest.c:1783,2332) and its comment calls the gap 'ac
  - at: World.java:1638-1649; BlockFlower.java:41-45 vs mcgalaxy/MCGalaxy/Network/SurvivalGrowth.cs:434,536-538; ClassiCube/src/IndevTest.c:1778-1784,2330-2333

### user-reported (2026-08-02, MP island map)
- **[V] FIXED** (medium, client) Wall-mounted torch renders wrong on MP: mostly a thin
  1px diagonal streak with the bright tip cap floating detached near the wall
  top (screenshot: torch auto-mounted on a dirt ledge face, zoom fov 15).
  Builder_DrawWallTorch and the wall-torch defs were NOT touched in the recent
  rounds (git log confirms), so likely pre-existing and only now noticed while
  testing torch-lit grass. Check: quad winding/backface of the side quads, the
  full-tile side-quad span vs genuine renderBlockTorch's 2px-wide quads, and
  whether the MP path renders the SERVER's CPE sprite def instead of the local
  tilted builder (ordering of local redefine vs server BlockDefinitions).
  INVESTIGATED (2026-08-02): geometry verified faithful against genuine
  renderBlockTorch line by line (offsets, tilt, cap position, UVs all match)
  and the sprite vertex count is correctly 8 quads. PRIME SUSPECT: bank
  facing - the engine draws sprite banks selectively by camera quadrant
  (crops put one double-sided pair per bank for this reason), but the torch
  packs both X-plane quads in bank 0 and both Z-plane quads in bank 1, so
  from some camera angles the visible pair is skipped/backfaced, leaving an
  edge-on 1px line + the cap. Fix candidate: rearrange to one side quad per
  bank matching each bank's expected facing. Needs a live client to confirm.
  CONFIRMED BY OBSERVATION (2026-08-03): the torch renders correctly from one
  side and breaks specifically viewed from the UPPER-LEFT quadrant - the bug
  is camera-facing-dependent, which is the bank-facing hypothesis exactly.
  Fix: redistribute the side quads one per bank per facing (crops pattern).

- **[V] FIXED** (medium, sp-mp-split) Creative-mode drops float midair on MP
  (user screenshot: tossed workbenches hovering). Hypothesis: MP gates the
  local drop sim to net drops only, but a creative toss never reaches the
  server (no inventory consume path), so the client spawns a LOCAL drop
  (netId 0) that nothing ticks - no gravity, no settle, no despawn. Check
  SurvivalTest drop tick's ServerDriven gating vs the creative toss path;
  fix is either routing creative tosses through SURV_DROP_ITEM like survival
  ones (server spawns an ordinary net drop) or ticking local drops' physics
  in MP. Genuine creative (growTrees mode) has no drops at all - decide
  whether creative tossing should even spawn an entity, or just delete.
