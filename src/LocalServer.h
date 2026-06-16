#ifndef CC_LOCALSERVER_H
#define CC_LOCALSERVER_H
#include "Core.h"
CC_BEGIN_HEADER

/*
   Manages spawning and stopping a local MCGalaxy server process for LAN multiplayer.
   Copyright 2024 ClassiCube | Licensed under BSD-3
*/

#define LOCAL_WORLD_CLASSIC 0
#define LOCAL_WORLD_FLAT    1

#define LOCAL_SERVER_PORT 25565
#define LOCAL_SERVER_DIR  "server"

/* Set by --local-server argument, consumed in RunProgram */
extern cc_bool LocalServer_Requested;
extern int     LocalServer_WorldType;

/* Writes MCGalaxy config files, spawns the server process, and blocks until the
   server is accepting TCP connections. Returns 0 on success. */
cc_result LocalServer_Start(const cc_string* username, int worldType);
/* Terminates the MCGalaxy process if it is running */
void      LocalServer_Stop(void);
/* Returns true if the MCGalaxy child process is still alive */
cc_bool   LocalServer_IsRunning(void);

CC_END_HEADER
#endif
