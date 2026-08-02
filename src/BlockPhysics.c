#include "BlockPhysics.h"
#include "World.h"
#include "Constants.h"
#include "Funcs.h"
#include "Event.h"
#include "ExtMath.h"
#include "Block.h"
#include "Lighting.h"
#include "Options.h"
#include "Generator.h"
#include "Platform.h"
#include "Game.h"
#include "Logger.h"
#include "Vectors.h"
#include "Chat.h"
#include "Audio.h"
#include "SurvivalTest.h"
#include "IndevTest.h"
#include "IndevFire.h"
#include "SurvivalNet.h"

/* Data for a resizable queue, used for liquid physic tick entries. */
struct TickQueue {
	cc_uint32* entries; /* Buffer holding the items in the tick queue */
	int capacity; /* Max number of elements in the buffer */
	int mask;     /* capacity - 1, as capacity is always a power of two */
	int count;    /* Number of used elements */
	int head;     /* Head index into the buffer */
	int tail;     /* Tail index into the buffer */
};

static void TickQueue_Init(struct TickQueue* queue) {
	queue->entries  = NULL;
	queue->capacity = 0;
	queue->mask  = 0;
	queue->count = 0;
	queue->head  = 0;
	queue->tail  = 0;
}

static void TickQueue_Clear(struct TickQueue* queue) {
	if (!queue->entries) return;
	Mem_Free(queue->entries);
	TickQueue_Init(queue);
}

static void TickQueue_Resize(struct TickQueue* queue) {
	cc_uint32* entries;
	int i, idx, capacity;

	if (queue->capacity >= (Int32_MaxValue / 4)) {
		Chat_AddRaw("&cToo many physics entries, clearing");
		TickQueue_Clear(queue);
	}

	capacity = queue->capacity * 2;
	if (capacity < 32) capacity = 32;
	entries = (cc_uint32*)Mem_Alloc(capacity, 4, "physics tick queue");

	/* Elements must be readjusted to avoid index wrapping issues */
	/* https://stackoverflow.com/questions/55343683/resizing-of-the-circular-queue-using-dynamic-array */
	for (i = 0; i < queue->count; i++) {
		idx = (queue->head + i) & queue->mask;
		entries[i] = queue->entries[idx];
	}
	Mem_Free(queue->entries);

	queue->entries  = entries;
	queue->capacity = capacity;
	queue->mask     = capacity - 1; /* capacity is power of two */
	queue->head = 0;
	queue->tail = queue->count;
}

/* Appends an entry to the end of the queue, resizing if necessary. */
static void TickQueue_Enqueue(struct TickQueue* queue, cc_uint32 item) {
	if (queue->count == queue->capacity)
		TickQueue_Resize(queue);

	queue->entries[queue->tail] = item;
	queue->tail = (queue->tail + 1) & queue->mask;
	queue->count++;
}

/* Retrieves the entry from the front of the queue. */
static cc_uint32 TickQueue_Dequeue(struct TickQueue* queue) {
	cc_uint32 result = queue->entries[queue->head];
	queue->head = (queue->head + 1) & queue->mask;
	queue->count--;
	return result;
}


struct Physics_ Physics;
static RNGState physics_rnd;
static int physics_tickCount;
static int physics_maxWaterX, physics_maxWaterY, physics_maxWaterZ;
static struct TickQueue lavaQ, waterQ;

#define PHYSICS_DELAY_MASK 0xF8000000UL
#define PHYSICS_POS_MASK   0x07FFFFFFUL
#define PHYSICS_DELAY_SHIFT 27
#define PHYSICS_ONE_DELAY   (1U << PHYSICS_DELAY_SHIFT)
#define PHYSICS_LAVA_DELAY (30U << PHYSICS_DELAY_SHIFT)
/* Indev BlockFlowing.tickRate(): lava reflows every 25 ticks, not 30 */
#define PHYSICS_LAVA_DELAY_NOW ((IndevTest_Enabled ? 25U : 30U) << PHYSICS_DELAY_SHIFT)
#define PHYSICS_WATER_DELAY (5U << PHYSICS_DELAY_SHIFT)

