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
#include "Entity.h"
#include "Model.h"
#include "EnvRenderer.h"
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
#define GL_ONE                      1
#define GL_TRIANGLES                0x0004
#define GL_SRC_ALPHA                0x0302
#define GL_ONE_MINUS_SRC_ALPHA      0x0303
#define GL_BLEND                    0x0BE2
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
#define GL_STREAM_DRAW              0x88E0
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
	GL_FUNC(void,    glEnable,           (RTenum cap)) \
	GL_FUNC(void,    glDisable,          (RTenum cap)) \
	GL_FUNC(void,    glBlendFunc,        (RTenum sfactor, RTenum dfactor)) \
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
	cc_int32 window[4];
	float clouds[4];
};

/* Must match BlockInfo in rt_common.glsl (std430 layout) */
struct RTBlockInfo {
	float minBB[4];
	float maxBB[4];
	cc_uint32 texA[4];
	cc_uint32 texB[4];
	float tint[4];
};

/* Entity geometry recorded each frame for shadow rays */
#define RT_MAX_ENTITIES     64
#define RT_MAX_ENTITY_QUADS 8192

/* Must match EntityInfo in rt_common.glsl (std430 layout) */
struct RTEntity {
	float bbMin[4];
	float bbMax[4];
	cc_uint32 quadStart, quadCount, pad0, pad1;
};

static struct RTEntity rt_entities[RT_MAX_ENTITIES];
static float rt_quads[RT_MAX_ENTITY_QUADS * 16]; /* 4 vertices x vec4 per quad */
static struct {
	int count, quadCount, current;
	cc_bool localCaptured;
	struct Matrix transform;
	RTuint entitySsbo, quadSsbo;
} rt_ent = { 0, 0, -1 };

/* Light emitting (full bright) blocks, sampled explicitly for global illumination. */
/* Kept in 16x16x16 buckets so that selecting the ones near the camera (and keeping the */
/*  list in sync on block changes) doesn't scan every emitter in maps full of lava/lamps */
#define RT_MAX_EMITTERS   128
#define RT_EM_BUCKET_SHIFT 4
#define RT_EM_RADIUS       32.0f  /* must match the cutoff in emitterLight() */
struct RTEmitterBucket { cc_int32* items; int count, capacity; };
static struct {
	struct RTEmitterBucket* buckets;
	int bucketsX, bucketsY, bucketsZ;
	int count;              /* total emitters */
	cc_bool dirty, rebuild; /* selection needs redoing / list needs rebuilding from the world */
	Vec3 lastCam;
	int uploaded;
	RTuint ssbo;
} rt_em;

#define RT_FLAG_GI           1
#define RT_FLAG_REFLECTIONS  2
#define RT_FLAG_SOFT_SHADOWS 4

static struct {
	cc_bool triedInit, initialised, supported;
	cc_bool worldValid, worldWide, worldTried, blocksDirty, atlasWarned, frameDrawn;
	int width, height, frame;
	RTuint traceProg, temporalProg, atrousProg, compositeProg, waterProg;
	RTint  atrousStepLoc;
	RTuint paramsUbo, blocksSsbo, worldTex, coarseTex, vao;
	int coarseW, coarseH, coarseL;
	/* [0] and [1] alternate each frame, so the previous frame's data stays available */
	RTuint gbuf[2], normal[2], accumDir[2], accumInd[2];
	RTuint albedo, direct, indirect, extra, atrousTmp[2];
	struct Matrix prevViewProj;
	cc_bool havePrev;
	int max3DSize;
} rt;

static struct { float sunX, sunZ, sunRadius, ambient, emissive, giDistance, cloudShadow; int debug, scale, giRate; } rt_opts;


/*########################################################################################################################*
*--------------------------------------------------------Utilities--------------------------------------------------------*
*#########################################################################################################################*/
/* Writes a message to client.log (Platform_Log only goes to the console/debugger on Windows) */
static void RT_LogToFile(const char* title, const char* detail) {
	cc_string msg; char msgBuffer[3072];
	String_InitArray(msg, msgBuffer);
	String_Format2(&msg, "%c\n%c\n", title, detail);
	Logger_Log(&msg);
	Platform_Log2("%c %c", title, detail);
}

