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

/* The three mutually-exclusive gamemodes, stored as ONE option value so the
    conflicting "both survival and indev set" state is unrepresentable (that
    exact state caused the Indev-drops-in-survival and dead-Indev-button bugs
    when the modes were two independent booleans). */
enum SurvivalGamemode {
	SURVIVAL_GAMEMODE_OFF   = 0, /* plain creative ClassiCube */
	SURVIVAL_GAMEMODE_C030  = 1, /* faithful c0.30 Survival Test */
	SURVIVAL_GAMEMODE_INDEV = 2  /* Indev (in-20100223) layer over the core */
};
/* Resolves the current gamemode from OPT_SURVIVAL_GAMEMODE, falling back to
    the legacy survival-mode/indev-mode booleans (survival wins a tie) when
    the new key is absent. Reads options each call - order-independent, safe
    from any component's Init regardless of init sequence. */
int SurvivalTest_Gamemode(void);

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
/* Crafting grid, addressed as extended slots 36..44. Up to 3x3 (workbench); */
/*  the pocket inventory uses the top 2x2, a placed workbench all 9. Which is */
/*  active is SurvivalTest_CraftDim (2 or 3). */
#define SURVIVAL_CRAFT_SLOTS  9
#define SURVIVAL_CRAFT_BASE   SURVIVAL_INV_SLOTS
/* Open-container (chest/furnace tile entity) slots, addressed as extended */
/*  slots 45..71 (chest all 27; furnace uses 0=input 1=fuel 2=output). */
#define SURVIVAL_CONTAINER_SLOTS 27
#define SURVIVAL_CONTAINER_BASE  (SURVIVAL_CRAFT_BASE + SURVIVAL_CRAFT_SLOTS)
/* Worn armor (InventoryPlayer.armorInventory), addressed as extended slots */
/*  72..75 in genuine array order: [0] boots, [1] legs, [2] chest, [3] helmet */
/*  (piece type = 3 - array index). Saved to .mclevel as Slot 100+index. */
#define SURVIVAL_ARMOR_SLOTS 4
#define SURVIVAL_ARMOR_BASE  (SURVIVAL_CONTAINER_BASE + SURVIVAL_CONTAINER_SLOTS)

/* One ItemStack: block id 0-255 / item id 256+ (shiftedIndex), count, and */
/*  accumulated damage (tool wear). Shared with the Indev tile entity store. */
struct SurvivalSlot { cc_uint16 id; cc_int16 count; cc_int16 damage; };

/* Applies damage to the player (respects invincibility frames). */
/* hurtDir (for the hurt camera tilt) is randomised, matching the original's */
/*  hurt(null, damage) call sites (environmental damage - fall/lava/etc). */
void SurvivalTest_Hurt(int damage);
/* Same as SurvivalTest_Hurt, but bearing the hurt camera tilt towards/away */
/*  from attackerPos, matching the original's hurt(Entity, damage) call sites */
/*  (melee/arrow hits, where the source is a specific entity). */
void SurvivalTest_HurtFrom(int damage, Vec3 attackerPos);
/* LevelGenerator's "Spawning.." phase - 1000 MobSpawner passes that
    populate a freshly generated Indev world. No-op outside Indev mode. */
void SurvivalTest_IndevInitialSpawn(void);
/* Restores health to the player (capped at SURVIVAL_MAX_HEALTH). */
void SurvivalTest_Heal(int amount);
/* GameOverScreen's Respawn: clears the inventory, restores health/arrows */
/*  (air only to 20 ticks - a genuine quirk), teleports back to the spawn */
/*  point and unfreezes the world. The score is kept. */
void SurvivalTest_Respawn(void);
/* Gets the player's current score (Player.getScore() - awarded on credited mob kills). */
int SurvivalTest_Score(void);
/* Remaining invulnerability window in ticks (Mob.invulnerableTime), and the */
/*  health snapshot when it opened - drives the HUD's flashing ghost hearts. */
int SurvivalTest_InvulnTicks(void);
int SurvivalTest_LastHealth(void);
/* FOV divisor for the death camera zoom (1.0 while alive). Applied by the */
/*  perspective camera's projection so the view slowly zooms in while dead. */
float SurvivalTest_DeathFovZoom(void);

