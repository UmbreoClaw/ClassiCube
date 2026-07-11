#ifndef CC_INDEVTEST_H
#define CC_INDEVTEST_H
#include "Core.h"
#include "Graphics.h"
#include "BlockID.h"
#include "Vectors.h"
#include "SurvivalTest.h" /* struct SurvivalSlot (shared container slot type) */
CC_BEGIN_HEADER

/* Indev (in-20100223) gamemode - version layer on top of the survival core.
   Copyright 2014-2025 ClassiCube | Licensed under BSD-3
*/
struct IGameComponent;
extern struct IGameComponent IndevTest_Component;

/* Whether Indev mode is active. Implies the survival core (SurvivalTest) is */
/*  active too - Indev is a version layer over it, not a separate system. */
/*  When false, every Indev hook is a no-op and both creative mode and the */
/*  faithful c0.30-s survival mode are completely unaffected. */
extern cc_bool IndevTest_Enabled;

/* Terrain tile the procedural fire animation is drawn into every tick */
/*  (TextureFlamesFX port in Animations.c) - a spare tile in row 7, next to */
/*  our crops (107-114) / farmland (115-116) / torch-top (117) patches. The */
/*  burning-mob billboards (SurvivalTest_RenderMobFires) sample it. */
#define INDEV_FIRE_TEX_LOC 118

/* The items.png atlas texture (0 until a texture pack supplies it). Item */
/*  sprite rendering (hotbar/hand/drops) bails gracefully while it's 0. */
GfxResourceID IndevTest_ItemsTex(void);
/* The painting art atlas texture (art/kz.png), 0 until a pack provides it. */
GfxResourceID IndevTest_KzTex(void);
/* The genuine gui/inventory.png (0 until loaded) - the Indev inventory */
/*  screen draws its whole panel from this 176x166 region. */
GfxResourceID IndevTest_InvGuiTex(void);
/* The workbench gui/crafting.png (0 until loaded) - the 3x3 crafting screen. */
GfxResourceID IndevTest_CraftGuiTex(void);
/* Whether the block is the Indev workbench (right-click opens the 3x3 grid). */
cc_bool IndevTest_IsWorkbench(BlockID b);
/* Wall-mounted torch variants (genuine torch metadata 1-4) - the chunk */
/*  builder renders them tilted via the genuine renderBlockTorch geometry. */
cc_bool IndevTest_IsWallTorch(BlockID b);
int IndevTest_WallTorchMeta(BlockID b);
/* Atlas UVs of an item id's sprite (items.png, 16x16 grid). False when the */
/*  id isn't a known Indev item - callers skip drawing then. */
cc_bool IndevTest_ItemSpriteUV(int id, float* u1, float* v1, float* u2, float* v2);
/* Item display name (NULL for unknown ids), and name -> 256+ id lookup */
/*  ("iron_pickaxe"/"Iron Pickaxe"/"ironpickaxe" all match; -1 = no match). */
/*  Both are debug/chat helpers for the /client give command. */
const char* IndevTest_ItemName(int id);
int IndevTest_FindItemByName(const cc_string* name);
int IndevTest_FindBlockByName(const cc_string* name);
/* Heal amount when id is an edible Indev item (apple/soup/bread/porkchops), */
/*  else 0. Always 0 while Indev mode is off. */
int IndevTest_ItemFoodHeal(int id);
/* ItemTool.maxDamage (32 << tier) for damageable tools, else 0. */
int IndevTest_ToolMaxDamage(int id);
/* hitEntity/onBlockDestroyed wear: swords 1/2, tools 2/1, others 0. */
int IndevTest_ToolUseWear(int id, cc_bool entityHit);
/* ItemArmor: piece worn (0 helmet / 1 chest / 2 legs / 3 boots), or -1 when
    the id is not armor. MaxDamage/Reduce return 0 for non-armor ids. */
