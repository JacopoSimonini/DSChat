// Voice input (v1.1, protocol v4): hold Y to talk, release to transcribe.
//
// The microphone records 12-bit at 16 kHz (libnds soundMicRecord; the stock
// ARM7 turns the amplifier on at x160 for each recording and off again on
// soundMicOff). The first 0.3 s is thrown away: the amplifier's power-up
// burst (full-scale spikes every 2.6 ms for ~250 ms, measured with mictest).
// Audio is IMA ADPCM encoded in 1025-sample blocks (adpcm.c, 8 KB/s) and
// streamed to the relay while the user is still speaking:
//   VBEG codec=ima-adpcm rate=16000 block=1025 lang=en   (no reply)
//   VDAT <1-2 whole blocks>                              (no reply)
//   VEND                                                 -> VTXT / FAIL
// B cancels (CNCL). The transcript goes to the app (app_voice_*), never sent.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define VOICE_RATE        16000
#define VOICE_MAX_MS      20000      // speech limit after the warm-up
#define VOICE_WARMUP_MS   300

enum voice_phase
{
    V_OFF = 0,
    V_WARMUP,          // recording, first 0.3 s discarded ("Wait")
    V_REC,             // recording and streaming ("Speak")
    V_FLUSH,           // Y released: last samples, last chunks, VEND
    V_WAIT,            // VEND sent, waiting for VTXT
};

typedef struct
{
    bool valid;
    uint32_t rec_ms;       // speech recorded (after the warm-up)
    uint32_t bytes;        // ADPCM bytes sent
    uint32_t send_ms;      // first VDAT handed to TCP .. last byte handed to TCP
    uint32_t tail_ms;      // Y released .. last byte handed to TCP
    uint32_t total_ms;     // Y released .. transcript shown
    uint32_t stt_ms;       // transcription time on the laptop (from VTXT)
    uint32_t rx_ms;        // first .. last VDAT at the relay (from VTXT)
} voice_stats;

void voice_init(void);
// Y pressed. lang: "en" / "it". False (and no recording) if it can't start.
bool voice_start(uint32_t now, const char *lang);
void voice_release(uint32_t now);           // Y released
void voice_cancel(void);                    // B
void voice_poll(uint32_t now);              // every frame
void voice_lost(void);                      // the connection dropped
// The VEND reply (VTXT or FAIL); tag-matched by the app.
void voice_on_reply(uint32_t now, const char *type, const char *payload);

int voice_phase(void);
bool voice_active(void);
int voice_level_pct(void);                  // last block's peak, 0..100
uint32_t voice_speech_ms(uint32_t now);     // speech time so far (V_REC)
const voice_stats *voice_last_stats(void);

// Implemented by the app.
void app_voice_text(const char *text);      // transcript ("" = nothing heard)
void app_voice_fail(const char *why);       // error shown to the user
#define TAG_VOICE 99                        // net_request tag of VEND
