// IMA ADPCM encoder (4 bits per sample, 4:1 vs 16-bit PCM) for voice input.
// Pure C, unit-tested natively against an independent reference decoder.
//
// Block format (one VDAT frame = one or more whole blocks; docs/PROTOCOL.md):
//   bytes 0-1  predictor: the first sample, int16 little-endian
//   byte  2    step index 0..88
//   byte  3    0 (reserved)
//   then (n - 1) samples as 4-bit codes, two per byte, LOW nibble first
//   (the first sample is the header's predictor itself)
// so a block of n samples takes 4 + n/2 bytes (n odd: the last nibble is 0).
// Every block restarts the decoder, so a lost frame never corrupts the next.
#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct
{
    int predictor;          // last decoded value, -32768..32767
    int index;              // step index, 0..88
} adpcm_state;

// Bytes needed for a block of n samples.
static inline size_t adpcm_block_bytes(size_t n)
{
    return n == 0 ? 0 : 4 + n / 2;
}

// Encodes n >= 1 samples into one self-contained block. *st carries the step
// index from block to block (better start); the predictor restarts at the
// block's first sample. Returns the bytes written (adpcm_block_bytes(n)).
size_t adpcm_encode_block(adpcm_state *st, const int16_t *pcm, size_t n, uint8_t *out);

// Decodes one block (for tests and tools). Returns the samples written.
size_t adpcm_decode_block(const uint8_t *in, size_t bytes, int16_t *pcm, size_t max_samples);
