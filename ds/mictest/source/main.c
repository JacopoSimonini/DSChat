// mictest: DS Lite microphone experiment (push-to-talk -> laptop -> Whisper).
// NOT part of DSChat 1.0: a test ROM to decide whether voice input is worth
// building (see docs/TECHNICAL-NOTES.md, "Microphone").
//
// Hold Y to record (max 10 s), release to upload the clip to the laptop's
// receiver (tools/mic/receiver.py, TCP port 4243), which saves it, measures
// it and transcribes it; the transcripts and timings appear on the top screen.
// Network settings: the DSChat settings file (dschat/settings.txt), with the
// port replaced by MIC_PORT. Reuses dschat's net/config/storage/gfx/proto/
// textbuf modules unchanged (shared/ = symlinks).
//
// Receiver protocol (DSChat framing "TYPE LLLL\n" + payload; the receiver
// also answers PING/PGET/LIVE like the relay so net.c works unchanged):
//   MBEG <meta key=value line>  -> OKAY
//   MDAT <raw samples, <=4096>  -> OKAY <bytes received so far>
//   MEND                        -> OKAY <bytes> <ms the laptop measured>
//   SEND transcribe             -> TEXT <line>* then DONE (or FAIL)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <nds.h>
#include <dswifi9.h>

#include "config.h"
#include "gfx.h"
#include "micutil.h"
#include "net.h"
#include "storage.h"
#include "textbuf.h"

#define MIC_PORT     4243
#define FIFO_MIC_GAIN FIFO_USER_01     // handled by our ARM7 (arm7/source/main.c)
#define MAX_SEC      10
#define REC_CAP      (16000 * 2 * MAX_SEC)
#define DBUF_BYTES   2048              // ARM7 double buffer (2 halves)
#define CHUNK        4096
#define PING_FLIGHT  4                 // PINGs kept in flight with traffic on

enum tag { TAG_PING = 1, TAG_BEG, TAG_DAT, TAG_END, TAG_TX, TAG_STA };
enum phase { PH_IDLE, PH_REC, PH_STOPPING, PH_UPLOAD, PH_WAIT };
#define STOP_FRAMES 6                 // wait for the ARM7's last (partial) buffer
enum opt { OPT_FMT, OPT_RATE, OPT_GAIN, OPT_TRAFFIC, OPT_PACE, OPT_REPLAY, OPT_COUNT };
#define PACE_BYTES 1260                // one TCP segment per frame (dschat's net.c default)

static const int rates[2] = { 16000, 8000 };

static dschat_config cfg, mic_cfg;
static storage_t store;
static textbuf tb;
static volatile uint32_t frames;

// options
static int opt_sel;
static bool opt_12bit = true;
static int opt_rate = 0;               // index into rates
static int opt_gain = 3;               // PM_GAIN_160 (the libnds default)
static bool opt_traffic = false;
static bool opt_paced = true;          // false = burst: the 1.0 behaviour (stall test)
static uint32_t rej_start;

// recording (written by the mic callback = IRQ context)
static u8 rec[REC_CAP] __attribute__((aligned(32)));
static u8 dbuf[DBUF_BYTES] __attribute__((aligned(32)));
static volatile uint32_t rec_len, rec_cap;
static volatile uint32_t lvl_peak;      // peak of the last buffer (meter)
static volatile mu_stats rec_stats;
static int rec_bits = 16, rec_rate = 16000, rec_gain = 3;
static uint32_t rec_start;
static mu_meta meta;
static uint32_t clip_no;
static uint32_t peak_hold, peak_hold_at;

// upload
static volatile int phase = PH_IDLE;
static uint32_t up_off, up_acked, up_start, up_ms, tx_start;
static bool up_beg, up_end;
static int pings_out;

// drawing
static bool dirty_top = true, dirty_opts = true, dirty_meter = true, dirty_net = true;
static uint32_t view_top;
static bool follow = true;

static void say(int kind, const char *s)
{
    tb_puts(&tb, kind, s);
    dirty_top = true;
}

// --------------------------------------------------------------------------
// Microphone
// --------------------------------------------------------------------------

