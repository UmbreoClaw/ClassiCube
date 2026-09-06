/* glTF 2.0 loader and pose evaluator for the BlenderModel plugin.
 * Licensed under BSD-3, same as ClassiCube.
 */
#include "src/PluginAPI.h"
#include "src/Errors.h"
#include "src/ExtMath.h"
#include "src/Platform.h"
#include "src/Stream.h"
#include "src/String_.h"
#include "bm_json.h"
#include "bm_gltf.h"

#define GLB_MAGIC      0x46546C67UL
#define GLB_CHUNK_JSON 0x4E4F534AUL
#define GLB_CHUNK_BIN  0x004E4942UL

#define COMP_I8  5120
#define COMP_U8  5121
#define COMP_I16 5122
#define COMP_U16 5123
#define COMP_U32 5125
#define COMP_F32 5126

struct GltfBuffer { cc_uint8* data; cc_uint32 len; cc_bool owned; };

static cc_uint32 Gltf_U32(const cc_uint8* p) {
	return (cc_uint32)p[0] | ((cc_uint32)p[1] << 8) | ((cc_uint32)p[2] << 16) | ((cc_uint32)p[3] << 24);
}

struct GltfLoader {
	struct BMJ_Doc doc;
	cc_uint8* file;  cc_uint32 fileLen;   /* whole .glb / .gltf file */
	char*     json;  int jsonLen;
	cc_uint8* bin;   cc_uint32 binLen;    /* GLB BIN chunk, may be NULL */
	struct GltfBuffer* buffers; int numBuffers;
	int* skinPaletteBase; int numSkins;
	char dirBuffer[FILENAME_SIZE];
	cc_string dir; /* directory of the file, including trailing separator */
	struct GltfModel* m;
	int corners_cap;
};

static void Gltf_Log1(const char* format, const void* a1) {
	char buffer[STRING_SIZE * 2];
	cc_string msg = String_FromArray(buffer);
	String_Format1(&msg, format, a1);
	buffer[msg.length] = '\0';
	Gltf_LogMsg(buffer);
}


/*########################################################################################################################*
*-----------------------------------------------------------Maths--------------------------------------------------------*
*#########################################################################################################################*/
void Gltf_M4_Identity(float* m) {
	int i;
	for (i = 0; i < 16; i++) m[i] = 0.0f;
	m[0] = 1.0f; m[5] = 1.0f; m[10] = 1.0f; m[15] = 1.0f;
}

/* out = a * b (column-major), out may alias neither a nor b */
void Gltf_M4_Mul(float* out, const float* a, const float* b) {
	int row, col, k;
	for (col = 0; col < 4; col++) {
		for (row = 0; row < 4; row++) {
			float sum = 0.0f;
			for (k = 0; k < 4; k++) sum += a[k * 4 + row] * b[col * 4 + k];
			out[col * 4 + row] = sum;
		}
	}
}

Vec3 Gltf_M4_TransformPoint(const float* m, Vec3 p) {
	Vec3 r;
	r.x = m[0] * p.x + m[4] * p.y + m[8]  * p.z + m[12];
	r.y = m[1] * p.x + m[5] * p.y + m[9]  * p.z + m[13];
	r.z = m[2] * p.x + m[6] * p.y + m[10] * p.z + m[14];
	return r;
}

Vec3 Gltf_M4_TransformDir(const float* m, Vec3 d) {
	Vec3 r;
	r.x = m[0] * d.x + m[4] * d.y + m[8]  * d.z;
	r.y = m[1] * d.x + m[5] * d.y + m[9]  * d.z;
	r.z = m[2] * d.x + m[6] * d.y + m[10] * d.z;
	return r;
}

static void M4_FromTRS(float* m, Vec3 t, const float* q, Vec3 s) {
	float x = q[0], y = q[1], z = q[2], w = q[3];
	float xx = x * x, yy = y * y, zz = z * z;
	float xy = x * y, xz = x * z, yz = y * z, wx = w * x, wy = w * y, wz = w * z;

	m[0] = (1 - 2 * (yy + zz)) * s.x; m[1] = (2 * (xy + wz)) * s.x; m[2]  = (2 * (xz - wy)) * s.x; m[3]  = 0;
	m[4] = (2 * (xy - wz)) * s.y; m[5] = (1 - 2 * (xx + zz)) * s.y; m[6]  = (2 * (yz + wx)) * s.y; m[7]  = 0;
	m[8] = (2 * (xz + wy)) * s.z; m[9] = (2 * (yz - wx)) * s.z; m[10] = (1 - 2 * (xx + yy)) * s.z; m[11] = 0;
	m[12] = t.x; m[13] = t.y; m[14] = t.z; m[15] = 1;
}

static void M4_RotateX(float* m, float angle) {
	float c = Math_CosF(angle), s = Math_SinF(angle);
	Gltf_M4_Identity(m);
	m[5] = c; m[6] = s; m[9] = -s; m[10] = c;
}

static void M4_RotateY(float* m, float angle) {
	float c = Math_CosF(angle), s = Math_SinF(angle);
	Gltf_M4_Identity(m);
	m[0] = c; m[2] = -s; m[8] = s; m[10] = c;
}

static void Vec3_NormaliseSafe(Vec3* v) {
	float len = Vec3_LengthSquared(v);
	if (len <= 0.0f) return;
	len = Math_SqrtF(len);
	v->x /= len; v->y /= len; v->z /= len;
}

void Gltf_SetRootXform(struct GltfModel* m, float scale, cc_bool flipZ) {
	Gltf_M4_Identity(m->rootXform);
	/* Rotating 180 degrees about Y turns glTF's +Z front into the game's -Z front */
	m->rootXform[0]  = flipZ ? -scale : scale;
	m->rootXform[5]  = scale;
	m->rootXform[10] = flipZ ? -scale : scale;
}


