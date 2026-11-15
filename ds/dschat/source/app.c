// App logic: input handling, sending / cancelling, relay replies, menu,
// start-up (settings / auto-connect) and power off.

#include <stdio.h>
#include <string.h>

#include <nds.h>

#include "app.h"
#include "dschat_build.h"
#include "gfx.h"
#include "net.h"
#include "settings.h"
#include "soak.h"
#include "voice.h"

app_t app;

#define LONG_PRESS   24      // frames before a held key repeats
#define REPEAT_EVERY 4

// --------------------------------------------------------------------------
// Conversation helpers
// --------------------------------------------------------------------------

static void conv_changed(void)
{
    app_view_update();
    app.dirty_conv = true;
}

static void say(uint8_t kind, const char *text)
{
    if (app.reply_text)          // never glue a notice onto a streaming reply
    {
        tb_end(&app.tb);
        app.reply_text = false;
    }
    tb_puts(&app.tb, kind, text);
    conv_changed();
}

void app_say(uint8_t kind, const char *text)
{
    say(kind, text);
}

static void sayf(uint8_t kind, const char *fmt, const char *arg)
{
    char buf[160];
    snprintf(buf, sizeof(buf), fmt, arg);
    say(kind, buf);
}

uint32_t app_display_end(void)
{
    // While waiting for the first text an extra "Waiting... Ns" row is shown.
    return tb_end_line(&app.tb) + (app.reply == R_WAITING ? 1 : 0);
}

static uint32_t max_top(void)
{
    uint32_t end = app_display_end(), first = tb_first(&app.tb);
    return end > first + VIEW_ROWS ? end - VIEW_ROWS : first;
}

void app_view_update(void)
{
    uint32_t first = tb_first(&app.tb), top = max_top();
    if (app.follow || app.view_top > top)
        app.view_top = top;
    if (app.view_top < first)
        app.view_top = first;
    if (app.view_top >= top)
        app.follow = true;
}

static void scroll(int delta)
{
    uint32_t first = tb_first(&app.tb), top = max_top();
    int64_t v = (int64_t)app.view_top + delta;
    if (v < first)
        v = first;
    if (v > top)
        v = top;
    app.view_top = (uint32_t)v;
    app.follow = app.view_top >= top;
    app.dirty_conv = true;
}

static void restore_input(void)
{
    // Put a message that did not get an answer back into the input box, so
    // it can be sent again (unless the user already typed something new).
    if (app.last_sent[0] && input_blank(&app.in))
    {
        input_set(&app.in, app.last_sent);
        app.dirty_input = true;
        say(TB_INFO, "(your message is back in the input box)");
    }
    app.last_sent[0] = '\0';
}

static void end_reply(void)
{
    if (app.reply_text)
        tb_end(&app.tb);
    app.reply_text = false;
    app.reply = R_IDLE;
    app.local_cancel = false;
    app.dirty_kbd = true;        // the Send key shows "Send" again
    app.dirty_input = true;      // hint line
    conv_changed();
}

// --------------------------------------------------------------------------
// Actions
// --------------------------------------------------------------------------

static void do_send(void)
{
    if (app.reply != R_IDLE)
        return;                  // the key says "Stop" meanwhile
    if (input_blank(&app.in))
        return;
    if (!net_online())
    {
        say(TB_ERROR, "! not connected yet - your message is kept, send it again when online");
        return;
    }
    if (app.in.len > app.in.max)
    {
        char msg[64];
        snprintf(msg, sizeof(msg), "! message too long (max %u characters)", app.in.max);
        say(TB_ERROR, msg);
        return;
    }
    if (!net_request(TAG_SEND, "SEND", app.in.buf, app.in.len))
    {
        say(TB_ERROR, "! could not send, try again");
        return;
    }
    memcpy(app.last_sent, app.in.buf, app.in.len + 1);

    tb_blank(&app.tb);
    tb_begin(&app.tb, TB_USER);
    tb_append(&app.tb, "> ", 2);
    tb_append(&app.tb, app.in.buf, app.in.len);
    tb_end(&app.tb);

    input_clear(&app.in);
    app.in_scroll = 0;
    app.reply = R_WAITING;
    app.reply_start = app.now;
    app.reply_text = false;
    app.follow = true;           // jump to the newest text when sending
    app.dirty_input = true;
    app.dirty_kbd = true;
    conv_changed();
}

