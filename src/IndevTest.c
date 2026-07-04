#include "IndevTest.h"
#include "Game.h"
#include "Options.h"
#include "Chat.h"

/* Indev (in-20100223) gamemode - mode plumbing only so far.
   Ground truth: the deobfuscated EaglerPorts/in-20100223 tree (see
   SURVIVAL_TEST_NOTES.md). Design intent: SurvivalTest.c is the shared
   survival core (entities, combat, drops, HUD); this file grows the
   Indev-specific layer (ItemStacks, tools, crafting, day/night, ...) on
   top, with per-block/per-mob data kept in runtime tables so a server
   (e.g. an MCGalaxy plugin over a CPE channel) can override them later. */

cc_bool IndevTest_Enabled;

static void OnInit(void) {
	IndevTest_Enabled = Options_GetBool(OPT_INDEV_MODE, false);
	if (!IndevTest_Enabled) return;

	Chat_AddRaw("&eIndev mode: plumbing active (survival core + Indev layer WIP)");
}

struct IGameComponent IndevTest_Component = {
	OnInit /* Init */
};
