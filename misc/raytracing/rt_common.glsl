/* Shared code for the ClassiCube GPU voxel path tracer (see src/RayTracer.c)
   The world is traced directly as a voxel grid using a 3D DDA walk, so no
   acceleration structure has to be built or maintained on block changes.
   Copyright 2014-2025 ClassiCube | Licensed under BSD-3 */

/* Must match struct RTParams in RayTracer.c (std140 layout, vec4/mat4 only) */
layout(std140, binding = 0) uniform Params {
	mat4  invViewProj;  /* clip space -> world space */
	mat4  viewProj;     /* world space -> clip space (current frame) */
	mat4  prevViewProj; /* world space -> clip space (previous frame) */
	mat4  view;         /* world space -> eye space */
	vec4  camPos;       /* xyz = camera position, w = time in seconds */
	vec4  sunDir;       /* xyz = direction towards the sun, w = tan(sun angular radius) */
	vec4  sunCol, sunXSide, sunZSide, sunYMin;
	vec4  shadowCol, shadowXSide, shadowZSide, shadowYMin;
	vec4  skyCol;       /* rgb = colour seen by rays that escape the world */
	vec4  fogCol;       /* rgb = fog colour, w = exp fog density (0 = linear fog) */
	vec4  fogParams;    /* x = linear fog end, y = minimum ambient, z = emissive strength, w = unused */
	ivec4 worldSize;    /* xyz = world dimensions */
	ivec4 screen;       /* xy = render size, z = frame index, w = flags */
	ivec4 atlas;        /* x = atlas1D shift, y = atlas1D mask, z = atlas count, w = tile size in pixels */
	vec4  misc;         /* x = atlas1D V per tile, y = max primary distance, z = GI ray distance, w = pixel angular size */
	ivec4 window;       /* xy = window size in pixels (render size may be smaller, see rt-scale) */
};

#define FLAG_GI          1
#define FLAG_REFLECTIONS 2
#define FLAG_SOFT_SHADOWS 4

/* Must match struct RTBlockInfo in RayTracer.c */
struct BlockInfo {
	vec4  minBB;  /* xyz = min bounds, w = draw type */
	vec4  maxBB;  /* xyz = max bounds, w = flags */
	uvec4 texA;   /* tile ids for faces XMIN, XMAX, ZMIN, ZMAX */
	uvec4 texB;   /* tile ids for faces YMIN, YMAX, then unused */
	vec4  tint;   /* rgb = colour multiplier, w = 1 if block is full bright */
};

layout(std430, binding = 1) readonly buffer BlockTable { BlockInfo blocks[]; };

layout(binding = 1) uniform usampler3D worldTex;
layout(binding = 2) uniform sampler2D atlas0;
layout(binding = 3) uniform sampler2D atlas1;
layout(binding = 4) uniform sampler2D atlas2;
layout(binding = 5) uniform sampler2D atlas3;
/* 1 byte per 8x8x8 region of the world: 0 if the region is entirely air, so rays can skip it */
layout(binding = 12) uniform usampler3D coarseTex;
#define COARSE_SHIFT 3
#define COARSE_SIZE  8.0

#define DRAW_OPAQUE            0
#define DRAW_TRANSPARENT       1
#define DRAW_TRANSPARENT_THICK 2
#define DRAW_TRANSLUCENT       3
#define DRAW_GAS               4
#define DRAW_SPRITE            5

#define BLOCKFLAG_BLOCKS_LIGHT 1

#define FACE_XMIN 0
#define FACE_XMAX 1
#define FACE_ZMIN 2
#define FACE_ZMAX 3
#define FACE_YMIN 4
#define FACE_YMAX 5

#define MAX_DDA_STEPS 1024

struct Hit {
	vec3  pos;     /* world space hit position */
	vec3  normal;  /* geometric normal facing the ray */
	vec2  uv;      /* texture coordinates within the tile */
	float t;       /* distance along the ray */
	uint  block;   /* block id that was hit */
	uint  face;    /* FACE_ constant of hit face */
	uint  draw;    /* draw type of hit block */
};

int  drawType(uint block)  { return int(blocks[block].minBB.w); }
bool blocksLight(uint block) { return (int(blocks[block].maxBB.w) & BLOCKFLAG_BLOCKS_LIGHT) != 0; }
bool fullBright(uint block) { return blocks[block].tint.w > 0.5; }

uint tileFor(uint block, uint face) {
	if (face < 4u) return blocks[block].texA[face];
	return blocks[block].texB[face - 4u];
}

