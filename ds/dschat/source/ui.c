// Drawing: top screen = status bar + conversation, bottom screen = input box
// + keyboard, or a menu page. The bottom screen is drawn into a back buffer
// (gfx.c) that is copied to VRAM at VBlank, and only what changed is redrawn:
// the keyboard redraws just the keys whose look changed.

#include <stdio.h>
#include <string.h>

#include "app.h"
#include "dschat_build.h"
#include "gfx.h"
#include "net.h"
#include "settings.h"
#include "soak.h"
#include "voice.h"

static u16 kind_color(uint8_t kind)
{
    switch (kind)
    {
        case TB_USER:  return COL_USER;
        case TB_INFO:  return COL_INFO;
        case TB_ERROR: return COL_ERROR;
        default:       return COL_TEXT;
    }
}

// --------------------------------------------------------------------------
// Top screen (drawn directly; its rows are only ever overwritten, never
// cleared first, so it does not flicker)
// --------------------------------------------------------------------------

// State shown on the right of the top status bar. kind: what the marker and
// the chip colour show (the text alone already says it; the LCD is dim, so
// nothing relies on colour only).
void ui_status_text(char *out, int size, int *kind)
{
    int k = ST_BUSY;
    if (!app.wifi_ok)
    {
        snprintf(out, size, "Wi-Fi init FAILED");
        k = ST_BAD;
    }
    else if (app.boot_wait)
    {
        snprintf(out, size, "SELECT: settings");
    }
    else
    {
        switch (net_state())
        {
            case NS_SCAN:    snprintf(out, size, "Scan: %d APs", net_scan_count()); break;
            case NS_ASSOC:   snprintf(out, size, "Joining Wi-Fi"); break;
            case NS_CONNECT: snprintf(out, size, "Connecting"); break;
            case NS_RADIO_RESET: snprintf(out, size, "Radio reset"); break;
            case NS_HELLO:   snprintf(out, size, "Connecting"); break;
            case NS_IDLE:
                snprintf(out, size, app.menu == MENU_SETTINGS ? "Settings" : "Not connected");
                k = app.menu == MENU_SETTINGS ? ST_BUSY : ST_BAD;
                break;
            case NS_RETRY:
                snprintf(out, size, "Offline, retry %ds", net_retry_seconds(app.now));
                k = ST_BAD;
                break;
            case NS_ONLINE:
                k = ST_OK;
                if (app.reply == R_WAITING)
                    snprintf(out, size, "Waiting %lus",
                             (unsigned long)((app.now - app.reply_start) / 60));
                else if (app.reply == R_STREAMING)
                    snprintf(out, size, "Receiving");
                else if (app.reply == R_CANCELLING)
                    snprintf(out, size, "Stopping");
                else
                    snprintf(out, size, "Online");
                break;
            default:
                snprintf(out, size, "Offline");
                k = ST_BAD;
                break;
        }
    }
    *kind = k;
}

