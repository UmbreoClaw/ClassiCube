#ifndef CC_INDEVARMOR_H
#define CC_INDEVARMOR_H
#include "Core.h"
CC_BEGIN_HEADER

/* Renders the player's worn armor over a humanoid entity, exactly like
   in-20100223 RenderPlayer's four armor passes: helmet = head + headwear,
   chest = body + arms, legs = body + legs on the _2 texture at 0.5
   inflation, boots = legs at 1.0 inflation. No-op outside Indev mode.
   Copyright 2014-2025 ClassiCube | Licensed under BSD-3
*/
struct Entity;

/* Registers the overlay models + their 10 armor textures. */
void IndevArmor_Register(void);
/* Draws the local player's worn armor onto the given humanoid entity
   (the third-person player, or the inventory paperdoll). */
void IndevArmor_Render(struct Entity* e);

CC_END_HEADER
#endif
