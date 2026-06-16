#include "SurvivalTest.h"
#include "Entity.h"
#include "World.h"
#include "Game.h"
#include "Event.h"
#include "Inventory.h"
#include "Chat.h"
#include "Block.h"
#include "BlockID.h"
#include "Constants.h"
#include "Options.h"
#include "Vectors.h"
#include "ExtMath.h"

/* Classic 0.30 Survival Test gamemode implementation.
   Copyright 2014-2025 ClassiCube | Licensed under BSD-3
*/

cc_bool SurvivalTest_Enabled;
int     SurvivalTest_Health = SURVIVAL_MAX_HEALTH;

/* How long (seconds) the player is invincible after taking damage */
#define INVINCIBILITY_SECS 0.5f
/* Lava damages this often (seconds) while the player is touching it */
#define LAVA_DMG_INTERVAL  0.5f
/* Damage dealt per lava damage tick */
#define LAVA_DAMAGE        4
/* Drowning damages this often (seconds) once air is depleted */
#define DROWN_DMG_INTERVAL 1.0f
/* Damage dealt per drowning tick (2 HP/sec, matching Survival Test) */
#define DROWN_DAMAGE       2
/* Starting air supply in seconds (15s before drowning, as in Survival Test) */
#define AIR_SUPPLY_SECS    15.0f
/* Falls of more than this many blocks deal damage (~1 HP per excess block) */
#define FALL_SAFE_BLOCKS   3.0f
/* Seconds to wait before respawning after death */
#define RESPAWN_DELAY_SECS 2.5f

static float st_airTimer;
static float st_invincTimer;
static float st_lavaTimer;
static float st_drownTimer;
static float st_fallDistance;
static float st_prevY;
static cc_bool st_prevYValid;
static cc_bool st_wasOnGround;
static cc_bool st_isDead;
static float st_respawnTimer;

/* Per-block-type counts of what the player is currently holding */
static cc_uint8 st_counts[BLOCK_COUNT];


/*########################################################################################################################*
*----------------------------------------------------Health & damage------------------------------------------------------*
*#########################################################################################################################*/
void SurvivalTest_Hurt(int damage) {
	if (!SurvivalTest_Enabled) return;
	if (st_isDead)             return;
	if (st_invincTimer > 0.0f) return;
	if (damage <= 0)           return;

	SurvivalTest_Health -= damage;
	st_invincTimer = INVINCIBILITY_SECS;

	if (SurvivalTest_Health <= 0) {
		SurvivalTest_Health = 0;
		st_isDead       = true;
		st_respawnTimer = RESPAWN_DELAY_SECS;
		Chat_AddRaw("&cYou died! Respawning...");
	}
}

void SurvivalTest_Heal(int amount) {
	if (!SurvivalTest_Enabled) return;
	if (st_isDead)             return;

	SurvivalTest_Health += amount;
	if (SurvivalTest_Health > SURVIVAL_MAX_HEALTH)
		SurvivalTest_Health = SURVIVAL_MAX_HEALTH;
}


/*########################################################################################################################*
*------------------------------------------------------Inventory----------------------------------------------------------*
*#########################################################################################################################*/
/* Maps a mined block to the block that drops from it (Survival Test rules). */
static BlockID SurvivalTest_DropFor(BlockID block) {
	switch (block) {
	case BLOCK_STONE:    return BLOCK_COBBLE;  /* stone yields cobblestone */
	case BLOCK_GRASS:    return BLOCK_DIRT;    /* grass yields dirt */
	case BLOCK_LEAVES:   return BLOCK_SAPLING; /* leaves yield saplings */
	case BLOCK_GOLD_ORE: return BLOCK_GOLD;    /* ore yields processed block */
	case BLOCK_IRON_ORE: return BLOCK_IRON;
	default:             return block;
	}
}

int SurvivalTest_BlockCount(BlockID block) {
	return st_counts[block];
}

cc_bool SurvivalTest_CanPlace(BlockID block) {
	if (!SurvivalTest_Enabled) return true;
	return st_counts[block] > 0;
}