static void mic_cb(void *completed, int length)
{
    // IRQ context (FIFO handler). The ARM7 wrote the half buffer to main RAM:
    // read it through the uncached mirror.
    if (phase != PH_REC && phase != PH_STOPPING)
        return;                     // late buffer after the clip was finished
    const u8 *src = memUncached(completed);
    uint32_t n = (uint32_t)length;
    if (rec_len + n > rec_cap)
        n = rec_cap - rec_len;
    if (n == 0)
        return;
    memcpy(rec + rec_len, src, n);
    mu_stats st = { 0 };
    mu_stats_add(&st, src, n, rec_bits);
    lvl_peak = st.peak;
    if (st.peak > rec_stats.peak)
        rec_stats.peak = st.peak;
    rec_stats.clipped += st.clipped;
    rec_stats.count += st.count;
    rec_len += n;
}

static void rec_start_now(void)
{
    rec_bits = opt_12bit ? 16 : 8;
    rec_rate = rates[opt_rate];
    rec_gain = opt_gain;
    rec_cap = (uint32_t)rec_rate * (rec_bits / 8) * MAX_SEC;
    if (rec_cap > REC_CAP)
        rec_cap = REC_CAP;
    rec_len = 0;
    lvl_peak = 0;
    memset((void *)&rec_stats, 0, sizeof(rec_stats));
    memset(&meta, 0, sizeof(meta));
    meta.traffic = opt_traffic;
    peak_hold = 0;
    pings_out = 0;
    if (!soundMicRecord(dbuf, sizeof(dbuf), opt_12bit ? MicFormat_12Bit : MicFormat_8Bit,
                        rec_rate, mic_cb))
    {
        say(TB_ERROR, "soundMicRecord failed");
        return;
    }
    // soundMicRecord() turned the amplifier on at x160: now set ours.
    fifoSendValue32(FIFO_MIC_GAIN, (u32)rec_gain);
    rec_start = frames;
    phase = PH_REC;
    dirty_meter = dirty_net = true;
}

static void start_upload(void);

static uint32_t stop_at;

// Y released: stop the mic, but finish only after a few frames - the ARM7
// still delivers the last partial buffer after soundMicOff().
static void rec_stop_now(void)
{
    soundMicOff();
    meta.rec_ms = (frames - rec_start) * 1000 / 60;
    stop_at = frames;
    phase = PH_STOPPING;
}

static void rec_finish(void)
{
    meta.clip_no = ++clip_no;
    meta.bits = rec_bits;
    meta.rate = rec_rate;
    meta.gain = rec_gain;
    meta.bytes = rec_len;
    char line[96];
    uint32_t audio_ms = rec_len / (rec_bits / 8) * 1000 / rec_rate;
    snprintf(line, sizeof(line), "#%lu %d-bit %dk x%d traffic %s",
             (unsigned long)clip_no, rec_bits == 16 ? 12 : 8, rec_rate / 1000,
             mu_gain_mult(rec_gain), meta.traffic ? "on" : "off");
    say(TB_USER, line);
    mu_stats st = rec_stats;
    snprintf(line, sizeof(line), "rec %lu.%02lus (audio %lu.%02lus) %lu B peak %d%% clip %d.%d%%",
             (unsigned long)(meta.rec_ms / 1000), (unsigned long)(meta.rec_ms % 1000 / 10),
             (unsigned long)(audio_ms / 1000), (unsigned long)(audio_ms % 1000 / 10),
             (unsigned long)rec_len, mu_peak_pct(st.peak, rec_bits),
             mu_clip_permille(&st) / 10, mu_clip_permille(&st) % 10);
    say(TB_INFO, line);
    if (meta.traffic)
    {
        snprintf(line, sizeof(line), "traffic: %lu PINGs, %lu PONGs%s",
                 (unsigned long)meta.pings, (unsigned long)meta.pongs,
                 meta.lost ? ", CONNECTION LOST" : "");
        say(meta.lost || meta.pongs + PING_FLIGHT < meta.pings ? TB_ERROR : TB_INFO, line);
    }
    if (meta.touches)
    {
        snprintf(line, sizeof(line), "%lu taps during the recording", (unsigned long)meta.touches);
        say(TB_INFO, line);
    }
    dirty_meter = dirty_opts = true;
    if (rec_len == 0)
    {
        say(TB_ERROR, "no samples recorded");
        phase = PH_IDLE;
        return;
    }
    start_upload();
}