static void RT_Disable(const char* reason) {
	rt.supported = false;
	RayTracer_Mode = RT_MODE_OFF;
	Chat_Add1("&cRay tracing disabled: %c", reason);
	RT_LogToFile("Ray tracing disabled:", reason);
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
	RT_LogToFile("Failed to compile ray tracing shader:", log);
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
	RT_LogToFile("Failed to link ray tracing program:", log);
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

	vs = RT_CompileShader(GL_VERTEX_SHADER,   RT_COMPOSITE_VS, "composite vertex");
	if (!vs) return false;
	fs = RT_CompileShader(GL_FRAGMENT_SHADER, RT_WATER_FS, "water fragment");
	if (!fs) { _glDeleteShader(vs); return false; }
	rt.waterProg = RT_LinkProgram(vs, fs, "water");
	if (!rt.waterProg) return false;

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

static void RT_FreeEmitters(void);

static void RT_FreeWorldTexture(void) {
	RT_DeleteTexture(&rt.worldTex);
	RT_DeleteTexture(&rt.coarseTex);
	rt.worldValid = false;
	RT_FreeEmitters();
	rt_em.uploaded = 0;
}


/*########################################################################################################################*
*----------------------------------------------------Emitting blocks------------------------------------------------------*
*#########################################################################################################################*/
static cc_bool RT_IsEmitter(BlockID block) {
	/* any block with a brightness emits, including invisible (gas) light sources */
	return Blocks.Brightness[block] != 0;
}

static struct RTEmitterBucket* RT_EmitterBucket(int x, int y, int z) {
	int bx = x >> RT_EM_BUCKET_SHIFT, by = y >> RT_EM_BUCKET_SHIFT, bz = z >> RT_EM_BUCKET_SHIFT;
	return &rt_em.buckets[(by * rt_em.bucketsZ + bz) * rt_em.bucketsX + bx];
}

static void RT_FreeEmitters(void) {
	int i, total = rt_em.bucketsX * rt_em.bucketsY * rt_em.bucketsZ;
	if (rt_em.buckets) {
		for (i = 0; i < total; i++) Mem_Free(rt_em.buckets[i].items);
		Mem_Free(rt_em.buckets);
	}
	rt_em.buckets = NULL;
	rt_em.bucketsX = 0; rt_em.bucketsY = 0; rt_em.bucketsZ = 0;
	rt_em.count = 0;
	rt_em.dirty = true;
}

static int RT_FindEmitter(struct RTEmitterBucket* b, int index) {
	int i;
	for (i = 0; i < b->count; i++) {
		if (b->items[i] == index) return i;
	}
	return -1;
}

static void RT_AddEmitter(struct RTEmitterBucket* b, int index) {
	cc_int32* grown;
	int newCapacity;

	if (b->count >= b->capacity) {
		newCapacity = b->capacity ? b->capacity * 2 : 16;
		grown = (cc_int32*)Mem_TryRealloc(b->items, newCapacity, sizeof(cc_int32));
		if (!grown) return;
		b->items    = grown;
		b->capacity = newCapacity;
	}
	b->items[b->count++] = index;
	rt_em.count++;
	rt_em.dirty = true;
}

static void RT_RemoveEmitter(struct RTEmitterBucket* b, int at) {
	b->items[at] = b->items[--b->count];
	rt_em.count--;
	rt_em.dirty = true;
}

/* Scans the whole world for light emitting blocks */
static void RT_BuildEmitters(void) {
	int x, y, z, i = 0, total;
	RT_FreeEmitters();
	rt_em.rebuild = false;
	if (!World.Blocks || !World.Loaded) return;

	rt_em.bucketsX = (World.Width  + (1 << RT_EM_BUCKET_SHIFT) - 1) >> RT_EM_BUCKET_SHIFT;
	rt_em.bucketsY = (World.Height + (1 << RT_EM_BUCKET_SHIFT) - 1) >> RT_EM_BUCKET_SHIFT;
	rt_em.bucketsZ = (World.Length + (1 << RT_EM_BUCKET_SHIFT) - 1) >> RT_EM_BUCKET_SHIFT;
	total = rt_em.bucketsX * rt_em.bucketsY * rt_em.bucketsZ;
	rt_em.buckets = (struct RTEmitterBucket*)Mem_TryAllocCleared(total, sizeof(struct RTEmitterBucket));
	if (!rt_em.buckets) { rt_em.bucketsX = 0; rt_em.bucketsY = 0; rt_em.bucketsZ = 0; return; }

	for (y = 0; y < World.Height; y++) {
		for (z = 0; z < World.Length; z++) {
			for (x = 0; x < World.Width; x++, i++) {
				if (RT_IsEmitter(World_GetRawBlock(i))) RT_AddEmitter(RT_EmitterBucket(x, y, z), i);
			}
		}
	}
	rt_em.dirty = true;
}

/* Uploads the emitters nearest to the camera (all of them if there are few enough) */
static void RT_UploadEmitters(void) {
	static cc_int32 selected[RT_MAX_EMITTERS * 4];
	static float    selDist[RT_MAX_EMITTERS];
	Vec3 cam = Camera.CurrentPos, d;
	int i, k, n = 0, worst = 0, x, y, z, index;
	float dist;

	struct RTEmitterBucket* bucket;
	int bx, by, bz, bx1, by1, bz1, bx2, by2, bz2, reach;

	if (rt_em.rebuild) RT_BuildEmitters();

	Vec3_Sub(&d, &cam, &rt_em.lastCam);
	if (!rt_em.dirty && Vec3_LengthSquared(&d) < 4.0f) return;
	rt_em.dirty   = false;
	rt_em.lastCam = cam;
	if (!rt_em.buckets || !rt_em.count) { rt_em.uploaded = 0; return; }

	/* Only buckets within reach of the camera (the shader ignores emitters further away */
	/*  than RT_EM_RADIUS from a surface, and surfaces beyond a few blocks of the camera */
	/*  matter less), so huge lava lakes elsewhere in the map cost nothing */
	reach = ((int)RT_EM_RADIUS + 16 + (1 << RT_EM_BUCKET_SHIFT) - 1) >> RT_EM_BUCKET_SHIFT;
	bx = ((int)cam.x) >> RT_EM_BUCKET_SHIFT; by = ((int)cam.y) >> RT_EM_BUCKET_SHIFT; bz = ((int)cam.z) >> RT_EM_BUCKET_SHIFT;
	bx1 = max(bx - reach, 0); bx2 = min(bx + reach, rt_em.bucketsX - 1);
	by1 = max(by - reach, 0); by2 = min(by + reach, rt_em.bucketsY - 1);
	bz1 = max(bz - reach, 0); bz2 = min(bz + reach, rt_em.bucketsZ - 1);

	for (by = by1; by <= by2; by++)
	for (bz = bz1; bz <= bz2; bz++)
	for (bx = bx1; bx <= bx2; bx++)
	{
	bucket = &rt_em.buckets[(by * rt_em.bucketsZ + bz) * rt_em.bucketsX + bx];
	for (i = 0; i < bucket->count; i++) {
		index = bucket->items[i];
		World_Unpack(index, x, y, z);
		d.x = x + 0.5f - cam.x; d.y = y + 0.5f - cam.y; d.z = z + 0.5f - cam.z;
		dist = Vec3_LengthSquared(&d);

		if (n < RT_MAX_EMITTERS) {
			k = n++;
		} else {
			/* keep only the nearest RT_MAX_EMITTERS */
			if (dist >= selDist[worst]) continue;
			k = worst;
		}
		selDist[k] = dist;
		selected[k * 4 + 0] = x; selected[k * 4 + 1] = y; selected[k * 4 + 2] = z;
		/* w = block id, with the block's light level (0-15, larger of lamp/lava nibbles) in the upper bits */
		{
			BlockID b = World_GetRawBlock(index);
			int level = max(Blocks.Brightness[b] >> 4, Blocks.Brightness[b] & 15);
			selected[k * 4 + 3] = b | (level << 16);
		}

		if (n == RT_MAX_EMITTERS) {
			worst = 0;
			for (k = 1; k < n; k++) { if (selDist[k] > selDist[worst]) worst = k; }
		}
	}
	}

	rt_em.uploaded = n;
	if (n) {
		_glBindBuffer(GL_SHADER_STORAGE_BUFFER, rt_em.ssbo);
		_glBufferData(GL_SHADER_STORAGE_BUFFER, n * 4 * sizeof(cc_int32), selected, GL_STREAM_DRAW);
		_glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
	}
}

/* Builds the coarse occupancy grid: one byte per 8x8x8 region, 0 if the region is all air. */
/* Rays skip straight across empty regions instead of stepping through every block in them */
static cc_bool RT_BuildCoarseGrid(void) {
	int cw = (World.Width + 7) >> 3, ch = (World.Height + 7) >> 3, cl = (World.Length + 7) >> 3;
	cc_uint8* coarse = (cc_uint8*)Mem_TryAllocCleared(cw * ch * cl, 1);
	int x, y, z, i = 0;
	if (!coarse) return false;

	for (y = 0; y < World.Height; y++) {
		for (z = 0; z < World.Length; z++) {
			cc_uint8* row = &coarse[((y >> 3) * cl + (z >> 3)) * cw];
			for (x = 0; x < World.Width; x++, i++) {
				if (World_GetRawBlock(i)) row[x >> 3] = 1;
			}
		}
	}

	_glGenTextures(1, &rt.coarseTex);
	_glActiveTexture(GL_TEXTURE0 + 14);
	_glBindTexture(GL_TEXTURE_3D, rt.coarseTex);
	_glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	_glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	_glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	_glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	_glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
	_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	_glTexImage3D(GL_TEXTURE_3D, 0, GL_R8UI, cw, cl, ch, 0, GL_RED_INTEGER, GL_UNSIGNED_BYTE, coarse);
	_glActiveTexture(GL_TEXTURE0);
	Mem_Free(coarse);

	rt.coarseW = cw; rt.coarseH = ch; rt.coarseL = cl;
	return true;
}

/* Updates the coarse grid entry containing the given block after it changed */
static void RT_UpdateCoarseGrid(int x, int y, int z, BlockID block) {
	int cx = x >> 3, cy = y >> 3, cz = z >> 3;
	cc_uint8 value = 1;
	int bx, by, bz, x1, y1, z1, x2, y2, z2;

	if (block == 0) {
		/* Region is only empty if every other block in it is air too */
		x1 = cx << 3; y1 = cy << 3; z1 = cz << 3;
		x2 = min(x1 + 8, World.Width); y2 = min(y1 + 8, World.Height); z2 = min(z1 + 8, World.Length);
		value = 0;

		for (by = y1; by < y2 && !value; by++) {
			for (bz = z1; bz < z2 && !value; bz++) {
				for (bx = x1; bx < x2; bx++) {
					if (World_GetRawBlock(World_Pack(bx, by, bz))) { value = 1; break; }
				}
			}
		}
	}

	_glActiveTexture(GL_TEXTURE0 + 14);
	_glBindTexture(GL_TEXTURE_3D, rt.coarseTex);
	_glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	_glTexSubImage3D(GL_TEXTURE_3D, 0, cx, cz, cy, 1, 1, 1, GL_RED_INTEGER, GL_UNSIGNED_BYTE, &value);
	_glActiveTexture(GL_TEXTURE0);
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

	if (_glGetError() != GL_NO_ERROR || !RT_BuildCoarseGrid()) {
		RT_FreeWorldTexture();
		Chat_AddRaw("&cRay tracing: failed to upload world to GPU");
		return;
	}
	RT_BuildEmitters();
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
	if (rt.waterProg)     _glDeleteProgram(rt.waterProg);
	rt.traceProg = 0; rt.temporalProg = 0; rt.atrousProg = 0; rt.compositeProg = 0; rt.waterProg = 0;

	if (rt.paramsUbo)  _glDeleteBuffers(1, &rt.paramsUbo);
	if (rt.blocksSsbo) _glDeleteBuffers(1, &rt.blocksSsbo);
	if (rt.vao)        _glDeleteVertexArrays(1, &rt.vao);
	if (rt_ent.entitySsbo) _glDeleteBuffers(1, &rt_ent.entitySsbo);
	if (rt_ent.quadSsbo)   _glDeleteBuffers(1, &rt_ent.quadSsbo);
	if (rt_em.ssbo)        _glDeleteBuffers(1, &rt_em.ssbo);
	rt.paramsUbo = 0; rt.blocksSsbo = 0; rt.vao = 0;
	rt_ent.entitySsbo = 0; rt_ent.quadSsbo = 0; rt_em.ssbo = 0;
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
	_glGenBuffers(1, &rt_ent.entitySsbo);
	_glGenBuffers(1, &rt_ent.quadSsbo);
	_glGenBuffers(1, &rt_em.ssbo);
	_glGenVertexArrays(1, &rt.vao);

	rt.initialised = true;
	rt.supported   = true;
	rt.blocksDirty = true;
	RT_UploadWorld();
	Platform_Log2("Ray tracing initialised (OpenGL %i.%i)", &major, &minor);
	{
		cc_string msg; char msgBuffer[128];
		String_InitArray(msg, msgBuffer);
		String_Format2(&msg, "Ray tracing initialised (OpenGL %i.%i)\n", &major, &minor);
		Logger_Log(&msg);
	}
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

	p->worldSize[0] = World.Width; p->worldSize[1] = World.Height; p->worldSize[2] = World.Length; p->worldSize[3] = rt_opts.giRate;
	p->screen[0] = rt.width; p->screen[1] = rt.height; p->screen[2] = rt.frame; p->screen[3] = flags;
	p->atlas[0] = Atlas1D.Shift; p->atlas[1] = Atlas1D.Mask; p->atlas[2] = Atlas1D.Count; p->atlas[3] = Atlas2D.TileSize;

	fovy = Camera.Fov * MATH_DEG2RAD;
	p->misc[0] = Atlas1D.InvTileSize;
	p->misc[1] = (float)Game_ViewDistance * 1.05f;
	p->misc[2] = rt_opts.giDistance;
	p->misc[3] = 2.0f * (float)(Math_Sin(fovy * 0.5f) / Math_Cos(fovy * 0.5f)) / (float)rt.height;
	p->window[0] = Game.Width; p->window[1] = Game.Height; p->window[2] = rt_ent.count; p->window[3] = rt_em.uploaded;

	/* Clouds: same texture mapping and scrolling as EnvRenderer_RenderClouds */
	p->clouds[0] = (float)Env.CloudsHeight;
	p->clouds[1] = (float)(Game.Time / 2048.0f * 0.6f * Env.CloudsSpeed);
	p->clouds[2] = rt_opts.cloudShadow;
	p->clouds[3] = (EnvRenderer_CloudsTexture() && Env.CloudsHeight >= -2000 && rt_opts.cloudShadow > 0.0f) ? 1.0f : 0.0f;
}

/*########################################################################################################################*
*-----------------------------------------------------Entity geometry-----------------------------------------------------*
*#########################################################################################################################*/
static cc_bool RT_IsWorldEntity(struct Entity* e) {
	int i;
	for (i = 0; i < ENTITIES_MAX_COUNT; i++) {
		if (Entities.List[i] == e) return true;
	}
	return false;
}

void RayTracer_BeginEntity(struct Entity* e, const struct Matrix* transform) {
	struct RTEntity* ent;
	rt_ent.current = -1;
	if (!e || RayTracer_Mode == RT_MODE_OFF || !rt.worldValid) return;
	if (!RT_IsWorldEntity(e) || rt_ent.count >= RT_MAX_ENTITIES) return;

	rt_ent.current   = rt_ent.count++;
	rt_ent.transform = *transform;
	if (Entities.CurPlayer && e == &Entities.CurPlayer->Base) rt_ent.localCaptured = true;

	ent = &rt_entities[rt_ent.current];
	ent->bbMin[0] = ent->bbMin[1] = ent->bbMin[2] =  1e30f; ent->bbMin[3] = 0.0f;
	ent->bbMax[0] = ent->bbMax[1] = ent->bbMax[2] = -1e30f; ent->bbMax[3] = 0.0f;
	ent->quadStart = rt_ent.quadCount;
	ent->quadCount = 0;
	ent->pad0 = 0; ent->pad1 = 0;
}

void RayTracer_AddEntityVertices(const struct VertexTextured* vertices, int count) {
	struct RTEntity* ent;
	Vec3 in, out;
	float* dst;
	int i, k;
	if (rt_ent.current < 0) return;
	ent = &rt_entities[rt_ent.current];

	for (i = 0; i + 4 <= count; i += 4) {
		if (rt_ent.quadCount >= RT_MAX_ENTITY_QUADS) return;
		dst = &rt_quads[rt_ent.quadCount * 16];

		for (k = 0; k < 4; k++) {
			in.x = vertices[i + k].x; in.y = vertices[i + k].y; in.z = vertices[i + k].z;
			Vec3_Transform(&out, &in, &rt_ent.transform);
			dst[k * 4 + 0] = out.x; dst[k * 4 + 1] = out.y; dst[k * 4 + 2] = out.z; dst[k * 4 + 3] = 1.0f;

			ent->bbMin[0] = min(ent->bbMin[0], out.x); ent->bbMax[0] = max(ent->bbMax[0], out.x);
			ent->bbMin[1] = min(ent->bbMin[1], out.y); ent->bbMax[1] = max(ent->bbMax[1], out.y);
			ent->bbMin[2] = min(ent->bbMin[2], out.z); ent->bbMax[2] = max(ent->bbMax[2], out.z);
		}
		rt_ent.quadCount++;
		ent->quadCount++;
	}
}

/* Uploads this frame's entity geometry, then resets the recording for the next frame */
static void RT_UploadEntities(void) {
	struct Entity* e;
	int i, valid = 0;

	/* In first person the local player's model is never drawn, so record it here to get its */
	/*  shadow. Colour and depth writes are disabled so the model doesn't actually show up */
	if (!rt_ent.localCaptured && Entities.CurPlayer) {
		e = &Entities.CurPlayer->Base;
		if (e->Model) {
			Gfx_SetColorWrite(false, false, false, false);
			Gfx_SetDepthWrite(false);
			Model_Render(e->Model, e);
			Gfx_SetDepthWrite(true);
			Gfx_SetColorWrite(true, true, true, true);
		}
	}

	/* Drop entities that produced no geometry */
	for (i = 0; i < rt_ent.count; i++) {
		if (!rt_entities[i].quadCount) continue;
		/* small margin so shadow rays starting exactly on the bounds still test the quads */
		rt_entities[i].bbMin[0] -= 0.01f; rt_entities[i].bbMin[1] -= 0.01f; rt_entities[i].bbMin[2] -= 0.01f;
		rt_entities[i].bbMax[0] += 0.01f; rt_entities[i].bbMax[1] += 0.01f; rt_entities[i].bbMax[2] += 0.01f;
		rt_entities[valid++] = rt_entities[i];
	}
	rt_ent.count = valid;

	if (rt_ent.count) {
		_glBindBuffer(GL_SHADER_STORAGE_BUFFER, rt_ent.entitySsbo);
		_glBufferData(GL_SHADER_STORAGE_BUFFER, rt_ent.count * sizeof(struct RTEntity), rt_entities, GL_STREAM_DRAW);
		_glBindBuffer(GL_SHADER_STORAGE_BUFFER, rt_ent.quadSsbo);
		_glBufferData(GL_SHADER_STORAGE_BUFFER, rt_ent.quadCount * 16 * sizeof(float), rt_quads, GL_STREAM_DRAW);
		_glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
	}
	_glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, rt_ent.entitySsbo);
	_glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 3, rt_ent.quadSsbo);
}

