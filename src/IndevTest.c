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
#include "Entity.h"
#include "Lighting.h"
#include "BlockPhysics.h"

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

/* terrain/sun.png + terrain/moon.png - the celestial quads renderSky draws */
static GfxResourceID indev_sunTexId, indev_moonTexId;
static void SunPngProcess(struct Stream* stream, const cc_string* name) {
	Game_UpdateTexture(&indev_sunTexId, stream, name, NULL, NULL);
}
static struct TextureEntry sun_entry = { "sun.png", SunPngProcess };
static void MoonPngProcess(struct Stream* stream, const cc_string* name) {
	Game_UpdateTexture(&indev_moonTexId, stream, name, NULL, NULL);
}
static struct TextureEntry moon_entry = { "moon.png", MoonPngProcess };

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
cc_bool IndevTest_IsHoe(int id) {
	const struct IndevItemDef* def = IndevItems_Find(id);
	return def && def->kind == ITEM_KIND_HOE;
}

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
/* Directional variants: front face per Indev facing metadata 2/3/4/5 */
/*  (north -Z / south +Z / west -X / east +X), id = base + (meta - 2). */
/*  The canonical ids above stay the inventory/recipe/drop form (their */
/*  front faces -Z, i.e. meta 2); placing converts to a variant. */
#define INDEV_BLOCK_CHEST_V0    71 /* 71-74 */
#define INDEV_BLOCK_FURN_V0     75 /* 75-78 idle */
#define INDEV_BLOCK_FURNL_V0    79 /* 79-82 lit */
/* Farming: farmland dry/wet (genuine id 60 + moisture metadata) and the */
/*  8 crop growth stages (genuine id 59 + stage metadata 0-7). */
#define INDEV_BLOCK_FARMLAND     83
#define INDEV_BLOCK_FARMLAND_WET 84
#define INDEV_BLOCK_CROPS_0      85 /* 85-92 */
#define INDEV_BLOCK_CROPS_7      92

static cc_bool Indev_IsFarmland(BlockID b) {
	return b == INDEV_BLOCK_FARMLAND || b == INDEV_BLOCK_FARMLAND_WET;
}
static cc_bool Indev_IsCrops(BlockID b) {
	return b >= INDEV_BLOCK_CROPS_0 && b <= INDEV_BLOCK_CROPS_7;
}

/* Chunk-builder hook: crop stages render as BlockCrops' row pattern. */
cc_bool IndevTest_IsCropBlock(BlockID b) {
	return IndevTest_Enabled && Indev_IsCrops(b);
}

static cc_bool Indev_IsChestBlock(BlockID b) {
	return b == INDEV_BLOCK_CHEST || (b >= INDEV_BLOCK_CHEST_V0 && b <= INDEV_BLOCK_CHEST_V0 + 3);
}
static cc_bool Indev_IsFurnaceIdle(BlockID b) {
	return b == INDEV_BLOCK_FURNACE || (b >= INDEV_BLOCK_FURN_V0 && b <= INDEV_BLOCK_FURN_V0 + 3);
}
static cc_bool Indev_IsFurnaceLit(BlockID b) {
	return b == INDEV_BLOCK_FURNACE_LIT || (b >= INDEV_BLOCK_FURNL_V0 && b <= INDEV_BLOCK_FURNL_V0 + 3);
}

/* Inventory/drop form of a block (directional variants -> canonical id). */
/* BlockFurnace.idDropped is Block.stoneOvenIdle for BOTH furnace states, so */
/*  every lit form (base or directional) canonicalises to the idle furnace - */
/*  mining a burning furnace must never put a lit one in the inventory. */
BlockID IndevTest_CanonicalBlock(BlockID b) {
	if (b >= INDEV_BLOCK_CHEST_V0 && b <= INDEV_BLOCK_CHEST_V0 + 3) return INDEV_BLOCK_CHEST;
	if (b >= INDEV_BLOCK_FURN_V0  && b <= INDEV_BLOCK_FURN_V0  + 3) return INDEV_BLOCK_FURNACE;
	if (b >= INDEV_BLOCK_FURNL_V0 && b <= INDEV_BLOCK_FURNL_V0 + 3) return INDEV_BLOCK_FURNACE;
	if (b == INDEV_BLOCK_FURNACE_LIT) return INDEV_BLOCK_FURNACE;
	return b;
}

/* Indev facing metadata (2-5) of a container block; canonical ids face -Z. */
int IndevTest_BlockFacingMeta(BlockID b) {
	if (b >= INDEV_BLOCK_CHEST_V0 && b <= INDEV_BLOCK_CHEST_V0 + 3) return 2 + (b - INDEV_BLOCK_CHEST_V0);
	if (b >= INDEV_BLOCK_FURN_V0  && b <= INDEV_BLOCK_FURN_V0  + 3) return 2 + (b - INDEV_BLOCK_FURN_V0);
	if (b >= INDEV_BLOCK_FURNL_V0 && b <= INDEV_BLOCK_FURNL_V0 + 3) return 2 + (b - INDEV_BLOCK_FURNL_V0);
	return 2;
}

/* Generic .mclevel metadata for a block: container facing, farmland */
/*  moisture, crop stage - whatever the genuine block keeps in its nibble. */
int IndevTest_BlockDataMeta(BlockID b) {
	if (IndevTest_IsContainerBlock(b))    return IndevTest_BlockFacingMeta(b);
	if (b == INDEV_BLOCK_FARMLAND_WET)    return 7;
	if (Indev_IsCrops(b))                 return b - INDEV_BLOCK_CROPS_0;
	if (b == INDEV_BLOCK_TORCH)           return 5; /* standing */
	return 0;
}

