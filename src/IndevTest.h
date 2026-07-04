#ifndef CC_INDEVTEST_H
#define CC_INDEVTEST_H
#include "Core.h"
#include "Graphics.h"
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
/* Atlas UVs of an item id's sprite (items.png, 16x16 grid). False when the */
/*  id isn't a known Indev item - callers skip drawing then. */
cc_bool IndevTest_ItemSpriteUV(int id, float* u1, float* v1, float* u2, float* v2);

CC_END_HEADER
#endif
