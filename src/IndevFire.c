#include "IndevFire.h"
#include "IndevTest.h"
#include "SurvivalTest.h"
#include "World.h"
#include "Block.h"
#include "Game.h"
#include "ExtMath.h"
#include "Platform.h"
#include "Funcs.h"
#include "Constants.h"
#include "Lighting.h"
#include "String_.h"
#include "Audio.h"

/* BlockFire (in-20100223), transcribed line for line. Fire lives at the
   genuine id 51, ages 0-15 in a per-map nibble store, and is driven by a
   port of World.java's scheduled-update list (tickRate 20, at most 200
   entries processed per game tick) plus the random updateTick coverage
   every block gets from World.tick.
   Copyright 2014-2025 ClassiCube | Licensed under BSD-3
*/

/* Gated on Indev mode: in c0.30/creative maps id 51 belongs to whatever
    CPE decoration the server defines, which must keep engine behaviour. */
cc_bool IndevFire_IsFire(BlockID b) {
	return IndevTest_Enabled && b == INDEV_BLOCK_FIRE;
}

/*########################################################################################################################*
*-----------------------------------------------------Burn rate tables---------------------------------------------------*
*#########################################################################################################################*/
/* BlockFire's constructor: setBurnRate(planks 5/20, log 5/5, leaves 30/60,
    bookshelf 30/20, tnt 15/100, every cloth colour 30/60). chance = how
    strongly a block encourages fire next to it, ability = how easily it
    catches when fire tries to consume it. */
static cc_uint8 fire_chance[256], fire_ability[256];
static RNGState fire_rng;
static cc_bool  fire_rngInited;

static void Fire_InitTables(void) {
	int i;
	if (fire_chance[BLOCK_WOOD]) return; /* already built */

	fire_chance[BLOCK_WOOD]      = 5;  fire_ability[BLOCK_WOOD]      = 20;
	fire_chance[BLOCK_LOG]       = 5;  fire_ability[BLOCK_LOG]       = 5;
	fire_chance[BLOCK_LEAVES]    = 30; fire_ability[BLOCK_LEAVES]    = 60;
	fire_chance[BLOCK_BOOKSHELF] = 30; fire_ability[BLOCK_BOOKSHELF] = 20;
	fire_chance[BLOCK_TNT]       = 15; fire_ability[BLOCK_TNT]       = 100;
	/* the 16 cloth colours (classic ids 21-36) */
	for (i = BLOCK_RED; i <= BLOCK_WHITE; i++) {
		fire_chance[i] = 30; fire_ability[i] = 60;
	}

	if (!fire_rngInited) {
		Random_SeedFromCurrentTime(&fire_rng);
		fire_rngInited = true;
	}
}

cc_bool IndevFire_CanCatch(BlockID b) {
	Fire_InitTables();
	return b < 256 && fire_chance[b] > 0;
}

/*########################################################################################################################*
*---------------------------------------------------Age store + tick queue-----------------------------------------------*
*#########################################################################################################################*/
static cc_uint8* fire_age; /* one byte per block, allocated per map */
static int fire_ageVolume;

static void Fire_EnsureAges(void) {
	if (fire_age && fire_ageVolume == World.Volume) return;
	Mem_Free(fire_age);
	fire_age = (cc_uint8*)Mem_TryAllocCleared(World.Volume, 1);
	fire_ageVolume = fire_age ? World.Volume : 0;
}

int IndevFire_Age(int index) {
	if (!fire_age || index < 0 || index >= fire_ageVolume) return 0;
	return fire_age[index];
}

void IndevFire_SetAge(int index, int age) {
	Fire_EnsureAges();
	if (!fire_age || index < 0 || index >= fire_ageVolume) return;
	fire_age[index] = (cc_uint8)(age & 15);
}

/* World.java's NextTickListEntry queue: entries wait tickRate (20) game
    ticks, then run updateTick if the block still matches. The genuine list
    is unbounded; ours drops new entries when full (a forest fire that big
    is already re-scheduling constantly, so the loss self-heals). */
#define FIRE_QUEUE_LEN 8192
struct FireTickEntry { int index; cc_uint8 time; };
static struct FireTickEntry fire_queue[FIRE_QUEUE_LEN];
static int fire_qHead, fire_qCount;

