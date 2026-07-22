#include "IndevArmor.h"
#include "Model.h"
#include "Entity.h"
#include "Graphics.h"
#include "ExtMath.h"
#include "String_.h"
#include "SurvivalTest.h"
#include "IndevTest.h"

/* in-20100223 RenderPlayer worn-armor overlay.
   Genuine renders up to four extra ModelBiped passes over the player:
     pass 0 helmet: bipedHead + bipedHeadwear      (ModelBiped(1.0F), tex _1)
     pass 1 chest:  bipedBody + both arms          (ModelBiped(1.0F), tex _1)
     pass 2 legs:   bipedBody + both legs          (ModelBiped(0.5F), tex _2)
     pass 3 boots:  both legs                      (ModelBiped(1.0F), tex _1)
   with the armorInventory checked as [3 - pass], and the texture picked by
   ItemArmor.renderIndex: cloth/chain/iron/diamond/gold.
   ModelBiped(f) inflates every box by f pixels on all sides (the headwear
   gets f + 0.5), with UVs unchanged - replicated below via the engine's
   Dims/expanded-Bounds pattern (see HumanModel's hat/layer boxes).
   Box positions and pivots MATCH THE ENGINE's humanoid model (not genuine
   ModelBiped's pivots) so the overlay tracks the body it is drawn over.
   Copyright 2014-2025 ClassiCube | Licensed under BSD-3
*/

/*########################################################################################################################*
*-------------------------------------------------------Armor textures----------------------------------------------------*
*#########################################################################################################################*/
/* [renderIndex][layer]: cloth/chain/iron/diamond/gold x _1/_2. The pngs are
    the authentic in-20100223 assets (byte-identical in the b1.7.3 jar,
    which Resources.c pulls them from into default.zip). */
static struct ModelTex armor_texs[5][2] = {
	{ { "armor_cloth_1.png"   }, { "armor_cloth_2.png"   } },
	{ { "armor_chain_1.png"   }, { "armor_chain_2.png"   } },
	{ { "armor_iron_1.png"    }, { "armor_iron_2.png"    } },
	{ { "armor_diamond_1.png" }, { "armor_diamond_2.png" } },
	{ { "armor_gold_1.png"    }, { "armor_gold_2.png"    } },
};

/*########################################################################################################################*
*--------------------------------------------------------Armor models-----------------------------------------------------*
*#########################################################################################################################*/
/* which pass/texture the current Model_Render call is drawing */
static int armor_pass, armor_texIdx;

/* full-inflation (1.0) parts: head, headwear, torso, arms, legs */
static struct ModelPart arm1_head, arm1_hat, arm1_torso;
static struct ModelPart arm1_lArm, arm1_rArm, arm1_lLeg, arm1_rLeg;
/* half-inflation (0.5) parts for the leggings pass */
static struct ModelPart arm2_torso, arm2_lLeg, arm2_rLeg;

static void Armor1Model_MakeParts(void) {
	/* engine humanoid boxes inflated by 1 pixel (headwear by 1.5) */
	static const struct BoxDesc head = {
		BoxDesc_Tex(0,0),
		BoxDesc_Dims(-4,24,-4, 4,32,4),
		BoxDesc_Bounds(-5,23,-5, 5,33,5),
		BoxDesc_Rot(0,24,0),
	};
	static const struct BoxDesc hat = {
		BoxDesc_Tex(32,0),
		BoxDesc_Dims(-4,24,-4, 4,32,4),
		BoxDesc_Bounds(-5.5f,22.5f,-5.5f, 5.5f,33.5f,5.5f),
		BoxDesc_Rot(0,24,0),
	};
	static const struct BoxDesc torso = {
		BoxDesc_Tex(16,16),
		BoxDesc_Dims(-4,12,-2, 4,24,2),
		BoxDesc_Bounds(-5,11,-3, 5,25,3),
		BoxDesc_Rot(0,12,0),
	};
	/* arms/legs keep the engine's swapped-coordinate UV mirroring for the
	    left limbs (64x32 layout has no separate left textures) */
	static const struct BoxDesc lArm = {
		BoxDesc_Tex(40,16),
		BoxDesc_Dims(-4,12,-2, -8,24,2),
		BoxDesc_Bounds(-3,11,-3, -9,25,3),
		BoxDesc_Rot(-5,22,0),
	};
	static const struct BoxDesc rArm = {
		BoxDesc_Tex(40,16),
		BoxDesc_Dims(4,12,-2, 8,24,2),
		BoxDesc_Bounds(3,11,-3, 9,25,3),
		BoxDesc_Rot(5,22,0),
	};
	static const struct BoxDesc lLeg = {
		BoxDesc_Tex(0,16),
		BoxDesc_Dims(0,0,-2, -4,12,2),
		BoxDesc_Bounds(1,-1,-3, -5,13,3),
		BoxDesc_Rot(0,12,0),
	};
	static const struct BoxDesc rLeg = {
		BoxDesc_Tex(0,16),
		BoxDesc_Dims(0,0,-2, 4,12,2),
		BoxDesc_Bounds(-1,-1,-3, 5,13,3),
		BoxDesc_Rot(0,12,0),
	};

	BoxDesc_BuildBox(&arm1_head,  &head);
	BoxDesc_BuildBox(&arm1_hat,   &hat);
	BoxDesc_BuildBox(&arm1_torso, &torso);
	BoxDesc_BuildBox(&arm1_lArm,  &lArm);
	BoxDesc_BuildBox(&arm1_rArm,  &rArm);
	BoxDesc_BuildBox(&arm1_lLeg,  &lLeg);
	BoxDesc_BuildBox(&arm1_rLeg,  &rLeg);
}

