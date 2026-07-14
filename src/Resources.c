#include "Resources.h"
#ifdef CC_BUILD_RESOURCES
#include "Funcs.h"
#include "String_.h"
#include "Constants.h"
#include "Deflate.h"
#include "Stream.h"
#include "Platform.h"
#include "Launcher.h"
#include "Utils.h"
#include "Vorbis.h"
#include "Errors.h"
#include "Logger.h"
#include "LWeb.h"
#include "Http.h"
#include "Game.h"
#include "Audio.h"

/* Represents a set of assets/resources */
/* E.g. music set, sounds set, textures set */
struct AssetSet {
	/* Checks whether all assets in this asset set exist on disc */
	void (*CheckExistence)(void);
	/* Counts the number of size of missing assets that need to be downloaded */
	void (*CountMissing)(void);
	/* Begins asynchronously downloading the missing assets in this asset set */
	void (*DownloadAssets)(void);
	/* Returns the name for the associated request, or NULL otherwise */
	const char* (*GetRequestName)(int reqID);
	/* Checks if any assets have been downloaded, and processes them if so */
	void (*CheckStatus)(void);
	/* Resets state and frees and allocated memory */
	void (*ResetState)(void);
};

int Resources_MissingCount, Resources_MissingSize;
cc_bool Resources_MissingRequired;

union ResourceValue {
	cc_uint8* data;
	struct Bitmap bmp;
};
struct ResourceZipEntry {
	const char* filename;
	/* zip data */
	cc_uint32 type : 3;
	cc_uint32 size : 29;
	union ResourceValue value;
	cc_uint32 offset, crc32;
};
#define RESOURCE_TYPE_DATA  1
#define RESOURCE_TYPE_PNG   2
#define RESOURCE_TYPE_CONST 3
#define RESOURCE_TYPE_SOUND 4

static CC_NOINLINE cc_bool Fetcher_Get(int reqID, struct HttpRequest* item);
CC_NOINLINE static struct ResourceZipEntry* ZipEntries_Find(const cc_string* name);
static cc_result ZipEntry_ExtractData(struct ResourceZipEntry* e, struct Stream* data, struct ZipEntry* source);


/*########################################################################################################################*
*------------------------------------------------------Utility functions -------------------------------------------------*
*#########################################################################################################################*/
static void ZipFile_InspectEntries(const cc_string* path, Zip_SelectEntry selector) {
	struct Stream stream;
	cc_result res;
	struct ZipEntry entries[64];
	cc_filepath raw_path;

	Platform_EncodePath(&raw_path, path);
	res = Stream_OpenPath(&stream, &raw_path);

	if (ReturnCode_IsNotFound(res)) return;
	if (res) { Logger_IOWarn2(res, "opening", &raw_path); return; }

	res = Zip_Extract(&stream, selector, NULL, 
						entries, Array_Elems(entries));
	if (res) Logger_IOWarn2(res, "inspecting", &raw_path);

	/* No point logging error for closing readonly file */
	(void)stream.Close(&stream);
}

static cc_result ZipEntry_ExtractData(struct ResourceZipEntry* e, struct Stream* data, struct ZipEntry* source) {
	cc_uint32 size = source->UncompressedSize;
	e->value.data  = (cc_uint8*)Mem_TryAlloc(size, 1);
	e->size        = size;

	if (!e->value.data) return ERR_OUT_OF_MEMORY;
	return Stream_Read(data, e->value.data, size);
}


/*########################################################################################################################*
*------------------------------------------------------Zip entry writer---------------------------------------------------*
*#########################################################################################################################*/
static void GetCurrentZipDate(int* modTime, int* modDate) {
	struct cc_datetime now;
	DateTime_CurrentLocal(&now);

	*modTime = (now.second / 2) | (now.minute << 5) | (now.hour << 11);
	*modDate = (now.day) | (now.month << 5) | ((now.year - 1980) << 9);
}

static cc_result ZipWriter_LocalFile(struct Stream* s, struct ResourceZipEntry* e) {
	int filenameLen = String_Length(e->filename);
	cc_uint8 header[30 + STRING_SIZE];
	cc_result res;
	int modTime, modDate;

	GetCurrentZipDate(&modTime, &modDate);
	if ((res = s->Position(s, &e->offset))) return res;

	Mem_WriteU32_LE(header + 0,  0x04034b50);  /* signature */
	Mem_WriteU16_LE(header + 4,  20);          /* version needed */
	Mem_WriteU16_LE(header + 6,  0);           /* bitflags */
	Mem_WriteU16_LE(header + 8,  0);           /* compression method */
	Mem_WriteU16_LE(header + 10, modTime);     /* last modified */
	Mem_WriteU16_LE(header + 12, modDate);     /* last modified */
	
	Mem_WriteU32_LE(header + 14, e->crc32);    /* CRC32 */
	Mem_WriteU32_LE(header + 18, e->size);     /* Compressed size */
	Mem_WriteU32_LE(header + 22, e->size);     /* Uncompressed size */
	 
	Mem_WriteU16_LE(header + 26, filenameLen); /* name length */
	Mem_WriteU16_LE(header + 28, 0);           /* extra field length */

	Mem_Copy(header + 30, e->filename, filenameLen);
	return Stream_Write(s, header, 30 + filenameLen);
}

static cc_result ZipWriter_CentralDir(struct Stream* s, struct ResourceZipEntry* e) {
	int filenameLen = String_Length(e->filename);
	cc_uint8 header[46 + STRING_SIZE];
	int modTime, modDate;
	GetCurrentZipDate(&modTime, &modDate);

	Mem_WriteU32_LE(header + 0,  0x02014b50);  /* signature */
	Mem_WriteU16_LE(header + 4,  20);          /* version */
	Mem_WriteU16_LE(header + 6,  20);          /* version needed */
	Mem_WriteU16_LE(header + 8,  0);           /* bitflags */
	Mem_WriteU16_LE(header + 10, 0);           /* compression method */
	Mem_WriteU16_LE(header + 12, modTime);     /* last modified */
	Mem_WriteU16_LE(header + 14, modDate);     /* last modified */

	Mem_WriteU32_LE(header + 16, e->crc32);    /* CRC32 */
	Mem_WriteU32_LE(header + 20, e->size);     /* compressed size */
	Mem_WriteU32_LE(header + 24, e->size);     /* uncompressed size */

	Mem_WriteU16_LE(header + 28, filenameLen); /* name length */
	Mem_WriteU16_LE(header + 30, 0);           /* extra field length */
	Mem_WriteU16_LE(header + 32, 0);           /* file comment length */
	Mem_WriteU16_LE(header + 34, 0);           /* disk number */
	Mem_WriteU16_LE(header + 36, 0);           /* internal attributes */
	Mem_WriteU32_LE(header + 38, 0);           /* external attributes */
	Mem_WriteU32_LE(header + 42, e->offset);   /* local header offset */

	Mem_Copy(header + 46, e->filename, filenameLen);
	return Stream_Write(s, header, 46 + filenameLen);
}

static cc_result ZipWriter_EndOfCentralDir(struct Stream* s, int numEntries,
											cc_uint32 centralDirBeg, cc_uint32 centralDirEnd) {
	cc_uint8 header[22];

	Mem_WriteU32_LE(header + 0,  0x06054b50); /* signature */
	Mem_WriteU16_LE(header + 4,  0);          /* disk number */
	Mem_WriteU16_LE(header + 6,  0);          /* disk number of start */
	Mem_WriteU16_LE(header + 8,  numEntries); /* disk entries */
	Mem_WriteU16_LE(header + 10, numEntries); /* total entries */
	Mem_WriteU32_LE(header + 12, centralDirEnd - centralDirBeg);  /* central dir size */
	Mem_WriteU32_LE(header + 16, centralDirBeg);                  /* central dir start */
	Mem_WriteU16_LE(header + 20, 0);         /* comment length */
	return Stream_Write(s, header, 22);
}

static cc_result ZipWriter_FixupLocalFile(struct Stream* s, struct ResourceZipEntry* e) {
	int filenameLen = String_Length(e->filename);
	cc_uint8 tmp[2048];
	cc_uint32 dataBeg, dataEnd;
	cc_uint32 i, crc, toRead, read;
	cc_result res;

	dataBeg = e->offset + 30 + filenameLen;
	if ((res = s->Position(s, &dataEnd))) return res;
	e->size = dataEnd - dataBeg;

	/* work out the CRC 32 */
	crc = 0xffffffffUL;
	if ((res = s->Seek(s, dataBeg))) return res;

	for (; dataBeg < dataEnd; dataBeg += read) {
		toRead = dataEnd - dataBeg;
		toRead = min(toRead, sizeof(tmp));

		if ((res = s->Read(s, tmp, toRead, &read))) return res;
		if (!read) return ERR_END_OF_STREAM;

		for (i = 0; i < read; i++) {
			crc = Utils_Crc32Table[(crc ^ tmp[i]) & 0xFF] ^ (crc >> 8);
		}
	}
	e->crc32 = crc ^ 0xffffffffUL;

	/* then fixup the header */
	if ((res = s->Seek(s, e->offset)))     return res;
	if ((res = ZipWriter_LocalFile(s, e))) return res;
	return s->Seek(s, dataEnd);
}

static cc_result ZipWriter_WriteData(struct Stream* dst, struct ResourceZipEntry* e) {
	cc_uint8* data = e->value.data;
	cc_result res;
	e->crc32 = Utils_CRC32(data, e->size);

	if ((res = ZipWriter_LocalFile(dst, e))) return res;
	return Stream_Write(dst, data, e->size);
}

static cc_result ZipWriter_WritePng(struct Stream* dst, struct ResourceZipEntry* e) {
	struct Bitmap* src = &e->value.bmp;
	cc_result res;

	if ((res = ZipWriter_LocalFile(dst, e)))            return res;
	if ((res = Png_Encode(src, dst, NULL, true, NULL))) return res;
	return ZipWriter_FixupLocalFile(dst, e);
}

static cc_result SoundPatcher_Save(struct Stream* s, struct ResourceZipEntry* e);
static cc_result ZipWriter_WriteWav(struct Stream* dst, struct ResourceZipEntry* e) {
	cc_result res;
	
	if ((res = ZipWriter_LocalFile(dst, e))) return res;
	if ((res = SoundPatcher_Save(dst,   e))) return res;
	return ZipWriter_FixupLocalFile(dst, e);
}


/*########################################################################################################################*
*------------------------------------------------------Zip file writer----------------------------------------------------*
*#########################################################################################################################*/
static cc_result ZipFile_WriteEntries(struct Stream* s, struct ResourceZipEntry* entries, int numEntries) {
	struct ResourceZipEntry* e;
	cc_uint32 beg, end;
	int i;
	cc_result res;

	for (i = 0; i < numEntries; i++)
	{
		e = &entries[i];

		if (e->type == RESOURCE_TYPE_PNG) {
			if ((res = ZipWriter_WritePng(s,  e))) return res;
		} else if (e->type == RESOURCE_TYPE_SOUND) {
			if ((res = ZipWriter_WriteWav(s,  e))) return res;
		} else {
			if ((res = ZipWriter_WriteData(s, e))) return res;
		} 
	}
	
	if ((res = s->Position(s, &beg))) return res;
	for (i = 0; i < numEntries; i++)
	{
		if ((res = ZipWriter_CentralDir(s, &entries[i]))) return res;
	}

	if ((res = s->Position(s, &end))) return res;
	return ZipWriter_EndOfCentralDir(s, numEntries, beg, end);
}

static void ZipFile_Create(const cc_string* path, struct ResourceZipEntry* entries, int numEntries) {
	struct Stream s;
	cc_filepath raw_path;
	cc_result res;

	Platform_EncodePath(&raw_path, path);
	res = Stream_CreatePath(&s, &raw_path);
	if (res) {
		Logger_IOWarn2(res, "creating", &raw_path); return;
	}
		
	res = ZipFile_WriteEntries(&s, entries, numEntries);
	if (res) Logger_IOWarn2(res, "making", &raw_path);

	res = s.Close(&s);
	if (res) Logger_IOWarn2(res, "closing", &raw_path);
}


#ifdef CC_BUILD_NOMUSIC
static int MusicAsset_Download(const char* hash) { return ERR_NOT_SUPPORTED; }
#else
/*########################################################################################################################*
*---------------------------------------------------------Music assets----------------------------------------------------*
*#########################################################################################################################*/
static struct MusicAsset {
	const char* name;
	const char* hash;
	short size;
	cc_bool downloaded;
	int reqID;
} musicAssets[] = {
	{ "calm1.ogg", "50a59a4f56e4046701b758ddbb1c1587efa4cadf", 2472 },
	{ "calm2.ogg", "74da65c99aa578486efa7b69983d3533e14c0d6e", 1931 },
	{ "calm3.ogg", "14ae57a6bce3d4254daa8be2b098c2d99743cc3f", 2181 },
	{ "hal1.ogg",  "df1ff11b79757432c5c3f279e5ecde7b63ceda64", 1926 },
	{ "hal2.ogg",  "ceaaaa1d57dfdfbb0bd4da5ea39628b42897a687", 1714 },
	{ "hal3.ogg",  "dd85fb564e96ee2dbd4754f711ae9deb08a169f9", 1879 },
	{ "hal4.ogg",  "5e7d63e75c6e042f452bc5e151276911ef92fed8", 2499 }
};

static void MusicAssets_CheckExistence(void) {
	cc_string path; char pathBuffer[FILENAME_SIZE];
	cc_filepath str;
	int i;
	String_InitArray(path, pathBuffer);

	for (i = 0; i < Array_Elems(musicAssets); i++) 
	{
		path.length = 0;
		String_Format1(&path, "audio/%c", musicAssets[i].name);

		Platform_EncodePath(&str, &path);
		musicAssets[i].downloaded = File_Exists(&str);
	}
}

static void MusicAssets_CountMissing(void) {
	int i;
	for (i = 0; i < Array_Elems(musicAssets); i++) 
	{
		if (musicAssets[i].downloaded) continue;

		Resources_MissingSize += musicAssets[i].size;
		Resources_MissingCount++;
	}
}


/*########################################################################################################################*
*-----------------------------------------------------Music asset fetching -----------------------------------------------*
*#########################################################################################################################*/
CC_NOINLINE static int MusicAsset_Download(const char* hash) {
	cc_string url; char urlBuffer[URL_MAX_SIZE];

	String_InitArray(url, urlBuffer);
	String_Format3(&url, "https://resources.download.minecraft.net/%r%r/%c", 
					&hash[0], &hash[1], hash);
	return Http_AsyncGetData(&url, 0);
}

static void MusicAssets_DownloadAssets(void) {
	int i;
	for (i = 0; i < Array_Elems(musicAssets); i++) 
	{
		if (musicAssets[i].downloaded) continue;
		musicAssets[i].reqID = MusicAsset_Download(musicAssets[i].hash);
	}
}

static const char* MusicAssets_GetRequestName(int reqID) {
	int i;
	for (i = 0; i < Array_Elems(musicAssets); i++) 
	{
		if (reqID == musicAssets[i].reqID) return musicAssets[i].name;
	}
	return NULL;
}


/*########################################################################################################################*
*----------------------------------------------------Music asset processing ----------------------------------------------*
*#########################################################################################################################*/
static void MusicAsset_Save(const char* name, struct HttpRequest* req) {
	cc_string path; char pathBuffer[STRING_SIZE];
	cc_result res;

	String_InitArray(path, pathBuffer);
	String_Format1(&path, "audio/%c", name);

	res = Stream_WriteAllTo(&path, req->data, req->size);
	if (res) Logger_SysWarn(res, "saving music file");
}

