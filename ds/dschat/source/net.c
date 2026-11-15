// Network code adapted from ds/wifitest (proven on the real DS Lite), turned
// into a non-blocking state machine.

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <nds.h>
#include <dswifi9.h>

#include "net.h"

#define FPS              60
#define SCAN_TIMEOUT     (20 * FPS)
#define ASSOC_TIMEOUT    (30 * FPS)
#define CONNECT_TIMEOUT  (8 * FPS)
#define REQ_TIMEOUT      (10 * FPS)     // max wait for a one-frame answer
// While a SEND is in progress the relay guarantees DONE/FAIL within its
// 120 s reply timeout; first text can take 20+ s (Gemini on the real DS).
#define REPLY_TIMEOUT    (130 * FPS)
#define KEEPALIVE_IDLE   (20 * FPS)     // PING after this much silence
#define WAKE_TIMEOUT     (5 * FPS)      // answer expected after lid opened
#define WAKE_REPLY_GRACE (15 * FPS)     // same, while a reply is streaming
// v3 liveness probe while a reply is pending: if nothing arrived for
// PROBE_AFTER, send LIVE (answered at once by the relay, even mid-reply);
// no frame within PROBE_TIMEOUT -> the connection is dead, reconnect.
// Without it a stalled connection was only noticed after REPLY_TIMEOUT.
#define PROBE_AFTER      (15 * FPS)
#define PROBE_TIMEOUT    (10 * FPS)
#define MAX_QUEUE        8
#define TX_SIZE          (PROTO_HDR + PROTO_MAX_PAYLOAD + 64)
// Upload pacing (found with a packet capture of the mictest uploads,
// docs/TECHNICAL-NOTES.md): handing lwIP several KB at once made the DS send
// bursts; ~7% of its 1260-byte segments never reached the laptop, and with
// only 1-2 segments in flight lwIP never got the 3 duplicate ACKs needed for
// a fast retransmit, so every loss cost a ~1.26 s retransmission timeout.
// Giving send() at most one segment's worth per frame avoids the bursts.
#define TX_PACE_DEFAULT  1260

enum q_kind
{
    Q_APP_ONE = 0,       // app request answered by one frame
    Q_APP_SEND,          // TEXT* then DONE/FAIL
    Q_APP_LONG,          // one frame, but may take long (VEND: transcription)
    Q_HELLO_PING,
    Q_HELLO_PGET,
    Q_KEEPALIVE,
};

typedef struct
{
    uint8_t kind;
    int tag;
    uint32_t sent;
} q_entry;

static const dschat_config *cfg;
static int tx_pace = TX_PACE_DEFAULT;    // bytes handed to send() per frame, 0 = no limit
static int state = NS_OFF;
static bool test_mode;               // single attempt, stage reports, no retry
static int test_stage;               // last stage that passed (TS_*)
static uint32_t hello_at;            // when PING was sent (round-trip time)
static uint32_t now_f, state_since, retry_at;
static int failures;                 // consecutive failed connection attempts
static bool full_rejoin;             // next attempt starts from the Wi-Fi scan
static bool was_online;
static int sock = -1;
static struct sockaddr_in relay_addr;

static q_entry queue[MAX_QUEUE];
static int qlen;
static uint32_t last_rx, last_tx;
static uint32_t wake_deadline;       // 0 = none
static uint32_t probe_sent;          // 0 = no LIVE probe outstanding

static char txbuf[TX_SIZE];
static size_t txlen;
static proto_parser parser;
static proto_server_info server;
static char hello_profile[17], hello_model[48];

// Diagnostics shown in About (also useful on real hardware).
static uint32_t stat_sent, stat_recv, stat_conns;
static int stat_last_send = 0, stat_last_errno = 0;

static void set_state(int s)
{
    state = s;
    state_since = now_f;
}

int net_failures(void)
{
    return failures;
}

int net_scan_count(void)
{
    return state == NS_SCAN ? Wifi_GetNumAP() : 0;
}

int net_state(void)
{
    return state;
}

bool net_online(void)
{
    return state == NS_ONLINE;
}

const proto_server_info *net_server(void)
{
    return &server;
}

int net_retry_seconds(uint32_t now)
{
    if (state != NS_RETRY || retry_at <= now)
        return 0;
    return (retry_at - now + FPS - 1) / FPS;
}

