/* Loader test for the BlenderModel plugin.
 * Parses an OBJ + PNG the same way the plugin does in game, then checks the
 * resulting parts, bounds, pivots, texture padding and the rotation helper.
 * Runs against a headless ("make terminal") build of the game, no GPU needed.
 */
#define BM_TEST
#include "../BlenderModel.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)
#define NEAR(a, b) (Math_AbsF((a) - (b)) < 0.001f)

static const struct BM_Part* FindPart(const char* name) {
	int i;
	for (i = 0; i < bm_numParts; i++) {
		if (!strncmp(bm_parts[i].name, name, bm_parts[i].nameLen) && (int)strlen(name) == bm_parts[i].nameLen) return &bm_parts[i];
	}
	return NULL;
}

static void TestParsers(void) {
	cc_string s;
	float f; int i;

	s = String_FromReadonly("-1.5");    CHECK(BM_ParseFloat(&s, &f) && NEAR(f, -1.5f));
	s = String_FromReadonly("2.5e-1");  CHECK(BM_ParseFloat(&s, &f) && NEAR(f, 0.25f));
	s = String_FromReadonly("3E2");     CHECK(BM_ParseFloat(&s, &f) && NEAR(f, 300.0f));
	s = String_FromReadonly(".5");      CHECK(BM_ParseFloat(&s, &f) && NEAR(f, 0.5f));
	s = String_FromReadonly("abc");     CHECK(!BM_ParseFloat(&s, &f));
	s = String_FromReadonly("");        CHECK(!BM_ParseFloat(&s, &f));
	s = String_FromReadonly("-12");     CHECK(BM_ParseInt(&s, &i) && i == -12);
	s = String_FromReadonly("7x");      CHECK(!BM_ParseInt(&s, &i));

	CHECK(BM_ResolveIndex( 1, 3) == 0);
	CHECK(BM_ResolveIndex( 3, 3) == 2);
	CHECK(BM_ResolveIndex( 4, 3) <  0);
	CHECK(BM_ResolveIndex(-1, 3) == 2);
	CHECK(BM_ResolveIndex(-4, 3) <  0);
	CHECK(BM_ResolveIndex( 0, 3) <  0);

	CHECK(BM_NextPow2(1) == 1 && BM_NextPow2(64) == 64 && BM_NextPow2(65) == 128 && BM_NextPow2(100) == 128);
}

static void TestClassify(void) {
	cc_string s;
	s = String_FromReadonly("Head");       CHECK(BM_ClassifyName(&s) == BM_PART_HEAD);
	s = String_FromReadonly("Arm.L");      CHECK(BM_ClassifyName(&s) == BM_PART_LEFT_ARM);
	s = String_FromReadonly("Arm.R");      CHECK(BM_ClassifyName(&s) == BM_PART_RIGHT_ARM);
	s = String_FromReadonly("arm");        CHECK(BM_ClassifyName(&s) == BM_PART_RIGHT_ARM);
	s = String_FromReadonly("LeftLeg");    CHECK(BM_ClassifyName(&s) == BM_PART_LEFT_LEG);
	s = String_FromReadonly("leg_r");      CHECK(BM_ClassifyName(&s) == BM_PART_RIGHT_LEG);
	s = String_FromReadonly("L_Foot");     CHECK(BM_ClassifyName(&s) == BM_PART_LEFT_LEG);
	s = String_FromReadonly("right_hand"); CHECK(BM_ClassifyName(&s) == BM_PART_RIGHT_ARM);
	s = String_FromReadonly("Body");       CHECK(BM_ClassifyName(&s) == BM_PART_STATIC);
	s = String_FromReadonly("Tail");       CHECK(BM_ClassifyName(&s) == BM_PART_STATIC);
}

