<!-- The game's own Online menu (Battle Arenas) against our servers: what the game does, found by tracing it. -->
# Online menu: reverse-engineering notes

Goal: the game's own Online > Battle Arenas screens working against our lobby; the fights are played with our rollback
and count for the ladder. Plan: phase 0 (this file) finds what the game speaks; phase 1 answers it.

## How to trace it

```
py -3 tools/net/nex_probe.py --log nex_probe.log
py -3 scripts/run.py --userdata <saves> --env SSBU_ONLINE=off --env SSBU_ONLINE_MENU=1 --log om.log
```
then Online in the game's main menu. `runtime/online_menu.inc` (only with `SSBU_ONLINE_MENU=1`):
- nn::nifm: the network request is accepted, the address is 127.0.0.1;
- nn::account: a network service account, a JWT-shaped ID token (not signed), `EnsureNetworkServiceAccountIdTokenCacheAsync`
  finished at once;
- every name is logged; a name under `nintendo.net` resolves to `SSBU_ONLINE_MENU_SERVER` (default 127.0.0.1), any other
  name is left to the offline resolver (which fails): the game never reaches a server of Nintendo;
- nn::ssl carries the bytes in the clear (no TLS: both ends are ours); sockets, connects and the first 64 packets are
  logged (`[online-menu]` lines).

## What the game does (13.0.4, 2026-10-29)

1. Online in the main menu: `LoadCachedNintendoAccountInfo`, `EnsureNetworkServiceAccountIdTokenCacheAsync`,
   `LoadNetworkServiceAccountIdTokenCache` (twice), then the game server's name.
2. Name: `g23380901-%.s.n.srv.nintendo.net` through `nsd::ResolveEx` (the `%` is the environment, `lp1` on a console;
   our ResolveEx leaves it as it is), then `GetAddrInfo` and `GetHostByNameWithOptions`.
3. **TCP port 443**, nn::ssl with that host name, then an HTTP WebSocket upgrade:
   ```
   GET / HTTP/1.1
   Host: g23380901-%.s.n.srv.nintendo.net
   Upgrade: websocket
   Connection: upgrade
   Sec-WebSocket-Key: NEX
   Sec-WebSocket-Version: 13
   Sec-WebSocket-Protocol: NEX
   ```
   It accepts a standard `101 Switching Protocols` answer (Sec-WebSocket-Accept computed from the key "NEX").
4. First binary WebSocket message, 18 bytes, resent about every 1 to 2 s for 30 s:
   ```
   80 06 00 00 aa 1f 01 00 40 00 00 00 00 04 06 00 00 00
   ```
   Read as a PRUDP "lite" packet (the NEX transport over WebSocket; layout from the public NEX documentation, still to be
   confirmed by a working answer): magic 0x80, options length 6, payload length 0, stream types 0xaa (source and
   destination 10), source port 0x1f, destination port 1, fragment 0, type/flags 0x0040 (type 0 = SYN, flag NEED_ACK),
   sequence 0, options `00 04 06 00 00 00` (option 0 "supported functions", 4 bytes).
5. No answer to the SYN: the socket is closed after 30 s and the game shows **2306-0502**
   (`nn::err::ShowError`, caller 0x7103346e9c). Without a listener on 443 the same code comes at once.

## Decision

**B1 (our own NEX server)**: the transport is plain enough. TLS is removed in our nn::ssl, WebSocket is standard, and
the NEX payload starts with a PRUDP lite SYN. Later the answers move into the runtime (each game answers itself on
loopback; the arena list goes over our existing lobby / host-as-server): the player runs nothing extra.

## Phase 1 so far (tools/net/nex_server.py, 2026-10-29)

Works, against `nex_server.py` on 127.0.0.1:443 (the game also logs in by itself at boot):
1. Authentication server: SYN / CONNECT (no payload), then TicketGranting 6 (ValidateAndRequestTicketWithParam:
   platform 3, username = the NSA id in decimal, AuthenticationInfo with our token, NEX 40603, client 40). Answer:
   structure v0 {pid u64, ticket, secure StationURL, DateTime, server name, source key hex}. The ticket is kerberos
   (RC4 + HMAC-MD5, key = the source key) of {session key 32, target pid u64 = 2 (the secure server), internal Buffer}.
   Target pid = the player made the game ask RequestTicket and crash (null session key at 0x71001cf570).