static void do_cancel(void)
{
    if (app.reply != R_WAITING && app.reply != R_STREAMING)
        return;
    if (net_server()->version >= 3)
    {
        net_cancel();            // relay answers FAIL cancelled (or DONE if late)
        app.reply = R_CANCELLING;
    }
    else
    {
        // Relays before v3 cannot stop a reply: hide the rest of it here.
        app.local_cancel = true;
        app.reply = R_CANCELLING;
        say(TB_INFO, "[stopped; this older relay still keeps the full reply]");
    }
    app.dirty_kbd = true;
    conv_changed();
}

static void open_menu(int page)
{
    app.menu = page;
    app.menu_sel = 0;
    app.menu_scroll = 0;
    app.menu_msg[0] = '\0';
    app.dirty_menu = true;
}

static void close_menu(void)
{
    app.menu = MENU_NONE;
    app.menu_wait = false;
    app.dirty_input = true;
    app.dirty_kbd = true;
}

void app_close_menu(void)
{
    close_menu();
}

void app_save_config(void)
{
    app.cfg_dirty = !storage_save(&app.store, app.cfg) && app.store.sd;
}

// Opens the settings screen (stops the connection while it is open).
static void open_settings(const char *why)
{
    if (app.reply != R_IDLE)
    {
        snprintf(app.menu_msg, sizeof(app.menu_msg), "wait for the reply (X: stop)");
        app.dirty_menu = true;
        return;
    }
    soak_stop(app.now);
    app.boot_wait = false;
    open_menu(MENU_SETTINGS);
    settings_open(why);
}

static void power_off(void)
{
    // Tell the user, stop a pending reply, leave the relay and the AP cleanly,
    // save what is unsaved, then switch the console off.
    say(TB_INFO, "Turning off...");
    gfx_fill(gfx_bottom(), 0, 0, SCREEN_W, SCREEN_H, COL_BG);
    gfx_text(gfx_bottom(), 72, 92, "Turning off...", 14, COL_TEXT, COL_BG);
    ui_render();
    if ((app.reply == R_WAITING || app.reply == R_STREAMING) && net_server()->version >= 3)
        net_cancel();
    if (app.cfg_dirty)
        app_save_config();
    net_shutdown();
    for (int i = 0; i < 30; i++)          // let the TCP FIN / deauth go out
    {
        cothread_yield_irq(IRQ_VBLANK);
        gfx_present();
    }
    systemShutDown();
    while (1)                              // only reached if shutdown failed
        cothread_yield_irq(IRQ_VBLANK);
}

static bool busy_for_menu(void)
{
    if (!net_online())
    {
        snprintf(app.menu_msg, sizeof(app.menu_msg), "not connected to the relay");
        app.dirty_menu = true;
        return true;
    }
    if (app.reply != R_IDLE || app.menu_wait)
    {
        snprintf(app.menu_msg, sizeof(app.menu_msg), "wait for the reply (X: stop)");
        app.dirty_menu = true;
        return true;
    }
    return false;
}

