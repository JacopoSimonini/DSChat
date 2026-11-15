#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <nds.h>
#include <dswifi9.h>

#include "app.h"
#include "net.h"
#include "settings.h"

enum page { SP_MAIN, SP_LIST, SP_TEXT, SP_NUM };
enum text_kind { TXT_SSID, TXT_WEP };
enum num_kind { NUM_IP, NUM_MASK, NUM_GW, NUM_RELAY, NUM_PORT };

static dschat_config edit;          // what is being edited (copied from app.cfg)
static int page;
static bool need_full;              // redraw the whole page
static int sel;
static char status[80];
static u16 status_col;
static bool testing;
static uint32_t now_f;

static void dirty(void)
{
    app.dirty_menu = true;
}

static void go(int p)
{
    page = p;
    need_full = true;
    dirty();
}

static void set_status(u16 col, const char *fmt, const char *arg)
{
    snprintf(status, sizeof(status), fmt, arg ? arg : "");
    status_col = col;
    dirty();
}

static void button_en(u16 *fb, int x, int y, int w, int h, const char *label, u16 bg, bool en)
{
    // disabled: no fill, just a dim outline and a dim label
    gfx_fill(fb, x, y, w, h, en ? bg : COL_BG);
    if (en)
        gfx_fill(fb, x, y, w, 1, COL_KEY_EDGE);
    else
    {
        gfx_fill(fb, x, y, w, 1, COL_KEY);
        gfx_fill(fb, x, y + h - 1, w, 1, COL_KEY);
        gfx_fill(fb, x, y, 1, h, COL_KEY);
        gfx_fill(fb, x + w - 1, y, 1, h, COL_KEY);
    }
    int len = strlen(label);
    gfx_text(fb, x + (w - len * FONT_W) / 2, y + (h - FONT_H) / 2, label, len,
             en ? COL_TEXT : COL_DIM, en ? bg : COL_BG);
}

static void button(u16 *fb, int x, int y, int w, int h, const char *label, u16 bg)
{
    button_en(fb, x, y, w, h, label, bg, true);
}

static void title(u16 *fb, const char *t, const char *right)
{
    gfx_fill(fb, 0, 0, SCREEN_W, SCREEN_H, COL_BG);
    ui_draw_header(t, right, false);
}

static bool touched(int *x, int *y, uint32_t down)
{
    if (!(down & KEY_TOUCH))
        return false;
    touchPosition t;
    touchRead(&t);
    *x = t.px;
    *y = t.py;
    return true;
}

// --------------------------------------------------------------------------
// Main page
// --------------------------------------------------------------------------

#define ROW_Y0  14
#define ROW_H   13
#define NROWS   10
#define MBTN_Y  152
#define MBTN_H  22
#define MBTN_W  80
#define MBTN_X(i) (4 + (i) * 84)

enum { R_NET = 0, R_SEC, R_KEY, R_IPMODE, R_IP, R_MASK, R_GW, R_RELAY, R_PORT, R_VOICE,
       B_TEST, B_SAVE, B_CANCEL, MAIN_COUNT };

static bool row_enabled(int i)
{
    if (i == R_KEY)
        return edit.security == SEC_WEP;
    if (i == R_IP || i == R_MASK || i == R_GW)
        return !edit.dhcp;
    return true;
}

static void row_text(int i, const char **label, char *val, size_t n)
{
    static const char *const labels[NROWS] = {
        "Network", "Security", "WEP key", "IP", "Address", "Netmask", "Gateway",
        "Relay", "Port", "Voice",
    };
    *label = labels[i];
    switch (i)
    {
        case R_NET:    snprintf(val, n, "%s", edit.ssid[0] ? edit.ssid : "(choose)"); break;
        case R_SEC:    snprintf(val, n, "%s", edit.security == SEC_WEP ? "WEP" : "open"); break;
        case R_KEY:
        {
            uint8_t kb[13];
            snprintf(val, n, "%s%s", edit.security != SEC_WEP ? "-"
                                     : edit.wep_key[0] ? edit.wep_key : "(missing)",
                     edit.security == SEC_WEP && edit.wep_key[0]
                         && wep_key_bytes(edit.wep_key, kb) == 0 ? " (bad)" : "");
            break;
        }
        case R_IPMODE: snprintf(val, n, "%s", edit.dhcp ? "DHCP (automatic)" : "static"); break;
        case R_IP:     snprintf(val, n, "%s", edit.static_ip); break;
        case R_MASK:   snprintf(val, n, "%s", edit.static_mask); break;
        case R_GW:     snprintf(val, n, "%s", edit.static_gw); break;
        case R_RELAY:  snprintf(val, n, "%s", edit.relay_host[0] ? edit.relay_host : "gateway (auto)"); break;
        case R_PORT:   snprintf(val, n, "%u", edit.relay_port); break;
        case R_VOICE:  snprintf(val, n, "%s", edit.voice_lang == VOICE_IT ? "Italian (hold Y)" : "English (hold Y)"); break;
    }
}

