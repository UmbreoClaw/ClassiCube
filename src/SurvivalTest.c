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
#include "Graphics.h"
#include "Platform.h"
#include "Lighting.h"
#include "TexturePack.h"
#include "Physics.h"
#include "Audio.h"
#include "Funcs.h"
#include "Model.h"

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
/* Defined later, in the Ticking section - forward declared so the Mobs */
/*  section below (which ticks before Ticking is reached) can reuse it. */
static cc_bool SurvivalTest_IsHeadInWater(struct Entity* e);


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
/* Survival Test items spin about Y at 3 degrees/tick = 60 deg/sec (20 TPS) */
#define DROP_SPIN_DEG_PER_SEC 60.0f
/* The spin angle also drives the bob and white-glow pulse, exactly as the */
/*  original Item.render did (var3 advances the spin, sin(var3/10) the rest) */

/* Survival Test's ItemModel is a separate, small hardcoded cube (-2..2 in */
/*  1/16-scale model units = a 0.25-block cube), textured with only the */
/*  middle 50% of the block's tile (u/v 0.25..0.75) on every face - NOT a */
/*  full-size block. (Decompiled ItemModel.java, confirmed across multiple */
/*  independent Survival Test source ports.) */
#define DROP_ITEM_HALF 0.125f

struct DropItem {
	Vec3 position;
	Vec3 velocity;
	BlockID block;
	float pickupDelay;
	float age;       /* seconds alive - drives spin/bob/glow */
	float rot0;      /* random initial spin angle (degrees) */
	cc_bool active;
};
static struct DropItem st_drops[DROP_MAX];

/* Vertex buffer for the textured item cubes */
#define ITEM_VERTICES_PER_DROP 24
#define ITEM_MAX_VERTICES (DROP_MAX * ITEM_VERTICES_PER_DROP)
static GfxResourceID st_itemVB;
static cc_uint16 item_1DCount[ATLAS1D_MAX_ATLASES];
static cc_uint16 item_1DIndices[ATLAS1D_MAX_ATLASES];

/* Untextured white glow shell, drawn additively over each item to give the */
/*  brief Survival Test "twinkle". Needs its own vertex buffer/format since */
/*  it has no texture - it's just a flat colour, alpha-blended over the item. */
#define GLOW_VERTICES_PER_DROP 24
#define GLOW_MAX_VERTICES (DROP_MAX * GLOW_VERTICES_PER_DROP)
static GfxResourceID st_glowVB;

/* Computes the spin/bob/glow animation phase for a drop. var3 is the running */
/*  spin angle in degrees; sin(var3/10) (matching the original) drives bob+glow. */
static float DropItem_Phase(struct DropItem* d) {
	return d->rot0 + d->age * DROP_SPIN_DEG_PER_SEC;
}

/* Survival Test redrew the item in additive white once per ~second for a */
/*  brief glint. NOTE: lerping the item's own lit colour towards white is a */
/*  no-op in full daylight (the lit colour is already pure white there), so */
/*  that approach was invisible outdoors. Instead this drives the alpha of a */
/*  separate white glow shell (see DropItem_BuildGlowCube), which genuinely */
/*  brightens the item regardless of how bright its lit colour already is. */
static float DropItem_GlowAmount(struct DropItem* d) {
	float s = Math_SinF(DropItem_Phase(d) / 10.0f) * 0.5f + 0.5f; /* 0..1 */
	s = s * s * s * s;       /* ^4, matching the decompiled glow curve exactly */
	return s * 0.4f;         /* max alpha 0.4, matching the decompiled glColor4f(1,1,1,g*0.4) */
}

/* Drops are lit by the world like in Survival Test (darker in shade); */
/*  the white pulse is a separate additive pass, not a tint here. */
static PackedCol DropItem_WorldColor(Vec3* pos) {
	int x = Math_Floor(pos->x);
	int y = Math_Floor(pos->y);
	int z = Math_Floor(pos->z);
	return Lighting.Color(x, y, z);
}

/* Computes the 4 rotated XZ corners (A=--, B=+-, C=++, D=-+) of a square of */
/*  the given half-size, centred at (cx,cz) and spun by the drop's current */
/*  spin angle - shared by both the item cube and its glow shell so they */
/*  always line up with each other. */
static void DropItem_RotatedCorners(float half, float cx, float cz, float angleRad,
									 Vec3* a, Vec3* b, Vec3* c, Vec3* d) {
	float cosA = Math_CosF(angleRad), sinA = Math_SinF(angleRad);
	a->x = cx + (-half) * cosA - (-half) * sinA; a->z = cz + (-half) * sinA + (-half) * cosA;
	b->x = cx + ( half) * cosA - (-half) * sinA; b->z = cz + ( half) * sinA + (-half) * cosA;
	c->x = cx + ( half) * cosA - ( half) * sinA; c->z = cz + ( half) * sinA + ( half) * cosA;
	d->x = cx + (-half) * cosA - ( half) * sinA; d->z = cz + (-half) * sinA + ( half) * cosA;
}

/* Computes the world-space geometry (top/bottom Y and the 4 spun XZ corners) */
/*  shared by the item cube, its glow shell, and the sprite-quad drop variant. */
static void DropItem_ComputeGeometry(struct DropItem* d, float* yLo, float* yHi,
									  Vec3* a, Vec3* b, Vec3* c, Vec3* e) {
	float var3 = DropItem_Phase(d);
	float bob  = Math_SinF(var3 / 10.0f) * 0.1f + 0.1f;
	*yLo = d->position.y + bob;
	*yHi = *yLo + DROP_ITEM_HALF * 2.0f;

	DropItem_RotatedCorners(DROP_ITEM_HALF, d->position.x, d->position.z,
							 var3 * MATH_DEG2RAD, a, b, c, e);
}