static void replay(void)
{
    if (rec_len == 0 || phase != PH_IDLE)
        return;
    DC_FlushRange(rec, rec_len);
    soundPlaySample(rec, rec_bits == 8 ? SoundFormat_8Bit : SoundFormat_16Bit, rec_len,
                    rec_rate, 127, 64, false, 0);
    say(TB_INFO, "replaying the last clip");
}

// --------------------------------------------------------------------------
// Upload / transcription
// --------------------------------------------------------------------------

static void start_upload(void)
{
    if (!net_online())
    {
        say(TB_ERROR, "not connected to the receiver: clip not sent (X replays it)");
        phase = PH_IDLE;
        return;
    }
    up_off = up_acked = 0;
    up_beg = up_end = false;
    up_start = frames;
    meta.pace = opt_paced ? PACE_BYTES : 0;
    net_set_tx_pace(meta.pace);
    rej_start = Wifi_GetStats(WSTAT_TXQUEUEDREJECTED);
    phase = PH_UPLOAD;
    dirty_meter = true;
}

static void poll_upload(void)
{
    if (!up_beg)
    {
        char m[256];
        mu_meta_format(m, sizeof(m), &meta);
        if (!net_request(TAG_BEG, "MBEG", m, strlen(m)))
            return;
        up_beg = true;
        up_start = frames;
    }
    size_t n;
    while ((n = mu_chunk(rec_len, up_off, CHUNK)) > 0)
    {
        if (!net_request(TAG_DAT, "MDAT", (const char *)rec + up_off, n))
            return;                 // TX buffer or queue full: next frame
        up_off += n;
    }
    if (!up_end && net_request(TAG_END, "MEND", NULL, 0))
        up_end = true;
}

static void abort_upload(const char *why)
{
    char line[96];
    snprintf(line, sizeof(line), "upload failed: %.70s", why);
    say(TB_ERROR, line);
    phase = PH_IDLE;
    dirty_meter = true;
}

void app_net_frame(int tag, const char *type, const char *payload, int len)
{
    (void)len;
    char line[128];
    switch (tag)
    {
        case TAG_PING:
            if (pings_out > 0)
                pings_out--;
            if (phase == PH_REC)
                meta.pongs++;
            dirty_net = true;
            return;
        case TAG_BEG:
            if (strcmp(type, "OKAY") != 0)
                abort_upload(payload);
            return;
        case TAG_DAT:
            if (strcmp(type, "OKAY") != 0)
            {
                abort_upload(payload);
                return;
            }
            up_acked = (uint32_t)strtoul(payload, NULL, 10);
            dirty_meter = true;
            return;
        case TAG_END:
            if (strcmp(type, "OKAY") != 0)
            {
                abort_upload(payload);
                return;
            }
            up_ms = (frames - up_start) * 1000 / 60;
            {
                uint32_t k = mu_kbps_x10(rec_len, up_ms);
                unsigned long lap_ms = 0, lap_bytes = 0;
                sscanf(payload, "%lu %lu", &lap_bytes, &lap_ms);
                uint32_t kl = mu_kbps_x10((uint32_t)lap_bytes, (uint32_t)lap_ms);
                snprintf(line, sizeof(line), "upload %lu.%02lus %lu.%lu KB/s (laptop: %lu.%lu)",
                         (unsigned long)(up_ms / 1000), (unsigned long)(up_ms % 1000 / 10),
                         (unsigned long)(k / 10), (unsigned long)(k % 10),
                         (unsigned long)(kl / 10), (unsigned long)(kl % 10));
                say(TB_INFO, line);
            }
            {
                // DSWiFi's TX ring refusals during this upload (Part A of the
                // stall analysis): sent to the receiver for its log/JSON.
                char sta[64];
                uint32_t rej = Wifi_GetStats(WSTAT_TXQUEUEDREJECTED) - rej_start;
                snprintf(sta, sizeof(sta), "rej=%lu pace=%d up_ms=%lu", (unsigned long)rej,
                         meta.pace, (unsigned long)up_ms);
                net_request(TAG_STA, "MSTA", sta, strlen(sta));
                snprintf(line, sizeof(line), "TX ring refusals: %lu  (%s)", (unsigned long)rej,
                         meta.pace ? "paced" : "burst");
                say(rej ? TB_ERROR : TB_INFO, line);
            }
            if (!net_request(TAG_TX, "SEND", "transcribe", 10))
            {
                abort_upload("could not ask for the transcript");
                return;
            }
            tx_start = frames;
            phase = PH_WAIT;
            up_acked = rec_len;
            dirty_meter = true;
            return;
        case TAG_STA:
            return;
        case TAG_TX:
            if (strcmp(type, "TEXT") == 0)
            {
                say(TB_AI, payload);
                return;
            }
            {
                uint32_t ms = (frames - tx_start) * 1000 / 60;
                if (strcmp(type, "DONE") == 0)
                    snprintf(line, sizeof(line), "(all transcripts after %lu.%lus)",
                             (unsigned long)(ms / 1000), (unsigned long)(ms % 1000 / 100));
                else
                    snprintf(line, sizeof(line), "transcription failed: %.80s", payload);
                say(strcmp(type, "DONE") == 0 ? TB_INFO : TB_ERROR, line);
                tb_blank(&tb);
            }
            phase = PH_IDLE;
            dirty_meter = true;
            return;
    }
}

