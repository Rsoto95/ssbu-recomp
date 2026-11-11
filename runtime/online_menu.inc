/* Included by nnnet.c: the game's own Online menu (Battle Arenas) against our servers, phase 0 (discovery spike).
 * Only with SSBU_ONLINE_MENU=1; registered by online_menu_register() from nnnet_register_final(), after every other table,
 * so these REPLACE (and mostly wrap) the handlers of nnfs.c, nnnet_c4a.inc and nnsvc_c5a.inc.
 *
 * What it models: the console is connected (nn::nifm accepts the request; the address is loopback) and the user has a
 * network service account (nn::account: available, an ID token, a finished EnsureIdTokenCacheAsync). Every name the game
 * resolves is logged; a name under nintendo.net is answered with OUR server (SSBU_ONLINE_MENU_SERVER, default
 * 127.0.0.1): the game must never reach a server of Nintendo. Sockets, sends and nn::ssl host names are logged, with the
 * first bytes of the first packets, to learn what the Online menu speaks (PRUDP over UDP? TLS?).
 * Run it with online play OFF (SSBU_ONLINE=off): the offline sockets (real Winsock, loopback only) carry the traffic to a
 * server on this PC (tools/net/nex_probe.py). See docs/ONLINE_MENU_RE.md. */
#define OM_TOKEN_MAGIC 0x6f6d746bu                         /* "omtk": stands for the IAsyncContext of EnsureIdTokenCacheAsync */
static char g_om_server[64] = "127.0.0.1";
static volatile LONG g_om_packets, g_om_udp;               /* nn::ssl / UDP packets whose first bytes were logged */
#define OM_PACKETS_LOGGED 64
#define OM_UDP_LOGGED 20000
/* Pia's NAT check asks nncs1 and nncs2 and compares the answers: they must be two addresses. nncs2 -> the second one
 * (SSBU_ONLINE_MENU_SERVER2, default 127.0.0.2: loopback too on Windows). */
static char g_om_server2[64] = "127.0.0.2";
/* (2026-11-02) A server on another PC has ONE address: nncs2 = nncs1 made Pia give up at once (2618-0006, before a single
 * socket was opened). The game is told nncs2 is OM_NNCS2_VIRTUAL; what it sends there goes to our server (port 10025 ->
 * 39599, already open on the server, 10125 -> 10126, any other the same) and what comes back from those ports is shown as
 * coming from the virtual address (tools/net/nex_server.py: the same mapping). */
#define OM_NNCS2_VIRTUAL "198.18.0.2"
static uint32_t g_om_v2, g_om_v2_server;                   /* network order; 0 = no virtual nncs2 */
static unsigned om_v2_port_out(unsigned p) { return p == 10025 ? 39599 : p == 10125 ? 10126 : p; }
static unsigned om_v2_port_in(unsigned p) { return p == 39599 ? 10025 : p == 10126 ? 10125 : 0; }
static ImportFn g_om_prev_gai2, g_om_prev_gainn, g_om_prev_ghbn, g_om_prev_resolve, g_om_prev_connect, g_om_prev_sendto,
                g_om_prev_send, g_om_prev_socket, g_om_prev_ssl_host, g_om_prev_primary_ip, g_om_prev_bind, g_om_prev_recvfrom,
                g_om_prev_bind2, g_om_prev_recvfrom2;
static volatile LONG g_om_rx;                              /* UDP packets received whose first bytes were logged */
#define OM_RX_LOGGED 20000

/* (2026-11-02) Pia drops 127.x from station URLs (a console never has a loopback address): two copies of the game on one
 * PC, which reach each other over 127.0.0.1, then had no address for each other and never sent a packet (2618-0521). The
 * game is told its address is SSBU_ONLINE_MENU_FAKEIP (default 10.200.0.1; "off" = none): its primary IP, and the source
 * of what other players' sockets send it; a datagram TO that address goes to 127.0.0.1. The servers' ports (NAT check
 * 10025 / 10125, reachability 33334 / 33335) are left alone: their answers must come from the address the game asked.
 * tools/net/nex_server.py --fake-ip reports the same address in the NAT check and the station URLs. Test on one PC only:
 * with two PCs each game has a real address. */
static uint32_t g_om_fake;                                 /* network order; 0 = off (any value: on) */
static int om_server_port(unsigned port) { return port == 10025 || port == 10125 || port == 33334 || port == 33335 || port == 443; }
/* (2026-11-02, second round) ONE fake address for both copies made the host take the joiner for a station it already
 * had (its own address): Pia's ConnectionRequest handler [0x710019d654] looks the requester's public address up among
 * its stations, finds itself, and denies (ConnectionResponse result 3 [0x710019dfd0]; the joiner: 2618-0502). So every
 * socket of a copy is seen at 10.200.<port / 256>.<port % 256> (from its own port: no shared table needed, and
 * tools/net/nex_server.py --fake-ip derives the same in the NAT check and the station URLs); the copy's own primary IP
 * is 10.201.x.y (from its player id). A datagram to 10.200.0.0/15 goes to 127.0.0.1, same port. */
static int om_is_fake(uint32_t a) { const uint8_t *b = (const uint8_t *)&a; return b[0] == 10 && (b[1] == 200 || b[1] == 201); }
static uint32_t om_fake_for_port(unsigned port) { uint8_t b[4] = {10, 200, (uint8_t)(port >> 8), (uint8_t)port}; uint32_t a; memcpy(&a, b, 4); return a; }

