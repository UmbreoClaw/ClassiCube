#include "IndevGen.h"
#include "World.h"
#include "Block.h"
#include "BlockID.h"
#include "ExtMath.h"
#include "Funcs.h"
#include "Platform.h"
#include "Utils.h"
#include "Constants.h"
#include "Entity.h"
#include "IndevTest.h"
#include "SurvivalTest.h"

/* Port of in-20100223's LevelGenerator.java. Structure and constants follow
    the decompiled source statement-for-statement; java.util.Random maps onto
    the engine's RNGState (same LCG), and the only knowing deviations are:
    - MathHelper's 65536-entry sine table becomes libm sinf/cosf (identical
      distribution, no bit-parity requirement since seeds aren't shared),
    - growGrassOnDirt/flower placement test "sky-exposed" instead of genuine
      getBlockLightValue >= 4/canBlockStay (generation happens before the
      engine lighting exists; the approximation only loses grass just inside
      cave mouths). */

static int indevgen_type, indevgen_theme;
static RNGState indevgen_rnd;

/* results applied after World_SetNewMap (see IndevGen_ApplyPostLoad) */
static cc_bool indevgen_ran;
static int  indevgen_spawnX, indevgen_spawnY, indevgen_spawnZ;
static int  indevgen_waterLevel, indevgen_groundLevel, indevgen_cloudHeight;

#define INDEVGEN_ISLAND   (indevgen_type == 1)
#define INDEVGEN_FLOATING (indevgen_type == 2)
#define INDEVGEN_FLAT     (indevgen_type == 3)

#define INDEV_BLOCK_TORCH       70
#define INDEV_BLOCK_DIAMOND_ORE 93

void IndevGen_Setup(int type, int theme) {
	indevgen_type  = type;
	indevgen_theme = theme;
}


/*########################################################################################################################*
*--------------------------------------------------------Noise stack------------------------------------------------------*
*#########################################################################################################################*/
struct IndevPerlin { int perm[512]; };

static void IndevPerlin_Init(struct IndevPerlin* p, RNGState* rnd) {
	int i, j, t;
	for (i = 0; i < 256; i++) p->perm[i] = i;

	for (i = 0; i < 256; i++) {
		j = Random_Next(rnd, 256 - i) + i;
		t = p->perm[i];
		p->perm[i]       = p->perm[j];
		p->perm[j]       = t;
		p->perm[i + 256] = p->perm[i];
	}
}

static double IndevPerlin_Fade(double t) {
	return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}
static double IndevPerlin_Lerp(double t, double a, double b) {
	return a + t * (b - a);
}
static double IndevPerlin_Grad(int hash, double x, double y, double z) {
	double u, v;
	hash &= 15;
	u = hash < 8 ? x : y;
	v = hash < 4 ? y : (hash != 12 && hash != 14 ? z : x);
	return ((hash & 1) == 0 ? u : -u) + ((hash & 2) == 0 ? v : -v);
}
static int IndevPerlin_Floor(double d) {
	int i = (int)d;
	return d < i ? i - 1 : i;
}

/* NoiseGeneratorPerlin.generateNoise(x, y) - 3D improved perlin sampled at
    z = 0, exactly as the original's odd formulation does. */
static double IndevPerlin_Noise(struct IndevPerlin* p, double x, double y) {
	double fx, fy, fz, u, v, w;
	int X, Y, Z, A, AA, AB, B, BA, BB;

	X  = IndevPerlin_Floor(x) & 255;
	Y  = IndevPerlin_Floor(y) & 255;
	Z  = 0;
	fx = x - IndevPerlin_Floor(x);
	fy = y - IndevPerlin_Floor(y);
	fz = 0.0;
	u  = IndevPerlin_Fade(fx);
	v  = IndevPerlin_Fade(fy);
	w  = IndevPerlin_Fade(fz);

	A  = p->perm[X] + Y;     AA = p->perm[A] + Z; AB = p->perm[A + 1] + Z;
	B  = p->perm[X + 1] + Y; BA = p->perm[B] + Z; BB = p->perm[B + 1] + Z;

	return IndevPerlin_Lerp(w,
		IndevPerlin_Lerp(v,
			IndevPerlin_Lerp(u, IndevPerlin_Grad(p->perm[AA], fx, fy, fz),
								IndevPerlin_Grad(p->perm[BA], fx - 1.0, fy, fz)),
			IndevPerlin_Lerp(u, IndevPerlin_Grad(p->perm[AB], fx, fy - 1.0, fz),
								IndevPerlin_Grad(p->perm[BB], fx - 1.0, fy - 1.0, fz))),
		IndevPerlin_Lerp(v,
			IndevPerlin_Lerp(u, IndevPerlin_Grad(p->perm[AA + 1], fx, fy, fz - 1.0),
								IndevPerlin_Grad(p->perm[BA + 1], fx - 1.0, fy, fz - 1.0)),
			IndevPerlin_Lerp(u, IndevPerlin_Grad(p->perm[AB + 1], fx, fy - 1.0, fz - 1.0),
								IndevPerlin_Grad(p->perm[BB + 1], fx - 1.0, fy - 1.0, fz - 1.0))));
}

#define INDEV_MAX_OCTAVES 8
struct IndevOctaves { struct IndevPerlin gens[INDEV_MAX_OCTAVES]; int count; };

static void IndevOctaves_Init(struct IndevOctaves* o, RNGState* rnd, int count) {
	int i;
	o->count = count;
	for (i = 0; i < count; i++) IndevPerlin_Init(&o->gens[i], rnd);
}

static double IndevOctaves_Noise(struct IndevOctaves* o, double x, double y) {
	double sum = 0.0, freq = 1.0;
	int i;
	for (i = 0; i < o->count; i++) {
		sum += IndevPerlin_Noise(&o->gens[i], x / freq, y / freq) * freq;
		freq *= 2.0;
	}
	return sum;
}

/* NoiseGeneratorDistort: source sampled at x offset by the distort noise */
struct IndevDistort { struct IndevOctaves source, distort; };