void app_net_online(const proto_server_info *si, const char *profile, const char *model)
{
    char line[96];
    snprintf(line, sizeof(line), "connected: %s %s (v%d)", profile, model, si->version);
    say(TB_INFO, line);
    dirty_net = true;
}

void app_net_lost(const char *reason)
{
    char line[96];
    snprintf(line, sizeof(line), "connection lost: %.70s", reason);
    say(TB_ERROR, line);
    pings_out = 0;
    if (phase == PH_REC)
        meta.lost = true;
    if (phase == PH_UPLOAD || phase == PH_WAIT)
        abort_upload("connection lost");
    dirty_net = true;
}

void app_net_notice(const char *text)
{
    say(TB_INFO, text);
}

void app_net_test(int stage, bool ok, const char *msg)
{
    (void)stage; (void)ok; (void)msg;
}

// --------------------------------------------------------------------------
// Drawing
// --------------------------------------------------------------------------

#define OPT_Y0   14
#define OPT_H    16
#define METER_Y  112
#define NET_Y    160

static void draw_top(void)
{
    u16 *fb = gfx_top();
    char bar[40];
    static const char *const st[] = { "off", "scan", "joining", "connect", "hello",
                                      "online", "retry", "radio", "idle" };
    int s = net_state();
    snprintf(bar, sizeof(bar), "mictest  rx %s:%d  %s", mic_cfg.relay_host[0] ? mic_cfg.relay_host : "gw",
             MIC_PORT, s >= 0 && s <= NS_IDLE ? st[s] : "?");
    gfx_text_cols(fb, 0, 0, bar, 32, COL_HDR_TEXT, COL_HDR_BG);
    uint32_t end = tb_end_line(&tb);
    if (follow)
        view_top = end > 23 ? end - 23 : tb_first(&tb);
    for (int row = 0; row < 23; row++)
    {
        uint32_t n = view_top + row;
        const tb_line *l = n < end ? tb_get(&tb, n) : NULL;
        u16 col = COL_TEXT;
        if (l)
            col = l->kind == TB_USER ? COL_USER : l->kind == TB_INFO ? COL_INFO
                : l->kind == TB_ERROR ? COL_ERROR : COL_TEXT;
        char buf[33];
        int len = l ? l->len : 0;
        memcpy(buf, l ? l->text : "", len);
        buf[len] = '\0';
        gfx_text_cols(fb, 0, (row + 1) * FONT_H, buf, 32, col, COL_BG);
    }
}

