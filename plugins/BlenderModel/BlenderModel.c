/*
 * BlenderModel - ClassiCube plugin
 *
 * Loads a model made in Blender and registers it as the "blender" entity
 * model. The model can then be used as the local player's model, and is
 * rendered in both third person (the whole mesh) and first person (the part
 * or bones named like an arm, drawn as the held arm).
 *
 * Two file formats are supported:
 *   - Wavefront OBJ (+ PNG): rigid parts. Parts named like head / arm / leg
 *     are animated the way ClassiCube animates the humanoid model.
 *   - glTF 2.0 (.glb or .gltf): skinned meshes with animation clips. Bones
 *     and weights are evaluated on the CPU every frame. Clips are picked by
 *     name (idle / walk / jump / fly) from the entity's state.
 *
 * Everything is client side: other players still see whatever model the
 * server tells them about.
 *
 * Mesh conventions:
 *   - 1 unit = 1 block (blendermodel-scale multiplies this), Y is up
 *   - the model faces -Z. OBJ files are used as they are; glTF files
 *     (whose front is +Z by spec) are turned around. "flip" toggles this.
 *   - faces are wound counter-clockwise
 *
 * Optional OBJ metadata comment lines (written by blender_export_classicube.py):
 *   # pivot <part name> <x> <y> <z>   rotation origin of an animated part
 *   # eye <y>                          eye height above the feet (blocks)
 *   # size <w> <h> <l>                 collision box size (blocks)
 * glTF files carry the same information as "extras": cc_eye_height, cc_size.
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
#include "bm_gltf.h"

#define BM_MODEL_NAME  "blender"
#define BM_PREFIX      "&eBlenderModel: &f"
#define BM_MAX_PARTS   128
#define BM_MAX_POLY    64
#define BM_LINE_SIZE   1024
#define BM_MAX_ANIM_STATES 32

/* The game draws quads through a shared 16-bit index buffer, so a single */
/* vertex buffer holds at most 65536 vertices. Triangles are uploaded as   */
/* degenerate quads (4 vertices each), which gives 16383 triangles per VB. */
#define BM_CHUNK_TRIS  16383
#define BM_CHUNK_VERTS (BM_CHUNK_TRIS * 4)
#define BM_MAX_CHUNKS  64

#define OPT_BM_FILE      "blendermodel-file"
#define OPT_BM_TEXTURE   "blendermodel-texture"
#define OPT_BM_SCALE     "blendermodel-scale"
#define OPT_BM_ARMSCALE  "blendermodel-arm-scale"
#define OPT_BM_AUTO      "blendermodel-auto-apply"
#define OPT_BM_FORCE     "blendermodel-force"
#define OPT_BM_FLIP      "blendermodel-flip"
#define OPT_BM_HEADLOOK  "blendermodel-head-look"
#define OPT_BM_ANIMSPEED "blendermodel-anim-speed"

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

/* Which animation clip an entity is playing, and since when */
struct BM_AnimState { struct Entity* entity; int clip; double clipStart, lastUsed; };


/*########################################################################################################################*
*-----------------------------------------------------------State---------------------------------------------------------*
*#########################################################################################################################*/
/* Loaded OBJ mesh */
static struct BM_Corner* bm_corners;
static int bm_numCorners, bm_capCorners;
static struct BM_Part bm_parts[BM_MAX_PARTS];
static int bm_numParts;
static int bm_staticTris, bm_animTris, bm_armPart = -1;

/* Loaded glTF model */
static struct GltfModel bm_gltf;
static struct GltfPose  bm_pose;
static cc_bool bm_isGltf;
static int bm_headNode = -1;
static int bm_clipIdle = -1, bm_clipWalk = -1, bm_clipJump = -1, bm_clipFly = -1, bm_forcedClip = -1;
static struct BM_Corner* bm_gltfArm; /* rest pose arm, already posed */
static int bm_gltfArmCount;
static Vec3 bm_gltfArmPivot;
static struct BM_AnimState bm_animStates[BM_MAX_ANIM_STATES];

