#include "Lighting.h"
#include "Block.h"
#include "Funcs.h"
#include "MapRenderer.h"
#include "Platform.h"
#include "World.h"
#include "Logger.h"
#include "Event.h"
#include "Game.h"
#include "String_.h"
#include "Chat.h"
#include "ExtMath.h"
#include "Options.h"
#include "Queue.h"
#include "IndevTest.h"

/* ==== Indev genuine lighting (Light.java) ====
   On Indev maps the sky is a real flooded 0..15 channel, not the binary
   sun/shadow bit: cells above the heightmap are sky SOURCES, and light
   flood-fills into caves losing (at least) 1 per block - so caves fade to
   light 0, which the genuine brightness curve renders at 5% (near-black).
   Storage reuses the LAVA nibble (genuine Indev has no tinted lava light;
   every emitter - lava 15, fire 15, torch 14, lit furnace 14, brown
   mushroom 1 - lives in the LAMP nibble at its genuine Block.lightValue).
   Above-heightmap cells stay implicit (no storage; the sun palette group
   ignores the stored nibble), so only cave light costs memory.

   Day/night never re-floods: genuine updateDaylightCycle injects the eased
   sky level k and re-walks the map, but with uniform attenuation that is
   exactly effSky = skyFlood - (15 - k), so dimming is only a palette
   rebuild. Final brightness = genuine table[max(lamp, effSky)]. */
static cc_bool indevSky;          /* Indev map: flooded-sky mode is active */
static int indevSkyLevel = 15;    /* eased day/night sky level k (4..15) */

void FancyLighting_SetIndevSky(int level) {
	if (level > 15) level = 15;
	if (level < 0)  level = 0;
	indevSkyLevel = level;
	/* The caller (Indev day/night tick) changes Env.SunCol right after,
	    which rebuilds the palettes and refreshes the chunk colours. */
}

struct LightNode {
	IVec3 coords; /* 12 bytes */
	cc_uint8 brightness; /* 1 byte */
	/* char padding[3]; */
};

static struct Queue lightQueue;
static struct Queue unlightQueue;

/* Top face, X face, Z face, bottomY face*/
#define PALETTE_SHADES 4
/* One palette-group for sunlight, one palette-group for shadow */
#define PALLETE_GROUP_COUNT 2
#define PALETTE_COUNT (PALETTE_SHADES * PALLETE_GROUP_COUNT)

#define PALETTE_YMAX_INDEX  0
#define PALETTE_XSIDE_INDEX 1
#define PALETTE_ZSIDE_INDEX 2
#define PALETTE_YMIN_INDEX  3

/* Index into palettes of light colors. */
/* There are 8 different palettes: Four block-face shades for shadowed areas and four block-face shades for sunlit areas. */
/* A palette is a 16x16 color array indexed by a byte where the leftmost 4 bits represent lamplight level and the rightmost 4 bits represent lavalight level */
/* E.G. myPalette[0b_0010_0001] will give us the color for lamp level 2 and lava level 1 (lowest level is 0) */
static PackedCol* palettes[PALETTE_COUNT];

typedef cc_uint8* LightingChunk;
static cc_uint8* chunkLightingDataFlags;
#define CHUNK_UNCALCULATED 0
#define CHUNK_SELF_CALCULATED 1
#define CHUNK_ALL_CALCULATED 2
static LightingChunk* chunkLightingData;

#define MakePaletteIndex(lampLevel, lavaLevel) ((lampLevel << FANCY_LIGHTING_LAMP_SHIFT) | lavaLevel)
/* Fill in a palette with values based on the current light colors, shaded by the given shade value and lightened by the given ambientColor */
static void InitPalette(PackedCol* palette, float shaded, PackedCol ambientColor) {
	PackedCol lavaColor, lampColor;
	int lampLevel, lavaLevel;
	float curLerp;

	for (lampLevel = 0; lampLevel < FANCY_LIGHTING_LEVELS; lampLevel++) {
		for (lavaLevel = 0; lavaLevel < FANCY_LIGHTING_LEVELS; lavaLevel++) {
			if (lampLevel == FANCY_LIGHTING_LEVELS - 1) {
				lampColor = Env.LampLightCol;
			}
			else {
				curLerp = lampLevel / (float)(FANCY_LIGHTING_LEVELS - 1);
				curLerp *= (MATH_PI / 2);
				curLerp = Math_CosF(curLerp);
				lampColor = PackedCol_Lerp(0, Env.LampLightCol, 1 - curLerp);
			}

			curLerp = lavaLevel / (float)(FANCY_LIGHTING_LEVELS - 1);
			curLerp *= (MATH_PI / 2);
			curLerp = Math_CosF(curLerp);

			lavaColor = PackedCol_Lerp(0, Env.LavaLightCol, 1 - curLerp);

			/* Blend the two light colors together, then blend that with the ambient color, then shade that by the face darkness */
			palette[MakePaletteIndex(lampLevel, lavaLevel)] =
				PackedCol_Scale(PackedCol_ScreenBlend(PackedCol_ScreenBlend(lampColor, lavaColor), ambientColor), shaded);
		}
	}
}
/* Genuine Indev palette: entry (lamp, skyStored) = white scaled through the
    genuine lightBrightnessTable curve at max(lamp, effective sky), shaded by
    the face factor (1.0 / 0.8 / 0.6 / 0.5 - the genuine RenderBlocks set).
    skyLit palettes (the sun group, picked for cells above the heightmap)
    ignore the stored nibble and use the current sky level directly - those
    cells are implicit sources and may have no storage at all. */