/* c0.30 Level.tick's random-update state (see Physics_TickRandomBlocksC030) */
static int physics_unprocessed;
static cc_uint32 physics_randId;

static void Physics_OnNewMapLoaded(void* obj) {
	TickQueue_Clear(&lavaQ);
	TickQueue_Clear(&waterQ);

	physics_maxWaterX = World.MaxX - 2;
	physics_maxWaterY = World.MaxY - 2;
	physics_maxWaterZ = World.MaxZ - 2;

	Tree_Blocks = World.Blocks;
	Random_SeedFromCurrentTime(&physics_rnd);
	Tree_Rnd = &physics_rnd;

	/* c0.30 Level.initTransient: randId = random.nextInt() */
	physics_randId = ((cc_uint32)Random_Next(&physics_rnd, 65536) << 16)
	               |  (cc_uint32)Random_Next(&physics_rnd, 65536);
	physics_unprocessed = 0;
}

void Physics_SetEnabled(cc_bool enabled) {
	Physics.Enabled = enabled;
	Physics_OnNewMapLoaded(NULL);
}

static void Physics_Activate(int index) {
	BlockID block = World.Blocks[index];
	PhysicsHandler activate = Physics.OnActivate[block];
	if (activate) activate(index, block);
}

static void Physics_ActivateNeighbours(int x, int y, int z, int index) {
	if (x > 0)          Physics_Activate(index - 1);
	if (x < World.MaxX) Physics_Activate(index + 1);
	if (z > 0)          Physics_Activate(index - World.Width);
	if (z < World.MaxZ) Physics_Activate(index + World.Width);
	if (y > 0)          Physics_Activate(index - World.OneY);
	if (y < World.MaxY) Physics_Activate(index + World.OneY);
}

static cc_bool Physics_IsEdgeWater(int x, int y, int z) {
	return
		(Env.EdgeBlock == BLOCK_WATER || Env.EdgeBlock == BLOCK_STILL_WATER)
		&& (y >= Env_SidesHeight && y < Env.EdgeHeight)
		&& (x == 0 || z == 0 || x == World.MaxX || z == World.MaxZ);
}


void Physics_OnBlockChanged(int x, int y, int z, BlockID old, BlockID now) {
	PhysicsHandler handler;
	int index;
	if (!Physics.Enabled) return;

	if (now == BLOCK_AIR && Physics_IsEdgeWater(x, y, z)) {
		now = BLOCK_STILL_WATER;
		Game_UpdateBlock(x, y, z, BLOCK_STILL_WATER);
	}
	index = World_Pack(x, y, z);

	/* User can place/delete blocks over ID 256 */
	if (now == BLOCK_AIR) {
		handler = Physics.OnDelete[(BlockRaw)old];
		if (handler) handler(index, old);
	} else {
		handler = Physics.OnPlace[(BlockRaw)now];
		if (handler) handler(index, now);
	}
	Physics_ActivateNeighbours(x, y, z, index);
}

static void Physics_TickRandomBlocks(void) {
	int lo, hi, index;
	BlockID block;
	PhysicsHandler tick;
	int x, y, z, x2, y2, z2;

	for (y = 0; y < World.Height; y += CHUNK_SIZE) {
		y2 = min(y + CHUNK_MAX, World.MaxY);
		for (z = 0; z < World.Length; z += CHUNK_SIZE) {
			z2 = min(z + CHUNK_MAX, World.MaxZ);
			for (x = 0; x < World.Width; x += CHUNK_SIZE) {
				x2 = min(x + CHUNK_MAX, World.MaxX);

				/* Inlined 3 random ticks for this chunk */
				lo = World_Pack( x,  y,  z);
				hi = World_Pack(x2, y2, z2);
				
				index = Random_Range(&physics_rnd, lo, hi);
				block = World.Blocks[index];
				tick = Physics.OnRandomTick[block];
				if (tick) tick(index, block);

				index = Random_Range(&physics_rnd, lo, hi);
				block = World.Blocks[index];
				tick = Physics.OnRandomTick[block];
				if (tick) tick(index, block);

				index = Random_Range(&physics_rnd, lo, hi);
				block = World.Blocks[index];
				tick = Physics.OnRandomTick[block];
				if (tick) tick(index, block);
			}
		}
	}
}