static void menu_choose(int i)
{
    if (app.menu == MENU_MAIN)
    {
        switch (i)
        {
            case MAIN_NEW:
                if (busy_for_menu())
                    return;
                if (net_request(TAG_RSET, "RSET", NULL, 0))
                {
                    app.menu_wait = true;
                    snprintf(app.menu_msg, sizeof(app.menu_msg), "starting a new chat...");
                }
                break;
            case MAIN_PROFILE:
                if (busy_for_menu())
                    return;
                if (net_request(TAG_PLST, "PLST", NULL, 0))
                {
                    open_menu(MENU_PROFILES);
                    app.menu_wait = true;
                    app.nprofiles = 0;
                }
                break;
            case MAIN_SETTINGS:
                open_settings(NULL);
                return;
            case MAIN_RECONNECT:
                if (net_state() == NS_IDLE)
                {
                    snprintf(app.menu_msg, sizeof(app.menu_msg), "not started: check Settings");
                    break;
                }
                close_menu();
                say(TB_INFO, "Reconnecting...");
                app.user_reconnect = true;
                net_reconnect();
                app.user_reconnect = false;
                return;
            case MAIN_SOAK:
                if (busy_for_menu())
                    return;
                open_menu(MENU_SOAK);
                soak_start(app.now);
                return;
            case MAIN_ABOUT:
                open_menu(MENU_ABOUT);
                return;
            case MAIN_POWER:
                open_menu(MENU_POWER);
                app.menu_sel = 1;           // "No" preselected
                return;
            default:
                close_menu();
                return;
        }
        app.dirty_menu = true;
    }
    else if (app.menu == MENU_POWER)
    {
        if (i == 0)
            power_off();
        else
            open_menu(MENU_MAIN);
    }
    else if (app.menu == MENU_PROFILES)
    {
        if (app.menu_wait || i < 0 || i >= app.nprofiles)
            return;
        proto_profile *p = &app.profiles[i];
        if (p->flag == '!')
        {
            snprintf(app.menu_msg, sizeof(app.menu_msg), "%s: no API key on the relay", p->name);
        }
        else if (p->flag == '*')
        {
            close_menu();
        }
        else if (!busy_for_menu() && net_request(TAG_PSET, "PSET", p->name, strlen(p->name)))
        {
            app.menu_wait = true;
            snprintf(app.menu_msg, sizeof(app.menu_msg), "switching to %s...", p->name);
        }
        app.dirty_menu = true;
    }
}

// Menu geometry, shared with ui.c through these helpers.
int ui_menu_item_at(int x, int y);
int ui_menu_items(void);

static void menu_input(uint32_t down, uint32_t rep, uint32_t held, uint32_t up)
{
    int count = ui_menu_items();
    if (app.menu == MENU_SETTINGS)
    {
        settings_input(down, rep, held, up);
        return;
    }
    if (app.menu == MENU_POWER)
    {
        if (down & KEY_B)
        {
            open_menu(MENU_MAIN);
            return;
        }
        if (rep & (KEY_LEFT | KEY_RIGHT | KEY_UP | KEY_DOWN))
        {
            app.menu_sel = 1 - app.menu_sel;
            app.dirty_menu = true;
        }
        if (down & KEY_A)
            menu_choose(app.menu_sel);
        if (down & KEY_TOUCH)
        {
            touchPosition t;
            touchRead(&t);
            int i = ui_menu_item_at(t.px, t.py);
            if (i >= 0)
                menu_choose(i);
        }
        return;
    }
    if (app.menu == MENU_SOAK)
    {
        // B stops a running test; once it has ended, B (or A) goes back.
        if (down & KEY_B)
        {
            if (soak_running())
                soak_stop(app.now);
            else
                open_menu(MENU_MAIN);
        }
        else if ((down & KEY_A) && !soak_running())
        {
            open_menu(MENU_MAIN);
        }
        return;
    }
    if (down & KEY_B)
    {
        if (app.menu == MENU_MAIN)
            close_menu();
        else
            open_menu(MENU_MAIN);
        return;
    }
    if (app.menu == MENU_ABOUT)
    {
        if (down & (KEY_A | KEY_START | KEY_SELECT | KEY_TOUCH))
            open_menu(MENU_MAIN);
        return;
    }
    if (down & KEY_SELECT)
    {
        close_menu();
        return;
    }
    // The main menu is a 2-column grid: up/down move by a row.
    int step = app.menu == MENU_MAIN ? 2 : 1;
    if ((rep & KEY_UP) && app.menu_sel - step >= 0)
    {
        app.menu_sel -= step;
        app.dirty_menu = true;
    }
    if ((rep & KEY_DOWN) && app.menu_sel + step < count)
    {
        app.menu_sel += step;
        app.dirty_menu = true;
    }
    if (app.menu == MENU_MAIN && (rep & KEY_LEFT) && app.menu_sel % 2 == 1)
    {
        app.menu_sel--;
        app.dirty_menu = true;
    }
    if (app.menu == MENU_MAIN && (rep & KEY_RIGHT) && app.menu_sel % 2 == 0)
    {
        app.menu_sel++;
        app.dirty_menu = true;
    }
    if (down & KEY_A)
        menu_choose(app.menu_sel);
    if (down & KEY_TOUCH)
    {
        touchPosition t;
        touchRead(&t);
        int i = ui_menu_item_at(t.px, t.py);
        if (i >= 0)
        {
            app.menu_sel = i;
            app.dirty_menu = true;
            menu_choose(i);
        }
    }
}