static void Fire_Schedule(int index) {
	int tail;
	if (fire_qCount >= FIRE_QUEUE_LEN) return;
	tail = (fire_qHead + fire_qCount) % FIRE_QUEUE_LEN;
	fire_queue[tail].index = index;
	fire_queue[tail].time  = 20; /* BlockFire.tickRate() */
	fire_qCount++;
}

void IndevFire_Reset(void) {
	Mem_Free(fire_age);
	fire_age = NULL; fire_ageVolume = 0;
	fire_qHead = 0;  fire_qCount = 0;
}

void IndevFire_OnMapLoaded(void) {
	int i;
	if (!IndevTest_Enabled || !World.Blocks) return;
	Fire_InitTables();
	Fire_EnsureAges();
	fire_qHead = 0; fire_qCount = 0;

	/* setTickOnLoad(true): every fire block present in a loaded/generated
	    map gets a scheduled update straight away */
	for (i = 0; i < World.Volume; i++) {
		if (World.Blocks[i] == INDEV_BLOCK_FIRE) Fire_Schedule(i);
	}
}

/*########################################################################################################################*
*------------------------------------------------------Spread mechanics--------------------------------------------------*
*#########################################################################################################################*/
static BlockID Fire_GetBlock(int x, int y, int z) {
	if (!World_Contains(x, y, z)) return BLOCK_AIR;
	return World_GetBlock(x, y, z);
}

/* World.isBlockNormalCube, same approximation the torch code uses */
static cc_bool Fire_NormalCube(int x, int y, int z) {
	if (!World_Contains(x, y, z)) return false;
	return Blocks.FullOpaque[World_GetBlock(x, y, z)];
}

static cc_bool Fire_CanBlockCatch(int x, int y, int z) {
	return IndevFire_CanCatch(Fire_GetBlock(x, y, z));
}

static cc_bool Fire_CanNeighborCatch(int x, int y, int z) {
	return Fire_CanBlockCatch(x + 1, y, z) || Fire_CanBlockCatch(x - 1, y, z)
		|| Fire_CanBlockCatch(x, y - 1, z) || Fire_CanBlockCatch(x, y + 1, z)
		|| Fire_CanBlockCatch(x, y, z - 1) || Fire_CanBlockCatch(x, y, z + 1);
}

static int Fire_EncourageChance(int x, int y, int z, int cur) {
	BlockID b = Fire_GetBlock(x, y, z);
	int c = b < 256 ? fire_chance[b] : 0;
	return c > cur ? c : cur;
}

static void Fire_Set(int x, int y, int z, BlockID b) {
	if (!World_Contains(x, y, z)) return;
	if (b == INDEV_BLOCK_FIRE) IndevFire_SetAge(World_Pack(x, y, z), 0);
	Game_ChangeBlock(x, y, z, b); /* with notify - neighbours react */
}

/* BlockFire.tryToCatchBlockOnFire: rand(bound) < ability consumes the
    block - half the time it becomes fire, half plain air - and consumed
    TNT is armed (onBlockDestroyedByPlayer) rather than deleted. */
static void Fire_TryCatch(int x, int y, int z, int bound) {
	BlockID b   = Fire_GetBlock(x, y, z);
	int ability = b < 256 ? fire_ability[b] : 0;
	IVec3 coords;

	if (Random_Next(&fire_rng, bound) >= ability) return;

	if (Random_Next(&fire_rng, 2) == 0) {
		/* onBlockAdded scheduling now runs via the Game_UpdateBlock notify
		    hook (IndevTest_BlockUpdated -> IndevFire_BlockChanged) */
		Fire_Set(x, y, z, INDEV_BLOCK_FIRE);
	} else {
		Fire_Set(x, y, z, BLOCK_AIR);
	}

	if (b == BLOCK_TNT) {
		coords.x = x; coords.y = y; coords.z = z;
		SurvivalTest_IgniteTnt(coords);
	}
}

/* BlockFire.updateTick, verbatim: age, keep rescheduling, retire when
    nothing nearby burns, otherwise consume neighbours (down 100 / up 200 /
    sides 300) and jump to air cells in the 3x3 column up to y+4. */
