#include <stdio.h>
#include <string.h>

#include <nds.h>

#include "adpcm.h"
#include "net.h"
#include "proto.h"
#include "voice.h"

#define BLOCK        1025                       // samples per ADPCM block
#define BLOCK_BYTES  516                        // adpcm_block_bytes(BLOCK)
#define RING         8192                       // PCM ring (0.5 s), power of 2
#define DBUF_BYTES   2048                       // ARM7 double buffer: 2 x 512 samples
#define WARMUP       (VOICE_RATE * VOICE_WARMUP_MS / 1000)
#define MAX_SAMPLES  (VOICE_RATE / 1000 * VOICE_MAX_MS)
#define ENC_CAP      ((MAX_SAMPLES / BLOCK + 2) * BLOCK_BYTES)
#define STOP_FRAMES  6                          // last partial buffer after soundMicOff
#define FPS          60

static int16_t dbuf[DBUF_BYTES / 2] __attribute__((aligned(32)));
static int16_t ring[RING];
static volatile uint32_t ring_head;             // written by the mic IRQ
static uint32_t ring_tail;
static volatile uint32_t ring_lost;             // samples dropped (ring full)

static uint8_t enc[ENC_CAP];
static uint32_t enc_len, enc_sent, enc_full_blocks_end;
static adpcm_state enc_state;
static int16_t blockbuf[BLOCK];
static uint32_t block_fill;

static int phase;
static uint32_t skipped, speech_samples;
static uint32_t release_at, stop_at, first_send_at, flushed_at;
static bool vbeg_sent, vend_sent, mic_on;
static int ignore_replies;             // VEND answers still due for cancelled recordings
static char vbeg[64];
static int level;
static voice_stats stats, cur;

static void mic_cb(void *completed, int length)
{
    // IRQ context: copy into the ring (the ARM7 wrote main RAM: read uncached)
    const int16_t *s = memUncached(completed);
    int n = length / 2;
    for (int i = 0; i < n; i++)
    {
        if (ring_head - ring_tail >= RING)
        {
            ring_lost++;
            continue;
        }
        ring[ring_head & (RING - 1)] = s[i];
        ring_head++;
    }
}

void voice_init(void)
{
    phase = V_OFF;
}

int voice_phase(void) { return phase; }
bool voice_active(void) { return phase != V_OFF; }
int voice_level_pct(void) { return level; }
const voice_stats *voice_last_stats(void) { return &stats; }

uint32_t voice_speech_ms(uint32_t now)
{
    (void)now;
    return speech_samples * 1000 / VOICE_RATE;
}

static void mic_stop(void)
{
    if (mic_on)
        soundMicOff();          // also turns the amplifier off (ARM7 micStopRecording)
    mic_on = false;
}

bool voice_start(uint32_t now, const char *lang)
{
    if (phase != V_OFF)
        return false;
    ring_head = ring_tail = ring_lost = 0;
    enc_len = enc_sent = enc_full_blocks_end = 0;
    enc_state.predictor = 0;
    enc_state.index = 0;
    block_fill = skipped = speech_samples = 0;
    vbeg_sent = vend_sent = false;
    first_send_at = flushed_at = 0;
    level = 0;
    memset(&cur, 0, sizeof(cur));
    snprintf(vbeg, sizeof(vbeg), "codec=ima-adpcm rate=%d block=%d lang=%s",
             VOICE_RATE, BLOCK, lang);
    if (!soundMicRecord(dbuf, sizeof(dbuf), MicFormat_12Bit, VOICE_RATE, mic_cb))
        return false;
    mic_on = true;
    (void)now;
    phase = V_WARMUP;
    return true;
}

static void finish_block(void)
{
    if (block_fill == 0 || enc_len + BLOCK_BYTES > ENC_CAP)
        return;
    enc_len += adpcm_encode_block(&enc_state, blockbuf, block_fill, enc + enc_len);
    if (block_fill == BLOCK)
        enc_full_blocks_end = enc_len;
    block_fill = 0;
}

// Pull samples from the ring: drop the warm-up, meter, encode full blocks.
static void drain_ring(void)
{
    while (ring_tail != ring_head)
    {
        int16_t s = ring[ring_tail & (RING - 1)];
        ring_tail++;
        if (skipped < WARMUP)
        {
            skipped++;
            continue;
        }
        if (phase == V_WARMUP)
            phase = V_REC;
        if (speech_samples >= MAX_SAMPLES)
            continue;
        speech_samples++;
        blockbuf[block_fill++] = s;
        int a = s < 0 ? -s : s;
        if (a * 100 / 32768 > level)
            level = a * 100 / 32768;
        if (block_fill == BLOCK)
            finish_block();
    }
}

