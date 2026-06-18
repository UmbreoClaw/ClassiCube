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
/* Defined later, in the Arrows section - forward declared so the Mobs */
/*  section below (skeletons firing/death-bursting arrows) can spawn them. */
static void SurvivalTest_SpawnArrow(Vec3 pos, float yaw, float pitch, float force,
									 int damage, cc_uint8 type, cc_bool ownerIsPlayer, int ownerMobSlot);
/* Defined later, in the TNT section - forward declared so the Drops section */
/*  above (which decides what mining a TNT block does) can ignite its fuse. */
static void SurvivalTest_ArmTnt(IVec3 coords);


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
	case BLOCK_TNT:
		/* TNTBlock.getDropCount()==0 - mining TNT never yields an item, it */
		/*  ignites a fuse instead (TNTPhysics.onBreak spawns a PrimedTnt). */
		SurvivalTest_ArmTnt(coords);
		return;
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

/* level.explode's blast radius - shared by TNT and the creeper's death blast */
/*  (both derive from the same original explosion code). */
#define EXPLOSION_RADIUS 4

/* Mirrors BlockPhysics.c's private BlocksTNT immunity check (liquids and */
/*  metal/stone-sounding solid blocks survive blasts) - duplicated here since */
/*  that function isn't exposed outside BlockPhysics.c. */
static cc_bool SurvivalTest_ExplosionImmune(BlockID b) {
	return (b >= BLOCK_WATER && b <= BLOCK_STILL_LAVA) ||
		(Blocks.ExtendedCollide[b] == COLLIDE_SOLID && (Blocks.DigSounds[b] == SOUND_METAL || Blocks.DigSounds[b] == SOUND_STONE));
}

/* level.explode(null, x, y, z, radius) - destroys a sphere of blocks around */
/*  center and damages the player with linear falloff if within range. Used */
/*  by both TNT (PrimedTnt's expiry) and the creeper's death blast. The exact */
/*  player-damage falloff curve wasn't recovered, so a simple linear falloff */
/*  is used. */
static void SurvivalTest_Explode(Vec3 center, int radius) {
	struct LocalPlayer* p = Entities.CurPlayer;
	int x = Math_Floor(center.x);
	int y = Math_Floor(center.y);
	int z = Math_Floor(center.z);
	int dx, dy, dz, xx, yy, zz;
	BlockID block;
	Vec3 diff;
	float dist;

	for (dy = -radius; dy <= radius; dy++) {
	for (dz = -radius; dz <= radius; dz++) {
	for (dx = -radius; dx <= radius; dx++) {
		if (dx * dx + dy * dy + dz * dz > radius * radius) continue;
		xx = x + dx; yy = y + dy; zz = z + dz;
		if (!World_Contains(xx, yy, zz)) continue;

		block = World_GetBlock(xx, yy, zz);
		if (block == BLOCK_AIR || SurvivalTest_ExplosionImmune(block)) continue;
		Game_UpdateBlock(xx, yy, zz, BLOCK_AIR);
	}}}

	if (!p) return;
	diff.x = p->Base.Position.x - center.x;
	diff.y = p->Base.Position.y - center.y;
	diff.z = p->Base.Position.z - center.z;
	dist   = Math_SqrtF(diff.x * diff.x + diff.y * diff.y + diff.z * diff.z);
	if (dist < (float)radius) {
		int dmg = (int)((1.0f - dist / (float)radius) * SURVIVAL_MAX_HEALTH * 0.6f);
		SurvivalTest_Hurt(dmg);
	}
}


/*########################################################################################################################*
*----------------------------------------------------------TNT-------------------------------------------------------------*
*#########################################################################################################################*/
/* PrimedTnt.java: mining a placed TNT block (TNTPhysics.onBreak, since */
/*  TNTBlock.getDropCount()==0) doesn't explode it instantly - it ignites a */
/*  fuse, then explodes life=40 ticks (2 seconds @ 20 TPS) later. Placing */
/*  TNT does nothing special (TNTPhysics.onPlace is a no-op); that's only */
/*  BlockPhysics.c's separate, older classic-multiplayer "place TNT to */
/*  explode instantly" feature, which Physics_HandleTnt now skips while in */
/*  survival mode. PrimedTnt is really a separate flashing/falling entity in */
/*  the original, but the block is simply left in the world ticking down */
/*  here instead, to avoid needing a whole new entity-rendering path. */
#define TNT_MAX        8
#define TNT_FUSE_TICKS 40 /* PrimedTnt's default life */

struct TntFuse { IVec3 coords; int ticksLeft; cc_bool active; };
static struct TntFuse st_tnt[TNT_MAX];

/* PrimedTnt.hurt(): hitting an already-lit TNT (mining it again) destroys it */
/*  without exploding, dropping a normal pickup item instead - so mining is a */
/*  way to "defuse" TNT, at the cost of losing it back into your inventory. */
static cc_bool SurvivalTest_DefuseTnt(IVec3 coords) {
	int i;
	for (i = 0; i < TNT_MAX; i++) {
		if (!st_tnt[i].active) continue;
		if (st_tnt[i].coords.x != coords.x || st_tnt[i].coords.y != coords.y || st_tnt[i].coords.z != coords.z) continue;

		st_tnt[i].active = false;
		SurvivalTest_SpawnDrop(coords, BLOCK_TNT);
		return true;
	}
	return false;
}

