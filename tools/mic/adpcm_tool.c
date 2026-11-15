// Offline ADPCM round trip with the DS app's own encoder (ds/dschat/source/adpcm.c):
// raw s16le PCM on stdin -> encode in 1025-sample blocks -> decode -> raw s16le on stdout.
// Prints the encoded size on stderr. Build: cc -O2 -I../../ds/dschat/source adpcm_tool.c ../../ds/dschat/source/adpcm.c
#include <stdio.h>
#include <stdlib.h>
#include "adpcm.h"
#define BLOCK 1025
int main(void)
{
    size_t cap = 1 << 20, n = 0;
    int16_t *pcm = malloc(cap * 2);
    size_t r;
    while ((r = fread(pcm + n, 2, cap - n, stdin)) > 0)
        if ((n += r) == cap)
            pcm = realloc(pcm, (cap *= 2) * 2);
    uint8_t enc[adpcm_block_bytes(BLOCK)];
    int16_t dec[BLOCK + 1];
    adpcm_state st = { 0, 0 };
    size_t total = 0;
    for (size_t off = 0; off < n; off += BLOCK)
    {
        size_t k = n - off < BLOCK ? n - off : BLOCK;
        size_t b = adpcm_encode_block(&st, pcm + off, k, enc);
        total += b;
        size_t m = adpcm_decode_block(enc, b, dec, k);
        fwrite(dec, 2, m, stdout);
    }
    fprintf(stderr, "%zu samples -> %zu bytes\n", n, total);
    return 0;
}