/*########################################################################################################################*
*----------------------------------------------------------Files---------------------------------------------------------*
*#########################################################################################################################*/
static cc_bool Gltf_ReadFile(const cc_string* path, cc_uint8** data, cc_uint32* len) {
	struct Stream stream;
	cc_result res;
	cc_uint32 size;
	cc_uint8* mem;

	res = Stream_OpenFile(&stream, path);
	if (res) { Gltf_Log1("Couldn't open %s", path); return false; }
	res = stream.Length(&stream, &size);
	if (res || size == 0) { stream.Close(&stream); Gltf_Log1("Couldn't read %s", path); return false; }

	mem = (cc_uint8*)Mem_TryAlloc(size + 1, 1);
	if (!mem) { stream.Close(&stream); Gltf_LogMsg("Out of memory reading file"); return false; }
	res = Stream_Read(&stream, mem, size);
	stream.Close(&stream);
	if (res) { Mem_Free(mem); Gltf_Log1("Couldn't read %s", path); return false; }

	mem[size] = '\0';
	*data = mem; *len = size;
	return true;
}

static int Base64_Value(char c) {
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+') return 62;
	if (c == '/') return 63;
	return -1;
}

/* Decodes base64 text into a newly allocated buffer */
static cc_bool Base64_Decode(const cc_string* text, cc_uint8** data, cc_uint32* len) {
	cc_uint8* out = (cc_uint8*)Mem_TryAlloc(text->length / 4 * 3 + 3, 1);
	cc_uint32 acc = 0, count = 0, written = 0;
	int i, v;
	if (!out) return false;

	for (i = 0; i < text->length; i++) {
		char c = text->buffer[i];
		if (c == '=' || c == '\r' || c == '\n') continue;
		v = Base64_Value(c);
		if (v < 0) { Mem_Free(out); return false; }

		acc = (acc << 6) | v;
		if (++count == 4) {
			out[written++] = (cc_uint8)(acc >> 16);
			out[written++] = (cc_uint8)(acc >> 8);
			out[written++] = (cc_uint8)acc;
			acc = 0; count = 0;
		}
	}
	if (count == 3) { out[written++] = (cc_uint8)(acc >> 10); out[written++] = (cc_uint8)(acc >> 2); }
	if (count == 2) { out[written++] = (cc_uint8)(acc >> 4); }

	*data = out; *len = written;
	return true;
}

/* Loads the bytes a URI refers to: either a data: URI or a file next to the model */
static cc_bool Gltf_LoadUri(struct GltfLoader* l, const cc_string* uri, cc_uint8** data, cc_uint32* len) {
	char pathBuffer[FILENAME_SIZE];
	cc_string path = String_FromArray(pathBuffer);
	cc_string prefix = String_FromConst("data:");

	if (String_CaselessStarts(uri, &prefix)) {
		int comma = String_IndexOfAt(uri, 0, ',');
		cc_string payload;
		if (comma < 0) return false;
		payload = String_UNSAFE_SubstringAt(uri, comma + 1);
		return Base64_Decode(&payload, data, len);
	}

	String_AppendString(&path, &l->dir);
	String_AppendString(&path, uri);
	return Gltf_ReadFile(&path, data, len);
}

static cc_bool Gltf_OpenContainer(struct GltfLoader* l, const cc_string* path) {
	cc_uint8* f;
	cc_uint32 len, offset, chunkLen, chunkType;
	if (!Gltf_ReadFile(path, &l->file, &l->fileLen)) return false;
	f = l->file; len = l->fileLen;

	if (len >= 12 && Gltf_U32(f) == GLB_MAGIC) {
		/* Binary container: 12 byte header then (length, type, data) chunks */
		offset = 12;
		while (offset + 8 <= len) {
			chunkLen  = Gltf_U32(f + offset);
			chunkType = Gltf_U32(f + offset + 4);
			offset += 8;
			if (chunkLen > len - offset) { Gltf_LogMsg("Corrupt .glb chunk"); return false; }

			if (chunkType == GLB_CHUNK_JSON) {
				l->json = (char*)(f + offset); l->jsonLen = (int)chunkLen;
			} else if (chunkType == GLB_CHUNK_BIN && !l->bin) {
				l->bin = f + offset; l->binLen = chunkLen;
			}
			offset += chunkLen;
		}
		if (!l->json) { Gltf_LogMsg("No JSON chunk in .glb"); return false; }
	} else {
		l->json = (char*)f; l->jsonLen = (int)len;
	}

	if (!BMJ_Parse(&l->doc, l->json, l->jsonLen)) { Gltf_LogMsg("Couldn't parse glTF JSON"); return false; }
	return true;
}

static cc_bool Gltf_LoadBuffers(struct GltfLoader* l) {
	const struct BMJ_Doc* d = &l->doc;
	int arr = BMJ_Get(d, 0, "buffers"), i;
	cc_string uri;

	l->numBuffers = BMJ_Count(d, arr);
	if (!l->numBuffers) return true;
	l->buffers = (struct GltfBuffer*)Mem_TryAllocCleared(l->numBuffers, sizeof(struct GltfBuffer));
	if (!l->buffers) return false;

	for (i = 0; i < l->numBuffers; i++) {
		int buf = BMJ_At(d, arr, i);
		uri = BMJ_GetStr(d, buf, "uri");

		if (uri.length) {
			if (!Gltf_LoadUri(l, &uri, &l->buffers[i].data, &l->buffers[i].len)) {
				Gltf_Log1("Couldn't load buffer %s", &uri); return false;
			}
			l->buffers[i].owned = true;
		} else if (l->bin) {
			l->buffers[i].data = l->bin; l->buffers[i].len = l->binLen;
		} else {
			Gltf_LogMsg("Buffer has no data"); return false;
		}
	}
	return true;
}


/*########################################################################################################################*
*---------------------------------------------------------Accessors------------------------------------------------------*
*#########################################################################################################################*/
struct GltfAccessor {
	const cc_uint8* data;
	int count, comps, compType, compSize, stride;
	cc_bool normalized;
};

static int Gltf_CompSize(int compType) {
	switch (compType) {
	case COMP_I8: case COMP_U8:   return 1;
	case COMP_I16: case COMP_U16: return 2;
	case COMP_U32: case COMP_F32: return 4;
	}
	return 0;
}

static int Gltf_TypeComps(const cc_string* type) {
	if (String_CaselessEqualsConst(type, "SCALAR")) return 1;
	if (String_CaselessEqualsConst(type, "VEC2"))   return 2;
	if (String_CaselessEqualsConst(type, "VEC3"))   return 3;
	if (String_CaselessEqualsConst(type, "VEC4"))   return 4;
	if (String_CaselessEqualsConst(type, "MAT4"))   return 16;
	return 0;
}

