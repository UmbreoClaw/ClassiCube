/*
 * BlenderModel - ClassiCube plugin
 *
 * Loads a Wavefront OBJ mesh (exported from Blender) plus a PNG texture and
 * registers it as the "blender" entity model. The model can then be used as
 * the local player's model, and is rendered in both third person (the whole
 * mesh) and first person (the part named like an arm, drawn as the held arm).
 *
 * Everything is client side: other players still see whatever model the
 * server tells them about.
 *
 * Mesh conventions (Blender's default OBJ export settings already match):
 *   - 1 OBJ unit = 1 block (blendermodel-scale multiplies this)
 *   - Y is up, the model faces -Z, faces are wound counter-clockwise
 *   - texture coordinates use the usual OBJ layout (V = 0 at the bottom)
 *   - each Blender object becomes an "o <name>" part. A part named like
 *     head / arm / leg (with .L/.R, _L/_R or left/right for the side) is
 *     animated the same way ClassiCube animates the humanoid model.
 *
 * Optional metadata comment lines (written by blender_export_classicube.py):
 *   # pivot <part name> <x> <y> <z>   rotation origin of an animated part
 *   # eye <y>                          eye height above the feet (blocks)
 *   # size <w> <h> <l>                 collision box size (blocks)
 *
 * Licensed under BSD-3, same as ClassiCube.
 */
#include "src/PluginAPI.h"
#include "src/Bitmap.h"
#include "src/Chat.h"
#include "src/Commands.h"
#include "src/Entity.h"
#include "src/Errors.h"
#include "src/Event.h"
#include "src/ExtMath.h"
#include "src/Game.h"
#include "src/Graphics.h"
#include "src/Model.h"
#include "src/Options.h"
#include "src/PackedCol.h"
#include "src/Platform.h"
#include "src/Stream.h"
#include "src/String_.h"
#include "src/Vectors.h"

#define BM_MODEL_NAME  "blender"
#define BM_PREFIX      "&eBlenderModel: &f"
#define BM_MAX_PARTS   128
#define BM_MAX_POLY    64
#define BM_LINE_SIZE   1024

/* The game draws quads through a shared 16-bit index buffer, so a single */
/* vertex buffer holds at most 65536 vertices. Triangles are uploaded as   */
/* degenerate quads (4 vertices each), which gives 16383 triangles per VB. */
#define BM_CHUNK_TRIS  16383
#define BM_CHUNK_VERTS (BM_CHUNK_TRIS * 4)
#define BM_MAX_CHUNKS  64

#define OPT_BM_FILE     "blendermodel-file"
#define OPT_BM_TEXTURE  "blendermodel-texture"
#define OPT_BM_SCALE    "blendermodel-scale"
#define OPT_BM_ARMSCALE "blendermodel-arm-scale"
#define OPT_BM_AUTO     "blendermodel-auto-apply"
#define OPT_BM_FORCE    "blendermodel-force"

#define BM_DEFAULT_FILE "blendermodel/model.obj"

/* Values of the humanoid model, used as fallbacks */
#define BM_HUMAN_EYE_Y   (26.0f / 16.0f)
#define BM_HUMAN_SIZE_X  (8.6f  / 16.0f)
#define BM_HUMAN_SIZE_Y  (28.1f / 16.0f)
#define BM_HUMAN_SIZE_Z  (8.6f  / 16.0f)
#define BM_HUMAN_ARM_PIVOT_X (5.0f  / 16.0f)
#define BM_HUMAN_ARM_PIVOT_Y (22.0f / 16.0f)

#ifdef BM_TEST
	/* Test harness runs without the game, so route messages to the log */
	#define BM_Msg(msg)         Platform_LogConst(msg)
	#define BM_Msg1(msg, a)     Platform_Log1(msg, a)
	#define BM_Msg2(msg, a, b)  Platform_Log2(msg, a, b)
#else
	#define BM_Msg(msg)         do { cc_string bm_str_ = String_FromConst(BM_PREFIX msg); Chat_Add(&bm_str_); } while (0)
	#define BM_Msg1(msg, a)     Chat_Add1(BM_PREFIX msg, a)
	#define BM_Msg2(msg, a, b)  Chat_Add2(BM_PREFIX msg, a, b)
#endif

enum BM_PartKind {
	BM_PART_STATIC, BM_PART_HEAD,
	BM_PART_LEFT_ARM, BM_PART_RIGHT_ARM,
	BM_PART_LEFT_LEG, BM_PART_RIGHT_LEG,
	BM_PART_COUNT
};
static const char* const bm_kindNames[BM_PART_COUNT] = {
	"static", "head", "left arm", "right arm", "left leg", "right leg"
};

struct BM_Corner { Vec3 pos, nrm; float u, v; };

struct BM_Part {
	char name[STRING_SIZE];
	int nameLen;
	int kind;
	int first, count; /* range of corners, count is always a multiple of 3 */
	Vec3 min, max, pivot;
	cc_bool hasPivot;
};

struct BM_Pivot { char name[STRING_SIZE]; int nameLen; Vec3 pos; };


/*########################################################################################################################*
*-----------------------------------------------------------State---------------------------------------------------------*
*#########################################################################################################################*/
/* Loaded mesh */
static struct BM_Corner* bm_corners;
static int bm_numCorners, bm_capCorners;
static struct BM_Part bm_parts[BM_MAX_PARTS];
static int bm_numParts;
static Vec3 bm_min, bm_max, bm_size;
static float bm_eyeY, bm_nameY;
static cc_bool bm_hasEye, bm_hasSize, bm_loaded;
static int bm_staticTris, bm_animTris, bm_armPart = -1;

/* Parser scratch state */
static Vec3*  bm_pos; static int bm_numPos, bm_capPos;
static Vec2*  bm_uv;  static int bm_numUV,  bm_capUV;
static Vec3*  bm_nrm; static int bm_numNrm, bm_capNrm;
static struct BM_Pivot bm_pivots[BM_MAX_PARTS];
static int bm_numPivots, bm_badFaces;
static cc_bool bm_seenObject;

/* Texture */
static struct Bitmap bm_bmp;
static GfxResourceID bm_texId;
static float bm_uScale = 1.0f, bm_vScale = 1.0f;
static struct ModelTex bm_modelTex = { "blendermodel.png" };

/* GPU buffers */
static GfxResourceID bm_chunkVbs[BM_MAX_CHUNKS];
static int bm_chunkCounts[BM_MAX_CHUNKS];
static int bm_numChunks;
static PackedCol bm_chunkCol;
static cc_bool bm_chunkNoShade, bm_chunksBuilt, bm_chunksFailed;
static GfxResourceID bm_dynVb;
static int bm_dynCap;

/* Settings */
static float bm_scale = 1.0f, bm_armScale = 1.0f;
static cc_bool bm_autoApply = true, bm_force;
static char bm_pathBuffer[FILENAME_SIZE];
static cc_string bm_path = String_FromArray(bm_pathBuffer);
static char bm_texPathBuffer[FILENAME_SIZE];
static cc_string bm_texPath = String_FromArray(bm_texPathBuffer);