static void om_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    printf("[online-menu] ");
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
    fflush(stdout);
}
static int om_packet_log(void) {                           /* SSBU_ONLINE_MENU_LOG=1: every packet's bytes (slow: a printf + flush in Pia's threads) */
    static int on = -1;
    if (on < 0) { const char *e = getenv("SSBU_ONLINE_MENU_LOG"); on = e && e[0] == '1'; }
    return on;
}
static void om_hex_n(volatile LONG *count, LONG most, const char *what, uint64_t buf, uint64_t len) {
    /* (2026-11-02) off by default: in a Battle Arena's fight with rollback the log of every Pia packet (whole, in hex, one
     * flush each) kept Pia's threads from reaching a wait in time: the ring's captures took ~85 ms and rollback gave up */
    if (!om_packet_log() && count != &g_om_packets) return;
    if (InterlockedIncrement(count) > most || !buf) return;
    static __declspec(thread) char t[3 * 1500 + 1];
    const uint8_t *b0 = (const uint8_t *)(uintptr_t)buf;
    int pia = len >= 4 && b0[0] == 0x32 && b0[1] == 0xab && b0[2] == 0x98 && b0[3] == 0x64;     /* Pia packets whole (decrypted offline) */
    int cap = pia ? 1500 : 48;
    int n = len < (uint64_t)cap ? (int)len : cap, k = 0;
    for (int i = 0; i < n; i++) k += sprintf(t + k, "%02x ", ((const uint8_t *)(uintptr_t)buf)[i]);
    t[k] = 0;
    om_log("%s %llu bytes: %s%s", what, (unsigned long long)len, t, len > (uint64_t)cap ? "..." : "");
}
static void om_hex(const char *what, uint64_t buf, uint64_t len) { om_hex_n(&g_om_packets, OM_PACKETS_LOGGED, what, buf, len); }
static const char *om_server_for(const char *name) {      /* our address for a nintendo.net name */
    return !_strnicmp(name, "nncs2", 5) ? g_om_server2 : g_om_server;
}
static void om_addr_text(uint64_t sa, char *out, size_t n) {
    struct sockaddr_in a;
    if (!sa || !so_addr_in(sa, &a)) { snprintf(out, n, "(none)"); return; }
    char ip[32] = "";
    InetNtopA(AF_INET, &a.sin_addr, ip, sizeof ip);
    snprintf(out, n, "%s:%u", ip, ntohs(a.sin_port));
}
static int om_ours(const char *name) {                     /* a name answered with our server */
    size_t l = name ? strlen(name) : 0;
    return l >= 12 && !_stricmp(name + l - 12, "nintendo.net");
}

/* ---------------------------------------------------------------- nn::nifm: connected */
/* (2026-11-05, owner: "with an old version the Online button must not let you in") menu.cpp asks GitHub at the start (and
 * again at every login): a game program that is not the newest of its branch answers "no internet" here, the game's own
 * offline path (as with the Online menu off), and a message says how to update. SSBU_VERSION_CHECK=0: off (tests). */
int menu_online_blocked(void); void menu_online_entered(void);
static void om_nifm_true(Ctx *c) { RET(menu_online_blocked() ? 0 : 1); }
static void om_nifm_false(Ctx *c) { RET(0); }
static void om_nifm_keep(Ctx *c) { (void)c; }
static uint64_t om_nsa_id(void);
static void om_nifm_GetCurrentPrimaryIpAddress(Ctx *c) {
    uint64_t out = A0;
    g_om_prev_primary_ip(c);
    if ((uint32_t)c->x[0] == 0) {
        if (g_om_fake) { uint64_t id = om_nsa_id(); uint8_t b[4] = {10, 201, (uint8_t)(id >> 16), (uint8_t)(id >> 8) | 1}; uint32_t a; memcpy(&a, b, 4); M32(out) = a; }
        else {                                             /* (2026-11-02) this PC's address toward our server (its Radmin / LAN address), else loopback */
            static uint32_t mine;
            if (!mine) { SOCKET u = socket(AF_INET, SOCK_DGRAM, 0); struct sockaddr_in t = {0}, l = {0}; int ln = sizeof l;
                t.sin_family = AF_INET; t.sin_port = htons(443); InetPtonA(AF_INET, g_om_server, &t.sin_addr);
                if (u != INVALID_SOCKET && connect(u, (struct sockaddr *)&t, sizeof t) == 0 && getsockname(u, (struct sockaddr *)&l, &ln) == 0) mine = l.sin_addr.s_addr;
                if (u != INVALID_SOCKET) closesocket(u);
                if (!mine) mine = htonl(INADDR_LOOPBACK);
                { char ip[32]; InetNtopA(AF_INET, &mine, ip, sizeof ip); om_log("this PC's address for the other players: %s (toward our server %s)", ip, g_om_server); } }
            M32(out) = mine;
        }
    }
}

/* ---------------------------------------------------------------- nn::account: a network service account */
uint64_t net_copy_id(void);
static uint64_t om_nsa_id(void) {                          /* a stable id per player: FNV-1a of the nickname (phase 1: the ladder id) */
    /* (2026-11-09) and of this copy's own random id (net_relay.c net_copy_id): the Windows user name alone made two PCs
     * of one owner the same player (both "Rober": the arena server took the join for the creator itself). Kept for the
     * whole run, so the id never changes under the arena server. */
    static uint64_t id;
    if (id) return id;
    const char *e = getenv("SSBU_NICKNAME");
    char name[64];
    DWORD n = sizeof name;
    if (!(e && *e)) { if (!GetUserNameA(name, &n)) strcpy(name, "Player"); e = name; }
    uint64_t h = 0xcbf29ce484222325ull;
    for (; *e; e++) { h ^= (uint8_t)*e; h *= 0x100000001b3ull; }
    uint64_t copy = net_copy_id();
    for (int i = 0; i < 8; i++) { h ^= (uint8_t)(copy >> (8 * i)); h *= 0x100000001b3ull; }
    /* the Windows install's own id too: a copy of the game folder (graphics.ini with it) taken to another PC is another player */
    char guid[64] = "";
#ifdef _WIN32
    DWORD gl = sizeof guid;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\Microsoft\\Cryptography", "MachineGuid", RRF_RT_REG_SZ | RRF_SUBKEY_WOW6464KEY,
                     NULL, guid, &gl) != ERROR_SUCCESS) {
        gl = sizeof guid;
        if (!GetComputerNameA(guid, &gl)) guid[0] = 0;
    }
