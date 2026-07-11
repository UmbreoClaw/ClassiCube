#include "Screens.h"
#include "Widgets.h"
#include "Game.h"
#include "Event.h"
#include "Platform.h"
#include "Inventory.h"
#include "Drawer2D.h"
#include "Graphics.h"
#include "Funcs.h"
#include "TexturePack.h"
#include "Model.h"
#include "Generator.h"
#include "Server.h"
#include "Chat.h"
#include "ExtMath.h"
#include "Window.h"
#include "Camera.h"
#include "Http.h"
#include "Block.h"
#include "Menus.h"
#include "World.h"
#include "Input.h"
#include "Utils.h"
#include "Options.h"
#include "InputHandler.h"
#include "Protocol.h"
#include "SurvivalTest.h"
#include "IndevTest.h"
#include "IndevArmor.h"
#include "IndevGen.h"
#include "IsometricDrawer.h"

#define CHAT_MAX_STATUS Array_Elems(Chat_Status)
#define CHAT_MAX_BOTTOMRIGHT Array_Elems(Chat_BottomRight)
#define CHAT_MAX_CLIENTSTATUS Array_Elems(Chat_ClientStatus)

int Screen_FInput(void* s, int key, struct InputDevice* device) { return false; }
int Screen_FKeyPress(void* s, char keyChar)     { return false; }
int Screen_FText(void* s, const cc_string* str) { return false; }
int Screen_FMouseScroll(void* s, float delta)   { return false; }
int Screen_FPointer(void* s, int id, int x, int y) { return false; }

int Screen_TInput(void* s, int key, struct InputDevice* device) { return true; }
int Screen_TKeyPress(void* s, char keyChar)     { return true; }
int Screen_TText(void* s, const cc_string* str) { return true; }
int Screen_TMouseScroll(void* s, float delta)   { return true; }
int Screen_TPointer(void* s, int id, int x, int y) { return true; }

void Screen_NullFunc(void* screen) { }
void Screen_NullUpdate(void* screen, float delta) { }

/* TODO: Remove these */
struct HUDScreen;
struct ChatScreen;
static struct HUDScreen*  Gui_HUD;
static struct ChatScreen* Gui_Chat;
static cc_bool tablist_active;

static cc_bool InventoryScreen_IsHotbarActive(void);
CC_NOINLINE static cc_bool IsOnlyChatActive(void) {
	struct Screen* s;
	int i;

	for (i = 0; i < Gui.ScreensCount; i++) {
		s = Gui_Screens[i];
		if (s->grabsInput && s != (struct Screen*)Gui_Chat) return false;
	}
	return true;
}


/*########################################################################################################################*
*--------------------------------------------------------HUDScreen--------------------------------------------------------*
*#########################################################################################################################*/
static struct HUDScreen {
	Screen_Body
	struct FontDesc font;
	struct TextWidget line1, line2;
	struct TextAtlas posAtlas;
	/* Separate, unpadded digit atlas for hotbar stack counts, so the glyph */
	/*  metrics are exact (the padded posAtlas threw off corner alignment). */
	struct TextAtlas countAtlas;
	float accumulator;
	int frames, posCount;
	cc_bool hacksChanged;
	float lastSpeed;
	int lastFov;
	int lastX, lastY, lastZ;
	struct HotbarWidget hotbar;
	/* Survival HUD text labels, rasterised on change like line1/line2: */
	/*  "Score: &eN" top-right and "Arrows: N" beside the heart row. */
	struct TextWidget score, arrows, indevTitle;
	int heartCount;     /* number of heart vertices built last frame */
	int countVertices;  /* number of stack-count vertices built last frame */
	int bubbleCount;    /* number of air-bubble vertices built last frame */
	int lastHealth;     /* SurvivalTest_Health value from last rebuild */
	int lastInvVersion; /* SurvivalTest_InvVersion() from last rebuild */
	int lastArrows;     /* SurvivalTest_ArrowCount() value from last rebuild */
	int lastScore;      /* SurvivalTest_Score() value from last rebuild */
} HUDScreen_Instance CC_BIG_VAR;

/* Each integer can be at most 10 digits + minus prefix */
#define POSITION_VAL_CHARS 11
/* [PREFIX] [(] [X] [,] [Y] [,] [Z] [)] */
#define POSITION_HUD_CHARS (1 + 1 + POSITION_VAL_CHARS + 1 + POSITION_VAL_CHARS + 1 + POSITION_VAL_CHARS + 1)
/* 10 heart backgrounds + up to 10 ghost hearts (invuln glow) + up to 10 */
/*  filled hearts + up to 10 armor icons = 40 quads = 160 vertices. (The */
/*  old budget of 80 could overflow into the counts region while glowing.) */
#define SURVIVAL_HEARTS_MAX_VERTICES 160
/* Up to 2 digits per hotbar slot for stack counts (4 vertices per digit) */
#define SURVIVAL_COUNTS_MAX_VERTICES (SURVIVAL_HOTBAR_SLOTS * 2 * 4)
/* Air bubble row when the head is underwater: up to 10 bubbles (4 verts each) */
#define SURVIVAL_BUBBLES_MAX_VERTICES (10 * 4)

/* Absolute vertex offsets of each region within the HUD vertex buffer. The */
/*  crosshair (4) + line1 (4) + line2 (4) + hotbar are built sequentially up */
/*  front (4 + TEXTWIDGET_MAX*2 + HOTBAR_MAX_VERTICES vertices); the survival */
/*  regions after that live at these fixed offsets. The Score and Arrows */
/*  labels are one textured quad each (TEXTWIDGET_MAX vertices). */
#define HUD_OFS_POSITION (4 + TEXTWIDGET_MAX * 2 + HOTBAR_MAX_VERTICES)
#define HUD_OFS_HEARTS   (HUD_OFS_POSITION + POSITION_HUD_CHARS * 4)
#define HUD_OFS_COUNTS   (HUD_OFS_HEARTS   + SURVIVAL_HEARTS_MAX_VERTICES)
#define HUD_OFS_BUBBLES  (HUD_OFS_COUNTS   + SURVIVAL_COUNTS_MAX_VERTICES)
#define HUD_OFS_SCORE    (HUD_OFS_BUBBLES  + SURVIVAL_BUBBLES_MAX_VERTICES)
#define HUD_OFS_ARROWS   (HUD_OFS_SCORE    + TEXTWIDGET_MAX)
#define HUD_MAX_VERTICES (HUD_OFS_ARROWS   + TEXTWIDGET_MAX)

/* Defined further down (beside the survival mesh builders), forward-declared */
/*  here since ContextRecreated rebuilds these label textures. */
static void HUDScreen_RemakeScore(struct HUDScreen* s);
static void HUDScreen_RemakeArrows(struct HUDScreen* s);

static void HUDScreen_RemakeLine1(struct HUDScreen* s) {
	cc_string status; char statusBuffer[STRING_SIZE * 2];
	int indices, ping, fps;
	float real_fps;

	String_InitArray(status, statusBuffer);
	/* Don't remake texture when FPS isn't being shown */
	if (!Gui.ShowFPS && s->line1.tex.ID) return;
	fps = s->accumulator == 0 ? 1 : (int)(s->frames / s->accumulator);

	if (Gfx.ReducedPerfMode || (Gfx.ReducedPerfModeCooldown > 0)) {
		String_AppendConst(&status, "(low perf mode), ");
		Gfx.ReducedPerfModeCooldown--;
	} else if (fps == 0) {
		/* Running at less than 1 FPS.. */
		real_fps = s->frames / s->accumulator;
		String_Format1(&status, "%f1 fps, ", &real_fps);
	} else {
		String_Format1(&status, "%i fps, ", &fps);
	}

	if (Game_ClassicMode) {
		String_Format1(&status, "%i chunk updates", &Game.ChunkUpdates);
	} else {
		if (Game.ChunkUpdates) {
			String_Format1(&status, "%i chunks/s, ", &Game.ChunkUpdates);
		}

		indices = ICOUNT(Game_Vertices);
		String_Format1(&status, "%i vertices", &indices);

		ping = Ping_AveragePingMS();
		if (ping) String_Format1(&status, ", ping %i ms", &ping);
	}
	TextWidget_Set(&s->line1, &status, &s->font);
	s->dirty = true;
}

static void HUDScreen_BuildPosition(struct HUDScreen* s, struct VertexTextured* data) {
	struct VertexTextured* cur = data;
	struct TextAtlas* atlas = &s->posAtlas;
	struct Texture tex;
	IVec3 pos;

	/* Make "Position: " prefix */
	tex = atlas->tex; 
	tex.x     = 2 + DisplayInfo.ContentOffsetX;
	tex.width = atlas->offset;
	Gfx_Make2DQuad(&tex, PACKEDCOL_WHITE, &cur);

	IVec3_Floor(&pos, &Entities.CurPlayer->Base.Position);
	atlas->curX = tex.x + tex.width;

	/* Make (X, Y, Z) suffix */
	TextAtlas_Add(atlas,       13, &cur);
	TextAtlas_AddInt(atlas, pos.x, &cur);
	TextAtlas_Add(atlas,       11, &cur);
	TextAtlas_AddInt(atlas, pos.y, &cur);
	TextAtlas_Add(atlas,       11, &cur);
	TextAtlas_AddInt(atlas, pos.z, &cur);
	TextAtlas_Add(atlas,       14, &cur);

	s->lastX = pos.x;
	s->lastY = pos.y;
	s->lastZ = pos.z;

	s->posCount = (int)(cur - data);
}

static cc_bool HUDScreen_HasHacksChanged(struct HUDScreen* s) {
	struct HacksComp* hacks = &Entities.CurPlayer->Hacks;
	float speed = HacksComp_CalcSpeedFactor(hacks, hacks->CanSpeed);
	return speed != s->lastSpeed || Camera.Fov != s->lastFov || s->hacksChanged;
}

static void HUDScreen_RemakeLine2(struct HUDScreen* s) {
	cc_string status; char statusBuffer[STRING_SIZE * 2];
	struct HacksComp* hacks = &Entities.CurPlayer->Hacks;
	float speed;
	s->dirty = true;

	if (Game_ClassicMode) {
		TextWidget_SetConst(&s->line2, Game_Version.Name, &s->font);
		return;
	}

	speed = HacksComp_CalcSpeedFactor(hacks, hacks->CanSpeed);
	s->lastSpeed = speed; s->lastFov = Camera.Fov;
	s->hacksChanged = false;

	String_InitArray(status, statusBuffer);
	if (Camera.Fov != Camera.DefaultFov) {
		String_Format1(&status, "Zoom fov %i  ", &Camera.Fov);
	}

	if (hacks->Flying) String_AppendConst(&status, "Fly ON   ");
	if (speed)         String_Format1(&status, "Speed %f1x   ", &speed);
	if (hacks->Noclip) String_AppendConst(&status, "Noclip ON   ");

	TextWidget_Set(&s->line2, &status, &s->font);
}


static void HUDScreen_ContextLost(void* screen) {
	struct HUDScreen* s = (struct HUDScreen*)screen;
	Font_Free(&s->font);
	Screen_ContextLost(screen);

	TextAtlas_Free(&s->posAtlas);
	TextAtlas_Free(&s->countAtlas);
	Elem_Free(&s->hotbar);
	Elem_Free(&s->line1);
	Elem_Free(&s->line2);
	Elem_Free(&s->score);
	Elem_Free(&s->arrows);
}

static void HUDScreen_ContextRecreated(void* screen) {
	static const cc_string chars  = String_FromConst("0123456789-, ()");
	static const cc_string prefix = String_FromConst("Position: ");
	static const cc_string digits = String_FromConst("0123456789");
	static const cc_string empty  = String_FromConst("");

	struct HUDScreen* s = (struct HUDScreen*)screen;
	struct FontDesc countFont;
	Screen_UpdateVb(s);

	Font_Make(&s->font, 16, FONT_FLAGS_PADDING);
	Font_SetPadding(&s->font, 2);
	HotbarWidget_SetFont(&s->hotbar, &s->font);

	HUDScreen_RemakeLine1(s);
	TextAtlas_Make(&s->posAtlas, &chars, &s->font, &prefix);
	HUDScreen_RemakeLine2(s);

	/* Unpadded digit atlas for stack counts (exact glyph metrics). The font */
	/*  is only needed to rasterise the atlas, so it can be freed right after. */
	Font_Make(&countFont, 16, FONT_FLAGS_NONE);
	TextAtlas_Make(&s->countAtlas, &digits, &countFont, &empty);
	Font_Free(&countFont);

	/* Survival Score / Arrows label textures (rebuilt here since ContextLost */
	/*  freed them); their text is refreshed on change in HUDScreen_Update. */
	HUDScreen_RemakeScore(s);
	if (IndevTest_Enabled) {
		struct FontDesc font;
		Gui_MakeBodyFont(&font);
		/* The compile stamp makes every build self-identifying - "which
		    artifact am I actually running" kept coming up during testing. */
		TextWidget_SetConst(&s->indevTitle, "Minecraft Indev (" __DATE__ " " __TIME__ ")", &font);
		s->indevTitle.tex.x = 2; s->indevTitle.tex.y = 2;
		Font_Free(&font);
	}
	HUDScreen_RemakeArrows(s);
}

int HUDScreen_LayoutHotbar(void) {
	struct HUDScreen* s = &HUDScreen_Instance;
	s->hotbar.scale     = Gui_GetHotbarScale();
	Widget_Layout(&s->hotbar);
	return s->hotbar.height;
}

void HUDScreen_SetSlotPop(int slot, float time) {
	if (!Gui_HUD || slot < 0 || slot >= INVENTORY_BLOCKS_PER_HOTBAR) return;
	Gui_HUD->hotbar.slotPopTime[slot] = time;
	/* Indev GuiIngame squashes; c0.30 HUDScreen bounces (see BuildEntriesMesh) */
	Gui_HUD->hotbar.popSquash = IndevTest_Enabled;
}

static void HUDScreen_Layout(void* screen) {
	struct HUDScreen* s = (struct HUDScreen*)screen;
	struct TextWidget* line1 = &s->line1;
	struct TextWidget* line2 = &s->line2;
	int posY;

	Widget_SetLocation(line1, ANCHOR_MIN, ANCHOR_MIN, 
						2 + DisplayInfo.ContentOffsetX, 2 + DisplayInfo.ContentOffsetY);
	posY = line1->y + line1->height;
	s->posAtlas.tex.y = posY;
	Widget_SetLocation(line2, ANCHOR_MIN, ANCHOR_MIN, 
						2 + DisplayInfo.ContentOffsetX, 0);

	if (Game_ClassicMode) {
		/* Swap around so 0.30 version is at top */
		line2->yOffset = line1->yOffset;
		line1->yOffset = posY;
		Widget_Layout(line1);
	} else {
		/* We can't use y in TextWidget_Make because that DPI scales it */
		line2->yOffset = posY + s->posAtlas.tex.height;
	}

	HUDScreen_LayoutHotbar();
	Widget_Layout(line2);
}

static int HUDScreen_KeyDown(void* screen, int key, struct InputDevice* device) {
	struct HUDScreen* s = (struct HUDScreen*)screen;
	return Elem_HandlesKeyDown(&s->hotbar, key, device);
}

static void HUDScreen_InputUp(void* screen, int key, struct InputDevice* device) {
	struct HUDScreen* s = (struct HUDScreen*)screen;
	if (!InventoryScreen_IsHotbarActive()) return;
	Elem_OnInputUp(&s->hotbar, key, device);
}

static int HUDscreen_PointerDown(void* screen, int id, int x, int y) {
	struct HUDScreen* s = (struct HUDScreen*)screen;
	if (Gui_TouchUI || Gui.InputGrab) {
		return Elem_HandlesPointerDown(&s->hotbar, id, x, y);
	}
	return false;
}

static void HUDScreen_PointerUp(void *screen, int id, int x, int y) {
	struct HUDScreen* s = (struct HUDScreen*)screen;
	if (!Gui_TouchUI) return;
	Elem_OnPointerUp(&s->hotbar, id, x, y);
}

static int HUDScreen_PointerMove(void *screen, int id, int x, int y) {
	struct HUDScreen* s = (struct HUDScreen*)screen;
	if (!Gui_TouchUI) return false;
	return Elem_HandlesPointerMove(&s->hotbar, id, x, y);
}

static int HUDscreen_MouseScroll(void* screen, float delta) {
	struct HUDScreen* s = (struct HUDScreen*)screen;
	/* The default scrolling behaviour (e.g. camera, zoom) needs to be checked */
	/*   BEFORE the hotbar is scrolled, but AFTER chat (maybe) handles scrolling. */
	/* Therefore need to check the default behaviour here, hacky as that may be. */
	if (Input_HandleMouseWheel(delta)) return false;

	if (!Inventory.CanChangeSelected)  return false;
	return Elem_HandlesMouseScroll(&s->hotbar, delta);
}

static void HUDScreen_HacksChanged(void* obj) {
	((struct HUDScreen*)obj)->hacksChanged = true;
}

static void HUDScreen_NeedRedrawing(void* obj) {
	((struct HUDScreen*)obj)->dirty = true;
}

static void HUDScreen_Init(void* screen) {
	struct HUDScreen* s = (struct HUDScreen*)screen;
	s->maxVertices      = HUD_MAX_VERTICES;

	HotbarWidget_Create(&s->hotbar);
	TextWidget_Init(&s->line1);
	TextWidget_Init(&s->line2);
	TextWidget_Init(&s->score);
	TextWidget_Init(&s->indevTitle);
	TextWidget_Init(&s->arrows);

	s->line1.flags  |= WIDGET_FLAG_MAINSCREEN;
	s->line2.flags  |= WIDGET_FLAG_MAINSCREEN;

	Event_Register_(&UserEvents.HacksStateChanged, s, HUDScreen_HacksChanged);
	Event_Register_(&TextureEvents.AtlasChanged,   s, HUDScreen_NeedRedrawing);
	Event_Register_(&BlockEvents.BlockDefChanged,  s, HUDScreen_NeedRedrawing);
}

static void HUDScreen_Free(void* screen) {
	Event_Unregister_(&UserEvents.HacksStateChanged, screen, HUDScreen_HacksChanged);
	Event_Unregister_(&TextureEvents.AtlasChanged,   screen, HUDScreen_NeedRedrawing);
	Event_Unregister_(&BlockEvents.BlockDefChanged,  screen, HUDScreen_NeedRedrawing);
}

static void HUDScreen_UpdateFPS(struct HUDScreen* s, float delta) {
	s->frames++;
	s->accumulator += delta;
	if (s->accumulator < 1.0f) return;

	HUDScreen_RemakeLine1(s);
	s->accumulator    = 0.0f;
	s->frames         = 0;
	Game.ChunkUpdates = 0;
}

static void HUDScreen_Update(void* screen, float delta) {
	struct HUDScreen* s = (struct HUDScreen*)screen;
	IVec3 pos;

	HUDScreen_UpdateFPS(s,          delta);
	HotbarWidget_Update(&s->hotbar, delta);
	if (Game_ClassicMode) return;

	if (IsOnlyChatActive() && Gui.ShowFPS) {
		if (HUDScreen_HasHacksChanged(s)) HUDScreen_RemakeLine2(s);
	}

	IVec3_Floor(&pos, &Entities.CurPlayer->Base.Position);
	if (pos.x != s->lastX || pos.y != s->lastY || pos.z != s->lastZ) {
		s->dirty = true;
	}

	if (SurvivalTest_Enabled && SurvivalTest_Health != s->lastHealth) {
		s->lastHealth = SurvivalTest_Health;
		s->dirty      = true;
	}
	/* Survival Test: the heart bar jitters while at 2 hearts (4 HP) or less, */
	/*  and flashes while the invulnerability window is live - keep rebuilding */
	/*  the HUD each frame to animate both. */
	if (SurvivalTest_Enabled &&
		((SurvivalTest_Health > 0 && SurvivalTest_Health <= 4) || SurvivalTest_InvulnTicks() > 0)) {
		s->dirty = true;
	}

	if (SurvivalTest_Enabled && SurvivalTest_InvVersion() != s->lastInvVersion) {
		s->lastInvVersion = SurvivalTest_InvVersion();
		s->dirty          = true;
	}

	if (SurvivalTest_Enabled && SurvivalTest_ArrowCount() != s->lastArrows) {
		HUDScreen_RemakeArrows(s); /* updates lastArrows */
		s->dirty      = true;
	}

	if (SurvivalTest_Enabled && SurvivalTest_Score() != s->lastScore) {
		HUDScreen_RemakeScore(s);  /* updates lastScore */
		s->dirty      = true;
	}

	/* Air bubbles deplete continuously while the head is underwater, so keep */
	/*  rebuilding the HUD to animate them (like the low-health heart shake). */
	if (SurvivalTest_Enabled && SurvivalTest_HeadUnderwater()) {
		s->dirty = true;
	}

	/* Rebuild each frame while any slot pop animation is running so the */
	/*  animated slot (which HotbarWidget_Update just decremented) is redrawn */
	if (SurvivalTest_Enabled) {
		int i;
		for (i = 0; i < INVENTORY_BLOCKS_PER_HOTBAR; i++) {
			if (s->hotbar.slotPopTime[i] > 0.0f) { s->dirty = true; break; }
		}
	}
}

#define CH_EXTENT 16
static void HUDScreen_BuildCrosshairsMesh(struct VertexTextured** ptr) {
	/* Only top quarter of icons.png is used */
	static struct Texture tex = { 0, Tex_Rect(0,0,0,0), Tex_UV(0.0f,0.0f, 15/256.0f,15/64.0f) };
	int extent;

	if (SurvivalTest_Enabled) {
		/* GuiIngame/HUDScreen: a 16x16 icon anchored at (w/2-7, h/2-7) -
		    one GUI px up-left of exact centring - sampling the full
		    (0,0)-(16,16) sprite */
		float scale = Gui_GetCrosshairScale();
		tex.x = (Window_Main.Width  / 2) - (int)(7 * scale * 2);
		tex.y = (Window_Main.Height / 2) - (int)(7 * scale * 2);
		tex.width  = (int)(16 * scale * 2);
		tex.height = (int)(16 * scale * 2);
		tex.uv.u2 = 16/256.0f; tex.uv.v2 = 16/64.0f;
		Gfx_Make2DQuad(&tex, PACKEDCOL_WHITE, ptr);
		tex.uv.u2 = 15/256.0f; tex.uv.v2 = 15/64.0f;
		return;
	}

	extent = (int)(CH_EXTENT * Gui_GetCrosshairScale());
	tex.x  = (Window_Main.Width  / 2) - extent;
	tex.y  = (Window_Main.Height / 2) - extent;

	tex.width  = extent * 2;
	tex.height = extent * 2;
	Gfx_Make2DQuad(&tex, PACKEDCOL_WHITE, ptr);
}

/* UV coordinates for hearts in icons.png (256 wide, top 64 pixels used) */
/* Empty heart background: 9x9 at pixel (16,0) */
#define HEART_BG_U1  (16/256.0f)
#define HEART_BG_U2  (25/256.0f)
/* Full heart: 9x9 at pixel (52,0) */
#define HEART_FULL_U1  (52/256.0f)
#define HEART_FULL_U2  (61/256.0f)
/* Half heart: 9x9 at pixel (61,0) */
#define HEART_HALF_U1  (61/256.0f)
#define HEART_HALF_U2  (70/256.0f)
#define HEART_V1  (0/64.0f)
#define HEART_V2  (9/64.0f)
/* White-flash heart background: 9x9 at pixel (25,0) */
#define HEART_FLASH_BG_U1 (25/256.0f)
#define HEART_FLASH_BG_U2 (34/256.0f)
/* Armor icons (GuiIngame): empty outline (16,9), half (25,9), full (34,9) */
#define ARMOR_EMPTY_U1 (16/256.0f)
#define ARMOR_EMPTY_U2 (25/256.0f)
#define ARMOR_HALF_U1  (25/256.0f)
#define ARMOR_HALF_U2  (34/256.0f)
#define ARMOR_FULL_U1  (34/256.0f)
#define ARMOR_FULL_U2  (43/256.0f)
#define ARMOR_V1 ( 9/64.0f)
#define ARMOR_V2 (18/64.0f)
/* Ghost (lastHealth) hearts drawn while the invuln window flashes: (70,0)/(79,0) */
#define HEART_GHOST_FULL_U1 (70/256.0f)
#define HEART_GHOST_FULL_U2 (79/256.0f)
#define HEART_GHOST_HALF_U1 (79/256.0f)
#define HEART_GHOST_HALF_U2 (88/256.0f)