static void opt_text(int i, const char **label, char *val, size_t n)
{
    static const char *const labels[OPT_COUNT] = { "Format", "Rate", "Gain", "Traffic", "Upload", "Replay" };
    *label = labels[i];
    switch (i)
    {
        case OPT_FMT:     snprintf(val, n, "%s", opt_12bit ? "12-bit" : "8-bit"); break;
        case OPT_RATE:    snprintf(val, n, "%d Hz", rates[opt_rate]); break;
        case OPT_GAIN:    snprintf(val, n, "x%d%s", mu_gain_mult(opt_gain), opt_gain == 3 ? " (default)" : ""); break;
        case OPT_TRAFFIC: snprintf(val, n, "%s", opt_traffic ? "PINGs while recording" : "off"); break;
        case OPT_PACE:    snprintf(val, n, "%s", opt_paced ? "paced (1 segment/frame)" : "burst (old, 1.0)"); break;
        case OPT_REPLAY:  snprintf(val, n, "%s", rec_len ? "X: play last clip" : "(no clip yet)"); break;
    }
}

static void draw_opts(void)
{
    u16 *fb = gfx_bottom();
    gfx_fill(fb, 0, 0, SCREEN_W, 11, COL_HDR_BG);
    gfx_text(fb, 0, 1, "mictest  hold Y to record", 32, COL_HDR_TEXT, COL_HDR_BG);
    gfx_fill(fb, 0, 11, SCREEN_W, OPT_Y0 - 11, COL_BG);
    for (int i = 0; i < OPT_COUNT; i++)
    {
        int y = OPT_Y0 + i * OPT_H;
        u16 bg = i == opt_sel ? COL_SEL : COL_KEY;
        gfx_fill(fb, 0, y, SCREEN_W, OPT_H - 2, bg);
        gfx_fill(fb, 0, y + OPT_H - 2, SCREEN_W, 2, COL_BG);
        const char *label;
        char val[32];
        opt_text(i, &label, val, sizeof(val));
        gfx_text(fb, 4, y + 4, label, 8, COL_INFO, bg);
        gfx_text_cols(fb, 4 + 9 * FONT_W, y + 4, val, 22, COL_TEXT, bg);
    }
    gfx_fill(fb, 0, OPT_Y0 + OPT_COUNT * OPT_H, SCREEN_W, METER_Y - (OPT_Y0 + OPT_COUNT * OPT_H), COL_BG);
}

static void draw_meter(void)
{
    u16 *fb = gfx_bottom();
    char line[40];
    gfx_fill(fb, 0, METER_Y, SCREEN_W, NET_Y - METER_Y, COL_BG);
    if (phase == PH_REC)
    {
        uint32_t ms = (frames - rec_start) * 1000 / 60;
        snprintf(line, sizeof(line), "REC %lu.%lu / %d.0 s", (unsigned long)(ms / 1000),
                 (unsigned long)(ms % 1000 / 100), MAX_SEC);
        gfx_text_cols(fb, 0, METER_Y, line, 32, COL_ERROR, COL_BG);
    }
    else if (phase == PH_UPLOAD || phase == PH_WAIT)
    {
        uint32_t pct = rec_len ? (uint64_t)up_acked * 100 / rec_len : 0;
        snprintf(line, sizeof(line), phase == PH_UPLOAD ? "Upload %lu%%  %lu/%lu B"
                                                         : "Transcribing... %lus",
                 phase == PH_UPLOAD ? (unsigned long)pct : (unsigned long)((frames - tx_start) / 60),
                 (unsigned long)up_acked, (unsigned long)rec_len);
        gfx_text_cols(fb, 0, METER_Y, line, 32, COL_INFO, COL_BG);
        gfx_fill(fb, 8, METER_Y + 12, SCREEN_W - 16, 8, COL_KEY);
        gfx_fill(fb, 8, METER_Y + 12, (SCREEN_W - 16) * pct / 100, 8, COL_KEY_SEND);
        return;
    }
    else
        gfx_text_cols(fb, 0, METER_Y, "Level (Y: record, max 10 s)", 32, COL_DIM, COL_BG);

    // level bar: current peak, peak-hold marker, red when clipping
    int bits = phase == PH_REC ? rec_bits : (opt_12bit ? 16 : 8);
    int pct = mu_peak_pct(phase == PH_REC ? lvl_peak : 0, bits);
    if ((uint32_t)pct >= peak_hold || frames - peak_hold_at > 90)
    {
        peak_hold = pct;
        peak_hold_at = frames;
    }
    int w = SCREEN_W - 16;
    gfx_fill(fb, 8, METER_Y + 12, w, 12, COL_KEY);
    gfx_fill(fb, 8, METER_Y + 12, w * pct / 100, 12, pct >= 98 ? COL_ERROR : pct >= 70 ? COL_BAR_WARN : COL_BAR_OK);
    gfx_fill(fb, 8 + (w - 2) * (int)peak_hold / 100, METER_Y + 10, 2, 16, COL_CURSOR);
    // guide marks at 25 / 50 / 75 %
    for (int m = 1; m < 4; m++)
        gfx_fill(fb, 8 + w * m / 4, METER_Y + 26, 1, 3, COL_DIM);
    mu_stats st = rec_stats;
    snprintf(line, sizeof(line), "peak %3d%% hold %3lu%% clip %d.%d%%", pct,
             (unsigned long)peak_hold, mu_clip_permille(&st) / 10, mu_clip_permille(&st) % 10);
    gfx_text_cols(fb, 0, METER_Y + 32, line, 32, peak_hold >= 98 ? COL_ERROR : COL_TEXT, COL_BG);
}