#endif
    for (const char *g = guid; *g; g++) { h ^= (uint8_t)*g; h *= 0x100000001b3ull; }
    id = h | 1;
    return id;
}
static void om_acc_IsNsaAvailable(Ctx *c) { wr8(A0, 1); RET(0); }
static void om_acc_ok(Ctx *c) { RET(0); }
static void om_acc_GetNsaId(Ctx *c) { wr64(A0, om_nsa_id()); RET(0); }
static void om_acc_LoadCachedNintendoAccountInfo(Ctx *c) {
    om_log("nn::account::LoadCachedNintendoAccountInfo (info 0x%llx, buffer 0x%llx, %llu bytes): answered 0, nothing written",
           (unsigned long long)A0, (unsigned long long)A2, (unsigned long long)c->x[3]);
    RET(0);
}
/* (size_t *outLen, char *buf, size_t bufLen, const UserHandle &): a JWT-shaped token (header.payload.signature, base64url);
 * the signature is not valid: only our server reads it. */
static void om_acc_LoadIdTokenCache(Ctx *c) {
    uint64_t out_len = A0, buf = A1, cap = A2;
    char tok[256];
    int n = snprintf(tok, sizeof tok, "eyJhbGciOiJub25lIn0.%016llx.c3NidS1yZWNvbXA", (unsigned long long)om_nsa_id());
    if (!buf || cap < (uint64_t)n + 1) { om_log("nn::account::LoadNetworkServiceAccountIdTokenCache: buffer of %llu bytes too small", (unsigned long long)cap); RET(0x1907cu); return; }
    memcpy((void *)(uintptr_t)buf, tok, (size_t)n + 1);
    if (out_len) wr64(out_len, (uint64_t)n);
    om_log("nn::account: ID token handed to the game (%d bytes)", n);
    RET(0);
}
/* AsyncContext {vtable, 8 + 8 bytes, IAsyncContext * @0x18} (nnsvc_c5a.inc): EnsureIdTokenCacheAsync stores a finished
 * request; HasDone true, GetResult 0, the event signalled. */
static uint32_t g_om_token_request = OM_TOKEN_MAGIC;
static int om_ctx_ours(uint64_t ctx) { return rd64(ctx + 0x18) == P(&g_om_token_request); }
static void om_acc_EnsureIdTokenCacheAsync(Ctx *c) {
    uint64_t ctx = A0;
    if (rd64(ctx + 0x18) && !om_ctx_ours(ctx)) guest_trap("nn::account::AsyncContext: a request pointer this model did not make", ctx);
    wr64(ctx + 0x18, P(&g_om_token_request));
    om_log("nn::account::EnsureNetworkServiceAccountIdTokenCacheAsync: finished at once");
    menu_online_entered();                                 /* (2026-11-05) the version asked again (a release out during the session) */
    RET(0);
}
static void om_acc_Async_HasDone(Ctx *c) { wr8(A1, om_ctx_ours(A0) ? 1 : 0); RET(0); }
static void om_acc_Async_GetResult(Ctx *c) { RET(0); }
static void om_acc_Async_Cancel(Ctx *c) { RET(0); }
static void om_acc_Async_GetSystemEvent(Ctx *c) { os_init_event(A1, 0); os_signal_event(A1); RET(0); }
static void om_acc_Async_move_assign(Ctx *c) {
    uint64_t s = A0, o = A1;
    uint64_t a = rd64(o + 8), b = rd64(o + 0x10), p = rd64(o + 0x18);
    wr64(o + 8, 0); wr64(o + 0x10, 0); wr64(o + 0x18, 0);
    wr64(s + 8, a); wr64(s + 0x10, b); wr64(s + 0x18, p);
    RET(s);
}
static void om_acc_Async_dtor(Ctx *c) { uint64_t s = A0; wr64(s, c5a_acc_vtable()); wr64(s + 0x18, 0); RET(s); }