static void Fire_UpdateTick(int x, int y, int z) {
	int index = World_Pack(x, y, z);
	int meta  = IndevFire_Age(index);
	int xx, yy, zz, bound, chance;

	if (meta < 15) {
		IndevFire_SetAge(index, meta + 1);
		Fire_Schedule(index);
	}

	if (!Fire_CanNeighborCatch(x, y, z)) {
		if (!Fire_NormalCube(x, y - 1, z) || meta > 3) Fire_Set(x, y, z, BLOCK_AIR);
		return;
	}
	if (!Fire_CanBlockCatch(x, y - 1, z) && meta == 15 && Random_Next(&fire_rng, 4) == 0) {
		Fire_Set(x, y, z, BLOCK_AIR);
		return;
	}

	if (meta % 5 != 0 || meta <= 5) return;
	Fire_TryCatch(x + 1, y, z, 300);
	Fire_TryCatch(x - 1, y, z, 300);
	Fire_TryCatch(x, y - 1, z, 100);
	Fire_TryCatch(x, y + 1, z, 200);
	Fire_TryCatch(x, y, z - 1, 300);
	Fire_TryCatch(x, y, z + 1, 300);

	for (xx = x - 1; xx <= x + 1; xx++) {
		for (zz = z - 1; zz <= z + 1; zz++) {
			for (yy = y - 1; yy <= y + 4; yy++) {
				if (xx == x && yy == y && zz == z) continue;

				bound = 100;
				if (yy > y + 1) bound += (yy - (y + 1)) * 100;

				if (Fire_GetBlock(xx, yy, zz) != BLOCK_AIR) continue;
				chance = Fire_EncourageChance(xx + 1, yy, zz, 0);
				chance = Fire_EncourageChance(xx - 1, yy, zz, chance);
				chance = Fire_EncourageChance(xx, yy - 1, zz, chance);
				chance = Fire_EncourageChance(xx, yy + 1, zz, chance);
				chance = Fire_EncourageChance(xx, yy, zz - 1, chance);
				chance = Fire_EncourageChance(xx, yy, zz + 1, chance);

				if (chance > 0 && Random_Next(&fire_rng, bound) <= chance) {
					if (World_Contains(xx, yy, zz)) {
						/* the notify hook runs onBlockAdded's schedule */
						Fire_Set(xx, yy, zz, INDEV_BLOCK_FIRE);
					}
				}
			}
		}
	}
}

void IndevFire_Tick(void) {
	int i, n;
	struct FireTickEntry e;
	if (!IndevTest_Enabled || !World.Blocks || !fire_qCount) return;

	/* World.tick: at most 200 entries per game tick; waiting entries are
	    decremented and requeued at the back, due ones run updateTick if
	    the block is still fire */
	n = min(fire_qCount, 200);
	for (i = 0; i < n; i++) {
		e = fire_queue[fire_qHead];
		fire_qHead = (fire_qHead + 1) % FIRE_QUEUE_LEN;
		fire_qCount--;

		if (e.time > 0) {
			e.time--;
			if (fire_qCount < FIRE_QUEUE_LEN) {
				int tail = (fire_qHead + fire_qCount) % FIRE_QUEUE_LEN;
				fire_queue[tail] = e;
				fire_qCount++;
			}
		} else if (e.index >= 0 && e.index < World.Volume &&
				   World.Blocks[e.index] == INDEV_BLOCK_FIRE) {
			int x, y, z;
			World_Unpack(e.index, x, y, z);
			Fire_UpdateTick(x, y, z);
		}
	}
}

void IndevFire_RandomTick(int index) {
	int x, y, z;
	if (!World.Blocks || World.Blocks[index] != INDEV_BLOCK_FIRE) return;
	World_Unpack(index, x, y, z);
	Fire_UpdateTick(x, y, z);
}

/*########################################################################################################################*
*-------------------------------------------------Placement / notifications----------------------------------------------*
*#########################################################################################################################*/
/* onBlockAdded / onNeighborBlockChange: fire needs solid ground below OR a
    flammable neighbour, else it disappears immediately. */