static struct Model bm_model;


/*########################################################################################################################*
*---------------------------------------------------------Utilities-------------------------------------------------------*
*#########################################################################################################################*/
static cc_bool BM_IsSpace(char c) { return c == ' ' || c == '\t'; }

/* Takes the next whitespace separated token from line, advancing line past it */
static cc_bool BM_NextToken(cc_string* line, cc_string* tok) {
	int i = 0, start;
	while (i < line->length && BM_IsSpace(line->buffer[i])) i++;
	start = i;
	while (i < line->length && !BM_IsSpace(line->buffer[i])) i++;

	*tok  = String_UNSAFE_Substring(line, start, i - start);
	*line = String_UNSAFE_SubstringAt(line, i);
	return tok->length > 0;
}

/* Parses a float, including the exponent form some OBJ writers use */
static cc_bool BM_ParseFloat(const cc_string* s, float* out) {
	const char* buf = s->buffer;
	int i = 0, len = s->length, digits = 0, expVal = 0;
	float sign = 1.0f, value = 0.0f, frac = 0.1f;
	cc_bool expNeg = false;

	if (i < len && (buf[i] == '-' || buf[i] == '+')) { if (buf[i] == '-') sign = -1.0f; i++; }
	while (i < len && buf[i] >= '0' && buf[i] <= '9') { value = value * 10.0f + (buf[i] - '0'); i++; digits++; }
	if (i < len && buf[i] == '.') {
		i++;
		while (i < len && buf[i] >= '0' && buf[i] <= '9') { value += (buf[i] - '0') * frac; frac *= 0.1f; i++; digits++; }
	}
	if (!digits) return false;

	if (i < len && (buf[i] == 'e' || buf[i] == 'E')) {
		i++;
		if (i < len && (buf[i] == '-' || buf[i] == '+')) { expNeg = buf[i] == '-'; i++; }
		if (i >= len) return false;
		while (i < len && buf[i] >= '0' && buf[i] <= '9') { expVal = expVal * 10 + (buf[i] - '0'); i++; }
	}
	if (i != len) return false;

	while (expVal-- > 0) value = expNeg ? value * 0.1f : value * 10.0f;
	*out = sign * value;
	return true;
}

static cc_bool BM_ParseInt(const cc_string* s, int* out) {
	const char* buf = s->buffer;
	int i = 0, len = s->length, value = 0, digits = 0;
	cc_bool neg = false;

	if (i < len && (buf[i] == '-' || buf[i] == '+')) { neg = buf[i] == '-'; i++; }
	while (i < len && buf[i] >= '0' && buf[i] <= '9') { value = value * 10 + (buf[i] - '0'); i++; digits++; }
	if (!digits || i != len) return false;

	*out = neg ? -value : value;
	return true;
}

static cc_bool BM_ParseFloats(cc_string* line, float* out, int count) {
	cc_string tok;
	int i;
	for (i = 0; i < count; i++) {
		if (!BM_NextToken(line, &tok) || !BM_ParseFloat(&tok, &out[i])) return false;
	}
	return true;
}

static int BM_NextPow2(int value) {
	int pow2 = 1;
	while (pow2 < value) pow2 <<= 1;
	return pow2;
}

static void* BM_Grow(void* mem, int* capacity, int needed, int elemSize, const char* place) {
	int newCap = *capacity ? *capacity : 256;
	void* newMem;
	while (newCap < needed) newCap *= 2;

	newMem = Mem_TryRealloc(mem, newCap, elemSize);
	if (!newMem) {
		BM_Msg1("Out of memory allocating %c", place);
		return NULL;
	}
	*capacity = newCap;
	return newMem;
}

static Vec3 BM_Vec3(float x, float y, float z) { Vec3 v; v.x = x; v.y = y; v.z = z; return v; }

static void BM_CopyName(char* dst, int* dstLen, const cc_string* src) {
	int len = src->length < STRING_SIZE ? src->length : STRING_SIZE;
	Mem_Copy(dst, src->buffer, len);
	*dstLen = len;
}

static cc_string BM_PartName(const struct BM_Part* part) {
	cc_string name;
	name.buffer   = (char*)part->name;
	name.length   = part->nameLen;
	name.capacity = STRING_SIZE;
	return name;
}


/*########################################################################################################################*
*------------------------------------------------------Model memory-------------------------------------------------------*
*#########################################################################################################################*/
static void BM_FreeChunks(void) {
	int i;
	for (i = 0; i < bm_numChunks; i++) {
		Gfx_DeleteVb(&bm_chunkVbs[i]);
	}
	bm_numChunks   = 0;
	bm_chunksBuilt = false;
}

static void BM_FreeGpu(void) {
	BM_FreeChunks();
	Gfx_DeleteDynamicVb(&bm_dynVb);
	bm_dynCap = 0;
}

static void BM_FreeTexture(void) {
	Gfx_DeleteTexture(&bm_texId);
	bm_modelTex.texID = 0;
	Mem_Free(bm_bmp.scan0);
	bm_bmp.scan0 = NULL;
	bm_bmp.width = 0; bm_bmp.height = 0;
	bm_uScale = 1.0f; bm_vScale = 1.0f;
}

static void BM_FreeScratch(void) {
	Mem_Free(bm_pos); bm_pos = NULL; bm_numPos = 0; bm_capPos = 0;
	Mem_Free(bm_uv);  bm_uv  = NULL; bm_numUV  = 0; bm_capUV  = 0;
	Mem_Free(bm_nrm); bm_nrm = NULL; bm_numNrm = 0; bm_capNrm = 0;
	bm_numPivots  = 0;
	bm_seenObject = false;
}

static void BM_ResetMesh(void) {
	BM_FreeGpu();
	BM_FreeScratch();
	Mem_Free(bm_corners);
	bm_corners = NULL; bm_numCorners = 0; bm_capCorners = 0;

	Mem_Set(bm_parts, 0, sizeof(bm_parts));
	bm_numParts   = 0;
	bm_badFaces   = 0;
	bm_loaded     = false;
	bm_hasEye     = false;
	bm_hasSize    = false;
	bm_staticTris = 0;
	bm_animTris   = 0;
	bm_armPart    = -1;
	bm_chunksFailed = false;
}


/*########################################################################################################################*
*--------------------------------------------------------OBJ parsing------------------------------------------------------*
*#########################################################################################################################*/
static struct BM_Part* BM_BeginPart(const cc_string* name) {
	struct BM_Part* part;
	cc_string trimmed = *name;
	String_UNSAFE_TrimStart(&trimmed);
	String_UNSAFE_TrimEnd(&trimmed);

