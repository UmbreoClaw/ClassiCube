#include "RayTracer.h"
#ifdef CC_BUILD_RAYTRACING
#include "Graphics.h"
#include "Window.h"
#include "World.h"
#include "Block.h"
#include "Game.h"
#include "Camera.h"
#include "Event.h"
#include "Chat.h"
#include "Options.h"
#include "Platform.h"
#include "Logger.h"
#include "TexturePack.h"
#include "ExtMath.h"
#include "String_.h"
#include "Funcs.h"
#include "Physics.h"
#include "_RayTracerShaders.h"

/*########################################################################################################################*
*-----------------------------------------------------OpenGL bindings-----------------------------------------------------*
*#########################################################################################################################*/
/* This file deliberately doesn't include the platform OpenGL headers - everything from */
/*  OpenGL 4.3 that is needed is declared here and loaded through GLContext_GetAddress */
#if defined CC_BUILD_WIN
	#define RT_APIENTRY __stdcall
#else
	#define RT_APIENTRY
#endif

typedef unsigned int  RTenum;
typedef unsigned int  RTuint;
typedef int           RTint;
typedef int           RTsizei;
typedef unsigned char RTboolean;
typedef unsigned char RTubyte;
typedef char          RTchar;
typedef cc_uintptr    RTsizeiptr;
typedef cc_uintptr    RTintptr;
typedef unsigned int  RTbitfield;

#define GL_NO_ERROR                 0
#define GL_TRIANGLES                0x0004
#define GL_UNSIGNED_BYTE            0x1401
#define GL_UNSIGNED_SHORT           0x1403
#define GL_FLOAT                    0x1406
#define GL_RGBA                     0x1908
#define GL_NEAREST                  0x2600
#define GL_TEXTURE_MAG_FILTER       0x2800
#define GL_TEXTURE_MIN_FILTER       0x2801
#define GL_TEXTURE_WRAP_S           0x2802
#define GL_TEXTURE_WRAP_T           0x2803
#define GL_TEXTURE_2D               0x0DE1
#define GL_UNPACK_ALIGNMENT         0x0CF5
#define GL_TEXTURE_3D               0x806F
#define GL_TEXTURE_WRAP_R           0x8072
#define GL_MAX_3D_TEXTURE_SIZE      0x8073
#define GL_CLAMP_TO_EDGE            0x812F
#define GL_MAJOR_VERSION            0x821B
#define GL_MINOR_VERSION            0x821C
#define GL_R8UI                     0x8232
#define GL_R16UI                    0x8234
#define GL_TEXTURE0                 0x84C0
#define GL_RGBA32F                  0x8814
#define GL_RGBA16F                  0x881A
#define GL_STATIC_DRAW              0x88E4
#define GL_DYNAMIC_DRAW             0x88E8
#define GL_READ_ONLY                0x88B8
#define GL_WRITE_ONLY               0x88B9
#define GL_UNIFORM_BUFFER           0x8A11
#define GL_FRAGMENT_SHADER          0x8B30
#define GL_VERTEX_SHADER            0x8B31
#define GL_COMPILE_STATUS           0x8B81
#define GL_LINK_STATUS              0x8B82
#define GL_INFO_LOG_LENGTH          0x8B84
#define GL_RED_INTEGER              0x8D94
#define GL_SHADER_STORAGE_BUFFER    0x90D2
#define GL_COMPUTE_SHADER           0x91B9
#define GL_TEXTURE_FETCH_BARRIER_BIT    0x00000008
#define GL_SHADER_IMAGE_ACCESS_BARRIER_BIT 0x00000020

