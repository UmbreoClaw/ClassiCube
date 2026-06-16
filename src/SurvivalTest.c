#include "SurvivalTest.h"
#include "Entity.h"
#include "World.h"
#include "Game.h"
#include "Event.h"
#include "Inventory.h"
#include "Chat.h"
#include "Block.h"
#include "Constants.h"
#include "Options.h"
#include "Vectors.h"
#include "ExtMath.h"

/* Classic 0.30 Survival Test gamemode implementation.
   Copyright 2014-2025 ClassiCube | Licensed under BSD-3
*/

cc_bool SurvivalTest_Enabled;
int     SurvivalTest_Health = SURVIVAL_MAX_HEALTH;
float   SurvivalTest_AirTimer;

/* How long (seconds) the player is invincible after taking damage */
#define INVINCIBILITY_SECS 0.5f
/* Lava damages every 0.5 seconds */
#define LAVA_DMG_INTERVAL  0.5f
/* Lava damage per tick */
#define LAVA_DAMAGE        4
/* Drowning damages every 1 second once air is depleted */
#define DROWN_DMG_INTERVAL 1.0f
/* Drowning damage per tick */
#define DROWN_DAMAGE       2
/* Starting air supply in seconds */
#define AIR_SUPPLY_SECS    15.0f
/* Falls longer than this (in blocks) deal damage */
#define FALL_SAFE_BLOCKS   3.5f

static float st_invincTimer;
static float st_lavaTimer;
static float st_drownTimer;
static float st_fallDistance;
static cc_bool st_wasOnGround;
static cc_bool st_isDead;
static float st_respawnTimer;


void SurvivalTest_Hurt(int damage) {
	if (!SurvivalTest_Enabled) return;
	if (st_isDead)            return;
	if (st_invincTimer > 0.0f) return;

	SurvivalTest_Health -= damage;
	st_invincTimer = INVINCIBILITY_SECS;

	if (SurvivalTest_Health <= 0) {
		SurvivalTest_Health = 0;
		st_isDead      = true;
		st_respawnTimer = 2.5f;
		Chat_AddRaw("&cYou died! Respawning...");
	}
}

void SurvivalTest_Heal(int amount) {
	if (!SurvivalTest_Enabled) return;
	SurvivalTest_Health += amount;
	if (SurvivalTest_Health > SURVIVAL_MAX_HEALTH)
		SurvivalTest_Health = SURVIVAL_MAX_HEALTH;
}

static void SurvivalTest_DoRespawn(void) {
	struct LocationUpdate update;
	struct LocalPlayer* p = Entities.CurPlayer;

	LocalPlayer_CalcDefaultSpawn(p, &update);
	LocalPlayers_MoveToSpawn(&update);

	SurvivalTest_Health  = SURVIVAL_MAX_HEALTH;
	SurvivalTest_AirTimer = AIR_SUPPLY_SECS;
	st_isDead       = false;
	st_fallDistance = 0.0f;
	st_invincTimer  = 1.0f; /* brief immunity after respawn */
	st_lavaTimer    = 0.0f;
	st_drownTimer   = 0.0f;
	st_wasOnGround  = false;
}

static cc_bool SurvivalTest_IsHeadSubmerged(struct Entity* e) {
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

static void SurvivalTest_Tick(struct ScheduledTask* task) {
	struct LocalPlayer* p;
	struct Entity* e;
	cc_bool onGround, inLava, headSubmerged;
	float delta = (float)task->interval;

	if (!SurvivalTest_Enabled || !World.Loaded) return;
	p = Entities.CurPlayer;
	if (!p) return;
	e = &p->Base;

	/* Handle death & respawn countdown */
	if (st_isDead) {
		st_respawnTimer -= delta;
		if (st_respawnTimer <= 0.0f) SurvivalTest_DoRespawn();
		return;
	}

	/* Tick down invincibility */
	if (st_invincTimer > 0.0f) {
		st_invincTimer -= delta;
		if (st_invincTimer < 0.0f) st_invincTimer = 0.0f;
	}

	onGround       = e->OnGround;
	inLava         = Entity_TouchesAnyLava(e);
	headSubmerged  = SurvivalTest_IsHeadSubmerged(e);

	/* Fall damage ---------------------------------------------------------- */
	if (!onGround && !p->Hacks.Flying && !p->Hacks.Noclip) {
		if (e->Velocity.y < 0.0f) {
			/* Accumulate downward distance (velocity is blocks/tick) */
			st_fallDistance += -e->Velocity.y;
		} else {
			/* Rising - not yet a real fall */
			st_fallDistance = 0.0f;
		}
	}

	if (onGround && !st_wasOnGround && st_fallDistance > 0.0f) {
		if (st_fallDistance > FALL_SAFE_BLOCKS) {
			float excess  = st_fallDistance - FALL_SAFE_BLOCKS;
			int damage    = (int)(excess * 2.0f + 0.5f);
			if (damage > 0) SurvivalTest_Hurt(damage);
		}
		st_fallDistance = 0.0f;
	}

	/* Landing in water/lava breaks the fall */
	if (onGround || Entity_TouchesAnyWater(e) || inLava)
		st_fallDistance = 0.0f;

	st_wasOnGround = onGround;

	/* Lava damage ---------------------------------------------------------- */
	if (inLava) {
		st_lavaTimer -= delta;
		if (st_lavaTimer <= 0.0f) {
			SurvivalTest_Hurt(LAVA_DAMAGE);
			st_lavaTimer = LAVA_DMG_INTERVAL;
		}
	} else {
		st_lavaTimer = 0.0f;
	}

	/* Drowning ------------------------------------------------------------- */
	if (headSubmerged) {
		SurvivalTest_AirTimer -= delta;
		if (SurvivalTest_AirTimer < 0.0f) {
			st_drownTimer -= delta;
			if (st_drownTimer <= 0.0f) {
				SurvivalTest_Hurt(DROWN_DAMAGE);
				st_drownTimer = DROWN_DMG_INTERVAL;
			}
		}
	} else {
		/* Replenish air at double speed when surfaced */
		SurvivalTest_AirTimer += delta * 2.0f;
		if (SurvivalTest_AirTimer > AIR_SUPPLY_SECS)
			SurvivalTest_AirTimer = AIR_SUPPLY_SECS;
		st_drownTimer = 0.0f;
	}
}

static void SurvivalTest_BlockChanged(void* obj,
									  IVec3 coords, BlockID oldBlock, BlockID block) {
	if (!SurvivalTest_Enabled) return;
	/* Block replaced with air = player mined it; give them the block */
	if (block != BLOCK_AIR)    return;
	if (oldBlock == BLOCK_AIR) return;

	Inventory_PickBlock(oldBlock);
}

static void SurvivalTest_ResetState(void) {
	st_fallDistance      = 0.0f;
	st_wasOnGround       = false;
	st_lavaTimer         = 0.0f;
	st_drownTimer        = 0.0f;
	st_isDead            = false;
	st_invincTimer       = 0.0f;
	st_respawnTimer      = 0.0f;
	SurvivalTest_AirTimer = AIR_SUPPLY_SECS;
	SurvivalTest_Health   = SURVIVAL_MAX_HEALTH;
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
}

struct IGameComponent SurvivalTest_Component = {
	SurvivalTest_Init,     /* Init          */
	SurvivalTest_Free,     /* Free          */
	NULL,                  /* Reset         */
	SurvivalTest_OnNewMap, /* OnNewMap      */
	NULL                   /* OnNewMapLoaded*/
};