	/* Reuse the current part if nothing has been added to it yet */
	if (bm_numParts && bm_parts[bm_numParts - 1].count == 0) {
		part = &bm_parts[bm_numParts - 1];
	} else {
		if (bm_numParts >= BM_MAX_PARTS) return NULL;
		part = &bm_parts[bm_numParts++];
		Mem_Set(part, 0, sizeof(*part));
		part->first = bm_numCorners;
	}
	BM_CopyName(part->name, &part->nameLen, &trimmed);
	return part;
}

static struct BM_Part* BM_CurrentPart(void) {
	static const cc_string defaultName = String_FromConst("model");
	if (!bm_numParts) BM_BeginPart(&defaultName);
	return &bm_parts[bm_numParts - 1];
}

static int BM_ResolveIndex(int idx, int count) {
	if (idx > 0) return idx <= count ? idx - 1 : -1;
	if (idx < 0) return count + idx >= 0 ? count + idx : -1;
	return -1;
}

static void BM_AddTriangle(const struct BM_Corner* a, const struct BM_Corner* b, const struct BM_Corner* c) {
	struct BM_Corner tri[3];
	struct BM_Part* part = BM_CurrentPart();
	Vec3 e1, e2, n;
	int i;
	if (!part) return;

	tri[0] = *a; tri[1] = *b; tri[2] = *c;
	/* Fill in the face normal for corners that didn't come with one */
	Vec3_Sub(&e1, &b->pos, &a->pos);
	Vec3_Sub(&e2, &c->pos, &a->pos);
	n.x = e1.y * e2.z - e1.z * e2.y;
	n.y = e1.z * e2.x - e1.x * e2.z;
	n.z = e1.x * e2.y - e1.y * e2.x;
	if (Vec3_LengthSquared(&n) > 0.0f) {
		float len = Math_SqrtF(Vec3_LengthSquared(&n));
		Vec3_Mul1By(&n, 1.0f / len);
	}

	for (i = 0; i < 3; i++) {
		if (Vec3_IsZero(tri[i].nrm)) tri[i].nrm = n;
	}

	if (bm_numCorners + 3 > bm_capCorners) {
		void* mem = BM_Grow(bm_corners, &bm_capCorners, bm_numCorners + 3, sizeof(struct BM_Corner), "corners");
		if (!mem) return;
		bm_corners = (struct BM_Corner*)mem;
	}
	Mem_Copy(&bm_corners[bm_numCorners], tri, sizeof(tri));
	bm_numCorners += 3;
	part->count   += 3;
}

static void BM_ParseFace(cc_string* line) {
	struct BM_Corner poly[BM_MAX_POLY];
	cc_string tok, subs[3];
	int n = 0, numSubs, idx, i;

	while (n < BM_MAX_POLY && BM_NextToken(line, &tok)) {
		struct BM_Corner* c = &poly[n];
		Mem_Set(c, 0, sizeof(*c));
		numSubs = String_UNSAFE_Split(&tok, '/', subs, 3);

		if (!BM_ParseInt(&subs[0], &idx) || (idx = BM_ResolveIndex(idx, bm_numPos)) < 0) { bm_badFaces++; return; }
		c->pos = bm_pos[idx];

		if (numSubs >= 2 && subs[1].length) {
			if (!BM_ParseInt(&subs[1], &idx) || (idx = BM_ResolveIndex(idx, bm_numUV)) < 0) { bm_badFaces++; return; }
			c->u = bm_uv[idx].x; c->v = bm_uv[idx].y;
		}
		if (numSubs >= 3 && subs[2].length) {
			if (!BM_ParseInt(&subs[2], &idx) || (idx = BM_ResolveIndex(idx, bm_numNrm)) < 0) { bm_badFaces++; return; }
			c->nrm = bm_nrm[idx];
		}
		n++;
	}
	if (n < 3) { bm_badFaces++; return; }

	/* Fan triangulation (Blender exports convex polygons, or triangles when asked) */
	for (i = 1; i + 1 < n; i++) {
		BM_AddTriangle(&poly[0], &poly[i], &poly[i + 1]);
	}
}

/* key is the word following '#', line is the rest of the comment */
static void BM_ParseMetadata(cc_string* key, cc_string* line) {
	cc_string name;
	float f[3];
	/* "# pivot" has the keyword as the next token, "#pivot" already has it */
	if (!key->length && !BM_NextToken(line, key)) return;

	if (String_CaselessEqualsConst(key, "pivot")) {
		struct BM_Pivot* pivot;
		if (!BM_NextToken(line, &name) || !BM_ParseFloats(line, f, 3)) return;
		if (bm_numPivots >= BM_MAX_PARTS) return;

		pivot = &bm_pivots[bm_numPivots++];
		BM_CopyName(pivot->name, &pivot->nameLen, &name);
		pivot->pos = BM_Vec3(f[0] * bm_scale, f[1] * bm_scale, f[2] * bm_scale);
	} else if (String_CaselessEqualsConst(key, "eye")) {
		if (!BM_ParseFloats(line, f, 1)) return;
		bm_eyeY  = f[0] * bm_scale;
		bm_hasEye = true;
	} else if (String_CaselessEqualsConst(key, "size")) {
		if (!BM_ParseFloats(line, f, 3)) return;
		bm_size    = BM_Vec3(f[0] * bm_scale, f[1] * bm_scale, f[2] * bm_scale);
		bm_hasSize = true;
	}
}

static void BM_ParseLine(cc_string* line) {
	cc_string key;
	float f[3];
	void* mem;

	String_UNSAFE_TrimEnd(line);
	if (!BM_NextToken(line, &key)) return;

	if (key.buffer[0] == '#') {
		cc_string word = String_UNSAFE_SubstringAt(&key, 1);
		BM_ParseMetadata(&word, line);
	} else if (String_CaselessEqualsConst(&key, "v")) {
		if (!BM_ParseFloats(line, f, 3)) return;
		if (bm_numPos >= bm_capPos) {
			if (!(mem = BM_Grow(bm_pos, &bm_capPos, bm_numPos + 1, sizeof(Vec3), "positions"))) return;
			bm_pos = (Vec3*)mem;
		}
		bm_pos[bm_numPos++] = BM_Vec3(f[0] * bm_scale, f[1] * bm_scale, f[2] * bm_scale);
	} else if (String_CaselessEqualsConst(&key, "vt")) {
		if (!BM_ParseFloats(line, f, 2)) return;
		if (bm_numUV >= bm_capUV) {
			if (!(mem = BM_Grow(bm_uv, &bm_capUV, bm_numUV + 1, sizeof(Vec2), "texture coords"))) return;
			bm_uv = (Vec2*)mem;
		}
		/* OBJ V runs bottom to top, textures in the game run top to bottom */
		bm_uv[bm_numUV].x = f[0];
		bm_uv[bm_numUV].y = 1.0f - f[1];
		bm_numUV++;
	} else if (String_CaselessEqualsConst(&key, "vn")) {
		if (!BM_ParseFloats(line, f, 3)) return;
		if (bm_numNrm >= bm_capNrm) {
			if (!(mem = BM_Grow(bm_nrm, &bm_capNrm, bm_numNrm + 1, sizeof(Vec3), "normals"))) return;
			bm_nrm = (Vec3*)mem;
		}
		bm_nrm[bm_numNrm++] = BM_Vec3(f[0], f[1], f[2]);
	} else if (String_CaselessEqualsConst(&key, "f")) {
		BM_ParseFace(line);
	} else if (String_CaselessEqualsConst(&key, "o")) {
		bm_seenObject = true;
		BM_BeginPart(line);
	} else if (String_CaselessEqualsConst(&key, "g")) {
		/* Only use groups as parts when the file doesn't have objects */
		if (!bm_seenObject) BM_BeginPart(line);
	}
	/* usemtl, mtllib, s etc are ignored: there's only ever one texture */
}