static void InitPaletteIndev(PackedCol* palette, float shaded, cc_bool skyLit) {
	int lampLevel, skyStored, effSky, light;
	float bright;

	for (lampLevel = 0; lampLevel < FANCY_LIGHTING_LEVELS; lampLevel++) {
		for (skyStored = 0; skyStored < FANCY_LIGHTING_LEVELS; skyStored++) {
			effSky = skyLit ? indevSkyLevel
			                : skyStored - (FANCY_LIGHTING_MAX_LEVEL - indevSkyLevel);
			if (effSky < 0) effSky = 0;
			light  = lampLevel > effSky ? lampLevel : effSky;
			bright = IndevTest_BrightnessOfLight(light) * shaded;

			palette[MakePaletteIndex(lampLevel, skyStored)] =
				PackedCol_Scale(PACKEDCOL_WHITE, bright);
		}
	}
}

static void InitPalettes(void) {
	int i;
	for (i = 0; i < PALETTE_COUNT; i++) {
		palettes[i] = (PackedCol*)Mem_Alloc(FANCY_LIGHTING_LEVELS * FANCY_LIGHTING_LEVELS, sizeof(PackedCol), "light color palette");
	}
	i = 0;
	if (indevSky) {
		InitPaletteIndev(palettes[i + PALETTE_YMAX_INDEX],  1,                    false);
		InitPaletteIndev(palettes[i + PALETTE_XSIDE_INDEX], PACKEDCOL_SHADE_X,    false);
		InitPaletteIndev(palettes[i + PALETTE_ZSIDE_INDEX], PACKEDCOL_SHADE_Z,    false);
		InitPaletteIndev(palettes[i + PALETTE_YMIN_INDEX],  PACKEDCOL_SHADE_YMIN, false);
		i += PALETTE_SHADES;
		InitPaletteIndev(palettes[i + PALETTE_YMAX_INDEX],  1,                    true);
		InitPaletteIndev(palettes[i + PALETTE_XSIDE_INDEX], PACKEDCOL_SHADE_X,    true);
		InitPaletteIndev(palettes[i + PALETTE_ZSIDE_INDEX], PACKEDCOL_SHADE_Z,    true);
		InitPaletteIndev(palettes[i + PALETTE_YMIN_INDEX],  PACKEDCOL_SHADE_YMIN, true);
		return;
	}
	InitPalette(palettes[i + PALETTE_YMAX_INDEX],  1,                    Env.ShadowCol);
	InitPalette(palettes[i + PALETTE_XSIDE_INDEX], PACKEDCOL_SHADE_X,    Env.ShadowCol);
	InitPalette(palettes[i + PALETTE_ZSIDE_INDEX], PACKEDCOL_SHADE_Z,    Env.ShadowCol);
	InitPalette(palettes[i + PALETTE_YMIN_INDEX],  PACKEDCOL_SHADE_YMIN, Env.ShadowCol);
	i += PALETTE_SHADES;
	InitPalette(palettes[i + PALETTE_YMAX_INDEX],  1,                    Env.SunCol);
	InitPalette(palettes[i + PALETTE_XSIDE_INDEX], PACKEDCOL_SHADE_X,    Env.SunCol);
	InitPalette(palettes[i + PALETTE_ZSIDE_INDEX], PACKEDCOL_SHADE_Z,    Env.SunCol);
	InitPalette(palettes[i + PALETTE_YMIN_INDEX],  PACKEDCOL_SHADE_YMIN, Env.SunCol);
}
static void FreePalettes(void) {
	int i;
	for (i = 0; i < PALETTE_COUNT; i++) {
		Mem_Free(palettes[i]);
	}
}

static int chunksCount;
static void AllocState(void) {
	/* Whether this map uses the genuine Indev flooded-sky model - resolved
	    per lighting (re)alloc so map changes and mode flips pick it up */
	indevSky = IndevTest_Enabled;

	ClassicLighting_AllocState();
	InitPalettes();
	chunksCount = World.ChunksCount;

	chunkLightingDataFlags = (cc_uint8*)Mem_AllocCleared(chunksCount, sizeof(cc_uint8), "light flags");
	chunkLightingData = (LightingChunk*)Mem_AllocCleared(chunksCount, sizeof(LightingChunk), "light chunks");
	Queue_Init(&lightQueue, sizeof(struct LightNode));
	Queue_Init(&unlightQueue, sizeof(struct LightNode));
}

static void FreeState(void) {
	int i;
	ClassicLighting_FreeState();
	
	/* This function can be called multiple times without calling AllocState, so... */
	if (!chunkLightingDataFlags) return;

	FreePalettes();

	for (i = 0; i < chunksCount; i++) {
		Mem_Free(chunkLightingData[i]);
	}

	Mem_Free(chunkLightingDataFlags);
	Mem_Free(chunkLightingData);
	chunkLightingDataFlags = NULL;
	chunkLightingData = NULL;
	Queue_Clear(&lightQueue);
	Queue_Clear(&unlightQueue);
}