/* Adds one of the given block to the player's holdings, placing it in an */
/*  empty hotbar slot if it isn't already present. */
static void SurvivalTest_AddBlock(BlockID block) {
	int i;
	if (block == BLOCK_AIR) return;

	if (st_counts[block] < SURVIVAL_STACK_MAX) st_counts[block]++;

	/* Already shown in the hotbar? */
	for (i = 0; i < INVENTORY_BLOCKS_PER_HOTBAR; i++) {
		if (Inventory_Get(i) == block) return;
	}
	/* Otherwise drop it into the first empty hotbar slot */
	for (i = 0; i < INVENTORY_BLOCKS_PER_HOTBAR; i++) {
		if (Inventory_Get(i) != BLOCK_AIR) continue;
		Inventory_Set(i, block);
		return;
	}
	/* Hotbar full - block is still counted, just not displayed */
}

/* Removes one of the given block, clearing its hotbar slot when it runs out. */
static void SurvivalTest_Consume(BlockID block) {
	int i;
	if (st_counts[block] > 0) st_counts[block]--;
	if (st_counts[block] > 0) return;

	for (i = 0; i < INVENTORY_BLOCKS_PER_HOTBAR; i++) {
		if (Inventory_Get(i) == block) Inventory_Set(i, BLOCK_AIR);
	}
}

static void SurvivalTest_BlockChanged(void* obj,
									  IVec3 coords, BlockID oldBlock, BlockID block) {
	if (!SurvivalTest_Enabled) return;

	if (block == BLOCK_AIR) {
		/* Block was mined - give the player its drop */
		SurvivalTest_AddBlock(SurvivalTest_DropFor(oldBlock));
	} else {
		/* Block was placed - consume one from the player's holdings */
		SurvivalTest_Consume(block);
	}
}


/*########################################################################################################################*
*--------------------------------------------------------Ticking----------------------------------------------------------*
*#########################################################################################################################*/
static cc_bool SurvivalTest_IsHeadInWater(struct Entity* e) {
	Vec3 eye;
	int x, y, z;
	BlockID b;

	eye = Entity_GetEyePosition(e);
	x = Math_Floor(eye.x);
	y = Math_Floor(eye.y);
	z = Math_Floor(eye.z);
	if (!World_Contains(x, y, z)) return false;

	b = World_GetBlock(x, y, z);
	return Blocks.Collide[b] == COLLIDE_WATER;
}

static void SurvivalTest_DoRespawn(void) {
	struct LocationUpdate update;
	struct LocalPlayer* p = Entities.CurPlayer;

	LocalPlayer_CalcDefaultSpawn(p, &update);
	LocalPlayers_MoveToSpawn(&update);

	SurvivalTest_Health = SURVIVAL_MAX_HEALTH;
	st_airTimer     = AIR_SUPPLY_SECS;
	st_isDead       = false;
	st_fallDistance = 0.0f;
	st_prevYValid   = false;
	st_invincTimer  = 1.0f; /* brief grace period after respawning */
	st_lavaTimer    = 0.0f;
	st_drownTimer   = 0.0f;
	st_wasOnGround  = false;
}

static void SurvivalTest_UpdateFall(struct Entity* e, struct LocalPlayer* p, cc_bool onGround) {
	float y = e->Position.y;

	/* Track fall distance from actual vertical movement (robust regardless */
	/*  of physics internals or scheduled task ordering) */
	if (st_prevYValid && !onGround && !p->Hacks.Flying && !p->Hacks.Noclip) {
		float dy = st_prevY - y; /* positive while descending */
		if (dy > 0.0f) {
			st_fallDistance += dy;
		} else {
			st_fallDistance = 0.0f; /* rising (e.g. jump) resets the fall */
		}
	}

	/* Landing on the ground applies accumulated fall damage */
	if (onGround && !st_wasOnGround && st_fallDistance > FALL_SAFE_BLOCKS) {
		float excess = st_fallDistance - FALL_SAFE_BLOCKS;
		int   damage = (int)(excess + 0.5f); /* ~1 HP per excess block */
		SurvivalTest_Hurt(damage);
	}
	if (onGround) st_fallDistance = 0.0f;

	st_prevY      = y;
	st_prevYValid = true;
}