/* Resolves accessor index into a validated view of its data */
static cc_bool Gltf_GetAccessor(struct GltfLoader* l, int index, struct GltfAccessor* a) {
	const struct BMJ_Doc* d = &l->doc;
	int acc = BMJ_GetAt(d, 0, "accessors", index);
	int view, buffer, viewOffset, viewLen, viewStride, accOffset;
	cc_string type;
	cc_uint32 needed;

	if (acc < 0) return false;
	if (BMJ_Get(d, acc, "sparse") >= 0) { Gltf_LogMsg("Sparse accessors aren't supported"); return false; }

	type = BMJ_GetStr(d, acc, "type");
	a->comps      = Gltf_TypeComps(&type);
	a->compType   = BMJ_GetInt(d, acc, "componentType", 0);
	a->compSize   = Gltf_CompSize(a->compType);
	a->count      = BMJ_GetInt(d, acc, "count", 0);
	a->normalized = BMJ_GetNum(d, acc, "normalized", 0) != 0;
	accOffset     = BMJ_GetInt(d, acc, "byteOffset", 0);
	if (!a->comps || !a->compSize || a->count <= 0) return false;

	view = BMJ_GetAt(d, 0, "bufferViews", BMJ_GetInt(d, acc, "bufferView", -1));
	if (view < 0) return false;
	buffer     = BMJ_GetInt(d, view, "buffer", -1);
	viewOffset = BMJ_GetInt(d, view, "byteOffset", 0);
	viewLen    = BMJ_GetInt(d, view, "byteLength", 0);
	viewStride = BMJ_GetInt(d, view, "byteStride", 0);
	if (buffer < 0 || buffer >= l->numBuffers) return false;

	a->stride = viewStride ? viewStride : a->comps * a->compSize;
	needed = (cc_uint32)accOffset + (cc_uint32)(a->count - 1) * a->stride + a->comps * a->compSize;
	if (viewOffset < 0 || viewLen < 0 || (cc_uint32)viewOffset + viewLen > l->buffers[buffer].len) return false;
	if (needed > (cc_uint32)viewLen) return false;

	a->data = l->buffers[buffer].data + viewOffset + accOffset;
	return true;
}

static float Gltf_ReadComp(const struct GltfAccessor* a, const cc_uint8* p) {
	union { float f; cc_uint32 u; } conv;
	switch (a->compType) {
	case COMP_I8:  { float v = (cc_int8)p[0];  return a->normalized ? (v < -127 ? -1.0f : v / 127.0f) : v; }
	case COMP_U8:  { float v = p[0];           return a->normalized ? v / 255.0f : v; }
	case COMP_I16: { float v = (cc_int16)(p[0] | (p[1] << 8)); return a->normalized ? (v < -32767 ? -1.0f : v / 32767.0f) : v; }
	case COMP_U16: { float v = (cc_uint16)(p[0] | (p[1] << 8)); return a->normalized ? v / 65535.0f : v; }
	case COMP_U32: return (float)Gltf_U32(p);
	case COMP_F32: conv.u = Gltf_U32(p); return conv.f;
	}
	return 0.0f;
}

static cc_uint32 Gltf_ReadUInt(const struct GltfAccessor* a, const cc_uint8* p) {
	switch (a->compType) {
	case COMP_I8: case COMP_U8:   return p[0];
	case COMP_I16: case COMP_U16: return p[0] | (p[1] << 8);
	default: return Gltf_U32(p);
	}
}

/* Reads count * comps floats into dst (dst must have room for a->count * comps) */
static void Gltf_ReadFloats(const struct GltfAccessor* a, float* dst, int comps) {
	int i, c;
	for (i = 0; i < a->count; i++) {
		const cc_uint8* p = a->data + i * a->stride;
		for (c = 0; c < comps; c++) {
			dst[i * comps + c] = c < a->comps ? Gltf_ReadComp(a, p + c * a->compSize) : 0.0f;
		}
	}
}

static float* Gltf_AllocFloats(struct GltfLoader* l, int accessor, int comps, int* count) {
	struct GltfAccessor a;
	float* data;
	if (!Gltf_GetAccessor(l, accessor, &a)) return NULL;

	data = (float*)Mem_TryAlloc(a.count * comps, sizeof(float));
	if (!data) return NULL;
	Gltf_ReadFloats(&a, data, comps);
	*count = a.count;
	return data;
}


/*########################################################################################################################*
*-----------------------------------------------------------Nodes--------------------------------------------------------*
*#########################################################################################################################*/
static void Gltf_CopyName(char* dst, int* dstLen, const cc_string* src) {
	int len = src->length < GLTF_NAME_SIZE ? src->length : GLTF_NAME_SIZE;
	Mem_Copy(dst, src->buffer, len);
	*dstLen = len;
}

static void Gltf_ReadFloatArray(const struct BMJ_Doc* d, int arr, float* dst, int count) {
	int i;
	for (i = 0; i < count; i++) dst[i] = (float)BMJ_Num(d, BMJ_At(d, arr, i), dst[i]);
}