static void IndevDistort_Init(struct IndevDistort* d, RNGState* rnd, int octaves) {
	IndevOctaves_Init(&d->source,  rnd, octaves);
	IndevOctaves_Init(&d->distort, rnd, octaves);
}
static double IndevDistort_Noise(struct IndevDistort* d, double x, double y) {
	return IndevOctaves_Noise(&d->source, x + IndevOctaves_Noise(&d->distort, x, y), y);
}

/* the full complement the pipeline needs, statically allocated (~200KB;
    only one generation runs at a time) */
static struct IndevDistort gen_d1, gen_d2;
static struct IndevOctaves gen_o1, gen_o2;


/*########################################################################################################################*
*---------------------------------------------------------Flood fill------------------------------------------------------*
*#########################################################################################################################*/
/* LevelGenerator.floodFill: scanline fill over the raw block array,
    matching `from` bytes and writing `to`. to = 255 is the probe pass -
    it aborts with -1 the moment the fill touches a map border (used to
    reject open-to-the-edge pockets before committing liquid). The genuine
    segmented 1M-int stack becomes a growable stack. */
static int* ff_stack;
static int  ff_count, ff_capacity;

static cc_bool FloodFill_Push(int v) {
	int* grown;
	if (ff_count == ff_capacity) {
		grown = (int*)Mem_TryAlloc(ff_capacity * 2, 4);
		if (!grown) return false;
		Mem_Copy(grown, ff_stack, ff_count * 4);
		Mem_Free(ff_stack);
		ff_stack    = grown;
		ff_capacity = ff_capacity * 2;
	}
	ff_stack[ff_count++] = v;
	return true;
}

static cc_int64 IndevGen_FloodFill(int x, int y, int z, int from, int to) {
	BlockRaw target = (BlockRaw)to, source = (BlockRaw)from;
	int shiftX = 1, shiftZ = 1;
	int maskZ, maskX, oneY;
	int i, x0, x1, zz, yy;
	cc_int64 filled = 0;
	cc_bool spreadNegZ, spreadPosZ, spreadNegY;
	BlockRaw below;

	while ((1 << shiftX) < World.Width)  shiftX++;
	while ((1 << shiftZ) < World.Length) shiftZ++;
	maskZ = World.Length - 1;
	maskX = World.Width  - 1;
	oneY  = World.Width * World.Length;

	ff_count = 0;
	if (!FloodFill_Push((((y << shiftZ) + z) << shiftX) + x)) return 0;

	while (ff_count > 0) {
		i  = ff_stack[--ff_count];
		zz = (i >> shiftX) & maskZ;
		yy =  i >> (shiftX + shiftZ);
		x0 = i & maskX;

		/* expand to the full matching run along x */
		x1 = x0;
		while (x0 > 0           && Gen_Blocks[i - 1]         == source) { x0--; i--; }
		while (x1 < World.Width && Gen_Blocks[i + (x1 - x0)] == source) { x1++; }

		if (to == 255 && (x0 == 0 || x1 == World.Width - 1 ||
			yy == 0 || yy == World.Height - 1 || zz == 0 || zz == World.Length - 1)) {
			return -1; /* probe touched the border - not a closed pocket */
		}

		spreadNegZ = false; spreadPosZ = false; spreadNegY = false;
		filled += (cc_int64)(x1 - x0);

		for (; x0 < x1; x0++, i++) {
			Gen_Blocks[i] = target;
			if (zz > 0) {
				cc_bool match = Gen_Blocks[i - World.Width] == source;
				if (match && !spreadNegZ) { if (!FloodFill_Push(i - World.Width)) return filled; }
				spreadNegZ = match;
			}
			if (zz < World.Length - 1) {
				cc_bool match = Gen_Blocks[i + World.Width] == source;
				if (match && !spreadPosZ) { if (!FloodFill_Push(i + World.Width)) return filled; }
				spreadPosZ = match;
			}
			if (yy > 0) {
				cc_bool match;
				below = Gen_Blocks[i - oneY];
				/* lava flooding over water freezes the water to stone */
				if ((target == BLOCK_LAVA || target == BLOCK_STILL_LAVA) &&
					(below == BLOCK_WATER || below == BLOCK_STILL_WATER)) {
					Gen_Blocks[i - oneY] = BLOCK_STONE;
				}
				match = below == source;
				if (match && !spreadNegY) { if (!FloodFill_Push(i - oneY)) return filled; }
				spreadNegY = match;
			}
		}
	}
	return filled;
}


/*########################################################################################################################*
*-------------------------------------------------------Terrain passes----------------------------------------------------*
*#########################################################################################################################*/
static int gen_waterLevel, gen_groundLevel;
static int* gen_heightmap;

static void IndevGen_SetState(const char* state, float progress) {
	Gen_CurrentState    = state;
	Gen_CurrentProgress = progress;
}