/* Shared by both formats */
static Vec3 bm_min, bm_max, bm_size;
static float bm_eyeY, bm_nameY;
static cc_bool bm_hasEye, bm_hasSize, bm_loaded;

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
static GfxResourceID bm_skinVbs[BM_MAX_CHUNKS]; /* dynamic, one per 16383 triangles of a glTF mesh */
static int bm_numSkinVbs;

/* Settings */
static float bm_scale = 1.0f, bm_armScale = 1.0f, bm_animSpeed = 1.0f;
static cc_bool bm_autoApply = true, bm_force, bm_flip;
static int bm_headLook = 1; /* 1 on, 0 off, -1 inverted */
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

/* Applies the 180 degree turn used to make a +Z facing model face -Z */
static Vec3 BM_Flipped(Vec3 v) { v.x = -v.x; v.z = -v.z; return v; }

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

static cc_bool BM_PathHasExt(const cc_string* path, const char* ext) {
	cc_string e = String_FromReadonly(ext);
	return String_CaselessEnds(path, &e);
}

static void BM_ExtendBounds(Vec3* min, Vec3* max, Vec3 p) {
	if (p.x < min->x) min->x = p.x;
	if (p.y < min->y) min->y = p.y;
	if (p.z < min->z) min->z = p.z;
	if (p.x > max->x) max->x = p.x;
	if (p.y > max->y) max->y = p.y;
	if (p.z > max->z) max->z = p.z;
}

/* Reported by the glTF loader */
void Gltf_LogMsg(const char* msg) { BM_Msg1("%c", msg); }


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
	int i;
	BM_FreeChunks();
	Gfx_DeleteDynamicVb(&bm_dynVb);
	bm_dynCap = 0;
	for (i = 0; i < bm_numSkinVbs; i++) {
		Gfx_DeleteDynamicVb(&bm_skinVbs[i]);
	}
	bm_numSkinVbs = 0;
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

	Gltf_Free(&bm_gltf);
	Gltf_Pose_Free(&bm_pose);
	Mem_Free(bm_gltfArm);
	bm_gltfArm = NULL; bm_gltfArmCount = 0;
	bm_isGltf   = false;
	bm_headNode = -1;
	bm_clipIdle = -1; bm_clipWalk = -1; bm_clipJump = -1; bm_clipFly = -1; bm_forcedClip = -1;
	Mem_Set(bm_animStates, 0, sizeof(bm_animStates));
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