// Stream whole blocks (two per VDAT at most, one VDAT per frame: paced).
static void stream(uint32_t now, bool final)
{
    if (!vbeg_sent)
    {
        if (!net_send_noreply("VBEG", vbeg, strlen(vbeg)))
            return;
        vbeg_sent = true;
    }
    uint32_t limit = final ? enc_len : enc_full_blocks_end;
    if (enc_sent < limit)
    {
        uint32_t n = limit - enc_sent;
        if (n > 2 * BLOCK_BYTES)
            n = 2 * BLOCK_BYTES;
        // keep whole blocks: a partial last block only goes out on its own
        if (enc_sent + n > enc_full_blocks_end && enc_sent < enc_full_blocks_end)
            n = enc_full_blocks_end - enc_sent;
        if (net_send_noreply("VDAT", enc + enc_sent, n))
        {
            if (first_send_at == 0)
                first_send_at = now;
            enc_sent += n;
        }
    }
}

void voice_poll(uint32_t now)
{
    static uint32_t level_decay;
    if (phase == V_OFF)
        return;
    if (++level_decay % 4 == 0 && level > 0)
        level = level * 3 / 4;
    if (phase == V_WARMUP || phase == V_REC)
    {
        drain_ring();
        if (phase == V_REC)
            stream(now, false);
        if (speech_samples >= MAX_SAMPLES)
            voice_release(now);
        return;
    }
    if (phase == V_FLUSH)
    {
        if (now - stop_at < STOP_FRAMES)
            return;
        drain_ring();
        if (speech_samples * 1000 / VOICE_RATE < 200)
        {
            // released during the warm-up or almost at once: nothing to send
            if (vbeg_sent)
                net_send_noreply("CNCL", NULL, 0);
            phase = V_OFF;
            app_voice_fail("too short - hold Y while you speak");
            return;
        }
        finish_block();                 // the last, partial block
        if (enc_sent < enc_len)
        {
            stream(now, true);
            return;
        }
        if (!vend_sent)
        {
            if (!net_request(TAG_VOICE, "VEND", NULL, 0))
                return;
            vend_sent = true;
        }
        if (net_tx_pending() == 0)
        {
            flushed_at = now;
            cur.bytes = enc_len;
            cur.rec_ms = speech_samples * 1000 / VOICE_RATE;
            cur.send_ms = first_send_at ? (now - first_send_at) * 1000 / FPS : 0;
            cur.tail_ms = (now - release_at) * 1000 / FPS;
            phase = V_WAIT;
        }
    }
}

void voice_release(uint32_t now)
{
    if (phase != V_WARMUP && phase != V_REC)
        return;
    mic_stop();
    release_at = stop_at = now;
    phase = V_FLUSH;
}

void voice_cancel(void)
{
    if (phase == V_OFF)
        return;
    mic_stop();
    if (phase == V_WAIT || vend_sent)
        net_cancel();                   // the relay answers VEND with FAIL cancelled
    else if (vbeg_sent)
        net_send_noreply("CNCL", NULL, 0);   // the relay drops the stream, no reply
    if (vend_sent)
        ignore_replies++;
    phase = V_OFF;
}

void voice_lost(void)
{
    if (phase == V_OFF)
        return;
    mic_stop();
    phase = V_OFF;
    app_voice_fail("connection lost - voice input dropped");
}

void voice_on_reply(uint32_t now, const char *type, const char *payload)
{
    if (ignore_replies > 0)
    {
        ignore_replies--;               // the answer to a cancelled VEND
        return;
    }
    if (phase != V_WAIT && phase != V_FLUSH)
        return;
    phase = V_OFF;
    if (strcmp(type, "VTXT") != 0)
    {
        app_voice_fail(payload[0] ? payload : "voice input failed");
        return;
    }
    proto_vtxt v;
    proto_parse_vtxt(payload, &v);
    cur.stt_ms = v.stt_ms;
    cur.rx_ms = v.rx_ms;
    cur.total_ms = (now - release_at) * 1000 / FPS;
    cur.valid = true;
    stats = cur;
    app_voice_text(v.text);
}
