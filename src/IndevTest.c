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
#include "World.h"
#include "Event.h"
#include "ExtMath.h"

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

/* gui/crafting.png - the workbench 3x3 crafting GUI texture */
static GfxResourceID indev_craftGuiTexId;
static void CraftGuiPngProcess(struct Stream* stream, const cc_string* name) {
	Game_UpdateTexture(&indev_craftGuiTexId, stream, name, NULL, NULL);
}
static struct TextureEntry craftgui_entry = { "crafting.png", CraftGuiPngProcess };

GfxResourceID IndevTest_CraftGuiTex(void) { return indev_craftGuiTexId; }

/* gui/furnace.png - the furnace GUI (GuiFurnace binds "/gui/furnace.png") */
static GfxResourceID indev_furnGuiTexId;
static void FurnGuiPngProcess(struct Stream* stream, const cc_string* name) {
	Game_UpdateTexture(&indev_furnGuiTexId, stream, name, NULL, NULL);
}
static struct TextureEntry furngui_entry = { "furnace.png", FurnGuiPngProcess };

GfxResourceID IndevTest_FurnGuiTex(void) { return indev_furnGuiTexId; }

/* gui/container.png - the chest GUI (GuiChest binds "/gui/container.png") */
static GfxResourceID indev_contGuiTexId;
static void ContGuiPngProcess(struct Stream* stream, const cc_string* name) {
	Game_UpdateTexture(&indev_contGuiTexId, stream, name, NULL, NULL);
}
static struct TextureEntry contgui_entry = { "container.png", ContGuiPngProcess };