static void RT_ResetEntities(void) {
	rt_ent.count = 0; rt_ent.quadCount = 0; rt_ent.current = -1;
	rt_ent.localCaptured = false;
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

	if (Atlas1D.Count > RT_MAX_ATLASES) {
		/* Only happens on GPUs with a tiny max texture size - draw normally until the atlas changes */
		if (!rt.atlasWarned) Chat_AddRaw("&cRay tracing: terrain atlas is split into too many textures for this GPU");
		rt.atlasWarned = true;
		return false;
	}

	/* e.g. ray tracing was enabled from the menu after the map had already loaded */
	if (!rt.worldValid && !rt.worldTried) RT_UploadWorld();
	return rt.worldValid;
}

void RayTracer_Render(float delta) {
	struct RTParams params;
	int cur, prev, flags, i, groupsX, groupsY;
	int width  = max(1, Game.Width  * rt_opts.scale / 100);
	int height = max(1, Game.Height * rt_opts.scale / 100);
	RTuint atrousSrc, atrousDst, filtered;

	if (!RayTracer_Active()) return;
	if (Gfx.LostContext) return;

	if (width != rt.width || height != rt.height) RT_CreateScreenTextures(width, height);
	if (rt.blocksDirty) RT_UploadBlocks();

	flags = 0;
	if (RayTracer_Mode >= RT_MODE_GI)   flags |= RT_FLAG_GI;
	if (RayTracer_Mode >= RT_MODE_FULL) flags |= RT_FLAG_REFLECTIONS | RT_FLAG_SOFT_SHADOWS;

	cur  = rt.frame & 1;
	prev = cur ^ 1;
	RT_UploadEntities();
	RT_UploadEmitters();
	_glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, rt_em.ssbo);
	RT_FillParams(&params, flags);
	RT_ResetEntities();

	_glBindBuffer(GL_UNIFORM_BUFFER, rt.paramsUbo);
	_glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(params), &params);
	_glBindBuffer(GL_UNIFORM_BUFFER, 0);
	_glBindBufferBase(GL_UNIFORM_BUFFER, 0, rt.paramsUbo);
	_glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, rt.blocksSsbo);

	/* Make sure the terrain textures exist, then bind them for the shaders */
	for (i = 0; i < Atlas1D.Count; i++) Atlas1D_Bind(i);
	RT_BindSampler(1,  GL_TEXTURE_3D, rt.worldTex);
	RT_BindSampler(14, GL_TEXTURE_3D, rt.coarseTex);
	RT_BindSampler(15, GL_TEXTURE_2D, (RTuint)(cc_uintptr)EnvRenderer_CloudsTexture());
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
	/* (inputs go through texture units, as only 8 image units are guaranteed) */
	_glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
	_glUseProgram(rt.temporalProg);
	RT_BindSampler(6,  GL_TEXTURE_2D, rt.gbuf[cur]);
	RT_BindSampler(7,  GL_TEXTURE_2D, rt.normal[cur]);
	RT_BindSampler(8,  GL_TEXTURE_2D, rt.direct);
	RT_BindSampler(9,  GL_TEXTURE_2D, rt.indirect);
	RT_BindSampler(10, GL_TEXTURE_2D, rt.gbuf[prev]);
	RT_BindSampler(11, GL_TEXTURE_2D, rt.normal[prev]);
	RT_BindSampler(12, GL_TEXTURE_2D, rt.accumDir[prev]);
	RT_BindSampler(13, GL_TEXTURE_2D, rt.accumInd[prev]);
	RT_BindImage(0, rt.accumDir[cur], GL_WRITE_ONLY, GL_RGBA16F);
	RT_BindImage(1, rt.accumInd[cur], GL_WRITE_ONLY, GL_RGBA16F);
	_glDispatchCompute(groupsX, groupsY, 1);
	_glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

	/* 3) Spatial denoising of the indirect light */
	filtered = rt.accumInd[cur];
	if (flags & RT_FLAG_GI) {
		_glUseProgram(rt.atrousProg);
		RT_BindSampler(6, GL_TEXTURE_2D, rt.gbuf[cur]);
		RT_BindSampler(7, GL_TEXTURE_2D, rt.normal[cur]);
		atrousSrc = rt.accumInd[cur];
		atrousDst = rt.atrousTmp[0];

		for (i = 0; i < 3; i++) {
			_glUniform1i(rt.atrousStepLoc, 1 << i);
			RT_BindSampler(8, GL_TEXTURE_2D, atrousSrc);
			RT_BindImage(0, atrousDst, GL_WRITE_ONLY, GL_RGBA16F);
			_glDispatchCompute(groupsX, groupsY, 1);
			_glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

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
	rt.frameDrawn = true;
}

void RayTracer_RenderTranslucent(void) {
	if (!rt.frameDrawn || Gfx.LostContext) return;
	rt.frameDrawn = false;

	/* Blend the water layer over the opaque world and the entities drawn since, */
	/*  with premultiplied alpha (the reflection isn't scaled by the water's alpha) */
	_glEnable(GL_BLEND);
	_glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	_glUseProgram(rt.waterProg);
	_glBindBufferBase(GL_UNIFORM_BUFFER, 0, rt.paramsUbo);
	RT_BindSampler(6, GL_TEXTURE_2D, rt.albedo);
	RT_BindSampler(7, GL_TEXTURE_2D, rt.extra);
	_glBindVertexArray(rt.vao);
	_glDrawArrays(GL_TRIANGLES, 0, 3);
	_glBindVertexArray(0);

	_glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	_glDisable(GL_BLEND);
	_glActiveTexture(GL_TEXTURE0);
	_glUseProgram(0);
	GLBackend_RestoreProgram();
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
	RT_UpdateCoarseGrid(x, y, z, block);

	/* keep the emitter list in sync */
	if (rt_em.buckets) {
		struct RTEmitterBucket* b = RT_EmitterBucket(x, y, z);
		int index = World_Pack(x, y, z);
		int at    = RT_FindEmitter(b, index);
		if (RT_IsEmitter(block)) {
			if (at < 0) RT_AddEmitter(b, index);
		} else if (at >= 0) {
			RT_RemoveEmitter(b, at);
		}
	}
}

const char* const RayTracerScale_Names[RT_SCALE_COUNT]   = { "100%", "75%", "50%" };
const char* const RayTracerGIRate_Names[RT_GIRATE_COUNT] = { "Half", "Full" };
static const int rt_scalePercent[RT_SCALE_COUNT] = { 100, 75, 50 };

int RayTracer_GetScaleIndex(void) {
	int i;
	for (i = 0; i < RT_SCALE_COUNT; i++) { if (rt_scalePercent[i] == rt_opts.scale) return i; }
	return 0;
}

void RayTracer_SetScaleIndex(int index) {
	if (index < 0 || index >= RT_SCALE_COUNT) index = 0;
	rt_opts.scale = rt_scalePercent[index];
	Options_SetInt("rt-scale", rt_opts.scale);
}

int  RayTracer_GetGIRateIndex(void)      { return rt_opts.giRate == 2 ? 0 : 1; }
void RayTracer_SetGIRateIndex(int index) {
	rt_opts.giRate = (index == 0) ? 2 : 1;
	Options_SetInt("rt-gi-rate", rt_opts.giRate);
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
static void OnBlockDefChanged(void* obj) { rt.blocksDirty = true; rt_em.rebuild = true; }
static void OnAtlasChanged(void* obj)    { rt.atlasWarned = false; }
static void OnContextLost(void* obj)     { RT_FreeResources(); }

static void OnInit(void) {
	RayTracer_Mode = Options_GetEnum(OPT_RAYTRACING, RT_MODE_OFF, RayTracerMode_Names, RT_MODE_COUNT);
	rt_opts.sunX       = Options_GetFloat("rt-sun-x",      -4.0f, 4.0f, 0.35f);
	rt_opts.sunZ       = Options_GetFloat("rt-sun-z",      -4.0f, 4.0f, 0.20f);
	rt_opts.sunRadius  = Options_GetFloat("rt-sun-radius",  0.0f, 0.5f, 0.04f);
	rt_opts.ambient    = Options_GetFloat("rt-ambient",     0.0f, 1.0f, 0.25f);
	rt_opts.emissive   = Options_GetFloat("rt-emissive",    0.0f, 16.0f, 3.0f);
	rt_opts.giDistance = Options_GetFloat("rt-gi-distance", 4.0f, 256.0f, 48.0f);
	rt_opts.debug      = Options_GetInt("rt-debug", 0, 16, 0);
	rt_opts.scale      = Options_GetInt("rt-scale", 25, 100, 100);
	rt_opts.cloudShadow = Options_GetFloat("rt-cloud-shadow", 0.0f, 1.0f, 0.6f);
	rt_opts.giRate     = Options_GetInt("rt-gi-rate", 1, 2, 2);

	Event_Register_(&BlockEvents.BlockDefChanged, NULL, OnBlockDefChanged);
	Event_Register_(&GfxEvents.ContextLost,       NULL, OnContextLost);
	Event_Register_(&TextureEvents.AtlasChanged,  NULL, OnAtlasChanged);
}

static void OnFree(void) {
	RT_FreeResources();
	RT_FreeEmitters();
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
