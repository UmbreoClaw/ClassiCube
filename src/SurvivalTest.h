#ifndef CC_SURVIVALTEST_H
#define CC_SURVIVALTEST_H
#include "Core.h"
CC_BEGIN_HEADER

/* Classic 0.30 Survival Test gamemode.
   Copyright 2014-2025 ClassiCube | Licensed under BSD-3
*/
struct IGameComponent;
extern struct IGameComponent SurvivalTest_Component;

/* Whether survival test mode is currently active */
extern cc_bool SurvivalTest_Enabled;
/* Player's current health points (0 to SURVIVAL_MAX_HEALTH). 0 = dead. */
extern int SurvivalTest_Health;
/* Maximum health points */
#define SURVIVAL_MAX_HEALTH 20
/* Remaining air supply in seconds (0 = drowning) */
extern float SurvivalTest_AirTimer;

/* Applies damage to the player (respects invincibility frames) */
void SurvivalTest_Hurt(int damage);
/* Restores health to the player (capped at SURVIVAL_MAX_HEALTH) */
void SurvivalTest_Heal(int amount);

CC_END_HEADER
#endif