/* "Raising.." + "Eroding.." - the distorted-noise heightmap */
static void IndevGen_RaiseAndErode(void) {
	int width = World.Width, length = World.Length;
	int x, z, sh;
	double distFromCentreX, distFromCentreZ;
	double hillA, hillB, mountains, height;

	IndevGen_SetState("Raising..", 0.0f);
	IndevDistort_Init(&gen_d1, &indevgen_rnd, 8);
	IndevDistort_Init(&gen_d2, &indevgen_rnd, 8);
	IndevOctaves_Init(&gen_o1, &indevgen_rnd, 6);
	IndevOctaves_Init(&gen_o2, &indevgen_rnd, 2);

	for (x = 0; x < width; x++) {
		distFromCentreX = Math_AbsF((float)((x / (width - 1.0) - 0.5) * 2.0));
		Gen_CurrentProgress = (float)x / (width - 1) * 0.5f;

		for (z = 0; z < length; z++) {
			distFromCentreZ = Math_AbsF((float)((z / (length - 1.0) - 0.5) * 2.0));
			hillA = IndevDistort_Noise(&gen_d1, (float)x * 1.3f, (float)z * 1.3f) / 6.0 + -4.0;
			hillB = IndevDistort_Noise(&gen_d2, (float)x * 1.3f, (float)z * 1.3f) / 5.0 + 10.0 + -4.0;
			mountains = IndevOctaves_Noise(&gen_o1, x, z) / 8.0;
			if (mountains > 0.0) hillB = hillA;

			height = max(hillA, hillB) / 2.0;
			if (INDEVGEN_ISLAND) {
				double edge = Math_SqrtF((float)(distFromCentreX * distFromCentreX +
				                                 distFromCentreZ * distFromCentreZ)) * 1.2f;
				double coast = IndevOctaves_Noise(&gen_o2, (float)x * 0.05f, (float)z * 0.05f) / 4.0 + 1.0;
				edge = min(edge, coast);
				edge = max(edge, max(distFromCentreX, distFromCentreZ));
				if (edge > 1.0) edge = 1.0;
				if (edge < 0.0) edge = 0.0;

				edge *= edge;
				height = height * (1.0 - edge) - edge * 10.0 + 5.0;
				if (height < 0.0) height -= height * height * 0.2f;
			} else if (height < 0.0) {
				height *= 0.8;
			}

			gen_heightmap[x + z * width] = (int)height;
		}
	}

	IndevGen_SetState("Eroding..", 0.5f);
	IndevDistort_Init(&gen_d1, &indevgen_rnd, 8);
	IndevDistort_Init(&gen_d2, &indevgen_rnd, 8);

	for (x = 0; x < width; x++) {
		Gen_CurrentProgress = 0.5f + (float)x / (width - 1) * 0.5f;
		for (z = 0; z < length; z++) {
			double erode = IndevDistort_Noise(&gen_d1, x << 1, z << 1) / 8.0;
			int eroded   = IndevDistort_Noise(&gen_d2, x << 1, z << 1) > 0.0 ? 1 : 0;
			if (erode > 2.0) {
				sh = gen_heightmap[x + z * width];
				sh = ((sh - eroded) / 2 << 1) + eroded;
				gen_heightmap[x + z * width] = sh;
			}
		}
	}
}

/* "Soiling.." - dirt over stone under the heightmap (with the floating-
    layer carve-out), heights relative to this layer's water level */
static void IndevGen_Soil(void) {
	int width = World.Width, length = World.Length, height = World.Height;
	int x, z, y, dirtY, stoneY, floatCut, index, id;
	double distX, distZ, corner, cliff;

	IndevGen_SetState("Soiling..", 0.0f);
	IndevOctaves_Init(&gen_o1, &indevgen_rnd, 8);
	IndevOctaves_Init(&gen_o2, &indevgen_rnd, 8);

	for (x = 0; x < width; x++) {
		distX = Math_AbsF((float)((x / (width - 1.0) - 0.5) * 2.0));
		Gen_CurrentProgress = (float)x / (width - 1);

		for (z = 0; z < length; z++) {
			distZ  = Math_AbsF((float)((z / (length - 1.0) - 0.5) * 2.0));
			corner = max(distX, distZ);
			corner = corner * corner * corner;

			stoneY = (int)(IndevOctaves_Noise(&gen_o1, x, z) / 24.0) - 4;
			dirtY  = gen_heightmap[x + z * width] + gen_waterLevel;
			stoneY = dirtY + stoneY;
			gen_heightmap[x + z * width] = max(dirtY, stoneY);
			if (gen_heightmap[x + z * width] > height - 2) gen_heightmap[x + z * width] = height - 2;
			if (gen_heightmap[x + z * width] <= 0)         gen_heightmap[x + z * width] = 1;

			cliff = IndevOctaves_Noise(&gen_o2, x * 2.3, z * 2.3) / 24.0;
			floatCut = (int)(Math_SqrtF((float)Math_AbsF((float)cliff)) *
			                 (cliff < 0.0 ? -1.0f : (cliff > 0.0 ? 1.0f : 0.0f)) * 20.0f) + gen_waterLevel;
			floatCut = (int)(floatCut * (1.0 - corner) + corner * height);
			if (floatCut > gen_waterLevel) floatCut = height;

			for (y = 0; y < height; y++) {
				index = (y * length + z) * width + x;
				id = 0;
				if (y <= dirtY)  id = BLOCK_DIRT;
				if (y <= stoneY) id = BLOCK_STONE;
				if (INDEVGEN_FLOATING && y < floatCut) id = 0;

				if (Gen_Blocks[index] == 0) Gen_Blocks[index] = (BlockRaw)id;
			}
		}
	}
}

/* "Growing.." - surface pass: gravel under shallow water, sand (or hell
    grass) on dry beach-height surfaces */
static void IndevGen_Grow(void) {
	int width = World.Width, length = World.Length;
	int x, z, surfaceY, index, above, id, sandY;
	cc_bool sandNoise, gravelNoise;

	IndevGen_SetState("Growing..", 0.0f);
	IndevOctaves_Init(&gen_o1, &indevgen_rnd, 8);
	IndevOctaves_Init(&gen_o2, &indevgen_rnd, 8);
	sandY = gen_waterLevel - 1;
	if (indevgen_theme == 2) sandY += 2; /* paradise: beaches reach higher */

	for (x = 0; x < width; x++) {
		Gen_CurrentProgress = (float)x / (width - 1);
		for (z = 0; z < length; z++) {
			sandNoise = IndevOctaves_Noise(&gen_o1, x, z) > 8.0;
			if (INDEVGEN_ISLAND)   sandNoise = IndevOctaves_Noise(&gen_o1, x, z) > -8.0;
			if (indevgen_theme == 2) sandNoise = IndevOctaves_Noise(&gen_o1, x, z) > -32.0;
			if (indevgen_theme == 1 || indevgen_theme == 3) {
				sandNoise = IndevOctaves_Noise(&gen_o1, x, z) > -8.0;
			}
			gravelNoise = IndevOctaves_Noise(&gen_o2, x, z) > 12.0;

			surfaceY = gen_heightmap[x + z * width];
			index    = (surfaceY * length + z) * width + x;
			above    = Gen_Blocks[((surfaceY + 1) * length + z) * width + x];

			if ((above == BLOCK_WATER || above == BLOCK_STILL_WATER || above == 0) &&
				surfaceY <= gen_waterLevel - 1 && gravelNoise) {
				Gen_Blocks[index] = BLOCK_GRAVEL;
			}

			if (above == 0) {
				id = -1;
				if (surfaceY <= sandY && sandNoise) {
					id = BLOCK_SAND;
					if (indevgen_theme == 1) id = BLOCK_GRASS; /* hell quirk */
				}
				if (Gen_Blocks[index] != 0 && id > 0) Gen_Blocks[index] = (BlockRaw)id;
			}
		}
	}
}

