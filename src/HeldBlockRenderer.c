#include "HeldBlockRenderer.h"
#include "Block.h"
#include "Game.h"
#include "Inventory.h"
#include "Graphics.h"
#include "Camera.h"
#include "ExtMath.h"
#include "Event.h"
#include "Entity.h"
#include "Model.h"
#include "Options.h"
#include "SurvivalTest.h"
#include "IndevTest.h"

cc_bool HeldBlockRenderer_Show;
#if CC_BUILD_FPU_MODE >= CC_FPU_MODE_REDUCED
static BlockID held_block;
static struct Entity held_entity;
static struct Matrix held_blockProj;

static cc_bool held_animating, held_breaking, held_swinging;
static float held_swingY;
static float held_time, held_period = 0.25f;
static BlockID held_lastBlock;

/* Since not using Entity_SetModel, which normally automatically does this */
static void SetHeldModel(struct Model* model) {
#ifdef CC_BUILD_CONSOLE
	static int maxVertices;
	if (model->maxVertices <= maxVertices) return;

	maxVertices = model->maxVertices;
	Gfx_DeleteDynamicVb(&held_entity.ModelVB);
#endif
}

/*########################################################################################################################*
*------------------------------------------Indev extruded held item (ItemRenderer)----------------------------------------*
*#########################################################################################################################*/
/* ItemRenderer.renderItemInFirstPerson's non-block branch: the item sprite
    extruded 1/16 deep - front + back quads plus 16 strip quads along each
    edge - baked with the genuine local transform chain (translate(-15/16,
    -1/16, 0) -> rotZ 335 -> rotY 50 -> scale 1.5 -> translate(0,-0.3,0)),
    then placed/animated by the SAME held-entity transform and swing/dig
    animations the block-in-hand path uses. */
static PackedCol HeldBlockRenderer_GetCol(struct Entity* entity);

#define HELDITEM_QUADS (2 + 4 * 16)
static struct VertexTextured helditem_verts[HELDITEM_QUADS * 4];
static GfxResourceID helditem_vb;
static int helditem_lastId = -1;
static PackedCol helditem_lastCol;

static void HeldItem_Vertex(struct VertexTextured* v, const struct Matrix* m,
							float x, float y, float z, float u, float vv, PackedCol col) {
	v->x = x * m->row1.x + y * m->row2.x + z * m->row3.x + m->row4.x;
	v->y = x * m->row1.y + y * m->row2.y + z * m->row3.y + m->row4.y;
	v->z = x * m->row1.z + y * m->row2.z + z * m->row3.z + m->row4.z;
	v->Col = col; v->U = u; v->V = vv;
}