/* ---------------------------------------------------------------- names: logged; nintendo.net -> our server */
static void om_gai(Ctx *c, ImportFn prev) {               /* (node, service, hints, out) */
    static __declspec(thread) char t_node[64];
    const char *node = (const char *)(uintptr_t)A0, *service = (const char *)(uintptr_t)A1;
    if (node && om_ours(node)) {
        om_log("GetAddrInfo \"%s\" port %s -> %s (our server)", node, service ? service : "-", om_server_for(node));
        snprintf(t_node, sizeof t_node, "%s", om_server_for(node));
        c->x[0] = P(t_node);
    } else om_log("GetAddrInfo \"%s\" port %s: not ours, left to the offline resolver", node ? node : "-", service ? service : "-");
    prev(c);
}
static void om_GetAddrInfo2(Ctx *c) { om_gai(c, g_om_prev_gai2); }
static void om_GetAddrInfoNn(Ctx *c) { om_gai(c, g_om_prev_gainn); }
static void om_GetHostByNameWithOptions(Ctx *c) {
    static __declspec(thread) char t_node[64];
    const char *name = (const char *)(uintptr_t)A0;
    if (name && om_ours(name)) {
        om_log("GetHostByName \"%s\" -> %s (our server)", name, om_server_for(name));
        snprintf(t_node, sizeof t_node, "%s", om_server_for(name));
        c->x[0] = P(t_node);
    } else om_log("GetHostByName \"%s\": not ours, left to the offline resolver", name ? name : "-");
    g_om_prev_ghbn(c);
}
static void om_nsd_ResolveEx(Ctx *c) {
    uint64_t out = A0;
    char in[0x100];
    memcpy(in, (const void *)(uintptr_t)A1, sizeof in);
    in[sizeof in - 1] = 0;
    g_om_prev_resolve(c);
    om_log("nsd::ResolveEx \"%s\" -> \"%s\"", in, (const char *)(uintptr_t)out);
}

/* ---------------------------------------------------------------- (2026-11-02) which Battle Arena this player is in
 * tools/net/nex_server.py UDP 39599: "ARENA?" + pid u64 -> "ARENA!" + gid u32 + n u8 + n x pid u64 (little-endian). For the
 * rollback of an arena's match (net_session.c): the opponent is looked for in the lobby with the arena as the key. 1 = asked
 * and answered (gid 0: in none); 0 = the Online menu is off or the server did not answer (3 tries, 300 ms each). */
static int g_om_on;
/* nnnet_c4a.inc so_sendto_player: 1 = openers for this address: the Online menu is on our servers and the address is a
 * player's public one (not our servers', not a private, loopback, link-local, virtual 198.18/15 or multicast address) */
static int om_openers_for(uint32_t be_ip) {
    if (!g_om_on) return 0;
    uint32_t ip = ntohl(be_ip), b0 = ip >> 24, b1 = ip >> 16 & 255;
    if (b0 == 0 || b0 == 10 || b0 == 127 || b0 >= 224 || (b0 == 172 && b1 >= 16 && b1 < 32) || (b0 == 192 && b1 == 168) || (b0 == 169 && b1 == 254) || (b0 == 198 && (b1 & 0xfe) == 18)) return 0;
    struct in_addr a;
    if (InetPtonA(AF_INET, g_om_server, &a) == 1 && a.s_addr == be_ip) return 0;
    if (InetPtonA(AF_INET, g_om_server2, &a) == 1 && a.s_addr == be_ip) return 0;
    return 1;
}
/* (2026-11-02, F1 > Experimental) the server's state: "STATUS?" -> "STATUS!" + players online u16 + arenas u8 + per
 * arena code[5] + members u8 + most u8 (tools/net/nex_server.py). Asked every 3 s by a thread of its own while F1 shows it
 * (online_menu_status called in the last 2 s): the menu never waits for the network. */
typedef struct { char code[6]; int members, most; } OmArenaRow;
static SRWLOCK g_om_st_lock = SRWLOCK_INIT;
static int g_om_st_state;                                 /* 0 not asked yet, 1 answered, 2 no answer */
static int g_om_st_players, g_om_st_n, g_om_st_ms; static OmArenaRow g_om_st_rows[32];
static volatile LONG64 g_om_st_wanted; static volatile LONG g_om_st_thread;
static DWORD WINAPI om_status_thread(LPVOID unused) {
    (void)unused;
    for (;;) {
        if (GetTickCount64() - (ULONGLONG)g_om_st_wanted > 2000) { Sleep(200); continue; }
        SOCKET u = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (u == INVALID_SOCKET) { Sleep(3000); continue; }
        struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons(39599);
        InetPtonA(AF_INET, g_om_server, &a.sin_addr);
        DWORD to = 1500; setsockopt(u, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof to);
        ULONGLONG t0 = GetTickCount64();
        sendto(u, "STATUS?", 7, 0, (struct sockaddr *)&a, sizeof a);
        uint8_t b[600]; int n = recv(u, (char *)b, sizeof b, 0);
        closesocket(u);
        AcquireSRWLockExclusive(&g_om_st_lock);
        if (n >= 10 && !memcmp(b, "STATUS!", 7)) {
            g_om_st_state = 1; g_om_st_ms = (int)(GetTickCount64() - t0); g_om_st_players = b[7] | b[8] << 8;
            int k = b[9]; if (k > 32) k = 32; g_om_st_n = 0;
            for (int i = 0; i < k && 10 + 7 * i + 7 <= n; i++) { OmArenaRow *r = &g_om_st_rows[g_om_st_n++]; memcpy(r->code, b + 10 + 7 * i, 5); r->code[5] = 0; r->members = b[15 + 7 * i]; r->most = b[16 + 7 * i]; }
        } else g_om_st_state = 2;
        ReleaseSRWLockExclusive(&g_om_st_lock);
        Sleep(3000);
    }
    return 0;
}
/* -1 = the Online menu is off; 0 = asking; 1 = answered (players, arenas, round trip ms); 2 = not answering */
int online_menu_status(int *players, int *ms, int *narenas, char codes[][6], int *members, int *most, int cap) {
    if (!g_om_on) return -1;
    InterlockedExchange64(&g_om_st_wanted, (LONG64)GetTickCount64());
    if (!InterlockedExchange(&g_om_st_thread, 1)) { HANDLE t = CreateThread(NULL, 0, om_status_thread, NULL, 0, NULL); if (t) CloseHandle(t); }
    AcquireSRWLockShared(&g_om_st_lock);
    int st = g_om_st_state; *players = g_om_st_players; *ms = g_om_st_ms; *narenas = g_om_st_n;
    for (int i = 0; i < g_om_st_n && i < cap; i++) { memcpy(codes[i], g_om_st_rows[i].code, 6); members[i] = g_om_st_rows[i].members; most[i] = g_om_st_rows[i].most; }
    ReleaseSRWLockShared(&g_om_st_lock);
    return st;
}
const char *online_menu_server(void) { return g_om_server; }
int online_menu_arena(uint32_t *gid, int *players, int *own_seat) {
    *gid = 0; *players = 0; *own_seat = -1;
    if (!g_om_on) return 0;
    SOCKET u = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (u == INVALID_SOCKET) return 0;
    struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons(39599);
    InetPtonA(AF_INET, g_om_server, &a.sin_addr);
    DWORD to = 300; setsockopt(u, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof to);
    uint8_t q[14] = {'A', 'R', 'E', 'N', 'A', '?'}; uint64_t me = om_nsa_id(); memcpy(q + 6, &me, 8);
    int ok = 0;
    for (int k = 0; k < 3 && !ok; k++) {
        sendto(u, (const char *)q, sizeof q, 0, (struct sockaddr *)&a, sizeof a);
        uint8_t b[256]; int n = recv(u, (char *)b, sizeof b, 0);
        if (n >= 11 && !memcmp(b, "ARENA!", 6)) {
            memcpy(gid, b + 6, 4); ok = 1; *players = 0;
            /* (the arena's seats; (2026-11-03) indexed by the game's seat, pid 0 = an empty seat: members keep their seats when
             * others leave, so only the real members count) */
            for (int i = 0; i < b[10] && 11 + 8 * i + 8 <= n; i++) { uint64_t p; memcpy(&p, b + 11 + 8 * i, 8); if (p) ++*players; if (p == me) *own_seat = i; }
        }
    }
    closesocket(u);
    om_log("arena of this player (pid %llu): %s, arena %u, %d players, this player's seat %d", (unsigned long long)me, ok ? "answered" : "NO ANSWER from our NEX server", *gid, *players, *own_seat);
    return ok;
}

