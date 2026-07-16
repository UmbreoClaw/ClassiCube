#include "SurvivalNet.h"
#include "Server.h"
#include "Protocol.h"
#include "Event.h"
#include "Game.h"
#include "Chat.h"
#include "String_.h"
#include "Logger.h"

/* Client reference implementation of the survival multiplayer sub-protocol.
   Everything here is inert until BOTH:
     - the server negotiated the "SurvivalTest" CPE extension (Server.SupportsSurvival), and
     - we're in multiplayer (!Server.IsSinglePlayer).
   So a stock Classic/CPE server is completely unaffected: the handler early-outs
   before touching any state, and we never send on the channel unless the server
   speaks it. See doc/networking-plan.md (§3 gate, §20 gating, §25 wire format).

   This is only the HANDSHAKE FOUNDATION. Server->client messages are parsed and
   logged here so the MCGalaxy server session can validate the wire contract, but
   nothing yet flips the local simulation into "server-authoritative" mode - that
   is deliberately deferred to the integrated server session, where the sim,
   entity, and inventory ownership handover can be designed as one piece.
   Copyright 2014-2025 ClassiCube | Licensed under BSD-3
*/

/* Whether the survival sub-protocol is live for this connection. */
static cc_bool SurvivalNet_Active(void) {
	return !Server.IsSinglePlayer && Server.SupportsSurvival;
}

void SurvivalNet_Send(cc_uint8* payload) {
	if (!SurvivalNet_Active()) return;
	CPE_SendPluginMessage(SURVNET_CHANNEL, payload);
}

/* --- server -> client handshake parsing (foundation only) --- */

static void SurvivalNet_HandleHello(cc_uint8* data) {
	/* [id][mode][flags][protoVer] - see §25. Logged only for now; the sim
	   handover (survivalWorld on/off, HUD, ownership) is deferred. */
	cc_uint8 mode    = data[1];
	cc_uint8 flags   = data[2];
	cc_uint8 proto   = data[3];
	cc_string msg; char buf[STRING_SIZE];
	String_InitArray(msg, buf);

	String_Format3(&msg, "&7[survival] hello: mode=%b flags=%b proto=%b", &mode, &flags, &proto);
	Chat_Add(&msg);
}

static void SurvivalNet_HandleWorldInfo(cc_uint8* data) {
	/* [id][ground][water][fluid][theme][flags]... - see §25. Foundation stub. */
	cc_uint8 ground = data[1];
	cc_uint8 water  = data[2];
	cc_string msg; char buf[STRING_SIZE];
	String_InitArray(msg, buf);

	String_Format2(&msg, "&7[survival] worldinfo: ground=%b water=%b", &ground, &water);
	Chat_Add(&msg);
}

static void SurvivalNet_OnPluginMessage(void* obj, cc_uint8 channel, cc_uint8* data) {
	if (!SurvivalNet_Active())        return;
	if (channel != SURVNET_CHANNEL)   return;

	switch (data[0]) {
	case SURV_HELLO:     SurvivalNet_HandleHello(data);     break;
	case SURV_WORLDINFO: SurvivalNet_HandleWorldInfo(data); break;
	/* Remaining server->client messages (mobs, inventory, drops, ...) are
	   defined in SurvivalNet.h and handled in the integrated server session. */
	default: break;
	}
}

static void SurvivalNet_Reset(void) {
	/* Nothing persistent yet; Server.SupportsSurvival is cleared by the net
	   layer on (re)connect alongside the other Supports* flags. */
}

static void SurvivalNet_Init(void) {
	Event_Register_(&NetEvents.PluginMessageReceived, NULL, SurvivalNet_OnPluginMessage);
}

struct IGameComponent SurvivalNet_Component = {
	SurvivalNet_Init,  /* Init  */
	NULL,              /* Free  */
	SurvivalNet_Reset, /* Reset */
	NULL,              /* OnNewMap */
	NULL               /* OnNewMapLoaded */
};
