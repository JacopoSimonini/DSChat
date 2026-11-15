// App state shared by the logic (app.c), the drawing code (ui.c) and the
// settings screen (settings.c). Everything is statically allocated; nothing is
// allocated in the main loop.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "gfx.h"
#include "config.h"
#include "input.h"
#include "kbd.h"
#include "proto.h"
#include "storage.h"
#include "textbuf.h"

#define VIEW_ROWS     23    // conversation rows on the top screen (row 0 = status)
#define INPUT_ROWS    4     // visible input rows on the bottom screen
#define INPUT_Y       12    // y of the first input row
#define KBD_Y         47    // keyboard rectangle
#define KBD_H         145
#define MAX_PROFILES  12
#define BOOT_WINDOW   (180) // frames (3 s) to hold SELECT for settings at start

enum reply_state { R_IDLE = 0, R_WAITING, R_STREAMING, R_CANCELLING };
enum menu_page
{
    MENU_NONE = 0, MENU_MAIN, MENU_PROFILES, MENU_ABOUT, MENU_SOAK, MENU_SETTINGS,
    MENU_POWER,
};
// Main menu items (2 columns x 4 rows)
enum main_item
{
    MAIN_NEW = 0, MAIN_PROFILE, MAIN_SETTINGS, MAIN_RECONNECT, MAIN_SOAK, MAIN_ABOUT,
    MAIN_POWER, MAIN_CLOSE, MAIN_ITEMS
};
enum req_tag { TAG_SEND = 1, TAG_RSET, TAG_PLST, TAG_PSET, TAG_SOAK, TAG_PSET_RESTORE };

typedef struct
{
    dschat_config *cfg;         // live settings (saved on the SD card)
    storage_t store;
    bool cfg_dirty;             // changed but not saved yet (save failed)
    textbuf tb;
    input_t in;
    kbd_t kbd;
    uint32_t now;

    // start-up
    bool boot_wait;             // "hold SELECT for settings" window
    uint32_t boot_until;
    bool auto_settings_done;    // settings already opened because connecting failed
    char last_reason[64];       // last connection failure

    // conversation view
    uint32_t view_top;          // absolute line number of the first visible row
    bool follow;                // stick to the newest text
    int in_scroll;              // first visible input row

    // current reply
    int reply;
    uint32_t reply_start;
    bool reply_text;            // a TEXT paragraph was started
    bool local_cancel;          // old relay: ignore the rest of the reply
    char last_sent[INPUT_CAP + 1];

    // connection
    bool wifi_ok;
    bool ever_online;
    char boot[17];
    char profile[17];
    char model[48];
    char notice[4][64];         // notices shown during this outage (deduplicated)
    bool user_reconnect;        // Reconnect chosen in the menu (no "lost" line)

    // menu
    int menu;
    int menu_sel;
    int menu_scroll;
    proto_profile profiles[MAX_PROFILES];
    int nprofiles;
    bool menu_wait;             // waiting for a relay answer (PLST / PSET / RSET)
    char menu_msg[33];

    // keyboard feedback
    int pressed;                // key index held down, -1 = none
    uint32_t pressed_since;

    // what must be redrawn
    bool dirty_conv, dirty_input, dirty_kbd, dirty_menu;
} app_t;

extern app_t app;

void app_init(dschat_config *cfg);
void app_boot(uint32_t now);        // after net_init: auto-connect or settings
void app_input(uint32_t now);
void app_tick(uint32_t now);
void app_say(uint8_t kind, const char *text);    // a line in the conversation
void app_close_menu(void);
void app_save_config(void);         // save app.cfg to the SD card (if any)

// View helpers (app.c)
uint32_t app_display_end(void);     // one past the last displayed line
void app_view_update(void);

// Drawing (ui.c)
void ui_render(void);
enum status_kind { ST_OK = 0, ST_BUSY, ST_BAD };
void ui_status_text(char *out, int size, int *kind);
void ui_draw_header(const char *left, const char *right, bool right_alert);
void ui_draw_input_rows(const input_t *in, int *scroll, int y0, int nrows);
void ui_draw_kbd(const kbd_t *k, int pressed, const char *send_label, u16 send_bg);
void ui_kbd_invalidate(void);