static void Fire_Validate(int x, int y, int z) {
	if (Fire_GetBlock(x, y, z) != INDEV_BLOCK_FIRE) return;
	if (!Fire_NormalCube(x, y - 1, z) && !Fire_CanNeighborCatch(x, y, z)) {
		Fire_Set(x, y, z, BLOCK_AIR);
	}
}

void IndevFire_BlockChanged(IVec3 coords, BlockID oldBlock, BlockID newBlock) {
	if (!IndevTest_Enabled) return;
	Fire_InitTables();

	if (newBlock == INDEV_BLOCK_FIRE && oldBlock != INDEV_BLOCK_FIRE) {
		/* onBlockAdded: validate, then schedule the first update */
		IndevFire_SetAge(World_Pack(coords.x, coords.y, coords.z), 0);
		Fire_Validate(coords.x, coords.y, coords.z);
		if (World_GetBlock(coords.x, coords.y, coords.z) == INDEV_BLOCK_FIRE) {
			Fire_Schedule(World_Pack(coords.x, coords.y, coords.z));
		}
	}
	if (oldBlock == INDEV_BLOCK_FIRE && newBlock != INDEV_BLOCK_FIRE) {
		IndevFire_SetAge(World_Pack(coords.x, coords.y, coords.z), 0);
	}

	/* onNeighborBlockChange for the six neighbouring fires */
	Fire_Validate(coords.x - 1, coords.y, coords.z);
	Fire_Validate(coords.x + 1, coords.y, coords.z);
	Fire_Validate(coords.x, coords.y - 1, coords.z);
	Fire_Validate(coords.x, coords.y + 1, coords.z);
	Fire_Validate(coords.x, coords.y, coords.z - 1);
	Fire_Validate(coords.x, coords.y, coords.z + 1);
}

/*########################################################################################################################*
*------------------------------------------------------Flint and steel---------------------------------------------------*
*#########################################################################################################################*/
cc_bool IndevFire_UseFlintSteel(IVec3 clickedPos, Face face) {
	int x = clickedPos.x, y = clickedPos.y, z = clickedPos.z;
	if (!IndevTest_Enabled) return false;
	Fire_InitTables();

	/* ItemFlintAndSteel.onItemUse: step out of the clicked face */
	switch (face) {
	case FACE_YMIN: y--; break;
	case FACE_YMAX: y++; break;
	case FACE_ZMIN: z--; break;
	case FACE_ZMAX: z++; break;
	case FACE_XMIN: x--; break;
	case FACE_XMAX: x++; break;
	}

	/* Genuine gates the WHOLE use on the target being interior (>0 and <dim-1
	    on every axis): a boundary click returns false with NO wear - damageItem
	    sits inside that branch. (An occupied interior target still wears; only
	    the fire itself needs the cell to be air.) */
	if (!(x > 0 && y > 0 && z > 0 &&
		  x < World.Width - 1 && y < World.Height - 1 && z < World.Length - 1)) return false;

	if (World_GetBlock(x, y, z) == BLOCK_AIR) {
		/* "fire.ignite", 1.0F, rand * 0.4F + 0.8F */
		SurvivalTest_PlaySoundAtBlock(x, y, z, MOBSND_IGNITE, 1.0f,
			Random_Float(&fire_rng) * 0.4f + 0.8f);
		Fire_Set(x, y, z, INDEV_BLOCK_FIRE); /* hook schedules it */
	}
	SurvivalTest_DamageHeldItem(1);
	return true;
}

/* World.extinguishFire: on EVERY left click that hits a block (before the
    dig even starts), step one cell out of the clicked face - if that cell
    holds fire, fizz it out. The click then proceeds to punch the block as
    usual, exactly like Minecraft.clickMouse. Fire itself is never in the
    pick ray (isCollidable false), so this is the only way to put it out. */
