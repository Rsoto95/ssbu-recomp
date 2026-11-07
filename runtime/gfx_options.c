/* Track O (options), first set (track V session 4): config\graphics.ini.
 *
 * Path: SSBU_GRAPHICS_CONFIG=<file> when set; else graphics.ini beside the file SSBU_CONFIG names (SSBU_CONFIG is the
 * path of controllers.ini, nnpad_sdl.c); else <exe folder>\..\config\graphics.ini. Written with the defaults and their
 * documentation when it does not exist. Read ONCE at start (the window, the device and the swapchain are made from it).
 * Environment switches of the test scripts (SSBU_RENDERER, SSBU_VK_DEVICE, SSBU_VK_PRESENT) win over the file.
 * The controllers.ini reader of nnpad_sdl.c is built around its device sections and is not reusable here: this file has
 * its own 30-line reader (key = value, ';' or '#' comments, section headers ignored). */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gfx_options.h"
#ifndef L1_LATENCY_DEFAULT
#define L1_LATENCY_DEFAULT 0      /* track L1: normal (coordinator 2026-10-14); low is an option */
#endif
static int g_gfx_latency = L1_LATENCY_DEFAULT;      /* track L1: latency = normal 0 | low 1 | lowest 2 (display.c); read with gfx_latency() */
int gfx_latency(void) { gfx_options(); return g_gfx_latency; }
static int g_gfx_runahead;                           /* track R1 round 5: runahead = off 0 | on 1 (r1_state.c); read with gfx_runahead() */
int gfx_runahead(void) { gfx_options(); return g_gfx_runahead; }
static int g_gfx_menu = 1, g_gfx_volume = -1;      /* U1: menu = on (default) | off; volume = auto | 0..100 */
static char g_gfx_language[16];                      /* U1: language = auto ("") | a code */
int gfx_menu(void) { gfx_options(); return g_gfx_menu; }
static int g_gfx_stats;                              /* U1: stats_overlay = off | on */
int gfx_stats_overlay(void) { gfx_options(); return g_gfx_stats; }
int gfx_volume(void) { gfx_options(); return g_gfx_volume; }
const char *gfx_language(void) { gfx_options(); return g_gfx_language; }
static int g_gfx_unlock_all;                         /* unlock_all = off | on (unlock_all.c) */
int gfx_unlock_all(void) { gfx_options(); return g_gfx_unlock_all; }
static volatile int g_gfx_input_delay = 0;            /* 0 = auto at every start; 1 .. 7 chosen in F1 for the session (net_session.c) */
int gfx_input_delay(void) { gfx_options(); return g_gfx_input_delay; }
void gfx_set_input_delay(int d) { gfx_options(); g_gfx_input_delay = d; }
/* (2026-11-02) the game's own Online menu (Battle Arenas) on our server: online_menu = off | on, online_menu_server =
 * <IPv4 address> (default the project's arena server). Turned into SSBU_ONLINE_MENU / _SERVER and SSBU_ONLINE (the lobby of
 * the arenas' rollback, <server>:39500) at the start by gfx_online_menu_env (boot.c), when SSBU_ONLINE_MENU is not set. */
static int g_gfx_online_menu = 1; static char g_gfx_online_menu_server[64] = GFX_ONLINE_MENU_SERVER_DEFAULT;
int gfx_online_menu(void) { gfx_options(); return g_gfx_online_menu; }
static int gfx_ipv4(const char *v) { int a, b, c, d; char x; return sscanf(v, "%d.%d.%d.%d%c", &a, &b, &c, &d, &x) == 4 && a >= 0 && a < 256 && b >= 0 && b < 256 && c >= 0 && c < 256 && d >= 0 && d < 256; }
void gfx_online_menu_env(void) {
    gfx_options();
    if (!g_gfx_online_menu || getenv("SSBU_ONLINE_MENU")) return;
    {   const char *was = getenv("SSBU_ONLINE");             /* (2026-11-03) a run asked to be offline stays offline: the installer's */
        if (was && !_stricmp(was, "off")) {                 /* self-tests (SSBU_ONLINE=off) ran on the live server with Battle Arenas */
            printf("graphics.ini online_menu = on, but this run is offline (SSBU_ONLINE=off): Battle Arenas off\n");     /* on: 19 SDK failures */
            return;
        } }
    char lobby[96]; snprintf(lobby, sizeof lobby, "%s:39500", g_gfx_online_menu_server);
    _putenv_s("SSBU_ONLINE_MENU", "1");
    _putenv_s("SSBU_ONLINE_MENU_SERVER", g_gfx_online_menu_server);
    {   const char *was = getenv("SSBU_ONLINE"); _putenv_s("SSBU_ONLINE_MENU_PREV_ONLINE", was && *was ? was : "-"); }
    _putenv_s("SSBU_ONLINE", lobby);                        /* (the arenas' lobby wins over Play SSBU.bat's group lobby: their rollback needs lobby 1.7) */
    _putenv_s("SSBU_ONLINE_MENU_FROM_INI", "1");            /* (menu.cpp restart_game: the next copy reads graphics.ini again) */
    printf("graphics.ini online_menu = on: the game's Online menu (Battle Arenas) on %s, lobby %s\n", g_gfx_online_menu_server, lobby);
}
static volatile int g_gfx_fight_sound = 1;            /* fight_sound = on | off (on when the key is missing, 2026-11-05) (snapshot_ring.c); the F1 menu changes it while running */
int gfx_fight_sound(void) { gfx_options(); return g_gfx_fight_sound; }
void gfx_set_fight_sound(int on) { gfx_options(); g_gfx_fight_sound = on; }
/* (2026-11-07, owner: friends with 16 GB PCs) rollback_delay_only = on: this game never makes the rollback ring in an online
 * match (rollback_controller.c): it plays our delay-based way (it waits for the other player's pad, the input delay chosen
 * from the ping for both games) and saves the ring's ~3 GB; the other game keeps its rollback. Read at the start only (a
 * ring made earlier in the session is not undone): F1 > Online writes it for the next start. */
static int g_gfx_rb_delay_only;
int gfx_rb_delay_only(void) { gfx_options(); return g_gfx_rb_delay_only; }
/* (2026-11-05) the game turned the sound off by itself for this session (a PC at its limit, rollback_controller.c); a player's
 * own choice in F1 (gfx_fight_sound_chosen) is never overridden */