/* Converts chunk x/y/z coordinates to the corresponding index in chunks array/list */
#define ChunkCoordsToIndex(cx, cy, cz) (((cy) * World.ChunksZ + (cz)) * World.ChunksX + (cx))
/* Converts local x/y/z coordinates to the corresponding index in a chunk */
#define LocalCoordsToIndex(lx, ly, lz) ((lx) | ((lz) << CHUNK_SHIFT) | ((ly) << (CHUNK_SHIFT * 2)))
/* Converts global x/y/z coordinates to the corresponding index in a chunk */
#define GlobalCoordsToChunkCoordsIndex(x, y, z) (LocalCoordsToIndex(x & CHUNK_MASK, y & CHUNK_MASK, z & CHUNK_MASK))

/* Sets the light level at this cell. Does NOT check that the cell is in bounds. */
static void SetBrightness(cc_uint8 brightness, int x, int y, int z, cc_bool isLamp, cc_bool refreshChunk) {
	cc_uint8 clearMask, shift = isLamp ? FANCY_LIGHTING_LAMP_SHIFT : 0, prevValue;
	int cx = x >> CHUNK_SHIFT, lx = x & CHUNK_MASK;
	int cy = y >> CHUNK_SHIFT, ly = y & CHUNK_MASK;
	int cz = z >> CHUNK_SHIFT, lz = z & CHUNK_MASK;
	int chunkIndex = ChunkCoordsToIndex(cx, cy, cz);
	int localIndex = LocalCoordsToIndex(lx, ly, lz);

	if (chunkLightingData[chunkIndex] == NULL) {
		chunkLightingData[chunkIndex] = (cc_uint8*)Mem_TryAllocCleared(CHUNK_SIZE_3, sizeof(cc_uint8));
	}

	/* 00001111 if lamp, otherwise 11110000*/
	clearMask = ~(FANCY_LIGHTING_MAX_LEVEL << shift);

	if (refreshChunk) {
		prevValue = chunkLightingData[chunkIndex][localIndex];

		chunkLightingData[chunkIndex][localIndex] &= clearMask;
		chunkLightingData[chunkIndex][localIndex] |= brightness << shift;

		/* There is no reason to refresh current chunk as the builder does that automatically */
		if (prevValue != chunkLightingData[chunkIndex][localIndex]) {
			if (lx == CHUNK_MAX) MapRenderer_RefreshChunk(cx + 1, cy, cz);
			if (lx == 0)         MapRenderer_RefreshChunk(cx - 1, cy, cz);
			if (ly == CHUNK_MAX) MapRenderer_RefreshChunk(cx, cy + 1, cz);
			if (ly == 0)         MapRenderer_RefreshChunk(cx, cy - 1, cz);
			if (lz == CHUNK_MAX) MapRenderer_RefreshChunk(cx, cy, cz + 1);
			if (lz == 0)         MapRenderer_RefreshChunk(cx, cy, cz - 1);
		}
	}
	else {
		chunkLightingData[chunkIndex][localIndex] &= clearMask;
		chunkLightingData[chunkIndex][localIndex] |= brightness << shift;
	}
}
/* Returns the light level at this cell. Does NOT check that the cell is in bounds. */
static cc_uint8 GetBrightness(int x, int y, int z, cc_bool isLamp) {
	int cx = x >> CHUNK_SHIFT, lx = x & CHUNK_MASK;
	int cy = y >> CHUNK_SHIFT, ly = y & CHUNK_MASK;
	int cz = z >> CHUNK_SHIFT, lz = z & CHUNK_MASK;
	int chunkIndex = ChunkCoordsToIndex(cx, cy, cz), localIndex;

	if (chunkLightingData[chunkIndex] == NULL) { return 0; }
	localIndex = LocalCoordsToIndex(lx, ly, lz);

	return isLamp ?
		chunkLightingData[chunkIndex][localIndex] >> FANCY_LIGHTING_LAMP_SHIFT :
		chunkLightingData[chunkIndex][localIndex] & FANCY_LIGHTING_MAX_LEVEL;
}


/* Light can never pass through a block that's full sized and blocks light */
/* We can assume a block is full sized if none of the LightOffset flags are 0 */
#define IsFullOpaque(thisBlock) (Blocks.BlocksLight[thisBlock] && Blocks.LightOffset[thisBlock] == 0xFF)

/* If it's not opaque and it doesn't block light, or it is brighter than 0, we can always pass through */
/* Light can always pass through leaves and water */
#define IsFullTransparent(thisBlock)\
(\
(Blocks.Draw[thisBlock] > DRAW_OPAQUE && !Blocks.BlocksLight[thisBlock]) || \
Blocks.Draw[thisBlock] == DRAW_TRANSPARENT_THICK || \
Blocks.Draw[thisBlock] == DRAW_TRANSLUCENT\
)