// Top status bar, same high-contrast style as the bottom header: black on
// near-white. Left: the profile. Right: the connection state in a dark chip
// with a marker in front - filled square = online, hollow square = working
// on it, "!" = offline / failed. "v more" (newer text below) sits between.
static void draw_status(void)
{
    static char last[48];
    static int last_kind = -1;
    static bool last_more;
    char state[24];
    int kind;
    ui_status_text(state, sizeof(state), &kind);
    bool more = !app.follow;
    char key[48];
    snprintf(key, sizeof(key), "%s|%s", app.profile, state);
    if (strcmp(key, last) == 0 && kind == last_kind && more == last_more)
        return;
    snprintf(last, sizeof(last), "%s", key);
    last_kind = kind;
    last_more = more;

    // Build the 32 columns left to right; every cell is drawn exactly once
    // (no clear first), so the bar never blinks.
    u16 *fb = gfx_top();
    int slen = strlen(state);
    int chip = 32 - (slen + 3);             // marker cell + space + text + space
    char left[33];
    snprintf(left, sizeof(left), "%-10.10s", app.profile[0] ? app.profile : "-");
    int x = gfx_text(fb, 0, 0, left, 10, COL_HDR_TEXT, COL_HDR_BG);
    if (more && chip - x >= 8)
    {
        gfx_text(fb, x * FONT_W, 0, " ", 1, COL_HDR_TEXT, COL_HDR_BG);
        gfx_text(fb, (x + 1) * FONT_W, 0, "v more", 6, COL_HDR_BG, COL_HDR_TEXT);
        x += 7;
    }
    if (chip > x)
        gfx_text_cols(fb, x * FONT_W, 0, "", chip - x, COL_HDR_TEXT, COL_HDR_BG);
    u16 bg = kind == ST_OK ? COL_ST_OK : kind == ST_BAD ? COL_ST_BAD : COL_ST_BUSY;
    int cx = chip * FONT_W;
    gfx_char(fb, cx, 0, kind == ST_BAD ? '!' : ' ', COL_ST_TEXT, bg);
    if (kind == ST_OK)
        gfx_fill(fb, cx + 2, 2, 4, 4, COL_ST_TEXT);
    else if (kind == ST_BUSY)
    {
        gfx_fill(fb, cx + 2, 2, 4, 1, COL_ST_TEXT);
        gfx_fill(fb, cx + 2, 5, 4, 1, COL_ST_TEXT);
        gfx_fill(fb, cx + 2, 3, 1, 2, COL_ST_TEXT);
        gfx_fill(fb, cx + 5, 3, 1, 2, COL_ST_TEXT);
    }
    char rest[34];
    snprintf(rest, sizeof(rest), " %s ", state);
    gfx_text(fb, cx + FONT_W, 0, rest, slen + 2, COL_ST_TEXT, bg);
}

static void draw_conversation(void)
{
    u16 *fb = gfx_top();
    uint32_t end = tb_end_line(&app.tb);
    for (int row = 0; row < VIEW_ROWS; row++)
    {
        int y = (row + 1) * FONT_H;
        uint32_t n = app.view_top + row;
        const tb_line *l = n < end ? tb_get(&app.tb, n) : NULL;
        if (l != NULL)
        {
            int w = gfx_text(fb, 0, y, l->text, l->len, kind_color(l->kind), COL_BG);
            if (w < TB_COLS)
                gfx_fill(fb, w * FONT_W, y, (TB_COLS - w) * FONT_W, FONT_H, COL_BG);
        }
        else if (n == end && app.reply == R_WAITING)
        {
            char wait[40];
            snprintf(wait, sizeof(wait), "Waiting... %lus   (X: stop)",
                     (unsigned long)((app.now - app.reply_start) / 60));
            gfx_text_cols(fb, 0, y, wait, TB_COLS, COL_DIM, COL_BG);
        }
        else
        {
            gfx_fill(fb, 0, y, SCREEN_W, FONT_H, COL_BG);
        }
    }
}

// --------------------------------------------------------------------------
// Bottom screen building blocks (shared with settings.c)
// --------------------------------------------------------------------------

void ui_draw_header(const char *left, const char *right, bool right_alert)
{
    u16 *fb = gfx_bottom();
    int rlen = right ? (int)strlen(right) : 0;
    gfx_fill(fb, 0, 0, SCREEN_W, INPUT_Y - 1, COL_HDR_BG);
    gfx_text(fb, 0, 1, left, 32 - (rlen ? rlen + 1 : 0), COL_HDR_TEXT, COL_HDR_BG);
    if (rlen)
        gfx_text(fb, SCREEN_W - rlen * FONT_W, 1, right, rlen,
                 right_alert ? COL_HDR_ALERT : COL_HDR_TEXT, COL_HDR_BG);
}