/* Whether the player's head is underwater (Player.isUnderWater()), i.e. whether */
/*  the HUD should draw the depleting air bubble row. */
cc_bool SurvivalTest_HeadUnderwater(void);
/* Remaining air, on the genuine Player.airSupply 0..300 scale (300 = full). */
int SurvivalTest_AirSupply(void);

/* Gets the block held in the given inventory slot (0 to SURVIVAL_INV_SLOTS-1). */
BlockID SurvivalTest_SlotBlock(int slot);
/* Raw slot id spanning blocks (0..255) and items (256+) - pair with */
/*  IndevTest_ItemSpriteUV to draw item sprites where SlotBlock returns AIR. */
int SurvivalTest_SlotId(int slot);
/* Accumulated tool wear in the slot (ItemStack.itemDamage; 0 = pristine). */
int SurvivalTest_SlotDamage(int slot);
/* Gets how many blocks are stacked in the given inventory slot. */
int SurvivalTest_SlotCount(int slot);
/* Gets the stack count in the given hotbar slot (0 to SURVIVAL_HOTBAR_SLOTS-1). */
int SurvivalTest_HotbarCount(int slot);
/* A counter that increments whenever inventory contents change. */
/* Lets the HUD cheaply detect when it needs to redraw stack counts. */
int SurvivalTest_InvVersion(void);
/* Bumps the inventory version (for external mutators like the furnace tick). */
void SurvivalTest_InvChanged(void);
/* Worn armor piece in array slot 0..3 (0 boots .. 3 helmet). */
int SurvivalTest_ArmorId(int i);
int SurvivalTest_ArmorCount(int i);
int SurvivalTest_ArmorDamage(int i);
/* .mclevel load: restore one inventory slot / the saved player stats. */
/* Slots 100..103 restore the armor array (the genuine save numbering). */
void SurvivalTest_RestoreSlot(int slot, int id, int count, int damage);
void SurvivalTest_RestoreStats(int health, int score);
/* ItemBow.onItemRightClick: fires an arrow when holding the bow (Indev),
    consuming one arrow ITEM from the inventory. Returns whether the click
    was handled (i.e. the bow was held - even if no arrows were left). */
cc_bool SurvivalTest_TryUseBow(void);
/* ItemPainting.onItemUse: hangs a painting on the clicked wall face (side
    faces of interior blocks only), choosing a random art that fits. */
cc_bool SurvivalTest_TryPlacePainting(IVec3 wall, Face face);
/* .mclevel painting persistence: iterate live paintings / restore one. */
int  SurvivalTest_PaintingNext(int prev, IVec3* tile, int* dir, const char** motive, Vec3* pos);
void SurvivalTest_RestorePainting(int tileX, int tileY, int tileZ, int dir, const cc_string* motive);
/* Held-item helpers for the Indev layer (hoe wear, seed consumption). */
void SurvivalTest_DamageHeldItem(int amount);
void SurvivalTest_ConsumeHeld(void);
/* Spawns a physical item drop entity at an exact world position (chest scatter). */
void SurvivalTest_SpawnDropWorld(Vec3 pos, int id, int count);
/* Arms a TNT block's fuse (fire consuming TNT calls this). */
void SurvivalTest_IgniteTnt(IVec3 coords);
/* Whether the local player is alight (first-person flame overlay). */
cc_bool SurvivalTest_PlayerBurning(void);
/* .mclevel entity save/load: live mob + item drop iteration and respawn. */
int  SurvivalTest_MobNext(int prev, int* type, Vec3* pos, float* yaw, int* health);
int  SurvivalTest_DropNext(int prev, Vec3* pos, int* id, int* count);
void SurvivalTest_RestoreMob(int type, Vec3 pos, float yaw, int health);

/* Swaps two slots. Indices 0..SURVIVAL_INV_SLOTS-1 are the inventory, */
/*  SURVIVAL_CRAFT_BASE..+3 are the 2x2 crafting grid (no-op when disabled). */
void SurvivalTest_SwapSlots(int a, int b);

