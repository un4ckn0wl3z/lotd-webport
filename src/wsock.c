// WSOCK32 for IpDrv: UDP game traffic (TcpNetDriver), LAN discovery broadcasts (UdpBeacon / the server
// browser), host name lookups.
//
// Native builds pass every call through to the host's Winsock, so two native copies play over a real LAN.
// The web build has a virtual LAN instead: the page keeps a WebSocket to the relay (server/server.js), which gives
// it an address 10.77.x.y in a room and forwards datagrams (and broadcasts) between the pages in that room. The
// game's own Host / Join Multiplayer menus then work as on a LAN. TCP (master server, HTTP redirects) is not
// available online: those connects fail.
#include "host.h"
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>

#define WSAEWOULDBLOCK 10035
#define WSAEADDRINUSE 10048
#define WSAECONNREFUSED 10061
#define WSAENOTSOCK 10038
#define WSAEINVAL 10022
#define WSAHOST_NOT_FOUND 11001
#define FIONBIO 0x8004667eu
#define FIONREAD 0x4004667fu
#define MAXS 64
#define SOCK_BASE 0x100u

static uint32_t wsa_error;
static uint32_t hostent_mem;        // guest copy of the last hostent

// a guest hostent (name, aliases, addrtype, length, addr list) for one IPv4 address in network order
static uint32_t make_hostent(const char *name, uint32_t addr) {
    if (!hostent_mem) hostent_mem = guest_static_alloc(256);
    uint32_t h = hostent_mem;
    memset(G + h, 0, 256);
    snprintf(GSTR(h + 64), 128, "%s", name);
    W32(h, h + 64); W32(h + 4, h + 40);            // h_name, h_aliases (empty list)
    W16(h + 8, 2); W16(h + 10, 4);                  // AF_INET, 4 bytes
    W32(h + 12, h + 32);                            // h_addr_list
    W32(h + 32, h + 48); W32(h + 36, 0);
    W32(h + 48, addr);
    return h;
}
static int parse_dotted(const char *s, uint32_t *out) {
    unsigned a, b, c, d; char extra;
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &extra) != 4 || a > 255 || b > 255 || c > 255 || d > 255) return 0;
    *out = a | (b << 8) | (c << 16) | (d << 24);
    return 1;
}
static void ws_htonl(void) { uint32_t v = ARG(0); SRET((v >> 24) | ((v >> 8) & 0xff00) | ((v << 8) & 0xff0000) | (v << 24), 1); }
static void ws_htons(void) { uint32_t v = ARG(0) & 0xffff; SRET(((v >> 8) | (v << 8)) & 0xffff, 1); }
static void ws_inet_addr(void) { uint32_t a; SRET(parse_dotted(GSTR(ARG(0)), &a) ? a : 0xffffffffu, 1); }
static void ws_WSAGetLastError(void) { SRET(wsa_error, 0); }
static void ws_WSAStartup(void) {
    uint32_t d = ARG(1);
    memset(G + d, 0, 400);
    W16(d, 0x0101); W16(d + 2, 0x0202);
    snprintf(GSTR(d + 4), 257, "WinSock 2.0");
    W16(d + 4 + 257 + 129, 64); W16(d + 4 + 257 + 129 + 2, 65467);   // iMaxSockets, iMaxUdpDg
    SRET(0, 2);
}