void net_wifi_counters(uint32_t *q, uint32_t *t, uint32_t *r)
{
    *q = Wifi_GetStats(WSTAT_TXQUEUEDPACKETS);
    *t = Wifi_GetStats(WSTAT_TXPACKETS);
    *r = Wifi_GetStats(WSTAT_RXPACKETS);
}

// Counters for the 32-column About screen: 0..9999 as is, then "13k", "4M".
static const char *count_str(char *buf, size_t size, uint32_t n)
{
    if (n < 10000)
        snprintf(buf, size, "%lu", (unsigned long)n);
    else if (n < 10000000)
        snprintf(buf, size, "%luk", (unsigned long)(n / 1000));
    else
        snprintf(buf, size, "%luM", (unsigned long)(n / 1000000));
    return buf;
}

void net_get_info(net_info_t *info)
{
    memset(info, 0, sizeof(*info));
    u8 mac[6] = { 0 };
    Wifi_GetData(WIFIGETDATA_MACADDRESS, 6, mac);
    snprintf(info->mac, sizeof(info->mac), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    struct in_addr gw, mask, dns1, dns2;
    struct in_addr ip = Wifi_GetIPInfo(&gw, &mask, &dns1, &dns2);
    snprintf(info->ip, sizeof(info->ip), "%s", inet_ntoa(ip));
    if (relay_addr.sin_port != 0)
        snprintf(info->relay, sizeof(info->relay), "%s:%u",
                 inet_ntoa(relay_addr.sin_addr), ntohs(relay_addr.sin_port));
    else
        snprintf(info->relay, sizeof(info->relay), "%s:%u",
                 cfg->relay_host[0] ? cfg->relay_host : "gateway", cfg->relay_port);
    info->static_ip = !cfg->dhcp;
    char a[8], b[8], c[8], d[8];
    // c connections, s/r bytes sent/received; only when not zero: q requests
    // waiting for an answer, t bytes not yet given to TCP, e last send() errno
    int n = snprintf(info->stats, sizeof(info->stats), "c%s s%s r%s",
                     count_str(a, sizeof(a), stat_conns), count_str(b, sizeof(b), stat_sent),
                     count_str(c, sizeof(c), stat_recv));
    if (qlen && n < (int)sizeof(info->stats))
        n += snprintf(info->stats + n, sizeof(info->stats) - n, " q%d", qlen);
    if (txlen && n < (int)sizeof(info->stats))
        n += snprintf(info->stats + n, sizeof(info->stats) - n, " t%u", (unsigned)txlen);
    if (stat_last_errno && n < (int)sizeof(info->stats))
        snprintf(info->stats + n, sizeof(info->stats) - n, " e%d", stat_last_errno);
    // DSWiFi packets: q queued by us, t handed to the radio (also the DS's own
    // management/null frames; radio retries and frames lost after all retries
    // are not counted anywhere), r received, x refused because the TX ring was
    // full (stayed 0 in the upload-stall tests)
    snprintf(info->wifi, sizeof(info->wifi), "q%s t%s r%s x%s",
             count_str(a, sizeof(a), Wifi_GetStats(WSTAT_TXQUEUEDPACKETS)),
             count_str(b, sizeof(b), Wifi_GetStats(WSTAT_TXPACKETS)),
             count_str(c, sizeof(c), Wifi_GetStats(WSTAT_RXPACKETS)),
             count_str(d, sizeof(d), Wifi_GetStats(WSTAT_TXQUEUEDREJECTED)));
}

// --------------------------------------------------------------------------
// Connection teardown and retry
// --------------------------------------------------------------------------

static void close_socket(void)
{
    if (sock >= 0)
        close(sock);
    sock = -1;
    txlen = 0;
    qlen = 0;
    wake_deadline = 0;
    probe_sent = 0;
    proto_init(&parser);
}

static bool link_ok;     // the last failure proves the Wi-Fi link works

static void schedule_retry(void)
{
    failures++;
    // 1, 2, 4, 8, 8 ... seconds. After 3 failures in a row also rejoin Wi-Fi,
    // because the DS has no reliable "link lost" signal (sleep, AP restart) -
    // unless the laptop actively refused the connection: then the link is
    // fine and only the relay is down (being restarted), so keep retrying TCP
    // every 2 s.
    int secs = failures >= 4 ? 8 : 1 << (failures - 1);
    if (link_ok)
        secs = failures == 1 ? 1 : 2;
    else if (failures >= 3)
        full_rejoin = true;
    link_ok = false;
    retry_at = now_f + secs * FPS;
    set_state(NS_RETRY);
}

static void test_report(int stage, bool ok, const char *msg)
{
    if (ok)
        test_stage = stage;
    app_net_test(stage, ok, msg);
}

static void lost(const char *reason)
{
    if (test_mode)
    {
        // Test connection: one attempt only. Report which stage failed.
        close_socket();
        Wifi_DisconnectAP();
        test_mode = false;
        set_state(NS_IDLE);
        test_report(test_stage + 1, false, reason);
        return;
    }
    bool notify = was_online;
    close_socket();
    if (notify)
    {
        was_online = false;
        app_net_lost(reason);
    }
    else
    {
        app_net_notice(reason);
    }
    schedule_retry();
}

// --------------------------------------------------------------------------
// Wi-Fi
// --------------------------------------------------------------------------

bool net_init(const dschat_config *c)
{
    cfg = c;
    proto_init(&parser);
    if (!Wifi_InitDefault(INIT_ONLY | WIFI_DS_MODE_ONLY))
    {
        set_state(NS_OFF);
        return false;
    }
    set_state(NS_IDLE);             // net_start() begins connecting
    return true;
}

// Set when a whole scan saw no access point at all: the radio is probably
// stuck (seen in melonDS: DSWiFi's ARM7 loop kept running, packets kept being
// queued, but nothing was sent or received any more). The next attempt then
// power-cycles the Wi-Fi hardware before scanning.
static bool radio_reset;
static int scan_max_aps;

static void start_scan(void)
{
    Wifi_DisconnectAP();
    if (radio_reset)
    {
        radio_reset = false;
        app_net_notice("Wi-Fi: resetting the radio");
        Wifi_DisableWifi();
        set_state(NS_RADIO_RESET);
        return;
    }
    Wifi_ScanMode();
    scan_max_aps = 0;
    set_state(NS_SCAN);
}

// Power-cycle the Wi-Fi hardware, then rescan. (Wifi_Deinit() + re-init is
// NOT used: it busy-waits for the ARM7, and with a stuck radio in melonDS it
// never returned, freezing the whole UI.)
static void poll_radio_reset(void)
{
    if (now_f - state_since < FPS)          // stay off for a second
        return;
    Wifi_EnableWifi();
    Wifi_ScanMode();
    scan_max_aps = 0;
    set_state(NS_SCAN);
}

// Radio watchdog. The AP beacons ~10 times a second, so a working radio
// always receives something. If the RX counter stops for RX_DEAD, the Wi-Fi
// hardware is stuck (seen in melonDS after ~950 TX packets: DSWiFi kept
// queueing, nothing was sent or received, Disable/Enable did not help).
#define RX_DEAD (8 * FPS)
static uint32_t rx_seen, rx_changed_at;

static bool radio_dead(void)
{
    uint32_t rx = Wifi_GetStats(WSTAT_RXPACKETS);
    if (rx != rx_seen || state == NS_RADIO_RESET || state == NS_OFF || state == NS_IDLE)
    {
        rx_seen = rx;
        rx_changed_at = now_f;
        return false;
    }
    return now_f - rx_changed_at > RX_DEAD;
}

static bool find_ap(Wifi_AccessPoint *out)
{
    int n = Wifi_GetNumAP();
    for (int i = 0; i < n; i++)
    {
        Wifi_AccessPoint ap;
        if (Wifi_GetAPData(i, &ap) != WIFI_RETURN_OK)
            continue;
        if (strcmp(ap.ssid, cfg->ssid) == 0)
        {
            *out = ap;
            return true;
        }
    }
    return false;
}

static void poll_scan(void)
{
    // Give the scan a moment, then look every 15 frames.
    if ((now_f - state_since) < 30 || (now_f - state_since) % 15 != 0)
        return;
    int seen = Wifi_GetNumAP();
    if (seen > scan_max_aps)
        scan_max_aps = seen;
    Wifi_AccessPoint ap;
    if (find_ap(&ap))
    {
        if (ap.security_type == AP_SECURITY_WPA || ap.security_type == AP_SECURITY_WPA2)
        {
            lost("Wi-Fi: network uses WPA - not supported by the DS Lite");
            return;
        }
        if (ap.security_type == AP_SECURITY_WEP && cfg->security != SEC_WEP)
        {
            lost("Wi-Fi: network uses WEP - set security to WEP + key");
            return;
        }
        if (ap.security_type == AP_SECURITY_OPEN && cfg->security == SEC_WEP)
        {
            lost("Wi-Fi: network is open - set security to open");
            return;
        }
        uint8_t key[13];
        int key_len = 0;
        if (cfg->security == SEC_WEP && (key_len = wep_key_bytes(cfg->wep_key, key)) == 0)
        {
            lost("Wi-Fi: WEP key not valid");
            return;
        }
        if (test_mode)
        {
            char msg[80];
            snprintf(msg, sizeof(msg), "found \"%.32s\" ch %d, signal %d, %s", ap.ssid,
                     ap.channel, ap.rssi, ap.security_type == AP_SECURITY_WEP ? "WEP" : "open");
            test_report(TS_FOUND, true, msg);
        }
        if (!cfg->dhcp)
        {
            u32 gw = inet_addr(cfg->static_gw);
            Wifi_SetIP(inet_addr(cfg->static_ip), gw, inet_addr(cfg->static_mask), gw, 0);
        }
        else
        {
            Wifi_SetIP(0, 0, 0, 0, 0);
        }
        if (Wifi_ConnectSecureAP(&ap, key_len ? key : NULL, key_len) != 0)
        {
            lost("Wi-Fi connect failed");
            return;
        }
        set_state(NS_ASSOC);
        return;
    }
    if (now_f - state_since > SCAN_TIMEOUT)
    {
        if (scan_max_aps == 0)
            radio_reset = true;
        char msg[64];
        snprintf(msg, sizeof(msg), "Wi-Fi: \"%s\" not found", cfg->ssid);
        lost(msg);
    }
}

static void start_connect(void);

static void poll_assoc(void)
{
    // Note: DSWiFi reports ASSOCIATED after DHCP only once it has an IP AND
    // a DNS server (the AP's dnsmasq sends both; see docs/TECHNICAL-NOTES.md).
    int st = Wifi_AssocStatus();
    if (test_mode && test_stage < TS_JOINED
        && (st == ASSOCSTATUS_ACQUIRINGDHCP || st == ASSOCSTATUS_ASSOCIATED))
        test_report(TS_JOINED, true, "associated with the access point");
    if (st == ASSOCSTATUS_ASSOCIATED)
    {
        full_rejoin = false;
        if (test_mode)
        {
            struct in_addr gw, mask, d1, d2;
            struct in_addr ip = Wifi_GetIPInfo(&gw, &mask, &d1, &d2);
            char a[16], g[16], d[16], msg[96];
            snprintf(a, sizeof(a), "%s", inet_ntoa(ip));
            snprintf(g, sizeof(g), "%s", inet_ntoa(gw));
            snprintf(d, sizeof(d), "%s", inet_ntoa(d1));
            snprintf(msg, sizeof(msg), "%s %s, gateway %s, DNS %s",
                     cfg->dhcp ? "DHCP" : "static", a, g, d);
            test_report(TS_IP, true, msg);
        }
        start_connect();
    }
    else if (st == ASSOCSTATUS_CANNOTCONNECT)
    {
        lost("Wi-Fi: cannot connect (rejected by the AP)");
    }
    else if (now_f - state_since > ASSOC_TIMEOUT)
    {
        struct in_addr gw, mask, d1, d2;
        struct in_addr ip = Wifi_GetIPInfo(&gw, &mask, &d1, &d2);
        const char *why;
        if (st != ASSOCSTATUS_ACQUIRINGDHCP)
            why = "Wi-Fi: association timed out";
        else if (ip.s_addr && !d1.s_addr)
            // DSWiFi only finishes DHCP once it also has a DNS server
            why = "DHCP gave an IP but no DNS server (DSWiFi needs one)";
        else if (cfg->security == SEC_WEP)
            why = "no IP from DHCP - wrong WEP key?";
        else
            why = "no IP address from DHCP";
        lost(why);
    }
}

// --------------------------------------------------------------------------
// TCP
// --------------------------------------------------------------------------

static void start_connect(void)
{
    struct in_addr gw, mask, dns1, dns2;
    Wifi_GetIPInfo(&gw, &mask, &dns1, &dns2);
    memset(&relay_addr, 0, sizeof(relay_addr));
    relay_addr.sin_family = AF_INET;
    relay_addr.sin_port = htons(cfg->relay_port);
    if (cfg->relay_host[0])
        inet_aton(cfg->relay_host, &relay_addr.sin_addr);
    else
        relay_addr.sin_addr = gw;

    close_socket();
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        lost("socket() failed");
        return;
    }
    int on = 1;
    ioctl(sock, FIONBIO, &on);
    // small frames (LIVE, CNCL, voice chunks) go out at once, not Nagle-delayed
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    int r = connect(sock, (struct sockaddr *)&relay_addr, sizeof(relay_addr));
    if (r < 0 && errno != EINPROGRESS)
    {
        lost("relay: connect failed");
        return;
    }
    set_state(NS_CONNECT);
}

