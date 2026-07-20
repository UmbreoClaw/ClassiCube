#include "IndevTest.h"
#include "Game.h"
#include "Options.h"
#include "Chat.h"
#include "SurvivalTest.h"
#include "SurvivalNet.h"
#include "IndevArmor.h"
#include "Funcs.h"
#include "Graphics.h"
#include "TexturePack.h"
#include "Block.h"
#include "Inventory.h"
#include "Audio.h"
#include "Platform.h"
#include "String_.h"
#include "World.h"
#include "Event.h"
#include "ExtMath.h"
#include "Entity.h"
#include "Lighting.h"
#include "BlockPhysics.h"
#include "Picking.h"
#include "IndevFire.h"
#include "Bitmap.h"
#include "Logger.h"

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

/* art/kz.png - the painting art atlas (the authentic in-20100223 sheet:
    Resources.c composites the two cells b1.7.3 later redrew back over it) */
static GfxResourceID indev_kzTexId;
static void KzPngProcess(struct Stream* stream, const cc_string* name) {
	Game_UpdateTexture(&indev_kzTexId, stream, name, NULL, NULL);
}
static struct TextureEntry kz_entry = { "kz.png", KzPngProcess };

GfxResourceID IndevTest_KzTex(void) { return indev_kzTexId; }

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
/* Genuine renderSky draws these with glBlendFunc(GL_ONE, GL_ONE) - texture
    alpha is completely IGNORED. The engine's additive blend is
    (SRC_ALPHA, ONE), which multiplies by alpha instead: any texture pack
    whose sun/moon carry a real alpha channel (feathered glow) gets dimmed
    to near-invisibility. Forcing alpha to 255 at load makes the two
    formulas identical for these textures, on every backend. */
