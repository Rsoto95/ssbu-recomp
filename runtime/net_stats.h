/* U1: what the stats overlay of the settings menu shows about online play (menu.cpp, F2; plan\phases\U1_settings_menu.md).
 *
 * The overlay calls net_stats_get() once per drawn frame on the render thread. menu.cpp has a default that answers
 * "Offline" (net_stats_get_default, bound with /alternatename): the online code defines net_stats_get() itself and
 * the linker takes that one instead, with no change here. It must be cheap and must not block (read a few ints).
 *
 * Who fills which field:
 *   state, players, ping_ms       the online layer (nnldn.c / net_relay.c, track N3): connection state, players in the
 *                                 session (this PC included), round-trip time to the other side in ms (-1 = unknown)
 *   delay                         the rollback code (rb_*.c, track N1/R2): input delay in frames used now
 *   rollback_last, rollback_max   the rollback code: frames rolled back at the last rollback, the most this session
 * Run-ahead is not here: the overlay reads it itself (graphics.ini runahead / SSBU_RUNAHEAD, r1_state.c r1_ra_phase). */
#ifndef NET_STATS_H
#define NET_STATS_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum { NET_OFFLINE = 0, NET_CONNECTING = 1, NET_CONNECTED = 2, NET_DISCONNECTED = 3 };
typedef struct { int state; int players; int ping_ms; int delay; int rollback_last, rollback_max; } NetStats;
void net_stats_get(NetStats *out);          /* fills every field; the default: state NET_OFFLINE, ping -1, the rest 0 */

/* O1 (plan\phases\O1_room_browser.md): the room browser of the settings menu (F1 > Online rooms), net_relay.c.
 * Every room of this lobby code, any game version or mod set; joinable = same build id as this game, otherwise why
 * says the cause in one or more lines ("Different version: this room is 13.0.4, you have 13.0.5", "MISSING: a, b"). */
typedef struct {
    uint32_t host;                          /* the host's virtual address */
    char name[33], build[41], version[24];  /* host name, build id, game version taken from it */
    int players, most, open, mine, joinable, mods_known, nmods;
    char mods[256];                         /* the room's mods, comma separated ("" = none) */
    char why[256];                          /* joinable == 0: the cause */
} NetRoom;
/* the rooms (at most max) and the browser's state: 0 online off, 1 waiting for the lobby, 2 the lobby does not answer
 * (an older lobby), 3 listed. Calling it keeps the list fresh (asked every 2 s while called). Never blocks long. */
int net_rooms_get(NetRoom *out, int max, int *state);
const char *net_build_id(void);             /* this game's build id ("ssbu-recomp-ldn1-13.0.5[+m<hash>]") */
const char *net_mods_text(void);            /* this game's mods ("none" or "name (hash), ...") */

/* O2 (plan\phases\O2_matchmaking.md): "Find an opponent" (F1 > Online), net_relay.c. The lobby pairs two searching players
 * of the same build id and lobby code and says who hosts; the host creates the Local Wireless room, the other finds it
 * first in the game's Rooms List. */
enum { MM_IDLE = 0, MM_SEARCHING, MM_MATCHED, MM_TIMEOUT, MM_PEER_LEFT, MM_TOGETHER, MM_EXPIRED,   /* (the lobby's states) */
       MM_ASKING, MM_OLD_LOBBY, MM_OFFLINE };                               /* asked, no answer yet; an older lobby; online off / not joined */
enum { MM_ROLE_HOST = 1, MM_ROLE_JOIN = 2 };
typedef struct {
    int state, role, room_open;             /* MM_*, MM_ROLE_* (MATCHED / TOGETHER), the host's room is open (listed) */
    uint32_t peer;                          /* the opponent's virtual address */
    char peer_name[33];
    int waited_s;                           /* seconds searching, or since the match */
    int players, searching, code_players;   /* lobby counts: players of this build and code, of them searching, players of
                                               every build of this code; -1 = not known (an older lobby / not joined yet) */
    int kind;                               /* (R3 net3) 1 = a Local Wireless search (F1 > Online), 2 = a rollback-match search (rb_net.c) */
} NetMatch;
void net_match_find(void);                  /* start searching (again) */
void net_match_cancel(void);                /* stop searching, or leave the match (the other player is told) */
void net_match_get(NetMatch *out);          /* never blocks */
/* (R3 net3, lobby 1.4) a search for a ROLLBACK match (rb_net.c: Smash versus with rollback on, in lobby / LAN mode, outside
 * a Local Wireless room): paired only with another such search; MM_MATCHED / MM_TOGETHER = the opponent is known, its
 * address is where the rollback messages go (net_rb_send) until the next search. */