#define RT_GL_FUNCS \
	GL_FUNC(void,    glGetIntegerv,      (RTenum pname, RTint* data)) \
	GL_FUNC(RTenum,  glGetError,         (void)) \
	GL_FUNC(void,    glGenTextures,      (RTsizei n, RTuint* textures)) \
	GL_FUNC(void,    glDeleteTextures,   (RTsizei n, const RTuint* textures)) \
	GL_FUNC(void,    glBindTexture,      (RTenum target, RTuint texture)) \
	GL_FUNC(void,    glTexParameteri,    (RTenum target, RTenum pname, RTint param)) \
	GL_FUNC(void,    glTexImage2D,       (RTenum target, RTint level, RTint internalformat, RTsizei width, RTsizei height, RTint border, RTenum format, RTenum type, const void* pixels)) \
	GL_FUNC(void,    glTexImage3D,       (RTenum target, RTint level, RTint internalformat, RTsizei width, RTsizei height, RTsizei depth, RTint border, RTenum format, RTenum type, const void* pixels)) \
	GL_FUNC(void,    glTexSubImage3D,    (RTenum target, RTint level, RTint xoffset, RTint yoffset, RTint zoffset, RTsizei width, RTsizei height, RTsizei depth, RTenum format, RTenum type, const void* pixels)) \
	GL_FUNC(void,    glPixelStorei,      (RTenum pname, RTint param)) \
	GL_FUNC(void,    glActiveTexture,    (RTenum texture)) \
	GL_FUNC(void,    glDrawArrays,       (RTenum mode, RTint first, RTsizei count)) \
	GL_FUNC(void,    glGenBuffers,       (RTsizei n, RTuint* buffers)) \
	GL_FUNC(void,    glDeleteBuffers,    (RTsizei n, const RTuint* buffers)) \
	GL_FUNC(void,    glBindBuffer,       (RTenum target, RTuint buffer)) \
	GL_FUNC(void,    glBufferData,       (RTenum target, RTsizeiptr size, const void* data, RTenum usage)) \
	GL_FUNC(void,    glBufferSubData,    (RTenum target, RTintptr offset, RTsizeiptr size, const void* data)) \
	GL_FUNC(void,    glBindBufferBase,   (RTenum target, RTuint index, RTuint buffer)) \
	GL_FUNC(RTuint,  glCreateShader,     (RTenum type)) \
	GL_FUNC(void,    glShaderSource,     (RTuint shader, RTsizei count, const RTchar* const* string, const RTint* length)) \
	GL_FUNC(void,    glCompileShader,    (RTuint shader)) \
	GL_FUNC(void,    glGetShaderiv,      (RTuint shader, RTenum pname, RTint* params)) \
	GL_FUNC(void,    glGetShaderInfoLog, (RTuint shader, RTsizei bufSize, RTsizei* length, RTchar* infoLog)) \
	GL_FUNC(void,    glDeleteShader,     (RTuint shader)) \
	GL_FUNC(RTuint,  glCreateProgram,    (void)) \
	GL_FUNC(void,    glAttachShader,     (RTuint program, RTuint shader)) \
	GL_FUNC(void,    glLinkProgram,      (RTuint program)) \
	GL_FUNC(void,    glGetProgramiv,     (RTuint program, RTenum pname, RTint* params)) \
	GL_FUNC(void,    glGetProgramInfoLog,(RTuint program, RTsizei bufSize, RTsizei* length, RTchar* infoLog)) \
	GL_FUNC(void,    glUseProgram,       (RTuint program)) \
	GL_FUNC(void,    glDeleteProgram,    (RTuint program)) \
	GL_FUNC(RTint,   glGetUniformLocation,(RTuint program, const RTchar* name)) \
	GL_FUNC(void,    glUniform1i,        (RTint location, RTint v0)) \
	GL_FUNC(void,    glDispatchCompute,  (RTuint x, RTuint y, RTuint z)) \
	GL_FUNC(void,    glMemoryBarrier,    (RTbitfield barriers)) \
	GL_FUNC(void,    glBindImageTexture, (RTuint unit, RTuint texture, RTint level, RTboolean layered, RTint layer, RTenum access, RTenum format)) \
	GL_FUNC(void,    glGenVertexArrays,  (RTsizei n, RTuint* arrays)) \
	GL_FUNC(void,    glBindVertexArray,  (RTuint array)) \
	GL_FUNC(void,    glDeleteVertexArrays,(RTsizei n, const RTuint* arrays))

/* e.g. static void (RT_APIENTRY *_glGetIntegerv)(RTenum pname, RTint* data); */
#define GL_FUNC(retType, name, args) static retType (RT_APIENTRY *_ ## name)args;
RT_GL_FUNCS
#undef GL_FUNC

static const struct DynamicLibSym rt_glFuncs[] = {
#define GL_FUNC(retType, name, args) { DYNAMICLIB_QUOTE(name), (void**)&_ ## name, true },
RT_GL_FUNCS
#undef GL_FUNC
};

/* Implemented by the OpenGL backend, rebinds the shader program it thinks is active */
void GLBackend_RestoreProgram(void);


/*########################################################################################################################*
*-------------------------------------------------------State/options-----------------------------------------------------*
*#########################################################################################################################*/
const char* const RayTracerMode_Names[RT_MODE_COUNT] = { "Off", "Shadows", "GI", "Full" };
int RayTracer_Mode;

#define RT_MAX_ATLASES 4

/* Must match Params uniform block in rt_common.glsl (std140 layout) */
struct RTParams {
	float invViewProj[16];
	float viewProj[16];
	float prevViewProj[16];
	float view[16];
	float camPos[4];
	float sunDir[4];
	float sunCol[4], sunXSide[4], sunZSide[4], sunYMin[4];
	float shadowCol[4], shadowXSide[4], shadowZSide[4], shadowYMin[4];
	float skyCol[4];
	float fogCol[4];
	float fogParams[4];
	cc_int32 worldSize[4];
	cc_int32 screen[4];
	cc_int32 atlas[4];
	float misc[4];
};

/* Must match BlockInfo in rt_common.glsl (std430 layout) */
struct RTBlockInfo {
	float minBB[4];
	float maxBB[4];
	cc_uint32 texA[4];
	cc_uint32 texB[4];
	float tint[4];
};

#define RT_FLAG_GI           1
#define RT_FLAG_REFLECTIONS  2
#define RT_FLAG_SOFT_SHADOWS 4

static struct {
	cc_bool triedInit, initialised, supported;
	cc_bool worldValid, worldWide, worldTried, blocksDirty;
	int width, height, frame;
	RTuint traceProg, temporalProg, atrousProg, compositeProg;
	RTint  atrousStepLoc;
	RTuint paramsUbo, blocksSsbo, worldTex, vao;
	/* [0] and [1] alternate each frame, so the previous frame's data stays available */
	RTuint gbuf[2], normal[2], accumDir[2], accumInd[2];
	RTuint albedo, direct, indirect, extra, atrousTmp[2];
	struct Matrix prevViewProj;
	cc_bool havePrev;
	int max3DSize;
} rt;

static struct { float sunX, sunZ, sunRadius, ambient, emissive, giDistance; int debug; } rt_opts;