static void CelestialPngProcess(GfxResourceID* texId, struct Stream* stream,
								const cc_string* name) {
	struct Bitmap bmp;
	cc_result res;
	int i, size;

	res = Png_Decode(&bmp, stream);
	if (res) { Logger_SysWarn2(res, "decoding", name); return; }

	if (Game_ValidateBitmap(name, &bmp)) {
		size = bmp.width * bmp.height;
		for (i = 0; i < size; i++) {
			bmp.scan0[i] |= BITMAPCOLOR_A_MASK;
		}
		Gfx_RecreateTexture(texId, &bmp, TEXTURE_FLAG_MANAGED, false);
	}
	Mem_Free(bmp.scan0);
}
static void SunPngProcess(struct Stream* stream, const cc_string* name) {
	CelestialPngProcess(&indev_sunTexId, stream, name);
}
static struct TextureEntry sun_entry = { "sun.png", SunPngProcess };
static void MoonPngProcess(struct Stream* stream, const cc_string* name) {
	CelestialPngProcess(&indev_moonTexId, stream, name);
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

/* Item display name for chat feedback (NULL when the id isn't a known item). */
const char* IndevTest_ItemName(int id) {
	int i, local = id - 256;
	if (local < 0 || !IndevTest_Enabled) return NULL;
	for (i = 0; i < (int)Array_Elems(indevItems); i++) {
		if (indevItems[i].id == local) return indevItems[i].name;
	}
	return NULL;
}

/* Caseless compare of an item's display name against a query, with spaces */
/*  and underscores skipped on both sides - so "Iron Pickaxe" matches */
/*  "iron_pickaxe", "ironpickaxe" and "iron pickaxe" alike. */
static cc_bool Indev_ItemNameMatches(const char* name, const cc_string* query) {
	int i = 0, j = 0;
	char a, b;
	for (;;) {
		while (name[i] == ' ') i++;
		while (j < query->length && (query->buffer[j] == ' ' || query->buffer[j] == '_')) j++;
		a = name[i]; b = j < query->length ? query->buffer[j] : '\0';
		if (!a || !b) return !a && !b;
		if (a >= 'A' && a <= 'Z') a += 32;
		if (b >= 'A' && b <= 'Z') b += 32;
		if (a != b) return false;
		i++; j++;
	}
}

/* Finds an Indev-layer BLOCK by display name, canonicalised - checked
    before the engine's own name lookup so the functional Indev blocks
    shadow same-named classic/CPE decoration (the engine ships an inert
    CPE "Fire" at 54; /client give fire must resolve to the real one). */
int IndevTest_FindBlockByName(const cc_string* name) {
	int b;
	cc_string bn;
	if (!IndevTest_Enabled) return -1;
	for (b = 98; b >= 50; b--) { /* our id range (genuine 50-62 + variants), newest first */
		bn = Block_UNSAFE_GetName((BlockID)b);
		if (String_CaselessEquals(&bn, name)) return IndevTest_CanonicalBlock((BlockID)b);
	}
	return -1;
}

/* Finds an item by display name (see the matcher above for the accepted */
/*  spellings). Returns the full 256+ id, or -1 when nothing matches or */
/*  Indev mode is off (items don't exist in plain c0.30). */
int IndevTest_FindItemByName(const cc_string* name) {
	int i;
	if (!IndevTest_Enabled) return -1;
	for (i = 0; i < (int)Array_Elems(indevItems); i++) {
		if (Indev_ItemNameMatches(indevItems[i].name, name)) return 256 + indevItems[i].id;
	}
	return -1;
}

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

/* Per-use tool wear: ItemSword wears 1 per entity hit / 2 per block
    destroyed, ItemTool (shovel/pick/axe) the reverse, and everything
    else - hoes and flint&steel included - wears from NEITHER (they only
    ever wear through their own onItemUse). */
int IndevTest_ToolUseWear(int id, cc_bool entityHit) {
	const struct IndevItemDef* d = IndevItems_Find(id);
	if (!d) return 0;
	switch (d->kind) {
	case ITEM_KIND_SWORD:
		return entityHit ? 1 : 2;
	case ITEM_KIND_SHOVEL: case ITEM_KIND_PICKAXE: case ITEM_KIND_AXE:
		return entityHit ? 2 : 1;
	}
	return 0;
}

int IndevTest_ToolMaxDamage(int id) {
	const struct IndevItemDef* d = IndevItems_Find(id);
	if (!d) return 0;
	switch (d->kind) {
	case ITEM_KIND_SWORD: case ITEM_KIND_SHOVEL: case ITEM_KIND_PICKAXE:
	case ITEM_KIND_AXE:   case ITEM_KIND_HOE:
		return 32 << d->param;
	case ITEM_KIND_FLINTSTEEL:
		return 64; /* ItemFlintAndSteel's explicit maxDamage */
	}
	return 0;
}

/* ItemArmor(id, tier, renderIndex, type): reduce {3,8,6,3} by piece and
    maxDamage {11,16,15,13}[piece] * 3 << tier. Set tiers are cloth 0,
    chain 1, iron 2, diamond 3, GOLD 1 (gold really has chain durability). */
int IndevTest_ArmorPiece(int id) {
	const struct IndevItemDef* d = IndevItems_Find(id);
	return (d && d->kind == ITEM_KIND_ARMOR) ? d->param : -1;
}

int IndevTest_ArmorMaxDamage(int id) {
	static const cc_uint8 base[]  = { 11, 16, 15, 13 };
	static const cc_uint8 tiers[] = { 0, 1, 2, 3, 1 };
	int piece = IndevTest_ArmorPiece(id);
	if (piece < 0) return 0;
	return base[piece] * 3 << tiers[(id - 256 - 42) / 4];
}

int IndevTest_ArmorReduce(int id) {
	static const cc_uint8 reduce[] = { 3, 8, 6, 3 };
	int piece = IndevTest_ArmorPiece(id);
	return piece < 0 ? 0 : reduce[piece];
}

cc_bool IndevTest_IsBow(int id) {
	const struct IndevItemDef* d = IndevItems_Find(id);
	return IndevTest_Enabled && d && d->kind == ITEM_KIND_BOW;
}


/* ItemTool.getStrVsBlock: efficiencyOnProperMaterial = (tier+1)*2 (so 2/4/6/8;
    gold tools are tier 0 = WOOD speed) when the block is in the tool class's
    blocksEffectiveAgainst array, else 1. ItemSword.getStrVsBlock is a flat
    1.5 against every block; hoes extend Item (not ItemTool) so they dig at 1.
    The per-class block lists below are the genuine ItemPickaxe/ItemAxe/
    ItemSpade arrays by id - notably brick/obsidian/furnace/workbench are in
    NO list (genuine quirk), and Block.crate 54 IS the chest. */
float IndevTest_StrVsBlock(int id, BlockID block) {
	static const cc_uint8 pickaxeBlocks[] = {
		BLOCK_COBBLE, BLOCK_DOUBLE_SLAB, BLOCK_SLAB, BLOCK_STONE,
		BLOCK_MOSSY_ROCKS, BLOCK_IRON_ORE, BLOCK_IRON, BLOCK_COAL_ORE,
		BLOCK_GOLD, BLOCK_GOLD_ORE, 56 /* INDEV_BLOCK_DIAMOND_ORE */,
		57 /* INDEV_BLOCK_DIAMOND (genuine blocksEffectiveAgainst) */, 0
	};
	static const cc_uint8 axeBlocks[] = {
		BLOCK_WOOD, BLOCK_BOOKSHELF, BLOCK_LOG, 54 /* INDEV_BLOCK_CHEST */, 0
	};
	static const cc_uint8 spadeBlocks[] = {
		BLOCK_GRASS, BLOCK_DIRT, BLOCK_SAND, BLOCK_GRAVEL, 0
	};
	const struct IndevItemDef* d = IndevItems_Find(id);
	const cc_uint8* list = NULL;
	int i;
	if (!IndevTest_Enabled || !d) return 1.0f;
	if (d->kind == ITEM_KIND_SWORD) return 1.5f;

	/* fold directional chest (71-74) / furnace (75-82) variants to canonical */
	block = IndevTest_CanonicalBlock(block);
	switch (d->kind) {
	case ITEM_KIND_PICKAXE: list = pickaxeBlocks; break;
	case ITEM_KIND_AXE:     list = axeBlocks;     break;
	case ITEM_KIND_SHOVEL:  list = spadeBlocks;   break;
	}
	if (!list) return 1.0f;

	for (i = 0; list[i]; i++) {
		if (list[i] == block) return (float)((d->param + 1) * 2);
	}
	return 1.0f;
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

	/* BlockGears is Material.circuits despite its stone sounds - the
	    material gate (rock/iron only) never applies, so ANYTHING harvests */
	if (block == 55 /* INDEV_BLOCK_GEARS (defined below) */) return true;

	d = IndevItems_Find(heldId);
	if (!d || d->kind != ITEM_KIND_PICKAXE) return false;
	level = d->param;
	switch (block) {
	case BLOCK_OBSIDIAN:                return level == 3;
	case 56 /* INDEV_BLOCK_DIAMOND_ORE (defined below) */: return level >= 2; /* genuine oreDiamond */
	case 57 /* INDEV_BLOCK_DIAMOND */:  return level >= 2; /* genuine blockDiamond */
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
	{ 58,            1, 2,2, { BLOCK_WOOD, BLOCK_WOOD, BLOCK_WOOD, BLOCK_WOOD } },        /* workbench */
	{ 50,            4, 1,2, { R_ITEM(7), R_ITEM(24) } },                                   /* torches */
	{ 54,            1, 3,3, { BLOCK_WOOD,BLOCK_WOOD,BLOCK_WOOD, BLOCK_WOOD,0,BLOCK_WOOD, BLOCK_WOOD,BLOCK_WOOD,BLOCK_WOOD } }, /* chest */
	{ 61,            1, 3,3, { BLOCK_COBBLE,BLOCK_COBBLE,BLOCK_COBBLE, BLOCK_COBBLE,0,BLOCK_COBBLE, BLOCK_COBBLE,BLOCK_COBBLE,BLOCK_COBBLE } }, /* furnace */
	/* bowls x4; mushroom soup (both mushroom orders); flint&steel */
	{ R_ITEM(25),    4, 3,2, { BLOCK_WOOD,0,BLOCK_WOOD, 0,BLOCK_WOOD,0 } },
	{ R_ITEM(26),    1, 1,3, { BLOCK_RED_SHROOM, BLOCK_BROWN_SHROOM, R_ITEM(25) } },
	{ R_ITEM(26),    1, 1,3, { BLOCK_BROWN_SHROOM, BLOCK_RED_SHROOM, R_ITEM(25) } },
	{ R_ITEM(3),     1, 2,2, { R_ITEM(9),0, 0,R_ITEM(62) } },
	/* RecipesWeapons extras: bow (" #X"/"# X"/" #X", # stick, X string) and */
	/*  arrows x4 ("X"/"#"/"Y" = iron ingot / stick / feather) */
	{ R_ITEM(5),     1, 3,3, { 0,R_ITEM(24),R_ITEM(31), R_ITEM(24),0,R_ITEM(31), 0,R_ITEM(24),R_ITEM(31) } },
	{ R_ITEM(6),     4, 1,3, { R_ITEM(9), R_ITEM(24), R_ITEM(32) } },
	/* RecipesIngots: 9 ingots/gems <-> storage block, both directions -
	    gold, steel AND diamond (the diamond block now lives at its genuine
	    id 57, so the genuine third pair applies too) */
	{ BLOCK_GOLD,    1, 3,3, { R_ITEM(10),R_ITEM(10),R_ITEM(10), R_ITEM(10),R_ITEM(10),R_ITEM(10), R_ITEM(10),R_ITEM(10),R_ITEM(10) } },
	{ BLOCK_IRON,    1, 3,3, { R_ITEM(9),R_ITEM(9),R_ITEM(9), R_ITEM(9),R_ITEM(9),R_ITEM(9), R_ITEM(9),R_ITEM(9),R_ITEM(9) } },
	{ 57,            1, 3,3, { R_ITEM(8),R_ITEM(8),R_ITEM(8), R_ITEM(8),R_ITEM(8),R_ITEM(8), R_ITEM(8),R_ITEM(8),R_ITEM(8) } },
	{ R_ITEM(10),    9, 1,1, { BLOCK_GOLD } },
	{ R_ITEM(9),     9, 1,1, { BLOCK_IRON } },
	{ R_ITEM(8),     9, 1,1, { 57 } },
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
								int gw, int gh, int ox, int oy, cc_bool mirror) {
	int x, y;
	for (y = 0; y < gh; y++) {
		for (x = 0; x < gw; x++) {
			int rx = x - ox, ry = y - oy;
			cc_uint16 want = 0;
			if (rx >= 0 && rx < r->w && ry >= 0 && ry < r->h) {
				if (mirror) rx = r->w - 1 - rx; /* horizontally flipped layout */
				want = r->cells[ry * r->w + rx];
			}
			if (grid[y * gw + x] != want) return false;
		}
	}
	return true;
}

static cc_bool Recipe_Matches(const struct IndevRecipe* r, const cc_uint16* grid, int gw, int gh) {
	/* CraftingRecipe.matchRecipe tries every offset both unmirrored AND
	    horizontally mirrored - axes/hoes/bows craft in either layout */
	int ox, oy;
	if (r->w > gw || r->h > gh) return false;
	for (oy = 0; oy + r->h <= gh; oy++) {
		for (ox = 0; ox + r->w <= gw; ox++) {
			if (Recipe_MatchesAt(r, grid, gw, gh, ox, oy, false)) return true;
			if (Recipe_MatchesAt(r, grid, gw, gh, ox, oy, true))  return true;
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

	/* RecipesArmor (X = material): helmet "XXX/X X", chest "X X/XXX/XXX",
	    legs "XXX/X X/X X", boots "X X/X X", from gray cloth / iron ingot /
	    diamond / gold ingot. (Genuine chain armor is crafted from FIRE
	    blocks - deferred until BlockFire exists in the Indev layer.) */
	{
		/* RecipesArmor's genuine material row is {clothGray, FIRE, ingotIron,
		    diamond, ingotGold} - chain armor is literally crafted from fire
		    blocks (unobtainable legitimately, a genuine quirk). */
		static const cc_uint16 armorMaterial[5] = { BLOCK_GRAY, INDEV_BLOCK_FIRE, R_ITEM(9), R_ITEM(8), R_ITEM(10) };
		static const cc_uint8  armorSet[5]  = { 42, 46, 50, 54, 58 }; /* cloth chain iron diamond gold */
		static const cc_uint8  armorPatW[4] = { 3, 3, 3, 3 };
		static const cc_uint8  armorPatH[4] = { 2, 3, 3, 2 };
		for (m = 0; m < 5; m++) {
			cc_uint16 X = armorMaterial[m];
			const cc_uint16 pats[4][9] = {
				{ X,X,X, X,0,X },        /* helmet 3x2 */
				{ X,0,X, X,X,X, X,X,X }, /* chestplate 3x3 */
				{ X,X,X, X,0,X, X,0,X }, /* leggings 3x3 */
				{ X,0,X, X,0,X },        /* boots 3x2 */
			};
			for (t = 0; t < 4; t++) {
				Mem_Copy(r.cells, pats[t], sizeof(r.cells));
				r.w = armorPatW[t]; r.h = armorPatH[t];
				r.result = (cc_uint16)R_ITEM(armorSet[m] + t); r.count = 1;
				if (Recipe_Matches(&r, grid, gw, gh)) {
					*outId = r.result; *outCount = 1;
					return true;
				}
			}
		}
	}
	return false;
}

static cc_bool IndevItem_StacksToOne(cc_uint8 kind) {
	/* ItemFood's constructor sets maxStackSize = 1 - apple, bread and both
	    porkchops are single-stack like the tools (soup inherits it too) */
	return kind == ITEM_KIND_SWORD || kind == ITEM_KIND_SHOVEL || kind == ITEM_KIND_PICKAXE ||
	       kind == ITEM_KIND_AXE   || kind == ITEM_KIND_HOE    || kind == ITEM_KIND_FLINTSTEEL ||
	       kind == ITEM_KIND_BOW   || kind == ITEM_KIND_SOUP   || kind == ITEM_KIND_ARMOR ||
	       kind == ITEM_KIND_FOOD;
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
/* Blocks the classic set lacks, defined at the GENUINE Indev ids (50-62, */
/*  shadowing ClassiCube's CPE decoration in Indev mode only - Game_Reset */
/*  restores the CPE definitions whenever a non-Indev map loads) with the */
/*  reserved atlas tiles (96+, patched in from the b1.7.3 jar's terrain.png */
/*  by Resources.c's BetaPatcher - see the notes' reservation table). */
#define INDEV_BLOCK_WORKBENCH   58 /* genuine id */
cc_bool IndevTest_IsWorkbench(BlockID b) { return IndevTest_Enabled && b == INDEV_BLOCK_WORKBENCH; }
#define INDEV_BLOCK_CHEST       54 /* genuine id */
#define INDEV_BLOCK_FURNACE     61 /* genuine id */
#define INDEV_BLOCK_FURNACE_LIT 62 /* genuine id */
#define INDEV_BLOCK_TORCH       50 /* genuine id */
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
#define INDEV_BLOCK_WATER_SOURCE 52 /* genuine waterSource (BlockSource) */
#define INDEV_BLOCK_LAVA_SOURCE  53 /* genuine lavaSource  (BlockSource) */
#define INDEV_BLOCK_GEARS        55 /* genuine cog/gears; tex 62 (Indev-only) */
#define INDEV_BLOCK_DIAMOND_ORE  56 /* genuine id; tile 119 */
#define INDEV_BLOCK_DIAMOND      57 /* genuine blockDiamond; tex 40 (Indev teal) */
/* Wall torches: genuine torch (50) metadata 1-4 = hanging on the solid
    block at -X / +X / -Z / +Z respectively (BlockTorch.onBlockAdded order).
    Id = base + (meta - 1). The canonical INDEV_BLOCK_TORCH is metadata 5
    (standing on the floor) and stays the inventory/drop form. */
#define INDEV_BLOCK_TORCH_W1     94 /* 94-97 */
static cc_bool Indev_IsWallTorch(BlockID b) {
	return b >= INDEV_BLOCK_TORCH_W1 && b <= INDEV_BLOCK_TORCH_W1 + 3;
}
cc_bool IndevTest_IsWallTorch(BlockID b) { return IndevTest_Enabled && Indev_IsWallTorch(b); }
/* Torch metadata of a wall-torch id (1-4), for the tilt tables. */
int IndevTest_WallTorchMeta(BlockID b) { return b - INDEV_BLOCK_TORCH_W1 + 1; }
cc_bool IndevTest_IsGears(BlockID b) { return IndevTest_Enabled && b == INDEV_BLOCK_GEARS; }

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

/* Inventory/naming form of a block (directional variants -> canonical id, */
/*  lit furnace -> idle). NOTE this is NOT the drop form: in-20100223 */
/*  BlockFurnace has no idDropped override, so Block.idDropped returns the */
/*  block's own id and a LIT furnace genuinely drops the lit block (62) - */
/*  see IndevTest_DropFormBlock. This fold is for recipes/naming/placement */
/*  bookkeeping only. */
BlockID IndevTest_CanonicalBlock(BlockID b) {
	if (Indev_IsWallTorch(b)) return INDEV_BLOCK_TORCH;
	if (b >= INDEV_BLOCK_CHEST_V0 && b <= INDEV_BLOCK_CHEST_V0 + 3) return INDEV_BLOCK_CHEST;
	if (b >= INDEV_BLOCK_FURN_V0  && b <= INDEV_BLOCK_FURN_V0  + 3) return INDEV_BLOCK_FURNACE;
	if (b >= INDEV_BLOCK_FURNL_V0 && b <= INDEV_BLOCK_FURNL_V0 + 3) return INDEV_BLOCK_FURNACE;
	if (b == INDEV_BLOCK_FURNACE_LIT) return INDEV_BLOCK_FURNACE;
	return b;
}

/* Block.idDropped form: directional variants fold to canonical, but a lit
    furnace keeps its lit id (no idDropped override in in-20100223). */
BlockID IndevTest_DropFormBlock(BlockID b) {
	if (Indev_IsFurnaceLit(b)) return INDEV_BLOCK_FURNACE_LIT;
	return IndevTest_CanonicalBlock(b);
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
	if (Indev_IsWallTorch(b))             return IndevTest_WallTorchMeta(b);
	return 0;
}

/* Position-aware Data-nibble forms: fire keeps its age 0-15 in a per-map
    side store rather than deriving it from the block id. */
static int  Indev_SaplingStage(int index);
static void Indev_SetSaplingStage(int index, int stage);
int IndevTest_BlockDataMetaAt(int index, BlockID b) {
	if (IndevFire_IsFire(b))  return IndevFire_Age(index);
	if (b == BLOCK_SAPLING)   return Indev_SaplingStage(index);
	return IndevTest_BlockDataMeta(b);
}
BlockID IndevTest_ApplyDataMetaAt(int index, BlockID b, int meta) {
	if (IndevFire_IsFire(b))  { IndevFire_SetAge(index, meta);      return b; }
	if (b == BLOCK_SAPLING)   { Indev_SetSaplingStage(index, meta); return b; }
	return IndevTest_ApplyDataMeta(b, meta);
}

/* Applies a loaded .mclevel metadata nibble to a base-mapped block. */
BlockID IndevTest_ApplyDataMeta(BlockID b, int meta) {
	if (IndevTest_IsContainerBlock(b)) return IndevTest_FacingVariant(b, meta);
	if (b == INDEV_BLOCK_FARMLAND && meta > 0) return INDEV_BLOCK_FARMLAND_WET;
	if (b == INDEV_BLOCK_CROPS_0 && meta > 0)  return (BlockID)(INDEV_BLOCK_CROPS_0 + (meta > 7 ? 7 : meta));
	if (b == INDEV_BLOCK_TORCH && meta >= 1 && meta <= 4)
		return (BlockID)(INDEV_BLOCK_TORCH_W1 + meta - 1);
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

	/* Genuine Indev has ONE light channel: every emitter lives in the LAMP
	    nibble at its genuine Block.lightValue, freeing the lava nibble to be
	    the flooded SKY channel (FancyLighting.c). Move the engine's default
	    lava-channel emitters over (lava lightValue = 15 genuine) and clear
	    any other low-nibble stragglers so nothing pollutes the sky data. */
	for (k = 0; k < BLOCK_COUNT; k++) {
		cc_uint8 lava = Blocks.Brightness[k] & FANCY_LIGHTING_MAX_LEVEL;
		cc_uint8 lamp = Blocks.Brightness[k] >> FANCY_LIGHTING_LAMP_SHIFT;
		if (!lava) continue;
		if (lava > lamp) lamp = lava;
		Blocks.Brightness[k] = lamp << FANCY_LIGHTING_LAMP_SHIFT;
	}
	Blocks.Brightness[BLOCK_LAVA]       = 15 << FANCY_LIGHTING_LAMP_SHIFT;
	Blocks.Brightness[BLOCK_STILL_LAVA] = 15 << FANCY_LIGHTING_LAMP_SHIFT;
	/* mushroomBrown.setLightValue(2/16) - the faint brown mushroom glow */
	Blocks.Brightness[BLOCK_BROWN_SHROOM] = 1 << FANCY_LIGHTING_LAMP_SHIFT;

	/* Block.java:496 - leaves are setLightOpacity(1), and ANY nonzero
	    opacity ends the heightmap top-scan, so tree canopies shadow the
	    ground below them. The flood already passes through leaves at the
	    minimum -1 per cell, which IS opacity 1's attenuation. (c0.30's
	    isLightBlocker() is isOpaque(), false for leaves - no shadow there,
	    hence Indev-only.) Clearing SHADES_FROM_BELOW parks the heightmap
	    AT the top blocker cell: genuine heightMap = blockerY + 1 = first
	    fully-lit cell, so the blocker itself is flood-lit, never a direct
	    sky seed - a leaf gets 15-1=14, a water surface 15-3=12 (water was
	    previously seeded at 15 and only paid its opacity going deeper). */
	Blocks.BlocksLight[BLOCK_LEAVES]       = true;
	Blocks.LightOffset[BLOCK_LEAVES]      &= ~(1 << LIGHT_FLAG_SHADES_FROM_BELOW);
	Blocks.LightOffset[BLOCK_WATER]       &= ~(1 << LIGHT_FLAG_SHADES_FROM_BELOW);
	Blocks.LightOffset[BLOCK_STILL_WATER] &= ~(1 << LIGHT_FLAG_SHADES_FROM_BELOW);

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
	/* Wall torch variants: same tile/light as the standing torch, rendered
	    tilted by the chunk builder (Builder_DrawWallTorch - the genuine
	    renderBlockTorch geometry). Draw = SPRITE routes them through the
	    builder's sprite path; MinBB/MaxBB are the genuine per-metadata
	    collisionRayTrace pick bounds (they don't drive rendering here). */
	{
		/* genuine BlockTorch.collisionRayTrace bounds for metadata 1-4 */
		static const float wtMin[4][3] = {
			{ 0.00f, 0.2f, 0.35f }, { 0.70f, 0.2f, 0.35f },
			{ 0.35f, 0.2f, 0.00f }, { 0.35f, 0.2f, 0.70f }
		};
		static const float wtMax[4][3] = {
			{ 0.30f, 0.8f, 0.65f }, { 1.00f, 0.8f, 0.65f },
			{ 0.65f, 0.8f, 0.30f }, { 0.65f, 0.8f, 1.00f }
		};
		int k;
		for (k = 0; k < 4; k++) {
			BlockID id = (BlockID)(INDEV_BLOCK_TORCH_W1 + k);
			IndevBlock_Define(id, "Torch", 106, 106, 106, 106, SOUND_WOOD, 0);
			Blocks.Collide[id]         = COLLIDE_NONE;
			Blocks.ExtendedCollide[id] = COLLIDE_NONE;
			Blocks.Draw[id]            = DRAW_SPRITE;
			Blocks.BlocksLight[id]     = false;
			Blocks.Brightness[id]      = 14 << FANCY_LIGHTING_LAMP_SHIFT;
			Vec3_Set(Blocks.MinBB[id], wtMin[k][0], wtMin[k][1], wtMin[k][2]);
			Vec3_Set(Blocks.MaxBB[id], wtMax[k][0], wtMax[k][1], wtMax[k][2]);
			Block_DefineCustom(id, false);
		}
	}

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
	/* (farmland's 15/16 MaxBB is set by the loop just below, next to CanPlace) */
	/* Diamond ore (genuine 56) - tile 119 is patched from the b1.7.3
	    terrain's diamond ore. Hardness 3.0F like the other ores; the item/
	    tool side of diamonds lands with the armor+tools roadmap stage. */
	IndevBlock_Define(INDEV_BLOCK_DIAMOND_ORE, "Diamond Ore", 119, 119, 119, 119, SOUND_STONE, 60);

	/* Genuine blocks that used to leak through as ClassiCube CPE defaults.
	    Diamond block's texture is fetched from the beta jar (tile 122, teal);
	    gears' tile is Indev-only (no jar hosts it) so it's embedded in Resources.c
	    at tile 123. None are craftable in in-20100223 (creative/technical). */
	/* Diamond block (57): opaque cube, hardness 5.0F (=100), drops itself.
	    soundMetalFootstep -> STONE (Indev has no metal footstep). Tile 122 is
	    the (teal) diamond block fetched from the beta jar's terrain.png (120 is
	    the 2nd Indev fire animation instance, so it can't be used here). */
	IndevBlock_Define(INDEV_BLOCK_DIAMOND, "Diamond Block", 122, 122, 122, 122, SOUND_METAL, 100);
	/* Gears/cog (55): non-solid, walk-through decorative (Material.circuits),
	    hardness 0.5F (=10), drops itself, tile 123 (embedded genuine tex 62).
	    Genuine bounds: NO collision (getCollisionBoundingBoxFromPool -> null) and
	    DEFAULT full-cube pick bounds (BlockGears sets no bounds). DRAW_SPRITE routes
	    it through Builder_DrawGears, which ports the genuine renderType 5 geometry:
	    a gear quad flush on each ADJACENT SOLID WALL (nothing if no wall adjacent). */
	IndevBlock_Define(INDEV_BLOCK_GEARS, "Gears", 123, 123, 123, 123, SOUND_METAL, 10);
	Blocks.Collide[INDEV_BLOCK_GEARS]         = COLLIDE_NONE;
	Blocks.ExtendedCollide[INDEV_BLOCK_GEARS] = COLLIDE_NONE;
	Blocks.Draw[INDEV_BLOCK_GEARS]            = DRAW_SPRITE;
	Blocks.BlocksLight[INDEV_BLOCK_GEARS]     = false;
	Vec3_Set(Blocks.MinBB[INDEV_BLOCK_GEARS], 0.0f, 0.0f, 0.0f);
	Vec3_Set(Blocks.MaxBB[INDEV_BLOCK_GEARS], 1.0f, 1.0f, 1.0f);
	Block_DefineCustom(INDEV_BLOCK_GEARS, false);
	/* Water/lava source (52/53): BlockSource - renders like the still fluid and
	    refills its 4 horizontal air neighbours each random tick (Indev_TickSource,
	    registered in Indev_RegisterFarmTicks). Copy the still fluid's appearance
	    so no new tile is needed; hardness 0. */
	{
		int f;
		BlockID src, fluid;
		for (f = 0; f < 2; f++) {
			int face;
			src   = f == 0 ? INDEV_BLOCK_WATER_SOURCE : INDEV_BLOCK_LAVA_SOURCE;
			fluid = f == 0 ? BLOCK_STILL_WATER         : BLOCK_STILL_LAVA;
			{
				cc_string nm = String_FromReadonly(f == 0 ? "Water Source" : "Lava Source");
				Block_SetName(src, &nm);
			}
			for (face = 0; face < FACE_COUNT; face++)
				Block_Tex(src, face) = Block_Tex(fluid, face);
			Blocks.Draw[src]            = Blocks.Draw[fluid];
			Blocks.Collide[src]         = Blocks.Collide[fluid];
			Blocks.ExtendedCollide[src] = Blocks.ExtendedCollide[fluid];
			Blocks.BlocksLight[src]     = Blocks.BlocksLight[fluid];
			Blocks.Brightness[src]      = Blocks.Brightness[fluid];
			Blocks.FogDensity[src]      = Blocks.FogDensity[fluid];
			Blocks.FogCol[src]          = Blocks.FogCol[fluid];
			Blocks.MinBB[src]           = Blocks.MinBB[fluid];
			Blocks.MaxBB[src]           = Blocks.MaxBB[fluid];
			Blocks.DigSounds[src]       = SOUND_NONE;
			Blocks.StepSounds[src]      = SOUND_NONE;
			Blocks.CanPlace[src]        = true;
			Blocks.CanDelete[src]       = true;
			Block_DefineCustom(src, false);
			SurvivalTest_SetHardness(src, 0);
		}
	}
	IndevFire_DefineBlock();

	/* BlockFarmland.setBlockBounds(0, 0, 0, 1, 15/16, 1): genuine farmland
	    sits 1/16 LOWER than a full block. The crop planes sink that same
	    1/16 to rest flush on it - with a full-cube farmland, the bottom
	    texture row of every crop plane (where stage 0's tiny sprout dots
	    live) was buried inside the block, which is why freshly planted
	    seeds looked half-missing compared to genuine (user-diagnosed!). */
	for (k = 0; k < 2; k++) {
		BlockID id = (BlockID)(INDEV_BLOCK_FARMLAND + k);
		Blocks.MaxBB[id].y  = 15.0f / 16.0f;
		Blocks.CanPlace[id] = false;
		Block_DefineCustom(id, false);
	}

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

	/* Indev classifies footsteps/breaks by Block.stepSound - footsteps use
	    stepSoundDir2() = "step." + name (also placing + mining-progress),
	    the break uses the overridable stepSoundDir(). in-20100223 differs
	    from ClassiCube's modern block-sound table on four classic blocks;
	    fixed here so only Indev mode is affected (c0.30 keeps the engine
	    defaults - this function never runs there):
	    - sand: soundSandFootstep - footstep "step.sand" (SAND), but the
	      break override is "step.gravel" (GRAVEL), not sand.
	    - glass: soundGlassFootstep = StepSoundGlass("stone") - footstep is
	      "step.stone" (STONE), not metal; the break stays "random.glass".
	    - gold/iron blocks: soundMetalFootstep = StepSound("stone", 1, 1.5) -
	      the stone SAMPLES at a raised pitch. That is exactly what the
	      engine's SOUND_METAL does (stone samples, dig rate 120 = the genuine
	      break pitch 1.5*0.8 = 1.2), so metal blocks use SOUND_METAL. */
	Blocks.DigSounds[BLOCK_SAND]   = SOUND_GRAVEL; /* footstep stays SAND */
	Blocks.StepSounds[BLOCK_GLASS] = SOUND_STONE;  /* footstep step.stone */
	Blocks.DigSounds[BLOCK_GLASS]  = SOUND_GLASS;  /* break random.glass (set
	    explicitly - the engine block default resolves to metal at runtime) */
	Blocks.StepSounds[BLOCK_GOLD] = SOUND_METAL; Blocks.DigSounds[BLOCK_GOLD] = SOUND_METAL;
	Blocks.StepSounds[BLOCK_IRON] = SOUND_METAL; Blocks.DigSounds[BLOCK_IRON] = SOUND_METAL;

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

	/* Faithfulness: genuine Indev's block registry ends at 62 (furnace lit).
	    52/53/55/57 now hold their genuine blocks (water/lava source, gears,
	    diamond block - defined above). The remaining leftovers are turquoise
	    wool(59) and ice(60) - whose genuine crops/farmland we host at 85+/83 -
	    and pillar(63)/crate(64)/stone brick(65), which don't exist in Indev at
	    all. Hide those from the Indev block set (not placeable, off the inventory
	    map). Indev-only: never runs in c0.30-s or plain creative. */
	{
		static const cc_uint8 nonGenuine[] = { 59, 60, 63, 64, 65 };
		int n;
		for (n = 0; n < (int)Array_Elems(nonGenuine); n++) {
			Blocks.CanPlace[nonGenuine[n]] = false;
			Inventory_Remove(nonGenuine[n]);
		}
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
static int indev_openTE  = -1;          /* pool index of the open container (the */
                                        /*  UPPER half of a large chest), -1 = none */
static int indev_openTE2 = -1;          /* the LOWER half's pool index for an open */
                                        /*  large chest (InventoryLargeChest), else -1 */
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

static cc_bool Indev_NormalCube(int x, int y, int z); /* defined with the torch helpers */
static int Indev_TorchAutoMeta(IVec3 p);              /* likewise */

static cc_bool Indev_ChestAt(int x, int y, int z) {
	if (!World_Contains(x, y, z)) return false;
	return Indev_IsChestBlock(World_GetBlock(x, y, z));
}

/* BlockChest.isThereANeighborChest: the cell holds a chest that already
    touches another chest (i.e. it's half of an existing double). */
static cc_bool Indev_ChestIsPaired(int x, int y, int z) {
	if (!Indev_ChestAt(x, y, z)) return false;
	return Indev_ChestAt(x - 1, y, z) || Indev_ChestAt(x + 1, y, z) ||
	       Indev_ChestAt(x, y, z - 1) || Indev_ChestAt(x, y, z + 1);
}

/* BlockChest.canPlaceBlockAt: a chest may touch at most ONE other chest,
    and never one that is already half of a double - doubles are the limit,
    triples and L-shapes are refused outright. BlockTorch.canPlaceBlockAt:
    one of the four side walls or the floor must be a normal cube - checked
    BEFORE placement by ItemBlock.onItemUse, so a refused click consumes
    nothing and plays nothing. Everything else places. */
cc_bool IndevTest_CanPlaceBlockAt(BlockID b, IVec3 pos) {
	int n = 0;
	if (!IndevTest_Enabled) return true;
	if (b == INDEV_BLOCK_TORCH) return Indev_TorchAutoMeta(pos) != 0;
	if (!Indev_IsChestBlock(b)) return true;

	if (Indev_ChestAt(pos.x - 1, pos.y, pos.z)) n++;
	if (Indev_ChestAt(pos.x + 1, pos.y, pos.z)) n++;
	if (Indev_ChestAt(pos.x, pos.y, pos.z - 1)) n++;
	if (Indev_ChestAt(pos.x, pos.y, pos.z + 1)) n++;
	if (n > 1) return false;

	return !Indev_ChestIsPaired(pos.x - 1, pos.y, pos.z) &&
	       !Indev_ChestIsPaired(pos.x + 1, pos.y, pos.z) &&
	       !Indev_ChestIsPaired(pos.x, pos.y, pos.z - 1) &&
	       !Indev_ChestIsPaired(pos.x, pos.y, pos.z + 1);
}

int IndevTest_OpenContainer(IVec3 pos) {
	BlockID b;
	int kind, i;
	if (!IndevTest_Enabled) return INDEV_CONTAINER_NONE;

	b    = World_GetBlock(pos.x, pos.y, pos.z);
	kind = IndevTest_ContainerKindOf(b);
	if (!kind) return INDEV_CONTAINER_NONE;

	/* BlockChest.blockActivated: a normal cube directly above the chest -
	    or above the OTHER half of a double chest - keeps the lid shut.
	    Furnaces have no such rule. */
	if (kind == INDEV_CONTAINER_CHEST) {
		if (Indev_NormalCube(pos.x, pos.y + 1, pos.z)) return INDEV_CONTAINER_NONE;
		if (Indev_ChestAt(pos.x - 1, pos.y, pos.z) &&
			Indev_NormalCube(pos.x - 1, pos.y + 1, pos.z)) return INDEV_CONTAINER_NONE;
		if (Indev_ChestAt(pos.x + 1, pos.y, pos.z) &&
			Indev_NormalCube(pos.x + 1, pos.y + 1, pos.z)) return INDEV_CONTAINER_NONE;
		if (Indev_ChestAt(pos.x, pos.y, pos.z - 1) &&
			Indev_NormalCube(pos.x, pos.y + 1, pos.z - 1)) return INDEV_CONTAINER_NONE;
		if (Indev_ChestAt(pos.x, pos.y, pos.z + 1) &&
			Indev_NormalCube(pos.x, pos.y + 1, pos.z + 1)) return INDEV_CONTAINER_NONE;
	}

	i = IndevTE_Find(pos);
	if (i < 0) i = IndevTE_Create(kind, pos);
	if (i < 0) return INDEV_CONTAINER_NONE;

	indev_openTE  = i;
	indev_openTE2 = -1;

	/* BlockChest.blockActivated: a chest touching another chest opens as an
	    InventoryLargeChest spanning both tile entities. canPlaceBlockAt caps
	    doubles at one neighbour, so at most one of these fires. The upper/lower
	    ordering is genuine: the -X / -Z neighbour becomes the UPPER half (its
	    27 slots render on top), the clicked chest the lower; a +X / +Z
	    neighbour is the LOWER half. */
	if (kind == INDEV_CONTAINER_CHEST) {
		IVec3 np = pos; int j = -1; cc_bool neighbourUpper = false;
		if      (Indev_ChestAt(pos.x - 1, pos.y, pos.z)) { np.x = pos.x - 1; neighbourUpper = true;  }
		else if (Indev_ChestAt(pos.x + 1, pos.y, pos.z)) { np.x = pos.x + 1; neighbourUpper = false; }
		else if (Indev_ChestAt(pos.x, pos.y, pos.z - 1)) { np.z = pos.z - 1; neighbourUpper = true;  }
		else if (Indev_ChestAt(pos.x, pos.y, pos.z + 1)) { np.z = pos.z + 1; neighbourUpper = false; }

		if (np.x != pos.x || np.z != pos.z) {
			j = IndevTE_Find(np);
			if (j < 0) j = IndevTE_Create(INDEV_CONTAINER_CHEST, np);
			/* IndevTE_Create can fail if the pool is full - fall back to a
			    single-chest view rather than opening nothing. */
			if (j >= 0) {
				if (neighbourUpper) { indev_openTE = j; indev_openTE2 = i; }
				else                { indev_openTE = i; indev_openTE2 = j; }
			}
		}
	}
	return kind;
}

/* ---- MP net container view (rest of phase 4) ----
    On server maps the open container is SERVER state: SURV_CONT_OPEN sets the
    kind + slot count here, SURV_CONT_SLOT writes the slots, and the accessors
    below serve this view instead of the local tile-entity pool - so the
    existing chest/large-chest/furnace screens render it unchanged. Clicks
    leave as intents through the normal ServerOwnsInventory routing. */
static int indev_netContKind;   /* INDEV_CONTAINER_NONE = not net-open */
static int indev_netContSlots;
static struct SurvivalSlot indev_netCont[SURVIVAL_CONTAINER_MAX];
static int indev_netContLarge;  /* chest with 54 slots (renders the double GUI) */
static int indev_netBurn, indev_netCook; /* FURN_PROG: pre-scaled 0..12 / 0..24 */

void IndevTest_NetContOpen(int kind, int slots) {
	if (slots < 0) slots = 0;
	if (slots > SURVIVAL_CONTAINER_MAX) slots = SURVIVAL_CONTAINER_MAX;
	indev_netContKind  = kind;
	indev_netContSlots = slots;
	indev_netContLarge = kind == INDEV_CONTAINER_CHEST && slots > SURVIVAL_CONTAINER_SLOTS;
	indev_netBurn = 0; indev_netCook = 0;
	Mem_Set(indev_netCont, 0, sizeof(indev_netCont));
	SurvivalTest_MarkInvDirty();
}

void IndevTest_NetContSlot(int i, int id, int count, int dmg) {
	if (!indev_netContKind || i < 0 || i >= indev_netContSlots) return;
	indev_netCont[i].id     = (cc_uint16)id;
	indev_netCont[i].count  = (cc_int16)count;
	indev_netCont[i].damage = (cc_int16)dmg;
	if (indev_netCont[i].count <= 0) {
		indev_netCont[i].id = BLOCK_AIR; indev_netCont[i].count = 0; indev_netCont[i].damage = 0;
	}
	SurvivalTest_MarkInvDirty();
}

void IndevTest_NetFurnProg(int burn, int cook) {
	indev_netBurn = burn;
	indev_netCook = cook;
	SurvivalTest_MarkInvDirty(); /* repaint the flame/arrow overlays */
}

int IndevTest_OpenKind(void) {
	if (!IndevTest_Enabled) return INDEV_CONTAINER_NONE;
	if (indev_netContKind)  return indev_netContKind;
	if (indev_openTE < 0)   return INDEV_CONTAINER_NONE;
	return indev_tes[indev_openTE].kind;
}

void IndevTest_CloseContainer(void) {
	indev_openTE = -1; indev_openTE2 = -1;
	indev_netContKind = 0; indev_netContSlots = 0; indev_netContLarge = 0;
	indev_netBurn = 0; indev_netCook = 0;
}

/* Slot count of the open container: furnace 3, single chest 27, large (double) */
/*  chest 54 (InventoryLargeChest.getSizeInventory = upper 27 + lower 27). */
int IndevTest_ContainerSlotCount(void) {
	if (indev_netContKind) return indev_netContSlots;
	if (indev_openTE < 0) return 0;
	if (indev_tes[indev_openTE].kind == INDEV_CONTAINER_FURNACE) return 3;
	return indev_openTE2 >= 0 ? SURVIVAL_CONTAINER_SLOTS * 2 : SURVIVAL_CONTAINER_SLOTS;
}

struct SurvivalSlot* IndevTest_ContainerSlot(int i) {
	/* MP: the server-streamed view (clicks on it leave as intents and never
	    mutate it locally - the CONT_SLOT echo writes the result back) */
	if (indev_netContKind) {
		if (i >= 0 && i < indev_netContSlots) return &indev_netCont[i];
		indev_discardSlot.id = 0; indev_discardSlot.count = 0; indev_discardSlot.damage = 0;
		return &indev_discardSlot;
	}
	/* InventoryLargeChest routing: slots below the upper chest's size come from
	    the upper tile entity, the rest from the lower one. A single chest /
	    furnace has no lower half (indev_openTE2 < 0), so it only uses openTE. */
	if (indev_openTE >= 0 && i >= 0) {
		if (i < SURVIVAL_CONTAINER_SLOTS)
			return &indev_tes[indev_openTE].slots[i];
		if (indev_openTE2 >= 0 && i < SURVIVAL_CONTAINER_SLOTS * 2)
			return &indev_tes[indev_openTE2].slots[i - SURVIVAL_CONTAINER_SLOTS];
	}
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
    Diamond ore smelts to a diamond like genuine FurnaceRecipes. */
static int Furnace_SmeltResult(int id) {
	if (id == BLOCK_IRON_ORE) return 256 + 9;  /* Iron Ingot */
	if (id == BLOCK_GOLD_ORE) return 256 + 10; /* Gold Ingot */
	if (id == INDEV_BLOCK_DIAMOND_ORE) return 256 + 8; /* Diamond */
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
	if (id > 0 && id < 256 && id != INDEV_BLOCK_TORCH && id != INDEV_BLOCK_FIRE &&
		Blocks.DigSounds[id] == SOUND_WOOD) return 300; /* Material.wood only -
		torch (circuits) and fire (Material.fire) never burn as fuel */
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
	if (indev_netContKind) return indev_netContKind == INDEV_CONTAINER_FURNACE ? indev_netBurn : 0;
	if (indev_openTE < 0) return 0;
	te = &indev_tes[indev_openTE];
	if (te->kind != INDEV_CONTAINER_FURNACE || te->currentBurn <= 0) return 0;
	return te->burnTime * 12 / te->currentBurn; /* getBurnTimeRemainingScaled */
}

/* TileEntityFurnace.isBurning - drives the GUI flame (drawn even when the
    12-step scaled height has hit 0, leaving the genuine 2px ember stub). */
int IndevTest_FurnaceIsBurning(void) {
	struct IndevTE* te;
	if (indev_netContKind) return indev_netContKind == INDEV_CONTAINER_FURNACE && indev_netBurn > 0;
	if (indev_openTE < 0) return 0;
	te = &indev_tes[indev_openTE];
	return te->kind == INDEV_CONTAINER_FURNACE && te->burnTime > 0;
}

int IndevTest_FurnaceCookScaled(void) {
	struct IndevTE* te;
	if (indev_netContKind) return indev_netContKind == INDEV_CONTAINER_FURNACE ? indev_netCook : 0;
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
/* the lighting mode the player had before an Indev map force-switched to
    FANCY, so leaving the map restores their choice (see OnNewMap) */
static cc_uint8 indev_prevLighting;
static cc_bool  indev_forcedLighting;
static cc_bool   indev_baseColsKnown;
static int       indev_lastSkyLight = -1;

int  IndevTest_WorldTime(void)      { return indev_worldTime; }
void IndevTest_SetWorldTime(int t)  { indev_worldTime = t >= 0 ? t % 24000 : 0; }
void IndevTest_SetSkyBrightness(int b) { indev_skyBright = b; }

/* Applies theme colours as BOTH the live Env colours and the day/night
    scaling baseline - the generator calls this after World_SetNewMap, i.e.
    after OnNewMapLoaded already snapshotted the (default) colours. */
void IndevTest_SetBaseEnvColors(PackedCol sky, PackedCol fog, PackedCol clouds) {
	indev_baseSky      = sky;
	indev_baseFog      = fog;
	indev_baseClouds   = clouds;
	indev_baseColsKnown = true;
	Env_SetSkyCol(sky);
	Env_SetFogCol(fog);
	Env_SetCloudsCol(clouds);
}
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

/* World.lightBrightnessTable: render brightness of a light level 0-15,
    (1-v)/(3v+1)*0.95+0.05 with v = 1 - light/15 - so night (light 4) is
    ~0.129, NOT the linear 0.267. Feeds block shade, entity shade, spawn
    darkness tests and the env sun/shadow scale. */
float IndevTest_BrightnessOfLight(int light) {
	float v = 1.0f - (float)light / 15.0f;
	return (1.0f - v) / (v * 3.0f + 1.0f) * 0.95f + 0.05f;
}

/* World.skylightSubtracted (misnomer - it IS the live sky light): World.tick
    moves it at most ONE level per tick toward getSkyBrightness, giving the
    11-step dawn/dusk fade. -1 = not yet initialised for this map. */
static int Indev_EasedSkyLight(void) {
	return indev_lastSkyLight >= 0 ? indev_lastSkyLight : Indev_SkyLight();
}

static void Indev_TickDayNight(void) {
	float f;
	int   light;

	/* MP: the SERVER owns the clock (SURV_TIME sets indev_worldTime); only
	    advance it locally when the sim is ours. The visual application below
	    always runs, so server-pushed time still drives sun/sky/colours. */
	if (!SurvivalNet_ServerDriven()) {
		indev_worldTime++;
		if (indev_worldTime >= 24000) indev_worldTime = 0;
	}
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

	/* skylightSubtracted: World.tick eases the live sky level at most ONE
	    step per tick toward getSkyBrightness (11 relights across dawn/dusk,
	    gradual catch-up after /time jumps), and the sun/shadow scale runs
	    through the genuine lightBrightnessTable curve (night ~0.129, not
	    linear 0.267). Each Env change triggers a full relight, so only
	    apply when the eased level actually moves. */
	light = Indev_SkyLight();
	if (indev_lastSkyLight < 0)          indev_lastSkyLight = light;
	else if (indev_lastSkyLight > light) indev_lastSkyLight--;
	else if (indev_lastSkyLight < light) indev_lastSkyLight++;
	else return;

	/* The fancy-lighting Indev palettes dim by the eased level themselves
	    (effective sky = flood - (15 - k), the genuine table at the end) -
	    set k FIRST, then the sun-colour change below rebuilds the palettes
	    and refreshes the chunk colours in one pass. The scaled sun/shadow
	    colours still matter for the OOB horizon and non-fancy fallbacks. */
	FancyLighting_SetIndevSky(indev_lastSkyLight);

	f = IndevTest_BrightnessOfLight(indev_lastSkyLight);
	Env_SetSunCol(PackedCol_Scale(ENV_DEFAULT_SUN_COLOR,       f));
	Env_SetShadowCol(PackedCol_Scale(ENV_DEFAULT_SHADOW_COLOR, f));
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
	/* genuine World.getBlockLightValue CLAMPS out-of-range coords to the
	    nearest in-range block, and that clamp is load-bearing here: entities
	    off the map edge (mobs wandering off a floating island) and mob-AI
	    path sampling near borders query outside the world, and the engine's
	    classic-lighting heightmap does no bounds checking of its own. On
	    huge (512x512) maps the heightmap is its own mmap'd allocation, so
	    an unclamped query is an instant segfault (user-reported crash on
	    huge floating woods worlds). */
	if (!World.Blocks) return 15;
	if (x < 0) { x = 0; } else if (x >= World.Width)  { x = World.Width  - 1; }
	if (y < 0) { y = 0; } else if (y >= World.Height) { y = World.Height - 1; }
	if (z < 0) { z = 0; } else if (z >= World.Length) { z = World.Length - 1; }

	/* genuine getBlockLightValue reads the EASED sky level (the stored light
	    nibbles are nudged 1/tick by updateDaylightCycle), not the target */
	if (Lighting_Mode == LIGHTING_MODE_FANCY) {
		/* Genuine combined value: flooded sky (caves fade to 0) vs lamps */
		return FancyLighting_IndevLight(x, y, z);
	}
	if (Lighting.IsLit(x, y, z)) light = Indev_EasedSkyLight();
	return light;
}

/* The live sky light level - what zombie/skeleton daylight burning tests as
    "worldObj.skylightSubtracted > 7" (the deobf name is a misnomer: World.tick
    eases that field toward getSkyBrightness every tick, so it IS the sky
    light, 15 at noon / 4 at night). */
int IndevTest_CurSkyLight(void) {
	if (!IndevTest_Enabled) return 15;
	return Indev_EasedSkyLight(); /* the zombie-burn test reads the eased field */
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

/* BlockFarmland.onEntityWalking, fed by Entity.move's step trigger (fires
    once per accumulated 1/0.6 horizontal metres, NOT gated on onGround):
    a 1-in-4 roll converts the farmland below the walker's feet (posY - 0.2)
    to dirt with notify - the Game_UpdateBlock hook then pops any crop above.
    Player and all mobs trample (canTriggerWalking is only false for items,
    TNT and particles). */
void IndevTest_TrampleStep(float px, float feetY, float pz) {
	int bx, by, bz;
	BlockID b;
	if (!IndevTest_Enabled) return;

	bx = Math_Floor(px); by = Math_Floor(feetY - 0.2f); bz = Math_Floor(pz);
	if (!World_Contains(bx, by, bz)) return;
	b = World_GetBlock(bx, by, bz);
	if (!Indev_IsFarmland(b)) return;
	if (Random_Next(&indev_teRng, 4) != 0) return;

	Game_UpdateBlock(bx, by, bz, BLOCK_DIRT);
}

/* BlockFlower.checkFlowerChange's failure arm, for crops: dropBlockAsItem
    (BlockCrops.idDropped: stage 7 -> 1 wheat item, stages 0-6 -> nothing;
    seeds only ever drop from onBlockDestroyedByPlayer) then set air. */
static void Indev_PopCrop(int x, int y, int z, BlockID crop) {
	Vec3 dp;
	if (crop == INDEV_BLOCK_CROPS_7) {
		dp.x = x + Random_Float(&indev_teRng) * 0.7f + 0.15f;
		dp.y = y + 0.5f;
		dp.z = z + Random_Float(&indev_teRng) * 0.7f + 0.15f;
		SurvivalTest_SpawnDropWorld(dp, 256 + 40, 1); /* Item.wheat */
	}
	Game_UpdateBlock(x, y, z, BLOCK_AIR);
}

/* BlockCrops.updateTick: super.updateTick (BlockFlower.checkFlowerChange -
    the canBlockStay pop check) runs FIRST for every stage; then growth:
    rate from the farmland below (1 dry / 3 wet, neighbours at quarter
    weight), halved when crowded by adjacent crops; then a 1-in-(100/rate)
    roll advances the stage. */
static void Indev_TickCrops(int index, BlockID block) {
	float rate = 1.0f, f;
	int x, y, z, dx, dz, light;
	BlockID below;
	cc_bool rowX, rowZ, diag, stay;
	World_Unpack(index, x, y, z);

	/* canBlockStay: (light >= 8 OR (light >= 4 AND sky access)) AND farmland
	    below. Lighting.IsLit approximates canBlockSeeTheSky (both heightmap
	    based - genuine also scans past translucent cover; accepted). */
	light = IndevTest_LightLevel(x, y, z);
	below = y > 0 ? World_GetBlock(x, y - 1, z) : BLOCK_AIR;
	stay  = (light >= 8 || (light >= 4 && Lighting.IsLit(x, y, z))) &&
	        Indev_IsFarmland(below);
	if (!stay) { Indev_PopCrop(x, y, z, block); return; }

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

/* BlockSource.updateTick: an infinite spring - fill each of the 4 horizontal air
    neighbours with the flowing fluid (which then flows via the engine's own
    liquid physics). The source block itself just sits and refills. */
static void Indev_TickSource(int index, BlockID block) {
	int x, y, z;
	BlockID fluid = block == INDEV_BLOCK_LAVA_SOURCE ? BLOCK_LAVA : BLOCK_WATER;
	World_Unpack(index, x, y, z);
	if (World_Contains(x - 1, y, z) && World_GetBlock(x - 1, y, z) == BLOCK_AIR) Game_UpdateBlock(x - 1, y, z, fluid);
	if (World_Contains(x + 1, y, z) && World_GetBlock(x + 1, y, z) == BLOCK_AIR) Game_UpdateBlock(x + 1, y, z, fluid);
	if (World_Contains(x, y, z - 1) && World_GetBlock(x, y, z - 1) == BLOCK_AIR) Game_UpdateBlock(x, y, z - 1, fluid);
	if (World_Contains(x, y, z + 1) && World_GetBlock(x, y, z + 1) == BLOCK_AIR) Game_UpdateBlock(x, y, z + 1, fluid);
}

/* ==== genuine finite fluids - BlockFlowing / BlockStationary port ====
   The classic engine floods fluid infinitely (every neighbour + down, forever).
   Genuine in-20100223 fluid is FINITE:
   - a moving cell spreads DOWN first, else to at most ONE random horizontal
     neighbour per update - and each spread over a full body pulls its volume
     from a DONOR cell (World.fluidFlowCheck picks the farthest/highest cell of
     the connected body and removes it), conserving volume;
   - a body touching a SOURCE block (waterSource 52 / lavaSource 53) reports
     infinite supply (-9999) and never donates - that is what makes springs;
   - a cell with somewhere to go that fails to spread STAGNATES: 1/3 roll each
     update, then 1/3 to retry as a plain spread, else lava petrifies to stone
     and water evaporates;
   - water extinguishes adjacent fire and petrifies adjacent lava; lava ignites
     flammable neighbours (BlockFire.fireSpread); a settled cell turns STILL;
   - still fluid wakes back to moving on neighbour changes (BlockStationary),
     petrifying when the opposite fluid arrives;
   - updates ride the scheduled-update list: tickRate 5 (water) / 25 (lava).
   Registered per Indev map only - classic/c0.30 keep the engine flood. */

#define FLUID_SCHED_MAX 4096
static struct { int index; cc_uint16 delay; cc_uint8 water; } indev_fluidSched[FLUID_SCHED_MAX];
static int      indev_fluidSchedCount;
static RNGState indev_fluidRng;
static int      indev_liquidOrder[4] = { 0, 1, 2, 3 }; /* BlockFlowing.liquidIntArray */

/* flood-fill scratch, lazily sized; stamps use the genuine x + (z << 10)
    layer packing so the scanline walks port verbatim (maps cap at 512) */
static cc_uint16* fluid_stamps;
static int        fluid_stampsLen;
static cc_uint16  fluid_counter;
static int*       fluid_stackA;
static int*       fluid_stackB;
static int        fluid_stackCap;

static cc_bool Fluid_EnsureScratch(void) {
	int stamps = 1024 * World.Length;
	int cap    = World.Width * World.Length + 64;
	if (fluid_stamps && fluid_stampsLen == stamps && fluid_stackCap == cap) return true;
	Mem_Free(fluid_stamps); Mem_Free(fluid_stackA); Mem_Free(fluid_stackB);
	fluid_stampsLen = stamps; fluid_stackCap = cap; fluid_counter = 0;
	fluid_stamps = (cc_uint16*)Mem_TryAllocCleared(stamps, 2);
	fluid_stackA = (int*)Mem_TryAlloc(cap, 4);
	fluid_stackB = (int*)Mem_TryAlloc(cap, 4);
	if (fluid_stamps && fluid_stackA && fluid_stackB) return true;
	Mem_Free(fluid_stamps); Mem_Free(fluid_stackA); Mem_Free(fluid_stackB);
	fluid_stamps = NULL; fluid_stackA = NULL; fluid_stackB = NULL; fluid_stampsLen = 0;
	return false;
}
static void Fluid_BumpCounter(void) {
	/* the genuine 30000-generation stamp reset */
	if (++fluid_counter == 30000) {
		Mem_Set(fluid_stamps, 0, fluid_stampsLen * 2);
		fluid_counter = 1;
	}
}

static cc_bool Fluid_IsWaterMat(BlockID b) {
	return b == BLOCK_WATER || b == BLOCK_STILL_WATER || b == INDEV_BLOCK_WATER_SOURCE;
}
static cc_bool Fluid_IsLavaMat(BlockID b) {
	return b == BLOCK_LAVA || b == BLOCK_STILL_LAVA || b == INDEV_BLOCK_LAVA_SOURCE;
}

/* BlockFluid.canFlow: target material must be neither liquid nor solid (air,
    plants, torches, fire, gears - flowing over them destroys them), and water
    additionally refuses within 2 blocks of a sponge. */
static cc_bool Fluid_CanFlowInto(cc_bool water, int x, int y, int z) {
	BlockID b;
	int sx, sy, sz;
	if (!World_Contains(x, y, z)) return false;
	b = World_GetBlock(x, y, z);
	if (b != BLOCK_AIR && Blocks.Collide[b] != COLLIDE_NONE) return false;

	if (water) {
		for (sx = x - 2; sx <= x + 2; sx++)
			for (sy = y - 2; sy <= y + 2; sy++)
				for (sz = z - 2; sz <= z + 2; sz++) {
					if (!World_Contains(sx, sy, sz)) continue;
					if (World_GetBlock(sx, sy, sz) == BLOCK_SPONGE) return false;
				}
	}
	return true;
}

#define FLUID_PACK2(x, z)  ((x) + ((z) << 10))
#define FLUID_BODY(b)      ((b) == moving || (b) == still)

/* World.floodFill(x, y, z, moving, still): scanline-floods the connected
    fluid body on ONE layer. 0 = found adjacent air (body can still absorb),
    1 = fully closed, 2 = touches the map border. */
static int Fluid_FloodFill(int x, int y, int z, BlockID moving, BlockID still) {
	int top = 0, p2, runB;
	cc_bool spanN, spanS, match;
	BlockID b;
	if (!World_Contains(x, y, z))  return 0;
	if (!Fluid_EnsureScratch())    return 2; /* alloc failure: act border-ish (no equalize) */
	Fluid_BumpCounter();

	fluid_stackA[top++] = FLUID_PACK2(x, z);

	while (top > 0) {
		p2 = fluid_stackA[--top];
		if (fluid_stamps[p2] == fluid_counter) continue;
		x = p2 % 1024; z = p2 / 1024;
		if (x == 0 || x == World.Width - 1 || y == 0 || y == World.Height - 1 ||
			z == 0 || z == World.Length - 1) return 2;

		/* walk to the west end of this run */
		while (x > 0 && fluid_stamps[p2 - 1] != fluid_counter &&
			   FLUID_BODY(World_GetBlock(x - 1, y, z))) { x--; p2--; }
		if (x > 0 && World_GetBlock(x - 1, y, z) == BLOCK_AIR) return 0;

		spanN = false; spanS = false;
		for (; x < World.Width && fluid_stamps[p2] != fluid_counter &&
			   FLUID_BODY(World_GetBlock(x, y, z)); x++, p2++) {
			if (x == 0 || x == World.Width - 1) return 2;

			if (z > 0) {
				b = World_GetBlock(x, y, z - 1);
				if (b == BLOCK_AIR) return 0;
				match = fluid_stamps[p2 - 1024] != fluid_counter && FLUID_BODY(b);
				if (match && !spanN) {
					if (top < fluid_stackCap) fluid_stackA[top++] = p2 - 1024;
				}
				spanN = match;
			}
			if (z < World.Length - 1) {
				b = World_GetBlock(x, y, z + 1);
				if (b == BLOCK_AIR) return 0;
				match = fluid_stamps[p2 + 1024] != fluid_counter && FLUID_BODY(b);
				if (match && !spanS) {
					if (top < fluid_stackCap) fluid_stackA[top++] = p2 + 1024;
				}
				spanS = match;
			}
			fluid_stamps[p2] = fluid_counter;
		}
		if (x < World.Width && World_GetBlock(x, y, z) == BLOCK_AIR) return 0;
	}
	return 1;
}

/* World.fluidFlowCheck(x, y, z, moving, still): walks the connected body
    upward from layer y looking for a donor cell to pull volume from.
    Returns the packed ((y << 10 | z) << 10 | x) of the farthest cell on the
    highest layer, -9999 when the body touches a SOURCE block (infinite
    supply), or -1 out of bounds. */
static int Fluid_FlowCheck(int x, int y, int z, BlockID moving, BlockID still) {
	int ox = x, oz = z, top = 0, upTop, best, donor, p2, dz2, d;
	cc_bool sourced, spanN, spanS, spanUp, match;
	BlockID source, b;
	int* tmp;
	if (!World_Contains(x, y, z)) return -1;
	if (!Fluid_EnsureScratch())   return -1;

	source = moving == BLOCK_WATER ? INDEV_BLOCK_WATER_SOURCE : INDEV_BLOCK_LAVA_SOURCE;
	donor  = ((y << 10 | z) << 10) | x;
	sourced = false;
	fluid_stackA[top++] = FLUID_PACK2(x, z);

	for (; y < World.Height; y++) {
		best  = -1;
		upTop = 0;
		Fluid_BumpCounter();

		while (top > 0) {
			p2 = fluid_stackA[--top];
			if (fluid_stamps[p2] == fluid_counter) continue;
			x = p2 % 1024; z = p2 / 1024;
			dz2 = (z - oz) * (z - oz);

			while (x > 0 && fluid_stamps[p2 - 1] != fluid_counter &&
				   FLUID_BODY(World_GetBlock(x - 1, y, z))) { x--; p2--; }
			if (x > 0 && World_GetBlock(x - 1, y, z) == source) sourced = true;

			spanN = false; spanS = false; spanUp = false;
			for (; x < World.Width && fluid_stamps[p2] != fluid_counter &&
				   FLUID_BODY(World_GetBlock(x, y, z)); x++, p2++) {
				if (z > 0) {
					b = World_GetBlock(x, y, z - 1);
					if (b == source) sourced = true;
					match = fluid_stamps[p2 - 1024] != fluid_counter && FLUID_BODY(b);
					if (match && !spanN) {
						if (top < fluid_stackCap) fluid_stackA[top++] = p2 - 1024;
					}
					spanN = match;
				}
				if (z < World.Length - 1) {
					b = World_GetBlock(x, y, z + 1);
					if (b == source) sourced = true;
					match = fluid_stamps[p2 + 1024] != fluid_counter && FLUID_BODY(b);
					if (match && !spanS) {
						if (top < fluid_stackCap) fluid_stackA[top++] = p2 + 1024;
					}
					spanS = match;
				}
				if (y < World.Height - 1) {
					b = World_GetBlock(x, y + 1, z);
					match = FLUID_BODY(b);
					if (match && !spanUp) {
						if (upTop < fluid_stackCap) fluid_stackB[upTop++] = p2;
					}
					spanUp = match;
				}

				d = (x - ox) * (x - ox) + dz2;
				if (d > best) {
					best  = d;
					donor = ((y << 10 | z) << 10) | x;
				}
				fluid_stamps[p2] = fluid_counter;
			}
			if (x < World.Width && World_GetBlock(x, y, z) == source) sourced = true;
		}

		if (upTop == 0) break;
		tmp = fluid_stackA; fluid_stackA = fluid_stackB; fluid_stackB = tmp;
		top = upTop;
	}

	return sourced ? -9999 : donor;
}

static void Indev_FluidSchedule(int index, cc_bool water) {
	int i;
	cc_uint16 delay = water ? 5 : 25; /* BlockFlowing.tickRate */
	for (i = 0; i < indev_fluidSchedCount; i++) {
		if (indev_fluidSched[i].index == index) return; /* already queued */
	}
	if (indev_fluidSchedCount >= FLUID_SCHED_MAX) return; /* overflow: drop (self-heals) */
	indev_fluidSched[indev_fluidSchedCount].index = index;
	indev_fluidSched[indev_fluidSchedCount].delay = delay;
	indev_fluidSched[indev_fluidSchedCount].water = water;
	indev_fluidSchedCount++;
}

/* BlockFlowing.liquidSpread: the plain (stagnation-retry) spread */
static cc_bool Fluid_Spread(BlockID moving, cc_bool water, int tx, int ty, int tz) {
	if (!Fluid_CanFlowInto(water, tx, ty, tz)) return false;
	Game_UpdateBlock(tx, ty, tz, moving);
	Indev_FluidSchedule(World_Pack(tx, ty, tz), water);
	return true;
}

/* BlockFlowing.liquidSpread2: spread + donor removal (volume conservation) */
static cc_bool Fluid_Spread2(int x, int y, int z, BlockID moving, BlockID still,
							 cc_bool water, int tx, int ty, int tz) {
	int r, dx, dy, dz;
	if (!Fluid_CanFlowInto(water, tx, ty, tz)) return false;

	r = Fluid_FlowCheck(x, y, z, moving, still);
	if (r != -9999) {
		if (r < 0) return false;
		dx = r & 1023; r >>= 10;
		dz = r & 1023; r >>= 10;
		dy = r & 1023;
		/* the genuine donor sanity test, ported verbatim: refuse when the
		    donor sits at/below the target level in the interior and the
		    target can't keep falling */
		if ((dy > ty || !Fluid_CanFlowInto(water, tx, ty - 1, tz)) && dy <= ty &&
			dx != 0 && dx != World.Width - 1 && dz != 0 && dz != World.Length - 1) {
			return false;
		}
		Game_UpdateBlock(dx, dy, dz, BLOCK_AIR);
	}

	Game_UpdateBlock(tx, ty, tz, moving);
	Indev_FluidSchedule(World_Pack(tx, ty, tz), water);
	return true;
}

/* water side effects: extinguish adjacent fire, petrify adjacent lava */
static cc_bool Fluid_WaterContact(int x, int y, int z) {
	BlockID b;
	if (!World_Contains(x, y, z)) return false;
	b = World_GetBlock(x, y, z);
	if (b == INDEV_BLOCK_FIRE) { Game_UpdateBlock(x, y, z, BLOCK_AIR);   return true; }
	if (b == BLOCK_LAVA || b == BLOCK_STILL_LAVA) {
		Game_UpdateBlock(x, y, z, BLOCK_STONE);
		return true;
	}
	return false;
}

/* BlockFlowing.update - the whole genuine driver */
static void Indev_FluidUpdate(int index, BlockID block) {
	int x, y, z, i, j, t, r, dx, dy, dz;
	cc_bool water = block == BLOCK_WATER;
	BlockID moving = water ? BLOCK_WATER : BLOCK_LAVA;
	BlockID still  = water ? BLOCK_STILL_WATER : BLOCK_STILL_LAVA;
	cc_bool spread = false, canSide;
	BlockID below;
	World_Unpack(index, x, y, z);

	canSide = Fluid_CanFlowInto(water, x - 1, y, z) || Fluid_CanFlowInto(water, x + 1, y, z) ||
	          Fluid_CanFlowInto(water, x, y, z - 1) || Fluid_CanFlowInto(water, x, y, z + 1);

	below = y > 0 ? World_GetBlock(x, y - 1, z) : BLOCK_AIR;
	if (canSide && y > 0 &&
		(water ? Fluid_IsWaterMat(below) : Fluid_IsLavaMat(below))) {
		/* sitting on our own fluid with somewhere to go: equalize through
		    the body below when it is closed */
		if (Fluid_FloodFill(x, y - 1, z, moving, still) == 1) {
			r = Fluid_FlowCheck(x, y, z, moving, still);
			if (r != -9999) {
				if (r < 0) return;
				dx = r & 1023; r >>= 10;
				dz = r & 1023; r >>= 10;
				dy = r & 1023;
				Game_UpdateBlock(dx, dy, dz, BLOCK_AIR);
			}
			return;
		}
	}

	spread = Fluid_Spread2(x, y, z, moving, still, water, x, y - 1, z);

	/* one random horizontal direction per update (the genuine partial
	    Fisher-Yates over liquidIntArray) */
	for (i = 0; i < 4; i++) {
		j = Random_Next(&indev_fluidRng, 4 - i) + i;
		t = indev_liquidOrder[i]; indev_liquidOrder[i] = indev_liquidOrder[j]; indev_liquidOrder[j] = t;
		if (indev_liquidOrder[i] == 0 && !spread) spread = Fluid_Spread2(x, y, z, moving, still, water, x - 1, y, z);
		if (indev_liquidOrder[i] == 1 && !spread) spread = Fluid_Spread2(x, y, z, moving, still, water, x + 1, y, z);
		if (indev_liquidOrder[i] == 2 && !spread) spread = Fluid_Spread2(x, y, z, moving, still, water, x, y, z - 1);
		if (indev_liquidOrder[i] == 3 && !spread) spread = Fluid_Spread2(x, y, z, moving, still, water, x, y, z + 1);
	}

	if (!spread && canSide) {
		/* stagnation: 1/3 roll, then 1/3 plain-spread retry, else water
		    evaporates / lava petrifies */
		if (Random_Next(&indev_fluidRng, 3) == 0) {
			if (Random_Next(&indev_fluidRng, 3) == 0) {
				spread = false;
				for (i = 0; i < 4; i++) {
					j = Random_Next(&indev_fluidRng, 4 - i) + i;
					t = indev_liquidOrder[i]; indev_liquidOrder[i] = indev_liquidOrder[j]; indev_liquidOrder[j] = t;
					if (indev_liquidOrder[i] == 0 && !spread) spread = Fluid_Spread(moving, water, x - 1, y, z);
					if (indev_liquidOrder[i] == 1 && !spread) spread = Fluid_Spread(moving, water, x + 1, y, z);
					if (indev_liquidOrder[i] == 2 && !spread) spread = Fluid_Spread(moving, water, x, y, z - 1);
					if (indev_liquidOrder[i] == 3 && !spread) spread = Fluid_Spread(moving, water, x, y, z + 1);
				}
			} else if (!water) {
				Game_UpdateBlock(x, y, z, BLOCK_STONE);
			} else {
				Game_UpdateBlock(x, y, z, BLOCK_AIR);
			}
		}
		return;
	}

	if (water) {
		spread |= Fluid_WaterContact(x - 1, y, z);
		spread |= Fluid_WaterContact(x + 1, y, z);
		spread |= Fluid_WaterContact(x, y, z - 1);
		spread |= Fluid_WaterContact(x, y, z + 1);
	} else {
		spread |= IndevFire_LavaFlowInto(x - 1, y, z);
		spread |= IndevFire_LavaFlowInto(x + 1, y, z);
		spread |= IndevFire_LavaFlowInto(x, y, z - 1);
		spread |= IndevFire_LavaFlowInto(x, y, z + 1);
	}

	if (!spread) {
		/* settled: become still (genuine setTileNoUpdate - our update raises
		    neighbour notifies too; the still-wake handler ignores them unless
		    flow is actually possible, so the body converges) */
		Game_UpdateBlock(x, y, z, still);
	} else {
		Indev_FluidSchedule(index, water);
	}
}

/* processes the scheduled fluid updates (World.tick's scheduledUpdates) */
void IndevTest_TickFluids(void) {
	int i, index;
	BlockID b;
	if (!IndevTest_Enabled || !World.Blocks) return;

	for (i = 0; i < indev_fluidSchedCount; ) {
		if (indev_fluidSched[i].delay > 0) { indev_fluidSched[i].delay--; i++; continue; }
		index = indev_fluidSched[i].index;
		/* swap-remove BEFORE running (the update may reschedule this cell) */
		indev_fluidSched[i] = indev_fluidSched[--indev_fluidSchedCount];

		b = World.Blocks[index];
		if (b == BLOCK_WATER || b == BLOCK_LAVA) Indev_FluidUpdate(index, b);
	}
}

/* onBlockAdded: a placed/spawned moving fluid schedules its first update */
static void IndevFluid_PlaceMoving(int index, BlockID block) {
	Indev_FluidSchedule(index, block == BLOCK_WATER);
}
/* genuine BlockFlowing.onNeighborBlockChange is EMPTY - moving fluid ignores
    neighbour changes entirely (only scheduled + random ticks drive it) */
static void IndevFluid_Noop(int index, BlockID block) { }

/* BlockStationary.onNeighborBlockChange: wake to moving when flow is possible
    (or fire encourages, for lava), petrify on contact with the opposite fluid.
    Engine activations don't carry the changed neighbour's id, so the petrify
    check scans the 6 neighbours instead - same trigger in practice. */
static void IndevFluid_ActivateStill(int index, BlockID block) {
	int x, y, z;
	cc_bool water = block == BLOCK_STILL_WATER;
	cc_bool wake;
	static const int NX[6] = { -1, 1, 0, 0, 0, 0 };
	static const int NY[6] = { 0, 0, -1, 1, 0, 0 };
	static const int NZ[6] = { 0, 0, 0, 0, -1, 1 };
	int n;
	BlockID nb;
	World_Unpack(index, x, y, z);

	for (n = 0; n < 6; n++) {
		int nx = x + NX[n], ny = y + NY[n], nz = z + NZ[n];
		if (!World_Contains(nx, ny, nz)) continue;
		nb = World_GetBlock(nx, ny, nz);
		if (water ? Fluid_IsLavaMat(nb) : Fluid_IsWaterMat(nb)) {
			Game_UpdateBlock(x, y, z, BLOCK_STONE);
			return;
		}
	}

	wake = Fluid_CanFlowInto(water, x, y - 1, z) ||
	       Fluid_CanFlowInto(water, x - 1, y, z) || Fluid_CanFlowInto(water, x + 1, y, z) ||
	       Fluid_CanFlowInto(water, x, y, z - 1) || Fluid_CanFlowInto(water, x, y, z + 1);
	if (!wake && !water) {
		for (n = 0; n < 6; n++) {
			int nx = x + NX[n], ny = y + NY[n], nz = z + NZ[n];
			if (!World_Contains(nx, ny, nz)) continue;
			if (IndevFire_CanCatch(World_GetBlock(nx, ny, nz))) { wake = true; break; }
		}
	}
	if (!wake) return;

	Game_UpdateBlock(x, y, z, water ? BLOCK_WATER : BLOCK_LAVA);
	Indev_FluidSchedule(index, water);
}

/* random ticks hit moving fluid too (World.tick random pass calls updateTick;
    BlockStationary's is empty) */
static void IndevFluid_RandomMoving(int index, BlockID block) {
	Indev_FluidUpdate(index, block);
}

/* setTickOnLoad: schedule every moving-fluid cell when a map arrives */
void IndevTest_FluidsOnMapLoaded(void) {
	int i;
	indev_fluidSchedCount = 0;
	if (!World.Blocks) return;
	for (i = 0; i < World.Volume; i++) {
		if (World.Blocks[i] == BLOCK_WATER) Indev_FluidSchedule(i, true);
		else if (World.Blocks[i] == BLOCK_LAVA) Indev_FluidSchedule(i, false);
	}
}

/* --- genuine plant ticks (BlockFlower/BlockMushroom/BlockSapling) --- */

/* Sapling growth stage (genuine metadata 0-15) - a lazily sized per-map side
    store, the same pattern as IndevFire's age nibble. */
static cc_uint8* indev_saplingStage;
static int       indev_saplingVol;
static void Sapling_EnsureStages(void) {
	if (indev_saplingVol == World.Volume && indev_saplingStage) return;
	Mem_Free(indev_saplingStage);
	indev_saplingVol   = World.Volume;
	indev_saplingStage = World.Volume ? (cc_uint8*)Mem_TryAllocCleared(World.Volume, 1) : NULL;
}
static int Indev_SaplingStage(int index) {
	if (!indev_saplingStage || index < 0 || index >= indev_saplingVol) return 0;
	return indev_saplingStage[index];
}
static void Indev_SetSaplingStage(int index, int stage) {
	Sapling_EnsureStages();
	if (!indev_saplingStage || index < 0 || index >= indev_saplingVol) return;
	indev_saplingStage[index] = (cc_uint8)(stage & 15);
}

/* BlockFlower.canThisPlantGrowOnThisBlockID: grass, dirt or farmland */
static cc_bool Indev_PlantSoilOk(BlockID below) {
	return below == BLOCK_GRASS || below == BLOCK_DIRT ||
	       below == INDEV_BLOCK_FARMLAND || below == INDEV_BLOCK_FARMLAND_WET;
}

/* checkFlowerChange: dropBlockAsItem(self) THEN remove - the popped plant
    drops itself with the genuine block-drop scatter (floor + rand*0.7 + 0.15) */
static void Indev_PopPlant(int x, int y, int z, BlockID block) {
	Vec3 pos;
	pos.x = x + Random_Float(&indev_teRng) * 0.7f + 0.15f;
	pos.y = y + Random_Float(&indev_teRng) * 0.7f + 0.15f;
	pos.z = z + Random_Float(&indev_teRng) * 0.7f + 0.15f;
	SurvivalTest_SpawnDropWorld(pos, block, 1);
	Game_UpdateBlock(x, y, z, BLOCK_AIR);
}

/* BlockFlower.canBlockStay: light >= 8, or light >= 4 with open sky above,
    on grass/dirt/farmland. Returns whether the plant popped. */
static cc_bool Indev_FlowerStayCheck(int index, BlockID block) {
	int x, y, z, light;
	BlockID below;
	World_Unpack(index, x, y, z);

	light = IndevTest_LightLevel(x, y, z);
	below = y > 0 ? World.Blocks[index - World.OneY] : BLOCK_AIR;
	if ((light >= 8 || (light >= 4 && Lighting.IsLit(x, y, z))) &&
		Indev_PlantSoilOk(below)) return false;

	Indev_PopPlant(x, y, z, block);
	return true;
}

static void Indev_TickFlower(int index, BlockID block) {
	Indev_FlowerStayCheck(index, block);
}

/* BlockMushroom.canBlockStay: light <= 13 and ANY opaque cube below */
static void Indev_TickMushroom(int index, BlockID block) {
	int x, y, z;
	BlockID below;
	World_Unpack(index, x, y, z);

	below = y > 0 ? World.Blocks[index - World.OneY] : BLOCK_AIR;
	if (IndevTest_LightLevel(x, y, z) <= 13 && Blocks.FullOpaque[below]) return;
	Indev_PopPlant(x, y, z, block);
}

/* World.growTrees runtime port: trunk rand(3)+4, clearance envelope, grass/
    dirt below (converted to dirt), diamond canopy with corner trimming.
    (The generator keeps its own bit-exact copy on the gen RNG streams;
    runtime growth draws from the world-side indev_teRng instead.) */
static cc_bool Indev_GrowTree(int x, int y, int z) {
	int trunkH = Random_Next(&indev_teRng, 3) + 4;
	int xx, yy, zz, clearance, dy, radius, dxa, dza;
	BlockID below;

	if (y <= 0 || y + trunkH + 1 > World.Height) return false;

	for (yy = y; yy <= y + 1 + trunkH; yy++) {
		clearance = 1;
		if (yy == y) clearance = 0;
		if (yy >= y + 1 + trunkH - 2) clearance = 2;

		for (xx = x - clearance; xx <= x + clearance; xx++) {
			for (zz = z - clearance; zz <= z + clearance; zz++) {
				if (!World_Contains(xx, yy, zz))               return false;
				if (World_GetBlock(xx, yy, zz) != BLOCK_AIR)   return false;
			}
		}
	}

	below = World_GetBlock(x, y - 1, z);
	if (below != BLOCK_GRASS && below != BLOCK_DIRT) return false;
	if (y >= World.Height - trunkH - 1)              return false;
	Game_UpdateBlock(x, y - 1, z, BLOCK_DIRT);

	for (yy = y - 3 + trunkH; yy <= y + trunkH; yy++) {
		dy     = yy - (y + trunkH);
		radius = 1 - dy / 2;

		for (xx = x - radius; xx <= x + radius; xx++) {
			dxa = xx - x; if (dxa < 0) dxa = -dxa;
			for (zz = z - radius; zz <= z + radius; zz++) {
				dza = zz - z; if (dza < 0) dza = -dza;
				if (dxa == radius && dza == radius &&
					(Random_Next(&indev_teRng, 2) == 0 || dy == 0)) continue;
				if (!World_Contains(xx, yy, zz)) continue;
				if (!Blocks.FullOpaque[World_GetBlock(xx, yy, zz)]) {
					Game_UpdateBlock(xx, yy, zz, BLOCK_LEAVES);
				}
			}
		}
	}

	for (yy = 0; yy < trunkH; yy++) {
		if (!Blocks.FullOpaque[World_GetBlock(x, y + yy, z)]) {
			Game_UpdateBlock(x, y + yy, z, BLOCK_LOG);
		}
	}
	return true;
}

/* BlockSapling.updateTick: the flower stay check first (super), then with
    light(x, y+1, z) >= 9 and a 1-in-5 roll the metadata stage climbs 0-15;
    only the 16th successful roll attempts the tree, restoring the sapling
    if growTrees fails. (Genuine removes/restores via setTileNoUpdate - the
    engine equivalent raises a couple of extra neighbour updates, harmless.) */
static void Indev_TickSapling(int index, BlockID block) {
	int x, y, z, stage;
	World_Unpack(index, x, y, z);

	if (Indev_FlowerStayCheck(index, block)) return;
	if (IndevTest_LightLevel(x, y + 1, z) < 9)     return;
	if (Random_Next(&indev_teRng, 5) != 0)         return;

	stage = Indev_SaplingStage(index);
	if (stage < 15) { Indev_SetSaplingStage(index, stage + 1); return; }

	Indev_SetSaplingStage(index, 0);
	Game_UpdateBlock(x, y, z, BLOCK_AIR);
	if (!Indev_GrowTree(x, y, z)) {
		Game_UpdateBlock(x, y, z, BLOCK_SAPLING);
	}
}

static void Indev_RegisterFarmTicks(void) {
	int k;
	Physics.OnRandomTick[INDEV_BLOCK_FARMLAND]     = Indev_TickFarmland;
	Physics.OnRandomTick[INDEV_BLOCK_FARMLAND_WET] = Indev_TickFarmland;
	for (k = 0; k < 8; k++) {
		Physics.OnRandomTick[INDEV_BLOCK_CROPS_0 + k] = Indev_TickCrops;
	}
	Physics.OnRandomTick[INDEV_BLOCK_WATER_SOURCE] = Indev_TickSource;
	Physics.OnRandomTick[INDEV_BLOCK_LAVA_SOURCE]  = Indev_TickSource;
	/* BlockSource.onBlockAdded fills the 4 sides immediately too */
	Physics.OnPlace[INDEV_BLOCK_WATER_SOURCE]      = Indev_TickSource;
	Physics.OnPlace[INDEV_BLOCK_LAVA_SOURCE]       = Indev_TickSource;
	/* genuine finite fluids replace the classic infinite flood on Indev
	    maps: moving fluid = scheduled/random updates only, still fluid wakes
	    on activation, and the classic Place/Activate flood hooks are dead */
	Physics.OnPlace[BLOCK_WATER]          = IndevFluid_PlaceMoving;
	Physics.OnPlace[BLOCK_LAVA]           = IndevFluid_PlaceMoving;
	Physics.OnActivate[BLOCK_WATER]       = IndevFluid_Noop;
	Physics.OnActivate[BLOCK_LAVA]        = IndevFluid_Noop;
	Physics.OnActivate[BLOCK_STILL_WATER] = IndevFluid_ActivateStill;
	Physics.OnActivate[BLOCK_STILL_LAVA]  = IndevFluid_ActivateStill;
	Physics.OnRandomTick[BLOCK_WATER]       = IndevFluid_RandomMoving;
	Physics.OnRandomTick[BLOCK_LAVA]        = IndevFluid_RandomMoving;
	Physics.OnRandomTick[BLOCK_STILL_WATER] = IndevFluid_Noop;
	Physics.OnRandomTick[BLOCK_STILL_LAVA]  = IndevFluid_Noop;

	/* genuine BlockFlower/BlockMushroom/BlockSapling ticks replace the
	    classic handlers on Indev maps (c0.30 keeps the classic ones) */
	Physics.OnRandomTick[BLOCK_SAPLING]      = Indev_TickSapling;
	Physics.OnRandomTick[BLOCK_DANDELION]    = Indev_TickFlower;
	Physics.OnRandomTick[BLOCK_ROSE]         = Indev_TickFlower;
	Physics.OnRandomTick[BLOCK_BROWN_SHROOM] = Indev_TickMushroom;
	Physics.OnRandomTick[BLOCK_RED_SHROOM]   = Indev_TickMushroom;
}

/* World.tick's random block update loop, at the genuine rate: updateLCG
    accumulates the world volume every tick and pays out volume/200 random
    updates (the remainder carries), with coordinates unpacked from the
    genuine randId LCG (randId*3 + 1013904223, >> 2, x from the low bits).
    The engine's own loop is 3 blocks per 16^3 chunk = volume/1365 - about
    6.8x sparser - which left crops visibly stalled and farmland dry for
    minutes. Handlers are the same Physics.OnRandomTick table, so classic
    grass/sapling/flower behaviour simply runs at the genuine rate too.
    Coordinate masks assume power-of-two dimensions like genuine Indev;
    the bounds check skips the (biased) picks on any odd-sized import. */
static int indev_updateLCG;
static cc_uint32 indev_randId;

/* genuine BlockGrass.updateTick: covered grass decays to dirt on a 1-in-4
    roll; lit grass spreads to a random nearby dirt block (+-1, y -3..+1).
    Genuine light thresholds (<4 decay, >=9 spread, >=4 target) are
    approximated with the engine's binary sky lighting. Note dirt has NO
    updateTick in Indev - it only becomes grass by spreading. */
static void IndevTest_TickGrass(int index) {
	int x, y, z;
	BlockID above;
	World_Unpack(index, x, y, z);

	/* Decay: genuine gates on `getBlockLightValue(x,y+1,z) < 4 &&
	    canBlockGrass(x,y+1,z)`. canBlockGrass is FALSE for air/glass/sprites
	    (transparent + logic materials) and true for opaque blocks - exactly
	    the blocks that also cut the skylight below them, so BlocksLight of the
	    block DIRECTLY above is a faithful stand-in. The key point: grass only
	    reverts when a light-blocker sits right on top of it. Grass merely
	    SHADOWED from afar (e.g. under a floating island, with open air above)
	    is NOT covered, so it keeps its grass - matching genuine. (The old
	    !IsLit test reverted any un-sky-lit grass, stripping every island's
	    underside back to dirt.) */
	above = (y + 1 < World.Height) ? World_GetBlock(x, y + 1, z) : BLOCK_AIR;
	if (Blocks.BlocksLight[above]) {
		if (Random_Next(&indev_teRng, 4) == 0) Game_UpdateBlock(x, y, z, BLOCK_DIRT);
		return;
	}

	/* Spread: a sky-lit grass block (approx. light >= 9) seeds a nearby sky-lit
	    dirt block (approx. light >= 4, nothing blocking grass above it). */
	if (!Lighting.IsLit(x, y, z)) return;
	x += Random_Next(&indev_teRng, 3) - 1;
	y += Random_Next(&indev_teRng, 5) - 3;
	z += Random_Next(&indev_teRng, 3) - 1;
	if (!World_Contains(x, y, z))              return;
	if (World_GetBlock(x, y, z) != BLOCK_DIRT) return;
	if (!Lighting.IsLit(x, y, z))              return;
	Game_UpdateBlock(x, y, z, BLOCK_GRASS);
}

/* genuine BlockLeaves.updateTick: a leaf whose block BELOW is non-solid and
    that has no log (Block.wood == id 17 == our BLOCK_LOG) within x+-2, y-1..y,
    z+-2 decays - it drops a sapling on the same 1-in-10 roll as mining a leaf,
    then vanishes. Because the decay only fires when the block below is not
    solid, a chopped canopy peels away from the bottom up over successive
    random ticks. c0.30's LeavesBlock has NO updateTick, so leaf decay is
    Indev-only (there leaves are permanent unless mined). */
static void IndevTest_TickLeaves(int index) {
	int x, y, z, dx, dy, dz;
	BlockID below;
	Vec3 pos;
	World_Unpack(index, x, y, z);

	below = (y > 0) ? World_GetBlock(x, y - 1, z) : BLOCK_AIR;
	if (Blocks.Collide[below] == COLLIDE_SOLID) return; /* !isSolid(below) gate */

	for (dx = x - 2; dx <= x + 2; dx++) {
		for (dy = y - 1; dy <= y; dy++) {
			for (dz = z - 2; dz <= z + 2; dz++) {
				if (World_Contains(dx, dy, dz) &&
					World_GetBlock(dx, dy, dz) == BLOCK_LOG) return; /* log nearby - keep */
			}
		}
	}

	/* no log: dropBlockAsItem (sapling, quantityDropped 1-in-10) then remove */
	if (Random_Next(&indev_teRng, 10) == 0) {
		pos.x = x + Random_Float(&indev_teRng) * 0.7f + 0.15f;
		pos.y = y + Random_Float(&indev_teRng) * 0.7f + 0.15f;
		pos.z = z + Random_Float(&indev_teRng) * 0.7f + 0.15f;
		SurvivalTest_SpawnDropWorld(pos, BLOCK_SAPLING, 1);
	}
	Game_UpdateBlock(x, y, z, BLOCK_AIR);
}

void IndevTest_TickRandomBlocks(void) {
	int shiftX = 1, shiftZ = 1;
	int maskX, maskY, maskZ;
	int count, i, x, y, z, index;
	cc_uint32 bits;
	BlockID block;
	PhysicsHandler tick;
	if (!IndevTest_Enabled || !World.Blocks) return;

	while ((1 << shiftX) < World.Width)  shiftX++;
	while ((1 << shiftZ) < World.Length) shiftZ++;
	maskX = World.Width - 1; maskZ = World.Length - 1; maskY = World.Height - 1;

	indev_updateLCG += World.Volume;
	count = indev_updateLCG / 200;
	indev_updateLCG -= count * 200;

	for (i = 0; i < count; i++) {
		indev_randId = indev_randId * 3u + 1013904223u;
		bits = indev_randId >> 2;
		x = (int)(bits & maskX);
		z = (int)((bits >> shiftX) & maskZ);
		y = (int)((bits >> (shiftX + shiftZ)) & maskY);
		if (x >= World.Width || y >= World.Height || z >= World.Length) continue;

		index = World_Pack(x, y, z);
		block = World.Blocks[index];

		/* genuine in-20100223 updateTick coverage: sand/gravel/dirt have no
		    updateTick at all and BlockStationary's is empty, so the engine's
		    classic random-tick handlers for them must not run in Indev mode
		    (random-tick sand rain hollows out Floating maps in seconds) */
		if (block == BLOCK_SAND || block == BLOCK_GRAVEL || block == BLOCK_DIRT ||
			block == BLOCK_STILL_WATER || block == BLOCK_STILL_LAVA) continue;
		if (block == BLOCK_GRASS) { IndevTest_TickGrass(index); continue; }
		if (block == BLOCK_LEAVES) { IndevTest_TickLeaves(index); continue; }
		if (IndevFire_IsFire(block)) { IndevFire_RandomTick(index); continue; }

		tick  = Physics.OnRandomTick[block];
		if (tick) tick(index, block);
	}
}

/*########################################################################################################################*
*--------------------------------------Random display ticks (client ambience)---------------------------------------------*
*#########################################################################################################################*/
static cc_bool Indev_NormalCube(int x, int y, int z);
static RNGState indev_dispRng;
static cc_bool  indev_dispRngInited;

/* BlockFire.randomDisplayTick: the ambient "fire.fire" crackle roll plus
    the large smoke plumes - hugging each burnable neighbour face when the
    fire clings to walls, or pouring out the top when it sits on fuel. */
static void Indev_FireDisplayTick(int x, int y, int z) {
	RNGState* r = &indev_dispRng;
	float fx, fy, fz;
	int i;

	if (Random_Next(r, 24) == 0) {
		SurvivalTest_PlaySoundAtBlock(x, y, z, MOBSND_FIRE,
			1.0f + Random_Float(r), Random_Float(r) * 0.7f + 0.3f);
	}

	if (!Indev_NormalCube(x, y - 1, z) &&
		!IndevFire_CanCatch(World_GetBlock(x, y - 1, z))) {
		if (IndevFire_CanCatch(World_GetBlock(x - 1, y, z))) {
			for (i = 0; i < 2; i++) {
				fx = (float)x + Random_Float(r) * 0.1f;
				fy = (float)y + Random_Float(r);
				fz = (float)z + Random_Float(r);
				SurvivalTest_SpawnSmokeFX(fx, fy, fz, 2.5f);
			}
		}
		if (IndevFire_CanCatch(World_GetBlock(x + 1, y, z))) {
			for (i = 0; i < 2; i++) {
				fx = (float)(x + 1) - Random_Float(r) * 0.1f;
				fy = (float)y + Random_Float(r);
				fz = (float)z + Random_Float(r);
				SurvivalTest_SpawnSmokeFX(fx, fy, fz, 2.5f);
			}
		}
		if (IndevFire_CanCatch(World_GetBlock(x, y, z - 1))) {
			for (i = 0; i < 2; i++) {
				fx = (float)x + Random_Float(r);
				fy = (float)y + Random_Float(r);
				fz = (float)z + Random_Float(r) * 0.1f;
				SurvivalTest_SpawnSmokeFX(fx, fy, fz, 2.5f);
			}
		}
		if (IndevFire_CanCatch(World_GetBlock(x, y, z + 1))) {
			for (i = 0; i < 2; i++) {
				fx = (float)x + Random_Float(r);
				fy = (float)y + Random_Float(r);
				fz = (float)(z + 1) - Random_Float(r) * 0.1f;
				SurvivalTest_SpawnSmokeFX(fx, fy, fz, 2.5f);
			}
		}
		if (IndevFire_CanCatch(World_GetBlock(x, y + 1, z))) {
			for (i = 0; i < 2; i++) {
				fx = (float)x + Random_Float(r);
				fy = (float)(y + 1) - Random_Float(r) * 0.1f;
				fz = (float)z + Random_Float(r);
				SurvivalTest_SpawnSmokeFX(fx, fy, fz, 2.5f);
			}
		}
	} else {
		for (i = 0; i < 3; i++) {
			fx = (float)x + Random_Float(r);
			fy = (float)y + Random_Float(r) * 0.5f + 0.5f;
			fz = (float)z + Random_Float(r);
			SurvivalTest_SpawnSmokeFX(fx, fy, fz, 2.5f);
		}
	}
}

/* BlockFluid.randomDisplayTick (both still and flowing inherit it):
    lava under open air spits a fullbright ember 1/100, and water resting
    at an exposed ledge edge throws 4 foam droplets off each open side.
    (The liquid.lava/liquid.water ambient-sound roll in the genuine method
    is dead code in this build - nextInt(128) == -1 is never true.) */
static cc_bool Indev_LiquidAirCheck(int x, int y, int z) {
	BlockID side  = World_Contains(x, y, z)     ? World_GetBlock(x, y, z)     : BLOCK_AIR;
	BlockID below = World_Contains(x, y - 1, z) ? World_GetBlock(x, y - 1, z) : BLOCK_AIR;
	if (Blocks.Collide[side] == COLLIDE_SOLID)  return false;
	if (Blocks.Collide[side] == COLLIDE_LIQUID) return false;
	return Blocks.Collide[below] == COLLIDE_SOLID ||
	       Blocks.Collide[below] == COLLIDE_LIQUID;
}

static void Indev_FluidDisplayTick(int x, int y, int z, cc_bool lava) {
	RNGState* r = &indev_dispRng;
	int i;

	if (lava) {
		BlockID above = World_Contains(x, y + 1, z) ? World_GetBlock(x, y + 1, z) : BLOCK_AIR;
		if (above == BLOCK_AIR && !Indev_NormalCube(x, y + 1, z) &&
			Random_Next(r, 100) == 0) {
			/* this.maxY: the fluid's render top (setBlockBounds 0.91) */
			SurvivalTest_SpawnLavaFX((float)x + Random_Float(r),
									 (float)y + 0.91f,
									 (float)z + Random_Float(r));
		}
		return;
	}

	if (Indev_LiquidAirCheck(x + 1, y, z)) {
		for (i = 0; i < 4; i++)
			SurvivalTest_SpawnSplashFX((float)(x + 1) + 2.0f/16.0f, (float)y,
									   (float)z + Random_Float(r));
	}
	if (Indev_LiquidAirCheck(x - 1, y, z)) {
		for (i = 0; i < 4; i++)
			SurvivalTest_SpawnSplashFX((float)x - 2.0f/16.0f, (float)y,
									   (float)z + Random_Float(r));
	}
	if (Indev_LiquidAirCheck(x, y, z + 1)) {
		for (i = 0; i < 4; i++)
			SurvivalTest_SpawnSplashFX((float)x + Random_Float(r), (float)y,
									   (float)(z + 1) + 2.0f/16.0f);
	}
	if (Indev_LiquidAirCheck(x, y, z - 1)) {
		for (i = 0; i < 4; i++)
			SurvivalTest_SpawnSplashFX((float)x + Random_Float(r), (float)y,
									   (float)z - 2.0f/16.0f);
	}
}

/* BlockTorch.randomDisplayTick: one smoke wisp + one flame fleck above the
    torch head, offset toward the wall for the hanging metas. */
static void Indev_TorchDisplayTick(int x, int y, int z, int meta) {
	float fx = (float)x + 0.5f;
	float fy = (float)y + 0.7f;
	float fz = (float)z + 0.5f;

	switch (meta) {
	case 1: fx -= 0.27f; fy += 0.22f; break;
	case 2: fx += 0.27f; fy += 0.22f; break;
	case 3: fz -= 0.27f; fy += 0.22f; break;
	case 4: fz += 0.27f; fy += 0.22f; break;
	}
	SurvivalTest_SpawnSmokeFX(fx, fy, fz, 1.0f);
	SurvivalTest_SpawnFlameFX(fx, fy, fz);
}

/* BlockFurnace.randomDisplayTick (lit only): smoke + flame licking out of
    the front face, at a random height along the mouth. */
static void Indev_FurnaceDisplayTick(int x, int y, int z, int meta) {
	RNGState* r = &indev_dispRng;
	float fx = (float)x + 0.5f;
	float fy = (float)y + Random_Float(r) * 6.0f / 16.0f;
	float fz = (float)z + 0.5f;
	float o  = Random_Float(r) * 0.6f - 0.3f;

	switch (meta) {
	case 4:  fx -= 0.52f; fz += o; break;
	case 5:  fx += 0.52f; fz += o; break;
	case 2:  fx += o; fz -= 0.52f; break;
	case 3:  fx += o; fz += 0.52f; break;
	default: return;
	}
	SurvivalTest_SpawnSmokeFX(fx, fy, fz, 1.0f);
	SurvivalTest_SpawnFlameFX(fx, fy, fz);
}

void IndevTest_RandomDisplayTicks(void) {
	struct LocalPlayer* p = Entities.CurPlayer;
	RNGState* r = &indev_dispRng;
	int i, px, py, pz, x, y, z;
	BlockID b;
	if (!IndevTest_Enabled || !World.Blocks || !p) return;

	if (!indev_dispRngInited) {
		Random_SeedFromCurrentTime(&indev_dispRng);
		indev_dispRngInited = true;
	}

	px = (int)p->Base.Position.x;
	py = (int)p->Base.Position.y;
	pz = (int)p->Base.Position.z;

	for (i = 0; i < 1000; i++) {
		x = px + Random_Next(r, 16) - Random_Next(r, 16);
		y = py + Random_Next(r, 16) - Random_Next(r, 16);
		z = pz + Random_Next(r, 16) - Random_Next(r, 16);
		if (!World_Contains(x, y, z)) continue;

		b = World_GetBlock(x, y, z);
		if (b == INDEV_BLOCK_FIRE) {
			Indev_FireDisplayTick(x, y, z);
		} else if (b == INDEV_BLOCK_TORCH) {
			Indev_TorchDisplayTick(x, y, z, 5);
		} else if (Indev_IsWallTorch(b)) {
			Indev_TorchDisplayTick(x, y, z, IndevTest_WallTorchMeta(b));
		} else if (Indev_IsFurnaceLit(b)) {
			Indev_FurnaceDisplayTick(x, y, z, IndevTest_BlockFacingMeta(b));
		} else if (b == BLOCK_LAVA || b == BLOCK_STILL_LAVA) {
			Indev_FluidDisplayTick(x, y, z, true);
		} else if (b == BLOCK_WATER || b == BLOCK_STILL_WATER) {
			Indev_FluidDisplayTick(x, y, z, false);
		}
	}
}

/* ItemHoe.onItemUse: turns grass (with non-solid above) or dirt into dry */
/*  farmland, wearing the tool; hoed GRASS has a 1-in-8 seed drop. */
/* ItemSeeds.onItemUse: plants stage-0 crops above farmland. */
cc_bool IndevTest_UseHeldItem(int heldId, IVec3 pos) {
	BlockID target, above;
	cc_bool solidAbove;
	if (!IndevTest_Enabled) return false;

	/* ItemFlintAndSteel.onItemUse: fire in the air cell on the clicked face */
	{
		const struct IndevItemDef* fs = IndevItems_Find(heldId);
		if (fs && fs->kind == ITEM_KIND_FLINTSTEEL) {
			return IndevFire_UseFlintSteel(pos,
				Game_SelectedPos.valid ? Game_SelectedPos.closest : FACE_YMAX);
		}
	}

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
						  cc_bool flipU) {
	/* the moon's quad mirrors U only (genuine: u=1 at -x, v=0 at -z) */
	float u0 = flipU ? 1.0f : 0.0f, u1 = 1.0f - u0;
	v[0].x = -size; v[0].y = y; v[0].z = -size; v[0].Col = PACKEDCOL_WHITE; v[0].U = u0; v[0].V = 0.0f;
	v[1].x =  size; v[1].y = y; v[1].z = -size; v[1].Col = PACKEDCOL_WHITE; v[1].U = u1; v[1].V = 0.0f;
	v[2].x =  size; v[2].y = y; v[2].z =  size; v[2].Col = PACKEDCOL_WHITE; v[2].U = u1; v[2].V = 1.0f;
	v[3].x = -size; v[3].y = y; v[3].z =  size; v[3].Col = PACKEDCOL_WHITE; v[3].U = u0; v[3].V = 1.0f;
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
	/* Depth WRITES off, but the depth TEST stays ON: entities rendered
	    earlier must still occlude the sun/moon/stars (disabling the test
	    made stars shine through mobs - user report). The engine sky
	    ceiling's own depth writes are suppressed in Indev mode instead
	    (EnvRenderer_RenderSky), matching genuine's glDepthMask(false) sky -
	    that is what un-hides the upper hemisphere. */
	Gfx_SetDepthWrite(false);
	Gfx_SetAlphaTest(false);
	Gfx_SetAlphaBlendingAdditive(true); /* dst + src, like glBlendFunc(ONE, ONE) */

	if (!indev_sunTexId || !indev_moonTexId) {
		/* sun.png/moon.png live in default.zip (packed there by the resource
		    fetcher) - a custom texture pack without them silently loses the
		    sky, so say it once instead of leaving users guessing */
		static cc_bool warned;
		if (!warned) {
			warned = true;
			Chat_AddRaw("&cIndev sky: sun.png/moon.png missing from the texture pack");
		}
	}

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
	if (SurvivalNet_ServerDriven()) return; /* MP: furnaces are server state (phase 4) */
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
	/* If either half of the OPEN container is destroyed, drop the whole view
	    to the discard slot (the surviving half keeps its own tile entity). */
	if (indev_openTE == i || indev_openTE2 == i) { indev_openTE = -1; indev_openTE2 = -1; }
	indev_tes[i].used = false;
}

/* World.isBlockNormalCube: a solid material rendered as a full opaque cube */
/*  (what torches may hang on / stand on). */
static cc_bool Indev_NormalCube(int x, int y, int z) {
	if (!World_Contains(x, y, z)) return false;
	return Blocks.FullOpaque[World_GetBlock(x, y, z)];
}

/* BlockTorch.onBlockAdded's auto wall-pick: first solid neighbour in the
    genuine -X, +X, -Z, +Z, floor order gives metadata 1/2/3/4/5 (0 = no
    support anywhere). */
static int Indev_TorchAutoMeta(IVec3 p) {
	if (Indev_NormalCube(p.x - 1, p.y, p.z)) return 1;
	if (Indev_NormalCube(p.x + 1, p.y, p.z)) return 2;
	if (Indev_NormalCube(p.x, p.y, p.z - 1)) return 3;
	if (Indev_NormalCube(p.x, p.y, p.z + 1)) return 4;
	if (Indev_NormalCube(p.x, p.y - 1, p.z)) return 5;
	return 0;
}

/* BlockTorch.onBlockPlaced: the clicked face picks the mounting when the
    block behind that face is solid (0 = fall back to the auto pick). */
static int Indev_TorchFaceMeta(IVec3 pos) {
	if (!Game_SelectedPos.valid) return 0;
	switch (Game_SelectedPos.closest) {
	case FACE_YMAX: return Indev_NormalCube(pos.x, pos.y - 1, pos.z) ? 5 : 0;
	case FACE_XMAX: return Indev_NormalCube(pos.x - 1, pos.y, pos.z) ? 1 : 0;
	case FACE_XMIN: return Indev_NormalCube(pos.x + 1, pos.y, pos.z) ? 2 : 0;
	case FACE_ZMAX: return Indev_NormalCube(pos.x, pos.y, pos.z - 1) ? 3 : 0;
	case FACE_ZMIN: return Indev_NormalCube(pos.x, pos.y, pos.z + 1) ? 4 : 0;
	default:        return 0;
	}
}

/* BlockTorch.onNeighborBlockChange for one position: a torch whose OWN
    support is no longer a normal cube pops off as a torch item. A wall
    torch only re-checks its wall (it never re-mounts elsewhere), a
    standing torch only its floor - both genuine. */
static void Indev_TorchCheckPop(int x, int y, int z) {
	BlockID b; int sx, sy, sz, meta;
	Vec3 p;
	if (!World_Contains(x, y, z)) return;
	b = World_GetBlock(x, y, z);
	if (b == INDEV_BLOCK_TORCH)    { meta = 5; }
	else if (Indev_IsWallTorch(b)) { meta = IndevTest_WallTorchMeta(b); }
	else return;

	sx = x; sy = y; sz = z;
	if      (meta == 1) { sx--; }
	else if (meta == 2) { sx++; }
	else if (meta == 3) { sz--; }
	else if (meta == 4) { sz++; }
	else                { sy--; }
	if (Indev_NormalCube(sx, sy, sz)) return;

	p.x = x + 0.5f; p.y = y + 0.5f; p.z = z + 0.5f;
	SurvivalTest_SpawnDropWorld(p, INDEV_BLOCK_TORCH, 1);
	Game_ChangeBlock(x, y, z, BLOCK_AIR);
}

/* World.setBlockWithNotify's notification fan-out, invoked from
    Game_UpdateBlock for EVERY block mutation - player edits, engine block
    physics (fluids, falling sand, sponges), fire spread, farm ticks and
    explosions alike. Genuine block code never mutates without notifying,
    which is what keeps torches, crops, farmland and tile entities
    consistent. Player-INTENT logic (chest/furnace facing, torch face-meta
    pick) stays in the UserEvents.BlockChanged handler below. */
void IndevTest_BlockUpdated(int x, int y, int z, BlockID oldBlock, BlockID block) {
	static int depth;
	IVec3 coords;
	BlockID below;
	if (!IndevTest_Enabled || !World.Loaded || !World.Blocks) return;
	/* validators mutate via Game_UpdateBlock themselves - cascades converge
	    in genuine (everything ends at air), the cap bounds pathologies */
	if (depth >= 8) return;
	depth++;
	coords.x = x; coords.y = y; coords.z = z;

	/* BlockFlower.onNeighborBlockChange -> canBlockStay: a crop pops (stage
	    7 drops 1 wheat) when its farmland turns into anything else */
	if (Indev_IsFarmland(oldBlock) && !Indev_IsFarmland(block) &&
		y + 1 < World.Height &&
		Indev_IsCrops(World_GetBlock(x, y + 1, z))) {
		Indev_PopCrop(x, y + 1, z, World_GetBlock(x, y + 1, z));
	}

	/* BlockFarmland.onNeighborBlockChange: a solid block landing on top
	    reverts the farmland below to dirt immediately (not on random tick) */
	if (Blocks.Collide[block] == COLLIDE_SOLID && y > 0) {
		below = World_GetBlock(x, y - 1, z);
		if (Indev_IsFarmland(below)) Game_UpdateBlock(x, y - 1, z, BLOCK_DIRT);
	}

	/* BlockContainer.onBlockRemoval (chest scatter / furnace TE cleanup).
	    Same-kind swaps - furnace lit<->unlit, chest/furnace facing rotation -
	    are metadata changes in genuine and keep the tile entity. */
	if (IndevTest_ContainerKindOf(oldBlock) != IndevTest_ContainerKindOf(block)) {
		IndevTest_NotifyBlockRemoved(coords, oldBlock);
	}

	/* Fire lifecycle: onBlockAdded validation/scheduling, age cleanup and
	    onNeighborBlockChange for the six neighbouring fire blocks */
	IndevFire_BlockChanged(coords, oldBlock, block);

	/* Any block change makes the six neighbouring torches re-check their
	    support (BlockTorch.onNeighborBlockChange) */
	Indev_TorchCheckPop(x - 1, y, z);
	Indev_TorchCheckPop(x + 1, y, z);
	Indev_TorchCheckPop(x, y - 1, z);
	Indev_TorchCheckPop(x, y + 1, z);
	Indev_TorchCheckPop(x, y, z - 1);
	Indev_TorchCheckPop(x, y, z + 1);
	depth--;
}

/* Player-intent placement handling, driven off the UserEvents.BlockChanged
    event (raised only for direct player actions). World-consistency
    validation lives in IndevTest_BlockUpdated above, which already ran
    inside Game_UpdateBlock for this same mutation. */
static void IndevTest_BlockChanged(void* obj, IVec3 coords, BlockID oldBlock, BlockID block) {
	struct Entity* p;
	int q, meta;
	if (!IndevTest_Enabled) return;

	/* Player placed a canonical chest/furnace: rotate it so the front faces */
	/*  the player (BlockFurnace.setDefaultDirection / Beta onBlockPlacedBy: */
	/*  quadrant of the placer's yaw picks metadata 2/5/3/4). */
	if (block == INDEV_BLOCK_CHEST || block == INDEV_BLOCK_FURNACE ||
		block == INDEV_BLOCK_FURNACE_LIT) { /* placed lit furnace (drop of a mined one) */
		p = &Entities.CurPlayer->Base;
		q = (int)Math_Floor(p->Yaw * 4.0f / 360.0f + 0.5f) & 3;
		/* ClassiCube's yaw is 180 degrees from Beta's convention (live-test */
		/*  showed fronts facing AWAY) - so the metadata picks are swapped */
		/*  north<->south / east<->west vs onBlockPlacedBy's 2/5/3/4. */
		meta = q == 0 ? 3 : (q == 1 ? 4 : (q == 2 ? 2 : 5));
		Game_UpdateBlock(coords.x, coords.y, coords.z,
			IndevTest_FacingVariant(block, meta)); /* same container kind - TE survives */
	}

	/* Player placed a torch: onBlockAdded's auto wall-pick, overridden by
	    onBlockPlaced's clicked-face mounting. Support is guaranteed here -
	    IndevTest_CanPlaceBlockAt refused the click otherwise (genuine
	    ItemBlock.onItemUse checks canPlaceBlockAt BEFORE placing). */
	if (block == INDEV_BLOCK_TORCH) {
		meta = Indev_TorchAutoMeta(coords);
		q    = Indev_TorchFaceMeta(coords);
		if (q) meta = q;
		if (meta && meta != 5) {
			Game_UpdateBlock(coords.x, coords.y, coords.z,
				(BlockID)(INDEV_BLOCK_TORCH_W1 + meta - 1));
		}
	}
}

/* World.groundLevel / waterLevel / defaultFluid - what genuine calls "the
    surroundings". Stored by the generator and the .mclevel loader, applied
    to the engine env planes on map load, written back on save. */
static int indev_surGround, indev_surWater, indev_surFluid;
static cc_bool indev_surKnown;

void IndevTest_SetSurroundings(int groundLevel, int waterLevel, int fluid) {
	indev_surGround = groundLevel;
	indev_surWater  = waterLevel;
	indev_surFluid  = fluid;
	indev_surKnown  = true;
}
int IndevTest_SurroundGroundLevel(void) { return indev_surKnown ? indev_surGround : Env_SidesHeight; }
int IndevTest_SurroundWaterLevel(void)  { return indev_surKnown ? indev_surWater  : Env.EdgeHeight; }
int IndevTest_SurroundFluid(void)       { return indev_surKnown ? indev_surFluid  : Env.EdgeBlock; }

/* RenderGlobal.oobGroundRenderer/oobWaterRenderer: genuine Indev draws NO
    border WALLS, but DOES draw infinite horizon planes outside the map -
    the ground plane at groundLevel (grass.png when the world is dry land
    with water as its fluid, dirt.png otherwise) and the default fluid
    plane at waterLevel. Mapped onto the engine's edge/sides machinery:
    - dry worlds (ground > water, e.g. Flat/Inland): edge plane = grass at
      groundLevel, no side walls (the fluid plane sits below the ground
      plane and is invisible from outside)
    - water worlds (Island): edge plane = the fluid at waterLevel, sides =
      dirt up to groundLevel (the submerged OOB ground plane - the engine
      wall skirt approximates it and is hidden underwater)
    - Floating (groundLevel -128): void all around */
static void Indev_ApplySurroundings(int groundLevel, int waterLevel, int fluid) {
	cc_bool waterFluid = fluid == BLOCK_WATER || fluid == BLOCK_STILL_WATER;

	if (groundLevel < 0) {
		Env_SetEdgeBlock(BLOCK_AIR);
		Env_SetSidesBlock(BLOCK_AIR);
		return;
	}
	if (groundLevel > waterLevel) {
		Env_SetEdgeBlock(waterFluid ? BLOCK_GRASS : BLOCK_DIRT);
		Env_SetEdgeHeight(groundLevel);
		Env_SetSidesBlock(BLOCK_AIR);
	} else {
		Env_SetEdgeBlock(waterFluid ? BLOCK_STILL_WATER : BLOCK_STILL_LAVA);
		Env_SetEdgeHeight(waterLevel);
		Env_SetSidesBlock(BLOCK_DIRT);
		Env_SetSidesOffset(groundLevel - waterLevel);
	}
}

/* Applies the stored surroundings to the env planes. Called on Indev map
    load, and directly by the generator so non-Indev-mode generations get
    the same genuine horizon (the map-load hook is Indev-gated). */
void IndevTest_ApplySurroundings(void) {
	if (!indev_surKnown) return;
	Indev_ApplySurroundings(indev_surGround, indev_surWater, indev_surFluid);
}

/* Map loading runs Game_Reset, which wipes ALL custom block definitions - */
/*  the Indev block ids survive in the map data but rendered as undefined */
/*  (the reported "green blocks"). Re-define them once the map is in. Also */
/*  snapshot the env colours as the day/night cycle's full-daylight base */
/*  (a loaded .mclevel has applied its Environment colours by now), and */
/*  switch to fancy lighting so torches/lit furnaces cast real light. */
static void IndevTest_MapActivate(void) {
	IndevBlocks_Define();
	Indev_RegisterFarmTicks(); /* in case physics re-registered its handlers */
	IndevFire_OnMapLoaded();   /* setTickOnLoad: schedule existing fire */
	IndevTest_FluidsOnMapLoaded(); /* setTickOnLoad: schedule moving fluid */

	/* Genuine getBlockId clamps y<0 to y=0, so floating maps (air at y=0) are
	    bottomless - you fall through into the void instead of landing on the
	    engine's default invisible bedrock floor. SurvivalTest kills you once
	    you cross well below the world. */
	World_FallThroughFloor = true;

	/* Genuine surroundings: no border walls, but the OOB ground/fluid
	    horizon planes (see Indev_ApplySurroundings). When the generator
	    didn't record levels, pin them from what the map loader left in the
	    engine env (mclevel: SidesHeight = SurroundingGroundHeight,
	    EdgeHeight = SurroundingWaterHeight, EdgeBlock = water type) BEFORE
	    applying - the apply mutates those env fields, and the .mclevel
	    saver reads the pinned values back. */
	if (!indev_surKnown) {
		IndevTest_SetSurroundings(Env_SidesHeight, Env.EdgeHeight, Env.EdgeBlock);
	}
	IndevTest_ApplySurroundings();

	indev_baseSky      = Env.SkyCol;
	indev_baseFog      = Env.FogCol;
	indev_baseClouds   = Env.CloudsCol;
	indev_baseColsKnown = true;
	indev_lastSkyLight  = -1; /* reapply sun/shadow for the new map */

	/* Level.initTransient: randId = random.nextInt() - without a per-map
	    seed every world would replay the same random-update pick sequence */
	{
		RNGState seedRng;
		Random_SeedFromCurrentTime(&seedRng);
		indev_randId = ((cc_uint32)Random_Next(&seedRng, 65536) << 16)
		             |  (cc_uint32)Random_Next(&seedRng, 65536);
	}

	if (Lighting_Mode != LIGHTING_MODE_FANCY && !Lighting_ModeLockedByServer) {
		/* remember what the player had, so leaving the Indev map can put it
		    back (else every visit permanently flips their lighting option) */
		indev_prevLighting   = Lighting_Mode;
		indev_forcedLighting = true;
		Lighting_SetMode(LIGHTING_MODE_FANCY, false);
	}
}

static void OnNewMapLoaded(void) {
	/* Per-map re-derive: in MP the mode is server-dictated (OFF until - unless -
	    a SURV_HELLO follows the level), so Indev never self-activates on someone
	    else's server no matter what the local options say. */
	IndevTest_Enabled = SurvivalTest_EffectiveGamemode() == SURVIVAL_GAMEMODE_INDEV;
	if (!IndevTest_Enabled) return;
	IndevTest_MapActivate();
}

/* The MP mode flip (SURV_HELLO lands after the level, so OnNewMapLoaded already
    ran with mode OFF). Called via SurvivalTest_NetworkModeChanged - Indev first,
    matching the component ordering the survival core relies on. */
void IndevTest_NetworkModeChanged(void) {
	IndevTest_Enabled = SurvivalTest_EffectiveGamemode() == SURVIVAL_GAMEMODE_INDEV;
	if (IndevTest_Enabled) {
		IndevTest_MapActivate();
	} else {
		/* Flipped off mid-map (mode-0 HELLO from a live /Survival change):
		    gameplay stops at once; the custom block DEFINITIONS linger visually
		    until the next map load wipes them (Game_Reset) - v1 limitation.
		    The day/night presentation does NOT linger: colours return to the
		    full-day base and the lighting resets (same cleanup as OnNewMap). */
		World_FallThroughFloor = false;
		if (indev_baseColsKnown) {
			Env_SetSkyCol(indev_baseSky);
			Env_SetFogCol(indev_baseFog);
			Env_SetCloudsCol(indev_baseClouds);
			Env_SetSunCol(ENV_DEFAULT_SUN_COLOR);
			Env_SetShadowCol(ENV_DEFAULT_SHADOW_COLOR);
		}
		FancyLighting_SetIndevSky(15);
		indev_baseColsKnown = false;
		if (indev_forcedLighting) {
			indev_forcedLighting = false;
			if (Lighting_Mode == LIGHTING_MODE_FANCY && !Lighting_ModeLockedByServer) {
				Lighting_SetMode(indev_prevLighting, false);
			}
		}
	}
}

static void OnInit(void) {
	/* Derived from the single authoritative gamemode value - the conflicting */
	/*  "both modes set" state is unrepresentable there, and this works */
	/*  regardless of component init order. Effective mode: OFF in MP until a
	    server SURV_HELLO flips it at runtime. */
	IndevTest_Enabled = SurvivalTest_EffectiveGamemode() == SURVIVAL_GAMEMODE_INDEV;

	/* Registered unconditionally so a survival server can flip Indev ON at
	    runtime via SURV_HELLO: the events/tick self-guard on IndevTest_Enabled,
	    the texture entries are inert without their PNGs being drawn, and the
	    item/armor tables are pure data. What stays mode-gated is anything
	    classic-visible: the block table (IndevBlocks_Define) and the farm
	    physics handlers (Indev_RegisterFarmTicks) only exist while Indev is on. */
	IndevItems_Seed();
	IndevArmor_Register();
	TextureEntry_Register(&items_entry);
	TextureEntry_Register(&kz_entry);
	TextureEntry_Register(&invgui_entry);
	TextureEntry_Register(&craftgui_entry);
	TextureEntry_Register(&furngui_entry);
	TextureEntry_Register(&contgui_entry);
	TextureEntry_Register(&sun_entry);
	TextureEntry_Register(&moon_entry);
	Random_Seed(&indev_teRng, (int)Game.Time + 1);
	Event_Register_(&UserEvents.BlockChanged, NULL, IndevTest_BlockChanged);
	Event_Register_(&GfxEvents.ContextLost,   NULL, IndevTest_ContextLost);
	ScheduledTask_Add(GAME_DEF_TICKS, IndevTest_Tick);

	if (!IndevTest_Enabled) return;
	IndevBlocks_Define();
	Indev_RegisterFarmTicks();
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
	/* our blocks live AT the genuine ids now - 50 torch, 51 fire, 54 chest,
	    56 diamond ore, 58 workbench, 61/62 furnace save as-is. The leftover
	    CPE decoration ids (unreachable in Indev mode, but old worlds might
	    carry them) keep lossy-but-sensible fallbacks. */
	/* our blocks now sit AT the genuine ids: 52 waterSource, 53 lavaSource,
	    55 gears, 57 diamond block all save 1:1 alongside the rest. */
	case 50: case 51: case 52: case 53: case 54: case 55: case 56: case 57:
	case 58: case 61: case 62:
		return b;
	case 59: return 28; /* turquoise wool (unused) -> clothCapri */
	case 60: return 20; /* ice (unused)            -> glass */
	case 63: return 1;  /* pillar       -> stone */
	case 64: return 54; /* crate        -> chest */
	case 65: return 1;  /* stone brick  -> stone */
	default:
		if (b <= 49) return b; /* classic identity */
		/* directional variants: same Indev block, facing carried by the */
		/*  Data array metadata nibble (IndevTest_BlockFacingMeta) */
		if (Indev_IsChestBlock(b))   return 54;
		if (Indev_IsFurnaceIdle(b))  return 61;
		if (Indev_IsFurnaceLit(b))   return 62;
		if (Indev_IsWallTorch(b))    return 50; /* meta 1-4 via the Data nibble */
		if (Indev_IsFarmland(b))     return 60; /* moisture in the Data nibble */
		if (Indev_IsCrops(b))        return 59; /* stage in the Data nibble */
		return 1; /* anything else -> stone */
	}
}

BlockRaw IndevTest_BlockFromIndev(BlockRaw b) {
	switch (b) {
	/* torch/fire/sources/chest/gears/diamond ore+block/workbench/furnaces load
	    1:1 - our definitions all sit at the genuine ids */
	case 50: case 51: case 52: case 53: case 54: case 55: case 56: case 57:
	case 58: case 61: case 62:
		return b;
	case 59: return INDEV_BLOCK_CROPS_0;  /* + stage from the Data nibble */
	case 60: return INDEV_BLOCK_FARMLAND; /* wet variant from the Data nibble */
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
	IndevTest_CloseContainer(); /* local open refs AND the MP net container view */
	indev_surKnown = false; /* each map records its own surroundings */

	/* Every genuine level is a brand-new World object: worldTime starts 0
	    and skyBrightness 15 (World.java field defaults), with the loader's
	    Environment tags overwriting them during load (a missing TimeOfDay
	    tag reads as 0 there too). These are statics here, so without this
	    the previous world's clock leaked into newly generated worlds. */
	indev_worldTime    = 0;
	indev_skyBright    = 15;
	indev_lastSkyLight = -1;

	/* Day/night presentation must not leak into the NEXT map (user report:
	    leaving a night-time Indev server map for a plain map kept the world
	    rendering at night while the env colours reset - a mismatched look):
	    - the fancy-lighting eased sky level is a static in FancyLighting.c,
	      stuck at the old map's value (4 at night) until the next tick - and
	      on non-Indev maps that tick never comes, so reset it to full day;
	    - the base-colour snapshot belongs to the old map (MapActivate
	      re-snapshots when an Indev map comes around again);
	    - the lighting mode we force-flipped to FANCY goes back to whatever
	      the player had, unless the server owns the mode or the player
	      changed it themselves mid-map. */
	FancyLighting_SetIndevSky(15);
	indev_baseColsKnown = false;
	if (indev_forcedLighting) {
		indev_forcedLighting = false;
		if (Lighting_Mode == LIGHTING_MODE_FANCY && !Lighting_ModeLockedByServer) {
			Lighting_SetMode(indev_prevLighting, false);
		}
	}

	IndevFire_Reset();
}

struct IGameComponent IndevTest_Component = {
	OnInit,   /* Init  */
	NULL,     /* Free  */
	OnNewMap, /* Reset (reconnect) - same invalidation applies */
	OnNewMap, /* OnNewMap */
	OnNewMapLoaded /* re-define the Indev blocks Game_Reset wiped */
};