static volatile int g_gfx_fight_sound_chosen, g_gfx_fight_sound_auto_off;
void gfx_fight_sound_chosen(void) { g_gfx_fight_sound_chosen = 1; g_gfx_fight_sound_auto_off = 0; }
int gfx_fight_sound_auto_off(void) {
    gfx_options();
    if (g_gfx_fight_sound_chosen || !g_gfx_fight_sound) return 0;
    g_gfx_fight_sound = 0; g_gfx_fight_sound_auto_off = 1; return 1;
}
int gfx_fight_sound_was_auto_off(void) { return g_gfx_fight_sound_auto_off; }
static int g_gfx_sharpening;                         /* sharpening = 0 .. 100 */
int gfx_sharpening(void) { gfx_options(); return g_gfx_sharpening; }
static int g_gfx_refresh_switch = 1;                  /* refresh_switch = auto (1) | off (0): display.c refresh_switch */
int gfx_refresh_switch(void) { gfx_options(); return g_gfx_refresh_switch; }
static char g_gfx_shot_dir[400];                     /* screenshot_dir = a folder (UTF-8) | empty: <install>\screenshots */
const char *gfx_screenshot_dir(void) { gfx_options(); return g_gfx_shot_dir; }
static int g_gfx_shot_key = 0x7b;                    /* screenshot_key = F12 (VK_F12) by default; F1..F24, off = 0 */
int gfx_screenshot_key(void) { gfx_options(); return g_gfx_shot_key; }

/* (2026-11-04) Battle Arenas and rollback on by default: this comment line in a graphics.ini marks that the file has been
 * moved to those defaults once (ini_migrate_online_defaults); a later OFF chosen by the player stays OFF. */