/* Positions and pivots in the file are scaled (and turned around when flip is on) */
static Vec3 BM_ObjPoint(float x, float y, float z) {
	Vec3 v = BM_Vec3(x * bm_scale, y * bm_scale, z * bm_scale);
	return bm_flip ? BM_Flipped(v) : v;
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
		pivot->pos = BM_ObjPoint(f[0], f[1], f[2]);
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
		bm_pos[bm_numPos++] = BM_ObjPoint(f[0], f[1], f[2]);
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
		Vec3 n;
		if (!BM_ParseFloats(line, f, 3)) return;
		if (bm_numNrm >= bm_capNrm) {
			if (!(mem = BM_Grow(bm_nrm, &bm_capNrm, bm_numNrm + 1, sizeof(Vec3), "normals"))) return;
			bm_nrm = (Vec3*)mem;
		}
		n = BM_Vec3(f[0], f[1], f[2]);
		bm_nrm[bm_numNrm++] = bm_flip ? BM_Flipped(n) : n;
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
		BM_ExtendBounds(&part->min, &part->max, bm_corners[i].pos);
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

static void BM_ApplyDefaults(void) {
	if (!bm_hasEye)  bm_eyeY = bm_min.y + (bm_max.y - bm_min.y) * (BM_HUMAN_EYE_Y / 2.0f);
	if (!bm_hasSize) bm_size = BM_Vec3(BM_HUMAN_SIZE_X, BM_HUMAN_SIZE_Y, BM_HUMAN_SIZE_Z);
	bm_nameY = bm_max.y + 0.1f;
}

static void BM_FinishObjLoad(void) {
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
		BM_ExtendBounds(&bm_min, &bm_max, part->min);
		BM_ExtendBounds(&bm_min, &bm_max, part->max);
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
	BM_ApplyDefaults();
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

	BM_FinishObjLoad();
	if (bm_badFaces) BM_Msg1("Skipped %i faces with invalid indices", &bm_badFaces);
	BM_FreeScratch();

	if (!bm_loaded) { BM_Msg1("%s doesn't contain any faces", path); return false; }
	return true;
}


/*########################################################################################################################*
*----------------------------------------------------------Texture--------------------------------------------------------*
*#########################################################################################################################*/
/* Takes ownership of bmp, padding it to power of two dimensions if needed */
static cc_bool BM_AdoptTexture(struct Bitmap bmp) {
	int w = bmp.width, h = bmp.height, w2, h2, y;
	BM_FreeTexture();

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

static cc_bool BM_LoadTexture(const cc_string* path) {
	struct Stream stream;
	struct Bitmap bmp;
	cc_result res;

	res = Stream_OpenFile(&stream, path);
	if (res) { BM_Msg2("Couldn't open texture %s (error %h)", path, &res); return false; }
	res = Png_Decode(&bmp, &stream);
	stream.Close(&stream);
	if (res) {
		BM_Msg2("Couldn't decode texture %s (error %h)", path, &res);
		Mem_Free(bmp.scan0);
		return false;
	}
	return BM_AdoptTexture(bmp);
}

static cc_bool BM_EnsureTexture(void) {
	if (bm_texId) return true;
	if (!bm_bmp.scan0 || Gfx.LostContext) return false;

	bm_texId = Gfx_CreateTexture(&bm_bmp, TEXTURE_FLAG_MANAGED, false);
	bm_modelTex.texID = bm_texId;
	return bm_texId != 0;
}


/*########################################################################################################################*
*--------------------------------------------------------glTF loading-----------------------------------------------------*
*#########################################################################################################################*/
/* Whether node or one of its ancestors is the given node */
static cc_bool BM_NodeIsUnder(int node, int ancestor) {
	int depth = 0;
	while (node >= 0 && depth++ < 256) {
		if (node == ancestor) return true;
		node = bm_gltf.nodes[node].parent;
	}
	return false;
}

/* Palette entry with the largest weight of a corner, or -1 */
static int BM_HeaviestNode(const struct GltfCorner* c) {
	int j, best = -1;
	float bestW = 0.0f;
	for (j = 0; j < 4; j++) {
		if (c->weights[j] > bestW && c->joints[j] < bm_gltf.numPalette) { bestW = c->weights[j]; best = c->joints[j]; }
	}
	return best < 0 ? -1 : bm_gltf.paletteNode[best];
}

/* Finds the topmost node of the right (or failing that left) arm chain, or -1 */
static int BM_FindArmRoot(void) {
	int i, kind, wanted, best = -1;
	for (wanted = BM_PART_RIGHT_ARM; wanted >= BM_PART_LEFT_ARM && best < 0; wanted--) {
		for (i = 0; i < bm_gltf.numNodes; i++) {
			cc_string name = Gltf_NodeName(&bm_gltf, i);
			int parent = bm_gltf.nodes[i].parent;
			kind = BM_ClassifyName(&name);
			if (kind != wanted) continue;
			/* Skip bones whose parent is already part of the same arm */
			if (parent >= 0) {
				cc_string parentName = Gltf_NodeName(&bm_gltf, parent);
				if (BM_ClassifyName(&parentName) == wanted) continue;
			}
			best = i;
			break;
		}
	}
	return best;
}

static void BM_BuildGltfArm(void) {
	int armRoot = BM_FindArmRoot(), i, count = 0;
	Vec3 pos, nrm;
	bm_armPart = -1;
	if (armRoot < 0) return;

	for (i = 0; i < bm_gltf.numCorners; i++) {
		if (BM_NodeIsUnder(BM_HeaviestNode(&bm_gltf.corners[i]), armRoot)) count++;
	}
	/* Triangles are whole, so counts are multiples of 3 as long as a triangle's corners agree; */
	/* be safe and round down */
	count -= count % 3;
	if (!count) return;
	if (count / 3 > BM_CHUNK_TRIS) count = BM_CHUNK_TRIS * 3;

	bm_gltfArm = (struct BM_Corner*)Mem_TryAlloc(count, sizeof(struct BM_Corner));
	if (!bm_gltfArm) return;

	for (i = 0; i + 2 < bm_gltf.numCorners && bm_gltfArmCount < count; i += 3) {
		int k;
		if (!BM_NodeIsUnder(BM_HeaviestNode(&bm_gltf.corners[i]), armRoot)) continue;
		for (k = 0; k < 3; k++) {
			struct BM_Corner* dst = &bm_gltfArm[bm_gltfArmCount++];
			Gltf_SkinCorner(&bm_gltf, &bm_pose, &bm_gltf.corners[i + k], &pos, &nrm);
			dst->pos = pos; dst->nrm = nrm;
			dst->u = bm_gltf.corners[i + k].u;
			dst->v = bm_gltf.corners[i + k].v;
		}
	}
	bm_gltfArmPivot = Gltf_NodePosition(&bm_gltf, &bm_pose, armRoot);
	bm_armPart = armRoot; /* only used as "has an arm" for glTF models */
}

static void BM_PickClips(void) {
	static const char* const idleWords[] = { "idle", "stand", "rest", "breath" };
	static const char* const walkWords[] = { "walk", "run", "move", "sprint" };
	static const char* const jumpWords[] = { "jump", "fall", "air" };
	static const char* const flyWords[]  = { "fly", "float", "hover", "swim" };

	bm_clipIdle = Gltf_FindClip(&bm_gltf, idleWords, 4);
	bm_clipWalk = Gltf_FindClip(&bm_gltf, walkWords, 4);
	bm_clipJump = Gltf_FindClip(&bm_gltf, jumpWords, 3);
	bm_clipFly  = Gltf_FindClip(&bm_gltf, flyWords,  4);

	/* A model with clips but no recognisable names just plays the first one */
	if (bm_gltf.numClips && bm_clipIdle < 0 && bm_clipWalk < 0) bm_clipIdle = 0;
}

static cc_bool BM_LoadGltf(const cc_string* path) {
	int i;
	Vec3 pos, nrm;
	BM_ResetMesh();

	/* glTF's front is +Z, so turn the model around unless the user asked not to */
	if (!Gltf_Load(&bm_gltf, path, bm_scale, !bm_flip)) {
		BM_Msg1("Couldn't load %s", path);
		Gltf_Free(&bm_gltf);
		return false;
	}
	if (!Gltf_Pose_Alloc(&bm_gltf, &bm_pose)) { BM_Msg("Out of memory"); Gltf_Free(&bm_gltf); return false; }

	/* Rest pose gives the bounds, the first person arm and the head node */
	Gltf_EvalPose(&bm_gltf, &bm_pose, -1, 0.0f, -1, 0.0f, 0.0f);
	bm_min = Vec3_BigPos();
	Vec3_Negate(&bm_max, &bm_min);
	for (i = 0; i < bm_gltf.numCorners; i++) {
		Gltf_SkinCorner(&bm_gltf, &bm_pose, &bm_gltf.corners[i], &pos, &nrm);
		BM_ExtendBounds(&bm_min, &bm_max, pos);
	}

	for (i = 0; i < bm_gltf.numNodes; i++) {
		cc_string name = Gltf_NodeName(&bm_gltf, i);
		if (BM_ClassifyName(&name) == BM_PART_HEAD) { bm_headNode = i; break; }
	}

	BM_BuildGltfArm();
	BM_PickClips();

	bm_hasEye = bm_gltf.hasEye; bm_eyeY = bm_gltf.eyeY * bm_scale;
	bm_hasSize = bm_gltf.hasSize;
	bm_size = BM_Vec3(bm_gltf.size.x * bm_scale, bm_gltf.size.y * bm_scale, bm_gltf.size.z * bm_scale);
	BM_ApplyDefaults();

	/* Embedded texture, if any; ownership moves to the texture code */
	if (bm_gltf.texture.scan0) {
		struct Bitmap bmp = bm_gltf.texture;
		bm_gltf.texture.scan0 = NULL;
		BM_AdoptTexture(bmp);
	}

	bm_isGltf = true;
	bm_loaded = true;
	return true;
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

static void BM_WriteVertex(struct VertexTextured* dst, Vec3 p, const Vec3* n, float u, float v, PackedCol col, cc_bool noShade) {
	dst->x = p.x; dst->y = p.y; dst->z = p.z;
	dst->Col = BM_Shade(col, n, noShade);
	dst->U   = u * bm_uScale;
	dst->V   = v * bm_vScale;
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
		BM_WriteVertex(dst, p, &c[i].nrm, c[i].u, c[i].v, col, noShade);
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

static int BM_ArmTris(void) {
	if (bm_isGltf) return bm_gltfArmCount / 3;
	return bm_armPart >= 0 ? bm_parts[bm_armPart].count / 3 : 0;
}

static cc_bool BM_EnsureDynVb(void) {
	int armTris = BM_ArmTris();
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

static void BM_DrawAnimatedParts(struct Entity* e, PackedCol col) {
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

static void BM_DrawObj(struct Entity* e, PackedCol col) {
	int i;
	if (bm_chunksFailed) return;

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
	BM_DrawAnimatedParts(e, col);
}


/*########################################################################################################################*
*------------------------------------------------------glTF animation-----------------------------------------------------*
*#########################################################################################################################*/
static struct BM_AnimState* BM_GetAnimState(struct Entity* e) {
	struct BM_AnimState* oldest = &bm_animStates[0];
	int i;
	for (i = 0; i < BM_MAX_ANIM_STATES; i++) {
		struct BM_AnimState* s = &bm_animStates[i];
		if (s->entity == e) { s->lastUsed = Game.Time; return s; }
		if (!s->entity || s->lastUsed < oldest->lastUsed) oldest = s;
	}

	oldest->entity    = e;
	oldest->clip      = -2; /* forces a clip start */
	oldest->clipStart = Game.Time;
	oldest->lastUsed  = Game.Time;
	return oldest;
}

static int BM_ChooseClip(struct Entity* e) {
	cc_bool moving = e->Anim.Swing > 0.05f, flying = false;
	if (bm_forcedClip >= 0) return bm_forcedClip;
	if (Entities.CurPlayer && e == &Entities.CurPlayer->Base) flying = Entities.CurPlayer->Hacks.Flying;

	if (flying && bm_clipFly >= 0) return bm_clipFly;
	if (!e->OnGround && !flying) {
		if (bm_clipJump >= 0) return bm_clipJump;
		if (bm_clipFly  >= 0) return bm_clipFly;
	}
	if (moving && bm_clipWalk >= 0) return bm_clipWalk;
	if (bm_clipIdle >= 0) return bm_clipIdle;
	return bm_clipWalk;
}

static float BM_ClipTime(struct Entity* e, int clip) {
	struct BM_AnimState* state = BM_GetAnimState(e);
	double elapsed, duration;
	if (state->clip != clip) { state->clip = clip; state->clipStart = Game.Time; }
	if (clip < 0) return 0.0f;

	duration = bm_gltf.clips[clip].duration;
	elapsed  = (Game.Time - state->clipStart) * bm_animSpeed;
	if (duration <= 0.0) return 0.0f;
	/* Loop */
	elapsed -= duration * (double)(long)(elapsed / duration);
	return (float)elapsed;
}

static cc_bool BM_EnsureSkinVbs(void) {
	int needed = (bm_gltf.numCorners / 3 + BM_CHUNK_TRIS - 1) / BM_CHUNK_TRIS, i;
	if (needed > BM_MAX_CHUNKS) needed = BM_MAX_CHUNKS;
	if (bm_numSkinVbs >= needed) return true;

	for (i = bm_numSkinVbs; i < needed; i++) {
		int tris = bm_gltf.numCorners / 3 - i * BM_CHUNK_TRIS;
		if (tris > BM_CHUNK_TRIS) tris = BM_CHUNK_TRIS;
		bm_skinVbs[i] = Gfx_CreateDynamicVb(VERTEX_FORMAT_TEXTURED, tris * 4);
		if (!bm_skinVbs[i]) return false;
		bm_numSkinVbs = i + 1;
	}
	return true;
}

static void BM_DrawGltf(struct Entity* e, PackedCol col) {
	int clip = BM_ChooseClip(e), chunk, i, tris, t, totalTris;
	float time = BM_ClipTime(e, clip);
	float pitch = 0.0f, yaw = 0.0f;
	Vec3 pos, nrm;
	if (!BM_EnsureSkinVbs()) return;

	if (bm_headLook && bm_headNode >= 0) {
		/* Same angles the humanoid head uses: pitch, plus how far the head is turned from the body */
		pitch = -e->Pitch * MATH_DEG2RAD * bm_headLook;
		yaw   = (e->Yaw - e->RotY) * MATH_DEG2RAD * bm_headLook;
	}
	Gltf_EvalPose(&bm_gltf, &bm_pose, clip, time, bm_headNode, pitch, yaw);

	totalTris = bm_gltf.numCorners / 3;
	for (chunk = 0; chunk < bm_numSkinVbs; chunk++) {
		struct VertexTextured* dst;
		tris = totalTris - chunk * BM_CHUNK_TRIS;
		if (tris > BM_CHUNK_TRIS) tris = BM_CHUNK_TRIS;
		if (tris <= 0) break;

		dst = (struct VertexTextured*)Gfx_LockDynamicVb(bm_skinVbs[chunk], VERTEX_FORMAT_TEXTURED, tris * 4);
		for (t = 0; t < tris; t++, dst += 4) {
			const struct GltfCorner* c = &bm_gltf.corners[(chunk * BM_CHUNK_TRIS + t) * 3];
			for (i = 0; i < 3; i++) {
				Gltf_SkinCorner(&bm_gltf, &bm_pose, &c[i], &pos, &nrm);
				BM_WriteVertex(&dst[i], pos, &nrm, c[i].u, c[i].v, col, e->NoShade);
			}
			dst[3] = dst[2];
		}
		Gfx_UnlockDynamicVb(bm_skinVbs[chunk]);
		Gfx_DrawVb_IndexedTris(tris * 4);
	}
}


/*########################################################################################################################*
*---------------------------------------------------------Model hooks-----------------------------------------------------*
*#########################################################################################################################*/
static void BM_Draw(struct Entity* e) {
	PackedCol col = Models.Cols[0];
	if (!bm_loaded || Gfx.LostContext) return;
	if (!BM_EnsureTexture()) return;
	Gfx_BindTexture(bm_texId);

	if (bm_isGltf) { BM_DrawGltf(e, col); }
	else { BM_DrawObj(e, col); }
}

/* Called by Model_RenderArm, which has already set up the view matrix so that */
/* the humanoid's right arm appears in the bottom right of the screen */
static void BM_DrawArm(struct Entity* e) {
	const struct BM_Corner* corners;
	struct VertexTextured* dst;
	struct BM_Xform xform;
	struct BM_Rot rot;
	Vec3 armPivot, partPivot;
	int t, tris, total;

	if (!bm_loaded || Gfx.LostContext) return;
	if (bm_isGltf) {
		corners = bm_gltfArm; tris = bm_gltfArmCount / 3; partPivot = bm_gltfArmPivot;
	} else if (bm_armPart >= 0) {
		corners = &bm_corners[bm_parts[bm_armPart].first]; tris = bm_parts[bm_armPart].count / 3; partPivot = bm_parts[bm_armPart].pivot;
	} else {
		return;
	}
	if (!tris || !BM_EnsureTexture() || !BM_EnsureDynVb()) return;
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
	xform.sub   = partPivot;
	xform.scale = bm_armScale;
	xform.add   = BM_Vec3(BM_HUMAN_ARM_PIVOT_X, BM_HUMAN_ARM_PIVOT_Y, 0.0f);
	xform.rot   = &rot;

	dst = (struct VertexTextured*)Gfx_LockDynamicVb(bm_dynVb, VERTEX_FORMAT_TEXTURED, total);
	for (t = 0; t < tris; t++, dst += 4) {
		BM_WriteTri(dst, &corners[t * 3], Models.Cols[0], e->NoShade, &xform);
	}
	Gfx_UnlockDynamicVb(bm_dynVb);
	Gfx_DrawVb_IndexedTris(total);
}

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

static cc_bool BM_LoadModelFile(const cc_string* path) {
	if (BM_PathHasExt(path, ".glb") || BM_PathHasExt(path, ".gltf")) return BM_LoadGltf(path);
	return BM_LoadObj(path);
}

static cc_bool BM_Reload(void) {
	cc_bool wasApplied = BM_IsApplied();
	int tris;
	BM_DeriveTexturePath();

	if (!BM_LoadModelFile(&bm_path)) return false;
	/* glTF files usually bring their own texture */
	if (!bm_bmp.scan0 && !BM_LoadTexture(&bm_texPath)) {
		BM_Msg("Model will be drawn untextured until a texture is loaded");
	}

	tris = bm_isGltf ? bm_gltf.numCorners / 3 : bm_staticTris + bm_animTris;
	BM_Msg2("Loaded %s (%i triangles)", &bm_path, &tris);
	if (bm_isGltf) BM_Msg2("%i bones, %i animation clips", &bm_gltf.numNodes, &bm_gltf.numClips);
	if (bm_armPart < 0) BM_Msg("No part or bone named like an arm, so nothing is drawn in first person");

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

static void BM_PrintClip(const char* role, int clip) {
	cc_string name = Gltf_ClipName(&bm_gltf, clip);
	if (clip < 0) { Chat_Add1("  &7%c&f: none", role); }
	else { Chat_Add2("  &7%c&f: %s", role, &name); }
}

static void BM_PrintInfo(void) {
	int i;
	if (!bm_loaded) { BM_Msg("No model is loaded"); return; }
	BM_Msg2("Model: %s, texture: %s", &bm_path, &bm_texPath);

	if (bm_isGltf) {
		int tris = bm_gltf.numCorners / 3;
		cc_string head = Gltf_NodeName(&bm_gltf, bm_headNode);
		cc_string arm  = Gltf_NodeName(&bm_gltf, bm_armPart);
		BM_Msg2("glTF: %i triangles, %i bones", &tris, &bm_gltf.numNodes);
		BM_Msg2("Head bone: %s, arm bone: %s", &head, &arm);
		BM_PrintClip("idle", bm_clipIdle);
		BM_PrintClip("walk", bm_clipWalk);
		BM_PrintClip("jump", bm_clipJump);
		BM_PrintClip("fly",  bm_clipFly);
		for (i = 0; i < bm_gltf.numClips; i++) {
			cc_string name = Gltf_ClipName(&bm_gltf, i);
			float dur = bm_gltf.clips[i].duration;
			Chat_Add3("  &7clip %i&f: %s (%f2 s)", &i, &name, &dur);
		}
		return;
	}

	BM_Msg2("Static triangles: %i, animated triangles: %i", &bm_staticTris, &bm_animTris);
	for (i = 0; i < bm_numParts; i++) {
		cc_string name = BM_PartName(&bm_parts[i]);
		int tris = bm_parts[i].count / 3;
		Chat_Add4("  &7%s&f: %c, %i triangles%c", &name, bm_kindNames[bm_parts[i].kind], &tris,
			i == bm_armPart ? " (first person arm)" : "");
	}
}

static void BM_CommandAnim(const cc_string* rest) {
	int clip;
	if (!bm_isGltf) { BM_Msg("Only glTF models have animation clips"); return; }
	if (!rest->length || String_CaselessEqualsConst(rest, "auto")) {
		bm_forcedClip = -1;
		BM_Msg("Clips are chosen automatically from movement");
		return;
	}
	if (String_CaselessEqualsConst(rest, "list")) { BM_PrintInfo(); return; }

	clip = Gltf_FindClipByName(&bm_gltf, rest);
	if (clip < 0 && Convert_ParseInt(rest, &clip) && (clip < 0 || clip >= bm_gltf.numClips)) clip = -1;
	if (clip < 0) { BM_Msg1("No clip named %s", rest); return; }
	bm_forcedClip = clip;
	BM_Msg1("Playing clip %s", rest);
}

static void BM_Command(const cc_string* args, int argsCount) {
	cc_string line, sub, rest;
	float value;
	cc_bool flag;

	line = argsCount ? args[0] : String_Empty;
	if (!BM_NextToken(&line, &sub)) {
		BM_Msg("&a/client blendermodel &fload [file] | apply | reset | info | anim [name/auto]");
		BM_Msg("&a/client blendermodel &fscale [n] | armscale [n] | animspeed [n] | flip [on/off]");
		BM_Msg("&a/client blendermodel &fauto [on/off] | force [on/off] | headlook [on/off/invert]");
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
	} else if (String_CaselessEqualsConst(&sub, "anim")) {
		BM_CommandAnim(&rest);
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
	} else if (String_CaselessEqualsConst(&sub, "animspeed")) {
		if (!rest.length) { BM_Msg1("Animation speed is %f3", &bm_animSpeed); return; }
		if (!Convert_ParseFloat(&rest, &value) || value <= 0.0f) { BM_Msg("Speed must be a positive number"); return; }
		bm_animSpeed = value;
		BM_SaveFloat(OPT_BM_ANIMSPEED, value);
	} else if (String_CaselessEqualsConst(&sub, "flip")) {
		if (!BM_ParseOnOff(&rest, &flag)) { BM_Msg1("Flip (turn the model around) is %t", &bm_flip); return; }
		bm_flip = flag;
		BM_SaveBool(OPT_BM_FLIP, flag);
		BM_Reload();
	} else if (String_CaselessEqualsConst(&sub, "headlook")) {
		if (String_CaselessEqualsConst(&rest, "invert")) { bm_headLook = -1; }
		else if (BM_ParseOnOff(&rest, &flag)) { bm_headLook = flag ? 1 : 0; }
		else { BM_Msg1("Head look is %i (1 on, 0 off, -1 inverted)", &bm_headLook); return; }
		Options_SetInt(OPT_BM_HEADLOOK, bm_headLook);
		Options_SaveIfChanged();
		BM_Msg1("Head look is now %i", &bm_headLook);
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
		"&eLoads an OBJ (and file.png) or glTF (.glb/.gltf) model exported from Blender.",
		"&a/client blendermodel apply / reset / info / anim / scale / armscale / animspeed",
		"&a/client blendermodel flip / auto [on/off] / force [on/off] / headlook",
		"&eauto applies the model on map load, force re-applies it if the server changes it.",
	}
};


/*########################################################################################################################*
*-----------------------------------------------------------Plugin--------------------------------------------------------*
*#########################################################################################################################*/
static void BM_LoadOptions(void) {
	Options_Get(OPT_BM_FILE,    &bm_path,    BM_DEFAULT_FILE);
	Options_Get(OPT_BM_TEXTURE, &bm_texPath, "");
	bm_scale     = Options_GetFloat(OPT_BM_SCALE,     0.001f, 1000.0f, 1.0f);
	bm_armScale  = Options_GetFloat(OPT_BM_ARMSCALE,  0.001f, 1000.0f, 1.0f);
	bm_animSpeed = Options_GetFloat(OPT_BM_ANIMSPEED, 0.001f, 1000.0f, 1.0f);
	bm_autoApply = Options_GetBool(OPT_BM_AUTO,  true);
	bm_force     = Options_GetBool(OPT_BM_FORCE, false);
	bm_flip      = Options_GetBool(OPT_BM_FLIP,  false);
	bm_headLook  = Options_GetInt(OPT_BM_HEADLOOK, -1, 1, 1);
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