static cc_bool Gltf_LoadNodes(struct GltfLoader* l) {
	const struct BMJ_Doc* d = &l->doc;
	struct GltfModel* m = l->m;
	int arr = BMJ_Get(d, 0, "nodes"), i, c, ordered = 0, pass;
	cc_uint8* placed;

	m->numNodes = BMJ_Count(d, arr);
	if (!m->numNodes) { Gltf_LogMsg("glTF has no nodes"); return false; }
	m->nodes     = (struct GltfNode*)Mem_TryAllocCleared(m->numNodes, sizeof(struct GltfNode));
	m->nodeOrder = (int*)Mem_TryAlloc(m->numNodes, sizeof(int));
	placed       = (cc_uint8*)Mem_TryAllocCleared(m->numNodes, 1);
	if (!m->nodes || !m->nodeOrder || !placed) { Mem_Free(placed); return false; }

	for (i = 0; i < m->numNodes; i++) {
		struct GltfNode* node = &m->nodes[i];
		int json = BMJ_At(d, arr, i), mat;
		cc_string name = BMJ_GetStr(d, json, "name");
		float t[3] = { 0, 0, 0 }, r[4] = { 0, 0, 0, 1 }, s[3] = { 1, 1, 1 };

		Gltf_CopyName(node->name, &node->nameLen, &name);
		node->parent  = -1;
		node->palette = -1;
		Gltf_ReadFloatArray(d, BMJ_Get(d, json, "translation"), t, 3);
		Gltf_ReadFloatArray(d, BMJ_Get(d, json, "rotation"),    r, 4);
		Gltf_ReadFloatArray(d, BMJ_Get(d, json, "scale"),       s, 3);
		Vec3_Set(node->t, t[0], t[1], t[2]);
		Vec3_Set(node->s, s[0], s[1], s[2]);
		Mem_Copy(node->r, r, sizeof(r));

		mat = BMJ_Get(d, json, "matrix");
		if (BMJ_Count(d, mat) == 16) {
			Gltf_M4_Identity(node->matrix);
			Gltf_ReadFloatArray(d, mat, node->matrix, 16);
			node->hasMatrix = true;
		}
	}

	/* Second pass: parents from children lists */
	for (i = 0; i < m->numNodes; i++) {
		int children = BMJ_Get(d, BMJ_At(d, arr, i), "children");
		for (c = 0; c < BMJ_Count(d, children); c++) {
			int child = BMJ_Int(d, BMJ_At(d, children, c), -1);
			if (child >= 0 && child < m->numNodes && child != i) m->nodes[child].parent = i;
		}
	}

	/* Order nodes so that parents always come before children */
	for (pass = 0; pass < m->numNodes && ordered < m->numNodes; pass++) {
		int before = ordered;
		for (i = 0; i < m->numNodes; i++) {
			int parent = m->nodes[i].parent;
			if (placed[i]) continue;
			if (parent >= 0 && !placed[parent]) continue;
			m->nodeOrder[ordered++] = i;
			placed[i] = 1;
		}
		if (ordered == before) break; /* cycle */
	}
	Mem_Free(placed);
	if (ordered != m->numNodes) { Gltf_LogMsg("glTF node hierarchy has a cycle"); return false; }
	return true;
}

/* Allocates palette entries: one per skin joint, then one per node that directly holds a mesh */
static cc_bool Gltf_BuildPalette(struct GltfLoader* l) {
	const struct BMJ_Doc* d = &l->doc;
	struct GltfModel* m = l->m;
	int skins = BMJ_Get(d, 0, "skins"), nodes = BMJ_Get(d, 0, "nodes");
	int s, i, j, total = 0, entry = 0;

	l->numSkins = BMJ_Count(d, skins);
	if (l->numSkins) {
		l->skinPaletteBase = (int*)Mem_TryAlloc(l->numSkins, sizeof(int));
		if (!l->skinPaletteBase) return false;
	}
	for (s = 0; s < l->numSkins; s++) {
		l->skinPaletteBase[s] = total;
		total += BMJ_Count(d, BMJ_Get(d, BMJ_At(d, skins, s), "joints"));
	}
	for (i = 0; i < m->numNodes; i++) {
		int json = BMJ_At(d, nodes, i);
		if (BMJ_Get(d, json, "mesh") >= 0 && BMJ_Get(d, json, "skin") < 0) total++;
	}
	if (!total) { Gltf_LogMsg("glTF has no meshes"); return false; }

	m->numPalette     = total;
	m->paletteNode    = (int*)Mem_TryAlloc(total, sizeof(int));
	m->paletteInvBind = (float*)Mem_TryAlloc(total * 16, sizeof(float));
	if (!m->paletteNode || !m->paletteInvBind) return false;
	for (i = 0; i < total; i++) Gltf_M4_Identity(&m->paletteInvBind[i * 16]);

	for (s = 0; s < l->numSkins; s++) {
		int skin = BMJ_At(d, skins, s), joints = BMJ_Get(d, skin, "joints");
		int numJoints = BMJ_Count(d, joints), ibm = BMJ_GetInt(d, skin, "inverseBindMatrices", -1);
		float* inv = NULL; int invCount = 0;

		if (ibm >= 0) {
			inv = Gltf_AllocFloats(l, ibm, 16, &invCount);
			if (!inv) { Gltf_LogMsg("Couldn't read inverse bind matrices"); return false; }
		}
		for (j = 0; j < numJoints; j++) {
			int node = BMJ_Int(d, BMJ_At(d, joints, j), -1);
			if (node < 0 || node >= m->numNodes) { Mem_Free(inv); Gltf_LogMsg("Skin refers to a missing node"); return false; }
			m->paletteNode[entry] = node;
			if (inv && j < invCount) Mem_Copy(&m->paletteInvBind[entry * 16], &inv[j * 16], 16 * sizeof(float));
			entry++;
		}
		Mem_Free(inv);
	}

	for (i = 0; i < m->numNodes; i++) {
		int json = BMJ_At(d, nodes, i);
		if (BMJ_Get(d, json, "mesh") < 0 || BMJ_Get(d, json, "skin") >= 0) continue;
		m->nodes[i].palette   = entry;
		m->paletteNode[entry] = i;
		entry++;
	}
	return true;
}


/*########################################################################################################################*
*-----------------------------------------------------------Meshes-------------------------------------------------------*
*#########################################################################################################################*/
static cc_bool Gltf_AddCorner(struct GltfLoader* l, const struct GltfCorner* c) {
	struct GltfModel* m = l->m;
	if (m->numCorners >= l->corners_cap) {
		int newCap = l->corners_cap ? l->corners_cap * 2 : 1024;
		void* mem  = Mem_TryRealloc(m->corners, newCap, sizeof(struct GltfCorner));
		if (!mem) { Gltf_LogMsg("Out of memory loading mesh"); return false; }
		m->corners = (struct GltfCorner*)mem;
		l->corners_cap = newCap;
	}
	m->corners[m->numCorners++] = *c;
	return true;
}

static void Gltf_FixNormals(struct GltfCorner* tri) {
	Vec3 e1, e2, n;
	int i;
	Vec3_Sub(&e1, &tri[1].pos, &tri[0].pos);
	Vec3_Sub(&e2, &tri[2].pos, &tri[0].pos);
	n.x = e1.y * e2.z - e1.z * e2.y;
	n.y = e1.z * e2.x - e1.x * e2.z;
	n.z = e1.x * e2.y - e1.y * e2.x;
	Vec3_NormaliseSafe(&n);
	for (i = 0; i < 3; i++) {
		if (Vec3_IsZero(tri[i].nrm)) tri[i].nrm = n;
	}
}