#define GFX_FIGHT_SOUND_MARK ";   (2026-11-05: the sound of online fights is on by default; F1 > Gameplay turns it off.)\r\n"
#define GFX_VSYNC_AUTO_MARK ";   (2026-11-07: vsync = on / clock / mailbox of an older file became auto once: online fights follow the monitor now.)\r\n"
#define GFX_ONLINE_DEFAULTS_MARK ";   (2026-11-04: Battle Arenas and rollback netcode are on by default; F1 turns them off.)\r\n"
static const char *const k_default_ini =
"; graphics.ini: display and rendering options of the native build (read once at start by gfx_options.c).\r\n"
"; Delete the file to get it written again with these defaults. A line starting with ';' is a comment.\r\n"
"; Every value below is the default; the defaults draw exactly what the game draws on the console.\r\n"
"\r\n"
"; renderer = vulkan | opengl\r\n"
";   vulkan: the default since 2026-10-09. opengl: the first renderer (NVIDIA only), kept as a fallback.\r\n"
"renderer = vulkan\r\n"
"\r\n"
"; window = windowed | borderless | fullscreen\r\n"
";   windowed:   a normal window with the client size below.\r\n"
";   borderless: no frame, covers the monitor the window is on (the desktop keeps its resolution).\r\n"
";   fullscreen: as borderless, and always on top. (Exclusive fullscreen with a mode change is not implemented.)\r\n"
";   F11 switches between the window and a borderless window over the monitor at any time. The picture keeps its 16:9\r\n"
";   shape: the window resizes in 16:9, and other shapes (maximized, another monitor) get black bars.\r\n"
"window = windowed\r\n"
"width = 1280\r\n"
"height = 720\r\n"
"\r\n"
"; vsync = auto | display | clock | off | on | mailbox | vrr      (Vulkan)\r\n"
";   vrr:     for a G-Sync / FreeSync (variable refresh rate) monitor: the program's own 60.000 Hz clock and immediate\r\n"
";            presentation, so the monitor refreshes when each frame arrives: every frame on screen equally long at any\r\n"
";            maximum refresh rate (100, 144, 165 Hz ...). Turn variable refresh on in the GPU's control panel; most\r\n"
";            drivers apply it only to a fullscreen or borderless window. Without it: as 'off'.\r\n"
";   auto:    the refresh rate of the monitor is measured at start. 60, 120, 240 Hz (within 1.1 %: 59.94, 119.88 too):\r\n"
";            as 'display'. Any other rate (75, 100, 144, 165 Hz): as 'clock'. The log says which ('display:' lines).\r\n"
";   display: the game's frame clock IS the display's refresh (every refresh at 60 Hz, every second one at 120 Hz):\r\n"
";            each frame is on screen for exactly the same time. No tearing. The game runs at refresh / k frames per\r\n"
";            second (59.94 on a 119.88 Hz panel). Falls back to 'clock' when the rate is not a multiple of 60.\r\n"
";   clock:   the program's own 60.000 Hz clock; a finished frame is shown at the next tick of that clock (mailbox when\r\n"
";            the GPU offers it, else immediate). On a display that is not 60 / 120 / 240 Hz some frames stay on screen\r\n"
";            one refresh longer than others: that cannot be avoided there.\r\n"
";   off:     immediate presentation as soon as a frame is ready, tearing allowed. Lowest latency. Still 60 frames per second.\r\n"
";   on:      the swapchain's own vertical sync (FIFO) next to the program's clock: two clocks, not recommended.\r\n"
";   mailbox: as clock, mailbox or nothing.\r\n"
"vsync = auto\r\n"
GFX_VSYNC_AUTO_MARK
"\r\n"
"; latency = normal | low | lowest      (track L1, input latency; Vulkan)\r\n"
";   normal:  the game is let go at the tick. (The game's picture is shown one frame sooner than before 2026-10-14 in\r\n"
";            every mode: it goes to the window as soon as the game has submitted it.)\r\n"
";   low:     the game is let go late in each frame, just in time for its picture to be ready for the next tick, so it\r\n"
";            reads the controller 2-5 ms later = closer to the screen. Same cadence; with four players a few more late frames.\r\n"
";   lowest:  as low, and the picture goes to the window as soon as it is ready (no wait for the tick): about 4 ms\r\n"
";            less again, but the frames are no longer all on screen for the same time (judder).\r\n"
"latency = lowest\r\n"
"\r\n"
"; runahead = off | on      (track R1, input latency; Vulkan; needs the build with dirty-page marks)\r\n"
";   on: in a fight each frame's picture is drawn one game frame earlier: the state is saved after the game's\r\n"
";       submission, the next frame is run ahead only to draw the current one, then the state is restored. One frame\r\n"
";       less delay; costs a second pass of the game's frame (CPU) every frame. Leave it off on a weak PC.\r\n"
"runahead = off\r\n"
"\r\n"
"; gpu = auto | <number> | <part of the name>      (Vulkan)\r\n"
";   auto: a discrete GPU first, then an integrated one. The log lists the devices with their numbers:\r\n"
";   'nvn_vk: device 0: ...'. Examples: gpu = 1      gpu = Intel      gpu = RTX\r\n"
"gpu = auto\r\n"
"\r\n"
"; console_mode = docked | handheld\r\n"
";   What nn::oe::GetOperationMode answers. MEASURED on 13.0.4 (2026-10-09): with handheld the game still makes its\r\n"
";   1920x1080 window texture, so this alone does not lower the render size; kept for study, leave it at docked.\r\n"
"console_mode = docked\r\n"
"\r\n"
"; resolution_scale = 1.0\r\n"
/* PR #8 (stephenjayakar): the range goes above 1.0 (supersampling); capped at 2.0 here (review 2026-10-16), strong GPUs only */
";   Internal resolution scale, 0.25 to 2.0 (Vulkan only; OpenGL ignores it). 1.0 = the game's 1920x1080. 0.75 = 1440x810,\r\n"
";   0.5 = 960x540: the main switch for a weak GPU. The whole picture is drawn smaller, the 2D interface too (it gets\r\n"
";   softer), and is scaled to the window size. Above 1.0 it is drawn larger (strong GPUs only: GPU time and memory grow\r\n"
";   with the square): 1.5 = 2880x1620, 2.0 = 3840x2160 (a 4K screen pixel for pixel); a picture more than twice the\r\n"
";   window is reduced to it in 2:1 steps (supersampling). Details and limits: plan\\phases\\O1_options.md.\r\n"
"resolution_scale = 1.0\r\n"
"\r\n"
"; anisotropic_filtering = game | 2 | 4 | 8 | 16      (Vulkan)\r\n"
";   game: each texture sampler keeps the value the game gave it. A number: at least that much on every sampler that\r\n"
";   filters between mip levels (sharper textures at flat angles; changes the picture, costs little on any GPU).\r\n"
"anisotropic_filtering = game\r\n"
"\r\n"
"; shader_wait = hold | skip      (Vulkan)\r\n"
";   What happens when a draw needs a shader or pipeline that is not ready yet (mostly the first time something is shown\r\n"
";   on this PC). hold: the frame waits for it (a late frame, a correct picture). skip: in and around a fight the draw is\r\n"
";   left out until it is ready (no late frame; the object or effect is missing for some frames). The log counts both.\r\n"
"shader_wait = hold\r\n"
"\r\n"
"; shader_prepare = first_run | background | off      (Vulkan)\r\n"
";   first_run:  the first start on this PC (or after a new graphics driver / new game data) shows 'PREPARING SHADERS NN %'\r\n"
";               and builds every shader of the game before the title: several minutes, once. Closing the window stops it\r\n"
";               cleanly; the next start goes on from there. Later starts go straight to the game.\r\n"
";   background: the game starts at once and the same work runs behind it (first-seen things can make a frame late until it is done).\r\n"
";   off:        nothing is prepared ahead; shaders are built when the game first needs them.\r\n"
"shader_prepare = first_run\r\n"
"\r\n"
"; shader_load = auto | start | background | loading      (Vulkan, on a PC where the preparation above is done)\r\n"
";   auto:    loading on a PC with 16 GB of RAM or less, start on a PC with more (the log says which and why).\r\n"
";   start:   every shader of the game is loaded into memory at the start, behind a 'LOADING SHADERS NN %' screen (a few\r\n"
";            seconds, more on a CPU with few cores): the loading screens before a fight do no shader work. Uses more memory.\r\n"
";   background: the same loading, but the game starts at once: intro, title and menus run while it finishes (the\r\n"
";            menus may be a little less smooth meanwhile; a fight's loading screen waits for what is left).\r\n"
";   loading: as before: the shaders of a fighter or stage are loaded at its loading screen (less memory).\r\n"
"shader_load = auto\r\n"
"\r\n"
"; profile = custom | low\r\n"
";   custom: every key of this file as written. low: the set measured for a weak or integrated GPU (2026-10-12, Intel\r\n"
";   integrated: 4.2 ms of GPU time per fight frame instead of 9.0): resolution_scale = 0.5, anisotropic_filtering = game,\r\n"
";   vsync = clock; those three keys of the file are then not used. The log says which profile is active.\r\n"
"profile = custom\r\n"
"\r\n"
"; frame_time_overlay = off | on\r\n"
";   on: the average frame time of the last second in the window title ('.. fps - 16.67 ms per frame') and as a\r\n"
";   'frame-time:' line in the log. (Bars drawn into the picture are not implemented yet.)\r\n"
"frame_time_overlay = off\r\n"
"\r\n"
"; menu = on | off (default on)\r\n"
";   on: the settings menu over the game, opened with F1 or the Home / Guide (or Capture / Share) button of\r\n"
";   a controller; closed, it does nothing. It changes the keys of this file (and the controls in controllers.ini) and\r\n"
";   writes them here; the menu marks which ones need a restart. SSBU_MENU=1 / 0 wins over this key.\r\n"
"menu = on\r\n"
"\r\n"
"; stats_overlay = off | on      (with menu = on)\r\n"
";   on: a small box in the top right corner over the game: frames per second, frame time, online state, ping, input\r\n"
";   delay, rollback frames, run-ahead. F2 (or the menu) switches it and writes this key.\r\n"
"stats_overlay = off\r\n"
"\r\n"
"; volume = auto | 0 .. 100\r\n"
";   The sound volume of the copies a person plays (ssbu_native.exe, ssbu_user.exe). auto = 100. Test copies stay silent;\r\n"
";   SSBU_VOLUME=<0..100> wins. Music and effects apart: the game's own Options > Sound.\r\n"
"volume = auto\r\n"
"\r\n"
"; language = auto | ja | en-US | en-GB | fr | fr-CA | de | it | es | es-419 | zh-Hans | zh-Hant | ko | nl | ru\r\n"
";   auto: the Windows display language. SSBU_LANGUAGE=<code> wins. Read at start.\r\n"
"language = auto\r\n"
"\r\n"
"; unlock_all = off | on\r\n"
";   on: every fighter of the base roster and every stage is available at once, for going straight to competitive\r\n"
";   play, and no challenger fights interrupt. Nothing is written into the save; off again = as before. Downloadable\r\n"
";   fighters are not touched (owning them unlocks them). Spirits, music and other rewards stay as in the game.\r\n"
";   13.0.4 and 13.0.5. SSBU_UNLOCK_ALL=1 / 0 wins. Read at start.\r\n"
"unlock_all = off\r\n"
"\r\n"
"; input_delay: no longer used (2026-11-05): the input delay of online fights is always automatic, from the ping.\r\n"
"\r\n"
"; fight_sound = on | off      (on by default)\r\n"
";   Music and sound effects in online rollback fights (F1 > Gameplay). on: the sound plays. off: silent fights (the\r\n"
";   game's audio threads are held for the fight). Only this game. Used from the next match.\r\n"
GFX_FIGHT_SOUND_MARK
"fight_sound = on\r\n"
"\r\n"
"; online_menu = on | off      (on by default)\r\n"
";   The game's own Online menu (Online > Battle Arenas) on the project's server (F1 > Experimental). on: arenas with\r\n"
";   rollback 1-on-1 fights and spectators; Local Wireless rooms are not available then. Used from the next start.\r\n"
";   online_menu_server = <IPv4 address> changes the server (default " GFX_ONLINE_MENU_SERVER_DEFAULT ").\r\n"
GFX_ONLINE_DEFAULTS_MARK
"online_menu = on\r\n"
"\r\n"
"; refresh_switch = auto | off\r\n"
";   auto: on a screen that is not a multiple of 60 Hz (75, 144, 165, 175 Hz ...) the game sets it to 120 Hz (or 60 /\r\n"
";   180 / 240) while it runs, so every frame stays on screen equally long; Windows puts the old rate back when the game\r\n"
";   closes. Not with vsync = vrr (G-Sync / FreeSync). off: the screen is never changed. Read at start.\r\n"
"refresh_switch = auto\r\n"
"\r\n"
"; sharpening = 0 .. 100      (Vulkan)\r\n"
";   0 = off. Sharpens the picture on its way to the window, without halos (kept inside each pixel's neighbours).\r\n"
";   Try 30 to 50. Costs one small GPU pass per frame. Read at start.\r\n"
"sharpening = 0\r\n"
"\r\n"
"; screenshot_key = F12 | F1 .. F24 | off\r\n"
";   One press while the game window has the focus saves the picture as a PNG in the screenshots folder next to saves.\r\n"
";   Avoid F1 (menu), F2 (stats) and F11 (fullscreen). Read at start.\r\n"
"screenshot_key = F12\r\n"
"\r\n"
"; screenshot_dir = <a folder> | empty\r\n"
";   Where the screenshot key saves its pictures. Empty: the screenshots folder next to saves. Made when missing.\r\n"
"screenshot_dir =\r\n";