/*########################################################################################################################*
*----------------------------------------------------Part classification--------------------------------------------------*
*#########################################################################################################################*/
enum BM_Side { BM_SIDE_NONE, BM_SIDE_LEFT, BM_SIDE_RIGHT };

static cc_bool BM_NameHas(const cc_string* name, const char* sub) {
	cc_string s = String_FromReadonly(sub);
	return String_CaselessContains(name, &s);
}

static int BM_SideFromAffix(const cc_string* name, const char* affixes) {
	/* affixes: e.g. ".l" "_l" "-l" "l." "l_" "l-" */
	int i, len = name->length;
	char first, last, sep;
	if (len < 3) return BM_SIDE_NONE;

	first = name->buffer[0]     | 0x20;
	last  = name->buffer[len-1] | 0x20;
	for (i = 0; affixes[i]; i++) {
		sep = affixes[i];
		if (name->buffer[len-2] == sep && last  == 'l') return BM_SIDE_LEFT;
		if (name->buffer[len-2] == sep && last  == 'r') return BM_SIDE_RIGHT;
		if (name->buffer[1]     == sep && first == 'l') return BM_SIDE_LEFT;
		if (name->buffer[1]     == sep && first == 'r') return BM_SIDE_RIGHT;
	}
	return BM_SIDE_NONE;
}

static int BM_SideOf(const cc_string* name) {
	if (BM_NameHas(name, "left"))  return BM_SIDE_LEFT;
	if (BM_NameHas(name, "right")) return BM_SIDE_RIGHT;
	return BM_SideFromAffix(name, "._-");
}

static int BM_ClassifyName(const cc_string* name) {
	int side = BM_SideOf(name);
	if (BM_NameHas(name, "head")) return BM_PART_HEAD;
	if (BM_NameHas(name, "arm") || BM_NameHas(name, "hand")) {
		return side == BM_SIDE_LEFT ? BM_PART_LEFT_ARM : BM_PART_RIGHT_ARM;
	}
	if (BM_NameHas(name, "leg") || BM_NameHas(name, "foot")) {
		return side == BM_SIDE_LEFT ? BM_PART_LEFT_LEG : BM_PART_RIGHT_LEG;
	}
	return BM_PART_STATIC;
}

static void BM_ComputePartBounds(struct BM_Part* part) {
	int i;
	part->min = Vec3_BigPos();
	Vec3_Negate(&part->max, &part->min);

	for (i = part->first; i < part->first + part->count; i++) {
		Vec3 p = bm_corners[i].pos;
		if (p.x < part->min.x) part->min.x = p.x;
		if (p.y < part->min.y) part->min.y = p.y;
		if (p.z < part->min.z) part->min.z = p.z;
		if (p.x > part->max.x) part->max.x = p.x;
		if (p.y > part->max.y) part->max.y = p.y;
		if (p.z > part->max.z) part->max.z = p.z;
	}
}

static void BM_AssignPivot(struct BM_Part* part) {
	cc_string name = BM_PartName(part), pivotName;
	float cx = (part->min.x + part->max.x) * 0.5f;
	float cz = (part->min.z + part->max.z) * 0.5f;
	int i;

	for (i = 0; i < bm_numPivots; i++) {
		pivotName.buffer   = bm_pivots[i].name;
		pivotName.length   = bm_pivots[i].nameLen;
		pivotName.capacity = STRING_SIZE;
		if (!String_CaselessEquals(&name, &pivotName)) continue;

		part->pivot    = bm_pivots[i].pos;
		part->hasPivot = true;
		return;
	}

	/* No pivot given: heads turn about their base, limbs swing from their top */
	if (part->kind == BM_PART_HEAD) {
		part->pivot = BM_Vec3(cx, part->min.y, cz);
	} else {
		part->pivot = BM_Vec3(cx, part->max.y, cz);
	}
}

static void BM_FinishLoad(void) {
	int i, kept = 0, animTris;

	/* Drop parts that ended up without any faces */
	for (i = 0; i < bm_numParts; i++) {
		if (bm_parts[i].count == 0) continue;
		if (kept != i) bm_parts[kept] = bm_parts[i];
		kept++;
	}
	bm_numParts = kept;

	bm_min = Vec3_BigPos();
	Vec3_Negate(&bm_max, &bm_min);
	if (!bm_numParts) { Vec3_Set(bm_min, 0,0,0); Vec3_Set(bm_max, 0,0,0); }

	for (i = 0; i < bm_numParts; i++) {
		struct BM_Part* part = &bm_parts[i];
		cc_string name = BM_PartName(part);
		BM_ComputePartBounds(part);
		part->kind = BM_ClassifyName(&name);
		BM_AssignPivot(part);

		if (part->min.x < bm_min.x) bm_min.x = part->min.x;
		if (part->min.y < bm_min.y) bm_min.y = part->min.y;
		if (part->min.z < bm_min.z) bm_min.z = part->min.z;
		if (part->max.x > bm_max.x) bm_max.x = part->max.x;
		if (part->max.y > bm_max.y) bm_max.y = part->max.y;
		if (part->max.z > bm_max.z) bm_max.z = part->max.z;
	}

	/* All animated parts share one dynamic vertex buffer, so demote parts */
	/* from the end until the animated triangles fit in it */
	animTris = 0;
	for (i = 0; i < bm_numParts; i++) {
		if (bm_parts[i].kind != BM_PART_STATIC) animTris += bm_parts[i].count / 3;
	}
	for (i = bm_numParts - 1; i >= 0 && animTris > BM_CHUNK_TRIS; i--) {
		struct BM_Part* part = &bm_parts[i];
		cc_string name;
		if (part->kind == BM_PART_STATIC) continue;

		name = BM_PartName(part);
		BM_Msg1("Too many animated triangles, part %s will not animate", &name);
		animTris  -= part->count / 3;
		part->kind = BM_PART_STATIC;
	}

	bm_staticTris = 0; bm_animTris = 0; bm_armPart = -1;
	for (i = 0; i < bm_numParts; i++) {
		struct BM_Part* part = &bm_parts[i];
		if (part->kind == BM_PART_STATIC) { bm_staticTris += part->count / 3; }
		else { bm_animTris += part->count / 3; }

		/* Prefer the right arm for first person, like the humanoid model does */
		if (part->kind == BM_PART_RIGHT_ARM && (bm_armPart < 0 || bm_parts[bm_armPart].kind != BM_PART_RIGHT_ARM)) bm_armPart = i;
		if (part->kind == BM_PART_LEFT_ARM  && bm_armPart < 0) bm_armPart = i;
	}

	if (bm_armPart >= 0 && bm_parts[bm_armPart].count / 3 > BM_CHUNK_TRIS) {
		BM_Msg("Arm part has too many triangles, first person arm will be cut off");
	}

	if (!bm_hasEye)  bm_eyeY = bm_min.y + (bm_max.y - bm_min.y) * (BM_HUMAN_EYE_Y / 2.0f);
	if (!bm_hasSize) bm_size = BM_Vec3(BM_HUMAN_SIZE_X, BM_HUMAN_SIZE_Y, BM_HUMAN_SIZE_Z);
	bm_nameY  = bm_max.y + 0.1f;
	bm_loaded = bm_numCorners > 0;
}