static cc_bool Gltf_LoadPrimitive(struct GltfLoader* l, int prim, int node, int skin) {
	const struct BMJ_Doc* d = &l->doc;
	struct GltfModel* m = l->m;
	struct GltfAccessor pos, nrm, uv, joints, weights, idx;
	int attrs = BMJ_Get(d, prim, "attributes"), mode = BMJ_GetInt(d, prim, "mode", 4);
	int numVerts, numIndices, i, k, paletteBase = 0;
	cc_bool hasNrm, hasUV, hasSkin = false, hasIdx;
	struct GltfCorner tri[3];

	if (mode != 4) { Gltf_LogMsg("Skipping a mesh primitive that isn't a triangle list"); return true; }
	if (!Gltf_GetAccessor(l, BMJ_GetInt(d, attrs, "POSITION", -1), &pos) || pos.comps < 3) {
		Gltf_LogMsg("Mesh primitive has no usable POSITION data"); return true;
	}
	numVerts = pos.count;
	hasNrm = Gltf_GetAccessor(l, BMJ_GetInt(d, attrs, "NORMAL",     -1), &nrm) && nrm.count == numVerts && nrm.comps >= 3;
	hasUV  = Gltf_GetAccessor(l, BMJ_GetInt(d, attrs, "TEXCOORD_0", -1), &uv)  && uv.count  == numVerts && uv.comps  >= 2;

	if (skin >= 0 && skin < l->numSkins) {
		hasSkin = Gltf_GetAccessor(l, BMJ_GetInt(d, attrs, "JOINTS_0",  -1), &joints)  && joints.count  == numVerts && joints.comps  == 4
		       && Gltf_GetAccessor(l, BMJ_GetInt(d, attrs, "WEIGHTS_0", -1), &weights) && weights.count == numVerts && weights.comps == 4;
		paletteBase = l->skinPaletteBase[skin];
		if (!hasSkin) Gltf_LogMsg("Skinned mesh is missing JOINTS_0/WEIGHTS_0, drawing it rigid");
	}

	hasIdx = BMJ_Get(d, prim, "indices") >= 0;
	if (hasIdx) {
		if (!Gltf_GetAccessor(l, BMJ_GetInt(d, prim, "indices", -1), &idx)) { Gltf_LogMsg("Couldn't read mesh indices"); return true; }
		numIndices = idx.count;
	} else {
		numIndices = numVerts;
	}

	for (i = 0; i + 2 < numIndices; i += 3) {
		for (k = 0; k < 3; k++) {
			struct GltfCorner* c = &tri[k];
			const cc_uint8* p;
			cc_uint32 v = hasIdx ? Gltf_ReadUInt(&idx, idx.data + (i + k) * idx.stride) : (cc_uint32)(i + k);
			if (v >= (cc_uint32)numVerts) { Gltf_LogMsg("Mesh index out of range, primitive truncated"); return true; }
			Mem_Set(c, 0, sizeof(*c));

			p = pos.data + v * pos.stride;
			c->pos.x = Gltf_ReadComp(&pos, p);
			c->pos.y = Gltf_ReadComp(&pos, p + pos.compSize);
			c->pos.z = Gltf_ReadComp(&pos, p + pos.compSize * 2);
			if (hasNrm) {
				p = nrm.data + v * nrm.stride;
				c->nrm.x = Gltf_ReadComp(&nrm, p);
				c->nrm.y = Gltf_ReadComp(&nrm, p + nrm.compSize);
				c->nrm.z = Gltf_ReadComp(&nrm, p + nrm.compSize * 2);
			}
			if (hasUV) {
				p = uv.data + v * uv.stride;
				c->u = Gltf_ReadComp(&uv, p);
				c->v = Gltf_ReadComp(&uv, p + uv.compSize);
			}

			if (hasSkin) {
				float sum = 0.0f;
				int j;
				for (j = 0; j < 4; j++) {
					cc_uint32 joint = Gltf_ReadUInt(&joints, joints.data + v * joints.stride + j * joints.compSize);
					float w = Gltf_ReadComp(&weights, weights.data + v * weights.stride + j * weights.compSize);
					if (paletteBase + (int)joint >= m->numPalette) { w = 0; joint = 0; }
					c->joints[j]  = (cc_uint16)(paletteBase + joint);
					c->weights[j] = w < 0 ? 0 : w;
					sum += c->weights[j];
				}
				if (sum > 0.0001f) {
					for (j = 0; j < 4; j++) c->weights[j] /= sum;
				} else {
					c->weights[0] = 1.0f;
				}
			} else {
				c->joints[0]  = (cc_uint16)m->nodes[node].palette;
				c->weights[0] = 1.0f;
			}
		}
		if (!hasNrm) Gltf_FixNormals(tri);
		for (k = 0; k < 3; k++) {
			if (!Gltf_AddCorner(l, &tri[k])) return false;
		}
	}
	return true;
}

static cc_bool Gltf_LoadMeshes(struct GltfLoader* l) {
	const struct BMJ_Doc* d = &l->doc;
	struct GltfModel* m = l->m;
	int nodes = BMJ_Get(d, 0, "nodes"), i, p;

	for (i = 0; i < m->numNodes; i++) {
		int json = BMJ_At(d, nodes, i), skin = BMJ_GetInt(d, json, "skin", -1);
		int mesh = BMJ_GetAt(d, 0, "meshes", BMJ_GetInt(d, json, "mesh", -1)), prims;
		if (mesh < 0) continue;

		prims = BMJ_Get(d, mesh, "primitives");
		for (p = 0; p < BMJ_Count(d, prims); p++) {
			if (!Gltf_LoadPrimitive(l, BMJ_At(d, prims, p), i, skin)) return false;
		}
	}
	if (!m->numCorners) { Gltf_LogMsg("glTF meshes contain no triangles"); return false; }
	return true;
}


