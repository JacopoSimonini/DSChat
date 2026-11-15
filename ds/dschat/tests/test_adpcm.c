// IMA ADPCM encoder: round trip against an independent reference decoder
// written here from the IMA/DVI spec (not adpcm_decode_block), on synthetic
// audio only.
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "adpcm.h"
#include "check.h"

static const int ref_steps[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60,
    66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371,
    408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707,
    1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484,
    7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385,
    24623, 27086, 29794, 32767,
};

// Reference decoder: the textbook formula, diff = (2*|code| + 1) * step / 8,
// with the same integer truncation the IMA spec uses (shift-and-add).
static size_t ref_decode(const uint8_t *in, size_t bytes, int16_t *out, size_t ncodes)
{
    int pred = (int16_t)(in[0] | in[1] << 8), idx = in[2];
    static const int adj[8] = { -1, -1, -1, -1, 2, 4, 6, 8 };
    size_t n = 0;
    out[n++] = (int16_t)pred;
    for (size_t k = 0; k < ncodes && 4 + k / 2 < bytes; k++)
    {
        int code = (k % 2 == 0) ? (in[4 + k / 2] & 15) : (in[4 + k / 2] >> 4);
        int st = ref_steps[idx];
        int mag = code & 7;
        int diff = st >> 3;
        for (int bit = 2; bit >= 0; bit--)
            if (mag & (1 << bit))
                diff += st >> (2 - bit);
        pred += (code & 8) ? -diff : diff;
        pred = pred > 32767 ? 32767 : pred < -32768 ? -32768 : pred;
        idx += adj[mag];
        idx = idx < 0 ? 0 : idx > 88 ? 88 : idx;
        out[n++] = (int16_t)pred;
    }
    return n;
}

static double snr_db(const int16_t *a, const int16_t *b, size_t n)
{
    double sig = 0, err = 0;
    for (size_t i = 0; i < n; i++)
    {
        sig += (double)a[i] * a[i];
        err += ((double)a[i] - b[i]) * ((double)a[i] - b[i]);
    }
    return err == 0 ? 200 : 10 * log10(sig / err);
}

static void roundtrip(const int16_t *pcm, size_t total, size_t block, double min_snr, const char *what)
{
    uint8_t *enc = malloc(adpcm_block_bytes(block) + 4);
    int16_t *dec = malloc(total * sizeof(int16_t)), *ref = malloc((block + 2) * sizeof(int16_t));
    int16_t *mine = malloc((block + 2) * sizeof(int16_t));
    adpcm_state st = { 0, 0 };
    size_t off = 0;
    bool same = true;
    while (off < total)
    {
        size_t n = total - off < block ? total - off : block;
        size_t bytes = adpcm_encode_block(&st, pcm + off, n, enc);
        CHECK(bytes == adpcm_block_bytes(n));
        size_t got = ref_decode(enc, bytes, ref, n - 1);
        CHECK(got == n);
        // the module's own decoder agrees bit for bit with the reference
        size_t got2 = adpcm_decode_block(enc, bytes, mine, n);
        CHECK(got2 == n);
        for (size_t i = 0; i < n; i++)
            same = same && mine[i] == ref[i];
        // the encoder tracked exactly what the decoder rebuilds
        CHECK(st.predictor == ref[n - 1]);
        for (size_t i = 0; i < n; i++)
            dec[off + i] = ref[i];
        off += n;
    }
    CHECK(same);
    double s = snr_db(pcm, dec, total);
    if (s < min_snr)
        fprintf(stderr, "%s: SNR %.1f dB < %.1f\n", what, s, min_snr);
    CHECK(s >= min_snr);
    free(enc); free(dec); free(ref); free(mine);
}

void test_adpcm(void)
{
    enum { N = 16000 };
    static int16_t pcm[N];

    // header layout: predictor LE, index, 0; block size 4 + (n-1 codes)/2 rounded up
    {
        int16_t s[3] = { -2, 100, 300 };
        uint8_t out[8];
        adpcm_state st = { 0, 5 };
        CHECK(adpcm_encode_block(&st, s, 3, out) == 5);
        CHECK(out[0] == 0xFE && out[1] == 0xFF && out[2] == 5 && out[3] == 0);
        CHECK(adpcm_block_bytes(1025) == 516 && adpcm_block_bytes(1) == 4 && adpcm_block_bytes(0) == 0);
        int16_t one[1] = { 1234 };
        CHECK(adpcm_encode_block(&st, one, 1, out) == 4);
        int16_t d[4];
        CHECK(adpcm_decode_block(out, 4, d, 4) == 1 && d[0] == 1234);
    }

    // speech-like: 300 Hz + 1.2 kHz tones with a slow envelope, 16 kHz
    for (int i = 0; i < N; i++)
    {
        double env = 0.5 + 0.5 * sin(2 * M_PI * 3 * i / 16000.0);
        pcm[i] = (int16_t)(env * (9000 * sin(2 * M_PI * 300 * i / 16000.0)
                                  + 4000 * sin(2 * M_PI * 1200 * i / 16000.0)));
    }
    roundtrip(pcm, N, 1025, 20, "tones 1025");
    roundtrip(pcm, N, 1024, 20, "tones even block");
    roundtrip(pcm, 1, 1025, 100, "single sample");

    // pseudo-random noise (worst case for ADPCM), full scale extremes and clamping
    srand(1);
    for (int i = 0; i < N; i++)
        pcm[i] = (int16_t)((rand() % 20001) - 10000);
    roundtrip(pcm, N, 1025, 3, "noise");
    for (int i = 0; i < N; i++)
        pcm[i] = (i / 40) % 2 ? 32767 : -32768;            // square wave at the rails
    roundtrip(pcm, N, 1025, 5, "square at rails");      // worst case: ~6 dB
    for (int i = 0; i < N; i++)
        pcm[i] = 0;
    roundtrip(pcm, N, 1025, 100, "silence");                 // exact

    // the 12-bit mic format: low nibble always 0
    for (int i = 0; i < N; i++)
        pcm[i] = (int16_t)((int)(12000 * sin(2 * M_PI * 440 * i / 16000.0)) & ~15);
    roundtrip(pcm, N, 1025, 20, "12-bit sine");
}