/* ---------------------------------------------------------------- sockets and nn::ssl: logged */
static void om_Socket(Ctx *c) {
    int family = (int)A0, type = (int)A1, proto = (int)A2;
    g_om_prev_socket(c);
    om_log("Socket(family %d, type %d = %s, protocol %d) -> %d", family, type, type == 1 ? "TCP" : type == 2 ? "UDP" : "?", proto, (int)c->x[0]);
}
static void om_Connect(Ctx *c) {
    int fd = (int)A0;
    char to[48];
    om_addr_text(A1, to, sizeof to);
    g_om_prev_connect(c);
    om_log("Connect(fd %d, %s) -> %d", fd, to, (int)c->x[0]);
}
static void om_SendTo(Ctx *c) {
    int fd = (int)A0;
    uint64_t buf = A1, len = A2;
    char to[48], what[96];
    om_addr_text(c->x[4], to, sizeof to);
    snprintf(what, sizeof what, "SendTo(fd %d, %s)", fd, to);
    om_hex_n(&g_om_udp, OM_UDP_LOGGED, what, buf, len);
    uint64_t dst = c->x[4];
    uint32_t was = dst ? M32(dst + 4) : 0;
    int swapped = g_om_fake && dst && M8(dst + 1) == 2 && om_is_fake(was);
    if (swapped) M32(dst + 4) = htonl(INADDR_LOOPBACK);
    uint16_t port_was = dst ? M16(dst + 2) : 0;
    int v2 = g_om_v2 && dst && M8(dst + 1) == 2 && was == g_om_v2;
    if (v2) { M32(dst + 4) = g_om_v2_server; M16(dst + 2) = htons((uint16_t)om_v2_port_out(ntohs(port_was))); }
    g_om_prev_sendto(c);
    if (swapped || v2) M32(dst + 4) = was;
    if (v2) M16(dst + 2) = port_was;
}
static void om_Bind(Ctx *c) {                              /* (fd, const sockaddr *, len): which port each socket asks for (Pia: 2026-11-02) */
    int fd = (int)A0;
    char at[48];
    om_addr_text(A1, at, sizeof at);
    uint64_t sa = A1;
    uint32_t was = sa ? M32(sa + 4) : 0;
    int swapped = g_om_fake && sa && M8(sa + 1) == 2 && om_is_fake(was);
    if (swapped) M32(sa + 4) = 0;
    g_om_prev_bind(c);
    if (swapped) M32(sa + 4) = was;
    om_log("Bind(fd %d, %s) -> %d", fd, at, (int)c->x[0]);
}
static void om_bind_log(Ctx *c, ImportFn prev) {
    int fd = (int)A0;
    char at[48];
    om_addr_text(A1, at, sizeof at);
    prev(c);
    om_log("Bind(fd %d, %s) -> %d", fd, at, (int)c->x[0]);
}
static void om_Bind2(Ctx *c) { om_bind_log(c, g_om_prev_bind2); }
void hook_trace_window(const char *why);                   /* fighter_sdk/hook_host.c (SSBU_HOOK_TRACE) */
static void om_trace_rx(uint64_t buf, long long got) {     /* a Pia packet of the size of a station connection request */
    const uint8_t *b = (const uint8_t *)(uintptr_t)buf;
    if (got == 144 && b[0] == 0x32 && b[1] == 0xab && b[2] == 0x98 && b[3] == 0x64) hook_trace_window("a 144-byte Pia packet received");
}
static void om_fake_from(uint64_t from) {
    if (g_om_v2 && from && M8(from + 1) == 2 && M32(from + 4) == g_om_v2_server) {
        unsigned p = om_v2_port_in(ntohs(M16(from + 2)));
        if (p) { M32(from + 4) = g_om_v2; M16(from + 2) = htons((uint16_t)p); return; }
    }
    if (g_om_fake && from && M8(from + 1) == 2 && M32(from + 4) == htonl(INADDR_LOOPBACK) && !om_server_port(ntohs(M16(from + 2))))
        M32(from + 4) = om_fake_for_port(ntohs(M16(from + 2)));
}
static void om_recv_log(Ctx *c, ImportFn prev) {
    int fd = (int)A0;
    uint64_t buf = A1, from = A4;
    prev(c);
    long long got = (long long)(int64_t)c->x[0];
    if (got <= 0) return;
    om_fake_from(from);
    om_trace_rx(buf, got);
    char who[48], what[96];
    om_addr_text(from, who, sizeof who);
    snprintf(what, sizeof what, "RecvFrom(fd %d, %s)", fd, who);
    om_hex_n(&g_om_rx, OM_RX_LOGGED, what, buf, (uint64_t)got);
}
static void om_RecvFrom2(Ctx *c) { om_recv_log(c, g_om_prev_recvfrom2); }
static void om_RecvFrom(Ctx *c) {                          /* (fd, buf, len, flags, sockaddr *from, socklen *): what comes back */
    int fd = (int)A0;
    uint64_t buf = A1, from = A4;
    g_om_prev_recvfrom(c);
    long long got = (long long)(int64_t)c->x[0];
    if (got <= 0) return;
    om_fake_from(from);
    om_trace_rx(buf, got);
    char who[48], what[96];
    om_addr_text(from, who, sizeof who);
    snprintf(what, sizeof what, "RecvFrom(fd %d, %s)", fd, who);
    om_hex_n(&g_om_rx, OM_RX_LOGGED, what, buf, (uint64_t)got);
}
static void om_Send(Ctx *c) {
    char what[48];
    snprintf(what, sizeof what, "Send(fd %d)", (int)A0);
    om_hex(what, A1, A2);
    g_om_prev_send(c);
}
static void om_ssl_SetHostName(Ctx *c) {                   /* (connection, const char *, uint32_t) */
    char name[256] = "";
    uint32_t n = (uint32_t)A2;
    if (A1) { if (n >= sizeof name) n = sizeof name - 1; memcpy(name, (const void *)(uintptr_t)A1, n); name[n] = 0; }
    om_log("nn::ssl connection host name \"%s\" (no TLS: in the clear)", name);
    g_om_prev_ssl_host(c);
}