int IndevTest_ArmorPiece(int id);
int IndevTest_ArmorMaxDamage(int id);
int IndevTest_ArmorReduce(int id);
/* Whether the id is the bow item (fires arrows on right-click). */
cc_bool IndevTest_IsBow(int id);
/* Item.getStrVsBlock: dig speed multiplier of the held id against a block. */
/*  (tier+1)*2 when the block is in the tool's blocksEffectiveAgainst list, */
/*  swords a flat 1.5, everything else (incl. hoes) 1. */
float IndevTest_StrVsBlock(int id, BlockID block);
/* Melee damage of the held id (bare fist / non-weapons = 1; tools base+tier; */
/*  swords 4 + tier*2), per Minecraft.java's attack path. */
int IndevTest_MeleeDamage(int id);
/* Whether a block dropped when broken by hand/held item - false for rock/iron */
/*  material (stone-sound) blocks unless a pickaxe of sufficient level is held. */
cc_bool IndevTest_CanHarvest(int heldId, BlockID block);
/* Matches a gw*gh crafting grid of full-space ids (0 = empty) against the */
/*  in-20100223 recipe list. True + result id/count when a recipe fits. */
cc_bool IndevTest_MatchRecipe(const cc_uint16* grid, int gw, int gh, int* outId, int* outCount);

/* The furnace gui/furnace.png and chest gui/container.png (0 until loaded). */
GfxResourceID IndevTest_FurnGuiTex(void);
GfxResourceID IndevTest_ContGuiTex(void);

/* Container (chest/furnace) tile entities - per-position storage, ported */
/*  from TileEntityChest/TileEntityFurnace (in-20100223). */
enum IndevContainerKind {
	INDEV_CONTAINER_NONE  = 0,
	INDEV_CONTAINER_CHEST = 1, /* 27 slots */
	INDEV_CONTAINER_FURNACE = 2 /* 3 slots: 0 input, 1 fuel, 2 output */
};
/* Whether the held item id is a hoe (any tier). */
cc_bool IndevTest_IsHoe(int id);
/* ItemHoe/ItemSeeds.onItemUse: hoe tills grass/dirt to farmland (with the
    genuine 1-in-8 grass seed drop), seeds plant crops on farmland. True when
    the right-click was consumed. */
cc_bool IndevTest_UseHeldItem(int heldId, IVec3 pos);
/* .mclevel Data-nibble metadata: container facing / farmland moisture /
    crop stage on save, and its application to a loaded base block. */
int     IndevTest_BlockDataMeta(BlockID b);
BlockID IndevTest_ApplyDataMeta(BlockID b, int meta);
/* Position-aware forms (fire keeps its age in a per-position store). */
int     IndevTest_BlockDataMetaAt(int index, BlockID b);
BlockID IndevTest_ApplyDataMetaAt(int index, BlockID b, int meta);
/* First-person extruded held item (ItemRenderer port): whether the id uses */
/*  it, and the texture bind + UV rect for building its mesh. */
cc_bool IndevTest_HeldIsExtruded(int id);
cc_bool IndevTest_BindHeldTexture(int id, TextureRec* rec);
/* Whether a dropped id renders as an upright sprite instead of a miniature */
/*  block: item ids, sprite-draw blocks (flowers etc) and torches. False */
/*  outside Indev mode (c0.30 renders every block drop as its cropped cube). */
cc_bool IndevTest_DropIsSprite(int id);
/* Whether the block is an Indev crop stage (85-92) - the chunk builder */
/*  renders those as BlockCrops' "#" row pattern instead of the X-cross */
/*  sprite. Always false outside Indev mode. */
cc_bool IndevTest_IsCropBlock(BlockID b);
/* Whether the block is a container (chest/furnace) - right-clicking one is */
/*  always consumed (blockActivated returns true even when a blocked chest */
/*  refuses to open), so no block gets placed against it. */
cc_bool IndevTest_IsContainerBlock(BlockID b);
/* Opens the container at pos if that block is a chest/furnace: finds (or */
/*  lazily creates) its tile entity and returns its kind, or NONE. Mirrors */
/*  BlockChest.blockActivated's rule that a chest with a solid block directly */
/*  above it refuses to open. */
int  IndevTest_OpenContainer(IVec3 pos);
/* Kind of the currently open container (NONE when no container screen). */
int  IndevTest_OpenKind(void);
/* Closes the open container (contents stay in the tile entity). */
void IndevTest_CloseContainer(void);
/* Slot i of the OPEN container (0..26 chest, 0..2 furnace). Never NULL - */
/*  returns a discard slot when nothing is open, so clicks can't corrupt. */
struct SurvivalSlot* IndevTest_ContainerSlot(int i);
/* Furnace GUI overlays: flame height 0..12 (burnTime*12/currentItemBurnTime) */
/*  and arrow width 0..24 (cookTime*24/200) of the OPEN furnace. */
int  IndevTest_FurnaceBurnScaled(void);
int  IndevTest_FurnaceCookScaled(void);
int  IndevTest_FurnaceIsBurning(void);