uint coarseAt(ivec3 cc) {
	return texelFetch(coarseTex, ivec3(cc.x, cc.z, cc.y), 0).r;
}

uint blockAt(ivec3 p) {
	if (any(lessThan(p, ivec3(0))) || any(greaterThanEqual(p, worldSize.xyz))) return 0u;
	/* world texture is stored as (x, z, y) so that height is the depth axis */
	return texelFetch(worldTex, ivec3(p.x, p.z, p.y), 0).r;
}

vec4 sampleTile(uint texLoc, vec2 uv, float lod) {
	uint idx = texLoc >> uint(atlas.x);
	uint row = texLoc &  uint(atlas.y);
	/* keep slightly inside the tile, so mipmaps/filtering don't bleed neighbouring tiles */
	float pad = 0.5 / float(atlas.w);
	uv = clamp(uv, vec2(pad), vec2(1.0 - pad));
	vec2 st = vec2(uv.x, (float(row) + uv.y) * misc.x);

	if (idx == 0u) return textureLod(atlas0, st, lod);
	if (idx == 1u) return textureLod(atlas1, st, lod);
	if (idx == 2u) return textureLod(atlas2, st, lod);
	return textureLod(atlas3, st, lod);
}

/* Texture coordinates for a point on the given face, matching Drawer.c orientation */
vec2 faceUV(uint face, vec3 local) {
	if (face == FACE_XMIN) return vec2(local.z,       1.0 - local.y);
	if (face == FACE_XMAX) return vec2(1.0 - local.z, 1.0 - local.y);
	if (face == FACE_ZMIN) return vec2(1.0 - local.x, 1.0 - local.y);
	if (face == FACE_ZMAX) return vec2(local.x,       1.0 - local.y);
	return vec2(local.x, local.z); /* YMIN / YMAX */
}

/* Whether a texel of the given tile should be treated as solid for alpha tested blocks */
bool texelSolid(uint block, uint face, vec2 uv) {
	return sampleTile(tileFor(block, face), uv, 0.0).a > 0.5;
}

/* Intersects the ray with the geometry of one block inside cell c.
   tMin/tMax bound the ray segment that lies inside the cell */
bool hitCell(ivec3 c, uint block, vec3 ro, vec3 rd, vec3 invRd, float tMin, float tMax, bool shadowRay, bool skipTranslucent, inout Hit h) {
	BlockInfo bi = blocks[block];
	int  draw = int(bi.minBB.w);
	vec3 base = vec3(c);

	if (draw == DRAW_GAS) return false;
	if (shadowRay && (int(bi.maxBB.w) & BLOCKFLAG_BLOCKS_LIGHT) == 0) return false;
	if (draw == DRAW_TRANSLUCENT && skipTranslucent) return false;

	if (draw == DRAW_SPRITE) {
		/* Two crossed quads along the diagonals of the cell */
		float bestT = 1e30;
		vec2  bestUV = vec2(0.0);
		vec3  bestN  = vec3(0.0);
		vec3  o = ro - base;

		for (int i = 0; i < 2; i++) {
			/* plane i = 0: x == z    plane i = 1: x == 1 - z */
			vec3 n = (i == 0) ? vec3(1.0, 0.0, -1.0) : vec3(1.0, 0.0, 1.0);
			float d = (i == 0) ? 0.0 : 1.0;
			float denom = dot(rd, n);
			if (abs(denom) < 1e-6) continue;

			float t = (d - dot(o, n)) / denom;
			if (t < tMin - 1e-4 || t > tMax + 1e-4 || t >= bestT) continue;

			vec3 p = o + rd * t;
			if (p.y < 0.0 || p.y > 1.0 || p.x < 0.0 || p.x > 1.0 || p.z < 0.0 || p.z > 1.0) continue;

			vec2 uv = vec2(p.x, 1.0 - p.y);
			if (!texelSolid(block, FACE_XMIN, uv)) continue;

			bestT  = t;
			bestUV = uv;
			bestN  = normalize(n) * ((denom > 0.0) ? -1.0 : 1.0);
		}
		if (bestT >= 1e30) return false;

		h.t = bestT; h.pos = ro + rd * bestT; h.normal = bestN; h.uv = bestUV;
		h.block = block; h.face = FACE_XMIN; h.draw = uint(draw);
		return true;
	}

	vec3 bmin = base + bi.minBB.xyz;
	vec3 bmax = base + bi.maxBB.xyz;
	vec3 t1 = (bmin - ro) * invRd;
	vec3 t2 = (bmax - ro) * invRd;
	vec3 tn = min(t1, t2);
	vec3 tf = max(t1, t2);
	float tEnter = max(max(tn.x, tn.y), tn.z);
	float tExit  = min(min(tf.x, tf.y), tf.z);

	/* small tolerance, since the DDA's accumulated cell boundaries drift slightly from the exact slab distances */
	if (tExit < tEnter || tExit < tMin - 1e-4 || tEnter > tMax + 1e-4) return false;
	/* ray starts inside this block - look through it (like backface culling does) */
	if (tEnter < 0.0) return false;

	vec3 normal; uint face;
	if (tEnter == tn.x) {
		normal = vec3(rd.x > 0.0 ? -1.0 : 1.0, 0.0, 0.0);
		face   = rd.x > 0.0 ? FACE_XMIN : FACE_XMAX;
	} else if (tEnter == tn.y) {
		normal = vec3(0.0, rd.y > 0.0 ? -1.0 : 1.0, 0.0);
		face   = rd.y > 0.0 ? FACE_YMIN : FACE_YMAX;
	} else {
		normal = vec3(0.0, 0.0, rd.z > 0.0 ? -1.0 : 1.0);
		face   = rd.z > 0.0 ? FACE_ZMIN : FACE_ZMAX;
	}

	/* Faces shared with an identical neighbouring block aren't drawn (e.g. between two water */
	/*  blocks, which are slightly lower than a full cell so rays can slip between them) */
	if (draw != DRAW_TRANSPARENT_THICK && blockAt(c + ivec3(normal)) == block) return false;

	vec3 local = clamp(ro + rd * tEnter - base, 0.0, 1.0);
	vec2 uv    = faceUV(face, local);

	if (draw == DRAW_TRANSPARENT || draw == DRAW_TRANSPARENT_THICK) {
		if (!texelSolid(block, face, uv)) return false;
	}

	h.t = tEnter; h.pos = ro + rd * tEnter; h.normal = normal; h.uv = uv;
	h.block = block; h.face = face; h.draw = uint(draw);
	return true;
}