static bool queue_push(uint8_t kind, int tag)
{
    if (qlen >= MAX_QUEUE)
        return false;
    queue[qlen].kind = kind;
    queue[qlen].tag = tag;
    queue[qlen].sent = now_f;
    qlen++;
    return true;
}

static void queue_pop(void)
{
    if (qlen == 0)
        return;
    memmove(&queue[0], &queue[1], (qlen - 1) * sizeof(queue[0]));
    qlen--;
    if (qlen > 0)
        queue[0].sent = now_f;     // its wait starts now
}

static bool write_frame(const char *type, const char *payload, size_t len)
{
    size_t n = proto_encode(txbuf + txlen, sizeof(txbuf) - txlen, type, payload, len);
    if (n == 0)
        return false;
    txlen += n;
    return true;
}

static void start_hello(void)
{
    stat_conns++;
    hello_at = now_f;
    memset(&server, 0, sizeof(server));
    hello_profile[0] = hello_model[0] = '\0';
    write_frame("PING", NULL, 0);
    queue_push(Q_HELLO_PING, 0);
    write_frame("PGET", NULL, 0);
    queue_push(Q_HELLO_PGET, 0);
    last_rx = now_f;
    set_state(NS_HELLO);
}

static void poll_connect(void)
{
    struct pollfd pfd = { .fd = sock, .events = POLLOUT, .revents = 0 };
    int r = poll(&pfd, 1, 0);
    if (r > 0)
    {
        int err = 0;
        socklen_t l = sizeof(err);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &l);
        if (err == 0 && (pfd.revents & POLLOUT) && !(pfd.revents & (POLLERR | POLLHUP)))
        {
            start_hello();
            return;
        }
        // A fast error means the laptop answered (RST): the Wi-Fi link works.
        link_ok = true;
        char msg[48];
        // lwIP reports a RST during connect as ECONNRESET (seen in melonDS)
        if (err == ECONNREFUSED || err == ECONNRESET)
            snprintf(msg, sizeof(msg), "relay not running (refused)");
        else
            snprintf(msg, sizeof(msg), "relay: connect failed (err %d/%x)", err, pfd.revents);
        lost(msg);
        return;
    }
    if (now_f - state_since > CONNECT_TIMEOUT)
        lost("relay: connect timed out");
}