static void MusicAsset_Check(struct MusicAsset* music) {
	struct HttpRequest item;
	if (!Fetcher_Get(music->reqID, &item)) return;

	music->downloaded = true;
	MusicAsset_Save(music->name, &item);
	HttpRequest_Free(&item);
}

static void MusicAssets_CheckStatus(void) {
	int i;
	for (i = 0; i < Array_Elems(musicAssets); i++) 
	{
		if (musicAssets[i].downloaded) continue;
		MusicAsset_Check(&musicAssets[i]);
	}
}

static void MusicAssets_ResetState(void) {
}

static const struct AssetSet mccMusicAssetSet = {
	MusicAssets_CheckExistence,
	MusicAssets_CountMissing,
	MusicAssets_DownloadAssets,
	MusicAssets_GetRequestName,
	MusicAssets_CheckStatus,
	MusicAssets_ResetState
};
#endif


#ifdef CC_BUILD_NOSOUNDS
static cc_result SoundPatcher_Save(struct Stream* s, struct ResourceZipEntry* e) { return ERR_NOT_SUPPORTED; }
#else
/*########################################################################################################################*
*-----------------------------------------------------Sound asset writing ------------------------------------------------*
*#########################################################################################################################*/
#define WAV_FourCC(a, b, c, d) (((cc_uint32)a << 24) | ((cc_uint32)b << 16) | ((cc_uint32)c << 8) | (cc_uint32)d)
#define WAV_HDR_SIZE 44

/* Fixes up the .WAV header after having written all samples */
static cc_result SoundPatcher_FixupHeader(struct Stream* s, struct VorbisState* ctx, cc_uint32 offset, cc_uint32 len) {
	cc_uint8 header[WAV_HDR_SIZE];
	cc_result res = s->Seek(s, offset);
	if (res) return res;

	Mem_WriteU32_BE(header +  0, WAV_FourCC('R','I','F','F'));
	Mem_WriteU32_LE(header +  4, len - 8);
	Mem_WriteU32_BE(header +  8, WAV_FourCC('W','A','V','E'));
	Mem_WriteU32_BE(header + 12, WAV_FourCC('f','m','t',' '));
	Mem_WriteU32_LE(header + 16, 16); /* fmt chunk size */
	Mem_WriteU16_LE(header + 20, 1);  /* PCM audio format */
	Mem_WriteU16_LE(header + 22, ctx->channels);
	Mem_WriteU32_LE(header + 24, ctx->sampleRate);

	Mem_WriteU32_LE(header + 28, ctx->sampleRate * ctx->channels * 2); /* byte rate */
	Mem_WriteU16_LE(header + 32, ctx->channels * 2);                   /* block align */
	Mem_WriteU16_LE(header + 34, 16);                                  /* bits per sample */
	Mem_WriteU32_BE(header + 36, WAV_FourCC('d','a','t','a'));
	Mem_WriteU32_LE(header + 40, len - WAV_HDR_SIZE);

	return Stream_Write(s, header, WAV_HDR_SIZE);
}

/* Decodes all samples, then produces a .WAV file from them */
static cc_result SoundPatcher_WriteWav(struct Stream* s, struct VorbisState* ctx) {
	cc_int16* samples;
	cc_uint32 begOffset;
	cc_uint32 len = WAV_HDR_SIZE;
	cc_result res;
	int count;

	if ((res = s->Position(s, &begOffset))) return res;

	/* reuse context here for a temp garbage header */
	if ((res = Stream_Write(s, (const cc_uint8*)ctx, WAV_HDR_SIZE))) return res;
	if ((res = Vorbis_DecodeHeaders(ctx))) return res;

	samples = (cc_int16*)Mem_TryAlloc(ctx->blockSizes[1] * ctx->channels, 2);
	if (!samples) return ERR_OUT_OF_MEMORY;

	for (;;) {
		res = Vorbis_DecodeFrame(ctx);
		if (res == ERR_END_OF_STREAM) {
			/* reached end of samples, so done */
			res = SoundPatcher_FixupHeader(s, ctx, begOffset, len);
			break;
		}
		if (res) break;

		count = Vorbis_OutputFrame(ctx, samples);
		len  += count * 2;

#ifdef CC_BIG_ENDIAN
		Utils_SwapEndian16(samples, count);
#endif
		res = Stream_Write(s, (cc_uint8*)samples, count * 2);
		if (res) break;
	}

	Mem_Free(samples);
	if (!res) res = s->Seek(s, begOffset + len);
	return res;
}

/* Converts an OGG sound to a WAV sound for faster decoding later */
static cc_result SoundPatcher_Save(struct Stream* s, struct ResourceZipEntry* e) {
	struct OggState* ogg    = NULL;
	struct VorbisState* ctx = NULL;
	struct Stream src;
	cc_result res;

	ogg = (struct OggState*)Mem_TryAlloc(1,    sizeof(struct OggState));
	if (!ogg) { res = ERR_OUT_OF_MEMORY; goto cleanup; }

	ctx = (struct VorbisState*)Mem_TryAlloc(1, sizeof(struct VorbisState));
	if (!ctx) { res = ERR_OUT_OF_MEMORY; goto cleanup; }

	Stream_ReadonlyMemory(&src, e->value.data, e->size);

	Ogg_Init(ogg, &src);
	Vorbis_Init(ctx);
	ctx->source = ogg;
	res = SoundPatcher_WriteWav(s, ctx);

cleanup:
	if (ctx) Vorbis_Free(ctx);
	Mem_Free(ctx);
	Mem_Free(ogg);
	return res;
}


/*########################################################################################################################*
*---------------------------------------------------------Sound assets----------------------------------------------------*
*#########################################################################################################################*/
static struct SoundAsset {
	const char* filename;
	const char* hash;
	int reqID, size;
	void* data;
} soundAssets[] = {
	{ "dig_cloth1.wav",  "5fd568d724ba7d53911b6cccf5636f859d2662e8" }, { "dig_cloth2.wav",  "56c1d0ac0de2265018b2c41cb571cc6631101484" },
	{ "dig_cloth3.wav",  "9c63f2a3681832dc32d206f6830360bfe94b5bfc" }, { "dig_cloth4.wav",  "55da1856e77cfd31a7e8c3d358e1f856c5583198" },
	{ "dig_grass1.wav",  "41cbf5dd08e951ad65883854e74d2e034929f572" }, { "dig_grass2.wav",  "86cb1bb0c45625b18e00a64098cd425a38f6d3f2" },
	{ "dig_grass3.wav",  "f7d7e5c7089c9b45fa5d1b31542eb455fad995db" }, { "dig_grass4.wav",  "c7b1005d4926f6a2e2387a41ab1fb48a72f18e98" },
	{ "dig_gravel1.wav", "e8b89f316f3e9989a87f6e6ff12db9abe0f8b09f" }, { "dig_gravel2.wav", "c3b3797d04cb9640e1d3a72d5e96edb410388fa3" },
	{ "dig_gravel3.wav", "48f7e1bb098abd36b9760cca27b9d4391a23de26" }, { "dig_gravel4.wav", "7bf3553a4fe41a0078f4988a13d6e1ed8663ef4c" },
	{ "dig_sand1.wav",   "9e59c3650c6c3fc0a475f1b753b2fcfef430bf81" }, { "dig_sand2.wav",   "0fa4234797f336ada4e3735e013e44d1099afe57" },
	{ "dig_sand3.wav",   "c75589cc0087069f387de127dd1499580498738e" }, { "dig_sand4.wav",   "37afa06f97d58767a1cd1382386db878be1532dd" },
	{ "dig_snow1.wav",   "e9bab7d3d15541f0aaa93fad31ad37fd07e03a6c" }, { "dig_snow2.wav",   "5887d10234c4f244ec5468080412f3e6ef9522f3" },
	{ "dig_snow3.wav",   "a4bc069321a96236fde04a3820664cc23b2ea619" }, { "dig_snow4.wav",   "e26fa3036cdab4c2264ceb19e1cd197a2a510227" },
	{ "dig_stone1.wav",  "4e094ed8dfa98656d8fec52a7d20c5ee6098b6ad" }, { "dig_stone2.wav",  "9c92f697142ae320584bf64c0d54381d59703528" },
	{ "dig_stone3.wav",  "8f23c02475d388b23e5faa680eafe6b991d7a9d4" }, { "dig_stone4.wav",  "363545a76277e5e47538b2dd3a0d6aa4f7a87d34" },
	{ "dig_wood1.wav",   "9bc2a84d0aa98113fc52609976fae8fc88ea6333" }, { "dig_wood2.wav",   "98102533e6085617a2962157b4f3658f59aea018" },
	{ "dig_wood3.wav",   "45b2aef7b5049e81b39b58f8d631563fadcc778b" }, { "dig_wood4.wav",   "dc66978374a46ab2b87db6472804185824868095" },
	{ "dig_glass1.wav",  "7274a2231ed4544a37e599b7b014e589e5377094" }, { "dig_glass2.wav",  "87c47bda3645c68f18a49e83cbf06e5302d087ff" },
	{ "dig_glass3.wav",  "ad7d770b7fff3b64121f75bd60cecfc4866d1cd6" },

	{ "step_cloth1.wav",  "5fd568d724ba7d53911b6cccf5636f859d2662e8" }, { "step_cloth2.wav",  "56c1d0ac0de2265018b2c41cb571cc6631101484" },
	{ "step_cloth3.wav",  "9c63f2a3681832dc32d206f6830360bfe94b5bfc" }, { "step_cloth4.wav",  "55da1856e77cfd31a7e8c3d358e1f856c5583198" },
	{ "step_grass1.wav",  "41cbf5dd08e951ad65883854e74d2e034929f572" }, { "step_grass2.wav",  "86cb1bb0c45625b18e00a64098cd425a38f6d3f2" },
	{ "step_grass3.wav",  "f7d7e5c7089c9b45fa5d1b31542eb455fad995db" }, { "step_grass4.wav",  "c7b1005d4926f6a2e2387a41ab1fb48a72f18e98" },
	{ "step_gravel1.wav", "e8b89f316f3e9989a87f6e6ff12db9abe0f8b09f" }, { "step_gravel2.wav", "c3b3797d04cb9640e1d3a72d5e96edb410388fa3" },
	{ "step_gravel3.wav", "48f7e1bb098abd36b9760cca27b9d4391a23de26" }, { "step_gravel4.wav", "7bf3553a4fe41a0078f4988a13d6e1ed8663ef4c" },
	{ "step_sand1.wav",   "9e59c3650c6c3fc0a475f1b753b2fcfef430bf81" }, { "step_sand2.wav",   "0fa4234797f336ada4e3735e013e44d1099afe57" },
	{ "step_sand3.wav",   "c75589cc0087069f387de127dd1499580498738e" }, { "step_sand4.wav",   "37afa06f97d58767a1cd1382386db878be1532dd" },
	{ "step_snow1.wav",   "e9bab7d3d15541f0aaa93fad31ad37fd07e03a6c" }, { "step_snow2.wav",   "5887d10234c4f244ec5468080412f3e6ef9522f3" },
	{ "step_snow3.wav",   "a4bc069321a96236fde04a3820664cc23b2ea619" }, { "step_snow4.wav",   "e26fa3036cdab4c2264ceb19e1cd197a2a510227" },
	{ "step_stone1.wav",  "4e094ed8dfa98656d8fec52a7d20c5ee6098b6ad" }, { "step_stone2.wav",  "9c92f697142ae320584bf64c0d54381d59703528" },
	{ "step_stone3.wav",  "8f23c02475d388b23e5faa680eafe6b991d7a9d4" }, { "step_stone4.wav",  "363545a76277e5e47538b2dd3a0d6aa4f7a87d34" },
	{ "step_wood1.wav",   "9bc2a84d0aa98113fc52609976fae8fc88ea6333" }, { "step_wood2.wav",   "98102533e6085617a2962157b4f3658f59aea018" },
	{ "step_wood3.wav",   "45b2aef7b5049e81b39b58f8d631563fadcc778b" }, { "step_wood4.wav",   "dc66978374a46ab2b87db6472804185824868095" },

	/* Indev (SurvivalTest layer) mob/entity sounds - the genuine classic-era
	    assets, still served by mojang's asset host. mob_<group><n>.wav names
	    feed Audio.c's mob soundboard (see MobSounds_Load). */
	{ "mob_pig1.wav",     "a99bf88163bcb576e31e6e2275145afba6d1b4c7" }, { "mob_pig2.wav",     "ab615a912fb8ea06648836e0ec1cbeeefe117da6" },
	{ "mob_pig3.wav",     "58efedf302e0203a6ff9e59a6535d300286c5594" }, { "mob_pigdeath1.wav","4bc87ab869e17732a20c7518a327136baf5b2c26" },
	{ "mob_sheep1.wav",   "a3ffeaa0a75b8d2bdc949c181a6f8db78f8976ca" }, { "mob_sheep2.wav",   "1cfd864cbda555477ed9523e640de0d234c18858" },
	{ "mob_sheep3.wav",   "c9ac72409cbe6093e84d72a2a5c719d9e4a0e6b2" }, { "mob_hurt1.wav",    "9d485556b89bf776042080774679c37300bc744b" },
	{ "mob_bow1.wav",     "87edc11141fb5a045f2ed830b545aaa73f96ee99" }, { "mob_fuse1.wav",    "a92ba2b8f6abc41aa8d679ad808a81d0aafa04b2" },
	{ "mob_drr1.wav",     "40a5a4307c1a2a5a1b9f71254275689845374104" }, { "mob_pop1.wav",     "8f45b5faf6dfae2065846d26612f7552b73640dd" },
	{ "mob_explode1.wav", "9e22b5aeec31de99410b682cc161a6096a1cd00a" }, { "mob_fizz1.wav",    "c649e60ea9a99c97501a50d2dc4e579343e91ea8" },
	/* fire.fire ambient crackle + fire.ignite (flint & steel), from the same
	    era's newsound/fire/ - BlockFire.randomDisplayTick / ItemFlintAndSteel */
	{ "mob_fire1.wav",    "8b260108a73470c16cd244325242d4780cfb7d78" }, { "mob_ignite1.wav",  "19c729c3ceb753a824246b494bce5fa5c802f0f0" },
	/* random.splash (newsound/random/splash.ogg) - Entity water-entry */
	{ "mob_splash1.wav",  "22a491f266f5c3cdd6e669a9493daaf40c9c8575" }
};
static cc_bool allSoundsExist;

static void SoundAssets_ResetState(void) {
	int i;
	allSoundsExist = false;

	for (i = 0; i < Array_Elems(soundAssets); i++)
	{
		Mem_Free(soundAssets[i].data);
		soundAssets[i].data = NULL;
		soundAssets[i].size = 0;
	}
}


/*########################################################################################################################*
*-----------------------------------------------------Sound asset checking -----------------------------------------------*
*#########################################################################################################################*/
static int soundEntriesFound;

static struct SoundAsset* SoundAssest_Find(const cc_string* name) {
	struct SoundAsset* a;
	int i;

	for (i = 0; i < Array_Elems(soundAssets); i++) 
	{
		a = &soundAssets[i];
		if (String_CaselessEqualsConst(name, a->filename)) return a;
	}
	return NULL;
}

static cc_bool SoundAssets_CheckEntry(const cc_string* path) {
	cc_string name = *path;
	Utils_UNSAFE_GetFilename(&name);

	if (SoundAssest_Find(&name)) soundEntriesFound++;
	return false;
}

