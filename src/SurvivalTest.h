#ifndef CC_SURVIVALTEST_H
#define CC_SURVIVALTEST_H
#include "Core.h"
#include "Vectors.h"
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

/* Whether the non-authentic "Enhanced" survival extras are enabled (off by */
/*  default). Classic mode stays faithful to c0.30-s; Enhanced adds decorative */
/*  niceties like the Indev/Beta-style paperdoll inventory screen. Faithful */
/*  c0.30-s mechanics are unaffected and apply in both modes. */
extern cc_bool SurvivalTest_Enhanced;

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
/* hurtDir (for the hurt camera tilt) is randomised, matching the original's */
/*  hurt(null, damage) call sites (environmental damage - fall/lava/etc). */
void SurvivalTest_Hurt(int damage);
/* Same as SurvivalTest_Hurt, but bearing the hurt camera tilt towards/away */
/*  from attackerPos, matching the original's hurt(Entity, damage) call sites */
/*  (melee/arrow hits, where the source is a specific entity). */
void SurvivalTest_HurtFrom(int damage, Vec3 attackerPos);
/* Restores health to the player (capped at SURVIVAL_MAX_HEALTH). */
void SurvivalTest_Heal(int amount);
/* Gets the player's current score (Player.getScore() - awarded on credited mob kills). */
int SurvivalTest_Score(void);

/* Whether the player's head is underwater (Player.isUnderWater()), i.e. whether */
/*  the HUD should draw the depleting air bubble row. */
cc_bool SurvivalTest_HeadUnderwater(void);
/* Remaining air, on the genuine Player.airSupply 0..300 scale (300 = full). */
int SurvivalTest_AirSupply(void);

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

/* Attempts to eat the currently selected hotbar item (mushrooms). */
/* Returns true if something was eaten, so block placement should be skipped. */
cc_bool SurvivalTest_TryEat(void);

/* Renders all physical dropped-item entities in the 3D world. */
/* No-op when survival mode is disabled. Call once per frame, alongside */
/*  Entities_RenderModels (e.g. in Render3DFrame). */
void SurvivalTest_RenderDrops(float delta, float t);

/* Renders all living/dying mobs in the 3D world. */
/* No-op when survival mode is disabled. Call once per frame, alongside */
/*  SurvivalTest_RenderDrops (e.g. in Render3DFrame). */
void SurvivalTest_RenderMobs(float delta, float t);

/* Attempts to melee-attack whichever mob the player is looking at, within */
/*  reach distance. Returns true if a mob was hit, so the caller can skip */
/*  its normal block-breaking action for that input (no-op, returns false */
/*  when survival mode is disabled). */
cc_bool SurvivalTest_TryAttackMob(void);

/* Renders all in-flight/stuck arrow entities in the 3D world. No-op when */
/*  survival mode is disabled. Call once per frame, alongside */
/*  SurvivalTest_RenderDrops/RenderMobs (e.g. in Render3DFrame). */
void SurvivalTest_RenderArrows(float delta, float t);

/* Renders the flashing glow overlay on every currently-fused (lit) TNT */
/*  block, which speeds up as its fuse nears zero. No-op when survival mode */
/*  is disabled. Call once per frame, alongside SurvivalTest_RenderDrops/ */
/*  RenderMobs/RenderArrows (e.g. in Render3DFrame). */
void SurvivalTest_RenderTnt(float delta, float t);

/* Fires an arrow from the player along their current look direction, */
/*  decrementing their arrow count (Tab key, matching Survival Test). */
/*  Returns false (and does nothing) if out of arrows or survival mode */
/*  is disabled. */
cc_bool SurvivalTest_TryShootArrow(void);

/* Gets how many arrows the player currently has (0 to 99). */
int SurvivalTest_ArrowCount(void);

/* Whether the given block should break the instant it's clicked, rather than */
/*  needing sustained mining (true for 0-hardness blocks, e.g. flowers, TNT). */
/*  Always true when survival mode is disabled. */
cc_bool SurvivalTest_CanInstaBreak(BlockID block);

