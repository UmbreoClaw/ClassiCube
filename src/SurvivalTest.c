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
#include "Stream.h"
#include "Bitmap.h"
#include "HeldBlockRenderer.h"
#include "Camera.h"
#include "IndevTest.h"
#include "Input.h"
#include "Gui.h"
#include "Picking.h"
#include "Particle.h"

/* Classic 0.30 Survival Test gamemode implementation.
   Copyright 2014-2025 ClassiCube | Licensed under BSD-3
*/

int SurvivalTest_Gamemode(void) {
	int mode = Options_GetInt(OPT_SURVIVAL_GAMEMODE, 0, 2, -1);
	if (mode >= 0) return mode;

	/* Legacy two-boolean migration: survival-mode wins a tie, matching the
	    old runtime backstop, so upgraded installs keep their behaviour. */
	if (Options_GetBool(OPT_SURVIVAL_MODE, false)) return SURVIVAL_GAMEMODE_C030;
	if (Options_GetBool(OPT_INDEV_MODE,    false)) return SURVIVAL_GAMEMODE_INDEV;
	return SURVIVAL_GAMEMODE_OFF;
}

cc_bool SurvivalTest_Enabled;
cc_bool SurvivalTest_Enhanced;
int     SurvivalTest_Health = SURVIVAL_MAX_HEALTH;

/* Mob.invulnerableDuration: 20 ticks (1s). hurt() uses a dual threshold - */
/*  while the window is fresher than its half-point only the excess over the */
/*  hit that opened it lands; past the half-point any hit lands in full and */
/*  re-arms the window. This is what turns Mob.tick's every-tick hurt(null,10) */
/*  lava / hurt(null,2) drowning calls into their effective once-per-0.5s */
/*  cadence - no separate damage timers exist in the original. */
#define INVULN_DURATION_SECS 1.0f
/* Damage dealt per lava tick (Mob.tick: hurt(null, 10)) */
#define LAVA_DAMAGE        10
/* Damage dealt per drowning tick (Mob.tick: hurt(null, 2)) */
#define DROWN_DAMAGE       2
/* Starting air supply in seconds (15s before drowning, as in Survival Test) */
#define AIR_SUPPLY_SECS    15.0f
/* Falls of more than this many blocks deal damage (~1 HP per excess block) */
#define FALL_SAFE_BLOCKS   3.0f
/* Mob.hurtTime/hurtDuration: every successful hit sets a fixed 10-tick */
/*  window (regardless of damage dealt), used only for the camera-tilt cue. */
#define HURT_TILT_TICKS    10
/* Renderer.hurtEffect's peak camera roll angle, in degrees. */
#define HURT_TILT_MAX_DEG  14.0f

static float st_airTimer;
static cc_bool st_headInWater; /* whether the player's head is submerged (drives the HUD air bubbles) */
static float st_invincTimer; /* Mob.invulnerableTime, in seconds (counts down from 1.0) */
static int   st_deathTicks;  /* Mob.deathTime - ticks since dying, drives the death camera */
static int   st_lastHealth;  /* Mob.lastHealth - health snapshot when the window opened */
static float st_fallPeakY;    /* highest Y reached during the current fall */
static cc_bool st_falling;    /* whether a fall is currently being tracked */
static cc_bool st_isDead;
/* Player.score: awarded on player-credited mob kills, shown on GameOverScreen. */
static int   st_score;
/* Mob.hurtTime equivalent for the player - counts down from HURT_TILT_TICKS */
/*  each game tick, purely cosmetic (drives the hurt camera-tilt effect). */
static int   st_hurtTicks;
/* Mob.hurtDir: horizontal bearing of the attacker relative to the player's */
/*  yaw at the moment of the hit, baked in (not recomputed while it decays). */
static float st_hurtDir;

/* Debug/testing toggles, driven by the F9 SurvivalDebugScreen - NOT part of */
/*  genuine c0.30-s parity (see the Debug/testing tools section at the bottom). */
/*  st_godMode blocks ALL player damage; st_debugNoAI and st_debugForceArmor */
/*  affect only mobs subsequently spawned via the debug menu, never natural */
/*  spawns. All default false (zero-init), so they're inert unless toggled on. */
static cc_bool st_godMode;
static cc_bool st_debugNoAI;
static cc_bool st_debugForceArmor;

/* Slot-based inventory: slots 0..8 are the hotbar, 9..35 are storage. */
/* ItemStack groundwork for the Indev layer: `id` spans BLOCKS (0..255) and, */
/*  later, ITEMS (ST_ITEM_ID_START+). In c0.30 survival ids are always block */
/*  ids and damage is unused - behaviour is unchanged; the wider type exists */
/*  so tools/food (Indev ItemStack(id, count, damage)) can slot in without a */
/*  second inventory rewrite. Use the helpers below, never assume id==block. */
#define ST_ITEM_ID_START     256
#define ST_ID_IS_BLOCK(id)   ((id) < ST_ITEM_ID_START)
#define ST_ID_BLOCK(id)      (ST_ID_IS_BLOCK(id) ? (BlockID)(id) : BLOCK_AIR)
/* Full id space: 256 block ids + Item.itemsList[]'s shifted ids (id+256, */
/*  sized so Indev's 1024-entry list fits). */
#define ST_MAX_IDS           1024
/* struct SurvivalSlot now lives in SurvivalTest.h (shared with IndevTest's
    tile entity store for chest/furnace contents). */

/* Runtime per-id max stack size. c0.30 stacks everything to 99; the Indev */
/*  layer (or later a server) overrides per id (Item.maxStackSize: 64 for */
/*  materials, 1 for tools/armor) via the setter. Data, not code. */
static cc_int16 st_maxStack[ST_MAX_IDS];
static cc_bool  st_maxStackInited;

static void ST_SeedMaxStack(void) {
	int i;
	if (st_maxStackInited) return;
	for (i = 0; i < ST_MAX_IDS; i++) st_maxStack[i] = SURVIVAL_STACK_MAX;
	st_maxStackInited = true;
}

void SurvivalTest_SetMaxStack(int id, int maxStack) {
	if (id < 0 || id >= ST_MAX_IDS) return;
	/* Seed BEFORE overriding - IndevItems_Seed calls this at component-init */
	/*  time, and the old first-read lazy seed rewrote the WHOLE table */
	/*  afterwards, silently clobbering Indev's 64-max / tools-stack-to-1 */
	/*  limits back to the c0.30 default of 99. (Same bug family as the */
	/*  st_hardness seed-order fix - see SurvivalTest_SetHardness.) */
	ST_SeedMaxStack();
	st_maxStack[id] = (cc_int16)maxStack;
}

static int ST_MaxStack(cc_uint16 id) {
	ST_SeedMaxStack();
	return id < ST_MAX_IDS ? st_maxStack[id] : 1;
}
static struct SurvivalSlot st_inv[SURVIVAL_INV_SLOTS];
/* Bumped on every inventory change so the HUD knows to redraw counts. */
static int st_invVersion;
static RNGState st_dropRng;
/* General-purpose RNG also used outside the Mobs section (e.g. the random */
/*  hurtDir fallback for environmental damage in SurvivalTest_CalcHurtDir). */
static RNGState st_mobRng;
/* Defined later, in the Inventory section - forward declared so the */
/*  dropped-item pickup logic below can hand picked-up blocks to it. */
static cc_bool SurvivalTest_AddItem(cc_uint16 id);
#define SurvivalTest_AddBlock(block) SurvivalTest_AddItem(block)
/* Defined in the Mining section - forward declared for the melee attack path */
static void SurvivalTest_DamageHeldTool(int amount);
/* Defined later, in the Ticking section - forward declared so the Mobs */
/*  section below (which ticks before Ticking is reached) can reuse it. */
static cc_bool SurvivalTest_IsHeadInWater(struct Entity* e);
/* Defined later, in the Arrows section - forward declared so the Mobs */
/*  section below (skeletons firing/death-bursting arrows) can spawn them. */
static void SurvivalTest_SpawnArrow(Vec3 pos, float yaw, float pitch, float force,
									 int damage, cc_uint8 type, cc_bool ownerIsPlayer, int ownerMobSlot);
/* Defined later, in the TNT section - forward declared so the Drops section */
/*  above (which decides what mining a TNT block does) can ignite its fuse. */
/*  fuseTicks lets callers other than mining (the explosion chain-reaction) */
/*  arm a shorter, randomized fuse instead of the full PrimedTnt default. */
static void SurvivalTest_ArmTnt(IVec3 coords, int fuseTicks);
#define TNT_FUSE_TICKS 40   /* PrimedTnt's default life */
/* Defined later (TNT section, where its billboard-render siblings live) - */
/*  forward declared so RenderDrops above it can draw item-id drop sprites. */
static void SurvivalTest_RenderItemDropSprites(float t);

/* Entity.isInWater()/isInLava(): the liquid test box is bb.grow(0, -0.4, 0) - */
/*  shrunk 0.4 blocks at both top and bottom - so sliver contact at the feet */
/*  or head doesn't count as being in the liquid. Used by the player's lava/ */
/*  fall-cushion checks and the mobs' swim logic (Entity_TouchesAnyWater/Lava */
/*  test the FULL-height box, which is subtly different). */
static cc_bool ST_IsLavaBlock(BlockID b)  { return Blocks.ExtendedCollide[b] == COLLIDE_LAVA; }
static cc_bool ST_IsWaterBlock(BlockID b) { return Blocks.ExtendedCollide[b] == COLLIDE_WATER; }
static cc_bool ST_InLiquid(struct Entity* e, cc_bool lava) {
	struct AABB bb;
	Entity_GetBounds(e, &bb);
	bb.Min.y += 0.4f; bb.Max.y -= 0.4f;
	return Entity_TouchesAny(&bb, lava ? ST_IsLavaBlock : ST_IsWaterBlock);
}


/*########################################################################################################################*
*------------------------------------------------------Dropped items-------------------------------------------------------*
*#########################################################################################################################*/
/* Survival Test (since 0.24-s) drops physical items on the ground instead */
/*  of putting mined blocks straight into the inventory - the player has */
/*  to walk over them to collect them. */
/* Genuine c0.30 has NO entity cap at all (Level.addEntity is an unbounded
    ArrayList.add) - so the pool is generous, and when it does overflow the
    OLDEST drop is evicted so fresh drops always spawn (silently discarding
    the new drop made mining on a littered map yield nothing). */
#define DROP_MAX           256
/* Item.tick(): yd -= 0.04F per tick = 0.04 * 20^2 = 16 blocks/sec^2, then all */
/*  three axes are damped by *0.98F every tick (which is also what limits fall */
/*  speed - there is no explicit terminal-velocity clamp in the original). */
#define DROP_GRAVITY        16.0f  /* blocks/sec^2 */
#define DROP_DRAG            0.98f /* per-tick */
/* Item.tick(): ++age; if (age >= 6000) remove() - drops despawn after 6000 */
/*  ticks (5 minutes at 20 TPS) if never collected. */
#define DROP_LIFETIME_SECS 300.0f
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
	Vec3 prevPos;    /* position as of the end of the previous tick - RenderDropBlocks blends */
	                 /*  prevPos->position by the partial-tick t so drops (especially the fast */
	                 /*  pickup fly-in, which only steps ~3 ticks) move smoothly every frame */
	                 /*  instead of at the 20 Hz tick rate. */
	Vec3 velocity;
	cc_uint16 block;  /* full id space: a BLOCK id (rendered as the spinning */
	                  /*  cube) or an ITEM id 256+ (rendered as a billboard */
	                  /*  sprite from items.png, Indev EntityItem style) */
	int  count;      /* Item.count - how many blocks this one drop entity carries */
	                 /*  (death scatters one drop per slot with its full stack) */
	float pickupDelay; /* EntityItem.delayBeforeCanPickup - only ever non-zero for */
	                   /*  items the player tossed (40 ticks), so a Q-drop isn't */
	                   /*  instantly vacuumed back up. c0.30 drops keep 0. */
	float age;       /* seconds alive - drives spin/bob/glow */
	float prevAge;   /* age as of the end of the previous tick - RenderDropBlocks blends */
	                 /*  prevAge->age by the partial-tick t so the spin/bob/glow animation */
	                 /*  (which all derive from age via DropItem_Phase) advances smoothly */
	                 /*  every frame instead of snapping forward once per tick. */
	float rot0;      /* random initial spin angle (degrees) */
	cc_bool active;
	cc_bool onGround; /* set by DropPhysics's collision pass, drives the Item.tick() ground damping below */

	/* TakeEntityAnim: once collected, the item isn't removed immediately - it */
	/*  eases towards the player over a few ticks first (the classic "zip into */
	/*  you" pickup effect), instead of just vanishing in place. */
	cc_bool pickingUp;
	float   pickupTime;  /* seconds into the pickup-fly animation */
	Vec3    pickupFrom;  /* position captured the instant pickup started */
};
static struct DropItem st_drops[DROP_MAX];
/* TakeEntityAnim.tick(): removes itself once time >= 3, at 20 ticks/sec. */
#define DROP_PICKUP_ANIM_SECS (3.0f / 20.0f)

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
/* age is passed in (rather than read from d->age directly) so callers can */
/*  supply the interpolated, partial-tick age and get a smooth render-rate */
/*  animation instead of one that steps once per 20 Hz tick. */
static float DropItem_Phase(struct DropItem* d, float age) {
	return d->rot0 + age * DROP_SPIN_DEG_PER_SEC;
}

/* Survival Test redrew the item in additive white once per ~second for a */
/*  brief glint. NOTE: lerping the item's own lit colour towards white is a */
/*  no-op in full daylight (the lit colour is already pure white there), so */
/*  that approach was invisible outdoors. Instead this drives the alpha of a */
/*  separate white glow shell (see DropItem_BuildGlowCube), which genuinely */
/*  brightens the item regardless of how bright its lit colour already is. */
static float DropItem_GlowAmount(struct DropItem* d, float age) {
	float s = Math_SinF(DropItem_Phase(d, age) / 10.0f) * 0.5f + 0.5f; /* 0..1 */
	s = s * s * s * s;       /* ^4, matching the decompiled glow curve exactly */
	return s * 0.4f;         /* max alpha 0.4, matching the decompiled glColor4f(1,1,1,g*0.4) */
}

/* Drops are lit by the world like in Survival Test (darker in shade); */
/*  the white pulse is a separate additive pass, not a tint here. */
/* Renderer.updateFog's GL_LIGHT_MODEL_AMBIENT: while the CAMERA is inside */
/*  water everything renders with a bluish (0.4, 0.4, 0.9) ambient cast, and */
/*  inside lava a dark reddish (0.4, 0.3, 0.3) one. Applied to the colours of */
/*  everything survival renders (mobs, drops, arrows, TNT). */
static PackedCol SurvivalTest_AmbientTint(PackedCol col) {
	IVec3 pos;
	BlockID b;
	cc_uint8 collide;
	if (!SurvivalTest_Enabled || !World.Loaded) return col;

	IVec3_Floor(&pos, &Camera.CurrentPos);
	if (!World_Contains(pos.x, pos.y, pos.z)) return col;
	b = World_GetBlock(pos.x, pos.y, pos.z);
	collide = Blocks.ExtendedCollide[b];

	if (collide == COLLIDE_WATER) {
		return PackedCol_Make((cc_uint8)(PackedCol_R(col) * 2 / 5), (cc_uint8)(PackedCol_G(col) * 2 / 5),
		                      (cc_uint8)(PackedCol_B(col) * 9 / 10), PackedCol_A(col));
	}
	if (collide == COLLIDE_LAVA) {
		return PackedCol_Make((cc_uint8)(PackedCol_R(col) * 2 / 5), (cc_uint8)(PackedCol_G(col) * 3 / 10),
		                      (cc_uint8)(PackedCol_B(col) * 3 / 10), PackedCol_A(col));
	}
	return col;
}