/* Walks the voxel grid along the ray. Returns true if any block geometry was hit within maxT */
bool traceRay(vec3 ro, vec3 rd, float maxT, bool shadowRay, bool skipTranslucent, out Hit h) {
	vec3  wsize = vec3(worldSize.xyz);
	float tOffset = 0.0;
	h.t = -1.0; h.block = 0u;

	/* Clip the ray to the world bounds first */
	vec3 invRd = 1.0 / rd;
	if (any(lessThan(ro, vec3(0.0))) || any(greaterThanEqual(ro, wsize))) {
		vec3 t1 = (vec3(0.0) - ro) * invRd;
		vec3 t2 = (wsize      - ro) * invRd;
		vec3 tn = min(t1, t2);
		vec3 tf = max(t1, t2);
		float tEnter = max(max(tn.x, tn.y), tn.z);
		float tExit  = min(min(tf.x, tf.y), tf.z);
		if (tExit < tEnter || tExit < 0.0 || tEnter > maxT) return false;

		tOffset = max(tEnter, 0.0) + 1e-4;
		ro     += rd * tOffset;
		maxT   -= tOffset;
	}

	ivec3 c   = ivec3(floor(ro));
	c = clamp(c, ivec3(0), worldSize.xyz - 1);
	ivec3 stp = ivec3(sign(rd));
	vec3  side = vec3(greaterThan(rd, vec3(0.0)));
	float t = 0.0;
	ivec3 lastCoarse = ivec3(-1);
	bool  coarseEmpty = false;

	for (int i = 0; i < MAX_DDA_STEPS; i++) {
		ivec3 cc = c >> COARSE_SHIFT;
		if (cc != lastCoarse) {
			lastCoarse  = cc;
			coarseEmpty = coarseAt(cc) == 0u;
		}

		if (coarseEmpty) {
			/* Whole 8x8x8 region is air - jump straight to where the ray leaves it */
			vec3 cmin = vec3(cc << COARSE_SHIFT);
			vec3 tf = max((cmin - ro) * invRd, (cmin + COARSE_SIZE - ro) * invRd);
			if (rd.x == 0.0) tf.x = 1e30;
			if (rd.y == 0.0) tf.y = 1e30;
			if (rd.z == 0.0) tf.z = 1e30;
			t = min(min(tf.x, tf.y), tf.z);
			if (t > maxT) return false;

			ivec3 lo = cc << COARSE_SHIFT;
			c = clamp(ivec3(floor(ro + rd * t)), lo, lo + int(COARSE_SIZE) - 1);
			/* step the axis that was crossed into the neighbouring region exactly */
			if (t == tf.x)      c.x = (stp.x > 0) ? lo.x + int(COARSE_SIZE) : lo.x - 1;
			else if (t == tf.y) c.y = (stp.y > 0) ? lo.y + int(COARSE_SIZE) : lo.y - 1;
			else                c.z = (stp.z > 0) ? lo.z + int(COARSE_SIZE) : lo.z - 1;

			if (any(lessThan(c, ivec3(0))) || any(greaterThanEqual(c, worldSize.xyz))) return false;
			continue;
		}

		/* Distance to each of the three cell boundaries in front of the ray. Recomputed from the */
		/*  cell coordinates every step, so no floating point drift accumulates between the cell */
		/*  walk and the exact slab tests in hitCell (drift caused cracks between blocks) */
		vec3 tNext = (vec3(c) + side - ro) * invRd;
		if (rd.x == 0.0) tNext.x = 1e30;
		if (rd.y == 0.0) tNext.y = 1e30;
		if (rd.z == 0.0) tNext.z = 1e30;
		float tExit = min(min(tNext.x, tNext.y), tNext.z);
		uint  b = blockAt(c);

		if (b != 0u) {
			if (hitCell(c, b, ro, rd, invRd, t, min(tExit, maxT), shadowRay, skipTranslucent, h)) {
				h.t += tOffset;
				return true;
			}
		}

		if (tNext.x < tNext.y && tNext.x < tNext.z) {
			c.x += stp.x;
		} else if (tNext.y < tNext.z) {
			c.y += stp.y;
		} else {
			c.z += stp.z;
		}
		t = tExit;

		if (t > maxT) return false;
		if (any(lessThan(c, ivec3(0))) || any(greaterThanEqual(c, worldSize.xyz))) return false;
	}
	return false;
}