/*########################################################################################################################*
*--------------------------------------------------------Utilities--------------------------------------------------------*
*#########################################################################################################################*/
static void RT_Disable(const char* reason) {
	rt.supported = false;
	RayTracer_Mode = RT_MODE_OFF;
	Chat_Add1("&cRay tracing disabled: %c", reason);
	Platform_Log1("Ray tracing disabled: %c", reason);
}

static void RT_ColToVec(float* dst, PackedCol col) {
	dst[0] = PackedCol_R(col) / 255.0f;
	dst[1] = PackedCol_G(col) / 255.0f;
	dst[2] = PackedCol_B(col) / 255.0f;
	dst[3] = 1.0f;
}

static void RT_CopyMatrix(float* dst, const struct Matrix* m) {
	Mem_Copy(dst, m, 16 * sizeof(float));
}

/* General 4x4 matrix inverse (cofactor expansion) */
static cc_bool RT_InvertMatrix(struct Matrix* dst, const struct Matrix* src) {
	const float* m = (const float*)src;
	float inv[16], det;
	int i;

	inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
	inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
	inv[8]  =  m[4]*m[9]*m[15]  - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
	inv[12] = -m[4]*m[9]*m[14]  + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
	inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
	inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
	inv[9]  = -m[0]*m[9]*m[15]  + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
	inv[13] =  m[0]*m[9]*m[14]  - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
	inv[2]  =  m[1]*m[6]*m[15]  - m[1]*m[7]*m[14]  - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7]  - m[13]*m[3]*m[6];
	inv[6]  = -m[0]*m[6]*m[15]  + m[0]*m[7]*m[14]  + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7]  + m[12]*m[3]*m[6];
	inv[10] =  m[0]*m[5]*m[15]  - m[0]*m[7]*m[13]  - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7]  - m[12]*m[3]*m[5];
	inv[14] = -m[0]*m[5]*m[14]  + m[0]*m[6]*m[13]  + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6]  + m[12]*m[2]*m[5];
	inv[3]  = -m[1]*m[6]*m[11]  + m[1]*m[7]*m[10]  + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7]   + m[9]*m[3]*m[6];
	inv[7]  =  m[0]*m[6]*m[11]  - m[0]*m[7]*m[10]  - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7]   - m[8]*m[3]*m[6];
	inv[11] = -m[0]*m[5]*m[11]  + m[0]*m[7]*m[9]   + m[4]*m[1]*m[11] - m[4]*m[3]*m[9]  - m[8]*m[1]*m[7]   + m[8]*m[3]*m[5];
	inv[15] =  m[0]*m[5]*m[10]  - m[0]*m[6]*m[9]   - m[4]*m[1]*m[10] + m[4]*m[2]*m[9]  + m[8]*m[1]*m[6]   - m[8]*m[2]*m[5];

	det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
	if (det == 0.0f) return false;
	det = 1.0f / det;

	for (i = 0; i < 16; i++) ((float*)dst)[i] = inv[i] * det;
	return true;
}


/*########################################################################################################################*
*-------------------------------------------------------Shader setup------------------------------------------------------*
*#########################################################################################################################*/
static RTuint RT_CompileShader(RTenum type, const char* src, const char* name) {
	RTuint shader = _glCreateShader(type);
	RTint status = 0, logLen = 0;
	char log[2048];

	_glShaderSource(shader, 1, &src, NULL);
	_glCompileShader(shader);
	_glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
	if (status) return shader;

	_glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &logLen);
	log[0] = '\0';
	if (logLen > 0) _glGetShaderInfoLog(shader, sizeof(log) - 1, NULL, log);
	Platform_Log2("Failed to compile ray tracing shader %c:\n%c", name, log);
	Window_ShowDialog("Failed to compile ray tracing shader", log);
	_glDeleteShader(shader);
	return 0;
}

static RTuint RT_LinkProgram(RTuint s1, RTuint s2, const char* name) {
	RTuint prog = _glCreateProgram();
	RTint status = 0, logLen = 0;
	char log[2048];

	_glAttachShader(prog, s1);
	if (s2) _glAttachShader(prog, s2);
	_glLinkProgram(prog);
	_glDeleteShader(s1);
	if (s2) _glDeleteShader(s2);

	_glGetProgramiv(prog, GL_LINK_STATUS, &status);
	if (status) return prog;

	_glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &logLen);
	log[0] = '\0';
	if (logLen > 0) _glGetProgramInfoLog(prog, sizeof(log) - 1, NULL, log);
	Platform_Log2("Failed to link ray tracing program %c:\n%c", name, log);
	Window_ShowDialog("Failed to link ray tracing program", log);
	_glDeleteProgram(prog);
	return 0;
}

static RTuint RT_MakeCompute(const char* src, const char* name) {
	RTuint cs = RT_CompileShader(GL_COMPUTE_SHADER, src, name);
	if (!cs) return 0;
	return RT_LinkProgram(cs, 0, name);
}