static void draw_main(void)
{
    u16 *fb = gfx_bottom();
    title(fb, "Settings", testing ? "testing" : "B: cancel");
    for (int i = 0; i < NROWS; i++)
    {
        int y = ROW_Y0 + i * ROW_H;
        bool en = row_enabled(i);
        u16 bg = i == sel ? COL_SEL : COL_KEY;
        gfx_fill(fb, 0, y, SCREEN_W, ROW_H - 1, bg);
        const char *label;
        char val[40];
        row_text(i, &label, val, sizeof(val));
        gfx_text(fb, 4, y + 2, label, 8, en ? COL_INFO : COL_DIM, bg);
        gfx_text(fb, 4 + 9 * FONT_W, y + 2, val, 22, en ? COL_TEXT : COL_DIM, bg);
    }
    // Test and Save are both off while the settings are not usable (e.g.
    // WEP without a valid key); the status line then says why.
    char why[48];
    bool ok = config_validate(&edit, why, sizeof(why));
    static const char *const names[3] = { "Test", "Save", "Cancel" };
    for (int b = 0; b < 3; b++)
    {
        int item = B_TEST + b;
        bool en = item == B_CANCEL || ok;
        u16 bg = item == sel ? COL_SEL : (item == B_SAVE ? COL_KEY_SEND : COL_KEY);
        if (!en && item == sel)         // keep the cursor visible on a disabled button
            gfx_fill(fb, MBTN_X(b) - 2, MBTN_Y - 2, MBTN_W + 4, MBTN_H + 4, COL_SEL);
        button_en(fb, MBTN_X(b), MBTN_Y, MBTN_W, MBTN_H, names[b], bg, en);
    }
    if (ok || testing)
        gfx_text_cols(fb, 0, 178, status, 32, status_col, COL_BG);
    else
    {
        char line[40];
        snprintf(line, sizeof(line), "! %.30s", why);
        gfx_text_cols(fb, 0, 178, line, 32, COL_ERROR, COL_BG);
    }
}

static void open_list(void);
static void open_text(int kind);
static void open_num(int kind);

static void start_test(void)
{
    char why[48];
    if (!config_validate(&edit, why, sizeof(why)))
    {
        dirty();            // refused: the status line already says "! <why>"
        return;
    }
    testing = true;
    set_status(COL_INFO, "Testing... (B: stop)", NULL);
    char msg[80];
    snprintf(msg, sizeof(msg), "[test] Testing \"%.32s\"...", edit.ssid);
    app_say(TB_INFO, msg);
    net_start(&edit, true);
}

static void stop_test(void)
{
    if (!testing)
        return;
    testing = false;
    net_stop();
    app_say(TB_INFO, "[test] stopped");
}

static void close_settings(void)
{
    page = SP_MAIN;
    app_close_menu();
}

static void save(void)
{
    char why[48];
    if (!config_validate(&edit, why, sizeof(why)))
    {
        dirty();            // refused: the status line already says "! <why>"
        return;
    }
    stop_test();
    edit.source = app.store.sd ? "settings file" : "this session (no SD)";
    *app.cfg = edit;
    app_save_config();
    if (!app.store.sd)
        app_say(TB_INFO, "Settings kept until power off (no SD card).");
    else if (app.cfg_dirty)
        app_say(TB_ERROR, app.store.save_msg);
    else
    {
        app.store.status = ST_LOADED;
        app_say(TB_INFO, "Settings saved to dschat/settings.txt.");
    }
    close_settings();
    char msg[80];
    snprintf(msg, sizeof(msg), "Connecting to \"%.32s\"...", app.cfg->ssid);
    app_say(TB_INFO, msg);
    app.auto_settings_done = false;
    net_start(app.cfg, false);
}