/*########################################################################################################################*
*---------------------------------------------------------Animations-----------------------------------------------------*
*#########################################################################################################################*/
static cc_bool Gltf_LoadAnimations(struct GltfLoader* l) {
	const struct BMJ_Doc* d = &l->doc;
	struct GltfModel* m = l->m;
	int anims = BMJ_Get(d, 0, "animations"), a, c, total = 0;

	m->numClips = BMJ_Count(d, anims);
	if (!m->numClips) return true;
	for (a = 0; a < m->numClips; a++) total += BMJ_Count(d, BMJ_Get(d, BMJ_At(d, anims, a), "channels"));

	m->clips    = (struct GltfClip*)Mem_TryAllocCleared(m->numClips, sizeof(struct GltfClip));
	m->channels = (struct GltfChannel*)Mem_TryAllocCleared(total ? total : 1, sizeof(struct GltfChannel));
	if (!m->clips || !m->channels) return false;

	for (a = 0; a < m->numClips; a++) {
		int anim = BMJ_At(d, anims, a), channels = BMJ_Get(d, anim, "channels"), samplers = BMJ_Get(d, anim, "samplers");
		struct GltfClip* clip = &m->clips[a];
		cc_string name = BMJ_GetStr(d, anim, "name");
		Gltf_CopyName(clip->name, &clip->nameLen, &name);
		clip->firstChannel = m->numChannels;

		for (c = 0; c < BMJ_Count(d, channels); c++) {
			int chan = BMJ_At(d, channels, c), target = BMJ_Get(d, chan, "target");
			int sampler = BMJ_At(d, samplers, BMJ_GetInt(d, chan, "sampler", -1));
			int node = BMJ_GetInt(d, target, "node", -1), comps, numTimes, numValues;
			cc_string path = BMJ_GetStr(d, target, "path"), interp = BMJ_GetStr(d, sampler, "interpolation");
			struct GltfChannel* ch = &m->channels[m->numChannels];
			float* raw;

			if (node < 0 || node >= m->numNodes || sampler < 0) continue;
			if (String_CaselessEqualsConst(&path, "translation"))   { ch->path = GLTF_PATH_TRANSLATION; comps = 3; }
			else if (String_CaselessEqualsConst(&path, "rotation")) { ch->path = GLTF_PATH_ROTATION;    comps = 4; }
			else if (String_CaselessEqualsConst(&path, "scale"))    { ch->path = GLTF_PATH_SCALE;       comps = 3; }
			else continue; /* morph target weights aren't supported */

			ch->times = Gltf_AllocFloats(l, BMJ_GetInt(d, sampler, "input", -1), 1, &numTimes);
			raw       = Gltf_AllocFloats(l, BMJ_GetInt(d, sampler, "output", -1), comps, &numValues);
			if (!ch->times || !raw) { Mem_Free(ch->times); Mem_Free(raw); ch->times = NULL; continue; }

			if (String_CaselessEqualsConst(&interp, "CUBICSPLINE")) {
				/* in-tangent, value, out-tangent per key: keep the values and interpolate linearly */
				int k;
				if (numValues < numTimes * 3) { Mem_Free(ch->times); Mem_Free(raw); ch->times = NULL; continue; }
				for (k = 0; k < numTimes; k++) Mem_Copy(&raw[k * comps], &raw[(k * 3 + 1) * comps], comps * sizeof(float));
				ch->interp = GLTF_INTERP_LINEAR;
			} else if (numValues < numTimes) {
				Mem_Free(ch->times); Mem_Free(raw); ch->times = NULL; continue;
			} else {
				ch->interp = String_CaselessEqualsConst(&interp, "STEP") ? GLTF_INTERP_STEP : GLTF_INTERP_LINEAR;
			}

			ch->node    = node;
			ch->values  = raw;
			ch->numKeys = numTimes;
			if (numTimes && ch->times[numTimes - 1] > clip->duration) clip->duration = ch->times[numTimes - 1];
			m->numChannels++;
		}
		clip->numChannels = m->numChannels - clip->firstChannel;
	}
	return true;
}


/*########################################################################################################################*
*-------------------------------------------------------Texture/extras---------------------------------------------------*
*#########################################################################################################################*/
static cc_bool Gltf_DecodePng(struct GltfModel* m, cc_uint8* data, cc_uint32 len) {
	struct Stream stream;
	cc_result res;
	Stream_ReadonlyMemory(&stream, data, len);
	res = Png_Decode(&m->texture, &stream);
	if (res) { Mem_Free(m->texture.scan0); m->texture.scan0 = NULL; Gltf_LogMsg("Embedded texture isn't a valid PNG"); return false; }
	return true;
}

static void Gltf_LoadTexture(struct GltfLoader* l) {
	const struct BMJ_Doc* d = &l->doc;
	int materials = BMJ_Get(d, 0, "materials"), i, image = -1, view;
	cc_string uri;
	cc_uint8* data; cc_uint32 len;

	for (i = 0; i < BMJ_Count(d, materials) && image < 0; i++) {
		int mat = BMJ_At(d, materials, i);
		int pbr = BMJ_Get(d, mat, "pbrMetallicRoughness");
		int tex = BMJ_GetInt(d, BMJ_Get(d, pbr, "baseColorTexture"), "index", -1);
		image = BMJ_GetInt(d, BMJ_GetAt(d, 0, "textures", tex), "source", -1);
	}
	image = BMJ_GetAt(d, 0, "images", image);
	if (image < 0) return;

	uri = BMJ_GetStr(d, image, "uri");
	if (uri.length) {
		if (!Gltf_LoadUri(l, &uri, &data, &len)) return;
		Gltf_DecodePng(l->m, data, len);
		Mem_Free(data);
		return;
	}

	view = BMJ_GetAt(d, 0, "bufferViews", BMJ_GetInt(d, image, "bufferView", -1));
	if (view >= 0) {
		int buffer = BMJ_GetInt(d, view, "buffer", -1);
		int offset = BMJ_GetInt(d, view, "byteOffset", 0), length = BMJ_GetInt(d, view, "byteLength", 0);
		if (buffer < 0 || buffer >= l->numBuffers || offset < 0 || length <= 0) return;
		if ((cc_uint32)offset + length > l->buffers[buffer].len) return;
		Gltf_DecodePng(l->m, l->buffers[buffer].data + offset, length);
	}
}

