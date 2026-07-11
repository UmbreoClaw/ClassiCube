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
6. [P] Hotbar slot pop animation: genuine c0.30 X=sin(t^2*pi)+1 but
   Y=sin(t*pi)+1 (HUDScreen.java:119-124); genuine Indev is a squash
   glScalef(1/(1+t), (2+t)/2, 1) around (x+8, y+12) (GuiIngame.java:137-143).
   Ours: uniform sin(t^2*pi)+1 both axes, both modes (Widgets.c:457-467).
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
13. [P] Paperdoll anchors: genuine panel (51,75) feet anchor, fixed scale 30,
    mouse anchor (x+51, y+25) (GuiInventory.java:100-118); ours box-derived
    (~1-3 units off, scale 29.1). Convert to panel-relative constants.
14. [P] Minor: (a) heart shake gated health>0, genuine shakes at <=4 incl 0;
    (b) held-stack count anchor +8*texF vs genuine +9; (c) foreground labels
    drawn before items, genuine draws them last (over held stack).

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
9. [P] Indev drowning: 2 HP every 20 ticks (air==-20 reset) + 8-bubble
   burst (EntityLiving.java:85-100); ours c0.30 cadence (~2 HP/10 ticks),
   no bubbles (SurvivalTest.c:3706-3711).
10. [P] Mob bbox sizes come from engine models, differ from genuine
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

1. [P] **Indev food maxStackSize 1** (MEDIUM): ItemFood sets maxStackSize=1
   (apple/bread/porkchops); ours stacks them to 64 (IndevTest.c:543-556).
2. [P] Sword dig speed 1.5x vs everything (ItemSword.java:15-17); ours 1
   and MiningSpeed returns int (IndevTest.c:328-342, SurvivalTest.c:6081).
3. [P] **Sword wear inverted** (MEDIUM): sword 1/hit 2/block; tools 2/hit
   1/block (ItemSword.java:19-25, ItemTool.java:29-35). Ours flat 2/hit
   1/block for all (SurvivalTest.c:4483, 6083). Notes:2239 wrong — fix.
4. [P] **Hoe + flint&steel must not wear from digging/melee** (MEDIUM):
   genuine no-op hitEntity/onBlockDestroyed; ours wears them like tools
   (IndevTest.c:284-295 ToolMaxDamage drives DamageHeldTool).
5. [P] Tool breakage off-by-one: genuine breaks when damage > maxDamage;
   ours >= (SurvivalTest.c:6016). Armor already correct (>).
6. [P] **Indev obsidian drop**: BlockStone(49) -> drops cobblestone; ours
   drops obsidian in Indev (SurvivalTest.c:662-679; c0.30 path correct).
7. [P] c0.30 hardness: dirt 10 (ours 12), sand 10 (ours 12), slab+double 40
   (ours 20), brick 40 (ours 0!) per /tmp/mcraft_client Block.java setData.
   Notes:2560 claims brick 0 from "getHardness switch" — no such switch in
   this decompile. RE-VERIFY carefully (previous session used a different
   decompile?), then fix values + notes.
8. [P] **c0.30 double slab drops 1 slab** not 2 (SlabBlock.java:45-47 only
   overrides getDrop; getDropCount default 1). Ours 2 = slab dupe
   (SurvivalTest.c:603-605). Notes self-contradict (2533 vs 3631).
9. [P] **Indev dig-time model** (MEDIUM): Block.blockStrength = strVsBlock/
   hardness/30 per tick, /5 in water, /5 airborne; non-harvestable digs at
   1/hardness/100 with NO tool bonus (slow dig, no drop). Ours: c0.30
   hardness*20 ticks model, no penalties, non-harvestable digs full speed
   (SurvivalTest.c:6078-6082). Obsidian w/o diamond pick: ours ~201 ticks
   vs genuine 1000.
10. [P] Tool effectiveness lists: genuine explicit block lists (pickaxe
    excludes brick/obsidian/furnace!; axe excludes workbench; spade excludes
    leaves/sponge); ours dig-sound proxy over-applies (IndevTest.c:334-341).
11. [P] Mirrored recipe matching missing: genuine tries mirrored layouts
    (CraftingRecipe.java:19-32); axe/hoe/bow/flint&steel can't be crafted
    mirrored in ours (IndevTest.c:443-466).
12. [P] Indev arrow physics: drag 0.99 (0.8 water), gravity flat 0.03,
    gaussian spread 0.0075/axis (EntityArrow.java:43-60,177-189); ours uses
    c0.30 constants in both modes (SurvivalTest.c:4498, 4581, 4723).
13. [P] Lit furnace should drop LIT furnace (62) in Indev (no idDropped
    override); ours canonicalises to idle. Low priority; notes claim wrong.
14. [P] Mushroom eating is c0.30-only (SurvivalGameMode.useItem); ours
    allows in Indev too (SurvivalTest.c:5872-5878).
15. [P] Hardness-0 blocks don't wear tools in our insta-break path; genuine
    onBlockDestroyed always fires.

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
8. [P] Indev EntityItem lava pop + burn (health 5) + push-out-of-solid
   missing; drops rest inert in lava.
9. [P] Indev drop spin 2.86deg/tick + bob arg 0.1 rad/tick w/ random
   hoverStart phase; ours c0.30 3deg/0.3 both modes. Indev pickup is
   instant (no fly-in anim).
10. [P] Indev TNT render: swell 1+t^4*0.3 last 10 ticks, flash fuse/5%2
    alpha (1-(fuse+1)/100)*0.8, smoke at y+0.5; ours c0.30 pattern.
11. [P] Indev explosion ray-march block destruction + entity velocity
    knockback (overlaps mobs finding 4 - one combined fix).
12. [P] **Farmland trampling missing** (MEDIUM): onEntityWalking 1/4 ->
    dirt. Not implemented at all.
13. [P] Crops need BlockFlower.canBlockStay light check (pop unless
    light>=8 or >=4+sky, ground farmland).
14. [P] Indev lava flow tickRate 25 (ours 30 = c0.30-correct).
15. [P] c0.30 random-tick rate should ALSO be volume/200 (engine default
    ~volume/1365 is 6.8x slow for c0.30 grass/saplings).
16. [P] Night terrain brightness: genuine lightBrightnessTable curve
    (1-v)/(3v+1)*0.95+0.05 -> 0.13 at night; ours linear light/15 -> 0.27.
    Nights ~2x too bright. (Also genuine eases 1 step/tick.)
17. [P] Physics/fire block changes bypass UserEvents.BlockChanged so
    torch/crop/fire neighbour pops miss non-player changes.
18. [P] Torch placement with no support should FAIL (canPlaceBlockAt),
    not place-then-pop.
19. RESOLVED (user decision 2026-07-11): death inventory scatter is NOT
    genuine (no dropAllItems in either ground truth) but is KEPT as a
    deliberate deviation because multiplayer support is planned; may
    later be gated behind a multiplayer option when that work starts.
    Notes + code comments corrected.
