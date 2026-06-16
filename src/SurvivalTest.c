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
#include "Screens.h"
#include "Model.h"
#include "Graphics.h"
#include "Platform.h"

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

static float st_airTimer;
static float st_invincTimer;
static float st_lavaTimer;
static float st_drownTimer;
static float st_fallPeakY;    /* highest Y reached during the current fall */
static cc_bool st_falling;    /* whether a fall is currently being tracked */
static cc_bool st_isDead;

/* Slot-based inventory: slots 0..8 are the hotbar, 9..35 are storage. */
struct SurvivalSlot { BlockID block; cc_int16 count; };
static struct SurvivalSlot st_inv[SURVIVAL_INV_SLOTS];
/* Bumped on every inventory change so the HUD knows to redraw counts. */
static int st_invVersion;
static RNGState st_dropRng;
/* Defined later, in the Inventory section - forward declared so the */
/*  dropped-item pickup logic below can hand picked-up blocks to it. */
static void SurvivalTest_AddBlock(BlockID block);


/*########################################################################################################################*
*------------------------------------------------------Dropped items-------------------------------------------------------*
*#########################################################################################################################*/
/* Survival Test (since 0.24-s) drops physical items on the ground instead */
/*  of putting mined blocks straight into the inventory - the player has */
/*  to walk over them to collect them. */
#define DROP_MAX           64
#define DROP_GRAVITY        20.0f  /* blocks/sec^2 */
#define DROP_TERMINAL_VEL   10.0f  /* blocks/sec   */
#define DROP_PICKUP_DELAY    0.5f  /* seconds before a fresh drop can be collected */
#define DROP_PICKUP_RADIUS   1.0f  /* blocks */

struct DropItem {
	struct Entity entity;
	Vec3 velocity;
	BlockID block;
	float pickupDelay;
	float age;       /* used to animate the white pulse */
	cc_bool active;
};
static struct DropItem st_drops[DROP_MAX];
/* The drop currently being rendered - lets DropItem_GetCol find its pulse phase. */
static struct DropItem* st_renderingDrop;

static PackedCol DropItem_GetCol(struct Entity* e) {
	float pulse = st_renderingDrop ? st_renderingDrop->age : 0.0f;
	/* Survival Test items pulse white rather than using normal world lighting */
	return PackedCol_Scale(PACKEDCOL_WHITE, 0.7f + 0.3f * Math_SinF(pulse * 6.0f));
}

static void DropItem_Despawn(struct Entity* e) { }
static void DropItem_Tick(struct Entity* e, float delta) { }
static void DropItem_SetLocation(struct Entity* e, struct LocationUpdate* update) { }
static void DropItem_RenderModel(struct Entity* e, float delta, float t) { }
static cc_bool DropItem_ShouldRenderName(struct Entity* e) { return false; }

static const struct EntityVTABLE dropItem_VTABLE = {
	DropItem_Tick, DropItem_Despawn, DropItem_SetLocation, DropItem_GetCol,
	DropItem_RenderModel, DropItem_ShouldRenderName
};

static int SurvivalTest_FindFreeDropSlot(void) {
	int i;
	for (i = 0; i < DROP_MAX; i++) {
		if (!st_drops[i].active) return i;
	}
	return -1;
}

/* Spawns one physical item drop at the centre of the given block coords, */
/*  with a small random scatter-pop velocity (matches Survival Test's look). */
static void SurvivalTest_SpawnDrop(IVec3 coords, BlockID block) {
	struct DropItem* d;
	struct Entity* e;
	float ang, speed;
	int slot = SurvivalTest_FindFreeDropSlot();
	if (slot < 0) return; /* drop limit reached - oldest drops simply aren't replaced */

	d = &st_drops[slot];
	e = &d->entity;
	Mem_Set(e, 0, sizeof(struct Entity));

	e->VTABLE     = &dropItem_VTABLE;
	e->Model      = Models.Block;
	e->ModelBlock = block;
	Vec3_Set(e->ModelScale, 1, 1, 1);
	e->Flags = ENTITY_FLAG_HAS_MODELVB;

	e->Position.x = coords.x + 0.5f;
	e->Position.y = coords.y + 0.3f;
	e->Position.z = coords.z + 0.5f;
	Entity_UpdateModelBounds(e);

	ang   = Random_Float(&st_dropRng) * 2.0f * MATH_PI;
	speed = 0.6f + Random_Float(&st_dropRng) * 0.6f;
	d->velocity.x = Math_CosF(ang) * speed;
	d->velocity.z = Math_SinF(ang) * speed;
	d->velocity.y = 2.5f + Random_Float(&st_dropRng) * 1.0f;

	d->block       = block;
	d->pickupDelay = DROP_PICKUP_DELAY;
	d->age         = 0.0f;
	d->active      = true;
}