static void SoundAssets_CheckExistence(void) {
	soundEntriesFound = 0;
	ZipFile_InspectEntries(&Sounds_ZipPathMC, SoundAssets_CheckEntry);

	/* >= in case somehow have say "gui.png", "GUI.png" */
	allSoundsExist = soundEntriesFound >= Array_Elems(soundAssets);
}

static void SoundAssets_CountMissing(void) {
	if (allSoundsExist) return;

	Resources_MissingCount += Array_Elems(soundAssets);
	Resources_MissingSize  += 617; /* 417 dig/step + ~200 mob/entity/fire/splash oggs */
}


/*########################################################################################################################*
*----------------------------------------------------Sound asset generation ----------------------------------------------*
*#########################################################################################################################*/
static void SoundAsset_CreateZip(void) {
	struct ResourceZipEntry entries[Array_Elems(soundAssets)];
	int i;

	for (i = 0; i < Array_Elems(soundAssets); i++)
	{
		entries[i].filename = soundAssets[i].filename;
		entries[i].type     = RESOURCE_TYPE_SOUND;

		entries[i].value.data = soundAssets[i].data;
		entries[i].size       = soundAssets[i].size;
	}

	ZipFile_Create(&Sounds_ZipPathMC, entries, Array_Elems(soundAssets));
	SoundAssets_ResetState();
}


/*########################################################################################################################*
*-----------------------------------------------------Sound asset fetching -----------------------------------------------*
*#########################################################################################################################*/
#define SoundAsset_Download(hash) MusicAsset_Download(hash)

static void SoundAssets_DownloadAssets(void) {
	int i;
	for (i = 0; i < Array_Elems(soundAssets); i++)
	{
		if (allSoundsExist) continue;
		soundAssets[i].reqID = SoundAsset_Download(soundAssets[i].hash);
	}
}

static const char* SoundAssets_GetRequestName(int reqID) {
	int i;
	for (i = 0; i < Array_Elems(soundAssets); i++) 
	{
		if (reqID == soundAssets[i].reqID) return soundAssets[i].filename;
	}
	return NULL;
}

static void SoundAsset_Check(struct SoundAsset* sound, int i) {
	struct HttpRequest item;
	if (!Fetcher_Get(sound->reqID, &item)) return;

	sound->data = item.data;
	sound->size = item.size;
	item.data   = NULL;
	HttpRequest_Free(&item);

	if (i == Array_Elems(soundAssets) - 1)
		SoundAsset_CreateZip();
}

static void SoundAssets_CheckStatus(void) {
	int i;
	for (i = 0; i < Array_Elems(soundAssets); i++)
	{
		SoundAsset_Check(&soundAssets[i], i);
	}
}

static const struct AssetSet mccSoundAssetSet = {
	SoundAssets_CheckExistence,
	SoundAssets_CountMissing,
	SoundAssets_DownloadAssets,
	SoundAssets_GetRequestName,
	SoundAssets_CheckStatus,
	SoundAssets_ResetState
};
#endif


/*########################################################################################################################*
*------------------------------------------------------CC texture assets--------------------------------------------------*
*#########################################################################################################################*/
static const cc_string ccTexPack = String_FromConst("texpacks/classicube.zip");
static cc_bool ccTexturesExist, ccTexturesDownloaded;
static int ccTexturesReqID;

static void CCTextures_CheckExistence(void) {
	cc_filepath path;
	Platform_EncodePath(&path, &ccTexPack);
	
	ccTexturesExist = File_Exists(&path);
}

static void CCTextures_CountMissing(void) {
	if (ccTexturesExist) return;

	Resources_MissingCount++;
	Resources_MissingSize += 83;
	Resources_MissingRequired = true;
}


/*########################################################################################################################*
*--------------------------------------------------CC texture assets fetching --------------------------------------------*
*#########################################################################################################################*/
static void CCTextures_DownloadAssets(void) {
	static cc_string url = String_FromConst(RESOURCE_SERVER "/default.zip");
	if (ccTexturesExist) return;

	ccTexturesReqID = Http_AsyncGetData(&url, 0);
}

static const char* CCTextures_GetRequestName(int reqID) {
	return reqID == ccTexturesReqID ? "ClassiCube textures" : NULL;
}


/*########################################################################################################################*
*-------------------------------------------------CC texture assets processing -------------------------------------------*
*#########################################################################################################################*/
/* Android needs the touch.png */
/* TODO: Unify both android and desktop platforms to both just extract from default.zip */
static cc_bool CCTextures_SelectEntry(const cc_string* path) {
	return String_CaselessEqualsConst(path, "touch.png");
}

static cc_result CCTextures_ProcessEntry(const cc_string* path, struct Stream* data, struct ZipEntry* source) {
	struct ResourceZipEntry* e = ZipEntries_Find(path);
	if (!e) return 0; /* TODO exteact on PC too */

	return ZipEntry_ExtractData(e, data, source);
}

static cc_result CCTextures_ExtractZip(struct HttpRequest* req) {
	struct Stream src;
	cc_result res;
	struct ZipEntry entries[64];

	Stream_ReadonlyMemory(&src, req->data, req->size);
	if ((res = Zip_Extract(&src, CCTextures_SelectEntry, CCTextures_ProcessEntry,
							entries, Array_Elems(entries)))) return res;

	return Stream_WriteAllTo(&ccTexPack, req->data, req->size);
}

static void CCTextures_CheckStatus(void) {
	struct HttpRequest item;
	cc_result res;

	if (ccTexturesDownloaded) return;
	if (!Fetcher_Get(ccTexturesReqID, &item)) return;

	ccTexturesDownloaded = true;
	res = CCTextures_ExtractZip(&item);
	if (res) Logger_SysWarn(res, "saving ClassiCube textures");

	HttpRequest_Free(&item);
}

static void CCTextures_ResetState(void) {
	ccTexturesExist      = false;
	ccTexturesDownloaded = false;
}

static const struct AssetSet ccTexsAssetSet = {
	CCTextures_CheckExistence,
	CCTextures_CountMissing,
	CCTextures_DownloadAssets,
	CCTextures_GetRequestName,
	CCTextures_CheckStatus,
	CCTextures_ResetState
};


/*########################################################################################################################*
*----------------------------------------------------default.zip resources------------------------------------------------*
*#########################################################################################################################*/
#define ANIMS_TXT \
"# This file defines the animations used in a texture pack for ClassiCube.\r\n" \
"# Each line is in the format : <TileX> <TileY> <FrameX> <FrameY> <Frame size> <Frames count> <Tick delay>\r\n" \
"# - TileX and TileY are the coordinates of the tile in terrain.png that will be replaced by the animation frames.\r\n" \
"#     Essentially, TileX and TileY are the remainder and quotient of an ID in F10 menu divided by 16\r\n" \
"#     For instance, obsidian texture(37) has TileX of 5, and TileY of 2\r\n" \
"# - FrameX and FrameY are the pixel coordinates of the first animation frame in animations.png.\r\n" \
"# - Frame Size is the size in pixels of an animation frame.\r\n" \
"# - Frames count is the number of used frames.  The first frame is located at\r\n" \
"#     (FrameX, FrameY), the second one at (FrameX + FrameSize, FrameY) and so on.\r\n" \
"# - Tick delay is the number of ticks a frame doesn't change. For instance, delay of 0\r\n" \
"#     means that the tile would be replaced every tick, while delay of 2 means\r\n" \
"#     'replace with frame 1, don't change frame, don't change frame, replace with frame 2'.\r\n" \
"# NOTE: If a file called 'uselavaanim' is in the texture pack, the game instead generates the lava texture animation.\r\n" \
"# NOTE: If a file called 'usewateranim' is in the texture pack, the game instead generates the water texture animation.\r\n" \
"\r\n" \
"# fire\r\n" \
"6 2 0 0 16 32 0"

/* The entries that are required to exist within default.zip */
static struct ResourceZipEntry defaultZipEntries[] = {
	/* classic jar files */
	{ "terrain.png",  RESOURCE_TYPE_PNG  }, { "particles.png",   RESOURCE_TYPE_DATA },
	{ "clouds.png",   RESOURCE_TYPE_DATA }, { "rain.png",        RESOURCE_TYPE_DATA },
	{ "char.png",     RESOURCE_TYPE_DATA }, { "default.png",     RESOURCE_TYPE_DATA }, 
	/* icons.png is PNG-decoded so the armor icons can be patched from the
	    beta jar: the classic jar's row-9 armor sprites are MIRRORED
	    ((16,9) full .. (34,9) empty) vs the Indev/beta sheet the genuine
	    armor HUD coordinates expect ((16,9) empty .. (34,9) full). */
	{ "icons.png",    RESOURCE_TYPE_PNG  }, { "gui_classic.png", RESOURCE_TYPE_DATA },
	{ "creeper.png",  RESOURCE_TYPE_DATA }, { "pig.png",         RESOURCE_TYPE_DATA }, 
	{ "sheep.png",    RESOURCE_TYPE_DATA }, { "sheep_fur.png",   RESOURCE_TYPE_DATA },
	{ "skeleton.png", RESOURCE_TYPE_DATA }, { "spider.png",      RESOURCE_TYPE_DATA },
	{ "zombie.png",   RESOURCE_TYPE_DATA }, { "arrows.png",      RESOURCE_TYPE_DATA },
	{ "plate.png",    RESOURCE_TYPE_DATA },
	/* other files */
	{ "snow.png", RESOURCE_TYPE_DATA }, { "chicken.png",    RESOURCE_TYPE_DATA },
	{ "gui.png",  RESOURCE_TYPE_DATA }, { "animations.png", RESOURCE_TYPE_PNG  },
	{ "items.png", RESOURCE_TYPE_DATA }, /* gui/items.png from the beta jar (Indev mode item sprites) */
	{ "inventory.png", RESOURCE_TYPE_DATA }, /* gui/inventory.png from the beta jar (Indev inventory GUI) */
	{ "crafting.png",  RESOURCE_TYPE_DATA }, /* gui/crafting.png from the beta jar (Indev workbench 3x3 GUI) */
	{ "furnace.png",   RESOURCE_TYPE_DATA }, /* gui/furnace.png from the beta jar (Indev furnace GUI) */
	{ "container.png", RESOURCE_TYPE_DATA }, /* gui/container.png from the beta jar (Indev chest GUI) */
	{ "sun.png",       RESOURCE_TYPE_DATA }, /* terrain/sun.png from the beta jar (Indev sky) */
	/* armor/*.png from the beta jar (byte-identical to the authentic
	    in-20100223 armor overlays) - the Indev worn-armor renderer */
	{ "armor_cloth_1.png",   RESOURCE_TYPE_DATA }, { "armor_cloth_2.png",   RESOURCE_TYPE_DATA },
	{ "armor_chain_1.png",   RESOURCE_TYPE_DATA }, { "armor_chain_2.png",   RESOURCE_TYPE_DATA },
	{ "armor_iron_1.png",    RESOURCE_TYPE_DATA }, { "armor_iron_2.png",    RESOURCE_TYPE_DATA },
	{ "armor_diamond_1.png", RESOURCE_TYPE_DATA }, { "armor_diamond_2.png", RESOURCE_TYPE_DATA },
	{ "armor_gold_1.png",    RESOURCE_TYPE_DATA }, { "armor_gold_2.png",    RESOURCE_TYPE_DATA },
	{ "moon.png",      RESOURCE_TYPE_DATA }, /* terrain/moon.png from the beta jar (Indev sky) */
	{ "kz.png",        RESOURCE_TYPE_PNG  }, /* art/kz.png from the beta jar + the 2 authentic in-20100223 cells below */
	{ "animations.txt", RESOURCE_TYPE_CONST, sizeof(ANIMS_TXT) - 1, (cc_uint8*)ANIMS_TXT },
#ifdef CC_BUILD_MOBILE
	{ "touch.png", RESOURCE_TYPE_DATA }
#endif
};

CC_NOINLINE static struct ResourceZipEntry* ZipEntries_Find(const cc_string* name) {
	struct ResourceZipEntry* e;
	int i;

	for (i = 0; i < Array_Elems(defaultZipEntries); i++) 
	{
		e = &defaultZipEntries[i];
		if (String_CaselessEqualsConst(name, e->filename)) return e;
	}
	return NULL;
}


static cc_result ClassicPatcher_ExtractFiles(struct HttpRequest* req);
static cc_result ModernPatcher_ExtractFiles(struct HttpRequest* req);
static cc_result TerrainPatcher_Process(struct HttpRequest* req);
static cc_result NewTextures_ExtractGui(struct HttpRequest* req);
static cc_result BetaPatcher_ExtractItems(struct HttpRequest* req);
static void PatchTerrainTile(struct Bitmap* src, int srcX, int srcY, int tileX, int tileY);
static void PatchTerrainTileShifted(struct Bitmap* src, int srcX, int srcY, int tileX, int tileY);

static cc_result Classic0023Patcher_OldGoldBlock(struct HttpRequest* req);
static cc_result Classic0023Patcher_OldGoldOre(  struct HttpRequest* req);
static cc_result Classic0023Patcher_OldBlackWool(struct HttpRequest* req);
static cc_result Classic0023Patcher_OldGrayWool( struct HttpRequest* req);

/* URLs which data is downloaded from in order to generate the entries in default.zip */
struct ZipfileSource {
	const char* name;
	const char* url;
	cc_result (*Process)(struct HttpRequest* req);
	short size;
	cc_bool downloaded;
	int reqID;
};

#define DEFAULTZIP_0030_ENTRIES_COUNT 5
static struct ZipfileSource defaultZipSources_0030_0023[] = {
	{ "classic jar", "http://launcher.mojang.com/mc/game/c0.30_01c/client/54622801f5ef1bcc1549a842c5b04cb5d5583005/client.jar", ClassicPatcher_ExtractFiles, 291 },
	{ "1.6.2 jar",   "http://launcher.mojang.com/mc/game/1.6.2/client/b6cb68afde1d9cf4a20cbf27fa90d0828bf440a4/client.jar",     ModernPatcher_ExtractFiles, 4621 },
	{ "terrain.png patch", RESOURCE_SERVER "/terrain-patch2.png", TerrainPatcher_Process, 7 },
	{ "gui.png patch",     RESOURCE_SERVER "/gui.png",            NewTextures_ExtractGui, 21 },
	/* Official Mojang CDN b1.7.3 client (sha1 43db9b49..., from piston-meta's
	    version manifest) - source of gui/items.png, the item sprite atlas the
	    Indev gamemode draws from. Early items.png cell layout is identical
	    from Indev through beta (icons were only ever appended). */
	{ "beta jar", "https://launcher.mojang.com/v1/objects/43db9b498cb67058d2e12d394e6507722e71bb45/client.jar", BetaPatcher_ExtractItems, 1431 },
	/* 0.0.23 textures */
	{ "0.0.23 gold",  "https://classic.minecraft.net/assets/textures/gold.png",      Classic0023Patcher_OldGoldBlock, 1 },
	{ "0.0.23 ore",   "https://classic.minecraft.net/assets/textures/rock_gold.png", Classic0023Patcher_OldGoldOre,   1 },
	{ "0.0.23 black", "https://classic.minecraft.net/assets/textures/color13.png",   Classic0023Patcher_OldBlackWool, 1 },
	{ "0.0.23 gray",  "https://classic.minecraft.net/assets/textures/color14.png",   Classic0023Patcher_OldGrayWool,  1 },
};
static struct ZipfileSource* defaultZipSources;
static int numDefaultZipSources, numDefaultZipProcessed;