/* c0.30 Level.tick's random block updates, at the genuine rate: unprocessed
    accumulates the world volume every tick and pays out volume/200 updates
    (the remainder carries), coordinates unpacked from the genuine randId LCG
    (randId*3 + 1013904223, >> 2, x from the low bits, z next, y highest).
    The engine loop above is 3 per 16^3 chunk = volume/1365 - about 6.8x
    sparser, so grass/saplings/flowers/mushrooms/liquid activation all ran
    visibly slow in c0.30 survival. Handlers are the same OnRandomTick table
    (genuine gates on Block.physics[], the same block set). Masks assume
    power-of-two dims like genuine; the bounds check skips (biased) picks on
    odd-sized imports. Mirrors IndevTest_TickRandomBlocks. */
/* Genuine c0.30 GrassTile.tick (tile q.a, from the jar): a 1-in-4 gate on the
    WHOLE tick; an unlit grass block turns to dirt; a lit one makes FOUR spread
    attempts at x+rand(3)-1, y+rand(5)-3, z+rand(3)-1, each converting a lit
    dirt block to grass. "Lit" is Level.isLit = pure sky exposure (c0.30 has no
    block light). The dirt tile has NO tick method at all - dirt only greens by
    spread from adjacent grass, never spontaneously. The classic-creative
    handlers below (Physics_HandleDirt/HandleGrass) mimic classic SERVER
    physics instead and stay registered for that mode; c0.30 survival must not
    use them (no 1/4 gate = 4x die-back, no spread, and spontaneous greening
    ignores adjacency entirely). */
static void Physics_TickGrassC030(int index) {
	int x, y, z, i, xi, yi, zi;
	World_Unpack(index, x, y, z);

	if (Random_Next(&physics_rnd, 4) != 0) return;
	if (!Lighting.IsLit(x, y, z)) {
		Game_UpdateBlock(x, y, z, BLOCK_DIRT);
		return;
	}
	for (i = 0; i < 4; i++) {
		xi = x + Random_Next(&physics_rnd, 3) - 1;
		yi = y + Random_Next(&physics_rnd, 5) - 3;
		zi = z + Random_Next(&physics_rnd, 3) - 1;
		if (!World_Contains(xi, yi, zi))                 continue;
		if (World_GetBlock(xi, yi, zi) != BLOCK_DIRT)    continue;
		if (!Lighting.IsLit(xi, yi, zi))                 continue;
		Game_UpdateBlock(xi, yi, zi, BLOCK_GRASS);
	}
}

static void Physics_TickRandomBlocksC030(void) {
	int shiftX = 1, shiftZ = 1;
	int maskX, maskY, maskZ;
	int count, i, x, y, z, index;
	cc_uint32 bits;
	BlockID block;
	PhysicsHandler tick;

	while ((1 << shiftX) < World.Width)  shiftX++;
	while ((1 << shiftZ) < World.Length) shiftZ++;
	maskX = World.Width - 1; maskZ = World.Length - 1; maskY = World.Height - 1;

	physics_unprocessed += World.Volume;
	count = physics_unprocessed / 200;
	physics_unprocessed -= count * 200;

	for (i = 0; i < count; i++) {
		physics_randId = physics_randId * 3u + 1013904223u;
		bits = physics_randId >> 2;
		x = (int)(bits & maskX);
		z = (int)((bits >> shiftX) & maskZ);
		y = (int)((bits >> (shiftX + shiftZ)) & maskY);
		if (x >= World.Width || y >= World.Height || z >= World.Length) continue;

		index = World_Pack(x, y, z);
		block = World.Blocks[index];
		/* survival-only overrides: the shared OnRandomTick table keeps the
		    classic-creative behaviour for the engine's own physics loop */
		if (block == BLOCK_GRASS) { Physics_TickGrassC030(index); continue; }
		if (block == BLOCK_DIRT)  continue; /* genuine dirt never ticks */
		tick  = Physics.OnRandomTick[block];
		if (tick) tick(index, block);
	}
}

