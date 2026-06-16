#ifndef CC_LANDISCOVERY_H
#define CC_LANDISCOVERY_H
#include "Core.h"
CC_BEGIN_HEADER

/*
   Broadcasts LAN presence via UDP so other players on the same network can discover the game.
   Packet format: "CLASSICUBE_LAN|[port]|[motd]\n"
   Copyright 2024 ClassiCube | Licensed under BSD-3
*/

#define LAN_BROADCAST_PORT     4445
#define LAN_BROADCAST_INTERVAL 1500 /* ms between beacons */
#define LAN_MAGIC              "CLASSICUBE_LAN"

/* Starts the background broadcast thread. gamePort is the MCGalaxy TCP port. */
void LANDiscovery_StartBroadcast(int gamePort, const cc_string* motd);
/* Signals the broadcast thread to stop and waits for it to exit. */
void LANDiscovery_StopBroadcast(void);
/* Returns true if a broadcast thread is currently running. */
cc_bool LANDiscovery_IsBroadcasting(void);

CC_END_HEADER
#endif