static int HUDScreen_BuildHeartsMesh(struct HUDScreen* s, struct VertexTextured* dst) {
	struct Texture tex;
	struct VertexTextured* cur = dst;
	int fullHearts, i, x, y, heartSize, step, invuln;
	int jitter[10];
	RNGState jitterRng;
	cc_bool hasHalf, glow, shaking;
	float scale;

	if (!SurvivalTest_Enabled) return 0;

	/* Match the hotbar's TRUE on-screen scale (Gui_GetHotbarScale bakes out */
	/*  DPI, then HotbarWidget_Reposition multiplies it back in via */
	/*  DisplayInfo.ScaleY). Using the bare hotbar scale here left the hearts */
	/*  smaller than the hotbar on HiDPI/fullscreen; folding ScaleY back in */
	/*  keeps the whole survival HUD scaling as one unit. No-op when ScaleY==1. */
	scale     = Gui_GetHotbarScale() * DisplayInfo.ScaleY;
	heartSize = (int)(9.0f * scale);
	step      = (int)(8.0f * scale); /* genuine packs icons 8 units apart (9px sprites overlap 1px) */

	/* HUDScreen.render: glow = invulnerableTime / 3 % 2 == 1 (and only during */
	/*  the fresher half of the window) - background flashes white and the */
	/*  pre-hit "lastHealth" ghost hearts are drawn over it. */
	invuln = SurvivalTest_InvulnTicks();
	glow   = invuln >= 10 && (invuln / 3) % 2 == 1;

	/* Survival Test draws the heart row flush with the hotbar's left edge */
	/*  (not centred), so the bar grows rightwards from the first slot. */
	x = s->hotbar.x;
	y = s->hotbar.y - heartSize - (int)(1.0f * scale);

	/* HUDScreen seeds its jitter RNG with ticks * 312871, so the offsets are */
	/*  stable within a tick but reroll each tick - and each heart jitters */
	/*  INDEPENDENTLY (only at 2 hearts / 4 HP or less). */
	Random_Seed(&jitterRng, (int)(Game.Time * 20.0) * 312871);
	shaking = SurvivalTest_Health <= 4; /* genuine shakes at 0 too */

	tex.ID = Gui.IconsTex;

	/* Draw 10 heart backgrounds (white-flash variant while glowing) */
	if (glow) { Tex_SetUV(tex, HEART_FLASH_BG_U1, HEART_V1, HEART_FLASH_BG_U2, HEART_V2); }
	else      { Tex_SetUV(tex, HEART_BG_U1,       HEART_V1, HEART_BG_U2,       HEART_V2); }
	for (i = 0; i < 10; i++) {
		jitter[i] = shaking ? Random_Next(&jitterRng, 2) : 0;
		Tex_SetRect(tex, x + i * step, y + jitter[i], heartSize, heartSize);
		Gfx_Make2DQuad(&tex, PACKEDCOL_WHITE, &cur);
	}

	/* While glowing, the health from before the hit is drawn as ghost hearts */
	if (glow) {
		int last = SurvivalTest_LastHealth();
		fullHearts = last / 2;
		Tex_SetUV(tex, HEART_GHOST_FULL_U1, HEART_V1, HEART_GHOST_FULL_U2, HEART_V2);
		for (i = 0; i < fullHearts; i++) {
			Tex_SetRect(tex, x + i * step, y + jitter[i], heartSize, heartSize);
			Gfx_Make2DQuad(&tex, PACKEDCOL_WHITE, &cur);
		}
		if (last & 1) {
			Tex_SetUV(tex, HEART_GHOST_HALF_U1, HEART_V1, HEART_GHOST_HALF_U2, HEART_V2);
			Tex_SetRect(tex, x + fullHearts * step, y + jitter[fullHearts], heartSize, heartSize);
			Gfx_Make2DQuad(&tex, PACKEDCOL_WHITE, &cur);
		}
	}

	/* Draw filled hearts according to current health (2 HP per heart) */
	fullHearts = SurvivalTest_Health / 2;
	hasHalf    = (SurvivalTest_Health & 1) != 0;

	Tex_SetUV(tex, HEART_FULL_U1, HEART_V1, HEART_FULL_U2, HEART_V2);
	for (i = 0; i < fullHearts; i++) {
		Tex_SetRect(tex, x + i * step, y + jitter[i], heartSize, heartSize);
		Gfx_Make2DQuad(&tex, PACKEDCOL_WHITE, &cur);
	}

	if (hasHalf) {
		Tex_SetUV(tex, HEART_HALF_U1, HEART_V1, HEART_HALF_U2, HEART_V2);
		Tex_SetRect(tex, x + fullHearts * step, y + jitter[fullHearts], heartSize, heartSize);
		Gfx_Make2DQuad(&tex, PACKEDCOL_WHITE, &cur);
	}

	/* GuiIngame's armor bar (Indev): 10 icons on the hearts row, flush with
	    the hotbar's RIGHT edge and filling right-to-left - icon i covers
	    protection points 2i+1/2i+2, full below the wear-weighted
	    getPlayerArmorValue, half at it, empty outline above it. The whole
	    row only appears while some armor is worn (value > 0), and never
	    shakes with the low-health hearts (genuine adds the jitter after
	    the armor draw). */
	if (IndevTest_Enabled) {
		int armor = SurvivalTest_PlayerArmorValue();
		int ax    = s->hotbar.x + s->hotbar.width - heartSize;
		for (i = 0; i < 10 && armor > 0; i++) {
			int threshold = (i << 1) + 1;
			if (threshold < armor) {
				Tex_SetUV(tex, ARMOR_FULL_U1,  ARMOR_V1, ARMOR_FULL_U2,  ARMOR_V2);
			} else if (threshold == armor) {
				Tex_SetUV(tex, ARMOR_HALF_U1,  ARMOR_V1, ARMOR_HALF_U2,  ARMOR_V2);
			} else {
				Tex_SetUV(tex, ARMOR_EMPTY_U1, ARMOR_V1, ARMOR_EMPTY_U2, ARMOR_V2);
			}
			Tex_SetRect(tex, ax - i * step, y, heartSize, heartSize);
			Gfx_Make2DQuad(&tex, PACKEDCOL_WHITE, &cur);
		}
	}

	return (int)(cur - dst);
}

/* Builds the stack-count digits drawn over each hotbar slot. Survival Test */
/*  (HUDScreen.java) draws these with the 8px-tall GUI font in its 240-unit- */
/*  tall virtual screen space, where the hotbar is 22 units tall and each slot */
/*  cell is 20 units wide. Our HotbarWidget bakes that exact scale into its */
/*  pixel geometry (w->height = 22 * hotbarScale * ScaleY, slotWidth = 20 * */
/*  hotbarScale * ScaleX), so a faithful digit is (8/22) of the hotbar height, */
/*  right-aligned to each slot cell's right edge. Counts of 1 are left implicit. */
static int HUDScreen_BuildCountsMesh(struct HUDScreen* s, struct VertexTextured* dst) {
	struct TextAtlas* atlas = &s->countAtlas;
	struct HotbarWidget* w  = &s->hotbar;
	struct VertexTextured* cur = dst;
	struct Texture part;
	char digits[STRING_INT_CHARS];
	int i, j, count, nDigits, d;
	float f, digitH, totalW, penX, slotRight, top, bottom;

	if (!SurvivalTest_Enabled) return 0;
	if (!atlas->tex.ID)        return 0; /* digit atlas not created yet */
	if (!atlas->tex.height)    return 0;

	/* 8px-tall font glyphs in the original's 22-unit-tall hotbar space. */
	digitH = w->height * (8.0f / 22.0f);
	f      = digitH / atlas->tex.height;

	/* Original font top y = slotY + 6, with slotY = screenBottom - 16, i.e. */
	/*  10 virtual units above the hotbar's bottom edge (so the digit's bottom */
	/*  sits 2 units above it). */
	bottom = (float)(w->y + w->height);
	top    = bottom - w->height * (10.0f / 22.0f);

	part.ID     = atlas->tex.ID;
	part.uv.v1  = atlas->tex.uv.v1;
	part.uv.v2  = atlas->tex.uv.v2;
	part.height = (cc_uint16)digitH;
	part.y      = (short)top;

	for (i = 0; i < SURVIVAL_HOTBAR_SLOTS; i++) {
		count = SurvivalTest_HotbarCount(i);
		if (count <= 1) continue;

		nDigits = String_MakeUInt32((cc_uint32)count, digits);
		totalW  = 0.0f;
		for (j = 0; j < nDigits; j++) totalW += atlas->widths[digits[j] - '0'] * f;

		/* Right-align to slot i's cell right edge: slotWidth*(i+1) from the */
		/*  hotbar's left edge, matching var26 + 19 in HUDScreen.java (no inset). */
		slotRight = w->x + w->slotWidth * (i + 1);
		penX      = slotRight - totalW;

		/* String_MakeUInt32 writes least-significant first, so emit reversed */
		for (j = nDigits - 1; j >= 0; j--) {
			d           = digits[j] - '0';
			part.x      = (short)penX;
			part.width  = (cc_uint16)(atlas->widths[d] * f);
			part.uv.u1  = atlas->offsets[d] * atlas->uScale;
			part.uv.u2  = part.uv.u1 + atlas->widths[d] * atlas->uScale;
			Gfx_Make2DQuad(&part, PACKEDCOL_WHITE, &cur);
			penX += part.width;
		}
	}
	return (int)(cur - dst);
}

/* UV coordinates for the air bubbles in icons.png (HUDScreen.java): a full */
/*  bubble at pixel (16,18) and a bursting one at (25,18), both 9x9. */
#define BUBBLE_FULL_U1 (16/256.0f)
#define BUBBLE_FULL_U2 (25/256.0f)
#define BUBBLE_POP_U1  (25/256.0f)
#define BUBBLE_POP_U2  (34/256.0f)
#define BUBBLE_V1      (18/64.0f)
#define BUBBLE_V2      (27/64.0f)

/* Builds the depleting air bubble row shown above the hearts while the head */
/*  is underwater (HUDScreen.java's isUnderWater() block). The full/bursting */
/*  split is the original's: full = ceil((air-2)*10/300), and one extra */
/*  bursting bubble appears as the current one drains, total = ceil(air*10/300). */
static int HUDScreen_BuildBubblesMesh(struct HUDScreen* s, struct VertexTextured* dst) {
	struct Texture tex;
	struct VertexTextured* cur = dst;
	int air, full, total, i, x, y, size;
	float scale;

	if (!SurvivalTest_Enabled)         return 0;
	if (!SurvivalTest_HeadUnderwater()) return 0;
	if (!Gui.IconsTex)                 return 0;

	scale = Gui_GetHotbarScale() * DisplayInfo.ScaleY;
	size  = (int)(9.0f * scale);

	air   = SurvivalTest_AirSupply();
	full  = Math_Ceil((air - 2) * 10.0f / 300.0f);
	total = Math_Ceil( air      * 10.0f / 300.0f);
	if (full  < 0)  full  = 0;
	if (total > 10) total = 10;

	/* Sit one bubble-height above the heart row (which is itself above the */
	/*  hotbar), matching the original's height-32-9 vs hearts at height-32. */
	x = s->hotbar.x;
	y = s->hotbar.y - size - (int)(2.0f * scale) - size;

	tex.ID = Gui.IconsTex;
	for (i = 0; i < total; i++) {
		if (i < full) {
			Tex_SetUV(tex, BUBBLE_FULL_U1, BUBBLE_V1, BUBBLE_FULL_U2, BUBBLE_V2);
		} else {
			Tex_SetUV(tex, BUBBLE_POP_U1,  BUBBLE_V1, BUBBLE_POP_U2,  BUBBLE_V2);
		}
		/* Genuine packs icons 8 units apart (9px sprites overlap 1px) */
		Tex_SetRect(tex, x + i * (int)(8.0f * scale), y, size, size);
		Gfx_Make2DQuad(&tex, PACKEDCOL_WHITE, &cur);
	}
	return (int)(cur - dst);
}

/* HUDScreen.java's SurvivalGameMode labels, rasterised on change like the FPS */
/*  line: "Score: &eN" (yellow number) and "Arrows: N". Their on-screen */
/*  positions are pinned per-frame in HUDScreen_BuildMesh. */
static void HUDScreen_RemakeScore(struct HUDScreen* s) {
	cc_string str; char buf[STRING_SIZE];
	int score = SurvivalTest_Score();
	String_InitArray(str, buf);
	String_Format1(&str, "Score: &e%i", &score);
	TextWidget_Set(&s->score, &str, &s->font);
	s->lastScore = score;
}

static void HUDScreen_RemakeArrows(struct HUDScreen* s) {
	cc_string str; char buf[STRING_SIZE];
	int arrows = SurvivalTest_ArrowCount();
	String_InitArray(str, buf);
	String_Format1(&str, "Arrows: %i", &arrows);
	TextWidget_Set(&s->arrows, &str, &s->font);
	s->lastArrows = arrows;
}

static void HUDScreen_BuildMesh(void* screen) {
	struct HUDScreen* s = (struct HUDScreen*)screen;
	struct VertexTextured* base;
	struct VertexTextured* data;
	struct VertexTextured** ptr;
	struct VertexTextured* p;
	float scale;
	int heartSize, rowY;

	base = Screen_LockVb(s);
	data = base;
	ptr  = &data;

	HUDScreen_BuildCrosshairsMesh(ptr);
	Widget_BuildMesh(&s->line1,  ptr);
	Widget_BuildMesh(&s->line2,  ptr);
	Widget_BuildMesh(&s->hotbar, ptr);

	if (!Game_ClassicMode)
		HUDScreen_BuildPosition(s, base + HUD_OFS_POSITION);

	s->heartCount    = HUDScreen_BuildHeartsMesh (s, base + HUD_OFS_HEARTS);
	s->countVertices = HUDScreen_BuildCountsMesh (s, base + HUD_OFS_COUNTS);
	s->bubbleCount   = HUDScreen_BuildBubblesMesh(s, base + HUD_OFS_BUBBLES);

	/* Survival Score / Arrows labels. The text textures are rasterised once (on */
	/*  change) at a fixed font size, but the hotbar/hearts scale with the GUI */
	/*  scale, so at a large scale the labels looked tiny next to them. Stretch */
	/*  each label's quad to the SAME on-screen height the original HUDScreen */
	/*  draws its 8px font at - (8/22) of the hotbar height, exactly like the */
	/*  stack-count digits - so they track the hotbar at any scale/DPI. Built as */
	/*  a scaled copy of the widget's texture rather than mutating the widget */
	/*  (which persists across frames and would compound the scaling). */
	/* Genuine in-20100223's GuiIngame draws NEITHER label: no score HUD and
	    arrows are ordinary inventory items - both are c0.30-only. */
	if (SurvivalTest_Enabled && !IndevTest_Enabled) {
		struct Texture lbl;
		float labelH = s->hotbar.height * (8.0f / 22.0f);
		scale     = Gui_GetHotbarScale() * DisplayInfo.ScaleY;
		heartSize = (int)(9.0f * scale);
		rowY      = s->hotbar.y - heartSize - (int)(2.0f * scale);

		/* Score: top-right corner */
		lbl = s->score.tex;
		if (lbl.height) {
			lbl.width  = (cc_uint16)(lbl.width * labelH / lbl.height);
			lbl.height = (cc_uint16)labelH;
		}
		lbl.x = Window_Main.Width - lbl.width - (int)(2.0f * scale);
		lbl.y = (int)(2.0f * scale);
		p = base + HUD_OFS_SCORE;
		Gfx_Make2DQuad(&lbl, s->score.color, &p);

		/* Arrows: beside the heart row, vertically centred on it */
		lbl = s->arrows.tex;
		if (lbl.height) {
			lbl.width  = (cc_uint16)(lbl.width * labelH / lbl.height);
			lbl.height = (cc_uint16)labelH;
		}
		lbl.x = s->hotbar.x + s->hotbar.width / 2 + (int)(8.0f * scale);
		lbl.y = rowY + (heartSize - (int)labelH) / 2;
		p = base + HUD_OFS_ARROWS;
		Gfx_Make2DQuad(&lbl, s->arrows.color, &p);
	}
	Gfx_UnlockDynamicVb(s->vb);
}

static void HUDScreen_Render(void* screen, float delta) {
	struct HUDScreen* s = (struct HUDScreen*)screen;
	if (Game_HideGui) return;

	Gfx_3DS_SetRenderScreen(TOP_SCREEN);

	Gfx_SetVertexFormat(VERTEX_FORMAT_TEXTURED);
	Gfx_BindDynamicVb(s->vb);
	if (Gui.ShowFPS) Widget_Render2(&s->line1, 4);

	if (Game_ClassicMode) {
		Widget_Render2(&s->line2, 8);
	} else if (IsOnlyChatActive() && Gui.ShowFPS) {
		Widget_Render2(&s->line2, 8);
		Gfx_BindTexture(s->posAtlas.tex.ID);
		Gfx_DrawVb_IndexedTris_Range(s->posCount, HUD_OFS_POSITION, DRAW_HINT_RECT);
		/* TODO swap these two lines back */
	}

	if (!Gui_GetBlocksWorld()) {
		Gfx_BindDynamicVb(s->vb);
		if (!Gui.HideHotbar) Widget_Render2(&s->hotbar, 12);

		if (!Gui.HideCrosshair && Gui.IconsTex && !tablist_active) {
			Gfx_BindTexture(Gui.IconsTex);
			Gfx_BindDynamicVb(s->vb); /* Have to rebind for mobile right now... */
			/* GuiIngame draws the Indev crosshair with the colour-inverting
			    glBlendFunc(ONE_MINUS_DST_COLOR, ONE_MINUS_SRC_COLOR) */
			if (IndevTest_Enabled) Gfx_SetInvertedBlending(true);
			Gfx_DrawVb_IndexedTris_Range(4, 0, DRAW_HINT_SPRITE);
			if (IndevTest_Enabled) Gfx_SetInvertedBlending(false);
		}

		/* Indev: item sprites over hotbar slots holding item ids (256+). */
		/*  Drawn as immediate textures - slots are engine BLOCK_AIR there, so */
		/*  nothing else occupies the cell. Bails without an items.png. */
		if (IndevTest_Enabled && IndevTest_ItemsTex()) {
			struct Texture itex;
			float slotW = s->hotbar.width / (float)INVENTORY_BLOCKS_PER_HOTBAR;
			/* GuiIngame: 16x16 GUI-px icons at (cell + 2, height - 19) -
			    dead centre of the 20px cell (the old +3 shifted every
			    sprite one GUI px right - most visible on compact sprites
			    like coal/diamond, user report) */
			int size = (int)(slotW * (16.0f / 20.0f)), k;

			for (k = 0; k < SURVIVAL_HOTBAR_SLOTS; k++) {
				int id = SurvivalTest_SlotId(k);
				if (id < 256) continue;
				if (!IndevTest_ItemSpriteUV(id, &itex.uv.u1, &itex.uv.v1, &itex.uv.u2, &itex.uv.v2)) continue;

				int maxDmg, dmg;
				itex.ID     = IndevTest_ItemsTex();
				itex.x      = (short)(s->hotbar.x + k * slotW + slotW * (2.0f / 20.0f));
				itex.y      = (short)(s->hotbar.y + s->hotbar.height * (3.0f / 22.0f));
				itex.width  = (cc_uint16)size;
				itex.height = (cc_uint16)size;
				Texture_Render(&itex);

				/* RenderItem.renderItemOverlayIntoGUI's durability bar: at */
				/*  (x+2, y+13) in 16px icon space, a 13x2 black backing, a */
				/*  12x1 dark track, then (13 - dmg*13/max) x1 of the red-> */
				/*  green gradient colour (255-v)<<16 | v<<8, v=255-dmg*255/max. */
				maxDmg = IndevTest_ToolMaxDamage(SurvivalTest_SlotId(k));
				dmg    = SurvivalTest_SlotDamage(k);
				if (maxDmg > 0 && dmg > 0) {
					float u  = size / 16.0f;
					int   bx = itex.x + (int)(2 * u), by = itex.y + (int)(13 * u);
					int   v  = 255 - dmg * 255 / maxDmg;
					int   w  = 13 - dmg * 13 / maxDmg;
					int   h  = (int)u; if (h < 1) h = 1;

					Gfx_Draw2DFlat(bx, by, (int)(13 * u), h * 2, PackedCol_Make(0, 0, 0, 255));
					Gfx_Draw2DFlat(bx, by, (int)(12 * u), h,
						PackedCol_Make((cc_uint8)((255 - v) / 4), 63, 0, 255));
					Gfx_Draw2DFlat(bx, by, (int)(w * u), h,
						PackedCol_Make((cc_uint8)(255 - v), (cc_uint8)v, 0, 255));
				}
			}
			/* Texture_Render/Gfx_Draw2DFlat switch vertex format + VB - the */
			/*  rest of the HUD (hearts/counts/bubbles) draws from s->vb in */
			/*  TEXTURED format, so restore BOTH or those meshes corrupt. */
			Gfx_SetVertexFormat(VERTEX_FORMAT_TEXTURED);
			Gfx_BindDynamicVb(s->vb);
		}

		/* "Minecraft Indev" top-left, only while the F3/FPS line is hidden */
		if (IndevTest_Enabled && !Gui.ShowFPS && s->indevTitle.tex.ID) {
			Texture_Render(&s->indevTitle.tex);
			Gfx_SetVertexFormat(VERTEX_FORMAT_TEXTURED);
			Gfx_BindDynamicVb(s->vb);
		}

		/* Draw survival health hearts above the hotbar */
		if (SurvivalTest_Enabled && s->heartCount > 0 && Gui.IconsTex) {
			Gfx_BindTexture(Gui.IconsTex);
			Gfx_BindDynamicVb(s->vb);
			Gfx_DrawVb_IndexedTris_Range(s->heartCount, HUD_OFS_HEARTS, DRAW_HINT_SPRITE);
		}

		/* Draw survival hotbar stack counts (digit atlas) */
		if (SurvivalTest_Enabled && s->countVertices > 0 && s->countAtlas.tex.ID) {
			Gfx_BindTexture(s->countAtlas.tex.ID);
			Gfx_BindDynamicVb(s->vb);
			Gfx_DrawVb_IndexedTris_Range(s->countVertices, HUD_OFS_COUNTS, DRAW_HINT_RECT);
		}

		/* Draw survival air bubbles (icons.png) above the hearts when underwater */
		if (SurvivalTest_Enabled && s->bubbleCount > 0 && Gui.IconsTex) {
			Gfx_BindTexture(Gui.IconsTex);
			Gfx_BindDynamicVb(s->vb);
			Gfx_DrawVb_IndexedTris_Range(s->bubbleCount, HUD_OFS_BUBBLES, DRAW_HINT_SPRITE);
		}

		/* Draw survival Score / Arrows labels (each binds its own text texture) */
		/* c0.30 only - genuine Indev's GuiIngame has neither */
		if (SurvivalTest_Enabled && !IndevTest_Enabled && s->score.tex.ID) {
			Gfx_BindDynamicVb(s->vb);
			Widget_Render2(&s->score, HUD_OFS_SCORE);
		}
		if (SurvivalTest_Enabled && !IndevTest_Enabled && s->arrows.tex.ID) {
			Gfx_BindDynamicVb(s->vb);
			Widget_Render2(&s->arrows, HUD_OFS_ARROWS);
		}
	}

	Gfx_3DS_SetRenderScreen(BOTTOM_SCREEN);
}

static const struct ScreenVTABLE HUDScreen_VTABLE = {
	HUDScreen_Init,        HUDScreen_Update,    HUDScreen_Free,
	HUDScreen_Render,      HUDScreen_BuildMesh,
	HUDScreen_KeyDown,     HUDScreen_InputUp,   Screen_FKeyPress, Screen_FText,
	HUDscreen_PointerDown, HUDScreen_PointerUp, HUDScreen_PointerMove,  HUDscreen_MouseScroll,
	HUDScreen_Layout,      HUDScreen_ContextLost, HUDScreen_ContextRecreated
};
void HUDScreen_Show(void) {
	struct HUDScreen* s = &HUDScreen_Instance;
	s->VTABLE = &HUDScreen_VTABLE;
	Gui_HUD   = s;
	Gui_Add((struct Screen*)s, GUI_PRIORITY_HUD);
}


/*########################################################################################################################*
*----------------------------------------------------TabListOverlay-----------------------------------------------------*
*#########################################################################################################################*/
#ifdef CC_BUILD_NETWORKING

#define GROUP_NAME_ID UInt16_MaxValue
#define LIST_COLUMN_PADDING 5
#define LIST_NAMES_PER_COLUMN 16
#define TABLIST_MAX_ENTRIES (TABLIST_MAX_NAMES * 2)
typedef int (*TabListEntryCompare)(int x, int y);

static struct TabListOverlay {
	Screen_Body
	int x, y, width, height;
	cc_bool classic, staysOpen;
	int usedCount, elementOffset;
	struct TextWidget title;
	struct FontDesc font;
	TabListEntryCompare compare;
	cc_uint16 ids[TABLIST_MAX_ENTRIES];
	struct Texture textures[TABLIST_MAX_ENTRIES];
} TabListOverlay_Instance CC_BIG_VAR;
#define TABLIST_MAX_VERTICES (TEXTWIDGET_MAX + 4 * TABLIST_MAX_ENTRIES)

static void TabListOverlay_DrawText(struct Texture* tex, struct TabListOverlay* s, const cc_string* name) {
	cc_string tmp; char tmpBuffer[STRING_SIZE];
	struct DrawTextArgs args;

	if (Game_PureClassic) {
		String_InitArray(tmp, tmpBuffer);
		String_AppendColorless(&tmp, name);
	} else {
		tmp = *name;
	}

	DrawTextArgs_Make(&args, &tmp, &s->font, !s->classic);
	Drawer2D_MakeTextTexture(tex, &args);
}

static int TabListOverlay_GetColumnWidth(struct TabListOverlay* s, int column) {
	int i   = column * LIST_NAMES_PER_COLUMN;
	int end = min(s->usedCount, i + LIST_NAMES_PER_COLUMN);
	int maxWidth = 0;

	for (; i < end; i++) 
	{
		maxWidth = max(maxWidth, s->textures[i].width);
	}
	return maxWidth + LIST_COLUMN_PADDING + s->elementOffset;
}

static int TabListOverlay_GetColumnHeight(struct TabListOverlay* s, int column) {
	int i   = column * LIST_NAMES_PER_COLUMN;
	int end = min(s->usedCount, i + LIST_NAMES_PER_COLUMN);
	int height = 0;

	for (; i < end; i++) 
	{
		height += s->textures[i].height + 1;
	}
	return height;
}

