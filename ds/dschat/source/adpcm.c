#include "adpcm.h"

// The standard IMA/DVI ADPCM tables.
static const int16_t step_table[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767,
};
static const int8_t index_table[16] = {
    -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8,
};

static int clamp16(int v)
{
    return v > 32767 ? 32767 : v < -32768 ? -32768 : v;
}

// One step of the decoder: shared by the encoder (it tracks what the
// decoder will reconstruct) and the decoder, so both stay in lock step.
static void step(adpcm_state *st, int code)
{
    int s = step_table[st->index];
    int diff = s >> 3;
    if (code & 4) diff += s;
    if (code & 2) diff += s >> 1;
    if (code & 1) diff += s >> 2;
    st->predictor = clamp16(code & 8 ? st->predictor - diff : st->predictor + diff);
    st->index += index_table[code];
    st->index = st->index < 0 ? 0 : st->index > 88 ? 88 : st->index;
}

static int encode_sample(adpcm_state *st, int sample)
{
    int s = step_table[st->index];
    int diff = sample - st->predictor;
    int code = 0;
    if (diff < 0)
    {
        code = 8;
        diff = -diff;
    }
    if (diff >= s) { code |= 4; diff -= s; }
    s >>= 1;
    if (diff >= s) { code |= 2; diff -= s; }
    s >>= 1;
    if (diff >= s) code |= 1;
    step(st, code);
    return code;
}

size_t adpcm_encode_block(adpcm_state *st, const int16_t *pcm, size_t n, uint8_t *out)
{
    if (n == 0)
        return 0;
    if (st->index < 0 || st->index > 88)
        st->index = 0;
    st->predictor = pcm[0];
    out[0] = (uint8_t)(pcm[0] & 0xFF);
    out[1] = (uint8_t)((uint16_t)pcm[0] >> 8);
    out[2] = (uint8_t)st->index;
    out[3] = 0;
    uint8_t *p = out + 4;
    for (size_t i = 1; i < n; i++)
    {
        int code = encode_sample(st, pcm[i]);
        if ((i - 1) % 2 == 0)
            *p = (uint8_t)code;
        else
            *p++ |= (uint8_t)(code << 4);
    }
    if ((n - 1) % 2 == 1)
        p++;                       // odd count of codes: the high nibble stays 0
    return (size_t)(p - out);
}

size_t adpcm_decode_block(const uint8_t *in, size_t bytes, int16_t *pcm, size_t max_samples)
{
    if (bytes < 4 || max_samples == 0)
        return 0;
    adpcm_state st;
    st.predictor = (int16_t)(in[0] | (in[1] << 8));
    st.index = in[2] > 88 ? 88 : in[2];
    size_t n = 0;
    pcm[n++] = (int16_t)st.predictor;
    for (size_t b = 4; b < bytes && n < max_samples; b++)
    {
        step(&st, in[b] & 15);
        pcm[n++] = (int16_t)st.predictor;
        if (n >= max_samples)
            break;
        step(&st, in[b] >> 4);
        pcm[n++] = (int16_t)st.predictor;
    }
    return n;
}
