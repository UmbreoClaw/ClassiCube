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
#include "IndevArmor.h"
#include "IndevFire.h"
#include "SurvivalNet.h"
#include "Server.h"
#include "Input.h"
#include "Gui.h"
#include "Picking.h"
#include "Particle.h"
#include "Commands.h"

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
cc_bool SurvivalTest_Creative;
int     SurvivalTest_Health = SURVIVAL_MAX_HEALTH;

/* Whether the SERVER owns the inventory/cursor state (the phase-4 streams).
    False in SP - and in MP CREATIVE, where the inventory is the genuine
    client-side palette: the server tracks no inventory on creative maps
    (free build, no pickup/consume), so clicks stay local, the palette
    hotbar fills locally, and the INV/CURSOR stream appliers are ignored. */
cc_bool SurvivalTest_ServerOwnsInventory(void) {
	return SurvivalNet_ServerDriven() && !SurvivalTest_CreativeActive();
}

cc_bool SurvivalTest_CreativeActive(void) {
	/* SP: the local toggle. MP: server-dictated (deferred - SurvivalNet will set
	    this from SURV_HELLO, so downstream checks need no change). Indev-only, so
	    faithful c0.30-s and plain creative ClassiCube stay completely untouched. */
	return SurvivalTest_Creative && IndevTest_Enabled;
}

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
/* EntityPlayer.fireResistance: burning ticks needed before catching alight */
#define PLAYER_FIRE_RESIST 20
/* Falls of more than this many blocks deal damage (~1 HP per excess block) */
#define FALL_SAFE_BLOCKS   3.0f
/* Y below which falling through the world's bottom (a floating-map void) is
   fatal. Genuine in-20100223 has no explicit void death at all - normally you
   land on a lower floating-island layer and die from fall damage, or (on a
   single-layer world) fall forever - so this threshold is purely our backstop.
   -32 gives a clear void plunge below the world bottom (y=0) while the death
   camera zoom/roll still play out with terrain in frame, not an empty-sky drop. */
#define ST_VOID_KILL_Y   (-32.0f)
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

/* Entity.fire for the local player (Indev layer): > 0 while alight. */
static int st_playerFire;
static cc_bool st_playerWasInWater = true; /* true = no splash on the first tick */
/* set while an arrow's hit is applied - Indev sheep shear only for LIVING
    attackers, and a genuine arrow passes ITSELF (a plain Entity) as cause */
static cc_bool st_hurtViaArrow;

/* Debug/testing toggle, driven by the /client god command - NOT part of */
/*  genuine c0.30-s parity (see the Debug/testing tools section at the bottom). */
/*  Blocks ALL player damage; defaults false (zero-init) so it's inert. */
static cc_bool st_godMode;

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
/* InventoryPlayer.armorInventory: [0] boots .. [3] helmet (piece = 3-index) */
static struct SurvivalSlot st_armor[SURVIVAL_ARMOR_SLOTS];

/* Remote players' streamed equipment (SURV_PLAYER_EQUIP), per Classic entity id
    (0..ENTITIES_SELF_ID-1): the held item id + the 4 worn armor ids (0 = none,
    boots..helmet). Applied to Entities.List[id] in the third-person render pass;
    the LOCAL player renders its own equipment from st_armor / the held slot. */
struct NetEquip { cc_uint16 held; cc_uint16 armor[4]; cc_bool set; };
static struct NetEquip st_netEquip[ENTITIES_SELF_ID];
/* EntityPlayer.damageRemainder - the sub-1HP carry of armor-scaled damage */
static int st_damageRemainder;
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
/* Plain-server local stash (defined near the creative palette code) */
static void SurvivalTest_PlainWriteThrough(const cc_uint16* prev);
/* /client debug commands (bottom of file) - registered while survival is on. */
static void SurvivalTest_RegisterCommands(void);
static void SurvivalTest_UnregisterCommands(void);
/* Paintings (defined later, used by render/attack/arrow code above them) */
static void SurvivalTest_RenderPaintings(void);
static void SurvivalTest_TickPaintings(void);
static cc_bool SurvivalTest_TryPunchPainting(Vec3 eyePos, Vec3 dir, float maxDist);
static cc_bool SurvivalTest_ArrowHitPainting(Vec3 pos);
/* Defined in the Mining section - forward declared for the melee attack path */
static void SurvivalTest_DamageHeldTool(int amount);
static void SurvivalTest_ConsumeSelected(void);
/* Defined later, in the Ticking section - forward declared so the Mobs */
/*  section below (which ticks before Ticking is reached) can reuse it. */
static cc_bool SurvivalTest_IsHeadInWater(struct Entity* e);
/* Defined later, in the Arrows section - forward declared so the Mobs */
/*  section below (skeletons firing/death-bursting arrows) can spawn them. */
static void SurvivalTest_SpawnArrow(Vec3 pos, float yaw, float pitch, float force,
									 int damage, cc_uint8 type, cc_bool ownerIsPlayer, int ownerMobSlot);
static void SurvivalTest_SpawnArrowIndev(Vec3 pos, Vec3 rawDir, float speed,
										  float spreadFactor, cc_bool ownerIsPlayer, int ownerMobSlot);
/* Defined later, in the Mobs section - the Indev entity-sound funnel (a no-op
    in c0.30 mode) and the standard living-sound pitch jitter, forward declared
    for the player damage / drops / arrows / explosion code that plays them. */
static void  Indev_PlaySoundAt(Vec3 pos, int type, float vol, float pitch);
static float Mob_SndPitch(void);
/* Entity.onEntityUpdate's water-entry splash - forward declared for the
    drop/mob/player water hooks that fire it. vel is per-TICK motion. */
static void  Indev_EntitySplash(Vec3 pos, Vec3 vel, float width);
/* Defined later, in the TNT section - forward declared so the Drops section */
/*  above (which decides what mining a TNT block does) can ignite its fuse. */
/*  fuseTicks lets callers other than mining (the explosion chain-reaction) */
/*  arm a shorter, randomized fuse instead of the full PrimedTnt default. */
static void SurvivalTest_ArmTnt(IVec3 coords, int fuseTicks);
#define TNT_FUSE_TICKS 40   /* c0.30 PrimedTnt's default life */
/* Indev EntityTNTPrimed defaults to fuse = 80 (4 seconds) */
#define TNT_FUSE_DEFAULT() (IndevTest_Enabled ? 80 : TNT_FUSE_TICKS)
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
static cc_bool ST_IsFireBlock(BlockID b)  { return IndevFire_IsFire(b); }
/* World.isBoundingBoxBurning's fire-block half (lava is tested separately) */
static cc_bool ST_InFire(struct Entity* e) {
	struct AABB bb;
	if (!IndevTest_Enabled) return false;
	Entity_GetBounds(e, &bb);
	return Entity_TouchesAny(&bb, ST_IsFireBlock);
}
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
	cc_bool wasInWater;  /* last tick's water state - the air->water edge splashes */
	cc_int8 health;      /* EntityItem.health = 5; fire/lava contact deals 1/tick */

	/* MP (server-owned) drops: the server assigns the id, runs the pickup-delay
	    countdown and decides who collects, so a net drop ticks its LOCAL physics
	    and animation only - never local pickup or lifetime despawn (those arrive
	    as SURV_DROP_PICKUP / SURV_DROP_REMOVE). */
	cc_bool net;          /* streamed in by the server (SurvivalNet_NetDropSpawn) */
	int     netId;        /* server drop id (wire key) - 0 for local SP drops */
	Vec3    pickupTarget; /* body the pickup fly-in eases toward (captured on PICKUP) */
};
static struct DropItem st_drops[DROP_MAX];
/* TakeEntityAnim.tick(): removes itself once time >= 3, at 20 ticks/sec. */
#define DROP_PICKUP_ANIM_SECS (3.0f / 20.0f)

/* Vertex buffer for the textured item cubes */
#define ITEM_VERTICES_PER_DROP 24
/* Indev draws up to 4 jumbled mini-block copies per stack (RenderItem's */
/*  stackSize > 1/5/20 thresholds), so the budget is 4x a single cube. */
#define ITEM_MAX_VERTICES (DROP_MAX * ITEM_VERTICES_PER_DROP * 4)

/* RenderItem.doRender's deterministic stack jumble (Random seed 187): the */
/*  same offsets every frame; copy count from the stack size thresholds. */
static const Vec3 drop_jumble[4] = {
	{  0.00f,  0.00f,  0.00f }, {  0.16f, -0.10f,  0.22f },
	{ -0.20f,  0.12f, -0.14f }, {  0.08f, -0.18f, -0.24f }
};
static int Drop_Copies(int count) {
	return count > 20 ? 4 : (count > 5 ? 3 : (count > 1 ? 2 : 1));
}
/* Face iteration order for the Indev mini-block builder */
static const cc_uint8 drop_faces[6] = {
	FACE_YMIN, FACE_YMAX, FACE_ZMIN, FACE_ZMAX, FACE_XMIN, FACE_XMAX
};
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

/* Indev RenderItem.doRender animates from EntityItem.hoverStart instead:
    spin = (age_ticks/20 + hoverStart) radians (57.3 deg/sec, close to
    c0.30's 60), bob = sin(age_ticks/10 + hoverStart) * 0.1 + 0.1 - a THIRD
    of c0.30's bob frequency. rot0 (uniform 0..360 deg) doubles as
    hoverStart (uniform 0..2pi) via a deg->rad fold. */
static float Drop_SpinDeg(struct DropItem* d, float age) {
	if (IndevTest_Enabled) {
		float hover = d->rot0 * MATH_DEG2RAD;
		return (age + hover) * MATH_RAD2DEG; /* age secs = age_ticks/20 */
	}
	return DropItem_Phase(d, age);
}