#define GFX_LOW_SCALE 0.5f
#define GFX_LOW_SCALE_TEXT "0.50"
static GfxOptions g_opt;
static INIT_ONCE g_once = INIT_ONCE_STATIC_INIT;

static void trim(char *s) {
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
    char *b = s;
    while (*b == ' ' || *b == '\t') b++;
    if (b != s) memmove(s, b, strlen(b) + 1);
}
static void opt_path(char *out) {
    const char *e = getenv("SSBU_GRAPHICS_CONFIG"), *c = getenv("SSBU_CONFIG");
    char tmp[MAX_PATH], exe[MAX_PATH];
    if (e && e[0]) { snprintf(out, MAX_PATH, "%s", e); return; }
    if (c && c[0]) {
        snprintf(tmp, sizeof tmp, "%s", c);
        char *s = strrchr(tmp, '\\'), *s2 = strrchr(tmp, '/');
        if (s2 > s) s = s2;
        if (s) { *s = 0; snprintf(out, MAX_PATH, "%s\\graphics.ini", tmp); } else snprintf(out, MAX_PATH, "graphics.ini");
        return;
    }
    GetModuleFileNameA(NULL, exe, MAX_PATH);
    char *s = strrchr(exe, '\\');
    if (s) *s = 0;
    snprintf(tmp, sizeof tmp, "%s\\..\\config\\graphics.ini", exe);
    if (!_fullpath(out, tmp, MAX_PATH)) snprintf(out, MAX_PATH, "%s", tmp);
}
/* (2026-11-02) a graphics.ini written before shader_load = auto existed: its comment header lists "start | background |
 * loading" and the template wrote "shader_load = start", which nearly every player kept. On a PC with 16 GB of RAM that
 * start (~4 GB more: every shader of the game in memory) plus the rollback's state went to the page file (the owner's
 * tests Test_F/G_mem: 12.0 GB in RAM in a fight with start, 8.7 GB with loading). Once per file: the header is replaced by
 * the new one, and shader_load = start becomes auto; any other value (a player's own choice) stays. The new header is the
 * mark that this was done: a later "start" chosen in the F1 menu stays start. */
