/* U1: the in-game settings menu (Dear ImGui over the game's picture, plan\phases\U1_settings_menu.md).
 * menu.cpp holds the interface and the settings; nvn_vk_menu.c (included by nvn_vk_draw.c) draws what it built.
 * Off unless SSBU_MENU=1 or graphics.ini `menu = on` (SSBU_MENU=0 wins over the file). The on-screen keyboard
 * (osk.c) uses the same drawing layer on Linux / macOS, also when the menu itself is off. */
#ifndef MENU_H
#define MENU_H
#include <stdint.h>
#include <wchar.h>
#ifdef __cplusplus
extern "C" {
#endif

/* any thread */
int  menu_enabled(void);                /* the menu exists in this run (read once) */
int  menu_is_open(void);
int  menu_input_blocked(void);          /* nnhid.c: the game gets no input from the real pads and keyboard (open, or a button still held after closing) */
void menu_request_toggle(void);         /* F1 (display.c) or the pad combination: applied at the next drawn frame */
/* R3 (rb_net.c): a message box in the middle of the picture while the game itself shows nothing new (the online match
 * waiting for the other player). NULL or "" removes it. The caller redraws the last frame (nvn_front_osk_redraw). */
void menu_notice(const char *text);
/* O1 (net_relay.c): a small box at the top of the picture for 7 s, over the game, taking no input ("Cannot join this
 * room" + the cause). Any thread; a new one replaces the old. */
void menu_popup(const char *title, const char *text);
void menu_toast(const char *text, int ms);      /* a short note at the bottom that goes by itself after ms (default 3000) */

/* display.c, window thread: 1 = the message was taken by the menu (the window procedure returns 0) */
int  menu_wndproc(void *hwnd, unsigned msg, uintptr_t w, intptr_t l);

/* render thread (nvn_vk_menu.c) */
typedef struct { float clip[4]; uint32_t idx_off, vtx_off, count; } MenuCmd;
typedef struct {
    const void *vtx; const uint16_t *idx; const MenuCmd *cmd;   /* vertices: 20 bytes each (pos x y, uv u v, colour RGBA8) */
    int nvtx, nidx, ncmd;
    float disp_w, disp_h;
} MenuDraw;
int  menu_frame(int width, int height, MenuDraw *out);      /* 1 = something to draw this frame (out filled, valid until the next call) */
const unsigned char *menu_font_rgba(int *w, int *h);         /* the font atlas, after the first menu_frame that returned 1 */
int  menu_shot_wanted(char *path, int n);                    /* SSBU_MENU_TEST: 1 = read the swapchain image back after drawing this frame, into path */
void menu_shot_done(const char *path, int w, int h, int ok); /* the BMP was written (or not) */

/* The on-screen keyboard (osk.c). Windows draws it with GDI; Linux / macOS have the menu's ImGui layer draw it.
 * osk_view is a copy of what the keyboard shows, taken on the render thread while it is open. */
#define OSK_COLS 11
#define OSK_ROWS 4
#define OSK_TEXT_MAX 500
typedef struct {
    int is_message, question;                       /* the game's text check instead of the keys */
    wchar_t header[96], guide[288], note[128], text[OSK_TEXT_MAX + 2], message[1024];
    wchar_t keys[OSK_ROWS][OSK_COLS];               /* the characters shown on the keys (page / shift applied) */
    int len, cur, max_len, password, page, row, col, shift_once;
} OskView;
int  osk_active(void);                              /* any thread: 1 while the keyboard is open */
int  osk_view(OskView *v);                          /* non-Windows render thread: 1 = open, v filled */
int  osk_key(unsigned msg, uintptr_t w);            /* display.c / menu.cpp: 1 = the key went to the keyboard */
int  menu_osk_ready(void);                          /* non-Windows: 1 = the menu's ImGui layer can draw the keyboard */

#ifdef __cplusplus
}
#endif
#endif