static void HeldItem_BuildMesh(TextureRec rec, PackedCol col) {
	struct VertexTextured* v = helditem_verts;
	struct Matrix m, r;
	float x, u, y, vv, eu, ev;
	int i;
	/* NOTE: genuine mirrors u (x=0 samples the sprite's right edge), but our
	    mesh rides the engine's held-entity transform whose handedness differs
	    from genuine's raw camera chain - matching genuine's ON-SCREEN result
	    (head up, blade toward the screen centre) needs u unmirrored here. */
	float u1 = rec.u1, u2 = rec.u2, v1 = rec.v1, v2 = rec.v2;

	Matrix_Translate(&m, -15.0f/16.0f, -1.0f/16.0f, 0.0f);
	Matrix_RotateZ(&r, 335.0f * MATH_DEG2RAD); Matrix_MulBy(&m, &r);
	Matrix_RotateY(&r,  50.0f * MATH_DEG2RAD); Matrix_MulBy(&m, &r);
	Matrix_Scale(&r, 1.5f, 1.5f, 1.5f);         Matrix_MulBy(&m, &r);
	Matrix_Translate(&r, 0.0f, -0.3f, 0.0f);    Matrix_MulBy(&m, &r);

	eu = (rec.u2 - rec.u1) * (0.5f / 16.0f); /* the genuine 0.001953125 half-texel */
	ev = (rec.v2 - rec.v1) * (0.5f / 16.0f);

	#define HI_V(px, py, pz, uu, vvv) HeldItem_Vertex(v, &m, px, py, pz, uu, vvv, col); v++;
	/* front (z = 0) and back (z = -1/16) faces */
	HI_V(0,0,0, u1,v1) HI_V(1,0,0, u2,v1) HI_V(1,1,0, u2,v2) HI_V(0,1,0, u1,v2)
	HI_V(0,1,-1.0f/16, u1,v2) HI_V(1,1,-1.0f/16, u2,v2) HI_V(1,0,-1.0f/16, u2,v1) HI_V(0,0,-1.0f/16, u1,v1)

	for (i = 0; i < 16; i++) {
		x = i / 16.0f;
		u = u1 + (u2 - u1) * x - eu;
		/* -X edge strips */
		HI_V(x,0,-1.0f/16, u,v1) HI_V(x,0,0, u,v1) HI_V(x,1,0, u,v2) HI_V(x,1,-1.0f/16, u,v2)
	}
	for (i = 0; i < 16; i++) {
		x = i / 16.0f + 1.0f/16.0f;
		u = u1 + (u2 - u1) * (x - 1.0f/16.0f) - eu;
		/* +X edge strips */
		HI_V(x,1,-1.0f/16, u,v2) HI_V(x,1,0, u,v2) HI_V(x,0,0, u,v1) HI_V(x,0,-1.0f/16, u,v1)
	}
	for (i = 0; i < 16; i++) {
		y  = i / 16.0f + 1.0f/16.0f;
		vv = v1 + (v2 - v1) * (y - 1.0f/16.0f) - ev;
		/* +Y edge strips */
		HI_V(0,y,0, u1,vv) HI_V(1,y,0, u2,vv) HI_V(1,y,-1.0f/16, u2,vv) HI_V(0,y,-1.0f/16, u1,vv)
	}
	for (i = 0; i < 16; i++) {
		y  = i / 16.0f;
		vv = v1 + (v2 - v1) * y - ev;
		/* -Y edge strips */
		HI_V(1,y,0, u2,vv) HI_V(0,y,0, u1,vv) HI_V(0,y,-1.0f/16, u1,vv) HI_V(1,y,-1.0f/16, u2,vv)
	}
	#undef HI_V
}

static void HeldItem_Render(int heldId) {
	struct Matrix transform, m;
	TextureRec rec;
	PackedCol col;
	Vec3 scale;

	if (!IndevTest_BindHeldTexture(heldId, &rec)) return;
	col = HeldBlockRenderer_GetCol(&held_entity);

	if (heldId != helditem_lastId || col != helditem_lastCol) {
		HeldItem_BuildMesh(rec, col);
		helditem_lastId  = heldId;
		helditem_lastCol = col;
	}
	if (!helditem_vb) {
		helditem_vb = Gfx_CreateDynamicVb(VERTEX_FORMAT_TEXTURED, HELDITEM_QUADS * 4);
		if (!helditem_vb) return;
	}

	/* same placement/animation transform the block-in-hand path gets */
	Vec3_Set(scale, 0.4f, 0.4f, 0.4f);
	Entity_GetTransform(&held_entity, held_entity.Position, scale, &transform);
	Matrix_Mul(&m, &transform, &Gfx.View);
	Gfx_LoadMatrix(MATRIX_VIEW, &m);

	Gfx_SetAlphaTest(true);
	Gfx_SetFaceCulling(false); /* thin shell; winding varies per strip */
	Gfx_SetVertexFormat(VERTEX_FORMAT_TEXTURED);
	Gfx_SetDynamicVbData(helditem_vb, helditem_verts, HELDITEM_QUADS * 4);
	Gfx_DrawVb_IndexedTris(HELDITEM_QUADS * 4);
	Gfx_SetFaceCulling(true);

	Gfx_LoadMatrix(MATRIX_VIEW, &Gfx.View);
}