/* The cursor-held stack (what the mouse carries in the inventory screen). */
int SurvivalTest_CursorId(void);
int SurvivalTest_CursorCount(void);
/* GuiContainer click semantics on a slot (0-35 inventory, 36-39 craft grid): */
/*  empty cursor: left takes all / right takes half; same id: left merges / */
/*  right places one; otherwise the stacks swap. */
void SurvivalTest_SlotClick(int idx, cc_bool rightClick);
/* Takes from the crafting result: crafts once onto the cursor. */
void SurvivalTest_ResultClick(void);
/* Empties the cursor back into the inventory (call when the screen closes). */
void SurvivalTest_CursorReturn(void);

/* 2x2 crafting grid cell contents (i = 0..SURVIVAL_CRAFT_SLOTS-1). */
int SurvivalTest_CraftSlotId(int i);
int SurvivalTest_CraftSlotCount(int i);
/* Current crafted output for the grid: result id (0 = nothing craftable) and */
/*  its yield via outCount. Recomputed from the grid via the Indev recipe engine. */
int SurvivalTest_CraftResult(int* outCount);
/* Crafts once: yields the output into the inventory and consumes one of each */
/*  grid ingredient (SlotCrafting.onPickupFromSlot). No-op if nothing craftable. */
void SurvivalTest_CraftTake(void);
/* Returns all grid ingredients to the inventory (call on inventory close). */
void SurvivalTest_CraftReturnAll(void);
/* Current crafting grid dimension: 2 (pocket 2x2) or 3 (workbench 3x3). */
int  SurvivalTest_CraftDim(void);
/* Sets the crafting grid dimension for the next screen (returns any items */
/*  already on the grid first, so switching pocket<->workbench never strands */
/*  ingredients). */
void SurvivalTest_SetCraftDim(int dim);

/* Whether the player is allowed to place their currently selected block. */
/* Returns true (always allowed) when survival mode is disabled. */
cc_bool SurvivalTest_CanPlace(BlockID block);

/* Attempts to eat the currently selected hotbar item (mushrooms). */
/* Returns true if something was eaten, so block placement should be skipped. */
cc_bool SurvivalTest_TryEat(void);
/* Right-click use of the aimed block (Indev workbench opens the 3x3 grid). */
/*  Returns true if handled, so the caller skips placing a block. */
cc_bool SurvivalTest_TryUseBlock(void);

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

/* Q key (Indev): tosses one of the held stack out in front of the player. */
void SurvivalTest_TryDropHeld(void);
/* Debug: adds one of the given block-or-item id to the inventory. */
void SurvivalTest_DebugGiveItem(int id);

/* Gets how many arrows the player currently has (0 to 99). */
int SurvivalTest_ArrowCount(void);

/* Whether the given block should break the instant it's clicked, rather than */
/*  needing sustained mining (true for 0-hardness blocks, e.g. flowers, TNT). */
/*  Always true when survival mode is disabled. */
cc_bool SurvivalTest_CanInstaBreak(BlockID block);
/* Overrides a block's mining hardness (in hits; 0 = instant break). Defaults */
/*  are the faithful c0.30 values - this hook exists so custom blocks (CPE */
/*  BlockDefs) or a server plugin can supply their own hardness later. */
void SurvivalTest_SetHardness(BlockID block, int hardness);
/* Overrides an id's max stack size (c0.30 default: 99 for everything). The */
/*  Indev layer seeds Item.maxStackSize values (64 materials, 1 tools/armor); */
/*  a server plugin can override per id the same way. */
void SurvivalTest_SetMaxStack(int id, int maxStack);

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
/*  table (SurvivalTest_RenderMobs et al) is full. noAI freezes the mob in place; */
/*  forceArmor puts plate + helmet on zombies/skeletons (both /client spawn modifiers). */
void SurvivalTest_DebugSpawnMob(int type, cc_bool noAI, cc_bool forceArmor);
/* Instantly kills every currently active mob in the world (no death-score credit, */
/*  matching a debug/console kill rather than a real player kill). No-op when survival */
/*  mode is disabled. */
void SurvivalTest_DebugKillAllMobs(void);
/* Prints live mob count / world-size-scaled cap / per-tick spawn roll to chat. */
void SurvivalTest_DebugMobCensus(void);
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

/* Debug invincibility (blocks all player damage) - flipped by /client god, */
/*  which reports the new state from the getter. No-op when survival is off. */
cc_bool SurvivalTest_DebugGodMode(void);
void SurvivalTest_DebugToggleGodMode(void);

CC_END_HEADER
#endif
