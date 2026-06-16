#include "LocalServer.h"
#include "String_.h"
#include "Platform.h"
#include "Stream.h"
#include "Utils.h"
#include "Errors.h"
#include "Logger.h"

cc_bool LocalServer_Requested;
int     LocalServer_WorldType = LOCAL_WORLD_CLASSIC;

/* Platform-specific process tracking */
#if defined CC_BUILD_WIN
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  static PROCESS_INFORMATION srv_proc;
  static cc_bool srv_proc_valid;
#elif !defined CC_BUILD_MOBILE && !defined CC_BUILD_CONSOLE && !defined CC_BUILD_WEB
  #include <errno.h>
  #include <unistd.h>
  #include <signal.h>
  #include <sys/types.h>
  #include <sys/wait.h>
  static pid_t srv_pid = -1;
  #define LOCALSERVER_POSIX
#endif


/*########################################################################################################################*
*--------------------------------------------------Config file writing----------------------------------------------------*
*#########################################################################################################################*/
static cc_result WriteServerProps(const cc_string* username, int worldType) {
	cc_string buf, path; char bufData[512];
	String_InitArray(buf, bufData);

	String_AppendConst(&buf, "name = LAN Game\r\n");
	String_AppendConst(&buf, "motd = A local ClassiCube game\r\n");
	String_AppendConst(&buf, "max-players = 16\r\n");
	String_AppendConst(&buf, "port = 25565\r\n");
	String_AppendConst(&buf, "verify-names = false\r\n");
	String_AppendConst(&buf, "public = false\r\n");
	String_AppendConst(&buf, "main-level = main\r\n");
	String_AppendConst(&buf, "default-map-theme = ");
	String_AppendConst(&buf, worldType == LOCAL_WORLD_FLAT ? "flat" : "normal");
	String_AppendConst(&buf, "\r\n");
	String_Format1(&buf, "owner = %s\r\n", username);

	path = String_FromConst(LOCAL_SERVER_DIR "/properties/server.properties");
	return Stream_WriteAllTo(&path, (const cc_uint8*)buf.buffer, buf.length);
}

static cc_result WriteRankProps(void) {
	cc_string buf, path; char bufData[256];
	String_InitArray(buf, bufData);

	/* Guest can build but run no commands */
	String_AppendConst(&buf, "guest = 0\r\n");
	/* Owner is highest in-game rank */
	String_AppendConst(&buf, "owner = 120\r\n");

	path = String_FromConst(LOCAL_SERVER_DIR "/properties/ranks.properties");
	return Stream_WriteAllTo(&path, (const cc_uint8*)buf.buffer, buf.length);
}

static cc_result LocalServer_WriteConfigs(const cc_string* username, int worldType) {
	cc_result res;
	Utils_EnsureDirectory(LOCAL_SERVER_DIR);
	Utils_EnsureDirectory(LOCAL_SERVER_DIR "/properties");

	if ((res = WriteServerProps(username, worldType))) return res;
	if ((res = WriteRankProps()))                      return res;
	return 0;
}


/*########################################################################################################################*
*---------------------------------------------------TCP readiness check---------------------------------------------------*
*#########################################################################################################################*/
static cc_bool LocalServer_CanConnect(void) {
	static const cc_string addr = String_FromConst("127.0.0.1");
	cc_sockaddr addrs[SOCKET_MAX_ADDRS];
	cc_socket sock;
	int numAddrs;
	cc_bool canWrite;
	cc_result res;

	if (Socket_ParseAddress(&addr, LOCAL_SERVER_PORT, addrs, &numAddrs)) return false;
	if (numAddrs == 0)                                                    return false;
	if (Socket_Create(&sock, &addrs[0]))                                  return false;

	Socket_SetNonBlocking(sock, true);
	Socket_Connect(sock, &addrs[0]);
	res = Socket_Poll(sock, 300, SOCKET_POLL_WRITE, &canWrite);
	Socket_Close(sock);
	return !res && canWrite;
}

static cc_result LocalServer_WaitReady(void) {
	int i;
	for (i = 0; i < 60; i++) { /* up to 30 seconds */
		Thread_Sleep(500);
		if (LocalServer_CanConnect()) return 0;
	}
	return ERR_INVALID_ARGUMENT;
}


/*########################################################################################################################*
*---------------------------------------------------Platform: Windows-----------------------------------------------------*
*#########################################################################################################################*/
#if defined CC_BUILD_WIN

static cc_result LocalServer_Spawn(void) {
	STARTUPINFOA si;
	char cmdline[] = "MCGalaxy_.exe";

	Mem_Set(&si,       0, sizeof(si));
	Mem_Set(&srv_proc, 0, sizeof(srv_proc));
	si.cb = sizeof(si);

	if (!CreateProcessA(NULL, cmdline, NULL, NULL, FALSE, 0, NULL,
		LOCAL_SERVER_DIR, &si, &srv_proc))
		return GetLastError();

	srv_proc_valid = true;
	return 0;
}

cc_bool LocalServer_IsRunning(void) {
	DWORD code;
	if (!srv_proc_valid) return false;
	if (!GetExitCodeProcess(srv_proc.hProcess, &code)) return false;
	return code == STILL_ACTIVE;
}

void LocalServer_Stop(void) {
	if (!srv_proc_valid) return;
	TerminateProcess(srv_proc.hProcess, 0);
	WaitForSingleObject(srv_proc.hProcess, 3000);
	CloseHandle(srv_proc.hProcess);
	CloseHandle(srv_proc.hThread);
	srv_proc_valid = false;
}


/*########################################################################################################################*
*----------------------------------------------------Platform: POSIX------------------------------------------------------*
*#########################################################################################################################*/
#elif defined LOCALSERVER_POSIX

static cc_result LocalServer_Spawn(void) {
	pid_t pid = fork();
	if (pid == -1) return errno;

	if (pid == 0) {
		/* child: cd into server dir then try standalone binary, fall back to dotnet */
		if (chdir(LOCAL_SERVER_DIR) != 0) _exit(127);
		execl("./MCGalaxy_", "MCGalaxy_", (char*)NULL);
		execl("/usr/bin/env", "env", "dotnet", "MCGalaxy_.dll", (char*)NULL);
		_exit(127);
	}

	srv_pid = pid;
	return 0;
}

cc_bool LocalServer_IsRunning(void) {
	int status;
	if (srv_pid == -1) return false;
	return waitpid(srv_pid, &status, WNOHANG) == 0;
}

void LocalServer_Stop(void) {
	if (srv_pid == -1) return;
	kill(srv_pid, SIGTERM);
	waitpid(srv_pid, NULL, 0);
	srv_pid = -1;
}


/*########################################################################################################################*
*---------------------------------------------------Platform: unsupported-------------------------------------------------*
*#########################################################################################################################*/
#else

static cc_result LocalServer_Spawn(void) { return ERR_NOT_SUPPORTED; }
cc_bool LocalServer_IsRunning(void)      { return false; }
void    LocalServer_Stop(void)           { }

#endif


/*########################################################################################################################*
*-----------------------------------------------------Public API----------------------------------------------------------*
*#########################################################################################################################*/
cc_result LocalServer_Start(const cc_string* username, int worldType) {
	cc_result res;
	if (LocalServer_IsRunning()) return 0;

	if ((res = LocalServer_WriteConfigs(username, worldType))) return res;
	if ((res = LocalServer_Spawn()))                          return res;
	if ((res = LocalServer_WaitReady())) {
		LocalServer_Stop();
		return res;
	}
	return 0;
}
