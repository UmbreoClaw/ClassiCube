#ifndef CC_INDEVTEST_H
#define CC_INDEVTEST_H
#include "Core.h"
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

CC_END_HEADER
#endif