static void cancel(void)
{
    stop_test();
    close_settings();
    char why[48];
    if (config_validate(app.cfg, why, sizeof(why)))
    {
        app_say(TB_INFO, "Settings unchanged. Connecting...");
        net_start(app.cfg, false);
    }
    else
    {
        char msg[128];
        snprintf(msg, sizeof(msg), "Settings not usable (%.47s): not connecting. SELECT > Settings.", why);
        app_say(TB_ERROR, msg);
    }
}

static void activate(int i)
{
    if (testing && i != B_CANCEL)
    {
        set_status(COL_INFO, "Testing... (B: stop)", NULL);
        return;
    }
    if (i < NROWS && !row_enabled(i))
    {
        set_status(COL_DIM, i == R_KEY ? "only for WEP networks" : "only with a static IP", NULL);
        return;
    }
    switch (i)
    {
        case R_NET:    open_list(); break;
        case R_SEC:
        {
            uint8_t k[13];
            edit.security = edit.security == SEC_WEP ? SEC_OPEN : SEC_WEP;
            if (edit.security == SEC_WEP && wep_key_bytes(edit.wep_key, k) == 0)
                open_text(TXT_WEP);
            break;
        }
        case R_KEY:    open_text(TXT_WEP); break;
        case R_IPMODE: edit.dhcp = !edit.dhcp; break;
        case R_IP:     open_num(NUM_IP); break;
        case R_MASK:   open_num(NUM_MASK); break;
        case R_GW:     open_num(NUM_GW); break;
        case R_RELAY:  open_num(NUM_RELAY); break;
        case R_PORT:   open_num(NUM_PORT); break;
        case R_VOICE:
            edit.voice_lang = edit.voice_lang == VOICE_IT ? VOICE_EN : VOICE_IT;
            set_status(COL_INFO, "voice language changed - Save to keep it", NULL);
            break;
        case B_TEST:   start_test(); break;
        case B_SAVE:   save(); return;
        case B_CANCEL: cancel(); return;
    }
    dirty();
}

static int main_item_at(int x, int y)
{
    for (int i = 0; i < NROWS; i++)
    {
        if (y >= ROW_Y0 + i * ROW_H && y < ROW_Y0 + (i + 1) * ROW_H - 1)
            return i;
    }
    for (int b = 0; b < 3; b++)
    {
        if (x >= MBTN_X(b) && x < MBTN_X(b) + MBTN_W && y >= MBTN_Y && y < MBTN_Y + MBTN_H)
            return B_TEST + b;
    }
    return -1;
}

static void main_input(uint32_t down, uint32_t rep)
{
    if (down & KEY_B)
    {
        if (testing)
        {
            stop_test();
            set_status(COL_INFO, "test stopped", NULL);
        }
        else
        {
            cancel();
        }
        return;
    }
    if (rep & KEY_UP)
    {
        int i = sel >= B_TEST ? R_VOICE : sel - 1;
        while (i > 0 && !row_enabled(i))
            i--;
        if (i >= 0)
            sel = i;
        dirty();
    }
    if (rep & KEY_DOWN)
    {
        int i = sel + 1;
        while (i < NROWS && !row_enabled(i))
            i++;
        if (sel < B_TEST)
            sel = i < NROWS ? i : B_TEST;
        dirty();
    }
    if ((rep & KEY_LEFT) && sel > B_TEST)
    {
        sel--;
        dirty();
    }
    if ((rep & KEY_RIGHT) && sel >= B_TEST && sel < B_CANCEL)
    {
        sel++;
        dirty();
    }
    if (down & KEY_A)
        activate(sel);
    int x, y;
    if (touched(&x, &y, down))
    {
        int i = main_item_at(x, y);
        if (i >= 0)
        {
            sel = i;
            activate(i);
        }
    }
}

// --------------------------------------------------------------------------
// Wi-Fi network list (scan)
// --------------------------------------------------------------------------

#define LROW_Y0  14
#define LROW_H   18
#define LROWS    7
#define LBTN_Y   146
#define MAX_APS  16