// --------------------------------------------------------------------------
// Keyboard / input box
// --------------------------------------------------------------------------

static void press_key(int idx)
{
    kbd_action a = kbd_press(&app.kbd, idx);
    switch (a.type)
    {
        case K_CHAR:
        case K_SPACE:
            input_insert(&app.in, a.ch);
            break;
        case K_BKSP:
            input_backspace(&app.in);
            break;
        case K_LEFT:
            input_left(&app.in);
            break;
        case K_RIGHT:
            input_right(&app.in);
            break;
        case K_SEND:
            if (app.reply == R_WAITING || app.reply == R_STREAMING)
                do_cancel();
            else
                do_send();
            break;
        default:
            break;
    }
    app.dirty_input = true;
    app.dirty_kbd = true;
}

static void touch_input_box(int x, int y)
{
    static input_row rows[INPUT_CAP + 2];
    int crow, ccol;
    int n = input_layout(&app.in, TB_COLS, rows, INPUT_CAP + 2, &crow, &ccol);
    int r = app.in_scroll + (y - INPUT_Y) / FONT_H;
    if (r < 0 || r >= n)
    {
        input_end(&app.in);
    }
    else
    {
        int c = x / 8;
        if (c > rows[r].len)
            c = rows[r].len;
        app.in.cur = rows[r].start + c;
    }
    app.dirty_input = true;
}

// Hold Y: voice input (protocol v4). Refused with a reason when it can't work.
static void start_voice(uint32_t now)
{
    if (!net_online())
        say(TB_ERROR, "Voice: not connected to the relay.");
    else if (!net_server()->voice)
        say(TB_INFO, "Voice input not available on this relay.");
    else if (app.reply != R_IDLE)
        say(TB_INFO, "Voice: wait for the reply to finish.");
    else if (!voice_start(now, config_voice_code(app.cfg->voice_lang)))
        say(TB_ERROR, "Voice: the microphone did not start.");
    app.dirty_input = true;
}

void app_voice_text(const char *text)
{
    // trim the relay's leading spaces; nothing heard -> notice, input untouched
    while (*text == ' ')
        text++;
    if (*text == '\0')
    {
        say(TB_INFO, "Didn't catch that.");
        app.dirty_input = true;
        return;
    }
    // insert at the cursor, separated from the text around it by spaces
    if (app.in.cur > 0 && app.in.buf[app.in.cur - 1] != ' ')
        input_insert(&app.in, ' ');
    int dropped = 0;
    for (const char *p = text; *p; p++)
        if (*p != '\n' && !input_insert(&app.in, *p))
            dropped++;
    if (app.in.cur < app.in.len && app.in.buf[app.in.cur] != ' ')
        input_insert(&app.in, ' ');
    if (dropped)
        say(TB_ERROR, "Voice: the input box is full, part of the text was dropped.");
    app.dirty_input = true;
}

void app_voice_fail(const char *why)
{
    sayf(TB_ERROR, "Voice: %s", why);
    app.dirty_input = true;
}