static void ini_migrate_shader_load(const char *path) {
    static const char old_hdr[] = "; shader_load = start |";      /* both older headers: "start | loading" (2026-10-16) and "start | background | loading" */
    static const char new_hdr[] = "; shader_load = auto | start | background | loading      (Vulkan, on a PC where the preparation above is done)\r\n"
                                  ";   auto:    loading on a PC with 16 GB of RAM or less, start on a PC with more (the log says which and why).\r\n";
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *old = n > 0 ? malloc((size_t)n + 1) : NULL;
    if (old) { n = (long)fread(old, 1, (size_t)n, f); old[n] = 0; }
    fclose(f);
    if (!old || !strstr(old, old_hdr)) { free(old); return; }
    char *out = malloc((size_t)n + sizeof new_hdr + 64);
    if (!out) { free(old); return; }
    size_t o2 = 0; int hdr = 0, val = 0;
    for (char *p = old; *p; ) {
        char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) + 1 : strlen(p);
        char line[512]; size_t ll = l < sizeof line - 1 ? l : sizeof line - 1;
        memcpy(line, p, ll); line[ll] = 0; trim(line);
        if (!hdr && !strncmp(line, old_hdr, sizeof old_hdr - 1)) { memcpy(out + o2, new_hdr, sizeof new_hdr - 1); o2 += sizeof new_hdr - 1; hdr = 1; }
        else if (!val && line[0] != ';' && !_strnicmp(line, "shader_load", 11)) {
            char *q = line + 11; while (*q == ' ' || *q == '\t') q++;
            if (*q == '=') { q++; while (*q == ' ' || *q == '\t') q++; }
            if (!_stricmp(q, "start")) { o2 += (size_t)sprintf(out + o2, "shader_load = auto\r\n"); val = 1; }
            else { memcpy(out + o2, p, l); o2 += l; }
        }
        else { memcpy(out + o2, p, l); o2 += l; }
        p += l;
    }
    char tmp[MAX_PATH + 8]; int ok = 0;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if ((f = fopen(tmp, "wb")) != NULL) {
        ok = fwrite(out, 1, o2, f) == o2;
        ok = !fclose(f) && ok;
        ok = ok && MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }
    printf("graphics options: %s: the shader_load comment updated (new value auto)%s%s\n", path, val ? "; shader_load = start (the old default) became auto" : "; shader_load kept as written",
           ok ? "" : " - could NOT be written: the file is read as it is");
    free(old); free(out);
}
/* (2026-11-04) the players did not know Battle Arenas (online_menu) and rollback had to be turned on in F1: once per file,
 * online_menu = off becomes on and rollback = off becomes on (a missing key is on already), and the mark line is added before
 * online_menu (or at the end). With the mark there, the file is left as written: a later OFF from F1 or the launcher stays. */
static void ini_migrate_online_defaults(const char *path) {
    static const char mark[] = GFX_ONLINE_DEFAULTS_MARK;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *old = n > 0 ? malloc((size_t)n + 1) : NULL;
    if (old) { n = (long)fread(old, 1, (size_t)n, f); old[n] = 0; }
    fclose(f);
    if (!old || strstr(old, "Battle Arenas and rollback netcode are on by default")) { free(old); return; }
    char *out = malloc((size_t)n + sizeof mark + 256);
    if (!out) { free(old); return; }
    size_t o2 = 0; int marked = 0, om = 0, rb = 0;
    for (char *p = old; *p; ) {
        char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) + 1 : strlen(p);
        char line[512]; size_t ll = l < sizeof line - 1 ? l : sizeof line - 1;
        memcpy(line, p, ll); line[ll] = 0; trim(line);
        int is_om = line[0] != ';' && !_strnicmp(line, "online_menu", 11) && (line[11] == ' ' || line[11] == '\t' || line[11] == '=');
        int is_rb = line[0] != ';' && !_strnicmp(line, "rollback", 8) && (line[8] == ' ' || line[8] == '\t' || line[8] == '=');
        const char *q = NULL;
        if (is_om || is_rb) { q = strchr(line, '='); q = q ? q + 1 : ""; while (*q == ' ' || *q == '\t') q++; }
        int off = q && (!_stricmp(q, "off") || !strcmp(q, "0") || !_stricmp(q, "no") || !_stricmp(q, "false"));
        if (is_om && !marked) { memcpy(out + o2, mark, sizeof mark - 1); o2 += sizeof mark - 1; marked = 1; }
        if (is_om && off) { o2 += (size_t)sprintf(out + o2, "online_menu = on\r\n"); om = 1; }
        else if (is_rb && off) { o2 += (size_t)sprintf(out + o2, "rollback = on\r\n"); rb = 1; }
        else { memcpy(out + o2, p, l); o2 += l; }
        p += l;
    }
    if (!marked) {
        if (o2 && out[o2 - 1] != '\n') { out[o2++] = '\r'; out[o2++] = '\n'; }
        memcpy(out + o2, mark, sizeof mark - 1); o2 += sizeof mark - 1;
    }
    char tmp[MAX_PATH + 8]; int ok = 0;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if ((f = fopen(tmp, "wb")) != NULL) {
        ok = fwrite(out, 1, o2, f) == o2;
        ok = !fclose(f) && ok;
        ok = ok && MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }
    printf("graphics options: %s: Battle Arenas and rollback are on by default now (once)%s%s%s\n", path, om ? "; online_menu = off became on" : "",
           rb ? "; rollback = off became on" : "", ok ? "" : " - could NOT be written: the file is read as it is");
    free(old); free(out);
}
/* (2026-11-05, owner: "we played with sound and everything goes well": the sound of online fights on by default) once per
 * file: fight_sound = off becomes on and the mark line goes before it (or at the end). With the mark there, the file is left
 * as written: a later OFF from F1 > Gameplay stays. */