typedef struct
{
    char ssid[33];
    int channel;
    int rssi;
    int sec;                    // 0 open, 1 WEP, 2 WPA (not supported)
} ap_row;

static ap_row aps[MAX_APS];
static int naps, lscroll;

static void refresh_list(void)
{
    ap_row found[MAX_APS];
    int nf = 0;
    int n = Wifi_GetNumAP();
    for (int i = 0; i < n; i++)
    {
        Wifi_AccessPoint ap;
        if (Wifi_GetAPData(i, &ap) != WIFI_RETURN_OK || ap.ssid[0] == '\0')
            continue;           // hidden networks: use "Hidden..."
        int sec = ap.security_type == AP_SECURITY_OPEN ? 0
                : ap.security_type == AP_SECURITY_WEP ? 1 : 2;
        int j;
        for (j = 0; j < nf; j++)
        {
            if (strcmp(found[j].ssid, ap.ssid) == 0)
                break;
        }
        if (j < nf)
        {
            if (ap.rssi > found[j].rssi)
                found[j].rssi = ap.rssi, found[j].channel = ap.channel;
            continue;
        }
        if (nf == MAX_APS)
            continue;
        snprintf(found[nf].ssid, sizeof(found[nf].ssid), "%s", ap.ssid);
        found[nf].channel = ap.channel;
        found[nf].rssi = ap.rssi;
        found[nf].sec = sec;
        nf++;
    }
    // strongest first (insertion sort, tiny lists)
    for (int i = 1; i < nf; i++)
    {
        ap_row t = found[i];
        int j = i - 1;
        while (j >= 0 && found[j].rssi < t.rssi)
        {
            found[j + 1] = found[j];
            j--;
        }
        found[j + 1] = t;
    }
    if (nf != naps || memcmp(found, aps, sizeof(ap_row) * nf) != 0)
    {
        memcpy(aps, found, sizeof(ap_row) * nf);
        naps = nf;
        if (sel >= naps + 3)
            sel = naps + 2;
        dirty();
    }
}

static void open_list(void)
{
    net_stop();
    Wifi_ScanMode();
    naps = 0;
    lscroll = 0;
    sel = 0;
    set_status(COL_INFO, "Scanning... WPA networks can't be used", NULL);
    go(SP_LIST);
}

static void draw_list(void)
{
    u16 *fb = gfx_bottom();
    char t[33];
    snprintf(t, sizeof(t), "Wi-Fi networks (%d found)", naps);
    title(fb, t, NULL);
    if (sel < naps)
    {
        if (sel < lscroll)
            lscroll = sel;
        if (sel >= lscroll + LROWS)
            lscroll = sel - LROWS + 1;
    }
    for (int i = 0; i < LROWS && lscroll + i < naps; i++)
    {
        const ap_row *a = &aps[lscroll + i];
        int y = LROW_Y0 + i * LROW_H;
        u16 bg = lscroll + i == sel ? COL_SEL : COL_KEY;
        gfx_fill(fb, 0, y, SCREEN_W, LROW_H - 1, bg);
        char line[40];
        snprintf(line, sizeof(line), "%c%-16.16s c%-2d %3d %s",
                 strcmp(a->ssid, edit.ssid) == 0 ? '*' : ' ', a->ssid, a->channel, a->rssi,
                 a->sec == 0 ? "open" : a->sec == 1 ? "WEP" : "WPA");
        gfx_text(fb, 0, y + 5, line, 32, a->sec == 2 ? COL_DIM : COL_TEXT, bg);
    }
    if (naps == 0)
        gfx_text(fb, 8, LROW_Y0 + 8, "Searching...", 12, COL_DIM, COL_BG);
    static const char *const names[3] = { "Hidden...", "Rescan", "Back" };
    for (int b = 0; b < 3; b++)
        button(fb, MBTN_X(b), LBTN_Y, MBTN_W, MBTN_H, names[b],
               sel == naps + b ? COL_SEL : COL_KEY);
    gfx_text_cols(fb, 0, 172, status, 32, status_col, COL_BG);
    gfx_text_cols(fb, 0, 183, "* = current  A/touch: choose", 32, COL_DIM, COL_BG);
}