static cc_bool RT_CreatePrograms(void) {
	RTuint vs, fs;
	rt.traceProg    = RT_MakeCompute(RT_TRACE_SRC,    "trace");
	rt.temporalProg = RT_MakeCompute(RT_TEMPORAL_SRC, "temporal");
	rt.atrousProg   = RT_MakeCompute(RT_ATROUS_SRC,   "atrous");
	if (!rt.traceProg || !rt.temporalProg || !rt.atrousProg) return false;

	vs = RT_CompileShader(GL_VERTEX_SHADER,   RT_COMPOSITE_VS, "composite vertex");
	if (!vs) return false;
	fs = RT_CompileShader(GL_FRAGMENT_SHADER, RT_COMPOSITE_FS, "composite fragment");
	if (!fs) { _glDeleteShader(vs); return false; }
	rt.compositeProg = RT_LinkProgram(vs, fs, "composite");
	if (!rt.compositeProg) return false;

	rt.atrousStepLoc = _glGetUniformLocation(rt.atrousProg, "stepSize");
	return true;
}


/*########################################################################################################################*
*------------------------------------------------------GPU resources------------------------------------------------------*
*#########################################################################################################################*/
/* zeros must be uploaded, as the history buffers are read before they are first written */
static RTuint RT_CreateTexture2D(RTenum internalFormat, int width, int height, void* zeros) {
	RTuint tex = 0;
	_glGenTextures(1, &tex);
	_glBindTexture(GL_TEXTURE_2D, tex);
	_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	_glTexImage2D(GL_TEXTURE_2D, 0, internalFormat, width, height, 0, GL_RGBA, GL_FLOAT, zeros);
	return tex;
}

static void RT_DeleteTexture(RTuint* tex) {
	if (*tex) _glDeleteTextures(1, tex);
	*tex = 0;
}

static void RT_FreeScreenTextures(void) {
	int i;
	for (i = 0; i < 2; i++) {
		RT_DeleteTexture(&rt.gbuf[i]);
		RT_DeleteTexture(&rt.normal[i]);
		RT_DeleteTexture(&rt.accumDir[i]);
		RT_DeleteTexture(&rt.accumInd[i]);
		RT_DeleteTexture(&rt.atrousTmp[i]);
	}
	RT_DeleteTexture(&rt.albedo);
	RT_DeleteTexture(&rt.direct);
	RT_DeleteTexture(&rt.indirect);
	RT_DeleteTexture(&rt.extra);
	rt.width = 0; rt.height = 0;
	rt.havePrev = false;
}

static void RT_CreateScreenTextures(int width, int height) {
	void* zeros = Mem_AllocCleared(width * height, 4 * sizeof(float), "ray tracer textures");
	int i;
	RT_FreeScreenTextures();
	_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

	for (i = 0; i < 2; i++) {
		rt.gbuf[i]      = RT_CreateTexture2D(GL_RGBA32F, width, height, zeros);
		rt.normal[i]    = RT_CreateTexture2D(GL_RGBA32F, width, height, zeros);
		rt.accumDir[i]  = RT_CreateTexture2D(GL_RGBA16F, width, height, zeros);
		rt.accumInd[i]  = RT_CreateTexture2D(GL_RGBA16F, width, height, zeros);
		rt.atrousTmp[i] = RT_CreateTexture2D(GL_RGBA16F, width, height, zeros);
	}
	rt.albedo   = RT_CreateTexture2D(GL_RGBA16F, width, height, zeros);
	rt.direct   = RT_CreateTexture2D(GL_RGBA16F, width, height, zeros);
	rt.indirect = RT_CreateTexture2D(GL_RGBA16F, width, height, zeros);
	rt.extra    = RT_CreateTexture2D(GL_RGBA16F, width, height, zeros);
	Mem_Free(zeros);
	rt.width  = width;
	rt.height = height;
	/* Previous frame textures hold garbage now, so don't reproject from them */
	rt.havePrev = false;
}

static void RT_FreeWorldTexture(void) {
	RT_DeleteTexture(&rt.worldTex);
	rt.worldValid = false;
}

/* Uploads the entire world into a 3D integer texture, stored as (x, z, y) */
static void RT_UploadWorld(void) {
	int volume = World.Volume;
	cc_uint16* wide = NULL;
	int i;

	RT_FreeWorldTexture();
	if (!World.Blocks || !World.Loaded) return;
	rt.worldTried = true;

	if (World.Width > rt.max3DSize || World.Height > rt.max3DSize || World.Length > rt.max3DSize) {
		Chat_Add1("&cRay tracing: map is larger than the GPU's 3D texture limit (%i)", &rt.max3DSize);
		return;
	}

#ifdef EXTENDED_BLOCKS
	rt.worldWide = World.Blocks != World.Blocks2;
#else
	rt.worldWide = false;
#endif

	_glGenTextures(1, &rt.worldTex);
	_glActiveTexture(GL_TEXTURE0 + 1);
	_glBindTexture(GL_TEXTURE_3D, rt.worldTex);
	_glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	_glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	_glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	_glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	_glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
	_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

	/* World.Blocks is laid out as [y][z][x], which maps directly onto a WxLxH 3D texture */
	if (rt.worldWide) {
#ifdef EXTENDED_BLOCKS
		wide = (cc_uint16*)Mem_TryAlloc(volume, 2);
		if (!wide) { RT_FreeWorldTexture(); Chat_AddRaw("&cRay tracing: out of memory uploading world"); return; }
		for (i = 0; i < volume; i++) wide[i] = (cc_uint16)World_GetRawBlock(i);
#endif
		_glTexImage3D(GL_TEXTURE_3D, 0, GL_R16UI, World.Width, World.Length, World.Height, 0, GL_RED_INTEGER, GL_UNSIGNED_SHORT, wide);
		Mem_Free(wide);
	} else {
		_glTexImage3D(GL_TEXTURE_3D, 0, GL_R8UI,  World.Width, World.Length, World.Height, 0, GL_RED_INTEGER, GL_UNSIGNED_BYTE, World.Blocks);
	}
	_glActiveTexture(GL_TEXTURE0);

	if (_glGetError() != GL_NO_ERROR) {
		RT_FreeWorldTexture();
		Chat_AddRaw("&cRay tracing: failed to upload world to GPU");
		return;
	}
	rt.worldValid = true;
	rt.havePrev   = false;
}

