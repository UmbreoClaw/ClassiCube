#include "IndevTest.h"
#include "Game.h"
#include "Options.h"
#include "Chat.h"
#include "SurvivalTest.h"
#include "Funcs.h"
#include "Graphics.h"
#include "TexturePack.h"
#include "Block.h"
#include "Audio.h"
#include "Platform.h"
#include "String_.h"

/* Indev (in-20100223) gamemode - mode plumbing only so far.
   Ground truth: the deobfuscated EaglerPorts/in-20100223 tree (see
   SURVIVAL_TEST_NOTES.md). Design intent: SurvivalTest.c is the shared
   survival core (entities, combat, drops, HUD); this file grows the
   Indev-specific layer (ItemStacks, tools, crafting, day/night, ...) on
   top, with per-block/per-mob data kept in runtime tables so a server
   (e.g. an MCGalaxy plugin over a CPE channel) can override them later. */

cc_bool IndevTest_Enabled;

/* gui/items.png - the 16x16-sprite item atlas (Indev draws item icons from
    iconIndex cells of this sheet, ItemRenderer-style). Loaded from texture
    packs via the standard TextureEntry route; auto-provisioning it into
    default.zip is planned via Resources.c's existing jar patchers (the
    engine already downloads Mojang's classic + 1.6.2 jars at first launch -
    an items atlas can be composed from those assets the same way the other
    survival textures are extracted). Until then, packs supply it; rendering
    code must bail gracefully while this is 0. */
static GfxResourceID indev_itemsTexId;
static void ItemsPngProcess(struct Stream* stream, const cc_string* name) {
	Game_UpdateTexture(&indev_itemsTexId, stream, name, NULL, NULL);
}
static struct TextureEntry items_entry = { "items.png", ItemsPngProcess };

GfxResourceID IndevTest_ItemsTex(void) { return indev_itemsTexId; }

/* gui/inventory.png - the genuine 176x166 inventory/crafting GUI texture */
static GfxResourceID indev_invGuiTexId;
static void InvGuiPngProcess(struct Stream* stream, const cc_string* name) {
	Game_UpdateTexture(&indev_invGuiTexId, stream, name, NULL, NULL);
}
static struct TextureEntry invgui_entry = { "inventory.png", InvGuiPngProcess };

GfxResourceID IndevTest_InvGuiTex(void) { return indev_invGuiTexId; }

/* Item definitions - the complete in-20100223 roster from Item.java's static
    init (local ids; shiftedIndex = id + 256). kind drives behaviour:
    tools/swords/hoes carry a tier (maxDamage = 32 << tier, ItemTool.java:14)
    and stack to 1; ItemFood carries its heal amount; plain materials stack
    to 64 (Item.java:77 default). Data-driven so later steps (durability,
    eating, sprites) and eventually server overrides read one table. */