/* Applies a loaded .mclevel metadata nibble to a base-mapped block. */
BlockID IndevTest_ApplyDataMeta(BlockID b, int meta) {
	if (IndevTest_IsContainerBlock(b)) return IndevTest_FacingVariant(b, meta);
	if (b == INDEV_BLOCK_FARMLAND && meta > 0) return INDEV_BLOCK_FARMLAND_WET;
	if (b == INDEV_BLOCK_CROPS_0 && meta > 0)  return (BlockID)(INDEV_BLOCK_CROPS_0 + (meta > 7 ? 7 : meta));
	return b;
}

/* Directional variant of a canonical container for facing metadata 2-5. */
BlockID IndevTest_FacingVariant(BlockID canonical, int meta) {
	int k = meta - 2;
	if (k < 0 || k > 3) k = 0;
	if (canonical == INDEV_BLOCK_CHEST)       return (BlockID)(INDEV_BLOCK_CHEST_V0 + k);
	if (canonical == INDEV_BLOCK_FURNACE)     return (BlockID)(INDEV_BLOCK_FURN_V0  + k);
	if (canonical == INDEV_BLOCK_FURNACE_LIT) return (BlockID)(INDEV_BLOCK_FURNL_V0 + k);
	return canonical;
}

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
	/* front texture rotated onto the face matching Indev metadata 2/3/4/5 */
	static const cc_uint8 metaFace[4] = { FACE_ZMIN, FACE_ZMAX, FACE_XMIN, FACE_XMAX };
	int k;

	/* Hardness from Block.java registration: workbench/chest setHardness(2.5F), */
	/*  furnace 3.5F, torch 0.0F - our units are 20 per hardness-second. */
	IndevBlock_Define(INDEV_BLOCK_WORKBENCH,   "Workbench",  96, 97,  98,  4, SOUND_WOOD,  50);
	IndevBlock_Define(INDEV_BLOCK_CHEST,       "Chest",     105, 104, 103, 105, SOUND_WOOD, 50);
	IndevBlock_Define(INDEV_BLOCK_FURNACE,     "Furnace",   102, 101,  99, 102, SOUND_STONE, 70);
	IndevBlock_Define(INDEV_BLOCK_FURNACE_LIT, "Furnace (lit)", 102, 101, 100, 102, SOUND_STONE, 70);

	/* Directional variants (placement rotates the canonical block so the */
	/*  front faces the player, like BlockFurnace.setDefaultDirection) */
	for (k = 0; k < 4; k++) {
		IndevBlock_Define((BlockID)(INDEV_BLOCK_CHEST_V0 + k), "Chest",
						  105, 104, 104, 105, SOUND_WOOD, 50);
		Block_Tex((BlockID)(INDEV_BLOCK_CHEST_V0 + k), metaFace[k]) = 103;
		Block_DefineCustom((BlockID)(INDEV_BLOCK_CHEST_V0 + k), false);

		IndevBlock_Define((BlockID)(INDEV_BLOCK_FURN_V0 + k),  "Furnace",
						  102, 101, 101, 102, SOUND_STONE, 70);
		Block_Tex((BlockID)(INDEV_BLOCK_FURN_V0 + k), metaFace[k]) = 99;
		Block_DefineCustom((BlockID)(INDEV_BLOCK_FURN_V0 + k), false);

		IndevBlock_Define((BlockID)(INDEV_BLOCK_FURNL_V0 + k), "Furnace (lit)",
						  102, 101, 101, 102, SOUND_STONE, 70);
		Block_Tex((BlockID)(INDEV_BLOCK_FURNL_V0 + k), metaFace[k]) = 100;
		Block_DefineCustom((BlockID)(INDEV_BLOCK_FURNL_V0 + k), false);
	}

	/* Torch: a thin 2/16-wide, 10/16-tall column (BlockTorch's stick model, */
	/*  NOT a flower-style X sprite), walk-through, instant to break, and a */
	/*  light source - Indev registers it with setLightValue(14/16). The */
	/*  lamp (white) nibble drives fancy lighting's light propagation. */
	IndevBlock_Define(INDEV_BLOCK_TORCH, "Torch", 106, 106, 106, 106, SOUND_WOOD, 0);
	Blocks.Collide[INDEV_BLOCK_TORCH]         = COLLIDE_NONE;
	Blocks.ExtendedCollide[INDEV_BLOCK_TORCH] = COLLIDE_NONE;
	Blocks.Draw[INDEV_BLOCK_TORCH]            = DRAW_TRANSPARENT;
	Blocks.BlocksLight[INDEV_BLOCK_TORCH]     = false;
	Blocks.Brightness[INDEV_BLOCK_TORCH]      = 14 << FANCY_LIGHTING_LAMP_SHIFT;
	Vec3_Set(Blocks.MinBB[INDEV_BLOCK_TORCH],  7.0f/16.0f, 0.0f,        7.0f/16.0f);
	Vec3_Set(Blocks.MaxBB[INDEV_BLOCK_TORCH],  9.0f/16.0f, 10.0f/16.0f, 9.0f/16.0f);
	/* Top face uses tile 117 (torch tile shifted down 1px in the patcher) so */
	/*  the bounds crop (x 7-9, y 7-9) shows the ember, matching genuine */
	/*  renderBlockTorch's top UVs of x 7-9, y 6-8. */
	Block_Tex(INDEV_BLOCK_TORCH, FACE_YMAX) = 117;
	Block_DefineCustom(INDEV_BLOCK_TORCH, false);

	/* Lit furnaces also glow (BlockFurnace active: setLightValue(14/16)) */
	Blocks.Brightness[INDEV_BLOCK_FURNACE_LIT] = 14 << FANCY_LIGHTING_LAMP_SHIFT;
	for (k = 0; k < 4; k++) {
		Blocks.Brightness[INDEV_BLOCK_FURNL_V0 + k] = 14 << FANCY_LIGHTING_LAMP_SHIFT;
	}

	/* Farmland: dirt sides/bottom, tilled top (tile 116 dry / 115 wet), */
	/*  hardness dirt-like (0.6s). Only obtainable by hoeing - not placeable. */
	IndevBlock_Define(INDEV_BLOCK_FARMLAND,     "Farmland", 116, 2, 2, 2, SOUND_GRAVEL, 12);
	IndevBlock_Define(INDEV_BLOCK_FARMLAND_WET, "Farmland", 115, 2, 2, 2, SOUND_GRAVEL, 12);
	Blocks.CanPlace[INDEV_BLOCK_FARMLAND]     = false;
	Blocks.CanPlace[INDEV_BLOCK_FARMLAND_WET] = false;

	/* Indev rebalanced the CLASSIC blocks' hardness (Block.java setHardness
	    registrations, x20 = our tick units). Applied only here - the c0.30
	    gamemode never runs this function, so classic stays faithful. Notable
	    changes vs c0.30: stone 1.0->1.5s, cobble/planks/brick/mossy/slabs
	    1.5->2.0s (brick was a 0-hardness quirk in classic!), dirt/sand
	    0.6->0.5s, log 2.5->2.0s, flowing lava 100->0s (a genuine quirk). */
	{
		static const struct { cc_uint8 b; cc_uint16 h; } indevHardness[] = {
			{ BLOCK_STONE, 30 },  { BLOCK_GRASS, 12 },  { BLOCK_DIRT, 10 },
			{ BLOCK_COBBLE, 40 }, { BLOCK_WOOD, 40 },   { BLOCK_SAND, 10 },
			{ BLOCK_GRAVEL, 12 }, { BLOCK_GOLD_ORE, 60 },{ BLOCK_IRON_ORE, 60 },
			{ BLOCK_COAL_ORE, 60 },{ BLOCK_LOG, 40 },   { BLOCK_LEAVES, 4 },
			{ BLOCK_SPONGE, 12 }, { BLOCK_GLASS, 6 },   { BLOCK_GOLD, 60 },
			{ BLOCK_IRON, 100 },  { BLOCK_DOUBLE_SLAB, 40 }, { BLOCK_SLAB, 40 },
			{ BLOCK_BRICK, 40 },  { BLOCK_BOOKSHELF, 30 }, { BLOCK_MOSSY_ROCKS, 40 },
			{ BLOCK_OBSIDIAN, 200 }, { BLOCK_LAVA, 0 }
		};
		int hi;
		for (hi = 0; hi < (int)Array_Elems(indevHardness); hi++) {
			SurvivalTest_SetHardness(indevHardness[hi].b, indevHardness[hi].h);
		}
		for (hi = BLOCK_RED; hi <= BLOCK_WHITE; hi++) SurvivalTest_SetHardness((BlockID)hi, 16);
	}

	/* Crop stages 0-7: tiles 107-114 drawn as the genuine "#" row pattern */
	/*  (Builder_DrawCrops), walk-through, instant break, not placeable */
	/*  (planted via seeds). */
	for (k = 0; k < 8; k++) {
		BlockID id = (BlockID)(INDEV_BLOCK_CROPS_0 + k);
		IndevBlock_Define(id, "Crops", 107 + k, 107 + k, 107 + k, 107 + k, SOUND_GRASS, 0);
		Blocks.Collide[id]         = COLLIDE_NONE;
		Blocks.ExtendedCollide[id] = COLLIDE_NONE;
		Blocks.Draw[id]            = DRAW_SPRITE;
		Blocks.BlocksLight[id]     = false;
		Blocks.CanPlace[id]        = false;
		/* BlockCrops.setBlockBounds(0, 0, 0, 1, 0.25, 1): the pick/outline
		    box is the full tile but only 4/16 tall, not a whole cube. */
		Vec3_Set(Blocks.MinBB[id], 0.0f, 0.0f,        0.0f);
		Vec3_Set(Blocks.MaxBB[id], 1.0f, 4.0f/16.0f,  1.0f);
		Block_DefineCustom(id, false);
	}
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
	if (Indev_IsChestBlock(b)) return INDEV_CONTAINER_CHEST;
	if (Indev_IsFurnaceIdle(b) || Indev_IsFurnaceLit(b)) return INDEV_CONTAINER_FURNACE;
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