static void HeldBlockRenderer_RenderModel(void) {
	struct Model* model;

	Gfx_SetFaceCulling(true);
	Gfx_SetDepthTest(false);
	/* Gfx_SetDepthWrite(false); */
	/* TODO: Need to properly reallocate per model VB here */

	if (Blocks.Draw[held_block] == DRAW_GAS) {
		/* When survival holds an ITEM id, its sprite is drawn by SurvivalTest's */
		/*  drop-sprite pass anchored in front of the camera (rendering it here */
		/*  with raw quads proved unreliable across the held renderer's matrix/ */
		/*  culling state) - skip the bare arm then, so the sprite reads as the */
		/*  held item rather than floating beside an empty hand. The genuine */
		/*  extruded ItemRenderer mesh + swing is a future port (back-burnered). */
		int heldId = SurvivalTest_SlotId(Inventory.SelectedIndex);
		cc_bool holdingItem = IndevTest_HeldIsExtruded(heldId);
		if (holdingItem) {
			/* Indev's first-person extruded item sprite, riding the same */
			/*  swing/dig animations as the held block */
			HeldItem_Render(heldId);
			/* HeldItem_Render enables alpha test for its cutout mesh - it MUST
			    be turned back off before the 2D pass, exactly like the bare-arm
			    branch below. Leaking it discards every GUI pixel whose alpha is
			    below the 0.5 test threshold on fixed-function backends (D3D9):
			    the pause-menu dim gradient (alpha 105 top -> 162 bottom) lost
			    its whole upper part whenever an item was held. */
			Gfx_SetAlphaTest(false);
		} else {
			/* Bare arm - skipped when holding an item id (its sprite renders */
			/*  in SurvivalTest's drop pass); must still fall through to the */
			/*  depth/cull teardown below or the 2D HUD/menus get corrupted. */
			model = Entities.CurPlayer->Base.Model;
			SetHeldModel(model);
			Vec3_Set(held_entity.ModelScale, 1.0f, 1.0f, 1.0f);

			Model_RenderArm(model, &held_entity);
			Gfx_SetAlphaTest(false);
		}
	}
	else if (IndevTest_HeldIsExtruded(held_block)) {
		/* genuine ItemRenderer: only renderType 0 blocks are held as 3D
		    blocks - torches/flowers/mushrooms/saplings render as the same
		    extruded terrain-tile sprite items use, IN the hand (the block
		    model path drew the torch at arm's length, visibly floating) */
		HeldItem_Render(held_block);
		Gfx_SetAlphaTest(false);
	}
	else {
		model = Models.Block;
		SetHeldModel(model);
		Vec3_Set(held_entity.ModelScale, 0.4f, 0.4f, 0.4f);

		Gfx_SetupAlphaState(Blocks.Draw[held_block]);
		Model_Render(model, &held_entity);
		Gfx_RestoreAlphaState(Blocks.Draw[held_block]);
	}
	
	Gfx_SetDepthTest(true);
	/* Gfx_SetDepthWrite(true); */
	Gfx_SetFaceCulling(false);
}

static void SetMatrix(void) {
	struct Entity* p = &Entities.CurPlayer->Base;
	struct Matrix lookAt;
	Vec3 eye = { 0,0,0 }; eye.y = Entity_GetEyeHeight(p);

	Matrix_Translate(&lookAt, -eye.x, -eye.y, -eye.z);
	Matrix_Mul(&Gfx.View, &lookAt, &Camera.TiltM);
}

static void ResetHeldState(void) {
	/* Based off details from http://pastebin.com/KFV0HkmD (Thanks goodlyay!) */
	struct Entity* p = &Entities.CurPlayer->Base;
	Vec3 eye = { 0,0,0 }; eye.y = Entity_GetEyeHeight(p);
	held_entity.Position = eye;

	held_entity.Position.x -= Camera.BobbingHor;
	held_entity.Position.y -= Camera.BobbingVer;
	held_entity.Position.z -= Camera.BobbingHor;

	held_entity.Yaw   = -45.0f; held_entity.RotY = -45.0f;
	held_entity.Pitch = 0.0f;   held_entity.RotX = 0.0f;
	held_entity.ModelBlock   = held_block;

	held_entity.SkinType     = p->SkinType;
	held_entity.TextureId    = p->TextureId;
	held_entity.NonHumanSkin = p->NonHumanSkin;
	held_entity.uScale       = p->uScale;
	held_entity.vScale       = p->vScale;
}

