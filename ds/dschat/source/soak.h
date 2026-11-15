// Wi-Fi soak test (menu): sends protocol PINGs to the relay in a loop (no AI
// calls) and watches the DSWiFi packet counters, to find out whether the
// radio stalls on real hardware as it does in melonDS (see docs/TECHNICAL-NOTES.md).
//
// Passes when the hardware TX counter has grown by SOAK_TARGET packets AND
// SOAK_MIN_TIME has elapsed (in melonDS the stall turned out to be time-based,
// ~70 s after the ROM starts, not packet-based; 3000 packets alone take only
// ~30 s). Stops early at the first stall (PINGs outstanding and no PONG for
// SOAK_STALL), when the connection is lost, or when the user presses B.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SOAK_TARGET    3000        // hardware TX packets
#define SOAK_MIN_TIME  (180 * 60)  // frames: run at least 3 minutes
#define SOAK_INFLIGHT  4           // PINGs kept in flight
#define SOAK_STALL     (5 * 60)    // frames without a PONG = stall

enum soak_result { SOAK_RUNNING = 0, SOAK_PASS, SOAK_STALLED, SOAK_LOST, SOAK_STOPPED };

typedef struct
{
    bool active;                   // started at least once (page has data)
    int result;                    // SOAK_RUNNING while it runs
    uint32_t start, end;           // frames
    uint32_t sent, recv, fails;    // PINGs sent, PONGs received, other answers
    int inflight;
    uint32_t last_pong;            // frame of the last PONG (or the start)
    uint32_t q0, t0, r0;           // DSWiFi counters at the start
    uint32_t q, t, r;              // latest
    char reason[40];               // for SOAK_LOST
} soak_t;

extern soak_t soak;

bool soak_running(void);
void soak_start(uint32_t now);
void soak_poll(uint32_t now);                  // every frame
void soak_on_frame(const char *type, uint32_t now);
void soak_on_lost(const char *reason, uint32_t now);
void soak_stop(uint32_t now);                  // B pressed
// One-line summary for the conversation, e.g. "PASS: ..." / "STALL: ...".
void soak_summary(char *out, int size);
