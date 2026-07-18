#include "SurvivalNet.h"
#include "Server.h"
#include "Protocol.h"
#include "Event.h"
#include "Game.h"
#include "Chat.h"
#include "String_.h"
#include "Logger.h"
#include "Inventory.h"
#include "SurvivalTest.h"
#include "IndevTest.h"

/* Client reference implementation of the survival multiplayer sub-protocol.
   Everything here is inert until BOTH:
     - the server negotiated the "SurvivalTest" CPE extension (Server.SupportsSurvival), and
     - we're in multiplayer (!Server.IsSinglePlayer).
   So a stock Classic/CPE server is completely unaffected: the handler early-outs
   before touching any state, and we never send on the channel unless the server
   speaks it. See doc/networking-plan.md (§3 gate, §20 gating, §25 wire format).

   Two-layer design: the CPE extension is the per-CONNECTION capability; SURV_HELLO
   is the per-MAP activation. A capable client on a non-survival map stays plain
   Classic; a /goto onto a survival map activates via a fresh HELLO; a mode-0 HELLO
   (the server's live /Survival refresh) or any map change deactivates.

   With activation up, the SERVER owns the simulation (SurvivalNet_ServerDriven):
   the client applies authoritative state (health, time, world info) and sends
   intents; every local mutation source is gated off in SurvivalTest/IndevTest.
   Byte layouts verified against the MCGalaxy fork's Network/SurvivalNet.cs.
   Copyright 2014-2025 ClassiCube | Licensed under BSD-3
*/

/* Whether the survival sub-protocol is live for this connection. */
static cc_bool SurvivalNet_Active(void) {
	return !Server.IsSinglePlayer && Server.SupportsSurvival;
}

/* --- per-map activation (set by SURV_HELLO, cleared on map change) --- */
static cc_uint8 net_mode;  /* 0 off / 1 c0.30-s / 2 indev - SurvivalMode enum on the server */
static cc_uint8 net_flags; /* HELLO flags: bit0 enhanced, bit1 creative, bit2 pvp, bit3 deathDrops */

int SurvivalNet_ActiveMode(void)  { return SurvivalNet_Active() ? net_mode  : 0; }
int SurvivalNet_ActiveFlags(void) { return SurvivalNet_Active() ? net_flags : 0; }

cc_bool SurvivalNet_ServerDriven(void) {
	return SurvivalNet_Active() && net_mode != 0;
}

void SurvivalNet_Send(cc_uint8* payload) {
	if (!SurvivalNet_Active()) return;
	CPE_SendPluginMessage(SURVNET_CHANNEL, payload);
}


/*########################################################################################################################*
*----------------------------------------------server -> client appliers--------------------------------------------------*
*#########################################################################################################################*/

static void SurvivalNet_HandleHello(cc_uint8* data) {
	/* [id][mode][flags][protoVer] - SurvivalNet.cs SendHello. Mode 0 = leave
	   survival (sent by the server's live /Survival config refresh). */
	cc_uint8 mode  = data[1];
	cc_uint8 flags = data[2];
	cc_uint8 proto = data[3];
	cc_string msg; char buf[STRING_SIZE];
	String_InitArray(msg, buf);

	if (mode > 2) {
		/* Unknown mode from a newer server revision - treat as off rather than
		   half-activating a sim we don't understand. */
		String_Format1(&msg, "&c[survival] unknown mode %b in HELLO - staying in classic mode", &mode);
		Chat_Add(&msg);
		mode = 0;
	} else {
		String_Format3(&msg, "&7[survival] hello: mode=%b flags=%b proto=%b", &mode, &flags, &proto);
		Chat_Add(&msg);
	}

	net_mode  = mode;
	net_flags = flags;
	/* HELLO arrives after the level, so the components already ran their map
	   hooks with mode OFF - re-derive both (Indev first: SurvivalTest's hook
	   expects the Indev flag to be settled, same as the component ordering). */
	SurvivalTest_NetworkModeChanged();
}