static cc_bool BM_LoadObj(const cc_string* path) {
	struct Stream file, buffered;
	cc_uint8 buffer[4096];
	char lineBuffer[BM_LINE_SIZE];
	cc_string line;
	cc_result res;

	res = Stream_OpenFile(&file, path);
	if (res) { BM_Msg2("Couldn't open %s (error %h)", path, &res); return false; }
	Stream_ReadonlyBuffered(&buffered, &file, buffer, sizeof(buffer));
	BM_ResetMesh();

	for (;;) {
		String_InitArray(line, lineBuffer);
		res = Stream_ReadLine(&buffered, &line);
		if (res == ERR_END_OF_STREAM) break;
		if (res) { BM_Msg2("Error reading %s (error %h)", path, &res); break; }
		BM_ParseLine(&line);
	}
	file.Close(&file);

	BM_FinishLoad();
	if (bm_badFaces) BM_Msg1("Skipped %i faces with invalid indices", &bm_badFaces);
	BM_FreeScratch();

	if (!bm_loaded) { BM_Msg1("%s doesn't contain any faces", path); return false; }
	return true;
}


/*########################################################################################################################*
*----------------------------------------------------------Texture--------------------------------------------------------*
*#########################################################################################################################*/
static cc_bool BM_LoadTexture(const cc_string* path) {
	struct Stream stream;
	struct Bitmap bmp;
	cc_result res;
	int w, h, w2, h2, y;

	res = Stream_OpenFile(&stream, path);
	if (res) { BM_Msg2("Couldn't open texture %s (error %h)", path, &res); return false; }
	res = Png_Decode(&bmp, &stream);
	stream.Close(&stream);
	if (res) {
		BM_Msg2("Couldn't decode texture %s (error %h)", path, &res);
		Mem_Free(bmp.scan0);
		return false;
	}
	BM_FreeTexture();

	w = bmp.width; h = bmp.height;
	if (Gfx.MaxTexWidth && (w > Gfx.MaxTexWidth || h > Gfx.MaxTexHeight)) {
		BM_Msg2("Texture is too large for this GPU (max %i x %i)", &Gfx.MaxTexWidth, &Gfx.MaxTexHeight);
		Mem_Free(bmp.scan0);
		return false;
	}

	/* Textures must have power of two dimensions, so pad and rescale UVs */
	w2 = BM_NextPow2(w); h2 = BM_NextPow2(h);
	if (w2 != w || h2 != h) {
		BitmapCol* padded = (BitmapCol*)Mem_TryAllocCleared(w2 * h2, BITMAPCOLOR_SIZE);
		if (!padded) { BM_Msg("Out of memory padding texture"); Mem_Free(bmp.scan0); return false; }

		for (y = 0; y < h; y++) {
			Mem_Copy(padded + y * w2, bmp.scan0 + y * w, w * BITMAPCOLOR_SIZE);
		}
		Mem_Free(bmp.scan0);
		bmp.scan0 = padded; bmp.width = w2; bmp.height = h2;
	}
	bm_uScale = (float)w / w2;
	bm_vScale = (float)h / h2;
	bm_bmp    = bmp;
	return true;
}

static cc_bool BM_EnsureTexture(void) {
	if (bm_texId) return true;
	if (!bm_bmp.scan0 || Gfx.LostContext) return false;

	bm_texId = Gfx_CreateTexture(&bm_bmp, TEXTURE_FLAG_MANAGED, false);
	bm_modelTex.texID = bm_texId;
	return bm_texId != 0;
}


/*########################################################################################################################*
*---------------------------------------------------------Rendering-------------------------------------------------------*
*#########################################################################################################################*/
struct BM_Rot {
	float cosX, sinX, cosY, sinY, cosZ, sinZ;
	Vec3 pivot;
	int order;
	cc_bool head;
};

/* Describes how a corner is moved before being uploaded */
struct BM_Xform {
	Vec3 sub, add;
	float scale;
	const struct BM_Rot* rot; /* may be NULL */
};

static void BM_Rot_Init(struct BM_Rot* r, Vec3 pivot, float angleX, float angleY, float angleZ, int order, cc_bool head) {
	r->cosX = Math_CosF(-angleX); r->sinX = Math_SinF(-angleX);
	r->cosY = Math_CosF(-angleY); r->sinY = Math_SinF(-angleY);
	r->cosZ = Math_CosF(-angleZ); r->sinZ = Math_SinF(-angleZ);
	r->pivot = pivot; r->order = order; r->head = head;
}

/* Same maths as Model_DrawRotate, but for an arbitrary pivot */
#define BM_RotateX t = cosX * v.y + sinX * v.z; v.z = -sinX * v.y + cosX * v.z; v.y = t;
#define BM_RotateY t = cosY * v.x - sinY * v.z; v.z =  sinY * v.x + cosY * v.z; v.x = t;
#define BM_RotateZ t = cosZ * v.x + sinZ * v.y; v.y = -sinZ * v.x + cosZ * v.y; v.x = t;

static Vec3 BM_Rot_Apply(const struct BM_Rot* r, Vec3 v) {
	float cosX = r->cosX, sinX = r->sinX;
	float cosY = r->cosY, sinY = r->sinY;
	float cosZ = r->cosZ, sinZ = r->sinZ;
	float t;
	Vec3_SubBy(&v, &r->pivot);

	switch (r->order) {
	case ROTATE_ORDER_ZYX: BM_RotateZ BM_RotateY BM_RotateX break;
	case ROTATE_ORDER_XZY: BM_RotateX BM_RotateZ BM_RotateY break;
	case ROTATE_ORDER_YZX: BM_RotateY BM_RotateZ BM_RotateX break;
	case ROTATE_ORDER_XYZ: BM_RotateX BM_RotateY BM_RotateZ break;
	}

	if (r->head) {
		t = Models.cosHead * v.x - Models.sinHead * v.z; v.z = Models.sinHead * v.x + Models.cosHead * v.z; v.x = t;
	}
	Vec3_AddBy(&v, &r->pivot);
	return v;
}