void net_match_find_rb(void);
void net_match_find_rb_key(uint64_t key);    /* (2026-11-02) the same, paired only with a search of the same key (a Battle Arena: its gathering id) */

/* Ranked ladder (lobby 1.5, tools/net/ladder.py): this player's standing as the lobby last said it (WELCOME, RESULT_ACK;
 * F1 > Online), and the result of a rollback match that ladder_report.c hands over at its end (sent until acknowledged). */
enum { NET_LADDER_OFF = 0, NET_LADDER_UNKNOWN, NET_LADDER_UNRANKED, NET_LADDER_RANKED };
typedef struct {
    int state;                              /* NET_LADDER_*: off (SSBU_LADDER=0 / online off), not told yet (an older lobby says nothing) */
    int rating, rd, wins, losses, place;    /* (ranked) Glicko-2 rating and deviation, sets won and lost, place in the table */
    char text[33];                          /* the ladder name (ranked) or why not ranked */
    int last_outcome;                       /* the last match's result: -1 none, 0 waiting for the other game, 1 counted, 2 draw,
                                               3 disputed, 4 not ranked */
    int set_mine, set_theirs;               /* the set's games won by each, after that result */
} NetLadder;
void net_ladder_get(NetLadder *out);        /* never blocks */
/* Accounts (the ladder identity: graphics.ini ladder_id / ladder_key; F1 > Online > Your account). The account code is
 * the id and the key as "SSBU-" + Crockford base32 with a check: whoever has it IS this account (like a password). */
int net_account_code(char *out, int n);     /* this game's account code; 0 without an identity (online or the ladder off); n >= 96 */
int net_account_use_code(const char *code); /* 1: a valid code, written to graphics.ini, used from the next start; 0: not a valid code */
const char *net_online_name(void);          /* the name this game sends to the lobby ("" with online play off) */
enum { NET_LADDER_F_ABANDON = 1, NET_LADDER_F_DISCONNECT = 2 };
typedef struct {
    uint64_t seed;                          /* the match handshake's seed: the same on both games */
    int slot;                               /* this game's player: 0 = player 1 */
    int rule_kind, stock;                   /* the agreed rules (1 = stock, stocks each) */
    int flags;                              /* NET_LADDER_F_* */
    uint32_t stage;                         /* the stage played (match block + 0x420) */
    uint64_t chara[2];                      /* the agreed ui_chara ids of player 1 and 2 */
    int stocks[2], dmg10[2];                /* stocks left, damage in tenths of a percent, at the last final frame */
    uint32_t frames, desyncs;               /* fight frames to that frame; checksum mismatches seen in the match */
} NetLadderResult;
void net_ladder_result(const NetLadderResult *r);

/* Arena (lobby 1.6, F1 > Arena, net_relay.c): a queue of players of this build and lobby code taking turns at 1-on-1
 * rollback matches (Smash versus); the winner stays. During this player's turn a Smash (versus) match searches the lobby
 * for the opponent of the turn by itself (net_session.c rbn_handshake). */
enum { NET_ARENA_OUT = 0, NET_ARENA_WAITING, NET_ARENA_TURN, NET_ARENA_PLAYING };
typedef struct { char name[33]; int streak; } NetArenaEntry;
typedef struct {
    int known;                              /* the lobby answered (0: not joined yet, or an older lobby without the arena) */
    int in;                                 /* this player asked to be in the queue */
    int status, place, streak, streak_limit, secs;   /* NET_ARENA_*, place in the queue (1 = next), wins in a row, the lobby's
                                               limit (0 none), seconds in this status */
    char opponent[33];                      /* (turn / playing) */
    int has_match, match_state;             /* the match of the arena now: NET_ARENA_TURN (not started yet) or NET_ARENA_PLAYING */
    NetArenaEntry match[2];
    int nqueue;
    NetArenaEntry queue[16];
    /* (room, 2026-10-30) a player makes the room and is its host (the lobby in its game in LAN mode); one room at a time */
    int has_room, is_host, room_players;    /* a room is open, this player is its host, players in it (queue + match) */
    char host[33];                          /* the host's name */
    int why;                                /* why this player was last taken out: 0 none, 1 removed by the host, 2 the room closed, 3 turn not started */
    int making;                             /* this game is making a room (becoming the network's lobby first) */
} NetArena;

/* Tournaments (lobby 1.7, tools/net/tournament.py; F1 > Tournament): what the lobby last said (TOURNEY_STATE: the list and
 * this player's set; TOURNEY_DOC: the bracket of the tournament looked at), and the requests (each sent until the lobby
 * acknowledges it). The rollback search (net_session.c) holds a tournament player at the set-up until its set may start. */