static void list_choose(int i)
{
    if (i == naps)
    {
        open_text(TXT_SSID);
        return;
    }
    if (i == naps + 1)
    {
        Wifi_ScanMode();
        naps = 0;
        set_status(COL_INFO, "Scanning...", NULL);
        return;
    }
    if (i == naps + 2 || i < 0 || i >= naps)
    {
        sel = R_NET;
        go(SP_MAIN);
        return;
    }
    const ap_row *a = &aps[i];
    if (a->sec == 2)
    {
        set_status(COL_ERROR, "WPA: not supported by the DS Lite", NULL);
        return;
    }
    bool same = strcmp(a->ssid, edit.ssid) == 0;
    snprintf(edit.ssid, sizeof(edit.ssid), "%s", a->ssid);
    edit.security = a->sec == 1 ? SEC_WEP : SEC_OPEN;
    uint8_t k[13];
    if (edit.security == SEC_WEP && (!same || wep_key_bytes(edit.wep_key, k) == 0))
    {
        if (!same)
            edit.wep_key[0] = '\0';
        open_text(TXT_WEP);
        return;
    }
    sel = R_NET;
    set_status(COL_INFO, "network chosen - Test or Save", NULL);
    go(SP_MAIN);
}

static void list_input(uint32_t down, uint32_t rep)
{
    int count = naps + 3;
    if (down & KEY_B)
    {
        sel = R_NET;
        go(SP_MAIN);
        return;
    }
    if ((rep & KEY_UP) && sel > 0)
    {
        sel = sel > naps ? (naps > 0 ? naps - 1 : 0) : sel - 1;
        dirty();
    }
    if ((rep & KEY_DOWN) && sel < count - 1)
    {
        sel = sel < naps ? sel + 1 : sel;
        dirty();
    }
    if ((rep & KEY_LEFT) && sel > naps)
    {
        sel--;
        dirty();
    }
    if ((rep & KEY_RIGHT) && sel >= naps && sel < count - 1)
    {
        sel++;
        dirty();
    }
    if (down & KEY_A)
        list_choose(sel);
    int x, y;
    if (touched(&x, &y, down))
    {
        for (int i = 0; i < LROWS && lscroll + i < naps; i++)
        {
            if (y >= LROW_Y0 + i * LROW_H && y < LROW_Y0 + (i + 1) * LROW_H - 1)
            {
                sel = lscroll + i;
                list_choose(sel);
                return;
            }
        }
        for (int b = 0; b < 3; b++)
        {
            if (x >= MBTN_X(b) && x < MBTN_X(b) + MBTN_W && y >= LBTN_Y && y < LBTN_Y + MBTN_H)
            {
                sel = naps + b;
                list_choose(sel);
                return;
            }
        }
    }
}

// --------------------------------------------------------------------------
// Text entry (hidden SSID, WEP key) with the touch keyboard
// --------------------------------------------------------------------------

static int text_kind;
static input_t tin;
static kbd_t tkbd;
static int tpressed = -1, tscroll;
static uint32_t tpressed_since;
static char terr[40];

static void open_text(int kind)
{
    text_kind = kind;
    input_init(&tin, kind == TXT_SSID ? 32 : 26);
    input_set(&tin, kind == TXT_SSID ? edit.ssid : edit.wep_key);
    kbd_init(&tkbd, 0, KBD_Y, 256, KBD_H);
    tpressed = -1;
    tscroll = 0;
    terr[0] = '\0';
    ui_kbd_invalidate();
    go(SP_TEXT);
}

static void draw_text(bool full)
{
    u16 *fb = gfx_bottom();
    if (full)
        gfx_fill(fb, 0, 0, SCREEN_W, KBD_Y, COL_BG);
    char count[12];
    snprintf(count, sizeof(count), "%u/%u", tin.len, tin.max);
    ui_draw_header(text_kind == TXT_SSID ? "Network name (SSID)" : "WEP key", count, false);
    ui_draw_input_rows(&tin, &tscroll, INPUT_Y, 2);
    const char *h1 = text_kind == TXT_SSID ? "Hidden network name, 1-32 chars"
                                           : "5 or 13 chars, or 10/26 hex";
    gfx_fill(fb, 0, INPUT_Y + 17, SCREEN_W, 17, COL_INPUT_BG);
    gfx_text(fb, 0, INPUT_Y + 18, terr[0] ? terr : h1, 32, terr[0] ? COL_ERROR : COL_INFO,
             COL_INPUT_BG);
    gfx_text(fb, 0, INPUT_Y + 26, "START/OK: done  SELECT: back", 32, COL_DIM, COL_INPUT_BG);
    gfx_fill(fb, 0, KBD_Y - 2, SCREEN_W, 2, COL_BG);
    ui_draw_kbd(&tkbd, tpressed, "OK", COL_KEY_SEND);
}