GfxResourceID IndevTest_ContGuiTex(void) { return indev_contGuiTexId; }

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
	/* RecipesWeapons extras: bow (" #X"/"# X"/" #X", # stick, X string) and */
	/*  arrows x4 ("X"/"#"/"Y" = iron ingot / stick / feather) */
	{ R_ITEM(5),     1, 3,3, { 0,R_ITEM(24),R_ITEM(31), R_ITEM(24),0,R_ITEM(31), 0,R_ITEM(24),R_ITEM(31) } },
	{ R_ITEM(6),     4, 1,3, { R_ITEM(9), R_ITEM(24), R_ITEM(32) } },
	/* RecipesIngots: 9 ingots <-> storage block, both directions. (The
	    diamond pair is genuine too, but the classic block set has no diamond
	    block id to map it onto, so that pair is omitted.) */
	{ BLOCK_GOLD,    1, 3,3, { R_ITEM(10),R_ITEM(10),R_ITEM(10), R_ITEM(10),R_ITEM(10),R_ITEM(10), R_ITEM(10),R_ITEM(10),R_ITEM(10) } },
	{ BLOCK_IRON,    1, 3,3, { R_ITEM(9),R_ITEM(9),R_ITEM(9), R_ITEM(9),R_ITEM(9),R_ITEM(9), R_ITEM(9),R_ITEM(9),R_ITEM(9) } },
	{ R_ITEM(10),    9, 1,1, { BLOCK_GOLD } },
	{ R_ITEM(9),     9, 1,1, { BLOCK_IRON } },
	/* painting: ring of planks around gray cloth */
	{ R_ITEM(65),    1, 3,3, { BLOCK_WOOD,BLOCK_WOOD,BLOCK_WOOD, BLOCK_WOOD,BLOCK_GRAY,BLOCK_WOOD, BLOCK_WOOD,BLOCK_WOOD,BLOCK_WOOD } },
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
	    sword "X/X/#". Cells are TIGHTLY packed row-major at each pattern's
	    own width (Recipe_MatchesAt reads cells[ry*w + rx]) - the old 3-wide
	    storage for the 2-wide axe/hoe scrambled their rows, which made axes
	    and hoes permanently uncraftable. */
	for (m = 0; m < 5; m++) {
		cc_uint16 X = toolMaterial[m], S = (cc_uint16)R_ITEM(24);
		const cc_uint16 pats[5][9] = {
			{ X,X,X, 0,S,0, 0,S,0 }, /* pickaxe 3x3 */
			{ X,S,S },               /* shovel  1x3 */
			{ X,X, X,S, 0,S },       /* axe     2x3 */
			{ X,X, 0,S, 0,S },       /* hoe     2x3 */
			{ X,X,S },               /* sword   1x3 */
		};
		for (t = 0; t < 5; t++) {
			Mem_Copy(r.cells, pats[t], sizeof(r.cells));
			r.w = toolPatW[t]; r.h = toolPatH[t];
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
cc_bool IndevTest_IsWorkbench(BlockID b) { return IndevTest_Enabled && b == INDEV_BLOCK_WORKBENCH; }
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
	/* Hardness from Block.java registration: workbench/chest setHardness(2.5F), */
	/*  furnace 3.5F, torch 0.0F - our units are 20 per hardness-second. */
	IndevBlock_Define(INDEV_BLOCK_WORKBENCH,   "Workbench",  96, 97,  98,  4, SOUND_WOOD,  50);
	IndevBlock_Define(INDEV_BLOCK_CHEST,       "Chest",     105, 104, 103, 105, SOUND_WOOD, 50);
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

/*########################################################################################################################*
*-----------------------------------------------Container tile entities---------------------------------------------------*
*#########################################################################################################################*/
/* Per-position storage for chests + furnaces, ported from TileEntityChest /
    TileEntityFurnace (in-20100223). ClassiCube has no tile entity concept,
    so this is a flat pool keyed by block position. Created lazily on first
    open (which also covers blocks that predate this feature or came from a
    loaded map); destroyed by the BlockChanged hook when the block goes away.
    KNOWN GAP: contents are NOT saved with the map - a save/load loses
    chest/furnace contents (serialisation needs a sidecar format; planned). */
#define INDEV_TE_MAX 192
struct IndevTE {
	cc_bool  used;
	cc_uint8 kind;                /* INDEV_CONTAINER_* */
	IVec3    pos;
	struct SurvivalSlot slots[SURVIVAL_CONTAINER_SLOTS];
	int burnTime, cookTime, currentBurn; /* furnace: furnaceBurnTime/furnaceCookTime/currentItemBurnTime */
};
static struct IndevTE indev_tes[INDEV_TE_MAX];
static int indev_openTE = -1;           /* pool index of the open container, -1 = none */
static struct SurvivalSlot indev_discardSlot; /* safe target when nothing is open */
static RNGState indev_teRng;

static int IndevTest_ContainerKindOf(BlockID b) {
	if (b == INDEV_BLOCK_CHEST) return INDEV_CONTAINER_CHEST;
	if (b == INDEV_BLOCK_FURNACE || b == INDEV_BLOCK_FURNACE_LIT) return INDEV_CONTAINER_FURNACE;
	return INDEV_CONTAINER_NONE;
}

static int IndevTE_Find(IVec3 pos) {
	int i;
	for (i = 0; i < INDEV_TE_MAX; i++) {
		if (indev_tes[i].used && indev_tes[i].pos.x == pos.x &&
			indev_tes[i].pos.y == pos.y && indev_tes[i].pos.z == pos.z) return i;
	}
	return -1;
}

static int IndevTE_Create(int kind, IVec3 pos) {
	int i;
	for (i = 0; i < INDEV_TE_MAX; i++) {
		if (indev_tes[i].used) continue;
		Mem_Set(&indev_tes[i], 0, sizeof(struct IndevTE));
		indev_tes[i].used = true;
		indev_tes[i].kind = (cc_uint8)kind;
		indev_tes[i].pos  = pos;
		return i;
	}
	return -1; /* pool exhausted - the container simply won't open */
}

/* BlockChest.onBlockRemoval: scatter the contents as item drops - one random
    0.1..0.9 offset per slot, stacks split into random chunks of 10..30. */
static void IndevTE_Scatter(struct IndevTE* te) {
	struct SurvivalSlot* s;
	Vec3 p; int i, chunk;
	for (i = 0; i < SURVIVAL_CONTAINER_SLOTS; i++) {
		s = &te->slots[i];
		if (s->count <= 0 || !s->id) continue;
		p.x = te->pos.x + Random_Float(&indev_teRng) * 0.8f + 0.1f;
		p.y = te->pos.y + Random_Float(&indev_teRng) * 0.8f + 0.1f;
		p.z = te->pos.z + Random_Float(&indev_teRng) * 0.8f + 0.1f;
		while (s->count > 0) {
			chunk = Random_Next(&indev_teRng, 21) + 10;
			if (chunk > s->count) chunk = s->count;
			s->count -= chunk;
			SurvivalTest_SpawnDropWorld(p, s->id, chunk);
		}
		s->id = 0; s->count = 0; s->damage = 0;
	}
}

cc_bool IndevTest_IsContainerBlock(BlockID b) {
	return IndevTest_Enabled && IndevTest_ContainerKindOf(b) != INDEV_CONTAINER_NONE;
}

int IndevTest_OpenContainer(IVec3 pos) {
	BlockID b, above;
	int kind, i;
	if (!IndevTest_Enabled) return INDEV_CONTAINER_NONE;

	b    = World_GetBlock(pos.x, pos.y, pos.z);
	kind = IndevTest_ContainerKindOf(b);
	if (!kind) return INDEV_CONTAINER_NONE;

	/* BlockChest.blockActivated: a normal (opaque) cube directly above the */
	/*  chest keeps it shut. Furnaces have no such rule. */
	if (kind == INDEV_CONTAINER_CHEST && World_Contains(pos.x, pos.y + 1, pos.z)) {
		above = World_GetBlock(pos.x, pos.y + 1, pos.z);
		if (Blocks.Draw[above] == DRAW_OPAQUE) return INDEV_CONTAINER_NONE;
	}

	i = IndevTE_Find(pos);
	if (i < 0) i = IndevTE_Create(kind, pos);
	if (i < 0) return INDEV_CONTAINER_NONE;

	indev_openTE = i;
	return kind;
}

int IndevTest_OpenKind(void) {
	if (!IndevTest_Enabled || indev_openTE < 0) return INDEV_CONTAINER_NONE;
	return indev_tes[indev_openTE].kind;
}

void IndevTest_CloseContainer(void) { indev_openTE = -1; }

struct SurvivalSlot* IndevTest_ContainerSlot(int i) {
	if (indev_openTE >= 0 && i >= 0 && i < SURVIVAL_CONTAINER_SLOTS)
		return &indev_tes[indev_openTE].slots[i];
	/* Nothing open (or the container block was destroyed under an open */
	/*  screen): hand back a zeroed discard slot so clicks can't corrupt. */
	indev_discardSlot.id = 0; indev_discardSlot.count = 0; indev_discardSlot.damage = 0;
	return &indev_discardSlot;
}

/*########################################################################################################################*
*--------------------------------------------------Furnace (TileEntityFurnace)--------------------------------------------*
*#########################################################################################################################*/
/* TileEntityFurnace.smeltItem: iron ore -> iron ingot, gold ore -> gold
    ingot, sand -> glass, cobblestone -> stone, raw -> cooked porkchop.
    (Indev also smelts diamond ore -> diamond, but the classic block set has
    no diamond ore block, so that entry has nothing to map from.) */
static int Furnace_SmeltResult(int id) {
	if (id == BLOCK_IRON_ORE) return 256 + 9;  /* Iron Ingot */
	if (id == BLOCK_GOLD_ORE) return 256 + 10; /* Gold Ingot */
	if (id == BLOCK_SAND)     return BLOCK_GLASS;
	if (id == BLOCK_COBBLE)   return BLOCK_STONE;
	if (id == 256 + 63)       return 256 + 64; /* Raw -> Cooked Porkchop */
	return 0;
}

/* TileEntityFurnace.getItemBurnTime: wood-material blocks 300 ticks, stick
    100, coal 1600. Wood material is approximated by the wood dig sound
    (planks/log/bookshelf/workbench/chest) - the torch is excluded since its
    genuine material is circuits, not wood. */
static int Furnace_FuelTime(int id) {
	if (id == 256 + 7)  return 1600; /* Coal */
	if (id == 256 + 24) return 100;  /* Stick */
	if (id > 0 && id < 256 && id != INDEV_BLOCK_TORCH &&
		Blocks.DigSounds[id] == SOUND_WOOD) return 300;
	return 0;
}

/* TileEntityFurnace.canSmelt: input present + smeltable + output empty or
    same id with room (inventory stack limit 64). */
static cc_bool Furnace_CanSmelt(struct IndevTE* te) {
	int result;
	if (te->slots[0].count <= 0) return false;
	result = Furnace_SmeltResult(te->slots[0].id);
	if (!result) return false;
	if (te->slots[2].count <= 0) return true;
	if (te->slots[2].id != (cc_uint16)result) return false;
	return te->slots[2].count < 64;
}

/* TileEntityFurnace.updateEntity, 20Hz. Faithful order: burn down; consume
    fuel only when burnt out AND smeltable; progress while burning+smeltable
    (200 ticks per item); ALL partial progress lost the moment it can't
    smelt; lit/unlit block swap on burning-state change (the BlockChanged
    hook recognises furnace<->furnace swaps and keeps this tile entity). */
static void Furnace_Tick(struct IndevTE* te) {
	cc_bool wasBurning = te->burnTime > 0, burning;
	cc_bool slotsChanged = false;
	int result;

	if (te->burnTime > 0) te->burnTime--;

	if (te->burnTime == 0 && Furnace_CanSmelt(te)) {
		te->currentBurn = te->burnTime = Furnace_FuelTime(te->slots[1].id);
		if (te->burnTime > 0) {
			te->slots[1].count--;
			if (te->slots[1].count <= 0) { te->slots[1].id = 0; te->slots[1].count = 0; }
			slotsChanged = true;
		}
	}

	if (te->burnTime > 0 && Furnace_CanSmelt(te)) {
		te->cookTime++;
		if (te->cookTime >= 200) {
			te->cookTime = 0;
			result = Furnace_SmeltResult(te->slots[0].id);
			if (te->slots[2].count > 0) {
				te->slots[2].count++;
			} else {
				te->slots[2].id = (cc_uint16)result; te->slots[2].count = 1; te->slots[2].damage = 0;
			}
			te->slots[0].count--;
			if (te->slots[0].count <= 0) { te->slots[0].id = 0; te->slots[0].count = 0; }
			slotsChanged = true;
		}
	} else {
		te->cookTime = 0;
	}

	burning = te->burnTime > 0;
	if (burning != wasBurning) {
		Game_ChangeBlock(te->pos.x, te->pos.y, te->pos.z,
			burning ? INDEV_BLOCK_FURNACE_LIT : INDEV_BLOCK_FURNACE);
	}
	if (slotsChanged) SurvivalTest_InvChanged();
}

int IndevTest_FurnaceBurnScaled(void) {
	struct IndevTE* te;
	if (indev_openTE < 0) return 0;
	te = &indev_tes[indev_openTE];
	if (te->kind != INDEV_CONTAINER_FURNACE || te->currentBurn <= 0) return 0;
	return te->burnTime * 12 / te->currentBurn; /* getBurnTimeRemainingScaled */
}

int IndevTest_FurnaceCookScaled(void) {
	struct IndevTE* te;
	if (indev_openTE < 0) return 0;
	te = &indev_tes[indev_openTE];
	if (te->kind != INDEV_CONTAINER_FURNACE) return 0;
	return te->cookTime * 24 / 200; /* getCookProgressScaled */
}

static void IndevTest_Tick(struct ScheduledTask* task) {
	int i;
	if (!IndevTest_Enabled) return;
	for (i = 0; i < INDEV_TE_MAX; i++) {
		if (!indev_tes[i].used || indev_tes[i].kind != INDEV_CONTAINER_FURNACE) continue;
		Furnace_Tick(&indev_tes[i]);
	}
}

/* Tile entity lifecycle, driven off block changes: a chest scatters its
    contents when broken (BlockChest.onBlockRemoval); a furnace's contents
    are silently destroyed (faithful - BlockContainer.onBlockRemoval only
    removes the tile entity); the lit<->unlit furnace swap keeps its state. */
static void IndevTest_BlockChanged(void* obj, IVec3 coords, BlockID oldBlock, BlockID block) {
	cc_bool oldFurn, nowFurn;
	int i;
	if (!IndevTest_Enabled) return;

	oldFurn = oldBlock == INDEV_BLOCK_FURNACE || oldBlock == INDEV_BLOCK_FURNACE_LIT;
	nowFurn = block    == INDEV_BLOCK_FURNACE || block    == INDEV_BLOCK_FURNACE_LIT;
	if (oldFurn && nowFurn) return; /* lit/unlit swap - state survives */

	if (oldBlock == INDEV_BLOCK_CHEST || oldFurn) {
		i = IndevTE_Find(coords);
		if (i < 0) return;
		if (oldBlock == INDEV_BLOCK_CHEST) IndevTE_Scatter(&indev_tes[i]);
		if (indev_openTE == i) indev_openTE = -1; /* screen falls back to the discard slot */
		indev_tes[i].used = false;
	}
}

static void OnInit(void) {
	/* Derived from the single authoritative gamemode value - the conflicting */
	/*  "both modes set" state is unrepresentable there, and this works */
	/*  regardless of component init order. */
	IndevTest_Enabled = SurvivalTest_Gamemode() == SURVIVAL_GAMEMODE_INDEV;
	if (!IndevTest_Enabled) return;

	IndevItems_Seed();
	IndevBlocks_Define();
	TextureEntry_Register(&items_entry);
	TextureEntry_Register(&invgui_entry);
	TextureEntry_Register(&craftgui_entry);
	TextureEntry_Register(&furngui_entry);
	TextureEntry_Register(&contgui_entry);

	Random_Seed(&indev_teRng, (int)Game.Time + 1);
	Event_Register_(&UserEvents.BlockChanged, NULL, IndevTest_BlockChanged);
	ScheduledTask_Add(GAME_DEF_TICKS, IndevTest_Tick);
	Chat_AddRaw("&eIndev mode: plumbing active (survival core + Indev layer WIP)");
}

/*########################################################################################################################*
*-----------------------------------------------.mclevel format support---------------------------------------------------*
*#########################################################################################################################*/
/* Genuine Indev block ids: 0-49 match classic 1:1; 50-62 are Indev's own */
/*  (torch/fire/sources/chest/gear/diamond/workbench/crops/farmland/ovens), */
/*  which collide with ClassiCube's CPE ids 50-65. Mapping is exact for our */
/*  custom blocks and lossy-but-sensible for the rest. */
BlockRaw IndevTest_BlockToIndev(BlockRaw b) {
	switch (b) {
	case 50: return 44; /* cobble slab  -> stairSingle */
	case 51: return 0;  /* rope         -> air */
	case 52: return 12; /* sandstone    -> sand */
	case 53: return 0;  /* snow layer   -> air */
	case 54: return 51; /* fire         -> fire (exact) */
	case 55: return 33; /* light pink   -> clothRose */
	case 56: return 25; /* forest green -> clothGreen */
	case 57: return 3;  /* brown        -> dirt */
	case 58: return 29; /* deep blue    -> clothUltramarine */
	case 59: return 28; /* turquoise    -> clothCapri */
	case 60: return 20; /* ice          -> glass */
	case 61: return 45; /* ceramic tile -> brick */
	case 62: return 49; /* magma        -> obsidian */
	case 63: return 1;  /* pillar       -> stone */
	case 64: return 54; /* crate        -> chest */
	case 65: return 1;  /* stone brick  -> stone */
	case 66: return 58; /* workbench (exact) */
	case 67: return 54; /* chest (exact) */
	case 68: return 61; /* furnace idle (exact) */
	case 69: return 62; /* furnace lit (exact) */
	case 70: return 50; /* torch (exact) */
	default: return b <= 49 ? b : 1; /* classic identity; anything else -> stone */
	}
}

BlockRaw IndevTest_BlockFromIndev(BlockRaw b) {
	switch (b) {
	case 50: return 70; /* torch */
	case 51: return 54; /* fire -> CPE fire (exact) */
	case 52: return 8;  /* waterSource -> water */
	case 53: return 10; /* lavaSource  -> lava */
	case 54: return 67; /* chest */
	case 55: return 0;  /* gear -> air */
	case 56: return 16; /* diamond ore   -> coal ore (closest visual) */
	case 57: return 42; /* diamond block -> iron block */
	case 58: return 66; /* workbench */
	case 59: return 0;  /* crops    -> air (no crop blocks yet) */
	case 60: return 3;  /* farmland -> dirt (no farmland block yet) */
	case 61: return 68; /* furnace idle */
	case 62: return 69; /* furnace lit */
	default: return b <= 49 ? b : 0;
	}
}

int IndevTest_TENext(int prev) {
	int i;
	for (i = prev + 1; i < INDEV_TE_MAX; i++) {
		if (indev_tes[i].used) return i;
	}
	return -1;
}

void IndevTest_TEInfo(int i, int* kind, IVec3* pos, int* burn, int* cook) {
	*kind = indev_tes[i].kind;
	*pos  = indev_tes[i].pos;
	*burn = indev_tes[i].burnTime;
	*cook = indev_tes[i].cookTime;
}

void IndevTest_TEItem(int i, int slot, int* id, int* count, int* damage) {
	struct SurvivalSlot* s = &indev_tes[i].slots[slot];
	*id = s->id; *count = s->count; *damage = s->damage;
}

void IndevTest_RestoreTE(int kind, int x, int y, int z, int burn, int cook,
						 const cc_uint16* ids, const cc_int16* counts, const cc_int16* damages) {
	IVec3 pos; int i, idx;
	if (!IndevTest_Enabled) return;
	pos.x = x; pos.y = y; pos.z = z;

	idx = IndevTE_Find(pos);
	if (idx < 0) idx = IndevTE_Create(kind, pos);
	if (idx < 0) return; /* pool exhausted */

	indev_tes[idx].kind     = (cc_uint8)kind;
	indev_tes[idx].burnTime = burn;
	indev_tes[idx].cookTime = cook;
	for (i = 0; i < SURVIVAL_CONTAINER_SLOTS; i++) {
		indev_tes[idx].slots[i].id     = ids[i];
		indev_tes[idx].slots[i].count  = counts[i];
		indev_tes[idx].slots[i].damage = damages[i];
	}
	/* currentItemBurnTime isn't saved - recomputed from the fuel slot like */
	/*  TileEntityFurnace.readFromNBT does */
	indev_tes[idx].currentBurn = Furnace_FuelTime(indev_tes[idx].slots[1].id);
	if (indev_tes[idx].currentBurn < burn) indev_tes[idx].currentBurn = burn;
}

/* A new/reloaded map invalidates every block position - clear the tile */
/*  entity pool, else a chest placed at the same coords in the NEW world */
/*  would inherit (duplicate) the old world's contents. */
static void OnNewMap(void) {
	int i;
	for (i = 0; i < INDEV_TE_MAX; i++) indev_tes[i].used = false;
	indev_openTE = -1;
}

struct IGameComponent IndevTest_Component = {
	OnInit,   /* Init  */
	NULL,     /* Free  */
	OnNewMap, /* Reset (reconnect) - same invalidation applies */
	OnNewMap  /* OnNewMap */
};