static void SetBaseOffset(void) {
	cc_bool sprite = Blocks.Draw[held_block] == DRAW_SPRITE;
	Vec3 normalOffset = { 0.56f, -0.72f, -0.72f };
	Vec3 spriteOffset = { 0.46f, -0.52f, -0.72f };
	Vec3 itemOffset   = { 0.56f, -0.52f, -0.72f }; /* ItemRenderer's hand anchor */
	Vec3 offset = sprite ? spriteOffset : normalOffset;
	if (IndevTest_HeldIsExtruded(SurvivalTest_SlotId(Inventory.SelectedIndex))) offset = itemOffset;

	Vec3_AddBy(&held_entity.Position, &offset);
	if (!sprite && Blocks.Draw[held_block] != DRAW_GAS) {
		float height = Blocks.MaxBB[held_block].y - Blocks.MinBB[held_block].y;
		held_entity.Position.y += 0.2f * (1.0f - height);
	}
}

static void OnProjectionChanged(void* obj) {
	float fov = 70.0f * MATH_DEG2RAD;
	float aspectRatio = (float)Game.Width / (float)Game.Height;
	Gfx_CalcPerspectiveMatrix(&held_blockProj, fov, aspectRatio, (float)Game_ViewDistance);
}

/* Based off incredible gifs from (Thanks goodlyay!)
	https://dl.dropboxusercontent.com/s/iuazpmpnr89zdgb/slowBreakTranslate.gif
	https://dl.dropboxusercontent.com/s/z7z8bset914s0ij/slowBreakRotate1.gif
	https://dl.dropboxusercontent.com/s/pdq79gkzntquld1/slowBreakRotate2.gif
	https://dl.dropboxusercontent.com/s/w1ego7cy7e5nrk1/slowBreakFull.gif

	https://github.com/ClassiCube/ClassicalSharp/wiki/Dig-animation-details
*/
static void HeldBlockRenderer_DigAnimation(void) {
	float sinHalfCircle, sinHalfCircleWeird;
	float t, sqrtLerpPI;

	t = held_time / held_period;
	sinHalfCircle = Math_SinF(t * MATH_PI);
	sqrtLerpPI    = Math_SqrtF(t) * MATH_PI;

	held_entity.Position.x -= Math_SinF(sqrtLerpPI)     * 0.4f;
	held_entity.Position.y += Math_SinF(sqrtLerpPI * 2) * 0.2f;
	held_entity.Position.z -= sinHalfCircle            * 0.2f;

	sinHalfCircleWeird = Math_SinF(t * t * MATH_PI);
	held_entity.RotY  -= Math_SinF(sqrtLerpPI) * 80.0f;
	held_entity.Yaw   -= Math_SinF(sqrtLerpPI) * 80.0f;
	held_entity.RotX  += sinHalfCircleWeird    * 20.0f;
}

static void HeldBlockRenderer_ResetAnim(cc_bool setLastHeld, float period) {
	held_time = 0.0f; held_swingY = 0.0f;
	held_animating = false; held_swinging = false;
	held_period = period;
	if (setLastHeld) { held_lastBlock = Inventory_SelectedBlock; }
}

static PackedCol HeldBlockRenderer_GetCol(struct Entity* entity) {
	struct Entity* player;
	PackedCol col;
	float adjPitch, t, scale;

	player = &Entities.CurPlayer->Base;
	col    = player->VTABLE->GetCol(player);

	/* Adjust pitch so angle when looking straight down is 0. */
	adjPitch = player->Pitch - 90.0f;
	if (adjPitch < 0.0f) adjPitch += 360.0f;

	/* Adjust color so held block is brighter when looking straight up */
	t     = Math_AbsF(adjPitch - 180.0f) / 180.0f;
	scale = Math_Lerp(0.9f, 0.7f, t);
	return PackedCol_Scale(col, scale);
}