static void Physics_DoFalling(int index, BlockID block) {
	int found = -1, start = index;
	BlockID other;
	int x, y, z;

	/* Find lowest block can fall into. Indev BlockSand.tryToFall also falls
	    THROUGH fire (extinguishing it en route). */
	while (index >= World.OneY) {
		index -= World.OneY;
		other  = World.Blocks[index];

		if (other == BLOCK_AIR || (other >= BLOCK_WATER && other <= BLOCK_STILL_LAVA)) {
			found = index;
		} else if (IndevTest_Enabled && other == INDEV_BLOCK_FIRE) {
			found = index;
			World_Unpack(index, x, y, z);
			Game_UpdateBlock(x, y, z, BLOCK_AIR);
		} else {
			break;
		}
	}

	if (found == -1) return;

	/* Genuine tryToFall: past the world bottom (getBlockId clamps y<0 to the
	    y=0 layer - air on a floating map) the faller is DESTROYED, not rested
	    on an invisible floor. Only reachable on Indev floating maps. */
	if (IndevTest_Enabled && found < World.OneY) {
		World_Unpack(found, x, y, z);
		if (World_GetPhysicsBlock(x, -1, z) == BLOCK_AIR) {
			World_Unpack(start, x, y, z);
			Game_UpdateBlock(x, y, z, BLOCK_AIR);
			Physics_ActivateNeighbours(x, y, z, start);
			return;
		}
	}

	World_Unpack(found, x, y, z);
	Game_UpdateBlock(x, y, z, block);

	World_Unpack(start, x, y, z);
	Game_UpdateBlock(x, y, z, BLOCK_AIR);
	Physics_ActivateNeighbours(x, y, z, start);
}

static cc_bool Physics_CheckItem(struct TickQueue* queue, int* posIndex) {
	cc_uint32 item = TickQueue_Dequeue(queue);
	*posIndex     = (int)(item & PHYSICS_POS_MASK);

	if (item >= PHYSICS_ONE_DELAY) {
		item -= PHYSICS_ONE_DELAY;
		TickQueue_Enqueue(queue, item);
		return false;
	}
	return true;
}


static void Physics_HandleSapling(int index, BlockID block) {
	IVec3 coords[TREE_MAX_COUNT];
	BlockRaw blocks[TREE_MAX_COUNT];
	int i, count, height;

	BlockID below;
	int x, y, z;
	World_Unpack(index, x, y, z);

	below = BLOCK_AIR;
	if (y > 0) below = World.Blocks[index - World.OneY];
	/* Saplings stay alive on dirt */
	if (below == BLOCK_DIRT) return;

	/* Saplings grow if on grass in light, otherwise turn to air */
	Game_UpdateBlock(x, y, z, BLOCK_AIR);
	if (below != BLOCK_GRASS || !Lighting.IsLit(x, y, z)) return;

	height = 5 + Random_Next(&physics_rnd, 3);

	if (TreeGen_CanGrow(x, y, z, height)) {
		count = TreeGen_Grow(x, y, z, height, coords, blocks);

		for (i = 0; i < count; i++) 
		{
			Game_UpdateBlock(coords[i].x, coords[i].y, coords[i].z, blocks[i]);
		}
	}
}

