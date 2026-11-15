#include <stdio.h>
#include <string.h>

#include "app.h"
#include "net.h"
#include "soak.h"
#include "textbuf.h"

soak_t soak;

bool soak_running(void)
{
    return soak.active && soak.result == SOAK_RUNNING;
}

static void read_counters(void)
{
    net_wifi_counters(&soak.q, &soak.t, &soak.r);
}

static void finish(int result, uint32_t now)
{
    if (!soak_running())
        return;
    read_counters();
    soak.result = result;
    soak.end = now;
    char line[96];
    soak_summary(line, sizeof(line));
    tb_puts(&app.tb, result == SOAK_PASS ? TB_INFO : TB_ERROR, line);
    app_view_update();
    app.dirty_conv = true;
    app.dirty_menu = true;
}

void soak_start(uint32_t now)
{
    memset(&soak, 0, sizeof(soak));
    soak.active = true;
    soak.result = SOAK_RUNNING;
    soak.start = now;
    soak.last_pong = now;
    read_counters();
    soak.q0 = soak.q;
    soak.t0 = soak.t;
    soak.r0 = soak.r;
    tb_puts(&app.tb, TB_INFO, "Wi-Fi soak test started (PINGs only, no AI).");
    app_view_update();
    app.dirty_conv = true;
}

void soak_poll(uint32_t now)
{
    if (!soak_running())
        return;
    read_counters();
    if (soak.t - soak.t0 >= SOAK_TARGET && now - soak.start >= SOAK_MIN_TIME)
    {
        finish(SOAK_PASS, now);
        return;
    }
    if (soak.inflight > 0 && now - soak.last_pong > SOAK_STALL)
    {
        finish(SOAK_STALLED, now);
        return;
    }
    while (soak.inflight < SOAK_INFLIGHT && net_online())
    {
        if (!net_request(TAG_SOAK, "PING", NULL, 0))
            break;
        soak.sent++;
        soak.inflight++;
    }
}

void soak_on_frame(const char *type, uint32_t now)
{
    if (soak.inflight > 0)
        soak.inflight--;
    if (!soak_running())
        return;                    // late answers after the end: ignore
    if (strcmp(type, "PONG") == 0)
    {
        soak.recv++;
        soak.last_pong = now;
    }
    else
    {
        soak.fails++;
    }
}

void soak_on_lost(const char *reason, uint32_t now)
{
    soak.inflight = 0;             // the queue was dropped with the socket
    if (!soak_running())
        return;
    snprintf(soak.reason, sizeof(soak.reason), "%s", reason);
    finish(SOAK_LOST, now);
}

void soak_stop(uint32_t now)
{
    finish(SOAK_STOPPED, now);
}

void soak_summary(char *out, int size)
{
    static const char *const names[] = { "running", "PASS", "STALL", "LOST", "stopped" };
    uint32_t secs = ((soak.result == SOAK_RUNNING ? app.now : soak.end) - soak.start) / 60;
    snprintf(out, size,
             "Soak %s: %lu PINGs %lu PONGs in %lus; Wi-Fi t+%lu r+%lu q+%lu (t%lu r%lu q%lu)",
             names[soak.result], (unsigned long)soak.sent, (unsigned long)soak.recv,
             (unsigned long)secs, (unsigned long)(soak.t - soak.t0),
             (unsigned long)(soak.r - soak.r0), (unsigned long)(soak.q - soak.q0),
             (unsigned long)soak.t, (unsigned long)soak.r, (unsigned long)soak.q);
}