void ui_draw_input_rows(const input_t *in, int *scroll, int y0, int nrows)
{
    static input_row rows[INPUT_CAP + 2];
    u16 *fb = gfx_bottom();
    int crow, ccol;
    int n = input_layout(in, TB_COLS, rows, INPUT_CAP + 2, &crow, &ccol);
    if (crow < *scroll)
        *scroll = crow;
    if (crow >= *scroll + nrows)
        *scroll = crow - nrows + 1;
    if (*scroll > n - 1)
        *scroll = n > 0 ? n - 1 : 0;
    gfx_fill(fb, 0, y0 - 1, SCREEN_W, nrows * FONT_H + 2, COL_INPUT_BG);
    for (int i = 0; i < nrows; i++)
    {
        int r = *scroll + i;
        int y = y0 + i * FONT_H;
        if (r < n)
            gfx_text(fb, 0, y, in->buf + rows[r].start, rows[r].len, COL_TEXT, COL_INPUT_BG);
        if (r == crow)
        {
            char c = in->cur < in->len ? in->buf[in->cur] : ' ';
            gfx_char(fb, ccol * FONT_W, y, c, COL_INPUT_BG, COL_CURSOR);
        }
    }
    // scroll hints when the text is longer than the box
    if (*scroll > 0)
        gfx_char(fb, SCREEN_W - FONT_W, y0, '^', COL_DIM, COL_INPUT_BG);
    if (*scroll + nrows < n)
        gfx_char(fb, SCREEN_W - FONT_W, y0 + (nrows - 1) * FONT_H, 'v', COL_DIM, COL_INPUT_BG);
}

// What each key looked like when last drawn; only keys whose look changed
// are redrawn (a key press redraws one key, not the whole keyboard).
typedef struct
{
    u16 bg, fg;
    char label[6];
} key_look;
static key_look drawn[KBD_MAX_KEYS];
static bool kbd_valid;

void ui_kbd_invalidate(void)
{
    kbd_valid = false;
}

static void key_label(const kbd_t *k, const kbd_key *key, const char *send_label, char *out)
{
    const char *s;
    switch (key->type)
    {
        case K_BKSP:  s = "Bksp"; break;
        case K_SEND:  s = send_label; break;
        case K_SHIFT: s = "Shift"; break;
        case K_CAPS:  s = "Caps"; break;
        case K_LEFT:  s = "<"; break;
        case K_RIGHT: s = ">"; break;
        case K_SPACE: s = "space"; break;
        default:
            out[0] = kbd_char(k, key);
            out[1] = '\0';
            return;
    }
    snprintf(out, 6, "%s", s);
}

void ui_draw_kbd(const kbd_t *k, int pressed, const char *send_label, u16 send_bg)
{
    u16 *fb = gfx_bottom();
    if (!kbd_valid)
        gfx_fill(fb, 0, KBD_Y, SCREEN_W, KBD_H, COL_BG);    // gaps between keys
    for (int i = 0; i < k->count; i++)
    {
        const kbd_key *key = &k->keys[i];
        key_look look;
        memset(&look, 0, sizeof(look));
        look.bg = COL_KEY;
        look.fg = COL_TEXT;
        if (key->type == K_SEND)
            look.bg = send_bg;
        if ((key->type == K_SHIFT && k->shift) || (key->type == K_CAPS && k->caps))
            look.bg = COL_KEY_ON;
        if (i == pressed)
        {
            look.bg = COL_KEY_DOWN;
            look.fg = COL_BG;
        }
        key_label(k, key, send_label, look.label);
        if (kbd_valid && memcmp(&look, &drawn[i], sizeof(look)) == 0)
            continue;
        drawn[i] = look;
        gfx_fill(fb, key->x, key->y, key->w, key->h, look.bg);
        gfx_fill(fb, key->x, key->y, key->w, 1, COL_KEY_EDGE);     // top highlight
        int len = strlen(look.label);
        gfx_text(fb, key->x + (key->w - len * FONT_W) / 2, key->y + (key->h - FONT_H) / 2,
                 look.label, len, look.fg, look.bg);
    }
    kbd_valid = true;
}

// --------------------------------------------------------------------------
// Bottom screen: chat input box + keyboard
// --------------------------------------------------------------------------