static cc_bool CanLightPass(BlockID thisBlock, Face face) {
	if (IsFullTransparent(thisBlock)) { return true; }
	if (Blocks.Brightness[thisBlock]) { return true; }
	if (IsFullOpaque(thisBlock)) { return false; }
	/* Is stone's face hidden by thisBlock? TODO: Don't hardcode using stone */
	return !Block_IsFaceHidden(BLOCK_STONE, thisBlock, face);
}

#define Light_TrySpreadInto(axis, AXIS, dir, limit, isLamp, thisFace, thatFace) \
	if (ln.coords.axis dir ## = limit && \
		CanLightPass(thisBlock, FACE_ ## AXIS ## thisFace) && \
		CanLightPass(World_GetBlock(ln.coords.x, ln.coords.y, ln.coords.z), FACE_ ## AXIS ## thatFace) && \
		GetBrightness(ln.coords.x, ln.coords.y, ln.coords.z, isLamp) < ln.brightness) { \
		Queue_Enqueue(&lightQueue, &ln); \
	} \

static void FlushLightQueue(cc_bool isLamp, cc_bool refreshChunk) {
	struct LightNode ln;
	cc_uint8 brightnessHere;
	BlockID thisBlock;

	while (lightQueue.count > 0) {
		ln = *(struct LightNode*)(Queue_Dequeue(&lightQueue));

		thisBlock = World_GetBlock(ln.coords.x, ln.coords.y, ln.coords.z);
		/* Genuine Light.java attenuates by the RECEIVING cell's lightOpacity
		    (minimum 1): water is 3, so light entering a water cell pays 2 on
		    top of the standard 1 the sender already subtracted. */
		if (indevSky && (thisBlock == BLOCK_WATER || thisBlock == BLOCK_STILL_WATER)) {
			ln.brightness = ln.brightness > 2 ? ln.brightness - 2 : 0;
		}

		brightnessHere = GetBrightness(ln.coords.x, ln.coords.y, ln.coords.z, isLamp);

		/* If this cell is already more lit, we can assume this cell and its neighbors have been accounted for */
		if (brightnessHere >= ln.brightness) { continue; }
		if (ln.brightness == 0) { continue; }

		SetBrightness(ln.brightness, ln.coords.x, ln.coords.y, ln.coords.z, isLamp, refreshChunk);

		ln.brightness--;
		if (ln.brightness == 0) continue;

		ln.coords.x--;
		Light_TrySpreadInto(x, X, > , 0, isLamp, MAX, MIN)
		ln.coords.x += 2;
		Light_TrySpreadInto(x, X, < , World.MaxX, isLamp, MIN, MAX)
		ln.coords.x--;

		ln.coords.y--;
		Light_TrySpreadInto(y, Y, >, 0, isLamp, MAX, MIN)
		ln.coords.y += 2;
		Light_TrySpreadInto(y, Y, <, World.MaxY, isLamp, MIN, MAX)
		ln.coords.y--;

		ln.coords.z--;
		Light_TrySpreadInto(z, Z, > , 0, isLamp, MAX, MIN)
		ln.coords.z += 2;
		Light_TrySpreadInto(z, Z, < , World.MaxZ, isLamp, MIN, MAX)
	}
}

cc_uint8 GetBlockBrightness(BlockID curBlock, cc_bool isLamp) {
	if (isLamp) return Blocks.Brightness[curBlock] >> FANCY_LIGHTING_LAMP_SHIFT;
	return Blocks.Brightness[curBlock] & FANCY_LIGHTING_MAX_LEVEL;
}

#define LightNode_Init(node, X, Y, Z, bright) \
	node.coords.x = X; node.coords.y = Y; node.coords.z = Z; node.brightness = bright;

/* Enqueues the sky SOURCE seeds for one column, restricted to [yMin, yMax]:
    the bottom-most sky cell (spreads down into the column top / caves) and
    every sky cell with a non-sky horizontal neighbor (cliff/cave-mouth faces,
    i.e. y at or below some neighbor column's heightmap). Cells above every
    neighbouring heightmap border only sky and spread nothing useful. */
static void Indev_SeedSkyColumn(int x, int z, int yMin, int yMax) {
	struct LightNode entry;
	int h = ClassicLighting_GetLightHeight(x, z);
	int top = h, y, hn;

	if (x > 0)            { hn = ClassicLighting_GetLightHeight(x - 1, z); if (hn > top) top = hn; }
	if (x < World.MaxX)   { hn = ClassicLighting_GetLightHeight(x + 1, z); if (hn > top) top = hn; }
	if (z > 0)            { hn = ClassicLighting_GetLightHeight(x, z - 1); if (hn > top) top = hn; }
	if (z < World.MaxZ)   { hn = ClassicLighting_GetLightHeight(x, z + 1); if (hn > top) top = hn; }

	if (top < h + 1)        top = h + 1; /* always seed the column-bottom sky cell */
	if (top > World.MaxY)   top = World.MaxY;
	if (yMin < h + 1)       yMin = h + 1;
	if (yMax > top)         yMax = top;

	for (y = yMin; y <= yMax; y++) {
		LightNode_Init(entry, x, y, z, FANCY_LIGHTING_MAX_LEVEL);
		Queue_Enqueue(&lightQueue, &entry);
	}
}

static void CalculateChunkLightingSelf(int chunkIndex, int cx, int cy, int cz) {
	int x, y, z;
	/* Block coordinates */
	int chunkStartX, chunkStartY, chunkStartZ, chunkEndX, chunkEndY, chunkEndZ;
	cc_uint8 brightness;
	BlockID curBlock;
	struct LightNode entry;

	chunkStartX = cx * CHUNK_SIZE;
	chunkStartY = cy * CHUNK_SIZE;
	chunkStartZ = cz * CHUNK_SIZE;
	chunkEndX = chunkStartX + CHUNK_SIZE;
	chunkEndY = chunkStartY + CHUNK_SIZE;
	chunkEndZ = chunkStartZ + CHUNK_SIZE;

	if (chunkEndX > World.Width ) { chunkEndX = World.Width;  }
	if (chunkEndY > World.Height) { chunkEndY = World.Height; }
	if (chunkEndZ > World.Length) { chunkEndZ = World.Length; }

	for (y = chunkStartY; y < chunkEndY; y++) {
		for (z = chunkStartZ; z < chunkEndZ; z++) {
			for (x = chunkStartX; x < chunkEndX; x++) {

				curBlock = World_GetBlock(x, y, z);
				
				if (Blocks.Brightness[curBlock] > 0) {

					brightness = GetBlockBrightness(curBlock, false);

					if (brightness > 0) {
						LightNode_Init(entry, x, y, z, brightness);
						Queue_Enqueue(&lightQueue, &entry);
						FlushLightQueue(false, false);
					}
					else {
						/* If no lava brightness, it must use lamp brightness */
						brightness = Blocks.Brightness[curBlock] >> FANCY_LIGHTING_LAMP_SHIFT;
						LightNode_Init(entry, x, y, z, brightness);
						Queue_Enqueue(&lightQueue, &entry);
						FlushLightQueue(true, false);
					}
				}

				/* Note: This code only deals with generating light from block sources.
				Regular sun light is added on as a "post process" step when returning light color in the exposed API.
				This has the added benefit of being able to skip allocating chunk lighting data in regions that have no light-casting blocks*/
			}
		}
	}

	/* Indev: sky is a real flooded channel - seed this chunk's columns' sky
	    sources (boundary cells only) and flood them into caves/overhangs.
	    Above-heightmap cells stay implicit via the sun palette group. */
	if (indevSky) {
		for (z = chunkStartZ; z < chunkEndZ; z++) {
			for (x = chunkStartX; x < chunkEndX; x++) {
				Indev_SeedSkyColumn(x, z, chunkStartY, chunkEndY - 1);
			}
		}
		FlushLightQueue(false, false);
	}

	chunkLightingDataFlags[chunkIndex] = CHUNK_SELF_CALCULATED;
}

static void CalculateChunkLightingAll(int chunkIndex, int cx, int cy, int cz) {
	int x, y, z;
	/* Chunk coordinates */
	int chunkStartX, chunkStartY, chunkStartZ;
	int chunkEndX, chunkEndY, chunkEndZ;
	int curChunkIndex;

	chunkStartX = cx - 1;
	chunkStartY = cy - 1;
	chunkStartZ = cz - 1;
	chunkEndX = cx + 1;
	chunkEndY = cy + 1;
	chunkEndZ = cz + 1;

	if (chunkStartX == -1) { chunkStartX++; }
	if (chunkStartY == -1) { chunkStartY++; }
	if (chunkStartZ == -1) { chunkStartZ++; }
	if (chunkEndX == World.ChunksX) { chunkEndX--; }
	if (chunkEndY == World.ChunksY) { chunkEndY--; }
	if (chunkEndZ == World.ChunksZ) { chunkEndZ--; }

	for (y = chunkStartY; y <= chunkEndY; y++) {
		for (z = chunkStartZ; z <= chunkEndZ; z++) {
			for (x = chunkStartX; x <= chunkEndX; x++) {
				curChunkIndex = ChunkCoordsToIndex(x, y, z);

				if (chunkLightingDataFlags[curChunkIndex] == CHUNK_UNCALCULATED) {
					CalculateChunkLightingSelf(curChunkIndex, x, y, z);
				}
			}
		}
	}
	chunkLightingDataFlags[chunkIndex] = CHUNK_ALL_CALCULATED;
}


#define Light_TryUnSpreadInto(axis, dir, limit, AXIS, thisFace, thatFace) \
		if (neighborCoords.axis dir ## = limit && \
			CanLightPass(thisBlock, FACE_ ## AXIS ## thisFace) && \
			CanLightPass(World_GetBlock(neighborCoords.x, neighborCoords.y, neighborCoords.z), FACE_ ## AXIS ## thatFace) \
		) \
		{ \
			neighborBrightness = GetBrightness(neighborCoords.x, neighborCoords.y, neighborCoords.z, isLamp); \
			neighborBlockBrightness = GetBlockBrightness(World_GetBlock(neighborCoords.x, neighborCoords.y, neighborCoords.z), isLamp); \
			/* This spot is a light caster, mark this spot as needing to be re-spread */ \
			if (neighborBlockBrightness > 0) { \
				LightNode_Init(otherNode, neighborCoords.x, neighborCoords.y, neighborCoords.z, neighborBlockBrightness); \
				Queue_Enqueue(&lightQueue, &otherNode); \
			} \
			if (neighborBrightness > 0) { \
				/* This neighbor is darker than cur spot, darken it*/ \
				if (neighborBrightness < curNode.brightness) { \
					SetBrightness(0, neighborCoords.x, neighborCoords.y, neighborCoords.z, isLamp, true); \
					LightNode_Init(otherNode, neighborCoords.x, neighborCoords.y, neighborCoords.z, neighborBrightness); \
					Queue_Enqueue(&unlightQueue, &otherNode); \
				} \
				/* This neighbor is brighter or same, mark this spot as needing to be re-spread */ \
				else { \
					/* But only if the neighbor actually *can* spread to this block */ \
					if ( \
						CanLightPass(thisBlockTrue, FACE_ ## AXIS ## thisFace) && \
						CanLightPass(World_GetBlock(neighborCoords.x, neighborCoords.y, neighborCoords.z), FACE_ ## AXIS ## thatFace) \
					) \
					{ \
						otherNode = curNode; \
						otherNode.brightness = neighborBrightness-1; \
						Queue_Enqueue(&lightQueue, &otherNode); \
					} \
				} \
			} \
		} \

/* Spreads darkness out from this point and relights any necessary areas afterward */
static void CalcUnlight(int x, int y, int z, cc_uint8 brightness, cc_bool isLamp) {
	int count = 0;
	struct LightNode curNode, otherNode;
	cc_uint8 neighborBrightness, neighborBlockBrightness;
	IVec3 neighborCoords;
	BlockID thisBlockTrue, thisBlock;

	SetBrightness(0, x, y, z, isLamp, true);
	LightNode_Init(curNode, x, y, z, brightness);
	Queue_Enqueue(&unlightQueue, &curNode);

	while (unlightQueue.count > 0) {
		curNode = *(struct LightNode*)(Queue_Dequeue(&unlightQueue));
		neighborCoords = curNode.coords;

		thisBlockTrue = World_GetBlock(neighborCoords.x, neighborCoords.y, neighborCoords.z);
		/* For the original cell in the queue, assume this block is air
		so that light can unspread "out" of it in the case of a solid blocks. */
		thisBlock = count == 0 ? BLOCK_AIR : thisBlockTrue;

		count++;

		neighborCoords.x--;
		Light_TryUnSpreadInto(x, >, 0, X, MAX, MIN)
		neighborCoords.x += 2;
		Light_TryUnSpreadInto(x, <, World.MaxX, X, MIN, MAX)
		neighborCoords.x--;

		neighborCoords.y--;
		Light_TryUnSpreadInto(y, >, 0, Y, MAX, MIN)
		neighborCoords.y += 2;
		Light_TryUnSpreadInto(y, <, World.MaxY, Y, MIN, MAX)
		neighborCoords.y--;

		neighborCoords.z--;
		Light_TryUnSpreadInto(z, >, 0, Z, MAX, MIN)
		neighborCoords.z += 2;
		Light_TryUnSpreadInto(z, <, World.MaxZ, Z, MIN, MAX)
	}

	FlushLightQueue(isLamp, true);
}
static void CalcBlockChange(int x, int y, int z, BlockID oldBlock, BlockID newBlock, cc_bool isLamp) {
	cc_uint8 oldBlockLightLevel = GetBlockBrightness(oldBlock, isLamp);
	cc_uint8 newBlockLightLevel = GetBlockBrightness(newBlock, isLamp);
	cc_uint8 oldLightLevelHere = GetBrightness(x, y, z, isLamp);
	struct LightNode entry;

	/* Cell has no lighting and new block doesn't cast light and blocks all light, no change */
	if (!oldLightLevelHere && !newBlockLightLevel && IsFullOpaque(newBlock)) return;

	/* Cell is darker than the new block, only brighter case */
	if (oldLightLevelHere < newBlockLightLevel) {
		/* brighten this spot, recalculate lighting */
		LightNode_Init(entry, x, y, z, newBlockLightLevel);
		Queue_Enqueue(&lightQueue, &entry);
		FlushLightQueue(isLamp, true);
		return;
	}

	/* Light passes through old and new, old block does not cast light, new block does not cast light; no change */
	if (IsFullTransparent(oldBlock) && IsFullTransparent(newBlock) && !oldBlockLightLevel && !newBlockLightLevel) return;

	CalcUnlight(x, y, z, oldLightLevelHere, isLamp);
}
/* Indev sky maintenance after a block change: columns whose heightmap moved
    exchange implicit-sun status with stored/flooded status, and the seed
    boundary shifts. Unlight cells that lost the sky, then re-seed the five
    affected columns and flood - the unlight machinery relights the rest. */
static void Indev_SkyBlockChanged(int x, int y, int z, int oldH, int newH) {
	int yy;
	cc_uint8 stored;

	if (newH > oldH) {
		/* Heightmap rose: cells in (oldH, newH] lost their implicit sun.
		    Boundary cells among them carry stored 15s that were spread into
		    caves - unlight those; the placed cell itself was handled by the
		    generic CalcBlockChange pass. oldH is the -10 empty-column
		    sentinel when this is the column's FIRST block (floating maps'
		    open void) - clamp the walk at y=0 or it reads lighting data at
		    negative y (user crash: placing over the void on a deep
		    floating map). */
		for (yy = newH; yy > oldH && yy >= 0; yy--) {
			if (yy == y) continue;
			stored = GetBrightness(x, yy, z, false);
			if (stored > 0) CalcUnlight(x, yy, z, stored, false);
		}
	}

	Indev_SeedSkyColumn(x, z, 0, World.MaxY);
	if (x > 0)          Indev_SeedSkyColumn(x - 1, z, 0, World.MaxY);
	if (x < World.MaxX) Indev_SeedSkyColumn(x + 1, z, 0, World.MaxY);
	if (z > 0)          Indev_SeedSkyColumn(x, z - 1, 0, World.MaxY);
	if (z < World.MaxZ) Indev_SeedSkyColumn(x, z + 1, 0, World.MaxY);
	FlushLightQueue(false, true);
}

static void OnBlockChanged(int x, int y, int z, BlockID oldBlock, BlockID newBlock) {
	int oldH, newH;
	/* For some reason this is a possible case */
	if (oldBlock == newBlock) { return; }

	oldH = indevSky ? ClassicLighting_GetLightHeight(x, z) : 0;
	ClassicLighting_OnBlockChanged(x, y, z, oldBlock, newBlock);

	CalcBlockChange(x, y, z, oldBlock, newBlock, false);
	CalcBlockChange(x, y, z, oldBlock, newBlock, true);

	if (indevSky) {
		newH = ClassicLighting_GetLightHeight(x, z);
		Indev_SkyBlockChanged(x, y, z, oldH, newH);
	}
}
/* Invalidates/Resets lighting state for all of the blocks in the world */
/*  (e.g. because a block changed whether it is full bright or not) */
static void Refresh(void) {
	ClassicLighting_Refresh();
	FreeState();
	AllocState();
}
static cc_bool IsLit(int x, int y, int z) { return ClassicLighting_IsLit(x, y, z); }
static cc_bool IsLit_Fast(int x, int y, int z) { return ClassicLighting_IsLit_Fast(x, y, z); }

#define CalcForChunkIfNeeded(cx, cy, cz, chunkIndex) \
	if (chunkLightingDataFlags[chunkIndex] < CHUNK_ALL_CALCULATED) { \
		CalculateChunkLightingAll(chunkIndex, cx, cy, cz); \
	}

/* The block-emitted light level (0-15) at a position: the stronger of the
    cached lamp/lava nibbles. Sky light is NOT included - callers combine it
    with the sky heightmap + time of day themselves (see Lighting.IsLit).
    Only meaningful while fancy lighting is the active mode. */
/* Genuine Indev World.getBlockLightValue: the combined 0..15 light at a
    position - max of the lamp level and the effective sky (implicit current
    level above the heightmap, stored-flood minus the day/night subtraction
    below it). Drives crop growth, monster spawn/burn rules, grass. */
int FancyLighting_IndevLight(int x, int y, int z) {
	cc_uint8 lightData = 0;
	int cx, cy, cz, chunkIndex, lamp, effSky;
	if (!World_Contains(x, y, z) || !chunkLightingData) return indevSkyLevel;

	cx = x >> CHUNK_SHIFT;
	cy = y >> CHUNK_SHIFT;
	cz = z >> CHUNK_SHIFT;
	chunkIndex = ChunkCoordsToIndex(cx, cy, cz);
	CalcForChunkIfNeeded(cx, cy, cz, chunkIndex);

	if (chunkLightingData[chunkIndex] != NULL) {
		lightData = chunkLightingData[chunkIndex][GlobalCoordsToChunkCoordsIndex(x, y, z)];
	}
	lamp = lightData >> FANCY_LIGHTING_LAMP_SHIFT;

	if (y > ClassicLighting_GetLightHeight(x, z)) {
		/* Implicit sky source: the CURRENT eased level, never the stored
		    nibble (a boundary cell stores its full-day 15). */
		effSky = indevSkyLevel;
	} else {
		effSky = (lightData & FANCY_LIGHTING_MAX_LEVEL) - (FANCY_LIGHTING_MAX_LEVEL - indevSkyLevel);
		if (effSky < 0) effSky = 0;
	}
	return lamp > effSky ? lamp : effSky;
}

int FancyLighting_BlockLightLevel(int x, int y, int z) {
	cc_uint8 lightData;
	int cx, cy, cz, chunkIndex, lamp, lava;
	if (!World_Contains(x, y, z) || !chunkLightingData) return 0;

	cx = x >> CHUNK_SHIFT;
	cy = y >> CHUNK_SHIFT;
	cz = z >> CHUNK_SHIFT;
	chunkIndex = ChunkCoordsToIndex(cx, cy, cz);
	CalcForChunkIfNeeded(cx, cy, cz, chunkIndex);

	if (chunkLightingData[chunkIndex] == NULL) return 0;
	lightData = chunkLightingData[chunkIndex][GlobalCoordsToChunkCoordsIndex(x, y, z)];
	lamp = lightData >> FANCY_LIGHTING_LAMP_SHIFT;
	lava = lightData &  FANCY_LIGHTING_MAX_LEVEL;
	return lamp > lava ? lamp : lava;
}

static PackedCol Color_Core(int x, int y, int z, int paletteFace) {
	cc_uint8 lightData;
	int cx, cy, cz, chunkIndex;
	int chunkCoordsIndex;

	cx = x >> CHUNK_SHIFT;
	cy = y >> CHUNK_SHIFT;
	cz = z >> CHUNK_SHIFT;

	chunkIndex = ChunkCoordsToIndex(cx, cy, cz);
	CalcForChunkIfNeeded(cx, cy, cz, chunkIndex);

	/* There might be no light data in this chunk even after it was calculated */
	if (chunkLightingData[chunkIndex] == NULL) {
		lightData = 0;
	} else {
		chunkCoordsIndex = GlobalCoordsToChunkCoordsIndex(x, y, z);
		lightData = chunkLightingData[chunkIndex][chunkCoordsIndex];
	}

	/* This cell is exposed to sunlight */
	if (y > ClassicLighting_GetLightHeight(x, z)) {
		/* Push the pointer forward into the sun lit palette section */
		paletteFace += PALETTE_SHADES;
	}

	return palettes[paletteFace][lightData];
}

#define TRY_OOB_CASE(sun, shadow) if (!World_Contains(x, y, z)) return y >= Env.EdgeHeight ? sun : shadow
static PackedCol Color(int x, int y, int z) {
	TRY_OOB_CASE(Env.SunCol, Env.ShadowCol);
	return Color_Core(x, y, z, PALETTE_YMAX_INDEX);
}
static PackedCol Color_YMaxSide(int x, int y, int z) {
	TRY_OOB_CASE(Env.SunCol, Env.ShadowCol);
	return Color_Core(x, y, z, PALETTE_YMAX_INDEX);
}
static PackedCol Color_YMinSide(int x, int y, int z) {
	TRY_OOB_CASE(Env.SunYMin, Env.ShadowYMin);
	return Color_Core(x, y, z, PALETTE_YMIN_INDEX);
}
static PackedCol Color_XSide(int x, int y, int z) {
	TRY_OOB_CASE(Env.SunXSide, Env.ShadowXSide);
	return Color_Core(x, y, z, PALETTE_XSIDE_INDEX);
}
static PackedCol Color_ZSide(int x, int y, int z) {
	TRY_OOB_CASE(Env.SunZSide, Env.ShadowZSide);
	return Color_Core(x, y, z, PALETTE_ZSIDE_INDEX);
}

static void LightHint(int startX, int startY, int startZ) {
	int cx, cy, cz, chunkIndex;
	ClassicLighting_LightHint(startX, startY, startZ);
	/* Add 1 to startX/Z, as coordinates are for the extended chunk (18x18x18) */
	startX++; startY++; startZ++;

	cx = (startX + HALF_CHUNK_SIZE) >> CHUNK_SHIFT;
	cy = (startY + HALF_CHUNK_SIZE) >> CHUNK_SHIFT;
	cz = (startZ + HALF_CHUNK_SIZE) >> CHUNK_SHIFT;

	chunkIndex = ChunkCoordsToIndex(cx, cy, cz);
	CalcForChunkIfNeeded(cx, cy, cz, chunkIndex);
}

void FancyLighting_SetActive(void) {
	Lighting.OnBlockChanged = OnBlockChanged;
	Lighting.Refresh = Refresh;
	Lighting.IsLit = IsLit;
	Lighting.Color = Color;
	Lighting.Color_XSide = Color_XSide;

	Lighting.IsLit_Fast = IsLit_Fast;
	Lighting.Color_Sprite_Fast = Color;
	Lighting.Color_YMax_Fast   = Color;
	Lighting.Color_YMin_Fast   = Color_YMinSide;
	Lighting.Color_XSide_Fast  = Color_XSide;
	Lighting.Color_ZSide_Fast  = Color_ZSide;

	Lighting.FreeState  = FreeState;
	Lighting.AllocState = AllocState;
	Lighting.LightHint  = LightHint;
}

static void OnEnvVariableChanged(void* obj, int envVar) {
	/* This is always called, but should only do anything if fancy lighting is on */
	if (Lighting_Mode == LIGHTING_MODE_CLASSIC) { return; }

	if (envVar == ENV_VAR_SUN_COLOR || envVar == ENV_VAR_SHADOW_COLOR || envVar == ENV_VAR_LAVALIGHT_COLOR || envVar == ENV_VAR_LAMPLIGHT_COLOR) {
		InitPalettes();
	}
	if (envVar == ENV_VAR_LAVALIGHT_COLOR || envVar == ENV_VAR_LAMPLIGHT_COLOR) MapRenderer_Refresh();
}

void FancyLighting_OnInit(void) {
	Event_Register_(&WorldEvents.EnvVarChanged, NULL, OnEnvVariableChanged);
}