static void TestRotation(void) {
	struct BM_Rot rot;
	Vec3 v, r;
	/* 90 degrees about Y through the origin: same convention as Model_DrawRotate, */
	/* which rotates by -angle, so +X ends up at -Z */
	BM_Rot_Init(&rot, BM_Vec3(0,0,0), 0, 90 * MATH_DEG2RAD, 0, ROTATE_ORDER_ZYX, false);
	v = BM_Vec3(1, 0, 0);
	r = BM_Rot_Apply(&rot, v);
	CHECK(NEAR(r.x, 0) && NEAR(r.y, 0) && NEAR(r.z, -1));

	/* Rotation about a pivot leaves the pivot in place */
	BM_Rot_Init(&rot, BM_Vec3(1, 2, 3), 0.7f, 1.1f, -0.4f, ROTATE_ORDER_XZY, false);
	r = BM_Rot_Apply(&rot, BM_Vec3(1, 2, 3));
	CHECK(NEAR(r.x, 1) && NEAR(r.y, 2) && NEAR(r.z, 3));

	/* 90 degrees about X: +Y goes to +Z (matches Model_RotateX with -angle) */
	BM_Rot_Init(&rot, BM_Vec3(0,0,0), 90 * MATH_DEG2RAD, 0, 0, ROTATE_ORDER_ZYX, false);
	r = BM_Rot_Apply(&rot, BM_Vec3(0, 1, 0));
	CHECK(NEAR(r.x, 0) && NEAR(r.y, 0) && NEAR(r.z, 1));
}

static void TestShade(void) {
	PackedCol white = PACKEDCOL_WHITE;
	Vec3 up = BM_Vec3(0,1,0), down = BM_Vec3(0,-1,0), side = BM_Vec3(1,0,0), front = BM_Vec3(0,0,-1);
	CHECK(BM_Shade(white, &up, false)    == white);
	CHECK(BM_Shade(white, &down, false)  == PackedCol_Scale(white, PACKEDCOL_SHADE_YMIN));
	CHECK(BM_Shade(white, &side, false)  == PackedCol_Scale(white, PACKEDCOL_SHADE_X));
	CHECK(BM_Shade(white, &front, false) == PackedCol_Scale(white, PACKEDCOL_SHADE_Z));
	CHECK(BM_Shade(white, &down, true)   == white);
}

static void TestExampleModel(const char* objPath, const char* pngPath) {
	cc_string obj = String_FromReadonly(objPath);
	cc_string png = String_FromReadonly(pngPath);
	const struct BM_Part* part;
	int i, tris;

	CHECK(BM_LoadObj(&obj));
	CHECK(bm_loaded);
	CHECK(bm_badFaces == 0);
	CHECK(bm_numParts == 6);
	tris = bm_staticTris + bm_animTris;
	CHECK(tris == 6 * 12); /* six boxes of 12 triangles */
	CHECK(bm_numCorners == tris * 3);
	CHECK(bm_staticTris == 12);
	CHECK(bm_animTris == 60);

	/* Bounds: 2 blocks tall humanoid-ish robot, feet at y = 0 */
	CHECK(NEAR(bm_min.y, 0.0f) && NEAR(bm_max.y, 2.0f));
	CHECK(NEAR(bm_min.x, -0.5f) && NEAR(bm_max.x, 0.5f));
	CHECK(NEAR(bm_nameY, 2.1f));
	CHECK(bm_hasEye && NEAR(bm_eyeY, 1.625f));
	CHECK(bm_hasSize && NEAR(bm_size.y, 28.1f / 16.0f));

	part = FindPart("Head");  CHECK(part && part->kind == BM_PART_HEAD);
	CHECK(part && part->hasPivot && NEAR(part->pivot.y, 1.5f));
	part = FindPart("Body");  CHECK(part && part->kind == BM_PART_STATIC);
	part = FindPart("Arm.L"); CHECK(part && part->kind == BM_PART_LEFT_ARM);
	part = FindPart("Arm.R"); CHECK(part && part->kind == BM_PART_RIGHT_ARM);
	CHECK(bm_armPart >= 0 && &bm_parts[bm_armPart] == part);
	part = FindPart("Leg.L"); CHECK(part && part->kind == BM_PART_LEFT_LEG);
	/* Leg.R has no pivot line in the file, so it swings from the top centre of its box */
	part = FindPart("Leg.R"); CHECK(part && part->kind == BM_PART_RIGHT_LEG);
	CHECK(part && !part->hasPivot && NEAR(part->pivot.y, 0.75f) && NEAR(part->pivot.x, 0.125f));

	/* Every corner must have a unit normal and UVs inside the texture */
	for (i = 0; i < bm_numCorners; i++) {
		const struct BM_Corner* c = &bm_corners[i];
		CHECK(NEAR(Vec3_LengthSquared(&c->nrm), 1.0f));
		CHECK(c->u >= 0 && c->u <= 1 && c->v >= 0 && c->v <= 1);
	}

	/* Texture: 48x40 gets padded to 64x64 and the UVs rescaled */
	CHECK(BM_LoadTexture(&png));
	CHECK(bm_bmp.width == 64 && bm_bmp.height == 64);
	CHECK(NEAR(bm_uScale, 48.0f / 64.0f) && NEAR(bm_vScale, 40.0f / 64.0f));
	/* Padding must be transparent, original pixels must be kept */
	CHECK(BitmapCol_A(Bitmap_GetPixel(&bm_bmp, 63, 63)) == 0);
	CHECK(BitmapCol_A(Bitmap_GetPixel(&bm_bmp, 0, 0)) == 255);

	/* Writing a triangle produces a degenerate quad with rescaled UVs */
	{
		struct VertexTextured quad[4];
		BM_WriteTri(quad, &bm_corners[0], PACKEDCOL_WHITE, true, NULL);
		CHECK(quad[3].x == quad[2].x && quad[3].y == quad[2].y && quad[3].z == quad[2].z);
		CHECK(NEAR(quad[0].U, bm_corners[0].u * bm_uScale));
		CHECK(quad[0].Col == PACKEDCOL_WHITE);
	}

	/* Reloading with a scale rescales positions, pivots and metadata */
	bm_scale = 2.0f;
	CHECK(BM_LoadObj(&obj));
	CHECK(NEAR(bm_max.y, 4.0f) && NEAR(bm_eyeY, 3.25f));
	part = FindPart("Head"); CHECK(part && NEAR(part->pivot.y, 3.0f));
	bm_scale = 1.0f;
}

