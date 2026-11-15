#include <stdio.h>
#include <string.h>

#include "micutil.h"

void mu_stats_add(mu_stats *st, const void *data, size_t bytes, int bits)
{
    if (bits == 8)
    {
        const int8_t *s = data;
        for (size_t i = 0; i < bytes; i++)
        {
            int v = s[i] < 0 ? -s[i] : s[i];
            if ((uint32_t)v > st->peak)
                st->peak = v;
            if (v >= MU_CLIP8)
                st->clipped++;
        }
        st->count += bytes;
    }
    else
    {
        const int16_t *s = data;
        size_t n = bytes / 2;
        for (size_t i = 0; i < n; i++)
        {
            int v = s[i] < 0 ? -(int)s[i] : s[i];
            if ((uint32_t)v > st->peak)
                st->peak = v;
            if (v >= MU_CLIP16)
                st->clipped++;
        }
        st->count += n;
    }
}

int mu_peak_pct(uint32_t peak, int bits)
{
    uint32_t full = bits == 8 ? 128 : 32768;
    uint32_t p = peak * 100 / full;
    return p > 100 ? 100 : (int)p;
}

int mu_clip_permille(const mu_stats *st)
{
    if (st->count == 0)
        return 0;
    return (int)((uint64_t)st->clipped * 1000 / st->count);
}

int mu_gain_mult(int code)
{
    static const int mult[4] = { 20, 40, 80, 160 };
    return code >= 0 && code < 4 ? mult[code] : -1;
}

size_t mu_meta_format(char *out, size_t cap, const mu_meta *m)
{
    int n = snprintf(out, cap,
                     "clip=%lu bits=%d rate=%d gain=%d pace=%d bytes=%lu rec_ms=%lu traffic=%d "
                     "pings=%lu pongs=%lu lost=%d touches=%lu touch_ms=",
                     (unsigned long)m->clip_no, m->bits, m->rate, mu_gain_mult(m->gain), m->pace,
                     (unsigned long)m->bytes, (unsigned long)m->rec_ms, m->traffic ? 1 : 0,
                     (unsigned long)m->pings, (unsigned long)m->pongs, m->lost ? 1 : 0,
                     (unsigned long)m->touches);
    if (n < 0 || (size_t)n >= cap)
        return 0;
    uint32_t k = m->touches < 8 ? m->touches : 8;
    if (k == 0)
        n += snprintf(out + n, cap - n, "-");
    for (uint32_t i = 0; i < k && (size_t)n < cap; i++)
        n += snprintf(out + n, cap - n, i ? ",%lu" : "%lu", (unsigned long)m->touch_ms[i]);
    if ((size_t)n >= cap)
        return 0;
    return (size_t)n;
}

size_t mu_chunk(size_t total, size_t off, size_t max)
{
    if (off >= total)
        return 0;
    size_t left = total - off;
    return left < max ? left : max;
}

uint32_t mu_kbps_x10(uint32_t bytes, uint32_t ms)
{
    if (ms == 0)
        return 0;
    // bytes / 1024 / (ms / 1000) * 10
    return (uint32_t)((uint64_t)bytes * 10000 / 1024 / ms);
}