static void Physics_HandleDirt(int index, BlockID block) {
	int x, y, z;
	World_Unpack(index, x, y, z);

	if (Lighting.IsLit(x, y, z)) {
		Game_UpdateBlock(x, y, z, BLOCK_GRASS);
	}
}

static void Physics_HandleGrass(int index, BlockID block) {
	int x, y, z;
	World_Unpack(index, x, y, z);

	if (!Lighting.IsLit(x, y, z)) {
		Game_UpdateBlock(x, y, z, BLOCK_DIRT);
	}
}

static void Physics_HandleFlower(int index, BlockID block) {
	BlockID below;
	int x, y, z;
	World_Unpack(index, x, y, z);

	if (!Lighting.IsLit(x, y, z)) {
		Game_UpdateBlock(x, y, z, BLOCK_AIR);
		Physics_ActivateNeighbours(x, y, z, index);
		return;
	}

	below = BLOCK_DIRT;
	if (y > 0) below = World.Blocks[index - World.OneY];
	if (!(below == BLOCK_DIRT || below == BLOCK_GRASS)) {
		Game_UpdateBlock(x, y, z, BLOCK_AIR);
		Physics_ActivateNeighbours(x, y, z, index);
	}
}

static void Physics_HandleMushroom(int index, BlockID block) {
	BlockID below;
	int x, y, z;
	World_Unpack(index, x, y, z);

	if (Lighting.IsLit(x, y, z)) {
		Game_UpdateBlock(x, y, z, BLOCK_AIR);
		Physics_ActivateNeighbours(x, y, z, index);
		return;
	}

	below = BLOCK_STONE;
	if (y > 0) below = World.Blocks[index - World.OneY];
	if (!(below == BLOCK_STONE || below == BLOCK_COBBLE)) {
		Game_UpdateBlock(x, y, z, BLOCK_AIR);
		Physics_ActivateNeighbours(x, y, z, index);
	}
}


static void Physics_PlaceLava(int index, BlockID block) {
	TickQueue_Enqueue(&lavaQ, PHYSICS_LAVA_DELAY_NOW | index);
}

static void Physics_PropagateLava(int posIndex, int x, int y, int z) {
	BlockID block = World.Blocks[posIndex];

	/* Indev lava ignites flammable blocks it tries to flow against
	    (BlockFlowing/BlockFluid's fireSpread) instead of flowing */
	if (IndevFire_LavaFlowInto(x, y, z)) return;

	if (block >= BLOCK_WATER && block <= BLOCK_STILL_LAVA) {
		/* Lava spreading into water turns the water solid */
		if (block == BLOCK_WATER || block == BLOCK_STILL_WATER) {
			Game_UpdateBlock(x, y, z, BLOCK_STONE);
		}
	} else if (Blocks.Draw[block] == DRAW_GAS) {
		TickQueue_Enqueue(&lavaQ, PHYSICS_LAVA_DELAY_NOW | posIndex);
		Game_UpdateBlock(x, y, z, BLOCK_LAVA);
	}
}

static void Physics_ActivateLava(int index, BlockID block) {
	int x, y, z;
	World_Unpack(index, x, y, z);

	if (x > 0)          Physics_PropagateLava(index - 1, x - 1, y, z);
	if (x < World.MaxX) Physics_PropagateLava(index + 1, x + 1, y, z);
	if (z > 0)          Physics_PropagateLava(index - World.Width, x, y, z - 1);
	if (z < World.MaxZ) Physics_PropagateLava(index + World.Width, x, y, z + 1);
	if (y > 0)          Physics_PropagateLava(index - World.OneY, x, y - 1, z);
}

static void Physics_TickLava(void) {
	int i, count = lavaQ.count;
	for (i = 0; i < count; i++) {
		int index;
		if (Physics_CheckItem(&lavaQ, &index)) {
			BlockID block = World.Blocks[index];
			if (!(block == BLOCK_LAVA || block == BLOCK_STILL_LAVA)) continue;
			Physics_ActivateLava(index, block);
		}
	}
}