static void TestEdgeCases(void) {
	cc_string path = String_FromConst("test/edge.obj");
	cc_string text = String_FromConst(
		"# comment only\n"
		"#pivot Thing 0 1 0\n"
		"v 0 0 0\r\n"
		"v 1 0 0\n"
		"v 1 1 0\n"
		"v 0 1 0\n"
		"vt 0 0\n"
		"vt 1 1\n"
		"g Thing\n"
		"f 1/1 2/2 3/1 4/2\n"     /* quad without normals: fan triangulated, normals computed */
		"f -1 -2 -3\n"            /* negative indices */
		"f 1 2 99\n"              /* bad index, skipped */
		"f 1 2\n"                 /* too few corners, skipped */
		"o Empty\n"               /* part without faces is dropped... */
		"o Last\n"                /* ...by being reused for the next named part */
		"f 1 2 3");               /* last line without newline */
	const struct BM_Part* part;
	cc_result res = Stream_WriteAllTo(&path, (const cc_uint8*)text.buffer, text.length);
	CHECK(res == 0);

	CHECK(BM_LoadObj(&path));
	CHECK(bm_badFaces == 2);
	CHECK(bm_numParts == 2);
	CHECK(bm_numCorners == (2 + 1 + 1) * 3);
	CHECK(FindPart("Empty") == NULL);
	part = FindPart("Last");
	CHECK(part && part->count == 3 && part->first == 9);
	part = FindPart("Thing");
	CHECK(part && part->count == 9);
	CHECK(part && part->hasPivot && NEAR(part->pivot.y, 1.0f));
	CHECK(part && part->kind == BM_PART_STATIC);
	/* Face lies in the XY plane and is wound counter-clockwise, so its normal points at +Z */
	CHECK(NEAR(bm_corners[0].nrm.z, 1.0f));
	/* OBJ V is flipped so that 0 is the top of the texture */
	CHECK(NEAR(bm_corners[0].v, 1.0f) && NEAR(bm_corners[1].v, 0.0f));
	CHECK(bm_armPart < 0);
}

int main(int argc, char** argv) {
	if (argc < 3) { printf("usage: test_load model.obj model.png\n"); return 2; }
	Platform_Init();

	TestParsers();
	TestClassify();
	TestRotation();
	TestShade();
	TestExampleModel(argv[1], argv[2]);
	TestEdgeCases();
	BM_Free();

	if (failures) { printf("%d check(s) failed\n", failures); return 1; }
	printf("All BlenderModel checks passed\n");
	return 0;
}