static void text_ok(void)
{
    if (text_kind == TXT_SSID)
    {
        if (input_blank(&tin))
        {
            snprintf(terr, sizeof(terr), "enter the network name");
            dirty();
            return;
        }
        snprintf(edit.ssid, sizeof(edit.ssid), "%.32s", tin.buf);
        set_status(COL_INFO, "hidden network set - check Security", NULL);
        sel = R_SEC;
    }
    else
    {
        uint8_t k[13];
        if (wep_key_bytes(tin.buf, k) == 0)
        {
            snprintf(terr, sizeof(terr), "need 5/13 chars or 10/26 hex");
            dirty();
            return;
        }
        snprintf(edit.wep_key, sizeof(edit.wep_key), "%.26s", tin.buf);
        edit.security = SEC_WEP;
        set_status(COL_INFO, "WEP key set - Test or Save", NULL);
        sel = R_KEY;
    }
    go(SP_MAIN);
}

static void text_key(int idx)
{
    kbd_action a = kbd_press(&tkbd, idx);
    switch (a.type)
    {
        case K_CHAR:
        case K_SPACE: input_insert(&tin, a.ch); break;
        case K_BKSP:  input_backspace(&tin); break;
        case K_LEFT:  input_left(&tin); break;
        case K_RIGHT: input_right(&tin); break;
        case K_SEND:  text_ok(); return;
        default: break;
    }
    terr[0] = '\0';
    dirty();
}

static void text_input(uint32_t down, uint32_t rep, uint32_t held, uint32_t up)
{
    if (down & KEY_SELECT)
    {
        go(SP_MAIN);
        return;
    }
    if (down & KEY_START)
    {
        text_ok();
        return;
    }
    if (rep & KEY_B)
    {
        input_backspace(&tin);
        dirty();
    }
    if (rep & KEY_LEFT)
    {
        input_left(&tin);
        dirty();
    }
    if (rep & KEY_RIGHT)
    {
        input_right(&tin);
        dirty();
    }
    int x, y;
    if (touched(&x, &y, down))
    {
        int idx = kbd_hit(&tkbd, x, y);
        if (idx >= 0)
        {
            tpressed = idx;
            tpressed_since = now_f;
            text_key(idx);
        }
    }
    else if ((held & KEY_TOUCH) && tpressed >= 0 && page == SP_TEXT)
    {
        uint8_t type = tkbd.keys[tpressed].type;
        uint32_t t = now_f - tpressed_since;
        if ((type == K_BKSP || type == K_LEFT || type == K_RIGHT) && t > 24 && t % 4 == 0)
            text_key(tpressed);
    }
    if ((up & KEY_TOUCH) && tpressed >= 0)
    {
        tpressed = -1;
        dirty();
    }
}

// --------------------------------------------------------------------------
// Number entry (IP addresses, port) with a big numeric keypad
// --------------------------------------------------------------------------

#define NP_Y   54
#define NP_KW  64
#define NP_KH  34
enum { NK_DEL = 3, NK_DOT = 7, NK_CLEAR = 11, NK_CANCEL = 12, NK_ZERO = 13, NK_AUTO = 14, NK_OK = 15 };
static const char *const np_labels[16] = {
    "7", "8", "9", "Del", "4", "5", "6", ".", "1", "2", "3", "Clear", "Cancel", "0", "Auto", "OK",
};

static int num_kind;
static char nval[16];
static char nerr[40];
static int npressed = -1, np_drawn_pressed = -1;

static bool np_enabled(int k)
{
    if (k == NK_AUTO)
        return num_kind == NUM_RELAY;
    if (k == NK_DOT)
        return num_kind != NUM_PORT;
    return true;
}