static PackedCol DropItem_WorldColor(Vec3* pos) {
	int x = Math_Floor(pos->x);
	int y = Math_Floor(pos->y);
	int z = Math_Floor(pos->z);
	return SurvivalTest_AmbientTint(Lighting.Color(x, y, z));
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
static void DropItem_ComputeGeometry(struct DropItem* d, Vec3 pos, float age, float* yLo, float* yHi,
									  Vec3* a, Vec3* b, Vec3* c, Vec3* e) {
	float var3 = DropItem_Phase(d, age);
	float bob  = Math_SinF(var3 / 10.0f) * 0.1f + 0.1f;
	*yLo = pos.y + bob;
	*yHi = *yLo + DROP_ITEM_HALF * 2.0f;

	DropItem_RotatedCorners(DROP_ITEM_HALF, pos.x, pos.z,
							 var3 * MATH_DEG2RAD, a, b, c, e);
}

/* Appends the 6-face, 24-vertex textured cube for one drop (cropped to the */
/*  given UV rect on every face, matching the decompiled ItemModel exactly). */
static void DropItem_BuildItemCube(struct DropItem* d, Vec3 pos, float age, TextureRec rec, PackedCol col,
									struct VertexTextured** vertices) {
	struct VertexTextured* v = *vertices;
	float yLo, yHi;
	float u1 = rec.u1, v1 = rec.v1, u2 = rec.u2, v2 = rec.v2;
	Vec3 a, b, c, e;

	DropItem_ComputeGeometry(d, pos, age, &yLo, &yHi, &a, &b, &c, &e);

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
static void DropItem_BuildGlowCube(struct DropItem* d, Vec3 pos, float age, PackedCol col,
									struct VertexColoured** vertices) {
	struct VertexColoured* v = *vertices;
	float yLo, yHi;
	Vec3 a, b, c, e;

	DropItem_ComputeGeometry(d, pos, age, &yLo, &yHi, &a, &b, &c, &e);

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
	int i, oldest = 0;
	float oldestAge = -1.0f;

	for (i = 0; i < DROP_MAX; i++) {
		if (!st_drops[i].active) return i;
		/* Mid-pickup drops are moments from freeing themselves - never evict */
		/*  those (their blocks were already added to the inventory). */
		if (!st_drops[i].pickingUp && st_drops[i].age > oldestAge) {
			oldestAge = st_drops[i].age;
			oldest    = i;
		}
	}
	/* Pool full: evict the longest-lived drop (it was nearest to its 5-minute */
	/*  despawn anyway) - genuine never fails to spawn an item. */
	st_drops[oldest].active = false;
	return oldest;
}

/* Spawns one physical item drop at the given world position. Item's ctor pop */
/*  velocity: xd/zd = rand*0.2-0.1 and yd = 0.2 blocks/tick, i.e. each */
/*  horizontal axis independently uniform in +/-2 blocks/sec and a fixed 4 */
/*  blocks/sec vertical hop. */
static void SurvivalTest_SpawnDropAt(Vec3 pos, cc_uint16 block, int count) {
	struct DropItem* d;
	int slot = SurvivalTest_FindFreeDropSlot(); /* always succeeds - evicts the oldest when full */

	d = &st_drops[slot];
	Mem_Set(d, 0, sizeof(struct DropItem));

	d->position = pos;
	d->prevPos  = pos; /* seed so the first frame doesn't lerp in from (0,0,0) */

	d->velocity.x = (Random_Float(&st_dropRng) * 0.2f - 0.1f) * 20.0f;
	d->velocity.z = (Random_Float(&st_dropRng) * 0.2f - 0.1f) * 20.0f;
	d->velocity.y = 0.2f * 20.0f;

	d->block       = block;
	d->count       = max(count, 1);
	d->age         = 0.0f;
	d->prevAge     = 0.0f; /* seed so the first frame doesn't lerp in from a stale phase */
	d->rot0        = Random_Float(&st_dropRng) * 360.0f;
	d->active      = true;
}

/* Public wrapper so the Indev layer (chest scatter on break) can spawn */
/*  drop entities at an exact world position. */
void SurvivalTest_SpawnDropWorld(Vec3 pos, int id, int count) {
	SurvivalTest_SpawnDropAt(pos, (cc_uint16)id, count);
}

/* Spawns one physical item drop inside the given block. BlockUtils.dropItems */
/*  rolls a fresh rand*0.7 + 0.15 offset (0.15..0.85) per axis per item, so */
/*  multi-item drops (logs, ores) start scattered rather than stacked. */
static void SurvivalTest_SpawnDrop(IVec3 coords, cc_uint16 block) {
	Vec3 pos;
	pos.x = coords.x + Random_Float(&st_dropRng) * 0.7f + 0.15f;
	pos.y = coords.y + Random_Float(&st_dropRng) * 0.7f + 0.15f;
	pos.z = coords.z + Random_Float(&st_dropRng) * 0.7f + 0.15f;
	SurvivalTest_SpawnDropAt(pos, block, 1);
}

/* BlockUtils.getDrop()/getDropCount(): decides what a block *would* drop and */
/*  how many, before any chance gate is applied. Shared by both the mining */
/*  path (chance always 1.0) and the explosion path (chance 0.3 per item, via */
/*  BlockUtils.dropItems(block, level, x, y, z, 0.3F)). Returns false for */
/*  blocks that never drop a plain item at all (water/lava/bookshelf/TNT - */
/*  TNT instead arms a fuse, handled separately by each caller). */
static cc_bool SurvivalTest_GetBlockDrop(BlockID oldBlock, BlockID* dropBlock, int* count) {
	*dropBlock = oldBlock;
	*count     = 1;

	switch (oldBlock) {
	case BLOCK_GRASS:
		*dropBlock = BLOCK_DIRT;
		break;
	case BLOCK_LEAVES:
		/* Leaves only drop a sapling 1/10 of the time, otherwise nothing - */
		/*  this roll lives inside getDropCount() itself, so it's separate */
		/*  from (and on top of) the explosion's own 0.3 chance gate. */
		*dropBlock = BLOCK_SAPLING;
		*count     = Random_Next(&st_dropRng, 10) == 0 ? 1 : 0;
		break;
	case BLOCK_LOG:
		*dropBlock = BLOCK_WOOD;
		*count     = 3 + Random_Next(&st_dropRng, 3); /* 3-5 planks */
		break;
	case BLOCK_STONE:
	case BLOCK_OBSIDIAN:
		/* StoneBlock.getDrop() always returns COBBLESTONE.id - and Obsidian */
		/*  is literally constructed as `new StoneBlock(49, 37)`, so it goes */
		/*  through the exact same override. Confirmed against the Wiki too */
		/*  ("breaking stone/obsidian yields cobblestone"). */
		*dropBlock = BLOCK_COBBLE;
		break;
	case BLOCK_COAL_ORE:
		/* OreBlock.getDrop(): coal ore is the one weird case - it yields a */
		/*  stone SLAB, not coal (there's no separate coal item yet). Wiki */
		/*  confirms this exact quirk ("stone slabs were obtained by mining */
		/*  coal ore" in Survival Test). getDropCount() = 1-3 for all ores. */
		*dropBlock = BLOCK_SLAB;
		*count     = 1 + Random_Next(&st_dropRng, 3); /* 1-3 */
		break;
	case BLOCK_GOLD_ORE:
		*dropBlock = BLOCK_GOLD; /* OreBlock.getDrop(): gold ore -> gold block */
		*count     = 1 + Random_Next(&st_dropRng, 3); /* 1-3 */
		break;
	case BLOCK_IRON_ORE:
		*dropBlock = BLOCK_IRON; /* OreBlock.getDrop(): iron ore -> iron block */
		*count     = 1 + Random_Next(&st_dropRng, 3); /* 1-3 */
		break;
	case BLOCK_DOUBLE_SLAB:
		*dropBlock = BLOCK_SLAB; /* SlabBlock.getDrop() always returns SLAB.id */
		*count     = 2;          /* getDropCount(): a double slab drops both halves */
		break;
	case BLOCK_BOOKSHELF:
		/* BookshelfBlock.getDropCount() == 0 - never drops anything */
		return false;
	case BLOCK_WATER:
	case BLOCK_STILL_WATER:
	case BLOCK_LAVA:
	case BLOCK_STILL_LAVA:
		/* LiquidBlock overrides dropItems()/onBreak() to no-ops and */
		/*  getDropCount() == 0 - liquids never yield an item drop. */
		return false;
	case BLOCK_TNT:
		/* TNTBlock.getDropCount()==0 - TNT never yields a plain item drop; */
		/*  callers handle it as a fuse instead. */
		return false;
	default:
		break; /* most blocks drop themselves */
	}
	return true;
}

/* Decides what physically drops when a block is mined (Survival Test rules). */
/*  Mining always uses chance=1.0 (BlockUtils.dropItems's default overload). */
/* Indev block drops (BlockStone/Log/Ore/... idDropped + quantityDropped), */
/*  gated by canHarvestBlock. Differs from c0.30: log drops the LOG block */
/*  (not planks), coal ore drops the coal ITEM, gravel has a 1/10 flint roll, */
/*  and rock/iron blocks yield nothing without a suitable pickaxe. */
static void SurvivalTest_SpawnIndevDrops(IVec3 coords, BlockID oldBlock) {
	int heldId = st_inv[Inventory.SelectedIndex].id;
	cc_uint16 dropId = oldBlock; /* most blocks drop themselves */
	int count = 1, i;

	if (oldBlock == BLOCK_TNT) { SurvivalTest_ArmTnt(coords, TNT_FUSE_TICKS); return; }
	/* canHarvestBlock: rock/iron needs a pickaxe (see IndevTest_CanHarvest) */
	if (!IndevTest_CanHarvest(heldId, oldBlock)) return;

	switch (oldBlock) {
	case BLOCK_GRASS:   dropId = BLOCK_DIRT; break;
	case BLOCK_STONE:   dropId = BLOCK_COBBLE; break;      /* BlockStone -> cobblestone */
	case BLOCK_LOG:     dropId = BLOCK_LOG; break;         /* BlockLog -> the log itself */
	case BLOCK_COAL_ORE: dropId = 256 + 7; break;          /* -> coal ITEM */
	case BLOCK_LEAVES:  dropId = BLOCK_SAPLING;
	                    count  = Random_Next(&st_dropRng, 10) == 0 ? 1 : 0; break;
	case BLOCK_GRAVEL:  if (Random_Next(&st_dropRng, 10) == 0) dropId = 256 + 62; /* flint */
	                    break;
	case BLOCK_DOUBLE_SLAB: dropId = BLOCK_SLAB; break;
	case BLOCK_GLASS: case BLOCK_BOOKSHELF:
	case BLOCK_WATER: case BLOCK_STILL_WATER:
	case BLOCK_LAVA:  case BLOCK_STILL_LAVA:
		return; /* quantityDropped 0 / liquids */
	default: break; /* dirt, sand, planks, ores(iron/gold->self), wool, etc */
	}

	for (i = 0; i < count; i++) { SurvivalTest_SpawnDrop(coords, dropId); }
}

static void SurvivalTest_SpawnDropsForBlock(IVec3 coords, BlockID oldBlock) {
	BlockID dropBlock;
	int count, i;

	if (IndevTest_Enabled) { SurvivalTest_SpawnIndevDrops(coords, oldBlock); return; }

	if (oldBlock == BLOCK_TNT) {
		/* TNTPhysics.onBreak spawns a PrimedTnt with the full default fuse. */
		SurvivalTest_ArmTnt(coords, TNT_FUSE_TICKS);
		return;
	}
	if (!SurvivalTest_GetBlockDrop(oldBlock, &dropBlock, &count)) return;

	for (i = 0; i < count; i++) { SurvivalTest_SpawnDrop(coords, dropBlock); }
}

/* Decides what physically drops when a block is destroyed by an explosion */
/*  (Level.explode -> BlockUtils.dropItems(block, level, x, y, z, 0.3F)): each */
/*  potential item only has a 30% chance of actually being spawned, on top of */
/*  (not instead of) the leaves' own 1/10 roll above. This is why explosions */
/*  visibly look like only a handful of the broken blocks pop loose. */
static void SurvivalTest_ExplodeDropsForBlock(IVec3 coords, BlockID oldBlock) {
	BlockID dropBlock;
	int count, i;
	if (!SurvivalTest_GetBlockDrop(oldBlock, &dropBlock, &count)) return;

	for (i = 0; i < count; i++) {
		if (Random_Float(&st_dropRng) <= 0.3f) SurvivalTest_SpawnDrop(coords, dropBlock);
	}
}

/* Item.tick()'s move() - a proper swept-AABB collision against every block */
/*  the drop's box overlaps, not just the single block beneath it. Reuses the */
/*  same Collisions_MoveAndWallSlide the player and mobs use (Mob_TravelGround), */
/*  via a throwaway scratch Entity, so drops get real wall/ledge collision on */
/*  X and Z instead of only ever checking the ground column. Previously, a drop */
/*  that drifted sideways into a taller neighbouring block would just get */
/*  vertically warped onto its top the instant the single-block ground check */
/*  passed, instead of being stopped by the block like a wall - that's what */
/*  made drops look like they clipped into terrain and rest out of pickup */
/*  reach unless you stood almost on top of them. StepSize = 0 matches */
/*  Item.java's footSize (defaults to 0, i.e. no auto step-up - genuine items */
/*  stop dead against any obstacle, never climb it). */
static void SurvivalTest_DropPhysics(struct DropItem* d, float delta) {
	struct Entity scratch;
	struct CollisionsComp coll;

	d->velocity.y -= DROP_GRAVITY * delta;

	Mem_Set(&scratch, 0, sizeof(scratch));
	scratch.Position = d->position;
	Vec3_Set(scratch.Size, DROP_ITEM_HALF * 2.0f, DROP_ITEM_HALF * 2.0f, DROP_ITEM_HALF * 2.0f);
	scratch.OnGround = d->onGround;
	/* Velocity here is the per-tick displacement Collisions_MoveAndWallSlide */
	/*  expects (see Mob_TravelGround) - d->velocity is a blocks/sec rate, so */
	/*  scale by delta going in and back out afterwards. */
	scratch.Velocity.x = d->velocity.x * delta;
	scratch.Velocity.y = d->velocity.y * delta;
	scratch.Velocity.z = d->velocity.z * delta;

	coll.Entity   = &scratch;
	coll.StepSize = 0.0f;
	Collisions_MoveAndWallSlide(&coll);
	Vec3_AddBy(&scratch.Position, &scratch.Velocity);

	d->position  = scratch.Position;
	d->onGround  = scratch.OnGround;
	d->velocity.x = scratch.Velocity.x / delta;
	d->velocity.y = scratch.Velocity.y / delta;
	d->velocity.z = scratch.Velocity.z / delta;

	/* Item.tick() after move(): *0.98 air drag on all three axes, plus *0.7 */
	/*  extra ground friction while resting. (The original's yd *= -0.5 bounce */
	/*  is dead code - move() zeroes yd on any vertical clip first, exactly as */
	/*  Collisions_MoveAndWallSlide does here.) */
	d->velocity.x *= DROP_DRAG;
	d->velocity.y *= DROP_DRAG;
	d->velocity.z *= DROP_DRAG;
	if (d->onGround) {
		d->velocity.x *= 0.7f;
		d->velocity.z *= 0.7f;
	}
}

static void SurvivalTest_DropTryPickup(struct DropItem* d, struct Entity* pe) {
	struct AABB pbb, ibb;

	/* Player.tick(): entities = level.findEntities(this, this.bb.grow(1, 0, 1)) - an */
	/*  AABB overlap test against the player's own bounding box widened a full block */
	/*  horizontally (not vertically), not a fixed-radius distance check. That means */
	/*  reach is generous sideways but limited to the player's own height vertically - */
	/*  an item resting on an adjacent block is still within reach as long as it sits */
	/*  somewhere between the player's feet and head. */
	Entity_GetBounds(pe, &pbb);
	pbb.Min.x -= 1.0f; pbb.Max.x += 1.0f;
	pbb.Min.z -= 1.0f; pbb.Max.z += 1.0f;

	ibb.Min.x = d->position.x - DROP_ITEM_HALF; ibb.Max.x = d->position.x + DROP_ITEM_HALF;
	ibb.Min.y = d->position.y;                  ibb.Max.y = d->position.y + DROP_ITEM_HALF * 2.0f;
	ibb.Min.z = d->position.z - DROP_ITEM_HALF; ibb.Max.z = d->position.z + DROP_ITEM_HALF;
	if (!AABB_Intersects(&pbb, &ibb)) return;

	/* Item.playerTouch(): only collected if addResource() accepts it - with a */
	/*  full inventory the drop simply stays on the ground. A multi-block drop */
	/*  (a death-scattered stack) transfers as many blocks as fit and keeps the */
	/*  remainder. On success the item entity isn't removed until the */
	/*  TakeEntityAnim finishes - start the fly-to-player animation instead of */
	/*  vanishing right away. */
	while (d->count > 0 && SurvivalTest_AddBlock(d->block)) d->count--;
	if (d->count > 0) return;

	d->pickingUp  = true;
	d->pickupTime = 0.0f;
	d->pickupFrom = d->position;
}

static void SurvivalTest_TickDrops(struct Entity* pe, float delta) {
	struct DropItem* d;
	float distance;
	int i;

	for (i = 0; i < DROP_MAX; i++) {
		d = &st_drops[i];
		if (!d->active) continue;

		/* Interpolation source for RenderDropBlocks - see DropItem.prevPos/prevAge. */
		d->prevPos = d->position;
		d->prevAge = d->age;

		if (d->pickingUp) {
			/* TakeEntityAnim.tick(): distance = (time/3)^2, eased towards the */
			/*  player every tick (Y target is player.y - 1 = ~0.62 above the */
			/*  feet), landing fully on the player before removal at time 3. */
			d->pickupTime += delta;
			distance = d->pickupTime / DROP_PICKUP_ANIM_SECS;
			if (distance > 1.0f) distance = 1.0f;
			distance = distance * distance;
			d->position.x = d->pickupFrom.x + ( pe->Position.x          - d->pickupFrom.x) * distance;
			d->position.y = d->pickupFrom.y + ((pe->Position.y + 0.62f) - d->pickupFrom.y) * distance;
			d->position.z = d->pickupFrom.z + ( pe->Position.z          - d->pickupFrom.z) * distance;
			if (d->pickupTime >= DROP_PICKUP_ANIM_SECS) d->active = false;
			continue;
		}

		d->age += delta;
		if (d->age >= DROP_LIFETIME_SECS) { d->active = false; continue; }
		SurvivalTest_DropPhysics(d, delta);
		if (d->pickupDelay > 0.0f) { d->pickupDelay -= delta; }
		else                       { SurvivalTest_DropTryPickup(d, pe); }
	}
}

/* Updates how many vertices belong to each 1D atlas, for batching draws */
/*  (each drop's tile can land in a different 1D atlas / GL texture). */
static void SurvivalTest_UpdateItem1DCounts(void) {
	int i, index;
	for (i = 0; i < Atlas1D.Count; i++) { item_1DCount[i] = 0; item_1DIndices[i] = 0; }

	for (i = 0; i < DROP_MAX; i++) {
		if (!st_drops[i].active) continue;
		if (!ST_ID_IS_BLOCK(st_drops[i].block)) continue; /* sprite pass instead */
		index = Atlas1D_Index(Block_Tex((BlockID)st_drops[i].block, FACE_XMIN));
		item_1DCount[index] += ITEM_VERTICES_PER_DROP;
	}
	for (i = 1; i < Atlas1D.Count; i++) {
		item_1DIndices[i] = item_1DIndices[i - 1] + item_1DCount[i - 1];
	}
}

/* Renders the lit, textured item cubes - cropped to the middle 50% of the */
/*  block's tile on every face, spinning about Y and bobbing up/down, with a */
/*  brief white glint (~1 Hz) applied as a colour lerp toward white. */
static void SurvivalTest_RenderDropBlocks(float t) {
	struct DropItem* d;
	struct VertexTextured* data;
	struct VertexTextured* ptr;
	struct VertexColoured* glowData;
	struct VertexColoured* glowPtr;
	TextureLoc loc;
	TextureRec base, rec;
	PackedCol col, glowCol;
	Vec3 renderPos;
	float du, dv, renderAge;
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
	/* Vertex format must be set before locking - some backends (e.g. D3D11) */
	/*  rebind the VB's stride as part of unlocking it, using whatever format */
	/*  is currently active, so setting it only after the lock/unlock (as the */
	/*  draw call below needs) would bind with a stale stride left over from */
	/*  whatever was drawn just before this (e.g. Entities_RenderModels). */
	Gfx_SetVertexFormat(VERTEX_FORMAT_TEXTURED);
	data = (struct VertexTextured*)Gfx_LockDynamicVb(st_itemVB, VERTEX_FORMAT_TEXTURED, ITEM_MAX_VERTICES);
	for (i = 0; i < DROP_MAX; i++) {
		d = &st_drops[i];
		if (!d->active) continue;
		/* Item-id drops render in the sprite pass - they are NOT in the 1D */
		/*  batch counts, so building them here would spill into (and corrupt) */
		/*  the other drops' vertex ranges. */
		if (!ST_ID_IS_BLOCK(d->block)) continue;

		loc   = Block_Tex((BlockID)d->block, FACE_XMIN);
		index = Atlas1D_Index(loc);
		ptr   = data + item_1DIndices[index];

		base = Atlas1D_TexRec(loc, 1, &texIndex);
		if (IndevTest_Enabled) {
			/* Indev RenderItem: block drops are miniature FULL blocks */
			/*  (renderBlockOnInventory at 0.25 scale) - whole tile per face, */
			/*  not classic's cropped ItemModel. */
			rec = base;
		} else {
			/* Crop to the middle 50% (texels 4..12 of 16) of the tile, on */
			/*  every face - classic ItemModel's look. */
			du = (base.u2 - base.u1) * 0.25f;
			dv = (base.v2 - base.v1) * 0.25f;
			rec.u1 = base.u1 + du; rec.u2 = base.u2 - du;
			rec.v1 = base.v1 + dv; rec.v2 = base.v2 - dv;
		}

		/* Blend prevPos->position and prevAge->age by the partial-tick t - see */
		/*  DropItem.prevPos/prevAge - so both motion and spin/bob are smooth. */
		Vec3_Lerp(&renderPos, &d->prevPos, &d->position, t);
		renderAge = d->prevAge + (d->age - d->prevAge) * t;
		col = DropItem_WorldColor(&renderPos);
		DropItem_BuildItemCube(d, renderPos, renderAge, rec, col, &ptr);
		item_1DIndices[index] += ITEM_VERTICES_PER_DROP;
	}
	Gfx_UnlockDynamicVb(st_itemVB);

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
	/* Vertex format set before locking, same reason as PASS 1 above. */
	Gfx_SetVertexFormat(VERTEX_FORMAT_COLOURED);
	glowData = (struct VertexColoured*)Gfx_LockDynamicVb(st_glowVB, VERTEX_FORMAT_COLOURED, GLOW_MAX_VERTICES);
	glowPtr  = glowData;
	glowCount = 0;
	for (i = 0; i < DROP_MAX; i++) {
		d = &st_drops[i];
		if (!d->active) continue;
		if (!ST_ID_IS_BLOCK(d->block)) continue; /* sprites have no glow shell */

		Vec3_Lerp(&renderPos, &d->prevPos, &d->position, t);
		renderAge = d->prevAge + (d->age - d->prevAge) * t;
		glowCol = PackedCol_Make(255, 255, 255, (cc_uint8)(255.0f * DropItem_GlowAmount(d, renderAge)));
		DropItem_BuildGlowCube(d, renderPos, renderAge, glowCol, &glowPtr);
		glowCount += GLOW_VERTICES_PER_DROP;
	}
	Gfx_UnlockDynamicVb(st_glowVB);

	if (glowCount > 0) {
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
	SurvivalTest_RenderDropBlocks(t);	SurvivalTest_RenderItemDropSprites(t);
}


/*########################################################################################################################*
*----------------------------------------------------Health & damage------------------------------------------------------*
*#########################################################################################################################*/
/* Mob.hurt(): hurtDir is the horizontal bearing of the attacker relative to */
/*  the victim's own yaw - Math_Atan2f(-dz, dx) matches Yaw's convention */
/*  elsewhere in this file (e.g. Mob_DoAttack's aiming code; recall */
/*  Math_Atan2f(x,y)==atan2(y,x)), then offset by the player's current yaw to */
/*  get the bearing relative to their facing. No attacker (environmental */
/*  damage) -> random 0 or 180, matching the original's `hurt(null, damage)`. */
static float SurvivalTest_CalcHurtDir(const Vec3* attackerPos) {
	struct LocalPlayer* p = Entities.CurPlayer;
	float dx, dz, dirYaw;
	if (!attackerPos) return (float)(Random_Next(&st_mobRng, 2) * 180);

	dx = attackerPos->x - p->Base.Position.x;
	dz = attackerPos->z - p->Base.Position.z;
	dirYaw = Math_Atan2f(-dz, dx) * MATH_RAD2DEG;
	return dirYaw - p->Base.Yaw;
}

/* Player.die(): scatters one item drop per non-empty inventory slot at the */
/*  player's position, each carrying the slot's full stack count - so a */
/*  respawned player can walk back and re-collect their whole inventory. */
static void SurvivalTest_DropInventory(void) {
	struct Entity* p = &Entities.CurPlayer->Base;
	Vec3 pos = p->Position;
	int i;
	pos.y += 1.0f; /* pop from around chest height rather than the feet */

	for (i = 0; i < SURVIVAL_INV_SLOTS; i++) {
		if (st_inv[i].id == BLOCK_AIR || st_inv[i].count <= 0) continue;
		SurvivalTest_SpawnDropAt(pos, st_inv[i].id, st_inv[i].count);
	}
}

/* Mob.knockback(cause, ...): halve the current velocity, then shove 0.4 */
/*  horizontally away from the attacker and 0.4 up (upward capped at 0.4). */
/*  Player inherits this - mob melee and arrow hits visibly shove the player. */
static void SurvivalTest_Knockback(struct Entity* e, const Vec3* attackerPos) {
	float dx = attackerPos->x - e->Position.x;
	float dz = attackerPos->z - e->Position.z;
	float dist = Math_SqrtF(dx * dx + dz * dz);
	if (dist < 0.001f) return;

	e->Velocity.x = e->Velocity.x * 0.5f - dx / dist * 0.4f;
	e->Velocity.y = e->Velocity.y * 0.5f + 0.4f;
	e->Velocity.z = e->Velocity.z * 0.5f - dz / dist * 0.4f;
	if (e->Velocity.y > 0.4f) e->Velocity.y = 0.4f;
}

/* Mob.hurt(cause, damage), as inherited by Player. Implements the genuine */
/*  dual-threshold invulnerability: while the 20-tick window is fresher than */
/*  its half-point, only the excess over the hit that opened it lands (and the */
/*  window/hurt-flash are NOT re-armed); past the half-point the hit lands in */
/*  full and re-opens the window. attackerPos is NULL for environmental damage */
/*  (fall/lava/drown/poison/explosion), matching hurt(null, damage) - which */
/*  also means no knockback and a random 0/180 hurtDir. */
static void SurvivalTest_Damage(int damage, const Vec3* attackerPos) {
	struct LocalPlayer* p = Entities.CurPlayer;
	if (!SurvivalTest_Enabled || !p) return;
	if (st_isDead)             return;
	if (st_godMode)            return; /* debug invincibility - blocks every damage source */
	if (damage <= 0)           return;

	if (st_invincTimer > INVULN_DURATION_SECS * 0.5f) {
		if (st_lastHealth - damage >= SurvivalTest_Health) return;
		SurvivalTest_Health = st_lastHealth - damage;
	} else {
		st_lastHealth  = SurvivalTest_Health;
		st_invincTimer = INVULN_DURATION_SECS;
		SurvivalTest_Health -= damage;
		st_hurtTicks = HURT_TILT_TICKS;
	}

	st_hurtDir = SurvivalTest_CalcHurtDir(attackerPos);
	if (attackerPos) SurvivalTest_Knockback(&p->Base, attackerPos);

	if (SurvivalTest_Health <= 0) {
		SurvivalTest_Health = 0;
		st_isDead = true;
		SurvivalTest_DropInventory();
		GameOverScreen_Show();
	}
}

void SurvivalTest_Hurt(int damage) { SurvivalTest_Damage(damage, NULL); }
void SurvivalTest_HurtFrom(int damage, Vec3 attackerPos) { SurvivalTest_Damage(damage, &attackerPos); }

/* Current hurt camera-tilt roll (Renderer.hurtEffect), eased via sin(t^4*pi) */
/*  over the HURT_TILT_TICKS window, t being the render partial-tick fraction. */
/*  Returns false (no roll to apply) once the window has fully decayed. */
static cc_bool SurvivalTest_GetHurtTilt(float t, float* degrees, float* dir) {
	float remaining;
	if (!SurvivalTest_Enabled || st_hurtTicks <= 0) return false;

	remaining = (float)st_hurtTicks - t;
	if (remaining <= 0.0f) return false;

	remaining /= (float)HURT_TILT_TICKS;
	*degrees = Math_SinF(remaining * remaining * remaining * remaining * MATH_PI) * HURT_TILT_MAX_DEG;
	*dir     = st_hurtDir;
	return true;
}

/* Applies the hurt camera-tilt roll directly to the already-built view */
/*  matrix, matching the original's glRotatef(-hurtDir) -> glRotatef(-roll, */
/*  Z) -> glRotatef(hurtDir) chain (rotate into the hit-direction frame, roll */
/*  around the view's forward axis, then rotate back). No-op when there's */
/*  nothing to apply, so this is safe to call unconditionally every frame. */
void SurvivalTest_ApplyHurtTilt(struct Matrix* view, float t) {
	float degrees, dir, dirRad;
	struct Matrix rot;

	/* Renderer.hurtEffect: once dead the camera keels sideways - */
	/*  glRotatef(40 - 8000/(deathTime + partial + 200), 0,0,1), easing from */
	/*  0 toward ~40 degrees as deathTime grows. */
	if (SurvivalTest_Enabled && st_isDead) {
		float roll = 40.0f - 8000.0f / ((float)st_deathTicks + t + 200.0f);
		Matrix_RotateZ(&rot, roll * MATH_DEG2RAD);
		Matrix_MulBy(view, &rot);
	}

	if (!SurvivalTest_GetHurtTilt(t, &degrees, &dir)) return;
	dirRad = dir * MATH_DEG2RAD;

	Matrix_RotateY(&rot, -dirRad);       Matrix_MulBy(view, &rot);
	Matrix_RotateZ(&rot, -degrees * MATH_DEG2RAD); Matrix_MulBy(view, &rot);
	Matrix_RotateY(&rot, dirRad);         Matrix_MulBy(view, &rot);
}

void SurvivalTest_Heal(int amount) {
	if (!SurvivalTest_Enabled) return;
	if (st_isDead)             return;

	SurvivalTest_Health += amount;
	if (SurvivalTest_Health > SURVIVAL_MAX_HEALTH)
		SurvivalTest_Health = SURVIVAL_MAX_HEALTH;
	/* Mob.heal also grants a half invulnerability window (10 ticks) */
	st_invincTimer = INVULN_DURATION_SECS * 0.5f;
}

/* Player.getScore() */
int SurvivalTest_Score(void) { return st_score; }

/* Mob.invulnerableTime in ticks, and the health snapshot from when the */
/*  window opened - the HUD flashes ghost "lastHealth" hearts while fresh. */
int SurvivalTest_InvulnTicks(void) { return (int)(st_invincTimer * 20.0f); }
int SurvivalTest_LastHealth(void)  { return st_lastHealth; }

/* Minecraft.setupCamera: while dead the FOV is divided by */
/*  (1 - 500/(deathTime + 500)) * 2 + 1 - a slow zoom-in from 1x toward 3x. */
float SurvivalTest_DeathFovZoom(void) {
	float dt;
	if (!SurvivalTest_Enabled || !st_isDead) return 1.0f;
	dt = (float)st_deathTicks;
	return (1.0f - 500.0f / (dt + 500.0f)) * 2.0f + 1.0f;
}

/* Player.isUnderWater() - drives whether the HUD draws the air bubble row. */
cc_bool SurvivalTest_HeadUnderwater(void) { return SurvivalTest_Enabled && st_headInWater; }

/* Player.airSupply, rescaled to the genuine 0..300 range the HUD bubble */
/*  formula expects (this port tracks air as 0..AIR_SUPPLY_SECS seconds). */
int SurvivalTest_AirSupply(void) { return (int)(st_airTimer / AIR_SUPPLY_SECS * 300.0f); }

/* level.explode's blast radius - shared by TNT and the creeper's death blast */
/*  (both derive from the same original explosion code). */
#define EXPLOSION_RADIUS 4

/* Level.explode's canExplode check: only the hard rock/metal blocks (stone, */
/*  cobblestone, bedrock, the three ores, gold/iron blocks, both slabs, brick, */
/*  mossy cobblestone, obsidian) survive a blast. That is exactly the set of */
/*  solid blocks with stone or metal dig sounds - and notably liquids are NOT */
/*  in it: TNT genuinely blows water and lava away in c0.30 (neighbouring */
/*  liquid then floods back in through normal physics). */
static cc_bool SurvivalTest_ExplosionImmune(BlockID b) {
	return Blocks.ExtendedCollide[b] == COLLIDE_SOLID &&
		(Blocks.DigSounds[b] == SOUND_METAL || Blocks.DigSounds[b] == SOUND_STONE);
}

/* level.explode(attacker, x, y, z, radius) - destroys a sphere of blocks and */
/*  damages every entity in range. Defined after Mob_Hurt/st_mobs (it damages */
/*  mobs as well as the player); forward-declared here since TNT calls it. */
static void SurvivalTest_Explode(Vec3 center, int radius);


/*########################################################################################################################*
*----------------------------------------------------------TNT-------------------------------------------------------------*
*#########################################################################################################################*/
/* PrimedTnt.java: mining a placed TNT block (TNTPhysics.onBreak, since */
/*  TNTBlock.getDropCount()==0) doesn't explode it instantly - it removes the */
/*  block and spawns a PrimedTnt *entity* in its place that pops up off the */
/*  ground, falls/bounces under gravity, smokes and flashes for life=40 ticks */
/*  (2 seconds @ 20 TPS), then explodes. Placing TNT does nothing special */
/*  (TNTPhysics.onPlace is a no-op); that's only BlockPhysics.c's separate, */
/*  older classic-multiplayer "place TNT to explode instantly" feature, which */
/*  Physics_HandleTnt now skips while in survival mode. */
/* Like drops/arrows/mobs, the entity is simulated by hand in a fixed array */
/*  (never a real Entities.List[] entry) and rendered as a textured cube. */
#define TNT_MAX        64
#define TNT_SIZE       0.98f /* PrimedTnt.setSize(0.98, 0.98) */
#define TNT_HALF       (TNT_SIZE * 0.5f)
#define TNT_HEIGHT_OFF (TNT_SIZE * 0.5f) /* heightOffset = bbHeight/2; pos is the centre */

struct TntFuse {
	Vec3 pos;       /* entity centre (PrimedTnt's x/y/z) */
	Vec3 prevPos;   /* centre as of the previous tick - render interpolates by t */
	Vec3 vel;       /* per-tick displacement, exactly as in PrimedTnt.tick() (xd/yd/zd) */
	int  ticksLeft; /* PrimedTnt.life */
	cc_bool active;
	cc_bool onGround;
};
static struct TntFuse st_tnt[TNT_MAX];

/* SmokeParticle.java: each lit TNT puffs out one smoke particle per tick that */
/*  drifts up and fades, which is the obvious "this is about to blow" visual */
/*  cue (the white flash overlay below is far subtler in daylight). Genuine */
/*  c0.30 turns the block into a PrimedTnt entity that physically hops and */
/*  smokes; we keep the block static (see above) but reproduce the smoke as a */
/*  small self-contained particle pool, rendered as billboards off the shared */
/*  particles.png atlas - the same texture and frames the real client used. */
#define TNT_SMOKE_MAX 96
struct TntSmoke {
	Vec3  pos, prevPos;
	Vec3  vel;        /* per-tick displacement, exactly as in Particle.java */
	int   age, life;  /* in ticks; tex frame = 7 - (age*8)/life */
	float gray;       /* SmokeParticle's random 0..0.3 grey tint */
	cc_bool active;
};
static struct TntSmoke st_tntSmoke[TNT_SMOKE_MAX];

/* Particle.java's constructor, with the (0,0,0) base velocity SmokeParticle */
/*  passes in, then SmokeParticle's own *0.1 damping and grey/lifetime setup. */
static void SurvivalTest_SpawnTntSmoke(float x, float y, float z) {
	struct TntSmoke* s = NULL;
	float xd, yd, zd, mag, scale;
	int i;

	for (i = 0; i < TNT_SMOKE_MAX; i++) {
		if (!st_tntSmoke[i].active) { s = &st_tntSmoke[i]; break; }
	}
	if (!s) return; /* pool full - skip this puff, oldest keep playing out */

	xd = (Random_Float(&st_dropRng) * 2.0f - 1.0f) * 0.4f;
	yd = (Random_Float(&st_dropRng) * 2.0f - 1.0f) * 0.4f;
	zd = (Random_Float(&st_dropRng) * 2.0f - 1.0f) * 0.4f;
	scale = (Random_Float(&st_dropRng) + Random_Float(&st_dropRng) + 1.0f) * 0.15f;
	mag   = Math_SqrtF(xd * xd + yd * yd + zd * zd);
	if (mag < 0.0001f) mag = 0.0001f;
	/* Particle base velocity, then SmokeParticle multiplies x/y/z by 0.1. */
	s->vel.x = (xd / mag * scale * 0.4f)         * 0.1f;
	s->vel.y = (yd / mag * scale * 0.4f + 0.1f)  * 0.1f;
	s->vel.z = (zd / mag * scale * 0.4f)         * 0.1f;

	s->pos.x = x; s->pos.y = y; s->pos.z = z;
	s->prevPos = s->pos;
	s->gray = Random_Float(&st_dropRng) * 0.3f;
	s->life = (int)(8.0f / (Random_Float(&st_dropRng) * 0.8f + 0.2f));
	if (s->life < 1) s->life = 1;
	s->age  = 0;
	s->active = true;
}

/* SmokeParticle.tick()/Particle.tick() with noPhysics=true (smoke ignores */
/*  block collision): integrate position, gently accelerate upward, damp. */
/* Particle.tick tests age++ >= lifetime (old value) and still runs the move */
/*  on its removal tick, so a puff gets lifetime+1 movement ticks in total. */
static void SurvivalTest_TickTntSmoke(void) {
	struct TntSmoke* s;
	int i;
	for (i = 0; i < TNT_SMOKE_MAX; i++) {
		s = &st_tntSmoke[i];
		if (!s->active) continue;

		s->prevPos = s->pos;
		if (s->age++ >= s->life) s->active = false;

		s->vel.y += 0.004f;
		s->pos.x += s->vel.x;
		s->pos.y += s->vel.y;
		s->pos.z += s->vel.z;
		s->vel.x *= 0.96f;
		s->vel.y *= 0.96f;
		s->vel.z *= 0.96f;
	}
}

static void SurvivalTest_ArmTnt(IVec3 coords, int fuseTicks) {
	struct TntFuse* tnt;
	float ang;
	int i, slot = -1;

	for (i = 0; i < TNT_MAX; i++) {
		if (!st_tnt[i].active) { slot = i; break; }
	}
	if (slot < 0) {
		/* Pool full (needs a 64+ simultaneous chain reaction): drop the TNT */
		/*  as a pickup item rather than deleting it - genuine has no cap, so */
		/*  at least no material silently vanishes. */
		Vec3 pos;
		pos.x = coords.x + 0.5f; pos.y = coords.y + 0.5f; pos.z = coords.z + 0.5f;
		SurvivalTest_SpawnDropAt(pos, BLOCK_TNT, 1);
		return;
	}
	tnt = &st_tnt[slot];

	/* InputHandler_DeleteBlock already cleared the block to air; unlike before */
	/*  we leave it that way - the PrimedTnt entity now stands in for the block, */
	/*  drawn as its own cube (see SurvivalTest_RenderTntCubes). */
	tnt->pos.x = coords.x + 0.5f;
	tnt->pos.y = coords.y + 0.5f;
	tnt->pos.z = coords.z + 0.5f;
	tnt->prevPos = tnt->pos;

	/* PrimedTnt ctor: a small upward pop (yd=0.2) with a tiny random horizontal */
	/*  drift. Note the original's xd/zd use sin/cos of an angle that's ALREADY */
	/*  in radians but then multiplied by PI/180 again - a Notch double-convert */
	/*  that makes the drift minuscule; reproduced verbatim for fidelity. */
	ang = Random_Float(&st_dropRng) * 2.0f * MATH_PI;
	tnt->vel.x = -Math_SinF(ang * MATH_DEG2RAD) * 0.02f;
	tnt->vel.y = 0.2f;
	tnt->vel.z = -Math_CosF(ang * MATH_DEG2RAD) * 0.02f;

	tnt->ticksLeft = fuseTicks;
	tnt->onGround  = false;
	tnt->active    = true;
}

/* PrimedTnt.hurt() when the attacker is the Player: the lit TNT is removed */
/*  without exploding and drops back into a pickup item - so meleeing a primed */
/*  TNT "defuses" it (at the cost of losing it to the ground). Routed through */
/*  the melee ray-cast (SurvivalTest_TryAttackMob), since there's no longer a */
/*  block to mine. Returns true if a primed TNT was hit and defused. */
static cc_bool SurvivalTest_TryDefuseTnt(Vec3 eyePos, Vec3 dir, float reach) {
	struct TntFuse* tnt;
	Vec3 invDir, min, max;
	float t0, t1, bestT = 1.0e30f;
	int i, best = -1;

	invDir.x = Math_SafeDiv(1.0f, dir.x);
	invDir.y = Math_SafeDiv(1.0f, dir.y);
	invDir.z = Math_SafeDiv(1.0f, dir.z);

	for (i = 0; i < TNT_MAX; i++) {
		tnt = &st_tnt[i];
		if (!tnt->active) continue;

		min.x = tnt->pos.x - TNT_HALF; max.x = tnt->pos.x + TNT_HALF;
		min.y = tnt->pos.y - TNT_HALF; max.y = tnt->pos.y + TNT_HALF;
		min.z = tnt->pos.z - TNT_HALF; max.z = tnt->pos.z + TNT_HALF;
		if (!Intersection_RayIntersectsBox(eyePos, invDir, min, max, &t0, &t1)) continue;
		if (t0 > reach) continue;
		if (t0 < bestT) { bestT = t0; best = i; }
	}
	if (best < 0) return false;

	tnt = &st_tnt[best];
	tnt->active = false;
	SurvivalTest_SpawnDropAt(tnt->pos, BLOCK_TNT, 1);
	return true;
}

/* PrimedTnt.tick()'s physics: gravity, a swept move with real block collision, */
/*  then air drag. Entity.move() zeroes each velocity axis it clips, so genuine */
/*  primed TNT lands dead and stops against walls - the yd *= -0.5 "bounce" */
/*  after it always multiplies an already-zeroed yd (dead code, same as items). */
static void SurvivalTest_TntPhysics(struct TntFuse* tnt) {
	struct Entity scratch;
	struct CollisionsComp coll;

	tnt->vel.y -= 0.04f;

	Mem_Set(&scratch, 0, sizeof(scratch));
	scratch.Position.x = tnt->pos.x;
	scratch.Position.y = tnt->pos.y - TNT_HEIGHT_OFF; /* CC Position is the bbox feet */
	scratch.Position.z = tnt->pos.z;
	Vec3_Set(scratch.Size, TNT_SIZE, TNT_SIZE, TNT_SIZE);
	scratch.OnGround   = tnt->onGround;
	scratch.Velocity   = tnt->vel; /* already a per-tick displacement */

	coll.Entity   = &scratch;
	coll.StepSize = 0.0f;
	Collisions_MoveAndWallSlide(&coll);
	Vec3_AddBy(&scratch.Position, &scratch.Velocity);

	tnt->pos.x   = scratch.Position.x;
	tnt->pos.y   = scratch.Position.y + TNT_HEIGHT_OFF;
	tnt->pos.z   = scratch.Position.z;
	tnt->onGround = scratch.OnGround;
	tnt->vel      = scratch.Velocity; /* clipped axes come back zeroed, like move() */

	tnt->vel.x *= 0.98f;
	tnt->vel.y *= 0.98f;
	tnt->vel.z *= 0.98f;
	if (tnt->onGround) {
		tnt->vel.x *= 0.7f;
		tnt->vel.z *= 0.7f;
	}
}

static void SurvivalTest_TickTnt(void) {
	struct TntFuse* tnt;
	IVec3 coords;
	int i;

	SurvivalTest_TickTntSmoke();

	for (i = 0; i < TNT_MAX; i++) {
		tnt = &st_tnt[i];
		if (!tnt->active) continue;

		tnt->prevPos = tnt->pos;
		SurvivalTest_TntPhysics(tnt);

		/* PrimedTnt.tick(): if (life-- > 0) smoke else explode - post-decrement, */
		/*  so a life=40 TNT smokes on 40 ticks (centre + 0.6, drifting up off the */
		/*  top of the cube) and detonates on the 41st, with no puff that tick. */
		if (tnt->ticksLeft-- > 0) {
			SurvivalTest_SpawnTntSmoke(tnt->pos.x, tnt->pos.y + 0.6f, tnt->pos.z);
			continue;
		}

		tnt->active = false;
		SurvivalTest_Explode(tnt->pos, EXPLOSION_RADIUS);

		/* PrimedTnt.tick: 100 TNT-textured TerrainParticles burst out at */
		/*  gaussian offsets after the blast. Approximated with the engine's */
		/*  block-break burst at the detonation point (the gaussian scatter */
		/*  isn't reachable through the public particle API). */
		coords.x = Math_Floor(tnt->pos.x);
		coords.y = Math_Floor(tnt->pos.y);
		coords.z = Math_Floor(tnt->pos.z);
		Particles_BreakBlockEffect(coords, BLOCK_TNT, BLOCK_AIR);
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

/* Untextured white glow shell, drawn additively over the lit TNT cube - same */
/*  technique as the dropped-item twinkle (DropItem_BuildGlowCube), just an */
/*  axis-aligned full block instead of a small spinning item cube. */
#define TNT_GLOW_VERTICES_PER_BLOCK 24
#define TNT_GLOW_MAX_VERTICES (TNT_MAX * TNT_GLOW_VERTICES_PER_BLOCK)
static GfxResourceID st_tntGlowVB;

/* The textured cube faces of the PrimedTnt entity itself (6 faces * 4 verts). */
#define TNT_CUBE_VERTICES_PER_BLOCK 24
#define TNT_CUBE_MAX_VERTICES (TNT_MAX * TNT_CUBE_VERTICES_PER_BLOCK)
static GfxResourceID st_tntCubeVB;

/* One camera-facing quad per smoke puff, textured from particles.png. */
#define TNT_SMOKE_MAX_VERTICES (TNT_SMOKE_MAX * 4)
static GfxResourceID st_tntSmokeVB;

static void TntFuse_BuildGlowCube(float ox, float oy, float oz, PackedCol col, struct VertexColoured** vertices) {
	struct VertexColoured* v = *vertices;
	float x0 = ox, x1 = ox + 1.0f;
	float y0 = oy, y1 = oy + 1.0f;
	float z0 = oz, z1 = oz + 1.0f;

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

/* Appends one textured face of the PrimedTnt cube (unit cube with min corner */
/*  at ox,oy,oz), tinted by a single uniform brightness - matching the original */
/*  model.renderAll(x-0.5, y-0.5, z-0.5, brightness), which does no per-face */
/*  shading. The four corners per face are wound so the tile sits upright. */
static void TntCube_BuildFace(float ox, float oy, float oz, int face,
							   TextureRec r, PackedCol col, struct VertexTextured** vertices) {
	struct VertexTextured* v = *vertices;
	float x0 = ox, x1 = ox + 1.0f;
	float y0 = oy, y1 = oy + 1.0f;
	float z0 = oz, z1 = oz + 1.0f;

	#define TNT_TV(px, py, pz, uu, vv) v->x = (px); v->y = (py); v->z = (pz); v->Col = col; v->U = (uu); v->V = (vv); v++;
	switch (face) {
	case FACE_XMIN:
		TNT_TV(x0,y1,z1, r.u1,r.v1) TNT_TV(x0,y0,z1, r.u1,r.v2) TNT_TV(x0,y0,z0, r.u2,r.v2) TNT_TV(x0,y1,z0, r.u2,r.v1) break;
	case FACE_XMAX:
		TNT_TV(x1,y1,z0, r.u1,r.v1) TNT_TV(x1,y0,z0, r.u1,r.v2) TNT_TV(x1,y0,z1, r.u2,r.v2) TNT_TV(x1,y1,z1, r.u2,r.v1) break;
	case FACE_ZMIN:
		TNT_TV(x0,y1,z0, r.u1,r.v1) TNT_TV(x0,y0,z0, r.u1,r.v2) TNT_TV(x1,y0,z0, r.u2,r.v2) TNT_TV(x1,y1,z0, r.u2,r.v1) break;
	case FACE_ZMAX:
		TNT_TV(x1,y1,z1, r.u1,r.v1) TNT_TV(x1,y0,z1, r.u1,r.v2) TNT_TV(x0,y0,z1, r.u2,r.v2) TNT_TV(x0,y1,z1, r.u2,r.v1) break;
	case FACE_YMIN:
		TNT_TV(x0,y0,z1, r.u1,r.v1) TNT_TV(x1,y0,z1, r.u2,r.v1) TNT_TV(x1,y0,z0, r.u2,r.v2) TNT_TV(x0,y0,z0, r.u1,r.v2) break;
	case FACE_YMAX:
		TNT_TV(x0,y1,z0, r.u1,r.v1) TNT_TV(x1,y1,z0, r.u2,r.v1) TNT_TV(x1,y1,z1, r.u2,r.v2) TNT_TV(x0,y1,z1, r.u1,r.v2) break;
	}
	#undef TNT_TV
	*vertices = v;
}

/* Draws the falling/bouncing TNT cubes themselves. Faces are batched by the */
/*  1D atlas their texture lives in (TNT's top/bottom/side tiles can land in */
/*  different atlases), one lock+draw per atlas - the same scheme the drops and */
/*  the world builder use. */
static void SurvivalTest_RenderTntCubes(float t) {
	struct VertexTextured* data;
	struct VertexTextured* ptr;
	struct TntFuse* tnt;
	TextureLoc loc;
	TextureRec rec;
	PackedCol col;
	Vec3 pos;
	int i, f, atlas, idx, count;
	cc_bool any = false;

	for (i = 0; i < TNT_MAX; i++) { if (st_tnt[i].active) { any = true; break; } }
	if (!any) return;

	if (!st_tntCubeVB) {
		st_tntCubeVB = Gfx_CreateDynamicVb(VERTEX_FORMAT_TEXTURED, TNT_CUBE_MAX_VERTICES);
		if (!st_tntCubeVB) return;
	}

	Gfx_SetAlphaTest(true);
	/* Vertex format set before locking - see SurvivalTest_RenderDropBlocks for why. */
	Gfx_SetVertexFormat(VERTEX_FORMAT_TEXTURED);
	for (atlas = 0; atlas < Atlas1D.Count; atlas++) {
		count = 0;
		data  = (struct VertexTextured*)Gfx_LockDynamicVb(st_tntCubeVB, VERTEX_FORMAT_TEXTURED, TNT_CUBE_MAX_VERTICES);
		ptr   = data;
		for (i = 0; i < TNT_MAX; i++) {
			tnt = &st_tnt[i];
			if (!tnt->active) continue;

			Vec3_Lerp(&pos, &tnt->prevPos, &tnt->pos, t);
			col = DropItem_WorldColor(&pos);
			for (f = 0; f < FACE_COUNT; f++) {
				loc = Block_Tex(BLOCK_TNT, f);
				if (Atlas1D_Index(loc) != atlas) continue;
				rec = Atlas1D_TexRec(loc, 1, &idx);
				TntCube_BuildFace(pos.x - 0.5f, pos.y - 0.5f, pos.z - 0.5f, f, rec, col, &ptr);
				count += 4;
			}
		}
		Gfx_UnlockDynamicVb(st_tntCubeVB);
		if (count) {
			Atlas1D_Bind(atlas);
			Gfx_DrawVb_IndexedTris(count);
		}
	}
	Gfx_SetAlphaTest(false);
}

/* The flashing white overlay - one additive shell per lit TNT cube. */
static void SurvivalTest_RenderTntGlow(float t) {
	struct VertexColoured* data;
	struct VertexColoured* ptr;
	struct TntFuse* tnt;
	PackedCol col;
	Vec3 pos;
	int i, count = 0;
	cc_bool any = false;

	for (i = 0; i < TNT_MAX; i++) { if (st_tnt[i].active) { any = true; break; } }
	if (!any) return;

	if (!st_tntGlowVB) {
		st_tntGlowVB = Gfx_CreateDynamicVb(VERTEX_FORMAT_COLOURED, TNT_GLOW_MAX_VERTICES);
		if (!st_tntGlowVB) return;
	}

	/* Vertex format set before locking - see SurvivalTest_RenderDropBlocks for why. */
	Gfx_SetVertexFormat(VERTEX_FORMAT_COLOURED);
	data = (struct VertexColoured*)Gfx_LockDynamicVb(st_tntGlowVB, VERTEX_FORMAT_COLOURED, TNT_GLOW_MAX_VERTICES);
	ptr  = data;
	for (i = 0; i < TNT_MAX; i++) {
		tnt = &st_tnt[i];
		if (!tnt->active) continue;

		Vec3_Lerp(&pos, &tnt->prevPos, &tnt->pos, t);
		col = PackedCol_Make(255, 255, 255, (cc_uint8)(255.0f * TntFuse_GlowAlpha(tnt->ticksLeft)));
		TntFuse_BuildGlowCube(pos.x - 0.5f, pos.y - 0.5f, pos.z - 0.5f, col, &ptr);
		count += TNT_GLOW_VERTICES_PER_BLOCK;
	}
	Gfx_UnlockDynamicVb(st_tntGlowVB);
	if (!count) return;

	Gfx_SetFaceCulling(true);
	Gfx_SetDepthWrite(false);
	Gfx_SetAlphaBlendingAdditive(true);
	Gfx_DrawVb_IndexedTris_Range(count, 0, DRAW_HINT_NONE);
	Gfx_SetAlphaBlendingAdditive(false);
	Gfx_SetDepthWrite(true);
	Gfx_SetFaceCulling(false);
}

/* The rising smoke puffs - drawn as alpha-tested billboards off particles.png, */
/*  exactly like the original SmokeParticle (greyed by the world's lighting). */
/* Indev EntityItem-style billboard sprites for drops carrying ITEM ids - */
/*  drawn from items.png with the same interpolated position/bob the block */
/*  cubes use. Bails while no items.png is loaded (texture packs supply it). */
#define ITEMDROP_MAX_VERTICES (DROP_MAX * 4 * 4 + 4) /* 4 jumbled copies per stack + held item */
static GfxResourceID st_itemDropVB;

/* Whether the held-item sprite should draw this frame, and its anchor pos - */
/*  in front of the camera, low-right, approximating the first-person hand. */
static cc_bool SurvivalTest_HeldSpriteState(int* id, Vec3* pos) {
	struct LocalPlayer* p = Entities.CurPlayer;
	Vec3 dir, right;
	float yaw;
	if (!IndevTest_Enabled || !p)          return false;
	if (Camera.Active->isThirdPerson)      return false;

	*id = SurvivalTest_SlotId(Inventory.SelectedIndex);
	if (*id < ST_ITEM_ID_START) return false;

	dir = Vec3_GetDirVector(p->Base.Yaw * MATH_DEG2RAD, p->Base.Pitch * MATH_DEG2RAD);
	yaw = p->Base.Yaw * MATH_DEG2RAD;
	right.x = Math_CosF(yaw); right.y = 0.0f; right.z = Math_SinF(yaw);

	*pos    = Camera.CurrentPos;
	pos->x += dir.x * 0.55f + right.x * 0.28f;
	pos->y += dir.y * 0.55f - 0.40f;
	pos->z += dir.z * 0.55f + right.z * 0.28f;
	return true;
}

static void SurvivalTest_RenderItemDropSprites(float t) {
	struct VertexTextured* data;
	struct VertexTextured* ptr;
	struct DropItem* d;
	GfxResourceID tex = IndevTest_ItemsTex();
	TextureRec rec;
	Vec3 pos;
	Vec2 size;
	float renderAge, bob;
	int i, count = 0, heldId;
	cc_bool any = false, held;

	if (!tex) return;
	held = SurvivalTest_HeldSpriteState(&heldId, &pos);
	for (i = 0; i < DROP_MAX; i++) {
		if (st_drops[i].active && !ST_ID_IS_BLOCK(st_drops[i].block)) { any = true; break; }
	}
	if (!any && !held) return;

	if (!st_itemDropVB) {
		st_itemDropVB = Gfx_CreateDynamicVb(VERTEX_FORMAT_TEXTURED, ITEMDROP_MAX_VERTICES);
	}
	Gfx_SetVertexFormat(VERTEX_FORMAT_TEXTURED);
	data = (struct VertexTextured*)Gfx_LockDynamicVb(st_itemDropVB, VERTEX_FORMAT_TEXTURED, ITEMDROP_MAX_VERTICES);
	ptr  = data;

	for (i = 0; i < DROP_MAX; i++) {
		d = &st_drops[i];
		if (!d->active || ST_ID_IS_BLOCK(d->block)) continue;
		if (!IndevTest_ItemSpriteUV(d->block, &rec.u1, &rec.v1, &rec.u2, &rec.v2)) continue;

		Vec3_Lerp(&pos, &d->prevPos, &d->position, t);
		renderAge = Math_Lerp(d->prevAge, d->age, t);
		/* same bob the block cubes use (sin(phase/10) * 0.1 + 0.1) */
		bob = Math_SinF(DropItem_Phase(d, renderAge) / 10.0f) * 0.1f + 0.1f;
		pos.y += bob + 0.125f;

		/* RenderItem.doRender: sprites are 0.5 world units, and a stack draws */
		/*  jumbled copies - 1, 2 (count>1), 3 (count>5), 4 (count>20) - offset */
		/*  by (rand*2-1)*0.3 per axis from a FIXED seed (187), i.e. the same */
		/*  deterministic jumble every frame. Precomputed equivalents below. */
		{
			static const Vec3 jumble[4] = {
				{  0.00f,  0.00f,  0.00f }, {  0.16f, -0.10f,  0.22f },
				{ -0.20f,  0.12f, -0.14f }, {  0.08f, -0.18f, -0.24f }
			};
			int copies = 1, c;
			if (d->count > 1)  copies = 2;
			if (d->count > 5)  copies = 3;
			if (d->count > 20) copies = 4;

			size.x = 0.5f; size.y = 0.5f;
			for (c = 0; c < copies; c++) {
				Vec3 cpos = pos;
				Vec3_AddBy(&cpos, &jumble[c]);
				Particle_DoRender(&size, &cpos, &rec, DropItem_WorldColor(&cpos), ptr);
				ptr   += 4;
				count += 4;
			}
		}
	}
	/* First-person held item - same pipeline as the drop sprites (which is */
	/*  known-good), just anchored to the camera instead of a drop entity. */
	if (held && IndevTest_ItemSpriteUV(heldId, &rec.u1, &rec.v1, &rec.u2, &rec.v2)) {
		SurvivalTest_HeldSpriteState(&heldId, &pos); /* recompute - pos was reused above */
		size.x = 0.35f; size.y = 0.35f;
		Particle_DoRender(&size, &pos, &rec, DropItem_WorldColor(&pos), ptr);
		ptr   += 4;
		count += 4;
	}

	Gfx_BindTexture(tex);
	Gfx_UnlockDynamicVb(st_itemDropVB);
	if (!count) return;

	Gfx_SetAlphaTest(true);
	Gfx_DrawVb_IndexedTris(count);
	Gfx_SetAlphaTest(false);
}

static void SurvivalTest_RenderTntSmoke(float t) {
	struct VertexTextured* data;
	struct VertexTextured* ptr;
	struct TntSmoke* s;
	GfxResourceID tex;
	TextureRec rec;
	Vec3 pos;
	Vec2 size;
	PackedCol lit, col;
	int i, frame, count = 0;
	cc_bool any = false;

	tex = Particles_TexId();
	if (!tex) return; /* particles.png not loaded yet - no smoke until it is */

	for (i = 0; i < TNT_SMOKE_MAX; i++) { if (st_tntSmoke[i].active) { any = true; break; } }
	if (!any) return;

	if (!st_tntSmokeVB) {
		st_tntSmokeVB = Gfx_CreateDynamicVb(VERTEX_FORMAT_TEXTURED, TNT_SMOKE_MAX_VERTICES);
		if (!st_tntSmokeVB) return;
	}

	/* Vertex format set before locking - see SurvivalTest_RenderDropBlocks for why. */
	Gfx_SetVertexFormat(VERTEX_FORMAT_TEXTURED);
	data = (struct VertexTextured*)Gfx_LockDynamicVb(st_tntSmokeVB, VERTEX_FORMAT_TEXTURED, TNT_SMOKE_MAX_VERTICES);
	ptr  = data;
	for (i = 0; i < TNT_SMOKE_MAX; i++) {
		s = &st_tntSmoke[i];
		if (!s->active) continue;

		Vec3_Lerp(&pos, &s->prevPos, &s->pos, t);

		/* SmokeParticle: tex = 7 - (age*8)/life, walking the 8 smoke frames in */
		/*  the top row of the 16-wide atlas from densest puff (7) to wisp (0). */
		frame = 7 - (s->age * 8) / s->life;
		if (frame < 0) frame = 0;
		rec.u1 = frame / 16.0f;
		rec.u2 = rec.u1 + 0.0624375f;
		rec.v1 = 0.0f;
		rec.v2 = 0.0624375f;

		/* rCol=gCol=bCol (the random 0..0.3 grey) * the block's brightness. */
		lit = DropItem_WorldColor(&pos);
		col = PackedCol_Make((cc_uint8)(PackedCol_R(lit) * s->gray),
							 (cc_uint8)(PackedCol_G(lit) * s->gray),
							 (cc_uint8)(PackedCol_B(lit) * s->gray), 255);

		size.x = 0.15f; size.y = 0.15f;
		Particle_DoRender(&size, &pos, &rec, col, ptr);
		ptr   += 4;
		count += 4;
	}
	Gfx_BindTexture(tex);
	Gfx_UnlockDynamicVb(st_tntSmokeVB);
	if (!count) return;

	Gfx_SetAlphaTest(true);
	Gfx_DrawVb_IndexedTris(count);
	Gfx_SetAlphaTest(false);
}

void SurvivalTest_RenderTnt(float delta, float t) {
	if (!SurvivalTest_Enabled) return;
	SurvivalTest_RenderTntCubes(t);  /* the opaque cube first... */
	SurvivalTest_RenderTntGlow(t);   /* ...then the additive flash over it */
	/* Smoke renders independently of live fuses: puffs spawned just before the */
	/*  blast keep drifting and fading for a moment after the entity is gone. */
	SurvivalTest_RenderTntSmoke(t);
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
/* Genuine spawn caps: the periodic spawner allows area*20 live mobs (320 on */
/*  a 256x256x64 map) and the initial population is volume/800 spawn attempts */
/*  of up to 9 mobs each. 256 slots comfortably covers the practical steady- */
/*  state population (the old cap of 32 silently strangled both). */
#define MOB_MAX            256
#define MOB_MAX_HEALTH     20  /* Mob.java's default health - same scale as the player's */
#define MOB_INVINC_TICKS   20  /* Mob.invulnerableDuration - the *full* window; equal-damage hits */
                               /*  are actually only blocked for half of it (see Mob_Hurt) */
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
	int         deathScore; /* points awarded to the player on a credited kill (Mob.deathScore) */
	float       heightOff;  /* Entity.heightOffset - genuine entity.y is feet + this (an */
	                        /*  eye-ish anchor), used for blast distances and LOS rays */
};
/* Order matches MobSpawner.spawn's `type = random.nextInt(6)` exactly, so */
/*  Mob_SpawnerRun can index straight into this table with that roll. */
static const struct MobTypeInfo mobTypeInfo[MOB_TYPE_COUNT] = {
	/* ZOMBIE   */ { "zombie",   MOB_AI_ATTACK,     1.00f, 30.0f, 6, false,  80, 1.62f },
	/* SKELETON */ { "skeleton", MOB_AI_ATTACK,     0.30f,  0.0f, 8, false, 120, 1.62f },
	/* PIG      */ { "pig",      MOB_AI_PASSIVE,    0.70f,  0.0f, 0, false,  10, 1.72f },
	/* CREEPER  */ { "creeper",  MOB_AI_ATTACK,     0.70f, 45.0f, 6, true,  200, 1.62f },
	/* SPIDER   */ { "spider",   MOB_AI_JUMPATTACK, 0.56f,  0.0f, 6, false, 105, 0.72f },
	/* SHEEP    */ { "sheep",    MOB_AI_PASSIVE,    0.70f,  0.0f, 0, false,  10, 1.72f },
};

struct Mob {
	struct Entity Base;
	struct CollisionsComp Collisions;
	cc_uint8 type;
	cc_bool  active;
	cc_bool  hasTarget;
	cc_int8  targetSlot;  /* who hasTarget points at: -1 = the player, else an */
	                      /*  st_mobs[] index (BasicAttackAI.attackTarget - mobs */
	                      /*  aggro onto whatever hurt them, not just the player) */
	cc_bool  jumping;

	int health;
	int lastHealth;  /* health snapshot when the invuln window last opened (Mob.lastHealth) */
	int invincTicks; /* Mob.invulnerableTime - counts down from invulnerableDuration (20) */
	int hurtTicks;    /* red hit-flash timer, purely cosmetic (Mob.hurtTime) */
	int attackTime;   /* swing timer, counts down from 5 after a landed hit (Mob.attackTime); */
	                  /*  drives the zombie/skeleton arm-swing animation at render time */
	int ticksAlive;   /* Mob.tickCount - drives zombie/skeleton arms' idle sway, see Mob_DoAttack */
	int attackDelay;  /* cooldown before this mob can attack again (BasicAttackAI.attackDelay) */
	int deathTicks;   /* ticks since health reached 0 - removed once this exceeds 20 */
	int airTicks;     /* underwater air supply (Mob.airSupply) */
	int noActionTime; /* ticks since last hurt/successful attack - drives the despawn roll below */

	/* BasicAI's wander/chase input axes and turn impulse - decayed every */
	/*  tick and refreshed at random, exactly as in the decompiled source. */
	float moveStrafe, moveForward, turnRate;

	/* Fall damage tracking (Mob.causeFallDamage), mirrors the player's */
	/*  st_falling/st_fallPeakY pair but per-mob since several can be */
	/*  airborne at once. Reads e->Position straight after Mob_Travel each */
	/*  tick, before that tick's result is snapshotted into Base.next for */
	/*  render-time interpolation (see TickOneMob/RenderMobs), so this always */
	/*  sees the fresh, fully-resolved tick position. */
	cc_bool falling;
	float   fallPeakY;

	/* Sheep-only (Sheep.hasFur): true until sheared. A player punch (not an */
	/*  arrow/other source) against a furred sheep shears it instead of */
	/*  dealing damage - drops 1-3 white wool, matching Sheep.hurt(). */
	cc_bool hasFur;
	/* Sheep-only grazing state (Sheep.SheepAI): when a sheep is over grass it */
	/*  stops to graze; after 60 ticks the grass becomes dirt and it has a 1/5 */
	/*  chance to regrow its fur (so a sheared sheep can become shearable again). */
	cc_bool grazing;
	int     grazingTime;
	float   graze, grazeO; /* Sheep.graze/grazeO - eased 0..1 head-dip, lerped at render */

	/* Entity.walkDist/nextStep - footstep sound cadence (Entity.move plays a
	    step sound for EVERY entity with makeStepSound, mobs included) */
	float   walkDist;
	int     nextStep;

	/* Zombie/skeleton-only (HumanoidMob.helmet/armor): independent ~20% rolls */
	/*  made once at spawn time (see SurvivalTest_SpawnMobAt), purely cosmetic - */
	/*  no damage reduction in the original. Forwarded to e->Anim.HasHelmet/ */
	/*  HasArmor every render frame for the zombie/skeleton models to draw. */
	cc_bool hasHelmet, hasArmor;
	/* Debug-only (F9 menu): when set, this mob runs no wander/chase/attack AI - */
	/*  it just stands still (gravity/hurt still apply) so it can be inspected. */
	/*  Never set on naturally-spawned mobs. Not part of c0.30-s parity. */
	cc_bool noAI;
};
static struct Mob st_mobs[MOB_MAX];
/* Which st_mobs[] slot dealt the hurt currently being applied (-1 = player or
    environment). Set by attack/arrow code right before Mob_Hurt so BasicAttack-
    AI.hurt's attackTarget=cause aggro can identify a mob attacker. */
static int st_hurtCauseSlot = -1;

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

/* Spawns 1-2 of the given block at the mob's position. (int)(rand+rand+1.0) */
/*  mathematically only ever yields 1 or 2 - never the "1-3" some ports guess. */
static void Mob_SpawnDeathDrops(struct Mob* m, BlockID block) {
	IVec3 coords;
	int count = (int)(Random_Float(&st_mobRng) + Random_Float(&st_mobRng) + 1.0f);
	int i;

	coords.x = Math_Floor(m->Base.Position.x);
	coords.y = Math_Floor(m->Base.Position.y);
	coords.z = Math_Floor(m->Base.Position.z);
	for (i = 0; i < count; i++) { SurvivalTest_SpawnDrop(coords, block); }
}

/* die(Entity) - called the instant health reaches 0 (separate from the mob's */
/*  20-tick removal delay, which is handled in SurvivalTest_TickOneMob). */
/* playerCredit mirrors `var1 != null` in the decompiled die(Entity var1) - */
/*  every mob type here awards points on a credited kill (Mob.deathScore for */
/*  most types, a flat 10 hardcoded in Pig.die()/Sheep.die() for those two - */
/*  see mobTypeInfo's deathScore column). Pig.die drops 1-2 brown mushrooms; */
/*  Sheep.die drops 1-2 white wool (NOT mushrooms - the two differ). */
static void Mob_Die(struct Mob* m, cc_bool playerCredit) {
	if (playerCredit) st_score += mobTypeInfo[m->type].deathScore;
	if (IndevTest_Enabled) {
		/* Indev EntityLiving.onDeath: rand(3) = 0-2 of scoreValue() as an */
		/*  item id - zombie feather(32), skeleton arrow(6), spider string(31), */
		/*  creeper gunpowder(33), pig raw porkchop(63). Sheep don't override */
		/*  scoreValue and drop nothing on death (wool comes from hitting them). */
		static const cc_int16 indevDeathDrop[MOB_TYPE_COUNT] = {
			/* ZOMBIE */ 256 + 32, /* SKELETON */ 256 + 6, /* PIG */ 256 + 63,
			/* CREEPER */ 256 + 33, /* SPIDER */ 256 + 31, /* SHEEP */ 0
		};
		if (indevDeathDrop[m->type]) {
			int n = Random_Next(&st_mobRng, 3), k;
			for (k = 0; k < n; k++) {
				Vec3 pos;
				pos.x = Math_Floor(m->Base.Position.x) + Random_Float(&st_dropRng) * 0.7f + 0.15f;
				pos.y = Math_Floor(m->Base.Position.y) + Random_Float(&st_dropRng) * 0.7f + 0.15f;
				pos.z = Math_Floor(m->Base.Position.z) + Random_Float(&st_dropRng) * 0.7f + 0.15f;
				SurvivalTest_SpawnDropAt(pos, (cc_uint16)indevDeathDrop[m->type], 1);
			}
		}
	} else if (m->type == MOB_TYPE_PIG) {
		Mob_SpawnDeathDrops(m, BLOCK_BROWN_SHROOM);
	}
	if (!IndevTest_Enabled && m->type == MOB_TYPE_SHEEP) Mob_SpawnDeathDrops(m, BLOCK_WHITE);
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
	Vec3 eye;

	/* damage=3, type=1 (mob-fired): Arrow's constructor picks these whenever */
	/*  the owner isn't a Player - the skeleton qualifies as a Mob owner here. */
	/* Spawn from eye level, not e->Position (feet). Skeleton.shootArrow uses */
	/*  the skeleton's own this.y, which - like every Classic entity - is the */
	/*  eye/camera position, with the bounding box hanging below it. Spawning */
	/*  at CC's feet position births the arrow on the ground, so it instantly */
	/*  collides with the block underfoot and sticks at the skeleton's feet */
	/*  instead of flying at the player (same root bug as the player's Tab-fire). */
	eye = Entity_GetEyePosition(e);
	SurvivalTest_SpawnArrow(eye, yaw, pitch, 1.0f, 3, 1, false, slot);
}

/* SkeletonAI.beforeRemove() - a parting burst of 4-9 pickupable arrows */
/*  scattered above the corpse as it disappears (count = (int)((rand+rand)*3+4), */
/*  which only ever yields 4-9). Owned by the player (not the skeleton) purely */
/*  so they can be picked back up, exactly as in the decompiled source. */
static void Mob_SkeletonDeathBurst(struct Mob* m) {
	struct Entity* e = &m->Base;
	int count = (int)((Random_Float(&st_mobRng) + Random_Float(&st_mobRng)) * 3.0f + 4.0f);
	/* Java's parent.y is the eye/camera position (the bbox hangs below it), so */
	/*  the burst originates up around the body. CC's e->Position is the feet, */
	/*  so spawning there (let alone 0.2 below it) births every arrow inside the */
	/*  ground block, where it instantly collides and sticks invisibly - the same */
	/*  feet-vs-eye bug Mob_ShootArrow documents. Use the eye position instead. */
	Vec3 pos  = Entity_GetEyePosition(e);
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
	/* level.explode(this, x, y, z, 4) - genuine mob.y = feet + heightOffset */
	Vec3 center = m->Base.Position;
	IVec3 coords;
	center.y += mobTypeInfo[m->type].heightOff;
	SurvivalTest_Explode(center, EXPLOSION_RADIUS);

	/* CreeperAI.beforeRemove: 500 LEAVES-textured TerrainParticles burst out */
	/*  at gaussian offsets. Approximated with the engine's block-break burst */
	/*  (see the TNT detonation note in SurvivalTest_TickTnt). */
	IVec3_Floor(&coords, &center);
	Particles_BreakBlockEffect(coords, BLOCK_LEAVES, BLOCK_AIR);
}

/* hurt(Entity attacker, int damage) - implements Mob.java's dual-threshold */
/*  invulnerableTime mechanic: inside the first half of the 20-tick window all */
/*  damage is absorbed; inside the second half only the excess over the hit */
/*  that opened the window (lastHealth - health) lands. knockback() pushes the */
/*  mob directly away from its attacker; aggroes attack-type mobs onto whoever */
/*  hit them (BasicAttackAI.hurt). */
/* playerCredit is distinct from attacker (which is only ever used for the */
/*  knockback direction math below) - it answers "should a kill from this hit */
/*  add to the player's score", matching `awardKillScore` being a no-op for */
/*  every Entity except Player. Arrow hits forward credit via the arrow's */
/*  owner (Arrow.awardKillScore), so they can't just check attacker==player. */
static void Mob_Hurt(struct Mob* m, struct Entity* attacker, int damage, cc_bool playerCredit) {
	struct Entity* e = &m->Base;
	float dx, dz, dist;
	IVec3 coords;
	int woolCount, i;

	if (m->health <= 0)        return;
	if (damage <= 0)           return;

	/* Sheep.hurt(): a Player punch against a still-furred sheep shears it */
	/*  instead of dealing damage at all - drops 1-3 white wool and clears */
	/*  hasFur, then returns without calling the normal hurt() body (so no */
	/*  damage, no invincibility window, no knockback). Only a genuine player */
	/*  punch counts (Entities.CurPlayer is singleplayer's only Player), not */
	/*  arrows or other sources - matches `attacker instanceof Player`. */
	if (m->type == MOB_TYPE_SHEEP && m->hasFur &&
		Entities.CurPlayer && attacker == &Entities.CurPlayer->Base) {
		m->hasFur = false;
		/* Sheep.renderModel only draws the fur layer while hasFur - swap to */
		/*  the engine's furless sheep model so shearing is actually visible. */
		{ cc_string mdl = String_FromReadonly("sheep_nofur"); Entity_SetModel(&m->Base, &mdl); }
		woolCount = (int)(Random_Float(&st_mobRng) * 3.0f + 1.0f); /* 1-3 */

		coords.x = Math_Floor(e->Position.x);
		coords.y = Math_Floor(e->Position.y);
		coords.z = Math_Floor(e->Position.z);
		for (i = 0; i < woolCount; i++) { SurvivalTest_SpawnDrop(coords, BLOCK_WHITE); }
		return;
	}

	/* ai.hurt(cause, damage): aggro + the despawn-timer reset happen on every */
	/*  hit, even one fully absorbed by the invulnerability window below. */
	/* BasicAttackAI.hurt: attackTarget = cause (arrows resolve to their OWNER, */
	/*  see Arrow_ApplyHit), skipped when the cause is the same species - so a */
	/*  stray skeleton arrow turns a zombie against the skeleton, but skeletons */
	/*  never aggro each other. st_hurtCauseSlot carries the attacking mob's */
	/*  slot (-1 = player/environment), set by the caller just before hurting. */
	if (attacker && mobTypeInfo[m->type].ai != MOB_AI_PASSIVE) {
		if (st_hurtCauseSlot < 0) {
			m->hasTarget = true; m->targetSlot = -1;
		} else if (st_mobs[st_hurtCauseSlot].type != m->type) {
			m->hasTarget = true; m->targetSlot = (cc_int8)st_hurtCauseSlot;
		}
	}
	st_hurtCauseSlot = -1; /* consumed - callers must re-arm per hit */
	m->noActionTime = 0; /* BasicAI.hurt: being hurt counts as "doing something" */

	/* Mob.hurt()'s dual-threshold invulnerability. While invulnerableTime is */
	/*  still in the FIRST half of its 20-tick window, a follow-up hit is */
	/*  ignored unless it's strictly stronger than the one that opened the */
	/*  window (and then only the extra damage lands). Once past the halfway */
	/*  point a fresh full hit lands and re-arms the window. The net effect is */
	/*  that equal-damage hits register at most every 10 ticks (0.5s) - half */
	/*  the old flat 1s block - so rapid clicking actually lands repeat hits. */
	if (m->invincTicks > MOB_INVINC_TICKS / 2) {
		if (m->lastHealth - damage >= m->health) return; /* absorbed */
		m->health = m->lastHealth - damage;
	} else {
		m->lastHealth  = m->health;
		m->invincTicks = MOB_INVINC_TICKS;
		m->health     -= damage;
		m->hurtTicks   = 10;
	}

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
		Mob_Die(m, playerCredit);
	}
}

/* Level.explode's entity damage: (int)((1 - dist/radius)*15 + 1) - 16 HP */
/*  point-blank tapering to 1 HP at the rim, 0 beyond. Distance is measured */
/*  from genuine entity.y = feet + heightOffset (1.62 player/humanoids, 1.72 */
/*  sheep/pig, 0.72 spider) - an eye-ish anchor, NOT the bbox centre. */
static int Explosion_Damage(struct Entity* e, float heightOff, Vec3 center, float inv) {
	float dx =  e->Position.x               - center.x;
	float dy = (e->Position.y + heightOff)  - center.y;
	float dz =  e->Position.z               - center.z;
	float dist = Math_SqrtF(dx * dx + dy * dy + dz * dz) * inv;
	return dist <= 1.0f ? (int)((1.0f - dist) * 15.0f + 1.0f) : 0;
}

/* level.explode(attacker, x, y, z, radius) - destroys a sphere of blocks */
/*  around center, then damages every entity (player + mobs) in range. Used by */
/*  both TNT (PrimedTnt's expiry) and the creeper's death blast. */
/* Explosion kills never credit the player's score: attacker is null for TNT */
/*  and the creeper itself for a creeper blast, and awardKillScore only fires */
/*  for a Player attacker - so mobs are hurt with playerCredit=false. attacker */
/*  is passed null (no knockback), matching TNT, the common case; the only */
/*  nuance dropped is a creeper blast knocking surviving mobs back. */
static void SurvivalTest_Explode(Vec3 center, int radius) {
	struct LocalPlayer* p = Entities.CurPlayer;
	struct Mob* m;
	/* Level.explode's exact box: (int)(c - r - 1) to (int)(c + r + 1), each */
	/*  block tested by the distance from its centre (+0.5) to the blast point, */
	/*  strictly inside r. */
	int x0 = (int)(center.x - radius - 1.0f), x1 = (int)(center.x + radius + 1.0f);
	int y0 = (int)(center.y - radius - 1.0f), y1 = (int)(center.y + radius + 1.0f);
	int z0 = (int)(center.z - radius - 1.0f), z1 = (int)(center.z + radius + 1.0f);
	int xx, yy, zz, i, dmg;
	float fx, fy, fz;
	BlockID block;
	IVec3 coords;
	float inv = 1.0f / (float)radius;

	for (yy = y0; yy < y1; yy++) {
	for (zz = z0; zz < z1; zz++) {
	for (xx = x0; xx < x1; xx++) {
		fx = xx + 0.5f - center.x;
		fy = yy + 0.5f - center.y;
		fz = zz + 0.5f - center.z;
		if (fx * fx + fy * fy + fz * fz >= radius * radius) continue;
		if (!World_Contains(xx, yy, zz)) continue;

		block = World_GetBlock(xx, yy, zz);
		if (block == BLOCK_AIR || SurvivalTest_ExplosionImmune(block)) continue;

		/* Level.explode: dropItems(block, ..., 0.3F) is rolled BEFORE the */
		/*  block is cleared, then setTile(...,0). If the destroyed block was */
		/*  TNT, a fresh PrimedTnt is spawned with a randomized PARTIAL fuse */
		/*  (rand.nextInt(life/4) + life/8 = 5-14 ticks) instead of a plain */
		/*  item drop - the classic TNT chain-reaction. */
		coords.x = xx; coords.y = yy; coords.z = zz;
		if (block == BLOCK_TNT) {
			Game_UpdateBlock(xx, yy, zz, BLOCK_AIR);
			SurvivalTest_ArmTnt(coords, TNT_FUSE_TICKS / 8 + Random_Next(&st_dropRng, TNT_FUSE_TICKS / 4));
		} else {
			SurvivalTest_ExplodeDropsForBlock(coords, block);
			Game_UpdateBlock(xx, yy, zz, BLOCK_AIR);
		}
	}}}

	if (p && (dmg = Explosion_Damage(&p->Base, 1.62f, center, inv))) SurvivalTest_Hurt(dmg);

	for (i = 0; i < MOB_MAX; i++) {
		m = &st_mobs[i];
		if (!m->active || m->health <= 0) continue;

		dmg = Explosion_Damage(&m->Base, mobTypeInfo[m->type].heightOff, center, inv);
		if (dmg) Mob_Hurt(m, NULL, dmg, false);
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
	}

	/* BasicAI.update: the water/lava bob roll (80% jump) is applied to EVERY */
	/*  mob unconditionally, not just chasing ones - it's what keeps passive */
	/*  mobs (pigs/sheep) bobbing at the surface instead of sinking and drowning. */
	if (inWater || inLava) m->jumping = Random_Next(&st_mobRng, 100) < 80;
}

/* Sheep.SheepAI.update(): a sheep standing over grass stops to graze. After 60 */
/*  ticks the grass turns to dirt and there's a 1/5 chance it regrows its fur, */
/*  so a sheared sheep can eventually be shearable again. While grazing it holds */
/*  still; the original also bobs the head down, which is cosmetic and omitted. */
/*  The grass block sampled is one in front (0.7 blocks along the body yaw) and */
/*  one below the feet, matching the original's (mob.x+xDiff, mob.y-2, mob.z+zDiff). */
static void Mob_SheepUpdate(struct Mob* m, cc_bool inWater, cc_bool inLava) {
	struct Entity* e = &m->Base;

	/* Sheep.aiStep: graze eases toward grazing at 0.2/tick, clamped 0..1 */
	m->grazeO = m->graze;
	m->graze += m->grazing ? 0.2f : -0.2f;
	Math_Clamp(m->graze, 0.0f, 1.0f);
	float sinYaw = Math_SinF(e->Yaw * MATH_DEG2RAD);
	float cosYaw = Math_CosF(e->Yaw * MATH_DEG2RAD);
	int x = Math_Floor(e->Position.x + 0.7f * sinYaw);
	int y = Math_Floor(e->Position.y) - 1;
	int z = Math_Floor(e->Position.z - 0.7f * cosYaw);
	cc_bool overGrass = World_Contains(x, y, z) && World_GetBlock(x, y, z) == BLOCK_GRASS;

	if (m->grazing) {
		if (!overGrass) {
			m->grazing = false;
		} else {
			if (m->grazingTime++ == 60) {
				Game_UpdateBlock(x, y, z, BLOCK_DIRT);
				if (Random_Next(&st_mobRng, 5) == 0) {
					cc_string mdl = String_FromReadonly("sheep");
					m->hasFur = true;
					Entity_SetModel(&m->Base, &mdl);
				}
			}
			m->moveStrafe  = 0.0f;
			m->moveForward = 0.0f;
			/* SheepAI.update: xRot = 40 + grazingTime/2 % 2 * 10 - the head */
			/*  nods between 40 and 50 degrees pitch while munching. */
			e->Pitch = 40.0f + (float)(m->grazingTime / 2 % 2) * 10.0f;
		}
	} else {
		if (overGrass) {
			m->grazing     = true;
			m->grazingTime = 0;
		}
		Mob_BasicAIUpdate(m, inWater, inLava);
	}
}

/* Faithful port of Level.clip's voxel DDA: walks the grid cells the segment */
/*  from->to passes through (the original's 20-step cap included) and reports */
/*  the first solid, non-liquid block hit. Used as the BasicAttackAI.attack */
/*  line-of-sight gate below. The original also handled non-cube blocks via */
/*  Block.clip (flowers/sprites etc.), but those are COLLIDE_NONE here and */
/*  don't block the ray, which is the same end result. Liquids never block. */
static cc_bool Mob_SightBlocked(Vec3 from, Vec3 to) {
	int x1 = Math_Floor(to.x),   y1 = Math_Floor(to.y),   z1 = Math_Floor(to.z);
	int x0 = Math_Floor(from.x), y0 = Math_Floor(from.y), z0 = Math_Floor(from.z);
	int steps = 20, face;
	float xb, yb, zb, tx, ty, tz, dx, dy, dz;
	BlockID b;

	while (steps-- >= 0) {
		if (x0 == x1 && y0 == y1 && z0 == z1) return false; /* reached target cell: clear */

		xb = yb = zb = 999.0f;
		if (x1 > x0) xb = (float)x0 + 1.0f;
		if (x1 < x0) xb = (float)x0;
		if (y1 > y0) yb = (float)y0 + 1.0f;
		if (y1 < y0) yb = (float)y0;
		if (z1 > z0) zb = (float)z0 + 1.0f;
		if (z1 < z0) zb = (float)z0;

		dx = to.x - from.x; dy = to.y - from.y; dz = to.z - from.z;
		tx = ty = tz = 999.0f;
		if (xb != 999.0f) tx = (xb - from.x) / dx;
		if (yb != 999.0f) ty = (yb - from.y) / dy;
		if (zb != 999.0f) tz = (zb - from.z) / dz;

		/* advance to whichever axis boundary is nearest (genuine face codes: */
		/*  the -X/-Y/-Z faces 5/1/3 need the entered cell nudged back by one) */
		if (tx < ty && tx < tz) {
			face = x1 > x0 ? 4 : 5;
			from.x = xb; from.y += dy * tx; from.z += dz * tx;
		} else if (ty < tz) {
			face = y1 > y0 ? 0 : 1;
			from.x += dx * ty; from.y = yb; from.z += dz * ty;
		} else {
			face = z1 > z0 ? 2 : 3;
			from.x += dx * tz; from.y += dy * tz; from.z = zb;
		}

		x0 = Math_Floor(from.x); if (face == 5) x0--;
		y0 = Math_Floor(from.y); if (face == 1) y0--;
		z0 = Math_Floor(from.z); if (face == 3) z0--;

		if (!World_Contains(x0, y0, z0)) continue;
		b = World_GetBlock(x0, y0, z0);
		if (b != BLOCK_AIR && Blocks.Collide[b] == COLLIDE_SOLID) return true;
	}
	return false;
}

/* BasicAttackAI.doAttack() - acquires/loses the player as a target based on */
/*  distance, faces them, and lands a hit once in range and off cooldown. */
/*  Facing uses CC's own atan2-based formula (re-derived/verified against */
/*  Entity_GetEyePosition/Vec3_GetDirVector), not Java's raw yRot formula. */
static void Mob_DoAttack(struct Mob* m) {
	const struct MobTypeInfo* info = &mobTypeInfo[m->type];
	struct Entity* e = &m->Base;
	struct LocalPlayer* p = Entities.CurPlayer;
	struct Mob* tm = NULL;   /* target mob, when aggroed onto another mob */
	struct Entity* te;       /* target entity - the player or tm */
	float targetHeightOff;
	Vec3 diff;
	float distSq, horDist;
	int damage;

	if (!p) return;

	/* BasicAttackAI.doAttack: a removed/dead attackTarget is dropped */
	if (m->hasTarget && m->targetSlot >= 0) {
		tm = &st_mobs[(int)m->targetSlot];
		if (!tm->active || tm->health <= 0) {
			m->hasTarget = false; m->targetSlot = -1; tm = NULL;
		}
	}
	te = tm ? &tm->Base : &p->Base;
	targetHeightOff = tm ? mobTypeInfo[tm->type].heightOff : 1.62f;

	diff.x = te->Position.x - e->Position.x;
	diff.y = te->Position.y - e->Position.y;
	diff.z = te->Position.z - e->Position.z;
	distSq = diff.x * diff.x + diff.y * diff.y + diff.z * diff.z;

	/* Only the PLAYER is ever acquired by proximity (doAttack's null-target */
	/*  branch checks level.getPlayer() alone) - mob targets come exclusively */
	/*  from being hurt (BasicAttackAI.hurt, see Mob_Hurt). */
	if (!m->hasTarget && distSq <= 256.0f) { m->hasTarget = true; m->targetSlot = -1; } /* aggroRange = 16 */
	if (!m->hasTarget) return;

	if (distSq > 1024.0f && Random_Next(&st_mobRng, 100) == 0) { /* 2x aggroRange give-up roll */
		m->hasTarget = false; m->targetSlot = -1;
		return;
	}

	/* Face the player. CC's Math_Atan2f(x, y) returns atan2(y, x) - the FIRST */
	/*  argument is the cosine (x) axis, the SECOND is the sine (y) axis (see */
	/*  its use in InputHandler's gamepad code: cos(atan2f(x,y))==x). To match */
	/*  Vec3_GetDirVector's basis (dir.x=sin(Yaw), dir.z=-cos(Yaw)) the yaw */
	/*  facing (diff.x, diff.z) is Math_Atan2f(-diff.z, diff.x). Both chase */
	/*  movement (Mob_MoveRelative's sin/cos(Yaw)) and arrow aim */
	/*  (Mob_ShootArrow's Vec3_GetDirVector) depend on this. */
	/* BasicAttackAI.doAttack's pitch is xRot = -atan2(dy, sqrt(distance)) where */
	/*  distance is the 3D distance - so the "adjacent" is sqrt(dist3d), NOT the */
	/*  horizontal distance. A genuine Notch quirk that steeply over-pitches at */
	/*  range; skeleton arrow aim (which reads e->Pitch) depends on reproducing it. */
	horDist  = Math_SqrtF(Math_SqrtF(distSq));
	e->Yaw   = Math_Atan2f(-diff.z, diff.x) * MATH_RAD2DEG;
	e->Pitch = Math_Atan2f(horDist, -diff.y) * MATH_RAD2DEG;

	if (distSq < 4.0f && m->attackDelay <= 0) {
		/* BasicAttackAI.attack: a solid block between the mob's and player's */
		/*  eye points (level.clip from mob.y to player.y = feet + heightOffset) */
		/*  blocks the hit entirely - no damage to either side, and attackDelay */
		/*  is left at 0 so it retries next tick once line of sight clears. */
		Vec3 mc = e->Position;  mc.y  += mobTypeInfo[m->type].heightOff;
		Vec3 pc = te->Position; pc.y  += targetHeightOff;
		if (Mob_SightBlocked(mc, pc)) return;

		m->attackTime   = 5;  /* BasicAttackAI.attack: triggers the model arm swing */
		m->attackDelay  = 10 + Random_Next(&st_mobRng, 20); /* 10-29 ticks (0.5-1.45s) */
		m->noActionTime = 0; /* BasicAttackAI.attack: landing a hit also resets the despawn timer */
		damage = (int)((Random_Float(&st_mobRng) + Random_Float(&st_mobRng)) / 2.0f * info->damage + 1.0f);
		if (tm) {
			st_hurtCauseSlot = (int)(m - st_mobs);
			Mob_Hurt(tm, e, damage, false); /* mob-vs-mob: no score credit */
		} else {
			SurvivalTest_HurtFrom(damage, e->Position);
		}

		/* CreeperAI.attack: this.mob.hurt(entity, 6) - headbutting also hurts */
		/*  the creeper WITH ITS VICTIM AS CAUSE, so each landed hit knocks the */
		/*  creeper back away from them, and when the self-damage kills it */
		/*  (after ~4 hits) the death blast credits its 200 points only if the */
		/*  victim was the player (die(cause) awards Player causes alone). */
		if (info->isCreeper) {
			if (tm) st_hurtCauseSlot = (int)(tm - st_mobs);
			Mob_Hurt(m, te, 6, tm == NULL);
		}
	}
}

/* Mob.tick()'s yBodyRot handling - the body/legs don't just snap to match */
/*  the head's yaw (e->Yaw) every tick, they ease toward the actual movement */
/*  direction at 0.1/tick, and are additionally clamped to stay within +-75 */
/*  degrees of wherever the head is currently looking. This is what lets a */
/*  mob's head swivel ahead to track/look at the player (Mob_DoAttack above) */
/*  while its body and legs visibly lag behind and catch up, instead of the */
/*  whole model rigidly snapping to face the target every tick. e->RotY is */
/*  used directly as the persistent yBodyRot state (nothing else needs RotY */
/*  for mobs), since Model_SetupState already reads it as the body/leg yaw */
/*  and e->Yaw on its own as the head-only yaw (yawDelta = Yaw - RotY). */
static void Mob_UpdateBodyYaw(struct Mob* m, Vec3 oldPos) {
	struct Entity* e = &m->Base;
	float dx   = e->Position.x - oldPos.x;
	float dz   = e->Position.z - oldPos.z;
	float dist = Math_SqrtF(dx * dx + dz * dz);
	float targetYaw = e->RotY;
	float diff;

	if (dist > 0.05f) {
		/* Java computes this as atan2(dz,dx)-90, but that's in Java's yRot */
		/*  convention. e->Yaw/e->RotY here are in ClassiCube's convention, so */
		/*  the body target must match Mob_DoAttack's yaw form exactly: */
		/*  Math_Atan2f(-dz, dx) (recall Math_Atan2f(x,y)==atan2(y,x), so this */
		/*  is the inverse of dir.x=sin(Yaw), dir.z=-cos(Yaw)). Using the */
		/*  swapped form here would ease the body/legs toward the OPPOSITE of */
		/*  the travel direction, making a chasing mob look like it's fleeing. */
		targetYaw = Math_Atan2f(-dz, dx) * MATH_RAD2DEG;
	}

	diff = targetYaw - e->RotY;
	while (diff <  -180.0f) diff += 360.0f;
	while (diff >=  180.0f) diff -= 360.0f;
	e->RotY += diff * 0.1f;

	diff = e->Yaw - e->RotY;
	while (diff <  -180.0f) diff += 360.0f;
	while (diff >=  180.0f) diff -= 360.0f;
	if (diff <  -75.0f) diff =  -75.0f;
	if (diff >=  75.0f) diff =  75.0f;

	e->RotY  = e->Yaw - diff;
	e->RotY += diff * 0.1f;
}

/* BasicAI.tick's post-travel shove pass: level.findEntities(mob, mob.bb.grow( */
/*  0.2,0,0.2)) then e.push(mob) for each pushable neighbour, so mobs don't pile */
/*  into a single point. Entity.push normalises the horizontal centre-to-centre */
/*  delta, divides by the distance AGAIN, then scales by 0.05 - i.e. each axis */
/*  component is 0.05*delta/dist^2 (a soft 1/dist falloff). pushthrough is 0 for */
/*  every mob (only NetworkPlayer sets it to 0.8), so the (1-pushthrough) factor */
/*  is always 1 here. The two mobs get equal-and-opposite shoves, and since this */
/*  runs from BOTH mobs' ticks each pair is processed twice per tick - faithful */
/*  to the original, which has the same double-processing. Debug frozen (noAI) */
/*  mobs are skipped on both sides so they stay put for inspection. */
/* Entity.push(Entity): if `other` overlaps m's grown box, shove the pair */
/*  apart with equal-and-opposite XZ impulses of 0.05/dist^2 (normalise /dist, */
/*  /dist again, *0.05), skipping near-coincident pairs (sqXZDiff >= 0.01). */
static void Mob_PushAgainst(struct Mob* m, const struct AABB* selfBB, struct Entity* other) {
	struct Entity* e = &m->Base;
	struct AABB otherBB;
	float dx, dz, sq, fx, fz;

	Entity_GetBounds(other, &otherBB);
	if (!AABB_Intersects(selfBB, &otherBB)) return;

	dx = e->Position.x - other->Position.x;
	dz = e->Position.z - other->Position.z;
	sq = dx * dx + dz * dz;
	if (sq < 0.01f) return;

	fx = dx / sq * 0.05f;
	fz = dz / sq * 0.05f;
	other->Velocity.x -= fx; other->Velocity.z -= fz;
	e->Velocity.x     += fx; e->Velocity.z     += fz;
}

/* BasicAI.tick: level.findEntities(mob, bb.grow(0.2, 0, 0.2)) then push each */
/*  result - which includes both other mobs AND the player (isPushable=true, */
/*  pushthrough=0 for all, so the same force applies to everyone). */
static void Mob_PushApart(struct Mob* m) {
	struct AABB selfBB;
	int i;
	if (m->noAI) return;

	Entity_GetBounds(&m->Base, &selfBB);
	selfBB.Min.x -= 0.2f; selfBB.Max.x += 0.2f;
	selfBB.Min.z -= 0.2f; selfBB.Max.z += 0.2f;

	for (i = 0; i < MOB_MAX; i++) {
		if (&st_mobs[i] == m || !st_mobs[i].active || st_mobs[i].noAI) continue;
		Mob_PushAgainst(m, &selfBB, &st_mobs[i].Base);
	}
	if (Entities.CurPlayer) Mob_PushAgainst(m, &selfBB, &Entities.CurPlayer->Base);
}

static void SurvivalTest_TickOneMob(struct Mob* m, float delta) {
	const struct MobTypeInfo* info;
	struct Entity* e = &m->Base;
	Vec3 oldPos;
	cc_bool inWater, inLava;

	if (!m->active) return;
	info = &mobTypeInfo[m->type];

	/* Double-buffer position/orientation exactly like LocalInterpComp_AdvanceState */
	/*  does for the player: last tick's resolved state (next) becomes this */
	/*  tick's starting point (prev), and the working fields are reset to it */
	/*  before any AI/movement runs. RenderMobs then blends prev->next by the */
	/*  partial-tick t every frame, same as NetPlayer_RenderModel - without */
	/*  this, mobs only visually moved once per game tick instead of once per */
	/*  render frame, which is what caused the reported stuttery "lower fps" look. */
	e->prev     = e->next;
	e->Position = e->prev.pos;
	e->Yaw      = e->prev.yaw;
	e->Pitch    = e->prev.pitch;
	e->RotY     = e->prev.rotY;

	if (m->invincTicks > 0) m->invincTicks--;
	if (m->hurtTicks   > 0) m->hurtTicks--;
	/* Mob.tick decrements attackTime before the AI runs, so a hit landed this */
	/*  tick (Mob_DoAttack below) leaves it freshly reset to 5 for the swing. */
	if (m->attackTime  > 0) m->attackTime--;
	m->ticksAlive++; /* Mob.tick's this.tickCount++ */

	if (m->health <= 0) {
		m->deathTicks++;
		if (m->deathTicks > 20) {
			if (info->isCreeper)              Mob_CreeperExplode(m);
			if (m->type == MOB_TYPE_SKELETON) Mob_SkeletonDeathBurst(m);
			m->active = false;
			return;
		}
	}

	inWater = ST_InLiquid(e, false);
	inLava  = ST_InLiquid(e, true);

	/* Environmental damage - Mob.tick()'s airSupply/lava handling. Both */
	/*  damage calls go through the same flat invincibility window as combat */
	/*  damage (see Mob_Hurt), so e.g. lava only actually ticks roughly once */
	/*  per second rather than truly every tick. */
	if (SurvivalTest_IsHeadInWater(e)) {
		if (m->airTicks > 0) { m->airTicks--; }
		else                 { Mob_Hurt(m, NULL, 2, false); }
	} else {
		m->airTicks = MOB_AIR_TICKS;
	}
	if (inLava) Mob_Hurt(m, NULL, 10, false);

	if (m->attackDelay > 0) m->attackDelay--;

	if (m->health <= 0) {
		/* BasicAI.tick's freeze branch: no more wandering/attacking, but */
		/*  gravity/physics below still run, so the body settles naturally. */
		m->jumping     = false;
		m->moveStrafe  = 0.0f;
		m->moveForward = 0.0f;
		m->turnRate    = 0.0f;
	} else if (m->noAI) {
		/* Debug frozen mob (F9 menu): skip all wander/chase/attack so it just */
		/*  stands still for inspection. Gravity/physics below still run, and it */
		/*  can still be hurt/killed. Held out of the despawn roll too, so a */
		/*  test mob can't vanish on its own while you're poking at it. */
		m->jumping     = false;
		m->moveStrafe  = 0.0f;
		m->moveForward = 0.0f;
		m->turnRate    = 0.0f;
		m->hasTarget   = false;
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

		if (m->type == MOB_TYPE_SHEEP) {
			Mob_SheepUpdate(m, inWater, inLava);
		} else {
			Mob_BasicAIUpdate(m, inWater, inLava);
		}
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
	/* BasicAI.tick shoves overlapping mobs apart right after travel (modifies */
	/*  velocity, so it takes effect next tick - same as the original). */
	Mob_PushApart(m);
	AnimatedComp_Update(e, oldPos, e->Position, delta);
	Mob_UpdateBodyYaw(m, oldPos);

	/* Entity.move: walkDist += horizontal distance * 0.6; each time it passes
	    the next whole step the block under the feet plays its step sound.
	    playSound's entity overload only reaches 32 blocks (1024 sq) - the
	    engine's API is non-positional, so gate by distance to the player. */
	{
		float sdx = e->Position.x - oldPos.x, sdz = e->Position.z - oldPos.z;
		m->walkDist += Math_SqrtF(sdx * sdx + sdz * sdz) * 0.6f;
		if (m->walkDist > (float)m->nextStep) {
			struct LocalPlayer* sp = Entities.CurPlayer;
			int bx = Math_Floor(e->Position.x);
			int by = Math_Floor(e->Position.y - 0.2f);
			int bz = Math_Floor(e->Position.z);
			BlockID under = World_Contains(bx, by, bz) ? World_GetBlock(bx, by, bz) : BLOCK_AIR;

			m->nextStep++;
			if (under != BLOCK_AIR && sp) {
				sdx = e->Position.x - sp->Base.Position.x;
				sdz = e->Position.z - sp->Base.Position.z;
				if (sdx * sdx + sdz * sdz < 1024.0f)
					Audio_PlayStepSound(Blocks.StepSounds[under]);
			}
		}
	}

	/* Fall damage (Mob.causeFallDamage) - same peak-tracking approach as the */
	/*  player's SurvivalTest_UpdateFall, but using e->Position directly since */
	/*  it's already this tick's fresh, fully-resolved value here (the prev/ */
	/*  next double-buffering below is only for render-time interpolation). */
	/*  Touching liquid cushions the landing, same as for the player. */
	if (inWater || inLava) m->falling = false;

	if (e->OnGround) {
		if (m->falling) {
			float dist = m->fallPeakY - e->Position.y;
			if (dist > FALL_SAFE_BLOCKS) {
				/* Mob.causeFallDamage: (int)Math.ceil(distance - 3) */
				int damage = Math_Ceil(dist - FALL_SAFE_BLOCKS);
				Mob_Hurt(m, NULL, damage, false);
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

	/* Mob.render(): once dead, the model rolls onto its side over the death */
	/*  window - (deathTicks/20)^2*800 degrees, capped at 90 (a fast keel-over */
	/*  that eases to a stop), via the same RotZ roll Entity_GetTransform */
	/*  already applies for the paperdoll. Reset to upright while alive in */
	/*  case a future change ever lets a mob's health recover after dying. */
	/* Mob.render's roll: while hurtTime counts down, sin((t/10)^4 * PI) * 14 */
	/*  degrees of hurt-wobble; once dead, (deathTime/20)^2 * 800 keel-over is */
	/*  ADDED on top, the sum capped at 90. (Genuine rotates this within the */
	/*  hurtDir yaw frame; this port rolls about the model Z axis directly - */
	/*  same simplification the death roll always used here.) */
	{
		float roll = 0.0f;
		if (m->hurtTicks > 0) {
			float ht = (float)m->hurtTicks / 10.0f;
			roll = Math_SinF(ht * ht * ht * ht * MATH_PI) * 14.0f;
		}
		if (m->health <= 0) {
			float deathT = (float)m->deathTicks;
			roll += deathT * deathT * 2.0f; /* (deathT/20)^2 * 800, simplified */
		}
		e->next.rotZ = min(roll, 90.0f);
	}

	/* Snapshot this tick's final, fully-resolved state as the interpolation */
	/*  target - RenderMobs blends prev->next by the partial-tick t every frame. */
	e->next.pos   = e->Position;
	e->next.yaw   = e->Yaw;
	e->next.pitch = e->Pitch;
	e->next.rotY  = e->RotY;
}

/* Ground-validity check shared by both the outer spawn-point roll and the */
/*  inner cluster jitter (MobSpawner.spawn's isSolidTile calls). */
static cc_bool Mob_BlockIsSolid(int x, int y, int z) {
	if (!World_Contains(x, y, z)) return true;
	return Blocks.Collide[World_GetBlock(x, y, z)] == COLLIDE_SOLID;
}

/* Level.isFree's per-block test: any solid OR liquid block occupying the */
/*  candidate mob's bounding box vetoes the spawn. */
static cc_bool Mob_SpawnBlockedBy(BlockID b) {
	return Blocks.Collide[b] == COLLIDE_SOLID || Blocks.Collide[b] == COLLIDE_LIQUID;
}

/* Duplicates the private Entity_GetColor (lighting at the entity's eye */
/*  position) since mobs aren't real Entities.List[] entries, plus a brief */
/*  red hit-flash while hurtTicks counts down from 10 (Mob.hurtTime), */
/*  blended into the lit colour rather than drawn as a separate pass. */
/* Set while RenderMobs is drawing the additive white hit-flash pass, so the */
/*  model VTABLE colour below turns flat translucent white for that pass. */
static cc_bool st_mobFlashPass;

static PackedCol Mob_GetColor(struct Entity* e) {
	struct Mob* m = (struct Mob*)e; /* Base is the first field of struct Mob */
	if (st_mobFlashPass) return PackedCol_Make(255, 255, 255, 191);
	Vec3 eyePos = Entity_GetEyePosition(e);
	IVec3 pos;
	PackedCol col;
	IVec3_Floor(&pos, &eyePos);
	col = Lighting.Color(pos.x, pos.y, pos.z);

	/* Creeper.getBrightness: the classic "creeper flickers brighter as it */
	/*  takes damage" pulse - hurt = (20-health)/20, then the base brightness */
	/*  is scaled by (sin(tickCount)*0.5+0.5)*hurt*0.5 + 0.25 + hurt*0.25. */
	/*  At full health that's a steady 0.25x... note the ORIGINAL is this dim */
	/*  too - creepers genuinely render darker than other mobs. */
	if (mobTypeInfo[m->type].isCreeper) {
		float hurt  = (20 - m->health) / 20.0f;
		float pulse = (Math_SinF((float)m->ticksAlive) * 0.5f + 0.5f) * hurt * 0.5f
		            + 0.25f + hurt * 0.25f;
		col = PackedCol_Scale(col, min(pulse, 1.0f));
	}

	if (m->hurtTicks > 0) {
		float f  = m->hurtTicks / 10.0f;
		int   r  = PackedCol_R(col), g = PackedCol_G(col), b = PackedCol_B(col);
		r = (int)(r + (255 - r) * f);
		g = (int)(g * (1.0f - f));
		b = (int)(b * (1.0f - f));
		col = PackedCol_Make((cc_uint8)r, (cc_uint8)g, (cc_uint8)b, PackedCol_A(col));
	}
	return SurvivalTest_AmbientTint(col);
}

/* Mobs are ticked/rendered by hand (SurvivalTest_TickOneMob/RenderMobs), */
/*  never through generic Entity dispatch - so only GetCol needs to be real, */
/*  since Model_SetupState calls it directly. The rest can stay NULL. */
static const struct EntityVTABLE mob_VTABLE = { NULL, NULL, NULL, Mob_GetColor, NULL, NULL };

static struct Mob* SurvivalTest_SpawnMobAt(cc_uint8 type, Vec3 pos) {
	struct Mob* m;
	struct AABB bb;
	cc_string model;
	int slot = SurvivalTest_FindFreeMobSlot();
	if (slot < 0) return NULL;

	m = &st_mobs[slot];
	Mem_Set(m, 0, sizeof(struct Mob));
	Entity_Init(&m->Base);
	m->Base.VTABLE = &mob_VTABLE;

	model = String_FromReadonly(mobTypeInfo[type].model);
	Entity_SetModel(&m->Base, &model);

	m->Base.Position = pos;

	/* MobSpawner.spawn only adds the mob if level.isFree(mob.bb) - the full */
	/*  bounding box must be clear of solid AND liquid blocks, so wide mobs */
	/*  (pig/sheep/spider) can't spawn clipping into walls that the spawner's */
	/*  1-wide air-column check missed. (m->active is still false, so the slot */
	/*  simply stays free on rejection.) */
	Entity_GetBounds(&m->Base, &bb);
	if (Entity_TouchesAny(&bb, Mob_SpawnBlockedBy)) return NULL;
	m->Base.Yaw      = Random_Float(&st_mobRng) * 360.0f;
	m->Base.RotY     = m->Base.Yaw; /* body faces the same way as the head (see TickOneMob) */

	/* Seed prev/next to the spawn state so the first tick's interpolation */
	/*  (see TickOneMob/RenderMobs) blends from the real spawn point, rather */
	/*  than warping in from Entity_Init's zeroed-out prev/next. */
	m->Base.prev.pos = m->Base.Position; m->Base.next.pos = m->Base.Position;
	m->Base.prev.yaw   = m->Base.Yaw;   m->Base.next.yaw   = m->Base.Yaw;
	m->Base.prev.rotY  = m->Base.RotY;  m->Base.next.rotY  = m->Base.RotY;

	m->Collisions.Entity   = &m->Base;
	m->Collisions.StepSize = 0.5f; /* matches LocalPlayer's default step size */

	m->type       = type;
	m->targetSlot = -1;
	m->nextStep   = 1; /* Entity.nextStep's field initialiser */
	m->health   = MOB_MAX_HEALTH;
	m->airTicks = MOB_AIR_TICKS;
	m->active   = true;
	m->hasFur   = true; /* irrelevant for non-sheep, but harmless */

	/* HumanoidMob's `helmet = Math.random() < 0.2`/`armor = Math.random() < 0.2` */
	/*  field initialisers - only zombies/skeletons extend HumanoidMob, so every */
	/*  other type is faithfully left with neither (pigs/sheep/creepers/spiders */
	/*  have no arms/head shaped to wear plate on in the original anyway). */
	if (type == MOB_TYPE_ZOMBIE || type == MOB_TYPE_SKELETON) {
		m->hasHelmet = Random_Float(&st_mobRng) < 0.2f;
		m->hasArmor  = Random_Float(&st_mobRng) < 0.2f;
	}
	return m;
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
		/* Y is min-of-two-uniforms (biased toward low altitude). Deliberately */
		/*  NOT the min() macro - it re-evaluates the winning argument, which */
		/*  would draw a third RNG float and destroy the bias. */
		float r1 = Random_Float(&st_mobRng), r2 = Random_Float(&st_mobRng);

		type = (cc_uint8)Random_Next(&st_mobRng, MOB_TYPE_COUNT);
		x    = Random_Next(&st_mobRng, World.Width);
		y    = (int)((r1 < r2 ? r1 : r2) * World.Height);
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

				/* MobSpawner.spawn skips the distance check entirely when */
				/*  called with a null avoid entity (the initial population). */
				if (avoidPos) {
					dx = candidate.x - avoidPos->x;
					dy = candidate.y - avoidPos->y;
					dz = candidate.z - avoidPos->z;
					distSq = dx * dx + dy * dy + dz * dz;
					if (distSq < MOB_SPAWN_MIN_DIST_SQ) continue;
				}

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
/*  when a new map finishes loading. It calls spawner.spawn(count, null, ...) */
/*  with NO avoid entity, so mobs may legitimately spawn right at spawn. */
static void SurvivalTest_SpawnInitialMobs(void) {
	cc_int64 volume = (cc_int64)World.Width * World.Height * World.Length;
	int area = (int)(volume / 800);
	if (area > 0) Mob_SpawnerRun(area, NULL);
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

	/* Mobs use cutout textures (e.g. skeleton's gaps between limbs), so this */
	/*  needs alpha test enabled - same as Entities_RenderModels does for */
	/*  ordinary entities. Without it, whatever the prior draw call left the */
	/*  alpha test state as (usually disabled, since RenderDrops disables it) */
	/*  leaks through and cutout regions render solid instead of transparent. */
	Gfx_SetAlphaTest(true);
	for (i = 0; i < MOB_MAX; i++) {
		m = &st_mobs[i];
		if (!m->active) continue;
		e = &m->Base;

		/* Blend prev->next by the partial-tick t, exactly like NetPlayer_RenderModel - */
		/*  mobs only update prev/next once per game tick (TickOneMob), so without */
		/*  this they'd visibly step to a new position/orientation only once per */
		/*  tick instead of every render frame. */
		Vec3_Lerp(&e->Position, &e->prev.pos, &e->next.pos, t);
		Entity_LerpAngles(e, t);

		AnimatedComp_GetCurrent(e, t);
		/* Mob.render: grounded = (attackTime - partialTick)/5, clamped >= 0, and */
		/*  age = tickCount + partialTick. These feed the humanoid attack/idle arm */
		/*  swing (read by the zombie/skeleton models); harmlessly ignored by the */
		/*  other models (pig/sheep/creeper/spider don't use either field). */
		{
			float prog = (float)m->attackTime - t;
			if (prog < 0.0f) prog = 0.0f;
			e->Anim.AttackSwing = prog / 5.0f;
			e->Anim.Age         = (float)m->ticksAlive + t;
		e->Anim.Graze       = Math_Lerp(m->grazeO, m->graze, t);
			e->Anim.HasHelmet   = m->hasHelmet;
			e->Anim.HasArmor    = m->hasArmor;
		}
		e->ShouldRender = Model_ShouldRender(e);
		if (!e->ShouldRender) continue;

		Model_Render(e->Model, e);

		/* Mob.render: while invulnerableTime is within 10 ticks of a fresh */
		/*  hit, the model is drawn a SECOND time additively in translucent */
		/*  white (glColor4f(1,1,1,0.75) + SRC_ALPHA/ONE) - the hit flash. */
		if (m->invincTicks > MOB_INVINC_TICKS - 10 && m->health > 0) {
			st_mobFlashPass = true;
			Gfx_SetAlphaBlendingAdditive(true);
			Gfx_SetDepthWrite(false);
			Model_Render(e->Model, e);
			Gfx_SetDepthWrite(true);
			Gfx_SetAlphaBlendingAdditive(false);
			st_mobFlashPass = false;
		}
	}
	Gfx_SetAlphaTest(false);
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

	/* PrimedTnt is pickable too (PrimedTnt.isPickable()), so a melee swing can */
	/*  hit a lit TNT and defuse it (hurt() with a Player attacker) instead of a */
	/*  mob. Only if it's closer than the best mob though (cap the reach at that */
	/*  mob's distance), so whichever the crosshair actually lands on wins. */
	if (SurvivalTest_TryDefuseTnt(eyePos, dir, best ? bestT : p->ReachDistance)) {
		HeldBlockRenderer_ClickAnim(true);
		return true;
	}
	if (!best) return false;

	/* Minecraft.onMouseClick(0) starts the held-item swing at the very top, */
	/*  before it branches into hurting a mob - so a melee hit swings the arm */
	/*  exactly like mining a block does. The block-delete path plays this */
	/*  itself (InputHandler_DeleteBlock), but that path is skipped when we */
	/*  attack a mob, so trigger the same swing here to match. */
	HeldBlockRenderer_ClickAnim(true);

	/* c0.30 fist: flat 4 HP/hit. Indev (Minecraft.java:352): the held item's */
	/*  getDamageVsEntity - bare fist 1, tools base+tier, swords 4 + tier*2. */
	Mob_Hurt(best, e,
		IndevTest_Enabled ? IndevTest_MeleeDamage(st_inv[Inventory.SelectedIndex].id) : 4,
		true);
	SurvivalTest_DamageHeldTool(2); /* ItemStack.hitEntity: weapons wear 2/hit */
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
	Vec3 prevPos;    /* pos as of the end of the previous tick - RenderArrows blends pos/prevPos */
	                 /*  by the partial-tick t, exactly like Arrow.render()'s xo/x interpolation, */
	                 /*  so arrows move smoothly every render frame instead of jumping once per tick. */
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

	/* TakeEntityAnim - like item drops, a collected arrow flies to the player */
	/*  over 3 ticks ((t/3)^2 ease) before being removed. */
	cc_bool  pickingUp;
	float    pickupTime;
	Vec3     pickupFrom;
};
static struct ArrowEntity st_arrows[ARROW_MAX];
static RNGState st_arrowRng;
static int st_playerArrows = ARROW_PLAYER_START;
static GfxResourceID st_arrowVB;
static GfxResourceID st_arrowsTexId;

/* Genuine c0.30 has no arrow cap either - when the pool fills, evict the */
/*  longest-STUCK arrow first (already inert scenery, nearest to despawning), */
/*  falling back to the oldest in-flight one, so firing never silently fails. */
static int SurvivalTest_FindFreeArrowSlot(void) {
	int i, oldest = 0, bestScore = -1, score;
	for (i = 0; i < ARROW_MAX; i++) {
		if (!st_arrows[i].active) return i;
		/* stuck arrows always outrank in-flight ones; older beats newer */
		score = st_arrows[i].hasHit ? 100000 + st_arrows[i].stickTime : st_arrows[i].age;
		if (score > bestScore) { bestScore = score; oldest = i; }
	}
	st_arrows[oldest].active = false;
	return oldest;
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
	int slot = SurvivalTest_FindFreeArrowSlot(); /* always succeeds - evicts when full */

	dir = Vec3_GetDirVector(yaw * MATH_DEG2RAD, pitch * MATH_DEG2RAD);

	a = &st_arrows[slot];
	Mem_Set(a, 0, sizeof(struct ArrowEntity));

	a->pos.x = pos.x - dir.x * 0.2f;
	a->pos.y = pos.y - dir.y * 0.2f;
	a->pos.z = pos.z - dir.z * 0.2f;
	a->prevPos = a->pos; /* seed so the first frame doesn't lerp in from a zeroed-out (0,0,0) */

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
	/* Entity.setPos centres the bb on the tracked position on ALL axes
	   (bb.y0 = y - bbHeight/2), so the arrow's position is the box CENTRE,
	   not its feet. AABB_Make uses CC's usual feet-at-position convention
	   (Min.y = pos.y), which would sit the box 0.25 too high - making
	   downward/angled shots sink ~0.25 deeper into the ground before the
	   box bottom collides (arrows buried almost flush instead of sticking
	   out). Centre it vertically by hand to match the original. */
	out->Min.x = pos->x - ARROW_WIDTH  * 0.5f;
	out->Min.y = pos->y - ARROW_HEIGHT * 0.5f;
	out->Min.z = pos->z - ARROW_WIDTH  * 0.5f;
	out->Max.x = pos->x + ARROW_WIDTH  * 0.5f;
	out->Max.y = pos->y + ARROW_HEIGHT * 0.5f;
	out->Max.z = pos->z + ARROW_WIDTH  * 0.5f;
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
		/* BasicAttackAI.hurt resolves an Arrow cause to its OWNER - so a mob */
		/*  struck by a skeleton's arrow aggros onto that skeleton (-1 = the */
		/*  player fired it, aggroing onto the player as before). */
		st_hurtCauseSlot = a->ownerIsPlayer ? -1 : a->ownerMobSlot;
		Mob_Hurt(hitMob, &fakeAttacker, a->damage, a->ownerIsPlayer);
	} else {
		SurvivalTest_HurtFrom(a->damage, a->pos);
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
	/* Snapshot this tick's starting position as the interpolation source - */
	/*  RenderArrows blends prevPos->pos by the partial-tick t every frame, */
	/*  same as Arrow.render()'s xo/x blending in the decompiled source. */
	a->prevPos = a->pos;

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

/* Arrow.playerTouch() - fed by the same Player.aiStep bb.grow(1, 0, 1) */
/*  entity sweep as item pickup, so arrows share the items' 1-block sideways */
/*  pickup reach. Only ever applies to player-owned arrows already stuck. */
static void Arrow_TryPickup(struct ArrowEntity* a) {
	struct LocalPlayer* p;
	struct AABB arrowBB, playerBB;
	if (!a->hasHit || !a->ownerIsPlayer)      return;
	if (st_playerArrows >= ARROW_PLAYER_MAX)  return;

	p = Entities.CurPlayer;
	if (!p) return;

	Arrow_BoxAt(&a->pos, &arrowBB);
	Entity_GetBounds(&p->Base, &playerBB);
	playerBB.Min.x -= 1.0f; playerBB.Max.x += 1.0f;
	playerBB.Min.z -= 1.0f; playerBB.Max.z += 1.0f;
	if (!AABB_Intersects(&arrowBB, &playerBB)) return;

	st_playerArrows++;
	/* Arrow.playerTouch: arrows++ happens immediately, then a TakeEntityAnim */
	/*  zips the arrow into the player before the entity is removed. */
	a->pickingUp  = true;
	a->pickupTime = 0.0f;
	a->pickupFrom = a->pos;
}

static void SurvivalTest_TickArrows(void) {
	struct ArrowEntity* a;
	int i;
	for (i = 0; i < ARROW_MAX; i++) {
		a = &st_arrows[i];
		if (!a->active) continue;

		if (a->pickingUp) {
			struct LocalPlayer* p = Entities.CurPlayer;
			float dist;
			a->prevPos = a->pos;
			a->pickupTime += 1.0f / 20.0f;
			dist = a->pickupTime / DROP_PICKUP_ANIM_SECS;
			if (dist > 1.0f) dist = 1.0f;
			dist = dist * dist;
			if (p) {
				a->pos.x = a->pickupFrom.x + ( p->Base.Position.x          - a->pickupFrom.x) * dist;
				a->pos.y = a->pickupFrom.y + ((p->Base.Position.y + 0.62f) - a->pickupFrom.y) * dist;
				a->pos.z = a->pickupFrom.z + ( p->Base.Position.z          - a->pickupFrom.z) * dist;
			}
			if (a->pickupTime >= DROP_PICKUP_ANIM_SECS) a->active = false;
			continue;
		}

		Arrow_Tick(a);
		if (a->active) Arrow_TryPickup(a);
	}
}

static void ArrowsPngProcess(struct Stream* stream, const cc_string* name) {
	Game_UpdateTexture(&st_arrowsTexId, stream, name, NULL, NULL);
}
static struct TextureEntry arrows_entry = { "arrows.png", ArrowsPngProcess };

/* Minecraft Classic's /item/arrows.png (32x32 RGBA). Resources.c pulls the */
/*  real asset from the classic jar into default.zip (same as char.png/ */
/*  zombie.png/etc.), so this embedded copy is just a fallback for before */
/*  that resource exists (e.g. very first launch) - otherwise st_arrowsTexId */
/*  would stay 0 and SurvivalTest_RenderArrows would bail, leaving every */
/*  arrow invisible. A custom pack CAN still override either source via the */
/*  arrows_entry TextureEntry above (Game_UpdateTexture deletes whichever */
/*  texture was active first, so no leak). */
static const cc_uint8 arrows_png[] = {
	0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0x00,0x00,0x00,0x0D,0x49,0x48,0x44,0x52,
	0x00,0x00,0x00,0x20,0x00,0x00,0x00,0x20,0x08,0x06,0x00,0x00,0x00,0x73,0x7A,0x7A,
	0xF4,0x00,0x00,0x00,0x04,0x67,0x41,0x4D,0x41,0x00,0x00,0xB1,0x8F,0x0B,0xFC,0x61,
	0x05,0x00,0x00,0x00,0x18,0x74,0x45,0x58,0x74,0x53,0x6F,0x66,0x74,0x77,0x61,0x72,
	0x65,0x00,0x50,0x61,0x69,0x6E,0x74,0x2E,0x4E,0x45,0x54,0x20,0x76,0x33,0x2E,0x33,
	0x36,0xA9,0xE7,0xE2,0x25,0x00,0x00,0x00,0xD5,0x49,0x44,0x41,0x54,0x58,0x47,0xED,
	0x54,0x31,0x0A,0x02,0x31,0x10,0xBC,0x6F,0xDD,0x23,0xEE,0x01,0x5A,0x58,0x1C,0xBE,
	0x40,0xBF,0x20,0xD8,0x5A,0x0A,0x81,0x68,0x69,0x2D,0x56,0x22,0x84,0xFC,0x24,0xE4,
	0x23,0x2B,0x13,0xC8,0x71,0xC5,0x21,0x2C,0xEC,0x31,0x16,0x09,0x0C,0x09,0x53,0xEC,
	0x4E,0x66,0x27,0xE9,0x72,0xCE,0x82,0x85,0xBD,0x63,0x2D,0xAD,0x88,0x18,0xA3,0x9D,
	0xE0,0xDB,0x69,0x10,0xE0,0xB8,0xEB,0xE5,0x75,0xD9,0x4E,0xF8,0x5C,0xC7,0xC2,0xCF,
	0x39,0x9C,0xC1,0x79,0xEF,0xC5,0x54,0x84,0xD6,0x01,0x08,0x00,0x4C,0x26,0xA6,0x6D,
	0x6E,0xD2,0x74,0xA9,0x08,0x35,0x84,0x10,0x84,0x97,0xB0,0xDA,0xED,0x7E,0x15,0xAE,
	0x23,0x40,0xC0,0xA8,0xCF,0x91,0xE6,0x40,0x75,0x87,0x96,0x81,0x94,0x52,0xB1,0x1E,
	0x3B,0x25,0x03,0x68,0xAA,0x15,0x11,0x42,0xB0,0x13,0xEC,0x87,0xB3,0x00,0x87,0x7E,
	0x2F,0xCF,0xCD,0x7D,0xC2,0x7B,0x7C,0x14,0x7E,0xCE,0xE1,0x0C,0xCE,0x39,0x27,0xA6,
	0x22,0xB4,0x0E,0x40,0x00,0x60,0x32,0x32,0x6D,0x73,0x93,0xA6,0x4B,0x45,0xA8,0x21,
	0xA4,0xFE,0x84,0x75,0x04,0x08,0x18,0xF5,0x39,0xD2,0x7F,0x42,0x7A,0x06,0x56,0x4B,
	0x77,0x2B,0xDC,0x1C,0x68,0x0E,0x34,0x07,0x9A,0x03,0xFF,0xEE,0xC0,0x17,0x4E,0xA7,
	0x0A,0xC6,0xD9,0xC6,0x63,0x0A,0x00,0x00,0x00,0x00,0x49,0x45,0x4E,0x44,0xAE,0x42,
	0x60,0x82,
};

static void SurvivalTest_EnsureArrowTexture(void) {
	struct Stream src;
	struct Bitmap bmp;
	if (st_arrowsTexId) return;

	Stream_ReadonlyMemory(&src, (void*)arrows_png, (cc_uint32)sizeof(arrows_png));
	if (Png_Decode(&bmp, &src)) { Mem_Free(bmp.scan0); return; }

	st_arrowsTexId = Gfx_CreateTexture(&bmp, 0, false);
	Mem_Free(bmp.scan0);
}

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
static void Arrow_BuildVertices(struct ArrowEntity* a, Vec3 renderPos, PackedCol col, struct VertexTextured** vertices) {
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

	center   = renderPos;
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
	Vec3 renderPos;
	int i, count = 0;
	cc_bool any = false;

	if (!SurvivalTest_Enabled) return;
	for (i = 0; i < ARROW_MAX; i++) { if (st_arrows[i].active) { any = true; break; } }
	if (!any) return;

	/* Lazily build the embedded arrow texture (also rebuilds it after a */
	/*  context loss, which deletes st_arrowsTexId). */
	SurvivalTest_EnsureArrowTexture();
	if (!st_arrowsTexId) return;

	if (!st_arrowVB) {
		st_arrowVB = Gfx_CreateDynamicVb(VERTEX_FORMAT_TEXTURED, ARROW_MAX_VERTICES);
		if (!st_arrowVB) return;
	}

	/* Vertex format set before locking - see SurvivalTest_RenderDropBlocks for why. */
	Gfx_SetVertexFormat(VERTEX_FORMAT_TEXTURED);
	ptr = data = (struct VertexTextured*)Gfx_LockDynamicVb(st_arrowVB, VERTEX_FORMAT_TEXTURED, ARROW_MAX_VERTICES);
	for (i = 0; i < ARROW_MAX; i++) {
		a = &st_arrows[i];
		if (!a->active) continue;

		/* Blend prevPos->pos by the partial-tick t - see the comment on */
		/*  ArrowEntity.prevPos above for why this is needed. */
		Vec3_Lerp(&renderPos, &a->prevPos, &a->pos, t);
		col = Lighting.Color(Math_Floor(renderPos.x), Math_Floor(renderPos.y), Math_Floor(renderPos.z));
		Arrow_BuildVertices(a, renderPos, col, &ptr);
		count += ARROW_VERTICES_PER_ARROW;
	}
	Gfx_UnlockDynamicVb(st_arrowVB);

	Gfx_SetAlphaTest(true);
	Gfx_BindTexture(st_arrowsTexId);
	Gfx_DrawVb_IndexedTris_Range(count, 0, DRAW_HINT_NONE);
	Gfx_SetAlphaTest(false);
}

/* Minecraft.java's Tab-fire gate: discrete key-down, survival mode, arrows>0. */
cc_bool SurvivalTest_TryShootArrow(void) {
	struct LocalPlayer* p;
	struct Entity* e;
	Vec3 eye;
	if (!SurvivalTest_Enabled) return false;
	if (st_playerArrows <= 0)  return false;

	p = Entities.CurPlayer;
	if (!p) return false;
	e = &p->Base;

	/* Spawn from eye level, NOT e->Position. In Minecraft Classic the player */
	/*  entity's y field IS the eye/camera position (its bounding box extends */
	/*  downward), so the original "spawns at this.player.y" means eye height. */
	/*  In ClassiCube Entity.Position is the feet - spawning there births the */
	/*  arrow at ground level, where it instantly collides with the block under */
	/*  the player, sticks, and is auto-picked-up the very next tick (refunding */
	/*  the count). That is why firing appeared to do nothing and the count */
	/*  stayed at 20. */
	eye = Entity_GetEyePosition(e);
	SurvivalTest_SpawnArrow(eye, e->Yaw, e->Pitch,
							 ARROW_PLAYER_FIRE_FORCE, ARROW_PLAYER_DAMAGE, 0, true, -1);
	st_playerArrows--;
	return true;
}

int SurvivalTest_ArrowCount(void) { return st_playerArrows; }


/*########################################################################################################################*
*------------------------------------------------------Inventory----------------------------------------------------------*
*#########################################################################################################################*/
/* Returns the BLOCK in a slot - BLOCK_AIR when the slot holds a (future) */
/*  item id, so block-only consumers can never misread an item as a block. */
BlockID SurvivalTest_SlotBlock(int slot) { return ST_ID_BLOCK(st_inv[slot].id); }
/* Raw id (block OR item) - item-sprite renderers key off this + ST_ID range */
int SurvivalTest_SlotId(int slot) { return st_inv[slot].id; }
/* Accumulated ItemStack.itemDamage - drives the HUD durability bar */
int SurvivalTest_SlotDamage(int slot) { return st_inv[slot].damage; }
int     SurvivalTest_SlotCount(int slot) { return st_inv[slot].count; }
int     SurvivalTest_HotbarCount(int slot) { return st_inv[slot].count; }
int     SurvivalTest_InvVersion(void) { return st_invVersion; }
/* Public bump so the Indev layer (furnace tick mutating container slots) */
/*  can tell the open inventory screen to rebuild its mesh. */
void    SurvivalTest_InvChanged(void)  { st_invVersion++; }

static void SurvivalTest_SyncHotbar(void);

/* .mclevel load: restores one inventory slot (id 0 clears the slot). */
void SurvivalTest_RestoreSlot(int slot, int id, int count, int damage) {
	if (!SurvivalTest_Enabled) return;
	if (slot < 0 || slot >= SURVIVAL_INV_SLOTS) return;
	if (count <= 0) id = 0;

	st_inv[slot].id     = (cc_uint16)id;
	st_inv[slot].count  = (cc_int16)(id ? count : 0);
	st_inv[slot].damage = (cc_int16)damage;
	SurvivalTest_SyncHotbar();
}

/* .mclevel load: restores the saved player stats. */
void SurvivalTest_RestoreStats(int health, int score) {
	if (!SurvivalTest_Enabled) return;
	Math_Clamp(health, 0, SURVIVAL_MAX_HEALTH);
	SurvivalTest_Health = health;
	st_lastHealth       = health;
	st_score            = score;
}

cc_bool SurvivalTest_CanPlace(BlockID block) {
	int slot = Inventory.SelectedIndex;
	if (!SurvivalTest_Enabled) return true;

	/* ITEM ids (string, tools, food...) are never placeable - genuine Indev */
	/*  right-click with them does the item's own action or nothing. Without */
	/*  this, the id can leak into the engine's 8-bit block path truncated */
	/*  (e.g. string 287 -> wool 31) and place garbage blocks. */
	if (!ST_ID_IS_BLOCK(st_inv[slot].id)) return false;
	/* Placement always uses the selected hotbar slot */
	return st_inv[slot].count > 0;
}

/* Mirrors the hotbar slots into the engine's inventory table so that the */
/*  hotbar widget and held block renderer reflect the survival inventory. */
static void SurvivalTest_SyncHotbar(void) {
	int i;
	for (i = 0; i < SURVIVAL_HOTBAR_SLOTS; i++) {
		Inventory_Set(i, ST_ID_BLOCK(st_inv[i].id)); /* items render separately (later) */
	}
	st_invVersion++;
}

/* 2x2 pocket crafting grid (GuiInventory's built-in crafting matrix). Slots */
/*  36..39 in the extended addressing below; the result is a virtual slot the */
/*  player takes from, which crafts and consumes one of each grid ingredient. */
static struct SurvivalSlot st_craft[SURVIVAL_CRAFT_SLOTS];
static int st_craftDim = 2; /* 2 = pocket 2x2, 3 = workbench 3x3 */

int SurvivalTest_CraftDim(void) { return st_craftDim; }
void SurvivalTest_SetCraftDim(int dim) {
	SurvivalTest_CraftReturnAll(); /* clear the grid before resizing it */
	st_craftDim = (dim == 3) ? 3 : 2;
}

/* Resolves an extended slot index to its backing SurvivalSlot: 0..35 = the */
/*  real inventory, 36..44 = the crafting grid, 45..71 = the open container */
/*  (chest/furnace tile entity). (The result slot is virtual and handled by */
/*  SurvivalTest_CraftTake, not this.) */
static struct SurvivalSlot* SurvivalTest_SlotPtr(int idx) {
	if (idx >= SURVIVAL_CONTAINER_BASE && idx < SURVIVAL_CONTAINER_BASE + SURVIVAL_CONTAINER_SLOTS)
		return IndevTest_ContainerSlot(idx - SURVIVAL_CONTAINER_BASE);
	if (idx >= SURVIVAL_CRAFT_BASE && idx < SURVIVAL_CRAFT_BASE + SURVIVAL_CRAFT_SLOTS)
		return &st_craft[idx - SURVIVAL_CRAFT_BASE];
	return &st_inv[idx];
}

/* The cursor-held stack (InventoryPlayer.itemStack) - what the mouse is */
/*  carrying between slot clicks in the inventory/crafting screen. */
static struct SurvivalSlot st_cursor;

int SurvivalTest_CursorId(void)    { return st_cursor.id; }
int SurvivalTest_CursorCount(void) { return st_cursor.count; }

/* GuiContainer slot-click rules:                                            */
/*  cursor empty:  left takes the whole stack, right takes the upper half.  */
/*  same id:       left merges as many as fit, right places exactly one.    */
/*  different id (or one side empty): the stacks swap.                      */
void SurvivalTest_SlotClick(int idx, cc_bool rightClick) {
	struct SurvivalSlot* p;
	struct SurvivalSlot tmp;
	int space, moved;
	if (!SurvivalTest_Enabled) return;
	p = SurvivalTest_SlotPtr(idx);

	if (st_cursor.count <= 0) {
		/* Pick up: all, or ceil(half) on right-click */
		if (p->count <= 0) return;
		moved = rightClick ? (p->count + 1) / 2 : p->count;
		st_cursor        = *p;
		st_cursor.count  = (cc_int16)moved;
		p->count        -= (cc_int16)moved;
		if (p->count == 0) { p->id = BLOCK_AIR; p->damage = 0; }
	} else if (p->count > 0 && p->id == st_cursor.id) {
		/* Merge into the slot (respecting the id's max stack size) */
		space = ST_MaxStack(p->id) - p->count;
		if (space <= 0) return;
		moved = rightClick ? 1 : st_cursor.count;
		if (moved > space) moved = space;
		p->count        += (cc_int16)moved;
		st_cursor.count -= (cc_int16)moved;
		if (st_cursor.count == 0) { st_cursor.id = BLOCK_AIR; st_cursor.damage = 0; }
	} else if (p->count <= 0 && rightClick) {
		/* Right-click into an empty slot: place exactly one */
		p->id     = st_cursor.id;
		p->damage = st_cursor.damage;
		p->count  = 1;
		if (--st_cursor.count == 0) { st_cursor.id = BLOCK_AIR; st_cursor.damage = 0; }
	} else {
		/* Different contents (or left-click into empty): swap */
		tmp = *p; *p = st_cursor; st_cursor = tmp;
	}
	st_invVersion++;
	SurvivalTest_SyncHotbar();
}

/* SlotCrafting pickup: crafting yields the result onto the CURSOR (stacking */
/*  when it already holds the same id with room), consuming one of each grid */
/*  ingredient. No-op when the cursor holds something else. */
void SurvivalTest_ResultClick(void) {
	int count, i, id = SurvivalTest_CraftResult(&count);
	if (!id) return;
	if (st_cursor.count > 0 &&
		(st_cursor.id != id || st_cursor.count + count > ST_MaxStack((cc_uint16)id))) return;

	st_cursor.id     = (cc_uint16)id;
	st_cursor.count += (cc_int16)count;
	for (i = 0; i < SURVIVAL_CRAFT_SLOTS; i++) {
		if (st_craft[i].count <= 0) continue;
		if (--st_craft[i].count == 0) { st_craft[i].id = BLOCK_AIR; st_craft[i].damage = 0; }
	}
	st_invVersion++;
	SurvivalTest_SyncHotbar();
}

/* Returns the cursor stack to the inventory - screen close must never eat it */
void SurvivalTest_CursorReturn(void) {
	while (st_cursor.count > 0 && SurvivalTest_AddItem(st_cursor.id)) st_cursor.count--;
	if (st_cursor.count <= 0) { st_cursor.id = BLOCK_AIR; st_cursor.damage = 0; }
	st_invVersion++;
	SurvivalTest_SyncHotbar();
}

void SurvivalTest_SwapSlots(int a, int b) {
	struct SurvivalSlot *pa, *pb, tmp;
	if (!SurvivalTest_Enabled) return;
	if (a == b) return;
	pa = SurvivalTest_SlotPtr(a);
	pb = SurvivalTest_SlotPtr(b);
	tmp = *pa; *pa = *pb; *pb = tmp;
	st_invVersion++;
	SurvivalTest_SyncHotbar();
}

int SurvivalTest_CraftSlotId(int i)    { return st_craft[i].id; }
int SurvivalTest_CraftSlotCount(int i) { return st_craft[i].count; }

/* Builds the 2x2 grid of full-space ids and asks the Indev recipe engine what */
/*  it makes. Returns the result id (0 if nothing), and its count via outCount. */
int SurvivalTest_CraftResult(int* outCount) {
	cc_uint16 grid[9];
	int id, count, i, n = st_craftDim * st_craftDim;
	*outCount = 0;
	if (!IndevTest_Enabled) return 0;

	for (i = 0; i < n; i++) grid[i] = st_craft[i].count > 0 ? st_craft[i].id : 0;
	if (!IndevTest_MatchRecipe(grid, st_craftDim, st_craftDim, &id, &count)) return 0;
	*outCount = count;
	return id;
}

/* SlotCrafting.onPickupFromSlot: crafting the result consumes one of each */
/*  non-empty grid ingredient, then yields the output into the inventory. */
void SurvivalTest_CraftTake(void) {
	int count, id = SurvivalTest_CraftResult(&count);
	int i;
	if (!id) return;

	for (i = 0; i < count; i++) {
		if (!SurvivalTest_AddItem((cc_uint16)id)) break; /* inventory full - stop */
	}
	for (i = 0; i < SURVIVAL_CRAFT_SLOTS; i++) {
		if (st_craft[i].count <= 0) continue;
		if (--st_craft[i].count == 0) { st_craft[i].id = BLOCK_AIR; st_craft[i].damage = 0; }
	}
	st_invVersion++;
	SurvivalTest_SyncHotbar();
}

/* Returns every grid ingredient to the inventory - called when the screen */
/*  closes so crafting materials are never lost (GuiContainer.onGuiClosed). */
void SurvivalTest_CraftReturnAll(void) {
	int i;
	for (i = 0; i < SURVIVAL_CRAFT_SLOTS; i++) {
		while (st_craft[i].count > 0) {
			if (!SurvivalTest_AddItem(st_craft[i].id)) break;
			st_craft[i].count--;
		}
		if (st_craft[i].count <= 0) { st_craft[i].id = BLOCK_AIR; st_craft[i].damage = 0; }
	}
	st_invVersion++;
	SurvivalTest_SyncHotbar();
}

/* Adds one of the given block: stacks onto an existing matching slot if */
/*  possible, otherwise fills the first empty slot (hotbar slots first). */
/* Returns whether it was accepted - Inventory.addResource() returns false on */
/*  a full inventory, and Item.playerTouch then leaves the drop on the ground. */
static cc_bool SurvivalTest_AddItem(cc_uint16 block) {
	int i;
	if (block == BLOCK_AIR) return true;

	/* Prefer topping up an existing, non-full stack of this block */
	for (i = 0; i < SURVIVAL_INV_SLOTS; i++) {
		if (st_inv[i].id == block && st_inv[i].count < ST_MaxStack(st_inv[i].id)) {
			st_inv[i].count++;
			/* Inventory.addResource(): popTime[slot] = 5 triggers the pop animation */
			if (i < SURVIVAL_HOTBAR_SLOTS) HUDScreen_SetSlotPop(i, 5.0f);
			SurvivalTest_SyncHotbar();
			return true;
		}
	}
	/* Otherwise place it into the first empty slot */
	for (i = 0; i < SURVIVAL_INV_SLOTS; i++) {
		if (st_inv[i].id != BLOCK_AIR) continue;
		st_inv[i].id = block;
		st_inv[i].count = 1;
		if (i < SURVIVAL_HOTBAR_SLOTS) HUDScreen_SetSlotPop(i, 5.0f);
		SurvivalTest_SyncHotbar();
		return true;
	}
	return false;
}

/* Consumes one block from the currently selected hotbar slot. */
static void SurvivalTest_ConsumeSelected(void) {
	int slot = Inventory.SelectedIndex;
	if (st_inv[slot].count <= 0) return;

	st_inv[slot].count--;
	if (st_inv[slot].count == 0) st_inv[slot].id = BLOCK_AIR;
	SurvivalTest_SyncHotbar();
}

/* Q key - EntityPlayer.dropPlayerItem: tosses ONE of the held stack out in */
/*  front (velocity = look direction * 0.3/tick, +0.1 upward bias, slight */
/*  jitter), spawned at eye height - 0.3, with a 40-tick self-pickup delay. */
void SurvivalTest_TryDropHeld(void) {
	struct LocalPlayer* p = Entities.CurPlayer;
	struct DropItem* d;
	Vec3 pos, dir;
	int slot = Inventory.SelectedIndex;
	cc_uint16 id;
	int i;
	if (!SurvivalTest_Enabled || !IndevTest_Enabled || !p) return;
	if (st_inv[slot].count <= 0) return;

	id = st_inv[slot].id;
	SurvivalTest_ConsumeSelected();

	pos    = p->Base.Position;
	pos.y += Entity_GetEyeHeight(&p->Base) - 0.3f;
	SurvivalTest_SpawnDropAt(pos, id, 1);

	/* SpawnDropAt gave it the mining pop velocity - replace with the toss */
	for (i = 0; i < DROP_MAX; i++) {
		d = &st_drops[i];
		if (!d->active || d->age > 0.0f) continue;
		if (d->position.x != pos.x || d->position.z != pos.z) continue;

		dir = Vec3_GetDirVector(p->Base.Yaw * MATH_DEG2RAD, p->Base.Pitch * MATH_DEG2RAD);
		d->velocity.x = dir.x * 0.3f * 20.0f + (Random_Float(&st_dropRng) - 0.5f) * 0.04f * 20.0f;
		d->velocity.y = dir.y * 0.3f * 20.0f + 0.1f * 20.0f
		              + (Random_Float(&st_dropRng) - Random_Float(&st_dropRng)) * 0.1f * 20.0f;
		d->velocity.z = dir.z * 0.3f * 20.0f + (Random_Float(&st_dropRng) - 0.5f) * 0.04f * 20.0f;
		d->pickupDelay = 40.0f / 20.0f;
		break;
	}
}

/* Debug: adds one of the given (block or item) id to the inventory. */
void SurvivalTest_DebugGiveItem(int id) {
	if (!SurvivalTest_Enabled) return;
	SurvivalTest_AddItem((cc_uint16)id);
}

/* Right-clicking a placed workbench opens the 3x3 crafting screen; a chest */
/*  or furnace opens its container screen. Returns true (click consumed) so */
/*  no block is placed. The regular E-inventory opens the pocket 2x2; all */
/*  variants share the same screen, differing by CraftDim / open container. */
cc_bool SurvivalTest_TryUseBlock(void) {
	IVec3 pos;
	BlockID block;
	if (!SurvivalTest_Enabled || !IndevTest_Enabled) return false;
	if (!Game_SelectedPos.valid) return false;

	pos = Game_SelectedPos.pos;
	if (!World_Contains(pos.x, pos.y, pos.z)) return false;
	block = World_GetBlock(pos.x, pos.y, pos.z);

	if (IndevTest_IsWorkbench(block)) {
		SurvivalTest_SetCraftDim(3);
		SurvivalInvScreen_Show();
		return true;
	}
	/* BlockChest/BlockFurnace.blockActivated: open the container GUI backed */
	/*  by the tile entity at this position (created lazily on first open). */
	/*  The click is consumed for ANY container block - genuine blockActivated */
	/*  returns true even when a blocked chest refuses to open. */
	if (IndevTest_IsContainerBlock(block)) {
		if (IndevTest_OpenContainer(pos)) SurvivalInvScreen_Show();
		return true;
	}
	return false;
}

cc_bool SurvivalTest_TryEat(void) {
	int slot;
	BlockID block;
	if (!SurvivalTest_Enabled) return false;

	slot  = Inventory.SelectedIndex;
	if (st_inv[slot].count <= 0) return false;
	block = ST_ID_BLOCK(st_inv[slot].id); /* item foods (soup/bread) arrive later */

	/* SurvivalGameMode.useItem: mushrooms are food, eaten with right-click. */
	/*  Red is player.hurt(null, 3) - an ordinary hurt that respects (and */
	/*  re-arms) the invulnerability window, so it can't be spam-eaten faster */
	/*  than any other damage source. */
	/* Indev item foods first: porkchops/bread/apple heal their defs value */
	if (IndevTest_Enabled) {
		int heal = IndevTest_ItemFoodHeal(st_inv[slot].id);
		if (heal > 0) {
			SurvivalTest_Heal(heal);
			SurvivalTest_ConsumeSelected();
			HeldBlockRenderer_ClickAnim(false);
			return true;
		}
	}

	if (block == BLOCK_BROWN_SHROOM) {
		SurvivalTest_Heal(5);            /* brown mushroom restores 5 HP */
	} else if (block == BLOCK_RED_SHROOM) {
		SurvivalTest_Hurt(3);            /* red mushroom is poisonous: -3 HP */
	} else {
		return false;                    /* not food - let normal placement run */
	}

	SurvivalTest_ConsumeSelected();
	/* Minecraft.onMouseClick: a successful useItem resets heldPosition - the */
	/*  held block dips down and rises back as the eating feedback. */
	HeldBlockRenderer_ClickAnim(false);
	return true;
}

static void SurvivalTest_BlockChanged(void* obj,
									  IVec3 coords, BlockID oldBlock, BlockID block) {
	if (!SurvivalTest_Enabled) return;

	if (block == BLOCK_AIR) {
		/* Block was mined - spawn its physical drop(s) on the ground */
		SurvivalTest_SpawnDropsForBlock(coords, oldBlock);
	} else {
		/* Block was placed - consume one from the selected hotbar slot. */
		/* Guarded so a placement that didn't come from the held slot (or a */
		/*  slot holding an item id) can never eat the wrong stack. */
		if (block == ST_ID_BLOCK(st_inv[Inventory.SelectedIndex].id))
			SurvivalTest_ConsumeSelected();
	}
}


/*########################################################################################################################*
*-----------------------------------------------------Block breaking-------------------------------------------------------*
*#########################################################################################################################*/
/* Block.getHardness() - every block's hardness is set once via Block.setData(), */
/*  as (int)(hardnessSeconds * 20.0F) ticks. Values below are taken directly from */
/*  Block.java's static init block. 0 means the block breaks on the very first hit */
/*  (SurvivalGameMode's 3-arg hitBlock() breaks these instantly on click, rather */
/*  than waiting for the continuous per-tick path below). Blocks with no explicit */
/*  c0.30 hardness (i.e. CPE-era blocks that didn't exist yet) default to instant, */
/*  matching this engine's pre-existing creative-style behaviour for them. */
/* Runtime per-block hardness table. Defaults are the faithful c0.30 values; */
/*  kept as DATA rather than a switch so custom blocks (CPE BlockDefs) or a */
/*  future server plugin (e.g. MCGalaxy over a CPE channel) can override */
/*  hardness per block id without touching this code. 0 = instant break. */
static cc_uint16 st_hardness[BLOCK_COUNT];
static cc_bool   st_hardnessInited;
static void SurvivalTest_SeedHardness(void);

void SurvivalTest_SetHardness(BlockID block, int hardness) {
	/* Seed BEFORE overriding: overrides can arrive at component-init time */
	/*  (IndevTest defines its blocks before any mining happens), and the */
	/*  first Hardness() call used to re-seed the WHOLE table afterwards, */
	/*  wiping such early overrides back to the c0.30 default (0 for ids */
	/*  the classic switch doesn't know) - which made the Indev blocks */
	/*  (workbench/chest/furnace) break instantly. */
	SurvivalTest_SeedHardness();
	st_hardness[block] = (cc_uint16)hardness;
}

static int SurvivalTest_DefaultHardness(BlockID block) {
	switch (block) {
		case BLOCK_STONE:       return 20;  /* 1.0s */
		case BLOCK_GRASS:       return 12;  /* 0.6s */
		case BLOCK_DIRT:        return 12;  /* 0.6s */
		case BLOCK_COBBLE:      return 30;  /* 1.5s */
		case BLOCK_WOOD:        return 30;  /* 1.5s (planks) */
		case BLOCK_BEDROCK:     return 19980; /* 999.0s - effectively unbreakable */
		case BLOCK_WATER: case BLOCK_STILL_WATER:
		case BLOCK_LAVA:  case BLOCK_STILL_LAVA:
			return 2000; /* 100.0s */
		case BLOCK_SAND:        return 12;  /* 0.6s */
		case BLOCK_GRAVEL:      return 12;  /* 0.6s */
		case BLOCK_GOLD_ORE: case BLOCK_IRON_ORE: case BLOCK_COAL_ORE:
			return 60;  /* 3.0s */
		case BLOCK_LOG:         return 50;  /* 2.5s */
		case BLOCK_LEAVES:      return 4;   /* 0.2s */
		case BLOCK_SPONGE:      return 12;  /* 0.6s */
		case BLOCK_GLASS:       return 6;   /* 0.3s */
		case BLOCK_RED: case BLOCK_ORANGE: case BLOCK_YELLOW: case BLOCK_LIME:
		case BLOCK_GREEN: case BLOCK_TEAL: case BLOCK_AQUA: case BLOCK_CYAN:
		case BLOCK_BLUE: case BLOCK_INDIGO: case BLOCK_VIOLET: case BLOCK_MAGENTA:
		case BLOCK_PINK: case BLOCK_BLACK: case BLOCK_GRAY: case BLOCK_WHITE:
			return 16;  /* 0.8s (all 16 wool colours) */
		case BLOCK_GOLD:        return 60;  /* 3.0s */
		case BLOCK_IRON:        return 100; /* 5.0s */
		case BLOCK_DOUBLE_SLAB: case BLOCK_SLAB:
			return 20;  /* 1.0s (grouped with stone/mossy cobblestone) */
		case BLOCK_BOOKSHELF:   return 30;  /* 1.5s */
		case BLOCK_MOSSY_ROCKS: return 20;  /* 1.0s */
		case BLOCK_OBSIDIAN:    return 200; /* 10.0s */
		/* BRICK is absent from getHardness's switch, so it falls through to the */
		/*  default 0 - brick breaks instantly. Same for DANDELION, ROSE, both */
		/*  mushrooms, SAPLING and TNT (all explicit hardness 0). */
		default: return 0;
	}
}

static void SurvivalTest_SeedHardness(void) {
	int i;
	if (st_hardnessInited) return;
	for (i = 0; i < BLOCK_COUNT; i++) st_hardness[i] = (cc_uint16)SurvivalTest_DefaultHardness((BlockID)i);
	st_hardnessInited = true;
}

static int SurvivalTest_Hardness(BlockID block) {
	SurvivalTest_SeedHardness();
	return st_hardness[block];
}

/* SurvivalGameMode's 3-arg hitBlock(x,y,z) override (used for the discrete click */
/*  path): true (allow instant break) only when the block has 0 hardness - */
/*  everything else only breaks through the continuous per-tick path below. */
/*  Always true outside survival mode, leaving creative's instant-delete untouched. */
cc_bool SurvivalTest_CanInstaBreak(BlockID block) {
	if (!SurvivalTest_Enabled) return true;
	return SurvivalTest_Hardness(block) == 0;
}

/* ItemStack.damageItem: the held tool takes wear (1 per block broken, 2 per */
/*  landed melee hit) and shatters at ItemTool's maxDamage (32 << tier). */
static void SurvivalTest_DamageHeldTool(int amount) {
	int slot = Inventory.SelectedIndex;
	int maxDamage;
	if (!IndevTest_Enabled) return;

	maxDamage = IndevTest_ToolMaxDamage(st_inv[slot].id);
	if (!maxDamage) return;

	st_inv[slot].damage += amount;
	if (st_inv[slot].damage >= maxDamage) {
		st_inv[slot].id     = BLOCK_AIR;
		st_inv[slot].count  = 0;
		st_inv[slot].damage = 0;
	}
	st_invVersion++;
	SurvivalTest_SyncHotbar();
}

static IVec3 st_breakPos;
static cc_bool st_breaking;
static int st_breakHits;
static int st_breakDelay;

/* SurvivalGameMode.applyBlockCracks: cracks = (hits + time - 1) / hardness, */
/*  0 when no hits yet. (time, the render partial-tick, is omitted here as */
/*  this is only sampled once per frame at an arbitrary phase - the -1/hardness */
/*  offset and denominator are what visibly set the crack stages.) */
float SurvivalTest_BreakProgress(void) {
	int hardness;
	if (!st_breaking || st_breakHits <= 0) return 0.0f;

	hardness = SurvivalTest_Hardness(World_GetBlock(st_breakPos.x, st_breakPos.y, st_breakPos.z));
	if (hardness <= 0) return 0.0f;
	return (float)(st_breakHits - 1) / (float)hardness;
}

cc_bool SurvivalTest_BreakTargeted(IVec3* pos) {
	if (!st_breaking) return false;
	*pos = st_breakPos;
	return true;
}

/* SurvivalGameMode.hitBlock(x,y,z,side)/resetHits() - runs every tick while the */
/*  left mouse button is held down and the player is aiming at a block. Hits */
/*  accumulate on whichever block was targeted last tick; aiming at a different */
/*  block resets the count. Reaching hardness+1 hits breaks the block and starts */
/*  a 5-tick cooldown (hitDelay) before the next block can start accumulating hits. */
static void SurvivalTest_TickBreaking(void) {
	IVec3 pos;
	BlockID block, old;
	int hardness;
	cc_bool holding = !Gui.InputGrab && Input.Pressed[CCMOUSE_L];

	if (!holding || !Game_SelectedPos.valid) {
		st_breaking   = false;
		st_breakHits  = 0;
		st_breakDelay = 0;
		return;
	}

	if (st_breakDelay > 0) { st_breakDelay--; return; }
	pos = Game_SelectedPos.pos;

	if (st_breaking && pos.x == st_breakPos.x && pos.y == st_breakPos.y && pos.z == st_breakPos.z) {
		if (!World_Contains(pos.x, pos.y, pos.z)) { st_breaking = false; st_breakHits = 0; return; }

		block = World_GetBlock(pos.x, pos.y, pos.z);
		if (Blocks.Draw[block] == DRAW_GAS || !Blocks.CanDelete[block]) {
			st_breaking = false; st_breakHits = 0; return;
		}

		hardness = SurvivalTest_Hardness(block);
		/* Indev tools: hits advance at getStrVsBlock speed ((tier+1)*2 when */
		/*  the tool class is effective against the block, else 1). */
		st_breakHits += IndevTest_MiningSpeed(st_inv[Inventory.SelectedIndex].id, block);
		if (st_breakHits >= hardness + 1) {
			SurvivalTest_DamageHeldTool(1);
			old = block;
			Game_ChangeBlock(pos.x, pos.y, pos.z, BLOCK_AIR);
			Event_RaiseBlock(&UserEvents.BlockChanged, pos, old, BLOCK_AIR);

			st_breaking   = false;
			st_breakHits  = 0;
			st_breakDelay = 5;
		}
	} else {
		st_breaking  = true;
		st_breakHits = 0;
		st_breakPos  = pos;
	}
}

static GfxResourceID st_cracksTexId;
static GfxResourceID st_cracksVB;
#define CRACKS_NUM_VERTICES (4 * 6)
/* The crack strip is 10 stages x 16px = 160px wide. 160 isn't a power of two, */
/*  so the bitmap is padded out to CRACKS_TEX_WIDTH before upload (backends like */
/*  D3D11 abort on non-power-of-two textures) - UVs only ever address the real */
/*  160px via the per-stage pixel maths below. */
#define CRACKS_STAGE_PX  16
#define CRACKS_TEX_WIDTH 256

static void CracksPngProcess(struct Stream* stream, const cc_string* name) {
	Game_UpdateTexture(&st_cracksTexId, stream, name, NULL, NULL);
}
static struct TextureEntry cracks_entry = { "cracks.png", CracksPngProcess };

/* The 10 mining-progress crack stages, cropped from genuine c0.30 terrain.png */
/*  (tile indices 240-249) and converted from the original's GL_DST_COLOR* */
/*  GL_SRC_COLOR multiply-blend look into an equivalent black/alpha image - */
/*  alpha = 255 minus the original grayscale value, which is mathematically */
/*  identical to multiplying the destination by the original colour once */
/*  alpha-blended with a pure black source (this engine has no multiply blend */
/*  mode). No ClassiCube texture pack ships a cracks.png, so this asset is */
/*  embedded and uploaded directly here - a custom pack can still override it */
/*  via the cracks_entry TextureEntry above, same pattern as arrows_png. */
static const cc_uint8 cracks_png[] = {
	0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0x00,0x00,0x00,0x0D,0x49,0x48,0x44,0x52,
	0x00,0x00,0x00,0xA0,0x00,0x00,0x00,0x10,0x08,0x06,0x00,0x00,0x00,0x91,0x05,0x74,
	0x58,0x00,0x00,0x01,0x63,0x49,0x44,0x41,0x54,0x78,0xDA,0xED,0xD9,0x4D,0x0E,0xC2,
	0x20,0x10,0x05,0x60,0x0E,0xFB,0x0E,0xD2,0xB3,0xBD,0x93,0xB9,0x71,0x61,0x0C,0x33,
	0xCC,0x1F,0x48,0x2B,0x0B,0x62,0x54,0xBE,0xA6,0xDA,0x17,0x98,0x69,0x5B,0x6B,0xED,
	0x3A,0xE3,0x8C,0xCE,0xE0,0xFB,0x15,0xC2,0xF7,0x10,0xE6,0x4B,0x9E,0xC2,0xE7,0xE7,
	0xCF,0xDE,0xF8,0xE2,0x47,0x2D,0x93,0x96,0x4A,0xC0,0xA8,0x04,0x32,0xE2,0xCF,0x05,
	0x9F,0x14,0x06,0x26,0x02,0x81,0x84,0x47,0xC0,0xF3,0xCB,0xF6,0x3C,0x3A,0xE7,0x56,
	0xE1,0x4F,0x78,0x06,0x1E,0x41,0x8B,0x0F,0x8F,0x05,0xFE,0x3B,0x68,0x9A,0xA7,0xB2,
	0x7A,0xAD,0xF6,0x27,0x3C,0x06,0xCF,0xC0,0xB9,0x8D,0x3C,0x0D,0x2B,0xA7,0xC7,0xF7,
	0x56,0x1E,0x08,0xF3,0x7B,0xEF,0xA3,0x9E,0x49,0xBF,0x75,0x80,0xAA,0x3C,0x03,0xC7,
	0xD5,0x3C,0x8C,0x5B,0xAF,0xE6,0xB1,0xD8,0x43,0x09,0x1B,0x12,0x1E,0x49,0x7F,0x9B,
	0x00,0xD2,0x19,0x1E,0x0A,0x5B,0x04,0x9A,0xBD,0x10,0x7F,0xBA,0xC7,0x06,0x7E,0x5A,
	0x88,0x60,0xE8,0x9C,0x3C,0xDE,0x53,0x4B,0x21,0xE8,0xA9,0x14,0xD2,0x3B,0x78,0x6E,
	0xE8,0x29,0xFC,0x4E,0xAB,0x37,0x87,0x20,0x5B,0x88,0xDF,0xC1,0x23,0xE1,0xB9,0xC0,
	0xA3,0xF9,0x6A,0x4E,0x0C,0x6A,0xC9,0x0A,0x0F,0x65,0xDE,0xC8,0x9B,0xB6,0x60,0xB4,
	0xE7,0xD4,0x82,0x33,0x3D,0x92,0x9E,0x89,0x86,0x47,0x6A,0x5E,0x2E,0x61,0x2B,0xAC,
	0xF0,0x2C,0xF0,0x68,0x6D,0x7E,0x37,0xFB,0x8F,0x01,0x66,0x22,0xC0,0x18,0x74,0xAC,
	0x59,0x8F,0xC1,0x13,0x0B,0x8B,0x97,0x56,0xBB,0x88,0xDF,0xFE,0x3E,0x1E,0x12,0x36,
	0xEA,0xB9,0x99,0x47,0x81,0xA7,0xB2,0x85,0xD2,0xE9,0x59,0xE8,0xB7,0x0E,0x20,0x92,
	0x16,0x49,0x1B,0xA9,0x81,0xF9,0x03,0x4F,0x83,0x87,0xB1,0x4C,0xB0,0x78,0x14,0x78,
	0xCE,0xBE,0x0D,0xF3,0x94,0x81,0xE4,0x7C,0x4F,0x08,0xB5,0x46,0x84,0xC6,0xE7,0xB0,
	0x77,0xF1,0x58,0x55,0x03,0x9E,0x11,0xEF,0xC2,0x7B,0x2B,0xB2,0xF5,0xD6,0x93,0x77,
	0x45,0xA5,0xB0,0x55,0xF7,0x3C,0x0B,0xFD,0x09,0xC8,0xC3,0xC3,0x0E,0x47,0xC8,0x2F,
	0xE5,0xC9,0x86,0x25,0xE8,0x6E,0xFF,0x02,0xA8,0x1C,0x3B,0xE2,0x4B,0x55,0x35,0x85,
	0x00,0x00,0x00,0x00,0x49,0x45,0x4E,0x44,0xAE,0x42,0x60,0x82,
};

static void SurvivalTest_EnsureCracksTexture(void) {
	struct Stream src;
	struct Bitmap bmp, pow2;
	BitmapCol* srcRow;
	BitmapCol* dstRow;
	int x, y, a;
	if (st_cracksTexId) return;

	Stream_ReadonlyMemory(&src, (void*)cracks_png, (cc_uint32)sizeof(cracks_png));
	if (Png_Decode(&bmp, &src)) { Mem_Free(bmp.scan0); return; }

	/* Pad the 160px-wide strip up to a power-of-two width (transparent filler */
	/*  on the right) so it uploads on every backend, not just ones that allow */
	/*  non-power-of-two textures. */
	Bitmap_Allocate(&pow2, CRACKS_TEX_WIDTH, bmp.height);
	Mem_Set(pow2.scan0, 0, Bitmap_DataSize(pow2.width, pow2.height));
	for (y = 0; y < bmp.height; y++) {
		srcRow = Bitmap_GetRow(&bmp,  y);
		dstRow = Bitmap_GetRow(&pow2, y);
		for (x = 0; x < bmp.width; x++) {
			/* The embedded asset's alpha was baked as (255 - grayscale), which */
			/*  is the inverse of a plain dst*src multiply (neutral = white). But */
			/*  genuine c0.30 draws cracks with glBlendFunc(GL_DST_COLOR, */
			/*  GL_SRC_COLOR) = 2*src*dst, whose neutral point is 50% grey - so */
			/*  the tiles have a 50% grey background that left every pixel at */
			/*  alpha 128, tinting the whole face half-black instead of only the */
			/*  crack lines. Re-derive the correct alpha for a black-source blend: */
			/*  out = dst*(1-a) must equal 2*src*dst, so a = 1 - 2*src = 2*aOld-1. */
			a = 2 * BitmapCol_A(srcRow[x]) - 255;
			if (a < 0) a = 0;
			dstRow[x] = BitmapCol_Make(0, 0, 0, a);
		}
	}

	st_cracksTexId = Gfx_CreateTexture(&pow2, 0, false);
	Mem_Free(bmp.scan0);
	Mem_Free(pow2.scan0);
}

static void Cracks_AddFace(struct VertexTextured** ptr, Vec3 a, Vec3 b, Vec3 c, Vec3 d,
							float u0, float u1, PackedCol col) {
	struct VertexTextured* v = *ptr;
	v[0].x = a.x; v[0].y = a.y; v[0].z = a.z; v[0].U = u0; v[0].V = 1.0f; v[0].Col = col;
	v[1].x = b.x; v[1].y = b.y; v[1].z = b.z; v[1].U = u1; v[1].V = 1.0f; v[1].Col = col;
	v[2].x = c.x; v[2].y = c.y; v[2].z = c.z; v[2].U = u1; v[2].V = 0.0f; v[2].Col = col;
	v[3].x = d.x; v[3].y = d.y; v[3].z = d.z; v[3].U = u0; v[3].V = 0.0f; v[3].Col = col;
	*ptr += 4;
}

/* Minecraft.java's applyCracks()+render(): builds an inflated copy of the */
/*  targeted block's 6 faces (GL11.glScalef(1.01,1.01,1.01) around its centre, */
/*  to avoid z-fighting with the block underneath), textured with whichever of */
/*  the 10 crack stages matches the current mining progress. */
void SurvivalTest_RenderCracks(float delta, float t) {
	struct VertexTextured* data;
	struct VertexTextured* ptr;
	IVec3 targetPos;
	float x0, y0, z0, x1, y1, z1, cx, cy, cz;
	float progress, u0, u1;
	int stage;
	PackedCol col = PACKEDCOL_WHITE;
	/* Inflate the overlay just enough to win the depth test against the block */
	/*  face without z-fighting. The genuine client uses 1.01 (0.005-block */
	/*  overhang) because its multiply blend makes the part that pokes out past */
	/*  the block invisible; our black+alpha approximation instead shows that */
	/*  overhang as dark slivers against the air/neighbouring blocks, so the */
	/*  overhang is kept minimal (cracks only ever draw on the block you're */
	/*  right next to, so a tiny offset is plenty to avoid flicker). */
	const float scale = 1.002f;

	if (!SurvivalTest_Enabled) return;
	if (!SurvivalTest_BreakTargeted(&targetPos)) return;

	progress = SurvivalTest_BreakProgress();
	if (progress <= 0.0f) return;

	stage = (int)(progress * 10.0f);
	if (stage > 9) stage = 9;
	u0 = (stage       * CRACKS_STAGE_PX) / (float)CRACKS_TEX_WIDTH;
	u1 = ((stage + 1) * CRACKS_STAGE_PX) / (float)CRACKS_TEX_WIDTH;

	SurvivalTest_EnsureCracksTexture();
	if (!st_cracksTexId) return;

	if (!st_cracksVB) {
		st_cracksVB = Gfx_CreateDynamicVb(VERTEX_FORMAT_TEXTURED, CRACKS_NUM_VERTICES);
		if (!st_cracksVB) return;
	}

	cx = (Game_SelectedPos.Min.x + Game_SelectedPos.Max.x) * 0.5f;
	cy = (Game_SelectedPos.Min.y + Game_SelectedPos.Max.y) * 0.5f;
	cz = (Game_SelectedPos.Min.z + Game_SelectedPos.Max.z) * 0.5f;
	x0 = cx + (Game_SelectedPos.Min.x - cx) * scale;
	y0 = cy + (Game_SelectedPos.Min.y - cy) * scale;
	z0 = cz + (Game_SelectedPos.Min.z - cz) * scale;
	x1 = cx + (Game_SelectedPos.Max.x - cx) * scale;
	y1 = cy + (Game_SelectedPos.Max.y - cy) * scale;
	z1 = cz + (Game_SelectedPos.Max.z - cz) * scale;

	/* Vertex format must be set before locking the VB, not after - some backends */
	/*  (e.g. D3D11) rebind the dynamic VB's stride as part of unlocking it, using */
	/*  whatever vertex format is currently active. Since SelOutlineRenderer (drawn */
	/*  right before this) leaves the format set to VERTEX_FORMAT_COLOURED, setting */
	/*  our own format only after the lock/unlock would bind this VB with the wrong */
	/*  (smaller) stride, scrambling every vertex past the first. */
	Gfx_SetVertexFormat(VERTEX_FORMAT_TEXTURED);

	ptr = data = (struct VertexTextured*)Gfx_LockDynamicVb(st_cracksVB, VERTEX_FORMAT_TEXTURED, CRACKS_NUM_VERTICES);
	Cracks_AddFace(&ptr, Vec3_Create3(x0,y0,z0), Vec3_Create3(x1,y0,z0), Vec3_Create3(x1,y0,z1), Vec3_Create3(x0,y0,z1), u0, u1, col); /* YMin */
	Cracks_AddFace(&ptr, Vec3_Create3(x0,y1,z0), Vec3_Create3(x0,y1,z1), Vec3_Create3(x1,y1,z1), Vec3_Create3(x1,y1,z0), u0, u1, col); /* YMax */
	Cracks_AddFace(&ptr, Vec3_Create3(x0,y0,z0), Vec3_Create3(x0,y0,z1), Vec3_Create3(x0,y1,z1), Vec3_Create3(x0,y1,z0), u0, u1, col); /* XMin */
	Cracks_AddFace(&ptr, Vec3_Create3(x1,y0,z1), Vec3_Create3(x1,y0,z0), Vec3_Create3(x1,y1,z0), Vec3_Create3(x1,y1,z1), u0, u1, col); /* XMax */
	Cracks_AddFace(&ptr, Vec3_Create3(x1,y0,z0), Vec3_Create3(x0,y0,z0), Vec3_Create3(x0,y1,z0), Vec3_Create3(x1,y1,z0), u0, u1, col); /* ZMin */
	Cracks_AddFace(&ptr, Vec3_Create3(x0,y0,z1), Vec3_Create3(x1,y0,z1), Vec3_Create3(x1,y1,z1), Vec3_Create3(x0,y1,z1), u0, u1, col); /* ZMax */
	Gfx_UnlockDynamicVb(st_cracksVB);

	Gfx_SetDepthWrite(false);
	Gfx_SetAlphaBlending(true);
	Gfx_BindTexture(st_cracksTexId);
	Gfx_DrawVb_IndexedTris_Range(CRACKS_NUM_VERTICES, 0, DRAW_HINT_NONE);
	Gfx_SetAlphaBlending(false);
	Gfx_SetDepthWrite(true);
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

	/* Blocks.Collide reduces COLLIDE_WATER/COLLIDE_LAVA down to the simpler */
	/*  COLLIDE_LIQUID for movement purposes - ExtendedCollide keeps the two */
	/*  distinct, which is what's needed here to tell water apart from lava. */
	b = World_GetBlock(x, y, z);
	return Blocks.ExtendedCollide[b] == COLLIDE_WATER;
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
				/* Mob.causeFallDamage: (int)Math.ceil(distance - 3) - the ceil */
				/*  means even a 3.1-block fall deals its first HP. */
				int damage = Math_Ceil(dist - FALL_SAFE_BLOCKS);
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

	/* While dead the Game Over screen is up and the world is frozen, but */
	/*  deathTime keeps counting - it drives the death camera roll + FOV zoom */
	/*  (the projection matrix is cached, so poke it while the FOV animates). */
	if (st_isDead) {
		st_deathTicks++;
		Camera_UpdateProjection();
		return;
	}

	if (st_invincTimer > 0.0f) {
		st_invincTimer -= delta;
		if (st_invincTimer < 0.0f) st_invincTimer = 0.0f;
	}
	/* Mob.tick(): hurtTime decrements by a flat 1 per tick (not by delta) */
	if (st_hurtTicks > 0) st_hurtTicks--;

	onGround    = e->OnGround;
	inLava      = ST_InLiquid(e, true);
	inWater     = ST_InLiquid(e, false);
	headInWater = SurvivalTest_IsHeadInWater(e);

	/* Fall damage -------------------------------------------------------- */
	/* Mob.tick resets fallDistance for water ONLY (isInWater) - landing in */
	/*  shallow lava does NOT cushion a fall. */
	if (inWater) st_falling = false;
	SurvivalTest_UpdateFall(e, p, onGround);

	/* Lava damage - Mob.tick: hurt(null, 10) every tick; the invulnerability */
	/*  window's dual threshold (see SurvivalTest_Damage) is what shapes this */
	/*  into the effective 10 HP per half-second cadence. */
	if (inLava) SurvivalTest_Hurt(LAVA_DAMAGE);

	/* Drowning - Mob.tick: airSupply-- while the head is underwater, then */
	/*  hurt(null, 2) every tick once it's empty; instant refill on surfacing. */
	st_headInWater = headInWater; /* exposed to the HUD for the air bubbles */
	if (headInWater) {
		st_airTimer -= delta;
		if (st_airTimer <= 0.0f) {
			st_airTimer = 0.0f;
			SurvivalTest_Hurt(DROWN_DAMAGE);
		}
	} else {
		st_airTimer = AIR_SUPPLY_SECS;
	}

	/* Dropped items --------------------------------------------------------- */
	SurvivalTest_TickDrops(e, delta);

	/* Block breaking --------------------------------------------------------- */
	SurvivalTest_TickBreaking();

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
/* GameOverScreen's Respawn button - the genuine c0.30 death screen DOES offer */
/*  respawning (the earlier claim here that "death ends the world" was wrong): */
/*  every inventory slot is cleared, health and arrows are restored, the air */
/*  supply resets to a mere 20 ticks (a genuine quirk - NOT the full 300), and */
/*  the player teleports back to the spawn point. Score persists, and the */
/*  death-scattered inventory drops stay in the world to be re-collected. */
void SurvivalTest_Respawn(void) {
	struct LocalPlayer* p = Entities.CurPlayer;
	struct LocationUpdate update;
	int i;
	if (!SurvivalTest_Enabled || !p) return;

	for (i = 0; i < SURVIVAL_INV_SLOTS; i++) {
		st_inv[i].id = BLOCK_AIR;
		st_inv[i].count = 0;
	}
	SurvivalTest_SyncHotbar();

	SurvivalTest_Health = SURVIVAL_MAX_HEALTH;
	st_lastHealth   = SURVIVAL_MAX_HEALTH;
	st_invincTimer  = 0.0f;
	st_hurtTicks    = 0;
	st_falling      = false;
	st_airTimer     = 20.0f / 20.0f; /* airSupply = 20 ticks, not 300 */
	st_playerArrows = ARROW_PLAYER_START;
	st_isDead       = false;
	st_deathTicks   = 0;
	Camera_UpdateProjection(); /* undo the death FOV zoom immediately */

	/* Player.resetPos() - back to the spawn point, facing its stored angles */
	update.flags = LU_HAS_POS | LU_HAS_YAW | LU_HAS_PITCH | LU_POS_ABSOLUTE_INSTANT;
	update.pos   = p->Spawn;
	update.yaw   = p->SpawnYaw;
	update.pitch = p->SpawnPitch;
	p->Base.VTABLE->SetLocation(&p->Base, &update);
	Vec3_Set(p->Base.Velocity, 0.0f, 0.0f, 0.0f);
}

static void SurvivalTest_ResetState(void) {
	int i;
	st_falling      = false;
	st_isDead       = false;
	st_deathTicks   = 0;
	st_score        = 0;
	st_invincTimer  = 0.0f;
	st_lastHealth   = SURVIVAL_MAX_HEALTH;
	st_hurtTicks    = 0;
	st_hurtDir      = 0.0f;
	st_breaking     = false;
	st_breakHits    = 0;
	st_breakDelay   = 0;
	st_airTimer     = AIR_SUPPLY_SECS;
	SurvivalTest_Health = SURVIVAL_MAX_HEALTH;

	for (i = 0; i < SURVIVAL_INV_SLOTS; i++) {
		st_inv[i].id = BLOCK_AIR;
		st_inv[i].count = 0;
	}
	/* SurvivalGameMode.apply(Player): the player always starts with 10 TNT */
	/*  in the last hotbar slot - this was missing entirely before. */
	st_inv[8].id = BLOCK_TNT;
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
	for (i = 0; i < TNT_SMOKE_MAX; i++) {
		st_tntSmoke[i].active = false;
	}
}

/* The item vertex buffer is a GPU resource and must be dropped/recreated */
/*  whenever the graphics context is lost (it is rebuilt lazily on render). */
static void SurvivalTest_OnContextLost(void* obj) {
	Gfx_DeleteDynamicVb(&st_itemVB);
	Gfx_DeleteDynamicVb(&st_itemDropVB);
	Gfx_DeleteDynamicVb(&st_glowVB);
	Gfx_DeleteDynamicVb(&st_arrowVB);
	Gfx_DeleteDynamicVb(&st_tntGlowVB);
	Gfx_DeleteDynamicVb(&st_tntCubeVB);
	Gfx_DeleteDynamicVb(&st_tntSmokeVB);
	Gfx_DeleteDynamicVb(&st_cracksVB);
	if (!Gfx.ManagedTextures) {
		Gfx_DeleteTexture(&st_arrowsTexId);
		Gfx_DeleteTexture(&st_cracksTexId);
	}
}

static void SurvivalTest_Init(void) {
	/* Loaded unconditionally so the inventory screen can read it even before */
	/*  any survival logic runs (it gates a UI choice, not a gameplay rule). */
	SurvivalTest_Enhanced = Options_GetBool(OPT_SURVIVAL_ENHANCED, false);

	/* The survival core also runs under Indev mode - IndevTest_Component's
	    Init ran first (see Game.c ordering), so its flag is already set. */
	SurvivalTest_Enabled = SurvivalTest_Gamemode() != SURVIVAL_GAMEMODE_OFF;
	if (!SurvivalTest_Enabled) return;

	Random_SeedFromCurrentTime(&st_dropRng);
	Random_SeedFromCurrentTime(&st_mobRng);
	Random_SeedFromCurrentTime(&st_arrowRng);
	SurvivalTest_ResetState();

	/* Renderer.updateFog: Survival Test's lava fog is denser than ClassiCube's
	    stock value (EXP density 2.0 vs 1.8) - override while survival is on. */
	Blocks.FogDensity[BLOCK_LAVA]       = 2.0f;
	Blocks.FogDensity[BLOCK_STILL_LAVA] = 2.0f;

	/* GameMode.breakBlock quirks: breaking SAND plays the GRAVEL sound, and */
	/*  glass breaks with its METAL step sound (stone at 2x pitch) - c0.30 has */
	/*  no glass shatter sound. Overridden here so creative stays stock. */
	Blocks.DigSounds[BLOCK_SAND]  = SOUND_GRAVEL;
	Blocks.DigSounds[BLOCK_GLASS] = SOUND_METAL;
	ScheduledTask_Add(GAME_DEF_TICKS, SurvivalTest_Tick);
	Event_Register_(&UserEvents.BlockChanged, NULL, SurvivalTest_BlockChanged);
	Event_Register_(&GfxEvents.ContextLost,   NULL, SurvivalTest_OnContextLost);
	TextureEntry_Register(&arrows_entry);
	TextureEntry_Register(&cracks_entry);
}

static void SurvivalTest_Free(void) {
	if (!SurvivalTest_Enabled) return;
	Event_Unregister_(&UserEvents.BlockChanged, NULL, SurvivalTest_BlockChanged);
	Event_Unregister_(&GfxEvents.ContextLost,   NULL, SurvivalTest_OnContextLost);
	Gfx_DeleteDynamicVb(&st_itemVB);
	Gfx_DeleteDynamicVb(&st_glowVB);
	Gfx_DeleteDynamicVb(&st_arrowVB);
	Gfx_DeleteDynamicVb(&st_tntGlowVB);
	Gfx_DeleteDynamicVb(&st_tntCubeVB);
	Gfx_DeleteDynamicVb(&st_tntSmokeVB);
	Gfx_DeleteDynamicVb(&st_cracksVB);
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

	/* SurvivalGameMode.getReachDistance() returns 4 blocks, vs 5 for Creative */
	p->ReachDistance = 4.0f;

	SurvivalTest_SpawnInitialMobs();
}

/*########################################################################################################################*
*-------------------------------------------------Debug/testing tools---------------------------------------------------*
*#########################################################################################################################*/
/* See SurvivalTest.h - not part of genuine c0.30-s parity, just manual-testing aids. */

/* Common helper: a point `dist` blocks in front of the player along their look */
/*  yaw (level, ignoring pitch), nudged up a touch. Returns false if there's no */
/*  player yet, so every debug spawner can bail cleanly. */
static cc_bool SurvivalTest_DebugFrontPos(float dist, Vec3* pos) {
	struct LocalPlayer* p = Entities.CurPlayer;
	struct Entity* e;
	Vec3 dir;
	if (!p) return false;
	e = &p->Base;

	dir = Vec3_GetDirVector(e->Yaw * MATH_DEG2RAD, 0.0f);
	pos->x = e->Position.x + dir.x * dist;
	pos->y = e->Position.y + 0.5f;
	pos->z = e->Position.z + dir.z * dist;
	return true;
}

void SurvivalTest_DebugSpawnMob(int type) {
	struct Mob* m;
	Vec3 pos;
	if (!SurvivalTest_Enabled) return;
	if (type < 0 || type >= SURVIVAL_DEBUG_MOB_COUNT) return;
	if (!SurvivalTest_DebugFrontPos(3.0f, &pos)) return;

	m = SurvivalTest_SpawnMobAt((cc_uint8)type, pos);
	if (!m) return;

	/* Apply the persistent debug spawn toggles (F9 menu) to this one mob. */
	if (st_debugNoAI) m->noAI = true;
	if (st_debugForceArmor && (m->type == MOB_TYPE_ZOMBIE || m->type == MOB_TYPE_SKELETON)) {
		m->hasHelmet = true;
		m->hasArmor  = true;
	}
}

void SurvivalTest_DebugSpawnDrops(void) {
	/* A spread of distinct item renderings to eyeball drop physics/pickup at once. */
	static const BlockID kinds[] = { BLOCK_STONE, BLOCK_LOG, BLOCK_RED_SHROOM, BLOCK_TNT };
	Vec3 pos;
	int i;
	if (!SurvivalTest_Enabled) return;
	if (!SurvivalTest_DebugFrontPos(2.0f, &pos)) return;

	for (i = 0; i < (int)Array_Elems(kinds); i++) {
		SurvivalTest_SpawnDropAt(pos, kinds[i], 1);
	}
}

void SurvivalTest_DebugSpawnTnt(void) {
	IVec3 coords;
	Vec3 pos;
	if (!SurvivalTest_Enabled) return;
	if (!SurvivalTest_DebugFrontPos(2.0f, &pos)) return;

	/* ArmTnt positions the primed entity at coords + 0.5 and ignites a fuse. */
	coords.x = Math_Floor(pos.x);
	coords.y = Math_Floor(pos.y);
	coords.z = Math_Floor(pos.z);
	SurvivalTest_ArmTnt(coords, TNT_FUSE_TICKS);
}

void SurvivalTest_DebugShootArrow(void) {
	struct Entity* e;
	Vec3 eye;
	if (!SurvivalTest_Enabled) return;
	if (!Entities.CurPlayer) return;
	e = &Entities.CurPlayer->Base;

	/* Same as a Tab-fire (eye-height, player force/damage), but free - doesn't */
	/*  spend an arrow from the count, so you can spam them while testing. */
	eye = Entity_GetEyePosition(e);
	SurvivalTest_SpawnArrow(eye, e->Yaw, e->Pitch,
							ARROW_PLAYER_FIRE_FORCE, ARROW_PLAYER_DAMAGE, 0, true, -1);
}

cc_bool SurvivalTest_DebugGodMode(void)     { return st_godMode; }
cc_bool SurvivalTest_DebugNoAI(void)        { return st_debugNoAI; }
cc_bool SurvivalTest_DebugForceArmor(void)  { return st_debugForceArmor; }
void SurvivalTest_DebugToggleGodMode(void)    { st_godMode          = !st_godMode; }
void SurvivalTest_DebugToggleNoAI(void)       { st_debugNoAI        = !st_debugNoAI; }
void SurvivalTest_DebugToggleForceArmor(void) { st_debugForceArmor  = !st_debugForceArmor; }

/* Prints the live mob population against the world-size-scaled caps, so the */
/*  spawn scaling (SurvivalGameMode.spawnMobs' area formulas) can be watched */
/*  in-game: gate = rand(100) < area each tick, live cap = area*20. */
void SurvivalTest_DebugMobCensus(void) {
	cc_int64 volume = (cc_int64)World.Width * World.Height * World.Length;
	int area  = (int)(volume / 64 / 64 / 64);
	int alive = SurvivalTest_CountMobs();
	int cap   = min(area * 20, MOB_MAX);
	cc_string msg; char msgBuffer[STRING_SIZE];

	String_InitArray(msg, msgBuffer);
	String_Format3(&msg, "&eMobs: &f%i&e alive / cap &f%i&e (spawn roll &f%i%%&e/tick)",
	               &alive, &cap, &area);
	Chat_Add(&msg);
}

void SurvivalTest_DebugKillAllMobs(void) {
	struct Mob* m;
	int i;
	if (!SurvivalTest_Enabled) return;

	for (i = 0; i < MOB_MAX; i++) {
		m = &st_mobs[i];
		if (!m->active || m->health <= 0) continue;
		Mob_Hurt(m, NULL, m->health, false);
	}
}

void SurvivalTest_DebugSetArrows(int count) {
	if (!SurvivalTest_Enabled) return;
	if (count < 0) count = 0;
	if (count > ARROW_PLAYER_MAX) count = ARROW_PLAYER_MAX;
	st_playerArrows = count;
}


struct IGameComponent SurvivalTest_Component = {
	SurvivalTest_Init,          /* Init           */
	SurvivalTest_Free,          /* Free           */
	NULL,                       /* Reset          */
	SurvivalTest_OnNewMap,      /* OnNewMap       */
	SurvivalTest_OnNewMapLoaded /* OnNewMapLoaded */
};