/* Colour of a face when in direct sunlight, following the classic per-face shading */
vec3 sunlitColour(vec3 n) {
	if (n.y > 0.5)  return sunCol.rgb;
	if (n.y < -0.5) return sunYMin.rgb;
	if (abs(n.x) > 0.5) return sunXSide.rgb;
	return sunZSide.rgb;
}

/* Colour of a face when only lit by the sky */
vec3 shadowColour(vec3 n) {
	if (n.y > 0.5)  return shadowCol.rgb;
	if (n.y < -0.5) return shadowYMin.rgb;
	if (abs(n.x) > 0.5) return shadowXSide.rgb;
	return shadowZSide.rgb;
}

/* Direct sunlight contribution for a surface point, tracing a shadow ray towards the sun */
vec3 directSun(vec3 p, vec3 n, vec3 L) {
	float ndl = dot(n, L);
	if (ndl <= 0.0) return vec3(0.0);

	Hit sh;
	if (traceRay(p, L, 512.0, true, false, sh)) return vec3(0.0);

	/* Normalise so that an upward facing face receives the full classic sun colour */
	float f = clamp(ndl / max(L.y, 0.2), 0.0, 1.0);
	return max(sunlitColour(n) - shadowColour(n), vec3(0.0)) * f;
}

/* Random number helpers */
uint pcgHash(uint v) {
	uint state = v * 747796405u + 2891336453u;
	uint word  = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
	return (word >> 22u) ^ word;
}

float rand(inout uint seed) {
	seed = pcgHash(seed);
	return float(seed) * (1.0 / 4294967296.0);
}

/* Cosine weighted direction in the hemisphere around n */
vec3 cosineSample(vec3 n, inout uint seed) {
	float r1 = rand(seed);
	float r2 = rand(seed);
	float phi = 6.28318530718 * r1;
	float sr2 = sqrt(r2);
	vec3 t = abs(n.x) > 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
	vec3 u = normalize(cross(t, n));
	vec3 v = cross(n, u);
	return normalize(u * (cos(phi) * sr2) + v * (sin(phi) * sr2) + n * sqrt(max(0.0, 1.0 - r2)));
}

/* Random direction inside a cone around dir, tan of half angle = tanRadius */
vec3 coneSample(vec3 dir, float tanRadius, inout uint seed) {
	float r1 = rand(seed);
	float r2 = rand(seed);
	float phi = 6.28318530718 * r1;
	float r   = tanRadius * sqrt(r2);
	vec3 t = abs(dir.x) > 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
	vec3 u = normalize(cross(t, dir));
	vec3 v = cross(dir, u);
	return normalize(dir + u * (cos(phi) * r) + v * (sin(phi) * r));
}