/* Decides what physically drops when a block is mined (Survival Test rules). */
static void SurvivalTest_SpawnDropsForBlock(IVec3 coords, BlockID oldBlock) {
	BlockID dropBlock = oldBlock;
	int count = 1, i;

	switch (oldBlock) {
	case BLOCK_GRASS:
		dropBlock = BLOCK_DIRT;
		break;
	case BLOCK_LEAVES:
		/* Leaves only drop a sapling 1/10 of the time, otherwise nothing */
		if (Random_Next(&st_dropRng, 10) != 0) return;
		dropBlock = BLOCK_SAPLING;
		break;
	case BLOCK_LOG:
		dropBlock = BLOCK_WOOD;
		count     = 3 + Random_Next(&st_dropRng, 3); /* 3-5 planks */
		break;
	default:
		break; /* most blocks drop themselves */
	}

	for (i = 0; i < count; i++) { SurvivalTest_SpawnDrop(coords, dropBlock); }
}

static float SurvivalTest_DropGroundY(int x, int y, int z) {
	BlockID b;
	if (!World_Contains(x, y, z)) return -100000.0f;

	b = World_GetBlock(x, y, z);
	if (Blocks.Collide[b] != COLLIDE_SOLID) return -100000.0f;
	return (float)y + Blocks.MaxBB[b].y;
}

static void SurvivalTest_DropPhysics(struct DropItem* d, float delta) {
	struct Entity* e = &d->entity;
	float groundY;
	int x, y, z;

	d->velocity.y -= DROP_GRAVITY * delta;
	if (d->velocity.y < -DROP_TERMINAL_VEL) d->velocity.y = -DROP_TERMINAL_VEL;

	e->Position.x += d->velocity.x * delta;
	e->Position.y += d->velocity.y * delta;
	e->Position.z += d->velocity.z * delta;

	x = Math_Floor(e->Position.x);
	z = Math_Floor(e->Position.z);
	y = Math_Floor(e->Position.y - 0.01f);
	groundY = SurvivalTest_DropGroundY(x, y, z);

	if (groundY > -1000.0f && e->Position.y <= groundY) {
		e->Position.y = groundY;
		d->velocity.y = 0.0f;
		d->velocity.x *= 0.7f;
		d->velocity.z *= 0.7f;
		if (Math_AbsF(d->velocity.x) < 0.01f) d->velocity.x = 0.0f;
		if (Math_AbsF(d->velocity.z) < 0.01f) d->velocity.z = 0.0f;
	}
}

static void SurvivalTest_DropTryPickup(struct DropItem* d, struct Entity* pe) {
	Vec3 diff;
	float distSq;
	if (d->pickupDelay > 0.0f) return;

	diff.x = d->entity.Position.x - pe->Position.x;
	diff.y = d->entity.Position.y - pe->Position.y;
	diff.z = d->entity.Position.z - pe->Position.z;
	distSq = diff.x * diff.x + diff.y * diff.y + diff.z * diff.z;
	if (distSq > DROP_PICKUP_RADIUS * DROP_PICKUP_RADIUS) return;

	SurvivalTest_AddBlock(d->block);
	d->active = false;
}

static void SurvivalTest_TickDrops(struct Entity* pe, float delta) {
	struct DropItem* d;
	int i;

	for (i = 0; i < DROP_MAX; i++) {
		d = &st_drops[i];
		if (!d->active) continue;

		d->age += delta;
		SurvivalTest_DropPhysics(d, delta);

		if (d->pickupDelay > 0.0f) {
			d->pickupDelay -= delta;
		} else {
			SurvivalTest_DropTryPickup(d, pe);
		}
	}
}

void SurvivalTest_RenderDrops(float delta, float t) {
	int i;
	if (!SurvivalTest_Enabled) return;

	Gfx_SetAlphaTest(true);
	for (i = 0; i < DROP_MAX; i++) {
		if (!st_drops[i].active) continue;

		st_renderingDrop = &st_drops[i];
		Model_Render(Models.Block, &st_drops[i].entity);
	}
	st_renderingDrop = NULL;
	Gfx_SetAlphaTest(false);
}