2. Secure server: a second WebSocket to the StationURL address; CONNECT payload = Buffer internal ticket + Buffer
   kerberos(session key){pid, cid, check}. Answer in the CONNECT-ACK: **Buffer(u32 check + 1) in the clear** (encrypted
   answers made the game disconnect).
3. SecureConnection 1 Register (two prudp:/ URLs, ports 11/12.., Tpt=2, sid=30): Result, cid, public URL.
4. PRUDP type 8 every 3 s on the secure connection: a keep-alive; it must be ACKed (else 2306-0332 after ~50 s).
5. Online menu: DataStore 61, 83, 94, 84, 60, 95, 97, 8 (Smash methods, answered DataStore::NotFound or an empty list),
   Utility 7 GetIntegerSettings (empty map), Utility 11 (en-US; NotImplemented is tolerated). Also an HTTP
   `POST /v1/NX/getToken` to api-%.znc.srv.nintendo.net (404 is tolerated).
6. Battle Arenas search: MatchmakeExtension 58 (u32 7) -> empty list, then 52 (search criteria + ResultRange 0..50) ->
   List<MatchmakeSession>: an empty list shows "no arenas".
7. Create arena: MatchmakeExtension 38 (CreateMatchmakeSessionWithParam: MatchmakeSession with game mode 7, attributes
   [0,2,3,2,1,1], 2..6 players, a 413-byte application buffer with the host's name) -> the session with gid / owner /
   host filled in. Then the game waits for a notification (NexMatchCreateSessionJob::WaitNotification, ~10 s, else
   "a communication error" and MatchMaking 2 UnregisterGathering). A NotificationEvents 1 call with type 3001
   (participation, param1 gid, param2 pid) moves it on to Pia's NAT check.

## Where it stops (next step)

Pia's NAT check: nncs1 / nncs2 (mapped to 127.0.0.1 / 127.0.0.2), UDP port **10025**. 16-byte packets, the 4th byte
a request type: 0x65 (x5) and 0x66 (x5) to nncs1, 0x67 to nncs2; also 16 zero bytes to 127.0.0.1:33334. The server
only logs them (no answer): the game shows "loading", then an error (2618-0006 before the second address existed).
Next: answer the NAT check (the client's address as seen, per request type), then Pia's own session / mesh between
two games; then route the arena's fight into our rollback match (plan phase 3).

## Salas (2026-11-21): several arenas under one code

Our own addition, in `tools/net/nex_server.py` (the comment above `SALAS`) and `runtime/online_menu.inc`. What changes in
the methods above, for a member of a sala only:

- 38 (create): the arena is the sala's next station (the lowest number free).
- 52 (search): the sala's stations only, in their order. Players in no sala get the arenas of no sala.
- 55 (code): the sala's code, for every station. (Also fixed: an arena asked twice got a new code each time.)
- 56 (typed code): a sala's code makes the player a member and answers the gid of a station (one player waiting first,
  then the fewest members; 0 when no station is open). Any other code: the arena of no sala that has it.
- 39 (join): entering a station makes the player a member of its sala.
- A player is in one arena at a time: entering or making one takes it out of the others, and closes one it had made.

On the arena query port (UDP 39599, little-endian):

- `"SALA?"` + version u8 (1) + op u8 (0 still here, 1 enter, 2 leave, 3 make) + pid u64 + code[5] + name length u8 + name
  -> `"SALA!"` + version u8 + status u8 (0 member, 1 no such sala, 2 in none, 3 no more salas) + code[5] + stations u8 +
  members u8. Never 16 bytes long (a 16-byte datagram there is a NAT check).
- `"STATUS?"` + version u8 (2) + pid u64 -> `"SALAST!"` + online u16 + sala[5] + rows u8 + per row: gid u32 + station u8
  (0: an arena of no sala) + code[5] + members u8 + most u8 + fighting seats u8 (0: not known) + n u8 + n x (seat u8 +
  name[10]). The plain `"STATUS?"` answers as before, with the arenas of no sala only.

Not known until tried with games: whether the game accepts a gid from 56 whose arena shows another code, or two arenas
with the same code (55); what it shows for an empty or shortened list (52), for a station that closed under it, and for a
full one; whether a host that leaves sends UnregisterGathering; the characters of the game's Arena ID keypad.

## Open

- The `%` in the host names (`lp1` on a console): harmless while we answer every name.
- The Smash DataStore methods (what 61, 83, 94... are) and Utility 11.
- The notification's exact NEX 4.6 layout (accepted as {pid, type, param1 u64, param2 u64, string, param3 u64} v0).