static void SurvivalTest_ArmTnt(IVec3 coords) {
	int i, slot = -1;
	if (SurvivalTest_DefuseTnt(coords)) return;

	for (i = 0; i < TNT_MAX; i++) {
		if (!st_tnt[i].active) { slot = i; break; }
	}
	if (slot < 0) return; /* no free slot - block just vanishes, untracked */

	st_tnt[slot].coords    = coords;
	st_tnt[slot].ticksLeft = TNT_FUSE_TICKS;
	st_tnt[slot].active    = true;

	/* InputHandler_DeleteBlock already cleared this to air - restore it */
	/*  without raising BlockChanged, which would otherwise make */
	/*  SurvivalTest_BlockChanged think the player just placed it. */
	Game_UpdateBlock(coords.x, coords.y, coords.z, BLOCK_TNT);
}

static void SurvivalTest_TickTnt(void) {
	int i;
	Vec3 center;

	for (i = 0; i < TNT_MAX; i++) {
		if (!st_tnt[i].active) continue;
		if (--st_tnt[i].ticksLeft > 0) continue;

		st_tnt[i].active = false;
		Game_UpdateBlock(st_tnt[i].coords.x, st_tnt[i].coords.y, st_tnt[i].coords.z, BLOCK_AIR);

		center.x = st_tnt[i].coords.x + 0.5f;
		center.y = st_tnt[i].coords.y + 0.5f;
		center.z = st_tnt[i].coords.z + 0.5f;
		SurvivalTest_Explode(center, EXPLOSION_RADIUS);
	}
}

/* PrimedTnt.render()'s flashing white overlay: redrawn over the model with */
/*  additive alpha blending that pulses slowly at first (every ~8 ticks) and */
/*  speeds up to every other tick once life<=16, finishing almost solid white */
/*  for the last 2 ticks. ticksLeft plays the role of PrimedTnt.life here. */
static float TntFuse_GlowAlpha(int ticksLeft) {
	float alpha = (float)((ticksLeft / 4 + 1) % 2) * 0.4f;
	if (ticksLeft <= 16) alpha = (float)((ticksLeft + 1) % 2) * 0.6f;
	if (ticksLeft <= 2)  alpha = 0.9f;
	return alpha;
}

/* Untextured white glow shell, drawn additively over a lit TNT block - same */
/*  technique as the dropped-item twinkle (DropItem_BuildGlowCube), just an */
/*  axis-aligned full block instead of a small spinning item cube. */
#define TNT_GLOW_VERTICES_PER_BLOCK 24
#define TNT_GLOW_MAX_VERTICES (TNT_MAX * TNT_GLOW_VERTICES_PER_BLOCK)
static GfxResourceID st_tntGlowVB;

static void TntFuse_BuildGlowCube(IVec3 coords, PackedCol col, struct VertexColoured** vertices) {
	struct VertexColoured* v = *vertices;
	float x0 = (float)coords.x, x1 = x0 + 1.0f;
	float y0 = (float)coords.y, y1 = y0 + 1.0f;
	float z0 = (float)coords.z, z1 = z0 + 1.0f;

	#define TNT_GLOW_V(px, py, pz) v->x = (px); v->y = (py); v->z = (pz); v->Col = col; v++;
	TNT_GLOW_V(x0,y0,z0) TNT_GLOW_V(x1,y0,z0) TNT_GLOW_V(x1,y0,z1) TNT_GLOW_V(x0,y0,z1) /* bottom */
	TNT_GLOW_V(x0,y1,z0) TNT_GLOW_V(x0,y1,z1) TNT_GLOW_V(x1,y1,z1) TNT_GLOW_V(x1,y1,z0) /* top    */
	TNT_GLOW_V(x0,y0,z0) TNT_GLOW_V(x0,y1,z0) TNT_GLOW_V(x1,y1,z0) TNT_GLOW_V(x1,y0,z0) /* side z0 */
	TNT_GLOW_V(x0,y0,z1) TNT_GLOW_V(x1,y0,z1) TNT_GLOW_V(x1,y1,z1) TNT_GLOW_V(x0,y1,z1) /* side z1 */
	TNT_GLOW_V(x0,y0,z0) TNT_GLOW_V(x0,y0,z1) TNT_GLOW_V(x0,y1,z1) TNT_GLOW_V(x0,y1,z0) /* side x0 */
	TNT_GLOW_V(x1,y0,z0) TNT_GLOW_V(x1,y1,z0) TNT_GLOW_V(x1,y1,z1) TNT_GLOW_V(x1,y0,z1) /* side x1 */
	#undef TNT_GLOW_V
	*vertices = v;
}