// Voice input in the input box's place: phase, seconds and a level bar.
static void draw_voice(void)
{
    u16 *fb = gfx_bottom();
    char right[16], line[33];
    int ph = voice_phase();
    uint32_t ms = voice_speech_ms(app.now);
    snprintf(right, sizeof(right), "%lu.%lu/%ds", (unsigned long)(ms / 1000),
             (unsigned long)(ms % 1000 / 100), VOICE_MAX_MS / 1000);
    ui_draw_header("B: cancel", ph == V_REC || ph == V_WARMUP ? right : NULL, false);
    u16 col = COL_TEXT;
    switch (ph)
    {
        case V_WARMUP: snprintf(line, sizeof(line), "Wait..."); col = COL_DIM; break;
        case V_REC:    snprintf(line, sizeof(line), "Speak  (release Y when done)"); col = COL_USER; break;
        case V_FLUSH:  snprintf(line, sizeof(line), "Sending..."); col = COL_INFO; break;
        default:       snprintf(line, sizeof(line), "Transcribing..."); col = COL_INFO; break;
    }
    int y0 = INPUT_Y - 1, h = INPUT_ROWS * FONT_H + 2;
    gfx_fill(fb, 0, y0, SCREEN_W, h, COL_INPUT_BG);
    gfx_text_cols(fb, 4, INPUT_Y + 2, line, 31, col, COL_INPUT_BG);
    // level bar: green, yellow from 70 %, red at the top (clipping)
    int pct = (ph == V_REC || ph == V_WARMUP) ? voice_level_pct() : 0;
    int w = SCREEN_W - 16;
    gfx_fill(fb, 8, INPUT_Y + 14, w, 10, COL_KEY);
    gfx_fill(fb, 8, INPUT_Y + 14, w * pct / 100, 10,
             pct >= 98 ? COL_ERROR : pct >= 70 ? COL_BAR_WARN : COL_BAR_OK);
    gfx_fill(fb, 0, KBD_Y - 2, SCREEN_W, 2, COL_BG);
}

static void draw_input(void)
{
    if (voice_active())
    {
        draw_voice();
        return;
    }
    char hint[33], count[16];
    if (app.reply == R_WAITING || app.reply == R_STREAMING)
        snprintf(hint, sizeof(hint), "X/Stop: stop reply");
    else if (!net_online())
        snprintf(hint, sizeof(hint), "offline - you can type");
    else
        snprintf(hint, sizeof(hint), "SELECT: menu");
    snprintf(count, sizeof(count), "%u/%u", app.in.len, app.in.max);
    ui_draw_header(hint, count, app.in.len >= app.in.max);
    ui_draw_input_rows(&app.in, &app.in_scroll, INPUT_Y, INPUT_ROWS);
    gfx_fill(gfx_bottom(), 0, KBD_Y - 2, SCREEN_W, 2, COL_BG);
}

static void draw_keyboard(void)
{
    bool busy = app.reply == R_WAITING || app.reply == R_STREAMING;
    ui_draw_kbd(&app.kbd, app.pressed, busy ? "Stop" : "Send", busy ? COL_KEY_STOP : COL_KEY_SEND);
}

// --------------------------------------------------------------------------
// Bottom screen: menu pages
// --------------------------------------------------------------------------

#define MENU_TOP     16
#define GRID_W       120
#define GRID_H       34
#define GRID_X(c)    (6 + (c) * (GRID_W + 4))
#define GRID_Y(r)    (MENU_TOP + (r) * (GRID_H + 4))
#define PROF_H       22
#define PROF_VISIBLE 6

static const char *const main_labels[MAIN_ITEMS] = {
    "New chat", "Switch profile", "Settings", "Reconnect",
    "Soak test", "About", "Power off", "Close",
};

int ui_menu_items(void)
{
    if (app.menu == MENU_MAIN)
        return MAIN_ITEMS;
    if (app.menu == MENU_PROFILES)
        return app.nprofiles;
    if (app.menu == MENU_POWER)
        return 2;
    return 0;
}

// Power dialog geometry
#define DLG_X 24
#define DLG_Y 40
#define DLG_W 208
#define DLG_H 104
#define BTN_Y (DLG_Y + 56)
#define BTN_W 80
#define BTN_H 32
#define BTN_X(i) (DLG_X + 16 + (i) * (BTN_W + 16))