static void SurvivalNet_HandleWorldInfo(cc_uint8* data) {
	/* v1 layout - SurvivalNet.cs SendWorldInfo:
	   [id][ground:u8][water:u8][fluid][theme][flags(b0 floating)][sidesBlk][edgeBlk]
	   (the fuller int16 heights + env colours of §25 are a deferred revision;
	   colours ride the stock EnvColors CPE path meanwhile). */
	cc_uint8 ground = data[1];
	cc_uint8 water  = data[2];
	cc_uint8 fluid  = data[3];
	cc_uint8 theme  = data[4];
	cc_uint8 flags  = data[5];
	cc_string msg; char buf[STRING_SIZE];
	String_InitArray(msg, buf);

	String_Format4(&msg, "&7[survival] worldinfo: ground=%b water=%b theme=%b flags=%b",
	               &ground, &water, &theme, &flags);
	Chat_Add(&msg);

	/* The OOB horizon planes only exist in the Indev sim (genuine Indev draws
	   ground/fluid planes, no walls - Indev_ApplySurroundings). */
	if (SurvivalNet_ActiveMode() == SURVIVAL_GAMEMODE_INDEV) {
		IndevTest_SetSurroundings(ground, water, fluid);
		IndevTest_ApplySurroundings();
	}
}

static void SurvivalNet_HandleHealth(cc_uint8* data) {
	/* [id][health:u8][score:i32 BE] - SurvivalNet.cs SendHealth. NOTE: score is
	   int32, not the int16 first drafted in §25 - the server shipped i32 and the
	   docs now match it. */
	int health = data[1];
	int score  = (int)(((cc_uint32)data[2] << 24) | ((cc_uint32)data[3] << 16) |
	                   ((cc_uint32)data[4] << 8)  |  (cc_uint32)data[5]);
	if (SurvivalNet_ActiveMode() == 0) return;

	SurvivalTest_ApplyNetHealth(health, score);
}

static void SurvivalNet_HandleTime(cc_uint8* data) {
	/* [id][worldTime:u16 BE][skyLight:u8] - SurvivalNet.cs SendTime. The client
	   intentionally uses only worldTime: our Indev day/night port computes the
	   genuine celestial-angle sky light and colour scaling from the time itself,
	   which is more faithful than the server's coarse ramp byte. The server owns
	   the CLOCK; the client owns the genuine presentation of it. */
	int time = ((int)data[1] << 8) | data[2];

	if (SurvivalNet_ActiveMode() == SURVIVAL_GAMEMODE_INDEV) {
		IndevTest_SetWorldTime(time);
	}
	/* c0.30-s has no day/night cycle - ignore TIME in that mode. */
}

/* --- phase 3: mob streaming (0x10-0x13) --- */
/* Positions are int16 fixed-point (coord*32), yaw/pitch uint8 (deg*256/360),
   per networking-plan §25. All appliers are keyed by the server's 16-bit mob
   id; st_mobs is the puppet pool (SurvivalTest_NetMob*, §15.1/§17.5). */

static cc_int16 SurvivalNet_I16(cc_uint8* data) {
	return (cc_int16)(((cc_uint16)data[0] << 8) | data[1]);
}

static Vec3 SurvivalNet_ReadPos(cc_uint8* data) {
	Vec3 pos;
	pos.x = SurvivalNet_I16(data)     / 32.0f;
	pos.y = SurvivalNet_I16(data + 2) / 32.0f;
	pos.z = SurvivalNet_I16(data + 4) / 32.0f;
	return pos;
}

#define SurvivalNet_Angle(b) ((b) * 360.0f / 256.0f)

static void SurvivalNet_HandleMobSpawn(cc_uint8* data) {
	/* [id][mobId:u16][type][pos:3xi16][yaw][pitch][health][flags] */
	int  mobId = ((int)data[1] << 8) | data[2];
	int  type  = data[3];
	Vec3 pos   = SurvivalNet_ReadPos(data + 4);
	if (SurvivalNet_ActiveMode() == 0) return;

	SurvivalTest_NetMobSpawn(mobId, type, pos,
		SurvivalNet_Angle(data[10]), SurvivalNet_Angle(data[11]),
		data[12], data[13]);
}

static void SurvivalNet_HandleMobMove(cc_uint8* data) {
	/* [id][mobId:u16][pos:3xi16][yaw][pitch] */
	int  mobId = ((int)data[1] << 8) | data[2];
	Vec3 pos   = SurvivalNet_ReadPos(data + 3);
	if (SurvivalNet_ActiveMode() == 0) return;

	SurvivalTest_NetMobMove(mobId, pos,
		SurvivalNet_Angle(data[9]), SurvivalNet_Angle(data[10]));
}