static void TabListOverlay_SetColumnPos(struct TabListOverlay* s, int column, int x, int y) {
	struct Texture tex;
	int i   = column * LIST_NAMES_PER_COLUMN;
	int end = min(s->usedCount, i + LIST_NAMES_PER_COLUMN);

	for (; i < end; i++) 
	{
		tex = s->textures[i];
		tex.x = x; tex.y = y - 10;

		y += tex.height + 1;
		/* offset player names a bit, compared to group name */
		if (!s->classic && s->ids[i] != GROUP_NAME_ID) {
			tex.x += s->elementOffset;
		}
		s->textures[i] = tex;
	}
}

static void TabListOverlay_Layout(void* screen) {
	struct TabListOverlay* s = (struct TabListOverlay*)screen;
	int minWidth, minHeight, paddingX, paddingY;
	int i, x, y, width = 0, height = 0;
	int columns = Math_CeilDiv(s->usedCount, LIST_NAMES_PER_COLUMN);

	for (i = 0; i < columns; i++) 
	{
		width += TabListOverlay_GetColumnWidth(s,  i);
		y      = TabListOverlay_GetColumnHeight(s, i);
		height = max(height, y);
	}

	minWidth = Display_ScaleX(480);
	width    = max(width, minWidth);
	paddingX = Display_ScaleX(10);
	paddingY = Display_ScaleY(10);

	width  += paddingX * 2;
	height += paddingY * 2;

	y    = Window_UI.Height / 4 - height / 2;
	s->x = Gui_CalcPos(ANCHOR_CENTRE,          0, width , Window_UI.Width );
	s->y = Gui_CalcPos(ANCHOR_CENTRE, -max(0, y), height, Window_UI.Height);

	x = s->x + paddingX;
	y = s->y + paddingY;

	for (i = 0; i < columns; i++) 
	{
		TabListOverlay_SetColumnPos(s, i, x, y);
		x += TabListOverlay_GetColumnWidth(s, i);
	}

	s->y -= (s->title.height + paddingY);
	s->width  = width;
	minHeight = Display_ScaleY(300);
	s->height = max(minHeight, height + s->title.height);

	s->title.horAnchor = ANCHOR_CENTRE;
	s->title.yOffset   = s->y + paddingY / 2;
	Widget_Layout(&s->title);
}

static void TabListOverlay_AddName(struct TabListOverlay* s, EntityID id, int index) {
	cc_string name;
	/* insert at end of list */
	if (index == -1) { index = s->usedCount; s->usedCount++; }

	name = TabList_UNSAFE_GetList(id);
	s->ids[index] = id;
	TabListOverlay_DrawText(&s->textures[index], s, &name);
}

static void TabListOverlay_DeleteAt(struct TabListOverlay* s, int i) {
	Gfx_DeleteTexture(&s->textures[i].ID);

	for (; i < s->usedCount - 1; i++)
	{
		s->ids[i]      = s->ids[i + 1];
		s->textures[i] = s->textures[i + 1];
	}

	s->usedCount--;
	s->ids[s->usedCount]         = 0;
	s->textures[s->usedCount].ID = 0;
}

static void TabListOverlay_AddGroup(struct TabListOverlay* s, int id, int* index) {
	cc_string group;
	int i;
	group = TabList_UNSAFE_GetGroup(id);

	for (i = Array_Elems(s->ids) - 1; i > (*index); i--) 
	{
		s->ids[i]      = s->ids[i - 1];
		s->textures[i] = s->textures[i - 1];
	}
	
	s->ids[*index] = GROUP_NAME_ID;
	s->textures[*index].ID = 0; /* TODO: TEMP HACK! */
	TabListOverlay_DrawText(&s->textures[*index], s, &group);

	(*index)++;
	s->usedCount++;
}

static int TabListOverlay_GetGroupCount(struct TabListOverlay* s, int id, int i) {
	cc_string group, curGroup;
	int count;
	group = TabList_UNSAFE_GetGroup(id);

	for (count = 0; i < s->usedCount; i++, count++)
	{
		curGroup = TabList_UNSAFE_GetGroup(s->ids[i]);
		if (!String_CaselessEquals(&group, &curGroup)) break;
	}
	return count;
}

static int TabListOverlay_PlayerCompare(int x, int y) {
	cc_string xName; char xNameBuffer[STRING_SIZE];
	cc_string yName; char yNameBuffer[STRING_SIZE];
	cc_uint8 xRank, yRank;
	cc_string xNameRaw, yNameRaw;

	xRank = TabList.GroupRanks[x];
	yRank = TabList.GroupRanks[y];
	if (xRank != yRank) return (xRank < yRank ? -1 : 1);
	
	String_InitArray(xName, xNameBuffer);
	xNameRaw = TabList_UNSAFE_GetList(x);
	String_AppendColorless(&xName, &xNameRaw);

	String_InitArray(yName, yNameBuffer);
	yNameRaw = TabList_UNSAFE_GetList(y);
	String_AppendColorless(&yName, &yNameRaw);

	return String_Compare(&xName, &yName);
}

static int TabListOverlay_GroupCompare(int x, int y) {
	cc_string xGroup, yGroup;
	/* TODO: should we use colourless comparison? ClassicalSharp sorts groups with colours */
	xGroup = TabList_UNSAFE_GetGroup(x);
	yGroup = TabList_UNSAFE_GetGroup(y);
	return String_Compare(&xGroup, &yGroup);
}

static void TabListOverlay_QuickSort(int left, int right) {
	struct Texture* values = TabListOverlay_Instance.textures; struct Texture value;
	cc_uint16* keys        = TabListOverlay_Instance.ids; cc_uint16 key;
	TabListEntryCompare compareEntries = TabListOverlay_Instance.compare;

	while (left < right) {
		int i = left, j = right;
		int pivot = keys[(i + j) / 2];

		/* partition the list */
		while (i <= j) {
			while (compareEntries(pivot, keys[i]) > 0) i++;
			while (compareEntries(pivot, keys[j]) < 0) j--;
			QuickSort_Swap_KV_Maybe();
		}
		/* recurse into the smaller subset */
		QuickSort_Recurse(TabListOverlay_QuickSort)
	}
}

static void TabListOverlay_SortEntries(struct TabListOverlay* s) {
	int i, id, count;
	if (!s->usedCount) return;

	if (s->classic) {
		TabListOverlay_Instance.compare = TabListOverlay_PlayerCompare;
		TabListOverlay_QuickSort(0, s->usedCount - 1);
		return;
	}

	/* Sort the list by group */
	/* Loop backwards, since DeleteAt() reduces NamesCount */
	for (i = s->usedCount - 1; i >= 0; i--)
	{
		if (s->ids[i] != GROUP_NAME_ID) continue;
		TabListOverlay_DeleteAt(s, i);
	}
	TabListOverlay_Instance.compare = TabListOverlay_GroupCompare;
	TabListOverlay_QuickSort(0, s->usedCount - 1);

	/* Sort the entries in each group */
	TabListOverlay_Instance.compare = TabListOverlay_PlayerCompare;
	for (i = 0; i < s->usedCount; )
	{
		id = s->ids[i];
		TabListOverlay_AddGroup(s, id, &i);

		count = TabListOverlay_GetGroupCount(s, id, i);
		TabListOverlay_QuickSort(i, i + (count - 1));
		i += count;
	}
}

static void TabListOverlay_SortAndLayout(struct TabListOverlay* s) {
	TabListOverlay_SortEntries(s);
	TabListOverlay_Layout(s);
	s->dirty = true;
}

static void TabListOverlay_Add(void* obj, int id) {
	struct TabListOverlay* s = (struct TabListOverlay*)obj;
	TabListOverlay_AddName(s, id, -1);
	TabListOverlay_SortAndLayout(s);
}

static void TabListOverlay_Update(void* obj, int id) {
	struct TabListOverlay* s = (struct TabListOverlay*)obj;
	int i;
	for (i = 0; i < s->usedCount; i++)
	{
		if (s->ids[i] != id) continue;
		Gfx_DeleteTexture(&s->textures[i].ID);

		TabListOverlay_AddName(s, id, i);
		TabListOverlay_SortAndLayout(s);
		return;
	}
}

static void TabListOverlay_Remove(void* obj, int id) {
	struct TabListOverlay* s = (struct TabListOverlay*)obj;
	int i;
	for (i = 0; i < s->usedCount; i++)
	{
		if (s->ids[i] != id) continue;

		TabListOverlay_DeleteAt(s, i);
		TabListOverlay_SortAndLayout(s);
		return;
	}
}

static int TabListOverlay_PointerDown(void* screen, int id, int x, int y) {
	struct TabListOverlay* s = (struct TabListOverlay*)screen;
	cc_string text; char textBuffer[STRING_SIZE * 4];
	struct Texture tex;
	cc_string player;
	int i;

	if (!((struct Screen*)Gui_Chat)->grabsInput) return false;
	String_InitArray(text, textBuffer);

	for (i = 0; i < s->usedCount; i++)
	{
		if (!s->textures[i].ID || s->ids[i] == GROUP_NAME_ID) continue;
		tex = s->textures[i];
		if (!Gui_Contains(tex.x, tex.y, tex.width, tex.height, x, y)) continue;

		player = TabList_UNSAFE_GetPlayer(s->ids[i]);
		String_Format1(&text, "%s ", &player);
		ChatScreen_AppendInput(&text);
		return TOUCH_TYPE_GUI;
	}
	return false;
}

static void TabListOverlay_KeyUp(void* screen, int key, struct InputDevice* device) {
	struct TabListOverlay* s = (struct TabListOverlay*)screen;
	if (!InputBind_Claims(BIND_TABLIST, key, device) || s->staysOpen) return;
	Gui_Remove((struct Screen*)s);
}

static void TabListOverlay_ContextLost(void* screen) {
	struct TabListOverlay* s = (struct TabListOverlay*)screen;
	int i;
	for (i = 0; i < s->usedCount; i++)
	{
		Gfx_DeleteTexture(&s->textures[i].ID);
	}

	Elem_Free(&s->title);
	Font_Free(&s->font);
	Screen_ContextLost(screen);
}

static void TabListOverlay_ContextRecreated(void* screen) {
	struct TabListOverlay* s = (struct TabListOverlay*)screen;
	int size, id;

	size = Drawer2D.BitmappedText ? 16 : 11;
	Font_Make(&s->font, size, FONT_FLAGS_PADDING);
	s->usedCount = 0;

	TextWidget_SetConst(&s->title, "Connected players:", &s->font);
	Font_SetPadding(&s->font, 1);
	Screen_UpdateVb(screen);

	/* TODO: Just recreate instead of this? maybe */
	for (id = 0; id < TABLIST_MAX_NAMES; id++) 
	{
		if (!TabList.NameOffsets[id]) continue;
		TabListOverlay_AddName(s, (EntityID)id, -1);
	}
	TabListOverlay_SortAndLayout(s); /* TODO: Not do layout here too */
}

static void TabListOverlay_BuildMesh(void* screen) {
	struct TabListOverlay* s = (struct TabListOverlay*)screen;
	struct Screen*   grabbed = Gui_GetInputGrab();
	struct VertexTextured* v;
	struct Texture tex;
	int i;
	
	v = (struct VertexTextured*)Gfx_LockDynamicVb(s->vb,
										VERTEX_FORMAT_TEXTURED, TEXTWIDGET_MAX + s->usedCount * 4);
	Widget_BuildMesh(&s->title, &v);

	for (i = 0; i < s->usedCount; i++)
	{
		if (!s->textures[i].ID) continue;
		tex = s->textures[i];

		if (grabbed && s->ids[i] != GROUP_NAME_ID) {
			if (Gui_ContainsPointers(tex.x, tex.y, tex.width, tex.height)) tex.x += 4;
		}
		Gfx_Make2DQuad(&tex, PACKEDCOL_WHITE, &v);
	}
	Gfx_UnlockDynamicVb(s->vb);
}

static void TabListOverlay_Render(void* screen, float delta) {
	struct TabListOverlay* s = (struct TabListOverlay*)screen;
	int i, offset = 0;
	PackedCol topCol    = PackedCol_Make( 0,  0,  0, 180);
	PackedCol bottomCol = PackedCol_Make(50, 50, 50, 205);

	if (Game_HideGui || !IsOnlyChatActive()) return;

	Gfx_3DS_SetRenderScreen(TOP_SCREEN);

	Gfx_Draw2DGradient(s->x, s->y, s->width, s->height, topCol, bottomCol);

	Gfx_SetVertexFormat(VERTEX_FORMAT_TEXTURED);
	Gfx_BindDynamicVb(s->vb);
	offset = Widget_Render2(&s->title, offset);

	for (i = 0; i < s->usedCount; i++)
	{
		if (!s->textures[i].ID) continue;
		Gfx_BindTexture(s->textures[i].ID);

		Gfx_DrawVb_IndexedTris_Range(4, offset, DRAW_HINT_RECT);
		offset += 4;
	}

	Gfx_3DS_SetRenderScreen(BOTTOM_SCREEN);
}

static void TabListOverlay_Free(void* screen) {
	struct TabListOverlay* s = (struct TabListOverlay*)screen;
	tablist_active = false;
	Event_Unregister_(&TabListEvents.Added,   s, TabListOverlay_Add);
	Event_Unregister_(&TabListEvents.Changed, s, TabListOverlay_Update);
	Event_Unregister_(&TabListEvents.Removed, s, TabListOverlay_Remove);
}

static void TabListOverlay_Init(void* screen) {
	struct TabListOverlay* s = (struct TabListOverlay*)screen;
	tablist_active   = true;
	s->classic       = Gui.ClassicTabList || !Server.SupportsExtPlayerList;
	s->elementOffset = s->classic ? 0 : 10;
	s->maxVertices   = TABLIST_MAX_VERTICES;
	TextWidget_Init(&s->title);

	Event_Register_(&TabListEvents.Added,   s, TabListOverlay_Add);
	Event_Register_(&TabListEvents.Changed, s, TabListOverlay_Update);
	Event_Register_(&TabListEvents.Removed, s, TabListOverlay_Remove);
}

static const struct ScreenVTABLE TabListOverlay_VTABLE = {
	TabListOverlay_Init,        Screen_NullUpdate,     TabListOverlay_Free,
	TabListOverlay_Render,      TabListOverlay_BuildMesh,
	Screen_FInput,              TabListOverlay_KeyUp,  Screen_FKeyPress, Screen_FText,
	TabListOverlay_PointerDown, Screen_PointerUp,      Screen_FPointer,  Screen_FMouseScroll,
	TabListOverlay_Layout, TabListOverlay_ContextLost, TabListOverlay_ContextRecreated
};
void TabListOverlay_Show(cc_bool staysOpen) {
	struct TabListOverlay* s  = &TabListOverlay_Instance;
	s->VTABLE    = &TabListOverlay_VTABLE;
	s->staysOpen = staysOpen;
	Gui_Add((struct Screen*)s, GUI_PRIORITY_TABLIST);
}
#else
void TabListOverlay_Show(cc_bool staysOpen) { }
#endif


/*########################################################################################################################*
*--------------------------------------------------------ChatScreen-------------------------------------------------------*
*#########################################################################################################################*/
static struct ChatScreen {
	Screen_Body
	float chatAcc;
	cc_bool suppressNextPress;
	int chatIndex, paddingX, paddingY;
	int lastDownloadStatus;
	struct FontDesc chatFont;
	struct ChatInputWidget input;
	struct TextGroupWidget chat, clientStatus;
	struct SpecialInputWidget altText;
#ifdef CC_BUILD_TOUCH
	struct ButtonWidget send, cancel, more;
#endif

	struct Texture clientStatusTextures[CHAT_MAX_CLIENTSTATUS];
	struct Texture chatTextures[GUI_MAX_CHATLINES];
} ChatScreen_Instance CC_BIG_VAR;

static void ChatScreen_UpdateChatYOffsets(struct ChatScreen* s) {
	int pad, y;
	/* Determining chat Y requires us to know hotbar's position */
	HUDScreen_LayoutHotbar();
		
	y = min(s->input.base.y, Gui_HUD->hotbar.y);
	/* Survival: the hearts (+ armor) row sits 10 GUI px above the hotbar
	    (9px icons + 1px gap) - lift the chat stack above it so chat
	    history doesn't cover the hearts while chatting */
	if (SurvivalTest_Enabled) {
		int rowH = (int)(10.0f * Gui_GetHotbarScale() * DisplayInfo.ScaleY);
		y = min(y, Gui_HUD->hotbar.y - rowH);
	}
	y -= s->input.base.yOffset; /* add some padding */
	s->altText.yOffset = Window_UI.Height - y;
	Widget_Layout(&s->altText);

	pad = s->altText.active ? 5 : 10;
	s->clientStatus.yOffset = Window_UI.Height - s->altText.y + pad;
	Widget_Layout(&s->clientStatus);
	s->chat.yOffset = s->clientStatus.yOffset + s->clientStatus.height;
	Widget_Layout(&s->chat);
}

static void ChatScreen_OnInputTextChanged(void* elem) {
	ChatScreen_UpdateChatYOffsets(&ChatScreen_Instance);
}

static cc_string ChatScreen_GetChat(int i) {
	i += ChatScreen_Instance.chatIndex;

	if (i >= 0 && i < Chat_Log.count) {
		return StringsBuffer_UNSAFE_Get(&Chat_Log, i);
	}
	return String_Empty;
}

static cc_string ChatScreen_GetClientStatus(int i) { return Chat_ClientStatus[i]; }

static void ChatScreen_FreeChatFonts(struct ChatScreen* s) {
	Font_Free(&s->chatFont);
}

static cc_bool ChatScreen_ChatUpdateFont(struct ChatScreen* s) {
	int size = (int)(8  * Gui_GetChatScale());
	Math_Clamp(size, 8, 64);

	/* don't recreate font if possible */
	/* TODO: Add function for this, don't use Display_ScaleY (Drawer2D_SameFontSize ??) */
	if (Display_ScaleY(size) == s->chatFont.size) return false;
	ChatScreen_FreeChatFonts(s);
	Font_Make(&s->chatFont, size, FONT_FLAGS_PADDING);

	ChatInputWidget_SetFont(&s->input,        &s->chatFont);
	TextGroupWidget_SetFont(&s->chat,         &s->chatFont);
	TextGroupWidget_SetFont(&s->clientStatus, &s->chatFont);
	return true;
}

static void ChatScreen_Redraw(struct ChatScreen* s) {
	TextGroupWidget_RedrawAll(&s->chat);
	TextGroupWidget_RedrawAll(&s->clientStatus);

	if (s->grabsInput) InputWidget_UpdateText(&s->input.base);
	SpecialInputWidget_Redraw(&s->altText);
}

static int ChatScreen_ClampChatIndex(int index) {
	int maxIndex = Chat_Log.count - Gui.Chatlines;
	int minIndex = min(0, maxIndex);
	Math_Clamp(index, minIndex, maxIndex);
	return index;
}

static void ChatScreen_ScrollChatBy(struct ChatScreen* s, int delta) {
	int newIndex = ChatScreen_ClampChatIndex(s->chatIndex + delta);
	delta = newIndex - s->chatIndex;
	if (Game_PureClassic) return;

	while (delta) {
		if (delta < 0) {
			/* scrolling up to oldest */
			s->chatIndex--; delta++;
			TextGroupWidget_ShiftDown(&s->chat);
		} else {
			/* scrolling down to newest */
			s->chatIndex++; delta--;
			TextGroupWidget_ShiftUp(&s->chat);
		}
	}
}

static void ChatScreen_EnterChatInput(struct ChatScreen* s, cc_bool close) {
	struct InputWidget* input;
	int defaultIndex;

	s->grabsInput = false;
	Gui_UpdateInputGrab();
	OnscreenKeyboard_Close();
	if (close) InputWidget_Clear(&s->input.base);

	input = &s->input.base;
	input->OnPressedEnter(input);
	SpecialInputWidget_SetActive(&s->altText, false);
	ChatScreen_UpdateChatYOffsets(s);

	/* Reset chat when user has scrolled up in chat history */
	defaultIndex = Chat_Log.count - Gui.Chatlines;
	if (s->chatIndex != defaultIndex) {
		s->chatIndex = defaultIndex;
		TextGroupWidget_RedrawAll(&s->chat);
	}
}

static void ChatScreen_ColCodeChanged(void* screen, int code) {
	struct ChatScreen* s = (struct ChatScreen*)screen;
	float caretAcc;
	if (Gfx.LostContext) return;

	SpecialInputWidget_UpdateCols(&s->altText);
	TextGroupWidget_RedrawAllWithCol(&s->chat,         code);
	TextGroupWidget_RedrawAllWithCol(&s->clientStatus, code);

	/* Some servers have plugins that redefine colours constantly */
	/* Preserve caret accumulator so caret blinking stays consistent */
	caretAcc = s->input.base.caretAccumulator;
	InputWidget_UpdateText(&s->input.base);
	s->input.base.caretAccumulator = caretAcc;
}

static void ChatScreen_ChatReceived(void* screen, const cc_string* msg, int type) {
	struct ChatScreen* s = (struct ChatScreen*)screen;
	if (Gfx.LostContext) return;

	if (type == MSG_TYPE_NORMAL) {
		s->dirty = true;
		s->chatIndex++;
		if (!Gui.Chatlines) return;

		TextGroupWidget_ShiftUp(&s->chat);
	} else if (type >= MSG_TYPE_CLIENTSTATUS_1 && type <= MSG_TYPE_CLIENTSTATUS_2) {
		s->dirty = true;
		TextGroupWidget_Redraw(&s->clientStatus, type - MSG_TYPE_CLIENTSTATUS_1);
		ChatScreen_UpdateChatYOffsets(s);
	}
}


static void ChatScreen_Update(void* screen, float delta) {
	
}

static void ChatScreen_DrawChatBackground(struct ChatScreen* s) {
	int usedHeight = TextGroupWidget_UsedHeight(&s->chat);
	int x = s->chat.x;
	int y = s->chat.y + s->chat.height - usedHeight;

	int width  = max(s->clientStatus.width, s->chat.width);
	int height = usedHeight + s->clientStatus.height;

	if (height > 0) {
		PackedCol backCol = PackedCol_Make(0, 0, 0, 127);
		Gfx_Draw2DFlat( x - s->paddingX,          y - s->paddingY, 
					width + s->paddingX * 2, height + s->paddingY * 2, backCol);
	}
}

static void ChatScreen_DrawChat(struct ChatScreen* s, float delta) {
	GfxResourceID texID;
	double now;
	int i, logIdx;

	Elem_Render(&s->clientStatus);

	Gfx_SetVertexFormat(VERTEX_FORMAT_TEXTURED);
	Gfx_BindDynamicVb(s->vb);
	now = Game.Time;

	if (s->grabsInput) {
		Widget_Render2(&s->chat, 0);
	} else {
		/* Only render recent chat */
		for (i = 0; i < s->chat.lines; i++) 
		{
			texID  = s->chat.textures[i].ID;
			if (!texID) continue;
			logIdx = s->chatIndex + i;

			if (logIdx < 0 || logIdx >= Chat_Log.count) continue;
			/* Only draw chat within last 10 seconds */
			if (Chat_GetLogTime(logIdx) + 10 < now) continue;
			
			Gfx_BindTexture(texID);
			Gfx_DrawVb_IndexedTris_Range(4, i * 4, DRAW_HINT_RECT);
		}
	}

	if (s->grabsInput) {
		Elem_Render(&s->input.base);
		if (s->altText.active) {
			Elem_Render(&s->altText);
		}

#ifdef CC_BUILD_TOUCH
		if (!Gui.TouchUI) return;
		Gfx_3DS_SetRenderScreen(BOTTOM_SCREEN);
		Elem_Render(&s->more);
		Elem_Render(&s->send);
		Elem_Render(&s->cancel);
		Gfx_3DS_SetRenderScreen(TOP_SCREEN);
#endif
	}
}

static void ChatScreen_ContextLost(void* screen) {
	struct ChatScreen* s = (struct ChatScreen*)screen;
	ChatScreen_FreeChatFonts(s);
	Screen_ContextLost(s);

	Elem_Free(&s->chat);
	Elem_Free(&s->input.base);
	Elem_Free(&s->altText);
	Elem_Free(&s->clientStatus);

#ifdef CC_BUILD_TOUCH
	Elem_Free(&s->more);
	Elem_Free(&s->send);
	Elem_Free(&s->cancel);
#endif
}

static void ChatScreen_ContextRecreated(void* screen) {
	struct ChatScreen* s = (struct ChatScreen*)screen;
	struct FontDesc font;
	ChatScreen_ChatUpdateFont(s);
	ChatScreen_Redraw(s);
	Screen_UpdateVb(s);

#ifdef CC_BUILD_TOUCH
	if (!Gui.TouchUI) return;
	Gui_MakeTitleFont(&font);
	ButtonWidget_SetConst(&s->more,   "More",   &font);
	ButtonWidget_SetConst(&s->send,   "Send",   &font);
	ButtonWidget_SetConst(&s->cancel, "Cancel", &font);
	Font_Free(&font);
#endif
}

static int ChatScreen_CalcMaxVertices(void* screen) {
	struct ChatScreen* s = (struct ChatScreen*)screen;
	struct TextGroupWidget* chat = &s->chat;
	/* In case chatlines is 0 */
	return max(4, chat->VTABLE->GetMaxVertices(chat));
}

static void ChatScreen_BuildMesh(void* screen) {
	struct ChatScreen* s = (struct ChatScreen*)screen;
	struct VertexTextured* data;
	struct VertexTextured** ptr;

	data = Screen_LockVb(s);
	ptr  = &data;

	Widget_BuildMesh(&s->chat, ptr);
	Gfx_UnlockDynamicVb(s->vb);
}