static void draw_net(void)
{
    u16 *fb = gfx_bottom();
    char line[40];
    net_info_t ni;
    net_get_info(&ni);
    uint32_t q, t, r;
    net_wifi_counters(&q, &t, &r);
    snprintf(line, sizeof(line), "%s %s", net_online() ? "online" : "offline", ni.ip);
    gfx_text_cols(fb, 0, NET_Y, line, 32, net_online() ? COL_BAR_OK : COL_ERROR, COL_BG);
    snprintf(line, sizeof(line), "wifi q%lu t%lu r%lu", (unsigned long)q, (unsigned long)t,
             (unsigned long)r);
    gfx_text_cols(fb, 0, NET_Y + 9, line, 32, COL_DIM, COL_BG);
    snprintf(line, sizeof(line), "pings %lu/%lu  taps %lu", (unsigned long)meta.pongs,
             (unsigned long)meta.pings, (unsigned long)meta.touches);
    gfx_text_cols(fb, 0, NET_Y + 18, line, 32, COL_DIM, COL_BG);
    gfx_text_cols(fb, 0, NET_Y + 27, "A:change X:replay L/R:scroll", 32, COL_DIM, COL_BG);
}

// --------------------------------------------------------------------------
// Input
// --------------------------------------------------------------------------

static void change_opt(int i, int dir)
{
    if (phase != PH_IDLE)
        return;
    switch (i)
    {
        case OPT_FMT:     opt_12bit = !opt_12bit; break;
        case OPT_RATE:    opt_rate ^= 1; break;
        case OPT_GAIN:    opt_gain = (opt_gain + (dir < 0 ? 3 : 1)) % 4; break;
        case OPT_TRAFFIC: opt_traffic = !opt_traffic; break;
        case OPT_PACE:    opt_paced = !opt_paced; break;
        case OPT_REPLAY:  replay(); break;
    }
    dirty_opts = true;
}