static void ini_migrate_fight_sound(const char *path) {
    static const char mark[] = GFX_FIGHT_SOUND_MARK;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *old = n > 0 ? malloc((size_t)n + 1) : NULL;
    if (old) { n = (long)fread(old, 1, (size_t)n, f); old[n] = 0; }
    fclose(f);
    if (!old || strstr(old, "the sound of online fights is on by default")) { free(old); return; }
    char *out = malloc((size_t)n + sizeof mark + 64);
    if (!out) { free(old); return; }
    size_t o2 = 0; int marked = 0, fs = 0;
    for (char *p = old; *p; ) {
        char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) + 1 : strlen(p);
        char line[512]; size_t ll = l < sizeof line - 1 ? l : sizeof line - 1;
        memcpy(line, p, ll); line[ll] = 0; trim(line);
        int is_fs = line[0] != ';' && !_strnicmp(line, "fight_sound", 11) && (line[11] == ' ' || line[11] == '\t' || line[11] == '=');
        const char *q = NULL;
        if (is_fs) { q = strchr(line, '='); q = q ? q + 1 : ""; while (*q == ' ' || *q == '\t') q++; }
        int off = q && (!_stricmp(q, "off") || !strcmp(q, "0") || !_stricmp(q, "no") || !_stricmp(q, "false"));
        if (is_fs && !marked) { memcpy(out + o2, mark, sizeof mark - 1); o2 += sizeof mark - 1; marked = 1; }
        if (is_fs && off) { o2 += (size_t)sprintf(out + o2, "fight_sound = on\r\n"); fs = 1; }
        else { memcpy(out + o2, p, l); o2 += l; }
        p += l;
    }
    if (!marked) {
        if (o2 && out[o2 - 1] != '\n') { out[o2++] = '\r'; out[o2++] = '\n'; }
        memcpy(out + o2, mark, sizeof mark - 1); o2 += sizeof mark - 1;
    }
    char tmp[MAX_PATH + 8]; int ok = 0;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if ((f = fopen(tmp, "wb")) != NULL) {
        ok = fwrite(out, 1, o2, f) == o2;
        ok = !fclose(f) && ok;
        ok = ok && MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }
    printf("graphics options: %s: the sound of online fights is on by default now (once)%s%s\n", path, fs ? "; fight_sound = off became on" : "",
           ok ? "" : " - could NOT be written: the file is read as it is");
    free(old); free(out);
}
/* (2026-11-07, owner: "it must not happen on any user's monitor") two players had vsync = on (Lenzzz, 180 Hz: the swapchain's
 * FIFO beside the game's clock) and vsync = clock (Porky, switched to 60 Hz but not locked to it): written by an older
 * version or tried once, and both keep the game's frames off the monitor's refresh, in the menus and now in online fights
 * (rollback_controller.c paces them on the display lock's ticks). Once per file: on / clock / mailbox becomes auto and the
 * mark line goes after it (or at the end). With the mark there the file is left as written: F1 can choose clock again. */