static void handle_frame(void)
{
    const char *type = parser.type;
    last_rx = now_f;
    wake_deadline = 0;
    probe_sent = 0;              // any frame proves the connection works

    if (strcmp(type, "LIVE") == 0)
        return;                  // probe answer, outside the request queue

    if (qlen == 0)
    {
        // Unsolicited: the relay only does this before closing (e.g. too many
        // connections or a protocol error).
        if (strcmp(type, "FAIL") == 0)
        {
            char msg[80];
            snprintf(msg, sizeof(msg), "relay: %.64s", parser.payload);
            app_net_notice(msg);
        }
        return;
    }

    q_entry head = queue[0];
    switch (head.kind)
    {
        case Q_APP_SEND:
            if (strcmp(type, "TEXT") == 0)
            {
                queue[0].sent = now_f;     // silence is measured per chunk
                app_net_frame(head.tag, type, parser.payload, parser.len);
                return;
            }
            queue_pop();
            app_net_frame(head.tag, type, parser.payload, parser.len);
            return;
        case Q_APP_ONE:
        case Q_APP_LONG:
            queue_pop();
            app_net_frame(head.tag, type, parser.payload, parser.len);
            return;
        case Q_KEEPALIVE:
            queue_pop();
            return;
        case Q_HELLO_PING:
            queue_pop();
            if (strcmp(type, "PONG") == 0)
                proto_parse_pong(parser.payload, &server);
            return;
        case Q_HELLO_PGET:
            queue_pop();
            if (strcmp(type, "INFO") == 0)
                proto_parse_name_model(parser.payload, hello_profile, sizeof(hello_profile),
                                       hello_model, sizeof(hello_model));
            // (v1 relays answer FAIL: no profiles, keep going)
            if (test_mode)
            {
                char msg[96];
                snprintf(msg, sizeof(msg), "%s:%u answered: DSChat relay v%d (%lu ms)",
                         inet_ntoa(relay_addr.sin_addr), ntohs(relay_addr.sin_port),
                         server.version, (unsigned long)((now_f - hello_at) * 1000 / FPS));
                test_report(TS_RELAY, true, msg);
                net_stop();
                test_report(TS_DONE, true, "all stages passed");
                return;
            }
            set_state(NS_ONLINE);
            failures = 0;
            was_online = true;
            app_net_online(&server, hello_profile, hello_model);
            return;
    }
}