/* "Carving.." - the worm-tunnel caves */
static void IndevGen_Carve(void) {
	int width = World.Width, length = World.Length, height = World.Height;
	int caves, i, step, steps, index;
	int xx, yy, zz;
	float x, y, z, dirXZ, dirXZChange, dirY, dirYChange, sizeMul;
	float cx, cy, cz, heightF, baseSize, size;
	float dx, dy, dz, distSq;

	IndevGen_SetState("Carving..", 0.0f);
	caves = width * length * height / 256 / 64 << 1;

	for (i = 0; i < caves; i++) {
		Gen_CurrentProgress = (float)i / (caves - 1);
		x = Random_Float(&indevgen_rnd) * width;
		y = Random_Float(&indevgen_rnd) * height;
		z = Random_Float(&indevgen_rnd) * length;
		steps = (int)((Random_Float(&indevgen_rnd) + Random_Float(&indevgen_rnd)) * 200.0f);

		dirXZ       = Random_Float(&indevgen_rnd) * MATH_PI * 2.0f;
		dirXZChange = 0.0f;
		dirY        = Random_Float(&indevgen_rnd) * MATH_PI * 2.0f;
		dirYChange  = 0.0f;
		sizeMul     = Random_Float(&indevgen_rnd) * Random_Float(&indevgen_rnd);

		for (step = 0; step < steps; step++) {
			x += Math_SinF(dirXZ) * Math_CosF(dirY);
			z += Math_CosF(dirXZ) * Math_CosF(dirY);
			y += Math_SinF(dirY);

			dirXZ += dirXZChange * 0.2f;
			dirXZChange *= 0.9f;
			dirXZChange += Random_Float(&indevgen_rnd) - Random_Float(&indevgen_rnd);
			dirY += dirYChange * 0.5f;
			dirY *= 0.5f;
			dirYChange *= 12.0f / 16.0f;
			dirYChange += Random_Float(&indevgen_rnd) - Random_Float(&indevgen_rnd);

			if (Random_Float(&indevgen_rnd) < 0.25f) continue;
			cx = x + (Random_Float(&indevgen_rnd) * 4.0f - 2.0f) * 0.2f;
			cy = y + (Random_Float(&indevgen_rnd) * 4.0f - 2.0f) * 0.2f;
			cz = z + (Random_Float(&indevgen_rnd) * 4.0f - 2.0f) * 0.2f;

			heightF  = ((float)height - cy) / (float)height;
			baseSize = 1.2f + (heightF * 3.5f + 1.0f) * sizeMul;
			size     = Math_SinF((float)step * MATH_PI / (float)steps) * baseSize;

			for (xx = (int)(cx - size); xx <= (int)(cx + size); xx++) {
				for (yy = (int)(cy - size); yy <= (int)(cy + size); yy++) {
					for (zz = (int)(cz - size); zz <= (int)(cz + size); zz++) {
						dx = (float)xx - cx;
						dy = (float)yy - cy;
						dz = (float)zz - cz;
						distSq = dx * dx + dy * dy * 2.0f + dz * dz;
						if (distSq < size * size &&
							xx > 0 && yy > 0 && zz > 0 &&
							xx < width - 1 && yy < height - 1 && zz < length - 1) {
							index = (yy * length + zz) * width + xx;
							if (Gen_Blocks[index] == BLOCK_STONE) Gen_Blocks[index] = 0;
						}
					}
				}
			}
		}
	}
}

/* populateOre - the same worm shape, thinner, replacing stone */
static int IndevGen_PopulateOre(BlockRaw ore, int abundance, int veinSize, int maxHeight) {
	int width = World.Width, length = World.Length, height = World.Height;
	int count, i, step, steps, placed = 0, index;
	int xx, yy, zz;
	float x, y, z, dirXZ, dirXZChange, dirY, dirYChange;
	float size, dx, dy, dz, distSq;

	count = width * length * height / 256 / 64 * abundance / 100;

	for (i = 0; i < count; i++) {
		Gen_CurrentProgress = (float)i / (count - 1);
		x = Random_Float(&indevgen_rnd) * width;
		y = Random_Float(&indevgen_rnd) * height;
		z = Random_Float(&indevgen_rnd) * length;
		if (y > (float)maxHeight) continue;

		steps = (int)((Random_Float(&indevgen_rnd) + Random_Float(&indevgen_rnd)) * 75.0f * veinSize / 100.0f);
		dirXZ       = Random_Float(&indevgen_rnd) * MATH_PI * 2.0f;
		dirXZChange = 0.0f;
		dirY        = Random_Float(&indevgen_rnd) * MATH_PI * 2.0f;
		dirYChange  = 0.0f;

		for (step = 0; step < steps; step++) {
			x += Math_SinF(dirXZ) * Math_CosF(dirY);
			z += Math_CosF(dirXZ) * Math_CosF(dirY);
			y += Math_SinF(dirY);

			dirXZ += dirXZChange * 0.2f;
			dirXZChange *= 0.9f;
			dirXZChange += Random_Float(&indevgen_rnd) - Random_Float(&indevgen_rnd);
			dirY += dirYChange * 0.5f;
			dirY *= 0.5f;
			dirYChange *= 0.9f;
			dirYChange += Random_Float(&indevgen_rnd) - Random_Float(&indevgen_rnd);

			size = Math_SinF((float)step * MATH_PI / (float)steps) * veinSize / 100.0f + 1.0f;

			for (xx = (int)(x - size); xx <= (int)(x + size); xx++) {
				for (yy = (int)(y - size); yy <= (int)(y + size); yy++) {
					for (zz = (int)(z - size); zz <= (int)(z + size); zz++) {
						dx = (float)xx - x;
						dy = (float)yy - y;
						dz = (float)zz - z;
						distSq = dx * dx + dy * dy * 2.0f + dz * dz;
						if (distSq < size * size &&
							xx > 0 && yy > 0 && zz > 0 &&
							xx < width - 1 && yy < height - 1 && zz < length - 1) {
							index = (yy * length + zz) * width + xx;
							if (Gen_Blocks[index] == BLOCK_STONE) {
								Gen_Blocks[index] = ore;
								placed++;
							}
						}
					}
				}
			}
		}
	}
	return placed;
}