/* nn::ssl without TLS: both ends are ours (the game and tools/net/nex_probe.py / nex_server.py), so the "TLS" connection
 * carries the bytes in the clear over its socket. DoHandshake succeeds with no certificate; Read / Write are recv / send. */
#define OM_SSL_WOULD_BLOCK 0x1987bu                        /* [suyu] ssl ResultWouldBlock {123, 204} */
#define OM_SSL_IO_ERROR 0xd27bu
static void om_ssl_DoHandshake(Ctx *c) {                   /* (connection, uint32_t *outCertSize, uint32_t *outCertCount) */
    uint64_t osize = A1, ocount = A2;
    SslConn *n = ssl_conn_enter(c, A0);
    if (!n) return;
    if (osize) M32(osize) = 0;
    if (ocount) M32(ocount) = 0;
    om_log("nn::ssl DoHandshake: no TLS (bytes in the clear to our server)");
    RET(0);
}
static void om_ssl_io(Ctx *c, int write) {                 /* (connection, buffer, int *outSize, uint32_t size) */
    uint64_t self = A0, buf = A1, out = A2;
    int size = (int)(uint32_t)A3, k, r = 0;
    SslConn *n = ssl_conn_enter(c, self);
    if (!n) return;
    SoObj *o = n->fd >= 0 ? so_get(n->fd, NULL) : NULL;
    if (!o) { M32(self + 0x24) = 0xce7bu; RET(0xce7bu); return; }
    if (write) k = send(o->s, (const char *)(uintptr_t)buf, size, 0);
    else {                                                 /* (2026-11-03) through our own buffer (nnnet_c4a.inc so_bounce: 10053) */
        char *tmp = size > 0 ? so_bounce((size_t)size) : NULL;
        k = recv(o->s, tmp ? tmp : (char *)(uintptr_t)buf, size, 0);
        if (k > 0 && tmp) memcpy((void *)(uintptr_t)buf, tmp, (size_t)k);
    }
    if (k < 0) {
        int w = WSAGetLastError();
        r = w == WSAEWOULDBLOCK ? OM_SSL_WOULD_BLOCK : OM_SSL_IO_ERROR;
        if (r != OM_SSL_WOULD_BLOCK) om_log("nn::ssl %s: socket error %d", write ? "Write" : "Read", w);
        k = 0;
    } else om_hex(write ? "ssl Write" : "ssl Read", buf, (uint64_t)k);
    so_put(o);
    if (out) M32(out) = (uint32_t)k;
    M32(self + 0x24) = (uint32_t)r;
    RET(r);
}
static void om_ssl_Read(Ctx *c) { om_ssl_io(c, 0); }
static void om_ssl_Write(Ctx *c) { om_ssl_io(c, 1); }
static void om_ssl_Pending(Ctx *c) {                       /* (connection, int *out) */
    uint64_t self = A0, out = A1;
    SslConn *n = ssl_conn_enter(c, self);
    if (!n) return;
    u_long avail = 0;
    SoObj *o = n->fd >= 0 ? so_get(n->fd, NULL) : NULL;
    if (o) { ioctlsocket(o->s, FIONREAD, &avail); so_put(o); }
    if (out) M32(out) = (uint32_t)avail;
    M32(self + 0x24) = 0;
    RET(0);
}
static void om_ssl_GetVerifyCertError(Ctx *c) { if (!ssl_conn_enter(c, A0)) return; if (A1) M32(A1) = 0; RET(0); }

