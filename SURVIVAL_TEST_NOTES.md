# Classic 0.30 Survival Test — Project Notes & Handoff

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
- **Respawn exists** (previous "c0.30-s has no respawn" note was wrong —
  `GameOverScreen.java` has the button): clears inventory, health 20,
  arrows 20, air **20 ticks** (genuine quirk, not 300), teleport to spawn,
  score kept. Death drops carry each slot's full stack count (one drop per
  slot, `Item(level, x,y,z, block, count)`), so they can be re-collected.
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