static bool flush_tx(void)
{
    size_t budget = tx_pace > 0 ? (size_t)tx_pace : txlen;
    while (txlen > 0 && budget > 0)
    {
        size_t chunk = txlen < budget ? txlen : budget;
        int n = send(sock, txbuf, chunk, 0);
        stat_last_send = n;
        if (n < 0)
            stat_last_errno = errno;
        if (n > 0)
        {
            stat_sent += n;
            memmove(txbuf, txbuf + n, txlen - n);
            txlen -= n;
            budget -= (size_t)n < budget ? (size_t)n : budget;
            last_tx = now_f;
            continue;
        }
        if (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN))
            return true;
        lost("connection lost (send)");
        return false;
    }
    return true;
}

static bool read_rx(void)
{
    static uint8_t buf[512];
    for (int rounds = 0; rounds < 8; rounds++)     // bounded work per frame
    {
        int n = recv(sock, buf, sizeof(buf), 0);
        if (n == 0)
        {
            lost("relay closed the connection");
            return false;
        }
        if (n < 0)
        {
            if (errno == EWOULDBLOCK || errno == EAGAIN)
                return true;
            lost("connection lost (recv)");
            return false;
        }
        stat_recv += n;
        size_t off = 0;
        while (off < (size_t)n)
        {
            size_t used;
            int r = proto_feed(&parser, buf + off, n - off, &used);
            off += used;
            if (r == PROTO_ERROR)
            {
                lost("protocol error from relay");
                return false;
            }
            if (r == PROTO_FRAME)
            {
                handle_frame();
                if (sock < 0)           // a callback reconnected
                    return false;
            }
        }
    }
    return true;
}