int ui_menu_item_at(int x, int y)
{
    if (app.menu == MENU_MAIN)
    {
        for (int i = 0; i < MAIN_ITEMS; i++)
        {
            int bx = GRID_X(i % 2), by = GRID_Y(i / 2);
            if (x >= bx && x < bx + GRID_W && y >= by && y < by + GRID_H)
                return i;
        }
    }
    else if (app.menu == MENU_PROFILES)
    {
        int i = (y - MENU_TOP) / PROF_H;
        if (y >= MENU_TOP && i < PROF_VISIBLE && app.menu_scroll + i < app.nprofiles)
            return app.menu_scroll + i;
    }
    else if (app.menu == MENU_POWER)
    {
        for (int i = 0; i < 2; i++)
        {
            if (x >= BTN_X(i) && x < BTN_X(i) + BTN_W && y >= BTN_Y && y < BTN_Y + BTN_H)
                return i;
        }
    }
    return -1;
}

static void draw_title(u16 *fb, const char *title)
{
    gfx_fill(fb, 0, 0, SCREEN_W, SCREEN_H, COL_BG);
    gfx_fill(fb, 0, 0, SCREEN_W, 11, COL_BAR);
    gfx_text(fb, 4, 2, title, 30, COL_TEXT, COL_BAR);
}

static void draw_footer(u16 *fb, const char *keys)
{
    if (app.menu_msg[0])
        gfx_text_cols(fb, 0, SCREEN_H - 20, app.menu_msg, 32, COL_INFO, COL_BG);
    gfx_text_cols(fb, 0, SCREEN_H - 9, keys, 32, COL_DIM, COL_BG);
}

static void draw_button(u16 *fb, int x, int y, int w, int h, const char *label, u16 bg)
{
    gfx_fill(fb, x, y, w, h, bg);
    gfx_fill(fb, x, y, w, 1, COL_KEY_EDGE);
    int len = strlen(label);
    gfx_text(fb, x + (w - len * FONT_W) / 2, y + (h - FONT_H) / 2, label, len, COL_TEXT, bg);
}

static int drawn_page = MENU_NONE;     // page currently on the bottom screen