static void Physics_PlaceWater(int index, BlockID block) {
	TickQueue_Enqueue(&waterQ, PHYSICS_WATER_DELAY | index);
}

static void Physics_PropagateWater(int posIndex, int x, int y, int z) {
	BlockID block = World.Blocks[posIndex];
	int xx, yy, zz;

	if (block >= BLOCK_WATER && block <= BLOCK_STILL_LAVA) {
		/* Water spreading into lava turns the lava solid */
		if (block == BLOCK_LAVA || block == BLOCK_STILL_LAVA) {
			Game_UpdateBlock(x, y, z, BLOCK_STONE);
		}
	} else if (Blocks.Draw[block] == DRAW_GAS) {
		/* Sponge check */		
		for (yy = (y < 2 ? 0 : y - 2); yy <= (y > physics_maxWaterY ? World.MaxY : y + 2); yy++) {
			for (zz = (z < 2 ? 0 : z - 2); zz <= (z > physics_maxWaterZ ? World.MaxZ : z + 2); zz++) {
				for (xx = (x < 2 ? 0 : x - 2); xx <= (x > physics_maxWaterX ? World.MaxX : x + 2); xx++) {
					block = World_GetBlock(xx, yy, zz);
					if (block == BLOCK_SPONGE) return;
				}
			}
		}

		TickQueue_Enqueue(&waterQ, PHYSICS_WATER_DELAY | posIndex);
		Game_UpdateBlock(x, y, z, BLOCK_WATER);
	}
}

static void Physics_ActivateWater(int index, BlockID block) {
	int x, y, z;
	World_Unpack(index, x, y, z);

	if (x > 0)          Physics_PropagateWater(index - 1,           x - 1, y,     z);
	if (x < World.MaxX) Physics_PropagateWater(index + 1,           x + 1, y,     z);
	if (z > 0)          Physics_PropagateWater(index - World.Width, x,     y,     z - 1);
	if (z < World.MaxZ) Physics_PropagateWater(index + World.Width, x,     y,     z + 1);
	if (y > 0)          Physics_PropagateWater(index - World.OneY,  x,     y - 1, z);
}

static void Physics_TickWater(void) {
	int i, count = waterQ.count;
	for (i = 0; i < count; i++) {
		int index;
		if (Physics_CheckItem(&waterQ, &index)) {
			BlockID block = World.Blocks[index];
			if (!(block == BLOCK_WATER || block == BLOCK_STILL_WATER)) continue;
			Physics_ActivateWater(index, block);
		}
	}
}


static void Physics_PlaceSponge(int index, BlockID block) {
	int x, y, z, xx, yy, zz;
	World_Unpack(index, x, y, z);

	for (yy = y - 2; yy <= y + 2; yy++) {
		for (zz = z - 2; zz <= z + 2; zz++) {
			for (xx = x - 2; xx <= x + 2; xx++) {
				if (!World_Contains(xx, yy, zz)) continue;

				block = World_GetBlock(xx, yy, zz);
				if (block == BLOCK_WATER || block == BLOCK_STILL_WATER) {
					Game_UpdateBlock(xx, yy, zz, BLOCK_AIR);
				}
			}
		}
	}
}

static void Physics_DeleteSponge(int index, BlockID block) {
	int x, y, z, xx, yy, zz;
	World_Unpack(index, x, y, z);

	for (yy = y - 3; yy <= y + 3; yy++) {
		for (zz = z - 3; zz <= z + 3; zz++) {
			for (xx = x - 3; xx <= x + 3; xx++) {
				if (Math_AbsI(yy - y) == 3 || Math_AbsI(zz - z) == 3 || Math_AbsI(xx - x) == 3) {
					if (!World_Contains(xx, yy, zz)) continue;

					index = World_Pack(xx, yy, zz);
					block = World.Blocks[index];
					if (block == BLOCK_WATER || block == BLOCK_STILL_WATER) {
						TickQueue_Enqueue(&waterQ, index | PHYSICS_ONE_DELAY);
					}
				}
			}
		}
	}
}