static void poll_session(void)
{
    if (!flush_tx() || !read_rx() || sock < 0)
        return;

    if (qlen > 0)
    {
        bool lng = queue[0].kind == Q_APP_SEND || queue[0].kind == Q_APP_LONG;
        uint32_t limit = lng ? REPLY_TIMEOUT : REQ_TIMEOUT;
        if (now_f - queue[0].sent > limit)
        {
            lost(lng ? "no data from relay for 130 s" : "relay not answering");
            return;
        }
    }
    if (qlen > 0 && (queue[0].kind == Q_APP_SEND || queue[0].kind == Q_APP_LONG)
        && server.version >= 3)
    {
        uint32_t quiet_since = last_rx > queue[0].sent ? last_rx : queue[0].sent;
        if (probe_sent == 0 && now_f - quiet_since > PROBE_AFTER)
        {
            write_frame("LIVE", NULL, 0);
            probe_sent = now_f;
        }
        else if (probe_sent != 0 && now_f - probe_sent > PROBE_TIMEOUT)
        {
            lost("relay not responding");
            return;
        }
    }
    if (wake_deadline && now_f > wake_deadline)
    {
        full_rejoin = true;     // after sleep the AP has probably dropped us
        lost("connection lost while asleep");
        return;
    }
    if (state == NS_ONLINE && qlen == 0)
    {
        uint32_t last = last_rx > last_tx ? last_rx : last_tx;
        if (now_f - last > KEEPALIVE_IDLE)
        {
            write_frame("PING", NULL, 0);
            queue_push(Q_KEEPALIVE, 0);
        }
    }
}