void HeldBlockRenderer_ClickAnim(cc_bool digging) {
	/* TODO: timing still not quite right, rotate2 still not quite right */
	HeldBlockRenderer_ResetAnim(true, digging ? 0.35 : 0.25);
	held_swinging  = false;
	held_breaking  = digging;
	held_animating = true;
	/* Start place animation at bottom of cycle */
	if (!digging) held_time = held_period / 2;

	/* Third-person arm swing on the player model (AnimatedComp_StartPunch) is */
	/*  disabled for now - deferred, see SURVIVAL_TEST_NOTES.md. */
}

static void DoSwitchBlockAnim(void* obj) {
	if (held_swinging) {
		/* Like graph -sin(x) : x=0.5 and x=2.5 have same y values,
		   but increasing x causes y to change in opposite directions */
		if (held_time > held_period * 0.5f) {
			held_time = held_period - held_time;
		}
	} else {
		if (held_block == Inventory_SelectedBlock) return;
		HeldBlockRenderer_ResetAnim(false, 0.25);
		held_animating = true;
		held_swinging = true;
	}
}

static void OnBlockChanged(void* obj, IVec3 coords, BlockID old, BlockID now) {
	if (now == BLOCK_AIR) return;
	HeldBlockRenderer_ClickAnim(false);
}

static void DoAnimation(float delta, float lastSwingY) {
	float t;
	if (!held_animating) return;

	if (held_swinging || !held_breaking) {
		t = held_time / held_period;
		held_swingY = -0.4f * Math_SinF(t * MATH_PI);
		held_entity.Position.y += held_swingY;

		if (held_swinging) {
			/* i.e. the block has gone to bottom of screen and is now returning back up. 
			   At this point we switch over to the new held block. */
			if (held_swingY > lastSwingY) held_lastBlock = held_block;
			held_block = held_lastBlock;
			held_entity.ModelBlock = held_block;
		}
	} else {
		HeldBlockRenderer_DigAnimation();
	}
	
	held_time += delta;
	if (held_time > held_period) {
		HeldBlockRenderer_ResetAnim(true, 0.25f);
	}
}

void HeldBlockRenderer_Render(float delta) {
	float lastSwingY;
	struct Matrix view;
	if (!HeldBlockRenderer_Show) return;

	lastSwingY  = held_swingY;
	held_swingY = 0.0f;
	held_block  = Inventory_SelectedBlock;
	view = Gfx.View;

	Gfx_LoadMatrix(MATRIX_PROJ, &held_blockProj);
	SetMatrix();

	ResetHeldState();
	DoAnimation(delta, lastSwingY);
	SetBaseOffset();
	if (!Camera.Active->isThirdPerson) HeldBlockRenderer_RenderModel();

	Gfx.View = view;
	Gfx_LoadMatrix(MATRIX_PROJ, &Gfx.Projection);
}


static void OnContextLost(void* obj) {
	Gfx_DeleteDynamicVb(&held_entity.ModelVB);
	Gfx_DeleteDynamicVb(&helditem_vb);
	helditem_lastId = -1;
}

static const struct EntityVTABLE heldEntity_VTABLE = {
	NULL, NULL, NULL, HeldBlockRenderer_GetCol,
	NULL, NULL
};
static void OnInit(void) {
	Entity_Init(&held_entity);
	held_entity.VTABLE  = &heldEntity_VTABLE;
	held_entity.NoShade = true;

	HeldBlockRenderer_Show = Options_GetBool(OPT_SHOW_BLOCK_IN_HAND, true);
	held_lastBlock         = Inventory_SelectedBlock;

	Event_Register_(&GfxEvents.ProjectionChanged, NULL, OnProjectionChanged);
	Event_Register_(&UserEvents.HeldBlockChanged, NULL, DoSwitchBlockAnim);
	Event_Register_(&UserEvents.BlockChanged,     NULL, OnBlockChanged);
	Event_Register_(&GfxEvents.ContextLost,       NULL, OnContextLost);
}
#else
void HeldBlockRenderer_ClickAnim(cc_bool digging) { }
void HeldBlockRenderer_Render(float delta) { }

static void OnInit(void) { }
#endif

struct IGameComponent HeldBlockRenderer_Component = {
	OnInit /* Init  */
};