static void ChatScreen_Layout(void* screen) {
	struct ChatScreen* s = (struct ChatScreen*)screen;
	if (ChatScreen_ChatUpdateFont(s)) ChatScreen_Redraw(s);

	s->paddingX = Display_ScaleX(5);
	s->paddingY = Display_ScaleY(5);

	Widget_SetLocation(&s->input.base,   ANCHOR_MIN, ANCHOR_MAX,  5, 5);
	Widget_SetLocation(&s->altText,      ANCHOR_MIN, ANCHOR_MAX,  5, 5);
	Widget_SetLocation(&s->chat,         ANCHOR_MIN, ANCHOR_MAX, 10, 0);
	Widget_SetLocation(&s->clientStatus, ANCHOR_MIN, ANCHOR_MAX, 10, 0);
	ChatScreen_UpdateChatYOffsets(s);

#ifdef CC_BUILD_TOUCH
	if (Window_Main.SoftKeyboard == SOFT_KEYBOARD_SHIFT) {
		Widget_SetLocation(&s->send,   ANCHOR_MAX, ANCHOR_MAX, 10,  60);
		Widget_SetLocation(&s->cancel, ANCHOR_MAX, ANCHOR_MAX, 10,  10);
		Widget_SetLocation(&s->more,   ANCHOR_MAX, ANCHOR_MAX, 10, 110);
	} else {
		Widget_SetLocation(&s->send,   ANCHOR_MAX, ANCHOR_MIN, 10,  10);
		Widget_SetLocation(&s->cancel, ANCHOR_MAX, ANCHOR_MIN, 10,  60);
		Widget_SetLocation(&s->more,   ANCHOR_MAX, ANCHOR_MIN, 10, 110);
	}
#endif
}

static int ChatScreen_KeyPress(void* screen, char keyChar) {
	struct ChatScreen* s = (struct ChatScreen*)screen;
	if (!s->grabsInput) return false;

	if (s->suppressNextPress) {
		s->suppressNextPress = false;
		return false;
	}

	InputWidget_Append(&s->input.base, keyChar);
	return true;
}

static int ChatScreen_TextChanged(void* screen, const cc_string* str) {
	struct ChatScreen* s = (struct ChatScreen*)screen;
	if (!s->grabsInput) return false;

	InputWidget_SetText(&s->input.base, str);
	return true;
}