/* Uploads the per-block table (shape, textures, tint, flags) to the GPU */
static void RT_UploadBlocks(void) {
	static struct RTBlockInfo infos[BLOCK_COUNT];
	int b, f, flags;
	PackedCol tint;

	for (b = 0; b < BLOCK_COUNT; b++) {
		struct RTBlockInfo* info = &infos[b];
		/* The rasteriser's render bounds are shifted slightly for liquids to avoid z-fighting, */
		/*  which doesn't matter for ray tracing, so use the actual bounds - just lowered for liquids */
		Vec3 minBB = Blocks.MinBB[b], maxBB = Blocks.MaxBB[b];
		if (Blocks.IsLiquid[b]) maxBB.y -= 1.5f / 16.0f;

		info->minBB[0] = minBB.x; info->minBB[1] = minBB.y; info->minBB[2] = minBB.z;
		info->maxBB[0] = maxBB.x; info->maxBB[1] = maxBB.y; info->maxBB[2] = maxBB.z;
		info->minBB[3] = (float)Blocks.Draw[b];

		flags = 0;
		if (Blocks.BlocksLight[b]) flags |= 1;
		info->maxBB[3] = (float)flags;

		for (f = 0; f < FACE_COUNT; f++) {
			cc_uint32 tex = Block_Tex(b, f);
			if (f < 4) info->texA[f] = tex; else info->texB[f - 4] = tex;
		}
		info->texB[2] = 0; info->texB[3] = 0;

		tint = Blocks.Tinted[b] ? Blocks.FogCol[b] : PACKEDCOL_WHITE;
		RT_ColToVec(info->tint, tint);
		info->tint[3] = Blocks.Brightness[b] ? 1.0f : 0.0f;
	}

	_glBindBuffer(GL_SHADER_STORAGE_BUFFER, rt.blocksSsbo);
	_glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(infos), infos, GL_STATIC_DRAW);
	_glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
	rt.blocksDirty = false;
}

static void RT_FreeResources(void) {
	if (!rt.initialised) return;
	RT_FreeScreenTextures();
	RT_FreeWorldTexture();

	if (rt.traceProg)     _glDeleteProgram(rt.traceProg);
	if (rt.temporalProg)  _glDeleteProgram(rt.temporalProg);
	if (rt.atrousProg)    _glDeleteProgram(rt.atrousProg);
	if (rt.compositeProg) _glDeleteProgram(rt.compositeProg);
	rt.traceProg = 0; rt.temporalProg = 0; rt.atrousProg = 0; rt.compositeProg = 0;

	if (rt.paramsUbo)  _glDeleteBuffers(1, &rt.paramsUbo);
	if (rt.blocksSsbo) _glDeleteBuffers(1, &rt.blocksSsbo);
	if (rt.vao)        _glDeleteVertexArrays(1, &rt.vao);
	rt.paramsUbo = 0; rt.blocksSsbo = 0; rt.vao = 0;
	rt.initialised = false;
	rt.triedInit   = false;
}

/* Loads OpenGL functions, checks GPU support, compiles shaders and creates buffers */
static cc_bool RT_TryInit(void) {
	RTint major = 0, minor = 0;
	int i;
	if (rt.initialised) return true;
	if (rt.triedInit)   return false;
	rt.triedInit = true;

	if (!Gfx.Created || Gfx.LostContext) { rt.triedInit = false; return false; }
	if (Gfx.BackendType != CC_GFX_BACKEND_GL1 && Gfx.BackendType != CC_GFX_BACKEND_GL2) {
		RT_Disable("requires the OpenGL graphics backend"); return false;
	}

	for (i = 0; i < Array_Elems(rt_glFuncs); i++) {
		*rt_glFuncs[i].symAddr = GLContext_GetAddress(rt_glFuncs[i].name);
		if (!*rt_glFuncs[i].symAddr) {
			Platform_Log1("Missing OpenGL function: %c", rt_glFuncs[i].name);
			RT_Disable("graphics driver lacks OpenGL 4.3 functions"); return false;
		}
	}

	_glGetIntegerv(GL_MAJOR_VERSION, &major);
	_glGetIntegerv(GL_MINOR_VERSION, &minor);
	if (major < 4 || (major == 4 && minor < 3)) {
		Platform_Log2("OpenGL %i.%i is too old for ray tracing", &major, &minor);
		RT_Disable("requires OpenGL 4.3 or newer"); return false;
	}
	_glGetIntegerv(GL_MAX_3D_TEXTURE_SIZE, &rt.max3DSize);
	/* clear any errors left over from the backend */
	while (_glGetError() != GL_NO_ERROR) { }

	if (!RT_CreatePrograms()) {
		rt.initialised = true; /* so RT_FreeResources cleans up partially created programs */
		RT_FreeResources();
		RT_Disable("shaders failed to compile (see client.log)"); return false;
	}

	_glGenBuffers(1, &rt.paramsUbo);
	_glBindBuffer(GL_UNIFORM_BUFFER, rt.paramsUbo);
	_glBufferData(GL_UNIFORM_BUFFER, sizeof(struct RTParams), NULL, GL_DYNAMIC_DRAW);
	_glBindBuffer(GL_UNIFORM_BUFFER, 0);
	_glGenBuffers(1, &rt.blocksSsbo);
	_glGenVertexArrays(1, &rt.vao);

	rt.initialised = true;
	rt.supported   = true;
	rt.blocksDirty = true;
	RT_UploadWorld();
	Platform_Log2("Ray tracing initialised (OpenGL %i.%i)", &major, &minor);
	return true;
}