/* Approximates the game's per-face shading (top bright, bottom darkest, sides in between) */
static PackedCol BM_Shade(PackedCol col, const Vec3* n, cc_bool noShade) {
	float ax = Math_AbsF(n->x), ay = Math_AbsF(n->y), az = Math_AbsF(n->z);
	float sum = ax + ay + az, factor;
	if (noShade || sum < 0.0001f) return col;

	factor = ax * PACKEDCOL_SHADE_X + az * PACKEDCOL_SHADE_Z;
	factor += n->y >= 0 ? ay : ay * PACKEDCOL_SHADE_YMIN;
	return PackedCol_Scale(col, factor / sum);
}

/* Writes one triangle as a degenerate quad (4 vertices, last one repeated) */
static void BM_WriteTri(struct VertexTextured* dst, const struct BM_Corner* c, PackedCol col, cc_bool noShade, const struct BM_Xform* x) {
	int i;
	for (i = 0; i < 3; i++, dst++) {
		Vec3 p = c[i].pos;
		if (x) {
			Vec3_SubBy(&p, &x->sub);
			Vec3_Mul1By(&p, x->scale);
			Vec3_AddBy(&p, &x->add);
			if (x->rot) p = BM_Rot_Apply(x->rot, p);
		}
		dst->x = p.x; dst->y = p.y; dst->z = p.z;
		dst->Col = BM_Shade(col, &c[i].nrm, noShade);
		dst->U   = c[i].u * bm_uScale;
		dst->V   = c[i].v * bm_vScale;
	}
	*dst = *(dst - 1);
}

/* Iterates the triangles of static parts across part boundaries */
static const struct BM_Corner* BM_NextStaticTri(int* part, int* tri) {
	while (*part < bm_numParts) {
		const struct BM_Part* p = &bm_parts[*part];
		if (p->kind == BM_PART_STATIC && *tri < p->count / 3) {
			const struct BM_Corner* c = &bm_corners[p->first + (*tri) * 3];
			(*tri)++;
			return c;
		}
		(*part)++; *tri = 0;
	}
	return NULL;
}

static cc_bool BM_BuildChunks(PackedCol col, cc_bool noShade) {
	int remaining = bm_staticTris, part = 0, tri = 0, i, tris;
	struct VertexTextured* dst;
	GfxResourceID vb;
	BM_FreeChunks();

	while (remaining > 0 && bm_numChunks < BM_MAX_CHUNKS) {
		tris = remaining < BM_CHUNK_TRIS ? remaining : BM_CHUNK_TRIS;
		vb   = Gfx_TryCreateStaticVb(VERTEX_FORMAT_TEXTURED, tris * 4);
		if (!vb) {
			BM_Msg("Out of video memory, model can't be drawn");
			BM_FreeChunks();
			bm_chunksFailed = true;
			return false;
		}

		dst = (struct VertexTextured*)Gfx_LockVb(vb, VERTEX_FORMAT_TEXTURED, tris * 4);
		for (i = 0; i < tris; i++, dst += 4) {
			const struct BM_Corner* c = BM_NextStaticTri(&part, &tri);
			if (!c) break;
			BM_WriteTri(dst, c, col, noShade, NULL);
		}
		Gfx_UnlockVb(vb);

		bm_chunkVbs[bm_numChunks]    = vb;
		bm_chunkCounts[bm_numChunks] = tris * 4;
		bm_numChunks++;
		remaining -= tris;
	}

	bm_chunkCol     = col;
	bm_chunkNoShade = noShade;
	bm_chunksBuilt  = true;
	return true;
}

static cc_bool BM_EnsureDynVb(void) {
	int armTris = bm_armPart >= 0 ? bm_parts[bm_armPart].count / 3 : 0;
	int needed  = bm_animTris > armTris ? bm_animTris : armTris;
	if (needed > BM_CHUNK_TRIS) needed = BM_CHUNK_TRIS;
	needed *= 4;
	if (!needed) return false;

	if (bm_dynVb && bm_dynCap >= needed) return true;
	Gfx_DeleteDynamicVb(&bm_dynVb);
	bm_dynVb  = Gfx_CreateDynamicVb(VERTEX_FORMAT_TEXTURED, needed);
	bm_dynCap = needed;
	return bm_dynVb != 0;
}

/* Mirrors the rotations HumanModel_DrawCore applies to each body part */
static void BM_PartRot(const struct Entity* e, const struct BM_Part* part, struct BM_Rot* rot) {
	const struct AnimatedComp* a = &e->Anim;
	switch (part->kind) {
	case BM_PART_HEAD:
		BM_Rot_Init(rot, part->pivot, -e->Pitch * MATH_DEG2RAD, 0, 0, ROTATE_ORDER_ZYX, true); break;
	case BM_PART_LEFT_LEG:
		BM_Rot_Init(rot, part->pivot, a->LeftLegX,  0, a->LeftLegZ,  ROTATE_ORDER_ZYX, false); break;
	case BM_PART_RIGHT_LEG:
		BM_Rot_Init(rot, part->pivot, a->RightLegX, 0, a->RightLegZ, ROTATE_ORDER_ZYX, false); break;
	case BM_PART_LEFT_ARM:
		BM_Rot_Init(rot, part->pivot, a->LeftArmX,  0, a->LeftArmZ,  ROTATE_ORDER_XZY, false); break;
	case BM_PART_RIGHT_ARM:
		BM_Rot_Init(rot, part->pivot, a->RightArmX, 0, a->RightArmZ, ROTATE_ORDER_XZY, false); break;
	default:
		BM_Rot_Init(rot, part->pivot, 0, 0, 0, ROTATE_ORDER_ZYX, false); break;
	}
}

static void BM_DrawAnimated(struct Entity* e, PackedCol col) {
	struct VertexTextured* dst;
	struct BM_Xform xform;
	struct BM_Rot rot;
	int i, t, total = bm_animTris * 4;
	if (!bm_animTris || !BM_EnsureDynVb()) return;

	Mem_Set(&xform, 0, sizeof(xform));
	xform.scale = 1.0f;
	xform.rot   = &rot;

	dst = (struct VertexTextured*)Gfx_LockDynamicVb(bm_dynVb, VERTEX_FORMAT_TEXTURED, total);
	for (i = 0; i < bm_numParts; i++) {
		const struct BM_Part* part = &bm_parts[i];
		if (part->kind == BM_PART_STATIC) continue;
		BM_PartRot(e, part, &rot);

		for (t = 0; t < part->count / 3; t++, dst += 4) {
			BM_WriteTri(dst, &bm_corners[part->first + t * 3], col, e->NoShade, &xform);
		}
	}
	Gfx_UnlockDynamicVb(bm_dynVb);
	Gfx_DrawVb_IndexedTris(total);
}