static void open_num(int kind)
{
    num_kind = kind;
    switch (kind)
    {
        case NUM_IP:    snprintf(nval, sizeof(nval), "%s", edit.static_ip); break;
        case NUM_MASK:  snprintf(nval, sizeof(nval), "%s", edit.static_mask); break;
        case NUM_GW:    snprintf(nval, sizeof(nval), "%s", edit.static_gw); break;
        case NUM_RELAY: snprintf(nval, sizeof(nval), "%s", edit.relay_host); break;
        case NUM_PORT:  snprintf(nval, sizeof(nval), "%u", edit.relay_port); break;
    }
    nerr[0] = '\0';
    npressed = -1;
    go(SP_NUM);
}

static void np_rect(int k, int *x, int *y, int *w, int *h)
{
    *x = (k % 4) * NP_KW + 1;
    *y = NP_Y + (k / 4) * NP_KH + 1;
    *w = NP_KW - 2;
    *h = NP_KH - 2;
}

static void draw_np_key(int k)
{
    int x, y, w, h;
    np_rect(k, &x, &y, &w, &h);
    u16 *fb = gfx_bottom();
    u16 bg = k == NK_OK ? COL_KEY_SEND : k == NK_CANCEL ? COL_KEY_STOP : COL_KEY;
    if (k == npressed)
        bg = COL_KEY_DOWN;
    if (!np_enabled(k))
    {
        gfx_fill(fb, x, y, w, h, COL_BG);
        return;
    }
    gfx_fill(fb, x, y, w, h, bg);
    gfx_fill(fb, x, y, w, 1, COL_KEY_EDGE);
    const char *l = np_labels[k];
    int len = strlen(l);
    if (len == 1)       // digits big
        gfx_text_big(fb, x + (w - 16) / 2, y + (h - 16) / 2, l, 2,
                     k == npressed ? COL_BG : COL_TEXT, bg);
    else
        gfx_text(fb, x + (w - len * FONT_W) / 2, y + (h - FONT_H) / 2, l, len,
                 k == npressed ? COL_BG : COL_TEXT, bg);
}

static void draw_num(bool full)
{
    static const char *const titles[] = { "Static IP address", "Netmask", "Gateway",
                                          "Relay IPv4 address", "Relay port" };
    static const char *const hints[] = { "e.g. 192.168.77.60", "e.g. 255.255.255.0",
                                         "e.g. 192.168.77.1", "empty or Auto = gateway",
                                         "1 - 65535 (default 4242)" };
    u16 *fb = gfx_bottom();
    if (full)
        title(fb, titles[num_kind], "START: OK");
    // value, big, with a cursor block
    gfx_fill(fb, 0, 14, SCREEN_W, 22, COL_INPUT_BG);
    char shown[18];
    snprintf(shown, sizeof(shown), "%s_", nval);
    gfx_text_big(fb, 4, 17, shown, 2, COL_TEXT, COL_INPUT_BG);
    gfx_text_cols(fb, 0, 40, nerr[0] ? nerr : hints[num_kind], 32,
                  nerr[0] ? COL_ERROR : COL_INFO, COL_BG);
    if (full)
    {
        for (int k = 0; k < 16; k++)
            draw_np_key(k);
    }
    else if (np_drawn_pressed != npressed)
    {
        if (np_drawn_pressed >= 0)
        {
            int old = np_drawn_pressed;
            np_drawn_pressed = -1;
            draw_np_key(old);
        }
        if (npressed >= 0)
            draw_np_key(npressed);
    }
    np_drawn_pressed = npressed;
}

static void num_ok(void)
{
    uint8_t b[4];
    switch (num_kind)
    {
        case NUM_IP:
        case NUM_GW:
            if (!ipv4_parse(nval, b))
            {
                snprintf(nerr, sizeof(nerr), "not a valid IPv4 address");
                dirty();
                return;
            }
            snprintf(num_kind == NUM_IP ? edit.static_ip : edit.static_gw, 16, "%s", nval);
            break;
        case NUM_MASK:
            if (!netmask_valid(nval))
            {
                snprintf(nerr, sizeof(nerr), "not a valid netmask");
                dirty();
                return;
            }
            snprintf(edit.static_mask, sizeof(edit.static_mask), "%s", nval);
            break;
        case NUM_RELAY:
            if (nval[0] && !ipv4_parse(nval, b))
            {
                snprintf(nerr, sizeof(nerr), "not a valid IPv4 address");
                dirty();
                return;
            }
            snprintf(edit.relay_host, sizeof(edit.relay_host), "%s", nval);
            break;
        case NUM_PORT:
        {
            long v = nval[0] ? strtol(nval, NULL, 10) : 0;
            if (v < 1 || v > 65535)
            {
                snprintf(nerr, sizeof(nerr), "port must be 1 - 65535");
                dirty();
                return;
            }
            edit.relay_port = (uint16_t)v;
            break;
        }
    }
    go(SP_MAIN);
}