/* "Melting.." - underground lava pockets, biased deep by a quadruple-min */
static void IndevGen_LavaGen(void) {
	int attempts = World.Width * World.Length * World.Height / 2000;
	int ground   = gen_groundLevel;
	int i, x, y, z;
	cc_int64 size;

	IndevGen_SetState("Melting..", 0.0f);
	for (i = 0; i < attempts; i++) {
		if (i % 100 == 0) Gen_CurrentProgress = (float)i / (attempts - 1);
		x = Random_Next(&indevgen_rnd, World.Width);
		y = min(min(Random_Next(&indevgen_rnd, ground), Random_Next(&indevgen_rnd, ground)),
		        min(Random_Next(&indevgen_rnd, ground), Random_Next(&indevgen_rnd, ground)));
		z = Random_Next(&indevgen_rnd, World.Length);

		if (Gen_Blocks[(y * World.Length + z) * World.Width + x] != 0) continue;
		size = IndevGen_FloodFill(x, y, z, 0, 255);
		if (size > 0 && size < 640) {
			IndevGen_FloodFill(x, y, z, 255, BLOCK_STILL_LAVA);
		} else {
			IndevGen_FloodFill(x, y, z, 255, 0);
		}
	}
}

/* "Watering.." part 1 - theme springs: small closed air pockets anywhere
    become still water (lava on hell) */
static void IndevGen_LiquidThemeSpawner(void) {
	BlockRaw fluid = indevgen_theme == 1 ? BLOCK_STILL_LAVA : BLOCK_STILL_WATER;
	int attempts = World.Width * World.Length * World.Height / 1000;
	int i, x, y, z;
	cc_int64 size;

	IndevGen_SetState("Watering..", 0.0f);
	for (i = 0; i < attempts; i++) {
		if (i % 100 == 0) Gen_CurrentProgress = (float)i / (attempts - 1);
		x = Random_Next(&indevgen_rnd, World.Width);
		y = Random_Next(&indevgen_rnd, World.Height);
		z = Random_Next(&indevgen_rnd, World.Length);

		if (Gen_Blocks[(y * World.Length + z) * World.Width + x] != 0) continue;
		size = IndevGen_FloodFill(x, y, z, 0, 255);
		if (size > 0 && size < 640) {
			IndevGen_FloodFill(x, y, z, 255, fluid);
		} else {
			IndevGen_FloodFill(x, y, z, 255, 0);
		}
	}
}


/*########################################################################################################################*
*----------------------------------------------------Decoration passes----------------------------------------------------*
*#########################################################################################################################*/
static BlockRaw IndevGen_Get(int x, int y, int z) {
	return Gen_Blocks[(y * World.Length + z) * World.Width + x];
}
static void IndevGen_Set(int x, int y, int z, BlockRaw b) {
	Gen_Blocks[(y * World.Length + z) * World.Width + x] = b;
}
static cc_bool IndevGen_Opaque(BlockRaw b) {
	return Blocks.FullOpaque[b];
}
/* World.getFirstUncoveredBlock: first y above the topmost solid/liquid */
static int IndevGen_FirstUncovered(int x, int z) {
	int y;
	for (y = World.Height; y > 0; y--) {
		BlockRaw b = IndevGen_Get(x, y - 1, z);
		if (b != 0) break;
	}
	return y;
}
/* sky-exposure approximation for the lighting-dependent plant passes */
static cc_bool IndevGen_SkyExposed(int x, int y, int z) {
	int yy;
	for (yy = y + 1; yy < World.Height; yy++) {
		if (IndevGen_Opaque(IndevGen_Get(x, yy, z))) return false;
	}
	return true;
}

/* "Assembling.." - World.generate()'s floor/border fill. Interior columns
    only touch y=0, y=1 and the very top layer (the genuine y-skip quirk at
    World.java:118); border columns are rebuilt top to bottom: bedrock shell
    below groundLevel-1 (still lava at y<=1 under air gaps), a dirt/grass cap
    at groundLevel-1, then fluid up to waterLevel. On Floating maps
    groundLevel=-128 makes every branch miss, so everything this pass touches
    becomes air - that is what hollows out the basin under the islands */
static void IndevGen_Assemble(void) {
	int width = World.Width, length = World.Length, height = World.Height;
	int x, y, z, id, index;
	cc_bool border;
	BlockRaw fluid = indevgen_theme == 1 ? BLOCK_STILL_LAVA : BLOCK_STILL_WATER;
	int cap = (gen_groundLevel > gen_waterLevel && indevgen_theme != 1) ? BLOCK_GRASS : BLOCK_DIRT;

	IndevGen_SetState("Assembling..", 0.0f);
	for (x = 0; x < width; x++) {
		Gen_CurrentProgress = (float)x / (width - 1);

		for (z = 0; z < length; z++) {
			border = x == 0 || z == 0 || x == width - 1 || z == length - 1;

			for (y = 0; y < height; y++) {
				index = (y * length + z) * width + x;
				id    = 0;
				if (y <= 1 && y < gen_groundLevel - 1 && Gen_Blocks[index + width * length] == 0) {
					id = BLOCK_STILL_LAVA;
				} else if (y < gen_groundLevel - 1) {
					id = BLOCK_BEDROCK;
				} else if (y < gen_groundLevel) {
					id = cap;
				} else if (y < gen_waterLevel) {
					id = fluid;
				}

				Gen_Blocks[index] = (BlockRaw)id;
				if (y == 1 && !border) y = height - 2;
			}
		}
	}
}