enum IndevItemKind {
	ITEM_KIND_MATERIAL, ITEM_KIND_SWORD, ITEM_KIND_SHOVEL, ITEM_KIND_PICKAXE,
	ITEM_KIND_AXE, ITEM_KIND_HOE, ITEM_KIND_FLINTSTEEL, ITEM_KIND_BOW,
	ITEM_KIND_FOOD, ITEM_KIND_SOUP, ITEM_KIND_ARMOR, ITEM_KIND_SEEDS
};
struct IndevItemDef { cc_uint8 id, kind, param; cc_uint8 icon; const char* name; };
/* param = tool tier / food heal amount / armor type; 0 otherwise */
static const struct IndevItemDef indevItems[] = {
	{  0, ITEM_KIND_SHOVEL, 2,  82, "Iron Shovel" }, {  1, ITEM_KIND_PICKAXE, 2,  98, "Iron Pickaxe" },
	{  2, ITEM_KIND_AXE, 2, 114, "Iron Axe" }, {  3, ITEM_KIND_FLINTSTEEL, 0,   5, "Flint and Steel" },
	{  4, ITEM_KIND_FOOD, 4,   4, "Apple" }, {  5, ITEM_KIND_BOW, 0,  21, "Bow" },
	{  6, ITEM_KIND_MATERIAL, 0,  37, "Arrow" }, {  7, ITEM_KIND_MATERIAL, 0,   7, "Coal" },
	{  8, ITEM_KIND_MATERIAL, 0,  55, "Diamond" }, {  9, ITEM_KIND_MATERIAL, 0,  23, "Iron Ingot" },
	{ 10, ITEM_KIND_MATERIAL, 0,  39, "Gold Ingot" }, { 11, ITEM_KIND_SWORD, 2,  66, "Iron Sword" },
	{ 12, ITEM_KIND_SWORD, 0,  64, "Wooden Sword" }, { 13, ITEM_KIND_SHOVEL, 0,  80, "Wooden Shovel" },
	{ 14, ITEM_KIND_PICKAXE, 0,  96, "Wooden Pickaxe" }, { 15, ITEM_KIND_AXE, 0, 112, "Wooden Axe" },
	{ 16, ITEM_KIND_SWORD, 1,  65, "Stone Sword" }, { 17, ITEM_KIND_SHOVEL, 1,  81, "Stone Shovel" },
	{ 18, ITEM_KIND_PICKAXE, 1,  97, "Stone Pickaxe" }, { 19, ITEM_KIND_AXE, 1, 113, "Stone Axe" },
	{ 20, ITEM_KIND_SWORD, 3,  67, "Diamond Sword" }, { 21, ITEM_KIND_SHOVEL, 3,  83, "Diamond Shovel" },
	{ 22, ITEM_KIND_PICKAXE, 3,  99, "Diamond Pickaxe" }, { 23, ITEM_KIND_AXE, 3, 115, "Diamond Axe" },
	{ 24, ITEM_KIND_MATERIAL, 0,  53, "Stick" }, { 25, ITEM_KIND_MATERIAL, 0,  71, "Bowl" },
	{ 26, ITEM_KIND_SOUP, 10,  72, "Mushroom Soup" }, { 27, ITEM_KIND_SWORD, 0,  68, "Golden Sword" },
	{ 28, ITEM_KIND_SHOVEL, 0,  84, "Golden Shovel" }, { 29, ITEM_KIND_PICKAXE, 0, 100, "Golden Pickaxe" },
	{ 30, ITEM_KIND_AXE, 0, 116, "Golden Axe" }, { 31, ITEM_KIND_MATERIAL, 0,   8, "String" },
	{ 32, ITEM_KIND_MATERIAL, 0,  24, "Feather" }, { 33, ITEM_KIND_MATERIAL, 0,  40, "Gunpowder" },
	{ 34, ITEM_KIND_HOE, 0, 128, "Wooden Hoe" }, { 35, ITEM_KIND_HOE, 1, 129, "Stone Hoe" },
	{ 36, ITEM_KIND_HOE, 2, 130, "Iron Hoe" }, { 37, ITEM_KIND_HOE, 3, 131, "Diamond Hoe" },
	{ 38, ITEM_KIND_HOE, 4, 132, "Golden Hoe" }, { 39, ITEM_KIND_SEEDS, 0,   9, "Seeds" },
	{ 40, ITEM_KIND_MATERIAL, 0,  25, "Wheat" }, { 41, ITEM_KIND_FOOD, 5,  41, "Bread" },
	/* 42-61: armor - ItemArmor(id, tier, texRow, piece 0=helmet..3=boots) */
	{ 42, ITEM_KIND_ARMOR, 0,   0, "Leather Cap" },    { 43, ITEM_KIND_ARMOR, 1,  16, "Leather Tunic" },
	{ 44, ITEM_KIND_ARMOR, 2,  32, "Leather Pants" },  { 45, ITEM_KIND_ARMOR, 3,  48, "Leather Boots" },
	{ 46, ITEM_KIND_ARMOR, 0,   1, "Chain Helmet" },   { 47, ITEM_KIND_ARMOR, 1,  17, "Chain Chestplate" },
	{ 48, ITEM_KIND_ARMOR, 2,  33, "Chain Leggings" }, { 49, ITEM_KIND_ARMOR, 3,  49, "Chain Boots" },
	{ 50, ITEM_KIND_ARMOR, 0,   2, "Iron Helmet" },    { 51, ITEM_KIND_ARMOR, 1,  18, "Iron Chestplate" },
	{ 52, ITEM_KIND_ARMOR, 2,  34, "Iron Leggings" },  { 53, ITEM_KIND_ARMOR, 3,  50, "Iron Boots" },
	{ 54, ITEM_KIND_ARMOR, 0,   3, "Diamond Helmet" }, { 55, ITEM_KIND_ARMOR, 1,  19, "Diamond Chestplate" },
	{ 56, ITEM_KIND_ARMOR, 2,  35, "Diamond Leggings" },{ 57, ITEM_KIND_ARMOR, 3,  51, "Diamond Boots" },
	{ 58, ITEM_KIND_ARMOR, 0,   4, "Golden Helmet" },  { 59, ITEM_KIND_ARMOR, 1,  20, "Golden Chestplate" },
	{ 60, ITEM_KIND_ARMOR, 2,  36, "Golden Leggings" },{ 61, ITEM_KIND_ARMOR, 3,  52, "Golden Boots" },
	/* The tail entries the first pass missed - porkchops DO exist in Indev */
	{ 62, ITEM_KIND_MATERIAL, 0,   6, "Flint" },
	{ 63, ITEM_KIND_FOOD,     3,  87, "Raw Porkchop" },
	{ 64, ITEM_KIND_FOOD,     8,  88, "Cooked Porkchop" },
	{ 65, ITEM_KIND_MATERIAL, 0,  26, "Painting" },
};