static int ChatScreen_KeyDown(void* screen, int key, struct InputDevice* device) {
	static const cc_string slash = String_FromConst("/");
	struct ChatScreen* s = (struct ChatScreen*)screen;
	int playerListKey    = KeyBind_Mappings[BIND_TABLIST].button1;
	cc_bool handlesList  = playerListKey != CCKEY_TAB || !Gui.TabAutocomplete || !s->grabsInput;

	if (InputBind_Claims(BIND_TABLIST, key, device) && handlesList) {
		if (!tablist_active && !Server.IsSinglePlayer) {
			TabListOverlay_Show(false);
		}
		return true;
	}

	s->suppressNextPress = false;
	/* Handle chat text input */
	if (s->grabsInput) {
#ifdef CC_BUILD_WEB
		/* See reason for this in HandleInputUp */
		if (InputBind_Claims(BIND_SEND_CHAT, key, device) || key == CCKEY_KP_ENTER) {
			ChatScreen_EnterChatInput(s, false);
#else
		if (InputBind_Claims(BIND_SEND_CHAT, key, device) || key == CCKEY_KP_ENTER || key == device->escapeButton) {
			ChatScreen_EnterChatInput(s, key == device->escapeButton);
#endif
		} else if (key == device->pageUpButton) {
			ChatScreen_ScrollChatBy(s, -Gui.Chatlines);
		} else if (key == device->pageDownButton) {
			ChatScreen_ScrollChatBy(s, +Gui.Chatlines);
		} else if (key == CCWHEEL_UP) {
			ChatScreen_ScrollChatBy(s, -1);
		} else if (key == CCWHEEL_DOWN) {
			ChatScreen_ScrollChatBy(s, +1);
		} else {
			Elem_HandlesKeyDown(&s->input.base, key, device);
		}
		return key < CCKEY_F1 || key > CCKEY_F24;
	}

	if (InputBind_Claims(BIND_CHAT, key, device)) {
		ChatScreen_OpenInput(&String_Empty);
	} else if (key == CCKEY_SLASH) {
		ChatScreen_OpenInput(&slash);
	} else if (InputBind_Claims(BIND_INVENTORY, key, device) ||
			(SurvivalTest_Enabled && key == 'E')) {
		/* Survival/Indev also opens the inventory with E (modern Minecraft's
		    inventory key); E is BIND_FLY_DOWN by default, which is inert in
		    survival since flying is disabled, so there's no conflict. */
		SurvivalInvScreen_Show();
	} else {
		return false;
	}
	return true;
}

static void ChatScreen_ToggleAltInput(struct ChatScreen* s) {
	SpecialInputWidget_SetActive(&s->altText, !s->altText.active);
	ChatScreen_UpdateChatYOffsets(s);
}

static void ChatScreen_KeyUp(void* screen, int key, struct InputDevice* device) {
	struct ChatScreen* s = (struct ChatScreen*)screen;
	if (!s->grabsInput || (struct Screen*)s != Gui.InputGrab) return;

#ifdef CC_BUILD_WEB
	/* See reason for this in HandleInputUp */
	if (key == CCKEY_ESCAPE) ChatScreen_EnterChatInput(s, true);
#endif

	if (Server.SupportsFullCP437 && InputBind_Claims(BIND_EXT_INPUT, key, device)) {
		if (!Window_Main.Focused) return;
		ChatScreen_ToggleAltInput(s);
	}
}

static int ChatScreen_MouseScroll(void* screen, float delta) {
	struct ChatScreen* s = (struct ChatScreen*)screen;
	return s->grabsInput;
}

static int ChatScreen_PointerDown(void* screen, int id, int x, int y) {
	cc_string text; char textBuffer[STRING_SIZE * 4];
	struct ChatScreen* s = (struct ChatScreen*)screen;
	int height, chatY, i;
	if (Game_HideGui) return false;

	if (!s->grabsInput) {
		if (!Gui_TouchUI) return false;
		String_InitArray(text, textBuffer);

		/* Should be able to click on links with touch */
		i = TextGroupWidget_GetSelected(&s->chat, &text, x, y);
		if (!Utils_IsUrlPrefix(&text)) return false;

		if (Chat_GetLogTime(s->chatIndex + i) + 10 < Game.Time) return false;
		UrlWarningOverlay_Show(&text); return TOUCH_TYPE_GUI;
	}

#ifdef CC_BUILD_TOUCH
	if (Gui.TouchUI) {
		if (Widget_Contains(&s->send, x, y)) {
			ChatScreen_EnterChatInput(s, false); return TOUCH_TYPE_GUI;
		}
		if (Widget_Contains(&s->cancel, x, y)) {
			ChatScreen_EnterChatInput(s, true); return TOUCH_TYPE_GUI;
		}
		if (Widget_Contains(&s->more, x, y)) {
			ChatScreen_ToggleAltInput(s); return TOUCH_TYPE_GUI;
		}
	}
#endif

	if (!Widget_Contains(&s->chat, x, y)) {
		if (s->altText.active && Widget_Contains(&s->altText, x, y)) {
			Elem_HandlesPointerDown(&s->altText, id, x, y);
			ChatScreen_UpdateChatYOffsets(s);
			return TOUCH_TYPE_GUI;
		}
		Elem_HandlesPointerDown(&s->input.base, id, x, y);
		return TOUCH_TYPE_GUI;
	}

	height = TextGroupWidget_UsedHeight(&s->chat);
	chatY  = s->chat.y + s->chat.height - height;
	if (!Gui_Contains(s->chat.x, chatY, s->chat.width, height, x, y)) return false;

	String_InitArray(text, textBuffer);
	TextGroupWidget_GetSelected(&s->chat, &text, x, y);
	if (!text.length) return false;

	if (Utils_IsUrlPrefix(&text) && Process_OpenSupported) {
		UrlWarningOverlay_Show(&text);
	} else if (Gui.ClickableChat) {
		ChatScreen_AppendInput(&text);
	}
	return TOUCH_TYPE_GUI;
}

static void ChatScreen_Init(void* screen) {
	struct ChatScreen* s = (struct ChatScreen*)screen;
	ChatInputWidget_Create(&s->input);
	s->input.base.OnTextChanged = ChatScreen_OnInputTextChanged;
	SpecialInputWidget_Create(&s->altText, &s->chatFont, &s->input.base);

	TextGroupWidget_Create(&s->chat, Gui.Chatlines,
							s->chatTextures, ChatScreen_GetChat);
	TextGroupWidget_Create(&s->clientStatus, CHAT_MAX_CLIENTSTATUS,
							s->clientStatusTextures, ChatScreen_GetClientStatus);

	s->clientStatus.collapsible[0] = true;
	s->clientStatus.collapsible[1] = true;

	s->chat.underlineUrls = !Game_ClassicMode;
	s->chatIndex = Chat_Log.count - Gui.Chatlines;

	Event_Register_(&ChatEvents.ChatReceived,   s, ChatScreen_ChatReceived);
	Event_Register_(&ChatEvents.ColCodeChanged, s, ChatScreen_ColCodeChanged);
	
	s->maxVertices = ChatScreen_CalcMaxVertices(s);
	
	/* For dual screen builds, chat is still rendered on the main game screen */
	s->input.base.flags   |= WIDGET_FLAG_MAINSCREEN;
	s->altText.flags      |= WIDGET_FLAG_MAINSCREEN;
	s->chat.flags         |= WIDGET_FLAG_MAINSCREEN;
	s->clientStatus.flags |= WIDGET_FLAG_MAINSCREEN;

#ifdef CC_BUILD_TOUCH
	ButtonWidget_Init(&s->send,   100, NULL);
	ButtonWidget_Init(&s->cancel, 100, NULL);
	ButtonWidget_Init(&s->more,   100, NULL);
#endif
}

static void ChatScreen_Render(void* screen, float delta) {
	struct ChatScreen* s = (struct ChatScreen*)screen;
	Gfx_3DS_SetRenderScreen(TOP_SCREEN);

	if (s->grabsInput) s->input.base.caretAccumulator += delta;

	if (Game_HideGui && s->grabsInput) {
		Elem_Render(&s->input.base);
	}
	if (!Game_HideGui) {
		if (s->grabsInput && !Gui.ClassicChat) {
			ChatScreen_DrawChatBackground(s);
		}

		ChatScreen_DrawChat(s, delta);
	}
	Gfx_3DS_SetRenderScreen(BOTTOM_SCREEN);
}

static void ChatScreen_Free(void* screen) {
	struct ChatScreen* s = (struct ChatScreen*)screen;
	Event_Unregister_(&ChatEvents.ChatReceived,   s, ChatScreen_ChatReceived);
	Event_Unregister_(&ChatEvents.ColCodeChanged, s, ChatScreen_ColCodeChanged);
}

static const struct ScreenVTABLE ChatScreen_VTABLE = {
	ChatScreen_Init,        ChatScreen_Update, ChatScreen_Free,
	ChatScreen_Render,      ChatScreen_BuildMesh,
	ChatScreen_KeyDown,     ChatScreen_KeyUp,  ChatScreen_KeyPress, ChatScreen_TextChanged,
	ChatScreen_PointerDown, Screen_PointerUp,  Screen_FPointer,     ChatScreen_MouseScroll,
	ChatScreen_Layout, ChatScreen_ContextLost, ChatScreen_ContextRecreated
};
void ChatScreen_Show(void) {
	struct ChatScreen* s  = &ChatScreen_Instance;
	s->lastDownloadStatus = HTTP_PROGRESS_NOT_WORKING_ON;

	s->VTABLE = &ChatScreen_VTABLE;
	Gui_Chat  = s;
	Gui_Add((struct Screen*)s, GUI_PRIORITY_CHAT);
}

void ChatScreen_OpenInput(const cc_string* text) {
	struct ChatScreen* s = &ChatScreen_Instance;
	struct OpenKeyboardArgs args;
	s->suppressNextPress = true;
	s->grabsInput        = true;

	Gui_UpdateInputGrab();
	String_Copy(&s->input.base.text, text);

	OpenKeyboardArgs_Init(&args, text, KEYBOARD_TYPE_TEXT | KEYBOARD_FLAG_SEND);
	args.placeholder = "Enter chat";
	args.multiline   = true;
	args.yOffset     = 30;
	OnscreenKeyboard_Open(&args);

	Widget_SetDisabled(&s->input.base, args.opaque);
	InputWidget_UpdateText(&s->input.base);
}

void ChatScreen_AppendInput(const cc_string* text) {
	struct ChatScreen* s = &ChatScreen_Instance;
	InputWidget_AppendText(&s->input.base, text);
}

void ChatScreen_SetChatlines(int lines) {
	struct ChatScreen* s = &ChatScreen_Instance;
	Elem_Free(&s->chat);
	s->chatIndex += s->chat.lines - lines;
	s->chat.lines = lines;
	TextGroupWidget_RedrawAll(&s->chat);

	s->maxVertices = ChatScreen_CalcMaxVertices(s);
	Screen_UpdateVb(s);
	s->dirty = true;
}


/*########################################################################################################################*
*----------------------------------------------------SpecialTextScreen----------------------------------------------------*
*#########################################################################################################################*/
#ifdef CC_BUILD_NETWORKING
static struct SpecialTextScreen {
	Screen_Body
	int lastDownloadStatus;
	struct FontDesc chatFont, announcementFont, bigAnnouncementFont, smallAnnouncementFont;
	struct TextWidget announcement, bigAnnouncement, smallAnnouncement;
	struct TextGroupWidget status, bottomRight;

	struct Texture statusTextures[CHAT_MAX_STATUS];
	struct Texture bottomRightTextures[CHAT_MAX_BOTTOMRIGHT];
	struct Widget* __widgets[3 + 2];
} SpecialTextScreen_Instance CC_BIG_VAR;

static cc_string SpecialTextScreen_GetStatus(int i)       { return Chat_Status[i]; }
static cc_string SpecialTextScreen_GetBottomRight(int i)  { return Chat_BottomRight[2 - i]; }

static void SpecialTextScreen_FreeChatFonts(struct SpecialTextScreen* s) {
	Font_Free(&s->chatFont);
	Font_Free(&s->announcementFont);
	Font_Free(&s->bigAnnouncementFont);
	Font_Free(&s->smallAnnouncementFont);
}

static cc_bool SpecialTextScreen_ChatUpdateFont(struct SpecialTextScreen* s) {
	int size = (int)(8  * Gui_GetChatScale());
	Math_Clamp(size, 8, 64);

	/* don't recreate font if possible */
	/* TODO: Add function for this, don't use Display_ScaleY (Drawer2D_SameFontSize ??) */
	if (Display_ScaleY(size) == s->chatFont.size) return false;
	SpecialTextScreen_FreeChatFonts(s);
	Font_Make(&s->chatFont, size, FONT_FLAGS_PADDING);

	size = (int)(16 * Gui_GetChatScale());
	Math_Clamp(size, 8, 64);
	Font_Make(&s->announcementFont, size, FONT_FLAGS_NONE);
	size = (int)(24 * Gui_GetChatScale());
	Math_Clamp(size, 8, 64);
	Font_Make(&s->bigAnnouncementFont, size, FONT_FLAGS_NONE);
	size = (int)(8 * Gui_GetChatScale());
	Math_Clamp(size, 8, 64);
	Font_Make(&s->smallAnnouncementFont, size, FONT_FLAGS_NONE);

	TextGroupWidget_SetFont(&s->status,       &s->chatFont);
	TextGroupWidget_SetFont(&s->bottomRight,  &s->chatFont);
	return true;
}

static void SpecialTextScreen_UpdateTexpackStatus(struct SpecialTextScreen* s) {
	int progress = Http_CheckProgress(TexturePack_ReqID);
	cc_string msg; char msgBuffer[STRING_SIZE];
	if (progress == s->lastDownloadStatus) return;

	s->lastDownloadStatus = progress;
	String_InitArray(msg, msgBuffer);

	if (progress == HTTP_PROGRESS_MAKING_REQUEST) {
		String_AppendConst(&msg, "&eRetrieving texture pack..");
	} else if (progress == HTTP_PROGRESS_FETCHING_DATA) {
		String_AppendConst(&msg, "&eDownloading texture pack");
	} else if (progress >= 0 && progress <= 100) {
		String_Format1(&msg, "&eDownloading texture pack (&7%i&e%%)", &progress);
	}
	Chat_AddOf(&msg, MSG_TYPE_EXTRASTATUS_1);
}

static void SpecialTextScreen_ColCodeChanged(void* screen, int code) {
	struct SpecialTextScreen* s = (struct SpecialTextScreen*)screen;
	if (Gfx.LostContext) return;

	TextGroupWidget_RedrawAllWithCol(&s->status,       code);
	TextGroupWidget_RedrawAllWithCol(&s->bottomRight,  code);
}

static void SpecialTextScreen_ChatReceived(void* screen, const cc_string* msg, int type) {
	struct SpecialTextScreen* s = (struct SpecialTextScreen*)screen;
	if (Gfx.LostContext) return;

	if (type >= MSG_TYPE_STATUS_1 && type <= MSG_TYPE_STATUS_3) {
		/* Status[0] is for texture pack downloading message */
		/* Status[1] is for reduced performance mode message */
		TextGroupWidget_Redraw(&s->status, 2 + (type - MSG_TYPE_STATUS_1));
		s->dirty = true;
	} else if (type >= MSG_TYPE_BOTTOMRIGHT_1 && type <= MSG_TYPE_BOTTOMRIGHT_3) {
		/* Bottom3 is top most line, so need to redraw index 0 */
		TextGroupWidget_Redraw(&s->bottomRight, 2 - (type - MSG_TYPE_BOTTOMRIGHT_1));
		s->dirty = true;
	} else if (type == MSG_TYPE_ANNOUNCEMENT) {
		TextWidget_Set(&s->announcement, msg, &s->announcementFont);
		s->dirty = true;
	} else if (type == MSG_TYPE_BIGANNOUNCEMENT) {
		TextWidget_Set(&s->bigAnnouncement, msg, &s->bigAnnouncementFont);
		s->dirty = true;
	} else if (type == MSG_TYPE_SMALLANNOUNCEMENT) {
		TextWidget_Set(&s->smallAnnouncement, msg, &s->smallAnnouncementFont);
		s->dirty = true;
	} else if (type >= MSG_TYPE_EXTRASTATUS_1 && type <= MSG_TYPE_EXTRASTATUS_2) {
		/* Status[0] is for texture pack downloading message */
		/* Status[1] is for reduced performance mode message */
		TextGroupWidget_Redraw(&s->status, type - MSG_TYPE_EXTRASTATUS_1);
		s->dirty = true;
	} 
}

static void SpecialTextScreen_Redraw(struct SpecialTextScreen* s) {
	TextWidget_Set(&s->announcement,      &Chat_Announcement, &s->announcementFont);
	TextWidget_Set(&s->bigAnnouncement,   &Chat_BigAnnouncement, &s->bigAnnouncementFont);
	TextWidget_Set(&s->smallAnnouncement, &Chat_SmallAnnouncement, &s->smallAnnouncementFont);

	TextGroupWidget_RedrawAll(&s->status);
	TextGroupWidget_RedrawAll(&s->bottomRight);
}

static void SpecialTextScreen_ContextLost(void* screen) {
	struct SpecialTextScreen* s = (struct SpecialTextScreen*)screen;
	SpecialTextScreen_FreeChatFonts(s);
	Screen_ContextLost(s);
}

static void SpecialTextScreen_ContextRecreated(void* screen) {
	struct SpecialTextScreen* s = (struct SpecialTextScreen*)screen;

	SpecialTextScreen_ChatUpdateFont(s);
	SpecialTextScreen_Redraw(s);
	Screen_UpdateVb(s);
}

static void SpecialTextScreen_Layout(void* screen) {
	struct SpecialTextScreen* s = (struct SpecialTextScreen*)screen;
	if (SpecialTextScreen_ChatUpdateFont(s)) SpecialTextScreen_Redraw(s);

	Widget_SetLocation(&s->status,      ANCHOR_MAX, ANCHOR_MIN,  0, 0);
	Widget_SetLocation(&s->bottomRight, ANCHOR_MAX, ANCHOR_MAX,  0, 0);

	/* Can't use Widget_SetLocation because it DPI scales input */
	s->bottomRight.yOffset = HUDScreen_LayoutHotbar() + Display_ScaleY(15);
	Widget_Layout(&s->bottomRight);

	Widget_SetLocation(&s->announcement, ANCHOR_CENTRE, ANCHOR_CENTRE, 0, 0);
	s->announcement.yOffset = -Window_UI.Height / 4;
	Widget_Layout(&s->announcement);

	Widget_SetLocation(&s->bigAnnouncement, ANCHOR_CENTRE, ANCHOR_CENTRE, 0, 0);
	s->bigAnnouncement.yOffset = -Window_UI.Height / 16;
	Widget_Layout(&s->bigAnnouncement);

	Widget_SetLocation(&s->smallAnnouncement, ANCHOR_CENTRE, ANCHOR_CENTRE, 0, 0);
	s->smallAnnouncement.yOffset = Window_UI.Height / 20;
	Widget_Layout(&s->smallAnnouncement);
}

static void SpecialTextScreen_Update(void* screen, float delta) {
	struct SpecialTextScreen* s = (struct SpecialTextScreen*)screen;
	SpecialTextScreen_UpdateTexpackStatus(s);

	/* Destroy announcement texture before even rendering it at all, */
	/* otherwise changing texture pack shows announcement for one frame */
	if (s->announcement.tex.ID && (Chat_AnnouncementLeft -= delta) <= 0) {
		Elem_Free(&s->announcement);
		s->dirty = true;
	}

	if (s->bigAnnouncement.tex.ID && (Chat_BigAnnouncementLeft -= delta) <= 0) {
		Elem_Free(&s->bigAnnouncement);
		s->dirty = true;
	}

	if (s->smallAnnouncement.tex.ID && (Chat_SmallAnnouncementLeft -= delta) <= 0) {
		Elem_Free(&s->smallAnnouncement);
		s->dirty = true;
	}
}

static void SpecialTextScreen_Render(void* screen, float delta) {
	if (Game_HideGui || Game_PureClassic) return;

	Gfx_3DS_SetRenderScreen(TOP_SCREEN);
	Screen_Render2Widgets(screen, delta);
	Gfx_3DS_SetRenderScreen(BOTTOM_SCREEN);
}

static void SpecialTextScreen_Init(void* screen) {
	struct SpecialTextScreen* s = (struct SpecialTextScreen*)screen;

	s->widgets     = s->__widgets;
	s->numWidgets  = 0;
	s->maxWidgets  = Array_Elems(s->__widgets);

	TextGroupWidget_Add(s, &s->status, CHAT_MAX_STATUS,
							s->statusTextures, SpecialTextScreen_GetStatus);
	TextGroupWidget_Add(s, &s->bottomRight, CHAT_MAX_BOTTOMRIGHT, 
							s->bottomRightTextures, SpecialTextScreen_GetBottomRight);

	TextWidget_Add(s, &s->announcement);
	TextWidget_Add(s, &s->bigAnnouncement);
	TextWidget_Add(s, &s->smallAnnouncement);

	Event_Register_(&ChatEvents.ChatReceived,   s, SpecialTextScreen_ChatReceived);
	Event_Register_(&ChatEvents.ColCodeChanged, s, SpecialTextScreen_ColCodeChanged);

	s->maxVertices = Screen_CalcDefaultMaxVertices(s);

	s->status.collapsible[0] = true; /* Texture pack downloading status */
	s->status.collapsible[1] = true; /* Reduced performance mode status */
	
	/* For dual screen builds, chat is still rendered on the main game screen */
	s->status.flags       |= WIDGET_FLAG_MAINSCREEN;
	s->bottomRight.flags  |= WIDGET_FLAG_MAINSCREEN;

	s->bottomRight.flags       |= WIDGET_FLAG_MAINSCREEN;
	s->announcement.flags      |= WIDGET_FLAG_MAINSCREEN;
	s->bigAnnouncement.flags   |= WIDGET_FLAG_MAINSCREEN;
	s->smallAnnouncement.flags |= WIDGET_FLAG_MAINSCREEN;
}

static void SpecialTextScreen_Free(void* screen) {
	struct SpecialTextScreen* s = (struct SpecialTextScreen*)screen;
	Event_Unregister_(&ChatEvents.ChatReceived,   s, SpecialTextScreen_ChatReceived);
	Event_Unregister_(&ChatEvents.ColCodeChanged, s, SpecialTextScreen_ColCodeChanged);
}

static const struct ScreenVTABLE SpecialTextScreen_VTABLE = {
	SpecialTextScreen_Init,        SpecialTextScreen_Update, SpecialTextScreen_Free,
	SpecialTextScreen_Render,      Screen_BuildMesh,
	Screen_FInput,           Screen_InputUp,    Screen_FKeyPress,   Screen_FText,
	Screen_FPointer,         Screen_PointerUp,  Screen_FPointer,    Screen_FMouseScroll,
	SpecialTextScreen_Layout, SpecialTextScreen_ContextLost, SpecialTextScreen_ContextRecreated
};
void SpecialTextScreen_Show(void) {
	struct SpecialTextScreen* s  = &SpecialTextScreen_Instance;
	s->lastDownloadStatus = HTTP_PROGRESS_NOT_WORKING_ON;

	s->VTABLE = &SpecialTextScreen_VTABLE;
	Gui_Add((struct Screen*)s, GUI_PRIORITY_SPECIALTEXT);
}
#else
void SpecialTextScreen_Show(void) { }
#endif


/*########################################################################################################################*
*-----------------------------------------------------InventoryScreen-----------------------------------------------------*
*#########################################################################################################################*/
static struct InventoryScreen {
	Screen_Body
	struct FontDesc font;
	struct TableWidget table;
	struct TextWidget title;
	cc_bool releasedInv, deferredSelect;
	struct Widget* __widgets[2];
} InventoryScreen CC_BIG_VAR;


static void InventoryScreen_GetTitleText(cc_string* desc, BlockID block) {
	cc_string name;
	int block_ = block;
	if (Game_PureClassic) { String_AppendConst(desc, "Select block"); return; }
	if (block == BLOCK_AIR) return;

	name = Block_UNSAFE_GetName(block);
	String_AppendString(desc, &name);
	if (Game_ClassicMode) return;

	String_Format1(desc, " (ID %i&f", &block_);
	if (!Blocks.CanPlace[block])  { String_AppendConst(desc,  ", place &cNo&f"); }
	if (!Blocks.CanDelete[block]) { String_AppendConst(desc, ", delete &cNo&f"); }
	String_Append(desc, ')');
}

static void InventoryScreen_UpdateTitle(struct InventoryScreen* s, BlockID block) {
	cc_string desc; char descBuffer[STRING_SIZE * 2];

	String_InitArray(desc, descBuffer);
	InventoryScreen_GetTitleText(&desc, block);
	TextWidget_Set(&s->title, &desc, &s->font);
	s->dirty = true;
}

static void InventoryScreen_OnUpdateTitle(BlockID block) {
	InventoryScreen_UpdateTitle(&InventoryScreen, block);
}


static void InventoryScreen_OnBlockChanged(void* screen) {
	struct InventoryScreen* s = (struct InventoryScreen*)screen;
	TableWidget_OnInventoryChanged(&s->table);
}

static void InventoryScreen_NeedRedrawing(void* screen) {
	struct InventoryScreen* s = (struct InventoryScreen*)screen;
	s->dirty = true;
}

static void InventoryScreen_ContextLost(void* screen) {
	struct InventoryScreen* s = (struct InventoryScreen*)screen;
	Font_Free(&s->font);
	Screen_ContextLost(s);
	s->table.vb = 0;
}

static void InventoryScreen_ContextRecreated(void* screen) {
	struct InventoryScreen* s = (struct InventoryScreen*)screen;
	Screen_UpdateVb(s);
	s->table.vb = s->vb;

	Gui_MakeBodyFont(&s->font);
	TableWidget_RecreateTitle(&s->table, true);
}

static void InventoryScreen_MoveToSelected(struct InventoryScreen* s) {
	struct TableWidget* table = &s->table;
	int blockForTitle;
	s->deferredSelect = false;

	if (Game_ClassicMode) {
		/* Accuracy: Original classic preserves selected block across inventory menu opens */
		TableWidget_SetToIndex(table, table->selectedIndex);
		TableWidget_RecreateTitle(table, true);
	} else {
		blockForTitle = -1;
		TableWidget_SetToBlock(table, Inventory_SelectedBlock, false);
		/* When using auto rotate, if the held block is hidden, try to find another one in its autorotate group */
		if (AutoRotate_Enabled && table->selectedIndex == -1) {
			TableWidget_SetToBlock(table, Inventory_SelectedBlock, true);
			/* We still need to be able to see the name and ID of the held block */
			/* rather than the one that the cursor snapped to */
			blockForTitle = Inventory_SelectedBlock;
		}

		if (table->selectedIndex == -1) {
			/* Hidden block in inventory - display title for it still */
			InventoryScreen_OnUpdateTitle(Inventory_SelectedBlock);
		} else {
			TableWidget_RecreateTitleForBlock(table, true, blockForTitle);
		}
	}
}

static void InventoryScreen_Init(void* screen) {
	struct InventoryScreen* s = (struct InventoryScreen*)screen;
	s->widgets     = s->__widgets;
	s->numWidgets  = 0;
	s->maxWidgets  = Array_Elems(s->__widgets);
	
	TextWidget_Add(s,  &s->title);
	TableWidget_Add(s, &s->table, 22 * Options_GetFloat(OPT_INV_SCROLLBAR_SCALE, 0, 10, 1));
	s->table.blocksPerRow = Inventory.BlocksPerRow;
	s->table.UpdateTitle   = InventoryScreen_OnUpdateTitle;
	TableWidget_RecreateBlocks(&s->table);

	/* Can't immediately move to selected here, because cursor grabbed  */
	/*  status might be toggled *after* InventoryScreen_Init() is called */
	/* That causes the cursor to be moved back to the middle of the window */
	s->deferredSelect = true;

	Event_Register_(&TextureEvents.AtlasChanged,     s, InventoryScreen_NeedRedrawing);
	Event_Register_(&BlockEvents.PermissionsChanged, s, InventoryScreen_OnBlockChanged);
	Event_Register_(&BlockEvents.BlockDefChanged,    s, InventoryScreen_OnBlockChanged);

	s->maxVertices = Screen_CalcDefaultMaxVertices(s);
}

static void InventoryScreen_Free(void* screen) {
	struct InventoryScreen* s = (struct InventoryScreen*)screen;

	Event_Unregister_(&TextureEvents.AtlasChanged,     s, InventoryScreen_NeedRedrawing);
	Event_Unregister_(&BlockEvents.PermissionsChanged, s, InventoryScreen_OnBlockChanged);
	Event_Unregister_(&BlockEvents.BlockDefChanged,    s, InventoryScreen_OnBlockChanged);
}

static void InventoryScreen_Update(void* screen, float delta) {
	struct InventoryScreen* s = (struct InventoryScreen*)screen;
	if (s->deferredSelect) InventoryScreen_MoveToSelected(s);
}

static void InventoryScreen_Render(void* screen, float delta) {
	struct InventoryScreen* s = (struct InventoryScreen*)screen;
	Widget_Render2(&s->table, TEXTWIDGET_MAX);
	Widget_Render2(&s->title,              0);
}

static void InventoryScreen_Layout(void* screen) {
	struct InventoryScreen* s = (struct InventoryScreen*)screen;
	s->table.scale = Gui_GetInventoryScale();
	Widget_SetLocation(&s->table, ANCHOR_CENTRE, ANCHOR_CENTRE, 0, 0);

	Widget_SetLocation(&s->title, ANCHOR_CENTRE, ANCHOR_MIN, 0, 0);
	/* use Table(Y) directly instead of s->title->height ??? */
	s->title.yOffset = s->table.y - s->title.height - 3;
	Widget_Layout(&s->title); /* Needed for yOffset */
}

static int InventoryScreen_KeyDown(void* screen, int key, struct InputDevice* device) {
	struct InventoryScreen* s = (struct InventoryScreen*)screen;
	struct TableWidget* table = &s->table;

	/* Accuracy: Original classic doesn't close inventory menu when B is pressed */
	if (InputBind_Claims(BIND_INVENTORY, key, device) && s->releasedInv && !Game_ClassicMode) {
		Gui_Remove((struct Screen*)s);
		CPE_SendNotifyAction(NOTIFY_ACTION_BLOCK_LIST_TOGGLED, 0);
	} else if (InputDevice_IsEnter(key, device) && table->selectedIndex != -1) {
		Inventory_SetSelectedBlock(table->blocks[table->selectedIndex]);
		Gui_Remove((struct Screen*)s);
		CPE_SendNotifyAction(NOTIFY_ACTION_BLOCK_LIST_TOGGLED, 0);
	} else if (Elem_HandlesKeyDown(table, key, device)) {
	} else {
		return Elem_HandlesKeyDown(&HUDScreen_Instance.hotbar, key, device);
	}
	return true;
}

static cc_bool InventoryScreen_IsHotbarActive(void) {
	struct Screen* grabbed = Gui.InputGrab;
	/* Only toggle hotbar when inventory or no grab screen is open */
	return !grabbed || grabbed == (struct Screen*)&InventoryScreen;
}

static void InventoryScreen_KeyUp(void* screen, int key, struct InputDevice* device) {
	struct InventoryScreen* s = (struct InventoryScreen*)screen;
	if (InputBind_Claims(BIND_INVENTORY, key, device)) s->releasedInv = true;
}

static int InventoryScreen_PointerDown(void* screen, int id, int x, int y) {
	struct InventoryScreen* s = (struct InventoryScreen*)screen;
	struct TableWidget* table = &s->table;
	cc_bool handled, hotbar;

	if (table->scroll.draggingId == id) return TOUCH_TYPE_GUI;
	if (HUDscreen_PointerDown(Gui_HUD, id, x, y)) return TOUCH_TYPE_GUI;
	handled = Elem_HandlesPointerDown(table, id, x, y);

	if (!handled || table->pendingClose) {
		hotbar = Input_IsCtrlPressed() || Input_IsShiftPressed();
		if (!hotbar) {
			Gui_Remove((struct Screen*)s);
			CPE_SendNotifyAction(NOTIFY_ACTION_BLOCK_LIST_TOGGLED, 0);
		}
	}
	return TOUCH_TYPE_GUI;
}

static void InventoryScreen_PointerUp(void* screen, int id, int x, int y) {
	struct InventoryScreen* s = (struct InventoryScreen*)screen;
	Elem_OnPointerUp(&s->table, id, x, y);
}

static int InventoryScreen_PointerMove(void* screen, int id, int x, int y) {
	struct InventoryScreen* s = (struct InventoryScreen*)screen;
	return Elem_HandlesPointerMove(&s->table, id, x, y);
}

static int InventoryScreen_MouseScroll(void* screen, float delta) {
	struct InventoryScreen* s = (struct InventoryScreen*)screen;

	cc_bool hotbar = Input_IsAltPressed() || Input_IsCtrlPressed() || Input_IsShiftPressed();
	if (hotbar) return false;
	return Elem_HandlesMouseScroll(&s->table, delta);
}

static int InventoryScreen_PadAxis(void* screen, struct PadAxisUpdate* upd) {
	struct InventoryScreen* s = (struct InventoryScreen*)screen;

	return Elem_HandlesPadAxis(&s->table, upd);
}

static const struct ScreenVTABLE InventoryScreen_VTABLE = {
	InventoryScreen_Init,        InventoryScreen_Update,    InventoryScreen_Free,
	InventoryScreen_Render,      Screen_BuildMesh,
	InventoryScreen_KeyDown,     InventoryScreen_KeyUp,     Screen_TKeyPress,            Screen_TText,
	InventoryScreen_PointerDown, InventoryScreen_PointerUp, InventoryScreen_PointerMove, InventoryScreen_MouseScroll,
	InventoryScreen_Layout,  InventoryScreen_ContextLost, InventoryScreen_ContextRecreated,
	InventoryScreen_PadAxis
};
void InventoryScreen_Show(void) {
	struct InventoryScreen* s = &InventoryScreen;
	s->grabsInput = true;
	s->closable   = true;

	s->VTABLE = &InventoryScreen_VTABLE;
	Gui_Add((struct Screen*)s, GUI_PRIORITY_INVENTORY);
	CPE_SendNotifyAction(NOTIFY_ACTION_BLOCK_LIST_TOGGLED, 1);
}

void InventoryScreen_Hide(void) {
	struct InventoryScreen* s = &InventoryScreen;
	Gui_Remove((struct Screen*)s);
	CPE_SendNotifyAction(NOTIFY_ACTION_BLOCK_LIST_TOGGLED, 0);
}


/*########################################################################################################################*
*---------------------------------------------------SurvivalInvScreen----------------------------------------------------*
*#########################################################################################################################*/
/* Indev-style inventory screen for Classic 0.30 Survival Test. NOT faithful to */
/* c0.30-s (which never had this screen at all) - this is a visual-only redesign */
/* requested on top of the existing survival inventory, replacing the previous   */
/* flat-grid look with a classic light-grey panel, recessed slot bevels, and a   */
/* 3D player-skin paperdoll that turns to face the mouse cursor.                 */
/*                                                                                */
/* Only the 27 storage slots (9-35) are shown/clickable here; the hotbar (0-8)   */
/* is intentionally left to the normal in-world HUD hotbar, which already        */
/* renders underneath this screen (matching the reference screenshot, where the  */
/* hotbar sits outside/below the grey panel in the regular HUD style).           */

/* Base slot size (pixels) before display scaling. */
#define SURVINV_SLOT_BASE     36
/* Number of storage rows/columns shown in the panel. */
#define SURVINV_STORAGE_ROWS  3
#define SURVINV_STORAGE_COLS  SURVIVAL_HOTBAR_SLOTS
#define SURVINV_STORAGE_SLOTS (SURVIVAL_INV_SLOTS - SURVIVAL_HOTBAR_SLOTS)
/* Paperdoll preview box size, in slot units (square). */
#define SURVINV_DOLL_UNITS    3
/* Pixel gap (base, before scaling) between the doll box and the storage grid. */
#define SURVINV_GAP_BASE      10
/* Pixel padding (base, before scaling) around the panel's inner content. */
#define SURVINV_PAD_BASE      8

/* Displayed block-picture slots: storage + hotbar row + craft grid + open */
/*  container (chest 27/furnace 3) + result + cursor. */
#define SURVINV_ISO_SLOTS      (SURVINV_STORAGE_SLOTS + SURVIVAL_HOTBAR_SLOTS + SURVIVAL_CRAFT_SLOTS + SURVIVAL_CONTAINER_SLOTS + SURVIVAL_ARMOR_SLOTS + 2)
#define SURVINV_MAX_ISO_VERTS  (SURVINV_ISO_SLOTS * ISOMETRICDRAWER_MAXVERTICES)
/* Two digits at most per slot, four vertices per digit. */
#define SURVINV_MAX_COUNT_VERTS (SURVINV_ISO_SLOTS * 2 * 4)
#define SURVINV_TOTAL_VERTS     (SURVINV_MAX_ISO_VERTS + SURVINV_MAX_COUNT_VERTS)
/* Click sentinel: the crafting result "slot" (taking from it crafts). */
#define SURVINV_RESULT_HIT     1000

/* Field of view and camera distance used for the paperdoll preview's own */
/*  perspective projection (a tighter, portrait-style FOV than the gameplay */
/*  camera, since it's a close-up of just the player model). */
#define SURVINV_DOLL_FOV  30.0f
#define SURVINV_DOLL_DIST  4.7f /* farther back so the full body fits the box with margin */
/* Widens the doll's horizontal FOV relative to vertical, so the body sits */
/*  with comfortable side margin instead of its shoulders touching the box */
/*  edges (matching the reference Indev/Beta paperdoll's proportions). */
#define SURVINV_DOLL_ASPECT 1.2f

static struct SurvivalInvScreen {
	Screen_Body
	int  isoState[SURVINV_MAX_ISO_VERTS / 4];
	int  isoVertCount;
	int  isoSlotVerts;    /* iso verts belonging to SLOTS - the batch tail past
	                          this is the cursor-held block, drawn later so the
	                          hover highlight sits between them (genuine order) */
	int  countSlotVerts;  /* likewise for the count digits mesh */
	int  heldSlot;        /* index of the "picked-up" slot, or -1 */
	int  lastInvVersion;
	int  gridX, gridY;     /* pixel origin of the top-left storage slot */
	int  hotY;             /* pixel y of the in-screen hotbar row (GuiInventory style) */
	float texF;            /* Indev: pixels per texture unit of the 176x166 gui panel */
	int  dollBoxH;         /* doll viewport height (== dollBoxSize except Indev's tall window) */
	float dollCamPitch;    /* GUI paperdoll: whole-scene vertical camera tilt */
	int  craftX, craftY;   /* pixel origin of the top-left 2x2 crafting cell */
	int  resultX, resultY; /* pixel origin of the crafting result slot */
	int  slotSize;         /* current pixel size per slot */
	int  panelX, panelY, panelW, panelH;
	int  dollBoxX, dollBoxY, dollBoxSize;
	int  mouseX, mouseY;   /* last known pointer position, or -1 if none yet */
	int  countVertCount;
	struct FontDesc  font;
	struct TextAtlas countAtlas;
	struct Texture   titleTex;
	/* Genuine GuiContainer foreground labels (0x404040 dark gray text): */
	/*  "Chest"/"Furnace"/"Crafting" panel titles + "Inventory" section label */
	struct Texture   lblChest, lblFurnace, lblCrafting, lblInventory;
	float fontTexF;  /* panel scale the label/count textures were built for */
	struct Entity    doll;
} SurvivalInvScreen_Instance CC_BIG_VAR;

/* Returns the pixel origin (top-left corner) of an inventory slot: hotbar */
/*  slots (0-8) sit on their own row below the storage grid, GuiInventory */
/*  style, so stacks can be moved between hotbar and storage/crafting. */
static void SurvivalInv_SlotXY(struct SurvivalInvScreen* s, int slot, int* ox, int* oy) {
	int st, col, row;
	if (slot < SURVIVAL_HOTBAR_SLOTS) {
		*ox = s->gridX + slot * s->slotSize;
		*oy = s->hotY;
		return;
	}
	st  = slot - SURVIVAL_HOTBAR_SLOTS;
	col = st % SURVINV_STORAGE_COLS;
	row = st / SURVINV_STORAGE_COLS;
	*ox = s->gridX + col * s->slotSize;
	*oy = s->gridY + row * s->slotSize;
}

/* Pixel origin of crafting grid cell i (0..3), laid out 2 wide x 2 tall. */
static void SurvivalInv_CraftXY(struct SurvivalInvScreen* s, int i, int* ox, int* oy) {
	int dim = SurvivalTest_CraftDim();
	*ox = s->craftX + (i % dim) * s->slotSize;
	*oy = s->craftY + (i / dim) * s->slotSize;
}

static cc_bool SurvivalInv_InSlot(int mx, int my, int x, int y, int size) {
	/* Slot.isAtCursorPos: [xPos-1, xPos+17) - one GUI px around the 16px
	    item area, i.e. shifted 1 left/up of the 18px cell */
	int one = size / 18;
	return mx >= x - one && mx < x + size - one &&
	       my >= y - one && my < y + size - one;
}

/* Active craft cells: dim*dim (pocket 4, workbench 9); none outside Indev, */
/*  and none while a container is open (chest/furnace GUIs have no grid). */
static int SurvivalInv_CraftCells(void) {
	int dim;
	if (!IndevTest_Enabled) return 0;
	if (IndevTest_OpenKind() != INDEV_CONTAINER_NONE) return 0;
	dim = SurvivalTest_CraftDim();
	return dim * dim;
}

/* Slots of the open container: chest 27, furnace 3 (input/fuel/output). */
static int SurvivalInv_ContainerCells(void) {
	switch (IndevTest_OpenKind()) {
	case INDEV_CONTAINER_CHEST:   return SURVIVAL_CONTAINER_SLOTS;
	case INDEV_CONTAINER_FURNACE: return 3;
	default: return 0;
	}
}

/* Pixel origin of open-container cell i. Genuine layouts: GuiChest grid at */
/*  (8,18) 9 wide; GuiFurnace input (56,17), fuel (56,53), output (116,35). */
static void SurvivalInv_ContainerSlotXY(struct SurvivalInvScreen* s, int i, int* ox, int* oy) {
	float f = s->texF;
	if (IndevTest_OpenKind() == INDEV_CONTAINER_FURNACE) {
		switch (i) {
		case 0:  *ox = s->panelX + (int)( 56 * f); *oy = s->panelY + (int)(17 * f); return;
		case 1:  *ox = s->panelX + (int)( 56 * f); *oy = s->panelY + (int)(53 * f); return;
		default: *ox = s->panelX + (int)(116 * f); *oy = s->panelY + (int)(35 * f); return;
		}
	}
	*ox = s->panelX + (int)((8  + (i % 9) * 18) * f);
	*oy = s->panelY + (int)((18 + (i / 9) * 18) * f);
}

/* Armor slots exist on the pocket inventory only (genuine GuiInventory) - */
/*  the workbench/chest/furnace screens have none. */
static int SurvivalInv_ArmorCells(void) {
	if (!IndevTest_Enabled)                           return 0;
	if (IndevTest_OpenKind() != INDEV_CONTAINER_NONE) return 0;
	if (SurvivalTest_CraftDim() != 2)                 return 0;
	return SURVIVAL_ARMOR_SLOTS;
}

/* Genuine GuiInventory: x=8, y=8+row*18 with the HELMET on top. The armor */
/*  array is [0] boots .. [3] helmet, so screen row = 3 - array index. */
static void SurvivalInv_ArmorSlotXY(struct SurvivalInvScreen* s, int i, int* ox, int* oy) {
	float f = s->texF;
	*ox = s->panelX + (int)(8 * f);
	*oy = s->panelY + (int)((8 + (3 - i) * 18) * f);
}

/* Slot under (mx,my): storage 9..35, craft 36.., container 45.., the result */
/*  sentinel, or -1. */
static int SurvivalInv_HitSlot(struct SurvivalInvScreen* s, int mx, int my) {
	int i, x, y, craft = SurvivalInv_CraftCells();
	for (i = 0; i < SURVIVAL_INV_SLOTS; i++) {
		SurvivalInv_SlotXY(s, i, &x, &y);
		if (SurvivalInv_InSlot(mx, my, x, y, s->slotSize)) return i;
	}
	if (!IndevTest_Enabled) return -1; /* no crafting slots in plain c0.30-s */
	for (i = 0; i < craft; i++) {
		SurvivalInv_CraftXY(s, i, &x, &y);
		if (SurvivalInv_InSlot(mx, my, x, y, s->slotSize)) return SURVIVAL_CRAFT_BASE + i;
	}
	for (i = 0; i < SurvivalInv_ContainerCells(); i++) {
		SurvivalInv_ContainerSlotXY(s, i, &x, &y);
		if (SurvivalInv_InSlot(mx, my, x, y, s->slotSize)) return SURVIVAL_CONTAINER_BASE + i;
	}
	for (i = 0; i < SurvivalInv_ArmorCells(); i++) {
		SurvivalInv_ArmorSlotXY(s, i, &x, &y);
		if (SurvivalInv_InSlot(mx, my, x, y, s->slotSize)) return SURVIVAL_ARMOR_BASE + i;
	}
	if (craft > 0 &&
		SurvivalInv_InSlot(mx, my, s->resultX, s->resultY, s->slotSize)) return SURVINV_RESULT_HIT;
	return -1;
}

/* Raw id + count for any displayed slot index (storage/craft/container/result). */
static void SurvivalInv_SlotContent(int slot, int* id, int* count) {
	if (slot == SURVINV_RESULT_HIT) {
		*id = SurvivalTest_CraftResult(count);
	} else if (slot >= SURVIVAL_ARMOR_BASE) {
		*id    = SurvivalTest_ArmorId(slot - SURVIVAL_ARMOR_BASE);
		*count = SurvivalTest_ArmorCount(slot - SURVIVAL_ARMOR_BASE);
	} else if (slot >= SURVIVAL_CONTAINER_BASE) {
		struct SurvivalSlot* p = IndevTest_ContainerSlot(slot - SURVIVAL_CONTAINER_BASE);
		*id    = p->id;
		*count = p->count;
	} else if (slot >= SURVIVAL_CRAFT_BASE) {
		*id    = SurvivalTest_CraftSlotId(slot - SURVIVAL_CRAFT_BASE);
		*count = SurvivalTest_CraftSlotCount(slot - SURVIVAL_CRAFT_BASE);
	} else {
		*id    = SurvivalTest_SlotId(slot);
		*count = SurvivalTest_SlotCount(slot);
	}
}

/* Pixel origin of any displayed slot index (storage/craft/container/result). */
static void SurvivalInv_AnySlotXY(struct SurvivalInvScreen* s, int slot, int* x, int* y) {
	if (slot == SURVINV_RESULT_HIT)            { *x = s->resultX; *y = s->resultY; }
	else if (slot >= SURVIVAL_ARMOR_BASE)      SurvivalInv_ArmorSlotXY(s, slot - SURVIVAL_ARMOR_BASE, x, y);
	else if (slot >= SURVIVAL_CONTAINER_BASE)  SurvivalInv_ContainerSlotXY(s, slot - SURVIVAL_CONTAINER_BASE, x, y);
	else if (slot >= SURVIVAL_CRAFT_BASE)      SurvivalInv_CraftXY(s, slot - SURVIVAL_CRAFT_BASE, x, y);
	else                                       SurvivalInv_SlotXY(s, slot, x, y);
}

/* Total displayed picture slots and a mapping from 0..N-1 to slot indices */
/*  (storage, then the 4 craft cells, then the result sentinel). */
#define SURVINV_DISPLAY_SLOTS (SURVINV_STORAGE_SLOTS + SURVIVAL_CRAFT_SLOTS + 1)
/* Crafting slots only exist in Indev mode; plain c0.30-s shows storage only */
/*  (it never had crafting), so its screen is byte-for-byte the old layout. */
static int SurvivalInv_DisplayCount(void) {
	/* hotbar row + storage, plus (Indev) either the open container's slots */
	/*  or the active craft cells + result slot */
	int n = SURVIVAL_HOTBAR_SLOTS + SURVINV_STORAGE_SLOTS;
	if (!IndevTest_Enabled) return n;
	if (SurvivalInv_ContainerCells()) return n + SurvivalInv_ContainerCells();
	return n + SurvivalInv_CraftCells() + 1 + SurvivalInv_ArmorCells();
}
static int SurvivalInv_DisplaySlot(int n) {
	int cells = SurvivalInv_CraftCells(), cont = SurvivalInv_ContainerCells();
	if (n < SURVIVAL_HOTBAR_SLOTS) return n; /* hotbar row first */
	n -= SURVIVAL_HOTBAR_SLOTS;
	if (n < SURVINV_STORAGE_SLOTS)         return SURVIVAL_HOTBAR_SLOTS + n;
	n -= SURVINV_STORAGE_SLOTS;
	if (cont)                              return SURVIVAL_CONTAINER_BASE + n;
	if (n < cells)                         return SURVIVAL_CRAFT_BASE + n;
	n -= cells;
	if (n == 0)                            return SURVINV_RESULT_HIT;
	return SURVIVAL_ARMOR_BASE + (n - 1);
}

/* Rendered pixel width of a stack count - summed from the atlas' per-glyph */
/*  widths (atlas->offset is the PREFIX width, not a digit advance). */
static int SurvivalInv_CountWidth(struct TextAtlas* atlas, int value) {
	int w = 0;
	if (value <= 0) return 0;
	for (; value > 0; value /= 10) w += atlas->widths[value % 10];
	return w;
}

/* Tool damage of any displayed slot (0 when not a damageable/damaged item). */
static void SurvivalInv_SlotDamage(int slot, int* dmg, int* maxDmg) {
	int id = 0, count = 0;
	*dmg = 0; *maxDmg = 0;
	if (slot == SURVINV_RESULT_HIT) return;

	if (slot >= SURVIVAL_ARMOR_BASE) {
		id     = SurvivalTest_ArmorId(slot - SURVIVAL_ARMOR_BASE);
		count  = SurvivalTest_ArmorCount(slot - SURVIVAL_ARMOR_BASE);
		*dmg   = SurvivalTest_ArmorDamage(slot - SURVIVAL_ARMOR_BASE);
	} else if (slot >= SURVIVAL_CONTAINER_BASE) {
		struct SurvivalSlot* p = IndevTest_ContainerSlot(slot - SURVIVAL_CONTAINER_BASE);
		id = p->id; count = p->count; *dmg = p->damage;
	} else if (slot >= SURVIVAL_CRAFT_BASE) {
		/* GuiContainer renders overlays for EVERY slot - grid cells too */
		id     = SurvivalTest_CraftSlotId(slot - SURVIVAL_CRAFT_BASE);
		count  = SurvivalTest_CraftSlotCount(slot - SURVIVAL_CRAFT_BASE);
		*dmg   = SurvivalTest_CraftSlotDamage(slot - SURVIVAL_CRAFT_BASE);
	} else {
		id = SurvivalTest_SlotId(slot); count = SurvivalTest_SlotCount(slot);
		*dmg = SurvivalTest_SlotDamage(slot);
	}
	if (count <= 0) { *dmg = 0; return; }
	*maxDmg = IndevTest_ToolMaxDamage(id);
	if (!*maxDmg) *maxDmg = IndevTest_ArmorMaxDamage(id);
}

/* The paperdoll is a static, unlit preview - just needs a constant base color. */
static PackedCol SurvivalInvDoll_GetCol(struct Entity* e) { return PACKEDCOL_WHITE; }
static const struct EntityVTABLE survivalDoll_VTABLE = {
	NULL, NULL, NULL, SurvivalInvDoll_GetCol, NULL, NULL
};

static void SurvivalInv_InitDoll(struct SurvivalInvScreen* s) {
	static const cc_string human = String_FromConst("humanoid");
	Entity_Init(&s->doll);
	s->doll.VTABLE = &survivalDoll_VTABLE;
	/* Guarantee the model + Size are set (Entity_Init calls SetModel, but be */
	/*  explicit so Position.y framing below never reads a zero Size). */
	Entity_SetModel(&s->doll, &human);
}

/* D3D9/D3D11 run a REVERSED depth buffer (ZFUNC GREATEREQUAL vs GL's
    LEQUAL) with the same ortho z row, so with culling off the depth test
    picks the model's FAR side there. Negating the view's Z axis makes the
    reversed test select exactly the fragments GL's test selects (screen
    x/y untouched). The scene camera-tilt rotation is applied AFTER that
    flip, so its visual sense inverts with it - DOLL_Z doubles as the
    tilt's sign (user-verified per backend: D3D leaned backwards with the
    unsigned tilt). */
#if CC_GFX_BACKEND == CC_GFX_BACKEND_D3D9 || CC_GFX_BACKEND == CC_GFX_BACKEND_D3D11
	#define DOLL_Z -1.0f
#else
	#define DOLL_Z  1.0f
#endif

/* Renders the 3D player-skin paperdoll, confined to the doll preview box. */
/*  The body always faces forward; only the head turns to track the cursor. */
/* Indev mode: verified against GuiInventory.drawGuiContainerBackgroundLayer */
/*  (in-20100223) - anchor (guiLeft+51, guiTop+75) z=50, glScalef(-30,30,30) */
/*  + rotZ 180, dx/dy from (51, 25) with the atan(d/40)*20/40 tracking, */
/*  camera tilt glRotatef(-atan(dy/40)*20, 1,0,0). The classic-mode doll box */
/*  is our own invention (c0.30-s has no doll) and keeps its approximation. */
static void SurvivalInv_RenderDoll(struct SurvivalInvScreen* s) {
	struct Entity* p = &Entities.CurPlayer->Base;
	struct Matrix proj, savedView;
	float aspect;
	int boxX = s->dollBoxX, boxY = s->dollBoxY, boxSize = s->dollBoxSize;
	int boxH  = s->dollBoxH > 0 ? s->dollBoxH : s->dollBoxSize;
	if (boxSize <= 0) return;

	/* Use the player's skin when it has one; otherwise fall back to the */
	/*  default char.png. In singleplayer with no ClassiCube skin, the local */
	/*  player's TextureId can be 0 - forcing NonHumanSkin=false + TextureId=0 */
	/*  makes Model_ApplyTexture use the model's defaultTex (default Steve), */
	/*  never an unbound/black texture. */
	if (p->TextureId) {
		s->doll.SkinType     = p->SkinType;
		s->doll.TextureId    = p->TextureId;
		s->doll.NonHumanSkin = p->NonHumanSkin;
		s->doll.uScale       = p->uScale;
		s->doll.vScale       = p->vScale;
	} else {
		s->doll.SkinType     = SKIN_64x32;
		s->doll.TextureId    = 0;      /* -> model defaultTex (char.png) */
		s->doll.NonHumanSkin = false;
		s->doll.uScale       = 1.0f;
		s->doll.vScale       = 1.0f;
	}

	/* The genuine GuiInventory tracking maths (in-20100223, transcribed):
	    dx/dy are measured in the genuine GUI's pixel units from the doll
	    anchor (window centre horizontally, eye level = 50px above the base
	    in a 70px-tall window), with a 40px arctan falloff. The BODY yaw
	    lightly tracks (atan(dx/40)*20), the HEAD tracks at double strength
	    (atan(dx/40)*40), and the whole body leans with the vertical offset
	    (atan(dy/40)*20) while the head pitches by the same amount on top.
	    Genuine multiplies raw radians by 20/40 and feeds glRotatef degrees -
	    quirky, but transcribed as-is. */
	{
		float gscale = 70.0f / (float)boxH; /* our box pixels -> genuine GUI px */
		float dx, dy, t, lean;
		if (s->mouseX < 0) {
			/* no PointerMove yet - look straight ahead */
			dx = 0.0f; dy = 0.0f;
		} else if (IndevTest_Enabled && s->texF > 0.0f) {
			/* genuine anchors: dx from (guiLeft+51), dy from (guiTop+25)
			    (= 75 - 50), both in genuine GUI px (mouse / texF) */
			dx = ((float)s->panelX + 51.0f * s->texF - (float)s->mouseX) / s->texF;
			dy = ((float)s->panelY + 25.0f * s->texF - (float)s->mouseY) / s->texF;
		} else {
			float eyeY = (float)boxY + (float)boxH * (20.0f / 70.0f);
			dx = ((float)(boxX + boxSize / 2) - (float)s->mouseX) * gscale;
			dy = (eyeY - (float)s->mouseY) * gscale;
		}
		t    = Math_Atan2f(40.0f, dx); /* engine Atan2f(x,y) = atan(y/x) */
		lean = Math_Atan2f(40.0f, dy);

		/* The view's x+y mirror below is a 180-degree spin about the Z
		    axis - which does NOT turn the model's face toward the camera -
		    so the face-the-camera base yaw is 180 (models face -z at yaw
		    0). WHICH depth direction is "toward the camera" differs per
		    backend (see DOLL_Z below), but the base yaw is the same for
		    all of them once the view z is normalised. */
		/* The view's x-mirror inverts the visual sense of model yaw on
		    EVERY backend (user-verified on both OGL and D3D), so the
		    genuine +t offsets are negated around the 180 base. */
		s->doll.RotY  = 180.0f - t * 20.0f;   /* body: light HORIZONTAL track */
		s->doll.Yaw   = 180.0f - t * 40.0f;   /* head: double strength */
		/* Rig-derived signs (the x+y view mirror inverts the MODEL-local
		    pitch sense but not the post-mirror scene rotation): head pitch
		    -lean, scene tilt +lean - both toward the cursor, head at
		    double visual strength like genuine (head rides on the scene
		    tilt). The old both-negative pair made the scene fight the
		    head - level head, body tilting AWAY (user-reported). */
		s->doll.Pitch = -lean * 20.0f;        /* head pitch (vertical) */
		s->doll.RotX  = 0.0f;                 /* body never pitches (genuine) */
		s->doll.RotZ  = 0.0f;
		s->dollCamPitch = DOLL_Z * lean * 20.0f; /* the vertical tilt is a CAMERA
		    rotation applied to the whole scene, not a body lean - genuine's
		    glRotatef(-atan(dy/40)*20, 1,0,0) before rendering the entity.
		    Our mirrored frame flips the pitch sense, so the sign here is
		    +lean (cursor below the eye anchor -> doll looks down at it -
		    user-reported inverted with the old -lean). */
	}
	s->doll.Position.x = 0.0f;
	s->doll.Position.y = 0.0f;
	s->doll.Position.z = 0.0f;

	/* Genuine renders the doll in the GUI's plain orthographic projection
	    (glTranslatef + glScalef 30, no perspective at all) - the previous
	    perspective camera here was what splayed the legs near the frame
	    edge. The -30/+30/+30 scale + 180-degree Z spin in genuine is an
	    x+y flip: it maps the y-up model into y-down GUI space while
	    preserving triangle winding (and mirrors the doll horizontally,
	    which is also genuine). Reproduced via the view matrix below. */
	if (IndevTest_Enabled && s->texF > 0.0f) {
		aspect = 30.0f * s->texF; /* genuine glScalef 30 in panel px */
	} else {
		aspect = 30.0f * (float)(boxH - 2) / 70.0f; /* our classic box approximation */
	}
	Gfx_CalcOrthoMatrix(&proj, (float)(boxSize - 2), (float)(boxH - 2), -1000.0f, 1000.0f);

	savedView = Gfx.View;
	{
		struct Matrix flip, place, camPitch, tmp;
		float px, py;
		/* x+y mirror (winding-preserving), then anchor at genuine's
		    (guiLeft+51, guiTop+75) - expressed relative to the viewport's
		    (boxX+1, boxY+1) origin in Indev mode - with z pushed into the
		    ortho range. The whole-scene pitch tilt (genuine's pre-render
		    glRotatef about X) rides on top, so vertical mouse movement
		    tips the CAMERA over the doll rather than bending the body. */
		if (IndevTest_Enabled && s->texF > 0.0f) {
			px = ((float)s->panelX + 51.0f * s->texF) - (float)(boxX + 1);
			py = ((float)s->panelY + 75.0f * s->texF) - (float)(boxY + 1);
		} else {
			px = (float)(boxSize - 2) * 0.5f;
			py = (float)(boxH - 2) * (67.0f / 70.0f);
		}
		Matrix_Scale(&flip, -aspect, -aspect, DOLL_Z * aspect);
		Matrix_RotateX(&camPitch, s->dollCamPitch * MATH_DEG2RAD);
		Matrix_Translate(&place, px, py, 50.0f);
		Matrix_Mul(&tmp, &flip, &camPitch);
		Matrix_Mul(&Gfx.View, &tmp, &place);
	}
	Gfx_LoadMatrix(MATRIX_VIEW, &Gfx.View);
	Gfx_LoadMatrix(MATRIX_PROJ, &proj);

	/* Matches the 1px-inset black square drawn in SurvivalInvScreen_Render, */
	/*  so the 3D render area never overflows into the box's border pixels. */
	/* Viewport and scissor both take top-left-origin window coordinates */
	/*  (they must describe the SAME region - see Graphics.h). */
	Gfx_SetViewport(boxX + 1, boxY + 1, boxSize - 2, boxH - 2);
	Gfx_SetScissor (boxX + 1, boxY + 1, boxSize - 2, boxH - 2);
	Gfx_ClearBuffers(GFX_BUFFER_DEPTH);

	/* Screens render in the 2D pass (depth off, alpha BLENDING on, culling */
	/*  off). A model needs the 3D baseline the world's entity pass provides: */
	/*  depth test+write and alpha TEST (not blend). Face culling stays OFF - */
	/*  the world draws models with culling off (Model.c only enables it for */
	/*  sprites), so model boxes have no winding guarantee: mirrored parts */
	/*  (corner-swapped bounds, e.g. armor limbs) wind backwards and culling */
	/*  eats their faces (= armor pieces invisible on the doll only). */
	Gfx_SetDepthTest(true);
	Gfx_SetDepthWrite(true);
	Gfx_SetAlphaTest(true);
	Gfx_SetAlphaBlending(false);

	Model_Render(s->doll.Model, &s->doll);
	/* the paperdoll wears the player's armor, like genuine GuiInventory */
	IndevArmor_Render(&s->doll);

	Gfx_SetAlphaBlending(true);
	Gfx_SetAlphaTest(false);
	Gfx_SetDepthWrite(false);
	Gfx_SetDepthTest(false);

	Gfx_SetViewport(0, 0, Game.Width, Game.Height);
	Gfx_SetScissor (0, 0, Game.Width, Game.Height);

	Gfx.View = savedView;
	/* Restore the 2D pass's matrices, NOT the world's (Gfx.Projection holds */
	/*  the 3D perspective matrix - loading that mid-GUI-pass makes every 2D */
	/*  quad drawn after the doll project through a perspective transform, */
	/*  garbling the rest of the frame's GUI). Rebuild the exact ortho + */
	/*  identity view that Gfx_Begin2D set up. */
	{
		struct Matrix ortho;
		Gfx_CalcOrthoMatrix(&ortho, (float)Game.Width, (float)Game.Height, -100.0f, 1000.0f);
		Gfx_LoadMatrix(MATRIX_PROJ, &ortho);
		Gfx_LoadMatrix(MATRIX_VIEW, &Matrix_Identity);
	}
}

static void SurvivalInvScreen_BuildMesh(void* screen) {
	struct SurvivalInvScreen* s = (struct SurvivalInvScreen*)screen;
	struct VertexTextured* data;
	struct VertexTextured* countDst;
	struct VertexTextured* cur;
	int i, slotX, slotY, count;
	BlockID block;
	float halfSize;

	data     = Screen_LockVb(s);
	halfSize = s->slotSize * 0.5f;
	/* Indev slots are the genuine texture's 18px cells with slotX at the item */
	/*  origin. Blocks fill the cell (same prominent size as the classic/HUD */
	/*  iso pictures - genuine renderBlockOnInventory blocks are big too), */
	/*  centred on the cell centre (item origin + 8 texture units). */
	{
	float itemHalf = IndevTest_Enabled ? s->texF * 7.0f : halfSize;
	int   ictr     = IndevTest_Enabled ? (int)(s->texF * 8.0f) : s->slotSize / 2;

	/* ISO block pictures for every occupied displayed slot that holds a BLOCK */
	/*  (item ids draw as flat sprites in the render pass instead). */
	IsometricDrawer_BeginBatch(data, s->isoState);
	for (i = 0; i < SurvivalInv_DisplayCount(); i++) {
		int slot = SurvivalInv_DisplaySlot(i), id;
		SurvivalInv_SlotContent(slot, &id, &count);
		if (id == BLOCK_AIR || count <= 0 || id >= 256) continue; /* items draw as sprites */
		SurvivalInv_AnySlotXY(s, slot, &slotX, &slotY);
		IsometricDrawer_AddBatch((BlockID)id, itemHalf,
			slotX + ictr, slotY + ictr);
	}
	/* EndBatch just measures - the batch continues below with the cursor block */
	s->isoSlotVerts = IsometricDrawer_EndBatch();
	/* Cursor-held BLOCK follows the mouse inside the same iso batch (held */
	/*  ITEMS draw as sprites in the render pass instead). */
	if (SurvivalTest_CursorCount() > 0 && SurvivalTest_CursorId() < 256 && s->mouseX >= 0) {
		IsometricDrawer_AddBatch((BlockID)SurvivalTest_CursorId(), itemHalf,
			s->mouseX, s->mouseY);
	}
	s->isoVertCount = IsometricDrawer_EndBatch();
	}

	/* Stack-count digit overlay for slots with count > 1 */
	countDst = data + SURVINV_MAX_ISO_VERTS;
	cur = countDst;
	s->countSlotVerts = 0;
	if (s->countAtlas.tex.ID) {
		int savedY = s->countAtlas.tex.y;
		for (i = 0; i < SurvivalInv_DisplayCount(); i++) {
			int slot = SurvivalInv_DisplaySlot(i), id;
			SurvivalInv_SlotContent(slot, &id, &count);
			if (count <= 1) continue;
			SurvivalInv_AnySlotXY(s, slot, &slotX, &slotY);
			/* Indev: count sits at the 16px item's bottom (slotY+16*f), not */
			/*  the 18px cell bottom; classic keeps its slotSize-relative spot. */
			if (IndevTest_Enabled) {
				/* renderItemOverlayIntoGUI right-aligns the count with its right */
				/*  edge at x+17 (drawString at x + 19 - 2 - stringWidth). NOTE: */
				/*  width must come from the atlas' per-glyph widths - offset is */
				/*  the PREFIX width (0 for the digits atlas), which used to make */
				/*  textW 0 and hang counts off the slot's right edge. */
				int textW = SurvivalInv_CountWidth(&s->countAtlas, count);
				s->countAtlas.tex.y = slotY + (int)(s->texF * 17.0f) - s->countAtlas.tex.height;
				s->countAtlas.curX  = slotX + (int)(s->texF * 17.0f) - textW;
			} else {
				s->countAtlas.tex.y = slotY + s->slotSize - s->countAtlas.tex.height - 2;
				s->countAtlas.curX  = slotX + 2;
			}
			TextAtlas_AddInt(&s->countAtlas, count, &cur);
		}
		s->countSlotVerts = (int)(cur - countDst);
		/* Cursor-held stack count follows the mouse (blocks and items alike). */
		if (SurvivalTest_CursorCount() > 1 && s->mouseX >= 0) {
			int cc    = SurvivalTest_CursorCount();
			/* renderItemIntoGUI draws at cursor-8, the count right-aligned to
			    +17 of that - so the count's right/bottom edge is cursor+9 */
			int half  = (int)(s->texF * 9.0f);
			s->countAtlas.tex.y = s->mouseY + half - s->countAtlas.tex.height;
			s->countAtlas.curX  = s->mouseX + half - SurvivalInv_CountWidth(&s->countAtlas, cc);
			TextAtlas_AddInt(&s->countAtlas, cc, &cur);
		}
		s->countAtlas.tex.y = savedY;
	}
	s->countVertCount = (int)(cur - countDst);
	s->lastInvVersion = SurvivalTest_InvVersion();
	(void)block;

	Gfx_UnlockDynamicVb(s->vb);
}

static void SurvivalInvScreen_Render(void* screen, float delta) {
	struct SurvivalInvScreen* s = (struct SurvivalInvScreen*)screen;
	int i, slotX, slotY, b;
	int contKind      = IndevTest_OpenKind();
	cc_bool workbench = IndevTest_Enabled && SurvivalTest_CraftDim() == 3;
	GfxResourceID guiTex;
	if (contKind == INDEV_CONTAINER_CHEST)        guiTex = IndevTest_ContGuiTex();
	else if (contKind == INDEV_CONTAINER_FURNACE) guiTex = IndevTest_FurnGuiTex();
	else guiTex = workbench ? IndevTest_CraftGuiTex() : IndevTest_InvGuiTex();

	/* GuiContainer.drawScreen begins with drawDefaultBackground(): a
	    full-screen gradient (0x60050500 -> 0xA0303060) dimming the world
	    behind every container screen. */
	Gfx_Draw2DGradient(0, 0, Game.Width, Game.Height,
		PackedCol_Make(0x05, 0x05, 0x00, 0x60),
		PackedCol_Make(0x30, 0x30, 0x60, 0xA0));

	/* Sampled from a reference classic-style inventory screenshot. */
	PackedCol panelBorder = PackedCol_Make( 55,  55,  55, 255);
	PackedCol panelBg     = PackedCol_Make(198, 198, 198, 255);
	PackedCol slotFill    = PackedCol_Make(139, 139, 139, 255);
	PackedCol heldFill    = PackedCol_Make(255, 255, 150, 220);
	PackedCol highlight   = PackedCol_Make(255, 255, 255, 255);
	PackedCol dollBg      = PackedCol_Make(  0,   0,   0, 255);

	{
	if (IndevTest_Enabled && guiTex) {
		/* Genuine look: the whole panel IS the 176-wide GUI texture (slot */
		/*  bevels, craft arrow, and - for the pocket inventory - the armor */
		/*  boxes and doll window). crafting.png for the workbench, */
		/*  container.png for a chest, furnace.png for a furnace, else */
		/*  inventory.png. */
		struct Texture panel;
		panel.ID     = guiTex;
		panel.x      = (short)s->panelX;
		panel.y      = (short)s->panelY;
		if (contKind == INDEV_CONTAINER_CHEST) {
			/* GuiChest composes the panel from two strips of container.png: */
			/*  rows strip (0,0)-(176, rows*18+17=71), then the player- */
			/*  inventory strip (0,126)-(176,222) below it. */
			int topH = (int)(71 * s->texF);
			panel.width  = (cc_uint16)s->panelW;
			panel.height = (cc_uint16)topH;
			panel.uv.u1  = 0.0f;            panel.uv.v1 = 0.0f;
			panel.uv.u2  = 176.0f / 256.0f; panel.uv.v2 = 71.0f / 256.0f;
			Texture_Render(&panel);
			panel.y      = (short)(s->panelY + topH);
			/* the strip is 96 texels drawn 96 units tall (GuiChest paints
			    71 + 96 = 167 of the 168-unit ySize; the last unit is bare) */
			panel.height = (cc_uint16)(int)(96 * s->texF);
			panel.uv.v1  = 126.0f / 256.0f; panel.uv.v2 = 222.0f / 256.0f;
			Texture_Render(&panel);
		} else {
			panel.width  = (cc_uint16)s->panelW;
			panel.height = (cc_uint16)s->panelH;
			panel.uv.u1  = 0.0f;            panel.uv.v1 = 0.0f;
			panel.uv.u2  = 176.0f / 256.0f; panel.uv.v2 = 166.0f / 256.0f;
			Texture_Render(&panel);
		}

		if (contKind == INDEV_CONTAINER_FURNACE) {
			/* GuiFurnace overlays. Flame while burning: dest (56, 36+12-h), */
			/*  src (176, 12-h) 14 x (h+2), h = burnTime*12/currentBurn. */
			/*  Progress arrow: dest (79,34), src (176,14) (w+1) x 16, */
			/*  w = cookTime*24/200. */
			struct Texture ovl;
			int h = IndevTest_FurnaceBurnScaled();
			int w = IndevTest_FurnaceCookScaled();
			ovl.ID = guiTex;
			/* GuiFurnace draws the flame whenever isBurning() - the h+2 rect
			    leaves a 2px ember stub visible right up until burnout */
			if (IndevTest_FurnaceIsBurning()) {
				ovl.x      = (short)(s->panelX + (int)(56 * s->texF));
				ovl.y      = (short)(s->panelY + (int)((36 + 12 - h) * s->texF));
				ovl.width  = (cc_uint16)(int)(14 * s->texF);
				ovl.height = (cc_uint16)(int)((h + 2) * s->texF);
				ovl.uv.u1  = 176.0f / 256.0f;      ovl.uv.v1 = (12.0f - h) / 256.0f;
				ovl.uv.u2  = 190.0f / 256.0f;      ovl.uv.v2 = 14.0f / 256.0f;
				Texture_Render(&ovl);
			}
			ovl.x      = (short)(s->panelX + (int)(79 * s->texF));
			ovl.y      = (short)(s->panelY + (int)(34 * s->texF));
			ovl.width  = (cc_uint16)(int)((w + 1) * s->texF);
			ovl.height = (cc_uint16)(int)(16 * s->texF);
			ovl.uv.u1  = 176.0f / 256.0f;              ovl.uv.v1 = 14.0f / 256.0f;
			ovl.uv.u2  = (176.0f + w + 1) / 256.0f;    ovl.uv.v2 = 30.0f / 256.0f;
			Texture_Render(&ovl);
		}

		/* Foreground labels + the hover highlight moved to the tail of this
		    function - genuine GuiContainer.drawScreen draws the highlight
		    per-slot AFTER that slot's item, and the labels LAST of all
		    (drawGuiContainerForegroundLayer, above even the held stack). */
		Gfx_SetVertexFormat(VERTEX_FORMAT_TEXTURED);
	} else {
	/* Panel: outer dark border then light-grey fill */
	Gfx_Draw2DFlat(s->panelX - 2, s->panelY - 2, s->panelW + 4, s->panelH + 4, panelBorder);
	Gfx_Draw2DFlat(s->panelX,     s->panelY,     s->panelW,     s->panelH,     panelBg);

	/* Paperdoll preview box: recessed dark square (not in container GUIs) */
	if (contKind == INDEV_CONTAINER_NONE) {
		b = s->dollBoxSize;
		Gfx_Draw2DFlat(s->dollBoxX,     s->dollBoxY,     b,     b,     panelBorder);
		Gfx_Draw2DFlat(s->dollBoxX + 1, s->dollBoxY + 1, b - 2, b - 2, dollBg);
	}

	/* Slot backgrounds (storage + craft grid + result): recessed bevel */
	for (i = 0; i < SurvivalInv_DisplayCount(); i++) {
		int slot = SurvivalInv_DisplaySlot(i);
		SurvivalInv_AnySlotXY(s, slot, &slotX, &slotY);
		Gfx_Draw2DFlat(slotX,     slotY,     s->slotSize,     s->slotSize,
		               slot == s->heldSlot ? heldFill : panelBorder);
		if (slot == s->heldSlot) continue;

		Gfx_Draw2DFlat(slotX + 1, slotY + 1, s->slotSize - 2, s->slotSize - 2, slotFill);
		Gfx_Draw2DFlat(slotX + 1, slotY + s->slotSize - 2, s->slotSize - 2, 1, highlight);
		Gfx_Draw2DFlat(slotX + s->slotSize - 2, slotY + 1, 1, s->slotSize - 2, highlight);
	}
	}
	}

	/* "Inventory" title above the panel (Indev's textured panel is self- */
	/*  contained, so no floating title there). */
	if (s->titleTex.ID && !(IndevTest_Enabled && (IndevTest_InvGuiTex() || IndevTest_CraftGuiTex()))) {
		s->titleTex.x = s->panelX + (s->panelW - s->titleTex.width) / 2;
		s->titleTex.y = s->panelY - s->titleTex.height - 4;
		Texture_Render(&s->titleTex);
	}

	/* Rebuild mesh whenever inventory has changed */
	if (SurvivalTest_InvVersion() != s->lastInvVersion) s->dirty = true;
	if (s->dirty) { SurvivalInvScreen_BuildMesh(screen); s->dirty = false; }

	/* ISO block pictures - SLOT entries only; the batch tail (the cursor-
	    held block) draws later, above the hover highlight like genuine's
	    z+32 held stack. */
	if (s->isoSlotVerts > 0) {
		Gfx_SetVertexFormat(VERTEX_FORMAT_TEXTURED);
		Gfx_BindDynamicVb(s->vb);
		IsometricDrawer_Render(s->isoSlotVerts, 0, s->isoState);
	}

	/* Item-id sprites (Indev items - tools, food, materials) drawn as flat */
	/*  icons from items.png; blocks already drew via the ISO pass above. */
	if (IndevTest_Enabled && IndevTest_ItemsTex()) {
		struct Texture itex;
		int isize = IndevTest_Enabled ? (int)(s->texF * 14.0f) : (int)(s->slotSize * 0.75f);
		int inset = IndevTest_Enabled ? (int)(s->texF * 1.0f) : (s->slotSize - isize) / 2;
		itex.ID = IndevTest_ItemsTex();
		for (i = 0; i < SurvivalInv_DisplayCount(); i++) {
			int slot = SurvivalInv_DisplaySlot(i), id, count;
			SurvivalInv_SlotContent(slot, &id, &count);
			if (count <= 0 || id < 256) continue;
			if (!IndevTest_ItemSpriteUV(id, &itex.uv.u1, &itex.uv.v1, &itex.uv.u2, &itex.uv.v2)) continue;

			SurvivalInv_AnySlotXY(s, slot, &slotX, &slotY);
			itex.x = (short)(slotX + inset);
			itex.y = (short)(slotY + inset);
			itex.width = (cc_uint16)isize; itex.height = (cc_uint16)isize;
			Texture_Render(&itex);
		}

		/* Slot.getBackgroundIconIndex: empty armor slots show their piece
		    silhouette from items.png (icon 15 + (piece << 4)) */
		for (i = 0; i < SurvivalInv_ArmorCells(); i++) {
			int icon;
			if (SurvivalTest_ArmorCount(i) > 0) continue;
			icon = 15 + ((3 - i) << 4);
			itex.uv.u1 = (icon % 16)     / 16.0f; itex.uv.v1 = (icon / 16)     / 16.0f;
			itex.uv.u2 = (icon % 16 + 1) / 16.0f; itex.uv.v2 = (icon / 16 + 1) / 16.0f;

			SurvivalInv_ArmorSlotXY(s, i, &slotX, &slotY);
			itex.x = (short)(slotX + inset);
			itex.y = (short)(slotY + inset);
			itex.width = (cc_uint16)isize; itex.height = (cc_uint16)isize;
			Texture_Render(&itex);
		}
	}

	/* Durability bars - renderItemOverlayIntoGUI draws them in EVERY slot */
	/*  (chest/storage/hotbar alike), not just the HUD hotbar: 13x2 black */
	/*  backing at (x+2, y+13), then the red->green remaining-durability bar. */
	if (IndevTest_Enabled) {
		float u = s->texF;
		int   dmg, maxDmg, bx, by, v, w, h;
		for (i = 0; i < SurvivalInv_DisplayCount(); i++) {
			int slot = SurvivalInv_DisplaySlot(i);
			SurvivalInv_SlotDamage(slot, &dmg, &maxDmg);
			if (maxDmg <= 0 || dmg <= 0) continue;

			SurvivalInv_AnySlotXY(s, slot, &slotX, &slotY);
			bx = slotX + (int)(2 * u); by = slotY + (int)(13 * u);
			v  = 255 - dmg * 255 / maxDmg;
			w  = 13  - dmg * 13  / maxDmg;
			h  = (int)u; if (h < 1) h = 1;

			Gfx_Draw2DFlat(bx, by, (int)(13 * u), h * 2, PackedCol_Make(0, 0, 0, 255));
			Gfx_Draw2DFlat(bx, by, (int)(12 * u), h,
				PackedCol_Make((cc_uint8)((255 - v) / 4), 63, 0, 255));
			Gfx_Draw2DFlat(bx, by, (int)(w * u), h,
				PackedCol_Make((cc_uint8)(255 - v), (cc_uint8)v, 0, 255));
		}
		Gfx_SetVertexFormat(VERTEX_FORMAT_TEXTURED);
	}

	/* Slot stack-count digits (the cursor-held stack's count draws later,
	    with the held stack itself) */
	if (s->countSlotVerts > 0 && s->countAtlas.tex.ID) {
		Gfx_SetVertexFormat(VERTEX_FORMAT_TEXTURED);
		Gfx_BindTexture(s->countAtlas.tex.ID);
		Gfx_BindDynamicVb(s->vb);
		Gfx_DrawVb_IndexedTris_Range(s->countSlotVerts,
		                             SURVINV_MAX_ISO_VERTS, DRAW_HINT_RECT);
	}

	/* GuiContainer's mouse-over highlight: translucent white over the 16px
	    slot area under the cursor - AFTER the slot item + count (the
	    highlight tints them), BEFORE the held stack. */
	if (IndevTest_Enabled && guiTex) {
		int hover = SurvivalInv_HitSlot(s, s->mouseX, s->mouseY);
		int inner = (int)(16 * s->texF);
		if (hover >= 0 && hover != SURVINV_RESULT_HIT) {
			SurvivalInv_AnySlotXY(s, hover, &slotX, &slotY);
			Gfx_Draw2DFlat(slotX, slotY, inner, inner, PackedCol_Make(255, 255, 255, 128));
		} else if (hover == SURVINV_RESULT_HIT) {
			Gfx_Draw2DFlat(s->resultX, s->resultY, inner, inner, PackedCol_Make(255, 255, 255, 128));
		}
	}

	/* Cursor-held BLOCK - the iso batch's tail (genuine held stack, z+32) */
	if (s->isoVertCount > s->isoSlotVerts) {
		Gfx_SetVertexFormat(VERTEX_FORMAT_TEXTURED);
		Gfx_BindDynamicVb(s->vb);
		IsometricDrawer_Render(s->isoVertCount - s->isoSlotVerts, s->isoSlotVerts,
		                       s->isoState + s->isoSlotVerts / 4);
	}

	/* Cursor-held stack follows the mouse, drawn over everything (blocks are */
	/*  in the iso mesh below - see BuildMesh's cursor entry - items here). */
	if (IndevTest_Enabled && SurvivalTest_CursorCount() > 0 && SurvivalTest_CursorId() >= 256
		&& IndevTest_ItemsTex()) {
		struct Texture ctex;
		int isize = (int)(s->texF * 14.0f);
		if (IndevTest_ItemSpriteUV(SurvivalTest_CursorId(),
				&ctex.uv.u1, &ctex.uv.v1, &ctex.uv.u2, &ctex.uv.v2)) {
			ctex.ID     = IndevTest_ItemsTex();
			ctex.x      = (short)(s->mouseX - isize / 2);
			ctex.y      = (short)(s->mouseY - isize / 2);
			ctex.width  = (cc_uint16)isize;
			ctex.height = (cc_uint16)isize;
			Texture_Render(&ctex);
			Gfx_SetVertexFormat(VERTEX_FORMAT_TEXTURED);
		}
	}

	/* Cursor-held stack count, over the held icon */
	if (s->countVertCount > s->countSlotVerts && s->countAtlas.tex.ID) {
		Gfx_SetVertexFormat(VERTEX_FORMAT_TEXTURED);
		Gfx_BindTexture(s->countAtlas.tex.ID);
		Gfx_BindDynamicVb(s->vb);
		Gfx_DrawVb_IndexedTris_Range(s->countVertCount - s->countSlotVerts,
		                             SURVINV_MAX_ISO_VERTS + s->countSlotVerts, DRAW_HINT_RECT);
	}

	/* GuiContainer foreground labels LAST - drawGuiContainerForegroundLayer
	    runs after the held stack, so labels are the topmost layer. Genuine
	    coordinates: chest "Chest"(8,6) + "Inventory"(8,74); furnace
	    "Furnace"(60,6) + "Inventory"(8,72); workbench "Crafting"(28,6) +
	    "Inventory"(8,72); pocket inventory "Crafting"(86,16). */
	if (IndevTest_Enabled && guiTex) {
		struct Texture* name = NULL;
		int nx = 8, ny = 6, invY = 72;
		if (contKind == INDEV_CONTAINER_CHEST) {
			name = &s->lblChest; invY = 74;
		} else if (contKind == INDEV_CONTAINER_FURNACE) {
			name = &s->lblFurnace; nx = 60;
		} else if (workbench) {
			name = &s->lblCrafting; nx = 28;
		} else if (s->lblCrafting.ID) {
			name = &s->lblCrafting; nx = 86; ny = 16; invY = -1;
		}
		if (name && name->ID) {
			name->x = (short)(s->panelX + (int)(nx * s->texF));
			name->y = (short)(s->panelY + (int)(ny * s->texF));
			Texture_Render(name);
		}
		if (invY >= 0 && s->lblInventory.ID) {
			s->lblInventory.x = (short)(s->panelX + (int)(8 * s->texF));
			s->lblInventory.y = (short)(s->panelY + (int)(invY * s->texF));
			Texture_Render(&s->lblInventory);
		}
		Gfx_SetVertexFormat(VERTEX_FORMAT_TEXTURED);
	}

	/* 3D paperdoll (only the pocket inventory has a doll window; the */
	/*  workbench/chest/furnace GUIs have none). Genuine draws it in the */
	/*  background layer; ours last for viewport reasons (accepted). */
	if (SurvivalTest_CraftDim() != 3 &&
		IndevTest_OpenKind() == INDEV_CONTAINER_NONE) SurvivalInv_RenderDoll(s);
}

static void SurvivalInvScreen_Init(void* screen) {
	struct SurvivalInvScreen* s = (struct SurvivalInvScreen*)screen;
	s->widgets     = NULL;
	s->numWidgets  = 0;
	s->maxWidgets  = 0;
	s->heldSlot    = -1;
	s->mouseX      = -1;
	s->mouseY      = -1;
	s->maxVertices = SURVINV_TOTAL_VERTS;
	SurvivalInv_InitDoll(s);
	/* Force an initial mesh build */
	s->lastInvVersion = SurvivalTest_InvVersion() - 1;
}

static void SurvivalInvScreen_Free(void* screen) {
	struct SurvivalInvScreen* s = (struct SurvivalInvScreen*)screen;
	s->heldSlot = -1;
	/* Safety net: any close path that bypassed the handlers still refunds. */
	SurvivalTest_SetCraftDim(2); /* return grid + reset to pocket 2x2 for next open */
	IndevTest_CloseContainer();
}

static void SurvivalInvScreen_ContextLost(void* screen) {
	struct SurvivalInvScreen* s = (struct SurvivalInvScreen*)screen;
	Font_Free(&s->font);
	TextAtlas_Free(&s->countAtlas);
	Gfx_DeleteTexture(&s->titleTex.ID);
	Gfx_DeleteTexture(&s->lblChest.ID);
	Gfx_DeleteTexture(&s->lblFurnace.ID);
	Gfx_DeleteTexture(&s->lblCrafting.ID);
	Gfx_DeleteTexture(&s->lblInventory.ID);
	Screen_ContextLost(screen);
}

/* The genuine 8px GUI font lives in 176-unit panel space, so on-screen
    text height must be 8 * texF - rasterise the label/count textures for
    the CURRENT panel scale (and again whenever it changes, e.g. resizing
    the window or toggling Indev GUI scale). */
static void SurvivalInv_MakeLabelTextures(struct SurvivalInvScreen* s) {
	static const cc_string digits = String_FromConst("0123456789");
	static const cc_string empty  = String_FromConst("");
	static const cc_string title  = String_FromConst("Inventory");
	/* GuiContainer foreground labels - colour 4210752 (0x404040) = &8, */
	/*  drawn without shadow like drawString(..., 4210752) */
	static const cc_string lblChe = String_FromConst("&8Chest");
	static const cc_string lblFur = String_FromConst("&8Furnace");
	static const cc_string lblCra = String_FromConst("&8Crafting");
	static const cc_string lblInv = String_FromConst("&8Inventory");
	struct DrawTextArgs args;
	struct FontDesc countFont;
	float texF;
	int size;

	/* same slot-size derivation Layout uses, so this is order-independent */
	texF = Display_ScaleX((int)(SURVINV_SLOT_BASE * Gui_GetInventoryScale())) / 18.0f;
	if (texF < 16.0f / 18.0f) texF = 16.0f / 18.0f; /* Layout's 16px floor */
	s->fontTexF = texF;
	size = (int)(8.0f * texF);
	if (size < 8) size = 8;

	/* free any previous-scale textures (no-ops right after context loss) */
	Font_Free(&s->font);
	TextAtlas_Free(&s->countAtlas);
	Gfx_DeleteTexture(&s->titleTex.ID);
	Gfx_DeleteTexture(&s->lblChest.ID);
	Gfx_DeleteTexture(&s->lblFurnace.ID);
	Gfx_DeleteTexture(&s->lblCrafting.ID);
	Gfx_DeleteTexture(&s->lblInventory.ID);

	Font_Make(&s->font, size, FONT_FLAGS_PADDING);
	Font_SetPadding(&s->font, 1);
	/* Unpadded digit atlas at the same 8-GUI-px size - the baked
	    drawStringWithShadow drop shadow scales with the font size, and
	    exact glyph metrics keep right-alignment tight. */
	Font_Make(&countFont, size, FONT_FLAGS_NONE);
	TextAtlas_Make(&s->countAtlas, &digits, &countFont, &empty);
	Font_Free(&countFont);

	DrawTextArgs_Make(&args, &title, &s->font, true);
	Drawer2D_MakeTextTexture(&s->titleTex, &args);

	DrawTextArgs_Make(&args, &lblChe, &s->font, false);
	Drawer2D_MakeTextTexture(&s->lblChest, &args);
	DrawTextArgs_Make(&args, &lblFur, &s->font, false);
	Drawer2D_MakeTextTexture(&s->lblFurnace, &args);
	DrawTextArgs_Make(&args, &lblCra, &s->font, false);
	Drawer2D_MakeTextTexture(&s->lblCrafting, &args);
	DrawTextArgs_Make(&args, &lblInv, &s->font, false);
	Drawer2D_MakeTextTexture(&s->lblInventory, &args);
	s->dirty = true;
}

static void SurvivalInvScreen_ContextRecreated(void* screen) {
	struct SurvivalInvScreen* s = (struct SurvivalInvScreen*)screen;
	Screen_UpdateVb(s);
	SurvivalInv_MakeLabelTextures(s);
}

static void SurvivalInvScreen_Layout(void* screen) {
	struct SurvivalInvScreen* s = (struct SurvivalInvScreen*)screen;
	int storageW, storageH, gap, pad, topAreaH;

	s->slotSize = Display_ScaleX((int)(SURVINV_SLOT_BASE * Gui_GetInventoryScale()));
	if (s->slotSize < 16) s->slotSize = 16; /* minimum usable size */

	/* label/count text is rasterised per panel scale - rebuild on change */
	if (Math_AbsF(s->slotSize / 18.0f - s->fontTexF) > 0.01f && s->font.handle) {
		SurvivalInv_MakeLabelTextures(s);
	}

	gap = (int)(SURVINV_GAP_BASE * Gui_GetInventoryScale());
	pad = (int)(SURVINV_PAD_BASE * Gui_GetInventoryScale());

	if (IndevTest_Enabled) {
		/* Genuine GuiInventory: a 176x166 texture panel, slots on its fixed */
		/*  18px grid - craft 2x2 at (88,26), result (144,36), storage (8,84), */
		/*  hotbar (8,142), doll window (26,8)-(74,78). texF scales it all. */
		/* GuiChest is 176x168 (114 + 3 rows * 18) with the player rows one */
		/*  pixel lower (85/143) - genuine to this version. GuiFurnace uses */
		/*  the standard 176x166 layout. */
		cc_bool chest = IndevTest_OpenKind() == INDEV_CONTAINER_CHEST;
		float f = s->slotSize / 18.0f;
		s->texF   = f;
		s->panelW = (int)(176 * f);
		s->panelH = (int)((chest ? 168 : 166) * f);
		s->panelX = (Window_Main.Width  - s->panelW) / 2;
		s->panelY = (Window_Main.Height - s->panelH) / 2;

		s->gridX   = s->panelX + (int)(8   * f);
		s->gridY   = s->panelY + (int)((chest ? 85  : 84)  * f);
		s->hotY    = s->panelY + (int)((chest ? 143 : 142) * f);
		if (SurvivalTest_CraftDim() == 3) {
			/* GuiCrafting (crafting.png): 3x3 grid at (30,17), result (124,35), */
			/*  no paperdoll window. */
			s->craftX  = s->panelX + (int)(30  * f);
			s->craftY  = s->panelY + (int)(17  * f);
			s->resultX = s->panelX + (int)(124 * f);
			s->resultY = s->panelY + (int)(35  * f);
		} else {
			s->craftX  = s->panelX + (int)(88  * f);
			s->craftY  = s->panelY + (int)(26  * f);
			s->resultX = s->panelX + (int)(144 * f);
			s->resultY = s->panelY + (int)(36  * f);
		}
		/* Genuine window: x 26..74 (48 wide), feet at y=76 - a TALL region, */
		/*  not square, so legs aren't clipped. dollBoxSize stays the WIDTH */
		/*  (mouse-follow + horizontal centring key off it); dollBoxH is the */
		/*  viewport height. */
		s->dollBoxX    = s->panelX + (int)(26 * f);
		s->dollBoxY    = s->panelY + (int)(8  * f);
		s->dollBoxSize = (int)(48 * f);
		s->dollBoxH    = (int)(68 * f);
		s->dirty = true;
		return;
	}

	storageW = SURVINV_STORAGE_COLS * s->slotSize;
	storageH = SURVINV_STORAGE_ROWS * s->slotSize;
	s->dollBoxSize = SURVINV_DOLL_UNITS * s->slotSize;
	topAreaH = s->dollBoxSize;

	/* Top row holds the paperdoll, then the 2x2 craft grid, an arrow gap, and */
	/*  the result slot. Panel widens to whichever of that row / the storage */
	/*  grid is wider, and the storage grid recentres under it. */
	{
		int craftW = 2 * s->slotSize + gap + s->slotSize; /* grid + arrow gap + result */
		int topW   = s->dollBoxSize + gap + craftW;
		/* Only reserve room for the crafting row in Indev mode. */
		int contentW = !IndevTest_Enabled ? storageW : (storageW > topW ? storageW : topW);

		s->panelW = contentW + pad * 2;
		/* top area + storage grid + (double gap + hotbar row) + padding */
		s->panelH = pad + topAreaH + gap + storageH + gap * 2 + s->slotSize + pad;
		s->panelX = (Window_Main.Width  - s->panelW) / 2;
		s->panelY = (Window_Main.Height - s->panelH) / 2;

		s->dollBoxX = s->panelX + pad;
		s->dollBoxY = s->panelY + pad;

		s->craftX = s->dollBoxX + s->dollBoxSize + gap;
		s->craftY = s->dollBoxY + (topAreaH - 2 * s->slotSize) / 2;
		s->resultX = s->craftX + 2 * s->slotSize + gap;
		s->resultY = s->dollBoxY + (topAreaH - s->slotSize) / 2;

		s->gridX = s->panelX + (s->panelW - storageW) / 2;
		s->gridY = s->panelY + pad + topAreaH + gap;
		/* Hotbar row sits below storage with a wider separating gap, exactly */
		/*  how GuiInventory separates the two regions. */
		s->hotY   = s->gridY + SURVINV_STORAGE_ROWS * s->slotSize + gap * 2;
	}
	s->dollBoxH = s->dollBoxSize; /* square doll box outside Indev */

	s->dirty = true;
}

static void SurvivalInv_Click(struct SurvivalInvScreen* s, int mx, int my, cc_bool rightClick) {
	int hit = SurvivalInv_HitSlot(s, mx, my);

	if (hit < 0) {
		/* Not on a slot. Genuine Minecraft only closes when the click lands */
		/*  OUTSIDE the GUI window; clicking the panel background does nothing */
		/*  (so it never eats slots). This also stops the very right-click that */
		/*  opened a workbench - delivered as a CCMOUSE_R key event to the just- */
		/*  opened screen at the crosshair/centre, i.e. on the panel - from */
		/*  instantly closing it again. */
		cc_bool insidePanel = mx >= s->panelX && mx < s->panelX + s->panelW &&
		                      my >= s->panelY && my < s->panelY + s->panelH;
		if (insidePanel) return;
		/* Clicked fully outside the window - refund cursor + grid and close */
		SurvivalTest_CursorReturn();
		SurvivalTest_SetCraftDim(2); /* return grid + reset to pocket 2x2 for next open */
		IndevTest_CloseContainer();  /* container contents stay in the tile entity */
		Gui_Remove((struct Screen*)s);
		return;
	}
	if (hit == SURVINV_RESULT_HIT) {
		SurvivalTest_ResultClick(); /* crafts once onto the cursor */
	} else {
		SurvivalTest_SlotClick(hit, rightClick);
	}
	s->dirty = true;
}

static int SurvivalInvScreen_KeyDown(void* screen, int key, struct InputDevice* device) {
	struct SurvivalInvScreen* s = (struct SurvivalInvScreen*)screen;
	/* E closes too, mirroring the survival E-to-open binding. */
	if (InputBind_Claims(BIND_INVENTORY, key, device) || key == CCKEY_ESCAPE ||
		(SurvivalTest_Enabled && key == 'E')) {
		s->heldSlot = -1;
		SurvivalTest_CursorReturn();
		SurvivalTest_SetCraftDim(2); /* return grid + reset to pocket 2x2 for next open */
		IndevTest_CloseContainer();  /* container contents stay in the tile entity */
		Gui_Remove((struct Screen*)s);
	}
	/* Right mouse arrives as a key event, not a pointer event - route it */
	/*  through the same click logic using the tracked mouse position. */
	if (key == CCMOUSE_R && s->mouseX >= 0) {
		SurvivalInv_Click(s, s->mouseX, s->mouseY, true);
	}
	return true;
}

static int SurvivalInvScreen_PointerDown(void* screen, int id, int x, int y) {
	struct SurvivalInvScreen* s = (struct SurvivalInvScreen*)screen;
	SurvivalInv_Click(s, x, y, false);
	return TOUCH_TYPE_GUI;
}

/* Tracks the cursor so the paperdoll can turn to face it. */
static int SurvivalInvScreen_PointerMove(void* screen, int id, int x, int y) {
	struct SurvivalInvScreen* s = (struct SurvivalInvScreen*)screen;
	s->mouseX = x;
	s->mouseY = y;
	if (SurvivalTest_CursorCount() > 0) s->dirty = true; /* held stack tracks the mouse */
	return false;
}

static const struct ScreenVTABLE SurvivalInvScreen_VTABLE = {
	SurvivalInvScreen_Init,        Screen_NullUpdate,           SurvivalInvScreen_Free,
	SurvivalInvScreen_Render,      SurvivalInvScreen_BuildMesh,
	SurvivalInvScreen_KeyDown,     Screen_InputUp,              Screen_FKeyPress, Screen_FText,
	SurvivalInvScreen_PointerDown, Screen_PointerUp,            SurvivalInvScreen_PointerMove, Screen_FMouseScroll,
	SurvivalInvScreen_Layout,      SurvivalInvScreen_ContextLost, SurvivalInvScreen_ContextRecreated,
	NULL
};

void SurvivalInvScreen_Show(void) {
	struct SurvivalInvScreen* s = &SurvivalInvScreen_Instance;
	/* Non-survival modes use the normal creative block-grid inventory. */
	if (!SurvivalTest_Enabled) { InventoryScreen_Show(); return; }
	/* Faithful Classic 0.30-s had no inventory screen whatsoever - just the */
	/*  fixed hotbar - so opening the inventory does nothing at all. The storage/ */
	/*  crafting screen is an Enhanced extra AND the Indev gamemode's crafting */
	/*  inventory (Indev's whole point is the 2x2 grid), so both open it; plain */
	/*  c0.30-s keeps the authentic no-op. */
	if (!SurvivalTest_Enhanced && !IndevTest_Enabled) return;
	s->grabsInput = true;
	s->closable   = true;
	s->VTABLE     = &SurvivalInvScreen_VTABLE;
	Gui_Add((struct Screen*)s, GUI_PRIORITY_INVENTORY);
}


/*########################################################################################################################*
*------------------------------------------------------GameOverScreen-----------------------------------------------------*
*#########################################################################################################################*/
/* Shown when the player dies in Survival Test. Both ground truths
    (GameOverScreen.java c0.30, GuiGameOver.java Indev) offer exactly two
    buttons - "Generate new level..." at (w/2-100, h/4+72) and "Load
    level.." at (w/2-100, h/4+96) - there is NO respawn: death means
    starting or loading another level. Title "Game over!" 2x-scaled at
    virtual y=60, "Score: &eN" at y=100, both top-anchored. (An earlier
    note here claimed a genuine Respawn button - the decompile has none.) */
static struct GameOverScreen {
	Screen_Body
	struct FontDesc titleFont, messageFont, btnFont;
	struct TextWidget title, message;
	struct ButtonWidget gen, load;
	struct Widget* __widgets[4];
	int guiScale; /* survival GUI scale the fonts/buttons were built at */
} GameOverScreen CC_BIG_VAR;

static int GameOverScreen_Scale(void) {
	int scale = (int)(Gui_GetHotbarScale() * DisplayInfo.ScaleY);
	return scale < 1 ? 1 : scale;
}

static void GameOverScreen_ContextRecreated(void* screen);

static void GameOverScreen_Layout(void* screen) {
	struct GameOverScreen* s = (struct GameOverScreen*)screen;
	/* genuine coordinates are in ScaledResolution units - top-anchored. */
	/*  (hotbar scale = raw scale x window scale, the survival HUD's GUI px) */
	int scale = GameOverScreen_Scale();
	/* fonts + button sizes are baked at a specific scale - rebuild them
	    when the window scale changes (e.g. resize with Indev GUI scale on),
	    or the offsets scale while the widgets stay small (user report) */
	if (scale != s->guiScale && s->titleFont.handle) GameOverScreen_ContextRecreated(s);

	Widget_SetLocation(&s->title,   ANCHOR_CENTRE, ANCHOR_MIN, 0,  60 * scale);
	Widget_SetLocation(&s->message, ANCHOR_CENTRE, ANCHOR_MIN, 0, 100 * scale);
	Widget_SetLocation(&s->gen,     ANCHOR_CENTRE, ANCHOR_MIN, 0, Game.Height / 4 + 72 * scale);
	Widget_SetLocation(&s->load,    ANCHOR_CENTRE, ANCHOR_MIN, 0, Game.Height / 4 + 96 * scale);
}

static void GameOverScreen_ContextLost(void* screen) {
	struct GameOverScreen* s = (struct GameOverScreen*)screen;
	Font_Free(&s->titleFont);
	Font_Free(&s->messageFont);
	Font_Free(&s->btnFont);
	Screen_ContextLost(screen);
}

static void GameOverScreen_ContextRecreated(void* screen) {
	struct GameOverScreen* s = (struct GameOverScreen*)screen;
	cc_string msg; char msgBuffer[STRING_SIZE];
	int score = SurvivalTest_Score();
	int scale = GameOverScreen_Scale();
	Screen_UpdateVb(screen);
	s->guiScale = scale;

	/* Everything in genuine GUI px x the survival GUI scale, like the HUD:
	    the 8px GUI font (our 8pt) at 2x for the title (GameOverScreen.render's
	    glScalef(2,2,2)), 1x for the score and buttons; buttons are the
	    genuine 200x20 GUI px. Fixed sizes here previously left the widgets
	    tiny while the offsets scaled (user report + reference screenshot). */
	Font_Free(&s->titleFont);
	Font_Free(&s->messageFont);
	Font_Free(&s->btnFont);
	Font_Make(&s->titleFont,  16 * scale, FONT_FLAGS_BOLD);
	Font_Make(&s->messageFont, 8 * scale, FONT_FLAGS_NONE);
	Font_Make(&s->btnFont,     8 * scale, FONT_FLAGS_BOLD);
	TextWidget_SetConst(&s->title, "Game over!", &s->titleFont);

	String_InitArray(msg, msgBuffer);
	String_Format1(&msg, "Score: &e%i", &score);
	TextWidget_Set(&s->message, &msg, &s->messageFont);

	s->gen.minWidth   = 200 * scale; s->gen.minHeight  = 20 * scale;
	s->load.minWidth  = 200 * scale; s->load.minHeight = 20 * scale;
	ButtonWidget_SetConst(&s->gen,  "Generate new level...", &s->btnFont);
	ButtonWidget_SetConst(&s->load, "Load level..",          &s->btnFont);
}

static void GameOverScreen_OnGen(void* screen, void* w) {
	Gui_Remove((struct Screen*)&GameOverScreen);
	GenLevelScreen_Show();
}

static void GameOverScreen_OnLoad(void* screen, void* w) {
	Gui_Remove((struct Screen*)&GameOverScreen);
	LoadLevelScreen_Show();
}

static void GameOverScreen_Init(void* screen) {
	struct GameOverScreen* s = (struct GameOverScreen*)screen;
	s->widgets     = s->__widgets;
	s->numWidgets  = 0;
	s->maxWidgets  = Array_Elems(s->__widgets);

	TextWidget_Add(s, &s->title);
	TextWidget_Add(s, &s->message);
	ButtonWidget_Add(s, &s->gen,  400, GameOverScreen_OnGen);
	ButtonWidget_Add(s, &s->load, 400, GameOverScreen_OnLoad);

	s->maxVertices = Screen_CalcDefaultMaxVertices(s);
}

static void GameOverScreen_Render(void* screen, float delta) {
	/* drawFadingBox(0, 0, width, height, 1615855616, -1602211792) - the two */
	/*  ARGB literals decode to a dark red top edge fading into a more opaque, */
	/*  lighter maroon bottom edge (not a neutral gray, like the original */
	/*  ClassiCube colors here used to be). */
	PackedCol top    = PackedCol_Make(80,  0,  0,  96);
	PackedCol bottom = PackedCol_Make(128, 48, 48, 160);
	Gfx_Draw2DGradient(0, 0, Window_UI.Width, Window_UI.Height, top, bottom);

	Screen_Render2Widgets(screen, delta);
}

static const struct ScreenVTABLE GameOverScreen_VTABLE = {
	GameOverScreen_Init,   Screen_NullUpdate, Screen_NullFunc,
	GameOverScreen_Render, Screen_BuildMesh,
	Menu_InputDown,        Screen_InputUp,    Screen_TKeyPress, Screen_TText,
	Menu_PointerDown,      Screen_PointerUp,  Menu_PointerMove, Screen_TMouseScroll,
	GameOverScreen_Layout, GameOverScreen_ContextLost, GameOverScreen_ContextRecreated
};

void GameOverScreen_Show(void) {
	struct GameOverScreen* s = &GameOverScreen;
	s->grabsInput  = true;
	/* The world MUST keep rendering behind the translucent red gradient - */
	/*  the death camera (sideways keel + slow FOV zoom, see SurvivalTest's */
	/*  ApplyHurtTilt/DeathFovZoom) plays out behind the Game Over screen. */
	s->blocksWorld = false;
	s->VTABLE      = &GameOverScreen_VTABLE;
	Gui_Add((struct Screen*)s, GUI_PRIORITY_DISCONNECT);
}


/*########################################################################################################################*
*------------------------------------------------------LoadingScreen------------------------------------------------------*
*#########################################################################################################################*/
static struct LoadingScreen {
	Screen_Body
	struct FontDesc font;
	float progress; 
	int rows;
	
	int progX, progY, progWidth, progHeight;
	struct TextWidget title, message;
	cc_string titleStr, messageStr;
	const char* lastState;

	char _titleBuffer[STRING_SIZE];
	char _messageBuffer[STRING_SIZE];
	struct Widget* __widgets[2];
} LoadingScreen CC_BIG_VAR;
#define LOADING_TILE_SIZE 64

static void LoadingScreen_SetTitle(struct LoadingScreen* s) {
	TextWidget_Set(&s->title, &s->titleStr, &s->font);
	s->dirty = true;
}
static void LoadingScreen_SetMessage(struct LoadingScreen* s) {
	TextWidget_Set(&s->message, &s->messageStr, &s->font);
	s->dirty = true;
}

static void LoadingScreen_CalcMaxVertices(struct LoadingScreen* s) {
	s->rows = Math_CeilDiv(Window_UI.Height, LOADING_TILE_SIZE);
	s->maxVertices = Screen_CalcDefaultMaxVertices(s) + s->rows * 4;
}

static void LoadingScreen_Layout(void* screen) {
	struct LoadingScreen* s = (struct LoadingScreen*)screen;
	int oldRows, y;
	Widget_SetLocation(&s->title,   ANCHOR_CENTRE, ANCHOR_CENTRE, 0, -31);
	Widget_SetLocation(&s->message, ANCHOR_CENTRE, ANCHOR_CENTRE, 0,  17);
	y = Display_ScaleY(34);

	s->progWidth  = Display_ScaleX(200);
	s->progX      = Gui_CalcPos(ANCHOR_CENTRE, 0, s->progWidth,  Window_UI.Width);
	s->progHeight = Display_ScaleY(4);
	s->progY      = Gui_CalcPos(ANCHOR_CENTRE, y, s->progHeight, Window_UI.Height);

	oldRows = s->rows;
	LoadingScreen_CalcMaxVertices(s);
	if (oldRows == s->rows) return;
	Screen_UpdateVb(s);
}

static void LoadingScreen_ContextLost(void* screen) {
	struct LoadingScreen* s = (struct LoadingScreen*)screen;
	Font_Free(&s->font);
	Screen_ContextLost(screen);
}

static void LoadingScreen_ContextRecreated(void* screen) {
	struct LoadingScreen* s = (struct LoadingScreen*)screen;
	Gui_MakeBodyFont(&s->font);
	LoadingScreen_SetTitle(s);
	LoadingScreen_SetMessage(s);
	Screen_UpdateVb(s);
}

static void LoadingScreen_BuildMesh(void* screen) {
	struct LoadingScreen* s = (struct LoadingScreen*)screen;
	struct VertexTextured* data;
	struct VertexTextured** ptr;
	struct Texture tex;
	TextureLoc loc;
	int atlasIndex, i;

	data = Screen_LockVb(s);
	ptr  = &data;

	loc       = Block_Tex(BLOCK_DIRT, FACE_YMAX);
	Tex_SetRect(tex, 0,0, Window_UI.Width,LOADING_TILE_SIZE);
	tex.uv    = Atlas1D_TexRec(loc, 1, &atlasIndex);
	tex.uv.u2 = (float)Window_UI.Width / LOADING_TILE_SIZE;
	
	for (i = 0; i < s->rows; i++) {
		tex.y = i * LOADING_TILE_SIZE;
		Gfx_Make2DQuad(&tex, PackedCol_Make(64, 64, 64, 255), ptr);
	}

	Widget_BuildMesh(&s->title,   ptr);
	Widget_BuildMesh(&s->message, ptr);
	Gfx_UnlockDynamicVb(s->vb);
}

static void LoadingScreen_MapLoading(void* screen, float progress) {
	((struct LoadingScreen*)screen)->progress = progress;
}

static void LoadingScreen_MapLoaded(void* screen) {
	Gui_Remove((struct Screen*)screen);
}

static void LoadingScreen_Init(void* screen) {
	struct LoadingScreen* s = (struct LoadingScreen*)screen;
	s->widgets     = s->__widgets;
	s->numWidgets  = 0;
	s->maxWidgets  = Array_Elems(s->__widgets);

	TextWidget_Add(s, &s->title);
	TextWidget_Add(s, &s->message);

	LoadingScreen_CalcMaxVertices(s);
	Gfx_SetFog(false);
	Event_Register_(&WorldEvents.Loading,   s, LoadingScreen_MapLoading);
	Event_Register_(&WorldEvents.MapLoaded, s, LoadingScreen_MapLoaded);
}

static void LoadingScreen_Render(void* screen, float delta) {
	struct LoadingScreen* s = (struct LoadingScreen*)screen;
	int offset, filledWidth;
	TextureLoc loc;

	Gfx_SetVertexFormat(VERTEX_FORMAT_TEXTURED);
	Gfx_BindDynamicVb(s->vb);

	/* Draw background dirt */
	offset = 0;
	if (s->rows) {
		loc = Block_Tex(BLOCK_DIRT, FACE_YMAX);
		Atlas1D_Bind(Atlas1D_Index(loc));
		Gfx_DrawVb_IndexedTris_Range(s->rows * 4, 0, DRAW_HINT_SPRITE);
		offset = s->rows * 4;
	}

	offset = Widget_Render2(&s->title,   offset);
	offset = Widget_Render2(&s->message, offset);

	filledWidth = (int)(s->progWidth * s->progress);
	Gfx_Draw2DFlat(s->progX, s->progY, s->progWidth, 
					s->progHeight, PackedCol_Make(128, 128, 128, 255));
	Gfx_Draw2DFlat(s->progX, s->progY, filledWidth,  
					s->progHeight, PackedCol_Make(128, 255, 128, 255));
}

static void LoadingScreen_Free(void* screen) {
	struct LoadingScreen* s = (struct LoadingScreen*)screen;
	Event_Unregister_(&WorldEvents.Loading,   s, LoadingScreen_MapLoading);
	Event_Unregister_(&WorldEvents.MapLoaded, s, LoadingScreen_MapLoaded);
}

CC_NOINLINE static void LoadingScreen_ShowCommon(const cc_string* title, const cc_string* message) {
	struct LoadingScreen* s = &LoadingScreen;
	s->lastState = NULL;
	s->progress  = 0.0f;

	String_InitArray(s->titleStr,   s->_titleBuffer);
	String_AppendString(&s->titleStr,   title);
	String_InitArray(s->messageStr, s->_messageBuffer);
	String_AppendString(&s->messageStr, message);
	
	s->grabsInput  = true;
	s->blocksWorld = true;
	Gui_Add((struct Screen*)s, 
		Game_ClassicMode ? GUI_PRIORITY_OLDLOADING : GUI_PRIORITY_LOADING);
}

static const struct ScreenVTABLE LoadingScreen_VTABLE = {
	LoadingScreen_Init,   Screen_NullUpdate, LoadingScreen_Free, 
	LoadingScreen_Render, LoadingScreen_BuildMesh,
	Screen_TInput,        Screen_InputUp,    Screen_TKeyPress,   Screen_TText,
	Screen_TPointer,      Screen_PointerUp,  Screen_TPointer,    Screen_TMouseScroll,
	LoadingScreen_Layout, LoadingScreen_ContextLost, LoadingScreen_ContextRecreated
};
void LoadingScreen_Show(const cc_string* title, const cc_string* message) {
	LoadingScreen.VTABLE = &LoadingScreen_VTABLE;
	LoadingScreen_ShowCommon(title, message);
}


/*########################################################################################################################*
*--------------------------------------------------GeneratingMapScreen----------------------------------------------------*
*#########################################################################################################################*/
static void GeneratingScreen_AtlasChanged(void* obj) {
	LoadingScreen.dirty = true; /* Dirt texture may have changed */
}

static void GeneratingScreen_Init(void* screen) {
	LoadingScreen_Init(screen);
	Event_Register_(&TextureEvents.AtlasChanged,   NULL, GeneratingScreen_AtlasChanged);
}
static void GeneratingScreen_Free(void* screen) {
	LoadingScreen_Free(screen);
	Event_Unregister_(&TextureEvents.AtlasChanged, NULL, GeneratingScreen_AtlasChanged);
}

static void GeneratingScreen_EndGeneration(void) {
	struct LocationUpdate update;
	World_SetNewMap(Gen_Blocks, World.Width, World.Height, World.Length);
	if (!Gen_Blocks) { Chat_AddRaw("&cFailed to generate the map."); return; }

	Gen_Blocks = NULL;
	/* An Indev generation supplies its own spawn (the spawn house), theme
	    environment and initial mob population; otherwise default spawn. */
	if (!IndevGen_ApplyPostLoad(&update)) {
		LocalPlayer_CalcDefaultSpawn(Entities.CurPlayer, &update);
	}
	LocalPlayers_MoveToSpawn(&update);
}

static void GeneratingScreen_Update(void* screen, float delta) {
	struct LoadingScreen* s    = (struct LoadingScreen*)screen;
	const char* state = (const char*)Gen_CurrentState;
	if (state == s->lastState) return;
	s->lastState = state;

	s->messageStr.length = 0;
	String_AppendConst(&s->messageStr, state);
	LoadingScreen_SetMessage(s);
}

static void GeneratingScreen_Render(void* screen, float delta) {
	struct LoadingScreen* s = (struct LoadingScreen*)screen;
	s->progress = Gen_CurrentProgress;
	LoadingScreen_Render(s, delta);
	if (Gen_IsDone()) GeneratingScreen_EndGeneration();
}

static const struct ScreenVTABLE GeneratingScreen_VTABLE = {
	GeneratingScreen_Init,   GeneratingScreen_Update, GeneratingScreen_Free,
	GeneratingScreen_Render, LoadingScreen_BuildMesh,
	Screen_TInput,           Screen_InputUp,    Screen_TKeyPress,   Screen_TText,
	Screen_TPointer,         Screen_PointerUp,  Screen_FPointer,    Screen_TMouseScroll,
	LoadingScreen_Layout, LoadingScreen_ContextLost, LoadingScreen_ContextRecreated
};
void GeneratingScreen_Show(void) {
	static const cc_string title   = String_FromConst("Generating level");
	static const cc_string message = String_FromConst("Generating..");

	LoadingScreen.VTABLE = &GeneratingScreen_VTABLE;
	LoadingScreen_ShowCommon(&title, &message);
}


/*########################################################################################################################*
*----------------------------------------------------DisconnectScreen-----------------------------------------------------*
*#########################################################################################################################*/
static struct DisconnectScreen {
	Screen_Body
	float delayLeft;
	cc_bool canReconnect, lastActive;
	int lastSecsLeft;
	struct ButtonWidget reconnect, quit;

	struct FontDesc titleFont, messageFont;
	struct TextWidget title, message;
	char _titleBuffer[STRING_SIZE * 2];
	char _messageBuffer[STRING_SIZE];
	cc_string titleStr, messageStr;
	struct Widget* __widgets[4];
} DisconnectScreen CC_BIG_VAR;

#define DISCONNECT_DELAY_SECS 5

static void DisconnectScreen_Layout(void* screen) {
	struct DisconnectScreen* s = (struct DisconnectScreen*)screen;
	Widget_SetLocation(&s->title,     ANCHOR_CENTRE, ANCHOR_CENTRE, 0, -30);
	Widget_SetLocation(&s->message,   ANCHOR_CENTRE, ANCHOR_CENTRE, 0,  10);
	Widget_SetLocation(&s->reconnect, ANCHOR_CENTRE, ANCHOR_CENTRE, 0,  80);
	Widget_SetLocation(&s->quit,      ANCHOR_CENTRE, ANCHOR_CENTRE, 0, 130);
}

static void DisconnectScreen_UpdateReconnect(struct DisconnectScreen* s) {
	cc_string msg; char msgBuffer[STRING_SIZE];
	int secsLeft;
	String_InitArray(msg, msgBuffer);

	if (s->canReconnect) {
		secsLeft = Math_Ceil(s->delayLeft);

		if (secsLeft > 0) {
			String_Format1(&msg, "Reconnect in %i", &secsLeft);
		}
		Widget_SetDisabled(&s->reconnect, secsLeft > 0);
	}

	if (!msg.length) String_AppendConst(&msg, "Reconnect");
	ButtonWidget_Set(&s->reconnect, &msg, &s->titleFont);
}

static void DisconnectScreen_ContextLost(void* screen) {
	struct DisconnectScreen* s = (struct DisconnectScreen*)screen;
	Font_Free(&s->titleFont);
	Font_Free(&s->messageFont);
	Screen_ContextLost(screen);
}

static void DisconnectScreen_ContextRecreated(void* screen) {
	struct DisconnectScreen* s = (struct DisconnectScreen*)screen;
	Screen_UpdateVb(screen);

	Gui_MakeTitleFont(&s->titleFont);
	Gui_MakeBodyFont(&s->messageFont);
	TextWidget_Set(&s->title,   &s->titleStr,   &s->titleFont);
	TextWidget_Set(&s->message, &s->messageStr, &s->messageFont);

	DisconnectScreen_UpdateReconnect(s);
	ButtonWidget_SetConst(&s->quit, "Quit game", &s->titleFont);
}

static void DisconnectScreen_OnReconnect(void* s, void* w) {
	Gui_Remove((struct Screen*)s);
	Gui_ShowDefault();
	Server.BeginConnect();
}

static void DisconnectScreen_OnQuit(void* s, void* w) { 
	Window_RequestClose(); 
}

static void DisconnectScreen_Init(void* screen) {
	struct DisconnectScreen* s = (struct DisconnectScreen*)screen;
	s->widgets      = s->__widgets;
	s->numWidgets   = 0;
	s->maxWidgets   = Array_Elems(s->__widgets);

	TextWidget_Add(s, &s->title);
	TextWidget_Add(s, &s->message);

	ButtonWidget_Add(s, &s->reconnect, 300, DisconnectScreen_OnReconnect);
	ButtonWidget_Add(s, &s->quit,      300, DisconnectScreen_OnQuit);
	if (!s->canReconnect) s->reconnect.flags = WIDGET_FLAG_DISABLED;

	Game_SetMinFrameTime(1000 / 5.0f);

	s->delayLeft    = DISCONNECT_DELAY_SECS;
	s->lastSecsLeft = DISCONNECT_DELAY_SECS;
	s->maxVertices  = Screen_CalcDefaultMaxVertices(s);
}

static void DisconnectScreen_Update(void* screen, float delta) {
	struct DisconnectScreen* s = (struct DisconnectScreen*)screen;
	int secsLeft;

	if (!s->canReconnect) return;
	s->delayLeft -= delta;
	secsLeft = Math_Ceil(s->delayLeft);

	if (secsLeft < 0) secsLeft = 0;
	if (s->lastSecsLeft == secsLeft && s->reconnect.active == s->lastActive) return;
	DisconnectScreen_UpdateReconnect(s);

	s->lastSecsLeft = secsLeft;
	s->lastActive   = s->reconnect.active;
	s->dirty        = true;
}

static void DisconnectScreen_Render(void* screen, float delta) {
	PackedCol top    = PackedCol_Make(64, 32, 32, 255);
	PackedCol bottom = PackedCol_Make(80, 16, 16, 255);
	Gfx_Draw2DGradient(0, 0, Window_UI.Width, Window_UI.Height, top, bottom);

	Screen_Render2Widgets(screen, delta);
}

static void DisconnectScreen_Free(void* screen) { Game_SetFpsLimit(Game_FpsLimit); }

static const struct ScreenVTABLE DisconnectScreen_VTABLE = {
	DisconnectScreen_Init,   DisconnectScreen_Update, DisconnectScreen_Free,
	DisconnectScreen_Render, Screen_BuildMesh,
	Menu_InputDown,          Screen_InputUp,          Screen_TKeyPress, Screen_TText,
	Menu_PointerDown,        Screen_PointerUp,        Menu_PointerMove, Screen_TMouseScroll,
	DisconnectScreen_Layout, DisconnectScreen_ContextLost, DisconnectScreen_ContextRecreated
};
void DisconnectScreen_Show(const cc_string* title, const cc_string* message) {
	static const cc_string kick = String_FromConst("Kicked ");
	static const cc_string ban  = String_FromConst("Banned ");
	cc_string why; char whyBuffer[STRING_SIZE];
	struct DisconnectScreen* s = &DisconnectScreen;
	int i;

	s->grabsInput  = true;
	s->blocksWorld = true;

	String_InitArray(s->titleStr,   s->_titleBuffer);
	String_AppendString(&s->titleStr,   title);
	String_InitArray(s->messageStr, s->_messageBuffer);
	String_AppendString(&s->messageStr, message);

	String_InitArray(why, whyBuffer);
	String_AppendColorless(&why, message);
	
	s->canReconnect = !(String_CaselessStarts(&why, &kick) || String_CaselessStarts(&why, &ban));
	s->VTABLE       = &DisconnectScreen_VTABLE;

	Gui_Add((struct Screen*)s, GUI_PRIORITY_DISCONNECT);
	/* Remove other screens instead of just drawing over them to reduce GPU usage */
	for (i = Gui.ScreensCount - 1; i >= 0; i--) 
	{
		if (Gui_Screens[i] == (struct Screen*)s) continue;
		Gui_Remove(Gui_Screens[i]);
	}
}