#ifndef __EMSCRIPTEN__
// ================================================================ native: the host's Winsock
#include <winsock2.h>
#undef GPTR             // winbase.h's GlobalAlloc flag
#define GPTR(a) ((void *)(G + (a)))
static SOCKET hs[MAXS];
static int hs_used[MAXS];
static SOCKET hsock(uint32_t s) { uint32_t i = s - SOCK_BASE; return (i < MAXS && hs_used[i]) ? hs[i] : INVALID_SOCKET; }
static int last(int r) { if (r == SOCKET_ERROR) wsa_error = (uint32_t)WSAGetLastError(); return r; }
static void ws_socket(void) {
    static int started;
    if (!started) { WSADATA w; WSAStartup(0x0202, &w); started = 1; }
    SOCKET s = socket((int)ARG(0), (int)ARG(1), (int)ARG(2));
    if (s == INVALID_SOCKET) { wsa_error = (uint32_t)WSAGetLastError(); SRET(0xffffffffu, 3); }
    for (int i = 0; i < MAXS; i++) if (!hs_used[i]) { hs_used[i] = 1; hs[i] = s; SRET(SOCK_BASE + i, 3); }
    closesocket(s); wsa_error = 10024; SRET(0xffffffffu, 3);
}
static void ws_closesocket(void) {
    uint32_t i = ARG(0) - SOCK_BASE;
    if (i < MAXS && hs_used[i]) { closesocket(hs[i]); hs_used[i] = 0; SRET(0, 1); }
    wsa_error = WSAENOTSOCK; SRET(0xffffffffu, 1);
}
static void ws_bind(void) { SRET(last(bind(hsock(ARG(0)), (struct sockaddr *)GPTR(ARG(1)), (int)ARG(2))), 3); }
static void ws_connect(void) { SRET(last(connect(hsock(ARG(0)), (struct sockaddr *)GPTR(ARG(1)), (int)ARG(2))), 3); }
static void ws_listen(void) { SRET(last(listen(hsock(ARG(0)), (int)ARG(1))), 2); }
static void ws_accept(void) {
    int len = ARG(2) ? (int)R32(ARG(2)) : 0;
    SOCKET n = accept(hsock(ARG(0)), ARG(1) ? (struct sockaddr *)GPTR(ARG(1)) : NULL, ARG(2) ? &len : NULL);
    if (n == INVALID_SOCKET) { wsa_error = (uint32_t)WSAGetLastError(); SRET(0xffffffffu, 3); }
    if (ARG(2)) W32(ARG(2), (uint32_t)len);
    for (int i = 0; i < MAXS; i++) if (!hs_used[i]) { hs_used[i] = 1; hs[i] = n; SRET(SOCK_BASE + i, 3); }
    closesocket(n); SRET(0xffffffffu, 3);
}
static void ws_send(void) { SRET(last(send(hsock(ARG(0)), GSTR(ARG(1)), (int)ARG(2), (int)ARG(3))), 4); }
static void ws_recv(void) { SRET(last(recv(hsock(ARG(0)), GSTR(ARG(1)), (int)ARG(2), (int)ARG(3))), 4); }
static void ws_sendto(void) {
    SRET(last(sendto(hsock(ARG(0)), GSTR(ARG(1)), (int)ARG(2), (int)ARG(3), (struct sockaddr *)GPTR(ARG(4)), (int)ARG(5))), 6);
}
static void ws_recvfrom(void) {
    int len = ARG(5) ? (int)R32(ARG(5)) : 0;
    int r = recvfrom(hsock(ARG(0)), GSTR(ARG(1)), (int)ARG(2), (int)ARG(3), ARG(4) ? (struct sockaddr *)GPTR(ARG(4)) : NULL, ARG(5) ? &len : NULL);
    last(r);
    if (r >= 0 && ARG(5)) W32(ARG(5), (uint32_t)len);
    SRET((uint32_t)r, 6);
}
// guest fd_set (u32 count, u32 sockets[64]) <-> host fd_set
static void fdset_in(uint32_t g, fd_set *h) {
    FD_ZERO(h);
    if (!g) return;
    uint32_t n = R32(g);
    for (uint32_t i = 0; i < n && i < 64; i++) { SOCKET s = hsock(R32(g + 4 + 4 * i)); if (s != INVALID_SOCKET) FD_SET(s, h); }
}
static void fdset_out(uint32_t g, fd_set *h) {
    if (!g) return;
    uint32_t n = R32(g), k = 0;
    for (uint32_t i = 0; i < n && i < 64; i++) {
        uint32_t gs = R32(g + 4 + 4 * i);
        SOCKET s = hsock(gs);
        if (s != INVALID_SOCKET && FD_ISSET(s, h)) W32(g + 4 + 4 * k++, gs);
    }
    W32(g, k);
}
static void ws_select(void) {
    fd_set r, w, e;
    fdset_in(ARG(1), &r); fdset_in(ARG(2), &w); fdset_in(ARG(3), &e);
    struct timeval tv, *ptv = NULL;
    if (ARG(4)) { tv.tv_sec = (long)R32(ARG(4)); tv.tv_usec = (long)R32(ARG(4) + 4); ptv = &tv; }
    int n = select(0, ARG(1) ? &r : NULL, ARG(2) ? &w : NULL, ARG(3) ? &e : NULL, ptv);
    last(n);
    if (n >= 0) { fdset_out(ARG(1), &r); fdset_out(ARG(2), &w); fdset_out(ARG(3), &e); }
    SRET((uint32_t)n, 5);
}
static void ws_FDIsSet(void) {
    uint32_t s = ARG(0), g = ARG(1), n = R32(g);
    for (uint32_t i = 0; i < n && i < 64; i++) if (R32(g + 4 + 4 * i) == s) SRET(1, 2);
    SRET(0, 2);
}
static void ws_ioctlsocket(void) {
    u_long v = R32(ARG(2));
    int r = last(ioctlsocket(hsock(ARG(0)), (long)ARG(1), &v));
    W32(ARG(2), (uint32_t)v);
    SRET((uint32_t)r, 3);
}
static void ws_setsockopt(void) { SRET(last(setsockopt(hsock(ARG(0)), (int)ARG(1), (int)ARG(2), GSTR(ARG(3)), (int)ARG(4))), 5); }
static void ws_getsockopt(void) {
    int len = (int)R32(ARG(4));
    int r = last(getsockopt(hsock(ARG(0)), (int)ARG(1), (int)ARG(2), GSTR(ARG(3)), &len));
    W32(ARG(4), (uint32_t)len);
    SRET((uint32_t)r, 5);
}
static void ws_getsockname(void) {
    int len = (int)R32(ARG(2));
    int r = last(getsockname(hsock(ARG(0)), (struct sockaddr *)GPTR(ARG(1)), &len));
    W32(ARG(2), (uint32_t)len);
    SRET((uint32_t)r, 3);
}
static void ws_shutdown(void) { SRET(last(shutdown(hsock(ARG(0)), (int)ARG(1))), 2); }
static void ws_gethostname(void) {
    char name[256];
    if (gethostname(name, sizeof name)) { wsa_error = (uint32_t)WSAGetLastError(); SRET(0xffffffffu, 2); }
    if ((int32_t)ARG(1) > 0) snprintf(GSTR(ARG(0)), ARG(1), "%s", name);
    SRET(0, 2);
}
static void ws_gethostbyname(void) {
    struct hostent *he = gethostbyname(GSTR(ARG(0)));
    if (!he || he->h_addrtype != AF_INET) { wsa_error = (uint32_t)WSAGetLastError(); SRET(0, 1); }
    uint32_t a; memcpy(&a, he->h_addr_list[0], 4);
    SRET(make_hostent(he->h_name, a), 1);
}
#else
// ================================================================ web: the virtual LAN
#include <emscripten.h>
// the page's WebSocket (web/vnet.js): Module.vnet
EM_JS(uint32_t, vnet_ip, (void), { return (Module.vnet && Module.vnet.ip) >>> 0; });
EM_JS(void, vnet_send, (uint32_t dst, uint32_t dport, uint32_t sport, const void *p, int n), {
    if (Module.vnet) Module.vnet.send(dst >>> 0, dport, sport, HEAPU8.subarray(p, p + n));
});
// next datagram for this page: returns its length (-1 if none), info = src ip, src port, dst port
EM_JS(int, vnet_recv, (void *buf, int cap, uint32_t *info), {
    var m = Module.vnet && Module.vnet.inbox.shift();
    if (!m) return -1;
    var n = Math.min(m.data.length, cap);
    HEAPU8.set(m.data.subarray(0, n), buf);
    HEAPU32[info >> 2] = m.src >>> 0; HEAPU32[(info >> 2) + 1] = m.sport; HEAPU32[(info >> 2) + 2] = m.dport;
    return n;
});