static void BM_Draw(struct Entity* e) {
	PackedCol col = Models.Cols[0];
	int i;
	if (!bm_loaded || Gfx.LostContext || bm_chunksFailed) return;
	if (!BM_EnsureTexture()) return;
	Gfx_BindTexture(bm_texId);

	if (bm_staticTris) {
		/* Lighting is baked into the vertices, so rebuild when the entity's colour changes */
		if (!bm_chunksBuilt || bm_chunkCol != col || bm_chunkNoShade != e->NoShade) {
			if (!BM_BuildChunks(col, e->NoShade)) return;
		}
		for (i = 0; i < bm_numChunks; i++) {
			Gfx_BindVb(bm_chunkVbs[i]);
			Gfx_DrawVb_IndexedTris(bm_chunkCounts[i]);
		}
	}
	BM_DrawAnimated(e, col);
}

/* Called by Model_RenderArm, which has already set up the view matrix so that */
/* the humanoid's right arm appears in the bottom right of the screen */
static void BM_DrawArm(struct Entity* e) {
	const struct BM_Part* part;
	struct VertexTextured* dst;
	struct BM_Xform xform;
	struct BM_Rot rot;
	Vec3 armPivot;
	int t, tris, total;

	if (!bm_loaded || bm_armPart < 0 || Gfx.LostContext) return;
	if (!BM_EnsureTexture() || !BM_EnsureDynVb()) return;
	part  = &bm_parts[bm_armPart];
	tris  = part->count / 3;
	if (tris > BM_CHUNK_TRIS) tris = BM_CHUNK_TRIS;
	total = tris * 4;

	Gfx_BindTexture(bm_texId);
	Gfx_SetAlphaTest(true);

	/* Same pivot and angles as Model_DrawArmPart */
	armPivot = BM_Vec3(bm_model.armX / 16.0f, (bm_model.armY + bm_model.armY / 2) / 16.0f, 0.0f);
	if (Models.ClassicArms) {
		BM_Rot_Init(&rot, armPivot, 0, -90 * MATH_DEG2RAD, 120 * MATH_DEG2RAD, ROTATE_ORDER_YZX, false);
	} else {
		BM_Rot_Init(&rot, armPivot, -20 * MATH_DEG2RAD, -70 * MATH_DEG2RAD, 135 * MATH_DEG2RAD, ROTATE_ORDER_YZX, false);
	}

	/* Move the arm so its own pivot sits where the humanoid's shoulder is */
	xform.sub   = part->pivot;
	xform.scale = bm_armScale;
	xform.add   = BM_Vec3(BM_HUMAN_ARM_PIVOT_X, BM_HUMAN_ARM_PIVOT_Y, 0.0f);
	xform.rot   = &rot;

	dst = (struct VertexTextured*)Gfx_LockDynamicVb(bm_dynVb, VERTEX_FORMAT_TEXTURED, total);
	for (t = 0; t < tris; t++, dst += 4) {
		BM_WriteTri(dst, &bm_corners[part->first + t * 3], Models.Cols[0], e->NoShade, &xform);
	}
	Gfx_UnlockDynamicVb(bm_dynVb);
	Gfx_DrawVb_IndexedTris(total);
}


/*########################################################################################################################*
*--------------------------------------------------------Model struct-----------------------------------------------------*
*#########################################################################################################################*/
static void  BM_MakeParts(void) { }
static float BM_GetNameY(struct Entity* e) { return bm_loaded ? bm_nameY : 32.5f / 16.0f; }
static float BM_GetEyeY(struct Entity* e)  { return bm_loaded ? bm_eyeY  : BM_HUMAN_EYE_Y; }

static void BM_GetCollisionSize(struct Entity* e) {
	if (bm_loaded) { e->Size = bm_size; }
	else { Vec3_Set(e->Size, BM_HUMAN_SIZE_X, BM_HUMAN_SIZE_Y, BM_HUMAN_SIZE_Z); }
}

static void BM_GetPickingBounds(struct Entity* e) {
	if (bm_loaded) {
		e->ModelAABB.Min = bm_min;
		e->ModelAABB.Max = bm_max;
	} else {
		Vec3_Set(e->ModelAABB.Min, -8/16.0f, 0,        -4/16.0f);
		Vec3_Set(e->ModelAABB.Max,  8/16.0f, 32/16.0f,  4/16.0f);
	}
}

static struct Model bm_model = {
	BM_MODEL_NAME, NULL, &bm_modelTex,
	BM_MakeParts, BM_Draw,
	BM_GetNameY,  BM_GetEyeY,
	BM_GetCollisionSize, BM_GetPickingBounds
};


/*########################################################################################################################*
*------------------------------------------------------Player handling----------------------------------------------------*
*#########################################################################################################################*/
static cc_bool BM_IsApplied(void) {
	return Entities.CurPlayer && Entities.CurPlayer->Base.Model == &bm_model;
}

static void BM_SetPlayerModel(const char* modelName) {
	cc_string name = String_FromReadonly(modelName);
	if (!Entities.CurPlayer) return;
	Entity_SetModel(&Entities.CurPlayer->Base, &name);
}

static void BM_DeriveTexturePath(void) {
	int i, dot = -1;
	if (bm_texPath.length) return;

	for (i = bm_path.length - 1; i >= 0; i--) {
		char c = bm_path.buffer[i];
		if (c == '/' || c == '\\') break;
		if (c == '.') { dot = i; break; }
	}
	bm_texPath.length = 0;
	String_AppendString(&bm_texPath, &bm_path);
	if (dot >= 0) bm_texPath.length = dot;
	String_AppendConst(&bm_texPath, ".png");
}

static cc_bool BM_Reload(void) {
	cc_bool wasApplied = BM_IsApplied();
	int tris;
	BM_DeriveTexturePath();

	if (!BM_LoadObj(&bm_path)) return false;
	if (!BM_LoadTexture(&bm_texPath)) {
		BM_Msg("Model will be drawn untextured until a texture is loaded");
	}

	tris = bm_staticTris + bm_animTris;
	BM_Msg2("Loaded %s (%i triangles)", &bm_path, &tris);
	if (bm_armPart < 0) BM_Msg("No part named like an arm, so nothing is drawn in first person");

	/* Refresh collision/picking bounds if the player is already using the model */
	if (wasApplied) BM_SetPlayerModel(BM_MODEL_NAME);
	return true;
}

static void BM_OnNewMapLoaded(void) {
	if (bm_autoApply && bm_loaded) BM_SetPlayerModel(BM_MODEL_NAME);
}

/* Servers can change the player's model at any time, so put it back if asked to */
static void BM_Tick(struct ScheduledTask* task) {
	if (bm_force && bm_loaded && Entities.CurPlayer && !BM_IsApplied()) {
		BM_SetPlayerModel(BM_MODEL_NAME);
	}
}

static void BM_OnContextLost(void* obj) {
	BM_FreeGpu();
	if (Gfx.ManagedTextures) return;
	Gfx_DeleteTexture(&bm_texId);
	bm_modelTex.texID = 0;
}


