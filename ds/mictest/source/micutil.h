// mictest: pure helpers (no libnds), unit-tested natively in ../tests.
// Sample formats as delivered by libnds soundMicRecord() on a DS:
//   8-bit : signed int8 (the ARM7 flips the top bit of the unsigned ADC value)
//   12-bit: signed int16, the 12-bit value shifted up 4 bits (low nibble 0)
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MU_CLIP8   127      // |s8| at or above = clipped (ADC end of range)
#define MU_CLIP16  32752    // |s16| at or above = clipped (0x7FF0 = 12-bit max << 4)

typedef struct
{
    uint32_t peak;          // largest |sample| seen (same unit as the samples)
    uint32_t clipped;       // samples at the ends of the range
    uint32_t count;         // samples looked at
} mu_stats;

// Add the samples of a buffer to the stats. bits = 8 or 16.
void mu_stats_add(mu_stats *st, const void *data, size_t bytes, int bits);
// Peak as percent of full scale (0..100).
int mu_peak_pct(uint32_t peak, int bits);
// Clipped samples in tenths of a percent (0..1000).
int mu_clip_permille(const mu_stats *st);

typedef struct
{
    int bits;               // 8 or 16 (12-bit recordings are sent as 16-bit)
    int rate;               // Hz
    int gain;               // PM_GAIN_* code 0..3 (x20, x40, x80, x160)
    uint32_t bytes;
    uint32_t rec_ms;        // recording length (frames * 1000 / 60)
    bool traffic;           // PINGs sent while recording
    uint32_t pings, pongs;  // during the recording
    bool lost;              // connection lost during the recording
    uint32_t touches;       // stylus taps during the recording
    uint32_t touch_ms[8];   // when (ms since the start), first 8 taps
    uint32_t clip_no;       // clip number since power on
    int pace;               // upload pacing in bytes per frame (0 = burst, the old way)
} mu_meta;

// "key=value ..." line sent in the MBEG frame (ASCII, < 256 chars).
size_t mu_meta_format(char *out, size_t cap, const mu_meta *m);
// Gain code -> multiplier (20, 40, 80, 160); -1 if invalid.
int mu_gain_mult(int code);
// Next upload chunk length (0 when done).
size_t mu_chunk(size_t total, size_t off, size_t max);
// KB/s with one decimal, as tenths (1 KB = 1024 bytes). 0 if ms == 0.
uint32_t mu_kbps_x10(uint32_t bytes, uint32_t ms);
