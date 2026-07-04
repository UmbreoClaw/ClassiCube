#include "IndevTest.h"
#include "Game.h"
#include "Options.h"
#include "Chat.h"
#include "SurvivalTest.h"
#include "Funcs.h"
#include "Graphics.h"
#include "TexturePack.h"

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

static void OnInit(void) {
	IndevTest_Enabled = Options_GetBool(OPT_INDEV_MODE, false);
	if (!IndevTest_Enabled) return;

	IndevItems_Seed();
	TextureEntry_Register(&items_entry);
	Chat_AddRaw("&eIndev mode: plumbing active (survival core + Indev layer WIP)");
}

struct IGameComponent IndevTest_Component = {
	OnInit /* Init */
};