#define NET_T_LIST 8
#define NET_T_MAX 256
#define NET_T_SETS 512
#define NET_T_STAGES 16
typedef struct { int id, players, signed_up, organizer; char state[12]; char name[33]; } NetTourneyItem;
typedef struct { int dq, rating; char name[33]; } NetTourneyEntrant;          /* rating -1: none yet */
typedef struct { int id, round, p[2], winner, score[2], bo; char br; char state[10]; } NetTourneySet;   /* br W / L / G; p: entrant, -1 a bye, -2 not known yet */
typedef struct {
    int known, off, account, can_create, pending;  /* off: the lobby has none; pending: a request not acknowledged yet */
    char err[160];                                 /* the lobby's answer to the last request ("" = done) */
    int n; NetTourneyItem list[NET_T_LIST];
    int focus, version;                            /* the tournament looked at (0 none), its bracket's version here (0: not here yet) */
    char tstate[12], seed_mode[10], organizer[33], name[33];
    int bo, bo_top8, bo_finals, bans, stocks;
    int nstarters, ncps; char starters[NET_T_STAGES][24], cps[NET_T_STAGES][24];
    int nent; NetTourneyEntrant ent[NET_T_MAX];
    int nsets; NetTourneySet sets[NET_T_SETS];
    int nplaces; int places[NET_T_MAX][2];         /* place, entrant */
    /* this player in the tournament looked at */
    int me, set, side, host;                       /* entrant (-1: not in it), its set now (-1: none), its side, it hosts the match */
    char status[8]; int place;                     /* without a set now: "wait", "out", "dq" or "done" (place) */
    char opp[33]; int mine, theirs, need, game, rule_stocks;
    char phase[8]; int my_turn, left; char stage[24];   /* phase strike / ban / pick / chars / go; my_turn: a stage step is ours */
    int npool, nstruck; char pool[NET_T_STAGES][24], struck[NET_T_STAGES][24];
    int first;                                     /* the fighter order: 0 both blind (game 1), 1 this player first, 2 the other first */
    int ready_me, ready_them; uint64_t their_chara; char their_chara_name[33];   /* (their fighter: when they chose first) */
    int go;                                        /* both ready: the match may start */
    /* (2026-11-02) the player's own tournament, whatever the list looks at ("mt"): the cards on screen (tourney_card.inc) */
    int own_id, own_in; char own_state[12], own_name[33];   /* own_id 0: in none; own_in: checked in */
    char br; int round, set_bo, turn_secs;         /* its set: W / L / G, round, best of; seconds left of a stage step (-1 none) */
    char next_br; int next_round; char next_who[80];   /* without a set: the next one (next_br 0: none known) */
    char feature[12];                              /* the set is on stream: played in the Battle Arena with this code ("" no) */
} NetTourney;
void net_tourney_get(NetTourney *out);           /* never blocks; while F1 > Tournament asks, the lobby is asked every second */
void net_tourney_peek(NetTourney *out);          /* the same, without asking the lobby more often (the cards on screen, every frame) */
void net_tourney_look(int tid);
void net_tourney_create(const char *name, int bo, int bo_top8, int bo_finals, int bans, int stocks);
void net_tourney_simple(int op, int tid);        /* 1 sign up, 2 withdraw, 4 start, 5 cancel, 13 check in, 14 open the check-in */
void net_tourney_seed_mode(int tid, int mode);   /* 0 rating, 1 random, 2 manual */
void net_tourney_move(int tid, int from, int to);
void net_tourney_dq(int tid, int entrant);
void net_tourney_result(int tid, int set, int winner);   /* the organizer's: side 0 / 1 won, -1 clears it */
void net_tourney_stage(int tid, int set, const char *stage);
const char *net_tourney_stage_name(const char *stage);   /* "battle_field" -> "Battlefield" */
int net_tourney_queued(void);                    /* (net_session.c) this player has a set to play now */
int net_tourney_turn(void);                      /* ... and both players are ready: the match may start */
void net_tourney_ready(uint64_t chara, int confirm);     /* at the set-up with that fighter (sent until acknowledged) */
void net_tourney_unready(void);
int net_tourney_match(char *stage, int n, int *stocks);  /* the set's stage (internal name) and stocks for the host's proposal; 0: none */
void net_arena_get(NetArena *out);          /* never blocks; while it is called the state is asked for every second */
void net_arena_join(int in);                /* 1 = into the queue, 0 = out of the arena */
int net_arena_turn(void);                   /* 1 = this player's turn (or match) in the arena now */
int net_arena_queued(void);                 /* 1 = in the room's queue, its turn or its match (a Smash match then waits for the turn) */
void net_arena_create(void);                /* make a room and be its host (and in its queue) */
void net_arena_close(void);                 /* the host closes its room */
void net_arena_kick(const char *name);      /* the host removes a player from its room */
#ifdef __cplusplus
}
#endif
#endif
