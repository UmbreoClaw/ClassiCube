#include "IndevTest.h"
#include "Game.h"
#include "Options.h"
#include "Chat.h"
#include "SurvivalTest.h"
#include "Funcs.h"

/* Indev (in-20100223) gamemode - mode plumbing only so far.
   Ground truth: the deobfuscated EaglerPorts/in-20100223 tree (see
   SURVIVAL_TEST_NOTES.md). Design intent: SurvivalTest.c is the shared
   survival core (entities, combat, drops, HUD); this file grows the
   Indev-specific layer (ItemStacks, tools, crafting, day/night, ...) on
   top, with per-block/per-mob data kept in runtime tables so a server
   (e.g. an MCGalaxy plugin over a CPE channel) can override them later. */

cc_bool IndevTest_Enabled;

/* Item.java's static init: item ids are shifted +256 (shiftedIndex), default
    maxStackSize 64, and every ItemTool/ItemSword/ItemBow/ItemArmor subclass
    sets maxStackSize = 1. Seeds the survival core's per-id stack table with
    the ids confirmed so far from the in-20100223 tree; the roster is
    completed alongside the item definitions table (ItemStack step 2b). */
static void IndevItems_Seed(void) {
	/* Confirmed single-stack ids (local id, before the +256 shift): steel
	    shovel/pick/axe 0-2, flint&steel 3, bow 5, steel/wood swords 11-12,
	    wood tools 13-15, stone sword/tools 16-19, diamond sword/tools 20-23,
	    gold sword/tools 27-30ish - completed against Item.java as the
	    definitions table lands. */
	static const cc_uint8 single[] = { 0,1,2,3,5, 11,12,13,14,15,16,17,18,19,20,21,22,23, 27,28,29,30 };
	int i;

	for (i = 256; i < 1024; i++) SurvivalTest_SetMaxStack(i, 64);
	for (i = 0; i < (int)Array_Elems(single); i++) {
		SurvivalTest_SetMaxStack(256 + single[i], 1);
	}
}

static void OnInit(void) {
	IndevTest_Enabled = Options_GetBool(OPT_INDEV_MODE, false);
	if (!IndevTest_Enabled) return;

	IndevItems_Seed();
	Chat_AddRaw("&eIndev mode: plumbing active (survival core + Indev layer WIP)");
}

struct IGameComponent IndevTest_Component = {
	OnInit /* Init */
};