/* items.png is a 16x16 grid of 16px sprites (icons 128+ live on row 8+). */
/* Returns false when the id isn't a known item (or is a block id). */
cc_bool IndevTest_ItemSpriteUV(int id, float* u1, float* v1, float* u2, float* v2) {
	int i, icon = -1, local = id - 256;
	if (local < 0) return false;
	for (i = 0; i < (int)Array_Elems(indevItems); i++) {
		if (indevItems[i].id == local) { icon = indevItems[i].icon; break; }
	}
	if (icon < 0) return false;

	*u1 = (icon % 16)       / 16.0f; *v1 = (icon / 16)       / 16.0f;
	*u2 = (icon % 16 + 1)   / 16.0f; *v2 = (icon / 16 + 1)   / 16.0f;
	return true;
}

/* Heal amount when the id is an edible item (ItemFood/ItemSoup param), 0 */
/*  otherwise. Soup also returns the empty bowl in genuine - TODO with bowls. */
int IndevTest_ItemFoodHeal(int id) {
	int i, local = id - 256;
	if (!IndevTest_Enabled || local < 0) return 0;
	for (i = 0; i < (int)Array_Elems(indevItems); i++) {
		if (indevItems[i].id != local) continue;
		if (indevItems[i].kind == ITEM_KIND_FOOD || indevItems[i].kind == ITEM_KIND_SOUP)
			return indevItems[i].param;
		return 0;
	}
	return 0;
}

static const struct IndevItemDef* IndevItems_Find(int id) {
	int i, local = id - 256;
	if (local < 0) return NULL;
	for (i = 0; i < (int)Array_Elems(indevItems); i++) {
		if (indevItems[i].id == local) return &indevItems[i];
	}
	return NULL;
}

/* ItemTool: maxDamage = 32 << tier. 0 when the id isn't a damageable tool. */
int IndevTest_ToolMaxDamage(int id) {
	const struct IndevItemDef* d = IndevItems_Find(id);
	if (!d) return 0;
	switch (d->kind) {
	case ITEM_KIND_SWORD: case ITEM_KIND_SHOVEL: case ITEM_KIND_PICKAXE:
	case ITEM_KIND_AXE:   case ITEM_KIND_HOE:
		return 32 << d->param;
	}
	return 0;
}

