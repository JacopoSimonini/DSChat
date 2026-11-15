// Native unit tests for mictest's pure helpers. Run: cd ds/mictest/tests && make
#include <stdio.h>
#include <string.h>

#include "micutil.h"

static int checks, failed;
#define CHECK(c) do { checks++; if (!(c)) { failed++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void test_stats8(void)
{
    mu_stats st = { 0 };
    int8_t s[6] = { 0, 10, -20, 127, -128, 5 };
    mu_stats_add(&st, s, sizeof(s), 8);
    CHECK(st.count == 6);
    CHECK(st.peak == 128);
    CHECK(st.clipped == 2);                 // 127 and -128
    CHECK(mu_peak_pct(st.peak, 8) == 100);
    CHECK(mu_clip_permille(&st) == 333);
    mu_stats q = { 0 };
    int8_t h[2] = { 64, -32 };
    mu_stats_add(&q, h, 2, 8);
    CHECK(mu_peak_pct(q.peak, 8) == 50);
    CHECK(q.clipped == 0);
}

static void test_stats16(void)
{
    mu_stats st = { 0 };
    int16_t s[5] = { 0, 16384, -8192, 0x7FF0, -32768 };
    mu_stats_add(&st, s, sizeof(s), 16);
    CHECK(st.count == 5);
    CHECK(st.peak == 32768);
    CHECK(st.clipped == 2);
    mu_stats q = { 0 };
    mu_stats_add(&q, s, 2 * sizeof(int16_t), 16);   // only 0 and 16384
    CHECK(mu_peak_pct(q.peak, 16) == 50);
    CHECK(q.clipped == 0);
    // odd byte count: the last half sample is ignored
    mu_stats r = { 0 };
    mu_stats_add(&r, s, 3, 16);
    CHECK(r.count == 1);
    mu_stats z = { 0 };
    CHECK(mu_clip_permille(&z) == 0);
}

static void test_meta(void)
{
    mu_meta m = { 0 };
    m.bits = 16; m.rate = 16000; m.gain = 3; m.bytes = 64000; m.rec_ms = 2000;
    m.traffic = true; m.pings = 10; m.pongs = 9; m.clip_no = 4;
    char out[256];
    size_t n = mu_meta_format(out, sizeof(out), &m);
    CHECK(n == strlen(out));
    CHECK(strcmp(out, "clip=4 bits=16 rate=16000 gain=160 pace=0 bytes=64000 rec_ms=2000 traffic=1 "
                      "pings=10 pongs=9 lost=0 touches=0 touch_ms=-") == 0);
    m.touches = 10;
    for (int i = 0; i < 8; i++)
        m.touch_ms[i] = 100 * (i + 1);
    n = mu_meta_format(out, sizeof(out), &m);
    CHECK(strstr(out, "touches=10 touch_ms=100,200,300,400,500,600,700,800") != NULL);
    CHECK(mu_meta_format(out, 20, &m) == 0);        // does not fit
    CHECK(mu_gain_mult(0) == 20 && mu_gain_mult(2) == 80 && mu_gain_mult(4) == -1);
}

static void test_chunks(void)
{
    size_t total = 10000, off = 0, n, parts = 0;
    while ((n = mu_chunk(total, off, 4096)) > 0)
    {
        off += n;
        parts++;
    }
    CHECK(off == total && parts == 3);
    CHECK(mu_chunk(4096, 0, 4096) == 4096);
    CHECK(mu_chunk(4096, 4096, 4096) == 0);
    CHECK(mu_chunk(0, 0, 4096) == 0);
}

static void test_kbps(void)
{
    CHECK(mu_kbps_x10(102400, 1000) == 1000);    // 100.0 KB/s
    CHECK(mu_kbps_x10(51200, 2000) == 250);      // 25.0
    CHECK(mu_kbps_x10(320000, 7000) == 446);     // 44.6
    CHECK(mu_kbps_x10(1000, 0) == 0);
}

int main(void)
{
    test_stats8();
    test_stats16();
    test_meta();
    test_chunks();
    test_kbps();
    printf("%d checks, %d failed\n", checks, failed);
    return failed != 0;
}