static void SurvivalTest_Tick(struct ScheduledTask* task) {
	struct LocalPlayer* p;
	struct Entity* e;
	cc_bool onGround, inLava, inWater, headInWater;
	float delta = (float)task->interval;

	if (!SurvivalTest_Enabled || !World.Loaded) return;
	p = Entities.CurPlayer;
	if (!p) return;
	e = &p->Base;

	/* Death & respawn countdown */
	if (st_isDead) {
		st_respawnTimer -= delta;
		if (st_respawnTimer <= 0.0f) SurvivalTest_DoRespawn();
		return;
	}

	if (st_invincTimer > 0.0f) {
		st_invincTimer -= delta;
		if (st_invincTimer < 0.0f) st_invincTimer = 0.0f;
	}

	onGround    = e->OnGround;
	inLava      = Entity_TouchesAnyLava(e);
	inWater     = Entity_TouchesAnyWater(e);
	headInWater = SurvivalTest_IsHeadInWater(e);

	/* Fall damage -------------------------------------------------------- */
	SurvivalTest_UpdateFall(e, p, onGround);
	/* Landing in liquid breaks the fall */
	if (inWater || inLava) st_fallDistance = 0.0f;

	/* Lava damage -------------------------------------------------------- */
	if (inLava) {
		st_lavaTimer -= delta;
		if (st_lavaTimer <= 0.0f) {
			SurvivalTest_Hurt(LAVA_DAMAGE);
			st_lavaTimer = LAVA_DMG_INTERVAL;
		}
	} else {
		st_lavaTimer = 0.0f;
	}

	/* Drowning ----------------------------------------------------------- */
	if (headInWater) {
		st_airTimer -= delta;
		if (st_airTimer <= 0.0f) {
			st_airTimer    = 0.0f;
			st_drownTimer -= delta;
			if (st_drownTimer <= 0.0f) {
				SurvivalTest_Hurt(DROWN_DAMAGE);
				st_drownTimer = DROWN_DMG_INTERVAL;
			}
		}
	} else {
		/* Refill air at double speed once the head surfaces */
		st_airTimer += delta * 2.0f;
		if (st_airTimer > AIR_SUPPLY_SECS) st_airTimer = AIR_SUPPLY_SECS;
		st_drownTimer = 0.0f;
	}

	st_wasOnGround = onGround;
}


/*########################################################################################################################*
*--------------------------------------------------------Component--------------------------------------------------------*
*#########################################################################################################################*/
static void SurvivalTest_ResetState(void) {
	int i;
	st_fallDistance = 0.0f;
	st_prevYValid   = false;
	st_wasOnGround  = false;
	st_lavaTimer    = 0.0f;
	st_drownTimer   = 0.0f;
	st_isDead       = false;
	st_invincTimer  = 0.0f;
	st_respawnTimer = 0.0f;
	st_airTimer     = AIR_SUPPLY_SECS;
	SurvivalTest_Health = SURVIVAL_MAX_HEALTH;

	for (i = 0; i < BLOCK_COUNT; i++) st_counts[i] = 0;
}

/* Clears the hotbar so the player starts empty and must gather blocks. */
static void SurvivalTest_ClearHotbar(void) {
	int i;
	for (i = 0; i < INVENTORY_BLOCKS_PER_HOTBAR; i++) {
		Inventory_Set(i, BLOCK_AIR);
	}
}

static void SurvivalTest_Init(void) {
	SurvivalTest_Enabled = Options_GetBool(OPT_SURVIVAL_MODE, false);
	if (!SurvivalTest_Enabled) return;

	SurvivalTest_ResetState();
	ScheduledTask_Add(GAME_DEF_TICKS, SurvivalTest_Tick);
	Event_Register_(&UserEvents.BlockChanged, NULL, SurvivalTest_BlockChanged);
}

static void SurvivalTest_Free(void) {
	if (!SurvivalTest_Enabled) return;
	Event_Unregister_(&UserEvents.BlockChanged, NULL, SurvivalTest_BlockChanged);
}

static void SurvivalTest_OnNewMap(void) {
	if (!SurvivalTest_Enabled) return;
	SurvivalTest_ResetState();
	SurvivalTest_ClearHotbar();
}

struct IGameComponent SurvivalTest_Component = {
	SurvivalTest_Init,     /* Init           */
	SurvivalTest_Free,     /* Free           */
	NULL,                  /* Reset          */
	SurvivalTest_OnNewMap, /* OnNewMap       */
	NULL                   /* OnNewMapLoaded */
};