static void SurvivalNet_HandleMobState(cc_uint8* data) {
	/* [id][mobId:u16][health][flags(b0 hurt,b1 fuse,b2 onFire,b3 graze,b4 dead,b5 noFur)] */
	int mobId = ((int)data[1] << 8) | data[2];
	if (SurvivalNet_ActiveMode() == 0) return;

	SurvivalTest_NetMobState(mobId, data[3], data[4]);
}

static void SurvivalNet_HandleMobDespawn(cc_uint8* data) {
	/* [id][mobId:u16][reason] */
	int mobId = ((int)data[1] << 8) | data[2];
	if (SurvivalNet_ActiveMode() == 0) return;

	SurvivalTest_NetMobDespawn(mobId, data[3]);
}

/* --- phase 4: inventory streaming (0x20-0x25) --- */
/* The server owns every slot and the cursor (networking-plan 27); these write
   the echoed authoritative state into st_inv/st_craft/st_armor + st_cursor. */

static void SurvivalNet_HandleInvFull(cc_uint8* data) {
	/* [id][baseSlot][runLen] then runLen x {id:u16, count:u8, dmg:i16} */
	int base = data[1], run = data[2], i;
	cc_uint8* f;
	if (SurvivalNet_ActiveMode() == 0) return;
	if (run > 12) run = 12; /* 3 + 12*5 = 63: never read past the 64-byte frame */

	for (i = 0; i < run; i++) {
		f = data + 3 + i * 5;
		SurvivalTest_NetInvSlot(base + i,
			((int)f[0] << 8) | f[1], f[2],
			(cc_int16)(((cc_uint16)f[3] << 8) | f[4]));
	}
}

static void SurvivalNet_HandleInvSlot(cc_uint8* data) {
	/* [id][slot][id:u16][count][dmg:i16] */
	if (SurvivalNet_ActiveMode() == 0) return;
	SurvivalTest_NetInvSlot(data[1],
		((int)data[2] << 8) | data[3], data[4],
		(cc_int16)(((cc_uint16)data[5] << 8) | data[6]));
}

static void SurvivalNet_HandleCursor(cc_uint8* data) {
	/* [id][id:u16][count][dmg:i16] - the server-owned held stack */
	if (SurvivalNet_ActiveMode() == 0) return;
	SurvivalTest_NetCursor(
		((int)data[1] << 8) | data[2], data[3],
		(cc_int16)(((cc_uint16)data[4] << 8) | data[5]));
}

static void SurvivalNet_OnPluginMessage(void* obj, cc_uint8 channel, cc_uint8* data) {
	if (!SurvivalNet_Active())        return;
	if (channel != SURVNET_CHANNEL)   return;

	switch (data[0]) {
	case SURV_HELLO:       SurvivalNet_HandleHello(data);      break;
	case SURV_WORLDINFO:   SurvivalNet_HandleWorldInfo(data);  break;
	case SURV_HEALTH:      SurvivalNet_HandleHealth(data);     break;
	case SURV_TIME:        SurvivalNet_HandleTime(data);       break;
	case SURV_MOB_SPAWN:   SurvivalNet_HandleMobSpawn(data);   break;
	case SURV_MOB_MOVE:    SurvivalNet_HandleMobMove(data);    break;
	case SURV_MOB_STATE:   SurvivalNet_HandleMobState(data);   break;
	case SURV_MOB_DESPAWN: SurvivalNet_HandleMobDespawn(data); break;
	case SURV_INV_FULL:    SurvivalNet_HandleInvFull(data);    break;
	case SURV_INV_SLOT:    SurvivalNet_HandleInvSlot(data);    break;
	case SURV_CURSOR:      SurvivalNet_HandleCursor(data);     break;
	/* Remaining server->client messages (containers 0x22-0x24, drops
	   0x30-0x32, blockmeta 0x40, equip 0x50) are reserved in SurvivalNet.h
	   and land with the rest of phase 4 + phase 5. */
	default: break;
	}
}


/*########################################################################################################################*
*----------------------------------------------client -> server intents---------------------------------------------------*
*#########################################################################################################################*/
/* Every sender no-ops unless the server owns the sim - a stock server never
   receives a byte on 0xB0, and singleplayer never touches the net path. */

void SurvivalNet_SendRespawn(void) {
	cc_uint8 payload[64] = { 0 };
	if (!SurvivalNet_ServerDriven()) return;
	payload[0] = SURV_RESPAWN;
	SurvivalNet_Send(payload);
}