static void Physics_HandleSlab(int index, BlockID block) {
	int x, y, z;
	World_Unpack(index, x, y, z);
	if (index < World.OneY) return;

	if (World.Blocks[index - World.OneY] != BLOCK_SLAB) return;
	Game_UpdateBlock(x, y,     z, BLOCK_AIR);
	Game_UpdateBlock(x, y - 1, z, BLOCK_DOUBLE_SLAB);
}

static void Physics_HandleCobblestoneSlab(int index, BlockID block) {
	int x, y, z;
	World_Unpack(index, x, y, z);
	if (index < World.OneY) return;

	if (World.Blocks[index - World.OneY] != BLOCK_COBBLE_SLAB) return;
	Game_UpdateBlock(x, y,     z, BLOCK_AIR);
	Game_UpdateBlock(x, y - 1, z, BLOCK_COBBLE);
}


/* TODO: should this be moved into a precomputed lookup table, instead of calculating every time? */
/*  performance difference probably isn't enough to really matter */
static cc_bool BlocksTNT(BlockID b) {
	/* NOTE: A bit hacky, but works well enough */
	return (b >= BLOCK_WATER && b <= BLOCK_STILL_LAVA) || 
		(Blocks.ExtendedCollide[b] == COLLIDE_SOLID && (Blocks.DigSounds[b] == SOUND_METAL || Blocks.DigSounds[b] == SOUND_STONE));
}

#define TNT_POWER 4
#define TNT_POWER_SQUARED (TNT_POWER * TNT_POWER)
static void Physics_HandleTnt(int index, BlockID block) {
	int x, y, z;
	int dx, dy, dz, xx, yy, zz;

	/* TNTPhysics.onPlace is a no-op in Survival Test - placing TNT there */
	/*  does nothing special, only mining an already-placed TNT block ignites */
	/*  a fuse (see SurvivalTest_ArmTnt). This instant-explode-on-place is a */
	/*  separate, older classic-multiplayer feature that survival must skip. */
	if (SurvivalTest_Enabled) return;

	World_Unpack(index, x, y, z);
	Game_UpdateBlock(x, y, z, BLOCK_AIR);
	Physics_ActivateNeighbours(x, y, z, index);
	
	for (dy = -TNT_POWER; dy <= TNT_POWER; dy++) {
		for (dz = -TNT_POWER; dz <= TNT_POWER; dz++) {
			for (dx = -TNT_POWER; dx <= TNT_POWER; dx++) {
				if (dx * dx + dy * dy + dz * dz > TNT_POWER_SQUARED) continue;

				xx = x + dx; yy = y + dy; zz = z + dz;
				if (!World_Contains(xx, yy, zz)) continue;
				index = World_Pack(xx, yy, zz);

				block = World.Blocks[index];
				if (BlocksTNT(block)) continue;

				Game_UpdateBlock(xx, yy, zz, BLOCK_AIR);
				Physics_ActivateNeighbours(xx, yy, zz, index);
			}
		}
	}
}

