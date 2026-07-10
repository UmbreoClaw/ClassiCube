#ifndef CC_INDEVFIRE_H
#define CC_INDEVFIRE_H
#include "Core.h"
#include "BlockID.h"
#include "Vectors.h"
CC_BEGIN_HEADER

/* BlockFire (in-20100223) - the fire block, its scheduled-update spread
   simulation, and the flint & steel that starts it. Everything is a no-op
   while Indev mode is off, so c0.30/creative are unaffected.
   Copyright 2014-2025 ClassiCube | Licensed under BSD-3
*/

/* The fire block id in our custom-id space (genuine id 51). */
#define INDEV_BLOCK_FIRE 98
/* The two animated flame tiles (genuine terrain tiles 31 and 31+16, both
    driven by their own TextureFlamesFX instance - see Animations.c). */
#define INDEV_FIRE_TEX_LOC2 120

cc_bool IndevFire_IsFire(BlockID b);
/* BlockFire.chanceToEncourageFire[b] > 0 - whether fire can spread to/from */
/*  this block. Used by spread, placement validity and the renderer. */
cc_bool IndevFire_CanCatch(BlockID b);

/* Block definition (fire's engine block properties) - called from the */
/*  Indev block-additions init in IndevTest.c. */
void IndevFire_DefineBlock(void);
/* setTickOnLoad: scans a freshly loaded/generated map and schedules an */
/*  update for every existing fire block; also (re)allocates the age store. */
void IndevFire_OnMapLoaded(void);
/* Frees the per-map fire age store. */
void IndevFire_Reset(void);
/* One 20Hz tick of the scheduled-update queue (World.tick's tickList: */
/*  at most 200 entries processed, tickRate 20 delay per entry). */
void IndevFire_Tick(void);
/* World.tick's random updateTick for a fire block (ages + spreads it). */
void IndevFire_RandomTick(int index);
/* Block-change reactions: onBlockAdded validation/scheduling for newly */
/*  placed fire, age cleanup for removed fire, and onNeighborBlockChange */
/*  re-validation of fire blocks adjacent to the change. */
void IndevFire_BlockChanged(IVec3 coords, BlockID oldBlock, BlockID newBlock);
/* ItemFlintAndSteel.onItemUse: places fire in the air cell on the clicked */
/*  face (interior cells only), wearing the item. True = click consumed. */
cc_bool IndevFire_UseFlintSteel(IVec3 clickedPos, Face face);
/* BlockFlowing/BlockFluid's lava ignition: when lava tries to flow into a */
/*  flammable block, fire spreads around/into it instead. True = handled */
/*  (the lava must not flow this tick). */
cc_bool IndevFire_LavaFlowInto(int x, int y, int z);

/* Fire age metadata 0-15 (the genuine Data nibble), for .mclevel round-trips. */
int  IndevFire_Age(int index);
void IndevFire_SetAge(int index, int age);

CC_END_HEADER
#endif