static void ini_migrate_vsync(const char *path) {
    static const char mark[] = GFX_VSYNC_AUTO_MARK;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *old = n > 0 ? malloc((size_t)n + 1) : NULL;
    if (old) { n = (long)fread(old, 1, (size_t)n, f); old[n] = 0; }
    fclose(f);
    if (!old || strstr(old, "of an older file became auto once")) { free(old); return; }
    char *out = malloc((size_t)n + sizeof mark + 64);
    if (!out) { free(old); return; }
    size_t o2 = 0; int marked = 0; char was[16] = "";
    for (char *p = old; *p; ) {
        char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) + 1 : strlen(p);
        char line[512]; size_t ll = l < sizeof line - 1 ? l : sizeof line - 1;
        memcpy(line, p, ll); line[ll] = 0; trim(line);
        int is_vs = line[0] != ';' && !_strnicmp(line, "vsync", 5) && (line[5] == ' ' || line[5] == '\t' || line[5] == '=');
        const char *q = NULL;
        if (is_vs) { q = strchr(line, '='); q = q ? q + 1 : ""; while (*q == ' ' || *q == '\t') q++; }
        int change = q && (!_stricmp(q, "on") || !_stricmp(q, "clock") || !_stricmp(q, "mailbox"));
        if (change) { snprintf(was, sizeof was, "%s", q); o2 += (size_t)sprintf(out + o2, "vsync = auto\r\n"); }
        else { memcpy(out + o2, p, l); o2 += l; }
        if (is_vs && !marked) { memcpy(out + o2, mark, sizeof mark - 1); o2 += sizeof mark - 1; marked = 1; }
        p += l;
    }
    if (!marked) {
        if (o2 && out[o2 - 1] != '\n') { out[o2++] = '\r'; out[o2++] = '\n'; }
        memcpy(out + o2, mark, sizeof mark - 1); o2 += sizeof mark - 1;
    }
    char tmp[MAX_PATH + 8]; int ok = 0;
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if ((f = fopen(tmp, "wb")) != NULL) {
        ok = fwrite(out, 1, o2, f) == o2;
        ok = !fclose(f) && ok;
        ok = ok && MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }
    printf("graphics options: %s: vsync follows the monitor by default now (once)%s%s%s%s\n", path, was[0] ? "; vsync = " : "", was, was[0] ? " became auto" : "",
           ok ? "" : " - could NOT be written: the file is read as it is");
    free(old); free(out);
}
static BOOL CALLBACK opt_load(PINIT_ONCE once, PVOID arg, PVOID *ctx) {
    (void)once; (void)arg; (void)ctx;
    GfxOptions *o = &g_opt;
    int written = 0, unknown = 0;
    memset(o, 0, sizeof *o);
    o->width = 1280; o->height = 720; o->scale = 1.0f; o->present = 5;      /* vsync = auto when the key is missing (track V session 14) */
    o->shader_load_auto = 1;                                                  /* shader_load = auto when the key is missing (2026-11-02) */
    opt_path(o->path);
    ini_migrate_shader_load(o->path);                                         /* (a file of an older version: once) */
    ini_migrate_online_defaults(o->path);                                     /* (2026-11-04: arenas and rollback on; once) */
    ini_migrate_fight_sound(o->path);                                         /* (2026-11-05: sound of online fights on; once) */
    ini_migrate_vsync(o->path);                                               /* (2026-11-07: vsync on / clock / mailbox -> auto; once) */
    FILE *f = fopen(o->path, "rb");
    if (!f) {
        char b[MAX_PATH];
        snprintf(b, sizeof b, "%s", o->path);
        for (char *p = b + 3; *p; p++) if (*p == '\\' || *p == '/') { *p = 0; CreateDirectoryA(b, NULL); *p = '\\'; }
        if ((f = fopen(o->path, "wb")) != NULL) { fwrite(k_default_ini, 1, strlen(k_default_ini), f); fclose(f); written = 1; }
        f = fopen(o->path, "rb");
    }
    char line[512];
    while (f && fgets(line, sizeof line, f)) {
        char *eq, *key = line, *val;
        trim(line);
        if (!line[0] || line[0] == ';' || line[0] == '#' || line[0] == '[' || !(eq = strchr(line, '='))) continue;
        *eq = 0; val = eq + 1;
        trim(key); trim(val);
        if (!_stricmp(key, "renderer")) o->renderer = !_stricmp(val, "opengl") || !_stricmp(val, "gl");
        else if (!_stricmp(key, "window")) o->window_mode = !_stricmp(val, "borderless") ? 1 : !_stricmp(val, "fullscreen") ? 2 : 0;
        else if (!_stricmp(key, "width")) { int v = atoi(val); if (v >= 320 && v <= 16384) o->width = v; }
        else if (!_stricmp(key, "height")) { int v = atoi(val); if (v >= 180 && v <= 16384) o->height = v; }
        else if (!_stricmp(key, "vsync")) o->present = !_stricmp(val, "on") ? 1 : !_stricmp(val, "off") ? 2 : !_stricmp(val, "mailbox") ? 3 : !_stricmp(val, "display") ? 4 : !_stricmp(val, "auto") ? 5 : !_stricmp(val, "vrr") ? 6 : 0;
        else if (!_stricmp(key, "gpu")) { if (_stricmp(val, "auto")) snprintf(o->gpu, sizeof o->gpu, "%s", val); }
        else if (!_stricmp(key, "console_mode")) o->handheld = !_stricmp(val, "handheld");
        else if (!_stricmp(key, "resolution_scale")) { float v = (float)atof(val); if (v > 0) o->scale = v; }
        else if (!_stricmp(key, "anisotropic_filtering")) { int v = atoi(val); o->aniso = v == 2 || v == 4 || v == 8 || v == 16 ? v : 0; }
        else if (!_stricmp(key, "frame_time_overlay")) o->overlay = !_stricmp(val, "on") || !strcmp(val, "1");
        else if (!_stricmp(key, "profile")) o->profile = !_stricmp(val, "low");
        else if (!_stricmp(key, "shader_wait")) o->shader_skip = !_stricmp(val, "skip");
        else if (!_stricmp(key, "runahead")) g_gfx_runahead = !_stricmp(val, "on") || !_stricmp(val, "1");      /* track R1 round 5 (r1_state.c) */
        else if (!_stricmp(key, "latency")) g_gfx_latency = !_stricmp(val, "lowest") ? 2 : !_stricmp(val, "low") ? 1 : 0;      /* track L1 (display.c latency_mode) */
        else if (!_stricmp(key, "shader_prepare")) o->shader_prepare = !_stricmp(val, "background") ? 1 : !_stricmp(val, "off") ? 2 : 0;
        else if (!_stricmp(key, "shader_load")) { o->shader_load_auto = !_stricmp(val, "auto"); o->shader_load = !_stricmp(val, "loading") ? 1 : !_stricmp(val, "background") ? 2 : 0; }
        else if (!_stricmp(key, "stats_overlay")) g_gfx_stats = !_stricmp(val, "on") || !strcmp(val, "1");      /* U1 (menu.cpp, F2) */
        else if (!_stricmp(key, "menu")) g_gfx_menu = !_stricmp(val, "on") || !strcmp(val, "1");      /* U1 (menu.cpp) */
        else if (!_stricmp(key, "volume")) { g_gfx_volume = -1; if (_stricmp(val, "auto")) { int v = atoi(val); g_gfx_volume = v < 0 ? 0 : v > 100 ? 100 : v; } }
        else if (!_stricmp(key, "language")) { g_gfx_language[0] = 0; if (_stricmp(val, "auto")) snprintf(g_gfx_language, sizeof g_gfx_language, "%s", val); }
        else if (!_stricmp(key, "unlock_all")) g_gfx_unlock_all = !_stricmp(val, "on") || !strcmp(val, "1");      /* unlock_all.c */
        else if (!_stricmp(key, "input_delay")) { }    /* (2026-11-05, owner) not read: every start is Auto (F1 > Gameplay changes it for the session) */
        else if (!_stricmp(key, "fight_sound")) g_gfx_fight_sound = !_stricmp(val, "on") || !strcmp(val, "1");      /* snapshot_ring.c */
        else if (!_stricmp(key, "rollback_delay_only")) g_gfx_rb_delay_only = !_stricmp(val, "on") || !strcmp(val, "1");      /* rollback_controller.c */
        else if (!_stricmp(key, "online_menu")) g_gfx_online_menu = !_stricmp(val, "on") || !strcmp(val, "1");      /* gfx_online_menu_env */
        else if (!_stricmp(key, "online_menu_server")) { if (gfx_ipv4(val)) snprintf(g_gfx_online_menu_server, sizeof g_gfx_online_menu_server, "%s", val); }
        else if (!_stricmp(key, "refresh_switch")) g_gfx_refresh_switch = !(!_stricmp(val, "off") || !strcmp(val, "0"));      /* display.c */
        else if (!_stricmp(key, "sharpening")) { int v = atoi(val); g_gfx_sharpening = v < 0 ? 0 : v > 100 ? 100 : v; }      /* nvn_vk_draw.c */
        else if (!_stricmp(key, "screenshot_dir")) snprintf(g_gfx_shot_dir, sizeof g_gfx_shot_dir, "%s", val);      /* nvn_vk_draw.c */
        else if (!_stricmp(key, "screenshot_key")) { int n = (val[0] == 'F' || val[0] == 'f') ? atoi(val + 1) : 0; g_gfx_shot_key = n >= 1 && n <= 24 ? 0x70 + n - 1 : 0; }      /* VK_F1 = 0x70 */
        else unknown++;
    }
    if (f) fclose(f);
    {   /* shader_load = auto (2026-11-02): loading with 16 GB of RAM or less (a "16 GB" PC reports ~15.8 GB; under 20 GB counts),
         * start with more. Measured on the owner's PC (Test_F/G_mem): in a rollback fight 12.0 GB in RAM with start, 8.7 GB with loading */
        MEMORYSTATUSEX ms; memset(&ms, 0, sizeof ms); ms.dwLength = sizeof ms;
        double gb = GlobalMemoryStatusEx(&ms) ? (double)ms.ullTotalPhys / 1073741824.0 : 0.0;
        { const char *t = getenv("SSBU_TEST_RAM_GB"); if (t && atof(t) > 0) { gb = atof(t); printf("graphics options: SSBU_TEST_RAM_GB=%s: TEST, this PC's RAM taken as %.1f GB\n", t, gb); } }
        o->ram_gb = (int)(gb + 0.5);
        /* (2026-11-07, owner: "this should happen by itself") Falsewolf: 31.9 GB of RAM, so start, but only ~14.5 GB of
         * commit free when the game opened (RAM + page file, minus the other programs): the game took 12.4 GB, the
         * rollback ring's base state needs ~2.4 GB + 1 GB kept free, 2.2-2.5 GB were left, and every online match of the
         * session ran WITHOUT rollback (delay-based, input delay forced to 3 for both players). Now auto also looks at the
         * commit free at start: under 18 GB (start's ~12.5 GB + the ring ~3.4 GB + 2 GB for the other programs) it is
         * loading, ~3.3 GB less, so the ring fits. SSBU_TEST_COMMIT_GB: that free commit (test). */
        double commit_gb = ms.dwLength && ms.ullAvailPageFile ? (double)ms.ullAvailPageFile / 1073741824.0 : 0.0;
        { const char *t = getenv("SSBU_TEST_COMMIT_GB"); if (t && atof(t) > 0) { commit_gb = atof(t); printf("graphics options: SSBU_TEST_COMMIT_GB=%s: TEST, free commit taken as %.1f GB\n", t, commit_gb); } }
        if (o->shader_load_auto) {
            int low_ram = gb > 0.0 && gb < 20.0, low_commit = commit_gb > 0.0 && commit_gb < 18.0;
            o->shader_load = low_ram || low_commit ? 1 : 0;
            printf("graphics options: shader_load = auto: %s (this PC has %.1f GB of RAM, %.1f GB of commit free now (RAM + page file, minus the other programs); 16 GB of RAM or less, or under 18 GB of commit free: loading, which uses ~4 GB less memory, so the online rollback has room)\n",
                   o->shader_load ? "loading" : "start", gb, commit_gb);
        }
    }
    if (o->profile == 1) {       /* the low-end profile wins over the three keys it sets, wherever they stand in the file (values: O1_options.md, session 9) */
        o->scale = GFX_LOW_SCALE; o->aniso = 0; o->present = 0;
    }
    printf("graphics profile: %s\n", o->profile == 1 ? "low (weak or integrated GPU): resolution_scale=" GFX_LOW_SCALE_TEXT ", anisotropic_filtering=game, vsync=clock; these three keys of the file are not used"
                                                     : "custom (every key of graphics.ini as written; 'profile = low' is the set for a weak or integrated GPU)");
    static const char *const k_win[3] = {"windowed", "borderless", "fullscreen"}, *const k_vs[7] = {"clock", "on", "off", "mailbox", "display", "auto", "vrr"};
    printf("graphics options: %s (%s): renderer=%s window=%s %dx%d vsync=%s gpu=%s console_mode=%s resolution_scale=%.2f anisotropic_filtering=%d frame_time_overlay=%s%s\n", o->path,
           written ? "written with the defaults" : f ? "read" : "NOT readable, defaults used", o->renderer ? "opengl" : "vulkan", k_win[o->window_mode], o->width, o->height, k_vs[o->present],
           o->gpu[0] ? o->gpu : "auto", o->handheld ? "handheld" : "docked", o->scale, o->aniso, o->overlay ? "on" : "off", unknown ? " (lines with an unknown key were ignored)" : "");
    if (o->scale != 1.0f && o->renderer) printf("graphics options: resolution_scale=%.2f is ignored by the OpenGL renderer (Vulkan only)\n", o->scale);
    if (o->scale != 1.0f && (o->scale < 0.25f || o->scale > 2.0f)) printf("graphics options: resolution_scale=%.2f is outside 0.25 .. 2.0: the nearest of the two is used\n", o->scale);      /* PR #8 (stephenjayakar): above 1.0 allowed (nvn_vk_draw.c RS_MAX = 2.0) */
    fflush(stdout);
    return TRUE;
}
const GfxOptions *gfx_options(void) {
    InitOnceExecuteOnce(&g_once, opt_load, NULL, NULL);
    return &g_opt;
}