/*########################################################################################################################*
*--------------------------------------------------------Rendering--------------------------------------------------------*
*#########################################################################################################################*/
static float RT_CalcBlendFactor(float x) {
	/* Same blend of fog and sky colour that EnvRenderer uses */
	float blend = -0.13f + 0.28f * ((float)Math_Log2(x) * 0.17329f);
	if (blend < 0.0f) blend = 0.0f;
	if (blend > 1.0f) blend = 1.0f;
	return blend;
}

static void RT_CalcFog(struct RTParams* p) {
	IVec3 coords;
	BlockID block;
	struct AABB blockBB;
	Vec3 pos;
	PackedCol col;
	float density = 0.0f;

	IVec3_Floor(&coords, &Camera.CurrentPos);
	block = World_SafeGetBlock(coords.x, coords.y, coords.z);
	IVec3_ToVec3(&pos, &coords);
	Vec3_Add(&blockBB.Min, &pos, &Blocks.MinBB[block]);
	Vec3_Add(&blockBB.Max, &pos, &Blocks.MaxBB[block]);

	if (Blocks.FogDensity[block] && AABB_ContainsPoint(&blockBB, &Camera.CurrentPos)) {
		density = Blocks.FogDensity[block];
		col     = Blocks.FogCol[block];
	} else {
		col = PackedCol_Lerp(Env.FogCol, Env.SkyCol, RT_CalcBlendFactor((float)Game_ViewDistance));
		if (Env.ExpFog) density = 4.60517018598809f / (Game_ViewDistance * 0.99f);
	}

	RT_ColToVec(p->fogCol, col);
	p->fogCol[3]    = density;
	p->fogParams[0] = (float)Game_ViewDistance;
}

static void RT_FillParams(struct RTParams* p, int flags) {
	struct Matrix viewProj, invViewProj;
	float len, fovy;

	Matrix_Mul(&viewProj, &Gfx.View, &Gfx.Projection);
	if (!RT_InvertMatrix(&invViewProj, &viewProj)) invViewProj = Matrix_Identity;
	RT_CopyMatrix(p->invViewProj, &invViewProj);
	RT_CopyMatrix(p->viewProj,    &viewProj);
	RT_CopyMatrix(p->prevViewProj, rt.havePrev ? &rt.prevViewProj : &viewProj);
	RT_CopyMatrix(p->view,        &Gfx.View);
	rt.prevViewProj = viewProj;
	rt.havePrev     = true;

	p->camPos[0] = Camera.CurrentPos.x; p->camPos[1] = Camera.CurrentPos.y; p->camPos[2] = Camera.CurrentPos.z;
	p->camPos[3] = (float)Game.Time;

	len = Math_SqrtF(rt_opts.sunX * rt_opts.sunX + 1.0f + rt_opts.sunZ * rt_opts.sunZ);
	p->sunDir[0] = rt_opts.sunX / len; p->sunDir[1] = 1.0f / len; p->sunDir[2] = rt_opts.sunZ / len;
	p->sunDir[3] = rt_opts.sunRadius;

	RT_ColToVec(p->sunCol,      Env.SunCol);
	RT_ColToVec(p->sunXSide,    Env.SunXSide);
	RT_ColToVec(p->sunZSide,    Env.SunZSide);
	RT_ColToVec(p->sunYMin,     Env.SunYMin);
	RT_ColToVec(p->shadowCol,   Env.ShadowCol);
	RT_ColToVec(p->shadowXSide, Env.ShadowXSide);
	RT_ColToVec(p->shadowZSide, Env.ShadowZSide);
	RT_ColToVec(p->shadowYMin,  Env.ShadowYMin);
	RT_ColToVec(p->skyCol,      Env.SkyCol);
	RT_CalcFog(p);
	p->fogParams[1] = rt_opts.ambient;
	p->fogParams[2] = rt_opts.emissive;
	p->fogParams[3] = (float)rt_opts.debug;

	p->worldSize[0] = World.Width; p->worldSize[1] = World.Height; p->worldSize[2] = World.Length; p->worldSize[3] = 0;
	p->screen[0] = rt.width; p->screen[1] = rt.height; p->screen[2] = rt.frame; p->screen[3] = flags;
	p->atlas[0] = Atlas1D.Shift; p->atlas[1] = Atlas1D.Mask; p->atlas[2] = Atlas1D.Count; p->atlas[3] = Atlas2D.TileSize;

	fovy = Camera.Fov * MATH_DEG2RAD;
	p->misc[0] = Atlas1D.InvTileSize;
	p->misc[1] = (float)Game_ViewDistance * 1.05f;
	p->misc[2] = rt_opts.giDistance;
	p->misc[3] = 2.0f * (float)(Math_Sin(fovy * 0.5f) / Math_Cos(fovy * 0.5f)) / (float)rt.height;
}