/* ItemTool.getStrVsBlock: (tier+1)*2 against the tool's effective materials */
/*  (approximated by dig-sound class), otherwise 1 - note gold tools are tier */
/*  0 in Indev, i.e. WOOD speed. Returns 1 for non-tools/ineffective pairs. */
int IndevTest_MiningSpeed(int id, BlockID block) {
	const struct IndevItemDef* d = IndevItems_Find(id);
	cc_uint8 snd;
	cc_bool effective = false;
	if (!IndevTest_Enabled || !d) return 1;

	snd = Blocks.DigSounds[block];
	switch (d->kind) {
	case ITEM_KIND_PICKAXE: effective = snd == SOUND_STONE  || snd == SOUND_METAL; break;
	case ITEM_KIND_SHOVEL:  effective = snd == SOUND_GRASS  || snd == SOUND_GRAVEL ||
	                                    snd == SOUND_SAND   || snd == SOUND_SNOW;  break;
	case ITEM_KIND_AXE:     effective = snd == SOUND_WOOD;  break;
	}
	return effective ? (d->param + 1) * 2 : 1;
}

/* Minecraft.java:352 melee: damage = held Item.getDamageVsEntity(), bare */
/*  fist (or any non-weapon item) = 1. ItemTool: base+tier with base 1/2/3 */
/*  for shovel/pickaxe/axe; ItemSword: 4 + tier*2; everything else 1. */
int IndevTest_MeleeDamage(int id) {
	const struct IndevItemDef* d = IndevItems_Find(id);
	if (!IndevTest_Enabled) return 1;
	if (!d) return 1;
	switch (d->kind) {
	case ITEM_KIND_SWORD:   return 4 + d->param * 2;
	case ITEM_KIND_SHOVEL:  return 1 + d->param;
	case ITEM_KIND_PICKAXE: return 2 + d->param;
	case ITEM_KIND_AXE:     return 3 + d->param;
	}
	return 1;
}

/* EntityPlayer.canHarvestBlock + ItemPickaxe.canHarvestBlock: rock/iron */
/*  material blocks (stone dig-sound proxy) only drop when the held item is a */
/*  pickaxe of sufficient harvestLevel (== tier: wood 0, stone 1, iron 2, */
/*  diamond 3, gold 0). Obsidian needs 3, gold ore/block >= 2, iron ore/block */
/*  > 0, all other rock any pickaxe. Non-rock blocks always drop (return true). */
cc_bool IndevTest_CanHarvest(int heldId, BlockID block) {
	const struct IndevItemDef* d;
	cc_uint8 snd = Blocks.DigSounds[block];
	int level;
	if (snd != SOUND_STONE && snd != SOUND_METAL) return true;

	d = IndevItems_Find(heldId);
	if (!d || d->kind != ITEM_KIND_PICKAXE) return false;
	level = d->param;
	switch (block) {
	case BLOCK_OBSIDIAN:                return level == 3;
	case BLOCK_GOLD_ORE: case BLOCK_GOLD: return level >= 2;
	case BLOCK_IRON_ORE: case BLOCK_IRON: return level > 0;
	default:                            return true;
	}
}