/*########################################################################################################################*
*-----------------------------------------------------------Command-------------------------------------------------------*
*#########################################################################################################################*/
static void BM_SaveFloat(const char* key, float value) {
	char buffer[STRING_SIZE];
	cc_string str = String_FromArray(buffer);
	String_AppendFloat(&str, value, 3);
	Options_Set(key, &str);
	Options_SaveIfChanged();
}

static void BM_SaveBool(const char* key, cc_bool value) {
	Options_SetBool(key, value);
	Options_SaveIfChanged();
}

/* Accepts on/off and yes/no as well as true/false */
static cc_bool BM_ParseOnOff(const cc_string* str, cc_bool* value) {
	if (String_CaselessEqualsConst(str, "on")  || String_CaselessEqualsConst(str, "yes")) { *value = true;  return true; }
	if (String_CaselessEqualsConst(str, "off") || String_CaselessEqualsConst(str, "no"))  { *value = false; return true; }
	return Convert_ParseBool(str, value);
}

static void BM_PrintInfo(void) {
	int i;
	if (!bm_loaded) { BM_Msg("No model is loaded"); return; }
	BM_Msg2("Model: %s, texture: %s", &bm_path, &bm_texPath);
	BM_Msg2("Static triangles: %i, animated triangles: %i", &bm_staticTris, &bm_animTris);

	for (i = 0; i < bm_numParts; i++) {
		cc_string name = BM_PartName(&bm_parts[i]);
		int tris = bm_parts[i].count / 3;
		Chat_Add4("  &7%s&f: %c, %i triangles%c", &name, bm_kindNames[bm_parts[i].kind], &tris,
			i == bm_armPart ? " (first person arm)" : "");
	}
}

static void BM_Command(const cc_string* args, int argsCount) {
	cc_string line, sub, rest;
	float value;
	cc_bool flag;

	line = argsCount ? args[0] : String_Empty;
	if (!BM_NextToken(&line, &sub)) {
		BM_Msg("&a/client blendermodel &fload [file] | apply | reset | info");
		BM_Msg("&a/client blendermodel &fscale [n] | armscale [n] | auto [on/off] | force [on/off]");
		return;
	}
	rest = line;
	String_UNSAFE_TrimStart(&rest);

	if (String_CaselessEqualsConst(&sub, "load") || String_CaselessEqualsConst(&sub, "reload")) {
		if (rest.length) {
			String_Copy(&bm_path, &rest);
			bm_texPath.length = 0;
			Options_Set(OPT_BM_FILE, &bm_path);
			Options_SaveIfChanged();
		}
		BM_Reload();
	} else if (String_CaselessEqualsConst(&sub, "apply")) {
		if (!bm_loaded) { BM_Msg("No model is loaded"); return; }
		BM_SetPlayerModel(BM_MODEL_NAME);
		BM_Msg("Now using the blender model");
	} else if (String_CaselessEqualsConst(&sub, "reset")) {
		bm_force = false;
		BM_SetPlayerModel("humanoid");
		BM_Msg("Back to the humanoid model (force is now off)");
	} else if (String_CaselessEqualsConst(&sub, "info")) {
		BM_PrintInfo();
	} else if (String_CaselessEqualsConst(&sub, "scale")) {
		if (!rest.length) { BM_Msg1("Scale is %f3", &bm_scale); return; }
		if (!Convert_ParseFloat(&rest, &value) || value <= 0.0f) { BM_Msg("Scale must be a positive number"); return; }
		bm_scale = value;
		BM_SaveFloat(OPT_BM_SCALE, value);
		BM_Reload();
	} else if (String_CaselessEqualsConst(&sub, "armscale")) {
		if (!rest.length) { BM_Msg1("Arm scale is %f3", &bm_armScale); return; }
		if (!Convert_ParseFloat(&rest, &value) || value <= 0.0f) { BM_Msg("Arm scale must be a positive number"); return; }
		bm_armScale = value;
		BM_SaveFloat(OPT_BM_ARMSCALE, value);
	} else if (String_CaselessEqualsConst(&sub, "auto")) {
		if (!BM_ParseOnOff(&rest, &flag)) { BM_Msg1("Auto apply on map load is %t", &bm_autoApply); return; }
		bm_autoApply = flag;
		BM_SaveBool(OPT_BM_AUTO, flag);
		BM_Msg1("Auto apply is now %t", &flag);
	} else if (String_CaselessEqualsConst(&sub, "force")) {
		if (!BM_ParseOnOff(&rest, &flag)) { BM_Msg1("Force (re-apply when the server changes it) is %t", &bm_force); return; }
		bm_force = flag;
		BM_SaveBool(OPT_BM_FORCE, flag);
		BM_Msg1("Force is now %t", &flag);
	} else {
		BM_Msg1("Unknown sub-command %s", &sub);
	}
}

static struct ChatCommand bm_cmd = {
	"BlenderModel", BM_Command, COMMAND_FLAG_UNSPLIT_ARGS,
	{
		"&a/client blendermodel load [file]",
		"&eLoads an OBJ model (and file.png) exported from Blender.",
		"&a/client blendermodel apply / reset / info / scale / armscale",
		"&a/client blendermodel auto [on/off] / force [on/off]",
		"&eauto applies the model on map load, force re-applies it if the server changes it.",
	}
};


/*########################################################################################################################*
*-----------------------------------------------------------Plugin--------------------------------------------------------*
*#########################################################################################################################*/
static void BM_LoadOptions(void) {
	Options_Get(OPT_BM_FILE,    &bm_path,    BM_DEFAULT_FILE);
	Options_Get(OPT_BM_TEXTURE, &bm_texPath, "");
	bm_scale     = Options_GetFloat(OPT_BM_SCALE,    0.001f, 1000.0f, 1.0f);
	bm_armScale  = Options_GetFloat(OPT_BM_ARMSCALE, 0.001f, 1000.0f, 1.0f);
	bm_autoApply = Options_GetBool(OPT_BM_AUTO,  true);
	bm_force     = Options_GetBool(OPT_BM_FORCE, false);
}

static void BM_Init(void) {
	Model_Init(&bm_model);
	bm_model.usesSkin    = false;
	bm_model.DrawArm     = BM_DrawArm;
	bm_model.maxVertices = BM_CHUNK_VERTS;
	Model_Register(&bm_model);

	Commands_Register(&bm_cmd);
	Event_Register_(&GfxEvents.ContextLost, NULL, BM_OnContextLost);
	ScheduledTask_Add(0.5, BM_Tick);

	BM_LoadOptions();
	BM_Reload();
}

static void BM_Free(void) {
	BM_ResetMesh();
	BM_FreeTexture();
}

#ifndef BM_TEST
PLUGIN_EXPORT int Plugin_ApiVersion = GAME_API_VER;
PLUGIN_EXPORT struct IGameComponent Plugin_Component = {
	BM_Init,  /* Init  */
	BM_Free,  /* Free  */
	NULL,     /* Reset */
	NULL,     /* OnNewMap */
	BM_OnNewMapLoaded
};
#endif