/* Current mining progress (0-1) towards breaking whatever block is being */
/*  continuously mined, for the crack overlay. 0 if nothing is being mined. */
float SurvivalTest_BreakProgress(void);

/* Gets the coordinates of the block currently being continuously mined. */
/* Returns false (and leaves *pos untouched) if nothing is being mined. */
cc_bool SurvivalTest_BreakTargeted(IVec3* pos);

/* Renders the crack overlay on whatever block is currently being mined. */
/* No-op when survival mode is disabled. Call once per frame, after the */
/*  selection outline (e.g. in Render3DFrame). */
void SurvivalTest_RenderCracks(float delta, float t);

/* Applies the hurt camera-tilt roll (Renderer.hurtEffect) on top of the */
/*  already-built view matrix - a brief roll away from the hit direction */
/*  right after taking damage. No-op when survival is disabled or there's */
/*  nothing to apply, so safe to call unconditionally every frame, right */
/*  after the camera's view matrix is computed (e.g. in Render3DFrame). */
void SurvivalTest_ApplyHurtTilt(struct Matrix* view, float t);

/* ----------------------------------------- Debug/testing tools ------------------------------------------ */
/* Everything below exists purely to make manual testing of survival mode easier (spawning */
/*  mobs on demand, instant heal/kill, etc.) - none of it is part of the genuine c0.30-s */
/*  feature set, and it can all be ripped out later without affecting parity. */

/* Mob type constants for SurvivalTest_DebugSpawnMob - order matches the internal MobType enum. */
enum SurvivalDebugMobType {
	SURVIVAL_DEBUG_MOB_ZOMBIE, SURVIVAL_DEBUG_MOB_SKELETON, SURVIVAL_DEBUG_MOB_PIG,
	SURVIVAL_DEBUG_MOB_CREEPER, SURVIVAL_DEBUG_MOB_SPIDER, SURVIVAL_DEBUG_MOB_SHEEP,
	SURVIVAL_DEBUG_MOB_COUNT
};

/* Spawns a mob of the given SurvivalDebugMobType a few blocks in front of the player, */
/*  along their current look direction. No-op if survival is disabled or the mob slot */
/*  table (SurvivalTest_RenderMobs et al) is full. */
void SurvivalTest_DebugSpawnMob(int type);
/* Instantly kills every currently active mob in the world (no death-score credit, */
/*  matching a debug/console kill rather than a real player kill). No-op when survival */
/*  mode is disabled. */
void SurvivalTest_DebugKillAllMobs(void);
/* Sets the player's arrow count directly (clamped 0-99). No-op when survival mode is disabled. */
void SurvivalTest_DebugSetArrows(int count);

/* Spawns a small spread of dropped-item entities a couple of blocks in front of the */
/*  player, to test drop physics/rendering/pickup. No-op when survival is disabled. */
void SurvivalTest_DebugSpawnDrops(void);
/* Ignites a primed TNT entity (full fuse) a couple of blocks in front of the player. */
/*  No-op when survival is disabled. */
void SurvivalTest_DebugSpawnTnt(void);
/* Fires a player-type arrow along the look direction WITHOUT spending an arrow from */
/*  the count, so it can be spammed while testing. No-op when survival is disabled. */
void SurvivalTest_DebugShootArrow(void);

/* Debug spawn-affecting toggles + invincibility. The Toggle* fns flip the flag; the */
/*  plain getters report current state (used to label the F9 menu buttons). God mode */
/*  blocks all player damage; NoAI/ForceArmor only affect mobs spawned via the debug */
/*  menu afterwards (never natural spawns). All no-op/false when survival is disabled. */
cc_bool SurvivalTest_DebugGodMode(void);
cc_bool SurvivalTest_DebugNoAI(void);
cc_bool SurvivalTest_DebugForceArmor(void);
void SurvivalTest_DebugToggleGodMode(void);
void SurvivalTest_DebugToggleNoAI(void);
void SurvivalTest_DebugToggleForceArmor(void);

CC_END_HEADER
#endif