static void MCCTextures_ResetState(void) {
	int i;
	for (i = 0; i < Array_Elems(defaultZipEntries); i++) 
	{
		if (defaultZipEntries[i].type == RESOURCE_TYPE_CONST) continue;

		/* can reuse value.data for value.bmp case too */
		Mem_Free(defaultZipEntries[i].value.data);
		defaultZipEntries[i].value.data = NULL;
		defaultZipEntries[i].size       = 0;
	}
}


/*########################################################################################################################*
*------------------------------------------------default.zip entry generators---------------------------------------------*
*#########################################################################################################################*/
static cc_bool ClassicPatcher_SelectEntry(const cc_string* path) {
	cc_string name = *path;
	Utils_UNSAFE_GetFilename(&name);
	return ZipEntries_Find(&name) != NULL;
}

static cc_result ClassicPatcher_ProcessEntry(const cc_string* path, struct Stream* data, struct ZipEntry* source) {
	static const cc_string guiClassicPng = String_FromConst("gui_classic.png");
	struct ResourceZipEntry* e;
	cc_string name;

	name = *path;
	Utils_UNSAFE_GetFilename(&name);
	if (String_CaselessEqualsConst(&name, "gui.png")) name = guiClassicPng;

	e = ZipEntries_Find(&name);

	/* terrain.png requires special handling */
	if (String_CaselessEqualsConst(path, "terrain.png")) {
		return Png_Decode(&e->value.bmp, data);
	}
	/* icons.png too - the beta patcher swaps its armor sprites in place */
	/*  (matched by FILENAME: the classic jar stores it at gui/icons.png) */
	if (String_CaselessEqualsConst(&name, "icons.png")) {
		return Png_Decode(&e->value.bmp, data);
	}
	return ZipEntry_ExtractData(e, data, source);
}

static cc_result ClassicPatcher_ExtractFiles(struct HttpRequest* req) {
	struct Stream src;
	cc_result res;
	struct ZipEntry entries[64];
	Stream_ReadonlyMemory(&src, req->data, req->size);
	
	return Zip_Extract(&src, 
			ClassicPatcher_SelectEntry, ClassicPatcher_ProcessEntry,
			entries, Array_Elems(entries));
}

/* Pulls gui/items.png out of the beta jar into default.zip as items.png, */
/*  and copies the Indev block tiles (workbench/furnace/chest/torch/crops) */
/*  from its terrain.png into the FREE atlas cells reserved for them (rows */
/*  6-7, indices 96+ - see SURVIVAL_TEST_NOTES.md's reservation table). */
static cc_bool BetaPatcher_SelectEntry(const cc_string* path) {
	static const cc_string armorPrefix = String_FromConst("armor/");
	if (String_CaselessStarts(path, &armorPrefix)) return true;
	return String_CaselessEqualsConst(path, "gui/items.png")
		|| String_CaselessEqualsConst(path, "gui/inventory.png")
		|| String_CaselessEqualsConst(path, "gui/crafting.png")
		|| String_CaselessEqualsConst(path, "gui/furnace.png")
		|| String_CaselessEqualsConst(path, "gui/container.png")
		|| String_CaselessEqualsConst(path, "gui/icons.png")
		|| String_CaselessEqualsConst(path, "terrain/sun.png")
		|| String_CaselessEqualsConst(path, "terrain/moon.png")
		|| String_CaselessEqualsConst(path, "art/kz.png")
		|| String_CaselessEqualsConst(path, "terrain.png");
}

/* src cell (b1.7.3 terrain.png, verified visually) -> dst cell (our atlas) */
static const struct BetaTile { cc_uint8 sx, sy, dx, dy; } beta_tiles[] = {
	{ 11,2,  0,6 }, /*  96 workbench top   */
	{ 12,3,  1,6 }, /*  97 workbench side  */
	{ 11,3,  2,6 }, /*  98 workbench front */
	{ 12,2,  3,6 }, /*  99 furnace front   */
	{ 13,3,  4,6 }, /* 100 furnace lit     */
	{ 13,2,  5,6 }, /* 101 furnace side    */
	{ 14,3,  6,6 }, /* 102 furnace top     */
	{ 11,1,  7,6 }, /* 103 chest front     */
	{ 10,1,  8,6 }, /* 104 chest side      */
	{  9,1,  9,6 }, /* 105 chest top       */
	{  0,5, 10,6 }, /* 106 torch           */
	{  8,5, 11,6 }, { 9,5, 12,6 }, { 10,5, 13,6 }, { 11,5, 14,6 }, /* 107-110 crops 0-3 */
	{ 12,5, 15,6 }, { 13,5,  0,7 }, { 14,5,  1,7 }, { 15,5,  2,7 }, /* 111-114 crops 4-7 */
	{  6,5,  3,7 }, /* 115 farmland wet */
	{  7,5,  4,7 }, /* 116 farmland dry */
	{  2,3,  7,7 }, /* 119 diamond ore (118 is the live fire animation) */
	{  8,1, 10,7 }, /* 122 diamond block (teal, from the beta jar terrain.png).
	                   NB: 120 is the 2nd Indev fire animation instance. */
};

/* The two kz.png cells b1.7.3 later redrew (Sea at 64,32 32x16 and Stage at
    64,128 32x32) - the authentic in-20100223 pixels, composited over the
    beta jar's sheet so paintings look exactly like genuine Indev. */
