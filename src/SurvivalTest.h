#ifndef CC_SURVIVALTEST_H
#define CC_SURVIVALTEST_H
#include "Core.h"
CC_BEGIN_HEADER

/* Classic 0.30 Survival Test gamemode.
   Copyright 2014-2025 ClassiCube | Licensed under BSD-3
*/
struct IGameComponent;
extern struct IGameComponent SurvivalTest_Component;

/* Whether survival test mode is currently active. */
/* NOTE: When false, every function here is a no-op and creative mode is */
/*  completely unaffected. This MUST be checked before any survival logic. */
extern cc_bool SurvivalTest_Enabled;

/* Player's current health points (0 to SURVIVAL_MAX_HEALTH). 0 = dead. */
extern int SurvivalTest_Health;
/* Maximum health points (10 hearts * 2 HP). */
#define SURVIVAL_MAX_HEALTH 20
/* Maximum number of a single block type the player can hold. */
#define SURVIVAL_STACK_MAX 99

/* Applies damage to the player (respects invincibility frames). */
void SurvivalTest_Hurt(int damage);
/* Restores health to the player (capped at SURVIVAL_MAX_HEALTH). */
void SurvivalTest_Heal(int amount);

/* Returns how many of the given block the player is currently holding. */
int SurvivalTest_BlockCount(BlockID block);
/* Whether the player is allowed to place the given block. */
/* Returns true (always allowed) when survival mode is disabled. */
cc_bool SurvivalTest_CanPlace(BlockID block);

CC_END_HEADER
#endif