static void Armor2Model_MakeParts(void) {
	/* the leggings pass: ModelBiped(0.5F) */
	static const struct BoxDesc torso = {
		BoxDesc_Tex(16,16),
		BoxDesc_Dims(-4,12,-2, 4,24,2),
		BoxDesc_Bounds(-4.5f,11.5f,-2.5f, 4.5f,24.5f,2.5f),
		BoxDesc_Rot(0,12,0),
	};
	static const struct BoxDesc lLeg = {
		BoxDesc_Tex(0,16),
		BoxDesc_Dims(0,0,-2, -4,12,2),
		BoxDesc_Bounds(0.5f,-0.5f,-2.5f, -4.5f,12.5f,2.5f),
		BoxDesc_Rot(0,12,0),
	};
	static const struct BoxDesc rLeg = {
		BoxDesc_Tex(0,16),
		BoxDesc_Dims(0,0,-2, 4,12,2),
		BoxDesc_Bounds(-0.5f,-0.5f,-2.5f, 4.5f,12.5f,2.5f),
		BoxDesc_Rot(0,12,0),
	};

	BoxDesc_BuildBox(&arm2_torso, &torso);
	BoxDesc_BuildBox(&arm2_lLeg,  &lLeg);
	BoxDesc_BuildBox(&arm2_rLeg,  &rLeg);
}

static void ArmorModel_DrawCore(struct Entity* e, int count) {
	GfxResourceID tex = armor_texs[armor_texIdx][armor_pass == 2 ? 1 : 0].texID;
	if (!tex) return;

	Model_LockVB(e, count);
	Models.Active->index = 0;
	/* the armor textures are 64x32 regardless of the wearer's skin type */
	Models.uScale = 1.0f / 64.0f;
	Models.vScale = 1.0f / 32.0f;

	switch (armor_pass) {
	case 0: /* helmet: head + headwear */
		Model_DrawRotate(-e->Pitch * MATH_DEG2RAD, 0, 0, &arm1_head, true);
		Model_DrawRotate(-e->Pitch * MATH_DEG2RAD, 0, 0, &arm1_hat,  true);
		break;
	case 1: /* chestplate: torso + both arms */
		Model_DrawPart(&arm1_torso);
		Models.Rotation = ROTATE_ORDER_XZY;
		Model_DrawRotate(e->Anim.LeftArmX,  e->Anim.LeftArmY,  e->Anim.LeftArmZ,  &arm1_lArm, false);
		Model_DrawRotate(e->Anim.RightArmX, e->Anim.RightArmY, e->Anim.RightArmZ, &arm1_rArm, false);
		Models.Rotation = ROTATE_ORDER_ZYX;
		break;
	case 2: /* leggings: torso + both legs (0.5 inflation, _2 texture) */
		Model_DrawPart(&arm2_torso);
		Model_DrawRotate(e->Anim.LeftLegX,  0, e->Anim.LeftLegZ,  &arm2_lLeg, false);
		Model_DrawRotate(e->Anim.RightLegX, 0, e->Anim.RightLegZ, &arm2_rLeg, false);
		break;
	default: /* boots: both legs */
		Model_DrawRotate(e->Anim.LeftLegX,  0, e->Anim.LeftLegZ,  &arm1_lLeg, false);
		Model_DrawRotate(e->Anim.RightLegX, 0, e->Anim.RightLegZ, &arm1_rLeg, false);
		break;
	}

	Model_UnlockVB();
	Gfx_BindTexture(tex);
	Gfx_DrawVb_IndexedTris(count);
}