void SurvivalTest_RenderTnt(float delta, float t) {
	struct VertexColoured* data;
	struct VertexColoured* ptr;
	PackedCol col;
	int i, count = 0;
	cc_bool any = false;

	if (!SurvivalTest_Enabled) return;
	for (i = 0; i < TNT_MAX; i++) { if (st_tnt[i].active) { any = true; break; } }
	if (!any) return;

	if (!st_tntGlowVB) {
		st_tntGlowVB = Gfx_CreateDynamicVb(VERTEX_FORMAT_COLOURED, TNT_GLOW_MAX_VERTICES);
		if (!st_tntGlowVB) return;
	}

	data = (struct VertexColoured*)Gfx_LockDynamicVb(st_tntGlowVB, VERTEX_FORMAT_COLOURED, TNT_GLOW_MAX_VERTICES);
	ptr  = data;
	for (i = 0; i < TNT_MAX; i++) {
		if (!st_tnt[i].active) continue;

		col = PackedCol_Make(255, 255, 255, (cc_uint8)(255.0f * TntFuse_GlowAlpha(st_tnt[i].ticksLeft)));
		TntFuse_BuildGlowCube(st_tnt[i].coords, col, &ptr);
		count += TNT_GLOW_VERTICES_PER_BLOCK;
	}
	Gfx_UnlockDynamicVb(st_tntGlowVB);
	if (!count) return;

	Gfx_SetVertexFormat(VERTEX_FORMAT_COLOURED);
	Gfx_SetFaceCulling(true);
	Gfx_SetDepthWrite(false);
	Gfx_SetAlphaBlendingAdditive(true);
	Gfx_DrawVb_IndexedTris_Range(count, 0, DRAW_HINT_NONE);
	Gfx_SetAlphaBlendingAdditive(false);
	Gfx_SetDepthWrite(true);
	Gfx_SetFaceCulling(false);
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
	int noActionTime; /* ticks since last hurt/successful attack - drives the despawn roll below */

	/* BasicAI's wander/chase input axes and turn impulse - decayed every */
	/*  tick and refreshed at random, exactly as in the decompiled source. */
	float moveStrafe, moveForward, turnRate;

	/* Fall damage tracking (Mob.causeFallDamage), mirrors the player's */
	/*  st_falling/st_fallPeakY pair but per-mob since several can be */
	/*  airborne at once. Unlike the player, mobs have no prev/next double- */
	/*  buffered position (Mob.Base.Position is mutated directly, never reset */
	/*  for interpolation), so reading it straight after Mob_Travel is safe. */
	cc_bool falling;
	float   fallPeakY;
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

/* Skeleton.shootArrow() - looses an arrow at the skeleton's current target. */
/*  Decompiled Skeleton.java: yRot+180+(random()*45-22.5) for yaw, but */
/*  xRot-(random()*45-10.0) for pitch - NOT a symmetric +-22.5 like yaw, it's */
/*  an asymmetric (-35, +10] spread biased toward less-downward/more-upward */
/*  shots. The yaw "+180" is purely an artifact of Java's yRot/Arrow-velocity */
/*  sign convention (verified by tracing Arrow's constructor trig through to */
/*  cancellation with BasicAttackAI's yRot formula) and isn't needed here - */
/*  e->Yaw/e->Pitch already face the target directly via Mob_DoAttack's own */
/*  CC-convention atan2 formula - but the asymmetric pitch *shape* is a real */
/*  gameplay detail, not a convention artifact, so it's ported as-is (CC's */
/*  Pitch is also positive-down, like Java's xRot, so the sign carries over */
/*  unchanged). */
static void Mob_ShootArrow(struct Mob* m) {
	struct Entity* e = &m->Base;
	float yaw   = e->Yaw   + (Random_Float(&st_mobRng) * 45.0f - 22.5f);
	float pitch = e->Pitch - (Random_Float(&st_mobRng) * 45.0f - 10.0f);
	int slot    = (int)(m - st_mobs);

	/* damage=3, type=1 (mob-fired): Arrow's constructor picks these whenever */
	/*  the owner isn't a Player - the skeleton qualifies as a Mob owner here. */
	/* Skeleton.shootArrow spawns at the skeleton's own this.x/y/z - its base */
	/*  (feet) position, not eye height - so e->Position is used, not */
	/*  Entity_GetEyePosition (unlike the melee-attack ray in Mob_DoAttack's */
	/*  player counterpart, SurvivalTest_TryAttackMob, which is a genuinely */
	/*  different camera-ray mechanic). */
	SurvivalTest_SpawnArrow(e->Position, yaw, pitch, 1.0f, 3, 1, false, slot);
}

/* SkeletonAI.beforeRemove() - a parting burst of 4-9 pickupable arrows */
/*  scattered above the corpse as it disappears (count = (int)((rand+rand)*3+4), */
/*  which only ever yields 4-9). Owned by the player (not the skeleton) purely */
/*  so they can be picked back up, exactly as in the decompiled source. */
static void Mob_SkeletonDeathBurst(struct Mob* m) {
	struct Entity* e = &m->Base;
	int count = (int)((Random_Float(&st_mobRng) + Random_Float(&st_mobRng)) * 3.0f + 4.0f);
	Vec3 pos  = e->Position;
	float yaw, pitch;
	int i;

	pos.y -= 0.2f;
	for (i = 0; i < count; i++) {
		yaw   = Random_Float(&st_mobRng) * 360.0f;
		pitch = -Random_Float(&st_mobRng) * 60.0f; /* always downward-biased, never upward */
		SurvivalTest_SpawnArrow(pos, yaw, pitch, 0.4f, 7, 0, true, -1);
	}
}

/* Creeper.beforeRemove's level.explode call, fired once the creeper's 20-tick */
/*  death animation finishes (it dies from its own repeated headbutt damage - */
/*  see Mob_Hurt). Shares its block-destruction/player-damage logic with TNT */
/*  via SurvivalTest_Explode, since both derive from the same original code. */
static void Mob_CreeperExplode(struct Mob* m) {
	SurvivalTest_Explode(m->Base.Position, EXPLOSION_RADIUS);
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
	m->noActionTime = 0; /* BasicAI.hurt: being hurt counts as "doing something" */

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
/* NOTE: Java's BasicAttackAI.attack() also does a level.clip() line-of-sight */
/*  check and aborts (no damage to either side) if a block is in the way; */
/*  that raycast-against-arbitrary-points isn't ported here, so a mob can */
/*  land a hit through a sufficiently thin wall if within the 2-block range. */
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

	/* atan2 argument order matters here: this must match Vec3_GetDirVector's */
	/*  convention (yaw=atan2(dx,-dz), pitch=atan2(-dy,horDist) - see the */
	/*  commented-out Vec3_GetHeading in Vectors.c, its documented inverse) */
	/*  since both mob chase movement (via Mob_MoveRelative's sin/cos(Yaw) */
	/*  basis) and arrow aim (Mob_ShootArrow's Vec3_GetDirVector(Yaw,Pitch)) */
	/*  depend on Yaw/Pitch actually pointing at the target. The swapped-arg */
	/*  form atan2(-dz,dx)/atan2(horDist,-dy) looks similar but is wrong by */
	/*  up to 90 degrees whenever the target isn't at a 45-degree bearing. */
	horDist  = Math_SqrtF(diff.x * diff.x + diff.z * diff.z);
	e->Yaw   = Math_Atan2f(diff.x, -diff.z) * MATH_RAD2DEG;
	e->Pitch = Math_Atan2f(-diff.y, horDist) * MATH_RAD2DEG;

	if (distSq < 4.0f && m->attackDelay <= 0) {
		m->attackDelay  = 10 + Random_Next(&st_mobRng, 20); /* 10-29 ticks (0.5-1.45s) */
		m->noActionTime = 0; /* BasicAttackAI.attack: landing a hit also resets the despawn timer */
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
			if (info->isCreeper)              Mob_CreeperExplode(m);
			if (m->type == MOB_TYPE_SKELETON) Mob_SkeletonDeathBurst(m);
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
		m->noActionTime++;
		/* BasicAI.tick's despawn roll: once a mob has gone 600+ ticks without */
		/*  being hurt or landing a hit, each tick has a 1/800 chance to check */
		/*  whether the player is still nearby (32 blocks) - if so the timer is */
		/*  reset (so this only ever fires repeatedly while genuinely far away), */
		/*  otherwise the mob silently despawns. Without this, idle/far mobs */
		/*  would accumulate forever instead of being recycled like in Java. */
		if (m->noActionTime > 600 && Random_Next(&st_mobRng, 800) == 0) {
			struct LocalPlayer* dp = Entities.CurPlayer;
			if (dp) {
				float ddx = dp->Base.Position.x - e->Position.x;
				float ddy = dp->Base.Position.y - e->Position.y;
				float ddz = dp->Base.Position.z - e->Position.z;
				if (ddx * ddx + ddy * ddy + ddz * ddz < 1024.0f) {
					m->noActionTime = 0;
				} else {
					m->active = false;
					return;
				}
			}
		}

		Mob_BasicAIUpdate(m, inWater, inLava);
		if (info->ai != MOB_AI_PASSIVE) Mob_DoAttack(m);

		/* SkeletonAI.tick(): on top of (not instead of) the melee attack above, */
		/*  a skeleton with a target has a 1/30 per-tick chance to loose an arrow. */
		if (m->type == MOB_TYPE_SKELETON && m->hasTarget && Random_Next(&st_mobRng, 30) == 0) {
			Mob_ShootArrow(m);
		}
	}

	Mob_DoJump(m, inWater, inLava);

	m->moveStrafe  *= 0.98f;
	m->moveForward *= 0.98f;
	m->turnRate    *= 0.9f;

	oldPos = e->Position;
	Mob_Travel(m, inWater, inLava);
	AnimatedComp_Update(e, oldPos, e->Position, delta);

	/* Fall damage (Mob.causeFallDamage) - same peak-tracking approach as the */
	/*  player's SurvivalTest_UpdateFall, but using e->Position directly since */
	/*  it's already fresh here (no prev/next double-buffering for mobs). */
	/*  Touching liquid cushions the landing, same as for the player. */
	if (inWater || inLava) m->falling = false;

	if (e->OnGround) {
		if (m->falling) {
			float dist = m->fallPeakY - e->Position.y;
			if (dist > FALL_SAFE_BLOCKS) {
				int damage = (int)dist - (int)FALL_SAFE_BLOCKS;
				Mob_Hurt(m, NULL, damage);
			}
		}
		m->falling = false;
	} else {
		if (!m->falling) {
			m->falling   = true;
			m->fallPeakY = oldPos.y;
		}
		if (e->Position.y > m->fallPeakY) m->fallPeakY = e->Position.y;
	}
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
*--------------------------------------------------------Arrows-------------------------------------------------------------*
*#########################################################################################################################*/
/* Decompiled from item/Arrow.java. Like drops and mobs, arrows are simulated */
/*  client-side in a fixed array - never real Entities.List[] entries - and */
/*  are ticked/rendered entirely by hand. */
#define ARROW_MAX           64
#define ARROW_WIDTH        0.3f  /* Arrow's setSize(0.3F, 0.5F) */
#define ARROW_HEIGHT       0.5f
#define ARROW_SUBSTEP_LEN  0.2f  /* Arrow.tick's per-substep travel distance */
#define ARROW_DRAG       0.998f
#define ARROW_OWNER_GRACE_TICKS              5  /* can't hit the entity that fired it for this many ticks */
#define ARROW_STICK_MOB_TICKS                20 /* mob-fired (type 1) arrows always despawn 20 ticks after sticking */
#define ARROW_STICK_PLAYER_MIN_TICKS        300 /* player-fired (type 0) arrows are only eligible to despawn after this long */
#define ARROW_STICK_PLAYER_DESPAWN_CHANCE  0.01f /* ...and even then, only a 1% roll per tick */
#define ARROW_PLAYER_START                   20 /* Player.java: public int arrows = 20; */
#define ARROW_PLAYER_MAX                     99 /* Player.java: MAX_ARROWS = 99 */
#define ARROW_PLAYER_FIRE_FORCE            1.2f /* Minecraft.java's Tab-fire call */
#define ARROW_PLAYER_DAMAGE                   7 /* Arrow's constructor: damage=7 when the owner is a Player */
#define ARROW_VERTICES_PER_ARROW             24 /* 2 head quads + 4 shaft quads, 4 verts each */
#define ARROW_MAX_VERTICES (ARROW_MAX * ARROW_VERTICES_PER_ARROW)

struct ArrowEntity {
	Vec3 pos;        /* feet-equivalent anchor - same convention as Entity_GetBounds/AABB_Make */
	Vec3 velocity;
	Vec3 facing;     /* unit direction the arrow visually points; frozen once stuck in a block */
	float gravity;   /* Arrow.java: gravity = 1/force, scales the per-tick fall acceleration below */
	int   stickTime; /* ticks since hasHit became true */
	int   age;       /* ticks since spawn - gates the owner-exclusion window above */
	int   damage;
	cc_uint8 type;         /* 0 = player-type (slow despawn, texture rows 0-9), 1 = mob-type (fast despawn, rows 10-19) */
	cc_bool  hasHit;
	cc_bool  ownerIsPlayer;
	cc_int8  ownerMobSlot; /* index into st_mobs when fired by a mob, else -1 */
	cc_bool  active;
};
static struct ArrowEntity st_arrows[ARROW_MAX];
static RNGState st_arrowRng;
static int st_playerArrows = ARROW_PLAYER_START;
static GfxResourceID st_arrowVB;
static GfxResourceID st_arrowsTexId;

static int SurvivalTest_FindFreeArrowSlot(void) {
	int i;
	for (i = 0; i < ARROW_MAX; i++) { if (!st_arrows[i].active) return i; }
	return -1;
}

/* Arrow's constructor - spawns at pos, backed off slightly opposite the */
/*  firing direction (so the visible tip starts roughly at the eye/bow */
/*  rather than inside the shooter's head), with initial velocity along */
/*  yaw/pitch scaled by force. CC's own Vec3_GetDirVector is used to turn */
/*  yaw/pitch into a direction (matching Mob_DoAttack/SurvivalTest_TryAttackMob's */
/*  reuse of the same helper), rather than porting Java's yRot/xRot trig */
/*  literally - the two conventions don't agree on axis directions. */
static void SurvivalTest_SpawnArrow(Vec3 pos, float yaw, float pitch, float force,
									 int damage, cc_uint8 type, cc_bool ownerIsPlayer, int ownerMobSlot) {
	struct ArrowEntity* a;
	Vec3 dir;
	int slot = SurvivalTest_FindFreeArrowSlot();
	if (slot < 0) return;

	dir = Vec3_GetDirVector(yaw * MATH_DEG2RAD, pitch * MATH_DEG2RAD);

	a = &st_arrows[slot];
	Mem_Set(a, 0, sizeof(struct ArrowEntity));

	a->pos.x = pos.x - dir.x * 0.2f;
	a->pos.y = pos.y - dir.y * 0.2f;
	a->pos.z = pos.z - dir.z * 0.2f;

	a->velocity.x = dir.x * force;
	a->velocity.y = dir.y * force;
	a->velocity.z = dir.z * force;
	a->facing     = dir;
	a->gravity    = 1.0f / force;

	a->type          = type;
	a->damage        = damage;
	a->ownerIsPlayer = ownerIsPlayer;
	a->ownerMobSlot  = (cc_int8)ownerMobSlot;
	a->active        = true;
}

static void Arrow_BoxAt(Vec3* pos, struct AABB* out) {
	Vec3 size = { ARROW_WIDTH, ARROW_HEIGHT, ARROW_WIDTH };
	AABB_Make(out, pos, &size);
}

/* AABB.expand()'s semantics: grows whichever corner the signed delta points */
/*  towards, turning the box into the swept volume covered by one substep. */
static void Arrow_ExpandBox(struct AABB* bb, Vec3* d) {
	if (d->x > 0.0f) bb->Max.x += d->x; else bb->Min.x += d->x;
	if (d->y > 0.0f) bb->Max.y += d->y; else bb->Min.y += d->y;
	if (d->z > 0.0f) bb->Max.z += d->z; else bb->Min.z += d->z;
}

/* level.getCubes(box).size() > 0 - true if any solid block overlaps the box. */
static cc_bool Arrow_BlockCollision(struct AABB* bb) {
	int x0, x1, y0, y1, z0, z1, x, y, z;
	BlockID b;
	struct AABB blockBB;

	x0 = Math_Floor(bb->Min.x); x1 = Math_Floor(bb->Max.x);
	y0 = Math_Floor(bb->Min.y); y1 = Math_Floor(bb->Max.y);
	z0 = Math_Floor(bb->Min.z); z1 = Math_Floor(bb->Max.z);

	for (y = y0; y <= y1; y++) {
	for (z = z0; z <= z1; z++) {
	for (x = x0; x <= x1; x++) {
		if (!World_Contains(x, y, z)) continue;
		b = World_GetBlock(x, y, z);
		if (Blocks.Collide[b] != COLLIDE_SOLID) continue;

		blockBB.Min.x = x + Blocks.MinBB[b].x; blockBB.Max.x = x + Blocks.MaxBB[b].x;
		blockBB.Min.y = y + Blocks.MinBB[b].y; blockBB.Max.y = y + Blocks.MaxBB[b].y;
		blockBB.Min.z = z + Blocks.MinBB[b].z; blockBB.Max.z = z + Blocks.MaxBB[b].z;
		if (AABB_Intersects(bb, &blockBB)) return true;
	}}}
	return false;
}

/* blockMap.getEntities + isShootable + owner-exclusion check, tested against */
/*  the player and every live mob (Entity.java: Mob/Player are the only two */
/*  classes that override isShootable() to return true). */
static cc_bool Arrow_EntityCollision(struct ArrowEntity* a, struct AABB* bb,
									  struct Entity** outEntity, struct Mob** outMob) {
	struct LocalPlayer* p = Entities.CurPlayer;
	struct AABB other;
	struct Mob* m;
	int i;

	if (p && !(a->ownerIsPlayer && a->age <= ARROW_OWNER_GRACE_TICKS)) {
		Entity_GetBounds(&p->Base, &other);
		if (AABB_Intersects(bb, &other)) {
			*outEntity = &p->Base; *outMob = NULL; return true;
		}
	}

	for (i = 0; i < MOB_MAX; i++) {
		m = &st_mobs[i];
		if (!m->active || m->health <= 0) continue;
		if (!a->ownerIsPlayer && a->ownerMobSlot == i && a->age <= ARROW_OWNER_GRACE_TICKS) continue;

		Entity_GetBounds(&m->Base, &other);
		if (AABB_Intersects(bb, &other)) {
			*outEntity = &m->Base; *outMob = m; return true;
		}
	}
	return false;
}

/* entity.hurt(this, damage) - knockback is computed from the ARROW's own */
/*  position (not the original shooter's), exactly as Mob.hurt()/Arrow.tick() */
/*  do in the decompiled source (the arrow passes itself as the cause). */
static void Arrow_ApplyHit(struct ArrowEntity* a, struct Entity* hitEntity, struct Mob* hitMob) {
	struct Entity fakeAttacker = { 0 };
	fakeAttacker.Position = a->pos;

	if (hitMob) {
		Mob_Hurt(hitMob, &fakeAttacker, a->damage);
	} else {
		SurvivalTest_Hurt(a->damage);
	}
	a->active = false; /* entity hits remove() the arrow immediately - it never sticks */
}

/* Arrow.tick() - drag+gravity, then a subdivided sweep (so fast arrows can't */
/*  tunnel through thin obstacles) checking blocks first, then entities. */
static void Arrow_Tick(struct ArrowEntity* a) {
	struct AABB bb, swept;
	struct Entity* hitEntity;
	struct Mob* hitMob;
	Vec3 step;
	float len;
	int steps, s;
	cc_bool collided = false;

	a->age++;

	if (a->hasHit) {
		a->stickTime++;
		if (a->type == 0) {
			if (a->stickTime >= ARROW_STICK_PLAYER_MIN_TICKS &&
				Random_Float(&st_arrowRng) < ARROW_STICK_PLAYER_DESPAWN_CHANCE) a->active = false;
		} else {
			if (a->stickTime >= ARROW_STICK_MOB_TICKS) a->active = false;
		}
		return;
	}

	a->velocity.x *= ARROW_DRAG;
	a->velocity.y *= ARROW_DRAG;
	a->velocity.z *= ARROW_DRAG;
	a->velocity.y -= 0.02f * a->gravity;

	len   = Math_SqrtF(a->velocity.x * a->velocity.x + a->velocity.y * a->velocity.y + a->velocity.z * a->velocity.z);
	steps = (int)(len / ARROW_SUBSTEP_LEN + 1.0f);
	step.x = a->velocity.x / steps;
	step.y = a->velocity.y / steps;
	step.z = a->velocity.z / steps;

	Arrow_BoxAt(&a->pos, &bb);

	for (s = 0; s < steps && !collided; s++) {
		swept = bb;
		Arrow_ExpandBox(&swept, &step);

		if (Arrow_BlockCollision(&swept)) collided = true;

		if (Arrow_EntityCollision(a, &swept, &hitEntity, &hitMob)) {
			Arrow_ApplyHit(a, hitEntity, hitMob);
			return; /* entity hits short-circuit the whole tick, exactly as in Arrow.tick() */
		}

		if (!collided) {
			a->pos.x += step.x; a->pos.y += step.y; a->pos.z += step.z;
			Arrow_BoxAt(&a->pos, &bb);
		}
	}

	if (collided) {
		a->hasHit = true;
		a->velocity.x = a->velocity.y = a->velocity.z = 0.0f;
	} else if (len > 0.0001f) {
		a->facing.x = a->velocity.x / len;
		a->facing.y = a->velocity.y / len;
		a->facing.z = a->velocity.z / len;
	}
}

/* playerTouch() - pickup is a plain AABB touch test (no pickup radius), and */
/*  only ever applies to player-owned arrows that are already stuck. */
static void Arrow_TryPickup(struct ArrowEntity* a) {
	struct LocalPlayer* p;
	struct AABB arrowBB, playerBB;
	if (!a->hasHit || !a->ownerIsPlayer)      return;
	if (st_playerArrows >= ARROW_PLAYER_MAX)  return;

	p = Entities.CurPlayer;
	if (!p) return;

	Arrow_BoxAt(&a->pos, &arrowBB);
	Entity_GetBounds(&p->Base, &playerBB);
	if (!AABB_Intersects(&arrowBB, &playerBB)) return;

	st_playerArrows++;
	a->active = false;
}

static void SurvivalTest_TickArrows(void) {
	struct ArrowEntity* a;
	int i;
	for (i = 0; i < ARROW_MAX; i++) {
		a = &st_arrows[i];
		if (!a->active) continue;

		Arrow_Tick(a);
		if (a->active) Arrow_TryPickup(a);
	}
}

static void ArrowsPngProcess(struct Stream* stream, const cc_string* name) {
	Game_UpdateTexture(&st_arrowsTexId, stream, name, NULL, NULL);
}
static struct TextureEntry arrows_entry = { "arrows.png", ArrowsPngProcess };

/* render() - reconstructs the model's final world-space orientation from an */
/*  orthonormal (facing, crossA, crossB) basis plus a fixed 45-degree roll, */
/*  rather than literally porting Java's RotY(yRot-90)*RotZ(xRot)*RotX(45) */
/*  Euler sequence (which depends on axis conventions this engine doesn't */
/*  share) - matching the precedent already set by Mob_MoveRelative's own */
/*  from-scratch re-derivation of a Java rotation formula. The exact local */
/*  geometry/UVs below (2 head quads at local x=-7, 4 shaft quads spanning */
/*  x=-8..8 rotated 0/90/180/270 around the shaft axis, all scaled by */
/*  0.05625) are copied directly from the decompiled render() though, since */
/*  that part has no convention mismatch to resolve. */
static void Arrow_BuildVertices(struct ArrowEntity* a, PackedCol col, struct VertexTextured** vertices) {
	struct VertexTextured* v = *vertices;
	Vec3 F, ref, right, up, crossA, crossB, center;
	const float s = 0.05625f;
	const float c45 = 0.70710678f;
	float var2, var3, var4, var5, var8;
	float cy, sy;
	int k;
	static const float thetaCos[4] = { 1.0f, 0.0f, -1.0f,  0.0f };
	static const float thetaSin[4] = { 0.0f, 1.0f,  0.0f, -1.0f };

	F = a->facing;
	ref.x = 0.0f; ref.y = 1.0f; ref.z = 0.0f;
	if (Math_AbsF(F.y) > 0.999f) { ref.x = 0.0f; ref.y = 0.0f; ref.z = 1.0f; }

	right.x = F.y * ref.z - F.z * ref.y;
	right.y = F.z * ref.x - F.x * ref.z;
	right.z = F.x * ref.y - F.y * ref.x;
	Vec3_Normalise(&right);

	up.x = right.y * F.z - right.z * F.y;
	up.y = right.z * F.x - right.x * F.z;
	up.z = right.x * F.y - right.y * F.x;

	crossA.x = (right.x + up.x) * c45; crossA.y = (right.y + up.y) * c45; crossA.z = (right.z + up.z) * c45;
	crossB.x = (up.x - right.x) * c45; crossB.y = (up.y - right.y) * c45; crossB.z = (up.z - right.z) * c45;

	center   = a->pos;
	center.y -= 0.125f; /* render() translates heightOffset/2 (=0.25/2) below the tracked position */

	var2 = 0.5f;                            /* shaft U width (16 of 32 texels) */
	var3 = (a->type * 10) / 32.0f;          /* shaft V1 */
	var4 = (5.0f + a->type * 10) / 32.0f;   /* shaft V2 == head V1 */
	var5 = 0.15625f;                        /* head U width (5 of 32 texels) */
	var8 = (10.0f + a->type * 10) / 32.0f;  /* head V2 */

	#define ARROW_V(lx, ly, lz, uu, vv) \
		v->x = center.x - F.x*(lx)*s + crossA.x*(ly)*s + crossB.x*(lz)*s; \
		v->y = center.y - F.y*(lx)*s + crossA.y*(ly)*s + crossB.y*(lz)*s; \
		v->z = center.z - F.z*(lx)*s + crossA.z*(ly)*s + crossB.z*(lz)*s; \
		v->Col = col; v->U = (uu); v->V = (vv); v++;

	/* Head - 2 coplanar quads (opposite winding for front+back visibility) at local x=-7 */
	ARROW_V(-7,-2,-2, 0.0f,var4) ARROW_V(-7,-2, 2, var5,var4) ARROW_V(-7, 2, 2, var5,var8) ARROW_V(-7, 2,-2, 0.0f,var8)
	ARROW_V(-7, 2,-2, 0.0f,var4) ARROW_V(-7, 2, 2, var5,var4) ARROW_V(-7,-2, 2, var5,var8) ARROW_V(-7,-2,-2, 0.0f,var8)

	/* Shaft - the same flat quad drawn 4 times, rotated 0/90/180/270 around */
	/*  the shaft axis to form the classic 4-bladed cross cross-section. */
	for (k = 0; k < 4; k++) {
		cy = thetaCos[k]; sy = thetaSin[k];
		ARROW_V(-8,-2.0f*cy,-2.0f*sy, 0.0f,var3) ARROW_V( 8,-2.0f*cy,-2.0f*sy, var2,var3)
		ARROW_V( 8, 2.0f*cy, 2.0f*sy, var2,var4) ARROW_V(-8, 2.0f*cy, 2.0f*sy, 0.0f,var4)
	}
	#undef ARROW_V
	*vertices = v;
}

void SurvivalTest_RenderArrows(float delta, float t) {
	struct ArrowEntity* a;
	struct VertexTextured* data;
	struct VertexTextured* ptr;
	PackedCol col;
	int i, count = 0;
	cc_bool any = false;

	if (!SurvivalTest_Enabled) return;
	for (i = 0; i < ARROW_MAX; i++) { if (st_arrows[i].active) { any = true; break; } }
	if (!any || !st_arrowsTexId) return;

	if (!st_arrowVB) {
		st_arrowVB = Gfx_CreateDynamicVb(VERTEX_FORMAT_TEXTURED, ARROW_MAX_VERTICES);
		if (!st_arrowVB) return;
	}

	ptr = data = (struct VertexTextured*)Gfx_LockDynamicVb(st_arrowVB, VERTEX_FORMAT_TEXTURED, ARROW_MAX_VERTICES);
	for (i = 0; i < ARROW_MAX; i++) {
		a = &st_arrows[i];
		if (!a->active) continue;

		col = Lighting.Color(Math_Floor(a->pos.x), Math_Floor(a->pos.y), Math_Floor(a->pos.z));
		Arrow_BuildVertices(a, col, &ptr);
		count += ARROW_VERTICES_PER_ARROW;
	}
	Gfx_UnlockDynamicVb(st_arrowVB);

	Gfx_SetVertexFormat(VERTEX_FORMAT_TEXTURED);
	Gfx_SetAlphaTest(true);
	Gfx_BindTexture(st_arrowsTexId);
	Gfx_DrawVb_IndexedTris_Range(count, 0, DRAW_HINT_NONE);
	Gfx_SetAlphaTest(false);
}

/* Minecraft.java's Tab-fire gate: discrete key-down, survival mode, arrows>0. */
cc_bool SurvivalTest_TryShootArrow(void) {
	struct LocalPlayer* p;
	struct Entity* e;
	if (!SurvivalTest_Enabled) return false;
	if (st_playerArrows <= 0)  return false;

	p = Entities.CurPlayer;
	if (!p) return false;
	e = &p->Base;

	/* Minecraft.java spawns at this.player.x/y/z (base position), not eye height. */
	SurvivalTest_SpawnArrow(e->Position, e->Yaw, e->Pitch,
							 ARROW_PLAYER_FIRE_FORCE, ARROW_PLAYER_DAMAGE, 0, true, -1);
	st_playerArrows--;
	return true;
}

int SurvivalTest_ArrowCount(void) { return st_playerArrows; }


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
	/* NOTE: e->Position is one tick stale here. LocalPlayer_Tick (called from */
	/*  the Entities_Component's task, which runs before ours every tick) ends */
	/*  by stashing this tick's freshly-computed position into e->next.pos and */
	/*  then resetting e->Position back to e->prev.pos for render interpolation */
	/*  (see LocalInterpComp_AdvanceState / the end of LocalPlayer_Tick). Reading */
	/*  e->Position.y here would silently drop the final tick of every fall from */
	/*  the measured distance, undercounting borderline falls (e.g. a fall just */
	/*  over the 3-block safe threshold could read as exactly 3.0 and deal no */
	/*  damage). e->next.pos.y is this tick's true, just-computed height. */
	float y = e->next.pos.y;

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
		/*  that the fall is measured from the apex (matches Survival Test). */
		/*  e->prev.pos.y is this tick's pre-movement height, i.e. exactly the */
		/*  resting height the moment the entity left the ground. */
		if (!st_falling) {
			st_falling   = true;
			st_fallPeakY = e->prev.pos.y;
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

	/* Arrows ----------------------------------------------------------------- */
	SurvivalTest_TickArrows();

	/* TNT -------------------------------------------------------------------- */
	SurvivalTest_TickTnt();
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
	/* SurvivalGameMode.apply(Player): the player always starts with 10 TNT */
	/*  in the last hotbar slot - this was missing entirely before. */
	st_inv[8].block = BLOCK_TNT;
	st_inv[8].count = 10;

	for (i = 0; i < DROP_MAX; i++) {
		st_drops[i].active = false;
	}
	for (i = 0; i < MOB_MAX; i++) {
		st_mobs[i].active = false;
	}
	for (i = 0; i < ARROW_MAX; i++) {
		st_arrows[i].active = false;
	}
	st_playerArrows = ARROW_PLAYER_START;

	for (i = 0; i < TNT_MAX; i++) {
		st_tnt[i].active = false;
	}
}

/* The item vertex buffer is a GPU resource and must be dropped/recreated */
/*  whenever the graphics context is lost (it is rebuilt lazily on render). */
static void SurvivalTest_OnContextLost(void* obj) {
	Gfx_DeleteDynamicVb(&st_itemVB);
	Gfx_DeleteDynamicVb(&st_glowVB);
	Gfx_DeleteDynamicVb(&st_arrowVB);
	Gfx_DeleteDynamicVb(&st_tntGlowVB);
	if (!Gfx.ManagedTextures) Gfx_DeleteTexture(&st_arrowsTexId);
}

static void SurvivalTest_Init(void) {
	SurvivalTest_Enabled = Options_GetBool(OPT_SURVIVAL_MODE, false);
	if (!SurvivalTest_Enabled) return;

	Random_SeedFromCurrentTime(&st_dropRng);
	Random_SeedFromCurrentTime(&st_mobRng);
	Random_SeedFromCurrentTime(&st_arrowRng);
	SurvivalTest_ResetState();
	ScheduledTask_Add(GAME_DEF_TICKS, SurvivalTest_Tick);
	Event_Register_(&UserEvents.BlockChanged, NULL, SurvivalTest_BlockChanged);
	Event_Register_(&GfxEvents.ContextLost,   NULL, SurvivalTest_OnContextLost);
	TextureEntry_Register(&arrows_entry);
}

static void SurvivalTest_Free(void) {
	if (!SurvivalTest_Enabled) return;
	Event_Unregister_(&UserEvents.BlockChanged, NULL, SurvivalTest_BlockChanged);
	Event_Unregister_(&GfxEvents.ContextLost,   NULL, SurvivalTest_OnContextLost);
	Gfx_DeleteDynamicVb(&st_itemVB);
	Gfx_DeleteDynamicVb(&st_glowVB);
	Gfx_DeleteDynamicVb(&st_arrowVB);
	Gfx_DeleteDynamicVb(&st_tntGlowVB);
}

static void SurvivalTest_OnNewMap(void) {
	if (!SurvivalTest_Enabled) return;
	/* Resets to the SurvivalGameMode.apply(Player) starting loadout (10 TNT, */
	/*  everything else gathered by mining) every time a new map is loaded. */
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
