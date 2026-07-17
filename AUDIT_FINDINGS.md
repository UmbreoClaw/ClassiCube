# Systematic Fidelity Audit — Findings Queue

User delegated systematic auditing of everything (GUI placement/scale,
gameplay, mob mechanics, all between) against the decompiled ground truths:
`/tmp/indev_eagler` (in-20100223) and `/tmp/mcraft_client` (c0.30).
Four domain audits were run; three completed, the fourth (entities +
environment: drops/arrows/TNT/paintings/day-night/random ticks/fluids)
was cut off by a usage limit and MUST BE RE-RUN next session.

Status legend: [V] = independently verified against the Java by the main
session; [P] = pending verification (audit finding, not yet re-checked).
Every [P] item must be verified against the genuine source before fixing.

VERIFICATION RULE: subagent findings are leads, not verdicts.

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