static void Armor1Model_Draw(struct Entity* e) {
	ArmorModel_DrawCore(e, (armor_pass == 1 ? 3 : 2) * MODEL_BOX_VERTICES);
}
static void Armor2Model_Draw(struct Entity* e) {
	ArmorModel_DrawCore(e, 3 * MODEL_BOX_VERTICES);
}

static float ArmorModel_GetNameY(struct Entity* e) { return 32.5f/16.0f; }
static float ArmorModel_GetEyeY(struct Entity* e)  { return 26.0f/16.0f; }
static void ArmorModel_GetSize(struct Entity* e) {
	static Vec3 size = { 8.6f/16.0f, 28.1f/16.0f, 8.6f/16.0f };
	e->Size = size;
}
static void ArmorModel_GetBounds(struct Entity* e) {
	static struct AABB bb = {
		{ -8.0f/16.0f,  0.0f/16.0f, -4.0f/16.0f },
		{  8.0f/16.0f, 32.0f/16.0f,  4.0f/16.0f }
	};
	e->ModelAABB = bb;
}

static struct ModelVertex armor1_vertices[MODEL_BOX_VERTICES * 7];
static struct ModelVertex armor2_vertices[MODEL_BOX_VERTICES * 3];

static struct Model armor1_model = {
	"indev_armor", armor1_vertices, &armor_texs[0][0],
	Armor1Model_MakeParts, Armor1Model_Draw,
	ArmorModel_GetNameY,   ArmorModel_GetEyeY,
	ArmorModel_GetSize,    ArmorModel_GetBounds,
};
static struct Model armor2_model = {
	"indev_armor2", armor2_vertices, &armor_texs[0][1],
	Armor2Model_MakeParts, Armor2Model_Draw,
	ArmorModel_GetNameY,   ArmorModel_GetEyeY,
	ArmorModel_GetSize,    ArmorModel_GetBounds,
};

void IndevArmor_Register(void) {
	static const cc_string name1 = String_FromConst("indev_armor");
	static const cc_string name2 = String_FromConst("indev_armor2");
	int i, j;

	Model_Init(&armor1_model);
	armor1_model.maxVertices = 7 * MODEL_BOX_VERTICES;
	Model_Register(&armor1_model);

	Model_Init(&armor2_model);
	armor2_model.maxVertices = 3 * MODEL_BOX_VERTICES;
	Model_Register(&armor2_model);

	/* MakeParts only runs lazily via Model_Get - these models are rendered
	    directly (never assigned by name), so force the parts to build now */
	Model_Get(&name1);
	Model_Get(&name2);

	for (i = 0; i < 5; i++) {
		for (j = 0; j < 2; j++) Model_RegisterTexture(&armor_texs[i][j]);
	}
}

/*########################################################################################################################*
*--------------------------------------------------------Render entry-----------------------------------------------------*
*#########################################################################################################################*/
/* ItemArmor.renderIndex = the armor set (cloth 0 .. gold 4) */
static int IndevArmor_RenderIndex(int id) {
	int local = id - 256 - 42;
	if (local < 0 || local >= 20) return -1;
	return local / 4;
}

/* Renders the four armor overlay passes from an explicit set of worn ids -
    ids[0] boots .. ids[3] helmet (the InventoryPlayer.armorInventory order), 0 =
    empty. Shared by the local player (fed from st_armor) and remote players (fed
    from the streamed SURV_PLAYER_EQUIP armor[4]). */
void IndevArmor_RenderIds(struct Entity* e, const cc_uint16* ids) {
	int pass, slot, id, texIdx;
	if (!IndevTest_Enabled || !e) return;

	for (pass = 0; pass < 4; pass++) {
		slot = 3 - pass; /* genuine shouldRenderPass: armorInventory[3 - pass] */
		id   = ids[slot];
		if (id == 0) continue; /* streamed ids have no count - present == non-zero */

		texIdx = IndevArmor_RenderIndex(id);
		if (texIdx < 0) continue;

		armor_pass   = pass;
		armor_texIdx = texIdx;
		Model_Render(pass == 2 ? &armor2_model : &armor1_model, e);
	}
}

void IndevArmor_Render(struct Entity* e) {
	cc_uint16 ids[4];
	int i;
	for (i = 0; i < 4; i++)
		ids[i] = SurvivalTest_ArmorCount(i) > 0 ? (cc_uint16)SurvivalTest_ArmorId(i) : 0;
	IndevArmor_RenderIds(e, ids);
}