/* World.findSpawn: a random mid-map surface spot above water with room
    for (and a solid foundation under) the spawn house */
static void IndevGen_FindSpawn(void) {
	int attempts = 0;
	int x, y, z, xx, yy, zz;
	cc_bool ok;

	for (;;) {
		attempts++;
		x = Random_Next(&indevgen_rnd, World.Width  / 2) + World.Width  / 4;
		z = Random_Next(&indevgen_rnd, World.Length / 2) + World.Length / 4;
		y = IndevGen_FirstUncovered(x, z) + 1;

		if (attempts == 1000000) {
			indevgen_spawnX = x;
			indevgen_spawnY = World.Height + 100;
			indevgen_spawnZ = z;
			return;
		}
		if (y < 4 || y <= gen_waterLevel) continue;

		/* the house interior volume must be clear of solids */
		ok = true;
		for (xx = x - 3; xx <= x + 3 && ok; xx++) {
			for (yy = y - 1; yy <= y + 2 && ok; yy++) {
				for (zz = z - 3 - 2; zz <= z + 3 && ok; zz++) {
					if (xx < 0 || yy < 0 || zz < 0 ||
						xx >= World.Width || yy >= World.Height || zz >= World.Length) continue;
					if (Blocks.Collide[IndevGen_Get(xx, yy, zz)] == COLLIDE_SOLID) ok = false;
				}
			}
		}
		if (!ok) continue;

		/* and the ground under the footprint must be fully opaque */
		yy = y - 2;
		for (xx = x - 3; xx <= x + 3 && ok; xx++) {
			for (zz = z - 3 - 2; zz <= z + 3 && ok; zz++) {
				if (xx < 0 || yy < 0 || zz < 0 ||
					xx >= World.Width || yy >= World.Height || zz >= World.Length) { ok = false; break; }
				if (!IndevGen_Opaque(IndevGen_Get(xx, yy, zz))) ok = false;
			}
		}
		if (!ok) continue;

		indevgen_spawnX = x;
		indevgen_spawnY = y;
		indevgen_spawnZ = z;
		return;
	}
}

/* LevelGenerator.generateHouse: the 7x5x7 stone/plank shelter around the
    spawn - obsidian floor slab, door gap on the -Z face, two torches */
static void IndevGen_GenerateHouse(void) {
	int x1 = indevgen_spawnX, y1 = indevgen_spawnY, z1 = indevgen_spawnZ;
	int x, y, z, id;
	if (y1 >= World.Height) return; /* the sky-fallback spawn */

	for (x = x1 - 3; x <= x1 + 3; x++) {
		for (y = y1 - 2; y <= y1 + 2; y++) {
			for (z = z1 - 3; z <= z1 + 3; z++) {
				if (x < 0 || y < 0 || z < 0 ||
					x >= World.Width || y >= World.Height || z >= World.Length) continue;

				id = y < y1 - 1 ? BLOCK_OBSIDIAN : 0;
				if (x == x1 - 3 || z == z1 - 3 || x == x1 + 3 || z == z1 + 3 ||
					y == y1 - 2 || y == y1 + 2) {
					id = BLOCK_STONE;
					if (y >= y1 - 1) id = BLOCK_WOOD;
				}
				if (z == z1 - 3 && x == x1 && y >= y1 - 1 && y <= y1) id = 0; /* doorway */

				IndevGen_Set(x, y, z, (BlockRaw)id);
			}
		}
	}
	/* torches are an Indev-layer block; classic generations leave the
	    house unlit rather than placing an undefined id */
	if (!IndevTest_Enabled) return;
	if (World_Contains(x1 - 2, y1, z1)) IndevGen_Set(x1 - 2, y1, z1, INDEV_BLOCK_TORCH);
	if (World_Contains(x1 + 2, y1, z1)) IndevGen_Set(x1 + 2, y1, z1, INDEV_BLOCK_TORCH);
}

/* growGrassOnDirt (light >= 4 approximated as sky exposure) */
static void IndevGen_GrowGrass(void) {
	int x, y, z;
	IndevGen_SetState("Planting..", 0.0f);

	for (x = 0; x < World.Width; x++) {
		Gen_CurrentProgress = (float)x / (World.Width - 1);
		for (z = 0; z < World.Length; z++) {
			for (y = World.Height - 2; y >= 0; y--) {
				if (IndevGen_Opaque(IndevGen_Get(x, y + 1, z))) break; /* covered from here down */
				if (IndevGen_Get(x, y, z) == BLOCK_DIRT) IndevGen_Set(x, y, z, BLOCK_GRASS);
			}
		}
	}
}

/* World.growTrees(x, y, z): trunk rand(3)+4, clearance envelope, diamond
    leaf canopy with random corner trimming */
static cc_bool IndevGen_GrowTree(int x, int y, int z) {
	int trunkH = Random_Next(&indevgen_rnd, 3) + 4;
	int xx, yy, zz, clearance, dy, radius, dxa, dza;
	BlockRaw below;

	if (y <= 0 || y + trunkH + 1 > World.Height) return false;

	for (yy = y; yy <= y + 1 + trunkH; yy++) {
		clearance = 1;
		if (yy == y) clearance = 0;
		if (yy >= y + 1 + trunkH - 2) clearance = 2;

		for (xx = x - clearance; xx <= x + clearance; xx++) {
			for (zz = z - clearance; zz <= z + clearance; zz++) {
				if (xx < 0 || yy < 0 || zz < 0 ||
					xx >= World.Width || yy >= World.Height || zz >= World.Length) return false;
				if (IndevGen_Get(xx, yy, zz) != 0) return false;
			}
		}
	}

	below = IndevGen_Get(x, y - 1, z);
	if (below != BLOCK_GRASS && below != BLOCK_DIRT) return false;
	if (y >= World.Height - trunkH - 1) return false;
	IndevGen_Set(x, y - 1, z, BLOCK_DIRT);

	for (yy = y - 3 + trunkH; yy <= y + trunkH; yy++) {
		dy     = yy - (y + trunkH);
		radius = 1 - dy / 2;

		for (xx = x - radius; xx <= x + radius; xx++) {
			dxa = xx - x; if (dxa < 0) dxa = -dxa;
			for (zz = z - radius; zz <= z + radius; zz++) {
				dza = zz - z; if (dza < 0) dza = -dza;
				if (dxa == radius && dza == radius &&
					(Random_Next(&indevgen_rnd, 2) == 0 || dy == 0)) continue;
				if (!IndevGen_Opaque(IndevGen_Get(xx, yy, zz))) {
					IndevGen_Set(xx, yy, zz, BLOCK_LEAVES);
				}
			}
		}
	}

	for (yy = 0; yy < trunkH; yy++) {
		if (!IndevGen_Opaque(IndevGen_Get(x, y + yy, z))) {
			IndevGen_Set(x, y + yy, z, BLOCK_LOG);
		}
	}
	return true;
}

