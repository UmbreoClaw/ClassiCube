/* glTF 2.0 loader and pose evaluator for the BlenderModel plugin.
 *
 * Loads .glb and .gltf files (external .bin/.png or base64 data URIs) with
 * skinned or rigid meshes and their animation clips. Skinning and animation
 * sampling run on the CPU; the plugin uploads the posed mesh every frame.
 *
 * Matrices are stored column-major as 16 floats, the same layout glTF uses:
 * translation lives in elements 12..14 and a point transforms as M * p.
 *
 * Licensed under BSD-3, same as ClassiCube.
 */
#ifndef BM_GLTF_H
#define BM_GLTF_H
#include "src/String_.h"
#include "src/Vectors.h"
#include "src/Bitmap.h"

#define GLTF_NAME_SIZE 64

struct GltfCorner {
	Vec3 pos, nrm;
	float u, v;
	cc_uint16 joints[4]; /* palette entries */
	float weights[4];
};

struct GltfNode {
	char name[GLTF_NAME_SIZE];
	int nameLen;
	int parent;          /* -1 for roots */
	Vec3 t;              /* rest pose translation */
	float r[4];          /* rest pose rotation quaternion x,y,z,w */
	Vec3 s;              /* rest pose scale */
	float matrix[16];    /* used instead of TRS when hasMatrix */
	cc_bool hasMatrix;
	int palette;         /* palette entry driven by this node (rigid meshes), or -1 */
};

/* One animated property of one node */
struct GltfChannel {
	int node;
	cc_uint8 path;   /* GLTF_PATH_* */
	cc_uint8 interp; /* GLTF_INTERP_* */
	int numKeys;
	float* times;    /* numKeys */
	float* values;   /* numKeys * comps (3 for T/S, 4 for R) */
};
#define GLTF_PATH_TRANSLATION 0
#define GLTF_PATH_ROTATION    1
#define GLTF_PATH_SCALE       2
#define GLTF_INTERP_LINEAR    0
#define GLTF_INTERP_STEP      1

struct GltfClip {
	char name[GLTF_NAME_SIZE];
	int nameLen;
	float duration;
	int firstChannel, numChannels;
};

struct GltfModel {
	struct GltfCorner* corners; int numCorners;
	struct GltfNode*   nodes;   int numNodes;
	int* nodeOrder; /* node indices with every parent before its children */
	/* Matrix palette layout: entry i is driven by paletteNode[i]; */
	/* paletteInvBind[i] (16 floats) is identity for rigid mesh nodes */
	int*   paletteNode;
	float* paletteInvBind;
	int numPalette;
	struct GltfChannel* channels; int numChannels;
	struct GltfClip*    clips;    int numClips;

	/* Applied above every root node (axis flip, user scale) */
	float rootXform[16];

	/* Base colour texture of the first textured material, scan0 NULL if none */
	struct Bitmap texture;
	/* Optional metadata from "extras" (cc_eye_height, cc_size) */
	cc_bool hasEye, hasSize;
	float eyeY;
	Vec3 size;
};

/* Working memory for one evaluated pose */
struct GltfPose {
	float* local;   /* numNodes * 16 */
	float* global;  /* numNodes * 16 */
	float* palette; /* numPalette * 16 */
};

/* Implemented by the plugin: reports a loading problem to the player */
void Gltf_LogMsg(const char* msg);

/* Loads a .glb or .gltf file. scale and flipZ are folded into rootXform */
cc_bool Gltf_Load(struct GltfModel* m, const cc_string* path, float scale, cc_bool flipZ);
void    Gltf_Free(struct GltfModel* m);
void    Gltf_SetRootXform(struct GltfModel* m, float scale, cc_bool flipZ);

cc_bool Gltf_Pose_Alloc(const struct GltfModel* m, struct GltfPose* pose);
void    Gltf_Pose_Free(struct GltfPose* pose);

/* Evaluates the pose for clip (or the rest pose when clip is -1) at time seconds. */
/* headNode >= 0 additionally turns that node by headPitch/headYaw (radians) in its local space */
void Gltf_EvalPose(const struct GltfModel* m, struct GltfPose* pose, int clip, float time,
                   int headNode, float headPitch, float headYaw);
/* Applies the pose to one corner */
void Gltf_SkinCorner(const struct GltfModel* m, const struct GltfPose* pose, const struct GltfCorner* c, Vec3* pos, Vec3* nrm);
/* World space position of a node in the given pose */
Vec3 Gltf_NodePosition(const struct GltfModel* m, const struct GltfPose* pose, int node);

/* Index of the first clip whose name caselessly contains any of the keywords, or -1 */
int Gltf_FindClip(const struct GltfModel* m, const char* const* keywords, int numKeywords);
/* Index of the first clip whose name caselessly equals name, or -1 */
int Gltf_FindClipByName(const struct GltfModel* m, const cc_string* name);
cc_string Gltf_NodeName(const struct GltfModel* m, int node);
cc_string Gltf_ClipName(const struct GltfModel* m, int clip);

/* 4x4 column-major helpers, exposed for the plugin and tests */
void Gltf_M4_Identity(float* m);
void Gltf_M4_Mul(float* out, const float* a, const float* b);
Vec3 Gltf_M4_TransformPoint(const float* m, Vec3 p);
Vec3 Gltf_M4_TransformDir(const float* m, Vec3 d);
#endif