/*########################################################################################################################*
*-------------------------------------------------CraftingManager recipes-------------------------------------------------*
*#########################################################################################################################*/
/* Shaped recipes from CraftingManager + Recipes{Tools,Weapons,Food,...}, */
/*  in-20100223. Cells are full-space ids (blocks<256, items 256+; 0=empty), */
/*  patterns are w*h anchored top-left, matched at ANY offset inside the */
/*  crafting grid (genuine slides the pattern the same way). Recipes needing */
/*  blocks the classic block set lacks (torches, workbench, crate, furnace) */
/*  are deferred until those blocks are added to the Indev layer. */
#define R_ITEM(n) (256 + (n))
struct IndevRecipe {
	cc_uint16 result; cc_uint8 count, w, h;
	cc_uint16 cells[9];
};
static const struct IndevRecipe indevRecipes[] = {
	/* planks x4 <- log; sticks x4 <- 2 planks; slabs x3; bread; gray cloth */
	{ BLOCK_WOOD,    4, 1,1, { BLOCK_LOG } },
	{ R_ITEM(24),    4, 1,2, { BLOCK_WOOD, BLOCK_WOOD } },
	{ BLOCK_SLAB,    3, 3,1, { BLOCK_COBBLE, BLOCK_COBBLE, BLOCK_COBBLE } },
	{ R_ITEM(41),    1, 3,1, { R_ITEM(40), R_ITEM(40), R_ITEM(40) } },
	{ BLOCK_GRAY,    1, 3,3, { R_ITEM(31),R_ITEM(31),R_ITEM(31), R_ITEM(31),R_ITEM(31),R_ITEM(31), R_ITEM(31),R_ITEM(31),R_ITEM(31) } },
	/* TNT: gunpowder/sand checkerboard */
	{ BLOCK_TNT,     1, 3,3, { R_ITEM(33),BLOCK_SAND,R_ITEM(33), BLOCK_SAND,R_ITEM(33),BLOCK_SAND, R_ITEM(33),BLOCK_SAND,R_ITEM(33) } },
	/* the new Indev blocks: workbench (2x2!), torch; chest/furnace need 3x3 */
	{ 66,            1, 2,2, { BLOCK_WOOD, BLOCK_WOOD, BLOCK_WOOD, BLOCK_WOOD } },
	{ 70,            4, 1,2, { R_ITEM(7), R_ITEM(24) } },
	{ 67,            1, 3,3, { BLOCK_WOOD,BLOCK_WOOD,BLOCK_WOOD, BLOCK_WOOD,0,BLOCK_WOOD, BLOCK_WOOD,BLOCK_WOOD,BLOCK_WOOD } },
	{ 68,            1, 3,3, { BLOCK_COBBLE,BLOCK_COBBLE,BLOCK_COBBLE, BLOCK_COBBLE,0,BLOCK_COBBLE, BLOCK_COBBLE,BLOCK_COBBLE,BLOCK_COBBLE } },
	/* bowls x4; mushroom soup (both mushroom orders); flint&steel */
	{ R_ITEM(25),    4, 3,2, { BLOCK_WOOD,0,BLOCK_WOOD, 0,BLOCK_WOOD,0 } },
	{ R_ITEM(26),    1, 1,3, { BLOCK_RED_SHROOM, BLOCK_BROWN_SHROOM, R_ITEM(25) } },
	{ R_ITEM(26),    1, 1,3, { BLOCK_BROWN_SHROOM, BLOCK_RED_SHROOM, R_ITEM(25) } },
	{ R_ITEM(3),     1, 2,2, { R_ITEM(9),0, 0,R_ITEM(62) } },
};

/* Tool recipes are generated like RecipesTools/RecipesWeapons: 5 materials */
/*  (planks, cobble, iron ingot, diamond, gold ingot) x 5 tool shapes. */
static const cc_uint16 toolMaterial[5] = { BLOCK_WOOD, BLOCK_COBBLE, R_ITEM(9), R_ITEM(8), R_ITEM(10) };
/* item local ids per material, in tool order: pickaxe, shovel, axe, hoe, sword */
static const cc_uint8 toolResult[5][5] = {
	{ 14, 13, 15, 34, 12 }, /* wood    */
	{ 18, 17, 19, 35, 16 }, /* stone   */
	{  1,  0,  2, 36, 11 }, /* iron    */
	{ 22, 21, 23, 37, 20 }, /* diamond */
	{ 29, 28, 30, 38, 27 }, /* gold    */
};

static cc_bool Recipe_MatchesAt(const struct IndevRecipe* r, const cc_uint16* grid,
								int gw, int gh, int ox, int oy) {
	int x, y;
	for (y = 0; y < gh; y++) {
		for (x = 0; x < gw; x++) {
			int rx = x - ox, ry = y - oy;
			cc_uint16 want = 0;
			if (rx >= 0 && rx < r->w && ry >= 0 && ry < r->h) want = r->cells[ry * r->w + rx];
			if (grid[y * gw + x] != want) return false;
		}
	}
	return true;
}