static const cc_uint8 kz_sea_png[] = {
	0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0x00,0x00,0x00,0x0D,0x49,0x48,0x44,0x52,
	0x00,0x00,0x00,0x20,0x00,0x00,0x00,0x10,0x08,0x06,0x00,0x00,0x00,0x77,0x00,0x7D,
	0x59,0x00,0x00,0x05,0x95,0x49,0x44,0x41,0x54,0x78,0x9C,0xC5,0x95,0x5B,0x8C,0x5D,
	0x65,0x01,0x85,0xBF,0xFF,0xB2,0x2F,0xE7,0x36,0xB7,0x33,0xA7,0xD4,0xDE,0xD2,0x19,
	0x6C,0x29,0xDA,0x0B,0xAD,0xD4,0x4B,0x87,0x06,0x8B,0x4D,0x85,0x46,0xD0,0x36,0xBC,
	0x28,0x56,0x45,0x2A,0x22,0x12,0x6B,0x20,0x12,0x22,0x26,0xA6,0x18,0x1B,0x12,0x8D,
	0x8A,0x80,0x28,0x0A,0x18,0x2B,0x69,0x30,0x24,0x4A,0xA2,0x62,0xD4,0xA0,0x2D,0xC5,
	0x1A,0xDA,0xDA,0x4A,0xDB,0x99,0x48,0x13,0x18,0xCA,0x74,0xA6,0x33,0xC3,0xF4,0xCC,
	0x9C,0xD9,0xFB,0xEC,0xB3,0x2F,0xFF,0xFF,0xFB,0x30,0x2F,0x9A,0xF8,0xEE,0xF7,0xB6,
	0x5E,0x56,0xD6,0xCB,0x5A,0x4B,0xDC,0xB2,0xA1,0xDB,0xCD,0x4D,0x1B,0x9C,0x83,0x5A,
	0xC9,0xC7,0x59,0x07,0x52,0x20,0x04,0x0B,0x08,0x10,0x41,0xC1,0x7F,0xE2,0xAA,0xFA,
	0xBF,0xB4,0x40,0x20,0xE2,0x9C,0xFF,0x85,0xEB,0x78,0xA0,0x04,0x42,0x2E,0x18,0xB6,
	0x93,0x14,0x80,0xF2,0x92,0x00,0xD1,0xCE,0xD1,0xB3,0x53,0x86,0xAF,0x3E,0xFA,0x1D,
	0x8E,0xFD,0xF1,0x0F,0x94,0xCB,0x92,0xFD,0x67,0xBF,0x06,0x9D,0x04,0xE9,0x79,0xB8,
	0xD4,0x80,0x02,0x67,0x35,0x04,0x02,0x91,0x58,0x9C,0x14,0x48,0xE5,0xB0,0xBD,0x40,
	0xA8,0x40,0x28,0x6E,0xDC,0x56,0xE7,0xEB,0x1F,0xEE,0x62,0x74,0xC6,0x30,0x6F,0x04,
	0xCE,0x19,0x12,0x23,0x59,0xD1,0x2D,0xF1,0x3D,0x81,0x90,0x8E,0x0A,0x05,0xE7,0xC6,
	0xDF,0xE0,0xC6,0xA5,0x0E,0x9C,0xC0,0x4D,0x1C,0xE7,0x9E,0xBD,0xFB,0xD1,0x5D,0x5D,
	0x21,0x41,0xA0,0x30,0x12,0x94,0x12,0x30,0xE8,0x81,0x17,0x60,0x03,0x20,0x56,0xC8,
	0x56,0x86,0xCC,0xC1,0x29,0x09,0xEF,0x52,0xB8,0x8E,0xC6,0x02,0x3B,0x56,0x07,0xEC,
	0xDC,0x28,0x18,0xE8,0x2D,0x73,0xF2,0x52,0xCA,0x9E,0xA3,0x73,0xD4,0x9B,0x21,0x65,
	0xCF,0xF1,0xD8,0xC7,0xAB,0x4C,0xB5,0x2D,0x47,0xC6,0x13,0x96,0x94,0x04,0xAB,0xEB,
	0x8A,0xA8,0x93,0xF3,0xD1,0xAB,0x96,0x41,0x34,0x49,0x18,0x06,0x24,0x4A,0x63,0x8D,
	0x43,0xE3,0x40,0x39,0x49,0xC9,0x13,0xB4,0xE3,0x18,0xEA,0x1A,0x11,0x48,0x96,0x05,
	0x92,0x03,0xBB,0x4A,0xAC,0x6D,0x28,0x46,0x5B,0xF0,0xDA,0x44,0xC6,0xF7,0x87,0x41,
	0xE9,0x85,0x8C,0xF7,0x5D,0x5F,0x66,0x79,0xA3,0xC2,0x64,0x2B,0xE6,0xCA,0xBA,0xC7,
	0xFD,0xBD,0x8A,0x66,0x06,0x81,0xD5,0xBC,0x36,0x5D,0xD0,0x57,0x95,0xEC,0x7C,0x77,
	0x85,0xBA,0x2F,0xF9,0xE7,0x54,0x1B,0x5F,0x28,0xA4,0x8B,0x10,0x52,0xE2,0x90,0xC8,
	0xA0,0x46,0xA5,0x1C,0xA0,0x9D,0x6F,0x31,0x26,0xA5,0x54,0xAA,0x30,0x9F,0xB5,0x59,
	0x53,0xD5,0x8C,0x17,0x16,0x29,0x35,0x77,0x1C,0xE9,0x90,0x0B,0x01,0x85,0x60,0xD7,
	0xD2,0x90,0x4F,0x0C,0x5A,0x32,0xE1,0x30,0x28,0x7E,0x7A,0xA6,0x60,0x71,0x25,0xE2,
	0xCA,0xBA,0x07,0xC2,0x32,0x3C,0xEB,0xA8,0xF8,0x8A,0x5B,0xFC,0x97,0xD9,0xB8,0x6A,
	0x27,0xA6,0xC8,0x30,0x42,0x71,0xE8,0x7C,0xCE,0x50,0xC3,0xA7,0x1A,0x2A,0x92,0xA4,
	0x4D,0x77,0x50,0xC6,0xE4,0x19,0xD6,0x80,0x0C,0x73,0xB4,0x30,0x39,0xDA,0xF7,0xE8,
	0xB4,0x13,0x4A,0x7E,0x40,0xAC,0x05,0x11,0x92,0x2D,0x75,0xC1,0xD5,0x56,0xD2,0x53,
	0x96,0x64,0x0E,0x22,0xDB,0x21,0xC9,0x24,0xFD,0x41,0x48,0xD9,0x17,0x48,0x1F,0x06,
	0xBB,0x35,0x53,0x9D,0x14,0x90,0xF4,0xF8,0x1E,0xA1,0x72,0x5C,0xB3,0xE9,0x26,0xAC,
	0xCB,0x28,0x4C,0xCC,0x6D,0xF7,0x1E,0x60,0xE8,0x8E,0x07,0x39,0x7D,0xB9,0xC6,0x5C,
	0x5E,0xB0,0x65,0x71,0x48,0xA5,0x48,0x50,0x08,0x9C,0x0A,0x20,0x06,0xED,0x6A,0x1E,
	0x38,0x83,0xB1,0x29,0x71,0x12,0xD3,0xED,0x5B,0xDE,0x96,0x05,0x17,0x73,0x4B,0xC3,
	0x17,0x5C,0x4A,0x2C,0xAF,0xB4,0x0B,0x4A,0xA1,0x66,0x83,0x2E,0xD1,0xED,0x19,0xC6,
	0x22,0xCB,0x97,0xD7,0xF8,0x5C,0xBD,0x48,0x31,0xD3,0xF1,0x18,0x79,0x27,0x66,0xF3,
	0x15,0x25,0xDE,0x9A,0x7C,0x9A,0xF1,0xE6,0x36,0xF6,0x7D,0xE3,0x7A,0xAE,0xB9,0x01,
	0x1E,0xF8,0xD2,0x31,0xB4,0x9A,0xE0,0xD9,0x29,0xE8,0xF7,0x35,0xDA,0xCC,0xA3,0x3D,
	0x45,0xD1,0x49,0x28,0x57,0x7B,0x00,0x90,0x00,0x85,0x71,0x64,0xD6,0x91,0x77,0x9A,
	0x14,0xBE,0xA5,0x2F,0xF0,0x78,0xD3,0x08,0x32,0x2B,0xF8,0x6B,0x24,0x68,0xD4,0x14,
	0x3F,0x58,0x5F,0xE2,0xBA,0xE5,0x8E,0x87,0x86,0x2A,0xDC,0xBA,0xD2,0x70,0xEB,0xCF,
	0xFF,0xCC,0xBE,0xDF,0x9D,0xE4,0x6F,0x6F,0xBE,0xC5,0xF3,0xC7,0x4F,0xF3,0xFC,0x99,
	0x31,0xAE,0x1D,0xB8,0x93,0x4F,0x7F,0xF1,0x76,0x96,0xF6,0xD7,0xF1,0x47,0x3E,0xC0,
	0xC5,0xC9,0x87,0x79,0xE8,0x2B,0x5B,0x58,0x33,0x33,0xC2,0xAE,0x41,0xC9,0x53,0x27,
	0x47,0xB0,0x79,0x86,0x52,0x02,0x93,0x27,0x98,0xBA,0x42,0x8A,0xB8,0xC0,0xC9,0x10,
	0x8C,0xA5,0xBE,0x68,0x05,0x83,0x4E,0xB1,0xB9,0x0A,0x71,0x2B,0xA5,0x95,0x2A,0x76,
	0xD7,0x7D,0x76,0x37,0x24,0x63,0x89,0x63,0x36,0x11,0x1C,0x1B,0x8B,0x58,0x52,0xAD,
	0xF0,0xEB,0xBD,0xDB,0xB9,0x7F,0xEB,0x7A,0xE6,0x6D,0x4E,0xA3,0x52,0x62,0xD7,0xEA,
	0x3E,0x86,0xFF,0x71,0x88,0xE7,0x9E,0x7C,0x91,0xE9,0xE1,0x69,0x3E,0x77,0xCF,0x8F,
	0x79,0xE5,0x85,0x8C,0xCA,0xC0,0x10,0xDB,0x36,0x5D,0xE2,0x7B,0x87,0x5F,0x65,0xC7,
	0xAA,0xF5,0x64,0x79,0x8C,0x43,0x92,0xB5,0x23,0x04,0x62,0xA1,0x05,0x4E,0x0A,0x74,
	0x10,0xD2,0x9A,0x9D,0xE6,0xC5,0x33,0x93,0xB8,0xD0,0x80,0xAE,0x71,0xA6,0x15,0x31,
	0xD6,0xAB,0x68,0x8E,0x74,0x70,0x5E,0xC2,0x96,0x46,0x8D,0x73,0xE3,0x92,0xBE,0x72,
	0xCC,0xCD,0x03,0x01,0x47,0x67,0x67,0x59,0x55,0xAD,0x53,0x1F,0xAC,0xD3,0x55,0x29,
	0x33,0x15,0x7A,0xDC,0xFD,0xA9,0x21,0xFA,0x87,0x06,0x79,0xE4,0xC1,0xBB,0x58,0xBB,
	0x75,0x3B,0xE9,0xF9,0xD7,0x09,0xED,0x07,0x79,0xEE,0xD8,0x1B,0xEC,0xDF,0x5E,0xC2,
	0xCC,0x19,0xAC,0x12,0xD8,0xA2,0xBD,0x30,0x44,0x00,0x36,0x2F,0x88,0xE6,0x63,0x4A,
	0x42,0xE2,0xFA,0x7A,0x78,0x7F,0xAF,0xE6,0x6C,0x5A,0xF0,0xDD,0x0D,0x11,0xE6,0xF2,
	0x09,0x66,0xCC,0x28,0xE7,0xC6,0x33,0xCE,0x9F,0x4F,0xB9,0xFB,0xB3,0xB7,0x71,0xE7,
	0x2F,0x67,0xF8,0xCD,0xE4,0x2C,0x1F,0xFA,0xC8,0xB5,0x3C,0x36,0x13,0x93,0x1E,0x9D,
	0xE1,0xDE,0xB5,0x05,0xBF,0xFF,0xE4,0x52,0x7E,0xF4,0x8B,0xA3,0xBC,0xD3,0xFC,0x17,
	0x7F,0x39,0xF1,0x13,0x36,0x6E,0x5E,0xC7,0xDC,0xE5,0xC3,0xAC,0x7D,0x6A,0x9A,0xE5,
	0x3D,0x96,0x27,0x8E,0xCC,0xF0,0x85,0x75,0x50,0xE4,0x6D,0xAC,0x5D,0x58,0x4A,0xED,
	0x2A,0x9A,0xD6,0xEC,0x38,0x69,0xD6,0xA1,0xBF,0xAF,0x04,0x6F,0x9F,0xE2,0xD5,0x53,
	0xA3,0x88,0xC5,0x57,0x31,0xEC,0xCE,0xD0,0x1C,0x3D,0xCE,0xB3,0x87,0x4F,0xF1,0xD2,
	0xC1,0x5F,0xF1,0xED,0x27,0x1F,0xE7,0xD0,0x33,0x07,0x10,0x2F,0x5C,0xC0,0xC9,0x88,
	0xBF,0x8F,0x6C,0xC5,0x2D,0xEB,0x46,0x56,0x2A,0xD8,0x7C,0x25,0x03,0x2B,0x6E,0xE2,
	0xC2,0xA5,0x09,0xCE,0x9E,0xF8,0x13,0x6B,0xD7,0xDD,0x85,0x1F,0xAE,0xE4,0x33,0x7B,
	0x0E,0xF2,0xCD,0x87,0x5F,0xE6,0x62,0xEB,0x0A,0x1E,0x79,0x7D,0x82,0xDB,0xDF,0x53,
	0xC6,0x3A,0x8B,0xB3,0x16,0x1C,0x68,0x52,0x90,0xCE,0xE2,0x09,0x48,0x3A,0xE7,0x39,
	0xB2,0xB7,0x07,0xA3,0x6F,0xC0,0xD7,0x92,0x1F,0xFE,0xF6,0x09,0xCA,0x03,0x5D,0xFC,
	0xEC,0xE6,0xFB,0x38,0xF8,0xD2,0xA3,0xD4,0x06,0x73,0xAE,0x5B,0x39,0xC8,0xB7,0xF6,
	0x3D,0x40,0x29,0xD4,0x64,0x85,0xA1,0xBB,0xAB,0x97,0x76,0x3C,0x8F,0x96,0x86,0xE8,
	0xF2,0x05,0x1A,0x8D,0x15,0xAC,0x58,0xFD,0x3E,0x3C,0x5D,0xA3,0xB7,0x14,0xD2,0x49,
	0x32,0x4E,0x7E,0xBE,0x8E,0x10,0x0A,0xA7,0x16,0x21,0x95,0xC4,0x98,0x98,0x24,0x6A,
	0x22,0x12,0xD0,0x18,0x30,0x59,0xCE,0xF6,0x1D,0xDB,0xA8,0x55,0x7B,0x99,0xB9,0x70,
	0x0E,0x83,0x45,0x6A,0xCD,0x9E,0xF7,0x7E,0x0C,0x70,0xD8,0xA4,0xC3,0xEE,0x81,0x4D,
	0xB8,0x22,0x21,0xF4,0x6B,0x14,0x97,0x4E,0x33,0x3E,0x3B,0x81,0x92,0x92,0x39,0xA5,
	0x50,0xA2,0x40,0xE9,0x00,0xA5,0x41,0x38,0x49,0xAF,0x01,0x3F,0xBF,0xC8,0xF8,0x64,
	0x4C,0xB9,0x5A,0x27,0x49,0x22,0x9C,0x6D,0xE3,0xC9,0x12,0x42,0x41,0xDA,0xE9,0x10,
	0xF8,0x15,0x8C,0xD0,0x48,0x52,0x10,0x41,0x9D,0x94,0x2A,0xCD,0xD6,0x3C,0xAD,0x56,
	0x44,0x61,0x34,0x18,0x8F,0xD9,0xB9,0x19,0xE2,0x38,0x26,0x0C,0x17,0x93,0x19,0x8D,
	0x93,0x01,0xF3,0x51,0x93,0xB9,0x68,0x8E,0xA0,0xD6,0x20,0xCE,0x53,0x9C,0xEA,0xA7,
	0x15,0xA5,0xE4,0xD6,0xA3,0x93,0x69,0xDA,0x85,0x24,0x97,0x21,0x99,0xD1,0xA4,0x06,
	0xA2,0x38,0xA5,0x9D,0x64,0x20,0x7B,0x89,0x72,0x8F,0xDC,0x54,0x48,0x73,0x8F,0xA8,
	0x23,0x10,0x15,0x81,0xF8,0x7F,0xDF,0xF1,0xBF,0x01,0xC2,0x12,0x9F,0x0C,0x4F,0xC6,
	0xD5,0x4A,0x00,0x00,0x00,0x00,0x49,0x45,0x4E,0x44,0xAE,0x42,0x60,0x82,
};
static const cc_uint8 kz_stage_png[] = {
	0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0x00,0x00,0x00,0x0D,0x49,0x48,0x44,0x52,
	0x00,0x00,0x00,0x20,0x00,0x00,0x00,0x20,0x08,0x06,0x00,0x00,0x00,0x73,0x7A,0x7A,
	0xF4,0x00,0x00,0x0A,0xD7,0x49,0x44,0x41,0x54,0x78,0x9C,0x6D,0x98,0x7B,0x90,0xD6,
	0xD5,0x79,0xC7,0x3F,0xE7,0xF2,0xBB,0xBC,0xBF,0xF7,0xDD,0xDD,0x77,0xAF,0x2C,0xC8,
	0xB2,0x84,0xFB,0x96,0x70,0xD7,0x40,0x66,0x89,0x60,0x83,0x93,0x68,0x12,0x33,0xA3,
	0x99,0x29,0x52,0x4D,0x46,0x93,0xB4,0x66,0x32,0x4D,0xCC,0xA8,0xD1,0x8E,0x89,0xBD,
	0xA5,0x8E,0x4D,0x4D,0x33,0xD1,0x34,0x01,0x34,0x36,0xA1,0x11,0xB1,0xA9,0xB6,0x44,
	0x4B,0x08,0xD6,0x5A,0x8C,0x34,0x22,0x22,0x50,0xB9,0x49,0x20,0xC0,0x02,0xBB,0xEC,
	0xB2,0x2C,0x7B,0x7B,0x2F,0xBF,0xCB,0x39,0xA7,0x7F,0xBC,0xEB,0x6A,0xD3,0x3E,0x7F,
	0xFD,0xE6,0xCC,0x33,0xE7,0xF2,0x3C,0xDF,0xE7,0x73,0x9E,0xF3,0x13,0x5B,0xEF,0x9B,
	0xEF,0xBC,0xA1,0x18,0x57,0x69,0x60,0xE0,0xF2,0x08,0x00,0x8D,0x3A,0xC0,0x65,0x16,
	0xAF,0xB5,0xCA,0xBB,0x96,0xB6,0x85,0xBC,0xDF,0xF4,0x85,0x32,0x6E,0xE2,0x5B,0xF0,
	0xFF,0x98,0x00,0x1B,0xE4,0xB1,0xCD,0x6E,0x72,0xE8,0xC2,0x9E,0x71,0x00,0xAE,0x2A,
	0x16,0x90,0x8B,0x25,0xAA,0xBF,0x82,0x36,0x69,0xC6,0x3F,0x9C,0x5B,0x4E,0x1A,0xC7,
	0x34,0xD6,0x2D,0xE0,0xCA,0xD8,0x38,0x7B,0xDF,0x78,0x99,0x2D,0x4F,0x6F,0x27,0xCB,
	0xAA,0x48,0xAD,0xF1,0xBC,0x80,0xEB,0x56,0x2E,0x66,0x7C,0x7C,0x98,0x86,0xFA,0x29,
	0x18,0x1B,0x63,0xAD,0x25,0x4B,0x63,0x00,0xA4,0xD2,0x54,0xAB,0x25,0xC2,0x30,0x8F,
	0xE7,0x85,0xA4,0x69,0x15,0xCF,0x0B,0xF1,0x74,0xC0,0x6B,0xAF,0xED,0x60,0xD9,0xD2,
	0xD9,0x0C,0x0F,0x8F,0x70,0xD7,0x93,0x9F,0x40,0xE1,0xF1,0xCD,0x6F,0xDE,0x4F,0x43,
	0x63,0x44,0xFD,0xD9,0xC7,0x91,0xE2,0x98,0x04,0x60,0xCF,0xEB,0xBB,0x18,0x4F,0x52,
	0xF6,0xBC,0xBE,0x8B,0xCD,0x4F,0x6D,0xC5,0x92,0xA1,0x75,0x88,0x44,0x93,0xCF,0xF9,
	0x8C,0x8F,0x0F,0x53,0x57,0xD7,0x8C,0x31,0x86,0x52,0xA5,0x82,0xA7,0x23,0x3C,0x3F,
	0x22,0x08,0x0B,0x58,0x93,0x51,0x57,0xD7,0x8C,0xEF,0x15,0x48,0xD3,0x2A,0x4A,0xF9,
	0x1C,0x3E,0xB4,0x8F,0xB1,0x91,0x23,0xAC,0x58,0x3A,0x1D,0x4F,0x65,0xB4,0x34,0xE5,
	0xD8,0xFF,0xEB,0xFF,0x00,0xC0,0xD7,0x30,0xB5,0xD8,0x40,0xA0,0xAA,0xE8,0x0B,0xC3,
	0xE3,0x1C,0xE9,0xD9,0x8F,0x96,0x8A,0x82,0xEF,0xF1,0xD4,0x8F,0x9F,0x65,0x68,0x78,
	0x90,0xFE,0x33,0xA7,0x59,0xB2,0x72,0x35,0x36,0xCB,0x18,0x1D,0x05,0xAD,0x06,0x49,
	0xCA,0x97,0x49,0x4D,0x42,0xE8,0x79,0x24,0x95,0x8B,0x9C,0x3C,0x35,0xCA,0x82,0x85,
	0x8B,0x28,0x8D,0xF6,0xE2,0x0B,0x45,0x42,0x86,0x74,0x80,0x13,0x2C,0x98,0xDF,0x88,
	0x92,0x0E,0x29,0x35,0xD6,0xD5,0xD2,0x10,0xE5,0x25,0x87,0x0E,0xBE,0x40,0xD7,0xC2,
	0x8F,0xF1,0xFC,0xCF,0x9E,0xE1,0xCC,0x50,0x8A,0x9C,0xD1,0x5E,0xA4,0xB9,0xB9,0x09,
	0x80,0xF5,0x77,0x7E,0x16,0x3F,0x08,0x98,0x36,0xA5,0x13,0x80,0xFA,0xB6,0x29,0x00,
	0x54,0xE3,0x12,0x81,0x56,0x08,0x61,0xD1,0x52,0x22,0xA5,0xC0,0xB9,0x8C,0xD9,0xB3,
	0x22,0x92,0xF2,0x69,0x8A,0x45,0x4D,0xE2,0x52,0x3C,0xA9,0x88,0xD3,0x94,0x34,0xCD,
	0x90,0x12,0x8C,0x15,0x18,0x2B,0x88,0x13,0xC8,0xAC,0x8F,0x20,0x41,0x4B,0xC9,0x89,
	0x63,0xBB,0x31,0xD5,0x04,0x13,0x1B,0xA4,0x0C,0x34,0xD6,0xC5,0xFC,0xEC,0x5F,0x77,
	0x22,0xD1,0x00,0xBC,0xFD,0xD6,0x1E,0x3E,0xF2,0xA9,0x5B,0xF0,0xB4,0xA2,0xED,0x03,
	0xB3,0x6B,0x22,0x4C,0x13,0x00,0x3C,0xCF,0x27,0xCE,0x1C,0x4E,0x0A,0x84,0x10,0x68,
	0x0D,0x4A,0x3A,0x7C,0x15,0x60,0x9C,0x42,0xC9,0x90,0xD8,0x85,0xE0,0x4D,0x63,0xB8,
	0x54,0xC0,0xCB,0xCD,0x45,0xD8,0x5A,0x9A,0x84,0x8A,0x90,0x52,0x92,0x38,0x39,0x29,
	0x4C,0x09,0xF0,0xBD,0x1F,0x3D,0x8D,0xCA,0x17,0xC8,0xB2,0x9A,0xEA,0xBB,0xD7,0xAE,
	0x9B,0x74,0x70,0x26,0xA5,0x7B,0xE5,0x07,0xF1,0x73,0x0B,0x09,0xF2,0x0B,0x90,0xC1,
	0x5C,0xC2,0x70,0x01,0xB9,0xE8,0xF7,0x38,0xDF,0x67,0x38,0x7A,0x6C,0x84,0x4A,0xEC,
	0x38,0x77,0x29,0xE0,0xC4,0xC9,0x12,0x7E,0x6E,0x16,0xCD,0xC5,0x2E,0xE6,0xCF,0x5B,
	0xCC,0xB9,0xD3,0x3D,0x08,0xE1,0xC8,0x37,0xB6,0xD3,0x31,0x73,0x25,0x3B,0x7E,0xB9,
	0x1F,0xE7,0x24,0x81,0xA8,0xFE,0xEF,0x0D,0xDC,0x74,0xFD,0x2A,0x00,0x3E,0xFF,0xC5,
	0xF5,0x48,0x34,0xE3,0xC6,0xA1,0x84,0x05,0x60,0xF9,0xBC,0x0F,0x50,0x28,0x14,0xC9,
	0x4C,0x85,0xDF,0x9E,0x3A,0x8A,0x12,0x1A,0xA1,0xC0,0x59,0xC1,0xAC,0x59,0x8B,0x59,
	0xB2,0x7C,0x15,0xF5,0x8D,0xF3,0x99,0xDD,0xD9,0xC9,0x92,0x25,0xCB,0x90,0x13,0x29,
	0x3A,0x73,0xE6,0x2C,0x1B,0x6E,0xBF,0x9D,0x8B,0xFD,0xBD,0x74,0x75,0x75,0x31,0x3C,
	0x3C,0xCA,0xAD,0xEB,0x37,0x60,0x70,0x2C,0x5A,0xF1,0x09,0x00,0xDA,0xDB,0x8A,0x48,
	0x93,0x8B,0x49,0x13,0xCB,0x2D,0x37,0x74,0x73,0xF2,0x97,0x3B,0x69,0x9C,0xD6,0x46,
	0x5D,0x21,0x87,0x32,0x29,0x4B,0xE7,0xCE,0xA4,0x5C,0x1A,0x41,0x08,0x05,0x40,0x67,
	0xE7,0x42,0xAA,0xF1,0x18,0x42,0x08,0x32,0x3B,0x8A,0xD2,0x02,0x29,0x34,0x4A,0x0A,
	0xD2,0xB4,0x8C,0xC9,0x1C,0x5A,0xC3,0x03,0xF7,0xDE,0x4D,0x66,0x0C,0x87,0x0F,0x1F,
	0xE6,0xC3,0xAB,0x56,0xF3,0xE6,0xBE,0x7D,0x5C,0xBF,0xA6,0x9B,0x81,0x81,0x5E,0x02,
	0xED,0xA1,0x84,0x8F,0x9F,0xAF,0x47,0x68,0x89,0x16,0x52,0xA1,0xA5,0x42,0x0A,0x41,
	0xB1,0xA4,0x59,0x38,0x3D,0x21,0x68,0x9D,0x8F,0x70,0x60,0x9D,0xC0,0x0F,0x72,0x35,
	0x05,0x47,0x75,0x0C,0x0D,0x5D,0xA4,0xD8,0xD0,0x8E,0x54,0xA0,0x55,0x03,0xCE,0x19,
	0xD2,0x24,0x23,0x4E,0x2F,0x61,0x4D,0x91,0x20,0x70,0x3C,0xF8,0xF5,0x07,0xF8,0xC6,
	0x5F,0xFC,0x39,0x0F,0x3D,0xF0,0x75,0x8A,0x4D,0x2D,0x0C,0x0F,0x5F,0xE1,0x8E,0x3B,
	0xEE,0x64,0xC7,0xCB,0xBB,0xB9,0xF1,0xA3,0x6B,0xD8,0xF2,0xEC,0x36,0x52,0x12,0x3C,
	0x4F,0xA0,0x1A,0xC6,0x90,0xCE,0x1A,0x5E,0x78,0xE5,0xD7,0x28,0x3C,0x5E,0x9E,0xDE,
	0xC1,0x9B,0x4C,0x65,0xCF,0xA5,0xD1,0x1A,0x6C,0x30,0x48,0xE5,0x71,0xF4,0xDC,0x45,
	0x32,0x63,0x68,0x6A,0x6A,0x47,0x88,0xF7,0xB8,0x17,0xC7,0x31,0x41,0x28,0x09,0x83,
	0x29,0x84,0xA1,0x8F,0x14,0x9A,0xBF,0x7C,0xF8,0x51,0x02,0xBF,0xC0,0x5F,0x7F,0xFB,
	0xDB,0xEC,0xDB,0xFF,0xC6,0xA4,0xAF,0xA7,0x35,0x2F,0xED,0xDE,0xC3,0xD4,0xF6,0x0E,
	0xB6,0xFD,0xF8,0x27,0x98,0xA4,0x06,0x31,0xAD,0x4C,0x85,0x34,0x33,0x18,0x52,0xAE,
	0x7D,0xEC,0x6B,0x9C,0x9A,0xF9,0x41,0x3A,0xA7,0xC4,0xE4,0x6F,0x6A,0xE5,0xED,0x13,
	0x1F,0x62,0xEE,0xBC,0x25,0x00,0x1C,0x3B,0xD7,0x4F,0x79,0x70,0x80,0xAB,0x17,0xE7,
	0xD1,0x3A,0xC4,0xC9,0x0E,0xA2,0x28,0xA2,0x5A,0xAD,0xE2,0xFB,0x21,0xB5,0x7D,0x59,
	0x3C,0xDF,0x21,0xF0,0xF8,0xE8,0xB5,0xAB,0x78,0x69,0xF7,0x1E,0xB6,0xFC,0x74,0x0B,
	0x73,0x66,0x4F,0xE7,0xFA,0x35,0xDD,0xBC,0xB4,0x7B,0x0F,0xD6,0x5A,0x7A,0x4F,0x9D,
	0x67,0xD6,0xFC,0x59,0xD0,0xE7,0x90,0x66,0x6A,0xC4,0xE6,0x47,0xFE,0x16,0x93,0xBE,
	0xC7,0xEC,0xFC,0x4D,0xAD,0x00,0x2C,0x9A,0xF7,0x06,0xC6,0x54,0x91,0xB2,0xA6,0x81,
	0xD4,0x58,0x36,0x6F,0x7E,0x05,0x27,0x3B,0x10,0xC2,0x62,0x2D,0x04,0x81,0xFF,0x5E,
	0xC5,0x58,0x49,0x9A,0x08,0x1C,0x29,0xDF,0xDF,0xF8,0x1D,0x84,0xB0,0xDC,0x76,0xEB,
	0x67,0x59,0xB1,0xBC,0x9B,0xEE,0x35,0xD7,0xD6,0x54,0x2F,0x25,0x1F,0xBB,0xF9,0x26,
	0x1E,0x7E,0xF4,0xEF,0x00,0x10,0xCF,0xDC,0xBF,0xC0,0xFD,0xE2,0xF4,0x47,0xF8,0xEA,
	0x83,0x5F,0xA1,0xB9,0xF9,0x12,0x52,0x0E,0xD2,0x16,0x9D,0x23,0x68,0xB8,0x87,0x8B,
	0x17,0x7B,0x08,0x82,0x7A,0x0E,0xF6,0xF4,0x11,0x28,0x8F,0x30,0xE7,0xD1,0x77,0xEA,
	0x34,0x0D,0x53,0xDA,0x59,0x70,0x55,0x2B,0x5A,0x43,0x66,0x04,0xBE,0x57,0xAB,0xEB,
	0x2C,0x95,0x78,0xBE,0xC5,0x98,0x5A,0x14,0xAC,0xCB,0x10,0x28,0x4A,0xC3,0x3D,0xE4,
	0x0A,0x55,0xCA,0xE5,0x8C,0x28,0xD2,0x13,0xBE,0x19,0xAF,0x7D,0x79,0x3D,0xD2,0x59,
	0xC7,0x1F,0xDE,0xF5,0x79,0x86,0x47,0x46,0xD9,0x7F,0x44,0x31,0xED,0x07,0x3B,0xD1,
	0x8F,0x1E,0xE1,0xC2,0xD9,0x5F,0xD1,0x77,0xE1,0x38,0x97,0xFA,0x8E,0x11,0x4A,0x41,
	0x69,0x74,0x88,0xF1,0x81,0x01,0xA6,0xB6,0x37,0xB3,0xF7,0xA5,0x9D,0x14,0xEA,0x7C,
	0xA4,0x54,0xC4,0x95,0xB1,0xC9,0x08,0x28,0x0D,0xD6,0x4A,0x84,0x50,0x48,0x95,0xA1,
	0x14,0x08,0xE9,0x18,0x4B,0x20,0xA3,0x40,0xE0,0xAB,0x49,0xDF,0x28,0x57,0x26,0xE9,
	0x0A,0x91,0x42,0x0A,0xB0,0x09,0x23,0x23,0x15,0x3E,0xFD,0xD4,0x9F,0xC2,0x6F,0x8E,
	0xC3,0x6F,0x8E,0x93,0x59,0x4B,0xE7,0x8C,0x66,0x8A,0x45,0xC9,0xD8,0xC0,0x00,0xAE,
	0x52,0xE5,0xC4,0xD1,0x03,0x0C,0x27,0x82,0x6B,0xBA,0xBB,0x49,0xCA,0x3D,0x08,0x3C,
	0xF2,0xF9,0x26,0x92,0xA4,0x06,0x16,0xAD,0x25,0x71,0x32,0x82,0x94,0x35,0xC4,0x38,
	0x27,0x11,0xC2,0xD2,0xDC,0xD4,0x8A,0x56,0x2D,0xF8,0xD1,0x3C,0x7E,0x7B,0xA6,0x8C,
	0xAF,0x47,0x51,0x07,0xFC,0x89,0x94,0xF4,0x97,0x49,0x8D,0x25,0x9F,0x2F,0xC0,0xDC,
	0x05,0x00,0x94,0x5A,0xA6,0x52,0xC8,0xE5,0x48,0xE2,0x0C,0x81,0x63,0xE1,0xFC,0x4E,
	0xC2,0xA6,0x66,0xBA,0x3E,0xB4,0x9A,0x42,0x14,0x31,0xE3,0xAA,0x36,0x4A,0xD5,0x22,
	0x42,0x25,0x28,0xED,0x08,0x82,0x5A,0xAF,0x60,0x4C,0x86,0x35,0x0E,0x63,0x52,0xAC,
	0x05,0x63,0x52,0x94,0x14,0x58,0x6B,0x27,0x00,0x05,0xB3,0xE7,0x2C,0x22,0x18,0xFC,
	0x1D,0x12,0x96,0x4A,0x25,0xDE,0xFA,0xAF,0xD7,0x18,0x99,0xD7,0x46,0xEF,0x77,0xEF,
	0x7E,0x5F,0xE9,0x08,0x94,0xE7,0xD1,0x73,0xA5,0x82,0xD2,0xA2,0x26,0x32,0x53,0xA5,
	0xA1,0xD0,0x48,0x2E,0x17,0x21,0xF0,0x38,0x70,0xE0,0x00,0x07,0x0E,0x1C,0xE2,0xF2,
	0xE5,0x01,0xA4,0x72,0x44,0x51,0x3D,0x59,0x56,0x8B,0x86,0xD6,0x8A,0x24,0x75,0xE4,
	0x72,0x21,0xCE,0x09,0xAC,0x85,0xD0,0xEF,0x83,0x4A,0x2B,0x47,0xEA,0x6A,0xA4,0xD5,
	0x00,0x2D,0x1D,0x9D,0xB4,0xCF,0x9C,0x49,0xC3,0xF3,0xDF,0x61,0x68,0xED,0xA5,0x5A,
	0x8D,0x1B,0x8B,0x74,0x16,0x9B,0x18,0x20,0x8F,0x10,0x3E,0x2A,0x94,0x64,0xB1,0x43,
	0x4A,0x89,0x13,0x31,0xFB,0xF7,0xFF,0x37,0x5F,0xB8,0xF3,0x8B,0x20,0xA1,0xA9,0xB1,
	0x19,0x80,0xED,0x3F,0xFF,0x17,0xF2,0x85,0x00,0x63,0x32,0xB2,0x2C,0xC1,0xD3,0x79,
	0x1C,0x29,0x5A,0x4B,0x92,0xD8,0x11,0x0D,0x9C,0xE1,0xD5,0x0B,0x09,0xD7,0xAC,0x9A,
	0xC3,0x99,0x81,0x0A,0x12,0x0B,0x5A,0x09,0xC6,0x2E,0xD5,0x16,0x0E,0x74,0x2B,0xF9,
	0xC6,0x06,0x64,0x49,0x70,0xFC,0x9D,0x51,0x0A,0xC5,0x2E,0x84,0xEF,0xA3,0xA5,0xC4,
	0x54,0x21,0xCA,0x6B,0x3C,0xDF,0x70,0xF2,0xC4,0x71,0x36,0x6E,0xDC,0xC8,0xE6,0xDD,
	0x7B,0xD9,0xFC,0xCA,0x5E,0x1E,0x79,0x7E,0x07,0x17,0xFB,0x7B,0x59,0xBE,0x7C,0x29,
	0xE7,0xCF,0xF7,0x22,0x95,0xC3,0x3A,0x8D,0xEF,0x36,0x4E,0xA4,0x01,0x3C,0xBF,0x56,
	0xEA,0xD7,0xAC,0x9A,0x33,0x79,0xF1,0x69,0x80,0x6C,0x7C,0x78,0x32,0xEC,0xD6,0xB6,
	0x70,0xFE,0xAE,0xDB,0x28,0x16,0xBA,0x58,0xDA,0x66,0x6A,0x79,0x32,0x60,0x91,0xA8,
	0xD0,0x62,0xD2,0x84,0xFB,0xEE,0xBE,0x97,0x45,0x4D,0x1D,0x3C,0xF4,0x67,0xDF,0x60,
	0xED,0xD5,0x8B,0xD0,0x4A,0xD2,0xB2,0x6C,0x35,0xEF,0x1C,0xFA,0x0A,0xFA,0x8F,0x9F,
	0xA3,0xF4,0xF8,0x43,0xE4,0x06,0xFB,0xF0,0x36,0xDD,0x02,0xC0,0x58,0x6C,0x89,0x64,
	0x2F,0x07,0xDF,0x1E,0x64,0xE9,0xA2,0x19,0x13,0x82,0x0D,0xDF,0xD3,0x80,0x2E,0x14,
	0xB9,0xD4,0xD7,0xC3,0xC0,0x27,0x3F,0x87,0x4E,0xA6,0x90,0xF3,0xE7,0x22,0x84,0x45,
	0x29,0x85,0x73,0x12,0x1D,0x28,0x94,0x4C,0xF1,0xB5,0xE2,0xC5,0xA7,0xFF,0x91,0x3F,
	0xB9,0xE7,0x6E,0xFC,0x69,0x8D,0x4C,0x3D,0x7C,0x8C,0xBF,0x7F,0xFD,0x38,0x37,0xBF,
	0x78,0x9C,0x6B,0xFF,0xEA,0x49,0xE4,0xD9,0x35,0x35,0x90,0x0D,0xF6,0x4D,0x1E,0xA8,
	0xFA,0xEF,0x9D,0x3C,0x7C,0x2E,0x20,0xFC,0xA1,0xE1,0x87,0x37,0x7E,0x9F,0x83,0x6F,
	0x0F,0x02,0xF0,0xFA,0x5B,0x97,0x6B,0x20,0xDA,0x7A,0xDF,0x7C,0xD7,0x71,0xDB,0x0E,
	0x70,0x29,0xFD,0xC5,0xE9,0xEC,0xBE,0x22,0xB9,0xB9,0x35,0x66,0x59,0x41,0xE1,0x79,
	0x0E,0x63,0x1C,0x47,0x7A,0x87,0x38,0xF1,0xAB,0xBD,0xFC,0xE0,0x89,0xC7,0x38,0xDB,
	0x73,0x0A,0x47,0xAD,0xBB,0x09,0xC2,0x90,0x2C,0x35,0x18,0x93,0x22,0x08,0x58,0x7B,
	0xDD,0x6A,0x00,0xA6,0xB4,0x36,0x91,0x54,0x12,0x8A,0x4D,0x2D,0x6C,0xFB,0xA7,0x6D,
	0xC4,0x89,0x99,0x20,0xA5,0xE0,0x99,0x6D,0x5B,0x18,0xAB,0x3A,0x3C,0x25,0x19,0xD9,
	0x7A,0x7B,0x2D,0x05,0xA9,0x4B,0xF0,0xA5,0xA3,0x7D,0xB4,0x07,0x98,0xC9,0xF3,0x97,
	0x02,0x2E,0x67,0x96,0x15,0xE2,0x32,0x2D,0xCD,0xF5,0x28,0x67,0x78,0xE0,0xA1,0x7B,
	0x11,0xCE,0x12,0x04,0x39,0xB2,0x2C,0x03,0xE9,0xB3,0xEB,0xDF,0x9E,0x65,0xDD,0x0D,
	0x9F,0x01,0x3C,0xDE,0x6D,0xCE,0x5F,0x7D,0xF5,0x55,0x94,0x92,0x38,0x2C,0x52,0x68,
	0xE2,0x38,0xA6,0x63,0x4A,0x2B,0xE7,0x07,0x86,0x90,0x12,0x36,0x6C,0xD8,0xC0,0xD6,
	0xAD,0x5B,0xA1,0xAE,0x99,0x11,0x37,0xA1,0x01,0x65,0x2D,0x52,0x2A,0x4C,0xAD,0x32,
	0xF8,0xD6,0xAC,0x04,0x6B,0x0D,0x07,0x4B,0xF5,0x34,0x19,0xC1,0xCB,0xCF,0x6D,0x27,
	0x4D,0x63,0x3C,0x9D,0xC3,0x64,0x09,0xC6,0x18,0x76,0xEF,0xDA,0xCE,0x0D,0x9F,0xDC,
	0x80,0x35,0x8A,0xC0,0x57,0x7C,0x7C,0xDD,0x3A,0x5E,0xD8,0xF1,0x0B,0x9C,0xB1,0xF8,
	0xB9,0x00,0x93,0x0A,0x8C,0x4B,0x90,0x08,0x2E,0x0E,0x5E,0x41,0x4B,0x81,0xF4,0x24,
	0x71,0xB5,0xD6,0xDA,0xF9,0x41,0x1E,0xE7,0x1C,0x92,0x11,0xF0,0xB4,0xC2,0x09,0x8D,
	0x54,0x35,0x4E,0x3B,0x27,0x51,0x32,0xE4,0x72,0xE6,0x21,0x04,0x3C,0xF2,0x37,0xDF,
	0xA2,0xBE,0x2E,0x42,0x2B,0x87,0xEF,0x7B,0x28,0x15,0xB2,0xB2,0xFB,0x46,0xCA,0x95,
	0x0A,0x4F,0x6E,0xFA,0x1E,0x99,0xC9,0xF8,0xF9,0x8E,0x17,0x09,0xA3,0x88,0x7C,0x5D,
	0x1D,0x42,0x68,0xB4,0xAF,0x30,0xC6,0x52,0x9F,0xAF,0xC7,0xF3,0x3D,0x3C,0xDF,0xA3,
	0x32,0x9E,0xD6,0xA2,0x07,0x64,0x69,0x15,0x3D,0x24,0x6B,0x22,0xFC,0xDA,0x17,0xEE,
	0x00,0xA0,0xE1,0xC0,0x01,0x00,0x4E,0x0D,0x5E,0x61,0xD7,0x15,0xC7,0xC7,0x5B,0xC0,
	0x18,0x83,0x17,0x68,0xAC,0xB0,0x68,0x1D,0x60,0xAC,0x00,0x0C,0x51,0x14,0xF0,0xE6,
	0xBE,0xBD,0xDC,0x7B,0xFF,0x83,0x68,0x25,0x10,0x28,0x36,0x3F,0xFE,0x5D,0x6E,0xFD,
	0x83,0xF5,0x24,0x49,0x8C,0xC9,0xAA,0xB5,0x45,0x4D,0x05,0xA9,0x04,0xE5,0x4A,0x09,
	0xE5,0x49,0x0A,0x75,0x05,0x0E,0x1E,0x7D,0x07,0x1D,0xFA,0x24,0x22,0x87,0xD8,0xFA,
	0xB9,0x79,0xEE,0x78,0xC7,0x7A,0xD6,0xFE,0xFE,0x52,0xDA,0xCE,0x1B,0x92,0xFE,0xC3,
	0x70,0x5D,0x37,0xD5,0x4A,0x89,0x37,0x0F,0x5D,0xC0,0x29,0x9F,0xC8,0x83,0x05,0xF3,
	0xA6,0xD7,0xCA,0x47,0x65,0x38,0x2B,0xF1,0xB5,0xE3,0xE6,0xCF,0x7C,0x15,0xED,0x67,
	0x64,0xC6,0xA1,0x95,0x00,0x14,0x9E,0x1F,0x52,0xAD,0x96,0x49,0x92,0x0C,0xDF,0xD7,
	0x14,0x0A,0x79,0x2A,0xE5,0x0A,0x49,0x52,0xE5,0xCB,0x5F,0xBA,0x87,0x7C,0x31,0x24,
	0x17,0xFA,0x54,0xC6,0xCA,0xB4,0xFE,0xE7,0x26,0xA4,0x1B,0x14,0xAC,0x5B,0xB7,0x02,
	0x80,0x2B,0x33,0x24,0xE7,0x5B,0x3B,0xE9,0x3D,0x73,0x85,0xDE,0x63,0x7D,0x2C,0x9C,
	0xDF,0xC2,0xEC,0xE9,0x45,0x16,0x2F,0x9C,0x09,0x40,0x9A,0x55,0x19,0x2D,0xA5,0x18,
	0x27,0xB9,0xF1,0x53,0x7F,0x84,0x0C,0x12,0x9C,0xF5,0x10,0x52,0xB0,0x75,0xD3,0x13,
	0x58,0x1C,0xD6,0x1A,0xC2,0x30,0xA2,0xBE,0xBE,0x1E,0xDF,0x0F,0x88,0xE3,0x2A,0x99,
	0xC9,0xF0,0x74,0xC4,0x13,0x3F,0x7A,0x0C,0x47,0xCC,0x8C,0x19,0xCD,0x5C,0x7D,0xF5,
	0x9C,0xDA,0x81,0xA4,0x96,0x98,0x2C,0x20,0x88,0x40,0x0B,0x49,0x30,0x0D,0xEA,0x72,
	0x92,0x6A,0x53,0x3B,0x5A,0x2A,0xA2,0x48,0x90,0xA4,0x19,0xD8,0x0A,0xA1,0x17,0xE0,
	0xC9,0x18,0xDF,0x77,0xEC,0xDC,0xF9,0x93,0x1A,0x39,0x03,0x3D,0x31,0x51,0x8E,0xED,
	0xFF,0xBC,0x09,0xED,0x7B,0x64,0x49,0x4A,0x3A,0x71,0x4D,0x4B,0x0D,0x36,0x83,0x4A,
	0x26,0x48,0xD2,0x94,0x5C,0x18,0x20,0xA4,0x02,0x2C,0x62,0x9A,0x40,0xAB,0x65,0x3E,
	0xD6,0x0D,0x53,0x2D,0x41,0x2E,0x0C,0xD0,0x22,0xA5,0x9A,0xFA,0xE4,0x02,0x8D,0xB3,
	0x06,0x67,0x3D,0x7C,0xDF,0xE1,0xE9,0x3A,0xD2,0x2C,0xC5,0x18,0x08,0x3D,0x4D,0x35,
	0x4D,0x09,0x3D,0xCD,0x78,0xA9,0x82,0xA7,0x3D,0x08,0x2A,0x98,0x74,0x8C,0x34,0x09,
	0x26,0x21,0xF4,0xEE,0xE2,0x9E,0x76,0x48,0x0D,0x39,0xAD,0x29,0x65,0x63,0x78,0xD4,
	0x01,0xA0,0x92,0x32,0xE2,0xB9,0x2F,0xCD,0x72,0x27,0xDF,0xAA,0x00,0x30,0xBD,0xA5,
	0x1E,0x97,0x59,0x84,0x7E,0xDF,0xCB,0x45,0x4B,0x64,0x63,0x89,0xF7,0x9B,0x9D,0x9A,
	0xE7,0x77,0x4D,0xF5,0x97,0xFF,0xCF,0x18,0x80,0xB9,0x1C,0x21,0x03,0x3D,0x39,0xE7,
	0xBB,0xBF,0x00,0xDA,0x3E,0xDC,0x80,0xEA,0x1F,0xE3,0x7F,0x00,0x5E,0xB5,0xB1,0xC8,
	0x0D,0x96,0xD8,0x4C,0x00,0x00,0x00,0x00,0x49,0x45,0x4E,0x44,0xAE,0x42,0x60,0x82,
};