// --------------------------------------------------------------------------
// Public
// --------------------------------------------------------------------------

void net_poll(uint32_t now)
{
    now_f = now;
    if (radio_dead())
    {
        rx_changed_at = now_f;              // one reset per detection
        radio_reset = true;
        full_rejoin = true;
        lost("Wi-Fi radio stopped receiving");
        retry_at = now_f;                   // reset right away
        return;
    }
    switch (state)
    {
        case NS_OFF:
        case NS_IDLE:
            return;
        case NS_RETRY:
            if (now_f >= retry_at)
            {
                if (full_rejoin)
                    start_scan();
                else
                    start_connect();
            }
            return;
        case NS_SCAN:
            poll_scan();
            return;
        case NS_RADIO_RESET:
            poll_radio_reset();
            return;
        case NS_ASSOC:
            poll_assoc();
            return;
        case NS_CONNECT:
            poll_connect();
            return;
        case NS_HELLO:
        case NS_ONLINE:
            if (state == NS_HELLO && now_f - state_since > REQ_TIMEOUT)
            {
                lost("relay did not answer PING");
                return;
            }
            poll_session();
            return;
    }
}

bool net_request(int tag, const char *type, const char *payload, size_t len)
{
    if (state != NS_ONLINE || qlen >= MAX_QUEUE)
        return false;
    if (!write_frame(type, payload, len))
        return false;
    queue_push(strcmp(type, "SEND") == 0 ? Q_APP_SEND
               : strcmp(type, "VEND") == 0 ? Q_APP_LONG : Q_APP_ONE, tag);
    return true;
}

bool net_send_noreply(const char *type, const void *payload, size_t len)
{
    if (state != NS_ONLINE)
        return false;
    return write_frame(type, (const char *)payload, len);
}

size_t net_tx_pending(void)
{
    return txlen;
}

void net_set_tx_pace(int bytes_per_frame)
{
    tx_pace = bytes_per_frame;
}

bool net_cancel(void)
{
    if (state != NS_ONLINE)
        return false;
    return write_frame("CNCL", NULL, 0);
}

void net_start(const dschat_config *c, bool test)
{
    if (state == NS_OFF)
        return;
    cfg = c;
    close_socket();
    was_online = false;
    test_mode = test;
    test_stage = 0;
    failures = 0;
    link_ok = false;
    radio_reset = false;
    full_rejoin = true;
    relay_addr.sin_port = 0;
    retry_at = now_f;
    set_state(NS_RETRY);
}

void net_stop(void)
{
    if (state == NS_OFF)
        return;
    close_socket();
    Wifi_DisconnectAP();
    was_online = false;
    test_mode = false;
    set_state(NS_IDLE);
}

void net_shutdown(void)
{
    if (sock >= 0 && txlen > 0)
        send(sock, txbuf, txlen, 0);          // best effort (e.g. a CNCL)
    close_socket();
    if (state != NS_OFF)
    {
        Wifi_DisconnectAP();                  // the AP gets a deauthentication
        Wifi_DisableWifi();
    }
    was_online = false;
    set_state(NS_OFF);
}

void net_reconnect(void)
{
    if (state == NS_OFF || state == NS_IDLE)
        return;
    bool notify = was_online;
    close_socket();
    was_online = false;
    if (notify)
        app_net_lost("reconnecting (menu)");
    failures = 0;
    full_rejoin = true;
    retry_at = now_f;
    set_state(NS_RETRY);
}

void net_wake(uint32_t now)
{
    now_f = now;
    if (state == NS_ONLINE)
    {
        // Probe right away instead of waiting for the keepalive. While a reply
        // is pending the PING would only be answered after it, so then any
        // frame within the grace period counts as proof of life.
        if (qlen == 0)
        {
            write_frame("PING", NULL, 0);
            queue_push(Q_KEEPALIVE, 0);
            wake_deadline = now_f + WAKE_TIMEOUT;
        }
        else
        {
            wake_deadline = now_f + (queue[0].kind == Q_APP_SEND ? WAKE_REPLY_GRACE
                                                                  : WAKE_TIMEOUT);
        }
    }
    else if (state == NS_RETRY)
    {
        retry_at = now_f;     // try again now
    }
}