cc_bool IndevFire_Extinguish(IVec3 clickedPos, Face face) {
	int x = clickedPos.x, y = clickedPos.y, z = clickedPos.z;
	if (!IndevTest_Enabled) return false;
	Fire_InitTables();

	switch (face) {
	case FACE_YMIN: y--; break;
	case FACE_YMAX: y++; break;
	case FACE_ZMIN: z--; break;
	case FACE_ZMAX: z++; break;
	case FACE_XMIN: x--; break;
	case FACE_XMAX: x++; break;
	}

	if (!World_Contains(x, y, z)) return false;
	if (World_GetBlock(x, y, z) != INDEV_BLOCK_FIRE) return false;

	/* "random.fizz", 0.5F, 2.6F + (rand - rand) * 0.8F */
	SurvivalTest_PlaySoundAtBlock(x, y, z, MOBSND_FIZZ, 0.5f,
		2.6f + (Random_Float(&fire_rng) - Random_Float(&fire_rng)) * 0.8f);
	Fire_Set(x, y, z, BLOCK_AIR);
	return true;
}

/*########################################################################################################################*
*------------------------------------------------------Lava ignition-----------------------------------------------------*
*#########################################################################################################################*/
/* BlockFire.fireSpread's fireCheck: an existing fire counts, an air cell
    becomes fire, anything else fails. */
static cc_bool Fire_SpreadCheck(int x, int y, int z) {
	BlockID b = Fire_GetBlock(x, y, z);
	if (b == INDEV_BLOCK_FIRE) return true;
	if (b != BLOCK_AIR)        return false;
	if (!World_Contains(x, y, z)) return false;
	Fire_Set(x, y, z, INDEV_BLOCK_FIRE); /* hook schedules it */
	return true;
}

cc_bool IndevFire_LavaFlowInto(int x, int y, int z) {
	cc_bool lit;
	BlockID b;
	if (!IndevTest_Enabled) return false;
	Fire_InitTables();

	b = Fire_GetBlock(x, y, z);
	if (!(b < 256 && fire_chance[b] > 0)) return false;

	/* BlockFire.fireSpread: light the first free spot around the block
	    (up, -x, +x, -z, +z, below), or turn the block itself to fire */
	lit = Fire_SpreadCheck(x, y + 1, z);
	if (!lit) lit = Fire_SpreadCheck(x - 1, y, z);
	if (!lit) lit = Fire_SpreadCheck(x + 1, y, z);
	if (!lit) lit = Fire_SpreadCheck(x, y, z - 1);
	if (!lit) lit = Fire_SpreadCheck(x, y, z + 1);
	if (!lit) lit = Fire_SpreadCheck(x, y - 1, z);
	if (!lit) {
		Fire_Set(x, y, z, INDEV_BLOCK_FIRE); /* hook schedules it */
	}
	return true;
}

/*########################################################################################################################*
*-----------------------------------------------------Block definition---------------------------------------------------*
*#########################################################################################################################*/
void IndevFire_DefineBlock(void) {
	cc_string name = String_FromConst("Fire");
	BlockID id = INDEV_BLOCK_FIRE;
	Fire_InitTables();

	Block_SetName(id, &name);
	Block_SetSide(INDEV_FIRE_TEX_LOC, id);
	Block_Tex(id, FACE_YMAX) = INDEV_FIRE_TEX_LOC;
	Block_Tex(id, FACE_YMIN) = INDEV_FIRE_TEX_LOC;

	Blocks.Collide[id]         = COLLIDE_NONE;
	Blocks.ExtendedCollide[id] = COLLIDE_NONE;
	Blocks.Draw[id]            = DRAW_SPRITE; /* custom mesh - Builder_DrawFire */
	Blocks.BlocksLight[id]     = false;
	/* lightValue 1.0 in genuine = full light level 15 */
	Blocks.Brightness[id]      = 15 << FANCY_LIGHTING_LAMP_SHIFT;
	Blocks.DigSounds[id]       = SOUND_WOOD;
	Blocks.StepSounds[id]      = SOUND_WOOD;
	Blocks.CanPlace[id]        = true;  /* genuine has no fire item at all -
	    ours exists for /client give + the chain armor recipe, so placing
	    it directly is allowed (it behaves like flint-and-steel fire) */
	Blocks.CanDelete[id]       = true;
	Vec3_Set(Blocks.MinBB[id], 0.0f, 0.0f, 0.0f);
	Vec3_Set(Blocks.MaxBB[id], 1.0f, 1.0f, 1.0f);
	Block_DefineCustom(id, false);
	/* punching fire extinguishes it instantly (and it drops nothing) */
	SurvivalTest_SetHardness(id, 0);
}