/*########################################################################################################################*
*----------------------------------------------------Health & damage------------------------------------------------------*
*#########################################################################################################################*/
/* Applies damage. When ignoreInvinc is set the invincibility window is */
/*  bypassed and not refreshed (used for self-inflicted poison damage). */
static void SurvivalTest_Damage(int damage, cc_bool ignoreInvinc) {
	if (!SurvivalTest_Enabled) return;
	if (st_isDead)             return;
	if (damage <= 0)           return;
	if (!ignoreInvinc && st_invincTimer > 0.0f) return;

	SurvivalTest_Health -= damage;
	if (!ignoreInvinc) st_invincTimer = INVINCIBILITY_SECS;

	if (SurvivalTest_Health <= 0) {
		SurvivalTest_Health = 0;
		st_isDead = true;
		/* Classic 0.30-s had no respawn: death ends the world. The Game */
		/*  Over screen offers generating a fresh level or quitting. */
		GameOverScreen_Show();
	}
}

void SurvivalTest_Hurt(int damage) { SurvivalTest_Damage(damage, false); }

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
BlockID SurvivalTest_SlotBlock(int slot) { return st_inv[slot].block; }
int     SurvivalTest_SlotCount(int slot) { return st_inv[slot].count; }
int     SurvivalTest_HotbarCount(int slot) { return st_inv[slot].count; }
int     SurvivalTest_InvVersion(void) { return st_invVersion; }

cc_bool SurvivalTest_CanPlace(BlockID block) {
	if (!SurvivalTest_Enabled) return true;
	/* Placement always uses the selected hotbar slot */
	return st_inv[Inventory.SelectedIndex].count > 0;
}

/* Mirrors the hotbar slots into the engine's inventory table so that the */
/*  hotbar widget and held block renderer reflect the survival inventory. */
static void SurvivalTest_SyncHotbar(void) {
	int i;
	for (i = 0; i < SURVIVAL_HOTBAR_SLOTS; i++) {
		Inventory_Set(i, st_inv[i].block);
	}
	st_invVersion++;
}

void SurvivalTest_SwapSlots(int a, int b) {
	struct SurvivalSlot tmp;
	if (!SurvivalTest_Enabled) return;
	if (a == b) return;
	tmp       = st_inv[a];
	st_inv[a] = st_inv[b];
	st_inv[b] = tmp;
	SurvivalTest_SyncHotbar();
}

/* Adds one of the given block: stacks onto an existing matching slot if */
/*  possible, otherwise fills the first empty slot (hotbar slots first). */
static void SurvivalTest_AddBlock(BlockID block) {
	int i;
	if (block == BLOCK_AIR) return;

	/* Prefer topping up an existing, non-full stack of this block */
	for (i = 0; i < SURVIVAL_INV_SLOTS; i++) {
		if (st_inv[i].block == block && st_inv[i].count < SURVIVAL_STACK_MAX) {
			st_inv[i].count++;
			SurvivalTest_SyncHotbar();
			return;
		}
	}
	/* Otherwise place it into the first empty slot */
	for (i = 0; i < SURVIVAL_INV_SLOTS; i++) {
		if (st_inv[i].block != BLOCK_AIR) continue;
		st_inv[i].block = block;
		st_inv[i].count = 1;
		SurvivalTest_SyncHotbar();
		return;
	}
	/* Inventory full - drop is discarded */
}

/* Consumes one block from the currently selected hotbar slot. */
static void SurvivalTest_ConsumeSelected(void) {
	int slot = Inventory.SelectedIndex;
	if (st_inv[slot].count <= 0) return;

	st_inv[slot].count--;
	if (st_inv[slot].count == 0) st_inv[slot].block = BLOCK_AIR;
	SurvivalTest_SyncHotbar();
}

cc_bool SurvivalTest_TryEat(void) {
	int slot;
	BlockID block;
	if (!SurvivalTest_Enabled) return false;

	slot  = Inventory.SelectedIndex;
	if (st_inv[slot].count <= 0) return false;
	block = st_inv[slot].block;

	/* Survival Test: mushrooms are food, eaten with right-click */
	if (block == BLOCK_BROWN_SHROOM) {
		SurvivalTest_Heal(5);            /* brown mushroom restores 5 HP */
	} else if (block == BLOCK_RED_SHROOM) {
		SurvivalTest_Damage(3, true);    /* red mushroom is poisonous: -3 HP */
	} else {
		return false;                    /* not food - let normal placement run */
	}

	SurvivalTest_ConsumeSelected();
	return true;
}