static cc_bool Recipe_Matches(const struct IndevRecipe* r, const cc_uint16* grid, int gw, int gh) {
	int ox, oy;
	if (r->w > gw || r->h > gh) return false;
	for (oy = 0; oy + r->h <= gh; oy++) {
		for (ox = 0; ox + r->w <= gw; ox++) {
			if (Recipe_MatchesAt(r, grid, gw, gh, ox, oy)) return true;
		}
	}
	return false;
}

/* Matches the grid (gw*gh of full-space ids, 0 = empty) against every */
/*  recipe. Returns the crafted result id/count, or false when nothing fits. */
cc_bool IndevTest_MatchRecipe(const cc_uint16* grid, int gw, int gh, int* outId, int* outCount) {
	static const cc_uint8 toolPatW[5] = { 3, 1, 2, 2, 1 };
	static const cc_uint8 toolPatH[5] = { 3, 3, 3, 3, 3 };
	struct IndevRecipe r;
	int i, m, t;
	if (!IndevTest_Enabled) return false;

	for (i = 0; i < (int)Array_Elems(indevRecipes); i++) {
		if (!Recipe_Matches(&indevRecipes[i], grid, gw, gh)) continue;
		*outId = indevRecipes[i].result; *outCount = indevRecipes[i].count;
		return true;
	}

	/* Generated tool shapes (X = material, # = stick), RecipesTools' patterns:
	    pickaxe "XXX/ # / # ", shovel "X/#/#", axe "XX/X#/ #", hoe "XX/ #/ #",
	    sword "X/X/#" */
	for (m = 0; m < 5; m++) {
		cc_uint16 X = toolMaterial[m], S = (cc_uint16)R_ITEM(24);
		const cc_uint16 pats[5][9] = {
			{ X,X,X, 0,S,0, 0,S,0 },
			{ X,S,S, 0,0,0, 0,0,0 }, /* 1x3 stored row-major below via w/h */
			{ X,X,0, X,S,0, 0,S,0 },
			{ X,X,0, 0,S,0, 0,S,0 },
			{ X,X,S, 0,0,0, 0,0,0 }, /* 1x3 sword */
		};
		for (t = 0; t < 5; t++) {
			Mem_Copy(r.cells, pats[t], sizeof(r.cells));
			r.w = toolPatW[t]; r.h = toolPatH[t];
			if (t == 1 || t == 4) { /* 1-wide column patterns packed tightly */
				r.cells[0] = pats[t][0]; r.cells[1] = (t == 4) ? X : S; r.cells[2] = S;
				r.w = 1; r.h = 3;
			}
			r.result = (cc_uint16)R_ITEM(toolResult[m][t]); r.count = 1;
			if (Recipe_Matches(&r, grid, gw, gh)) {
				*outId = r.result; *outCount = 1;
				return true;
			}
		}
	}
	return false;
}

static cc_bool IndevItem_StacksToOne(cc_uint8 kind) {
	return kind == ITEM_KIND_SWORD || kind == ITEM_KIND_SHOVEL || kind == ITEM_KIND_PICKAXE ||
	       kind == ITEM_KIND_AXE   || kind == ITEM_KIND_HOE    || kind == ITEM_KIND_FLINTSTEEL ||
	       kind == ITEM_KIND_BOW   || kind == ITEM_KIND_SOUP   || kind == ITEM_KIND_ARMOR;
}

static void IndevItems_Seed(void) {
	int i;
	for (i = 256; i < 1024; i++) SurvivalTest_SetMaxStack(i, 64);
	for (i = 0; i < (int)Array_Elems(indevItems); i++) {
		if (IndevItem_StacksToOne(indevItems[i].kind))
			SurvivalTest_SetMaxStack(256 + indevItems[i].id, 1);
	}
}