/* Whether the held id renders as the first-person extruded item sprite */
/*  (ItemRenderer's non-block branch): item ids and sprite-type blocks. */
cc_bool IndevTest_HeldIsExtruded(int id) {
	if (!IndevTest_Enabled) return false;
	if (id >= 256) return indev_itemsTexId != 0;
	return IndevTest_DropIsSprite(id);
}

/* Binds the texture for the extruded held item and returns its UV rect - */
/*  items.png for item ids, the terrain tile for sprite blocks. */
cc_bool IndevTest_BindHeldTexture(int id, TextureRec* rec) {
	TextureLoc loc;
	int texIndex;
	if (id >= 256) {
		if (!IndevTest_ItemSpriteUV(id, &rec->u1, &rec->v1, &rec->u2, &rec->v2)) return false;
		if (!indev_itemsTexId) return false;
		Gfx_BindTexture(indev_itemsTexId);
		return true;
	}
	loc  = Block_Tex((BlockID)id, FACE_XMIN);
	*rec = Atlas1D_TexRec(loc, 1, &texIndex);
	Atlas1D_Bind(texIndex);
	return true;
}

/* RenderItem.doRender: only blocks with renderType 0 (standard cubes) drop */
/*  as miniature 3D blocks - everything else (flowers/saplings/mushrooms, */
/*  torches, and all item ids) renders as an upright sprite quad. */
cc_bool IndevTest_DropIsSprite(int id) {
	if (!IndevTest_Enabled) return false;
	if (id >= 256) return true;
	return Blocks.Draw[id] == DRAW_SPRITE || id == INDEV_BLOCK_TORCH;
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
		/* Preserve the facing across the lit/unlit swap */
		BlockID curBlock = World_GetBlock(te->pos.x, te->pos.y, te->pos.z);
		int meta = IndevTest_BlockFacingMeta(curBlock);
		Game_ChangeBlock(te->pos.x, te->pos.y, te->pos.z,
			IndevTest_FacingVariant(burning ? INDEV_BLOCK_FURNACE_LIT : INDEV_BLOCK_FURNACE, meta));
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

/*########################################################################################################################*
*-------------------------------------------------Day/night cycle (World.java)--------------------------------------------*
*#########################################################################################################################*/
/* worldTime ticks at 20Hz and wraps at 24000 (20 minutes per day). All the
    colour/brightness curves are exact ports of World.getCelestialAngle /
    getSkyColor / getFogColor / getCloudColor / getSkyBrightness. */
static int indev_worldTime;
static int indev_skyBright = 15; /* Environment.SkyBrightness; > 15 = always day */
static PackedCol indev_baseSky, indev_baseFog, indev_baseClouds;
static cc_bool   indev_baseColsKnown;
static int       indev_lastSkyLight = -1;

int  IndevTest_WorldTime(void)      { return indev_worldTime; }
void IndevTest_SetWorldTime(int t)  { indev_worldTime = t >= 0 ? t % 24000 : 0; }
void IndevTest_SetSkyBrightness(int b) { indev_skyBright = b; }
int  IndevTest_SkyBrightness(void)      { return indev_skyBright; }

/* Full-daylight base colours for .mclevel saving - the live Env colours */
/*  are time-of-day scaled, and saving those (e.g. at night) would bake a */
/*  black sky into the file as the map's base colour. */
PackedCol IndevTest_BaseSkyCol(void)    { return indev_baseColsKnown ? indev_baseSky    : Env.SkyCol; }
PackedCol IndevTest_BaseFogCol(void)    { return indev_baseColsKnown ? indev_baseFog    : Env.FogCol; }
PackedCol IndevTest_BaseCloudsCol(void) { return indev_baseColsKnown ? indev_baseClouds : Env.CloudsCol; }

static float Indev_CelestialAngle(void) {
	if (indev_skyBright > 15) return 0.0f; /* "paradise" maps: always noon */
	return (float)indev_worldTime / 24000.0f - 0.15f;
}

/* clamp01(cos(angle * 2PI) * mul + add) */
static float Indev_DayFactor(float mul, float add) {
	float f = Math_CosF(Indev_CelestialAngle() * MATH_PI * 2.0f) * mul + add;
	if (f < 0.0f) f = 0.0f;
	if (f > 1.0f) f = 1.0f;
	return f;
}

static PackedCol Indev_ScaleColor(PackedCol base, float r, float g, float b) {
	return PackedCol_Make((cc_uint8)(PackedCol_R(base) * r),
						  (cc_uint8)(PackedCol_G(base) * g),
						  (cc_uint8)(PackedCol_B(base) * b), 255);
}

/* World.getSkyBrightness: the sky light level, 15 at noon down to 4 at night */
static int Indev_SkyLight(void) {
	float f = Indev_DayFactor(1.5f, 0.5f);
	int light = (int)(f * ((float)(15 * indev_skyBright) / 15.0f - 4.0f) + 4.0f);
	if (light > 15) light = 15;
	if (light < 4)  light = 4;
	return light;
}

static void Indev_TickDayNight(void) {
	float f;
	int   light;

	indev_worldTime++;
	if (indev_worldTime >= 24000) indev_worldTime = 0;
	if (!indev_baseColsKnown || !World.Loaded) return;

	/* getSkyColor: base * clamp01(cos*2 + 0.5) */
	f = Indev_DayFactor(2.0f, 0.5f);
	Env_SetSkyCol(Indev_ScaleColor(indev_baseSky, f, f, f));
	/* getFogColor: floors keep dawn/dusk fog slightly blue-tinted */
	Env_SetFogCol(Indev_ScaleColor(indev_baseFog,
		f * 0.94f + 0.06f, f * 0.94f + 0.06f, f * 0.91f + 0.09f));
	/* getCloudColor */
	Env_SetCloudsCol(Indev_ScaleColor(indev_baseClouds,
		f * 0.9f + 0.1f, f * 0.9f + 0.1f, f * 0.85f + 0.15f));

	/* skylightSubtracted: dim the world's sun/shadow lighting with the sky */
	/*  light level (4..15). Sun/shadow changes trigger a relight, so only */
	/*  apply when the level actually moves (11 steps across dawn/dusk). */
	light = Indev_SkyLight();
	if (light != indev_lastSkyLight) {
		indev_lastSkyLight = light;
		Env_SetSunCol(PackedCol_Scale(ENV_DEFAULT_SUN_COLOR,    (float)light / 15.0f));
		Env_SetShadowCol(PackedCol_Scale(ENV_DEFAULT_SHADOW_COLOR, (float)light / 15.0f));
	}
}

/*########################################################################################################################*
*-------------------------------------------------Farming (random ticks)--------------------------------------------------*
*#########################################################################################################################*/
/* World.getBlockLightValue: combined light at a position - sky contribution
    (the day/night sky level when the column is sky-lit) vs the fancy-
    lighting lamp/lava block level. Drives crop growth, the monster
    darkness-spawn rule, and light-accelerated monster aging. */
int IndevTest_LightLevel(int x, int y, int z) {
	int light = 0;
	if (Lighting.IsLit(x, y, z)) light = Indev_SkyLight();
	if (Lighting_Mode == LIGHTING_MODE_FANCY) {
		int block = FancyLighting_BlockLightLevel(x, y, z);
		if (block > light) light = block;
	}
	return light;
}

/* The live sky light level - what zombie/skeleton daylight burning tests as
    "worldObj.skylightSubtracted > 7" (the deobf name is a misnomer: World.tick
    eases that field toward getSkyBrightness every tick, so it IS the sky
    light, 15 at noon / 4 at night). */
int IndevTest_CurSkyLight(void) {
	if (!IndevTest_Enabled) return 15;
	return Indev_SkyLight();
}

static cc_bool Indev_GrowLightOk(int x, int y, int z) {
	return IndevTest_LightLevel(x, y, z) >= 9;
}

static cc_bool Indev_WaterNear(int x, int y, int z) {
	int wx, wy, wz;
	BlockID b;
	/* BlockFarmland.updateTick: any water within x/z +-4, at y or y+1 */
	for (wx = x - 4; wx <= x + 4; wx++) {
		for (wy = y; wy <= y + 1; wy++) {
			for (wz = z - 4; wz <= z + 4; wz++) {
				if (!World_Contains(wx, wy, wz)) continue;
				b = World_GetBlock(wx, wy, wz);
				if (b == BLOCK_WATER || b == BLOCK_STILL_WATER) return true;
			}
		}
	}
	return false;
}

/* BlockFarmland.updateTick (gated 1-in-5 per random tick): hydrate from */
/*  nearby water; dry out; revert to dirt once dry with no crops above. */
static void Indev_TickFarmland(int index, BlockID block) {
	int x, y, z;
	BlockID above;
	World_Unpack(index, x, y, z);
	if (Random_Next(&indev_teRng, 5) != 0) return;

	above = y + 1 < World.Height ? World_GetBlock(x, y + 1, z) : BLOCK_AIR;
	/* solid cover reverts it (genuine does this on neighbour change) */
	if (Blocks.Collide[above] == COLLIDE_SOLID) {
		Game_UpdateBlock(x, y, z, BLOCK_DIRT);
		return;
	}

	if (Indev_WaterNear(x, y, z)) {
		if (block != INDEV_BLOCK_FARMLAND_WET) Game_UpdateBlock(x, y, z, INDEV_BLOCK_FARMLAND_WET);
		return;
	}
	if (block == INDEV_BLOCK_FARMLAND_WET) {
		Game_UpdateBlock(x, y, z, INDEV_BLOCK_FARMLAND); /* moisture decays */
		return;
	}
	if (!Indev_IsCrops(above)) {
		Game_UpdateBlock(x, y, z, BLOCK_DIRT); /* dry + nothing planted */
	}
}

/* BlockCrops.updateTick: growth rate from the farmland below (1 dry / 3 */
/*  wet, neighbours at quarter weight), halved when crowded by adjacent */
/*  crops; then a 1-in-(100/rate) roll advances the stage. */
static void Indev_TickCrops(int index, BlockID block) {
	float rate = 1.0f, f;
	int x, y, z, dx, dz;
	BlockID below;
	cc_bool rowX, rowZ, diag;
	World_Unpack(index, x, y, z);

	if (block >= INDEV_BLOCK_CROPS_7) return;
	if (y + 1 >= World.Height || !Indev_GrowLightOk(x, y + 1, z)) return;

	for (dx = -1; dx <= 1; dx++) {
		for (dz = -1; dz <= 1; dz++) {
			if (!World_Contains(x + dx, y - 1, z + dz)) continue;
			below = World_GetBlock(x + dx, y - 1, z + dz);
			f = 0.0f;
			if (below == INDEV_BLOCK_FARMLAND)     f = 1.0f;
			if (below == INDEV_BLOCK_FARMLAND_WET) f = 3.0f;
			if (dx || dz) f /= 4.0f;
			rate += f;
		}
	}

	#define CROP_AT(cx, cz) (World_Contains((cx), y, (cz)) && Indev_IsCrops(World_GetBlock((cx), y, (cz))))
	rowX = CROP_AT(x - 1, z)     || CROP_AT(x + 1, z);
	rowZ = CROP_AT(x, z - 1)     || CROP_AT(x, z + 1);
	diag = CROP_AT(x - 1, z - 1) || CROP_AT(x + 1, z - 1) ||
		   CROP_AT(x + 1, z + 1) || CROP_AT(x - 1, z + 1);
	#undef CROP_AT
	if (diag || (rowX && rowZ)) rate /= 2.0f;

	if (Random_Next(&indev_teRng, (int)(100.0f / rate)) == 0) {
		Game_UpdateBlock(x, y, z, (BlockID)(block + 1));
	}
}

static void Indev_RegisterFarmTicks(void) {
	int k;
	Physics.OnRandomTick[INDEV_BLOCK_FARMLAND]     = Indev_TickFarmland;
	Physics.OnRandomTick[INDEV_BLOCK_FARMLAND_WET] = Indev_TickFarmland;
	for (k = 0; k < 8; k++) {
		Physics.OnRandomTick[INDEV_BLOCK_CROPS_0 + k] = Indev_TickCrops;
	}
}

/* ItemHoe.onItemUse: turns grass (with non-solid above) or dirt into dry */
/*  farmland, wearing the tool; hoed GRASS has a 1-in-8 seed drop. */
/* ItemSeeds.onItemUse: plants stage-0 crops above farmland. */
cc_bool IndevTest_UseHeldItem(int heldId, IVec3 pos) {
	BlockID target, above;
	cc_bool solidAbove;
	if (!IndevTest_Enabled) return false;

	target     = World_GetBlock(pos.x, pos.y, pos.z);
	above      = pos.y + 1 < World.Height ? World_GetBlock(pos.x, pos.y + 1, pos.z) : BLOCK_AIR;
	solidAbove = Blocks.Collide[above] == COLLIDE_SOLID;

	if (IndevTest_IsHoe(heldId)) {
		if ((target != BLOCK_GRASS || solidAbove) && target != BLOCK_DIRT) return false;
		Game_ChangeBlock(pos.x, pos.y, pos.z, INDEV_BLOCK_FARMLAND);
		SurvivalTest_DamageHeldItem(1);
		if (target == BLOCK_GRASS && Random_Next(&indev_teRng, 8) == 0) {
			Vec3 dp;
			dp.x = pos.x + Random_Float(&indev_teRng) * 0.7f + 0.15f;
			dp.y = pos.y + 1.2f;
			dp.z = pos.z + Random_Float(&indev_teRng) * 0.7f + 0.15f;
			SurvivalTest_SpawnDropWorld(dp, 256 + 39, 1); /* Seeds */
		}
		return true;
	}

	if (heldId == 256 + 39) { /* Seeds */
		if (!Indev_IsFarmland(target) || above != BLOCK_AIR) return false;
		Game_ChangeBlock(pos.x, pos.y + 1, pos.z, INDEV_BLOCK_CROPS_0);
		SurvivalTest_ConsumeHeld();
		return true;
	}
	return false;
}

/*########################################################################################################################*
*--------------------------------------------Sun, moon and stars (renderSky)----------------------------------------------*
*#########################################################################################################################*/
/* RenderGlobal.renderSky: sun quad (+-30 at y=+100, /terrain/sun.png), moon
    quad (+-20 at y=-100, flipped UVs), and 500 stars baked from
    Random(10842) with cumulative random rotations - all spinning around the
    X axis by celestialAngle * 360 with additive blending, fog off, no depth
    writes. ClassiCube's RNG is java.util.Random-compatible, so the star
    sizes/angles match genuine (composition handedness may mirror the field,
    which is indistinguishable for a random sky). */
#define INDEV_STARS 500
static struct VertexColoured indev_starVerts[INDEV_STARS * 4];
static cc_bool indev_starsBaked;
static GfxResourceID indev_starVb, indev_skyQuadVb;

static void Indev_TransformRow(float x, float y, float z, const struct Matrix* m,
							   struct VertexColoured* v) {
	v->x = x * m->row1.x + y * m->row2.x + z * m->row3.x + m->row4.x;
	v->y = x * m->row1.y + y * m->row2.y + z * m->row3.y + m->row4.y;
	v->z = x * m->row1.z + y * m->row2.z + z * m->row3.z + m->row4.z;
}

static void Indev_BakeStars(void) {
	RNGState rng;
	struct Matrix m = Matrix_Identity, r, tmp;
	float rx, ry, rz, s;
	int i;
	Random_Seed(&rng, 10842);

	for (i = 0; i < INDEV_STARS; i++) {
		/* the genuine display list never resets the matrix - rotations */
		/*  accumulate from star to star */
		rx = Random_Float(&rng) * 360.0f * MATH_DEG2RAD;
		ry = Random_Float(&rng) * 360.0f * MATH_DEG2RAD;
		rz = Random_Float(&rng) * 360.0f * MATH_DEG2RAD;
		Matrix_RotateX(&r, rx); tmp = m; Matrix_Mul(&m, &r, &tmp);
		Matrix_RotateY(&r, ry); tmp = m; Matrix_Mul(&m, &r, &tmp);
		Matrix_RotateZ(&r, rz); tmp = m; Matrix_Mul(&m, &r, &tmp);
		s = 0.25f + Random_Float(&rng) * 0.25f;

		Indev_TransformRow(-s, -100.0f,  s, &m, &indev_starVerts[i * 4 + 0]);
		Indev_TransformRow( s, -100.0f,  s, &m, &indev_starVerts[i * 4 + 1]);
		Indev_TransformRow( s, -100.0f, -s, &m, &indev_starVerts[i * 4 + 2]);
		Indev_TransformRow(-s, -100.0f, -s, &m, &indev_starVerts[i * 4 + 3]);
	}
}

static void Indev_SkyQuad(struct VertexTextured* v, float size, float y,
						  cc_bool flipUV) {
	float u0 = flipUV ? 1.0f : 0.0f, u1 = 1.0f - u0;
	v[0].x = -size; v[0].y = y; v[0].z = -size; v[0].Col = PACKEDCOL_WHITE; v[0].U = u0; v[0].V = u0;
	v[1].x =  size; v[1].y = y; v[1].z = -size; v[1].Col = PACKEDCOL_WHITE; v[1].U = u1; v[1].V = u0;
	v[2].x =  size; v[2].y = y; v[2].z =  size; v[2].Col = PACKEDCOL_WHITE; v[2].U = u1; v[2].V = u1;
	v[3].x = -size; v[3].y = y; v[3].z =  size; v[3].Col = PACKEDCOL_WHITE; v[3].U = u0; v[3].V = u1;
}

/* World.getStarBrightness: clamp01(1 - (cos(a*2PI)*2 + 12/16)) squared * 0.5 */
static float Indev_StarBrightness(void) {
	float f = 1.0f - (Math_CosF(Indev_CelestialAngle() * MATH_PI * 2.0f) * 2.0f + 12.0f/16.0f);
	if (f < 0.0f) f = 0.0f;
	if (f > 1.0f) f = 1.0f;
	return f * f * 0.5f;
}

void IndevTest_RenderSky(void) {
	struct Matrix view, rot, m;
	struct VertexTextured quad[4];
	PackedCol col;
	float bright;
	cc_bool hadFog;
	int i, b;

	if (!IndevTest_Enabled || !World.Loaded) return;

	if (!indev_starsBaked) { Indev_BakeStars(); indev_starsBaked = true; }
	if (!indev_starVb)   indev_starVb   = Gfx_CreateDynamicVb(VERTEX_FORMAT_COLOURED, INDEV_STARS * 4);
	if (!indev_skyQuadVb) indev_skyQuadVb = Gfx_CreateDynamicVb(VERTEX_FORMAT_TEXTURED, 4);
	if (!indev_starVb || !indev_skyQuadVb) return;

	/* sky-space rotation (celestial angle around X) then the camera's view */
	/*  rotation WITHOUT its translation - the sky is centred on the eye */
	view = Gfx.View;
	view.row4.x = 0.0f; view.row4.y = 0.0f; view.row4.z = 0.0f;
	Matrix_RotateX(&rot, Indev_CelestialAngle() * MATH_PI * 2.0f);
	Matrix_Mul(&m, &rot, &view);
	Gfx_LoadMatrix(MATRIX_VIEW, &m);

	hadFog = Gfx_GetFog();
	if (hadFog) Gfx_SetFog(false);
	Gfx_SetDepthWrite(false);
	Gfx_SetAlphaTest(false);
	Gfx_SetAlphaBlendingAdditive(true); /* dst + src, like glBlendFunc(ONE, ONE) */

	if (indev_sunTexId) {
		Gfx_SetVertexFormat(VERTEX_FORMAT_TEXTURED);
		Gfx_BindTexture(indev_sunTexId);
		Indev_SkyQuad(quad, 30.0f, 100.0f, false);
		Gfx_SetDynamicVbData(indev_skyQuadVb, quad, 4);
		Gfx_DrawVb_IndexedTris(4);
	}
	if (indev_moonTexId) {
		Gfx_SetVertexFormat(VERTEX_FORMAT_TEXTURED);
		Gfx_BindTexture(indev_moonTexId);
		Indev_SkyQuad(quad, 20.0f, -100.0f, true);
		Gfx_SetDynamicVbData(indev_skyQuadVb, quad, 4);
		Gfx_DrawVb_IndexedTris(4);
	}

	bright = Indev_StarBrightness();
	if (bright > 0.002f) {
		b   = (int)(bright * 255.0f);
		col = PackedCol_Make(b, b, b, 255);
		for (i = 0; i < INDEV_STARS * 4; i++) indev_starVerts[i].Col = col;

		Gfx_SetVertexFormat(VERTEX_FORMAT_COLOURED);
		Gfx_SetDynamicVbData(indev_starVb, indev_starVerts, INDEV_STARS * 4);
		Gfx_DrawVb_IndexedTris(INDEV_STARS * 4);
	}

	Gfx_SetAlphaBlendingAdditive(false);
	Gfx_SetAlphaBlending(false);
	Gfx_SetDepthWrite(true);
	if (hadFog) Gfx_SetFog(true);
	Gfx_LoadMatrix(MATRIX_VIEW, &Gfx.View);
}

static void IndevTest_ContextLost(void* obj) {
	Gfx_DeleteDynamicVb(&indev_starVb);
	Gfx_DeleteDynamicVb(&indev_skyQuadVb);
}

static void IndevTest_Tick(struct ScheduledTask* task) {
	int i;
	if (!IndevTest_Enabled) return;
	Indev_TickDayNight();
	for (i = 0; i < INDEV_TE_MAX; i++) {
		if (!indev_tes[i].used || indev_tes[i].kind != INDEV_CONTAINER_FURNACE) continue;
		Furnace_Tick(&indev_tes[i]);
	}
}

/* Container-block removal: a chest scatters its contents (BlockChest.
    onBlockRemoval); a furnace's contents are silently destroyed (faithful -
    BlockContainer.onBlockRemoval only removes the tile entity). PUBLIC so
    the explosion path (which removes blocks without raising BlockChanged)
    can drive the same lifecycle - otherwise TNT/creeper blasts leaked the
    tile entity and ate chest contents without the scatter. */
void IndevTest_NotifyBlockRemoved(IVec3 coords, BlockID oldBlock) {
	int i;
	if (!IndevTest_Enabled) return;
	if (IndevTest_ContainerKindOf(oldBlock) == INDEV_CONTAINER_NONE) return;

	i = IndevTE_Find(coords);
	if (i < 0) return;
	if (Indev_IsChestBlock(oldBlock)) IndevTE_Scatter(&indev_tes[i]);
	if (indev_openTE == i) indev_openTE = -1; /* screen falls back to the discard slot */
	indev_tes[i].used = false;
}

/* Tile entity lifecycle + placement rotation, driven off block changes. */
static void IndevTest_BlockChanged(void* obj, IVec3 coords, BlockID oldBlock, BlockID block) {
	cc_bool oldFurn, nowFurn;
	struct Entity* p;
	int q, meta;
	if (!IndevTest_Enabled) return;

	oldFurn = Indev_IsFurnaceIdle(oldBlock) || Indev_IsFurnaceLit(oldBlock);
	nowFurn = Indev_IsFurnaceIdle(block)    || Indev_IsFurnaceLit(block);
	if (oldFurn && nowFurn) return; /* lit/unlit swap - state survives */

	/* crops pop off when their farmland vanishes (BlockFlower.canBlockStay) */
	if (Indev_IsFarmland(oldBlock) && !Indev_IsFarmland(block) &&
		coords.y + 1 < World.Height &&
		Indev_IsCrops(World_GetBlock(coords.x, coords.y + 1, coords.z))) {
		Game_ChangeBlock(coords.x, coords.y + 1, coords.z, BLOCK_AIR);
	}

	IndevTest_NotifyBlockRemoved(coords, oldBlock);

	/* Player placed a canonical chest/furnace: rotate it so the front faces */
	/*  the player (BlockFurnace.setDefaultDirection / Beta onBlockPlacedBy: */
	/*  quadrant of the placer's yaw picks metadata 2/5/3/4). */
	if (block == INDEV_BLOCK_CHEST || block == INDEV_BLOCK_FURNACE) {
		p = &Entities.CurPlayer->Base;
		q = (int)Math_Floor(p->Yaw * 4.0f / 360.0f + 0.5f) & 3;
		/* ClassiCube's yaw is 180 degrees from Beta's convention (live-test */
		/*  showed fronts facing AWAY) - so the metadata picks are swapped */
		/*  north<->south / east<->west vs onBlockPlacedBy's 2/5/3/4. */
		meta = q == 0 ? 3 : (q == 1 ? 4 : (q == 2 ? 2 : 5));
		Game_UpdateBlock(coords.x, coords.y, coords.z,
			IndevTest_FacingVariant(block, meta)); /* no event - avoids recursion */
	}
}

/* Map loading runs Game_Reset, which wipes ALL custom block definitions - */
/*  the Indev block ids survive in the map data but rendered as undefined */
/*  (the reported "green blocks"). Re-define them once the map is in. Also */
/*  snapshot the env colours as the day/night cycle's full-daylight base */
/*  (a loaded .mclevel has applied its Environment colours by now), and */
/*  switch to fancy lighting so torches/lit furnaces cast real light. */
static void OnNewMapLoaded(void) {
	if (!IndevTest_Enabled) return;
	IndevBlocks_Define();
	Indev_RegisterFarmTicks(); /* in case physics re-registered its handlers */

	indev_baseSky      = Env.SkyCol;
	indev_baseFog      = Env.FogCol;
	indev_baseClouds   = Env.CloudsCol;
	indev_baseColsKnown = true;
	indev_lastSkyLight  = -1; /* reapply sun/shadow for the new map */

	if (Lighting_Mode != LIGHTING_MODE_FANCY && !Lighting_ModeLockedByServer) {
		Lighting_SetMode(LIGHTING_MODE_FANCY, false);
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
	TextureEntry_Register(&sun_entry);
	TextureEntry_Register(&moon_entry);

	Random_Seed(&indev_teRng, (int)Game.Time + 1);
	Indev_RegisterFarmTicks();
	Event_Register_(&UserEvents.BlockChanged, NULL, IndevTest_BlockChanged);
	Event_Register_(&GfxEvents.ContextLost,   NULL, IndevTest_ContextLost);
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
	default:
		if (b <= 49) return b; /* classic identity */
		/* directional variants: same Indev block, facing carried by the */
		/*  Data array metadata nibble (IndevTest_BlockFacingMeta) */
		if (Indev_IsChestBlock(b))   return 54;
		if (Indev_IsFurnaceIdle(b))  return 61;
		if (Indev_IsFurnaceLit(b))   return 62;
		if (Indev_IsFarmland(b))     return 60; /* moisture in the Data nibble */
		if (Indev_IsCrops(b))        return 59; /* stage in the Data nibble */
		return 1; /* anything else -> stone */
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
	case 59: return INDEV_BLOCK_CROPS_0;  /* + stage from the Data nibble */
	case 60: return INDEV_BLOCK_FARMLAND; /* wet variant from the Data nibble */
	case 61: return 68; /* furnace idle */
	case 62: return 69; /* furnace lit */
	default: return b <= 49 ? b : 0;
	}
}

/* Whether a tile entity exists at the position (for .mclevel save: every */
/*  container block must get a TileEntity entry, even never-opened ones - */
/*  genuine Indev NPE-crashes opening a chest with no tile entity). */
cc_bool IndevTest_HasTE(int x, int y, int z) {
	IVec3 pos;
	pos.x = x; pos.y = y; pos.z = z;
	return IndevTE_Find(pos) >= 0;
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
	OnNewMap, /* OnNewMap */
	OnNewMapLoaded /* re-define the Indev blocks Game_Reset wiped */
};
