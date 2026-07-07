#ifndef CC_INDEVTEST_H
#define CC_INDEVTEST_H
#include "Core.h"
#include "Graphics.h"
#include "BlockID.h"
CC_BEGIN_HEADER

/* Indev (in-20100223) gamemode - version layer on top of the survival core.
   Copyright 2014-2025 ClassiCube | Licensed under BSD-3
*/
struct IGameComponent;
extern struct IGameComponent IndevTest_Component;

/* Whether Indev mode is active. Implies the survival core (SurvivalTest) is */
/*  active too - Indev is a version layer over it, not a separate system. */
/*  When false, every Indev hook is a no-op and both creative mode and the */
/*  faithful c0.30-s survival mode are completely unaffected. */
extern cc_bool IndevTest_Enabled;

/* The items.png atlas texture (0 until a texture pack supplies it). Item */
/*  sprite rendering (hotbar/hand/drops) bails gracefully while it's 0. */
GfxResourceID IndevTest_ItemsTex(void);
/* The genuine gui/inventory.png (0 until loaded) - the Indev inventory */
/*  screen draws its whole panel from this 176x166 region. */
GfxResourceID IndevTest_InvGuiTex(void);
/* The workbench gui/crafting.png (0 until loaded) - the 3x3 crafting screen. */
GfxResourceID IndevTest_CraftGuiTex(void);
/* Whether the block is the Indev workbench (right-click opens the 3x3 grid). */
cc_bool IndevTest_IsWorkbench(BlockID b);
/* Atlas UVs of an item id's sprite (items.png, 16x16 grid). False when the */
/*  id isn't a known Indev item - callers skip drawing then. */
cc_bool IndevTest_ItemSpriteUV(int id, float* u1, float* v1, float* u2, float* v2);
/* Heal amount when id is an edible Indev item (apple/soup/bread/porkchops), */
/*  else 0. Always 0 while Indev mode is off. */
int IndevTest_ItemFoodHeal(int id);
/* ItemTool.maxDamage (32 << tier) for damageable tools, else 0. */
int IndevTest_ToolMaxDamage(int id);
/* Mining speed multiplier of the held id against a block ((tier+1)*2 when */
/*  the tool class is effective vs the block's material, else 1). */
int IndevTest_MiningSpeed(int id, BlockID block);
/* Melee damage of the held id (bare fist / non-weapons = 1; tools base+tier; */
/*  swords 4 + tier*2), per Minecraft.java's attack path. */
int IndevTest_MeleeDamage(int id);
/* Whether a block dropped when broken by hand/held item - false for rock/iron */
/*  material (stone-sound) blocks unless a pickaxe of sufficient level is held. */
cc_bool IndevTest_CanHarvest(int heldId, BlockID block);
/* Matches a gw*gh crafting grid of full-space ids (0 = empty) against the */
/*  in-20100223 recipe list. True + result id/count when a recipe fits. */
cc_bool IndevTest_MatchRecipe(const cc_uint16* grid, int gw, int gh, int* outId, int* outCount);

CC_END_HEADER
#endif