static void RT_BindImage(int unit, RTuint tex, RTenum access, RTenum format) {
	_glBindImageTexture(unit, tex, 0, 0, 0, access, format);
}

static void RT_BindSampler(int unit, RTenum target, RTuint tex) {
	_glActiveTexture(GL_TEXTURE0 + unit);
	_glBindTexture(target, tex);
}

cc_bool RayTracer_Active(void) {
	if (RayTracer_Mode == RT_MODE_OFF || !World.Loaded) return false;
	if (!rt.initialised && !RT_TryInit()) return false;
	if (!rt.supported) return false;

	/* e.g. ray tracing was enabled from the menu after the map had already loaded */
	if (!rt.worldValid && !rt.worldTried) RT_UploadWorld();
	return rt.worldValid;
}

void RayTracer_Render(float delta) {
	struct RTParams params;
	int cur, prev, flags, i, groupsX, groupsY;
	int width = Game.Width, height = Game.Height;
	RTuint atrousSrc, atrousDst, filtered;

	if (!RayTracer_Active()) return;
	if (Gfx.LostContext) return;
	if (Atlas1D.Count > RT_MAX_ATLASES) { RT_Disable("terrain atlas is split into too many textures"); return; }

	if (width != rt.width || height != rt.height) RT_CreateScreenTextures(width, height);
	if (rt.blocksDirty) RT_UploadBlocks();

	flags = 0;
	if (RayTracer_Mode >= RT_MODE_GI)   flags |= RT_FLAG_GI;
	if (RayTracer_Mode >= RT_MODE_FULL) flags |= RT_FLAG_REFLECTIONS | RT_FLAG_SOFT_SHADOWS;

	cur  = rt.frame & 1;
	prev = cur ^ 1;
	RT_FillParams(&params, flags);

	_glBindBuffer(GL_UNIFORM_BUFFER, rt.paramsUbo);
	_glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(params), &params);
	_glBindBuffer(GL_UNIFORM_BUFFER, 0);
	_glBindBufferBase(GL_UNIFORM_BUFFER, 0, rt.paramsUbo);
	_glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, rt.blocksSsbo);

	/* Make sure the terrain textures exist, then bind them for the shaders */
	for (i = 0; i < Atlas1D.Count; i++) Atlas1D_Bind(i);
	RT_BindSampler(1, GL_TEXTURE_3D, rt.worldTex);
	for (i = 0; i < RT_MAX_ATLASES; i++) {
		RT_BindSampler(2 + i, GL_TEXTURE_2D, i < Atlas1D.Count ? (RTuint)(cc_uintptr)Atlas1D.TexIds[i] : 0);
	}

	groupsX = (width  + 7) / 8;
	groupsY = (height + 7) / 8;

	/* 1) Trace primary rays and shade */
	_glUseProgram(rt.traceProg);
	RT_BindImage(0, rt.gbuf[cur],   GL_WRITE_ONLY, GL_RGBA32F);
	RT_BindImage(1, rt.normal[cur], GL_WRITE_ONLY, GL_RGBA32F);
	RT_BindImage(2, rt.albedo,      GL_WRITE_ONLY, GL_RGBA16F);
	RT_BindImage(3, rt.direct,      GL_WRITE_ONLY, GL_RGBA16F);
	RT_BindImage(4, rt.indirect,    GL_WRITE_ONLY, GL_RGBA16F);
	RT_BindImage(5, rt.extra,       GL_WRITE_ONLY, GL_RGBA16F);
	_glDispatchCompute(groupsX, groupsY, 1);
	_glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

	/* 2) Temporal accumulation against the previous frame */
	_glUseProgram(rt.temporalProg);
	RT_BindImage(0, rt.gbuf[cur],      GL_READ_ONLY,  GL_RGBA32F);
	RT_BindImage(1, rt.normal[cur],    GL_READ_ONLY,  GL_RGBA32F);
	RT_BindImage(2, rt.direct,         GL_READ_ONLY,  GL_RGBA16F);
	RT_BindImage(3, rt.indirect,       GL_READ_ONLY,  GL_RGBA16F);
	RT_BindImage(4, rt.gbuf[prev],     GL_READ_ONLY,  GL_RGBA32F);
	RT_BindImage(5, rt.normal[prev],   GL_READ_ONLY,  GL_RGBA32F);
	RT_BindImage(6, rt.accumDir[prev], GL_READ_ONLY,  GL_RGBA16F);
	RT_BindImage(7, rt.accumInd[prev], GL_READ_ONLY,  GL_RGBA16F);
	RT_BindImage(8, rt.accumDir[cur],  GL_WRITE_ONLY, GL_RGBA16F);
	RT_BindImage(9, rt.accumInd[cur],  GL_WRITE_ONLY, GL_RGBA16F);
	_glDispatchCompute(groupsX, groupsY, 1);
	_glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

	/* 3) Spatial denoising of the indirect light */
	filtered = rt.accumInd[cur];
	if (flags & RT_FLAG_GI) {
		_glUseProgram(rt.atrousProg);
		RT_BindImage(0, rt.gbuf[cur],   GL_READ_ONLY, GL_RGBA32F);
		RT_BindImage(1, rt.normal[cur], GL_READ_ONLY, GL_RGBA32F);
		atrousSrc = rt.accumInd[cur];
		atrousDst = rt.atrousTmp[0];

		for (i = 0; i < 3; i++) {
			_glUniform1i(rt.atrousStepLoc, 1 << i);
			RT_BindImage(2, atrousSrc, GL_READ_ONLY,  GL_RGBA16F);
			RT_BindImage(3, atrousDst, GL_WRITE_ONLY, GL_RGBA16F);
			_glDispatchCompute(groupsX, groupsY, 1);
			_glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);

			filtered  = atrousDst;
			atrousSrc = atrousDst;
			atrousDst = (atrousDst == rt.atrousTmp[0]) ? rt.atrousTmp[1] : rt.atrousTmp[0];
		}
	}
	_glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);

	/* 4) Composite into the framebuffer, writing depth for later rasterised drawing */
	_glUseProgram(rt.compositeProg);
	RT_BindSampler(6,  GL_TEXTURE_2D, rt.gbuf[cur]);
	RT_BindSampler(7,  GL_TEXTURE_2D, rt.albedo);
	RT_BindSampler(8,  GL_TEXTURE_2D, rt.accumDir[cur]);
	RT_BindSampler(9,  GL_TEXTURE_2D, filtered);
	RT_BindSampler(10, GL_TEXTURE_2D, rt.extra);
	RT_BindSampler(11, GL_TEXTURE_2D, rt.normal[cur]);
	_glBindVertexArray(rt.vao);
	_glDrawArrays(GL_TRIANGLES, 0, 3);
	_glBindVertexArray(0);

	/* Restore the state the graphics backend expects */
	_glActiveTexture(GL_TEXTURE0);
	_glUseProgram(0);
	GLBackend_RestoreProgram();
	rt.frame++;
}