static void IndevGen_GrowTrees(void) {
	int clusters = World.Width * World.Length * World.Height / 80000;
	int i, j, k, x, y, z, xx, yy, zz;

	for (i = 0; i < clusters; i++) {
		x = Random_Next(&indevgen_rnd, World.Width);
		y = Random_Next(&indevgen_rnd, World.Height);
		z = Random_Next(&indevgen_rnd, World.Length);

		for (j = 0; j < 25; j++) {
			xx = x; yy = y; zz = z;
			for (k = 0; k < 20; k++) {
				xx += Random_Next(&indevgen_rnd, 12) - Random_Next(&indevgen_rnd, 12);
				yy += Random_Next(&indevgen_rnd, 3)  - Random_Next(&indevgen_rnd, 6);
				zz += Random_Next(&indevgen_rnd, 12) - Random_Next(&indevgen_rnd, 12);
				if (xx >= 0 && yy >= 0 && zz >= 0 &&
					xx < World.Width && yy < World.Height && zz < World.Length) {
					IndevGen_GrowTree(xx, yy, zz);
				}
			}
		}
	}
}

/* populateFlowersAndMushrooms: flowers need grass/dirt + sky exposure,
    mushrooms just a solid floor (they live in caves) */
static void IndevGen_Populate(BlockRaw plant, int rawCount) {
	int count = (int)((cc_int64)World.Width * World.Length * World.Height * rawCount / 1600000);
	int i, j, k, x, y, z, xx, yy, zz;
	BlockRaw below;
	cc_bool isFlower = plant == BLOCK_DANDELION || plant == BLOCK_ROSE;

	for (i = 0; i < count; i++) {
		x = Random_Next(&indevgen_rnd, World.Width);
		y = Random_Next(&indevgen_rnd, World.Height);
		z = Random_Next(&indevgen_rnd, World.Length);

		for (j = 0; j < 10; j++) {
			xx = x; yy = y; zz = z;
			for (k = 0; k < 10; k++) {
				xx += Random_Next(&indevgen_rnd, 4) - Random_Next(&indevgen_rnd, 4);
				yy += Random_Next(&indevgen_rnd, 2) - Random_Next(&indevgen_rnd, 2);
				zz += Random_Next(&indevgen_rnd, 4) - Random_Next(&indevgen_rnd, 4);
				if (xx < 0 || zz < 0 || yy <= 0 ||
					xx >= World.Width || zz >= World.Length || yy >= World.Height) continue;
				if (IndevGen_Get(xx, yy, zz) != 0) continue;

				below = IndevGen_Get(xx, yy - 1, zz);
				if (isFlower) {
					/* BlockFlower.canBlockStay: grass/dirt below + light */
					if (below != BLOCK_GRASS && below != BLOCK_DIRT) continue;
					if (!IndevGen_SkyExposed(xx, yy, zz)) continue;
				} else {
					/* BlockMushroom.canBlockStay: opaque floor, shade */
					if (!IndevGen_Opaque(below)) continue;
				}
				IndevGen_Set(xx, yy, zz, plant);
			}
		}
	}
}


/*########################################################################################################################*
*--------------------------------------------------------Main pipeline----------------------------------------------------*
*#########################################################################################################################*/
static cc_bool IndevGen_Prepare(int seed) {
	Random_Seed(&indevgen_rnd, seed);
	indevgen_ran = false;

	gen_heightmap = (int*)Mem_TryAlloc(World.Width * World.Length, 4);
	ff_capacity   = 1048576;
	ff_stack      = (int*)Mem_TryAlloc(ff_capacity, 4);
	if (!gen_heightmap || !ff_stack) {
		Mem_Free(gen_heightmap); gen_heightmap = NULL;
		Mem_Free(ff_stack);      ff_stack      = NULL;
		return false;
	}
	return true;
}