/* abort() and CRT invalid parameters end the process with 0xC0000409 and no message: with the Online menu on, both print
 * the host stack first (exe-relative addresses: resolve them with build\work\ssbu_native.map). */
#include <signal.h>
#ifndef _WIN32
void plat_symbolize(uintptr_t pc, char *out, size_t n);   /* plat_posix.c: "symbol+0xoff (file)" */
#endif
static void om_host_stack(const char *why) {
    uintptr_t pcs[48];
#ifdef _WIN32
    USHORT n = CaptureStackBackTrace(1, 48, (PVOID *)pcs, NULL);
#else
    int n = plat_backtrace(pcs, 48);
#endif
    printf("\n[online-menu] %s; host stack:\n", why);
    for (int i = 0; i < n; i++) {
#ifdef _WIN32
        HMODULE m = NULL;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)pcs[i], &m))
            GetModuleFileNameA(m, name, sizeof name);
        const char *base = strrchr(name, '\\');
        printf("  %2d  %s+0x%llx\n", i, base ? base + 1 : name, (unsigned long long)(pcs[i] - (uintptr_t)m));
#else
        char sym[512];
        plat_symbolize(pcs[i], sym, sizeof sym);
        printf("  %2d  %s\n", i, sym);
#endif
    }
    fflush(stdout);
}
static void om_on_abort(int sig) { (void)sig; om_host_stack("abort()"); }
#ifdef _WIN32
static void om_on_invalid_parameter(const wchar_t *e, const wchar_t *f, const wchar_t *file, unsigned line, uintptr_t r) {
    (void)e; (void)f; (void)file; (void)line; (void)r;
    om_host_stack("CRT invalid parameter");
}
#endif