void Physics_Init(void) {
	Event_Register_(&WorldEvents.MapLoaded,    NULL, Physics_OnNewMapLoaded);
	Physics.Enabled = Options_GetBool(OPT_BLOCK_PHYSICS, true);
	TickQueue_Init(&lavaQ);
	TickQueue_Init(&waterQ);

	Physics.OnPlace[BLOCK_SAND]        = Physics_DoFalling;
	Physics.OnPlace[BLOCK_GRAVEL]      = Physics_DoFalling;
	Physics.OnActivate[BLOCK_SAND]     = Physics_DoFalling;
	Physics.OnActivate[BLOCK_GRAVEL]   = Physics_DoFalling;
	Physics.OnRandomTick[BLOCK_SAND]   = Physics_DoFalling;
	Physics.OnRandomTick[BLOCK_GRAVEL] = Physics_DoFalling;

	Physics.OnRandomTick[BLOCK_SAPLING] = Physics_HandleSapling;
	Physics.OnRandomTick[BLOCK_DIRT]    = Physics_HandleDirt;
	Physics.OnRandomTick[BLOCK_GRASS]   = Physics_HandleGrass;

	Physics.OnRandomTick[BLOCK_DANDELION]    = Physics_HandleFlower;
	Physics.OnRandomTick[BLOCK_ROSE]         = Physics_HandleFlower;
	Physics.OnRandomTick[BLOCK_RED_SHROOM]   = Physics_HandleMushroom;
	Physics.OnRandomTick[BLOCK_BROWN_SHROOM] = Physics_HandleMushroom;

	Physics.OnPlace[BLOCK_LAVA]    = Physics_PlaceLava;
	Physics.OnPlace[BLOCK_WATER]   = Physics_PlaceWater;
	Physics.OnPlace[BLOCK_SPONGE]  = Physics_PlaceSponge;
	Physics.OnDelete[BLOCK_SPONGE] = Physics_DeleteSponge;

	Physics.OnActivate[BLOCK_WATER]       = Physics.OnPlace[BLOCK_WATER];
	Physics.OnActivate[BLOCK_STILL_WATER] = Physics.OnPlace[BLOCK_WATER];
	Physics.OnActivate[BLOCK_LAVA]        = Physics.OnPlace[BLOCK_LAVA];
	Physics.OnActivate[BLOCK_STILL_LAVA]  = Physics.OnPlace[BLOCK_LAVA];

	Physics.OnRandomTick[BLOCK_WATER]       = Physics_ActivateWater;
	Physics.OnRandomTick[BLOCK_STILL_WATER] = Physics_ActivateWater;
	Physics.OnRandomTick[BLOCK_LAVA]        = Physics_ActivateLava;
	Physics.OnRandomTick[BLOCK_STILL_LAVA]  = Physics_ActivateLava;

	Physics.OnPlace[BLOCK_SLAB]        = Physics_HandleSlab;
	if (Game_ClassicMode) return;
	Physics.OnPlace[BLOCK_COBBLE_SLAB] = Physics_HandleCobblestoneSlab;
	Physics.OnPlace[BLOCK_TNT]         = Physics_HandleTnt;
}

void Physics_Free(void) {
	Event_Unregister_(&WorldEvents.MapLoaded,    NULL, Physics_OnNewMapLoaded);
}

void Physics_Tick(void) {
	if (!Physics.Enabled || !World.Blocks) return;

	/*if ((tickCount % 5) == 0) {*/
	Physics_TickLava();
	Physics_TickWater();
	/*}*/
	physics_tickCount++;
	/* Indev mode uses the genuine World.tick rate (volume/200 random
	    updates per tick) - this loop's 3-per-chunk is ~6.8x sparser,
	    which visibly stalled crop growth and farmland moisture. c0.30
	    and creative keep the engine loop untouched. */
	if (IndevTest_Enabled) {
		/* MP: the server owns ALL Indev world simulation (growth, leaf decay,
		    fire, finite fluids - SurvivalGrowth/SurvivalPhysics) and streams
		    every change as a SetBlock. The client must not run it locally or it
		    would double up and diverge. Ambient display ticks (fire crackle,
		    lava embers) are a separate path (IndevTest_RandomDisplayTicks) and
		    keep running. */
		if (SurvivalNet_ServerDriven()) return;
		IndevFire_Tick(); /* the scheduled-update list runs before random ticks */
		IndevTest_TickFluids();
		IndevTest_TickRandomBlocks();
	} else if (SurvivalTest_Enabled) {
		/* c0.30 survival: the genuine volume/200 Level.tick rate */
		Physics_TickRandomBlocksC030();
	} else {
		Physics_TickRandomBlocks();
	}
}