static void IndevGen_Generate(void) {
	int width = World.Width, length = World.Length, height = World.Height;
	int layers = 1, layer, i, x, z;
	BlockRaw edgeFluid;

	if (INDEVGEN_FLOATING) layers = (height - 64) / 48 + 1;

	Mem_Set(Gen_Blocks, 0, World.Volume);

	for (layer = 0; layer < layers; layer++) {
		gen_waterLevel  = height - 32 - layer * 48;
		gen_groundLevel = gen_waterLevel - 2;

		if (INDEVGEN_FLAT) {
			for (i = 0; i < width * length; i++) gen_heightmap[i] = 0;
		} else {
			IndevGen_RaiseAndErode();
		}
		IndevGen_Soil();
		IndevGen_Grow();
	}

	IndevGen_Carve();
	IndevGen_SetState("Mining..", 0.0f);
	IndevGen_PopulateOre(BLOCK_COAL_ORE, 1000, 10, (height << 2) / 5);
	IndevGen_PopulateOre(BLOCK_IRON_ORE,  800,  8, height * 3 / 5);
	IndevGen_PopulateOre(BLOCK_GOLD_ORE,  500,  6, (height << 1) / 5);
	/* diamond ore only exists as a block in Indev mode - classic/creative
	    generations substitute the closest classic look */
	IndevGen_PopulateOre(IndevTest_Enabled ? INDEV_BLOCK_DIAMOND_ORE : BLOCK_COAL_ORE,
	                     800, 2, height / 5);
	IndevGen_LavaGen();

	indevgen_cloudHeight = height + 2;
	if (INDEVGEN_FLOATING) {
		gen_groundLevel = -128;
		gen_waterLevel  = gen_groundLevel + 1;
		indevgen_cloudHeight = -16;
	} else if (!INDEVGEN_ISLAND) {
		gen_groundLevel = gen_waterLevel + 1;
		gen_waterLevel  = gen_groundLevel - 16;
	} else {
		gen_groundLevel = gen_waterLevel - 9;
	}

	IndevGen_LiquidThemeSpawner();
	if (!INDEVGEN_FLOATING) {
		edgeFluid = indevgen_theme == 1 ? BLOCK_STILL_LAVA : BLOCK_STILL_WATER;
		for (x = 0; x < width; x++) {
			IndevGen_FloodFill(x, gen_waterLevel - 1, 0,          0, edgeFluid);
			IndevGen_FloodFill(x, gen_waterLevel - 1, length - 1, 0, edgeFluid);
		}
		for (z = 0; z < length; z++) {
			IndevGen_FloodFill(width - 1, gen_waterLevel - 1, z, 0, edgeFluid);
			IndevGen_FloodFill(0,         gen_waterLevel - 1, z, 0, edgeFluid);
		}
	}
	if (indevgen_theme == 1 && INDEVGEN_FLOATING) {
		indevgen_cloudHeight = height + 2;
		gen_waterLevel = -16;
	}

	IndevGen_Assemble();

	IndevGen_SetState("Building..", 0.0f);
	IndevGen_FindSpawn();
	IndevGen_GenerateHouse();

	if (indevgen_theme != 1) IndevGen_GrowGrass();

	IndevGen_SetState("Planting..", 0.0f);
	IndevGen_GrowTrees();
	if (indevgen_theme == 3) { /* woods: 50 extra tree passes */
		for (i = 0; i < 50; i++) IndevGen_GrowTrees();
	}

	i = indevgen_theme == 2 ? 1000 : 100; /* paradise: 10x the flowers */
	IndevGen_Populate(BLOCK_DANDELION,    i);
	IndevGen_Populate(BLOCK_ROSE,         i);
	IndevGen_Populate(BLOCK_BROWN_SHROOM, 50);
	IndevGen_Populate(BLOCK_RED_SHROOM,   50);

	indevgen_waterLevel  = gen_waterLevel;
	indevgen_groundLevel = gen_groundLevel;
	indevgen_ran = true;

	Mem_Free(gen_heightmap); gen_heightmap = NULL;
	Mem_Free(ff_stack);      ff_stack      = NULL;
	ff_capacity = 0;
	Gen_SetDone();
}

const struct MapGenerator IndevGen = { IndevGen_Prepare, IndevGen_Generate };


/*########################################################################################################################*
*------------------------------------------------------Post-load applies--------------------------------------------------*
*#########################################################################################################################*/
static PackedCol IndevGen_Col(int rgb) {
	return PackedCol_Make(rgb >> 16, rgb >> 8, rgb, 255);
}

cc_bool IndevGen_ApplyPostLoad(struct LocationUpdate* update) {
	int theme = indevgen_theme;
	if (!indevgen_ran) return false;
	indevgen_ran = false;

	/* theme environments, straight from the generate() tail */
	if (theme == 0) {
		IndevTest_SetBaseEnvColors(IndevGen_Col(10079487), IndevGen_Col(16777215), IndevGen_Col(16777215));
		IndevTest_SetSkyBrightness(15);
	} else if (theme == 1) {
		IndevTest_SetBaseEnvColors(IndevGen_Col(1049600), IndevGen_Col(1049600), IndevGen_Col(2164736));
		IndevTest_SetSkyBrightness(7);
	} else if (theme == 2) {
		IndevTest_SetBaseEnvColors(IndevGen_Col(13033215), IndevGen_Col(13033215), IndevGen_Col(15658751));
		IndevTest_SetSkyBrightness(16); /* > 15 = the always-day paradise flag */
	} else {
		IndevTest_SetBaseEnvColors(IndevGen_Col(7699847), IndevGen_Col(5069403), IndevGen_Col(5069403));
		IndevTest_SetSkyBrightness(12);
	}

	Env_SetCloudsHeight(indevgen_cloudHeight);
	Env_SetEdgeHeight(indevgen_waterLevel);
	Env_SetSidesOffset(indevgen_groundLevel - indevgen_waterLevel);
	Env_SetEdgeBlock(theme == 1 ? BLOCK_STILL_LAVA : BLOCK_STILL_WATER);
	/* the genuine border wall is a bedrock shell (World.generate); bedrock
	    sides also give floating maps their empty bedrock basin, since the
	    engine always draws a SidesBlock plane at y=0 beneath the map */
	Env_SetSidesBlock(BLOCK_BEDROCK);

	/* spawn inside the house, facing the genuine rotSpawn = 180.
	    Genuine preparePlayerToSpawn puts the bounding box CENTRE at ySpawn,
	    so the feet sit at ySpawn - 0.9 (0.1 above the house floor) */
	update->flags = LU_HAS_POS | LU_HAS_YAW | LU_HAS_PITCH;
	update->pos.x = indevgen_spawnX + 0.5f;
	update->pos.y = indevgen_spawnY - 0.9f;
	update->pos.z = indevgen_spawnZ + 0.5f;
	update->yaw   = 180.0f;
	update->pitch = 0.0f;

	if (Entities.CurPlayer) {
		Entities.CurPlayer->Spawn      = update->pos;
		Entities.CurPlayer->SpawnYaw   = update->yaw;
		Entities.CurPlayer->SpawnPitch = update->pitch;
	}

	/* the generator's 1000 MobSpawner.performSpawning passes */
	SurvivalTest_IndevInitialSpawn();
	return true;
}