/* .mclevel format support: bidirectional block id mapping between the */
/*  genuine Indev id space (torch 50, chest 54, workbench 58, furnace 61/62) */
/*  and ours (66-70 custom ids; lossy fallbacks for the rest - see tables). */
BlockRaw IndevTest_BlockToIndev(BlockRaw b);
BlockRaw IndevTest_BlockFromIndev(BlockRaw b);
/* Directional container support: the inventory/drop (canonical) form of a */
/*  block, its Indev facing metadata (2-5), and the directional variant of */
/*  a canonical container for a given metadata. */
BlockID IndevTest_CanonicalBlock(BlockID b);
int     IndevTest_BlockFacingMeta(BlockID b);
BlockID IndevTest_FacingVariant(BlockID canonical, int meta);
/* Runs the container-removal lifecycle (chest scatter + tile entity */
/*  destruction) - for removal paths that don't raise BlockChanged, like */
/*  explosions. Safe to call for any block id. */
void IndevTest_NotifyBlockRemoved(IVec3 coords, BlockID oldBlock);

/* Day/night cycle: world time in ticks (0..23999, 20 minutes per day) and */
/*  the Environment SkyBrightness (> 15 = "paradise" maps, always day). */
/*  Round-trips through .mclevel's TimeOfDay/SkyBrightness tags. */
int  IndevTest_WorldTime(void);
void IndevTest_SetWorldTime(int t);
void IndevTest_SetSkyBrightness(int b);
int  IndevTest_SkyBrightness(void);
/* World.getBlockLightValue: combined sky+block light level (0-15). */
int  IndevTest_LightLevel(int x, int y, int z);
/* The current day/night sky light level (4..15) - what the deobfuscated
    source misnames World.skylightSubtracted. 15 outside Indev mode. */
int  IndevTest_CurSkyLight(void);
/* World.tick's random block updates at the genuine rate: volume/200 ticks
    per game tick via the genuine LCG. Replaces the engine's much sparser
    3-per-chunk loop while Indev mode is on (see Physics_Tick). */
void IndevTest_TickRandomBlocks(void);
/* World.randomDisplayUpdates: 1000 random cells in the 33^3 cube around
    the player each tick, running the visual-only randomDisplayTick of
    what it lands on (fire crackle + largesmoke, torch/furnace smoke and
    flame flecks). Client ambience - separate RNG, no world-state effect. */
void IndevTest_RandomDisplayTicks(void);
/* Block.canPlaceBlockAt for the Indev additions (currently the chest's
    no-triples rule) - true if placing block b at pos is legal. */
cc_bool IndevTest_CanPlaceBlockAt(BlockID b, IVec3 pos);
/* Full-daylight base env colours (the live Env colours are time-scaled). */
PackedCol IndevTest_BaseSkyCol(void);
PackedCol IndevTest_BaseFogCol(void);
PackedCol IndevTest_BaseCloudsCol(void);
/* Sets the live Env colours AND the day/night baseline together - used by
    the Indev generator's theme environments (which apply after the map-
    loaded snapshot has already run). */
void IndevTest_SetBaseEnvColors(PackedCol sky, PackedCol fog, PackedCol clouds);
/* Renders the sun, moon and star field (RenderGlobal.renderSky port) - */
/*  called from the 3D pass right after the sky plane, before clouds. */
void IndevTest_RenderSky(void);
/* Whether a tile entity exists at a position (save-side world scan). */
cc_bool IndevTest_HasTE(int x, int y, int z);
/* Tile entity iteration for .mclevel save: next used pool index after prev */
/*  (start with -1), or -1 when done; then info + per-slot reads. */
int  IndevTest_TENext(int prev);
void IndevTest_TEInfo(int i, int* kind, IVec3* pos, int* burn, int* cook);
void IndevTest_TEItem(int i, int slot, int* id, int* count, int* damage);
/* .mclevel load: recreates one tile entity with its contents. */
void IndevTest_RestoreTE(int kind, int x, int y, int z, int burn, int cook,
						 const cc_uint16* ids, const cc_int16* counts, const cc_int16* damages);

CC_END_HEADER
#endif