static void SurvivalTest_BlockChanged(void* obj,
									  IVec3 coords, BlockID oldBlock, BlockID block) {
	if (!SurvivalTest_Enabled) return;

	if (block == BLOCK_AIR) {
		/* Block was mined - spawn its physical drop(s) on the ground */
		SurvivalTest_SpawnDropsForBlock(coords, oldBlock);
	} else {
		/* Block was placed - consume one from the selected hotbar slot */
		SurvivalTest_ConsumeSelected();
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

static void SurvivalTest_UpdateFall(struct Entity* e, struct LocalPlayer* p, cc_bool onGround) {
	float y = e->Position.y;

	/* Flying/noclip never accumulate fall damage */
	if (onGround || p->Hacks.Flying || p->Hacks.Noclip) {
		/* Just landed: deal damage from the full drop (peak Y minus the */
		/*  actual landing Y, so the final tick of the fall is included) */
		if (st_falling && onGround) {
			float dist = st_fallPeakY - y;
			if (dist > FALL_SAFE_BLOCKS) {
				/* Survival Test: ~1 HP per block past the 3-block safe drop */
				int damage = (int)dist - (int)FALL_SAFE_BLOCKS;
				SurvivalTest_Hurt(damage);
			}
		}
		st_falling = false;
	} else {
		/* Airborne: begin tracking, and keep the highest point reached so */
		/*  that the fall is measured from the apex (matches Survival Test) */
		if (!st_falling) {
			st_falling   = true;
			st_fallPeakY = y;
		}
		if (y > st_fallPeakY) st_fallPeakY = y;
	}
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

	/* While dead the Game Over screen is up and the world is frozen */
	if (st_isDead) return;

	if (st_invincTimer > 0.0f) {
		st_invincTimer -= delta;
		if (st_invincTimer < 0.0f) st_invincTimer = 0.0f;
	}

	onGround    = e->OnGround;
	inLava      = Entity_TouchesAnyLava(e);
	inWater     = Entity_TouchesAnyWater(e);
	headInWater = SurvivalTest_IsHeadInWater(e);

	/* Fall damage -------------------------------------------------------- */
	/* Touching liquid breaks the fall (water/lava cushions the landing) */
	if (inWater || inLava) st_falling = false;
	SurvivalTest_UpdateFall(e, p, onGround);

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

	/* Dropped items --------------------------------------------------------- */
	SurvivalTest_TickDrops(e, delta);
}


/*########################################################################################################################*
*--------------------------------------------------------Component--------------------------------------------------------*
*#########################################################################################################################*/
static void SurvivalTest_ResetState(void) {
	int i;
	st_falling      = false;
	st_lavaTimer    = 0.0f;
	st_drownTimer   = 0.0f;
	st_isDead       = false;
	st_invincTimer  = 0.0f;
	st_airTimer     = AIR_SUPPLY_SECS;
	SurvivalTest_Health = SURVIVAL_MAX_HEALTH;

	for (i = 0; i < SURVIVAL_INV_SLOTS; i++) {
		st_inv[i].block = BLOCK_AIR;
		st_inv[i].count = 0;
	}
	for (i = 0; i < DROP_MAX; i++) {
		st_drops[i].active = false;
	}
}

static void SurvivalTest_Init(void) {
	SurvivalTest_Enabled = Options_GetBool(OPT_SURVIVAL_MODE, false);
	if (!SurvivalTest_Enabled) return;

	Random_SeedFromCurrentTime(&st_dropRng);
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
	/* Start empty - the player must gather blocks by mining */
	SurvivalTest_ResetState();
	SurvivalTest_SyncHotbar();
}

static void SurvivalTest_OnNewMapLoaded(void) {
	struct LocalPlayer* p;
	if (!SurvivalTest_Enabled) return;

	p = Entities.CurPlayer;
	if (!p) return;

	/* Classic 0.30-s had no fly, noclip, or speed hacks */
	p->Hacks.CanFly    = false;
	p->Hacks.CanNoclip = false;
	p->Hacks.CanSpeed  = false;
	HacksComp_Update(&p->Hacks);
}

struct IGameComponent SurvivalTest_Component = {
	SurvivalTest_Init,          /* Init           */
	SurvivalTest_Free,          /* Free           */
	NULL,                       /* Reset          */
	SurvivalTest_OnNewMap,      /* OnNewMap       */
	SurvivalTest_OnNewMapLoaded /* OnNewMapLoaded */
};