/* Appends the 6-face, 24-vertex textured cube for one drop (cropped to the */
/*  given UV rect on every face, matching the decompiled ItemModel exactly). */
static void DropItem_BuildItemCube(struct DropItem* d, TextureRec rec, PackedCol col,
									struct VertexTextured** vertices) {
	struct VertexTextured* v = *vertices;
	float yLo, yHi;
	float u1 = rec.u1, v1 = rec.v1, u2 = rec.u2, v2 = rec.v2;
	Vec3 a, b, c, e;

	DropItem_ComputeGeometry(d, &yLo, &yHi, &a, &b, &c, &e);

	#define ITEM_V(p, py, uu, vv) v->x = (p).x; v->y = (py); v->z = (p).z; v->Col = col; v->U = (uu); v->V = (vv); v++;
	ITEM_V(a,yLo, u1,v1) ITEM_V(b,yLo, u2,v1) ITEM_V(c,yLo, u2,v2) ITEM_V(e,yLo, u1,v2) /* bottom */
	ITEM_V(a,yHi, u1,v1) ITEM_V(e,yHi, u2,v1) ITEM_V(c,yHi, u2,v2) ITEM_V(b,yHi, u1,v2) /* top    */
	ITEM_V(a,yLo, u1,v1) ITEM_V(a,yHi, u2,v1) ITEM_V(b,yHi, u2,v2) ITEM_V(b,yLo, u1,v2) /* side AB */
	ITEM_V(e,yLo, u1,v1) ITEM_V(c,yLo, u2,v1) ITEM_V(c,yHi, u2,v2) ITEM_V(e,yHi, u1,v2) /* side DC */
	ITEM_V(a,yLo, u1,v1) ITEM_V(e,yLo, u2,v1) ITEM_V(e,yHi, u2,v2) ITEM_V(a,yHi, u1,v2) /* side AD */
	ITEM_V(b,yLo, u1,v1) ITEM_V(b,yHi, u2,v1) ITEM_V(c,yHi, u2,v2) ITEM_V(c,yLo, u1,v2) /* side BC */
	#undef ITEM_V
	*vertices = v;
}