static void Gltf_ReadExtras(struct GltfLoader* l, int obj) {
	const struct BMJ_Doc* d = &l->doc;
	struct GltfModel* m = l->m;
	int extras = BMJ_Get(d, obj, "extras"), eye, size;
	if (extras < 0) return;

	eye = BMJ_Get(d, extras, "cc_eye_height");
	if (eye >= 0 && !m->hasEye) { m->eyeY = (float)BMJ_Num(d, eye, 0); m->hasEye = true; }

	size = BMJ_Get(d, extras, "cc_size");
	if (BMJ_Count(d, size) == 3 && !m->hasSize) {
		float v[3] = { 0, 0, 0 };
		Gltf_ReadFloatArray(d, size, v, 3);
		Vec3_Set(m->size, v[0], v[1], v[2]);
		m->hasSize = true;
	}
}

static void Gltf_LoadExtras(struct GltfLoader* l) {
	const struct BMJ_Doc* d = &l->doc;
	int i;
	Gltf_ReadExtras(l, BMJ_Get(d, 0, "asset"));
	Gltf_ReadExtras(l, BMJ_GetAt(d, 0, "scenes", 0));
	for (i = 0; i < l->m->numNodes; i++) {
		if (l->m->nodes[i].parent < 0) Gltf_ReadExtras(l, BMJ_GetAt(d, 0, "nodes", i));
	}
}


/*########################################################################################################################*
*-------------------------------------------------------------Load-------------------------------------------------------*
*#########################################################################################################################*/
static void Gltf_LoaderFree(struct GltfLoader* l) {
	int i;
	for (i = 0; i < l->numBuffers; i++) {
		if (l->buffers[i].owned) Mem_Free(l->buffers[i].data);
	}
	Mem_Free(l->buffers);
	Mem_Free(l->skinPaletteBase);
	BMJ_Free(&l->doc);
	Mem_Free(l->file);
}

void Gltf_Free(struct GltfModel* m) {
	int i;
	for (i = 0; i < m->numChannels; i++) {
		Mem_Free(m->channels[i].times);
		Mem_Free(m->channels[i].values);
	}
	Mem_Free(m->channels);
	Mem_Free(m->clips);
	Mem_Free(m->corners);
	Mem_Free(m->nodes);
	Mem_Free(m->nodeOrder);
	Mem_Free(m->paletteNode);
	Mem_Free(m->paletteInvBind);
	Mem_Free(m->texture.scan0);
	Mem_Set(m, 0, sizeof(*m));
}

cc_bool Gltf_Load(struct GltfModel* m, const cc_string* path, float scale, cc_bool flipZ) {
	struct GltfLoader l;
	int i, slash = -1;
	cc_bool ok;

	Mem_Set(&l, 0, sizeof(l));
	Mem_Set(m,  0, sizeof(*m));
	l.m = m;
	String_InitArray(l.dir, l.dirBuffer);
	for (i = path->length - 1; i >= 0; i--) {
		if (path->buffer[i] == '/' || path->buffer[i] == '\\') { slash = i; break; }
	}
	if (slash >= 0) {
		cc_string dir = String_UNSAFE_Substring(path, 0, slash + 1);
		String_AppendString(&l.dir, &dir);
	}

	ok = Gltf_OpenContainer(&l, path) && Gltf_LoadBuffers(&l) && Gltf_LoadNodes(&l)
	  && Gltf_BuildPalette(&l) && Gltf_LoadMeshes(&l) && Gltf_LoadAnimations(&l);
	if (ok) {
		Gltf_LoadTexture(&l);
		Gltf_LoadExtras(&l);
		Gltf_SetRootXform(m, scale, flipZ);
	}

	Gltf_LoaderFree(&l);
	if (!ok) Gltf_Free(m);
	return ok;
}


/*########################################################################################################################*
*-------------------------------------------------------------Pose-------------------------------------------------------*
*#########################################################################################################################*/
cc_bool Gltf_Pose_Alloc(const struct GltfModel* m, struct GltfPose* pose) {
	Mem_Set(pose, 0, sizeof(*pose));
	pose->local   = (float*)Mem_TryAlloc(m->numNodes * 16,   sizeof(float));
	pose->global  = (float*)Mem_TryAlloc(m->numNodes * 16,   sizeof(float));
	pose->palette = (float*)Mem_TryAlloc(m->numPalette * 16, sizeof(float));
	if (pose->local && pose->global && pose->palette) return true;
	Gltf_Pose_Free(pose);
	return false;
}

void Gltf_Pose_Free(struct GltfPose* pose) {
	Mem_Free(pose->local);
	Mem_Free(pose->global);
	Mem_Free(pose->palette);
	Mem_Set(pose, 0, sizeof(*pose));
}

/* Samples a channel at time t into out (comps values) */
static void Gltf_SampleChannel(const struct GltfChannel* ch, float t, float* out) {
	int comps = ch->path == GLTF_PATH_ROTATION ? 4 : 3;
	int lo = 0, hi = ch->numKeys - 1, mid, c;
	float t0, t1, blend, dot;

	if (t <= ch->times[0]) { Mem_Copy(out, ch->values, comps * sizeof(float)); return; }
	if (t >= ch->times[hi]) { Mem_Copy(out, &ch->values[hi * comps], comps * sizeof(float)); return; }

	/* Find lo such that times[lo] <= t < times[lo + 1] */
	while (hi - lo > 1) {
		mid = (lo + hi) / 2;
		if (ch->times[mid] <= t) lo = mid; else hi = mid;
	}
	if (ch->interp == GLTF_INTERP_STEP) { Mem_Copy(out, &ch->values[lo * comps], comps * sizeof(float)); return; }

	t0 = ch->times[lo]; t1 = ch->times[lo + 1];
	blend = t1 > t0 ? (t - t0) / (t1 - t0) : 0.0f;
	if (comps == 4) {
		/* Normalised lerp, flipping sign so the shorter arc is taken */
		const float* a = &ch->values[lo * 4];
		const float* b = &ch->values[(lo + 1) * 4];
		float len = 0.0f;
		dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
		for (c = 0; c < 4; c++) {
			out[c] = a[c] * (1 - blend) + (dot < 0 ? -b[c] : b[c]) * blend;
			len += out[c] * out[c];
		}
		len = Math_SqrtF(len);
		if (len > 0.0f) for (c = 0; c < 4; c++) out[c] /= len;
	} else {
		for (c = 0; c < 3; c++) {
			out[c] = ch->values[lo * 3 + c] * (1 - blend) + ch->values[(lo + 1) * 3 + c] * blend;
		}
	}
}

