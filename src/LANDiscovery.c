#include "LANDiscovery.h"
#include "String_.h"
#include "Platform.h"
#include "Logger.h"

static cc_bool  lan_broadcasting;
static int      lan_game_port;
static char     lan_motd_buf[64];
static cc_string lan_motd = String_FromArray(lan_motd_buf);

cc_bool LANDiscovery_IsBroadcasting(void) { return lan_broadcasting; }

/* Platform-specific UDP broadcast */
#if defined CC_BUILD_WIN
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <winsock2.h>
  typedef SOCKET udp_sock_t;
  #define UDP_INVALID INVALID_SOCKET
  static void udp_close(udp_sock_t s) { closesocket(s); }
#elif !defined CC_BUILD_MOBILE && !defined CC_BUILD_CONSOLE && !defined CC_BUILD_WEB
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  typedef int udp_sock_t;
  #define UDP_INVALID (-1)
  static void udp_close(udp_sock_t s) { close(s); }
  #define LAN_POSIX
#endif

#if defined CC_BUILD_WIN || defined LAN_POSIX

static udp_sock_t LAN_CreateSocket(void) {
	udp_sock_t sock;
	int yes = 1;

	sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (sock == UDP_INVALID) return UDP_INVALID;

	setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (const char*)&yes, sizeof(yes));
	return sock;
}

static void LAN_SendBeacon(udp_sock_t sock) {
	struct sockaddr_in dst;
	char pkt[128];
	char portStr[12]; cc_string portBuf;
	int pktLen = 0;

	/* Build "CLASSICUBE_LAN|[port]|[motd]\n" */
	Mem_Copy(pkt + pktLen, LAN_MAGIC, sizeof(LAN_MAGIC) - 1);
	pktLen += sizeof(LAN_MAGIC) - 1;
	pkt[pktLen++] = '|';

	String_InitArray(portBuf, portStr);
	String_AppendInt(&portBuf, lan_game_port);
	Mem_Copy(pkt + pktLen, portBuf.buffer, portBuf.length);
	pktLen += portBuf.length;
	pkt[pktLen++] = '|';

	Mem_Copy(pkt + pktLen, lan_motd.buffer, lan_motd.length);
	pktLen += lan_motd.length;
	pkt[pktLen++] = '\n';

	Mem_Set(&dst, 0, sizeof(dst));
	dst.sin_family      = AF_INET;
	dst.sin_port        = htons(LAN_BROADCAST_PORT);
	dst.sin_addr.s_addr = INADDR_BROADCAST;

	sendto(sock, pkt, pktLen, 0, (struct sockaddr*)&dst, sizeof(dst));
}

static void LAN_BroadcastThread(void) {
	udp_sock_t sock = LAN_CreateSocket();

	while (lan_broadcasting) {
		if (sock != UDP_INVALID) LAN_SendBeacon(sock);
		Thread_Sleep(LAN_BROADCAST_INTERVAL);
	}

	if (sock != UDP_INVALID) udp_close(sock);
}

static void* broadcast_handle;

void LANDiscovery_StartBroadcast(int gamePort, const cc_string* motd) {
	if (lan_broadcasting) return;

	lan_game_port   = gamePort;
	lan_motd.length = 0;
	String_AppendString(&lan_motd, motd);

	lan_broadcasting = true;
	Thread_Run(&broadcast_handle, LAN_BroadcastThread, 65536, "LAN broadcast");
	Thread_Detach(broadcast_handle);
}

void LANDiscovery_StopBroadcast(void) {
	lan_broadcasting = false;
}

#else
void LANDiscovery_StartBroadcast(int gamePort, const cc_string* motd) { }
void LANDiscovery_StopBroadcast(void) { }
#endif