#define QN 256
typedef struct { uint32_t ip, port, len; uint8_t *data; } Dgram;
static struct {
    int used, type; uint32_t port; int nonblock, connected; uint32_t peer_ip, peer_port;
    Dgram q[QN]; int qh, qt;
} vs[MAXS];
static uint32_t next_ephemeral = 50000;
#define LOOPBACK 0x0100007fu
static int is_broadcast(uint32_t ip) { return ip == 0xffffffffu || (ip & 0xff000000u) == 0xff000000u; }
static int sidx(uint32_t s) { uint32_t i = s - SOCK_BASE; return (i < MAXS && vs[i].used) ? (int)i : -1; }
static uint32_t my_ip(void) { uint32_t ip = vnet_ip(); return ip ? ip : LOOPBACK; }

static void enqueue(int i, uint32_t ip, uint32_t port, const uint8_t *d, uint32_t n) {
    int nt = (vs[i].qt + 1) % QN;
    if (nt == vs[i].qh) return;            // full: the datagram is lost, as UDP allows
    Dgram *g = &vs[i].q[vs[i].qt];
    g->ip = ip; g->port = port; g->len = n; g->data = malloc(n ? n : 1);
    memcpy(g->data, d, n);
    vs[i].qt = nt;
}
static int sock_on_port(uint32_t port) {
    for (int i = 0; i < MAXS; i++) if (vs[i].used && vs[i].type == 2 && vs[i].port == port) return i;
    return -1;
}
// move everything the relay has for us into the sockets' queues
static void vnet_poll(void) {
    static uint8_t buf[65536];
    uint32_t info[3];
    int n;
    while ((n = vnet_recv(buf, sizeof buf, info)) >= 0) {
        int i = sock_on_port(info[2]);
        if (i >= 0) enqueue(i, info[0], info[1], buf, (uint32_t)n);
    }
}
static void ws_socket(void) {
    for (int i = 0; i < MAXS; i++) if (!vs[i].used) {
        memset(&vs[i], 0, sizeof vs[i]);
        vs[i].used = 1; vs[i].type = (int)ARG(1);
        SRET(SOCK_BASE + i, 3);
    }
    wsa_error = 10024; SRET(0xffffffffu, 3);
}
static void ws_closesocket(void) {
    int i = sidx(ARG(0));
    if (i < 0) { wsa_error = WSAENOTSOCK; SRET(0xffffffffu, 1); }
    while (vs[i].qh != vs[i].qt) { free(vs[i].q[vs[i].qh].data); vs[i].qh = (vs[i].qh + 1) % QN; }
    vs[i].used = 0;
    SRET(0, 1);
}
static void ws_bind(void) {
    int i = sidx(ARG(0));
    if (i < 0) { wsa_error = WSAENOTSOCK; SRET(0xffffffffu, 3); }
    uint32_t a = ARG(1), port = ((R8(a + 2) << 8) | R8(a + 3));
    if (!port) { do port = next_ephemeral++; while (sock_on_port(port) >= 0); }
    else if (sock_on_port(port) >= 0) { wsa_error = WSAEADDRINUSE; SRET(0xffffffffu, 3); }
    vs[i].port = port;
    host_log("net: socket %u bound to port %u\n", ARG(0), port);
    SRET(0, 3);
}
static void ensure_bound(int i) {
    if (!vs[i].port) { uint32_t p; do p = next_ephemeral++; while (sock_on_port(p) >= 0); vs[i].port = p; }
}
static void send_dgram(int i, uint32_t ip, uint32_t port, uint32_t buf, uint32_t len) {
    ensure_bound(i);
    uint32_t me = my_ip();
    if (ip == me || ip == LOOPBACK || is_broadcast(ip)) {
        int j = sock_on_port(port);
        if (j >= 0 && j != i) enqueue(j, me, vs[i].port, G + buf, len);
        if (!is_broadcast(ip)) return;
    }
    vnet_send(ip, port, vs[i].port, G + buf, (int)len);
}
static void ws_sendto(void) {
    int i = sidx(ARG(0));
    if (i < 0) { wsa_error = WSAENOTSOCK; SRET(0xffffffffu, 6); }
    uint32_t a = ARG(4);
    uint32_t port = (R8(a + 2) << 8) | R8(a + 3), ip = R32(a + 4);
    send_dgram(i, ip, port, ARG(1), ARG(2));
    SRET(ARG(2), 6);
}
static void ws_send(void) {
    int i = sidx(ARG(0));
    if (i < 0 || !vs[i].connected) { wsa_error = 10057; SRET(0xffffffffu, 4); }
    send_dgram(i, vs[i].peer_ip, vs[i].peer_port, ARG(1), ARG(2));
    SRET(ARG(2), 4);
}
static void ws_recvfrom(void) {
    int i = sidx(ARG(0));
    if (i < 0) { wsa_error = WSAENOTSOCK; SRET(0xffffffffu, 6); }
    vnet_poll();
    if (vs[i].qh == vs[i].qt) { wsa_error = WSAEWOULDBLOCK; SRET(0xffffffffu, 6); }
    Dgram *g = &vs[i].q[vs[i].qh];
    uint32_t n = g->len < ARG(2) ? g->len : ARG(2);
    memcpy(G + ARG(1), g->data, n);
    if (ARG(4)) {
        uint32_t a = ARG(4);
        memset(G + a, 0, 16);
        W16(a, 2); W8(a + 2, g->port >> 8); W8(a + 3, g->port & 255); W32(a + 4, g->ip);
        if (ARG(5)) W32(ARG(5), 16);
    }
    free(g->data);
    vs[i].qh = (vs[i].qh + 1) % QN;
    SRET(n, 6);
}
static void ws_recv(void) {
    // recv on a UDP socket: the same without the address
    int i = sidx(ARG(0));
    if (i < 0) { wsa_error = WSAENOTSOCK; SRET(0xffffffffu, 4); }
    vnet_poll();
    if (vs[i].qh == vs[i].qt) { wsa_error = WSAEWOULDBLOCK; SRET(0xffffffffu, 4); }
    Dgram *g = &vs[i].q[vs[i].qh];
    uint32_t n = g->len < ARG(2) ? g->len : ARG(2);
    memcpy(G + ARG(1), g->data, n);
    free(g->data);
    vs[i].qh = (vs[i].qh + 1) % QN;
    SRET(n, 4);
}
static void ws_connect(void) {
    int i = sidx(ARG(0));
    if (i < 0) { wsa_error = WSAENOTSOCK; SRET(0xffffffffu, 3); }
    if (vs[i].type != 2) { wsa_error = WSAECONNREFUSED; SRET(0xffffffffu, 3); }    // no TCP on the virtual LAN
    uint32_t a = ARG(1);
    vs[i].peer_port = (R8(a + 2) << 8) | R8(a + 3); vs[i].peer_ip = R32(a + 4); vs[i].connected = 1;
    ensure_bound(i);
    SRET(0, 3);
}
static void ws_listen(void) { wsa_error = WSAEINVAL; SRET(0xffffffffu, 2); }
static void ws_accept(void) { wsa_error = WSAEWOULDBLOCK; SRET(0xffffffffu, 3); }
static int readable(uint32_t s) { int i = sidx(s); return i >= 0 && vs[i].qh != vs[i].qt; }
static void fd_filter(uint32_t g, int want_read, int *count) {
    if (!g) return;
    uint32_t n = R32(g), k = 0;
    for (uint32_t j = 0; j < n && j < 64; j++) {
        uint32_t s = R32(g + 4 + 4 * j);
        int ok = want_read == 1 ? readable(s) : want_read == 0 ? (sidx(s) >= 0) : 0;
        if (ok) W32(g + 4 + 4 * k++, s);
    }
    W32(g, k);
    *count += (int)k;
}
static void ws_select(void) {
    vnet_poll();
    int n = 0;
    fd_filter(ARG(1), 1, &n); fd_filter(ARG(2), 0, &n); fd_filter(ARG(3), 2, &n);
    SRET((uint32_t)n, 5);
}
static void ws_FDIsSet(void) {
    uint32_t s = ARG(0), g = ARG(1), n = R32(g);
    for (uint32_t i = 0; i < n && i < 64; i++) if (R32(g + 4 + 4 * i) == s) SRET(1, 2);
    SRET(0, 2);
}
static void ws_ioctlsocket(void) {
    int i = sidx(ARG(0));
    if (i < 0) { wsa_error = WSAENOTSOCK; SRET(0xffffffffu, 3); }
    if (ARG(1) == FIONBIO) vs[i].nonblock = R32(ARG(2)) != 0;
    else if (ARG(1) == FIONREAD) { vnet_poll(); W32(ARG(2), vs[i].qh != vs[i].qt ? vs[i].q[vs[i].qh].len : 0); }
    SRET(0, 3);
}
static void ws_setsockopt(void) { SRET(0, 5); }
static void ws_getsockopt(void) { if (ARG(3) && ARG(4) && R32(ARG(4)) >= 4) W32(ARG(3), 65536); SRET(0, 5); }
static void ws_getsockname(void) {
    int i = sidx(ARG(0));
    if (i < 0) { wsa_error = WSAENOTSOCK; SRET(0xffffffffu, 3); }
    uint32_t a = ARG(1);
    memset(G + a, 0, 16);
    W16(a, 2); W8(a + 2, vs[i].port >> 8); W8(a + 3, vs[i].port & 255); W32(a + 4, my_ip());
    if (ARG(2)) W32(ARG(2), 16);
    SRET(0, 3);
}
static void ws_shutdown(void) { SRET(0, 2); }
static void ws_gethostname(void) {
    if ((int32_t)ARG(1) > 0) snprintf(GSTR(ARG(0)), ARG(1), "lotdweb");
    SRET(0, 2);
}
static void ws_gethostbyname(void) {
    const char *n = GSTR(ARG(0));
    uint32_t a;
    if (!strcasecmp(n, "lotdweb") || !strcasecmp(n, "localhost")) SRET(make_hostent(n, my_ip()), 1);
    if (parse_dotted(n, &a)) SRET(make_hostent(n, a), 1);
    wsa_error = WSAHOST_NOT_FOUND;
    SRET(0, 1);
}
#endif