static cc_result BetaPatcher_PatchKz(struct Bitmap* kz) {
	struct Stream mem;
	struct Bitmap cell;
	cc_result res;

	Stream_ReadonlyMemory(&mem, (void*)kz_sea_png, sizeof(kz_sea_png));
	if ((res = Png_Decode(&cell, &mem))) return res;
	Bitmap_UNSAFE_CopyBlock(0, 0, 64, 32, &cell, kz, 16);
	Bitmap_UNSAFE_CopyBlock(16, 0, 80, 32, &cell, kz, 16);
	Mem_Free(cell.scan0);

	Stream_ReadonlyMemory(&mem, (void*)kz_stage_png, sizeof(kz_stage_png));
	if ((res = Png_Decode(&cell, &mem))) return res;
	Bitmap_UNSAFE_CopyBlock(0,  0, 64, 128, &cell, kz, 16);
	Bitmap_UNSAFE_CopyBlock(16, 0, 80, 128, &cell, kz, 16);
	Bitmap_UNSAFE_CopyBlock(0, 16, 64, 144, &cell, kz, 16);
	Bitmap_UNSAFE_CopyBlock(16,16, 80, 144, &cell, kz, 16);
	Mem_Free(cell.scan0);
	return 0;
}

static cc_result BetaPatcher_ProcessEntry(const cc_string* path, struct Stream* data, struct ZipEntry* source) {
	static const cc_string itemsPng    = String_FromConst("items.png");
	static const cc_string armorPrefix = String_FromConst("armor/");
	cc_string basename = *path;
	struct ResourceZipEntry* e;
	struct Bitmap bmp;
	cc_result res;
	int i;

	if ((i = String_LastIndexOf(&basename, '/')) >= 0) {
		basename = String_UNSAFE_SubstringAt(&basename, i + 1);
	}

	if (String_CaselessEqualsConst(path, "terrain.png")) {
		res = Png_Decode(&bmp, data);
		if (res) return res;

		for (i = 0; i < Array_Elems(beta_tiles); i++) {
			PatchTerrainTile(&bmp, beta_tiles[i].sx * 16, beta_tiles[i].sy * 16,
							 beta_tiles[i].dx, beta_tiles[i].dy);
		}
		/* 117: torch-top tile - the torch tile (0,5) shifted down 1px so the */
		/*  block-bounds crop shows the genuine ember pixels (see the helper) */
		PatchTerrainTileShifted(&bmp, 0 * 16, 5 * 16, 5, 7);
		Mem_Free(bmp.scan0);
		return 0;
	}

	if (String_CaselessEqualsConst(path, "art/kz.png")) {
		static const cc_string kzPng = String_FromConst("kz.png");
		e = ZipEntries_Find(&kzPng);
		if ((res = Png_Decode(&e->value.bmp, data))) return res;
		return BetaPatcher_PatchKz(&e->value.bmp);
	}
	if (String_CaselessEqualsConst(path, "gui/icons.png")) {
		/* The classic jar's icons.png (already decoded into the entry by the
		    classic patcher) carries the armor sprites MIRRORED relative to
		    the Indev/beta sheet: (16,9) full .. (34,9) empty instead of
		    (16,9) empty .. (34,9) full. The armor HUD uses the genuine
		    coordinates, so overwrite the 27x9 strip with the beta jar's. */
		static const cc_string iconsPng = String_FromConst("icons.png");
		struct Bitmap* dst;
		int x, y;
		e = ZipEntries_Find(&iconsPng);
		dst = &e->value.bmp;
		if (!dst->scan0) return 0; /* classic icons.png failed to decode */

		res = Png_Decode(&bmp, data);
		if (res) return res;
		if (bmp.width >= 43 && bmp.height >= 18 &&
			dst->width >= 43 && dst->height >= 18) {
			for (y = 9; y < 18; y++) {
				BitmapCol* srcRow = Bitmap_GetRow(&bmp, y);
				BitmapCol* dstRow = Bitmap_GetRow(dst, y);
				for (x = 16; x < 43; x++) dstRow[x] = srcRow[x];
			}
		}
		Mem_Free(bmp.scan0);
		return 0;
	}
	/* armor/cloth_1.png etc -> armor_cloth_1.png (flat names in the pack) */
	if (String_CaselessStarts(path, &armorPrefix)) {
		cc_string name; char nameBuffer[64];
		String_InitArray(name, nameBuffer);
		String_Format1(&name, "armor_%s", &basename);
		e = ZipEntries_Find(&name);
		if (!e) return 0; /* power.png and other unclaimed armor entries */
		return ZipEntry_ExtractData(e, data, source);
	}
	if (String_CaselessEqualsConst(path, "gui/inventory.png")) {
		static const cc_string invPng = String_FromConst("inventory.png");
		e = ZipEntries_Find(&invPng);
		return ZipEntry_ExtractData(e, data, source);
	}
	if (String_CaselessEqualsConst(path, "gui/crafting.png")) {
		static const cc_string craftPng = String_FromConst("crafting.png");
		e = ZipEntries_Find(&craftPng);
		return ZipEntry_ExtractData(e, data, source);
	}
	if (String_CaselessEqualsConst(path, "gui/furnace.png")) {
		static const cc_string furnPng = String_FromConst("furnace.png");
		e = ZipEntries_Find(&furnPng);
		return ZipEntry_ExtractData(e, data, source);
	}
	if (String_CaselessEqualsConst(path, "gui/container.png")) {
		static const cc_string contPng = String_FromConst("container.png");
		e = ZipEntries_Find(&contPng);
		return ZipEntry_ExtractData(e, data, source);
	}
	if (String_CaselessEqualsConst(path, "terrain/sun.png")) {
		static const cc_string sunPng = String_FromConst("sun.png");
		e = ZipEntries_Find(&sunPng);
		return ZipEntry_ExtractData(e, data, source);
	}
	if (String_CaselessEqualsConst(path, "terrain/moon.png")) {
		static const cc_string moonPng = String_FromConst("moon.png");
		e = ZipEntries_Find(&moonPng);
		return ZipEntry_ExtractData(e, data, source);
	}

	e = ZipEntries_Find(&itemsPng);
	return ZipEntry_ExtractData(e, data, source);
}