static void input(void)
{
    scanKeys();
    uint32_t down = keysDown(), up = keysUp();

    if (phase == PH_REC)
    {
        if (down & KEY_TOUCH)
        {
            if (meta.touches < 8)
                meta.touch_ms[meta.touches] = (frames - rec_start) * 1000 / 60;
            meta.touches++;
            dirty_net = true;
        }
        if ((up & KEY_Y) || rec_len >= rec_cap)
        {
            if (rec_len >= rec_cap)
                say(TB_INFO, "(10 s limit reached)");
            rec_stop_now();
        }
        return;
    }
    if ((down & KEY_Y) && phase == PH_IDLE)
    {
        rec_start_now();
        return;
    }
    if ((down & KEY_B) && phase != PH_IDLE)
    {
        say(TB_ERROR, "abandoned; reconnecting");
        phase = PH_IDLE;
        net_reconnect();
        dirty_meter = true;
    }
    if (down & KEY_START)
        net_reconnect();
    if (down & KEY_X)
        replay();
    if (down & KEY_UP)
        opt_sel = (opt_sel + OPT_COUNT - 1) % OPT_COUNT, dirty_opts = true;
    if (down & KEY_DOWN)
        opt_sel = (opt_sel + 1) % OPT_COUNT, dirty_opts = true;
    if (down & (KEY_A | KEY_RIGHT))
        change_opt(opt_sel, 1);
    if (down & KEY_LEFT)
        change_opt(opt_sel, -1);
    if (down & (KEY_L | KEY_R))
    {
        uint32_t end = tb_end_line(&tb), first = tb_first(&tb);
        uint32_t last_top = end > 23 ? end - 23 : first;
        if (down & KEY_L)
            view_top = view_top > first + 20 ? view_top - 20 : first;
        else
            view_top = view_top + 20 < last_top ? view_top + 20 : last_top;
        follow = view_top >= last_top;
        dirty_top = true;
    }
    if (down & KEY_TOUCH)
    {
        touchPosition tp;
        touchRead(&tp);
        for (int i = 0; i < OPT_COUNT; i++)
            if (tp.py >= OPT_Y0 + i * OPT_H && tp.py < OPT_Y0 + (i + 1) * OPT_H)
            {
                opt_sel = i;
                change_opt(i, 1);
            }
    }
}

// --------------------------------------------------------------------------

static void vblank(void)
{
    frames++;
}

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;
    irqSet(IRQ_VBLANK, vblank);
    irqEnable(IRQ_VBLANK);
    gfx_init();
    soundEnable();
    tb_init(&tb);
    say(TB_USER, "mictest - DS microphone experiment");
    say(TB_INFO, "Hold Y to record, release to send. Results appear here.");

    storage_init(&store);
    storage_load(&store, &cfg);
    say(TB_INFO, store.msg);
    mic_cfg = cfg;
    mic_cfg.relay_port = MIC_PORT;
    char line[96];
    snprintf(line, sizeof(line), "Wi-Fi \"%.32s\" (%s), receiver %s:%d", cfg.ssid,
             cfg.security == SEC_WEP ? "WEP" : "open",
             cfg.relay_host[0] ? cfg.relay_host : "gateway", MIC_PORT);
    say(TB_INFO, line);
    char why[48];
    if (!net_init(&mic_cfg))
        say(TB_ERROR, "Wi-Fi init FAILED");
    else if (!config_validate(&mic_cfg, why, sizeof(why)))
        say(TB_ERROR, why);
    else
        net_start(&mic_cfg, false);

    int last_state = -1;
    while (1)
    {
        cothread_yield_irq(IRQ_VBLANK);
        gfx_present();
        uint32_t now = frames;
        input();
        net_poll(now);
        if (net_state() != last_state)
        {
            last_state = net_state();
            dirty_top = dirty_net = true;
        }
        if (phase == PH_STOPPING && frames - stop_at >= STOP_FRAMES)
            rec_finish();
        if (phase == PH_UPLOAD)
            poll_upload();
        if (phase == PH_REC)
        {
            if (opt_traffic && net_online())
                while (pings_out < PING_FLIGHT && net_request(TAG_PING, "PING", NULL, 0))
                {
                    pings_out++;
                    meta.pings++;
                }
            if (rec_len >= rec_cap)
            {
                say(TB_INFO, "(10 s limit reached)");
                rec_stop_now();
            }
            dirty_meter = true;
            if (now % 10 == 0)
                dirty_net = true;
        }
        else if (now % 30 == 0)
        {
            dirty_net = true;
            if (phase == PH_WAIT)
                dirty_meter = true;     // "Transcribing... Ns" counter
        }
        if (dirty_top)
        {
            draw_top();
            dirty_top = false;
        }
        if (dirty_opts)
        {
            draw_opts();
            dirty_opts = false;
            dirty_meter = true;
        }
        if (dirty_meter)
        {
            draw_meter();
            dirty_meter = false;
        }
        if (dirty_net)
        {
            draw_net();
            dirty_net = false;
        }
    }
}