const ShimDef shims_wsock[] = {
    {"WSOCK32", "WSAStartup", ws_WSAStartup}, {"WSOCK32", "WSAGetLastError", ws_WSAGetLastError},
    {"WSOCK32", "socket", ws_socket}, {"WSOCK32", "closesocket", ws_closesocket}, {"WSOCK32", "__WSAFDIsSet", ws_FDIsSet},
    {"WSOCK32", "bind", ws_bind}, {"WSOCK32", "connect", ws_connect}, {"WSOCK32", "listen", ws_listen},
    {"WSOCK32", "accept", ws_accept}, {"WSOCK32", "send", ws_send}, {"WSOCK32", "recv", ws_recv},
    {"WSOCK32", "sendto", ws_sendto}, {"WSOCK32", "recvfrom", ws_recvfrom}, {"WSOCK32", "select", ws_select},
    {"WSOCK32", "ioctlsocket", ws_ioctlsocket}, {"WSOCK32", "setsockopt", ws_setsockopt}, {"WSOCK32", "getsockopt", ws_getsockopt},
    {"WSOCK32", "getsockname", ws_getsockname}, {"WSOCK32", "shutdown", ws_shutdown},
    {"WSOCK32", "htonl", ws_htonl}, {"WSOCK32", "ntohl", ws_htonl}, {"WSOCK32", "htons", ws_htons}, {"WSOCK32", "ntohs", ws_htons},
    {"WSOCK32", "gethostname", ws_gethostname}, {"WSOCK32", "gethostbyname", ws_gethostbyname},
    {"WSOCK32", "inet_addr", ws_inet_addr},
    {0, 0, 0}
};