static cc_result BetaPatcher_ExtractItems(struct HttpRequest* req) {
	struct Stream src;
	struct ZipEntry entries[64];
	Stream_ReadonlyMemory(&src, req->data, req->size);

	return Zip_Extract(&src,
			BetaPatcher_SelectEntry, BetaPatcher_ProcessEntry,
			entries, Array_Elems(entries));
}

static void PatchTerrainTile(struct Bitmap* src, int srcX, int srcY, int tileX, int tileY) {
	static const cc_string terrainPng = String_FromConst("terrain.png");
	struct ResourceZipEntry* entry    = ZipEntries_Find(&terrainPng);
	struct Bitmap* dst = &entry->value.bmp;
	/* Can happen sometimes happen when allocating memory for terrain.png fails */
	if (!dst->scan0) return;

	Bitmap_UNSAFE_CopyBlock(srcX, srcY, tileX * 16, tileY * 16, src, dst, 16);
}

/* Copies a tile shifted DOWN one pixel. The torch's TOP face is cropped by */
/*  the engine to the block bounds (pixels x 7-9, y 7-9), but the genuine */
/*  renderBlockTorch samples the ember at y 6-8 - a 1px-shifted copy of the */
/*  torch tile makes the bounds crop land on the right pixels. */
static void PatchTerrainTileShifted(struct Bitmap* src, int srcX, int srcY, int tileX, int tileY) {
	static const cc_string terrainPng = String_FromConst("terrain.png");
	struct ResourceZipEntry* entry    = ZipEntries_Find(&terrainPng);
	struct Bitmap* dst = &entry->value.bmp;
	int y;
	if (!dst->scan0) return;

	for (y = 0; y < 15; y++) {
		Mem_Copy(Bitmap_GetRow(dst, tileY * 16 + y + 1) + tileX * 16,
				 Bitmap_GetRow(src, srcY + y) + srcX, 16 * BITMAPCOLOR_SIZE);
	}
}