/* Appends the 24-vertex untextured glow shell for one drop - identical */
/*  geometry to the item cube, but flat white with alpha-blended glow */
/*  intensity baked into the vertex colour's alpha channel. Drawn with face */
/*  culling on (the cube's winding is consistent, see DropItem_RotatedCorners */
/*  callers) so only the front faces blend - without culling, the unseen back */
/*  faces would also blend in, doubling up and producing a boxy flash. */
static void DropItem_BuildGlowCube(struct DropItem* d, PackedCol col,
									struct VertexColoured** vertices) {
	struct VertexColoured* v = *vertices;
	float yLo, yHi;
	Vec3 a, b, c, e;

	DropItem_ComputeGeometry(d, &yLo, &yHi, &a, &b, &c, &e);

	#define GLOW_V(p, py) v->x = (p).x; v->y = (py); v->z = (p).z; v->Col = col; v++;
	GLOW_V(a,yLo) GLOW_V(b,yLo) GLOW_V(c,yLo) GLOW_V(e,yLo) /* bottom   */
	GLOW_V(a,yHi) GLOW_V(e,yHi) GLOW_V(c,yHi) GLOW_V(b,yHi) /* top      */
	GLOW_V(a,yLo) GLOW_V(a,yHi) GLOW_V(b,yHi) GLOW_V(b,yLo) /* side AB  */
	GLOW_V(e,yLo) GLOW_V(c,yLo) GLOW_V(c,yHi) GLOW_V(e,yHi) /* side DC  */
	GLOW_V(a,yLo) GLOW_V(e,yLo) GLOW_V(e,yHi) GLOW_V(a,yHi) /* side AD  */
	GLOW_V(b,yLo) GLOW_V(b,yHi) GLOW_V(c,yHi) GLOW_V(c,yLo) /* side BC  */
	#undef GLOW_V
	*vertices = v;
}

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
	float ang, speed;
	int slot = SurvivalTest_FindFreeDropSlot();
	if (slot < 0) return; /* drop limit reached - oldest drops simply aren't replaced */

	d = &st_drops[slot];
	Mem_Set(d, 0, sizeof(struct DropItem));

	d->position.x = coords.x + 0.5f;
	d->position.y = coords.y + 0.3f;
	d->position.z = coords.z + 0.5f;

	ang   = Random_Float(&st_dropRng) * 2.0f * MATH_PI;
	speed = 0.6f + Random_Float(&st_dropRng) * 0.6f;
	d->velocity.x = Math_CosF(ang) * speed;
	d->velocity.z = Math_SinF(ang) * speed;
	d->velocity.y = 2.5f + Random_Float(&st_dropRng) * 1.0f;

	d->block       = block;
	d->pickupDelay = DROP_PICKUP_DELAY;
	d->age         = 0.0f;
	d->rot0        = Random_Float(&st_dropRng) * 360.0f;
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
	float groundY;
	int x, y, z;

	d->velocity.y -= DROP_GRAVITY * delta;
	if (d->velocity.y < -DROP_TERMINAL_VEL) d->velocity.y = -DROP_TERMINAL_VEL;

	d->position.x += d->velocity.x * delta;
	d->position.y += d->velocity.y * delta;
	d->position.z += d->velocity.z * delta;

	x = Math_Floor(d->position.x);
	z = Math_Floor(d->position.z);
	y = Math_Floor(d->position.y - 0.01f);
	groundY = SurvivalTest_DropGroundY(x, y, z);

	if (groundY > -1000.0f && d->position.y <= groundY) {
		d->position.y = groundY;
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

	diff.x = d->position.x - pe->Position.x;
	diff.y = d->position.y - pe->Position.y;
	diff.z = d->position.z - pe->Position.z;
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

/* Updates how many vertices belong to each 1D atlas, for batching draws */
/*  (each drop's tile can land in a different 1D atlas / GL texture). */
static void SurvivalTest_UpdateItem1DCounts(void) {
	int i, index;
	for (i = 0; i < Atlas1D.Count; i++) { item_1DCount[i] = 0; item_1DIndices[i] = 0; }

	for (i = 0; i < DROP_MAX; i++) {
		if (!st_drops[i].active) continue;
		index = Atlas1D_Index(Block_Tex(st_drops[i].block, FACE_XMIN));
		item_1DCount[index] += ITEM_VERTICES_PER_DROP;
	}
	for (i = 1; i < Atlas1D.Count; i++) {
		item_1DIndices[i] = item_1DIndices[i - 1] + item_1DCount[i - 1];
	}
}

/* Renders the lit, textured item cubes - cropped to the middle 50% of the */
/*  block's tile on every face, spinning about Y and bobbing up/down, with a */
/*  brief white glint (~1 Hz) applied as a colour lerp toward white. */
static void SurvivalTest_RenderDropBlocks(void) {
	struct DropItem* d;
	struct VertexTextured* data;
	struct VertexTextured* ptr;
	struct VertexColoured* glowData;
	struct VertexColoured* glowPtr;
	TextureLoc loc;
	TextureRec base, rec;
	PackedCol col, glowCol;
	float du, dv;
	int i, index, texIndex, offset, glowCount;
	cc_bool any = false;

	for (i = 0; i < DROP_MAX; i++) { if (st_drops[i].active) { any = true; break; } }
	if (!any) return;

	if (!st_itemVB) {
		st_itemVB = Gfx_CreateDynamicVb(VERTEX_FORMAT_TEXTURED, ITEM_MAX_VERTICES);
		if (!st_itemVB) return;
	}
	if (!st_glowVB) {
		st_glowVB = Gfx_CreateDynamicVb(VERTEX_FORMAT_COLOURED, GLOW_MAX_VERTICES);
		if (!st_glowVB) return;
	}

	SurvivalTest_UpdateItem1DCounts();

	/* PASS 1 - the lit textured item cubes. NOTE: only ONE dynamic VB may be */
	/*  locked at a time (the backend hands out a single shared scratch buffer */
	/*  and the unlock uploads it), so the item VB must be fully locked, built, */
	/*  unlocked and drawn before the glow VB is touched. The unlock also binds */
	/*  its VB, so each pass's draw reads from the right buffer. */
	data = (struct VertexTextured*)Gfx_LockDynamicVb(st_itemVB, VERTEX_FORMAT_TEXTURED, ITEM_MAX_VERTICES);
	for (i = 0; i < DROP_MAX; i++) {
		d = &st_drops[i];
		if (!d->active) continue;

		loc   = Block_Tex(d->block, FACE_XMIN);
		index = Atlas1D_Index(loc);
		ptr   = data + item_1DIndices[index];

		/* Crop to the middle 50% (texels 4..12 of 16) of the tile, on every face */
		base = Atlas1D_TexRec(loc, 1, &texIndex);
		du = (base.u2 - base.u1) * 0.25f;
		dv = (base.v2 - base.v1) * 0.25f;
		rec.u1 = base.u1 + du; rec.u2 = base.u2 - du;
		rec.v1 = base.v1 + dv; rec.v2 = base.v2 - dv;

		col = DropItem_WorldColor(&d->position);
		DropItem_BuildItemCube(d, rec, col, &ptr);
		item_1DIndices[index] += ITEM_VERTICES_PER_DROP;
	}
	Gfx_UnlockDynamicVb(st_itemVB);

	Gfx_SetVertexFormat(VERTEX_FORMAT_TEXTURED);
	Gfx_SetAlphaTest(true);
	offset = 0;
	for (i = 0; i < Atlas1D.Count; i++) {
		int vCount = item_1DCount[i];
		if (!vCount) continue;

		Atlas1D_Bind(i);
		Gfx_DrawVb_IndexedTris_Range(vCount, offset, DRAW_HINT_NONE);
		offset += vCount;
	}
	Gfx_SetAlphaTest(false);

	/* PASS 2 - the white glow shell, drawn over the items with alpha blending */
	/*  and face culling (so only front faces blend, no boxy double-blend). */
	glowData = (struct VertexColoured*)Gfx_LockDynamicVb(st_glowVB, VERTEX_FORMAT_COLOURED, GLOW_MAX_VERTICES);
	glowPtr  = glowData;
	glowCount = 0;
	for (i = 0; i < DROP_MAX; i++) {
		d = &st_drops[i];
		if (!d->active) continue;

		glowCol = PackedCol_Make(255, 255, 255, (cc_uint8)(255.0f * DropItem_GlowAmount(d)));
		DropItem_BuildGlowCube(d, glowCol, &glowPtr);
		glowCount += GLOW_VERTICES_PER_DROP;
	}
	Gfx_UnlockDynamicVb(st_glowVB);

	if (glowCount > 0) {
		Gfx_SetVertexFormat(VERTEX_FORMAT_COLOURED);
		Gfx_SetFaceCulling(true);
		Gfx_SetDepthWrite(false);
		Gfx_SetAlphaBlendingAdditive(true);
		Gfx_DrawVb_IndexedTris_Range(glowCount, 0, DRAW_HINT_NONE);
		Gfx_SetAlphaBlendingAdditive(false);
		Gfx_SetDepthWrite(true);
		Gfx_SetFaceCulling(false);
	}
}

void SurvivalTest_RenderDrops(float delta, float t) {
	if (!SurvivalTest_Enabled) return;
	SurvivalTest_RenderDropBlocks();
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
*---------------------------------------------------------Mobs-------------------------------------------------------------*
*#########################################################################################################################*/
/* Survival Test (c0.30-s) populates the world with hostile and passive mobs */
/*  (decompiled from Mob.java/BasicAI.java/BasicAttackAI.java/JumpAttackAI.java */
/*  and the per-species Zombie/Skeleton/Pig/Creeper/Spider classes). Mobs are */
/*  simulated client-side in a fixed array, the same way dropped items are -  */
/*  they are NOT real Entities.List[]/NetPlayer entries, just enough of an */
/*  Entity to reuse the model/animation/collision systems. */
#define MOB_MAX            32
#define MOB_MAX_HEALTH     20  /* Mob.java's default health - same scale as the player's */
#define MOB_INVINC_TICKS   20  /* simplified flat invincibility window (Mob.invulnerableDuration) */
#define MOB_AIR_TICKS     300  /* 15 seconds @ 20 TPS, matches Mob.airSupply */
#define MOB_EXPLODE_RADIUS  4  /* Creeper.beforeRemove's level.explode radius */

enum MobType {
	MOB_TYPE_ZOMBIE, MOB_TYPE_SKELETON, MOB_TYPE_PIG, MOB_TYPE_CREEPER, MOB_TYPE_SPIDER, MOB_TYPE_SHEEP,
	MOB_TYPE_COUNT
};
/* The 3 broad AI behaviours found in the decompiled source - which of these */
/*  a mob uses is entirely determined by its type (see mobTypeInfo below). */
enum MobAI { MOB_AI_PASSIVE, MOB_AI_ATTACK, MOB_AI_JUMPATTACK };

struct MobTypeInfo {
	const char* model;
	cc_uint8    ai;
	float       runSpeed;
	float       defaultLookAngle;
	int         damage;
	cc_bool     isCreeper; /* self-damages 6 HP per attack and explodes on death */
};
/* Order matches MobSpawner.spawn's `type = random.nextInt(6)` exactly, so */
/*  Mob_SpawnerRun can index straight into this table with that roll. */
static const struct MobTypeInfo mobTypeInfo[MOB_TYPE_COUNT] = {
	/* ZOMBIE   */ { "zombie",   MOB_AI_ATTACK,     1.00f, 30.0f, 6, false },
	/* SKELETON */ { "skeleton", MOB_AI_ATTACK,     0.30f,  0.0f, 8, false },
	/* PIG      */ { "pig",      MOB_AI_PASSIVE,    0.70f,  0.0f, 0, false },
	/* CREEPER  */ { "creeper",  MOB_AI_ATTACK,     0.70f, 45.0f, 6, true  },
	/* SPIDER   */ { "spider",   MOB_AI_JUMPATTACK, 0.56f,  0.0f, 6, false },
	/* SHEEP    */ { "sheep",    MOB_AI_PASSIVE,    0.70f,  0.0f, 0, false },
};

struct Mob {
	struct Entity Base;
	struct CollisionsComp Collisions;
	cc_uint8 type;
	cc_bool  active;
	cc_bool  hasTarget;
	cc_bool  jumping;

	int health;
	int invincTicks; /* simplified single-threshold version of Mob.invulnerableTime */
	int hurtTicks;    /* red hit-flash timer, purely cosmetic (Mob.hurtTime) */
	int attackDelay;  /* cooldown before this mob can attack again (BasicAttackAI.attackDelay) */
	int deathTicks;   /* ticks since health reached 0 - removed once this exceeds 20 */
	int airTicks;     /* underwater air supply (Mob.airSupply) */

	/* BasicAI's wander/chase input axes and turn impulse - decayed every */
	/*  tick and refreshed at random, exactly as in the decompiled source. */
	float moveStrafe, moveForward, turnRate;
};
static struct Mob st_mobs[MOB_MAX];
static RNGState st_mobRng;

static int SurvivalTest_CountMobs(void) {
	int i, n = 0;
	for (i = 0; i < MOB_MAX; i++) { if (st_mobs[i].active) n++; }
	return n;
}

static int SurvivalTest_FindFreeMobSlot(void) {
	int i;
	for (i = 0; i < MOB_MAX; i++) { if (!st_mobs[i].active) return i; }
	return -1;
}

/* Adds the world-space velocity for one tick of relative (forward/strafe) */
/*  input, exactly matching the decompiled Mob.moveRelative/Entity.moveRelative */
/*  formula (and re-derived/verified against PhysicsComp_MoveHor, which uses */
/*  the identical normalise-then-scale pattern for the local player). */
static void Mob_MoveRelative(struct Entity* e, float strafe, float forward, float friction) {
	float dist = strafe * strafe + forward * forward;
	float sinYaw, cosYaw;
	if (dist < 0.0001f) return;

	dist = Math_SqrtF(dist);
	if (dist < 1.0f) dist = 1.0f;
	dist     = friction / dist;
	strafe  *= dist;
	forward *= dist;

	sinYaw = Math_SinF(e->Yaw * MATH_DEG2RAD);
	cosYaw = Math_CosF(e->Yaw * MATH_DEG2RAD);
	/* CC's own forward/strafe basis (re-derived from LocalPlayer_Tick + */
	/*  PlayerInputNormal), NOT a literal port of Java's yRot-based formula - */
	/*  ClassiCube's Yaw convention is mirrored relative to Java's yRot. */
	e->Velocity.x += forward * sinYaw + strafe * cosYaw;
	e->Velocity.z += strafe  * sinYaw - forward * cosYaw;
}

/* Ground/air movement model - exactly Mob.travel()'s final `else` branch. */
/*  (Its constants - 0.91/0.98/0.91 drag, 0.08 gravity, 0.6/0.6 ground */
/*  friction - are exactly PhysicsComp_Init's constants; both derive from */
/*  the same original Minecraft source.) */
static void Mob_TravelGround(struct Mob* m, float forward, float strafe) {
	struct Entity* e = &m->Base;
	float friction = e->OnGround ? 0.1f : 0.02f;

	Mob_MoveRelative(e, strafe, forward, friction);
	Collisions_MoveAndWallSlide(&m->Collisions);
	Vec3_AddBy(&e->Position, &e->Velocity);

	e->Velocity.x *= 0.91f;
	e->Velocity.y *= 0.98f;
	e->Velocity.z *= 0.91f;
	e->Velocity.y -= 0.08f;

	if (e->OnGround) {
		e->Velocity.x *= 0.6f;
		e->Velocity.z *= 0.6f;
	}
}

/* Water/lava movement model - Mob.travel()'s water/lava branches, which */
/*  are identical apart from the drag factor (0.8 water, 0.5 lava). The */
/*  exact `isFree` paddle-up-stairs assist wasn't ported (needs a generic */
/*  collision probe this codebase doesn't expose) - approximated here with */
/*  a simple upward nudge when blocked, which is enough to stop mobs getting */
/*  permanently stuck against underwater terrain. */
static void Mob_TravelLiquid(struct Mob* m, float forward, float strafe, float drag) {
	struct Entity* e = &m->Base;

	Mob_MoveRelative(e, strafe, forward, 0.02f);
	Collisions_MoveAndWallSlide(&m->Collisions);
	Vec3_AddBy(&e->Position, &e->Velocity);

	e->Velocity.x *= drag;
	e->Velocity.y *= drag;
	e->Velocity.z *= drag;
	e->Velocity.y -= 0.02f;

	if (Collisions_HitHorizontal(&m->Collisions)) e->Velocity.y = 0.3f;
}

static void Mob_Travel(struct Mob* m, cc_bool inWater, cc_bool inLava) {
	if (inWater)      Mob_TravelLiquid(m, m->moveForward, m->moveStrafe, 0.8f);
	else if (inLava)  Mob_TravelLiquid(m, m->moveForward, m->moveStrafe, 0.5f);
	else              Mob_TravelGround(m, m->moveForward, m->moveStrafe);
}

/* BasicAI's jump dispatch: a held/random "jumping" intent only actually */
/*  does anything once on the ground (or paddles upward in liquid). Spiders */
/*  using JumpAttackAI instead lunge forward when jumping with a target */
/*  (matches JumpAttackAI.jumpFromGround's attackTarget != null branch). */
static void Mob_DoJump(struct Mob* m, cc_bool inWater, cc_bool inLava) {
	struct Entity* e = &m->Base;
	if (!m->jumping) return;

	if (inWater || inLava) {
		e->Velocity.y += 0.04f;
	} else if (e->OnGround) {
		if (m->type == MOB_TYPE_SPIDER && m->hasTarget) {
			e->Velocity.x = 0.0f;
			e->Velocity.z = 0.0f;
			Mob_MoveRelative(e, 0.0f, 1.0f, 0.6f);
			e->Velocity.y = 0.5f;
		} else {
			e->Velocity.y = 0.42f;
		}
	}
}

/* Spawns 1-2 brown mushrooms at the mob's position. (int)(rand+rand+1.0) */
/*  mathematically only ever yields 1 or 2 - never the "1-3" some ports guess. */
static void Mob_SpawnPigDrops(struct Mob* m) {
	IVec3 coords;
	int count = (int)(Random_Float(&st_mobRng) + Random_Float(&st_mobRng) + 1.0f);
	int i;

	coords.x = Math_Floor(m->Base.Position.x);
	coords.y = Math_Floor(m->Base.Position.y);
	coords.z = Math_Floor(m->Base.Position.z);
	for (i = 0; i < count; i++) { SurvivalTest_SpawnDrop(coords, BLOCK_BROWN_SHROOM); }
}

/* die(Entity) - called the instant health reaches 0 (separate from the mob's */
/*  20-tick removal delay, which is handled in SurvivalTest_TickOneMob). */
static void Mob_Die(struct Mob* m) {
	if (m->type == MOB_TYPE_PIG) Mob_SpawnPigDrops(m);
}

/* Mirrors BlockPhysics.c's private BlocksTNT immunity check (liquids and */
/*  metal/stone-sounding solid blocks survive blasts) - duplicated here since */
/*  that function isn't exposed outside BlockPhysics.c. */
static cc_bool Mob_ExplosionImmune(BlockID b) {
	return (b >= BLOCK_WATER && b <= BLOCK_STILL_LAVA) ||
		(Blocks.ExtendedCollide[b] == COLLIDE_SOLID && (Blocks.DigSounds[b] == SOUND_METAL || Blocks.DigSounds[b] == SOUND_STONE));
}

/* Creeper.beforeRemove's level.explode call, fired once the creeper's 20-tick */
/*  death animation finishes (it dies from its own repeated headbutt damage - */
/*  see Mob_Hurt). Block-destruction radius/shape matches TNT exactly (both */
/*  derive from the same original explosion code); the exact player-damage */
/*  falloff curve wasn't recovered, so a simple linear falloff is used. */
static void Mob_CreeperExplode(struct Mob* m) {
	struct Entity* e = &m->Base;
	struct LocalPlayer* p = Entities.CurPlayer;
	int x = Math_Floor(e->Position.x);
	int y = Math_Floor(e->Position.y);
	int z = Math_Floor(e->Position.z);
	int dx, dy, dz, xx, yy, zz;
	BlockID block;
	Vec3 diff;
	float dist;

	for (dy = -MOB_EXPLODE_RADIUS; dy <= MOB_EXPLODE_RADIUS; dy++) {
	for (dz = -MOB_EXPLODE_RADIUS; dz <= MOB_EXPLODE_RADIUS; dz++) {
	for (dx = -MOB_EXPLODE_RADIUS; dx <= MOB_EXPLODE_RADIUS; dx++) {
		if (dx * dx + dy * dy + dz * dz > MOB_EXPLODE_RADIUS * MOB_EXPLODE_RADIUS) continue;
		xx = x + dx; yy = y + dy; zz = z + dz;
		if (!World_Contains(xx, yy, zz)) continue;

		block = World_GetBlock(xx, yy, zz);
		if (block == BLOCK_AIR || Mob_ExplosionImmune(block)) continue;
		Game_UpdateBlock(xx, yy, zz, BLOCK_AIR);
	}}}

	if (!p) return;
	diff.x = p->Base.Position.x - e->Position.x;
	diff.y = p->Base.Position.y - e->Position.y;
	diff.z = p->Base.Position.z - e->Position.z;
	dist   = Math_SqrtF(diff.x * diff.x + diff.y * diff.y + diff.z * diff.z);
	if (dist < (float)MOB_EXPLODE_RADIUS) {
		int dmg = (int)((1.0f - dist / (float)MOB_EXPLODE_RADIUS) * MOB_MAX_HEALTH * 0.6f);
		SurvivalTest_Hurt(dmg);
	}
}

/* hurt(Entity attacker, int damage) - simplified to a single flat */
/*  invincibility window rather than porting Mob.java's dual-threshold */
/*  invulnerableTime mechanic (matches the player's own damage code, which */
/*  already uses the same simplification). knockback() pushes the mob */
/*  directly away from its attacker; aggroes attack-type mobs onto whoever */
/*  hit them (BasicAttackAI.hurt). */
static void Mob_Hurt(struct Mob* m, struct Entity* attacker, int damage) {
	struct Entity* e = &m->Base;
	float dx, dz, dist;

	if (m->health <= 0)        return;
	if (m->invincTicks > 0)    return;
	if (damage <= 0)           return;

	m->health      -= damage;
	m->invincTicks  = MOB_INVINC_TICKS;
	m->hurtTicks    = 10;

	if (attacker && mobTypeInfo[m->type].ai != MOB_AI_PASSIVE) m->hasTarget = true;

	if (attacker) {
		dx   = attacker->Position.x - e->Position.x;
		dz   = attacker->Position.z - e->Position.z;
		dist = Math_SqrtF(dx * dx + dz * dz);
		if (dist >= 0.0001f) {
			e->Velocity.x = e->Velocity.x / 2.0f - (dx / dist) * 0.4f;
			e->Velocity.z = e->Velocity.z / 2.0f - (dz / dist) * 0.4f;
		}
		e->Velocity.y = e->Velocity.y / 2.0f + 0.4f;
		if (e->Velocity.y > 0.4f) e->Velocity.y = 0.4f;
	}

	if (m->health <= 0) {
		m->health = 0;
		Mob_Die(m);
	}
}

/* BasicAI.update() - the shared wander/turn logic used by every mob, plus */
/*  the chase override applied once a mob has acquired a target (only ever */
/*  true for attack-type mobs - BasicAttackAI is what actually sets hasTarget). */
static void Mob_BasicAIUpdate(struct Mob* m, cc_bool inWater, cc_bool inLava) {
	const struct MobTypeInfo* info = &mobTypeInfo[m->type];
	struct Entity* e = &m->Base;

	if (Random_Next(&st_mobRng, 100) < 7) {
		m->moveStrafe  = (Random_Float(&st_mobRng) - 0.5f) * info->runSpeed;
		m->moveForward =  Random_Float(&st_mobRng)         * info->runSpeed;
	}
	m->jumping = Random_Next(&st_mobRng, 100) < 1;

	if (Random_Next(&st_mobRng, 100) < 4) {
		m->turnRate = (Random_Float(&st_mobRng) - 0.5f) * 60.0f;
	}
	e->Yaw  += m->turnRate;
	e->Pitch = info->defaultLookAngle;

	if (m->hasTarget) {
		m->moveForward = info->runSpeed;
		m->jumping = Random_Next(&st_mobRng, 100) < 4;
		if (inWater || inLava) m->jumping = Random_Next(&st_mobRng, 100) < 80;
	}
}

/* BasicAttackAI.doAttack() - acquires/loses the player as a target based on */
/*  distance, faces them, and lands a hit once in range and off cooldown. */
/*  Facing uses CC's own atan2-based formula (re-derived/verified against */
/*  Entity_GetEyePosition/Vec3_GetDirVector), not Java's raw yRot formula. */
static void Mob_DoAttack(struct Mob* m) {
	const struct MobTypeInfo* info = &mobTypeInfo[m->type];
	struct Entity* e = &m->Base;
	struct LocalPlayer* p = Entities.CurPlayer;
	struct Entity* pe;
	Vec3 diff;
	float distSq, horDist;
	int damage;

	if (!p) return;
	pe = &p->Base;

	diff.x = pe->Position.x - e->Position.x;
	diff.y = pe->Position.y - e->Position.y;
	diff.z = pe->Position.z - e->Position.z;
	distSq = diff.x * diff.x + diff.y * diff.y + diff.z * diff.z;

	if (!m->hasTarget && distSq <= 256.0f) m->hasTarget = true; /* aggroRange = 16 */
	if (!m->hasTarget) return;

	if (distSq > 1024.0f && Random_Next(&st_mobRng, 100) == 0) { /* 2x aggroRange give-up roll */
		m->hasTarget = false;
		return;
	}

	horDist  = Math_SqrtF(diff.x * diff.x + diff.z * diff.z);
	e->Yaw   = Math_Atan2f(-diff.z, diff.x) * MATH_RAD2DEG;
	e->Pitch = Math_Atan2f(horDist, -diff.y) * MATH_RAD2DEG;

	if (distSq < 4.0f && m->attackDelay <= 0) {
		m->attackDelay = 10 + Random_Next(&st_mobRng, 20); /* 10-29 ticks (0.5-1.45s) */
		damage = (int)((Random_Float(&st_mobRng) + Random_Float(&st_mobRng)) / 2.0f * info->damage + 1.0f);
		SurvivalTest_Hurt(damage);

		/* Creeper$1.attack: headbutting the player also hurts the creeper - */
		/*  after ~4 hits this kills it and triggers its death explosion. */
		if (info->isCreeper) Mob_Hurt(m, NULL, 6);
	}
}

static void SurvivalTest_TickOneMob(struct Mob* m, float delta) {
	const struct MobTypeInfo* info;
	struct Entity* e = &m->Base;
	Vec3 oldPos;
	cc_bool inWater, inLava;

	if (!m->active) return;
	info = &mobTypeInfo[m->type];

	if (m->invincTicks > 0) m->invincTicks--;
	if (m->hurtTicks   > 0) m->hurtTicks--;

	if (m->health <= 0) {
		m->deathTicks++;
		if (m->deathTicks > 20) {
			if (info->isCreeper) Mob_CreeperExplode(m);
			m->active = false;
			return;
		}
	}

	inWater = Entity_TouchesAnyWater(e);
	inLava  = Entity_TouchesAnyLava(e);

	/* Environmental damage - Mob.tick()'s airSupply/lava handling. Both */
	/*  damage calls go through the same flat invincibility window as combat */
	/*  damage (see Mob_Hurt), so e.g. lava only actually ticks roughly once */
	/*  per second rather than truly every tick. */
	if (SurvivalTest_IsHeadInWater(e)) {
		if (m->airTicks > 0) { m->airTicks--; }
		else                 { Mob_Hurt(m, NULL, 2); }
	} else {
		m->airTicks = MOB_AIR_TICKS;
	}
	if (inLava) Mob_Hurt(m, NULL, 10);

	if (m->attackDelay > 0) m->attackDelay--;

	if (m->health <= 0) {
		/* BasicAI.tick's freeze branch: no more wandering/attacking, but */
		/*  gravity/physics below still run, so the body settles naturally. */
		m->jumping     = false;
		m->moveStrafe  = 0.0f;
		m->moveForward = 0.0f;
		m->turnRate    = 0.0f;
	} else {
		Mob_BasicAIUpdate(m, inWater, inLava);
		if (info->ai != MOB_AI_PASSIVE) Mob_DoAttack(m);
	}

	Mob_DoJump(m, inWater, inLava);

	m->moveStrafe  *= 0.98f;
	m->moveForward *= 0.98f;
	m->turnRate    *= 0.9f;

	oldPos = e->Position;
	Mob_Travel(m, inWater, inLava);
	AnimatedComp_Update(e, oldPos, e->Position, delta);
}

/* Ground-validity check shared by both the outer spawn-point roll and the */
/*  inner cluster jitter (MobSpawner.spawn's isSolidTile calls). */
static cc_bool Mob_BlockIsSolid(int x, int y, int z) {
	if (!World_Contains(x, y, z)) return true;
	return Blocks.Collide[World_GetBlock(x, y, z)] == COLLIDE_SOLID;
}

/* Duplicates the private Entity_GetColor (lighting at the entity's eye */
/*  position) since mobs aren't real Entities.List[] entries, plus a brief */
/*  red hit-flash while hurtTicks counts down from 10 (Mob.hurtTime), */
/*  blended into the lit colour rather than drawn as a separate pass. */
static PackedCol Mob_GetColor(struct Entity* e) {
	struct Mob* m = (struct Mob*)e; /* Base is the first field of struct Mob */
	Vec3 eyePos = Entity_GetEyePosition(e);
	IVec3 pos;
	PackedCol col;
	IVec3_Floor(&pos, &eyePos);
	col = Lighting.Color(pos.x, pos.y, pos.z);

	if (m->hurtTicks > 0) {
		float f  = m->hurtTicks / 10.0f;
		int   r  = PackedCol_R(col), g = PackedCol_G(col), b = PackedCol_B(col);
		r = (int)(r + (255 - r) * f);
		g = (int)(g * (1.0f - f));
		b = (int)(b * (1.0f - f));
		col = PackedCol_Make((cc_uint8)r, (cc_uint8)g, (cc_uint8)b, PackedCol_A(col));
	}
	return col;
}

/* Mobs are ticked/rendered by hand (SurvivalTest_TickOneMob/RenderMobs), */
/*  never through generic Entity dispatch - so only GetCol needs to be real, */
/*  since Model_SetupState calls it directly. The rest can stay NULL. */
static const struct EntityVTABLE mob_VTABLE = { NULL, NULL, NULL, Mob_GetColor, NULL, NULL };

static void SurvivalTest_SpawnMobAt(cc_uint8 type, Vec3 pos) {
	struct Mob* m;
	cc_string model;
	int slot = SurvivalTest_FindFreeMobSlot();
	if (slot < 0) return;

	m = &st_mobs[slot];
	Mem_Set(m, 0, sizeof(struct Mob));
	Entity_Init(&m->Base);
	m->Base.VTABLE = &mob_VTABLE;

	model = String_FromReadonly(mobTypeInfo[type].model);
	Entity_SetModel(&m->Base, &model);

	m->Base.Position = pos;
	m->Base.Yaw      = Random_Float(&st_mobRng) * 360.0f;

	m->Collisions.Entity   = &m->Base;
	m->Collisions.StepSize = 0.5f; /* matches LocalPlayer's default step size */

	m->type     = type;
	m->health   = MOB_MAX_HEALTH;
	m->airTicks = MOB_AIR_TICKS;
	m->active   = true;
}

/* MobSpawner.spawn - for each of `count` attempts, picks a random point */
/*  (Y biased toward low altitude via min-of-two-uniforms) and, if valid, */
/*  scatters a small cluster of up to 9 mobs of the same random type around */
/*  it, skipping any that land too close to avoidPos but still consuming */
/*  the jitter step (a faithfully-preserved quirk of the original). */
#define MOB_SPAWN_MIN_DIST_SQ 256.0f /* 16 blocks */
static void Mob_SpawnerRun(int count, Vec3* avoidPos) {
	int attempt, outer, inner;
	int x, y, z, cx, cy, cz;
	cc_uint8 type;
	Vec3 candidate;
	float dx, dy, dz, distSq;

	for (attempt = 0; attempt < count; attempt++) {
		type = (cc_uint8)Random_Next(&st_mobRng, MOB_TYPE_COUNT);
		x    = Random_Next(&st_mobRng, World.Width);
		y    = (int)(min(Random_Float(&st_mobRng), Random_Float(&st_mobRng)) * World.Height);
		z    = Random_Next(&st_mobRng, World.Length);

		if (Mob_BlockIsSolid(x, y, z)) continue;
		if (Blocks.Collide[World_GetBlock(x, y, z)] == COLLIDE_LIQUID) continue;
		if (Lighting.IsLit(x, y, z) && Random_Next(&st_mobRng, 5) != 0) continue;

		for (outer = 0; outer < 3; outer++) {
			cx = x; cy = y; cz = z;

			for (inner = 0; inner < 3; inner++) {
				cx += Random_Next(&st_mobRng, 6) - Random_Next(&st_mobRng, 6);
				cz += Random_Next(&st_mobRng, 6) - Random_Next(&st_mobRng, 6);
				/* NOTE: the original's vertical jitter is rand(1)-rand(1), */
				/*  which is always 0 - cy is faithfully never adjusted here. */

				if (cx < 0 || cz < 1 || cy < 0 || cy >= World.Height - 2 ||
					cx >= World.Width || cz >= World.Length) continue;
				if (!Mob_BlockIsSolid(cx, cy - 1, cz))   continue;
				if (Mob_BlockIsSolid(cx, cy, cz))        continue;
				if (Mob_BlockIsSolid(cx, cy + 1, cz))    continue;

				candidate.x = cx + 0.5f;
				candidate.y = (float)(cy + 1);
				candidate.z = cz + 0.5f;

				dx = candidate.x - avoidPos->x;
				dy = candidate.y - avoidPos->y;
				dz = candidate.z - avoidPos->z;
				distSq = dx * dx + dy * dy + dz * dz;
				if (distSq < MOB_SPAWN_MIN_DIST_SQ) continue;

				SurvivalTest_SpawnMobAt(type, candidate);
			}
		}
	}
}

/* SurvivalGameMode.spawnMob() - the periodic per-tick spawn gate. */
static void SurvivalTest_TrySpawnMobs(void) {
	cc_int64 volume = (cc_int64)World.Width * World.Height * World.Length;
	int area = (int)(volume / 64 / 64 / 64);
	struct LocalPlayer* p = Entities.CurPlayer;
	if (!p || area <= 0) return;

	if (Random_Next(&st_mobRng, 100) < area && SurvivalTest_CountMobs() < area * 20) {
		Mob_SpawnerRun(area, &p->Base.Position);
	}
}

/* SurvivalGameMode.prepareLevel() - the one-time initial population done */
/*  when a new map finishes loading, avoiding the player's spawn point. */
static void SurvivalTest_SpawnInitialMobs(void) {
	struct LocalPlayer* p = Entities.CurPlayer;
	cc_int64 volume;
	int area;
	if (!p) return;

	volume = (cc_int64)World.Width * World.Height * World.Length;
	area   = (int)(volume / 800);
	if (area <= 0) return;

	Mob_SpawnerRun(area, &p->Spawn);
}

static void SurvivalTest_TickMobs(float delta) {
	int i;
	for (i = 0; i < MOB_MAX; i++) { SurvivalTest_TickOneMob(&st_mobs[i], delta); }
	SurvivalTest_TrySpawnMobs();
}


void SurvivalTest_RenderMobs(float delta, float t) {
	struct Mob* m;
	struct Entity* e;
	int i;
	if (!SurvivalTest_Enabled) return;

	for (i = 0; i < MOB_MAX; i++) {
		m = &st_mobs[i];
		if (!m->active) continue;
		e = &m->Base;

		AnimatedComp_GetCurrent(e, t);
		e->ShouldRender = Model_ShouldRender(e);
		if (!e->ShouldRender) continue;

		Model_Render(e->Model, e);
	}
}

/* The player's melee attack - casts a ray along the view direction (exactly */
/*  PerspectiveCamera_GetPickedBlock/Entities_GetClosest's pattern) and hits */
/*  the closest mob within reach, using the same rotated-box intersection */
/*  test the engine already uses for picking other entities. */
cc_bool SurvivalTest_TryAttackMob(void) {
	struct LocalPlayer* p;
	struct Entity* e;
	struct Mob* best = NULL;
	struct Mob* m;
	Vec3 eyePos, dir;
	float t0, t1, bestT = 1.0e30f;
	int i;

	if (!SurvivalTest_Enabled) return false;
	p = Entities.CurPlayer;
	if (!p) return false;
	e = &p->Base;

	eyePos = Entity_GetEyePosition(e);
	dir    = Vec3_GetDirVector(e->Yaw * MATH_DEG2RAD, e->Pitch * MATH_DEG2RAD);

	for (i = 0; i < MOB_MAX; i++) {
		m = &st_mobs[i];
		if (!m->active || m->health <= 0) continue;
		if (!Intersection_RayIntersectsRotatedBox(eyePos, dir, &m->Base, &t0, &t1)) continue;
		if (t0 > p->ReachDistance) continue;
		if (t0 < bestT) { bestT = t0; best = m; }
	}
	if (!best) return false;

	/* Player fist: flat 4 HP/hit, matching SurvivalTest_Hurt's own player-damage figure */
	Mob_Hurt(best, e, 4);
	return true;
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

	/* Mobs ----------------------------------------------------------------- */
	SurvivalTest_TickMobs(delta);
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
	for (i = 0; i < MOB_MAX; i++) {
		st_mobs[i].active = false;
	}
}

/* The item vertex buffer is a GPU resource and must be dropped/recreated */
/*  whenever the graphics context is lost (it is rebuilt lazily on render). */
static void SurvivalTest_OnContextLost(void* obj) {
	Gfx_DeleteDynamicVb(&st_itemVB);
	Gfx_DeleteDynamicVb(&st_glowVB);
}

static void SurvivalTest_Init(void) {
	SurvivalTest_Enabled = Options_GetBool(OPT_SURVIVAL_MODE, false);
	if (!SurvivalTest_Enabled) return;

	Random_SeedFromCurrentTime(&st_dropRng);
	Random_SeedFromCurrentTime(&st_mobRng);
	SurvivalTest_ResetState();
	ScheduledTask_Add(GAME_DEF_TICKS, SurvivalTest_Tick);
	Event_Register_(&UserEvents.BlockChanged, NULL, SurvivalTest_BlockChanged);
	Event_Register_(&GfxEvents.ContextLost,   NULL, SurvivalTest_OnContextLost);
}

static void SurvivalTest_Free(void) {
	if (!SurvivalTest_Enabled) return;
	Event_Unregister_(&UserEvents.BlockChanged, NULL, SurvivalTest_BlockChanged);
	Event_Unregister_(&GfxEvents.ContextLost,   NULL, SurvivalTest_OnContextLost);
	Gfx_DeleteDynamicVb(&st_itemVB);
	Gfx_DeleteDynamicVb(&st_glowVB);
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

	SurvivalTest_SpawnInitialMobs();
}

struct IGameComponent SurvivalTest_Component = {
	SurvivalTest_Init,          /* Init           */
	SurvivalTest_Free,          /* Free           */
	NULL,                       /* Reset          */
	SurvivalTest_OnNewMap,      /* OnNewMap       */
	SurvivalTest_OnNewMapLoaded /* OnNewMapLoaded */
};
