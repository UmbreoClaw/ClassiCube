#ifndef CC_INDEVGEN_H
#define CC_INDEVGEN_H
#include "Core.h"
#include "Generator.h"
CC_BEGIN_HEADER

/* Indev (in-20100223) level generator - a port of
   net/minecraft/game/level/generator/LevelGenerator.java and its noise
   stack (NoiseGeneratorPerlin/Octaves/Distort), producing the genuine
   island/inland/floating/flat terrain with themes, caves, ore veins,
   springs, edge flooding, beaches, trees and the spawn house.
   Copyright 2014-2025 ClassiCube | Licensed under BSD-3
*/

/* The generator itself - pass to Gen_Start like FlatgrassGen/NotchyGen.
   Dimensions come from Gen_Start; call IndevGen_Setup first to pick the
   world type and theme. */
extern const struct MapGenerator IndevGen;

/* World type: 0 = Inland, 1 = Island, 2 = Floating, 3 = Flat.
   Theme: 0 = Normal, 1 = Hell, 2 = Paradise, 3 = Woods. */
void IndevGen_Setup(int type, int theme);

/* After World_SetNewMap: applies the generated spawn point (house
   location), theme environment (colours, sky brightness, edge fluid and
   heights) and the initial 1000-pass mob population. Returns false when
   the last generation wasn't IndevGen (caller keeps the default spawn). */
struct LocationUpdate;
cc_bool IndevGen_ApplyPostLoad(struct LocationUpdate* update);

CC_END_HEADER
#endif