void RayTracer_OnBlockChanged(int x, int y, int z, BlockID block) {
	cc_uint16 wide = (cc_uint16)block;
	cc_uint8  narrow = (cc_uint8)block;
	if (!rt.worldValid || !rt.initialised) return;

	_glActiveTexture(GL_TEXTURE0 + 1);
	_glBindTexture(GL_TEXTURE_3D, rt.worldTex);
	_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	if (rt.worldWide) {
		_glTexSubImage3D(GL_TEXTURE_3D, 0, x, z, y, 1, 1, 1, GL_RED_INTEGER, GL_UNSIGNED_SHORT, &wide);
	} else {
		_glTexSubImage3D(GL_TEXTURE_3D, 0, x, z, y, 1, 1, 1, GL_RED_INTEGER, GL_UNSIGNED_BYTE, &narrow);
	}
	_glActiveTexture(GL_TEXTURE0);
}

void RayTracer_SetMode(int mode) {
	cc_string str;
	if (mode < 0 || mode >= RT_MODE_COUNT) mode = RT_MODE_OFF;
	RayTracer_Mode = mode;
	str = String_FromReadonly(RayTracerMode_Names[mode]);
	Options_Set(OPT_RAYTRACING, &str);

	if (mode != RT_MODE_OFF && RayTracer_Active()) {
		Chat_Add1("&eRay tracing: %c", RayTracerMode_Names[mode]);
	}
}


/*########################################################################################################################*
*----------------------------------------------------Ray tracer component-------------------------------------------------*
*#########################################################################################################################*/
static void OnBlockDefChanged(void* obj) { rt.blocksDirty = true; }
static void OnContextLost(void* obj)     { RT_FreeResources(); }

static void OnInit(void) {
	RayTracer_Mode = Options_GetEnum(OPT_RAYTRACING, RT_MODE_OFF, RayTracerMode_Names, RT_MODE_COUNT);
	rt_opts.sunX       = Options_GetFloat("rt-sun-x",      -4.0f, 4.0f, 0.35f);
	rt_opts.sunZ       = Options_GetFloat("rt-sun-z",      -4.0f, 4.0f, 0.20f);
	rt_opts.sunRadius  = Options_GetFloat("rt-sun-radius",  0.0f, 0.5f, 0.04f);
	rt_opts.ambient    = Options_GetFloat("rt-ambient",     0.0f, 1.0f, 0.25f);
	rt_opts.emissive   = Options_GetFloat("rt-emissive",    0.0f, 8.0f, 2.0f);
	rt_opts.giDistance = Options_GetFloat("rt-gi-distance", 4.0f, 256.0f, 48.0f);
	rt_opts.debug      = Options_GetInt("rt-debug", 0, 16, 0);

	Event_Register_(&BlockEvents.BlockDefChanged, NULL, OnBlockDefChanged);
	Event_Register_(&GfxEvents.ContextLost,       NULL, OnContextLost);
}

static void OnFree(void) {
	RT_FreeResources();
}

static void OnNewMap(void) {
	if (rt.initialised) RT_FreeWorldTexture();
	rt.worldTried = false;
}

static void OnNewMapLoaded(void) {
	rt.worldTried = false;
	if (RayTracer_Mode == RT_MODE_OFF) return;
	if (!rt.initialised && !RT_TryInit()) return;
	RT_UploadWorld();
}

struct IGameComponent RayTracer_Component = {
	OnInit,         /* Init  */
	OnFree,         /* Free  */
	NULL,           /* Reset */
	OnNewMap,       /* OnNewMap */
	OnNewMapLoaded  /* OnNewMapLoaded */
};
#endif