void Gltf_EvalPose(const struct GltfModel* m, struct GltfPose* pose, int clip, float time,
                   int headNode, float headPitch, float headYaw) {
	int i, c;
	float tmp[16], rot[16], tmp2[16];

	/* Rest pose transforms */
	for (i = 0; i < m->numNodes; i++) {
		const struct GltfNode* n = &m->nodes[i];
		if (n->hasMatrix) { Mem_Copy(&pose->local[i * 16], n->matrix, 16 * sizeof(float)); }
		else { M4_FromTRS(&pose->local[i * 16], n->t, n->r, n->s); }
	}

	/* Overwrite with animated transforms */
	if (clip >= 0 && clip < m->numClips) {
		const struct GltfClip* cl = &m->clips[clip];
		Vec3 t, s; float r[4]; float v[4];
		cc_uint8 touched;
		int node;

		/* Gather per node so T, R and S channels combine into one matrix */
		for (i = 0; i < m->numNodes; i++) {
			touched = 0;
			for (c = cl->firstChannel; c < cl->firstChannel + cl->numChannels; c++) {
				if (m->channels[c].node == i) { touched = 1; break; }
			}
			if (!touched) continue;

			node = i;
			t = m->nodes[node].t; s = m->nodes[node].s;
			Mem_Copy(r, m->nodes[node].r, sizeof(r));
			for (c = cl->firstChannel; c < cl->firstChannel + cl->numChannels; c++) {
				const struct GltfChannel* ch = &m->channels[c];
				if (ch->node != node || !ch->numKeys) continue;
				Gltf_SampleChannel(ch, time, v);
				if (ch->path == GLTF_PATH_TRANSLATION)   { Vec3_Set(t, v[0], v[1], v[2]); }
				else if (ch->path == GLTF_PATH_ROTATION) { Mem_Copy(r, v, sizeof(r)); }
				else                                     { Vec3_Set(s, v[0], v[1], v[2]); }
			}
			M4_FromTRS(&pose->local[node * 16], t, r, s);
		}
	}

	/* Head look: extra pitch about local X and yaw about local Y, applied in the node's own space */
	if (headNode >= 0 && headNode < m->numNodes && (headPitch != 0.0f || headYaw != 0.0f)) {
		M4_RotateY(rot, headYaw);
		M4_RotateX(tmp, headPitch);
		Gltf_M4_Mul(tmp2, rot, tmp);
		Gltf_M4_Mul(tmp, &pose->local[headNode * 16], tmp2);
		Mem_Copy(&pose->local[headNode * 16], tmp, 16 * sizeof(float));
	}

	/* Globals: parents are guaranteed to come first in nodeOrder */
	for (i = 0; i < m->numNodes; i++) {
		int node = m->nodeOrder[i], parent = m->nodes[node].parent;
		const float* above = parent >= 0 ? &pose->global[parent * 16] : m->rootXform;
		Gltf_M4_Mul(&pose->global[node * 16], above, &pose->local[node * 16]);
	}

	for (i = 0; i < m->numPalette; i++) {
		Gltf_M4_Mul(&pose->palette[i * 16], &pose->global[m->paletteNode[i] * 16], &m->paletteInvBind[i * 16]);
	}
}

void Gltf_SkinCorner(const struct GltfModel* m, const struct GltfPose* pose, const struct GltfCorner* c, Vec3* pos, Vec3* nrm) {
	Vec3 p, n, tp, tn;
	int j;
	Vec3_Set(p, 0, 0, 0);
	Vec3_Set(n, 0, 0, 0);

	for (j = 0; j < 4; j++) {
		const float* mat;
		float w = c->weights[j];
		if (w <= 0.0f || c->joints[j] >= m->numPalette) continue;
		mat = &pose->palette[c->joints[j] * 16];

		tp = Gltf_M4_TransformPoint(mat, c->pos);
		tn = Gltf_M4_TransformDir(mat, c->nrm);
		p.x += tp.x * w; p.y += tp.y * w; p.z += tp.z * w;
		n.x += tn.x * w; n.y += tn.y * w; n.z += tn.z * w;
	}
	Vec3_NormaliseSafe(&n);
	*pos = p; *nrm = n;
}

Vec3 Gltf_NodePosition(const struct GltfModel* m, const struct GltfPose* pose, int node) {
	Vec3 zero; Vec3_Set(zero, 0, 0, 0);
	if (node < 0 || node >= m->numNodes) return zero;
	return Gltf_M4_TransformPoint(&pose->global[node * 16], zero);
}


/*########################################################################################################################*
*------------------------------------------------------------Names-------------------------------------------------------*
*#########################################################################################################################*/
cc_string Gltf_NodeName(const struct GltfModel* m, int node) {
	cc_string name;
	if (node < 0 || node >= m->numNodes) return String_Empty;
	name.buffer   = (char*)m->nodes[node].name;
	name.length   = m->nodes[node].nameLen;
	name.capacity = GLTF_NAME_SIZE;
	return name;
}

cc_string Gltf_ClipName(const struct GltfModel* m, int clip) {
	cc_string name;
	if (clip < 0 || clip >= m->numClips) return String_Empty;
	name.buffer   = (char*)m->clips[clip].name;
	name.length   = m->clips[clip].nameLen;
	name.capacity = GLTF_NAME_SIZE;
	return name;
}

int Gltf_FindClip(const struct GltfModel* m, const char* const* keywords, int numKeywords) {
	int i, k;
	for (k = 0; k < numKeywords; k++) {
		cc_string word = String_FromReadonly(keywords[k]);
		for (i = 0; i < m->numClips; i++) {
			cc_string name = Gltf_ClipName(m, i);
			if (String_CaselessContains(&name, &word)) return i;
		}
	}
	return -1;
}

int Gltf_FindClipByName(const struct GltfModel* m, const cc_string* name) {
	int i;
	for (i = 0; i < m->numClips; i++) {
		cc_string clip = Gltf_ClipName(m, i);
		if (String_CaselessEquals(&clip, name)) return i;
	}
	return -1;
}