/* the x,y of tiles in terrain.png which get patched */
static const struct TilePatch { const char* name; cc_uint8 x1,y1, x2,y2; } modern_tiles[12] = {
	{ "assets/minecraft/textures/blocks/sandstone_bottom.png", 9,3 },
	{ "assets/minecraft/textures/blocks/sandstone_normal.png", 9,2 },
	{ "assets/minecraft/textures/blocks/sandstone_top.png", 9,1, },
	{ "assets/minecraft/textures/blocks/quartz_block_lines_top.png", 10,3, 10,1 },
	{ "assets/minecraft/textures/blocks/quartz_block_lines.png", 10,2 },
	{ "assets/minecraft/textures/blocks/stonebrick.png", 4,3 },
	{ "assets/minecraft/textures/blocks/snow.png", 2,3 },
	{ "assets/minecraft/textures/blocks/wool_colored_blue.png",  3,5 },
	{ "assets/minecraft/textures/blocks/wool_colored_brown.png", 2,5 },
	{ "assets/minecraft/textures/blocks/wool_colored_cyan.png",  4,5 },
	{ "assets/minecraft/textures/blocks/wool_colored_green.png", 1,5 },
	{ "assets/minecraft/textures/blocks/wool_colored_pink.png",  0,5 }
};

CC_NOINLINE static const struct TilePatch* ModernPatcher_GetTile(const cc_string* path) {
	int i;
	for (i = 0; i < Array_Elems(modern_tiles); i++) {
		if (String_CaselessEqualsConst(path, modern_tiles[i].name)) return &modern_tiles[i];
	}
	return NULL;
}

static cc_result ModernPatcher_PatchTile(struct Stream* data, const struct TilePatch* tile) {
	struct Bitmap bmp;
	cc_result res;

	if ((res = Png_Decode(&bmp, data))) return res;
	PatchTerrainTile(&bmp, 0, 0, tile->x1, tile->y1);

	/* only quartz needs copying to two tiles */
	if (tile->y2) PatchTerrainTile(&bmp, 0, 0, tile->x2, tile->y2);

	Mem_Free(bmp.scan0);
	return 0;
}


static cc_bool ModernPatcher_SelectEntry(const cc_string* path) {
	return
		String_CaselessEqualsConst(path, "assets/minecraft/textures/environment/snow.png") ||
		String_CaselessEqualsConst(path, "assets/minecraft/textures/entity/chicken.png")   ||
		String_CaselessEqualsConst(path, "assets/minecraft/textures/blocks/fire_layer_1.png") ||
		ModernPatcher_GetTile(path) != NULL;
}

static cc_result ModernPatcher_MakeAnimations(struct Stream* data) {
	static const cc_string animsPng = String_FromConst("animations.png");
	struct ResourceZipEntry* entry;
	struct Bitmap* anim;
	struct Bitmap bmp;
	cc_result res;
	int i;

	entry = ZipEntries_Find(&animsPng);
	anim  = &entry->value.bmp;
	Bitmap_TryAllocate(anim, 512, 16);

	if (!anim->scan0) return ERR_OUT_OF_MEMORY;
	if ((res = Png_Decode(&bmp, data))) return res;

	for (i = 0; i < 512; i += 16) {
		Bitmap_UNSAFE_CopyBlock(0, i, i, 0, &bmp, anim, 16);
	}

	Mem_Free(bmp.scan0); 
	return 0;
}

static cc_result ModernPatcher_ProcessEntry(const cc_string* path, struct Stream* data, struct ZipEntry* source) {
	struct ResourceZipEntry* e;
	const struct TilePatch* tile;
	cc_string name;

	if (String_CaselessEqualsConst(path, "assets/minecraft/textures/environment/snow.png")
		|| String_CaselessEqualsConst(path, "assets/minecraft/textures/entity/chicken.png")) {
		name = *path;
		Utils_UNSAFE_GetFilename(&name);

		e = ZipEntries_Find(&name);
		return ZipEntry_ExtractData(e, data, source);
	}

	if (String_CaselessEqualsConst(path, "assets/minecraft/textures/blocks/fire_layer_1.png")) {
		return ModernPatcher_MakeAnimations(data);
	}

	tile = ModernPatcher_GetTile(path);
	return ModernPatcher_PatchTile(data, tile);
}

static cc_result ModernPatcher_ExtractFiles(struct HttpRequest* req) {
	struct Stream src;
	struct ZipEntry entries[64];
	Stream_ReadonlyMemory(&src, req->data, req->size);

	return Zip_Extract(&src, 
			ModernPatcher_SelectEntry, ModernPatcher_ProcessEntry,
			entries, Array_Elems(entries));
}


static cc_result TerrainPatcher_Process(struct HttpRequest* req) {
	struct Bitmap bmp;
	struct Stream src;
	cc_result res;

	Stream_ReadonlyMemory(&src, req->data, req->size);
	if ((res = Png_Decode(&bmp, &src))) return res;

	PatchTerrainTile(&bmp,  0,0, 3,3);
	PatchTerrainTile(&bmp, 16,0, 6,3);
	PatchTerrainTile(&bmp, 32,0, 6,2);

	PatchTerrainTile(&bmp,  0,16,  5,3);
	PatchTerrainTile(&bmp, 16,16,  6,5);
	PatchTerrainTile(&bmp, 32,16, 11,0);

	Mem_Free(bmp.scan0);
	return 0;
}

static cc_result NewTextures_ExtractGui(struct HttpRequest* req) {
	static const cc_string guiPng = String_FromConst("gui.png");
	struct ResourceZipEntry* entry = ZipEntries_Find(&guiPng);

	entry->value.data = req->data;
	entry->size       = req->size;

	req->data = NULL; /* don't free memory yet */
	return 0;
}

static cc_result Classic0023Patcher_PatchBlocks(struct HttpRequest* req, const int* targets) {
	struct Bitmap bmp;
	struct Stream src;
	cc_result res;

	Stream_ReadonlyMemory(&src, req->data, req->size);
	if ((res = Png_Decode(&bmp, &src))) return res;

	while (*targets)
	{
		PatchTerrainTile(&bmp, 0,0, *targets >> 8, *targets & 0xFF);
		targets++;
	}

	Mem_Free(bmp.scan0);
	return 0;
}

static cc_result Classic0023Patcher_OldGoldBlock(struct HttpRequest* req) {
	static const int targets[] = { (8 << 8) | 1, (8 << 8) | 2, (8 << 8) | 3, 0 };

	return Classic0023Patcher_PatchBlocks(req, targets);
}

static cc_result Classic0023Patcher_OldGoldOre(struct HttpRequest* req) {
	static const int targets[] = { (0 << 8) | 2, 0 };

	return Classic0023Patcher_PatchBlocks(req, targets);
}

static cc_result Classic0023Patcher_OldBlackWool(struct HttpRequest* req) {
	static const int targets[] = { (13 << 8) | 4, 0 };

	return Classic0023Patcher_PatchBlocks(req, targets);
}

static cc_result Classic0023Patcher_OldGrayWool(struct HttpRequest* req) {
	static const int targets[] = { (14 << 8) | 4, 0 };

	return Classic0023Patcher_PatchBlocks(req, targets);
}


/*########################################################################################################################*
*-----------------------------------------------Minecraft Classic texture assets------------------------------------------*
*#########################################################################################################################*/
static cc_bool allZipEntriesExist;
static int zipEntriesFound;

static cc_bool DefaultZip_SelectEntry(const cc_string* path) {
	cc_string name = *path;
	Utils_UNSAFE_GetFilename(&name);

	if (ZipEntries_Find(&name)) zipEntriesFound++;
	return false;
}

static void MCCTextures_CheckExistence(void) {
	cc_string path  = String_FromReadonly(Game_Version.DefaultTexpack);
	zipEntriesFound = 0;

	ZipFile_InspectEntries(&path, DefaultZip_SelectEntry);
	/* >= in case somehow have say "gui.png", "GUI.png" */
	allZipEntriesExist = zipEntriesFound >= Array_Elems(defaultZipEntries);

	/* Need touch.png from ClassiCube textures */
	if (!allZipEntriesExist) ccTexturesExist = false;
}

static void MCCTextures_CountMissing(void) {
	int i;
	if (allZipEntriesExist) return;

	/* old gold texture only needed in 0.0.23 and earlier */
	if (Game_Version.Version > VERSION_0023) {
		numDefaultZipSources = DEFAULTZIP_0030_ENTRIES_COUNT;
		defaultZipSources    = defaultZipSources_0030_0023;
	} else {
		numDefaultZipSources = Array_Elems(defaultZipSources_0030_0023);
		defaultZipSources    = defaultZipSources_0030_0023;
	}

	for (i = 0; i < numDefaultZipSources; i++) {
		Resources_MissingCount++;
		Resources_MissingSize += defaultZipSources[i].size;
	}
}


/*########################################################################################################################*
*------------------------------------------Minecraft Classic texture assets fetching -------------------------------------*
*#########################################################################################################################*/
static void MCCTextures_DownloadAssets(void) {
	cc_string url;
	int i;
	if (allZipEntriesExist) return;
	numDefaultZipProcessed = 0;

	for (i = 0; i < numDefaultZipSources; i++)
	{
		url = String_FromReadonly(defaultZipSources[i].url);
		defaultZipSources[i].reqID = Http_AsyncGetData(&url, 0);
		defaultZipSources[i].downloaded = false;
	}
}

static const char* MCCTextures_GetRequestName(int reqID) {
	int i;
	for (i = 0; i < numDefaultZipSources; i++) 
	{
		if (reqID == defaultZipSources[i].reqID) return defaultZipSources[i].name;
	}
	return NULL;
}


/*########################################################################################################################*
*------------------------------------------Minecraft Classic texture assets processing -----------------------------------*
*#########################################################################################################################*/
static void MCCTextures_CreateDefaultZip(void) {
	cc_string path = String_FromReadonly(Game_Version.DefaultTexpack);
	ZipFile_Create(&path, defaultZipEntries, Array_Elems(defaultZipEntries));
	MCCTextures_ResetState();
}

static void MCCTextures_CheckSource(struct ZipfileSource* source) {
	struct HttpRequest item;
	cc_result res;
	if (!Fetcher_Get(source->reqID, &item)) return;
	
	source->downloaded = true;
	res = source->Process(&item);

	if (res) {
		cc_string name = String_FromReadonly(source->name);
		Logger_SysWarn2(res, "making", &name);
	}
	HttpRequest_Free(&item);

	if (++numDefaultZipProcessed < numDefaultZipSources) return;
	MCCTextures_CreateDefaultZip();
}

static void MCCTextures_CheckStatus(void) {
	int i;
	for (i = 0; i < numDefaultZipSources; i++) 
	{
		if (defaultZipSources[i].downloaded) continue;
		MCCTextures_CheckSource(&defaultZipSources[i]);
	}
}

static const struct AssetSet mccTexsAssetSet = {
	MCCTextures_CheckExistence,
	MCCTextures_CountMissing,
	MCCTextures_DownloadAssets,
	MCCTextures_GetRequestName,
	MCCTextures_CheckStatus,
	MCCTextures_ResetState
};


/*########################################################################################################################*
*-----------------------------------------------------------Fetcher-------------------------------------------------------*
*#########################################################################################################################*/
cc_bool Fetcher_Working, Fetcher_Completed, Fetcher_Failed;
int Fetcher_Downloaded;
FetcherErrorCallback Fetcher_ErrorCallback;

static const struct AssetSet* const asset_sets[] = {
	&ccTexsAssetSet,
	&mccTexsAssetSet,
#ifndef CC_BUILD_NOMUSIC
	&mccMusicAssetSet,
#endif
#ifndef CC_BUILD_NOSOUNDS
	&mccSoundAssetSet
#endif
};

static void ResetState() {
	int i;
	Resources_MissingCount = 0;
	Resources_MissingSize  = 0;

	for (i = 0; i < Array_Elems(asset_sets); i++)
	{
		asset_sets[i]->ResetState();
	}
}

void Resources_CheckExistence(void) {
	int i;
	ResetState();

	for (i = 0; i < Array_Elems(asset_sets); i++)
	{
		asset_sets[i]->CheckExistence();
	}

	for (i = 0; i < Array_Elems(asset_sets); i++)
	{
		asset_sets[i]->CountMissing();
	}
}

const char* Fetcher_RequestName(int reqID) {
	const char* name;
	int i;

	for (i = 0; i < Array_Elems(asset_sets); i++)
	{
		if ((name = asset_sets[i]->GetRequestName(reqID))) return name;
	}
	return NULL;
}

void Fetcher_Run(void) {
	int i;

	Fetcher_Failed     = false;
	Fetcher_Downloaded = 0;
	Fetcher_Working    = true;
	Fetcher_Completed  = false;

	for (i = 0; i < Array_Elems(asset_sets); i++)
	{
		asset_sets[i]->DownloadAssets();
	}

	Utils_EnsureDirectory("texpacks");
	Utils_EnsureDirectory("audio");
}

static void Fetcher_Finish(void) {
	Fetcher_Completed = true;
	Fetcher_Working   = false;
	ResetState();
}

static void Fetcher_Fail(struct HttpRequest* item) {
	Http_ClearPending();

	/* Only show error for first failed download */
	if (!Fetcher_Failed) Fetcher_ErrorCallback(item);
	Fetcher_Failed = true;
}

CC_NOINLINE static cc_bool Fetcher_Get(int reqID, struct HttpRequest* item) {
	if (!Http_GetResult(reqID, item)) return false;

	if (item->success) {
		Fetcher_Downloaded++;
		return true;
	}

	Fetcher_Fail(item);
	HttpRequest_Free(item);
	
	Fetcher_Finish();
	return false;
}

/* TODO: Implement this.. */
/* TODO: How expensive is it to constantly do 'Get' over and over */
void Fetcher_Update(void) {
	int i;
	for (i = 0; i < Array_Elems(asset_sets); i++)
	{
		asset_sets[i]->CheckStatus();
	}

	if (Fetcher_Downloaded != Resources_MissingCount) return; 
	Fetcher_Finish();
}
#endif