void app_input(uint32_t now)
{
    app.now = now;
    scanKeys();
    uint32_t down = keysDown();
    uint32_t rep = keysDownRepeat();
    uint32_t held = keysHeld();
    uint32_t up = keysUp();

    if (up & KEY_LID)
        net_wake(now);           // the console was asleep: check the link

    if (app.boot_wait && (held & KEY_SELECT))
    {
        open_settings("SELECT held at start");
        return;
    }

    if (app.menu != MENU_NONE)
    {
        menu_input(down, rep, held, up);
        return;
    }

    // Voice input: while it is busy only B (cancel) and releasing Y count.
    if (voice_active())
    {
        if (down & KEY_B)
        {
            voice_cancel();
            say(TB_INFO, "Voice input cancelled.");
            app.dirty_input = true;
        }
        else if ((up & KEY_Y) || !(held & KEY_Y))
            voice_release(now);
        return;
    }
    if (down & KEY_Y)
    {
        start_voice(now);
        return;
    }

    if (down & KEY_SELECT)
    {
        open_menu(MENU_MAIN);
        return;
    }
    if (down & KEY_START)
        do_send();
    if (down & KEY_X)
        do_cancel();
    if (rep & KEY_B)
    {
        input_backspace(&app.in);
        app.dirty_input = true;
    }
    if (rep & KEY_LEFT)
    {
        input_left(&app.in);
        app.dirty_input = true;
    }
    if (rep & KEY_RIGHT)
    {
        input_right(&app.in);
        app.dirty_input = true;
    }
    if (rep & KEY_UP)
        scroll(-1);
    if (rep & KEY_DOWN)
        scroll(1);
    if (rep & KEY_L)
        scroll(-(VIEW_ROWS - 1));
    if (rep & KEY_R)
        scroll(VIEW_ROWS - 1);

    if (down & KEY_TOUCH)
    {
        touchPosition t;
        touchRead(&t);
        int idx = kbd_hit(&app.kbd, t.px, t.py);
        if (idx >= 0)
        {
            app.pressed = idx;
            app.pressed_since = now;
            press_key(idx);
        }
        else if (t.py >= INPUT_Y && t.py < INPUT_Y + INPUT_ROWS * FONT_H)
        {
            touch_input_box(t.px, t.py);
        }
    }
    else if ((held & KEY_TOUCH) && app.pressed >= 0)
    {
        // Holding Bksp or the arrows repeats them.
        uint8_t type = app.kbd.keys[app.pressed].type;
        uint32_t t = now - app.pressed_since;
        if ((type == K_BKSP || type == K_LEFT || type == K_RIGHT) && t > LONG_PRESS
            && t % REPEAT_EVERY == 0)
            press_key(app.pressed);
    }
    if ((up & KEY_TOUCH) && app.pressed >= 0)
    {
        app.pressed = -1;
        app.dirty_kbd = true;
    }
}

// --------------------------------------------------------------------------
// Relay callbacks (from net.c)
// --------------------------------------------------------------------------

void app_net_online(const proto_server_info *si, const char *profile, const char *model)
{
    snprintf(app.profile, sizeof(app.profile), "%s", profile[0] ? profile : "?");
    snprintf(app.model, sizeof(app.model), "%s", model);
    input_set_max(&app.in, si->max_msg > 0 ? si->max_msg : 1024);
    app.dirty_input = true;

    char msg[128];
    if (!app.ever_online)
    {
        snprintf(msg, sizeof(msg), "Connected. Profile: %s (%s).", app.profile, app.model);
        say(TB_INFO, msg);
    }
    else if (si->boot[0] && app.boot[0] && strcmp(si->boot, app.boot) != 0)
    {
        snprintf(msg, sizeof(msg),
                 "Relay restarted: the AI has forgotten this conversation. "
                 "Profile: %s.", app.profile);
        say(TB_ERROR, msg);
    }
    else if (si->boot[0])
    {
        say(TB_INFO, "Reconnected. The conversation continues.");
    }
    else
    {
        say(TB_INFO, "Reconnected (old relay: it may have lost the context).");
    }
    snprintf(app.boot, sizeof(app.boot), "%s", si->boot);
    app.ever_online = true;
    memset(app.notice, 0, sizeof(app.notice));

    // Restore the last used profile (saved in the settings). Only when the
    // relay has a different one: after a relay restart or on first connect.
    if (app.cfg->profile[0] && strcmp(app.cfg->profile, app.profile) != 0 && si->version >= 2)
        net_request(TAG_PSET_RESTORE, "PSET", app.cfg->profile, strlen(app.cfg->profile));
}