static void draw_menu(void)
{
    u16 *fb = gfx_bottom();
    if (app.menu == MENU_MAIN)
    {
        draw_title(fb, "Menu");
        for (int i = 0; i < MAIN_ITEMS; i++)
            draw_button(fb, GRID_X(i % 2), GRID_Y(i / 2), GRID_W, GRID_H, main_labels[i],
                        i == app.menu_sel ? COL_SEL : (i == MAIN_POWER ? COL_KEY_STOP : COL_KEY));
        draw_footer(fb, "A/touch: choose  B: back");
    }
    else if (app.menu == MENU_POWER)
    {
        draw_title(fb, "Power off");
        gfx_fill(fb, DLG_X - 2, DLG_Y - 2, DLG_W + 4, DLG_H + 4, COL_HDR_BG);   // frame
        gfx_fill(fb, DLG_X, DLG_Y, DLG_W, DLG_H, COL_BAR);
        const char *q = "Turn off the DS?";
        gfx_text(fb, DLG_X + (DLG_W - 16 * FONT_W) / 2, DLG_Y + 20, q, 16, COL_TEXT, COL_BAR);
        draw_button(fb, BTN_X(0), BTN_Y, BTN_W, BTN_H, "Yes",
                    app.menu_sel == 0 ? COL_KEY_STOP : COL_KEY);
        draw_button(fb, BTN_X(1), BTN_Y, BTN_W, BTN_H, "No",
                    app.menu_sel == 1 ? COL_SEL : COL_KEY);
        draw_footer(fb, "A/touch: choose  B: cancel");
    }
    else if (app.menu == MENU_PROFILES)
    {
        draw_title(fb, "Switch profile (new chat)");
        if (app.menu_sel < app.menu_scroll)
            app.menu_scroll = app.menu_sel;
        if (app.menu_sel >= app.menu_scroll + PROF_VISIBLE)
            app.menu_scroll = app.menu_sel - PROF_VISIBLE + 1;
        if (app.nprofiles == 0 && app.menu_wait)
            gfx_text(fb, 8, MENU_TOP + 4, "Loading...", 10, COL_DIM, COL_BG);
        for (int i = 0; i < PROF_VISIBLE && app.menu_scroll + i < app.nprofiles; i++)
        {
            int idx = app.menu_scroll + i;
            const proto_profile *p = &app.profiles[idx];
            int y = MENU_TOP + i * PROF_H;
            u16 bg = idx == app.menu_sel ? COL_SEL : COL_KEY;
            u16 fg = p->flag == '!' ? COL_DIM : COL_TEXT;
            gfx_fill(fb, 0, y, SCREEN_W, PROF_H - 2, bg);
            char line[40];
            snprintf(line, sizeof(line), "%c %-8.8s %s", p->flag == '+' ? ' ' : p->flag,
                     p->name, p->flag == '!' ? "(no API key)" : p->model);
            gfx_text(fb, 0, y + 2, line, 32, fg, bg);
            if (strlen(line) > 32)
                gfx_text(fb, 11 * FONT_W, y + 2 + FONT_H, line + 32, 21, fg, bg);
        }
        draw_footer(fb, "* = current   A: switch  B: back");
    }
    else if (app.menu == MENU_SOAK)
    {
        static const char *const names[] = { "running", "PASS", "STALL", "LOST", "stopped" };
        if (drawn_page != MENU_SOAK)        // live page: no full clear each refresh
            draw_title(fb, "Wi-Fi soak test (PINGs only)");
        uint32_t end = soak.result == SOAK_RUNNING ? app.now : soak.end;
        uint32_t dt = soak.t - soak.t0;
        char l[12][40];
        int n = 0;
        snprintf(l[n++], 40, "State   %s, %lus", names[soak.result],
                 (unsigned long)((end - soak.start) / 60));
        snprintf(l[n++], 40, "PINGs   %lu sent", (unsigned long)soak.sent);
        snprintf(l[n++], 40, "PONGs   %lu received", (unsigned long)soak.recv);
        snprintf(l[n++], 40, "Other   %lu  in flight %d", (unsigned long)soak.fails, soak.inflight);
        snprintf(l[n++], 40, "Last    PONG %lu.%lus ago",
                 (unsigned long)((end - soak.last_pong) / 60),
                 (unsigned long)((end - soak.last_pong) % 60 / 6));
        snprintf(l[n++], 40, "Wi-Fi   q %lu", (unsigned long)soak.q);
        snprintf(l[n++], 40, "        t %lu (sent)", (unsigned long)soak.t);
        snprintf(l[n++], 40, "        r %lu (received)", (unsigned long)soak.r);
        snprintf(l[n++], 40, "Delta   t+%lu r+%lu q+%lu", (unsigned long)dt,
                 (unsigned long)(soak.r - soak.r0), (unsigned long)(soak.q - soak.q0));
        uint32_t el = end - soak.start;
        uint32_t pt = dt >= SOAK_TARGET ? 100 : dt * 100 / SOAK_TARGET;
        uint32_t pm = el >= SOAK_MIN_TIME ? 100 : el * 100 / SOAK_MIN_TIME;
        snprintf(l[n++], 40, "Target  t+%d %lu%%, %ds %lu%%", SOAK_TARGET,
                 (unsigned long)pt, SOAK_MIN_TIME / 60, (unsigned long)pm);
        if (soak.result == SOAK_LOST)
            snprintf(l[n++], 40, "%.32s", soak.reason);
        for (int i = 0; i < n; i++)
        {
            u16 col = COL_TEXT;
            if (i == 0)
                col = soak.result == SOAK_PASS ? COL_BAR_OK
                    : soak.result == SOAK_RUNNING ? COL_USER : COL_ERROR;
            gfx_text_cols(fb, 0, 15 + i * 12, l[i], 32, col, COL_BG);
        }
        uint32_t prog = pt < pm ? pt : pm;      // both conditions must be met
        int w = (int)(prog * (SCREEN_W - 16) / 100);
        gfx_fill(fb, 8, 15 + 11 * 12, SCREEN_W - 16, 6, COL_KEY);
        gfx_fill(fb, 8, 15 + 11 * 12, w, 6, soak.result == SOAK_STALLED ? COL_ERROR : COL_BAR_OK);
        draw_footer(fb, soak_running() ? "B: stop" : "A/B: back to the menu");
    }
    else if (app.menu == MENU_ABOUT)
    {
        if (drawn_page != MENU_ABOUT)
            draw_title(fb, "About DSChat");
        net_info_t ni;
        net_get_info(&ni);
        gfx_stats_t gs;
        gfx_get_stats(&gs);
        const proto_server_info *si = net_server();
        char l[18][40];
        int n = 0;
        snprintf(l[n++], 40, "DSChat %s", DSCHAT_VERSION);
        snprintf(l[n++], 40, "DS MAC  %s", ni.mac);
        snprintf(l[n++], 40, "IP      %s (%s)", ni.ip, ni.static_ip ? "static" : "DHCP");
        snprintf(l[n++], 40, "SSID    %.24s", app.cfg->ssid);
        snprintf(l[n++], 40, "Relay   %s", ni.relay);
        if (net_online())
            snprintf(l[n++], 40, "Server  v%d max %d id %.8s", si->version, si->max_msg,
                     si->boot[0] ? si->boot : "-");
        else
            snprintf(l[n++], 40, "Server  not connected");
        snprintf(l[n++], 40, "Profile %s", app.profile);
        snprintf(l[n++], 40, "Model   %.24s", app.model);
        snprintf(l[n++], 40, "Config  %.24s", app.cfg->source);
        snprintf(l[n++], 40, "SD      %.24s", app.store.sd ? app.store.drive : "not available");
        snprintf(l[n++], 40, "%.32s", app.store.save_msg[0] ? app.store.save_msg : app.store.msg);
        snprintf(l[n++], 40, "Net %.28s", ni.stats);
        snprintf(l[n++], 40, "WiFi %.27s", ni.wifi);
        snprintf(l[n++], 40, "Draw %lu late%lu max%lu", (unsigned long)gs.presents,
                 (unsigned long)gs.overruns, (unsigned long)gs.max_lines);
        const voice_stats *vs = voice_last_stats();
        if (!net_server()->voice)
            snprintf(l[n++], 40, "Voice   %s", net_online() ? "not on this relay" : "-");
        else if (!vs->valid)
            snprintf(l[n++], 40, "Voice   on (%s), not used yet", config_voice_code(app.cfg->voice_lang));
        else
        {
            uint32_t kbs = vs->rx_ms ? (uint64_t)vs->bytes * 10000 / 1024 / vs->rx_ms : 0;
            unsigned r = vs->rec_ms / 100 % 1000, kb = vs->bytes / 1024 % 1000, k = kbs % 10000;
            unsigned t = vs->tail_ms / 100 % 1000, st = vs->stt_ms / 100 % 1000, to = vs->total_ms / 100 % 1000;
            snprintf(l[n++], 40, "Voice %u.%us %uKB %u.%uKB/s", r / 10, r % 10, kb, k / 10, k % 10);
            snprintf(l[n++], 40, " tail %u.%us stt %u.%us all %u.%us", t / 10, t % 10, st / 10, st % 10,
                     to / 10, to % 10);
        }
        for (int i = 0; i < n; i++)
            gfx_text_cols(fb, 0, 13 + i * 10, l[i], 32, i == 0 ? COL_USER : COL_TEXT, COL_BG);
        draw_footer(fb, "any key: back");
    }
    else if (app.menu == MENU_SETTINGS)
    {
        settings_draw(drawn_page != MENU_SETTINGS);
    }
}

// --------------------------------------------------------------------------

void ui_render(void)
{
    draw_status();
    if (app.dirty_conv)
    {
        app_view_update();
        draw_conversation();
        app.dirty_conv = false;
    }
    if (app.menu != MENU_NONE)
    {
        if (app.dirty_menu || drawn_page != app.menu)
        {
            if (drawn_page != app.menu)
                ui_kbd_invalidate();        // the keyboard area gets overwritten
            draw_menu();
            drawn_page = app.menu;
        }
        app.dirty_menu = false;
        app.dirty_input = app.dirty_kbd = false;
        return;
    }
    if (drawn_page != MENU_NONE)            // back from a menu: redraw everything
    {
        drawn_page = MENU_NONE;
        ui_kbd_invalidate();
        app.dirty_input = app.dirty_kbd = true;
    }
    if (app.dirty_input)
        draw_input();
    if (app.dirty_kbd)
        draw_keyboard();
    app.dirty_input = app.dirty_kbd = false;
}