/*########################################################################################################################*
*---------------------------------------------------Indev block additions-------------------------------------------------*
*#########################################################################################################################*/
/* Blocks the classic set lacks, defined at the reserved ids (66+) with the */
/*  reserved atlas tiles (96+, patched in from the b1.7.3 jar's terrain.png */
/*  by Resources.c's BetaPatcher - see the notes' reservation table). */
#define INDEV_BLOCK_WORKBENCH   66
#define INDEV_BLOCK_CHEST       67
#define INDEV_BLOCK_FURNACE     68
#define INDEV_BLOCK_FURNACE_LIT 69
#define INDEV_BLOCK_TORCH       70

static void IndevBlock_Define(BlockID id, const char* name, int top, int side,
							  int front, int bottom, cc_uint8 sound, int hardness) {
	cc_string str = String_FromReadonly(name);
	Block_SetName(id, &str);

	Block_Tex(id, FACE_YMAX) = (TextureLoc)top;
	Block_Tex(id, FACE_YMIN) = (TextureLoc)bottom;
	Block_SetSide((TextureLoc)side, id);
	Block_Tex(id, FACE_ZMIN) = (TextureLoc)front; /* the "face" side */

	Blocks.Collide[id]         = COLLIDE_SOLID;
	Blocks.ExtendedCollide[id] = COLLIDE_SOLID;
	Blocks.Draw[id]            = DRAW_OPAQUE;
	Blocks.DigSounds[id]       = sound;
	Blocks.StepSounds[id]      = sound;
	Blocks.CanPlace[id]        = true;
	Blocks.CanDelete[id]       = true;
	Blocks.BlocksLight[id]     = true;
	Blocks.SpeedMultiplier[id] = 1.0f;
	Vec3_Set(Blocks.MinBB[id], 0.0f, 0.0f, 0.0f);
	Vec3_Set(Blocks.MaxBB[id], 1.0f, 1.0f, 1.0f);

	Block_DefineCustom(id, false);
	SurvivalTest_SetHardness(id, hardness);
}

static void IndevBlocks_Define(void) {
	/* tiles: 96 wb top, 97 wb side, 98 wb front, 99 furn front, 100 furn lit, */
	/*  101 furn side, 102 furn top, 103 chest front, 104 chest side, 105 top */
	IndevBlock_Define(INDEV_BLOCK_WORKBENCH,   "Workbench",  96, 97,  98,  4, SOUND_WOOD,  30);
	IndevBlock_Define(INDEV_BLOCK_CHEST,       "Chest",     105, 104, 103, 105, SOUND_WOOD, 30);
	IndevBlock_Define(INDEV_BLOCK_FURNACE,     "Furnace",   102, 101,  99, 102, SOUND_STONE, 70);
	IndevBlock_Define(INDEV_BLOCK_FURNACE_LIT, "Furnace (lit)", 102, 101, 100, 102, SOUND_STONE, 70);

	/* Torch: a fullbright sprite, walk-through, instant to break */
	IndevBlock_Define(INDEV_BLOCK_TORCH, "Torch", 106, 106, 106, 106, SOUND_WOOD, 0);
	Blocks.Collide[INDEV_BLOCK_TORCH]         = COLLIDE_NONE;
	Blocks.ExtendedCollide[INDEV_BLOCK_TORCH] = COLLIDE_NONE;
	Blocks.Draw[INDEV_BLOCK_TORCH]            = DRAW_SPRITE;
	Blocks.BlocksLight[INDEV_BLOCK_TORCH]     = false;
	Blocks.Brightness[INDEV_BLOCK_TORCH]      = Blocks.Brightness[BLOCK_LAVA];
	Block_DefineCustom(INDEV_BLOCK_TORCH, true);
}

static void OnInit(void) {
	IndevTest_Enabled = Options_GetBool(OPT_INDEV_MODE, false);
	if (!IndevTest_Enabled) return;

	IndevItems_Seed();
	IndevBlocks_Define();
	TextureEntry_Register(&items_entry);
	TextureEntry_Register(&invgui_entry);
	Chat_AddRaw("&eIndev mode: plumbing active (survival core + Indev layer WIP)");
}

struct IGameComponent IndevTest_Component = {
	OnInit /* Init */
};