void app_net_lost(const char *reason)
{
    if (app.reply != R_IDLE)
    {
        bool quiet = app.local_cancel;
        end_reply();
        if (!quiet)
        {
            say(TB_ERROR, "! connection lost during the reply");
            restore_input();
        }
    }
    soak_on_lost(reason, app.now);
    voice_lost();
    snprintf(app.last_reason, sizeof(app.last_reason), "%s", reason);
    if (!app.user_reconnect)
        sayf(TB_ERROR, "Connection lost (%s). Reconnecting...", reason);
    app.user_reconnect = false;
    memset(app.notice, 0, sizeof(app.notice));
    if (app.menu_wait)
    {
        app.menu_wait = false;
        snprintf(app.menu_msg, sizeof(app.menu_msg), "connection lost");
        app.dirty_menu = true;
    }
}

void app_net_test(int stage, bool ok, const char *msg)
{
    settings_on_test(stage, ok, msg);
}

void app_net_notice(const char *text)
{
    snprintf(app.last_reason, sizeof(app.last_reason), "%s", text);
    // Repeated failures while reconnecting: show each distinct reason once per
    // outage (they may alternate, e.g. "not found" / "resetting the radio").
    int free_slot = -1;
    for (int i = 0; i < 4; i++)
    {
        if (strcmp(text, app.notice[i]) == 0)
            return;
        if (free_slot < 0 && app.notice[i][0] == '\0')
            free_slot = i;
    }
    if (free_slot < 0)
        return;                  // enough said; the status bar shows the state
    snprintf(app.notice[free_slot], sizeof(app.notice[0]), "%s", text);
    sayf(TB_INFO, "(%s)", text);
}

static void on_send_frame(const char *type, const char *payload, int len)
{
    if (strcmp(type, "TEXT") == 0)
    {
        if (app.local_cancel)
            return;
        if (!app.reply_text)
        {
            tb_begin(&app.tb, TB_AI);
            app.reply_text = true;
        }
        if (app.reply == R_WAITING)
        {
            app.reply = R_STREAMING;
            app.dirty_kbd = true;
        }
        tb_append(&app.tb, payload, len);
        conv_changed();
        return;
    }

    bool quiet = app.local_cancel;
    end_reply();
    if (quiet)
        return;
    if (strcmp(type, "DONE") == 0)
    {
        app.last_sent[0] = '\0';
    }
    else if (strcmp(type, "FAIL") == 0)
    {
        if (strcmp(payload, "cancelled") == 0)
        {
            say(TB_INFO, "[stopped; not kept in the conversation]");
        }
        else
        {
            static char msg[PROTO_MAX_PAYLOAD + 4];
            snprintf(msg, sizeof(msg), "! %s", payload);
            say(TB_ERROR, msg);
        }
        restore_input();
    }
}

void app_net_frame(int tag, const char *type, const char *payload, int len)
{
    bool ok = strcmp(type, "OKAY") == 0 || strcmp(type, "INFO") == 0;
    switch (tag)
    {
        case TAG_SEND:
            on_send_frame(type, payload, len);
            return;
        case TAG_VOICE:
            voice_on_reply(app.now, type, payload);
            app.dirty_input = true;
            return;
        case TAG_SOAK:
            soak_on_frame(type, app.now);
            return;
        case TAG_RSET:
            app.menu_wait = false;
            if (ok)
            {
                tb_clear(&app.tb);
                app.follow = true;
                close_menu();
                char msg[64];
                snprintf(msg, sizeof(msg), "New chat with %s.", app.profile);
                say(TB_INFO, msg);
            }
            else
            {
                snprintf(app.menu_msg, sizeof(app.menu_msg), "%.32s", payload);
                app.dirty_menu = true;
            }
            return;
        case TAG_PLST:
            app.menu_wait = false;
            if (ok)
            {
                app.nprofiles = proto_parse_plst(payload, app.profiles, MAX_PROFILES);
                for (int i = 0; i < app.nprofiles; i++)
                {
                    if (app.profiles[i].flag == '*')
                        app.menu_sel = i;
                }
            }
            else
            {
                snprintf(app.menu_msg, sizeof(app.menu_msg), "%s",
                         strcmp(payload, "unknown frame type") == 0
                             ? "this relay has no profiles" : payload);
            }
            app.dirty_menu = true;
            return;
        case TAG_PSET_RESTORE:
            if (strcmp(type, "OKAY") == 0)
            {
                proto_parse_name_model(payload, app.profile, sizeof(app.profile),
                                       app.model, sizeof(app.model));
                char msg[96];
                snprintf(msg, sizeof(msg), "Profile %s (%s) restored.", app.profile, app.model);
                say(TB_INFO, msg);
            }
            else
            {
                char msg[96];
                snprintf(msg, sizeof(msg), "(saved profile %.16s not available: %.40s)",
                         app.cfg->profile, payload);
                say(TB_INFO, msg);
            }
            return;
        case TAG_PSET:
            app.menu_wait = false;
            if (strcmp(type, "OKAY") == 0)
            {
                proto_parse_name_model(payload, app.profile, sizeof(app.profile),
                                       app.model, sizeof(app.model));
                // remember it for the next start
                snprintf(app.cfg->profile, sizeof(app.cfg->profile), "%s", app.profile);
                app_save_config();
                close_menu();
                tb_blank(&app.tb);
                char msg[128];
                snprintf(msg, sizeof(msg), "Switched to %s (%s). New conversation.",
                         app.profile, app.model);
                say(TB_INFO, msg);
            }
            else
            {
                snprintf(app.menu_msg, sizeof(app.menu_msg), "%.32s", payload);
                app.dirty_menu = true;
            }
            return;
    }
}