static ImportFn om_prev(const char *name) {
    ImportFn f = import_lookup(name);
    if (!f) { printf("online menu: no handler for %s\n", name); guest_trap("online menu: missing handler", 0); }
    return f;
}
#define OM_SOK(n) "_ZN2nn6socket" n
static void online_menu_register(void) {
    const char *e = getenv("SSBU_ONLINE_MENU");
    if (!e || e[0] != '1') return;
    g_om_on = 1;
    if ((e = getenv("SSBU_ONLINE_MENU_SERVER")) && *e) {
        struct in_addr a;
        if (InetPtonA(AF_INET, e, &a) != 1) { printf("[online-menu] SSBU_ONLINE_MENU_SERVER=%s is not an IPv4 address: the Online menu stays off\n", e); return; }
        snprintf(g_om_server, sizeof g_om_server, "%s", e);
    }
    int remote = 0;                                        /* (2026-11-02) our server on another PC (Radmin, a VPS) */
    {   struct in_addr a; remote = InetPtonA(AF_INET, g_om_server, &a) == 1 && (a.s_addr & 0xff) != 127; }
    if ((e = getenv("SSBU_ONLINE_MENU_SERVER2")) && *e) snprintf(g_om_server2, sizeof g_om_server2, "%s", e);
    else if (remote) {                                     /* (one server: nncs2 is a virtual address of it, see OM_NNCS2_VIRTUAL) */
        snprintf(g_om_server2, sizeof g_om_server2, "%s", OM_NNCS2_VIRTUAL);
        InetPtonA(AF_INET, OM_NNCS2_VIRTUAL, &g_om_v2); InetPtonA(AF_INET, g_om_server, &g_om_v2_server);
        printf("[online-menu] NAT check: nncs1 %s, nncs2 %s (virtual: %s ports 39599 / 10126)\n", g_om_server, OM_NNCS2_VIRTUAL, g_om_server);
    }
    {   const char *f = getenv("SSBU_ONLINE_MENU_FAKEIP");
        struct in_addr a;
        if (!f || !*f) f = remote ? "off" : "10.200.0.1";     /* (the fake addresses are for copies on ONE PC only) */
        if (_stricmp(f, "off") && InetPtonA(AF_INET, f, &a) == 1) g_om_fake = a.s_addr;
        if (g_om_fake) printf("[online-menu] fake addresses on (SSBU_ONLINE_MENU_FAKEIP=%s): each socket is 10.200.<port/256>.<port%%256>, this game's own IP 10.201.x.y; sent to 127.0.0.1 (one-PC tests)\n", f); }
    g_om_prev_gai2 = om_prev(OM_SOK("11GetAddrInfoEPKcS2_PK8addrinfoPPS3_"));
    g_om_prev_gainn = om_prev(OM_SOK("11GetAddrInfoEPKcS2_PKNS0_8AddrInfoEPPS3_"));
    g_om_prev_ghbn = om_prev("nnsocketGetHostByNameWithOptions");
    g_om_prev_resolve = om_prev("_ZN2nn3nsd9ResolveExEPNS0_4FqdnERKS1_");
    g_om_prev_socket = om_prev(OM_SOK("6SocketEiii"));
    g_om_prev_connect = om_prev(OM_SOK("7ConnectEiPK8sockaddrj"));
    g_om_prev_sendto = om_prev(OM_SOK("6SendToEiPKvmiPK8sockaddrj"));
    g_om_prev_send = om_prev(OM_SOK("4SendEiPKvmi"));
    g_om_prev_bind = om_prev(OM_SOK("4BindEiPK8sockaddrj"));
    g_om_prev_recvfrom = om_prev(OM_SOK("8RecvFromEiPvmiP8sockaddrPj"));
    g_om_prev_bind2 = om_prev(OM_SOK("4BindEiPKNS0_8SockAddrEj"));
    g_om_prev_recvfrom2 = om_prev(OM_SOK("8RecvFromEiPvmNS0_7MsgFlagEPNS0_8SockAddrEPj"));
    g_om_prev_ssl_host = om_prev("nnsslConnectionSetHostName");
    g_om_prev_primary_ip = om_prev("_ZN2nn4nifm26GetCurrentPrimaryIpAddressEP7in_addr");
    static const ImportDef defs[] = {
        {"_ZN2nn4nifm18IsNetworkAvailableEv", om_nifm_true}, {"_ZN2nn4nifm22IsNetworkRequestOnHoldEv", om_nifm_false},
        {"_ZN2nn4nifm20SubmitNetworkRequestEv", om_nifm_keep}, {"_ZN2nn4nifm27SubmitNetworkRequestAndWaitEv", om_nifm_keep},
        {"_ZN2nn4nifm26GetCurrentPrimaryIpAddressEP7in_addr", om_nifm_GetCurrentPrimaryIpAddress},
        {"_ZN2nn7account32IsNetworkServiceAccountAvailableEPbRKNS0_10UserHandleE", om_acc_IsNsaAvailable},
        {"_ZN2nn7account36EnsureNetworkServiceAccountAvailableERKNS0_10UserHandleE", om_acc_ok},
        {"_ZN2nn7account44EnsureNetworkServiceAccountIdTokenCacheAsyncEPNS0_12AsyncContextERKNS0_10UserHandleE", om_acc_EnsureIdTokenCacheAsync},
        {"_ZN2nn7account26GetNetworkServiceAccountIdEPNS0_23NetworkServiceAccountIdERKNS0_10UserHandleE", om_acc_GetNsaId},
        {"_ZN2nn7account29LoadCachedNintendoAccountInfoEPNS0_25CachedNintendoAccountInfoERKNS0_10UserHandleEPvm", om_acc_LoadCachedNintendoAccountInfo},
        {"_ZN2nn7account37LoadNetworkServiceAccountIdTokenCacheEPmPcmRKNS0_10UserHandleE", om_acc_LoadIdTokenCache},
        {"_ZN2nn7account12AsyncContext7HasDoneEPb", om_acc_Async_HasDone}, {"_ZN2nn7account12AsyncContext9GetResultEv", om_acc_Async_GetResult},
        {"_ZN2nn7account12AsyncContext6CancelEv", om_acc_Async_Cancel},
        {"_ZN2nn7account12AsyncContext14GetSystemEventEPNS_2os11SystemEventE", om_acc_Async_GetSystemEvent},
        {"_ZN2nn7account12AsyncContextaSEOS1_", om_acc_Async_move_assign}, {"_ZN2nn7account12AsyncContextD1Ev", om_acc_Async_dtor},
        {OM_SOK("11GetAddrInfoEPKcS2_PK8addrinfoPPS3_"), om_GetAddrInfo2}, {OM_SOK("11GetAddrInfoEPKcS2_PKNS0_8AddrInfoEPPS3_"), om_GetAddrInfoNn},
        {"nnsocketGetHostByNameWithOptions", om_GetHostByNameWithOptions}, {"_ZN2nn3nsd9ResolveExEPNS0_4FqdnERKS1_", om_nsd_ResolveEx},
        {OM_SOK("6SocketEiii"), om_Socket}, {OM_SOK("6SocketENS0_6FamilyENS0_4TypeENS0_8ProtocolE"), om_Socket}, {"nnsocketSocket", om_Socket},
        {OM_SOK("7ConnectEiPK8sockaddrj"), om_Connect}, {"nnsocketConnect", om_Connect},
        {OM_SOK("6SendToEiPKvmiPK8sockaddrj"), om_SendTo}, {OM_SOK("6SendToEiPKvmNS0_7MsgFlagEPKNS0_8SockAddrEj"), om_SendTo},
        {OM_SOK("4SendEiPKvmi"), om_Send}, {"nnsocketSend", om_Send},
        {OM_SOK("4BindEiPK8sockaddrj"), om_Bind},
        {OM_SOK("8RecvFromEiPvmiP8sockaddrPj"), om_RecvFrom}, {OM_SOK("4BindEiPKNS0_8SockAddrEj"), om_Bind2},
        {OM_SOK("8RecvFromEiPvmNS0_7MsgFlagEPNS0_8SockAddrEPj"), om_RecvFrom2},
        {"nnsslConnectionSetHostName", om_ssl_SetHostName},
        {"nnsslConnectionDoHandshakeWithCertBuffer", om_ssl_DoHandshake}, {"nnsslConnectionRead", om_ssl_Read},
        {"nnsslConnectionWrite", om_ssl_Write}, {"nnsslConnectionPending", om_ssl_Pending},
        {"nnsslConnectionGetVerifyCertError", om_ssl_GetVerifyCertError},
    };
    register_imports(defs, (int)(sizeof defs / sizeof defs[0]));
    signal(SIGABRT, om_on_abort);
#ifdef _WIN32
    _set_invalid_parameter_handler(om_on_invalid_parameter);
#endif
    om_log("ON (phase 0, discovery): network and network service account modelled as available; nintendo.net names -> %s", g_om_server);
}