void SurvivalNet_SendHeldSlot(int slot) {
	cc_uint8 payload[64] = { 0 };
	if (!SurvivalNet_ServerDriven()) return;
	payload[0] = SURV_HELD_SLOT;
	payload[1] = (cc_uint8)slot;
	SurvivalNet_Send(payload);
}

void SurvivalNet_SendDropItem(int slot, cc_bool whole) {
	cc_uint8 payload[64] = { 0 };
	if (!SurvivalNet_ServerDriven()) return;
	payload[0] = SURV_DROP_ITEM;
	payload[1] = (cc_uint8)slot;
	payload[2] = whole ? 1 : 0;
	SurvivalNet_Send(payload);
}

void SurvivalNet_SendAttack(int targetKind, int targetId) {
	cc_uint8 payload[64] = { 0 };
	if (!SurvivalNet_ServerDriven()) return;
	payload[0] = SURV_ATTACK;
	payload[1] = (cc_uint8)targetKind;           /* 0 mob / 1 player */
	payload[2] = (cc_uint8)(targetId >> 8);      /* target id, u16 BE */
	payload[3] = (cc_uint8)targetId;
	SurvivalNet_Send(payload);
}

void SurvivalNet_SendUseItem(int heldSlot, int x, int y, int z, int face) {
	cc_uint8 payload[64] = { 0 };
	if (!SurvivalNet_ServerDriven()) return;
	payload[0] = SURV_USE_ITEM;
	payload[1] = (cc_uint8)heldSlot;
	payload[2] = (cc_uint8)(x >> 8); payload[3] = (cc_uint8)x; /* block coords, i16 BE */
	payload[4] = (cc_uint8)(y >> 8); payload[5] = (cc_uint8)y;
	payload[6] = (cc_uint8)(z >> 8); payload[7] = (cc_uint8)z;
	payload[8] = (cc_uint8)face;
	SurvivalNet_Send(payload);
}

void SurvivalNet_SendSlotClick(int slot, int button) {
	cc_uint8 payload[64] = { 0 };
	if (!SurvivalNet_ServerDriven()) return;
	payload[0] = SURV_SLOT_CLICK;
	payload[1] = (cc_uint8)(slot >> 8);          /* slot index, u16 BE (extended slots) */
	payload[2] = (cc_uint8)slot;
	payload[3] = (cc_uint8)button;               /* 0 left / 1 right */
	SurvivalNet_Send(payload);
}

void SurvivalNet_SendResultClick(void) {
	cc_uint8 payload[64] = { 0 };
	if (!SurvivalNet_ServerDriven()) return;
	payload[0] = SURV_RESULT_CLICK;
	SurvivalNet_Send(payload);
}

void SurvivalNet_SendContClose(void) {
	cc_uint8 payload[64] = { 0 };
	if (!SurvivalNet_ServerDriven()) return;
	payload[0] = SURV_CONT_CLOSE;
	SurvivalNet_Send(payload);
}

/* Mirror hotbar selection to the server (also lets it drive HeldBlock CPE). */
static void SurvivalNet_OnHeldBlockChanged(void* obj) {
	SurvivalNet_SendHeldSlot(Inventory.SelectedIndex);
}


/*########################################################################################################################*
*--------------------------------------------------------Component--------------------------------------------------------*
*#########################################################################################################################*/
static void SurvivalNet_Reset(void) {
	/* (Re)connect: back to no activation. Server.SupportsSurvival is cleared by
	   the net layer alongside the other Supports* flags. */
	net_mode  = 0;
	net_flags = 0;
}

static void SurvivalNet_OnNewMap(void) {
	/* Per-map activation must not leak across a /goto: every map change starts
	   deactivated until (unless) the server sends a fresh SURV_HELLO after the
	   level. The components' own map hooks then read mode OFF while loading. */
	net_mode  = 0;
	net_flags = 0;
}

static void SurvivalNet_Init(void) {
	Event_Register_(&NetEvents.PluginMessageReceived, NULL, SurvivalNet_OnPluginMessage);
	Event_Register_(&UserEvents.HeldBlockChanged,     NULL, SurvivalNet_OnHeldBlockChanged);
}

struct IGameComponent SurvivalNet_Component = {
	SurvivalNet_Init,      /* Init  */
	NULL,                  /* Free  */
	SurvivalNet_Reset,     /* Reset */
	SurvivalNet_OnNewMap,  /* OnNewMap */
	NULL                   /* OnNewMapLoaded */
};