// --------------------------------------------------------------------------

void app_init(dschat_config *cfg)
{
    memset(&app, 0, sizeof(app));
    app.cfg = cfg;
    app.pressed = -1;
    app.follow = true;
    tb_init(&app.tb);
    input_init(&app.in, 1024);
    kbd_init(&app.kbd, 0, KBD_Y, 256, KBD_H);
    snprintf(app.profile, sizeof(app.profile), "-");
    app.dirty_conv = app.dirty_input = app.dirty_kbd = true;

    say(TB_INFO, "DSChat " DSCHAT_VERSION " - chat with an AI from your DS.");
    say(TB_INFO, "START/Send: send  X: stop  SELECT: menu  Up/Down L/R: scroll");
    voice_init();
    storage_init(&app.store);
    storage_load(&app.store, cfg);
    say(app.store.status == ST_LOADED || app.store.status == ST_NO_SD ? TB_INFO : TB_ERROR,
        app.store.msg);
}

void app_boot(uint32_t now)
{
    app.now = now;
    if (!app.wifi_ok)
        return;
    bool usable = app.store.status == ST_LOADED || app.store.status == ST_NO_SD;
    if (!usable)
    {
        // No valid settings file: set up first (pre-filled with what was read).
        open_settings(app.store.msg);
        return;
    }
    char msg[80];
    snprintf(msg, sizeof(msg), "Wi-Fi \"%.32s\" - hold SELECT now for settings...", app.cfg->ssid);
    say(TB_INFO, msg);
    app.boot_wait = true;
    app.boot_until = now + BOOT_WINDOW;
}

void app_tick(uint32_t now)
{
    app.now = now;
    if (app.boot_wait && now >= app.boot_until)
    {
        app.boot_wait = false;
        net_start(app.cfg, false);
    }
    // Never got online since start-up and keeps failing: offer the settings.
    if (!app.ever_online && !app.auto_settings_done && app.menu == MENU_NONE
        && net_failures() >= 3)
    {
        app.auto_settings_done = true;
        char msg[96];
        snprintf(msg, sizeof(msg), "cannot connect: %.60s", app.last_reason);
        open_settings(msg);
    }
    settings_tick(now);
    soak_poll(now);
    if (voice_active())
    {
        voice_poll(now);
        if (now % 3 == 0 || !voice_active())
            app.dirty_input = true;     // meter, seconds, phase changes
    }
    if (app.menu == MENU_SOAK && now % 6 == 0)
        app.dirty_menu = true;          // live counters, 10 times a second
    // The "Waiting... Ns" row and the status bar change once per second.
    static uint32_t last_sec;
    uint32_t sec = now / 60;
    if (sec != last_sec)
    {
        last_sec = sec;
        if (app.reply == R_WAITING)
            app.dirty_conv = true;
        if (app.menu == MENU_ABOUT)
            app.dirty_menu = true;      // live network counters
    }
}