// imports by ordinal from system DLLs
const char *ordinal_name(const char *dll, uint32_t ord) {
    static const struct { const char *dll; uint32_t ord; const char *name; } t[] = {
        {"WSOCK32", 1, "accept"}, {"WSOCK32", 2, "bind"}, {"WSOCK32", 3, "closesocket"}, {"WSOCK32", 4, "connect"},
        {"WSOCK32", 6, "getsockname"}, {"WSOCK32", 7, "getsockopt"}, {"WSOCK32", 8, "htonl"}, {"WSOCK32", 9, "htons"},
        {"WSOCK32", 10, "inet_addr"}, {"WSOCK32", 11, "inet_ntoa"}, {"WSOCK32", 12, "ioctlsocket"}, {"WSOCK32", 13, "listen"},
        {"WSOCK32", 14, "ntohl"}, {"WSOCK32", 15, "ntohs"}, {"WSOCK32", 16, "recv"}, {"WSOCK32", 17, "recvfrom"},
        {"WSOCK32", 18, "select"}, {"WSOCK32", 19, "send"}, {"WSOCK32", 20, "sendto"}, {"WSOCK32", 21, "setsockopt"},
        {"WSOCK32", 22, "shutdown"}, {"WSOCK32", 23, "socket"}, {"WSOCK32", 52, "gethostbyname"},
        {"WSOCK32", 57, "gethostname"}, {"WSOCK32", 111, "WSAGetLastError"}, {"WSOCK32", 115, "WSAStartup"},
        {"WSOCK32", 116, "WSACleanup"}, {"WSOCK32", 151, "__WSAFDIsSet"},
        {"OLEAUT32", 2, "SysAllocString"}, {"OLEAUT32", 6, "SysFreeString"},
        {"COMCTL32", 17, "InitCommonControls"},
    };
    for (size_t i = 0; i < sizeof t / sizeof *t; i++)
        if (t[i].ord == ord && !strncasecmp(t[i].dll, dll, strlen(t[i].dll))) return t[i].name;
    return NULL;
}
