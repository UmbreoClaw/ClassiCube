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
/* Maximum number of a single block type a single inventory slot can hold. */
#define SURVIVAL_STACK_MAX 99

/* Total inventory slots (4 rows of 9, bottom row is the hotbar). */
#define SURVIVAL_INV_SLOTS    36
/* Number of inventory slots that make up the hotbar. */
#define SURVIVAL_HOTBAR_SLOTS 9

/* Applies damage to the player (respects invincibility frames). */
void SurvivalTest_Hurt(int damage);
/* Restores health to the player (capped at SURVIVAL_MAX_HEALTH). */
void SurvivalTest_Heal(int amount);

/* Gets the block held in the given inventory slot (0 to SURVIVAL_INV_SLOTS-1). */
BlockID SurvivalTest_SlotBlock(int slot);
/* Gets how many blocks are stacked in the given inventory slot. */
int SurvivalTest_SlotCount(int slot);
/* Gets the stack count in the given hotbar slot (0 to SURVIVAL_HOTBAR_SLOTS-1). */
int SurvivalTest_HotbarCount(int slot);
/* A counter that increments whenever inventory contents change. */
/* Lets the HUD cheaply detect when it needs to redraw stack counts. */
int SurvivalTest_InvVersion(void);

/* Swaps the contents of two inventory slots (no-op when survival is disabled). */
void SurvivalTest_SwapSlots(int a, int b);

/* Whether the player is allowed to place their currently selected block. */
/* Returns true (always allowed) when survival mode is disabled. */
cc_bool SurvivalTest_CanPlace(BlockID block);

CC_END_HEADER
#endif