static void num_key(int k)
{
    if (!np_enabled(k))
        return;
    size_t len = strlen(nval);
    size_t max = num_kind == NUM_PORT ? 5 : 15;
    nerr[0] = '\0';
    switch (k)
    {
        case NK_DEL:    if (len) nval[len - 1] = '\0'; break;
        case NK_CLEAR:  nval[0] = '\0'; break;
        case NK_CANCEL: go(SP_MAIN); return;
        case NK_AUTO:   edit.relay_host[0] = '\0'; go(SP_MAIN); return;
        case NK_OK:     num_ok(); return;
        default:
            if (len < max)
            {
                nval[len] = np_labels[k][0];
                nval[len + 1] = '\0';
            }
            break;
    }
    dirty();
}

static void num_input(uint32_t down, uint32_t rep, uint32_t up)
{
    if (down & KEY_SELECT)
    {
        go(SP_MAIN);
        return;
    }
    if (down & (KEY_START | KEY_A))
    {
        num_ok();
        return;
    }
    if (rep & KEY_B)
        num_key(NK_DEL);
    int x, y;
    if (touched(&x, &y, down) && y >= NP_Y)
    {
        int k = (y - NP_Y) / NP_KH * 4 + x / NP_KW;
        if (k >= 0 && k < 16 && np_enabled(k))
        {
            npressed = k;
            num_key(k);
            dirty();
        }
    }
    if ((up & KEY_TOUCH) && npressed >= 0)
    {
        npressed = -1;
        dirty();
    }
}

// --------------------------------------------------------------------------
// Public
// --------------------------------------------------------------------------

void settings_open(const char *why)
{
    edit = *app.cfg;
    page = SP_MAIN;
    sel = 0;
    testing = false;
    net_stop();
    if (why)
    {
        set_status(COL_ERROR, "%s", why);
        char msg[96];
        snprintf(msg, sizeof(msg), "Settings: %.80s", why);
        app_say(TB_ERROR, msg);
    }
    else
    {
        set_status(COL_DIM, "A/touch: change  B: cancel", NULL);
    }
    need_full = true;
    dirty();
}

void settings_input(uint32_t down, uint32_t rep, uint32_t held, uint32_t up)
{
    switch (page)
    {
        case SP_MAIN: main_input(down, rep); break;
        case SP_LIST: list_input(down, rep); break;
        case SP_TEXT: text_input(down, rep, held, up); break;
        case SP_NUM:  num_input(down, rep, up); break;
    }
}

void settings_tick(uint32_t now)
{
    now_f = now;
    if (app.menu == MENU_SETTINGS && page == SP_LIST && now % 20 == 0)
        refresh_list();
}

void settings_draw(bool full)
{
    full = full || need_full;
    need_full = false;
    switch (page)
    {
        case SP_MAIN: draw_main(); break;
        case SP_LIST: draw_list(); break;
        case SP_TEXT: draw_text(full); break;
        case SP_NUM:  draw_num(full); break;
    }
}

void settings_on_test(int stage, bool ok, const char *msg)
{
    static const char *const names[] = { "", "Find network", "Join", "IP address", "Relay", "Done" };
    const char *name = stage >= 1 && stage <= TS_DONE ? names[stage] : "?";
    char line[128];
    snprintf(line, sizeof(line), "[test] %s: %s - %s", name, ok ? "OK" : "FAILED", msg);
    app_say(ok ? TB_INFO : TB_ERROR, line);
    if (!ok)
    {
        testing = false;
        char s[80];
        snprintf(s, sizeof(s), "%s failed: %s", name, msg);
        set_status(COL_ERROR, "%s", s);
    }
    else if (stage == TS_DONE)
    {
        testing = false;
        set_status(COL_BAR_OK, "Test passed - Save to use it", NULL);
    }
    else
    {
        set_status(COL_INFO, "Testing: %s OK...", name);
    }
}