/* ---- U1, the settings menu (menu.cpp): changes while running, and the writer of graphics.ini */
void gfx_set_overlay(int on) { gfx_options(); g_opt.overlay = on != 0; }
/* `key = value` into the file: the first line that sets the key (not a comment) is replaced, every other line stays as it
 * is; a key that is not in the file is appended. Written to <file>.tmp, then moved over the file. */
int gfx_ini_set(const char *key, const char *value) {
    const GfxOptions *o = gfx_options();
    static CRITICAL_SECTION cs;
    static volatile LONG cs_ready;
    if (InterlockedCompareExchange(&cs_ready, 1, 0) == 0) { InitializeCriticalSection(&cs); cs_ready = 2; }
    while (cs_ready != 2) Sleep(0);
    EnterCriticalSection(&cs);
    FILE *f = fopen(o->path, "rb");
    char *old = NULL;
    long n = 0;
    if (f) { fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET); old = malloc((size_t)n + 1); if (old) { n = (long)fread(old, 1, (size_t)n, f); old[n] = 0; } fclose(f); }
    size_t cap = (size_t)n + strlen(key) + strlen(value) + 64, o2 = 0, kl = strlen(key);
    char *out = malloc(cap), tmp[MAX_PATH + 8];
    int done = 0, ok = 0;
    if (!out) { free(old); LeaveCriticalSection(&cs); return 0; }
    for (char *p = old; p && *p; ) {
        char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) + 1 : strlen(p);
        const char *q = p, *r;
        while (*q == ' ' || *q == '\t') q++;
        for (r = q + kl; *r == ' ' || *r == '\t'; r++) ;
        if (!done && !_strnicmp(q, key, kl) && *r == '=') { o2 += (size_t)sprintf(out + o2, "%s = %s\r\n", key, value); done = 1; }
        else { memcpy(out + o2, p, l); o2 += l; }
        p += l;
    }
    if (!done) { if (o2 && out[o2 - 1] != '\n') { out[o2++] = '\r'; out[o2++] = '\n'; } o2 += (size_t)sprintf(out + o2, "%s = %s\r\n", key, value); }
    snprintf(tmp, sizeof tmp, "%s.tmp", o->path);
    if ((f = fopen(tmp, "wb")) != NULL) {
        ok = fwrite(out, 1, o2, f) == o2;
        ok = !fclose(f) && ok;
        ok = ok && MoveFileExA(tmp, o->path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }
    printf("graphics options: %s = %s %s %s\n", key, value, ok ? "written to" : "could NOT be written to", o->path);
    fflush(stdout);
    free(old); free(out);
    LeaveCriticalSection(&cs);
    return ok;
}