static float Drop_Bob(struct DropItem* d, float age) {
	if (IndevTest_Enabled) {
		float hover = d->rot0 * MATH_DEG2RAD;
		return Math_SinF(age * 2.0f + hover) * 0.1f + 0.1f; /* 2 rad/sec */
	}
	return Math_SinF(DropItem_Phase(d, age) / 10.0f) * 0.1f + 0.1f;
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
	float bob = Drop_Bob(d, age);
	*yLo = pos.y + bob;
	*yHi = *yLo + DROP_ITEM_HALF * 2.0f;

	DropItem_RotatedCorners(DROP_ITEM_HALF, pos.x, pos.z,
							 Drop_SpinDeg(d, age) * MATH_DEG2RAD, a, b, c, e);
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

/* Appends one drop copy as an Indev miniature block: the same 0.25-cube
    geometry, but each face textured with the block's OWN tile for that face
    (renderBlockOnInventory style - so logs get bark sides + ring tops, grass
    gets its top, no more side edges on every face). Faces can land in
    different 1D atlases, so each writes at its own atlas' running index. */
static void DropItem_BuildMiniBlock(struct DropItem* d, Vec3 pos, float age, PackedCol col,
									struct VertexTextured* data, cc_uint16* indices) {
	struct VertexTextured* v;
	TextureRec r;
	TextureLoc loc;
	float yLo, yHi;
	Vec3 a, b, c, e;
	int f, idx, texIndex;

	DropItem_ComputeGeometry(d, pos, age, &yLo, &yHi, &a, &b, &c, &e);

	for (f = 0; f < 6; f++) {
		loc = Block_Tex((BlockID)d->block, drop_faces[f]);
		idx = Atlas1D_Index(loc);
		r   = Atlas1D_TexRec(loc, 1, &texIndex);
		v   = data + indices[idx];

		#define MINI_V(p, py, uu, vv) v->x = (p).x; v->y = (py); v->z = (p).z; v->Col = col; v->U = (uu); v->V = (vv); v++;
		switch (drop_faces[f]) {
		case FACE_YMIN:
			MINI_V(a,yLo, r.u1,r.v1) MINI_V(b,yLo, r.u2,r.v1) MINI_V(c,yLo, r.u2,r.v2) MINI_V(e,yLo, r.u1,r.v2) break;
		case FACE_YMAX:
			MINI_V(a,yHi, r.u1,r.v1) MINI_V(e,yHi, r.u2,r.v1) MINI_V(c,yHi, r.u2,r.v2) MINI_V(b,yHi, r.u1,r.v2) break;
		case FACE_ZMIN: /* side A-B */
			MINI_V(a,yLo, r.u1,r.v2) MINI_V(b,yLo, r.u2,r.v2) MINI_V(b,yHi, r.u2,r.v1) MINI_V(a,yHi, r.u1,r.v1) break;
		case FACE_ZMAX: /* side D-C */
			MINI_V(e,yLo, r.u1,r.v2) MINI_V(c,yLo, r.u2,r.v2) MINI_V(c,yHi, r.u2,r.v1) MINI_V(e,yHi, r.u1,r.v1) break;
		case FACE_XMIN: /* side A-D */
			MINI_V(a,yLo, r.u1,r.v2) MINI_V(e,yLo, r.u2,r.v2) MINI_V(e,yHi, r.u2,r.v1) MINI_V(a,yHi, r.u1,r.v1) break;
		default:        /* side B-C */
			MINI_V(b,yLo, r.u1,r.v2) MINI_V(c,yLo, r.u2,r.v2) MINI_V(c,yHi, r.u2,r.v1) MINI_V(b,yHi, r.u1,r.v1) break;
		}
		#undef MINI_V
		indices[idx] += 4;
	}
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
static struct DropItem* SurvivalTest_SpawnDropAtEx(Vec3 pos, cc_uint16 block, int count);
static void SurvivalTest_SpawnDropAt(Vec3 pos, cc_uint16 block, int count) {
	SurvivalTest_SpawnDropAtEx(pos, block, count);
}
static struct DropItem* SurvivalTest_SpawnDropAtEx(Vec3 pos, cc_uint16 block, int count) {
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
	/* Indev Block.dropBlockAsItemWithChance: every block drop spawns with
	    delayBeforeCanPickup = 10 ticks (c0.30 has no such delay) */
	d->pickupDelay = IndevTest_Enabled ? 10.0f / 20.0f : 0.0f;
	d->wasInWater  = true; /* Entity.isFirstUpdate: never splash on the spawn tick */
	d->health      = 5;    /* EntityItem.health */
	d->active      = true;
	return d;
}

/* Public wrapper so the Indev layer (chest scatter on break) can spawn */
/*  drop entities at an exact world position. */
void SurvivalTest_SpawnDropWorld(Vec3 pos, int id, int count) {
	SurvivalTest_SpawnDropAt(pos, (cc_uint16)id, count);
}

/*########################################################################################################################*
*----------------------------------------------MP (server-owned) drop appliers--------------------------------------------*
*#########################################################################################################################*/
/* SurvivalNet feeds these from SURV_DROP_SPAWN/PICKUP/REMOVE. The server owns
    the id, the pickup-delay countdown and the collection decision, so a net drop
    only runs its LOCAL visual physics (the pop arc + spin/bob) - never a local
    pickup or lifetime despawn. */
static struct DropItem* SurvivalTest_FindNetDrop(int netId) {
	int i;
	for (i = 0; i < DROP_MAX; i++) {
		if (st_drops[i].active && st_drops[i].net && st_drops[i].netId == netId)
			return &st_drops[i];
	}
	return NULL;
}

void SurvivalTest_NetDropSpawn(int netId, Vec3 pos, Vec3 vel, int id, int count, int rot0) {
	struct DropItem* d = SurvivalTest_FindNetDrop(netId);
	if (!d) {
		/* new drop - claim a pool slot (evicts the oldest when full) */
		d = &st_drops[SurvivalTest_FindFreeDropSlot()];
	}
	Mem_Set(d, 0, sizeof(struct DropItem));
	d->position   = pos;
	d->prevPos    = pos; /* seed so the first frame doesn't lerp in from (0,0,0) */
	d->velocity   = vel;
	d->block      = (cc_uint16)id;
	d->count      = max(count, 1);
	d->rot0       = (float)rot0 * 360.0f / 256.0f;
	d->wasInWater = true; /* never splash on the spawn tick */
	d->health     = 5;
	d->active     = true;
	d->net        = true;
	d->netId      = netId;
	/* pickupDelay stays 0: the SERVER gates collection, so the client never
	    runs SurvivalTest_DropTryPickup on a net drop at all. */
}

void SurvivalTest_NetDropPickup(int netId, int pickerEntityId) {
	struct DropItem* d = SurvivalTest_FindNetDrop(netId);
	struct Entity* picker;
	if (!d) return;

	/* Ease the drop into whoever collected it. 255 = ENTITIES_SELF_ID (this
	    viewer's own body); 0xFF from the server also lands here when the picker
	    isn't visible to us - fall back to the local player so the item still
	    zips away rather than freezing, then vanishes. */
	picker = Entities.List[pickerEntityId < ENTITIES_MAX_COUNT ? pickerEntityId : ENTITIES_SELF_ID];
	if (!picker) picker = &Entities.CurPlayer->Base;

	d->pickingUp   = true;
	d->pickupTime  = 0.0f;
	d->pickupFrom  = d->position;
	d->pickupTarget = picker->Position;
	Indev_PlaySoundAt(d->position, MOBSND_POP, 0.2f,
		((Random_Float(&st_dropRng) - Random_Float(&st_dropRng)) * 0.7f + 1.0f) * 2.0f);
}

void SurvivalTest_NetDropRemove(int netId) {
	struct DropItem* d = SurvivalTest_FindNetDrop(netId);
	if (d) d->active = false; /* despawn/destroyed: no pickup animation */
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
	/* punching fire just extinguishes it - quantityDropped 0 */
	if (IndevFire_IsFire(oldBlock)) return false;
	/* Directional chest/furnace variants drop their canonical block */
	oldBlock   = IndevTest_CanonicalBlock(oldBlock);
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
		*count     = 1;          /* getDropCount is NOT overridden - one slab only */
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
	cc_uint16 dropId;
	int count = 1, i;

	/* Directional chest/furnace variants drop their canonical block - but a
	    LIT furnace drops the lit block 62 (no idDropped override in Indev) */
	oldBlock = IndevTest_DropFormBlock(oldBlock);
	dropId   = oldBlock; /* most blocks drop themselves */

	if (oldBlock == BLOCK_TNT) { SurvivalTest_ArmTnt(coords, TNT_FUSE_DEFAULT()); return; }

	/* BlockCrops: wheat only at full stage, plus up to 3 bonus seed rolls */
	/*  weighted by the stage. Farmland drops dirt (BlockFarmland.idDropped). */
	if (IndevTest_Enabled && oldBlock >= 85 && oldBlock <= 92) {
		int stage = oldBlock - 85;
		if (stage == 7) SurvivalTest_SpawnDrop(coords, 256 + 40); /* Wheat */
		for (i = 0; i < 3; i++) {
			if (Random_Next(&st_dropRng, 15) <= stage) SurvivalTest_SpawnDrop(coords, 256 + 39); /* Seeds */
		}
		return;
	}
	if (IndevTest_Enabled && (oldBlock == 83 || oldBlock == 84)) {
		SurvivalTest_SpawnDrop(coords, BLOCK_DIRT);
		return;
	}

	/* canHarvestBlock: rock/iron needs a pickaxe (see IndevTest_CanHarvest) */
	if (!IndevTest_CanHarvest(heldId, oldBlock)) return;

	switch (oldBlock) {
	case BLOCK_GRASS:   dropId = BLOCK_DIRT; break;
	case BLOCK_STONE:   dropId = BLOCK_COBBLE; break;      /* BlockStone -> cobblestone */
	case BLOCK_OBSIDIAN: dropId = BLOCK_COBBLE; break;     /* obsidian IS a BlockStone(49) */
	case BLOCK_LOG:     dropId = BLOCK_LOG; break;         /* BlockLog -> the log itself */
	case BLOCK_COAL_ORE: dropId = 256 + 7; break;          /* -> coal ITEM */
	case 56 /* INDEV_BLOCK_DIAMOND_ORE */:
	                    dropId = 256 + 8; break;           /* -> diamond ITEM */
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
		SurvivalTest_ArmTnt(coords, TNT_FUSE_DEFAULT());
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

	/* Indev EntityItem.playerTouch plays the pickup pop (c0.30 was silent) */
	Indev_PlaySoundAt(d->position, MOBSND_POP, 0.2f,
		((Random_Float(&st_dropRng) - Random_Float(&st_dropRng)) * 0.7f + 1.0f) * 2.0f);
	d->pickingUp  = true;
	d->pickupTime = 0.0f;
	d->pickupFrom = d->position;
}

/* World.isBoundingBoxBurning for a drop: any fire or lava block overlapping
    the item's (unshrunk) box. NOTE: genuine handleLavaMovement's 10-damage
    hit never fires for the 0.25-tall item box - the -0.4 Y shrink makes the
    test band degenerate/empty - so burning contact at 1 damage/tick (dead
    in 5 ticks, silent - EntityItem.attackEntityFrom has no sound/particles)
    is the only kill path. Item fire counters are irrelevant at 5 health. */
static cc_bool Drop_BoxBurning(struct DropItem* d) {
	int x0 = Math_Floor(d->position.x - DROP_ITEM_HALF), x1 = Math_Floor(d->position.x + DROP_ITEM_HALF);
	int y0 = Math_Floor(d->position.y),                  y1 = Math_Floor(d->position.y + DROP_ITEM_HALF * 2.0f);
	int z0 = Math_Floor(d->position.z - DROP_ITEM_HALF), z1 = Math_Floor(d->position.z + DROP_ITEM_HALF);
	int x, y, z;
	BlockID b;
	for (x = x0; x <= x1; x++) {
	for (y = y0; y <= y1; y++) {
	for (z = z0; z <= z1; z++) {
		if (!World_Contains(x, y, z)) continue;
		b = World_GetBlock(x, y, z);
		if (IndevFire_IsFire(b) || ST_IsLavaBlock(b)) return true;
	}}}
	return false;
}

/* EntityItem.pushOutOfBlocks: when the item's centre cell is a full opaque
    cube, pick the nearest OPEN face among the six neighbours and overwrite
    that single axis's velocity (0.1..0.3 blocks/tick) toward it. Runs every
    tick, so a buried item oozes out over a few ticks. */
static void Drop_PushOutOfBlocks(struct DropItem* d) {
	float px = d->position.x, py = d->position.y + DROP_ITEM_HALF, pz = d->position.z;
	int bx = Math_Floor(px), by = Math_Floor(py), bz = Math_Floor(pz);
	float fx, fy, fz, best, mag;
	int face;

	if (!World_Contains(bx, by, bz))                     return;
	if (!Blocks.FullOpaque[World_GetBlock(bx, by, bz)]) return;

	fx = px - bx; fy = py - by; fz = pz - bz;
	face = -1; best = 9999.0f;
	#define DROP_OPEN(X, Y, Z) (!World_Contains(X, Y, Z) || !Blocks.FullOpaque[World_GetBlock((X), (Y), (Z))])
	if (DROP_OPEN(bx - 1, by, bz))                        { face = 0; best = fx; }
	if (DROP_OPEN(bx + 1, by, bz) && 1.0f - fx < best)    { face = 1; best = 1.0f - fx; }
	if (DROP_OPEN(bx, by - 1, bz) && fy < best)           { face = 2; best = fy; }
	if (DROP_OPEN(bx, by + 1, bz) && 1.0f - fy < best)    { face = 3; best = 1.0f - fy; }
	if (DROP_OPEN(bx, by, bz - 1) && fz < best)           { face = 4; best = fz; }
	if (DROP_OPEN(bx, by, bz + 1) && 1.0f - fz < best)    { face = 5; }
	#undef DROP_OPEN
	if (face < 0) return;

	mag = (Random_Float(&st_dropRng) * 0.2f + 0.1f) * 20.0f; /* per-second */
	switch (face) {
	case 0: d->velocity.x = -mag; break;
	case 1: d->velocity.x =  mag; break;
	case 2: d->velocity.y = -mag; break;
	case 3: d->velocity.y =  mag; break;
	case 4: d->velocity.z = -mag; break;
	case 5: d->velocity.z =  mag; break;
	}
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
			/* Indev EntityPickupFX eases to eye - 0.5; c0.30 TakeEntityAnim
			    to player.y - 1.0 (feet + 0.62) */
			float targetY = IndevTest_Enabled ? Entity_GetEyePosition(pe).y - 0.5f
			                                  : pe->Position.y + 0.62f;
			d->position.x = d->pickupFrom.x + ( pe->Position.x - d->pickupFrom.x) * distance;
			d->position.y = d->pickupFrom.y + ( targetY        - d->pickupFrom.y) * distance;
			d->position.z = d->pickupFrom.z + ( pe->Position.z - d->pickupFrom.z) * distance;
			if (d->pickupTime >= DROP_PICKUP_ANIM_SECS) d->active = false;
			continue;
		}

		d->age += delta;
		if (d->age >= DROP_LIFETIME_SECS) { d->active = false; continue; }

		/* Indev EntityItem: fire/lava contact + the lava fizz-bounce + the
		    push-out-of-solid-blocks nudge (c0.30 items have none of these).
		    ORDER matters: genuine subtracts gravity FIRST (motionY -= 0.04)
		    and the lava kick then OVERWRITES motionY = 0.2 - so the kick and
		    push-out run after the physics step, not before it. */
		if (IndevTest_Enabled && Drop_BoxBurning(d)) {
			d->health--;
			if (d->health <= 0) { d->active = false; continue; }
		}
		SurvivalTest_DropPhysics(d, delta);
		if (IndevTest_Enabled) {
			{
				int cx = Math_Floor(d->position.x);
				int cy = Math_Floor(d->position.y + DROP_ITEM_HALF);
				int cz = Math_Floor(d->position.z);
				if (World_Contains(cx, cy, cz) && ST_IsLavaBlock(World_GetBlock(cx, cy, cz))) {
					/* motion is per-tick in genuine; drop velocity per-second */
					d->velocity.y = 0.2f * 20.0f;
					d->velocity.x = (Random_Float(&st_dropRng) - Random_Float(&st_dropRng)) * 0.2f * 20.0f;
					d->velocity.z = (Random_Float(&st_dropRng) - Random_Float(&st_dropRng)) * 0.2f * 20.0f;
					Indev_PlaySoundAt(d->position, MOBSND_FIZZ,
						0.4f, 2.0f + Random_Float(&st_dropRng) * 0.4f);
				}
			}
			Drop_PushOutOfBlocks(d);
		}

		/* Entity.onEntityUpdate: items splash when they land in water too */
		if (IndevTest_Enabled) {
			int bx = (int)Math_Floor(d->position.x);
			int by = (int)Math_Floor(d->position.y);
			int bz = (int)Math_Floor(d->position.z);
			cc_bool inW = World_Contains(bx, by, bz) &&
				ST_IsWaterBlock(World_GetBlock(bx, by, bz));
			if (inW && !d->wasInWater) {
				/* drop velocities are per-second; genuine motion is per-tick */
				Vec3 v = d->velocity;
				v.x /= 20.0f; v.y /= 20.0f; v.z /= 20.0f;
				Indev_EntitySplash(d->position, v, 0.25f);
			}
			d->wasInWater = inW;
		}

		if (d->pickupDelay > 0.0f) { d->pickupDelay -= delta; }
		else                       { SurvivalTest_DropTryPickup(d, pe); }
	}
}

/* MP counterpart of SurvivalTest_TickDrops: runs each server-owned drop's LOCAL
    presentation only - the pop arc, spin/bob age, and the Indev lava-fizz / water
    splash / push-out-of-blocks visuals - but NEVER local collection or lifetime
    despawn (SURV_DROP_PICKUP / SURV_DROP_REMOVE drive those). Also NOT the fire
    health-kill: the server doesn't burn drops, so destroying one locally would
    desync it. The pickup fly-in eases toward the captured picker body. */
static void SurvivalTest_TickNetDrops(float delta) {
	struct DropItem* d;
	float distance;
	int i;

	for (i = 0; i < DROP_MAX; i++) {
		d = &st_drops[i];
		if (!d->active || !d->net) continue;

		d->prevPos = d->position;
		d->prevAge = d->age;

		if (d->pickingUp) {
			/* TakeEntityAnim toward the body that collected it (fixed target
			    captured on SURV_DROP_PICKUP - the fly-in is only ~3 ticks). */
			d->pickupTime += delta;
			distance = d->pickupTime / DROP_PICKUP_ANIM_SECS;
			if (distance > 1.0f) distance = 1.0f;
			distance = distance * distance;
			d->position.x = d->pickupFrom.x + (d->pickupTarget.x - d->pickupFrom.x) * distance;
			d->position.y = d->pickupFrom.y + (d->pickupTarget.y - d->pickupFrom.y) * distance;
			d->position.z = d->pickupFrom.z + (d->pickupTarget.z - d->pickupFrom.z) * distance;
			if (d->pickupTime >= DROP_PICKUP_ANIM_SECS) d->active = false;
			continue;
		}

		d->age += delta; /* drives spin/bob/glow - server owns the 5-min despawn */

		SurvivalTest_DropPhysics(d, delta);
		if (IndevTest_Enabled) {
			int cx = Math_Floor(d->position.x);
			int cy = Math_Floor(d->position.y + DROP_ITEM_HALF);
			int cz = Math_Floor(d->position.z);
			if (World_Contains(cx, cy, cz) && ST_IsLavaBlock(World_GetBlock(cx, cy, cz))) {
				d->velocity.y = 0.2f * 20.0f;
				d->velocity.x = (Random_Float(&st_dropRng) - Random_Float(&st_dropRng)) * 0.2f * 20.0f;
				d->velocity.z = (Random_Float(&st_dropRng) - Random_Float(&st_dropRng)) * 0.2f * 20.0f;
				Indev_PlaySoundAt(d->position, MOBSND_FIZZ,
					0.4f, 2.0f + Random_Float(&st_dropRng) * 0.4f);
			}
			Drop_PushOutOfBlocks(d);

			{
				int bx = (int)Math_Floor(d->position.x);
				int by = (int)Math_Floor(d->position.y);
				int bz = (int)Math_Floor(d->position.z);
				cc_bool inW = World_Contains(bx, by, bz) &&
					ST_IsWaterBlock(World_GetBlock(bx, by, bz));
				if (inW && !d->wasInWater) {
					Vec3 v = d->velocity;
					v.x /= 20.0f; v.y /= 20.0f; v.z /= 20.0f;
					Indev_EntitySplash(d->position, v, 0.25f);
				}
				d->wasInWater = inW;
			}
		}
	}
}

/* Updates how many vertices belong to each 1D atlas, for batching draws */
/*  (each drop's tile can land in a different 1D atlas / GL texture). Indev */
/*  mini-blocks count per FACE (each face has its own tile) and per stack */
/*  copy; classic counts one whole cube in the FACE_XMIN tile's atlas. */
static void SurvivalTest_UpdateItem1DCounts(void) {
	int i, f, index, copies;
	for (i = 0; i < Atlas1D.Count; i++) { item_1DCount[i] = 0; item_1DIndices[i] = 0; }

	for (i = 0; i < DROP_MAX; i++) {
		if (!st_drops[i].active) continue;
		if (!ST_ID_IS_BLOCK(st_drops[i].block)) continue;          /* sprite pass instead */
		if (IndevTest_DropIsSprite(st_drops[i].block)) continue;   /* flowers/torches too */

		if (IndevTest_Enabled) {
			copies = Drop_Copies(st_drops[i].count);
			for (f = 0; f < 6; f++) {
				index = Atlas1D_Index(Block_Tex((BlockID)st_drops[i].block, drop_faces[f]));
				item_1DCount[index] += 4 * copies;
			}
		} else {
			index = Atlas1D_Index(Block_Tex((BlockID)st_drops[i].block, FACE_XMIN));
			item_1DCount[index] += ITEM_VERTICES_PER_DROP;
		}
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
		if (IndevTest_DropIsSprite(d->block)) continue; /* flowers/torches: sprite pass */

		/* Blend prevPos->position and prevAge->age by the partial-tick t - see */
		/*  DropItem.prevPos/prevAge - so both motion and spin/bob are smooth. */
		Vec3_Lerp(&renderPos, &d->prevPos, &d->position, t);
		renderAge = d->prevAge + (d->age - d->prevAge) * t;
		col = DropItem_WorldColor(&renderPos);

		if (IndevTest_Enabled) {
			/* Indev RenderItem: miniature FULL blocks with per-face tiles, */
			/*  drawn as 1-4 jumbled copies by stack size. */
			int copies = Drop_Copies(d->count), c;
			Vec3 cpos;
			for (c = 0; c < copies; c++) {
				cpos = renderPos;
				Vec3_AddBy(&cpos, &drop_jumble[c]);
				DropItem_BuildMiniBlock(d, cpos, renderAge, col, data, item_1DIndices);
			}
			continue;
		}

		loc   = Block_Tex((BlockID)d->block, FACE_XMIN);
		index = Atlas1D_Index(loc);
		ptr   = data + item_1DIndices[index];

		base = Atlas1D_TexRec(loc, 1, &texIndex);
		/* Crop to the middle 50% (texels 4..12 of 16) of the tile, on */
		/*  every face - classic ItemModel's look. */
		du = (base.u2 - base.u1) * 0.25f;
		dv = (base.v2 - base.v1) * 0.25f;
		rec.u1 = base.u1 + du; rec.u2 = base.u2 - du;
		rec.v1 = base.v1 + dv; rec.v2 = base.v2 - dv;

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
		/* Indev drops don't glow at all - the white glint is Survival Test's */
		/*  Item.render second pass, dropped by RenderItem. */
		if (IndevTest_Enabled) break;
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
	/* DELIBERATE DEVIATION: neither ground truth drops ANY inventory on
	    death (no dropAllItems exists in either decompile) - kept, armor
	    included, because planned multiplayer support wants corpse drops */
	for (i = 0; i < SURVIVAL_ARMOR_SLOTS; i++) {
		if (st_armor[i].id == BLOCK_AIR || st_armor[i].count <= 0) continue;
		SurvivalTest_SpawnDropAt(pos, st_armor[i].id, st_armor[i].count);
		st_armor[i].id = BLOCK_AIR; st_armor[i].count = 0; st_armor[i].damage = 0;
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
/* InventoryPlayer.getPlayerArmorValue: the summed damageReduceAmount of worn
    pieces, weighted by their remaining durability: (sum-1)*remaining/max + 1 */
static int SurvivalTest_ArmorValue(void) {
	int reduce = 0, remain = 0, max = 0, i, m;
	for (i = 0; i < SURVIVAL_ARMOR_SLOTS; i++) {
		if (st_armor[i].count <= 0) continue;
		m = IndevTest_ArmorMaxDamage(st_armor[i].id);
		if (!m) continue;
		remain += m - st_armor[i].damage;
		max    += m;
		reduce += IndevTest_ArmorReduce(st_armor[i].id);
	}
	return max == 0 ? 0 : (reduce - 1) * remain / max + 1;
}

/* Exposed for the HUD's armor icon row (GuiIngame draws it from the same
    wear-weighted value the damage absorption uses). */
int SurvivalTest_PlayerArmorValue(void) {
	if (!SurvivalTest_Enabled) return 0;
	return SurvivalTest_ArmorValue();
}

/* Returns whether the hit landed (EntityPlayer.attackEntityFrom's boolean) - */
/*  false on god mode / death / the Indev invuln-window miss / armor rounding */
/*  the damage to zero. Indev arrows bounce off the player on false. */
static cc_bool SurvivalTest_Damage(int damage, const Vec3* attackerPos) {
	struct LocalPlayer* p = Entities.CurPlayer;
	if (!SurvivalTest_Enabled || !p) return false;
	if (st_isDead)             return false;
	if (st_godMode)            return false; /* debug invincibility - blocks every damage source */
	if (SurvivalTest_CreativeActive()) return false; /* creative: survivalWorld=false, no damage */
	/* MP: the SERVER owns health - every local damage source (fall, lava, fire,
	    drowning, mobs) is a no-op; authoritative damage arrives as SURV_HEALTH
	    and plays its presentation in SurvivalTest_ApplyNetHealth. One central
	    gate here covers every SurvivalTest_Hurt caller. */
	if (SurvivalNet_ServerDriven()) return false;
	if (damage <= 0)           return false;

	if (IndevTest_Enabled) {
		/* EntityPlayer.attackEntityFrom: unlike c0.30 there is NO delta
		    damage during the invulnerability window - hits simply miss - and
		    armor absorbs in 25ths with the remainder carried between hits.
		    Every worn piece is worn down by the raw damage, even when the
		    final result rounds to zero. */
		int scaled, k;
		if (st_invincTimer > INVULN_DURATION_SECS * 0.5f) return false;

		scaled = damage * (25 - SurvivalTest_ArmorValue()) + st_damageRemainder;
		for (k = 0; k < SURVIVAL_ARMOR_SLOTS; k++) {
			if (st_armor[k].count <= 0) continue;
			if (IndevTest_ArmorPiece(st_armor[k].id) < 0) continue;
			st_armor[k].damage += (cc_int16)damage;
			if (st_armor[k].damage > IndevTest_ArmorMaxDamage(st_armor[k].id)) {
				st_armor[k].id = BLOCK_AIR; st_armor[k].count = 0; st_armor[k].damage = 0;
			}
		}
		st_invVersion++;

		damage             = scaled / 25;
		st_damageRemainder = scaled % 25;
		if (damage == 0) return false;
	}

	if (st_invincTimer > INVULN_DURATION_SECS * 0.5f) {
		if (st_lastHealth - damage >= SurvivalTest_Health) return false;
		SurvivalTest_Health = st_lastHealth - damage;
	} else {
		st_lastHealth  = SurvivalTest_Health;
		st_invincTimer = INVULN_DURATION_SECS;
		SurvivalTest_Health -= damage;
		st_hurtTicks = HURT_TILT_TICKS;
	}

	st_hurtDir = SurvivalTest_CalcHurtDir(attackerPos);
	if (attackerPos) SurvivalTest_Knockback(&p->Base, attackerPos);

	/* EntityLiving.attackEntityFrom: the player is a living entity too, so a
	    landed hit (and the killing blow) plays random.hurt in Indev mode */
	Indev_PlaySoundAt(p->Base.Position, MOBSND_HURT, 1.0f, Mob_SndPitch());

	if (SurvivalTest_Health <= 0) {
		SurvivalTest_Health = 0;
		st_isDead = true;
		SurvivalTest_DropInventory();
		GameOverScreen_Show();
	}
	return true;
}

/* Server-authoritative SURV_HEALTH applier (MP). The server owns the health
    value and the death/respawn cycle; the client owns the PRESENTATION - the
    hurt tilt/sound on a decrease, the death camera + Game Over screen on 0,
    and the revive when health rises while dead (the server repositions us via
    the normal teleport packet around the same time). No inventory drop here:
    drops are server state (phase 5). */
void SurvivalTest_ApplyNetHealth(int health, int score) {
	struct LocalPlayer* p = Entities.CurPlayer;
	if (!SurvivalTest_Enabled || !p) return;
	Math_Clamp(health, 0, SURVIVAL_MAX_HEALTH);
	st_score = score;

	if (health <= 0) {
		if (st_isDead) return;
		Indev_PlaySoundAt(p->Base.Position, MOBSND_HURT, 1.0f, Mob_SndPitch());
		SurvivalTest_Health = 0;
		st_lastHealth  = 0;
		st_hurtTicks   = HURT_TILT_TICKS; /* the killing blow's impact wobble */
		st_hurtDir     = 0.0f;
		st_isDead      = true;
		st_deathTicks  = 0;
		GameOverScreen_Show();
		return;
	}

	if (st_isDead) {
		/* Server respawned us - drop the death presentation */
		st_isDead      = false;
		st_deathTicks  = 0;
		st_hurtTicks   = 0;
		st_invincTimer = 0.0f;
		st_falling     = false;
		Camera_UpdateProjection(); /* undo the death FOV zoom */
		GameOverScreen_Hide();
	} else if (health < SurvivalTest_Health) {
		/* Server-applied damage: play the landed-hit presentation */
		st_hurtTicks = HURT_TILT_TICKS;
		st_hurtDir   = 0.0f;
		Indev_PlaySoundAt(p->Base.Position, MOBSND_HURT, 1.0f, Mob_SndPitch());
	}
	SurvivalTest_Health = health;
	st_lastHealth       = health;
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
static void SurvivalTest_Explode(struct Entity* exploder, Vec3 center, float radius);


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
	cc_bool net;    /* streamed in by the server (SurvivalTest_NetTntSpawn) */
	int     netId;  /* server tnt id (wire key) - 0 for local SP entities */
};
static struct TntFuse st_tnt[TNT_MAX];

/* SmokeParticle.java: each lit TNT puffs out one smoke particle per tick that */
/*  drifts up and fades, which is the obvious "this is about to blow" visual */
/*  cue (the white flash overlay below is far subtler in daylight). Genuine */
/*  c0.30 turns the block into a PrimedTnt entity that physically hops and */
/*  smokes; we keep the block static (see above) but reproduce the smoke as a */
/*  small self-contained particle pool, rendered as billboards off the shared */
/*  particles.png atlas - the same texture and frames the real client used. */
#define TNT_SMOKE_MAX 192
/* what a pool slot is imitating - the constants differ per class */
#define PUFF_C030_SMOKE 0 /* c0.30 SmokeParticle (TNT fuse puffs) */
#define PUFF_INDEV_SMOKE 1 /* Indev EntitySmokeFX ("smoke"/"largesmoke") */
#define PUFF_INDEV_FLAME 2 /* Indev EntityFlameFX ("flame") */
#define PUFF_INDEV_BUBBLE 3 /* Indev EntityBubbleFX ("bubble") */
#define PUFF_INDEV_SPLASH 4 /* Indev EntitySplashFX ("splash", RainFX + 0.04 gravity) */
#define PUFF_INDEV_LAVA   5 /* Indev EntityLavaFX ("lava") */
struct TntSmoke {
	Vec3  pos, prevPos;
	Vec3  vel;        /* per-tick displacement, exactly as in Particle.java */
	int   age, life;  /* in ticks; tex frame = 7 - (age*8)/life */
	float gray;       /* SmokeParticle's random 0..0.3 grey tint */
	float scale;      /* EntityFX.particleScale (quad half-size = 0.1 * this) */
	cc_uint8 kind;
	cc_bool active;
};
static struct TntSmoke st_tntSmoke[TNT_SMOKE_MAX];

static struct TntSmoke* TntSmoke_FreeSlot(void) {
	int i;
	for (i = 0; i < TNT_SMOKE_MAX; i++) {
		if (!st_tntSmoke[i].active) return &st_tntSmoke[i];
	}
	return NULL; /* pool full - skip this puff, oldest keep playing out */
}

/* Particle.java / EntityFX's shared constructor velocity: a random direction
    scaled by (r+r+1)*0.15 with a +0.1 upward bias, from a (0,0,0) base. */
static void TntSmoke_BaseVel(Vec3* vel) {
	float xd, yd, zd, mag, scale;
	xd = (Random_Float(&st_dropRng) * 2.0f - 1.0f) * 0.4f;
	yd = (Random_Float(&st_dropRng) * 2.0f - 1.0f) * 0.4f;
	zd = (Random_Float(&st_dropRng) * 2.0f - 1.0f) * 0.4f;
	scale = (Random_Float(&st_dropRng) + Random_Float(&st_dropRng) + 1.0f) * 0.15f;
	mag   = Math_SqrtF(xd * xd + yd * yd + zd * zd);
	if (mag < 0.0001f) mag = 0.0001f;
	vel->x = xd / mag * scale * 0.4f;
	vel->y = yd / mag * scale * 0.4f + 0.1f;
	vel->z = zd / mag * scale * 0.4f;
}

/* Particle.java's constructor, with the (0,0,0) base velocity SmokeParticle */
/*  passes in, then SmokeParticle's own *0.1 damping and grey/lifetime setup. */
static void SurvivalTest_SpawnTntSmoke(float x, float y, float z) {
	struct TntSmoke* s = TntSmoke_FreeSlot();
	if (!s) return;

	TntSmoke_BaseVel(&s->vel);
	/* Particle base velocity, then SmokeParticle multiplies x/y/z by 0.1. */
	s->vel.x *= 0.1f; s->vel.y *= 0.1f; s->vel.z *= 0.1f;

	s->pos.x = x; s->pos.y = y; s->pos.z = z;
	s->prevPos = s->pos;
	s->gray = Random_Float(&st_dropRng) * 0.3f;
	s->life = (int)(8.0f / (Random_Float(&st_dropRng) * 0.8f + 0.2f));
	if (s->life < 1) s->life = 1;
	s->age  = 0;
	s->scale = 1.0f;
	s->kind  = PUFF_C030_SMOKE;
	s->active = true;
}

/* EntitySmokeFX: Indev's "smoke" (torches, furnaces; scaleMul 1) and
    "largesmoke" (fire; scaleMul 2.5). Compared to the c0.30 puff it keeps
    a particleScale ((r*0.5+0.5)*2 * 12/16 * mul), stretches its lifetime
    by the multiplier, collides with blocks and grows in when spawned. */
void SurvivalTest_SpawnSmokeFX(float x, float y, float z, float scaleMul) {
	struct TntSmoke* s = TntSmoke_FreeSlot();
	if (!s) return;

	TntSmoke_BaseVel(&s->vel);
	s->vel.x *= 0.1f; s->vel.y *= 0.1f; s->vel.z *= 0.1f;

	s->pos.x = x; s->pos.y = y; s->pos.z = z;
	s->prevPos = s->pos;
	s->gray  = Random_Float(&st_dropRng) * 0.3f;
	s->scale = (Random_Float(&st_dropRng) * 0.5f + 0.5f) * 2.0f
	           * (12.0f / 16.0f) * scaleMul;
	s->life  = (int)(8.0f / (Random_Float(&st_dropRng) * 0.8f + 0.2f));
	s->life  = (int)((float)s->life * scaleMul);
	if (s->life < 1) s->life = 1;
	s->age   = 0;
	s->kind  = PUFF_INDEV_SMOKE;
	s->active = true;
}

/* EntityFlameFX: the tiny flame flecks torches and lit furnaces emit.
    Nearly stationary (velocity * 0.01), white, fades from fullbright to
    world lighting and shrinks over its life. noClip - never collides. */
void SurvivalTest_SpawnFlameFX(float x, float y, float z) {
	struct TntSmoke* s = TntSmoke_FreeSlot();
	if (!s) return;

	TntSmoke_BaseVel(&s->vel);
	s->vel.x *= 0.01f; s->vel.y *= 0.01f; s->vel.z *= 0.01f;

	s->pos.x = x; s->pos.y = y; s->pos.z = z;
	s->prevPos = s->pos;
	s->gray  = 1.0f; /* particleRed/Green/Blue = 1 */
	s->scale = (Random_Float(&st_dropRng) * 0.5f + 0.5f) * 2.0f;
	s->life  = (int)(8.0f / (Random_Float(&st_dropRng) * 0.8f + 0.2f)) + 4;
	s->age   = 0;
	s->kind  = PUFF_INDEV_FLAME;
	s->active = true;
}

/* EntityBubbleFX: rises through water at the spawning entity's motion,
    dies the moment it leaves water. mx/my/mz = entity motion (per tick). */
static void Indev_SpawnBubbleFX(float x, float y, float z,
								float mx, float my, float mz) {
	struct TntSmoke* s = TntSmoke_FreeSlot();
	if (!s) return;

	s->vel.x = mx * 0.2f + (Random_Float(&st_dropRng) * 2.0f - 1.0f) * 0.02f;
	s->vel.y = my * 0.2f + (Random_Float(&st_dropRng) * 2.0f - 1.0f) * 0.02f;
	s->vel.z = mz * 0.2f + (Random_Float(&st_dropRng) * 2.0f - 1.0f) * 0.02f;

	s->pos.x = x; s->pos.y = y; s->pos.z = z;
	s->prevPos = s->pos;
	s->gray  = 1.0f; /* white */
	s->scale = (Random_Float(&st_dropRng) * 0.5f + 0.5f) * 2.0f
	           * (Random_Float(&st_dropRng) * 0.6f + 0.2f);
	s->life  = (int)(8.0f / (Random_Float(&st_dropRng) * 0.8f + 0.2f));
	if (s->life < 1) s->life = 1;
	s->age   = 0;
	s->kind  = PUFF_INDEV_BUBBLE;
	s->active = true;
}

/* EntitySplashFX (EntityRainFX with 0.04 gravity): the water droplets a
    splash-down and waterfall edges throw up. */
void SurvivalTest_SpawnSplashFX(float x, float y, float z) {
	struct TntSmoke* s = TntSmoke_FreeSlot();
	if (!s) return;

	TntSmoke_BaseVel(&s->vel);
	s->vel.x *= 0.3f;
	s->vel.y  = Random_Float(&st_dropRng) * 0.2f + 0.1f;
	s->vel.z *= 0.3f;

	s->pos.x = x; s->pos.y = y; s->pos.z = z;
	s->prevPos = s->pos;
	s->gray  = 1.0f;
	s->scale = (Random_Float(&st_dropRng) * 0.5f + 0.5f) * 2.0f;
	s->life  = (int)(8.0f / (Random_Float(&st_dropRng) * 0.8f + 0.2f));
	if (s->life < 1) s->life = 1;
	s->age   = 0;
	s->kind  = PUFF_INDEV_SPLASH;
	s->active = true;
}

/* EntityLavaFX: the fullbright embers lava spits, trailing smoke. */
void SurvivalTest_SpawnLavaFX(float x, float y, float z) {
	struct TntSmoke* s = TntSmoke_FreeSlot();
	if (!s) return;

	TntSmoke_BaseVel(&s->vel);
	s->vel.x *= 0.8f;
	s->vel.y  = Random_Float(&st_dropRng) * 0.4f + 0.05f;
	s->vel.z *= 0.8f;

	s->pos.x = x; s->pos.y = y; s->pos.z = z;
	s->prevPos = s->pos;
	s->gray  = 1.0f;
	s->scale = (Random_Float(&st_dropRng) * 0.5f + 0.5f) * 2.0f
	           * (Random_Float(&st_dropRng) * 2.0f + 0.2f);
	s->life  = (int)(16.0f / (Random_Float(&st_dropRng) * 0.8f + 0.2f));
	if (s->life < 1) s->life = 1;
	s->age   = 0;
	s->kind  = PUFF_INDEV_LAVA;
	s->active = true;
}

/* Point solidity test for the Indev smoke's moveEntity approximation */
static cc_bool TntSmoke_Solid(float x, float y, float z) {
	int bx = (int)Math_Floor(x), by = (int)Math_Floor(y), bz = (int)Math_Floor(z);
	if (!World_Contains(bx, by, bz)) return false;
	return Blocks.Collide[World_GetBlock(bx, by, bz)] == COLLIDE_SOLID;
}

static BlockID TntSmoke_BlockAt(float x, float y, float z) {
	int bx = (int)Math_Floor(x), by = (int)Math_Floor(y), bz = (int)Math_Floor(z);
	if (!World_Contains(bx, by, bz)) return BLOCK_AIR;
	return World_GetBlock(bx, by, bz);
}

/* SmokeParticle.tick()/Particle.tick() (c0.30, noPhysics=true) and Indev's
    EntitySmokeFX/EntityFlameFX.onEntityUpdate: integrate position, damp.
    Particle.tick tests age++ >= lifetime (old value) and still runs the move
    on its removal tick, so a puff gets lifetime+1 movement ticks in total. */
static void SurvivalTest_TickTntSmoke(void) {
	struct TntSmoke* s;
	cc_bool onGround;
	float ny;
	int i;
	for (i = 0; i < TNT_SMOKE_MAX; i++) {
		s = &st_tntSmoke[i];
		if (!s->active) continue;

		s->prevPos = s->pos;
		if (s->age++ >= s->life) s->active = false;

		if (s->kind == PUFF_INDEV_FLAME) {
			/* EntityFlameFX: no rise, noClip move, damp (never on ground) */
			s->pos.x += s->vel.x;
			s->pos.y += s->vel.y;
			s->pos.z += s->vel.z;
			s->vel.x *= 0.96f; s->vel.y *= 0.96f; s->vel.z *= 0.96f;
			continue;
		}

		if (s->kind == PUFF_INDEV_BUBBLE) {
			/* EntityBubbleFX: floats up, dies the moment its cell isn't water */
			BlockID in;
			s->vel.y += 0.002f;
			s->pos.x += s->vel.x;
			s->pos.y += s->vel.y;
			s->pos.z += s->vel.z;
			s->vel.x *= 0.85f; s->vel.y *= 0.85f; s->vel.z *= 0.85f;

			in = TntSmoke_BlockAt(s->pos.x, s->pos.y, s->pos.z);
			if (in != BLOCK_WATER && in != BLOCK_STILL_WATER) s->active = false;
			continue;
		}

		if (s->kind == PUFF_INDEV_SPLASH) {
			/* EntityRainFX tick with EntitySplashFX's 0.04 gravity: falls,
			    half-dies on landing, dies inside liquid/solid */
			BlockID in;
			s->vel.y -= 0.04f;
			if (!TntSmoke_Solid(s->pos.x + s->vel.x, s->pos.y, s->pos.z))
				s->pos.x += s->vel.x;
			if (TntSmoke_Solid(s->pos.x, s->pos.y + s->vel.y, s->pos.z)) {
				if (s->vel.y < 0.0f) {
					if (Random_Float(&st_dropRng) < 0.5f) s->active = false;
					s->vel.x *= 0.7f; s->vel.z *= 0.7f;
				}
				s->vel.y = 0.0f;
			} else {
				s->pos.y += s->vel.y;
			}
			if (!TntSmoke_Solid(s->pos.x, s->pos.y, s->pos.z + s->vel.z))
				s->pos.z += s->vel.z;
			s->vel.x *= 0.98f; s->vel.y *= 0.98f; s->vel.z *= 0.98f;

			in = TntSmoke_BlockAt(s->pos.x, s->pos.y, s->pos.z);
			if (Blocks.Collide[in] == COLLIDE_LIQUID ||
				Blocks.Collide[in] == COLLIDE_SOLID) s->active = false;
			continue;
		}

		if (s->kind == PUFF_INDEV_LAVA) {
			/* EntityLavaFX: falls under 0.03 gravity, trails smoke while young */
			cc_bool ground;
			if (Random_Float(&st_dropRng) > (float)s->age / (float)s->life) {
				SurvivalTest_SpawnSmokeFX(s->pos.x, s->pos.y, s->pos.z, 1.0f);
			}
			s->vel.y -= 0.03f;
			ground = false;
			if (!TntSmoke_Solid(s->pos.x + s->vel.x, s->pos.y, s->pos.z))
				s->pos.x += s->vel.x;
			if (TntSmoke_Solid(s->pos.x, s->pos.y + s->vel.y, s->pos.z)) {
				if (s->vel.y < 0.0f) ground = true;
				s->vel.y = 0.0f;
			} else {
				s->pos.y += s->vel.y;
			}
			if (!TntSmoke_Solid(s->pos.x, s->pos.y, s->pos.z + s->vel.z))
				s->pos.z += s->vel.z;
			s->vel.x *= 0.999f; s->vel.y *= 0.999f; s->vel.z *= 0.999f;
			if (ground) { s->vel.x *= 0.7f; s->vel.z *= 0.7f; }
			continue;
		}

		s->vel.y += 0.004f;

		if (s->kind == PUFF_INDEV_SMOKE) {
			/* EntitySmokeFX has noClip=false: blocked vertical movement
			    (a ceiling above a fire, the floor) makes the puff crawl
			    sideways (*1.1) and ground contact adds friction (*0.7) */
			onGround = false;
			if (!TntSmoke_Solid(s->pos.x + s->vel.x, s->pos.y, s->pos.z))
				s->pos.x += s->vel.x;
			ny = s->pos.y + s->vel.y;
			if (TntSmoke_Solid(s->pos.x, ny, s->pos.z)) {
				if (s->vel.y < 0.0f) onGround = true;
				s->vel.y = 0.0f;
			} else {
				s->pos.y = ny;
			}
			if (!TntSmoke_Solid(s->pos.x, s->pos.y, s->pos.z + s->vel.z))
				s->pos.z += s->vel.z;

			if (s->pos.y == s->prevPos.y) {
				s->vel.x *= 1.1f;
				s->vel.z *= 1.1f;
			}
			s->vel.x *= 0.96f; s->vel.y *= 0.96f; s->vel.z *= 0.96f;
			if (onGround) { s->vel.x *= 0.7f; s->vel.z *= 0.7f; }
			continue;
		}

		/* c0.30 SmokeParticle: noPhysics, no collision at all */
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
	/* BlockTNT.onBlockDestroyedByPlayer plays random.fuse 1.0/1.0 when a
	    full-fuse Indev TNT is primed (mined or consumed by fire); the
	    partial-fuse chain-reaction arms stay silent like genuine */
	if (IndevTest_Enabled && fuseTicks >= 80)
		SurvivalTest_PlaySoundAtBlock(coords.x, coords.y, coords.z, MOBSND_FUSE, 1.0f, 1.0f);
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
		if (tnt->net) continue; /* MP: the server owns the fuse - no local defuse */

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

/* MP counterpart of TryDefuseTnt: the nearest server-owned primed TNT the melee */
/*  ray hits within reach, returned as its netId (or -1). The defuse itself is a */
/*  SURV_ATTACK intent - the client never removes a net TNT or spawns its drop. */
static int SurvivalTest_FindNetTntHit(Vec3 eyePos, Vec3 dir, float reach) {
	struct TntFuse* tnt;
	Vec3 invDir, min, max;
	float t0, t1, bestT = 1.0e30f;
	int i, best = -1;

	invDir.x = Math_SafeDiv(1.0f, dir.x);
	invDir.y = Math_SafeDiv(1.0f, dir.y);
	invDir.z = Math_SafeDiv(1.0f, dir.z);

	for (i = 0; i < TNT_MAX; i++) {
		tnt = &st_tnt[i];
		if (!tnt->active || !tnt->net) continue;

		min.x = tnt->pos.x - TNT_HALF; max.x = tnt->pos.x + TNT_HALF;
		min.y = tnt->pos.y - TNT_HALF; max.y = tnt->pos.y + TNT_HALF;
		min.z = tnt->pos.z - TNT_HALF; max.z = tnt->pos.z + TNT_HALF;
		if (!Intersection_RayIntersectsBox(eyePos, invDir, min, max, &t0, &t1)) continue;
		if (t0 > reach) continue;
		if (t0 < bestT) { bestT = t0; best = i; }
	}
	return best < 0 ? -1 : st_tnt[best].netId;
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
			/* smoke spawn height: Indev EntityTNTPrimed uses +0.5, c0.30 +0.6 */
			SurvivalTest_SpawnTntSmoke(tnt->pos.x,
				tnt->pos.y + (IndevTest_Enabled ? 0.5f : 0.6f), tnt->pos.z);
			continue;
		}

		tnt->active = false;
		SurvivalTest_Explode(NULL, tnt->pos, (float)EXPLOSION_RADIUS);

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

/* ==================== MP primed TNT (server-driven) ==================== */
/* Like drops/arrows, the server owns each primed TNT's fuse and detonation; the
   client seeds an st_tnt entry and simulates the SAME PrimedTnt physics for the
   hop/smoke/flash, then removes it on SURV_TNT_REMOVE - never exploding locally. */

static struct TntFuse* SurvivalTest_FindNetTnt(int netId) {
	int i;
	for (i = 0; i < TNT_MAX; i++) {
		if (st_tnt[i].active && st_tnt[i].net && st_tnt[i].netId == netId)
			return &st_tnt[i];
	}
	return NULL;
}

void SurvivalTest_NetTntSpawn(int netId, Vec3 pos, Vec3 vel, int fuse) {
	struct TntFuse* t = SurvivalTest_FindNetTnt(netId);
	int i, slot = -1;
	if (!t) {
		for (i = 0; i < TNT_MAX; i++) { if (!st_tnt[i].active) { slot = i; break; } }
		if (slot < 0) return; /* pool full - the server caps the chain at 128 anyway */
		t = &st_tnt[slot];
	}
	Mem_Set(t, 0, sizeof(struct TntFuse));
	t->pos       = pos;
	t->prevPos   = pos; /* seed so the first frame doesn't lerp in from (0,0,0) */
	t->vel       = vel;
	t->ticksLeft = fuse;
	t->active    = true;
	t->net       = true;
	t->netId     = netId;
	/* BlockTNT.onBlockDestroyedByPlayer plays random.fuse when a full-fuse TNT is
	    primed (mined / fire); the partial-fuse chain arms stay silent, like genuine. */
	if (fuse >= 80)
		SurvivalTest_PlaySoundAtBlock((int)pos.x, (int)pos.y, (int)pos.z, MOBSND_FUSE, 1.0f, 1.0f);
}

void SurvivalTest_NetTntRemove(int netId, int detonated) {
	struct TntFuse* t = SurvivalTest_FindNetTnt(netId);
	IVec3 coords;
	if (!t) return;
	t->active = false;
	if (detonated) {
		/* PrimedTnt.tick's post-blast 100-particle burst - the blast's actual
		    block destruction streams in separately as authoritative SetBlocks. */
		coords.x = Math_Floor(t->pos.x);
		coords.y = Math_Floor(t->pos.y);
		coords.z = Math_Floor(t->pos.z);
		Particles_BreakBlockEffect(coords, BLOCK_TNT, BLOCK_AIR);
	}
}

/* MP counterpart of SurvivalTest_TickTnt: runs each server-owned primed TNT's
    LOCAL physics + smoke + flash, but NEVER detonates (the server owns the fuse
    and sends SURV_TNT_REMOVE). ticksLeft still counts down - clamped at 0 - to
    drive the flashing overlay speeding up as the fuse nears its end. */
static void SurvivalTest_TickNetTnt(void) {
	struct TntFuse* t;
	int i;

	SurvivalTest_TickTntSmoke();

	for (i = 0; i < TNT_MAX; i++) {
		t = &st_tnt[i];
		if (!t->active || !t->net) continue;

		t->prevPos = t->pos;
		SurvivalTest_TntPhysics(t);

		if (t->ticksLeft > 0) {
			t->ticksLeft--;
			SurvivalTest_SpawnTntSmoke(t->pos.x,
				t->pos.y + (IndevTest_Enabled ? 0.5f : 0.6f), t->pos.z);
		}
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

/* Indev RenderTNTPrimed: the cube swells over the LAST 10 fuse ticks -
    s = (1 - (fuse - partial + 1)/10) clamped 0..1, then ^4, scale
    1.0 -> 1.3. Render-only: pick/defuse boxes stay a full block. c0.30
    has no swell (returns 1). */
static float TntFuse_Swell(int ticksLeft, float t) {
	float f, s;
	if (!IndevTest_Enabled) return 1.0f;
	f = (float)ticksLeft - t + 1.0f;
	if (f >= 10.0f) return 1.0f;
	s = 1.0f - f / 10.0f;
	if (s < 0.0f) s = 0.0f;
	if (s > 1.0f) s = 1.0f;
	s *= s; s *= s;
	return 1.0f + s * 0.3f;
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

static void TntFuse_BuildGlowCube(float ox, float oy, float oz, float size, PackedCol col, struct VertexColoured** vertices) {
	struct VertexColoured* v = *vertices;
	float x0 = ox, x1 = ox + size;
	float y0 = oy, y1 = oy + size;
	float z0 = oz, z1 = oz + size;

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
static void TntCube_BuildFace(float ox, float oy, float oz, float size, int face,
							   TextureRec r, PackedCol col, struct VertexTextured** vertices) {
	struct VertexTextured* v = *vertices;
	float x0 = ox, x1 = ox + size;
	float y0 = oy, y1 = oy + size;
	float z0 = oz, z1 = oz + size;

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
	float swell;
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
			col   = DropItem_WorldColor(&pos);
			swell = TntFuse_Swell(tnt->ticksLeft, t);
			for (f = 0; f < FACE_COUNT; f++) {
				loc = Block_Tex(BLOCK_TNT, f);
				if (Atlas1D_Index(loc) != atlas) continue;
				rec = Atlas1D_TexRec(loc, 1, &idx);
				TntCube_BuildFace(pos.x - 0.5f * swell, pos.y - 0.5f * swell,
								  pos.z - 0.5f * swell, swell, f, rec, col, &ptr);
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
	float alpha, swell;
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
		if (IndevTest_Enabled) {
			/* RenderTNTPrimed: overlay only while fuse/5 % 2 == 0 (5 ticks
			    on / 5 off), alpha ramping (1 - (fuse+1)/100) * 0.8 - ~0.17
			    freshly armed up to 0.8 at detonation. Additive shell kept
			    as the established stand-in for genuine's SRC_ALPHA/
			    DST_ALPHA framebuffer blend. */
			int fuse = tnt->ticksLeft < 0 ? 0 : tnt->ticksLeft;
			if (fuse / 5 % 2 != 0) continue;
			alpha = (1.0f - ((float)fuse - t + 1.0f) / 100.0f) * 0.8f;
			if (alpha < 0.0f) alpha = 0.0f;
			if (alpha > 1.0f) alpha = 1.0f;
		} else {
			alpha = TntFuse_GlowAlpha(tnt->ticksLeft);
		}
		swell = TntFuse_Swell(tnt->ticksLeft, t);
		col   = PackedCol_Make(255, 255, 255, (cc_uint8)(255.0f * alpha));
		TntFuse_BuildGlowCube(pos.x - 0.5f * swell, pos.y - 0.5f * swell,
							  pos.z - 0.5f * swell, swell, col, &ptr);
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

/* An upright sprite quad that only turns around Y to face the camera
    (RenderItem's glRotatef(180 - playerViewY, 0,1,0)) - full camera-facing
    billboards tilt with the view pitch, which Indev item sprites never do. */
static void Drop_BuildUprightSprite(Vec3 pos, float half, TextureRec* rec, PackedCol col,
									struct VertexTextured** vertices) {
	struct VertexTextured* v = *vertices;
	float dx = Camera.CurrentPos.x - pos.x, dz = Camera.CurrentPos.z - pos.z;
	float len = Math_SqrtF(dx * dx + dz * dz), rx, rz;
	if (len < 0.001f) { rx = half; rz = 0.0f; }
	else              { rx = -dz / len * half; rz = dx / len * half; }

	#define SPR_V(px, py, pz, uu, vv) v->x = (px); v->y = (py); v->z = (pz); v->Col = col; v->U = (uu); v->V = (vv); v++;
	SPR_V(pos.x - rx, pos.y - half, pos.z - rz, rec->u1, rec->v2)
	SPR_V(pos.x + rx, pos.y - half, pos.z + rz, rec->u2, rec->v2)
	SPR_V(pos.x + rx, pos.y + half, pos.z + rz, rec->u2, rec->v1)
	SPR_V(pos.x - rx, pos.y + half, pos.z - rz, rec->u1, rec->v1)
	#undef SPR_V
	*vertices = v;
}

static void SurvivalTest_RenderItemDropSprites(float t) {
	static cc_uint8 blockSpriteAtlas[DROP_MAX * 4];
	struct VertexTextured* data;
	struct VertexTextured* ptr;
	struct DropItem* d;
	GfxResourceID tex = IndevTest_ItemsTex();
	TextureRec rec;
	Vec3 pos;
	float renderAge, bob;
	int i, count = 0, blockSpriteStart = 0, blockSpriteEnd = 0;
	cc_bool any = false;

	for (i = 0; i < DROP_MAX; i++) {
		if (!st_drops[i].active) continue;
		if (!ST_ID_IS_BLOCK(st_drops[i].block)) { if (tex) any = true; }
		else if (IndevTest_DropIsSprite(st_drops[i].block)) any = true;
		if (any) break;
	}
	if (!any) return;

	if (!st_itemDropVB) {
		st_itemDropVB = Gfx_CreateDynamicVb(VERTEX_FORMAT_TEXTURED, ITEMDROP_MAX_VERTICES);
	}
	Gfx_SetVertexFormat(VERTEX_FORMAT_TEXTURED);
	data = (struct VertexTextured*)Gfx_LockDynamicVb(st_itemDropVB, VERTEX_FORMAT_TEXTURED, ITEMDROP_MAX_VERTICES);
	ptr  = data;

	for (i = 0; i < DROP_MAX && tex; i++) {
		int copies, c;
		d = &st_drops[i];
		if (!d->active || ST_ID_IS_BLOCK(d->block)) continue;
		if (!IndevTest_ItemSpriteUV(d->block, &rec.u1, &rec.v1, &rec.u2, &rec.v2)) continue;

		Vec3_Lerp(&pos, &d->prevPos, &d->position, t);
		renderAge = Math_Lerp(d->prevAge, d->age, t);
		/* same bob the block cubes use (mode-split in Drop_Bob) */
		bob = Drop_Bob(d, renderAge);
		pos.y += bob + 0.125f;

		/* RenderItem.doRender: 0.5-unit UPRIGHT quads (yaw-only billboard), */
		/*  a stack drawing 1/2/3/4 jumbled copies (fixed seed 187). */
		copies = Drop_Copies(d->count);
		for (c = 0; c < copies; c++) {
			Vec3 cpos = pos;
			Vec3_AddBy(&cpos, &drop_jumble[c]);
			Drop_BuildUprightSprite(cpos, 0.25f, &rec, DropItem_WorldColor(&cpos), &ptr);
			count += 4;
		}
	}

	/* Indev sprite-type BLOCK drops (flowers/saplings/mushrooms/torches): */
	/*  the same upright sprite, but textured with the block's terrain tile. */
	/*  Batched after the item sprites so per-1D-atlas draw runs stay whole. */
	blockSpriteStart = count;
	for (i = 0; i < DROP_MAX && IndevTest_Enabled; i++) {
		int copies, c;
		TextureLoc loc;
		int texIndex;
		d = &st_drops[i];
		if (!d->active || !ST_ID_IS_BLOCK(d->block)) continue;
		if (!IndevTest_DropIsSprite(d->block)) continue;

		loc = Block_Tex((BlockID)d->block, FACE_XMIN);
		rec = Atlas1D_TexRec(loc, 1, &texIndex);

		Vec3_Lerp(&pos, &d->prevPos, &d->position, t);
		renderAge = Math_Lerp(d->prevAge, d->age, t);
		bob = Drop_Bob(d, renderAge);
		pos.y += bob + 0.125f;

		copies = Drop_Copies(d->count);
		for (c = 0; c < copies; c++) {
			Vec3 cpos = pos;
			Vec3_AddBy(&cpos, &drop_jumble[c]);
			if ((count - blockSpriteStart) / 4 >= DROP_MAX * 4) break;
			blockSpriteAtlas[(count - blockSpriteStart) / 4] = Atlas1D_Index(loc);
			Drop_BuildUprightSprite(cpos, 0.25f, &rec, DropItem_WorldColor(&cpos), &ptr);
			count += 4;
		}
	}
	blockSpriteEnd = count;

	Gfx_UnlockDynamicVb(st_itemDropVB);
	if (!count) return;

	Gfx_SetAlphaTest(true);
	/* range 1: item-id sprites (items.png) */
	if (blockSpriteStart > 0 && tex) {
		Gfx_BindTexture(tex);
		Gfx_DrawVb_IndexedTris_Range(blockSpriteStart, 0, DRAW_HINT_NONE);
	}
	/* range 2: sprite-block drops (terrain tiles), in per-1D-atlas runs */
	{
		int off = blockSpriteStart, quad = 0, runStart, runAtlas;
		while (off < blockSpriteEnd) {
			runStart = off; runAtlas = blockSpriteAtlas[quad];
			while (off < blockSpriteEnd && blockSpriteAtlas[quad] == runAtlas) { off += 4; quad++; }
			Atlas1D_Bind(runAtlas);
			Gfx_DrawVb_IndexedTris_Range(off - runStart, runStart, DRAW_HINT_NONE);
		}
	}
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
		float ageT, quadHalf, fade;
		s = &st_tntSmoke[i];
		if (!s->active) continue;

		Vec3_Lerp(&pos, &s->prevPos, &s->pos, t);
		ageT = ((float)s->age + t) / (float)s->life;
		if (ageT < 0.0f) ageT = 0.0f;
		if (ageT > 1.0f) ageT = 1.0f;

		/* EntityFX.renderParticle: 16x16 grid on particles.png, cell = the
		    class's particleTextureIndex (smoke walks 7..0 along the top row) */
		switch (s->kind) {
		case PUFF_INDEV_FLAME:  frame = 48; break;
		case PUFF_INDEV_BUBBLE: frame = 32; break;
		case PUFF_INDEV_SPLASH: frame = 17; break;
		case PUFF_INDEV_LAVA:   frame = 49; break;
		default:
			frame = 7 - (s->age * 8) / s->life;
			if (frame < 0) frame = 0;
			break;
		}
		rec.u1 = (frame % 16) / 16.0f;
		rec.u2 = rec.u1 + 0.0624375f;
		rec.v1 = (frame / 16) / 16.0f;
		rec.v2 = rec.v1 + 0.0624375f;

		lit = DropItem_WorldColor(&pos);
		switch (s->kind) {
		case PUFF_INDEV_SMOKE:
			/* renderParticle: scale grows in fast, clamp01(ageT * 32) */
			fade = ageT * 32.0f;
			if (fade > 1.0f) fade = 1.0f;
			quadHalf = 0.1f * s->scale * fade;
			col = PackedCol_Make((cc_uint8)(PackedCol_R(lit) * s->gray),
								 (cc_uint8)(PackedCol_G(lit) * s->gray),
								 (cc_uint8)(PackedCol_B(lit) * s->gray), 255);
			break;
		case PUFF_INDEV_FLAME:
			/* shrinks (1 - t^2 * 0.5); brightness lerps from fullbright at */
			/*  birth to the world's lighting at expiry (getEntityBrightness) */
			quadHalf = 0.1f * s->scale * (1.0f - ageT * ageT * 0.5f);
			col = PackedCol_Make(
				(cc_uint8)(PackedCol_R(lit) * ageT + 255.0f * (1.0f - ageT)),
				(cc_uint8)(PackedCol_G(lit) * ageT + 255.0f * (1.0f - ageT)),
				(cc_uint8)(PackedCol_B(lit) * ageT + 255.0f * (1.0f - ageT)), 255);
			break;
		case PUFF_INDEV_LAVA:
			/* shrinks (1 - t^2), always fullbright (getEntityBrightness = 1) */
			quadHalf = 0.1f * s->scale * (1.0f - ageT * ageT);
			col = PACKEDCOL_WHITE;
			break;
		case PUFF_INDEV_BUBBLE:
		case PUFF_INDEV_SPLASH:
			/* plain white, world-lit, constant scale */
			quadHalf = 0.1f * s->scale;
			col = lit;
			break;
		default:
			/* rCol=gCol=bCol (the random 0..0.3 grey) * the block's brightness. */
			quadHalf = 0.15f;
			col = PackedCol_Make((cc_uint8)(PackedCol_R(lit) * s->gray),
								 (cc_uint8)(PackedCol_G(lit) * s->gray),
								 (cc_uint8)(PackedCol_B(lit) * s->gray), 255);
			break;
		}

		size.x = quadHalf; size.y = quadHalf;
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
	/* Everything above is a genuine natural spawn - MobSpawner.spawn rolls */
	/*  nextInt(6) over exactly these six types, so this marks that range. */
	MOB_SPAWN_COUNT,
	/* The generic humanoid EntityLiving (char.png skin, passive wander AI) that */
	/*  in-20100201 spawns as the "Human" mob. We only ever create it via the */
	/*  /client spawn debug command - it is deliberately OUTSIDE MOB_SPAWN_COUNT */
	/*  so the natural spawner never rolls it. */
	MOB_TYPE_HUMAN = MOB_SPAWN_COUNT,
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
	float       w030, h030;   /* c0.30 setSize(width, height) */
	float       wIndev, hIndev; /* Indev setSize - pig/sheep shrink vs c0.30 */
};
/* Order matches MobSpawner.spawn's `type = random.nextInt(6)` exactly, so */
/*  Mob_SpawnerRun can index straight into this table with that roll. */
/* Collision sizes are the genuine setSize values (Entity default 0.6x1.8; */
/*  c0.30 QuadrupedMob 1.4x1.2, Sheep 1.4x1.72, Spider 1.4x0.9; Indev */
/*  shrinks EntityPig to 0.9x0.9 and EntitySheep to 0.9x1.3) - the engine */
/*  models' GetCollisionSize boxes were all slightly small. */
static const struct MobTypeInfo mobTypeInfo[MOB_TYPE_COUNT] = {
	/* ZOMBIE   */ { "zombie",   MOB_AI_ATTACK,     1.00f, 30.0f, 6, false,  80, 1.62f, 0.6f,1.8f,  0.6f,1.8f },
	/* SKELETON */ { "skeleton", MOB_AI_ATTACK,     0.30f,  0.0f, 8, false, 120, 1.62f, 0.6f,1.8f,  0.6f,1.8f },
	/* PIG      */ { "pig",      MOB_AI_PASSIVE,    0.70f,  0.0f, 0, false,  10, 1.72f, 1.4f,1.2f,  0.9f,0.9f },
	/* CREEPER  */ { "creeper",  MOB_AI_ATTACK,     0.70f, 45.0f, 6, true,  200, 1.62f, 0.6f,1.8f,  0.6f,1.8f },
	/* SPIDER   */ { "spider",   MOB_AI_JUMPATTACK, 0.56f,  0.0f, 6, false, 105, 0.72f, 1.4f,0.9f,  1.4f,0.9f },
	/* SHEEP    */ { "sheep",    MOB_AI_PASSIVE,    0.70f,  0.0f, 0, false,  10, 1.72f, 1.4f,1.72f, 0.9f,1.3f },
	/* HUMAN    */ { "humanoid", MOB_AI_PASSIVE,    0.70f,  0.0f, 0, false,   0, 1.62f, 0.6f,1.8f,  0.6f,1.8f },
};

struct Mob;
static void Mob_ApplySize(struct Mob* m);

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
	cc_bool  wasInWater;  /* last tick's water state - the air->water edge splashes */

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

	/* Indev EntityCreature: the current A* path being followed (Pathfinder */
	/*  port below). count 0 = no path. */
	cc_uint8 pathCount, pathIndex;
	cc_int16 pathX[64], pathY[64], pathZ[64];

	/* Indev layer state (unused in c0.30 mode) */
	cc_int16 fire;      /* Entity.fire burn ticks - 300 from sunlight, 600 from lava */
	cc_int16 livingSnd; /* EntityLiving.livingSoundTime - the ambient-sound roll counter */
	cc_int16 fuseTicks; /* EntityCreeper.timeSinceIgnited - swell/fuse progress */
	cc_int16 fuseLast;  /* EntityCreeper.lastActiveTime - previous tick's fuse, for render interp */
	cc_int8  fuseState; /* EntityCreeper.creeperState: -1 idle, 1 swelling, 2 winding down */

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

	/* MP puppet (phase 3, networking-plan 15.1/17.5): the server runs
	    AI/physics/damage and streams SURV_MOB_*; these fields key the slot to
	    the server's mob id and latch the most recent movement target, which
	    the puppet tick consumes so prev->next interpolation works exactly as
	    for local mobs. netState pins fire/fuse/graze between STATE messages. */
	cc_uint16 netId;
	cc_uint8  netState;
	cc_bool   netHasMove;
	Vec3      netPos;
	float     netYaw, netPitch;
};
static struct Mob st_mobs[MOB_MAX];

/* Genuine Entity.setSize per mob type and mode. Entity_SetModel resets
    Base.Size from the engine model's GetCollisionSize (all slightly small),
    so this is re-applied after EVERY model swap (sheep shearing). */
static void Mob_ApplySize(struct Mob* m) {
	const struct MobTypeInfo* info = &mobTypeInfo[m->type];
	float w = IndevTest_Enabled ? info->wIndev : info->w030;
	float h = IndevTest_Enabled ? info->hIndev : info->h030;
	Vec3_Set(m->Base.Size, w, h, w);
}
/* Which st_mobs[] slot dealt the hurt currently being applied (-1 = player or
    environment). Set by attack/arrow code right before Mob_Hurt so BasicAttack-
    AI.hurt's attackTarget=cause aggro can identify a mob attacker. */
static int st_hurtCauseSlot = -1;

/* World.lightBrightnessTable[light]: the float brightness curve every
    "getEntityBrightness > 0.5" style check reads. (1-v)/(v*3+1)*0.95+0.05
    with v = 1 - light/15, so > 0.5 needs a light level of 12+. */
static float Indev_LightBrightness(int x, int y, int z) {
	return IndevTest_BrightnessOfLight(IndevTest_LightLevel(x, y, z));
}

static float Mob_Brightness(struct Mob* m) {
	struct Entity* e = &m->Base;
	return Indev_LightBrightness(Math_Floor(e->Position.x),
								 Math_Floor(e->Position.y),
								 Math_Floor(e->Position.z));
}

/* Indev EntityLiving.moveSpeed: 0.7 default, zombie 0.5, spider 0.8. The
    c0.30 runSpeed table (zombie 1.0, skeleton 0.3) is a different tuning for
    a different mover - feeding those into the 20Hz waypoint steering made
    mobs overshoot the waypoint every tick and flip 180 degrees back. */
static float Mob_IndevMoveSpeed(int type) {
	if (type == MOB_TYPE_ZOMBIE) return 0.5f;
	if (type == MOB_TYPE_SPIDER) return 0.8f;
	return 0.7f;
}

/* The (rand - rand)*0.2 + 1 pitch jitter every living-entity sound uses */
static float Mob_SndPitch(void) {
	return (Random_Float(&st_mobRng) - Random_Float(&st_mobRng)) * 0.2f + 1.0f;
}

/* World.playSoundAtEntity for a sound sourced at an arbitrary position - the
    distance to the local player feeds the 16-block cutoff + falloff. All the
    Indev entity sounds funnel through here, so nothing plays in c0.30 mode. */
static void Indev_PlaySoundAt(Vec3 pos, int type, float vol, float pitch) {
	struct LocalPlayer* p = Entities.CurPlayer;
	float dx, dy, dz;
	if (!IndevTest_Enabled || !p) return;

	dx = p->Base.Position.x - pos.x;
	dy = p->Base.Position.y - pos.y;
	dz = p->Base.Position.z - pos.z;
	Audio_PlayMobSound(type, vol, pitch, Math_SqrtF(dx * dx + dy * dy + dz * dz));
}

static void Mob_PlaySound(struct Mob* m, int type, float vol, float pitch) {
	Indev_PlaySoundAt(m->Base.Position, type, vol, pitch);
}

/* World.playSoundAtPlayer for a sound at a block's centre - the form the
    block-driven sounds (fire crackle/ignite/fizz) use. */
void SurvivalTest_PlaySoundAtBlock(int x, int y, int z, int type, float vol, float pitch) {
	Vec3 pos;
	pos.x = (float)x + 0.5f; pos.y = (float)y + 0.5f; pos.z = (float)z + 0.5f;
	Indev_PlaySoundAt(pos, type, vol, pitch);
}

/* Entity.onEntityUpdate: falling into water plays "random.splash" at a
    speed-scaled volume and throws up a ring of bubbles + water droplets
    (1 + width*20 of each, spread +-width around the entity, one block
    above the surface cell the feet just entered). */
static void Indev_EntitySplash(Vec3 pos, Vec3 vel, float width) {
	float vol, fy, fx, fz;
	int i, n;
	if (!IndevTest_Enabled) return;

	vol = Math_SqrtF(vel.x * vel.x * 0.2f + vel.y * vel.y +
	                 vel.z * vel.z * 0.2f) * 0.2f;
	if (vol > 1.0f) vol = 1.0f;
	Indev_PlaySoundAt(pos, MOBSND_SPLASH, vol,
		1.0f + (Random_Float(&st_dropRng) - Random_Float(&st_dropRng)) * 0.4f);

	fy = (float)((int)pos.y) + 1.0f;
	n  = (int)(1.0f + width * 20.0f);
	for (i = 0; i < n; i++) {
		fx = pos.x + (Random_Float(&st_dropRng) * 2.0f - 1.0f) * width;
		fz = pos.z + (Random_Float(&st_dropRng) * 2.0f - 1.0f) * width;
		Indev_SpawnBubbleFX(fx, fy, fz,
			vel.x, vel.y - Random_Float(&st_dropRng) * 0.2f, vel.z);
	}
	for (i = 0; i < n; i++) {
		fx = pos.x + (Random_Float(&st_dropRng) * 2.0f - 1.0f) * width;
		fz = pos.z + (Random_Float(&st_dropRng) * 2.0f - 1.0f) * width;
		SurvivalTest_SpawnSplashFX(fx, fy, fz);
	}
}

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
/*  see mobTypeInfo's deathScore column). Pig.die AND Sheep.die both drop */
/*  1-2 brown mushrooms (identical bodies; wool only comes from the shear). */
static void Mob_Die(struct Mob* m, cc_bool playerCredit) {
	/* Indev has no kill score - EntityLiving.onDeath reinterprets
	    scoreValue() as the death-drop item id instead */
	if (playerCredit && !IndevTest_Enabled) st_score += mobTypeInfo[m->type].deathScore;
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
				/* Entity.dropItemWithOffset -> entityDropItem(id, 1, 0):
				    spawned AT the mob's position - the randomness is all in
				    the EntityItem ctor velocity SpawnDropAt already applies */
				pos = m->Base.Position;
				SurvivalTest_SpawnDropAt(pos, (cc_uint16)indevDeathDrop[m->type], 1);
			}
		}
	} else if (m->type == MOB_TYPE_PIG) {
		Mob_SpawnDeathDrops(m, BLOCK_BROWN_SHROOM);
	}
	if (!IndevTest_Enabled && m->type == MOB_TYPE_SHEEP) Mob_SpawnDeathDrops(m, BLOCK_BROWN_SHROOM);
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
	SurvivalTest_Explode(&m->Base, center, (float)EXPLOSION_RADIUS);

	/* CreeperAI.beforeRemove: 500 LEAVES-textured TerrainParticles burst out */
	/*  at gaussian offsets. Approximated with the engine's block-break burst */
	/*  (see the TNT detonation note in SurvivalTest_TickTnt). */
	IVec3_Floor(&coords, &center);
	Particles_BreakBlockEffect(coords, BLOCK_LEAVES, BLOCK_AIR);
}

/* Indev EntitySkeleton.attackEntity's bow shot: the raw UNNORMALIZED aim
    vector (dx, dy-to-target-eye-minus-0.2 + horizontal-distance * 0.2 lob,
    dz) goes straight into setArrowHeading(0.6F, 12.0F) - speed 0.6, spread
    factor 12 (gaussian sigma 0.09 per velocity axis), random.bow at the
    genuine pitch. Damage is the tick's flat 4 for every Indev arrow. */
static void Mob_IndevShootArrow(struct Mob* m, struct Entity* te) {
	struct Entity* e = &m->Base;
	Vec3 from = Entity_GetEyePosition(e);
	Vec3 aim;
	float hor, yawRad;
	int slot = (int)(m - st_mobs);

	/* EntityArrow's constructor offsets apply to every arrow (0.1 down,
	    0.16 sideways along the yaw) BEFORE the skeleton's ++posY - the same
	    adjustment the player bow makes */
	yawRad  = e->Yaw * MATH_DEG2RAD;
	from.x += Math_CosF(yawRad) * 0.16f;
	from.y -= 0.1f;
	from.z += Math_SinF(yawRad) * 0.16f;
	from.y += 1.0f; /* shootArrow: ++arrow.posY above the (eye-anchored) spawn */
	aim.x = te->Position.x - e->Position.x;
	aim.z = te->Position.z - e->Position.z;
	/* Genuine aims at target.posY - 0.2 where posY is the EYE-anchored Java
	    position - CC's Position.y is the feet, which made arrows dive at the
	    player's ankles. Aim relative to the target's eye point instead. */
	aim.y = (Entity_GetEyePosition(te).y - 0.2f) - from.y;
	hor   = Math_SqrtF(aim.x * aim.x + aim.z * aim.z);
	aim.y += hor * 0.2f; /* the lob that lets skeleton shots clear mid-range dips */

	Mob_PlaySound(m, MOBSND_BOW, 1.0f, 1.0f / (Random_Float(&st_mobRng) * 0.4f + 0.8f));
	SurvivalTest_SpawnArrowIndev(from, aim, 0.6f, 12.0f, false, slot);
}

/* Indev EntityCreeper.attackEntity's fuse expiry: createExplosion(radius 3)
    then setEntityDead - the creeper vanishes instantly, with NO death
    animation and NO gunpowder (drops only come from killing it first). */
static void Mob_IndevCreeperBlast(struct Mob* m) {
	Vec3 center = m->Base.Position;
	IVec3 coords;
	center.y += mobTypeInfo[m->type].heightOff;
	SurvivalTest_Explode(&m->Base, center, 3.0f);

	IVec3_Floor(&coords, &center);
	Particles_BreakBlockEffect(coords, BLOCK_LEAVES, BLOCK_AIR);
	m->health = 0;
	m->active = false;
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
/* Returns whether the hit actually landed (attackEntityFrom's boolean) - */
/*  false when fully absorbed by the invulnerability window, or a no-op. */
/*  Indev arrows bounce off on false (EntityArrow.java:131-139). */
static cc_bool Mob_Hurt(struct Mob* m, struct Entity* attacker, int damage, cc_bool playerCredit) {
	struct Entity* e = &m->Base;
	float dx, dz, dist;
	IVec3 coords;
	int woolCount, i;

	if (m->health <= 0)        return false;
	if (damage <= 0)           return false;

	/* Sheep.hurt(): a Player punch against a still-furred sheep shears it */
	/*  instead of dealing damage at all - drops 1-3 white wool and clears */
	/*  hasFur, then returns without calling the normal hurt() body (so no */
	/*  damage, no invincibility window, no knockback). Only a genuine player */
	/*  punch counts (Entities.CurPlayer is singleplayer's only Player), not */
	/*  arrows or other sources - matches `attacker instanceof Player`. */
	if (m->type == MOB_TYPE_SHEEP && m->hasFur && IndevTest_Enabled && attacker &&
		!st_hurtViaArrow) {
		/* Indev EntitySheep.attackEntityFrom: any LIVING attacker (player or
		    mob) shears 1 + rand(3) GRAY cloth - an arrow passes ITSELF (an
		    Entity, not an EntityLiving), so arrows never shear - and, unlike
		    c0.30, the code falls through to super.attackEntityFrom, so the
		    hit still deals its damage after shearing. */
		m->hasFur = false;
		{ cc_string mdl = String_FromReadonly("sheep_nofur"); Entity_SetModel(&m->Base, &mdl); Mob_ApplySize(m); }
		woolCount = 1 + Random_Next(&st_mobRng, 3);

		/* EntitySheep: entityDropItem(clothGray, 1, +1.0F) per wool, with
		    extra motion jitter on top of the EntityItem ctor velocity */
		for (i = 0; i < woolCount; i++) {
			struct DropItem* wd;
			Vec3 wpos = e->Position; wpos.y += 1.0f;
			wd = SurvivalTest_SpawnDropAtEx(wpos, BLOCK_GRAY, 1);
			if (!wd) continue;
			wd->velocity.y += Random_Float(&st_mobRng) * 0.05f * 20.0f;
			wd->velocity.x += (Random_Float(&st_mobRng) - Random_Float(&st_mobRng)) * 0.1f * 20.0f;
			wd->velocity.z += (Random_Float(&st_mobRng) - Random_Float(&st_mobRng)) * 0.1f * 20.0f;
		}
	} else if (m->type == MOB_TYPE_SHEEP && m->hasFur &&
		Entities.CurPlayer && attacker == &Entities.CurPlayer->Base) {
		m->hasFur = false;
		/* Sheep.renderModel only draws the fur layer while hasFur - swap to */
		/*  the engine's furless sheep model so shearing is actually visible. */
		{ cc_string mdl = String_FromReadonly("sheep_nofur"); Entity_SetModel(&m->Base, &mdl); Mob_ApplySize(m); }
		woolCount = (int)(Random_Float(&st_mobRng) * 3.0f + 1.0f); /* 1-3 */

		coords.x = Math_Floor(e->Position.x);
		coords.y = Math_Floor(e->Position.y);
		coords.z = Math_Floor(e->Position.z);
		for (i = 0; i < woolCount; i++) { SurvivalTest_SpawnDrop(coords, BLOCK_WHITE); }
		return false; /* c0.30 shear replaces the hit entirely - no damage landed */
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
		} else if (IndevTest_Enabled || st_mobs[st_hurtCauseSlot].type != m->type) {
			/* the same-species exemption is c0.30 BasicAttackAI.hurt only -
			    Indev EntityMob.attackEntityFrom retaliates against anything */
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
		if (m->lastHealth - damage >= m->health) return false; /* absorbed */
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

	/* EntityLiving.attackEntityFrom plays the death/hurt sound once a hit has
	    actually landed (absorbed hits above returned before this). Only pig
	    and sheep override the defaults - every monster is just "random.hurt"
	    in in-20100223 (their voices arrived in Alpha). */
	if (IndevTest_Enabled) {
		int snd = MOBSND_HURT;
		if (m->type == MOB_TYPE_SHEEP) snd = MOBSND_SHEEP;
		if (m->type == MOB_TYPE_PIG)   snd = m->health <= 0 ? MOBSND_PIGDEATH : MOBSND_PIG;
		Mob_PlaySound(m, snd, 1.0f, Mob_SndPitch());
	}

	if (m->health <= 0) {
		m->health = 0;
		Mob_Die(m, playerCredit);
	}
	return true;
}

/*------------------------------------------------------------------------*/
/* Indev World.createExplosion (World.java:1102-1233), ported in genuine   */
/* order: sound, 1352-boundary-ray block collection, entity damage over    */
/* the INTACT world (density raycast), then descending-order destruction   */
/* with the Indev drop table at 30% chance and TNT chain arming.           */
/*------------------------------------------------------------------------*/

/* Block.getExplosionResistance() = resistance/5, from setResistance(x*3) /
    setHardness(max(cur, h*5)) chains in Block.java's static init. Values
    below are the effective per-id results (engine ids; classic 1-49 map
    1:1 onto the genuine ids). */
static float Indev_ExplosionResistance(BlockID b) {
	switch (b) {
	case BLOCK_STONE: case BLOCK_COBBLE: case BLOCK_GOLD: case BLOCK_IRON:
	case BLOCK_DOUBLE_SLAB: case BLOCK_SLAB: case BLOCK_BRICK:
	case BLOCK_MOSSY_ROCKS: case BLOCK_OBSIDIAN: /* obsidian is NOT special in Indev */
	case 57: /* diamond block: setResistance(10) -> 30 -> effective 6.0 */
		return 6.0f;
	case 55: /* gears: setHardness(0.5) alone -> 2.5 -> effective 0.5 */
		return 0.5f;
	case BLOCK_WOOD: return 3.0f;
	case BLOCK_GOLD_ORE: case BLOCK_IRON_ORE: case BLOCK_COAL_ORE:
	case 56 /* diamond ore */: return 3.0f;
	case BLOCK_LOG: return 2.0f;
	case BLOCK_BOOKSHELF: return 1.5f;
	/* BlockFluid's ctor resistance 6 is raised to 500 by setHardness(100):
	    water and STILL lava are effectively blast-proof... */
	case BLOCK_WATER: case BLOCK_STILL_WATER: case BLOCK_STILL_LAVA:
		return 100.0f;
	/* ...but flowing lava's setHardness(0) leaves the ctor's 6 -> 1.2 */
	case BLOCK_LAVA: return 1.2f;
	case BLOCK_BEDROCK: return 3600000.0f;
	case BLOCK_RED: case BLOCK_ORANGE: case BLOCK_YELLOW: case BLOCK_LIME:
	case BLOCK_GREEN: case BLOCK_TEAL: case BLOCK_AQUA: case BLOCK_CYAN:
	case BLOCK_BLUE: case BLOCK_INDIGO: case BLOCK_VIOLET: case BLOCK_MAGENTA:
	case BLOCK_PINK: case BLOCK_BLACK: case BLOCK_GRAY: case BLOCK_WHITE:
		return 0.8f;
	case BLOCK_GRASS: case BLOCK_GRAVEL: case BLOCK_SPONGE:
	case 83: case 84 /* farmland */: return 0.6f;
	case BLOCK_DIRT: case BLOCK_SAND: return 0.5f;
	case BLOCK_GLASS: return 0.3f;
	case BLOCK_LEAVES: return 0.2f;
	case 54: case 71: case 72: case 73: case 74: /* chest + facings */
	case 58 /* workbench */: return 2.5f;
	case 61: case 62: case 75: case 76: case 77: case 78:
	case 79: case 80: case 81: case 82: /* furnaces */ return 3.5f;
	/* sapling, flowers, mushrooms, TNT, torch 50, fire 51, crops 85-92,
	    wall torches 94-97, air: resistance 0 */
	default: return 0.0f;
	}
}

/* World.rayTraceBlocks stand-in for the density test: does any collidable
    block sit between the two points? Fluids, fire and sprite-draw blocks
    pass through (genuine isCollidable()==false / tiny collision bounds).
    Sample-marched at quarter-block resolution, capped like the genuine
    20-cell DDA. */
static cc_bool Indev_RayBlocked(Vec3 from, Vec3 to) {
	float dx = to.x - from.x, dy = to.y - from.y, dz = to.z - from.z;
	float len = Math_SqrtF(dx * dx + dy * dy + dz * dz);
	float t, px, py, pz;
	int i, steps, bx, by, bz;
	BlockID b;

	if (len < 0.0001f) return false;
	steps = (int)(len / 0.25f) + 1;
	if (steps > 80) steps = 80; /* ~20 blocks, the genuine DDA cap */

	for (i = 1; i < steps; i++) {
		t  = (float)i / (float)steps;
		px = from.x + dx * t; py = from.y + dy * t; pz = from.z + dz * t;
		bx = (int)px; by = (int)py; bz = (int)pz;
		if (!World_Contains(bx, by, bz)) continue;

		b = World_GetBlock(bx, by, bz);
		if (b == BLOCK_AIR)                      continue;
		if (Blocks.Draw[b] == DRAW_SPRITE)       continue;
		if (Blocks.Collide[b] == COLLIDE_LIQUID) continue;
		if (IndevFire_IsFire(b))                 continue;
		return true;
	}
	return false;
}

/* World.getBlockDensity: the fraction of grid samples over the entity's
    AABB with unobstructed line of sight to the blast centre (0 = fully
    shielded, 1 = fully exposed). Grid step = 1/(size*2+1) per axis. */
static float Indev_BlockDensity(Vec3 center, struct Entity* e) {
	struct AABB bb;
	Vec3 s;
	float fx, fy, fz, sx, sy, sz;
	int seen = 0, total = 0;

	Entity_GetBounds(e, &bb);
	sx = 1.0f / ((bb.Max.x - bb.Min.x) * 2.0f + 1.0f);
	sy = 1.0f / ((bb.Max.y - bb.Min.y) * 2.0f + 1.0f);
	sz = 1.0f / ((bb.Max.z - bb.Min.z) * 2.0f + 1.0f);

	for (fx = 0.0f; fx <= 1.0f; fx += sx)
	for (fy = 0.0f; fy <= 1.0f; fy += sy)
	for (fz = 0.0f; fz <= 1.0f; fz += sz) {
		s.x = bb.Min.x + (bb.Max.x - bb.Min.x) * fx;
		s.y = bb.Min.y + (bb.Max.y - bb.Min.y) * fy;
		s.z = bb.Min.z + (bb.Max.z - bb.Min.z) * fz;
		if (!Indev_RayBlocked(s, center)) seen++;
		total++;
	}
	return total ? (float)seen / (float)total : 0.0f;
}

/* dropBlockAsItemWithChance(..., 0.3F) through the INDEV idDropped table -
    the 30% gate rolls FIRST, then idDropped's own rolls, like genuine.
    Crops drop wheat only at full growth and never seeds (the seed rolls
    live in onBlockDestroyedByPlayer, player mining only). */
static void Indev_ExplodeDrops(IVec3 coords, BlockID oldBlock) {
	int dropId = oldBlock;
	if (Random_Float(&st_dropRng) > 0.3f) return;

	switch (oldBlock) {
	case BLOCK_GRASS:    dropId = BLOCK_DIRT;   break;
	case BLOCK_STONE: case BLOCK_OBSIDIAN:
	                     dropId = BLOCK_COBBLE; break;
	case BLOCK_COAL_ORE: dropId = 256 + 7;      break;
	case 56:             dropId = 256 + 8;      break; /* diamond ore */
	case BLOCK_LEAVES:
		if (Random_Next(&st_dropRng, 10) != 0) return;
		dropId = BLOCK_SAPLING; break;
	case BLOCK_GRAVEL:
		dropId = Random_Next(&st_dropRng, 10) == 0 ? 256 + 62 : BLOCK_GRAVEL;
		break;
	case BLOCK_DOUBLE_SLAB: dropId = BLOCK_SLAB; break;
	case BLOCK_GLASS: case BLOCK_BOOKSHELF:
	case BLOCK_WATER: case BLOCK_STILL_WATER:
	case BLOCK_LAVA:  case BLOCK_STILL_LAVA:
		return;
	case 83: case 84:    dropId = BLOCK_DIRT;   break; /* farmland */
	case 92:             dropId = 256 + 40;     break; /* ripe crops -> wheat */
	case 85: case 86: case 87: case 88:
	case 89: case 90: case 91:                  return; /* growing crops */
	case 94: case 95: case 96: case 97:
	                     dropId = 50;           break; /* wall torches */
	case 71: case 72: case 73: case 74:
	                     dropId = 54;           break; /* chest facings */
	case 75: case 76: case 77: case 78:
	                     dropId = 61;           break; /* idle furnace facings */
	case 62: case 79: case 80: case 81: case 82:
	                     dropId = 62;           break; /* LIT furnace drops lit (no idDropped) */
	default: break;
	}
	SurvivalTest_SpawnDrop(coords, (cc_uint16)dropId);
}

/* Sets/tests bits in a +-16 window around the truncated blast centre
    (ray reach maxes out around 7 blocks, well inside). */
#define EXPL_DIM 33
static cc_uint8 expl_bits[(EXPL_DIM * EXPL_DIM * EXPL_DIM + 7) / 8];

static void Indev_CreateExplosion(struct Entity* exploder, Vec3 center, float r) {
	struct LocalPlayer* p = Entities.CurPlayer;
	struct Mob* m;
	int cx = (int)center.x, cy = (int)center.y, cz = (int)center.z;
	float dirx, diry, dirz, len, power, px, py, pz;
	float diam, dx, dy, dz, dist, d, dens, f;
	int i, j, k, bx, by, bz, rel, dmg, slot;
	BlockID id;
	IVec3 coords;

	Mem_Set(expl_bits, 0, sizeof(expl_bits));
	Indev_PlaySoundAt(center, MOBSND_EXPLODE, 4.0f,
		(1.0f + (Random_Float(&st_mobRng) - Random_Float(&st_mobRng)) * 0.2f) * 0.7f);

	/* (a) 16^3 boundary rays collect the destroy set. Power seeds
	    r*(0.7+rand*0.6), pays (resistance+0.3)*0.3 per occupied cell and a
	    flat 0.225 per 0.3-block step; cells passed while power > 0 die.
	    Plain (int) casts (truncation) match genuine. */
	for (i = 0; i < 16; i++)
	for (j = 0; j < 16; j++)
	for (k = 0; k < 16; k++) {
		if (!(i == 0 || i == 15 || j == 0 || j == 15 || k == 0 || k == 15)) continue;

		dirx = (float)i / 15.0f * 2.0f - 1.0f;
		diry = (float)j / 15.0f * 2.0f - 1.0f;
		dirz = (float)k / 15.0f * 2.0f - 1.0f;
		len  = Math_SqrtF(dirx * dirx + diry * diry + dirz * dirz);
		dirx /= len; diry /= len; dirz /= len;

		power = r * (0.7f + Random_Float(&st_dropRng) * 0.6f);
		px = center.x; py = center.y; pz = center.z;

		while (power > 0.0f) {
			bx = (int)px; by = (int)py; bz = (int)pz;
			if (World_Contains(bx, by, bz)) {
				id = World_GetBlock(bx, by, bz);
				if (id != BLOCK_AIR)
					power -= (Indev_ExplosionResistance(id) + 0.3f) * 0.3f;
				if (power > 0.0f) {
					rel = (bx - cx + 16) + (by - cy + 16) * EXPL_DIM +
					      (bz - cz + 16) * EXPL_DIM * EXPL_DIM;
					if (rel >= 0 && rel < EXPL_DIM * EXPL_DIM * EXPL_DIM)
						expl_bits[rel >> 3] |= (cc_uint8)(1 << (rel & 7));
				}
			}
			px += dirx * 0.3f; py += diry * 0.3f; pz += dirz * 0.3f;
			power -= 0.22500001f;
		}
	}

	/* (b) entity phase - BEFORE any block is removed, so the density rays
	    see the intact world. Range is 2*radius; damage
	    (int)((f^2+f)/2 * 8 * 2r + 1) with f = (1 - dist/2r) * density, and
	    an unconditional, uncapped velocity kick of dir * f on top. */
	diam = r * 2.0f;

	if (p && exploder != &p->Base) {
		struct Entity* e = &p->Base;
		dx = e->Position.x           - center.x;
		dy = (e->Position.y + 1.62f) - center.y;
		dz = e->Position.z           - center.z;
		dist = Math_SqrtF(dx * dx + dy * dy + dz * dz);
		d    = dist / diam;
		if (d <= 1.0f) {
			dens = Indev_BlockDensity(center, e);
			f    = (1.0f - d) * dens;
			dmg  = (int)((f * f + f) / 2.0f * 8.0f * diam + 1.0f);
			if (exploder) SurvivalTest_HurtFrom(dmg, exploder->Position);
			else          SurvivalTest_Hurt(dmg);
			if (dist > 0.0001f) {
				e->Velocity.x += dx / dist * f;
				e->Velocity.y += dy / dist * f;
				e->Velocity.z += dz / dist * f;
			}
		}
	}

	/* the exploding creeper's own mob slot, for correct aggro attribution */
	slot = -1;
	for (i = 0; exploder && i < MOB_MAX; i++) {
		if (&st_mobs[i].Base == exploder) { slot = i; break; }
	}

	for (i = 0; i < MOB_MAX; i++) {
		struct Entity* e;
		m = &st_mobs[i];
		if (!m->active || m->health <= 0) continue;
		if (exploder && &m->Base == exploder) continue; /* getEntities excludes var1 */

		e = &m->Base;
		dx = e->Position.x - center.x;
		dy = (e->Position.y + mobTypeInfo[m->type].heightOff) - center.y;
		dz = e->Position.z - center.z;
		dist = Math_SqrtF(dx * dx + dy * dy + dz * dz);
		d    = dist / diam;
		if (d > 1.0f) continue;

		dens = Indev_BlockDensity(center, e);
		f    = (1.0f - d) * dens;
		dmg  = (int)((f * f + f) / 2.0f * 8.0f * diam + 1.0f);

		st_hurtCauseSlot = slot; /* -1 = environmental/TNT, else the creeper */
		Mob_Hurt(m, exploder, dmg, false);
		if (dist > 0.0001f) {
			e->Velocity.x += dx / dist * f;
			e->Velocity.y += dy / dist * f;
			e->Velocity.z += dz / dist * f;
		}
	}

	/* (c) destruction, in genuine reverse-TreeSet order (descending z, y, x):
	    drops roll before the clear; consumed TNT arms a 10-29 tick fuse. */
	for (k = EXPL_DIM - 1; k >= 0; k--)
	for (j = EXPL_DIM - 1; j >= 0; j--)
	for (i = EXPL_DIM - 1; i >= 0; i--) {
		rel = i + j * EXPL_DIM + k * EXPL_DIM * EXPL_DIM;
		if (!(expl_bits[rel >> 3] & (1 << (rel & 7)))) continue;

		bx = cx + i - 16; by = cy + j - 16; bz = cz + k - 16;
		if (!World_Contains(bx, by, bz)) continue;
		id = World_GetBlock(bx, by, bz);
		if (id == BLOCK_AIR) continue;

		coords.x = bx; coords.y = by; coords.z = bz;
		if (id == BLOCK_TNT) {
			Game_UpdateBlock(bx, by, bz, BLOCK_AIR);
			SurvivalTest_ArmTnt(coords, 10 + Random_Next(&st_dropRng, 20));
		} else {
			Indev_ExplodeDrops(coords, id);
			Game_UpdateBlock(bx, by, bz, BLOCK_AIR);
			IndevTest_NotifyBlockRemoved(coords, id);
		}
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
/*  for a Player attacker - so mobs are hurt with playerCredit=false. The */
/*  attacker IS forwarded (genuine hurt(var1, dmg)), so creeper blasts knock */
/*  surviving entities back while TNT (null attacker) doesn't - genuine. */
static void SurvivalTest_Explode(struct Entity* exploder, Vec3 center, float fradius) {
	struct LocalPlayer* p = Entities.CurPlayer;
	struct Mob* m;
	int radius = (int)fradius;
	int exploderSlot = -1, es;

	/* Indev replaces Level.explode entirely with World.createExplosion */
	if (IndevTest_Enabled) { Indev_CreateExplosion(exploder, center, fradius); return; }

	for (es = 0; exploder && es < MOB_MAX; es++) {
		if (&st_mobs[es].Base == exploder) { exploderSlot = es; break; }
	}
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

	/* Indev World.createExplosion opens with random.explode at volume 4 -
	    audible out to 64 blocks - before any block is touched. */
	Indev_PlaySoundAt(center, MOBSND_EXPLODE, 4.0f,
		(1.0f + (Random_Float(&st_mobRng) - Random_Float(&st_mobRng)) * 0.2f) * 0.7f);

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
			{
				int f = TNT_FUSE_DEFAULT(); /* 5..14 c0.30, 10..29 Indev */
				SurvivalTest_ArmTnt(coords, f / 8 + Random_Next(&st_dropRng, f / 4));
			}
		} else {
			SurvivalTest_ExplodeDropsForBlock(coords, block);
			Game_UpdateBlock(xx, yy, zz, BLOCK_AIR);
			/* Explosions remove blocks without raising BlockChanged, so */
			/*  drive the container lifecycle (chest scatter, tile entity */
			/*  destruction) explicitly - else blasted chests silently ate */
			/*  their contents and leaked their tile entity. */
			IndevTest_NotifyBlockRemoved(coords, block);
		}
	}}}

	/* Level.explode: hurt(var1, dmg) - the attacker carries through, so a
	    creeper-attributed blast knocks the player and mobs back through the
	    normal Mob.hurt knockback (TNT's null attacker doesn't - genuine). */
	if (p && (!exploder || exploder != &p->Base) &&
		(dmg = Explosion_Damage(&p->Base, 1.62f, center, inv))) {
		if (exploder) SurvivalTest_HurtFrom(dmg, exploder->Position);
		else          SurvivalTest_Hurt(dmg);
	}

	for (i = 0; i < MOB_MAX; i++) {
		m = &st_mobs[i];
		if (!m->active || m->health <= 0) continue;
		if (exploder && &m->Base == exploder) continue;

		dmg = Explosion_Damage(&m->Base, mobTypeInfo[m->type].heightOff, center, inv);
		if (!dmg) continue;
		st_hurtCauseSlot = exploderSlot; /* -1 = TNT/environment */
		Mob_Hurt(m, exploder, dmg, false);
	}
}

/*########################################################################################################################*
*----------------------------------------------Indev pathfinding (Pathfinder)---------------------------------------------*
*#########################################################################################################################*/
/* A port of level/path/Pathfinder.java: A* over walkable columns, expanding
    the four horizontal neighbours with step-up (1) and drop-down (up to 3)
    handling, capped at 16 blocks from the target. Faithfully preserves the
    genuine passability quirk: getVerticalOffset's box scan reads the LOOP
    START coordinates (a decompile-visible Java bug), so only the corner
    block is actually tested regardless of entity size. */
#define PF_MAX_NODES 900
#define PF_HASH_SIZE 2048
#define PF_PATH_MAX  64

static void Mob_BasicAIUpdate(struct Mob* m, cc_bool inWater, cc_bool inLava);

struct PathNode {
	cc_int16 x, y, z;
	float g, h, f;
	cc_int16 prev, heapIdx;
	cc_uint8 visited, assigned;
};
static struct PathNode pf_nodes[PF_MAX_NODES];
static int      pf_nodeCount;
static cc_int16 pf_heap[PF_MAX_NODES];
static int      pf_heapCount;
static int      pf_hashKey[PF_HASH_SIZE];
static cc_int16 pf_hashVal[PF_HASH_SIZE];

static float PF_Dist(int a, int b) {
	float dx = (float)(pf_nodes[b].x - pf_nodes[a].x);
	float dy = (float)(pf_nodes[b].y - pf_nodes[a].y);
	float dz = (float)(pf_nodes[b].z - pf_nodes[a].z);
	return Math_SqrtF(dx * dx + dy * dy + dz * dz);
}

static int PF_OpenPoint(int x, int y, int z) {
	int key = x | (y << 10) | (z << 20), slot, idx;
	slot = (key * 2654435761u) & (PF_HASH_SIZE - 1);
	for (;;) {
		idx = pf_hashVal[slot];
		if (idx < 0) break;
		if (pf_hashKey[slot] == key) return idx;
		slot = (slot + 1) & (PF_HASH_SIZE - 1);
	}
	if (pf_nodeCount >= PF_MAX_NODES) return -1;

	idx = pf_nodeCount++;
	pf_nodes[idx].x = (cc_int16)x; pf_nodes[idx].y = (cc_int16)y; pf_nodes[idx].z = (cc_int16)z;
	pf_nodes[idx].g = 0.0f; pf_nodes[idx].h = 0.0f; pf_nodes[idx].f = 0.0f;
	pf_nodes[idx].prev = -1; pf_nodes[idx].heapIdx = -1;
	pf_nodes[idx].visited = 0; pf_nodes[idx].assigned = 0;
	pf_hashKey[slot] = key; pf_hashVal[slot] = (cc_int16)idx;
	return idx;
}

static void PF_HeapSiftUp(int i) {
	cc_int16 n = pf_heap[i];
	while (i > 0) {
		int parent = (i - 1) >> 1;
		if (pf_nodes[pf_heap[parent]].f <= pf_nodes[n].f) break;
		pf_heap[i] = pf_heap[parent]; pf_nodes[pf_heap[i]].heapIdx = (cc_int16)i;
		i = parent;
	}
	pf_heap[i] = n; pf_nodes[n].heapIdx = (cc_int16)i;
}

static void PF_HeapSiftDown(int i) {
	cc_int16 n = pf_heap[i];
	for (;;) {
		int child = i * 2 + 1;
		if (child >= pf_heapCount) break;
		if (child + 1 < pf_heapCount && pf_nodes[pf_heap[child + 1]].f < pf_nodes[pf_heap[child]].f) child++;
		if (pf_nodes[pf_heap[child]].f >= pf_nodes[n].f) break;
		pf_heap[i] = pf_heap[child]; pf_nodes[pf_heap[i]].heapIdx = (cc_int16)i;
		i = child;
	}
	pf_heap[i] = n; pf_nodes[n].heapIdx = (cc_int16)i;
}

static void PF_HeapPush(int idx) {
	pf_heap[pf_heapCount] = (cc_int16)idx;
	pf_nodes[idx].heapIdx = (cc_int16)pf_heapCount;
	pf_heapCount++;
	PF_HeapSiftUp(pf_heapCount - 1);
}

static int PF_HeapPop(void) {
	int top = pf_heap[0];
	pf_nodes[top].heapIdx = -1;
	pf_heapCount--;
	if (pf_heapCount > 0) {
		pf_heap[0] = pf_heap[pf_heapCount];
		pf_nodes[pf_heap[0]].heapIdx = 0;
		PF_HeapSiftDown(0);
	}
	return top;
}

/* getVerticalOffset (single-block, per the genuine quirk): 1 = passable, */
/*  0 = solid/out of bounds, -1 = liquid */
static int PF_VerticalOffset(int x, int y, int z) {
	BlockID b;
	if (!World_Contains(x, y, z)) return 0;
	b = World_GetBlock(x, y, z);
	if (Blocks.Collide[b] == COLLIDE_SOLID)  return 0;
	if (Blocks.Collide[b] == COLLIDE_LIQUID) return -1;
	return 1;
}

/* getSafePoint: passable here (or one step up), then drop down onto solid */
/*  ground (at most 3 blocks; landing next to water/lava is rejected). */
static int PF_GetSafePoint(int x, int y, int z, int stepUp) {
	int idx = -1, fall = 0, off;
	BlockID below;

	if (PF_VerticalOffset(x, y, z) > 0) {
		idx = PF_OpenPoint(x, y, z);
	} else if (stepUp && PF_VerticalOffset(x, y + 1, z) > 0) {
		y++; idx = PF_OpenPoint(x, y, z);
	}
	if (idx < 0) return -1;

	while (y > 0) {
		off = PF_VerticalOffset(x, y - 1, z);
		if (off <= 0) break;
		fall++;
		if (fall >= 4) return -1;
		y--;
		idx = PF_OpenPoint(x, y, z);
		if (idx < 0) return -1;
	}
	below = (y > 0 && World_Contains(x, y - 1, z)) ? World_GetBlock(x, y - 1, z) : BLOCK_AIR;
	if (Blocks.Collide[below] == COLLIDE_LIQUID) return -1;
	return idx;
}

/* Pathfinder.addToPath: A* from the mob to (tx,ty,tz), 16-block range cap. */
/*  Falls back to the best-effort nearest node when the target is */
/*  unreachable (genuine returns the closest-explored partial path). */
static cc_bool Mob_FindPath(struct Mob* m, float tx, float ty, float tz) {
	struct AABB bb;
	int start, target, node, best, count, i, stepUp;
	int nb[4], nbCount;
	float ng;

	Mem_Set(pf_hashVal, 0xFF, sizeof(pf_hashVal)); /* -1 fill */
	pf_nodeCount = 0; pf_heapCount = 0;
	m->pathCount = 0; m->pathIndex = 0;

	Entity_GetBounds(&m->Base, &bb);
	start  = PF_OpenPoint(Math_Floor(bb.Min.x), Math_Floor(bb.Min.y), Math_Floor(bb.Min.z));
	target = PF_OpenPoint(Math_Floor(tx - m->Base.Size.x / 2.0f), Math_Floor(ty),
						  Math_Floor(tz - m->Base.Size.x / 2.0f));
	if (start < 0 || target < 0) return false;

	pf_nodes[start].g = 0.0f;
	pf_nodes[start].h = PF_Dist(start, target);
	pf_nodes[start].f = pf_nodes[start].h;
	pf_nodes[start].assigned = 1;
	PF_HeapPush(start);
	best = start;

	while (pf_heapCount > 0) {
		node = PF_HeapPop();
		if (node == target) { best = target; break; }
		if (PF_Dist(node, target) < PF_Dist(best, target)) best = node;
		pf_nodes[node].visited = 1;

		stepUp = PF_VerticalOffset(pf_nodes[node].x, pf_nodes[node].y + 1, pf_nodes[node].z) > 0 ? 1 : 0;
		{
			int nx = pf_nodes[node].x, ny = pf_nodes[node].y, nz = pf_nodes[node].z;
			nbCount = 0;
			nb[nbCount++] = PF_GetSafePoint(nx,     ny, nz + 1, stepUp);
			nb[nbCount++] = PF_GetSafePoint(nx - 1, ny, nz,     stepUp);
			nb[nbCount++] = PF_GetSafePoint(nx + 1, ny, nz,     stepUp);
			nb[nbCount++] = PF_GetSafePoint(nx,     ny, nz - 1, stepUp);
		}

		for (i = 0; i < nbCount; i++) {
			int n2 = nb[i];
			if (n2 < 0 || pf_nodes[n2].visited) continue;
			if (PF_Dist(n2, target) >= 16.0f)   continue; /* range cap */

			ng = pf_nodes[node].g + PF_Dist(node, n2);
			if (pf_nodes[n2].assigned && ng >= pf_nodes[n2].g) continue;

			pf_nodes[n2].prev = (cc_int16)node;
			pf_nodes[n2].g = ng;
			pf_nodes[n2].h = PF_Dist(n2, target);
			pf_nodes[n2].f = ng + pf_nodes[n2].h;
			if (pf_nodes[n2].assigned) {
				if (pf_nodes[n2].heapIdx >= 0) PF_HeapSiftUp(pf_nodes[n2].heapIdx);
			} else {
				pf_nodes[n2].assigned = 1;
				PF_HeapPush(n2);
			}
		}
	}

	if (best == start) return false;

	/* walk the prev chain and reverse it into the mob's waypoint list */
	count = 0;
	for (node = best; node >= 0; node = pf_nodes[node].prev) count++;
	if (count > PF_PATH_MAX) return false;

	m->pathCount = (cc_uint8)count;
	i = count - 1;
	for (node = best; node >= 0; node = pf_nodes[node].prev, i--) {
		m->pathX[i] = pf_nodes[node].x;
		m->pathY[i] = pf_nodes[node].y;
		m->pathZ[i] = pf_nodes[node].z;
	}
	return true;
}

static cc_bool Mob_SightBlocked(Vec3 from, Vec3 to);

/* EntityMob.attackEntity and its per-type overrides. Returns hasAttacked -
    which only the skeleton's bow shot and the creeper's swelling set, so
    melee mobs keep running at their victim mid-swing, exactly like the
    original. te is the target entity, tm its mob struct (NULL = player). */
static cc_bool Mob_IndevAttackEntity(struct Mob* m, struct Entity* te, struct Mob* tm, float dist) {
	struct Entity* e = &m->Base;
	struct AABB mb, tb;
	float dx, dz, hor;
	int damage;

	if (mobTypeInfo[m->type].isCreeper) {
		/* EntityCreeper.attackEntity: the fuse. Starts within 3 blocks, keeps
		    burning within 7 once lit, blows at 30 ticks. */
		if ((m->fuseState <= 0 && dist < 3.0f) || (m->fuseState > 0 && dist < 7.0f)) {
			if (m->fuseTicks == 0)
				Mob_PlaySound(m, MOBSND_FUSE, 1.0f, 0.5f);
			m->fuseState = 1;
			m->fuseTicks++;
			if (m->fuseTicks >= 30) Mob_IndevCreeperBlast(m);
			return true;
		}
		return false;
	}

	if (m->type == MOB_TYPE_SKELETON) {
		/* EntitySkeleton.attackEntity: bow fire within 10 blocks, 30-tick
		    cooldown, always turning to face the victim and standing still. */
		if (dist >= 10.0f) return false;
		if (m->attackDelay == 0) {
			Mob_IndevShootArrow(m, te);
			m->attackDelay = 30;
		}
		dx = te->Position.x - e->Position.x;
		dz = te->Position.z - e->Position.z;
		e->Yaw = Math_Atan2f(-dz, dx) * MATH_RAD2DEG;
		return true;
	}

	if (m->type == MOB_TYPE_SPIDER) {
		/* EntitySpider.attackEntity: standing in light gives a 1-in-100 per
		    tick chance to lose interest entirely; from 2-6 blocks out a
		    1-in-10 roll pounces instead of closing (grounded only). */
		if (Mob_Brightness(m) > 0.5f && Random_Next(&st_mobRng, 100) == 0) {
			m->hasTarget = false; m->targetSlot = -1;
			m->pathCount = 0;
			return false;
		}
		if (dist > 2.0f && dist < 6.0f && Random_Next(&st_mobRng, 10) == 0) {
			if (e->OnGround) {
				dx  = te->Position.x - e->Position.x;
				dz  = te->Position.z - e->Position.z;
				hor = Math_SqrtF(dx * dx + dz * dz);
				e->Velocity.x = dx / hor * 0.5f * 0.8f + e->Velocity.x * 0.2f;
				e->Velocity.z = dz / hor * 0.5f * 0.8f + e->Velocity.z * 0.2f;
				e->Velocity.y = 0.4f;
			}
			return false;
		}
		/* otherwise the shared melee below */
	}

	/* EntityMob.attackEntity: melee lands within 2.5 blocks when the bounding
	    boxes overlap vertically. Flat per-type strength (zombie 5, everything
	    else the EntityMob default 2), no damage roll. */
	/* Genuine has no attacker-side gate (it re-arms its 20-tick swing timer
	    every tick and lets the victim's invulnerability absorb the spam) -
	    but our renderer's 5-tick swing restarting every tick made the arms
	    flail wildly. Gating on a 10-tick attackDelay keeps the same landed-
	    damage cadence (the victim's invuln half-window) while the swing
	    animates cleanly once per attempt. */
	if (dist >= 2.5f) return false;
	if (m->attackDelay > 0) return false;
	Entity_GetBounds(e,  &mb);
	Entity_GetBounds(te, &tb);
	if (tb.Max.y <= mb.Min.y || tb.Min.y >= mb.Max.y) return false;

	m->attackDelay  = 10;
	m->attackTime   = 5; /* the render-side arm-swing timer */
	m->noActionTime = 0;
	damage = m->type == MOB_TYPE_ZOMBIE ? 5 : 2;
	if (tm) {
		st_hurtCauseSlot = (int)(m - st_mobs);
		Mob_Hurt(tm, e, damage, false);
	} else {
		SurvivalTest_HurtFrom(damage, e->Position);
	}
	return false;
}

/* EntityCreature.updatePlayerActionState: resolve/acquire a target, attack it
    when in sight, then path toward it (re-pathing at 1-in-20 per tick) or
    wander to the best of 200 weighted random points (monsters prefer darkness,
    animals grass), steering along the A* waypoints. */
static void Mob_IndevCreatureAI(struct Mob* m, cc_bool inWater, cc_bool inLava) {
	struct Entity* e = &m->Base;
	struct LocalPlayer* p = Entities.CurPlayer;
	struct Entity* te = NULL; /* current target entity (player or mob) */
	struct Mob*    tm = NULL;
	float dx, dy, dz, distSq;
	cc_bool wantWander, hasAttacked = false;

	/* resolve the existing target, dropping dead/removed ones */
	if (m->hasTarget) {
		if (m->targetSlot >= 0) {
			tm = &st_mobs[(int)m->targetSlot];
			if (!tm->active || tm->health <= 0) {
				m->hasTarget = false; m->targetSlot = -1; tm = NULL;
			} else {
				te = &tm->Base;
			}
		} else if (p && SurvivalTest_Health > 0) {
			te = &p->Base;
		} else {
			m->hasTarget = false;
		}
	}

	if (!m->hasTarget) {
		/* findPlayerToAttack: monsters aggro on the player within 16 blocks;
		    EntitySpider's override only hunts while ITS OWN spot is dark
		    (brightness < 0.5). Passives never acquire by proximity. */
		cc_bool canHunt = mobTypeInfo[m->type].ai != MOB_AI_PASSIVE &&
			!(m->type == MOB_TYPE_SPIDER && Mob_Brightness(m) >= 0.5f);
		if (canHunt && p && SurvivalTest_Health > 0) {
			dx = p->Base.Position.x - e->Position.x;
			dy = p->Base.Position.y - e->Position.y;
			dz = p->Base.Position.z - e->Position.z;
			if (dx * dx + dy * dy + dz * dz < 256.0f) {
				m->hasTarget  = true;
				m->targetSlot = -1;
				te = &p->Base;
				Mob_FindPath(m, te->Position.x, te->Position.y, te->Position.z);
			}
		}
	} else if (te) {
		/* attack whenever nothing solid sits between the two eye points */
		Vec3 me = Entity_GetEyePosition(e);
		Vec3 pe = Entity_GetEyePosition(te);
		dx = te->Position.x - e->Position.x;
		dy = te->Position.y - e->Position.y;
		dz = te->Position.z - e->Position.z;
		distSq = dx * dx + dy * dy + dz * dz;
		if (!Mob_SightBlocked(me, pe))
			hasAttacked = Mob_IndevAttackEntity(m, te, tm, Math_SqrtF(distSq));
	}

	if (hasAttacked) {
		/* a shooting/swelling mob stands its ground this tick */
		m->moveStrafe  = 0.0f;
		m->moveForward = 0.0f;
		m->jumping     = false;
		return;
	}

	wantWander = !m->hasTarget || (m->pathCount > 0 && Random_Next(&st_mobRng, 20) != 0);
	if (wantWander) {
		if (m->pathCount == 0 || Random_Next(&st_mobRng, 100) == 0) {
			/* best of 200 random points by getBlockPathWeight */
			int bx = -1, by = -1, bz = -1, t, cx, cy, cz;
			float bestW = -99999.0f, w;
			for (t = 0; t < 200; t++) {
				cx = (int)(e->Position.x + (float)(Random_Next(&st_mobRng, 21) - 10));
				cy = (int)(e->Position.y + (float)(Random_Next(&st_mobRng, 9)  - 4));
				cz = (int)(e->Position.z + (float)(Random_Next(&st_mobRng, 21) - 10));
				if (mobTypeInfo[m->type].ai == MOB_AI_PASSIVE) {
					/* EntityAnimal.getBlockPathWeight: a grass block below is
					    worth 10, anything else its brightness - 0.5 */
					w = World_Contains(cx, cy - 1, cz) &&
						World_GetBlock(cx, cy - 1, cz) == BLOCK_GRASS
						? 10.0f : Indev_LightBrightness(cx, cy, cz) - 0.5f;
				} else {
					/* EntityMob: 0.5 - brightness (prefer the dark) */
					w = 0.5f - Indev_LightBrightness(cx, cy, cz);
				}
				if (w > bestW) { bestW = w; bx = cx; by = cy; bz = cz; }
			}
			if (bx > 0) Mob_FindPath(m, bx + 0.5f, by + 0.5f, bz + 0.5f);
		}
	} else if (te) {
		Mob_FindPath(m, te->Position.x, te->Position.y, te->Position.z);
	}

	if (m->pathCount > 0 && Random_Next(&st_mobRng, 100) != 0) {
		float half = e->Size.x + 1.0f, wx, wy, wz;
		cc_bool hasPoint = true;

		/* advance past waypoints we're standing on (within width*2) */
		for (;;) {
			if (m->pathIndex >= m->pathCount) { hasPoint = false; m->pathCount = 0; break; }
			wx = m->pathX[m->pathIndex] + (float)((int)half) * 0.5f;
			wy = (float)m->pathY[m->pathIndex];
			wz = m->pathZ[m->pathIndex] + (float)((int)half) * 0.5f;

			dx = e->Position.x - wx; dy = e->Position.y - wy; dz = e->Position.z - wz;
			distSq = dx * dx + dy * dy + dz * dz;
			if (distSq >= e->Size.x * 2.0f * e->Size.x * 2.0f || wy > e->Position.y) break;
			m->pathIndex++;
		}

		m->jumping = false;
		if (hasPoint) {
			dx = wx - e->Position.x;
			dz = wz - e->Position.z;
			dy = wy - e->Position.y;
			/* face the waypoint (same yaw convention as Mob_DoAttack) */
			e->Yaw = Math_Atan2f(-dz, dx) * MATH_RAD2DEG;
			m->moveForward = Mob_IndevMoveSpeed(m->type);
			if (dy > 0.0f) m->jumping = true;
		}
		if ((inWater || inLava) && Random_Float(&st_mobRng) < 0.8f) m->jumping = true;
		m->moveStrafe = 0.0f;
	} else {
		/* no path: EntityLiving's random wandering (our BasicAI) */
		m->pathCount = 0;
		Mob_BasicAIUpdate(m, inWater, inLava);
	}
}

/* The Indev per-tick AI entry point. EntityCreeper.updatePlayerActionState
    wraps the shared creature logic in fuse bookkeeping: the swell winds down
    while idle (timeSinceIgnited--), and creeperState falls back to -1 unless
    attackEntity re-armed it to 1 this very tick. */
static void Mob_IndevCreatureUpdate(struct Mob* m, cc_bool inWater, cc_bool inLava) {
	if (mobTypeInfo[m->type].isCreeper) {
		m->fuseLast = m->fuseTicks;
		if (m->fuseTicks > 0 && m->fuseState < 0) m->fuseTicks--;
		if (m->fuseState >= 0) m->fuseState = 2;
	}
	Mob_IndevCreatureAI(m, inWater, inLava);
	if (mobTypeInfo[m->type].isCreeper && m->fuseState != 1) m->fuseState = -1;
}

/* BasicAI.update() - the shared wander/turn logic used by every mob, plus */
/*  the chase override applied once a mob has acquired a target (only ever */
/*  true for attack-type mobs - BasicAttackAI is what actually sets hasTarget). */
static void Mob_BasicAIUpdate(struct Mob* m, cc_bool inWater, cc_bool inLava) {
	const struct MobTypeInfo* info = &mobTypeInfo[m->type];
	struct Entity* e = &m->Base;
	/* In Indev mode this only ever runs as EntityLiving's default action
	    state (the pathless fallback), whose random impulses scale by the
	    Indev moveSpeed - not by c0.30's differently-tuned runSpeed. */
	float speed = IndevTest_Enabled ? Mob_IndevMoveSpeed(m->type) : info->runSpeed;

	if (Random_Next(&st_mobRng, 100) < 7) {
		m->moveStrafe  = (Random_Float(&st_mobRng) - 0.5f) * speed;
		m->moveForward =  Random_Float(&st_mobRng)         * speed;
	}
	m->jumping = Random_Next(&st_mobRng, 100) < 1;

	if (Random_Next(&st_mobRng, 100) < 4) {
		m->turnRate = (Random_Float(&st_mobRng) - 0.5f) * 60.0f;
	}
	e->Yaw  += m->turnRate;
	/* Indev EntityLiving.updatePlayerActionState pins rotationPitch to 0 -
	    the c0.30 per-type defaultLookAngle (zombie 30 degrees down) is what
	    made Indev mobs stare at the player's feet. */
	e->Pitch = IndevTest_Enabled ? 0.0f : info->defaultLookAngle;

	/* c0.30 BasicAI.update's target branch: a mob with a target walks forward
	    (BasicAttackAI.doAttack, which runs right after, then turns yRot to face
	    the target - so the two together make it stride toward its victim).
	    Indev has NO equivalent here: this routine stands in for EntityLiving's
	    updatePlayerActionState, the PATHLESS fallback of the A* creature AI,
	    which is pure random wander (EntityCreature already handled the target +
	    attack up in Mob_IndevCreatureAI). Forcing forward here with no matching
	    yaw correction is what made Indev monsters barrel off in the wander
	    direction instead of chasing - "fighting their own AI". */
	if (m->hasTarget && !IndevTest_Enabled) {
		m->moveForward = speed;
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
					Mob_ApplySize(m); /* SetModel resets Size from the model */
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
	/* BasicAttackAI.doAttack's pitch is xRot = -atan2(dy, dist3d): the */
	/*  adjacent is the full 3D distance (not the horizontal one), a mild */
	/*  genuine quirk that slightly under-pitches. Skeleton arrow aim reads */
	/*  e->Pitch off this. */
	horDist  = Math_SqrtF(distSq);
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

	/* Wandered off a floating island into the bottomless void - despawn the */
	/*  slot rather than leave it falling forever (genuine mobs just vanish). */
	if (e->Position.y < ST_VOID_KILL_Y) { m->active = false; return; }

	if (m->invincTicks > 0) m->invincTicks--;
	if (m->hurtTicks   > 0) m->hurtTicks--;
	/* Mob.tick decrements attackTime before the AI runs, so a hit landed this */
	/*  tick (Mob_DoAttack below) leaves it freshly reset to 5 for the swing. */
	if (m->attackTime  > 0) m->attackTime--;
	m->ticksAlive++; /* Mob.tick's this.tickCount++ */

	if (m->health <= 0) {
		m->deathTicks++;
		if (m->deathTicks > 20) {
			/* Both are c0.30 behaviours: the Indev creeper only explodes from
			    its own fuse (a killed one just drops gunpowder), and the Indev
			    skeleton drops 0-2 arrow ITEMS via onDeath instead of the c0.30
			    pickupable-arrow burst. */
			if (info->isCreeper && !IndevTest_Enabled)              Mob_CreeperExplode(m);
			if (m->type == MOB_TYPE_SKELETON && !IndevTest_Enabled) Mob_SkeletonDeathBurst(m);
			m->active = false;
			return;
		}
	}

	inWater = ST_InLiquid(e, false);
	inLava  = ST_InLiquid(e, true);

	/* Entity.onEntityUpdate: falling into water splashes (Indev layer) */
	if (IndevTest_Enabled) {
		if (inWater && !m->wasInWater)
			Indev_EntitySplash(e->Position, e->Velocity, e->Size.x);
		m->wasInWater = inWater;
	}

	/* Environmental damage - Mob.tick()'s airSupply/lava handling. Both */
	/*  damage calls go through the same flat invincibility window as combat */
	/*  damage (see Mob_Hurt), so e.g. lava only actually ticks roughly once */
	/*  per second rather than truly every tick. */
	if (SurvivalTest_IsHeadInWater(e)) {
		if (IndevTest_Enabled) {
			/* EntityLiving.onEntityUpdate: --air; the -20 underflow IS the
			    damage timer - at exactly -20, 8 bubbles + 2 damage and reset
			    to 0. First hit 320 ticks after submerging, then every 20. */
			m->airTicks--;
			if (m->airTicks == -20) {
				Vec3 eye = Entity_GetEyePosition(e);
				int b;
				m->airTicks = 0;
				for (b = 0; b < 8; b++) {
					Indev_SpawnBubbleFX(
						e->Position.x + (Random_Float(&st_mobRng) - Random_Float(&st_mobRng)),
						eye.y         + (Random_Float(&st_mobRng) - Random_Float(&st_mobRng)),
						e->Position.z + (Random_Float(&st_mobRng) - Random_Float(&st_mobRng)),
						e->Velocity.x, e->Velocity.y, e->Velocity.z);
				}
				Mob_Hurt(m, NULL, 2, false);
			}
		} else {
			if (m->airTicks > 0) { m->airTicks--; }
			else                 { Mob_Hurt(m, NULL, 2, false); }
		}
	} else {
		m->airTicks = MOB_AIR_TICKS;
	}
	if (inLava) Mob_Hurt(m, NULL, 10, false);

	/* Entity.onEntityUpdate's fire handling (Indev layer): water puts a
	    burning mob out with a fizz, otherwise fire deals 1 HP every 20 ticks
	    while counting down, and lava contact re-arms it to 600. */
	if (IndevTest_Enabled) {
		if (inWater && m->fire > 0) {
			Mob_PlaySound(m, MOBSND_FIZZ, 0.7f,
				1.6f + (Random_Float(&st_mobRng) - Random_Float(&st_mobRng)) * 0.4f);
			m->fire = 0;
		}
		if (m->fire > 0) {
			if (m->fire % 20 == 0) Mob_Hurt(m, NULL, 1, false);
			m->fire--;
		}
		if (inLava) m->fire = 600;
		/* standing in a fire block sets mobs alight too (Entity.move's
		    isBoundingBoxBurning path; the 1 HP/sec burn above does the
		    damage once lit) */
		if (m->fire <= 0 && !inWater && ST_InFire(&m->Base)) m->fire = 300;

		/* EntityZombie/EntitySkeleton.onLivingUpdate: daylight sets them on
		    fire - sky light over 7 (daytime), bright spot, open sky overhead,
		    then a rand*30 < (brightness-0.4)*2 roll (~4%/tick in full sun).
		    Runs before the AI branch like the genuine onLivingUpdate (so the
		    debug No-AI freeze doesn't shield them from the sun). */
		if (m->health > 0 &&
			(m->type == MOB_TYPE_ZOMBIE || m->type == MOB_TYPE_SKELETON) &&
			IndevTest_CurSkyLight() > 7) {
			/* classic Level.isLit treats out-of-bounds as lit; the engine's
			    IsLit does NO bounds checks, so guard before indexing its
			    heightmap (mobs can wander off the map edge) */
			int mx = Math_Floor(e->Position.x), my = Math_Floor(e->Position.y), mz = Math_Floor(e->Position.z);
			float mb = Mob_Brightness(m);
			if (mb > 0.5f &&
				(!World_Contains(mx, my, mz) || Lighting.IsLit(mx, my, mz)) &&
				Random_Float(&st_mobRng) * 30.0f < (mb - 0.4f) * 2.0f) {
				m->fire = 300;
			}
		}

		/* EntityLiving.onEntityUpdate's ambient-sound roll. Every living
		    entity runs it, but in in-20100223 only the pig and sheep actually
		    return a living sound - monsters were still silent. */
		if (m->health > 0 && Random_Next(&st_mobRng, 1000) < m->livingSnd++) {
			m->livingSnd = -80;
			if (m->type == MOB_TYPE_PIG)   Mob_PlaySound(m, MOBSND_PIG,   1.0f, Mob_SndPitch());
			if (m->type == MOB_TYPE_SHEEP) Mob_PlaySound(m, MOBSND_SHEEP, 1.0f, Mob_SndPitch());
		}
	}

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
		/* EntityMob.onLivingUpdate: monsters in bright light (entity brightness */
		/*  over 0.5, which the genuine curve only reaches at light level 12+) */
		/*  age twice as fast toward the despawn roll. */
		if (IndevTest_Enabled && mobTypeInfo[m->type].ai != MOB_AI_PASSIVE &&
			Mob_Brightness(m) > 0.5f) {
			m->noActionTime += 2;
		}
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

		if (IndevTest_Enabled) {
			/* Indev: EntityCreature pathfinding + per-type attackEntity, all
			    inside updatePlayerActionState. Sheep included - the Indev sheep
			    is a plain EntityAnimal wanderer (no c0.30 grass-eating). */
			Mob_IndevCreatureUpdate(m, inWater, inLava);
		} else {
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
			/* Block.onEntityWalking: mobs trample farmland (1-in-4) on the
			    same step trigger, regardless of player distance */
			if (under != BLOCK_AIR) {
				IndevTest_TrampleStep(e->Position.x, e->Position.y, e->Position.z);
			}
			if (under != BLOCK_AIR && sp) {
				/* Heard with a distance-attenuated volume, like the genuine
				    playSound path: Indev playSoundAtEntity's audible range is
				    16 blocks (16*vol for loud sounds; footsteps are quiet so
				    16), c0.30 Level.playSound's is 32 (distanceToSqr<1024),
				    and both fall the volume off linearly to 0 at that range
				    (BaseSoundPos: 1 - dist/32). Was a flat 32-block horizontal
				    cutoff at full volume - too far AND too loud in Indev. */
				float range = IndevTest_Enabled ? 16.0f : 32.0f;
				float ddy   = e->Position.y - sp->Base.Position.y;
				float dist;
				sdx  = e->Position.x - sp->Base.Position.x;
				sdz  = e->Position.z - sp->Base.Position.z;
				dist = Math_SqrtF(sdx * sdx + ddy * ddy + sdz * sdz);
				if (dist < range)
					Audio_PlayStepSoundAt(Blocks.StepSounds[under], 1.0f - dist / range);
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
				/* EntityLiving.fall landing thud, mobs too (Indev only) */
				if (IndevTest_Enabled && damage > 0) {
					int bx = Math_Floor(e->Position.x);
					int by = Math_Floor(e->Position.y - 0.2f);
					int bz = Math_Floor(e->Position.z);
					if (World_Contains(bx, by, bz))
						Audio_PlayFallSound(Blocks.StepSounds[World_GetBlock(bx, by, bz)]);
				}
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
	/* Genuine World.getBlockId CLAMPS out-of-bounds coords to the edge block
	    (it does NOT treat the void as solid). The spawner's "ground below"
	    test therefore reads getBlockId(x, y-1, z) with y-1 = -1 CLAMPED to
	    y = 0 - so a spawn only sticks at the very bottom when the y=0 row is
	    itself a solid cube. In a floating world y=0 is air, which is exactly
	    why monsters do NOT carpet the void floor in genuine Indev. Returning
	    `true` for below-world (the old behaviour) faked solid ground at y=-1
	    and let the dark void spawn endless monsters onto the bottom. */
	if (x < 0) x = 0; else if (x >= World.Width)  x = World.Width  - 1;
	if (y < 0) y = 0; else if (y >= World.Height) y = World.Height - 1;
	if (z < 0) z = 0; else if (z >= World.Length) z = World.Length - 1;
	return Blocks.Collide[World_GetBlock(x, y, z)] == COLLIDE_SOLID;
}

/* World.isBlockNormalCube = Block.isOpaqueCube() - the INDEV spawner's ground
    test (MobSpawner.performSpawning gates on isBlockNormalCube(x, y-1, z)).
    Unlike c0.30's isSolidTile (any solid tile, which our Mob_SpawnerRun keeps),
    Indev requires a FULL OPAQUE cube beneath the spawn: leaves, glass and slabs
    are solid-collidable but NOT opaque cubes, so genuine monsters/animals never
    perch on tree canopies or glass. Blocks.FullOpaque is exactly DRAW_OPAQUE
    full cubes, matching isOpaqueCube. Clamps out-of-bounds like getBlockId. */
static cc_bool Mob_BlockIsNormalCube(int x, int y, int z) {
	if (x < 0) x = 0; else if (x >= World.Width)  x = World.Width  - 1;
	if (y < 0) y = 0; else if (y >= World.Height) y = World.Height - 1;
	if (z < 0) z = 0; else if (z >= World.Length) z = World.Length - 1;
	return Blocks.FullOpaque[World_GetBlock(x, y, z)];
}

/* Level.isFree's per-block test: any solid OR liquid block occupying the */
/*  candidate mob's bounding box vetoes the spawn. */
static cc_bool Mob_SpawnBlockedBy(BlockID b) {
	return Blocks.Collide[b] == COLLIDE_SOLID || Blocks.Collide[b] == COLLIDE_LIQUID;
}

/* Duplicates the private Entity_GetColor (lighting at the entity's eye */
/*  position) since mobs aren't real Entities.List[] entries. The hit-flash */
/*  overlays are separate render passes (see RenderMobs), NOT colour blends: */
/*  c0.30 flashes additive white, Indev overlays translucent red - neither */
/*  ground truth tints the base model itself. */
/* Set while RenderMobs is drawing a hit-flash overlay pass, so the model */
/*  VTABLE colour below returns that pass's flat colour instead. */
static cc_bool   st_mobFlashPass;
static PackedCol st_mobFlashCol;

/* 4x4 solid white, substituted for the skin during the Indev hurt-overlay
    pass - reproduces genuine RenderLiving's glDisable(GL_TEXTURE_2D) flat
    draw without leaving the engine's textured model pipeline. */
static GfxResourceID st_mobWhiteTex;
static GfxResourceID Mob_WhiteTex(void) {
	static BitmapCol pixels[4 * 4];
	struct Bitmap bmp;
	int i;
	if (st_mobWhiteTex) return st_mobWhiteTex;

	for (i = 0; i < 4 * 4; i++) pixels[i] = BITMAPCOLOR_WHITE;
	Bitmap_Init(bmp, 4, 4, pixels);
	st_mobWhiteTex = Gfx_CreateTexture(&bmp, 0, false);
	return st_mobWhiteTex;
}

static PackedCol Mob_GetColor(struct Entity* e) {
	struct Mob* m = (struct Mob*)e; /* Base is the first field of struct Mob */
	if (st_mobFlashPass) return st_mobFlashCol;
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
	if (mobTypeInfo[m->type].isCreeper && !IndevTest_Enabled) {
		/* c0.30 only: Indev's EntityCreeper has no getBrightness override -
		    it renders at normal brightness (its fuse blink is an overlay) */
		float hurt  = (20 - m->health) / 20.0f;
		float pulse = (Math_SinF((float)m->ticksAlive) * 0.5f + 0.5f) * hurt * 0.5f
		            + 0.25f + hurt * 0.25f;
		col = PackedCol_Scale(col, min(pulse, 1.0f));
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
	m->fuseState   = -1; /* EntityCreeper.creeperState idles at -1, not 0 */
	m->wasInWater  = true; /* Entity.isFirstUpdate: never splash on the spawn tick */

	model = String_FromReadonly(mobTypeInfo[type].model);
	Entity_SetModel(&m->Base, &model);
	m->type = type;
	Mob_ApplySize(m); /* genuine setSize - the model's collision box is off */

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
	/* EntityLiving defaults to 10 HP; only EntityMob raises it to 20 - so
	    Indev pigs/sheep have 10. c0.30 mobs are a flat 20. */
	m->health   = (IndevTest_Enabled &&
	               (type == MOB_TYPE_PIG || type == MOB_TYPE_SHEEP)) ? 10 : MOB_MAX_HEALTH;
	m->airTicks = MOB_AIR_TICKS;
	m->active   = true;
	m->hasFur   = true; /* irrelevant for non-sheep, but harmless */

	/* HumanoidMob's `helmet = Math.random() < 0.2`/`armor = Math.random() < 0.2` */
	/*  field initialisers - only zombies/skeletons extend HumanoidMob, so every */
	/*  other type is faithfully left with neither (pigs/sheep/creepers/spiders */
	/*  have no arms/head shaped to wear plate on in the original anyway). */
	/*  c0.30-ONLY: in-20100223's EntityZombie/EntitySkeleton have no such */
	/*  fields and RenderLiving has no plate pass, so Indev natural spawns */
	/*  are never armored (MobSpawner.java assigns nothing either). */
	if (!IndevTest_Enabled && (type == MOB_TYPE_ZOMBIE || type == MOB_TYPE_SKELETON)) {
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

		type = (cc_uint8)Random_Next(&st_mobRng, MOB_SPAWN_COUNT);
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

/* Indev MobSpawner.performSpawning: runs EVERY tick. Monsters spawn only
    in darkness (light <= rand(8)), at least 32 blocks from the player, up
    to a difficulty-scaled cap; animals need light > 8 up to their own cap.
    The genuine type roll is nextInt(5) with index 4 spawning NOTHING, and
    Y is biased toward the depths (min of two uniforms). */
static cc_bool Mob_IndevIsMonster(cc_uint8 type) {
	return type == MOB_TYPE_ZOMBIE || type == MOB_TYPE_SKELETON ||
	       type == MOB_TYPE_CREEPER || type == MOB_TYPE_SPIDER;
}

static void Mob_IndevCountKinds(int* monsters, int* animals) {
	int i;
	*monsters = 0; *animals = 0;
	for (i = 0; i < MOB_MAX; i++) {
		if (!st_mobs[i].active) continue;
		if (Mob_IndevIsMonster(st_mobs[i].type)) (*monsters)++;
		else (*animals)++;
	}
}

static void Mob_IndevSpawnPass(cc_bool monsters, int cap, int current) {
	/* the monster type table for nextInt(5): 4 = no spawn (genuine) */
	static const cc_int8 monsterRoll[5] = {
		MOB_TYPE_SKELETON, MOB_TYPE_CREEPER, MOB_TYPE_SPIDER, MOB_TYPE_ZOMBIE, -1
	};
	struct LocalPlayer* p = Entities.CurPlayer;
	int attempt, outer, inner, roll;
	int x, y, z, cx, cy, cz, light;
	cc_int8 type;
	Vec3 candidate;
	float r1, r2, dx, dy, dz;

	for (attempt = 0; attempt < 4; attempt++) {
		if (current >= cap) return;

		if (monsters) {
			roll = Random_Next(&st_mobRng, 5);
			type = monsterRoll[roll];
			if (type < 0) continue;
		} else {
			type = Random_Next(&st_mobRng, 2) ? MOB_TYPE_PIG : MOB_TYPE_SHEEP;
		}

		x  = Random_Next(&st_mobRng, World.Width);
		if (monsters) {
			/* the depth bias (min of two uniforms) is the MONSTER pass only */
			r1 = Random_Float(&st_mobRng); r2 = Random_Float(&st_mobRng);
			y  = (int)((r1 < r2 ? r1 : r2) * World.Height);
		} else {
			y  = Random_Next(&st_mobRng, World.Height);
		}
		z  = Random_Next(&st_mobRng, World.Length);

		for (outer = 0; outer < 2; outer++) {
			cx = x; cy = y; cz = z;
			for (inner = 0; inner < 3; inner++) {
				cx += Random_Next(&st_mobRng, 6) - Random_Next(&st_mobRng, 6);
				cz += Random_Next(&st_mobRng, 6) - Random_Next(&st_mobRng, 6);
				/* genuine vertical jitter is nextInt(1)-nextInt(1) = always 0 */

				if (cx < 0 || cz < 1 || cy < 0 || cy >= World.Height - 2 ||
					cx >= World.Width || cz >= World.Length) continue;
				/* genuine gate: isBlockNormalCube(cy-1) - a full OPAQUE cube, so
				    no perching on leaves/glass (the cell/head "not solid" tests
				    stay a stricter pre-filter; the real box clearance is enforced
				    by SpawnMobAt's isFree, so their net result is unchanged). */
				if (!Mob_BlockIsNormalCube(cx, cy - 1, cz)) continue;
				if (Mob_BlockIsSolid(cx, cy, cz))      continue;
				if (Mob_BlockIsSolid(cx, cy + 1, cz))  continue;
				if (Blocks.Collide[World_GetBlock(cx, cy, cz)] == COLLIDE_LIQUID) continue;

				/* EntityMob/EntityAnimal.getCanSpawnHere light rules */
				light = IndevTest_LightLevel(cx, cy, cz);
				if (monsters) {
					if (light > Random_Next(&st_mobRng, 8)) continue;
				} else {
					if (light <= 8) continue;
					/* EntityCreature.getCanSpawnHere also demands
					    getBlockPathWeight >= 0: animals weigh 10 over grass,
					    else lightBrightness - 0.5 - so non-grass ground
					    needs brightness >= 0.5 (light >= 12) */
					if (World_GetBlock(cx, cy - 1, cz) != BLOCK_GRASS &&
						IndevTest_BrightnessOfLight(light) < 0.5f) continue;
				}

				candidate.x = cx + 0.5f;
				candidate.y = (float)(cy + 1);
				candidate.z = cz + 0.5f;

				/* must be at least 32 blocks from the player */
				if (p) {
					dx = candidate.x - p->Base.Position.x;
					dy = candidate.y - p->Base.Position.y;
					dz = candidate.z - p->Base.Position.z;
					if (dx * dx + dy * dy + dz * dz < 1024.0f) continue;
				}

				if (SurvivalTest_SpawnMobAt((cc_uint8)type, candidate)) current++;
			}
		}
	}
}

static void Mob_IndevSpawnerRun(void) {
	cc_int64 volume = (cc_int64)World.Width * World.Height * World.Length;
	/* difficulty Normal (2): cap = vol*20/64^3 / 2 (the <<2)/4 is identity) */
	int monsterCap = (int)(volume * 20 / 64 / 64 / 64) / 2;
	int animalCap  = World.Width * World.Length / 4000;
	int monsters, animals;

	Mob_IndevCountKinds(&monsters, &animals);
	Mob_IndevSpawnPass(true,  monsterCap, monsters);
	Mob_IndevSpawnPass(false, animalCap,  animals);
}

/* LevelGenerator's "Spawning.." phase: 1000 MobSpawner.performSpawning
    passes populate a freshly generated world (mostly animals - monsters
    only stick where it's already dark, i.e. the caves). */
void SurvivalTest_IndevInitialSpawn(void) {
	int i;
	if (!IndevTest_Enabled || !World.Blocks) return;
	for (i = 0; i < 1000; i++) Mob_IndevSpawnerRun();
}

/* SurvivalGameMode.spawnMob() - the periodic per-tick spawn gate. */
static void SurvivalTest_TrySpawnMobs(void) {
	cc_int64 volume = (cc_int64)World.Width * World.Height * World.Length;
	int area = (int)(volume / 64 / 64 / 64);
	struct LocalPlayer* p = Entities.CurPlayer;
	if (!p || area <= 0) return;

	/* Indev replaces the classic spawn gate with MobSpawner's per-tick,
	    light-ruled, capped passes. */
	if (IndevTest_Enabled) { Mob_IndevSpawnerRun(); return; }

	if (Random_Next(&st_mobRng, 100) < area && SurvivalTest_CountMobs() < area * 20) {
		Mob_SpawnerRun(area, &p->Base.Position);
	}
}

/* SurvivalGameMode.prepareLevel() - the one-time initial population done */
/*  when a new map finishes loading. It calls spawner.spawn(count, null, ...) */
/*  with no avoid ENTITY - but MobSpawner's else branch still rejects */
/*  candidates within 16 blocks of the LEVEL SPAWN POINT. */
static void SurvivalTest_SpawnInitialMobs(void) {
	cc_int64 volume = (cc_int64)World.Width * World.Height * World.Length;
	int area = (int)(volume / 800);
	struct LocalPlayer* p = Entities.CurPlayer;
	/* Indev has no prepareLevel population - MobSpawner fills the world */
	/*  gradually under the darkness/distance rules instead (this is also */
	/*  what stops mobs from camping the spawn point on day one). */
	if (IndevTest_Enabled) return;
	if (area > 0) Mob_SpawnerRun(area, p ? &p->Spawn : NULL);
}

static void SurvivalTest_TickMobs(float delta) {
	int i;
	for (i = 0; i < MOB_MAX; i++) { SurvivalTest_TickOneMob(&st_mobs[i], delta); }
	SurvivalTest_TrySpawnMobs();
}


/*########################################################################################################################*
*----------------------------------------------MP mob puppet (phase 3)----------------------------------------------------*
*#########################################################################################################################*/
/* st_mobs as a network-driven view (networking-plan 15.1/17.5): the SERVER
    runs AI/physics/damage/spawning and streams SURV_MOB_*; the client only
    renders, interpolates and derives the cosmetic timers from state EDGES.
    None of the local sim (Mob_Travel, BasicAI, Mob_Hurt, spawner) runs on a
    puppet - TickOnePuppet below is the entire per-tick surface. */

static int Mob_FindByNetId(int id) {
	int i;
	for (i = 0; i < MOB_MAX; i++) {
		if (st_mobs[i].active && st_mobs[i].netId == (cc_uint16)id) return i;
	}
	return -1;
}

void SurvivalTest_NetMobSpawn(int id, int type, Vec3 pos, float yaw, float pitch, int health, int flags) {
	struct Mob* m;
	cc_string model;
	int slot;
	if (!SurvivalTest_Enabled || type < 0 || type >= MOB_TYPE_COUNT) return;

	/* A re-sent live id is an authoritative refresh of that mob, not a leak */
	slot = Mob_FindByNetId(id);
	if (slot < 0) slot = SurvivalTest_FindFreeMobSlot();
	if (slot < 0) return;

	m = &st_mobs[slot];
	Mem_Set(m, 0, sizeof(struct Mob));
	Entity_Init(&m->Base);
	m->Base.VTABLE = &mob_VTABLE;
	m->fuseState   = -1;   /* EntityCreeper.creeperState idles at -1, not 0 */
	m->wasInWater  = true; /* Entity.isFirstUpdate: never splash on the spawn tick */

	m->type      = (cc_uint8)type;
	m->hasHelmet = (flags & SURV_MOBFLAG_HELMET) != 0;
	m->hasArmor  = (flags & SURV_MOBFLAG_ARMOR)  != 0;
	m->hasFur    = (flags & SURV_MOBFLAG_FUR)    != 0;

	/* Sheep.renderModel only draws the fur layer while hasFur - an already
	    sheared sheep must arrive furless (same swap the local shear does). */
	model = String_FromReadonly(
		type == MOB_TYPE_SHEEP && !m->hasFur ? "sheep_nofur" : mobTypeInfo[type].model);
	Entity_SetModel(&m->Base, &model);
	Mob_ApplySize(m); /* genuine setSize - the model's collision box is off */

	/* No isFree spawn rejection here: the server already validated the spawn
	    and its word is final (rejecting would desync the mob population). */
	m->Base.Position = pos;
	m->Base.Yaw   = yaw;  m->Base.RotY = yaw; /* body faces the head's way at spawn */
	m->Base.Pitch = pitch;
	m->Base.prev.pos   = pos;   m->Base.next.pos   = pos;
	m->Base.prev.yaw   = yaw;   m->Base.next.yaw   = yaw;
	m->Base.prev.rotY  = yaw;   m->Base.next.rotY  = yaw;
	m->Base.prev.pitch = pitch; m->Base.next.pitch = pitch;

	m->targetSlot = -1;
	m->nextStep   = 1;
	m->health     = health;
	m->airTicks   = MOB_AIR_TICKS;
	m->netId      = (cc_uint16)id;
	m->active     = true;
}

void SurvivalTest_NetMobMove(int id, Vec3 pos, float yaw, float pitch) {
	int slot = Mob_FindByNetId(id);
	struct Mob* m;
	if (slot < 0) return; /* MOVE for an unknown id - its SPAWN will come */

	/* Latch the target; the puppet tick consumes it so the prev->next
	    interpolation window advances exactly like a local mob's. */
	m = &st_mobs[slot];
	m->netPos     = pos;
	m->netYaw     = yaw;
	m->netPitch   = pitch;
	m->netHasMove = true;
}

void SurvivalTest_NetMobState(int id, int health, int flags) {
	int slot = Mob_FindByNetId(id);
	struct Mob* m;
	cc_bool wasAlive;
	if (slot < 0) return;
	m = &st_mobs[slot];
	wasAlive = m->health > 0;

	/* Hurt presentation from the EDGE: red flash + the c0.30 white-flash
	    window + the hurt/death voice, mirroring Mob_Hurt's landed-hit path. */
	if ((health < m->health || (flags & SURV_MOBSTATE_HURT)) && wasAlive) {
		m->lastHealth  = m->health;
		m->invincTicks = MOB_INVINC_TICKS;
		m->hurtTicks   = 10;
		if (IndevTest_Enabled) {
			int snd = MOBSND_HURT;
			if (m->type == MOB_TYPE_SHEEP) snd = MOBSND_SHEEP;
			if (m->type == MOB_TYPE_PIG)   snd = health <= 0 ? MOBSND_PIGDEATH : MOBSND_PIG;
			Mob_PlaySound(m, snd, 1.0f, Mob_SndPitch());
		}
	}
	m->health = health;

	/* Creeper fuse arming plays the hiss on the rising edge (EntityCreeper
	    setCreeperState path); the swell itself ramps in the puppet tick. */
	if ((flags & SURV_MOBSTATE_FUSE) && !(m->netState & SURV_MOBSTATE_FUSE)) {
		Mob_PlaySound(m, MOBSND_FUSE, 1.0f, 0.5f);
	}

	/* A visible shear: fur off + the furless model, drops are server state. */
	if ((flags & SURV_MOBSTATE_NOFUR) && m->type == MOB_TYPE_SHEEP && m->hasFur) {
		cc_string mdl = String_FromReadonly("sheep_nofur");
		m->hasFur = false;
		Entity_SetModel(&m->Base, &mdl);
		Mob_ApplySize(m);
	}

	m->grazing = (flags & SURV_MOBSTATE_GRAZE) != 0;

	if ((health <= 0 || (flags & SURV_MOBSTATE_DEAD)) && wasAlive) {
		/* Death anim starts; the keel-over roll + removal run in the puppet
		    tick. Explosions/drops/score are all server events, not ours. */
		m->health     = 0;
		m->deathTicks = 0;
	}
	m->netState = (cc_uint8)flags;
}

void SurvivalTest_NetMobDespawn(int id, int reason) {
	int slot = Mob_FindByNetId(id);
	if (slot < 0) return;
	st_mobs[slot].active = false;
}

static void SurvivalTest_TickOnePuppet(struct Mob* m, float delta) {
	struct Entity* e = &m->Base;
	Vec3 oldPos;
	cc_bool inWater;
	if (!m->active) return;

	/* Advance the interpolation window exactly like TickOneMob: last tick's
	    resolved state (next) becomes this tick's starting point (prev). */
	e->prev     = e->next;
	e->Position = e->prev.pos;
	e->Yaw      = e->prev.yaw;
	e->Pitch    = e->prev.pitch;
	e->RotY     = e->prev.rotY;

	if (m->invincTicks > 0) m->invincTicks--;
	if (m->hurtTicks   > 0) m->hurtTicks--;
	if (m->attackTime  > 0) m->attackTime--;
	m->ticksAlive++;

	if (m->health <= 0) {
		m->deathTicks++;
		/* The server sends MOB_DESPAWN when the corpse window closes; free
		    locally after twice that window as a lost-packet belt-and-braces. */
		if (m->deathTicks > 40) { m->active = false; return; }
	}

	/* Creeper swell: the FUSE bit pins timeSinceIgnited ramping up (capped
	    at the pre-blast 30) or decaying; render lerps fuseLast->fuseTicks. */
	m->fuseLast = m->fuseTicks;
	if (m->netState & SURV_MOBSTATE_FUSE) {
		m->fuseState = 1;
		if (m->fuseTicks < 30) m->fuseTicks++;
	} else {
		m->fuseState = -1;
		if (m->fuseTicks > 0) m->fuseTicks--;
	}

	/* Fire overlay: STATE only arrives on change, so pin the burn counter
	    while the server says alight rather than letting it lapse. */
	m->fire = (m->netState & SURV_MOBSTATE_ONFIRE) ? 300 : 0;

	/* Sheep graze head-dip easing (Sheep.graze/grazeO) - the server decides
	    when grazing starts/stops; only the eased 0..1 dip is derived here. */
	m->grazeO = m->graze;
	m->graze += m->grazing ? 0.2f : -0.2f;
	Math_Clamp(m->graze, 0.0f, 1.0f);

	/* EntityLiving's ambient-voice roll is pure presentation, so it stays
	    client-side (matching the local sim: only pig and sheep have voices). */
	if (IndevTest_Enabled && m->health > 0 &&
		Random_Next(&st_mobRng, 1000) < m->livingSnd++) {
		m->livingSnd = -80;
		if (m->type == MOB_TYPE_PIG)   Mob_PlaySound(m, MOBSND_PIG,   1.0f, Mob_SndPitch());
		if (m->type == MOB_TYPE_SHEEP) Mob_PlaySound(m, MOBSND_SHEEP, 1.0f, Mob_SndPitch());
	}

	/* Apply the latched movement target, running the same presentation the
	    local tick would: walk animation, body-yaw easing, step sounds. */
	oldPos = e->Position;
	if (m->netHasMove) {
		e->Position   = m->netPos;
		e->Yaw        = m->netYaw;
		e->Pitch      = m->netPitch;
		m->netHasMove = false;
	}
	AnimatedComp_Update(e, oldPos, e->Position, delta);
	Mob_UpdateBodyYaw(m, oldPos);

	if (IndevTest_Enabled) {
		inWater = ST_InLiquid(e, false);
		if (inWater && !m->wasInWater)
			Indev_EntitySplash(e->Position, e->Velocity, e->Size.x);
		m->wasInWater = inWater;
	}

	/* Step sounds - same walkDist cadence + hearing ranges as TickOneMob
	    (trampling is skipped: farmland is the server's block state in MP). */
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
				float range = IndevTest_Enabled ? 16.0f : 32.0f;
				float ddy   = e->Position.y - sp->Base.Position.y;
				float dist;
				sdx  = e->Position.x - sp->Base.Position.x;
				sdz  = e->Position.z - sp->Base.Position.z;
				dist = Math_SqrtF(sdx * sdx + ddy * ddy + sdz * sdz);
				if (dist < range)
					Audio_PlayStepSoundAt(Blocks.StepSounds[under], 1.0f - dist / range);
			}
		}
	}

	/* Hurt wobble + death keel-over, identical to the local tick's block. */
	{
		float roll = 0.0f;
		if (m->hurtTicks > 0) {
			float ht = (float)m->hurtTicks / 10.0f;
			roll = Math_SinF(ht * ht * ht * ht * MATH_PI) * 14.0f;
		}
		if (m->health <= 0) {
			float deathT = (float)m->deathTicks;
			roll += deathT * deathT * 2.0f;
		}
		e->next.rotZ = min(roll, 90.0f);
	}

	e->next.pos   = e->Position;
	e->next.yaw   = e->Yaw;
	e->next.pitch = e->Pitch;
	e->next.rotY  = e->RotY;
}

static void SurvivalTest_TickPuppetMobs(float delta) {
	int i;
	for (i = 0; i < MOB_MAX; i++) { SurvivalTest_TickOnePuppet(&st_mobs[i], delta); }
}



/* Render.java's burning-entity pass: stacked camera-facing strips of the
    animated fire tile (see Animations.c's FireAnimation_Tick), each 1.4
    units tall and 10% narrower than the one below, scaled by width*1.4 and
    offset toward the viewer, drawn full-bright (glDisable(GL_LIGHTING)). */
#define FIRE_MAX_LAYERS   4
#define FIRE_MAX_VERTICES 512 /* 32 burning mobs at 4 layers */
static GfxResourceID st_fireVB;

static void SurvivalTest_RenderMobFires(void) {
	struct VertexTextured* data;
	struct VertexTextured* v;
	struct Mob* m;
	struct Entity* e;
	TextureRec rec;
	int i, texIndex, count = 0;
	cc_bool any = false;

	for (i = 0; i < MOB_MAX && IndevTest_Enabled; i++) {
		if (st_mobs[i].active && st_mobs[i].fire > 0) { any = true; break; }
	}
	if (!any) return;

	if (!st_fireVB) {
		st_fireVB = Gfx_CreateDynamicVb(VERTEX_FORMAT_TEXTURED, FIRE_MAX_VERTICES);
		if (!st_fireVB) return;
	}
	rec = Atlas1D_TexRec(INDEV_FIRE_TEX_LOC, 1, &texIndex);

	Gfx_SetVertexFormat(VERTEX_FORMAT_TEXTURED);
	data = (struct VertexTextured*)Gfx_LockDynamicVb(st_fireVB, VERTEX_FORMAT_TEXTURED, FIRE_MAX_VERTICES);
	v    = data;

	for (i = 0; i < MOB_MAX; i++) {
		float s, h, w, zoff, base, fx, fz, len, x0, x1, y0, y1;
		int layer, layers;
		m = &st_mobs[i];
		if (!m->active || m->fire <= 0) continue;
		e = &m->Base;

		s = e->Size.x * 1.4f;
		h = e->Size.y / e->Size.x;
		layers = (int)h; if (layers < h) layers++;
		if (layers < 1) layers = 1;
		if (layers > FIRE_MAX_LAYERS) layers = FIRE_MAX_LAYERS;
		if (count + layers * 4 > FIRE_MAX_VERTICES) break;

		/* the quads face the camera around Y only, like the item sprites */
		fx  = Camera.CurrentPos.x - e->Position.x;
		fz  = Camera.CurrentPos.z - e->Position.z;
		len = Math_SqrtF(fx * fx + fz * fz);
		if (len < 0.001f) { fx = 0.0f; fz = 1.0f; }
		else              { fx /= len; fz /= len; }

		w    = 1.0f;
		zoff = 0.4f + (float)((int)h) * 0.02f;
		base = 0.0f; /* var18: each layer starts a whole unit higher */

		for (layer = 0; layer < layers; layer++) {
			/* local x spans -0.5..w-0.5, y spans base..base+1.4, all scaled
			    by s; right vector = (-fz, fx), viewer offset = (fx, fz)*zoff */
			x0 = -0.5f * s;  x1 = (w - 0.5f) * s;
			y0 =  base * s;  y1 = (base + 1.4f) * s;

			#define FIRE_V(lx, ly, uu, vv) \
				v->x = e->Position.x + (-fz) * (lx) + fx * zoff * s; \
				v->y = e->Position.y + (ly); \
				v->z = e->Position.z +   fx  * (lx) + fz * zoff * s; \
				v->Col = PACKEDCOL_WHITE; v->U = (uu); v->V = (vv); v++;
			FIRE_V(x1, y0, rec.u2, rec.v2)
			FIRE_V(x0, y0, rec.u1, rec.v2)
			FIRE_V(x0, y1, rec.u1, rec.v1)
			FIRE_V(x1, y1, rec.u2, rec.v1)
			#undef FIRE_V

			count += 4;
			base  += 1.0f;
			w     *= 0.9f;
			zoff  -= 0.04f;
		}
	}

	Gfx_UnlockDynamicVb(st_fireVB);
	if (count) {
		Atlas1D_Bind(texIndex);
		Gfx_DrawVb_IndexedTris(count);
	}
}

/* Draws a remote player's held block/item in third person (SURV_PLAYER_EQUIP).
    DELIBERATELY A NO-OP: in-20100223 RenderPlayer never drew a held item on the
    third-person body (held-item-in-hand came in Alpha; the engine itself skips
    the held render outside first person, HeldBlockRenderer.c). Armor is the only
    faithful overlay. The held id is still streamed + stored so a NON-faithful
    "show held item" bonus could be gated on later for MP if desired - but it's
    intentionally off to match Indev. */
static void SurvivalTest_RenderHeldItem(struct Entity* e, cc_uint16 id) {
	(void)e; (void)id;
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
		/* Indev RenderCreeper.preRenderCallback: the fuse swell. s is the
		    interpolated timeSinceIgnited / (fuseTime - 2), a sin(s*100)*s*0.01
		    shiver modulates it, then s^4 fattens x/z by up to 40% and
		    stretches y by 10% (divided by the shiver so volume ~ conserved). */
		if (IndevTest_Enabled && mobTypeInfo[m->type].isCreeper) {
			float s = ((float)m->fuseLast + ((float)m->fuseTicks - (float)m->fuseLast) * t) / 28.0f;
			float shiver = 1.0f + Math_SinF(s * 100.0f) * s * 0.01f;
			float sxz, sy;
			if (s < 0.0f) s = 0.0f;
			if (s > 1.0f) s = 1.0f;
			s = s * s; s = s * s;
			sxz = (1.0f + s * 0.4f) * shiver;
			sy  = (1.0f + s * 0.1f) / shiver;
			e->ModelScale.x = sxz; e->ModelScale.y = sy; e->ModelScale.z = sxz;
		}

		e->ShouldRender = Model_ShouldRender(e);
		if (!e->ShouldRender) continue;

		Model_Render(e->Model, e);

		/* c0.30 Mob.render: while invulnerableTime is within 10 ticks of a */
		/*  fresh hit, the model is drawn a SECOND time additively in */
		/*  translucent white (glColor4f(1,1,1,0.75) + SRC_ALPHA/ONE) - the */
		/*  hit flash. Indev's RenderLiving has NO such pass - its flash is */
		/*  the red overlay below instead. */
		if (!IndevTest_Enabled && m->invincTicks > MOB_INVINC_TICKS - 10 && m->health > 0) {
			st_mobFlashCol  = PackedCol_Make(255, 255, 255, 191);
			st_mobFlashPass = true;
			Gfx_SetAlphaBlendingAdditive(true);
			Gfx_SetDepthWrite(false);
			Model_Render(e->Model, e);
			Gfx_SetDepthWrite(true);
			Gfx_SetAlphaBlendingAdditive(false);
			st_mobFlashPass = false;
		}

		/* Indev RenderLiving: while hurtTime or deathTime is live, the */
		/*  model is redrawn UNTEXTURED with glColor4f(brightness, 0, 0, 0.4) */
		/*  and plain alpha blending - a red overlay whose red channel is the */
		/*  mob's light brightness (getEntityBrightness), so a mob in a cave */
		/*  flashes DARK red, never fullbright. Constant 0.4 alpha for the */
		/*  whole 10-tick window - genuine has no fade-out. */
		if (IndevTest_Enabled && (m->hurtTicks > 0 || m->health <= 0)) {
			GfxResourceID oldTex;
			cc_bool oldNonHuman;
			Vec3 eyePos = Entity_GetEyePosition(e);
			IVec3 lp;
			float bright;
			IVec3_Floor(&lp, &eyePos);
			bright = IndevTest_BrightnessOfLight(IndevTest_LightLevel(lp.x, lp.y, lp.z));

			st_mobFlashCol  = PackedCol_Make((cc_uint8)(bright * 255.0f), 0, 0, 102);
			st_mobFlashPass = true;
			oldTex      = e->TextureId;    e->TextureId    = Mob_WhiteTex();
			oldNonHuman = e->NonHumanSkin; e->NonHumanSkin = true;
			Gfx_SetAlphaBlending(true);
			Gfx_SetDepthWrite(false);
			Model_Render(e->Model, e);
			Gfx_SetDepthWrite(true);
			Gfx_SetAlphaBlending(false);
			e->TextureId    = oldTex;
			e->NonHumanSkin = oldNonHuman;
			st_mobFlashPass = false;
		}
	}
	/* the local player's worn armor (third person only - genuine renders no
	    armor on the first-person arm) */
	if (IndevTest_Enabled && Camera.Active->isThirdPerson && Entities.CurPlayer) {
		IndevArmor_Render(&Entities.CurPlayer->Base);
	}

	/* remote players' worn armor + held item (SURV_PLAYER_EQUIP): the same
	    overlay hooks applied to each other player entity we've been sent equip
	    for. Always third-person (you view other players from outside). */
	if (IndevTest_Enabled) {
		int id;
		for (id = 0; id < ENTITIES_SELF_ID; id++) {
			struct Entity* pe = Entities.List[id];
			if (!pe || !st_netEquip[id].set) continue;
			IndevArmor_RenderIds(pe, st_netEquip[id].armor);
			SurvivalTest_RenderHeldItem(pe, st_netEquip[id].held);
		}
	}

	SurvivalTest_RenderPaintings();
	SurvivalTest_RenderMobFires();
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
	float t0, t1, bestT = 1.0e30f, capT;
	int i, pvpId = -1;

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
	capT = best ? bestT : p->ReachDistance;

	/* PvP (MP only, and only when the server flagged this map PvP - HELLO bit2):
	    other players' bodies are attackable exactly like mobs. The swing leaves
	    as SURV_ATTACK targetKind 1 with the per-viewer entity id; the server
	    resolves + validates it. Closest target under the crosshair wins. */
	if (SurvivalNet_ServerDriven() && (SurvivalNet_ActiveFlags() & 0x04)) {
		for (i = 0; i < ENTITIES_MAX_COUNT; i++) {
			struct Entity* ent = Entities.List[i];
			if (!ent || i == ENTITIES_SELF_ID) continue;
			if (!Intersection_RayIntersectsRotatedBox(eyePos, dir, ent, &t0, &t1)) continue;
			if (t0 > capT) continue;
			capT = t0; pvpId = i;
		}
	}

	/* PrimedTnt is pickable too (PrimedTnt.isPickable()), so a melee swing can */
	/*  hit a lit TNT and defuse it (hurt() with a Player attacker) instead of a */
	/*  mob. Only if it's closer than the best mob/player though (reach capped at */
	/*  that target's distance), so whichever the crosshair actually lands on wins. */
	if (!IndevTest_Enabled && /* Indev primed TNT cannot be punched out */
		!SurvivalNet_ServerDriven() &&
		SurvivalTest_TryDefuseTnt(eyePos, dir, capT)) {
		HeldBlockRenderer_ClickAnim(true);
		return true;
	}
	/* MP c0.30: primed TNT is server-owned, so a hit leaves as an attack intent
	    (targetKind 2) - the server removes it and drops the TNT back. */
	if (SurvivalNet_ServerDriven() && !IndevTest_Enabled) {
		int tntId = SurvivalTest_FindNetTntHit(eyePos, dir, capT);
		if (tntId >= 0) {
			HeldBlockRenderer_ClickAnim(true);
			SurvivalNet_SendAttack(2, tntId);
			return true;
		}
	}
	/* paintings are attackable too (EntityPainting.attackEntityFrom pops
	    them off as an item on ANY hit) - closest target wins */
	if (IndevTest_Enabled &&
		SurvivalTest_TryPunchPainting(eyePos, dir, capT)) {
		HeldBlockRenderer_ClickAnim(true);
		return true;
	}
	/* the PvP victim won the closest-target contest */
	if (pvpId >= 0) {
		HeldBlockRenderer_ClickAnim(true);
		SurvivalNet_SendAttack(1, pvpId);
		return true;
	}
	if (!best) return false;

	/* Minecraft.onMouseClick(0) starts the held-item swing at the very top, */
	/*  before it branches into hurting a mob - so a melee hit swings the arm */
	/*  exactly like mining a block does. The block-delete path plays this */
	/*  itself (InputHandler_DeleteBlock), but that path is skipped when we */
	/*  attack a mob, so trigger the same swing here to match. */
	HeldBlockRenderer_ClickAnim(true);

	/* MP: the swing lands as an intent - the server resolves damage/knockback/
	    aggro/shearing and streams the result back as MOB_STATE (tool wear is
	    server state too; DamageHeldTool already no-ops when ServerDriven). */
	if (SurvivalNet_ServerDriven()) {
		SurvivalNet_SendAttack(0, best->netId);
		return true;
	}

	/* c0.30 fist: flat 4 HP/hit. Indev (Minecraft.java:352): the held item's */
	/*  getDamageVsEntity - bare fist 1, tools base+tier, swords 4 + tier*2. */
	Mob_Hurt(best, e,
		IndevTest_Enabled ? IndevTest_MeleeDamage(st_inv[Inventory.SelectedIndex].id) : 4,
		true);
	/* hitEntity: swords wear 1, tools 2, hoes/flint&steel none */
	SurvivalTest_DamageHeldTool(
		IndevTest_ToolUseWear(st_inv[Inventory.SelectedIndex].id, true));
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

	/* Indev EntityArrow state (unused in c0.30 mode) */
	cc_uint8 arrowShake;  /* 7 on stick, decays 1/tick - gates pickup */
	int      airTicks;    /* ticksInAir - owner-grace clock, reset on bounce/re-loosen */
	IVec3    stuckTile;   /* xTile/yTile/zTile + inTile: the block stuck into - */
	BlockID  stuckBlock;  /*  mining it re-loosens the arrow */
	Vec3     stickVel;    /* remnant motion at stick time, scaled by the re-loosen kick */

	/* TakeEntityAnim - like item drops, a collected arrow flies to the player */
	/*  over 3 ticks ((t/3)^2 ease) before being removed. */
	cc_bool  pickingUp;
	float    pickupTime;
	Vec3     pickupFrom;

	/* MP (server-owned) arrows: the server assigns the id, owns the block-stick
	    and every hit; a net arrow client-simulates the SAME c0.30 flight for a
	    smooth arc but never resolves a collision itself - it freezes on a
	    SURV_ARROW_STICK and is retired by SURV_ARROW_REMOVE. */
	cc_bool  net;
	int      netId;
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

/* Standard-normal sample via Box-Muller over a Random_Float pair. Genuine
    java.util.Random.nextGaussian (Marsaglia polar with a cached second value)
    cannot be sequence-matched anyway - the arrow RNG is time-seeded - so
    distribution-shape parity is the target. ln(u) = log2(u) * ln(2). */
static float ST_NextGaussian(RNGState* rng) {
	float u1 = 1.0f - Random_Float(rng); /* (0,1] - guards log(0) */
	float u2 = Random_Float(rng);
	return Math_SqrtF(-2.0f * (float)(Math_Log2(u1) * 0.6931471805599453)) *
	       Math_CosF(2.0f * MATH_PI * u2);
}

/* Indev EntityArrow.setArrowHeading: normalize the raw aim vector, nudge each
    velocity axis by an independent gaussian * 0.0075 * spreadFactor (player
    bow 1.0, skeleton 12.0), then scale by speed WITHOUT re-normalizing.
    Damage is the flat 4 of EntityArrow.onEntityUpdate's attackEntityFrom -
    Indev has no per-owner damage, and RenderArrow has no per-type texture
    row either, so type is always 0. gravity field is unused by Indev tick. */
static void SurvivalTest_SpawnArrowIndev(Vec3 pos, Vec3 rawDir, float speed,
										  float spreadFactor, cc_bool ownerIsPlayer, int ownerMobSlot) {
	struct ArrowEntity* a;
	float len;
	int slot = SurvivalTest_FindFreeArrowSlot();

	a = &st_arrows[slot];
	Mem_Set(a, 0, sizeof(struct ArrowEntity));

	len = Math_SqrtF(rawDir.x * rawDir.x + rawDir.y * rawDir.y + rawDir.z * rawDir.z);
	if (len < 0.0001f) { rawDir.x = 0.0f; rawDir.y = 1.0f; rawDir.z = 0.0f; len = 1.0f; }
	rawDir.x /= len; rawDir.y /= len; rawDir.z /= len;

	rawDir.x += ST_NextGaussian(&st_arrowRng) * 0.0075f * spreadFactor;
	rawDir.y += ST_NextGaussian(&st_arrowRng) * 0.0075f * spreadFactor;
	rawDir.z += ST_NextGaussian(&st_arrowRng) * 0.0075f * spreadFactor;

	a->velocity.x = rawDir.x * speed;
	a->velocity.y = rawDir.y * speed;
	a->velocity.z = rawDir.z * speed;

	a->pos     = pos; /* callers apply the constructor hand offset themselves */
	a->prevPos = pos;
	len = Math_SqrtF(a->velocity.x * a->velocity.x + a->velocity.y * a->velocity.y + a->velocity.z * a->velocity.z);
	if (len > 0.0001f) {
		a->facing.x = a->velocity.x / len;
		a->facing.y = a->velocity.y / len;
		a->facing.z = a->velocity.z / len;
	} else { a->facing.y = 1.0f; }

	a->gravity       = 1.0f; /* unused by the Indev flight model */
	a->type          = 0;
	a->damage        = 4;
	a->ownerIsPlayer = ownerIsPlayer;
	a->ownerMobSlot  = (cc_int8)ownerMobSlot;
	a->active        = true;
}

/*########################################################################################################################*
*----------------------------------------------MP (server-owned) arrow appliers-------------------------------------------*
*#########################################################################################################################*/
/* SurvivalNet feeds these from SURV_ARROW_SPAWN/STICK/REMOVE. The server owns the
    id, the flight authority (block-stick + every hit) and the ammo; a net arrow
    client-simulates the c0.30 flight (drag+gravity, matching the server) for a
    smooth visual and freezes / vanishes when the server says so. */
static struct ArrowEntity* SurvivalTest_FindNetArrow(int netId) {
	int i;
	for (i = 0; i < ARROW_MAX; i++) {
		if (st_arrows[i].active && st_arrows[i].net && st_arrows[i].netId == netId)
			return &st_arrows[i];
	}
	return NULL;
}

void SurvivalTest_NetArrowSpawn(int netId, int type, float gravity, Vec3 pos, Vec3 vel) {
	struct ArrowEntity* a = SurvivalTest_FindNetArrow(netId);
	float len;
	if (!a) a = &st_arrows[SurvivalTest_FindFreeArrowSlot()];
	Mem_Set(a, 0, sizeof(struct ArrowEntity));

	a->pos     = pos;
	a->prevPos = pos;
	a->velocity = vel;
	a->gravity = gravity > 0.0001f ? gravity : 1.0f;
	a->type    = (cc_uint8)type;
	a->ownerIsPlayer = type == 0;
	a->ownerMobSlot  = -1;
	a->active  = true;
	a->net     = true;
	a->netId   = netId;

	len = Math_SqrtF(vel.x * vel.x + vel.y * vel.y + vel.z * vel.z);
	if (len > 0.0001f) { a->facing.x = vel.x/len; a->facing.y = vel.y/len; a->facing.z = vel.z/len; }
	else               { a->facing.y = 1.0f; }
}

void SurvivalTest_NetArrowStick(int netId, Vec3 pos) {
	struct ArrowEntity* a = SurvivalTest_FindNetArrow(netId);
	if (!a) return;
	a->pos      = pos;
	a->hasHit   = true;
	a->velocity.x = a->velocity.y = a->velocity.z = 0.0f;
	Indev_PlaySoundAt(a->pos, MOBSND_DRR,
		1.0f, 1.2f / (Random_Float(&st_arrowRng) * 0.2f + 0.9f));
}

void SurvivalTest_NetArrowRemove(int netId) {
	struct ArrowEntity* a = SurvivalTest_FindNetArrow(netId);
	if (a) a->active = false;
}

void SurvivalTest_NetSetArrowCount(int count) {
	if (count < 0) count = 0;
	if (count > ARROW_PLAYER_MAX) count = ARROW_PLAYER_MAX;
	st_playerArrows = count;
}

/* SURV_PLAYER_HURT applier: a remote player took a LANDED hit. Arm the same
    10-tick hurt roll the mob puppets keel with (rendered by the RemoteHurtRoll
    hook in NetPlayer_RenderModel) and voice the hit at their body. The server
    never sends the viewer's own id - the local presentation (camera tilt +
    sound) rides the SURV_HEALTH drop instead. */
void SurvivalTest_NetPlayerHurt(int entityId) {
	struct Entity* e;
	if (entityId < 0 || entityId >= ENTITIES_SELF_ID) return;
	e = Entities.List[entityId];
	if (!e) return;
	e->NetHurtTicks = HURT_TILT_TICKS;
	Indev_PlaySoundAt(e->Position, MOBSND_HURT, 1.0f, Mob_SndPitch());
}

/* SURV_PLAYER_HURT state 1/2 applier: the remote player died or revived. Death
    arms the killing blow's wobble + the mob-style keel-over ramp (the server
    unloads the entity ~1s later for the dwell, so the keel is what viewers see
    between the hit and the vanish); revive clears both. The hurt voice plays
    only if a landed-hit wobble isn't already fresh - melee kills broadcast the
    hurt AND the death, and one hit is one voice. */
void SurvivalTest_NetPlayerDeathState(int entityId, cc_bool died) {
	struct Entity* e;
	if (entityId < 0 || entityId >= ENTITIES_SELF_ID) return;
	e = Entities.List[entityId];
	if (!e) return;
	if (died) {
		if (!e->NetHurtTicks) Indev_PlaySoundAt(e->Position, MOBSND_HURT, 1.0f, Mob_SndPitch());
		e->NetHurtTicks  = HURT_TILT_TICKS;
		e->NetDeathTicks = 1;
	} else {
		e->NetHurtTicks  = 0;
		e->NetDeathTicks = 0;
	}
}

/* Render-time hurt roll for a remote player: sin((t/10)^4 * pi) * 14 degrees,
    the exact wobble the mob puppets use, interpolated within the tick - plus,
    once dead, the mob death keel (deathT^2 * 2, i.e. (t/20)^2*800 simplified),
    the pair capped at 90 like the puppets. Returns 0 when idle so the hook is
    free for untouched entities. */
float SurvivalTest_RemoteHurtRoll(struct Entity* e, float t) {
	float roll = 0.0f, remaining, deathT;
	if (!e->NetHurtTicks && !e->NetDeathTicks) return 0.0f;

	if (e->NetHurtTicks) {
		remaining = (float)e->NetHurtTicks - t;
		if (remaining > 0.0f) {
			remaining /= (float)HURT_TILT_TICKS;
			roll = Math_SinF(remaining * remaining * remaining * remaining * MATH_PI) * HURT_TILT_MAX_DEG;
		}
	}
	if (e->NetDeathTicks) {
		deathT = (float)e->NetDeathTicks - 1.0f + t;
		roll  += deathT * deathT * 2.0f;
	}
	return roll > 90.0f ? 90.0f : roll;
}

/* SURV_PLAYER_EQUIP applier: stores a remote player's held id + 4 armor ids so
    the render pass can draw them on Entities.List[entityId]. */
void SurvivalTest_NetPlayerEquip(int entityId, int heldId, const cc_uint16* armor) {
	struct NetEquip* eq;
	int i;
	if (entityId < 0 || entityId >= ENTITIES_SELF_ID) return;
	eq = &st_netEquip[entityId];
	eq->held = (cc_uint16)heldId;
	for (i = 0; i < 4; i++) eq->armor[i] = armor[i];
	eq->set = true;
}

/* MP flight tick for server-owned arrows: the SAME c0.30 drag+gravity+move as
    Arrow_Tick, but with NO collision or hit resolution (the server owns those and
    corrects us via STICK/REMOVE). Stuck arrows just hold their frozen pose. */
static void SurvivalTest_TickNetArrows(void) {
	struct ArrowEntity* a;
	float len;
	int i;

	for (i = 0; i < ARROW_MAX; i++) {
		a = &st_arrows[i];
		if (!a->active || !a->net) continue;

		a->age++;
		a->prevPos = a->pos;
		if (a->hasHit) continue; /* frozen in a block - waits for SURV_ARROW_REMOVE */

		/* Flight matches the SERVER (which branches on the map mode). c0.30 applies
		    drag + speed-scaled gravity BEFORE the move; Indev moves first, then drag
		    (0.99 air / 0.8 water) + a flat 0.03 gravity. Both must mirror the server
		    exactly so the client's arc stays in lock-step until a STICK/REMOVE. */
		if (!IndevTest_Enabled) {
			a->velocity.x *= ARROW_DRAG;
			a->velocity.y *= ARROW_DRAG;
			a->velocity.z *= ARROW_DRAG;
			a->velocity.y -= 0.02f * a->gravity;
		}

		a->pos.x += a->velocity.x;
		a->pos.y += a->velocity.y;
		a->pos.z += a->velocity.z;

		if (IndevTest_Enabled) {
			float drag = 0.99f;
			if (World_Contains(Math_Floor(a->pos.x), Math_Floor(a->pos.y), Math_Floor(a->pos.z)) &&
				ST_IsWaterBlock(World_GetBlock(Math_Floor(a->pos.x), Math_Floor(a->pos.y), Math_Floor(a->pos.z))))
				drag = 0.8f;
			a->velocity.x *= drag;
			a->velocity.y *= drag;
			a->velocity.z *= drag;
			a->velocity.y -= 0.03f;
		}

		len = Math_SqrtF(a->velocity.x * a->velocity.x + a->velocity.y * a->velocity.y + a->velocity.z * a->velocity.z);
		if (len > 0.0001f) {
			a->facing.x = a->velocity.x / len;
			a->facing.y = a->velocity.y / len;
			a->facing.z = a->velocity.z / len;
		}
	}
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
/* hitTile/hitBlock (optional) record the first overlapping cell - the swept- */
/*  box stand-in for Indev rayTraceBlocks' hit cell, needed for re-loosening. */
static cc_bool Arrow_BlockCollision(struct AABB* bb, IVec3* hitTile, BlockID* hitBlock) {
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
		if (AABB_Intersects(bb, &blockBB)) {
			if (hitTile)  { hitTile->x = x; hitTile->y = y; hitTile->z = z; }
			if (hitBlock) { *hitBlock = b; }
			return true;
		}
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
	/* Indev: owner grace is ticksInAir < 5 (re-arms on bounce/re-loosen since
	    those reset ticksInAir); c0.30 gates on the arrow's total age. Indev
	    also grows the TARGET's box 0.3 each side (bb.expand(0.3F,0.3F,0.3F))
	    before the intercept test. */
	cc_bool ownerShielded = IndevTest_Enabled ? a->airTicks < 5
	                                          : a->age <= ARROW_OWNER_GRACE_TICKS;
	float grow = IndevTest_Enabled ? 0.3f : 0.0f;

	if (p && !(a->ownerIsPlayer && ownerShielded)) {
		Entity_GetBounds(&p->Base, &other);
		other.Min.x -= grow; other.Min.y -= grow; other.Min.z -= grow;
		other.Max.x += grow; other.Max.y += grow; other.Max.z += grow;
		if (AABB_Intersects(bb, &other)) {
			*outEntity = &p->Base; *outMob = NULL; return true;
		}
	}

	for (i = 0; i < MOB_MAX; i++) {
		m = &st_mobs[i];
		if (!m->active || m->health <= 0) continue;
		if (!a->ownerIsPlayer && a->ownerMobSlot == i && ownerShielded) continue;

		Entity_GetBounds(&m->Base, &other);
		other.Min.x -= grow; other.Min.y -= grow; other.Min.z -= grow;
		other.Max.x += grow; other.Max.y += grow; other.Max.z += grow;
		if (AABB_Intersects(bb, &other)) {
			*outEntity = &m->Base; *outMob = m; return true;
		}
	}
	return false;
}

/* entity.hurt(this, damage) - knockback is computed from the ARROW's own */
/*  position (not the original shooter's), exactly as Mob.hurt()/Arrow.tick() */
/*  do in the decompiled source (the arrow passes itself as the cause). */
/* Returns whether the arrow was consumed. c0.30 always consumes silently; */
/*  Indev plays random.drr on a landed hit, and an ABSORBED hit (invuln */
/*  window / armor zero-round, attackEntityFrom returning false) BOUNCES: */
/*  motion *= -0.1 per axis, ticksInAir reset, arrow keeps flying. */
static cc_bool Arrow_ApplyHit(struct ArrowEntity* a, struct Entity* hitEntity, struct Mob* hitMob) {
	struct Entity fakeAttacker = { 0 };
	cc_bool landed;
	fakeAttacker.Position = a->pos;
	st_hurtViaArrow = true;

	if (hitMob) {
		/* BasicAttackAI.hurt resolves an Arrow cause to its OWNER - so a mob */
		/*  struck by a skeleton's arrow aggros onto that skeleton (-1 = the */
		/*  player fired it, aggroing onto the player as before). */
		st_hurtCauseSlot = a->ownerIsPlayer ? -1 : a->ownerMobSlot;
		landed = Mob_Hurt(hitMob, &fakeAttacker, a->damage, a->ownerIsPlayer);
	st_hurtViaArrow = false;
	} else {
		landed = SurvivalTest_Damage(a->damage, &a->pos);
	}

	if (!IndevTest_Enabled || landed) {
		/* Indev EntityArrow: a landed hit plays random.drr then removes */
		Indev_PlaySoundAt(a->pos, MOBSND_DRR,
			1.0f, 1.2f / (Random_Float(&st_arrowRng) * 0.2f + 0.9f));
		a->active = false;
		return true;
	}

	a->velocity.x *= -0.1f;
	a->velocity.y *= -0.1f;
	a->velocity.z *= -0.1f;
	a->airTicks = 0; /* re-arms the owner grace */
	return false;
}

/* c0.30 Arrow.tick(): drag+gravity FIRST, then a subdivided sweep (so fast */
/*  arrows can't tunnel through thin obstacles) checking blocks then entities. */
/* Indev EntityArrow.onEntityUpdate: movement FIRST at full current velocity, */
/*  then drag (0.99 air / 0.8 water) and a flat 0.03 gravity - plus stuck- */
/*  block re-loosening, the 1200-tick despawn, and the arrowShake decay. */
static void Arrow_Tick(struct ArrowEntity* a) {
	struct AABB bb, swept;
	struct Entity* hitEntity;
	struct Mob* hitMob;
	Vec3 step;
	IVec3 hitTile;
	BlockID hitBlock = BLOCK_AIR;
	float len, drag;
	int steps, s;
	cc_bool indev = IndevTest_Enabled;
	cc_bool collided = false;

	a->age++;
	/* Snapshot this tick's starting position as the interpolation source - */
	/*  RenderArrows blends prevPos->pos by the partial-tick t every frame, */
	/*  same as Arrow.render()'s xo/x blending in the decompiled source. */
	a->prevPos = a->pos;
	if (indev && a->arrowShake > 0) a->arrowShake--;

	if (a->hasHit) {
		if (!indev) {
			a->stickTime++;
			if (a->type == 0) {
				if (a->stickTime >= ARROW_STICK_PLAYER_MIN_TICKS &&
					Random_Float(&st_arrowRng) < ARROW_STICK_PLAYER_DESPAWN_CHANCE) a->active = false;
			} else {
				if (a->stickTime >= ARROW_STICK_MOB_TICKS) a->active = false;
			}
			return;
		}
		/* Indev: still stuck only while the recorded block is unchanged */
		if (World_Contains(a->stuckTile.x, a->stuckTile.y, a->stuckTile.z) &&
			World_GetBlock(a->stuckTile.x, a->stuckTile.y, a->stuckTile.z) == a->stuckBlock) {
			a->stickTime++;
			/* genuine EntityArrow: ANY stuck arrow dies at exactly
			    ticksInGround == 1200, no random roll, player and mob alike */
			if (a->stickTime >= 1200) a->active = false;
			return;
		}
		/* the block was mined out - re-loosen with a random fraction of the
		    remnant stick motion (three independent nextFloat rolls) and fall
		    through to the flight code THIS tick, exactly like genuine */
		a->hasHit     = false;
		a->velocity.x = a->stickVel.x * Random_Float(&st_arrowRng) * 0.2f;
		a->velocity.y = a->stickVel.y * Random_Float(&st_arrowRng) * 0.2f;
		a->velocity.z = a->stickVel.z * Random_Float(&st_arrowRng) * 0.2f;
		a->stickTime  = 0;
		a->airTicks   = 0;
	}

	if (indev) a->airTicks++;

	if (!indev) {
		/* c0.30: drag + speed-scaled gravity BEFORE the move */
		a->velocity.x *= ARROW_DRAG;
		a->velocity.y *= ARROW_DRAG;
		a->velocity.z *= ARROW_DRAG;
		a->velocity.y -= 0.02f * a->gravity;
	}

	len   = Math_SqrtF(a->velocity.x * a->velocity.x + a->velocity.y * a->velocity.y + a->velocity.z * a->velocity.z);
	steps = (int)(len / ARROW_SUBSTEP_LEN + 1.0f);
	step.x = a->velocity.x / steps;
	step.y = a->velocity.y / steps;
	step.z = a->velocity.z / steps;

	Arrow_BoxAt(&a->pos, &bb);

	for (s = 0; s < steps && !collided; s++) {
		swept = bb;
		Arrow_ExpandBox(&swept, &step);

		if (Arrow_BlockCollision(&swept, &hitTile, &hitBlock)) collided = true;

		if (Arrow_EntityCollision(a, &swept, &hitEntity, &hitMob)) {
			if (Arrow_ApplyHit(a, hitEntity, hitMob)) return;
			/* Indev absorbed-hit bounce: velocity reversed - stop sweeping
			    and let the same tick's drag/gravity act on it */
			collided = false;
			break;
		}

		if (!collided) {
			a->pos.x += step.x; a->pos.y += step.y; a->pos.z += step.z;
			Arrow_BoxAt(&a->pos, &bb);
		}
	}

	if (collided) {
		a->hasHit = true;
		if (indev) {
			/* xTile/yTile/zTile + inTile + the remnant motion the re-loosen
			    kick later scales; arrowShake gates pickup for 7 ticks */
			a->stuckTile  = hitTile;
			a->stuckBlock = hitBlock;
			a->stickVel   = a->velocity;
			a->arrowShake = 7;
		}
		a->velocity.x = a->velocity.y = a->velocity.z = 0.0f;
		/* Indev EntityArrow: sticking into a block plays random.drr */
		Indev_PlaySoundAt(a->pos, MOBSND_DRR,
			1.0f, 1.2f / (Random_Float(&st_arrowRng) * 0.2f + 0.9f));
		return;
	}

	/* re-measure - an Indev bounce reversed + shrank the velocity mid-sweep */
	if (indev) len = Math_SqrtF(a->velocity.x * a->velocity.x + a->velocity.y * a->velocity.y + a->velocity.z * a->velocity.z);

	if (len > 0.0001f) {
		if (indev) {
			/* rot = prevRot + (rot - prevRot) * 0.2F - the visual heading
			    lags the velocity, approximated as a vector lerp */
			a->facing.x += (a->velocity.x / len - a->facing.x) * 0.2f;
			a->facing.y += (a->velocity.y / len - a->facing.y) * 0.2f;
			a->facing.z += (a->velocity.z / len - a->facing.z) * 0.2f;
			Vec3_Normalise(&a->facing);
		} else {
			a->facing.x = a->velocity.x / len;
			a->facing.y = a->velocity.y / len;
			a->facing.z = a->velocity.z / len;
		}
	}

	if (indev) {
		/* move -> drag -> flat gravity, per Entity/EntityArrow order. The
		    handleWaterMovement band (bb.expand(0,-0.4,0) of a 0.5-tall box)
		    degenerates to ~the centre cell - test the centre block. */
		drag = 0.99f;
		if (World_Contains(Math_Floor(a->pos.x), Math_Floor(a->pos.y), Math_Floor(a->pos.z)) &&
			ST_IsWaterBlock(World_GetBlock(Math_Floor(a->pos.x), Math_Floor(a->pos.y), Math_Floor(a->pos.z))))
			drag = 0.8f;
		a->velocity.x *= drag;
		a->velocity.y *= drag;
		a->velocity.z *= drag;
		a->velocity.y -= 0.03f;
	}
}

/* Arrow.playerTouch() - fed by the same Player.aiStep bb.grow(1, 0, 1) */
/*  entity sweep as item pickup, so arrows share the items' 1-block sideways */
/*  pickup reach. Only ever applies to player-owned arrows already stuck. */
static void Arrow_TryPickup(struct ArrowEntity* a) {
	struct LocalPlayer* p;
	struct AABB arrowBB, playerBB;
	if (!a->hasHit || !a->ownerIsPlayer)      return;
	if (!IndevTest_Enabled && st_playerArrows >= ARROW_PLAYER_MAX) return;
	/* genuine playerTouch: pickup only after the 7-tick arrowShake decays */
	if (IndevTest_Enabled && a->arrowShake > 0) return;

	p = Entities.CurPlayer;
	if (!p) return;

	Arrow_BoxAt(&a->pos, &arrowBB);
	Entity_GetBounds(&p->Base, &playerBB);
	playerBB.Min.x -= 1.0f; playerBB.Max.x += 1.0f;
	playerBB.Min.z -= 1.0f; playerBB.Max.z += 1.0f;
	if (!AABB_Intersects(&arrowBB, &playerBB)) return;

	if (IndevTest_Enabled) {
		/* Indev arrows are ITEMS - picked back up into the inventory (and
		    left stuck when it is full, like genuine playerTouch) */
		if (!SurvivalTest_AddItem(256 + 6)) return;
	} else
	st_playerArrows++;
	/* Arrow.playerTouch: arrows++ happens immediately, then a TakeEntityAnim */
	/*  zips the arrow into the player before the entity is removed. */
	/* Indev EntityArrow.playerTouch also plays the pickup pop. */
	Indev_PlaySoundAt(a->pos, MOBSND_POP, 0.2f,
		((Random_Float(&st_arrowRng) - Random_Float(&st_arrowRng)) * 0.7f + 1.0f) * 2.0f);
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
		/* EntityPainting.canBeCollidedWith: a flying arrow that enters a
		    painting's box pops it (attackEntityFrom) and is consumed */
		if (a->active && !a->hasHit && IndevTest_Enabled &&
			SurvivalTest_ArrowHitPainting(a->pos)) {
			a->active = false;
		}
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
	/* Indev has no Tab-fire - arrows are items fired by the BOW's right
	    click (SurvivalTest_TryUseBow) */
	if (IndevTest_Enabled)     return false;

	p = Entities.CurPlayer;
	if (!p) return false;
	e = &p->Base;

	/* MP: firing is an intent - the server owns the arrow entity + the ammo
	    count (streamed back as SURV_ARROW_AMMO), so send the aim and let it
	    spawn + simulate the shot. */
	if (SurvivalNet_ServerDriven()) {
		SurvivalNet_SendFireArrow(e->Yaw, e->Pitch, 0);
		return true;
	}

	if (st_playerArrows <= 0)  return false;

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

/* ItemBow.onItemRightClick: consumeInventoryItem(arrow) - the FIRST slot
    holding arrows, in inventory order - then the bow twang and an arrow at
    the genuine 1.5 speed / spread 1.0 (damage is the tick's flat 4). The bow
    itself has NO durability in in-20100223, and the click is handled either
    way (a dry bow just does nothing, never falling through to placement). */
#define INDEV_ITEM_ARROW (256 + 6)
#define INDEV_ARROW_FIRE_SPEED  1.5f /* EntityArrow ctor's setArrowHeading(..., 1.5F, 1.0F) */
#define INDEV_ARROW_FIRE_SPREAD 1.0f

static void SurvivalTest_SyncHotbar(void);

cc_bool SurvivalTest_TryUseBow(void) {
	struct LocalPlayer* p = Entities.CurPlayer;
	struct Entity* e;
	Vec3 eye;
	int i;
	if (!IndevTest_Enabled || !p)                              return false;
	if (!IndevTest_IsBow(st_inv[Inventory.SelectedIndex].id))  return false;
	if (st_inv[Inventory.SelectedIndex].count <= 0)            return false;

	/* MP: firing is an intent - the server consumes an arrow item from the
	    server-owned inventory and spawns/simulates the arrow. */
	if (SurvivalNet_ServerDriven()) {
		e = &p->Base;
		SurvivalNet_SendFireArrow(e->Yaw, e->Pitch, 1);
		return true;
	}

	for (i = 0; i < SURVIVAL_INV_SLOTS; i++) {
		if (st_inv[i].id == INDEV_ITEM_ARROW && st_inv[i].count > 0) break;
	}
	if (i == SURVIVAL_INV_SLOTS) return true; /* no arrows - click still handled */

	if (--st_inv[i].count == 0) { st_inv[i].id = BLOCK_AIR; st_inv[i].damage = 0; }
	st_invVersion++;
	SurvivalTest_SyncHotbar();

	e   = &p->Base;
	eye = Entity_GetEyePosition(e);
	{
		/* EntityArrow's constructor offset: 0.1 down and 0.16 sideways along
		    the yaw's horizontal perpendicular (the bow hand), then the unit
		    aim vector into setArrowHeading(1.5F, 1.0F). */
		float yawRad = e->Yaw * MATH_DEG2RAD;
		Vec3 dir = Vec3_GetDirVector(yawRad, e->Pitch * MATH_DEG2RAD);
		eye.x += Math_CosF(yawRad) * 0.16f;
		eye.y -= 0.1f;
		eye.z += Math_SinF(yawRad) * 0.16f;
		Indev_PlaySoundAt(e->Position, MOBSND_BOW, 1.0f,
			1.0f / (Random_Float(&st_arrowRng) * 0.4f + 0.8f));
		SurvivalTest_SpawnArrowIndev(eye, dir,
			INDEV_ARROW_FIRE_SPEED, INDEV_ARROW_FIRE_SPREAD, true, -1);
	}
	return true;
}


/*########################################################################################################################*
*------------------------------------------------------Paintings----------------------------------------------------------*
*#########################################################################################################################*/
/* in-20100223 EntityPainting + ItemPainting + RenderPainting, ported with
    the genuine geometry to the pixel: the painting plane hangs 1/16 in
    front of the clicked wall block (tile centre - 9/16 along the facing),
    half-extents sizeX/32 x sizeY/32 with a 1/64 half-thickness, the odd
    +-0.5 re-centring for 32px-AND-64px-wide art (which leaves the 64px
    Fighters/Pointer genuinely off-centre), and the bounding box's MAX
    corner shrunk by 0.1/16 on all three axes. Genuine quirk kept: the
    "still on a wall?" check runs ONCE, at tickCounter == 100 - a painting
    whose wall is later mined stays floating (punch it to pop it). */
#define PAINTING_MAX 64
#define INDEV_ITEM_PAINTING (256 + 65)

struct PaintingArt { const char* title; cc_uint8 sizeX, sizeY, offX, offY; };
static const struct PaintingArt paintingArts[] = {
	{ "Kebab",     16,16,  0,  0 }, { "Aztec",   16,16, 16,  0 },
	{ "Alban",     16,16, 32,  0 }, { "Aztec2",  16,16, 48,  0 },
	{ "Bomb",      16,16, 64,  0 }, { "Plant",   16,16, 80,  0 },
	{ "Wasteland", 16,16, 96,  0 }, { "Pool",    32,16,  0, 32 },
	{ "Courbet",   32,16, 32, 32 }, { "Sea",     32,16, 64, 32 },
	{ "Sunset",    32,16, 96, 32 }, { "Wanderer",16,32,  0, 64 },
	{ "Match",     32,32,  0,128 }, { "Bust",    32,32, 32,128 },
	{ "Stage",     32,32, 64,128 }, { "Void",    32,32, 96,128 },
	{ "SkullAndRoses", 32,32,128,128 },
	{ "Fighters",  64,32,  0, 96 }, { "Pointer", 64,64,  0,192 },
};

struct PaintingEntity {
	cc_bool active;
	cc_uint8 dir, art;      /* dir 0..3 (yaw = dir * 90) */
	cc_int16 tileX, tileY, tileZ;
	Vec3 pos;               /* genuine posX/Y/Z - the painting centre */
	struct AABB bb;
	int tickCounter;
};
static struct PaintingEntity st_paintings[PAINTING_MAX];

/* EntityPainting.getArtSize: 0.5 for BOTH 32px and 64px - the genuine
    source of the off-centre large paintings */
static float Painting_ArtSize(int px) { return px >= 32 ? 0.5f : 0.0f; }

static void Painting_SetDirection(struct PaintingEntity* pt) {
	const struct PaintingArt* a = &paintingArts[pt->art];
	float w2 = (float)a->sizeX, h2 = (float)a->sizeY, d2 = (float)a->sizeX;
	float x, y, z;

	if (pt->dir != 0 && pt->dir != 2) w2 = 0.5f; else d2 = 0.5f;
	w2 /= 32.0f; h2 /= 32.0f; d2 /= 32.0f;

	x = pt->tileX + 0.5f; y = pt->tileY + 0.5f; z = pt->tileZ + 0.5f;
	if (pt->dir == 0) z -= 9.0f / 16.0f;
	if (pt->dir == 1) x -= 9.0f / 16.0f;
	if (pt->dir == 2) z += 9.0f / 16.0f;
	if (pt->dir == 3) x += 9.0f / 16.0f;

	if (pt->dir == 0) x -= Painting_ArtSize(a->sizeX);
	if (pt->dir == 1) z += Painting_ArtSize(a->sizeX);
	if (pt->dir == 2) x += Painting_ArtSize(a->sizeX);
	if (pt->dir == 3) z -= Painting_ArtSize(a->sizeX);
	y += Painting_ArtSize(a->sizeY);

	pt->pos.x = x; pt->pos.y = y; pt->pos.z = z;
	Vec3_Set(pt->bb.Min, x - w2, y - h2, z - d2);
	/* genuine shrinks only the MAX corner by 0.1/16 */
	Vec3_Set(pt->bb.Max, x + w2 - 0.1f/16.0f, y + h2 - 0.1f/16.0f, z + d2 - 0.1f/16.0f);
}

/* EntityPainting.onValidSurface */
static cc_bool Painting_ValidSurface(struct PaintingEntity* pt) {
	const struct PaintingArt* a = &paintingArts[pt->art];
	struct AABB blockBB;
	int x, y, z, i, j;
	int cellsX = a->sizeX / 16, cellsY = a->sizeY / 16;
	int bx = pt->tileX, bz = pt->tileZ, by;
	BlockID b;

	/* 1. no solid block collision boxes intersecting the painting box */
	for (y = (int)pt->bb.Min.y; y <= (int)pt->bb.Max.y; y++) {
		for (z = (int)pt->bb.Min.z; z <= (int)pt->bb.Max.z; z++) {
			for (x = (int)pt->bb.Min.x; x <= (int)pt->bb.Max.x; x++) {
				if (!World_Contains(x, y, z)) continue;
				b = World_GetBlock(x, y, z);
				if (Blocks.Collide[b] != COLLIDE_SOLID) continue;
				Vec3_Set(blockBB.Min, x + Blocks.MinBB[b].x, y + Blocks.MinBB[b].y, z + Blocks.MinBB[b].z);
				Vec3_Set(blockBB.Max, x + Blocks.MaxBB[b].x, y + Blocks.MaxBB[b].y, z + Blocks.MaxBB[b].z);
				if (AABB_Intersects(&pt->bb, &blockBB)) return false;
			}
		}
	}

	/* 2. every 16px cell must be backed by a solid-material wall block */
	if (pt->dir == 0 || pt->dir == 2) bx = (int)(pt->pos.x - a->sizeX / 32.0f);
	else                              bz = (int)(pt->pos.z - a->sizeX / 32.0f);
	by = (int)(pt->pos.y - a->sizeY / 32.0f);

	for (i = 0; i < cellsX; i++) {
		for (j = 0; j < cellsY; j++) {
			if (pt->dir != 0 && pt->dir != 2) {
				b = World_Contains(pt->tileX, by + j, bz + i) ?
					World_GetBlock(pt->tileX, by + j, bz + i) : BLOCK_AIR;
			} else {
				b = World_Contains(bx + i, by + j, pt->tileZ) ?
					World_GetBlock(bx + i, by + j, pt->tileZ) : BLOCK_AIR;
			}
			/* Material.isSolid - liquids and plants are not */
			if (Blocks.Collide[b] != COLLIDE_SOLID) return false;
		}
	}

	/* 3. no other painting overlapping */
	for (i = 0; i < PAINTING_MAX; i++) {
		if (!st_paintings[i].active || &st_paintings[i] == pt) continue;
		if (AABB_Intersects(&pt->bb, &st_paintings[i].bb)) return false;
	}
	return true;
}

static void Painting_PopOff(struct PaintingEntity* pt) {
	pt->active = false;
	SurvivalTest_SpawnDropAt(pt->pos, INDEV_ITEM_PAINTING, 1);
}

/* ItemPainting.onItemUse: side faces only, interior blocks only; tries
    every art on this wall spot and picks a random one that fits. */
cc_bool SurvivalTest_TryPlacePainting(IVec3 wall, Face face) {
	struct PaintingEntity probe;
	cc_uint8 valid[Array_Elems(paintingArts)];
	int i, validCount = 0, slot = -1, dir;

	if (!IndevTest_Enabled) return false;
	if (st_inv[Inventory.SelectedIndex].id != INDEV_ITEM_PAINTING) return false;
	if (st_inv[Inventory.SelectedIndex].count <= 0)                return false;

	switch (face) {
	case FACE_ZMIN: dir = 0; break;
	case FACE_XMIN: dir = 1; break;
	case FACE_ZMAX: dir = 2; break;
	case FACE_XMAX: dir = 3; break;
	default: return false; /* genuine rejects floors/ceilings */
	}
	if (!(wall.x > 0 && wall.y > 0 && wall.z > 0 &&
		wall.x < World.Width - 1 && wall.y < World.Height - 1 && wall.z < World.Length - 1))
		return false;

	Mem_Set(&probe, 0, sizeof(probe));
	probe.dir   = (cc_uint8)dir;
	probe.tileX = (cc_int16)wall.x;
	probe.tileY = (cc_int16)wall.y;
	probe.tileZ = (cc_int16)wall.z;

	for (i = 0; i < (int)Array_Elems(paintingArts); i++) {
		probe.art = (cc_uint8)i;
		Painting_SetDirection(&probe);
		if (Painting_ValidSurface(&probe)) valid[validCount++] = (cc_uint8)i;
	}
	/* click is consumed for any side face, like genuine onItemUse */
	if (!validCount) return true;

	for (i = 0; i < PAINTING_MAX; i++) {
		if (!st_paintings[i].active) { slot = i; break; }
	}
	if (slot < 0) return true;

	probe.art = valid[Random_Next(&st_dropRng, validCount)];
	Painting_SetDirection(&probe);
	probe.active      = true;
	probe.tickCounter = 0;
	st_paintings[slot] = probe;

	SurvivalTest_ConsumeHeld();
	return true;
}

/* the genuine once-at-100-ticks surface check (see the header comment) */
static void SurvivalTest_TickPaintings(void) {
	int i;
	for (i = 0; i < PAINTING_MAX; i++) {
		struct PaintingEntity* pt = &st_paintings[i];
		if (!pt->active) continue;
		if (pt->tickCounter++ == 100 && !Painting_ValidSurface(pt)) {
			Painting_PopOff(pt);
		}
	}
}

/* melee/arrow hits: any hit pops the painting off as an item (genuine
    attackEntityFrom). Ray test = simple slab test against the (axis-
    aligned) painting box. */
static cc_bool Painting_RayHit(struct PaintingEntity* pt, Vec3 origin, Vec3 dir, float* tHit) {
	float tmin = 0.0f, tmax = 1.0e30f;
	float o[3], d[3], mn[3], mx[3];
	int i;
	o[0]=origin.x; o[1]=origin.y; o[2]=origin.z;
	d[0]=dir.x;    d[1]=dir.y;    d[2]=dir.z;
	mn[0]=pt->bb.Min.x; mn[1]=pt->bb.Min.y; mn[2]=pt->bb.Min.z;
	mx[0]=pt->bb.Max.x; mx[1]=pt->bb.Max.y; mx[2]=pt->bb.Max.z;

	for (i = 0; i < 3; i++) {
		float t1, t2, tmp;
		if (d[i] == 0.0f) {
			if (o[i] < mn[i] || o[i] > mx[i]) return false;
			continue;
		}
		t1 = (mn[i] - o[i]) / d[i];
		t2 = (mx[i] - o[i]) / d[i];
		if (t1 > t2) { tmp = t1; t1 = t2; t2 = tmp; }
		if (t1 > tmin) tmin = t1;
		if (t2 < tmax) tmax = t2;
		if (tmin > tmax) return false;
	}
	*tHit = tmin;
	return true;
}

static cc_bool SurvivalTest_TryPunchPainting(Vec3 eyePos, Vec3 dir, float maxDist) {
	struct PaintingEntity* best = NULL;
	float t, bestT = maxDist;
	int i;
	for (i = 0; i < PAINTING_MAX; i++) {
		if (!st_paintings[i].active) continue;
		if (!Painting_RayHit(&st_paintings[i], eyePos, dir, &t)) continue;
		if (t < bestT) { bestT = t; best = &st_paintings[i]; }
	}
	if (!best) return false;
	Painting_PopOff(best);
	return true;
}

static cc_bool SurvivalTest_ArrowHitPainting(Vec3 pos) {
	int j;
	for (j = 0; j < PAINTING_MAX; j++) {
		if (!st_paintings[j].active) continue;
		if (pos.x < st_paintings[j].bb.Min.x || pos.x > st_paintings[j].bb.Max.x) continue;
		if (pos.y < st_paintings[j].bb.Min.y || pos.y > st_paintings[j].bb.Max.y) continue;
		if (pos.z < st_paintings[j].bb.Min.z || pos.z > st_paintings[j].bb.Max.z) continue;
		Painting_PopOff(&st_paintings[j]);
		return true;
	}
	return false;
}

/* .mclevel: iterate live paintings / restore one */
int SurvivalTest_PaintingNext(int prev, IVec3* tile, int* dir, const char** motive, Vec3* pos) {
	int i;
	for (i = prev + 1; i < PAINTING_MAX; i++) {
		if (!st_paintings[i].active) continue;
		tile->x = st_paintings[i].tileX;
		tile->y = st_paintings[i].tileY;
		tile->z = st_paintings[i].tileZ;
		*dir    = st_paintings[i].dir;
		*motive = paintingArts[st_paintings[i].art].title;
		*pos    = st_paintings[i].pos;
		return i;
	}
	return -1;
}

void SurvivalTest_RestorePainting(int tileX, int tileY, int tileZ, int dir, const cc_string* motive) {
	struct PaintingEntity* pt = NULL;
	int i;
	if (!IndevTest_Enabled) return;

	for (i = 0; i < PAINTING_MAX; i++) {
		if (!st_paintings[i].active) { pt = &st_paintings[i]; break; }
	}
	if (!pt) return;

	Mem_Set(pt, 0, sizeof(*pt));
	pt->tileX = (cc_int16)tileX; pt->tileY = (cc_int16)tileY; pt->tileZ = (cc_int16)tileZ;
	pt->dir   = (cc_uint8)(dir & 3);
	pt->art   = 0; /* genuine falls back to Kebab on unknown titles */
	for (i = 0; i < (int)Array_Elems(paintingArts); i++) {
		if (String_CaselessEqualsConst(motive, paintingArts[i].title)) pt->art = (cc_uint8)i;
	}
	Painting_SetDirection(pt);
	pt->active = true;
}

/* RenderPainting: one 16px cell at a time - the front quad samples the art
    (u mirrored, right to left like genuine), the back the canvas cell at
    (192..208, 0..16), and the four edges the thin strip at u=385/512.
    Every cell is lit individually from the block it hangs over. */
#define PAINTING_CELL_VERTS (6 * 4)
#define PAINTING_MAX_CELL_QUADS (4 * 4)
static GfxResourceID st_paintingVB;

static void SurvivalTest_RenderPaintings(void) {
	struct VertexTextured verts[PAINTING_CELL_VERTS * PAINTING_MAX_CELL_QUADS];
	struct VertexTextured* v;
	const struct PaintingArt* a;
	Vec3 along, up, norm;
	int i, cx, cy, count;
	GfxResourceID tex = IndevTest_KzTex();
	if (!IndevTest_Enabled || !tex) return;

	if (!st_paintingVB) {
		st_paintingVB = Gfx_CreateDynamicVb(VERTEX_FORMAT_TEXTURED,
							PAINTING_CELL_VERTS * PAINTING_MAX_CELL_QUADS);
		if (!st_paintingVB) return;
	}
	Gfx_BindTexture(tex);
	Gfx_SetVertexFormat(VERTEX_FORMAT_TEXTURED);

	for (i = 0; i < PAINTING_MAX; i++) {
		struct PaintingEntity* pt = &st_paintings[i];
		float halfW, halfH;
		if (!pt->active) continue;
		a = &paintingArts[pt->art];

		/* local +x (along wall), +y (up), local -z (out of the wall) */
		switch (pt->dir) {
		case 0:  Vec3_Set(along,  1,0,0); Vec3_Set(norm, 0,0,-1); break;
		case 1:  Vec3_Set(along,  0,0,-1); Vec3_Set(norm, -1,0,0); break;
		case 2:  Vec3_Set(along, -1,0,0); Vec3_Set(norm, 0,0, 1); break;
		default: Vec3_Set(along,  0,0,1); Vec3_Set(norm,  1,0,0); break;
		}
		Vec3_Set(up, 0,1,0);
		halfW = a->sizeX / 32.0f;
		halfH = a->sizeY / 32.0f;

		v = verts; count = 0;
		for (cx = 0; cx < a->sizeX / 16; cx++) {
			for (cy = 0; cy < a->sizeY / 16; cy++) {
				/* cell corners in local units (blocks) */
				float x1 = -halfW + (cx + 1),  x0 = -halfW + cx;
				float y1 = -halfH + (cy + 1),  y0 = -halfH + cy;
				float zF = -0.5f/16.0f, zB = 0.5f/16.0f;
				/* genuine art UVs: right-to-left across the art region */
				float uA1 = (a->offX + a->sizeX - (cx << 4))       / 256.0f;
				float uA0 = (a->offX + a->sizeX - ((cx + 1) << 4)) / 256.0f;
				float vA1 = (a->offY + a->sizeY - (cy << 4))       / 256.0f;
				float vA0 = (a->offY + a->sizeY - ((cy + 1) << 4)) / 256.0f;
				/* per-cell lighting from the block the cell centres on */
				float ccx = (x0 + x1) * 0.5f, ccy = (y0 + y1) * 0.5f;
				int lx = (int)pt->pos.x, ly = (int)(pt->pos.y + ccy), lz = (int)pt->pos.z;
				float br;
				PackedCol col;
				if (pt->dir == 0) lx = (int)(pt->pos.x + ccx);
				if (pt->dir == 1) lz = (int)(pt->pos.z - ccx);
				if (pt->dir == 2) lx = (int)(pt->pos.x - ccx);
				if (pt->dir == 3) lz = (int)(pt->pos.z + ccx);
				br  = Indev_LightBrightness(lx, ly, lz);
				col = PackedCol_Make((cc_uint8)(br * 255), (cc_uint8)(br * 255),
				                     (cc_uint8)(br * 255), 255);

				/* world = centre + X*along + Y*up + Z*(local z axis = -norm) */
				#define PV(X, Y, Z, PU, PVV) \
					v->x = pt->pos.x + (X)*along.x + (Y)*up.x - (Z)*norm.x; \
					v->y = pt->pos.y + (X)*along.y + (Y)*up.y - (Z)*norm.y; \
					v->z = pt->pos.z + (X)*along.z + (Y)*up.z - (Z)*norm.z; \
					v->Col = col; v->U = (PU); v->V = (PVV); v++;
				/* front (toward the room) */
				PV(x1,y0,zF, uA0,vA1) PV(x0,y0,zF, uA1,vA1) PV(x0,y1,zF, uA1,vA0) PV(x1,y1,zF, uA0,vA0)
				/* back canvas */
				PV(x1,y1,zB, 12.0f/16,0.0f) PV(x0,y1,zB, 13.0f/16,0.0f) PV(x0,y0,zB, 13.0f/16,1.0f/16) PV(x1,y0,zB, 12.0f/16,1.0f/16)
				/* top edge */
				PV(x1,y1,zF, 12.0f/16,0.001953125f) PV(x0,y1,zF, 13.0f/16,0.001953125f) PV(x0,y1,zB, 13.0f/16,0.001953125f) PV(x1,y1,zB, 12.0f/16,0.001953125f)
				/* bottom edge */
				PV(x1,y0,zB, 12.0f/16,0.001953125f) PV(x0,y0,zB, 13.0f/16,0.001953125f) PV(x0,y0,zF, 13.0f/16,0.001953125f) PV(x1,y0,zF, 12.0f/16,0.001953125f)
				/* left/right edges (the 385/512 strip) */
				PV(x1,y1,zB, 385.0f/512,0.0f) PV(x1,y0,zB, 385.0f/512,1.0f/16) PV(x1,y0,zF, 385.0f/512,1.0f/16) PV(x1,y1,zF, 385.0f/512,0.0f)
				PV(x0,y1,zF, 385.0f/512,0.0f) PV(x0,y0,zF, 385.0f/512,1.0f/16) PV(x0,y0,zB, 385.0f/512,1.0f/16) PV(x0,y1,zB, 385.0f/512,0.0f)
				#undef PV
				count += PAINTING_CELL_VERTS;
			}
		}
		Gfx_SetDynamicVbData(st_paintingVB, verts, count);
		Gfx_DrawVb_IndexedTris(count);
	}
}


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
/* Repaint hook for the Indev layer's NET container appliers (CONT_SLOT /
    FURN_PROG land in IndevTest state the inventory screen renders). */
void    SurvivalTest_MarkInvDirty(void) { st_invVersion++; }
/* Public bump so the Indev layer (furnace tick mutating container slots) */
/*  can tell the open inventory screen to rebuild its mesh. */
void    SurvivalTest_InvChanged(void)  { st_invVersion++; }

static void SurvivalTest_SyncHotbar(void);

/* .mclevel load: restores one inventory slot (id 0 clears the slot). */
/* Slots 100..103 are the genuine armor numbering (armorInventory[slot-100]). */
void SurvivalTest_RestoreSlot(int slot, int id, int count, int damage) {
	struct SurvivalSlot* p;
	if (!SurvivalTest_Enabled) return;
	if (count <= 0) id = 0;

	if (slot >= 100 && slot < 100 + SURVIVAL_ARMOR_SLOTS) {
		p = &st_armor[slot - 100];
	} else if (slot >= 0 && slot < SURVIVAL_INV_SLOTS) {
		p = &st_inv[slot];
	} else {
		return;
	}

	p->id     = (cc_uint16)id;
	p->count  = (cc_int16)(id ? count : 0);
	p->damage = (cc_int16)damage;
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
	/* Plain-server stash: NEVER blanket-write the engine hotbar - an extended
	    block (id >= 256) mirrors as an empty survival slot, and syncing that
	    would wrongly clear it. Changed slots write through per-click instead
	    (SurvivalTest_PlainWriteThrough). */
	if (!SurvivalTest_Enabled) { st_invVersion++; return; }
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
	if (idx >= SURVIVAL_ARMOR_BASE && idx < SURVIVAL_ARMOR_BASE + SURVIVAL_ARMOR_SLOTS)
		return &st_armor[idx - SURVIVAL_ARMOR_BASE];
	if (idx >= SURVIVAL_CONTAINER_BASE && idx < SURVIVAL_CONTAINER_BASE + SURVIVAL_CONTAINER_MAX)
		return IndevTest_ContainerSlot(idx - SURVIVAL_CONTAINER_BASE);
	if (idx >= SURVIVAL_CRAFT_BASE && idx < SURVIVAL_CRAFT_BASE + SURVIVAL_CRAFT_SLOTS)
		return &st_craft[idx - SURVIVAL_CRAFT_BASE];
	return &st_inv[idx];
}

int SurvivalTest_ArmorId(int i)     { return st_armor[i].id; }
int SurvivalTest_ArmorCount(int i)  { return st_armor[i].count; }
int SurvivalTest_ArmorDamage(int i) { return st_armor[i].damage; }

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
	cc_uint16 prev[SURVIVAL_HOTBAR_SLOTS];
	int space, moved, i;
	cc_bool plain = SurvivalTest_PlainInvActive();
	if (!SurvivalTest_Enabled && !plain) return;
	/* plain mode: engine hotbar is the authority - capture the row so only
	    slots this click actually changed get written through afterwards */
	if (plain) { for (i = 0; i < SURVIVAL_HOTBAR_SLOTS; i++) prev[i] = st_inv[i].id; }

	/* SlotArmor.isItemValid: an armor slot only ACCEPTS its matching piece
	    (array index 0 boots .. 3 helmet, piece type = 3 - index). Taking
	    out is always allowed. */
	if (idx >= SURVIVAL_ARMOR_BASE && idx < SURVIVAL_ARMOR_BASE + SURVIVAL_ARMOR_SLOTS &&
		st_cursor.count > 0 &&
		IndevTest_ArmorPiece(st_cursor.id) != 3 - (idx - SURVIVAL_ARMOR_BASE)) return;
	/* SlotFurnace (the output, container slot 2): TAKE-ONLY - placing into
	    it (incl. merging onto the smelted stack) is refused, like genuine
	    (user-reported; the same rule rides the MP intent server-side). */
	if (idx == SURVIVAL_CONTAINER_BASE + 2 && st_cursor.count > 0 &&
		IndevTest_OpenKind() == INDEV_CONTAINER_FURNACE) return;

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
	if (plain) SurvivalTest_PlainWriteThrough(prev);
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
	cc_uint16 prev[SURVIVAL_HOTBAR_SLOTS];
	int i;
	cc_bool plain = SurvivalTest_PlainInvActive();
	if (plain) { for (i = 0; i < SURVIVAL_HOTBAR_SLOTS; i++) prev[i] = st_inv[i].id; }

	while (st_cursor.count > 0 && SurvivalTest_AddItem(st_cursor.id)) st_cursor.count--;
	if (st_cursor.count <= 0) { st_cursor.id = BLOCK_AIR; st_cursor.damage = 0; }
	st_invVersion++;
	SurvivalTest_SyncHotbar();
	if (plain) SurvivalTest_PlainWriteThrough(prev);
}

/*########################################################################################################################*
*--------------------------------------------MP inventory view (phase 4)--------------------------------------------------*
*#########################################################################################################################*/
/* st_inv/st_craft/st_armor + st_cursor as a server-driven view (networking-plan
    27): the SERVER owns every slot and the cursor; clicks leave as intents
    (SurvivalNet_SendSlotClick and friends) and these appliers write the echoed
    authoritative result. No local mutation happens on the click itself in MP
    (echo-only v1 - optimistic prediction is a later polish). */

void SurvivalTest_NetInvSlot(int idx, int id, int count, int dmg) {
	struct SurvivalSlot* p;
	if (!SurvivalTest_Enabled) return;
	/* MP creative: the inventory is the local palette - a stray stream from
	    the server (e.g. sent before a live creative flip landed) must not
	    wipe it. The server also skips these sends on creative maps. */
	if (SurvivalTest_CreativeActive()) return;
	if (idx < 0 || idx >= SURVIVAL_ARMOR_BASE + SURVIVAL_ARMOR_SLOTS) return;
	/* container slots aren't streamed yet (rest of phase 4) */
	if (idx >= SURVIVAL_CONTAINER_BASE && idx < SURVIVAL_CONTAINER_BASE + SURVIVAL_CONTAINER_MAX) return;

	p = SurvivalTest_SlotPtr(idx);
	p->id     = (cc_uint16)id;
	p->count  = (cc_int16)count;
	p->damage = (cc_int16)dmg;
	if (p->count <= 0) { p->id = BLOCK_AIR; p->count = 0; p->damage = 0; }
	st_invVersion++;
	SurvivalTest_SyncHotbar();
}

void SurvivalTest_NetCursor(int id, int count, int dmg) {
	if (!SurvivalTest_Enabled) return;
	if (SurvivalTest_CreativeActive()) return; /* local palette - see NetInvSlot */
	st_cursor.id     = (cc_uint16)id;
	st_cursor.count  = (cc_int16)count;
	st_cursor.damage = (cc_int16)dmg;
	if (st_cursor.count <= 0) { st_cursor.id = BLOCK_AIR; st_cursor.count = 0; st_cursor.damage = 0; }
	st_invVersion++;
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

int SurvivalTest_CraftSlotId(int i)     { return st_craft[i].id; }
int SurvivalTest_CraftSlotCount(int i)  { return st_craft[i].count; }
int SurvivalTest_CraftSlotDamage(int i) { return st_craft[i].damage; }

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
	/* MP: dropping is an intent - the server owns the inventory and the drop
	    entity (phases 4/5); it spawns the drop and streams it back. MP
	    creative keeps the local palette inventory, so drops stay local too. */
	if (SurvivalTest_ServerOwnsInventory()) {
		SurvivalNet_SendDropItem(slot, false);
		return;
	}
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

void SurvivalTest_CreativeGive(int id) {
	int n, max;
	if (!SurvivalTest_CreativeActive() || id == BLOCK_AIR) return;
	/* Deposit one full stack of the picked id (Item.getItemStackLimit) into the
	    inventory - AddItem tops up a matching stack or fills the first free slot. */
	max = ST_MaxStack((cc_uint16)id);
	for (n = 0; n < max; n++) {
		if (!SurvivalTest_AddItem((cc_uint16)id)) break; /* inventory full */
	}
	st_invVersion++; /* redraw an open Indev inventory screen */
}

/* Public wrappers for the Indev layer's held-item actions (hoe wear, */
/*  seed consumption). */
void SurvivalTest_DamageHeldItem(int amount) { SurvivalTest_DamageHeldTool(amount); }

/* PlayerControllerSP.sendBlockRemoved runs Item.onBlockDestroyed for EVERY
    removal - including clickBlock's instant path - so insta-breaking a
    hardness-0 block (flowers, saplings, torches, crops, TNT...) wears the
    held tool exactly like a mined block. No-ops outside Indev survival
    (DamageHeldTool is Indev-gated and ToolUseWear returns 0 for non-tools). */
void SurvivalTest_WearHeldToolForBlockBreak(void) {
	if (!SurvivalTest_Enabled) return;
	if (SurvivalNet_ServerDriven()) return; /* MP: durability is server state */
	SurvivalTest_DamageHeldTool(
		IndevTest_ToolUseWear(st_inv[Inventory.SelectedIndex].id, false));
}
void SurvivalTest_ConsumeHeld(void)          { if (!SurvivalNet_ServerDriven()) SurvivalTest_ConsumeSelected(); }
/* Fire consuming a TNT block arms it (onBlockDestroyedByPlayer) */
void SurvivalTest_IgniteTnt(IVec3 coords)    { if (SurvivalTest_Enabled && !SurvivalNet_ServerDriven()) SurvivalTest_ArmTnt(coords, TNT_FUSE_DEFAULT()); }

/* .mclevel entity save: iterates live mobs (returns the next active index */
/*  after prev, or -1) and physical item drops, for the Entities list. */
int SurvivalTest_MobNext(int prev, int* type, Vec3* pos, float* yaw, int* health) {
	int i;
	for (i = prev + 1; i < MOB_MAX; i++) {
		if (!st_mobs[i].active || st_mobs[i].health <= 0) continue;
		*type   = st_mobs[i].type;
		*pos    = st_mobs[i].Base.Position;
		*yaw    = st_mobs[i].Base.Yaw;
		*health = st_mobs[i].health;
		return i;
	}
	return -1;
}

int SurvivalTest_DropNext(int prev, Vec3* pos, int* id, int* count) {
	int i;
	for (i = prev + 1; i < DROP_MAX; i++) {
		if (!st_drops[i].active) continue;
		*pos   = st_drops[i].position;
		*id    = st_drops[i].block;
		*count = st_drops[i].count;
		return i;
	}
	return -1;
}

/* .mclevel entity load: respawns a saved mob with its health. */
void SurvivalTest_RestoreMob(int type, Vec3 pos, float yaw, int health) {
	struct Mob* m;
	if (!SurvivalTest_Enabled) return;
	if (type < 0 || type >= MOB_TYPE_COUNT) return;

	m = SurvivalTest_SpawnMobAt((cc_uint8)type, pos);
	if (!m) return;
	m->Base.Yaw = yaw;
	if (health > 0 && health <= m->health) m->health = health;
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

	/* MP: containers AND item-on-block uses (hoe tilling, seed planting) are
	    SERVER state - the right-click leaves as a SURV_USE_ITEM intent and the
	    server answers with SURV_CONT_OPEN / the resulting block + inventory
	    changes. The click is consumed like genuine blockActivated/onItemUse. */
	if (SurvivalNet_ServerDriven()) {
		int heldId = st_inv[Inventory.SelectedIndex].id;
		cc_bool container = IndevTest_IsWorkbench(block) || IndevTest_IsContainerBlock(block);
		cc_bool itemUse   = IndevTest_IsHoe(heldId) || heldId == 256 + 39  /* Seeds */
		                 || heldId == 256 + 3; /* Flint & steel -> server places fire */
		if (!container && !itemUse) return false;
		SurvivalNet_SendUseItem(Inventory.SelectedIndex, pos.x, pos.y, pos.z,
		                        (int)Game_SelectedPos.closest);
		return true;
	}

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

	/* Item.onItemUse comes after blockActivated: hoe tilling, seed planting, */
	/*  and hanging paintings on the clicked wall face */
	if (SurvivalTest_TryPlacePainting(pos, Game_SelectedPos.closest)) return true;
	if (IndevTest_UseHeldItem(st_inv[Inventory.SelectedIndex].id, pos)) return true;
	return false;
}

cc_bool SurvivalTest_TryEat(void) {
	int slot;
	BlockID block;
	if (!SurvivalTest_Enabled) return false;

	/* MP: eating is a SERVER intent. A held food leaves as a targetless
	    SURV_USE_ITEM (sentinel coords) so the server takes the eat path
	    (heal + consume, soup -> bowl) and pushes SURV_HEALTH + the slot. */
	if (SurvivalNet_ServerDriven()) {
		slot = Inventory.SelectedIndex;
		if (st_inv[slot].count > 0 && IndevTest_ItemFoodHeal(st_inv[slot].id) > 0) {
			SurvivalNet_SendUseItem(slot, -1, -1, -1, 0xFF);
			return true;
		}
		return false;
	}

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
		cc_bool soup = st_inv[slot].id == 256 + 26; /* mushroom soup */
		if (heal > 0) {
			SurvivalTest_Heal(heal);
			SurvivalTest_ConsumeSelected();
			/* ItemSoup.onItemRightClick returns new ItemStack(bowlEmpty):
			    the bowl stays behind in the eaten soup's slot (soups don't
			    stack, so the slot is guaranteed free after the consume) */
			if (soup && st_inv[slot].count <= 0) {
				st_inv[slot].id     = 256 + 25; /* Bowl */
				st_inv[slot].count  = 1;
				st_inv[slot].damage = 0;
				SurvivalTest_InvChanged();
				SurvivalTest_SyncHotbar();
			}
			HeldBlockRenderer_ClickAnim(false);
			return true;
		}
	}

	if (IndevTest_Enabled) {
		/* mushroom-eating is c0.30 SurvivalGameMode.useItem only - Indev
		    mushrooms are just soup ingredients */
		return false;
	} else if (block == BLOCK_BROWN_SHROOM) {
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

	/* Any block change re-validates the hanging paintings, so breaking the
	    wall behind one pops it off immediately (user request - genuine
	    in-20100223 only ever re-checks at tickCounter==100 and lets a
	    painting hang forever if its wall breaks later, which reads as a
	    bug; b1.x made the check periodic). */
	if (IndevTest_Enabled) {
		int pi;
		for (pi = 0; pi < PAINTING_MAX; pi++) {
			if (!st_paintings[pi].active) continue;
			if (!Painting_ValidSurface(&st_paintings[pi])) Painting_PopOff(&st_paintings[pi]);
		}
	}

	/* MP: drops and the inventory are server state (phases 4/5) - never spawn
	    a local drop or debit a local stack for a block change on a server map. */
	if (SurvivalNet_ServerDriven()) return;

	if (block == BLOCK_AIR) {
		/* Block was mined - spawn its physical drop(s) on the ground. Creative */
		/*  never drops (base PlayerController.sendBlockRemoved has no dropItem). */
		if (!SurvivalTest_CreativeActive())
			SurvivalTest_SpawnDropsForBlock(coords, oldBlock);
	} else if (!SurvivalTest_CreativeActive()) {
		/* Block was placed - consume one from the selected hotbar slot. */
		/* Guarded so a placement that didn't come from the held slot (or a */
		/*  slot holding an item id) can never eat the wrong stack. Creative */
		/*  places from an infinite supply, so it never consumes. */
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
		case BLOCK_DIRT:        return 10;  /* setData 0.5 */
		case BLOCK_COBBLE:      return 30;  /* 1.5s */
		case BLOCK_WOOD:        return 30;  /* 1.5s (planks) */
		case BLOCK_BEDROCK:     return 19980; /* 999.0s - effectively unbreakable */
		case BLOCK_WATER: case BLOCK_STILL_WATER:
		case BLOCK_LAVA:  case BLOCK_STILL_LAVA:
			return 2000; /* 100.0s */
		case BLOCK_SAND:        return 10;  /* setData 0.5 */
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
			return 40;  /* setData 2.0 */
		case BLOCK_BRICK:       return 40;  /* setData 2.0 */
		case BLOCK_BOOKSHELF:   return 30;  /* 1.5s */
		case BLOCK_MOSSY_ROCKS: return 20;  /* 1.0s */
		case BLOCK_OBSIDIAN:    return 200; /* 10.0s */
		/* DANDELION, ROSE, both mushrooms, SAPLING and TNT are explicit */
		/*  hardness 0 in Block.java's static init - instant break. */
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
	if (!SurvivalTest_Enabled)         return true;
	if (SurvivalTest_CreativeActive()) return true; /* creative: instant-delete every block */
	return SurvivalTest_Hardness(block) == 0;
}

/* ItemStack.damageItem: the held tool takes wear (1 per block broken, 2 per */
/*  landed melee hit) and shatters at ItemTool's maxDamage (32 << tier). */
static void SurvivalTest_DamageHeldTool(int amount) {
	int slot = Inventory.SelectedIndex;
	int maxDamage;
	if (!IndevTest_Enabled || amount <= 0) return;

	maxDamage = IndevTest_ToolMaxDamage(st_inv[slot].id);
	if (!maxDamage) return;

	st_inv[slot].damage += amount;
	if (st_inv[slot].damage > maxDamage) { /* damageItem: strictly greater */
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
/* PlayerControllerSP.curBlockDamage - Indev's float progress accumulator */
static float st_breakDamage;
static int st_breakDelay;

/* Block.blockStrength(EntityPlayer): per-TICK dig progress, accumulated to */
/*  1.0 to break. Bedrock (genuine hardness -1) never progresses; hardness 0 */
/*  is instant (genuine divides by zero -> +Inf); a block the player cannot */
/*  harvest digs at 1/hardness/100 with NO tool speed and NO penalties; */
/*  otherwise tool speed is /5 with the head in water and /5 again airborne, */
/*  then /hardness/30. Our hardness table stores genuine seconds * 20. */
static float Indev_BlockStrength(BlockID block) {
	struct Entity* e = &Entities.CurPlayer->Base;
	int held = st_inv[Inventory.SelectedIndex].id;
	int h    = SurvivalTest_Hardness(block);
	float hardness, str;

	if (block == BLOCK_BEDROCK) return 0.0f; /* genuine -1 sentinel */
	if (h == 0) return 1.0f;
	hardness = h / 20.0f;

	if (!IndevTest_CanHarvest(held, block)) return 1.0f / hardness / 100.0f;

	str = IndevTest_StrVsBlock(held, block);
	if (SurvivalTest_IsHeadInWater(e)) str /= 5.0f;
	if (!e->OnGround)                  str /= 5.0f;
	return str / hardness / 30.0f;
}

/* SurvivalGameMode.applyBlockCracks: cracks = (hits + time - 1) / hardness, */
/*  0 when no hits yet. (time, the render partial-tick, is omitted here as */
/*  this is only sampled once per frame at an arbitrary phase - the -1/hardness */
/*  offset and denominator are what visibly set the crack stages.) */
float SurvivalTest_BreakProgress(void) {
	int hardness;
	if (IndevTest_Enabled) {
		/* setPartialTime: the crack overlay reads curBlockDamage directly */
		if (!st_breaking || st_breakDamage <= 0.0f) return 0.0f;
		return st_breakDamage > 1.0f ? 1.0f : st_breakDamage;
	}
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

/* c0.30 SurvivalGameMode.hitBlock(x,y,z,side)/resetHits() - runs every tick */
/*  while the left mouse button is held down and the player is aiming at a */
/*  block. Hits accumulate on whichever block was targeted last tick; aiming at */
/*  a different block resets the count. Reaching hardness+1 hits breaks the */
/*  block and starts a 5-tick cooldown (hitDelay) before the next block can */
/*  start accumulating hits. Indev's PlayerControllerSP.sendBlockRemoving has */
/*  the same lifecycle (incl. blockHitWait = 5) but accumulates the float */
/*  Block.blockStrength per tick and breaks at curBlockDamage >= 1.0. */
static void SurvivalTest_TickBreaking(void) {
	IVec3 pos;
	BlockID block, old;
	int hardness;
	cc_bool holding = !Gui.InputGrab && Input.Pressed[CCMOUSE_L];

	if (!holding || !Game_SelectedPos.valid) {
		st_breaking   = false;
		st_breakHits  = 0;
		st_breakDamage = 0.0f;
		st_breakDelay = 0;
		return;
	}

	if (st_breakDelay > 0) { st_breakDelay--; return; }
	pos = Game_SelectedPos.pos;

	if (st_breaking && pos.x == st_breakPos.x && pos.y == st_breakPos.y && pos.z == st_breakPos.z) {
		if (!World_Contains(pos.x, pos.y, pos.z)) {
			st_breaking = false; st_breakHits = 0; st_breakDamage = 0.0f; return;
		}

		block = World_GetBlock(pos.x, pos.y, pos.z);
		if (Blocks.Draw[block] == DRAW_GAS || !Blocks.CanDelete[block]) {
			st_breaking = false; st_breakHits = 0; st_breakDamage = 0.0f; return;
		}

		/* Indev: curBlockDamage += blockStrength per tick, break at 1.0. */
		/* c0.30: integer hits, break at hardness + 1 (and NO hit sound -
		    genuine SurvivalGameMode.hitBlock only spawns particles). */
		if (IndevTest_Enabled) {
			/* sendBlockRemoving: every 4th digging tick plays the block's
			    step sound at quarter volume, half pitch (the mining thunk).
			    st_breakHits is unused by the Indev damage path, so it serves
			    as the genuine blockDestroySoundCounter here. */
			if ((st_breakHits++ % 4) == 0) {
				Audio_PlayDigHitSound(Blocks.StepSounds[block]);
			}
			st_breakDamage += Indev_BlockStrength(block);
			if (st_breakDamage < 1.0f) return;
		} else {
			hardness = SurvivalTest_Hardness(block);
			st_breakHits++;
			if (st_breakHits < hardness + 1) return;
		}

		/* onBlockDestroyed: tools wear 1, swords 2, hoes/flint&steel none */
		SurvivalTest_DamageHeldTool(
			IndevTest_ToolUseWear(st_inv[Inventory.SelectedIndex].id, false));
		old = block;
		Game_ChangeBlock(pos.x, pos.y, pos.z, BLOCK_AIR);
		Event_RaiseBlock(&UserEvents.BlockChanged, pos, old, BLOCK_AIR);

		st_breaking    = false;
		st_breakHits   = 0;
		st_breakDamage = 0.0f;
		st_breakDelay  = 5;
	} else {
		st_breaking    = true;
		st_breakHits   = 0;
		st_breakDamage = 0.0f;
		st_breakPos    = pos;
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
				/* EntityLiving.fall: the damaging landing plays the block-
				    under's step sound at half volume, 3/4 pitch (Indev only) */
				if (IndevTest_Enabled && damage > 0) {
					int bx = Math_Floor(e->next.pos.x);
					int by = Math_Floor(e->next.pos.y - 0.2f);
					int bz = Math_Floor(e->next.pos.z);
					if (World_Contains(bx, by, bz))
						Audio_PlayFallSound(Blocks.StepSounds[World_GetBlock(bx, by, bz)]);
				}
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

/* Entity.distanceWalkedModified/nextStepDistance for the local player - the
    step-event cadence that drives farmland trampling (the engine's footstep
    sounds are leg-swing based and can't be reused for this). */
static float st_walkDist;
static int   st_nextStep = 1;
/* Indev Entity.air in ticks (max 300); c0.30 keeps st_airTimer seconds */
static int   st_airTicks = 300;

static void SurvivalTest_Tick(struct ScheduledTask* task) {
	struct LocalPlayer* p;
	struct Entity* e;
	cc_bool onGround, inLava, inWater, headInWater;
	int ei;
	float delta = (float)task->interval;

	/* Remote players' hurt-roll timers (SURV_PLAYER_HURT) decay a flat 1 per
	    tick like every Mob.hurtTime - BEFORE the enabled gate, since a mode-0
	    HELLO can land mid-wobble: the render hook is unconditional, so a
	    ticker frozen by the gate would rock that player forever (the exact
	    freeze st_hurtTicks once had - see the death-branch comment below). */
	for (ei = 0; ei < ENTITIES_SELF_ID; ei++) {
		struct Entity* re = Entities.List[ei];
		if (!re) continue;
		if (re->NetHurtTicks) re->NetHurtTicks--;
		/* death keel ramps UP (saturating well past the 90-degree cap) */
		if (re->NetDeathTicks && re->NetDeathTicks < 200) re->NetDeathTicks++;
	}
	if (!SurvivalTest_Enabled || !World.Loaded) return;
	p = Entities.CurPlayer;
	if (!p) return;
	e = &p->Base;

	/* While dead the Game Over screen is up and the world is frozen, but */
	/*  EntityLiving.onEntityUpdate keeps running: deathTime counts UP (drives */
	/*  the death camera roll + FOV zoom) AND hurtTime still counts DOWN. Missing */
	/*  the hurtTime decrement froze st_hurtTicks at 10, so the per-hit tilt */
	/*  re-fired every frame forever - a perpetual ~20Hz wobble stacked on the */
	/*  keel-roll. Decrement it here so the killing blow's wobble decays over its */
	/*  ~10 ticks and then only the slow keel-roll + zoom remain (genuine). */
	if (st_isDead) {
		/* Death is modal: the only ways out are generating/loading a world (both
		    clear st_isDead on map load) or respawning. But the Game Over screen's
		    "Generate new level" button opens a menu whose cancel/escape chain
		    (-> pause -> back to game) could drop you into live gameplay while still
		    dead. If we ever end up with no menu grabbing input while dead, the
		    screen was bypassed - re-assert it. (It grabs input, so this is a no-op
		    whenever it or any menu is already up.) */
		if (!Gui_GetInputGrab()) GameOverScreen_Show();
		st_deathTicks++;
		if (st_hurtTicks > 0) st_hurtTicks--;
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

	/* Entity.move's step trigger for the PLAYER: distanceWalkedModified
	    accumulates horizontal metres * 0.6 and fires once per whole unit -
	    NOT gated on onGround (genuine isn't) - trampling farmland below the
	    feet (Block.onEntityWalking). e->next.pos is this tick's fresh
	    position; e->Position is one tick stale here (see UpdateFall). */
	if (IndevTest_Enabled && !SurvivalNet_ServerDriven()
			&& !p->Hacks.Flying && !p->Hacks.Noclip) {
		float wdx = e->next.pos.x - e->prev.pos.x;
		float wdz = e->next.pos.z - e->prev.pos.z;
		st_walkDist += Math_SqrtF(wdx * wdx + wdz * wdz) * 0.6f;
		if (st_walkDist > (float)st_nextStep) {
			int fx = Math_Floor(e->next.pos.x);
			int fy = Math_Floor(e->next.pos.y - 0.2f);
			int fz = Math_Floor(e->next.pos.z);
			st_nextStep++;
			if (World_Contains(fx, fy, fz) && World_GetBlock(fx, fy, fz) != BLOCK_AIR) {
				IndevTest_TrampleStep(e->next.pos.x, e->next.pos.y, e->next.pos.z);
			}
		}
	}

	/* Fall damage -------------------------------------------------------- */
	/* Mob.tick resets fallDistance for water ONLY (isInWater) - landing in */
	/*  shallow lava does NOT cushion a fall. */
	if (inWater) st_falling = false;
	SurvivalTest_UpdateFall(e, p, onGround);

	/* Void death: on a floating Indev map you fall through the bottom of the */
	/*  world (World_FallThroughFloor) - past ST_VOID_KILL_Y that is fatal. */
	/*  Creative can't take damage, so instead snap it back to spawn to avoid */
	/*  an endless fall. Flying/noclip are exempt (you're not really falling). */
	/*  MP: the server's hazard detection owns dying, including the void. */
	if (IndevTest_Enabled && !SurvivalNet_ServerDriven()
			&& e->next.pos.y < ST_VOID_KILL_Y
			&& !p->Hacks.Flying && !p->Hacks.Noclip) {
		if (SurvivalTest_CreativeActive()) {
			struct LocationUpdate update;
			update.flags = LU_HAS_POS | LU_HAS_YAW | LU_HAS_PITCH | LU_POS_ABSOLUTE_INSTANT;
			update.pos   = p->Spawn;
			update.yaw   = p->SpawnYaw;
			update.pitch = p->SpawnPitch;
			e->VTABLE->SetLocation(e, &update);
			Vec3_Set(e->Velocity, 0.0f, 0.0f, 0.0f);
		} else if (!st_isDead) {
			/* Mirror a genuine fatal landing: the impact hurt-wobble, then the */
			/*  death state that drives the FOV zoom + keel-over roll (st_deathTicks). */
			/*  Freeze the fall so the death camera is steady, like onDeath's near-stop. */
			Indev_PlaySoundAt(e->Position, MOBSND_HURT, 1.0f, Mob_SndPitch());
			Vec3_Set(e->Velocity, 0.0f, 0.0f, 0.0f);
			st_hurtTicks  = HURT_TILT_TICKS;
			st_hurtDir    = 0.0f;
			st_deathTicks = 0;
			SurvivalTest_Health = 0;
			st_isDead = true;
			SurvivalTest_DropInventory();
			GameOverScreen_Show();
			return;
		}
	}

	/* Lava damage - Mob.tick: hurt(null, 10) every tick; the invulnerability */
	/*  window's dual threshold (see SurvivalTest_Damage) is what shapes this */
	/*  into the effective 10 HP per half-second cadence. */
	if (inLava) SurvivalTest_Hurt(LAVA_DAMAGE);

	/* Standing in a fire block (Indev Entity.move's isBoundingBoxBurning):
	    contact damage every tick (the invulnerability window shapes the
	    cadence, like lava) and the player is set alight; being alight burns
	    1 HP a second while the 300-tick counter runs down, water fizzes it
	    out, lava re-arms it to 600 (Entity.onEntityUpdate). */
	if (IndevTest_Enabled) {
		/* Entity.onEntityUpdate: hitting the water splashes (sound volume
		    scales with entry speed, so wading in barely whispers while a
		    high dive is loud) */
		if (inWater && !st_playerWasInWater)
			Indev_EntitySplash(e->Position, e->Velocity, 0.6f);
		st_playerWasInWater = inWater;

		/* Burning is local sim state - in MP the server owns fire damage and
		    can't tell us we're alight yet, so don't fake the burn overlay. */
		if (!SurvivalNet_ServerDriven()) {
			/* Entity.move tail: fire contact deals 1/tick and RAMPS the fire
			    counter up from -fireResistance (EntityPlayer sets 20) - only
			    after 20 consecutive burning ticks does it reach 0 and catch
			    alight (fire = 300). Leaving fire un-ignited resets the ramp,
			    and the water fizz resets to -fireResistance too - a brief
			    brush with fire never sets the player alight (mobs DO insta-
			    ignite: their fireResistance stays the Entity default 1). */
			if (ST_InFire(e)) {
				SurvivalTest_Hurt(1);
				if (!inWater) {
					st_playerFire++;
					if (st_playerFire == 0) st_playerFire = 300;
				}
			} else if (st_playerFire <= 0) {
				st_playerFire = -PLAYER_FIRE_RESIST;
			}
			if (inWater && st_playerFire > 0) {
				Audio_PlayMobSound(MOBSND_FIZZ, 0.7f,
					1.6f + (Random_Float(&st_mobRng) - Random_Float(&st_mobRng)) * 0.4f, 0.0f);
				st_playerFire = -PLAYER_FIRE_RESIST;
			}
			if (st_playerFire > 0) {
				if (st_playerFire % 20 == 0) SurvivalTest_Hurt(1);
				st_playerFire--;
			}
			if (inLava) st_playerFire = 600;
		}
	}

	/* Drowning. c0.30 Mob.tick: airSupply-- while the head is underwater,
	    then hurt(null, 2) every tick once it's empty (the invuln window
	    shapes that into ~2 per half second); instant refill on surfacing.
	    Indev EntityLiving.onEntityUpdate: --air with the -20 underflow as
	    the damage timer - at -20, 8 bubble particles + 2 damage, air reset
	    to 0 (so first hit 320 ticks under, then exactly every 20). */
	st_headInWater = headInWater; /* exposed to the HUD for the air bubbles */
	if (headInWater) {
		if (IndevTest_Enabled) {
			st_airTicks--;
			if (st_airTicks == -20) {
				Vec3 eye = Entity_GetEyePosition(e);
				int b;
				st_airTicks = 0;
				for (b = 0; b < 8; b++) {
					Indev_SpawnBubbleFX(
						e->Position.x + (Random_Float(&st_mobRng) - Random_Float(&st_mobRng)),
						eye.y         + (Random_Float(&st_mobRng) - Random_Float(&st_mobRng)),
						e->Position.z + (Random_Float(&st_mobRng) - Random_Float(&st_mobRng)),
						e->Velocity.x, e->Velocity.y, e->Velocity.z);
				}
				SurvivalTest_Hurt(DROWN_DAMAGE);
			}
			/* keep the HUD's seconds-based air source coherent (clamped 0) */
			st_airTimer = st_airTicks > 0 ? (float)st_airTicks / 20.0f : 0.0f;
		} else {
			st_airTimer -= delta;
			if (st_airTimer <= 0.0f) {
				st_airTimer = 0.0f;
				SurvivalTest_Hurt(DROWN_DAMAGE);
			}
		}
	} else {
		st_airTicks = 300; /* Entity.maxAir */
		st_airTimer = AIR_SUPPLY_SECS;
	}

	/* Block breaking (dig timing is client presentation in MP too; the final
	    break goes through the normal SetBlock flow the server validates) ----- */
	SurvivalTest_TickBreaking();

	/* World simulation the SERVER owns in MP: drops, mobs, arrows, paintings
	    and TNT all become streamed state (phases 3/5) - running them locally
	    would desync the moment the server starts streaming. ------------------ */
	if (!SurvivalNet_ServerDriven()) {
		SurvivalTest_TickDrops(e, delta);
		SurvivalTest_TickMobs(delta);
		SurvivalTest_TickArrows();
		if (IndevTest_Enabled) SurvivalTest_TickPaintings();
		SurvivalTest_TickTnt();
	} else {
		/* Phase 3: mobs stream in as puppets - only their presentation
		    (interpolation, animation timers, sounds) ticks locally.
		    Phase 5: drops stream in the same way (physics/spin local, the
		    server owns spawn/pickup/despawn). */
		SurvivalTest_TickPuppetMobs(delta);
		SurvivalTest_TickNetDrops(delta);
		SurvivalTest_TickNetArrows();
		SurvivalTest_TickNetTnt();
	}

	/* World.randomDisplayUpdates (Indev client ambience: fire crackle + */
	/*  smoke, torch/furnace flames) - visual only, runs its own RNG */
	IndevTest_RandomDisplayTicks();
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
	for (i = 0; i < SURVIVAL_ARMOR_SLOTS; i++) {
		st_armor[i].id = BLOCK_AIR;
		st_armor[i].count = 0;
		st_armor[i].damage = 0;
	}
	st_damageRemainder = 0;
	SurvivalTest_SyncHotbar();

	SurvivalTest_Health = SURVIVAL_MAX_HEALTH;
	st_lastHealth   = SURVIVAL_MAX_HEALTH;
	st_invincTimer  = 0.0f;
	st_hurtTicks    = 0;
	st_falling      = false;
	st_airTimer     = 20.0f / 20.0f; /* airSupply = 20 ticks, not 300 */
	st_airTicks     = 20;
	st_playerArrows = IndevTest_Enabled ? 0 : ARROW_PLAYER_START;
	st_playerFire   = 0;
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
	/* the projection was last rebuilt mid-death-zoom (DeathFovZoom); undo it
	    or a world loaded from the Game Over screen keeps the low FOV */
	Camera_UpdateProjection();
	st_score        = 0;
	st_invincTimer  = 0.0f;
	st_lastHealth   = SURVIVAL_MAX_HEALTH;
	st_hurtTicks    = 0;
	st_hurtDir      = 0.0f;
	st_breaking     = false;
	st_breakHits    = 0;
	st_breakDamage  = 0.0f;
	st_breakDelay   = 0;
	st_walkDist     = 0.0f;
	st_nextStep     = 1; /* Entity.nextStepDistance's field initialiser */
	st_airTimer     = AIR_SUPPLY_SECS;
	st_airTicks     = 300;
	SurvivalTest_Health = SURVIVAL_MAX_HEALTH;

	for (i = 0; i < SURVIVAL_INV_SLOTS; i++) {
		st_inv[i].id = BLOCK_AIR;
		st_inv[i].count = 0;
	}
	for (i = 0; i < SURVIVAL_ARMOR_SLOTS; i++) {
		st_armor[i].id = BLOCK_AIR;
		st_armor[i].count = 0;
		st_armor[i].damage = 0;
	}
	/* remote players' streamed equipment doesn't survive a map change (entity ids
	    are per-map); the server re-sends everyone's equip on the new map's join */
	for (i = 0; i < ENTITIES_SELF_ID; i++) {
		st_netEquip[i].set = false;
		st_netEquip[i].held = 0;
		st_netEquip[i].armor[0] = st_netEquip[i].armor[1] = 0;
		st_netEquip[i].armor[2] = st_netEquip[i].armor[3] = 0;
	}
	st_damageRemainder = 0;
	/* SurvivalGameMode.apply(Player): the player always starts with 10 TNT */
	/*  in the last hotbar slot. That kit (and the 20 free arrows below) is
	    c0.30's - genuine Indev starts with an EMPTY inventory. */
	if (!IndevTest_Enabled) {
		st_inv[8].id = BLOCK_TNT;
		st_inv[8].count = 10;
	}

	for (i = 0; i < DROP_MAX; i++) {
		st_drops[i].active = false;
	}
	for (i = 0; i < MOB_MAX; i++) {
		st_mobs[i].active = false;
	}
	for (i = 0; i < ARROW_MAX; i++) {
		st_arrows[i].active = false;
	}
	for (i = 0; i < PAINTING_MAX; i++) {
		st_paintings[i].active = false;
	}
	st_playerArrows = IndevTest_Enabled ? 0 : ARROW_PLAYER_START;
	st_playerFire   = 0;

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
	Gfx_DeleteTexture(&st_mobWhiteTex);
	Gfx_DeleteDynamicVb(&st_itemVB);
	Gfx_DeleteDynamicVb(&st_itemDropVB);
	Gfx_DeleteDynamicVb(&st_glowVB);
	Gfx_DeleteDynamicVb(&st_arrowVB);
	Gfx_DeleteDynamicVb(&st_tntGlowVB);
	Gfx_DeleteDynamicVb(&st_tntCubeVB);
	Gfx_DeleteDynamicVb(&st_tntSmokeVB);
	Gfx_DeleteDynamicVb(&st_cracksVB);
	Gfx_DeleteDynamicVb(&st_fireVB);
	Gfx_DeleteDynamicVb(&st_paintingVB);
	if (!Gfx.ManagedTextures) {
		Gfx_DeleteTexture(&st_arrowsTexId);
		Gfx_DeleteTexture(&st_cracksTexId);
	}
}

/* Mode-scoped one-time setup for entering survival (from launch options in SP,
    or a SURV_HELLO mode flip in MP). The per-map hooks re-run the per-map parts;
    this is the "became enabled at all" half. */
static void SurvivalTest_EnableMode(void) {
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
	if (!IndevTest_Enabled) {
		/* c0.30 Tile$SoundType table: DIRT is grass (not the gravel the
		    engine defaults to), SAND is gravel for FOOTSTEPS too, the
		    plants are SoundType.none (silent break), and sponge/TNT are
		    cloth (grass samples pitched 1.2 - see Sounds_PlayScaled). */
		Blocks.DigSounds[BLOCK_DIRT]  = SOUND_GRASS;
		Blocks.StepSounds[BLOCK_DIRT] = SOUND_GRASS;
		Blocks.StepSounds[BLOCK_SAND] = SOUND_GRAVEL;
		Blocks.DigSounds[BLOCK_SAPLING]     = SOUND_NONE;
		Blocks.DigSounds[BLOCK_DANDELION]    = SOUND_NONE;
		Blocks.DigSounds[BLOCK_ROSE]         = SOUND_NONE;
		Blocks.DigSounds[BLOCK_BROWN_SHROOM] = SOUND_NONE;
		Blocks.DigSounds[BLOCK_RED_SHROOM]   = SOUND_NONE;
		Blocks.DigSounds[BLOCK_SPONGE]  = SOUND_CLOTH;
		Blocks.StepSounds[BLOCK_SPONGE] = SOUND_CLOTH;
		Blocks.DigSounds[BLOCK_TNT]     = SOUND_CLOTH;
		Blocks.StepSounds[BLOCK_TNT]    = SOUND_CLOTH;
	}
	SurvivalTest_RegisterCommands();
}

static void SurvivalTest_Init(void) {
	/* Loaded unconditionally so the inventory screen can read it even before */
	/*  any survival logic runs (it gates a UI choice, not a gameplay rule). */
	SurvivalTest_Enhanced = Options_GetBool(OPT_SURVIVAL_ENHANCED, false);
	SurvivalTest_Creative = Options_GetBool(OPT_INDEV_CREATIVE,    false);

	/* The survival core also runs under Indev mode - IndevTest_Component's
	    Init ran first (see Game.c ordering), so its flag is already set.
	    Effective mode: in MP this is OFF until a server SURV_HELLO flips it. */
	SurvivalTest_Enabled = SurvivalTest_EffectiveGamemode() != SURVIVAL_GAMEMODE_OFF;

	/* Registered unconditionally: every hook self-guards on SurvivalTest_Enabled,
	    and a survival server can flip the mode ON at runtime via SURV_HELLO
	    (SurvivalNet) - the hooks must already exist by then. */
	ScheduledTask_Add(GAME_DEF_TICKS, SurvivalTest_Tick);
	Event_Register_(&UserEvents.BlockChanged, NULL, SurvivalTest_BlockChanged);
	Event_Register_(&GfxEvents.ContextLost,   NULL, SurvivalTest_OnContextLost);
	TextureEntry_Register(&arrows_entry);
	TextureEntry_Register(&cracks_entry);

	if (SurvivalTest_Enabled) SurvivalTest_EnableMode();
}

static void SurvivalTest_Free(void) {
	Event_Unregister_(&UserEvents.BlockChanged, NULL, SurvivalTest_BlockChanged);
	Event_Unregister_(&GfxEvents.ContextLost,   NULL, SurvivalTest_OnContextLost);
	if (!SurvivalTest_Enabled) return;
	Gfx_DeleteDynamicVb(&st_itemVB);
	Gfx_DeleteDynamicVb(&st_glowVB);
	Gfx_DeleteDynamicVb(&st_arrowVB);
	Gfx_DeleteDynamicVb(&st_tntGlowVB);
	Gfx_DeleteDynamicVb(&st_tntCubeVB);
	Gfx_DeleteDynamicVb(&st_tntSmokeVB);
	Gfx_DeleteDynamicVb(&st_cracksVB);
	Gfx_DeleteDynamicVb(&st_fireVB);
	Gfx_DeleteDynamicVb(&st_paintingVB);
	SurvivalTest_UnregisterCommands();
}

static void SurvivalTest_OnNewMap(void) {
	/* A map change ALWAYS tears down the death presentation - the Game Over
	    screen belongs to the old map's session. This must run before the
	    enabled check: when a server moves a dead player to a NON-survival map
	    (or they /goto off one), SurvivalTest deactivates and no revive
	    SURV_HEALTH will ever arrive to dismiss the screen, which left it
	    stuck over the new map with full health (user-hit live). */
	if (st_isDead) {
		st_isDead     = false;
		st_deathTicks = 0;
		GameOverScreen_Hide();
		Camera_UpdateProjection(); /* undo the death-zoom FOV */
	}
	if (!SurvivalTest_Enabled) return;
	/* Resets to the SurvivalGameMode.apply(Player) starting loadout (10 TNT, */
	/*  everything else gathered by mining) every time a new map is loaded. */
	SurvivalTest_ResetState();
	SurvivalTest_SyncHotbar();
}

/* PlayerControllerCreative.onRespawn: the creative palette hotbar. Fills each
    EMPTY hotbar slot with the genuine Session.registeredBlocksList block (stone,
    cobblestone, brick, dirt, planks, log, leaves, torch, slab); existing slots
    (e.g. a reloaded creative world) are kept. Creative placement never depletes
    these, so one of each is a genuine infinite palette. */
/* ==================== plain-server local inventory ==================== */
/* On servers WITHOUT the survival plugin, the survival screen still opens as a
    LOCAL block stash (the c0.30-storage 27+9 layout - crafting/items stay off
    with IndevTest disabled, so no simulation can leak onto someone else's
    server). The engine inventory remains the placement authority: the hotbar
    row mirrors it on open, and click changes write through slot-by-slot. */

cc_bool SurvivalTest_PlainInvActive(void) {
	if (SurvivalTest_Enabled || Server.IsSinglePlayer) return false;
	return Options_GetBool(OPT_PLAIN_SURV_INVENTORY, true);
}

/* Mirrors the engine hotbar into the survival hotbar row on screen open.
    Extended blocks (id >= 256, from BlockDefinitions) cannot live in the
    survival id space (256+ means ITEMS there) - those slots show empty, and
    write-through only touches slots the player actually changed, so an
    untouched extended-block slot survives intact. */
void SurvivalTest_PlainInvOpen(void) {
	int i;
	for (i = 0; i < SURVIVAL_HOTBAR_SLOTS; i++) {
		BlockID b = Inventory_Get(i);
		if (b != BLOCK_AIR && b < 256) {
			st_inv[i].id = b; st_inv[i].count = 1; st_inv[i].damage = 0;
		} else {
			st_inv[i].id = BLOCK_AIR; st_inv[i].count = 0; st_inv[i].damage = 0;
		}
	}
	/* items from an earlier singleplayer survival session mean nothing on a
	    plain server - strip them from the stash (block stacks may stay) */
	for (i = SURVIVAL_HOTBAR_SLOTS; i < SURVIVAL_INV_SLOTS; i++) {
		if (ST_ID_IS_BLOCK(st_inv[i].id)) continue;
		st_inv[i].id = BLOCK_AIR; st_inv[i].count = 0; st_inv[i].damage = 0;
	}
	st_cursor.id = BLOCK_AIR; st_cursor.count = 0; st_cursor.damage = 0;
	st_invVersion++;
}

/* Writes hotbar-row slots whose id changed through to the engine hotbar. */
static void SurvivalTest_PlainWriteThrough(const cc_uint16* prev) {
	int i;
	for (i = 0; i < SURVIVAL_HOTBAR_SLOTS; i++) {
		if (st_inv[i].id == prev[i]) continue;
		Inventory_Set(i, ST_ID_BLOCK(st_inv[i].id));
	}
}

static void SurvivalTest_CreativeFillPalette(void) {
	static const cc_uint16 pal[SURVIVAL_HOTBAR_SLOTS] = {
		BLOCK_STONE, BLOCK_COBBLE, BLOCK_BRICK, BLOCK_DIRT, BLOCK_WOOD,
		BLOCK_LOG, BLOCK_LEAVES, 50 /* INDEV_BLOCK_TORCH */, BLOCK_SLAB
	};
	int i;
	for (i = 0; i < SURVIVAL_HOTBAR_SLOTS; i++) {
		if (st_inv[i].id != BLOCK_AIR && st_inv[i].count > 0) continue;
		st_inv[i].id     = pal[i];
		st_inv[i].count  = 1;
		st_inv[i].damage = 0;
	}
	SurvivalTest_SyncHotbar();
}

/* Applies the current mode's fly/speed/reach to the local player. Creative gets
    flight + speed + reach 5; survival revokes them (and, via HacksComp_Update,
    drops the player out of any active flight/noclip immediately - so toggling
    creative OFF stops flying right away, not on the next map load). Called on map
    load and whenever the creative toggle changes. */
void SurvivalTest_CreativeUpdateHacks(void) {
	struct LocalPlayer* p = Entities.CurPlayer;
	if (!SurvivalTest_Enabled || !p) return;

	if (!Server.IsSinglePlayer) {
		/* MP: hack PERMISSIONS belong to the server's HackControl packet (the
		    server resolves them from the same per-session creative decision -
		    networking-plan §25). Granting/revoking locally would fight it, or
		    worse, grant flight the server never allowed. Reach is the one
		    local piece: creative reach 5 is genuine, and the server validates
		    every placement's reach anyway. */
		p->ReachDistance = SurvivalTest_CreativeActive() ? 5.0f : 4.0f;
		return;
	}

	if (SurvivalTest_CreativeActive()) {
		p->Hacks.CanFly   = true;
		p->Hacks.CanSpeed = true;
		p->ReachDistance  = 5.0f;
	} else {
		/* Classic 0.30-s / Indev survival had no fly, noclip or speed hacks. */
		p->Hacks.CanFly    = false;
		p->Hacks.CanNoclip = false;
		p->Hacks.CanSpeed  = false;
		p->ReachDistance   = 4.0f;
	}
	HacksComp_Update(&p->Hacks);
}

int SurvivalTest_EffectiveGamemode(void) {
	/* SP: the local option. MP: the server-dictated per-map mode (OFF on stock
	    servers, before SURV_HELLO, and on non-survival maps) - local options
	    never activate survival on someone else's server. */
	return Server.IsSinglePlayer ? SurvivalTest_Gamemode() : SurvivalNet_ActiveMode();
}

/* Re-derives Enabled/Enhanced/Creative from the effective mode, running the
    enable/disable transition work when the mode actually changed. Idempotent -
    safe to call on every map load AND on a mid-map SURV_HELLO flip. */
static void SurvivalTest_ApplyMode(void) {
	cc_bool wasEnabled = SurvivalTest_Enabled;
	SurvivalTest_Enabled = SurvivalTest_EffectiveGamemode() != SURVIVAL_GAMEMODE_OFF;

	if (Server.IsSinglePlayer) {
		SurvivalTest_Enhanced = Options_GetBool(OPT_SURVIVAL_ENHANCED, false);
		SurvivalTest_Creative = Options_GetBool(OPT_INDEV_CREATIVE,    false);
	} else {
		/* SURV_HELLO flags byte overrides the local options entirely in MP */
		int flags = SurvivalNet_ActiveFlags();
		SurvivalTest_Enhanced = (flags & 0x01) != 0;
		SurvivalTest_Creative = (flags & 0x02) != 0;
	}

	if (!SurvivalTest_Enabled) {
		if (wasEnabled) SurvivalTest_UnregisterCommands();
		return;
	}
	if (!wasEnabled) SurvivalTest_EnableMode();
}

/* Runs the map-activation work that OnNewMapLoaded would have done, for the MP
    case where SURV_HELLO arrives AFTER the level finished loading. */
static void SurvivalTest_MapActivate(void) {
	struct LocalPlayer* p = Entities.CurPlayer;
	if (!SurvivalTest_Enabled || !p) return;

	SurvivalTest_CreativeUpdateHacks(); /* fly/speed/reach for the current mode */

	/* Creative's palette hotbar is CLIENT state in both SP and MP - on a
	    creative map the server tracks no inventory at all, so the genuine
	    palette fills locally either way (user request: MP Indev creative
	    uses the Indev creative inventory, not the classic picker flow). */
	if (SurvivalTest_CreativeActive())
		SurvivalTest_CreativeFillPalette();

	/* Server-driven maps: the (survival) inventory and mobs are the server's
	    job - phases 4 and 3 stream them. Touching the classic hotbar or
	    spawning local mobs here would fight the server's state. */
	if (SurvivalNet_ServerDriven()) return;

	SurvivalTest_SpawnInitialMobs();
}

void SurvivalTest_NetworkModeChanged(void) {
	/* Indev layer first - this component's activation reads IndevTest_Enabled,
	    mirroring the IndevTest-before-SurvivalTest component ordering. */
	IndevTest_NetworkModeChanged();
	SurvivalTest_ApplyMode();
	SurvivalTest_MapActivate();
}

static void SurvivalTest_OnNewMapLoaded(void) {
	SurvivalTest_ApplyMode(); /* per-map re-derive (MP: OFF until SURV_HELLO) */
	SurvivalTest_MapActivate();
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

void SurvivalTest_DebugSpawnMob(int type, cc_bool noAI, cc_bool forceArmor) {
	struct Mob* m;
	Vec3 pos;
	if (!SurvivalTest_Enabled) return;
	if (type < 0 || type >= SURVIVAL_DEBUG_MOB_COUNT) return;
	if (!SurvivalTest_DebugFrontPos(3.0f, &pos)) return;

	m = SurvivalTest_SpawnMobAt((cc_uint8)type, pos);
	if (!m) return;

	/* Per-spawn modifiers from the /client spawn command args. */
	if (noAI) m->noAI = true;
	if (forceArmor && (m->type == MOB_TYPE_ZOMBIE || m->type == MOB_TYPE_SKELETON)) {
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
	SurvivalTest_ArmTnt(coords, TNT_FUSE_DEFAULT());
}

void SurvivalTest_DebugShootArrow(void) {
	struct Entity* e;
	Vec3 eye;
	if (!SurvivalTest_Enabled) return;
	if (!Entities.CurPlayer) return;
	e = &Entities.CurPlayer->Base;

	/* Same as a Tab-fire (eye-height, player force/damage), but free - doesn't */
	/*  spend an arrow from the count, so you can spam them while testing. */
	/*  In Indev mode fire through the genuine bow physics instead. */
	eye = Entity_GetEyePosition(e);
	if (IndevTest_Enabled) {
		Vec3 dir = Vec3_GetDirVector(e->Yaw * MATH_DEG2RAD, e->Pitch * MATH_DEG2RAD);
		SurvivalTest_SpawnArrowIndev(eye, dir, 1.5f, 1.0f, true, -1);
	} else {
		SurvivalTest_SpawnArrow(eye, e->Yaw, e->Pitch,
								ARROW_PLAYER_FIRE_FORCE, ARROW_PLAYER_DAMAGE, 0, true, -1);
	}
}

/* Whether the local player is alight (drives the first-person flames). */
cc_bool SurvivalTest_PlayerBurning(void)   { return SurvivalTest_Enabled && st_playerFire > 0; }

cc_bool SurvivalTest_DebugGodMode(void)    { return st_godMode; }
void SurvivalTest_DebugToggleGodMode(void) { st_godMode = !st_godMode; }

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


/*########################################################################################################################*
*------------------------------------------------/client debug commands-------------------------------------------------*
*#########################################################################################################################*/
/* The chat-command interface to the debug tools above (replaced the old F9 */
/*  button menu, which had outgrown its grid). Registered only while survival */
/*  mode is on, so they never appear in /client help for normal ClassiCube. */

/* Must match the SurvivalDebugMobType enum order (SurvivalTest.h). */
static const char* const debugMobNames[SURVIVAL_DEBUG_MOB_COUNT] = {
	"zombie", "skeleton", "pig", "creeper", "spider", "sheep", "human"
};

static void SpawnCommand_Execute(const cc_string* args, int argsCount) {
	cc_bool noAI = false, armor = false;
	int type = -1, count = 1, i, n;

	if (!argsCount) {
		Chat_AddRaw("&e/client spawn: &cno entity given - see /client help spawn");
		return;
	}
	/* the non-mob spawners take no modifiers */
	if (String_CaselessEqualsConst(&args[0], "tnt"))   { SurvivalTest_DebugSpawnTnt();   return; }
	if (String_CaselessEqualsConst(&args[0], "drops")) { SurvivalTest_DebugSpawnDrops(); return; }
	if (String_CaselessEqualsConst(&args[0], "arrow")) { SurvivalTest_DebugShootArrow(); return; }

	for (i = 0; i < SURVIVAL_DEBUG_MOB_COUNT; i++) {
		if (String_CaselessEqualsConst(&args[0], debugMobNames[i])) type = i;
	}
	if (type == -1) {
		Chat_Add1("&e/client spawn: &cunknown entity \"%s\"", &args[0]);
		return;
	}

	for (i = 1; i < argsCount; i++) {
		if (String_CaselessEqualsConst(&args[i], "noai")) {
			noAI = true;
		} else if (String_CaselessEqualsConst(&args[i], "armor")) {
			armor = true;
		} else if (Convert_ParseInt(&args[i], &n) && n > 0) {
			count = n > 10 ? 10 : n;
		} else {
			Chat_Add1("&e/client spawn: &cunknown modifier \"%s\" (noai/armor/count)", &args[i]);
			return;
		}
	}
	while (count-- > 0) SurvivalTest_DebugSpawnMob(type, noAI, armor);
}

static struct ChatCommand SpawnCommand = {
	"Spawn", SpawnCommand_Execute,
	COMMAND_FLAG_SINGLEPLAYER_ONLY,
	{
		"&a/client spawn [entity] [count] [noai] [armor]",
		"&eSpawns entities in front of you. Entities: zombie, skeleton,",
		"&e  spider, creeper, pig, sheep, human, tnt, drops, arrow.",
		"&enoai &f- mob stands still. &earmor &f- zombie/skeleton wears plate.",
	}
};

static void GiveCommand_Execute(const cc_string* args, int argsCount) {
	cc_string name; char nameBuffer[STRING_SIZE];
	cc_string display;
	const char* itemName;
	int id, count = 1, given = 0, i;

	if (!argsCount) {
		Chat_AddRaw("&e/client give: &cno item given - see /client help give");
		return;
	}
	/* a trailing integer is the count: "give iron pickaxe 5" */
	if (argsCount > 1 && Convert_ParseInt(&args[argsCount - 1], &i)) {
		count = i < 1 ? 1 : (i > 99 ? 99 : i);
		argsCount--;
	}
	/* remaining args joined = the (possibly multi-word) item/block name */
	String_InitArray(name, nameBuffer);
	for (i = 0; i < argsCount; i++) {
		if (i) String_Append(&name, ' ');
		String_AppendString(&name, &args[i]);
	}

	if (Convert_ParseInt(&name, &id)) {
		/* raw numeric id - blocks below 256, Indev items at 256+ */
		if (id >= 256 ? !IndevTest_ItemName(id) : (id <= 0 || id > 255)) {
			Chat_Add1("&e/client give: &cunknown id \"%s\"", &name);
			return;
		}
	} else {
		id = IndevTest_FindItemByName(&name);
		if (id == -1) id = IndevTest_FindBlockByName(&name);
		if (id == -1) id = Block_Parse(&name);
		if (id == -1) {
			Chat_Add1("&e/client give: &cunknown item/block \"%s\"", &name);
			return;
		}
	}

	while (given < count && SurvivalTest_AddItem((cc_uint16)id)) given++;

	itemName = IndevTest_ItemName(id);
	display  = itemName ? String_FromReadonly(itemName) : Block_UNSAFE_GetName((BlockID)id);
	Chat_Add2("&eGave &f%i&e x &f%s", &given, &display);
	if (given < count) Chat_AddRaw("&e/client give: &cinventory full");
}

static struct ChatCommand GiveCommand = {
	"Give", GiveCommand_Execute,
	COMMAND_FLAG_SINGLEPLAYER_ONLY,
	{
		"&a/client give [item] [count]",
		"&eAdds items/blocks to your inventory, by name or numeric id.",
		"&ee.g. &fgive iron pickaxe&e, &fgive painting&e, &fgive planks 32",
	}
};

static void TimeCommand_Execute(const cc_string* args, int argsCount) {
	int t;
	if (!IndevTest_Enabled) {
		Chat_AddRaw("&e/client time: &conly Indev worlds have a day/night cycle");
		return;
	}
	if (!argsCount) {
		t = IndevTest_WorldTime();
		Chat_Add1("&eTime: &f%i&e/24000", &t);
		return;
	}
	/* worldTime presets: celestial angle = t/24000 - 0.15, so noon (angle 0) */
	/*  is t=3600; midnight t=15600; dawn/dusk are the half-lit cosine zeroes. */
	if      (String_CaselessEqualsConst(&args[0], "dawn"))     { t = 21600; }
	else if (String_CaselessEqualsConst(&args[0], "noon"))     { t =  3600; }
	else if (String_CaselessEqualsConst(&args[0], "dusk"))     { t =  9600; }
	else if (String_CaselessEqualsConst(&args[0], "midnight")) { t = 15600; }
	else if (!Convert_ParseInt(&args[0], &t) || t < 0 || t > 23999) {
		Chat_AddRaw("&e/client time: &cexpected dawn/noon/dusk/midnight or 0-23999");
		return;
	}
	IndevTest_SetWorldTime(t);
}

static struct ChatCommand TimeCommand = {
	"Time", TimeCommand_Execute,
	COMMAND_FLAG_SINGLEPLAYER_ONLY,
	{
		"&a/client time [dawn/noon/dusk/midnight/ticks]",
		"&eSets the Indev world time (0-23999). No arg shows the time.",
	}
};

static void GodCommand_Execute(const cc_string* args, int argsCount) {
	SurvivalTest_DebugToggleGodMode();
	Chat_AddRaw(SurvivalTest_DebugGodMode() ? "&eGod mode: &aON" : "&eGod mode: &cOFF");
}

static struct ChatCommand GodCommand = {
	"God", GodCommand_Execute,
	COMMAND_FLAG_SINGLEPLAYER_ONLY,
	{
		"&a/client god",
		"&eToggles invincibility (blocks all player damage).",
	}
};

static void HealCommand_Execute(const cc_string* args, int argsCount) {
	int n = SURVIVAL_MAX_HEALTH;
	if (argsCount && (!Convert_ParseInt(&args[0], &n) || n <= 0)) {
		Chat_AddRaw("&e/client heal: &cexpected a positive number");
		return;
	}
	SurvivalTest_Heal(n);
}

static struct ChatCommand HealCommand = {
	"Heal", HealCommand_Execute,
	COMMAND_FLAG_SINGLEPLAYER_ONLY,
	{
		"&a/client heal [amount]",
		"&eRestores health (full heal when no amount is given).",
	}
};

static void HurtCommand_Execute(const cc_string* args, int argsCount) {
	int n = 5;
	if (argsCount && (!Convert_ParseInt(&args[0], &n) || n <= 0)) {
		Chat_AddRaw("&e/client hurt: &cexpected a positive number");
		return;
	}
	SurvivalTest_Hurt(n);
}

static struct ChatCommand HurtCommand = {
	"Hurt", HurtCommand_Execute,
	COMMAND_FLAG_SINGLEPLAYER_ONLY,
	{
		"&a/client hurt [amount]",
		"&eDamages you (5 when no amount is given), for testing armor etc.",
	}
};

static void ArrowsCommand_Execute(const cc_string* args, int argsCount) {
	int n = 99;
	if (IndevTest_Enabled) {
		Chat_AddRaw("&e/client arrows: &cIndev arrows are items - use /client give arrow [n]");
		return;
	}
	if (argsCount && !Convert_ParseInt(&args[0], &n)) {
		Chat_AddRaw("&e/client arrows: &cexpected a number");
		return;
	}
	SurvivalTest_DebugSetArrows(n);
}

static struct ChatCommand ArrowsCommand = {
	"Arrows", ArrowsCommand_Execute,
	COMMAND_FLAG_SINGLEPLAYER_ONLY,
	{
		"&a/client arrows [count]",
		"&eSets your c0.30 quiver count (99 when no count is given).",
	}
};

static void MobsCommand_Execute(const cc_string* args, int argsCount) {
	if (argsCount && String_CaselessEqualsConst(&args[0], "kill")) {
		SurvivalTest_DebugKillAllMobs();
		Chat_AddRaw("&eKilled all mobs");
		return;
	}
	SurvivalTest_DebugMobCensus();
}

static struct ChatCommand MobsCommand = {
	"Mobs", MobsCommand_Execute,
	COMMAND_FLAG_SINGLEPLAYER_ONLY,
	{
		"&a/client mobs [kill]",
		"&eNo arg: prints the mob census (alive/cap/spawn roll).",
		"&ekill &f- instantly kills every mob in the world.",
	}
};

static void SurvivalTest_RegisterCommands(void) {
	Commands_Register(&SpawnCommand);
	Commands_Register(&GiveCommand);
	Commands_Register(&TimeCommand);
	Commands_Register(&GodCommand);
	Commands_Register(&HealCommand);
	Commands_Register(&HurtCommand);
	Commands_Register(&ArrowsCommand);
	Commands_Register(&MobsCommand);
}

static void SurvivalTest_UnregisterCommands(void) {
	Commands_Unregister(&SpawnCommand);
	Commands_Unregister(&GiveCommand);
	Commands_Unregister(&TimeCommand);
	Commands_Unregister(&GodCommand);
	Commands_Unregister(&HealCommand);
	Commands_Unregister(&HurtCommand);
	Commands_Unregister(&ArrowsCommand);
	Commands_Unregister(&MobsCommand);
}


struct IGameComponent SurvivalTest_Component = {
	SurvivalTest_Init,          /* Init           */
	SurvivalTest_Free,          /* Free           */
	NULL,                       /* Reset          */
	SurvivalTest_OnNewMap,      /* OnNewMap       */
	SurvivalTest_OnNewMapLoaded /* OnNewMapLoaded */
};
