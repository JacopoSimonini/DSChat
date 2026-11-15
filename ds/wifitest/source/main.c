// DSChat Phase 1 Wi-Fi test ROM.
//
// Top screen: text log. Bottom screen: status / key help.
// Flow: init DSWiFi -> scan + list APs -> join CFG_SSID (open) -> DHCP ->
// print MAC/IP/gateway -> TCP to relay -> PING -> SEND one message and print
// the streamed reply. Protocol: docs/PROTOCOL.md.

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <nds.h>
#include <dswifi9.h>

#include "dschat_cfg.h"

#define HEADER_LEN       10
#define MAX_PAYLOAD      4096
#define SCAN_FRAMES      (60 * 4)    // initial scan time before listing
#define FIND_FRAMES      (60 * 20)   // give up looking for the SSID after this
#define ASSOC_FRAMES     (60 * 30)   // association + DHCP timeout
#define IO_TIMEOUT       (60 * 20)   // max silence while waiting for a frame
// While a reply is in progress the relay guarantees DONE or FAIL within its
// 120 s reply timeout (docs/PROTOCOL.md), and the AI API may stay silent for
// 20+ s before the first chunk (Gemini, measured on real hardware), so the DS
// waits a little longer than the relay instead of giving up first.
#define REPLY_TIMEOUT    (60 * 130)

static PrintConsole top_screen;
static PrintConsole bottom_screen;
static volatile u32 frames;          // VBlank counter (60 Hz)

static void vblank_handler(void)
{
    frames++;
}

static void status(const char *fmt, ...)
{
    consoleSelect(&bottom_screen);
    consoleClear();
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    consoleSelect(&top_screen);
}

static void wait_frames(int n)
{
    while (n-- > 0)
        cothread_yield_irq(IRQ_VBLANK);
}

static u32 wait_keys(u32 mask)
{
    while (1)
    {
        cothread_yield_irq(IRQ_VBLANK);
        scanKeys();
        u32 k = keysDown() & mask;
        if (k)
            return k;
    }
}

// ---------------------------------------------------------------------------
// Wi-Fi
// ---------------------------------------------------------------------------

static void print_mac(void)
{
    u8 mac[6] = { 0 };
    Wifi_GetData(WIFIGETDATA_MACADDRESS, 6, mac);
    printf("DS MAC %02X:%02X:%02X:%02X:%02X:%02X\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void list_aps(void)
{
    int n = Wifi_GetNumAP();
    printf("%d AP(s) seen:\n", n);
    for (int i = 0; i < n; i++)
    {
        Wifi_AccessPoint ap;
        if (Wifi_GetAPData(i, &ap) != WIFI_RETURN_OK)
            continue;
        printf(" %-14.14s c%-2d %-4.4s %3d%s\n", ap.ssid, ap.channel,
               Wifi_ApSecurityTypeString(ap.security_type), ap.rssi,
               (ap.flags & WFLAG_APDATA_COMPATIBLE) ? "" : " X");
    }
}

static bool find_ap(const char *ssid, Wifi_AccessPoint *out)
{
    int n = Wifi_GetNumAP();
    for (int i = 0; i < n; i++)
    {
        Wifi_AccessPoint ap;
        if (Wifi_GetAPData(i, &ap) != WIFI_RETURN_OK)
            continue;
        if (strcmp(ap.ssid, ssid) == 0)
        {
            *out = ap;
            return true;
        }
    }
    return false;
}

static bool join_network(void)
{
    Wifi_AccessPoint ap;

    printf("Scanning...\n");
    status("Scanning for APs\nlooking for \"%s\"\n", CFG_SSID);
    Wifi_ScanMode();
    wait_frames(SCAN_FRAMES);
    list_aps();

    u32 start = frames;
    while (!find_ap(CFG_SSID, &ap))
    {
        if (frames - start > FIND_FRAMES)
        {
            printf("SSID \"%s\" not found\n", CFG_SSID);
            list_aps();
            return false;
        }
        cothread_yield_irq(IRQ_VBLANK);
    }

    printf("Found %s ch%d rssi %d %s\n", ap.ssid, ap.channel, ap.rssi,
           Wifi_ApSecurityTypeString(ap.security_type));
    printf("BSSID %02X:%02X:%02X:%02X:%02X:%02X\n", ap.bssid[0], ap.bssid[1],
           ap.bssid[2], ap.bssid[3], ap.bssid[4], ap.bssid[5]);
    if (ap.security_type != AP_SECURITY_OPEN)
    {
        printf("AP is not open, aborting\n");
        return false;
    }
    if (!(ap.flags & WFLAG_APDATA_COMPATIBLE))
        printf("Warning: AP not flagged compatible\n");

    if (CFG_STATIC_IP[0])
    {
        // Static mode: no DHCP at all. DSWiFi then reports "Associated" as
        // soon as the 802.11 association completes.
        u32 gwaddr = inet_addr(CFG_STATIC_GW);
        printf("Static IP %s\n", CFG_STATIC_IP);
        Wifi_SetIP(inet_addr(CFG_STATIC_IP), gwaddr, inet_addr(CFG_STATIC_MASK),
                   gwaddr, 0);
    }
    else
    {
        Wifi_SetIP(0, 0, 0, 0, 0); // DHCP
    }
    if (Wifi_ConnectSecureAP(&ap, NULL, 0) != 0)
    {
        printf("Wifi_ConnectSecureAP failed\n");
        return false;
    }

    // Note: DSWiFi 0.4.2 only reports ASSOCIATED after DHCP when lwIP has
    // BOTH an IPv4 address and a DNS server (access_point.c, WIFI_CONNECT_DHCPING),
    // so the DHCP server must send option 6 even though we never use DNS.
    int old = -1;
    u32 last_sec = 0;
    start = frames;
    while (1)
    {
        cothread_yield_irq(IRQ_VBLANK);
        int st = Wifi_AssocStatus();
        u32 sec = (frames - start) / 60;
        bool changed = (st != old);
        if (changed)
        {
            printf("[%2lus] %s\n", sec, ASSOCSTATUS_STRINGS[st]);
            old = st;
        }
        if (changed || sec != last_sec)
        {
            // Once per second: what lwIP has right now (0.0.0.0 = nothing yet).
            struct in_addr g, m, d1, d2;
            struct in_addr a = Wifi_GetIPInfo(&g, &m, &d1, &d2);
            consoleSelect(&bottom_screen);
            consoleClear();
            printf("Wi-Fi: %s\n[%2lus]\n", ASSOCSTATUS_STRINGS[st], sec);
            printf("IP   %s\n", inet_ntoa(a));
            printf("GW   %s\n", inet_ntoa(g));
            printf("Mask %s\n", inet_ntoa(m));
            printf("DNS  %s\n", inet_ntoa(d1));
            consoleSelect(&top_screen);
            if (st == ASSOCSTATUS_ACQUIRINGDHCP && sec != last_sec && sec % 5 == 0)
                printf("[%2lus] ip %s dns %s\n", sec, inet_ntoa(a),
                       d1.s_addr ? "set" : "none");
            last_sec = sec;
        }
        if (st == ASSOCSTATUS_ASSOCIATED)
            break;
        if (st == ASSOCSTATUS_CANNOTCONNECT)
            return false;
        if (frames - start > ASSOC_FRAMES)
        {
            struct in_addr g, m, d1, d2;
            struct in_addr a = Wifi_GetIPInfo(&g, &m, &d1, &d2);
            printf("Timed out connecting\n");
            printf("ip %s\n", inet_ntoa(a));
            printf("dns %s\n", inet_ntoa(d1));
            if (a.s_addr && !d1.s_addr)
                printf("Got an IP but no DNS server:\nDHCP reply lacks option 6\n");
            Wifi_DisconnectAP();
            return false;
        }
    }

    struct in_addr gw, mask, dns1, dns2;
    struct in_addr ip = Wifi_GetIPInfo(&gw, &mask, &dns1, &dns2);
    printf("IP   %s\n", inet_ntoa(ip));
    printf("GW   %s\n", inet_ntoa(gw));
    printf("Mask %s\n", inet_ntoa(mask));
    printf("DNS  %s\n", inet_ntoa(dns1));
    return true;
}

// ---------------------------------------------------------------------------
// Relay protocol
// ---------------------------------------------------------------------------

static int sock = -1;
static u32 io_timeout = IO_TIMEOUT;  // REPLY_TIMEOUT while a reply is streaming
static u32 reply_start;              // frames when SEND went out (0 = no reply)
static char payload[MAX_PAYLOAD + 1];

static bool send_all(const void *buf, size_t len)
{
    const char *p = buf;
    u32 start = frames;
    while (len > 0)
    {
        int n = send(sock, p, len, 0);
        if (n > 0)
        {
            p += n;
            len -= n;
            start = frames;
            continue;
        }
        if (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)
            && frames - start < IO_TIMEOUT)
        {
            cothread_yield_irq(IRQ_VBLANK);
            continue;
        }
        printf("send failed (errno %d)\n", errno);
        return false;
    }
    return true;
}

static bool recv_exact(void *buf, size_t len)
{
    char *p = buf;
    u32 start = frames;
    while (len > 0)
    {
        int n = recv(sock, p, len, 0);
        if (n > 0)
        {
            p += n;
            len -= n;
            start = frames;
            continue;
        }
        if (n == 0)
        {
            printf("relay closed connection\n");
            return false;
        }
        if ((errno == EWOULDBLOCK || errno == EAGAIN) && frames - start < io_timeout)
        {
            cothread_yield_irq(IRQ_VBLANK);
            if (reply_start && (frames - reply_start) % 60 == 0)
                status("Waiting for reply... %lus\n", (frames - reply_start) / 60);
            continue;
        }
        if (errno == EWOULDBLOCK || errno == EAGAIN)
            printf("no data from relay for %lus\n", io_timeout / 60);
        else
            printf("recv failed (errno %d)\n", errno);
        return false;
    }
    return true;
}

static bool send_frame(const char *type, const char *data, size_t len)
{
    char hdr[HEADER_LEN + 1];
    snprintf(hdr, sizeof(hdr), "%.4s %04u\n", type, (unsigned)len);
    return send_all(hdr, HEADER_LEN) && (len == 0 || send_all(data, len));
}

// Reads one frame into type[5] / payload. Returns payload length or -1.
static int recv_frame(char type[5])
{
    char hdr[HEADER_LEN];
    if (!recv_exact(hdr, HEADER_LEN))
        return -1;
    if (hdr[4] != ' ' || hdr[9] != '\n')
    {
        printf("bad frame header\n");
        return -1;
    }
    int len = 0;
    for (int i = 5; i < 9; i++)
    {
        if (hdr[i] < '0' || hdr[i] > '9')
        {
            printf("bad frame length\n");
            return -1;
        }
        len = len * 10 + (hdr[i] - '0');
    }
    if (len > MAX_PAYLOAD)
    {
        printf("frame too long\n");
        return -1;
    }
    memcpy(type, hdr, 4);
    type[4] = '\0';
    if (len > 0 && !recv_exact(payload, len))
        return -1;
    payload[len] = '\0';
    return len;
}

static bool relay_connect(void)
{
    struct in_addr gw, mask, dns1, dns2;
    Wifi_GetIPInfo(&gw, &mask, &dns1, &dns2);

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(CFG_PORT);
    if (CFG_HOST[0] != '\0')
        inet_aton(CFG_HOST, &sa.sin_addr);
    else
        sa.sin_addr = gw;

    printf("TCP %s:%d ...\n", inet_ntoa(sa.sin_addr), CFG_PORT);
    status("Connecting to relay\n%s:%d\n", inet_ntoa(sa.sin_addr), CFG_PORT);

    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0)
    {
        printf("socket() failed (errno %d)\n", errno);
        return false;
    }
    if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) < 0)
    {
        printf("connect() failed (errno %d)\n", errno);
        close(sock);
        sock = -1;
        return false;
    }
    int opt = 1;
    ioctl(sock, FIONBIO, &opt);
    printf("Connected\n");
    return true;
}

static bool do_ping(void)
{
    char type[5];
    u32 t0 = frames;
    if (!send_frame("PING", NULL, 0))
        return false;
    int len = recv_frame(type);
    if (len < 0)
        return false;
    printf("%s \"%s\" (%lu ms)\n", type, payload, (frames - t0) * 1000 / 60);
    return strcmp(type, "PONG") == 0;
}

static bool do_chat_reply(u32 t0)
{
    char type[5];
    int chunks = 0, chars = 0;
    while (1)
    {
        int len = recv_frame(type);
        if (len < 0)
            return false;
        if (strcmp(type, "TEXT") == 0)
        {
            if (chunks == 0)
            {
                reply_start = 0;    // stop the "Waiting for reply" counter
                status("Streaming reply...\nfirst chunk after %lu ms\n",
                       (frames - t0) * 1000 / 60);
            }
            chunks++;
            chars += len;
            fputs(payload, stdout);
        }
        else if (strcmp(type, "DONE") == 0)
        {
            printf("\n[DONE %s] %d chunks\n%d chars, %lu ms\n", payload, chunks,
                   chars, (frames - t0) * 1000 / 60);
            return true;
        }
        else if (strcmp(type, "FAIL") == 0)
        {
            printf("\n[FAIL] %s\n", payload);
            return false;
        }
        else
        {
            printf("\n[unexpected %s]\n", type);
            return false;
        }
    }
}

static bool do_chat(const char *msg)
{
    u32 t0 = frames;

    printf("\n> %s\n\n", msg);
    status("Waiting for reply...\n");
    if (!send_frame("SEND", msg, strlen(msg)))
        return false;

    reply_start = frames | 1;   // never 0
    io_timeout = REPLY_TIMEOUT;
    bool ok = do_chat_reply(t0);
    io_timeout = IO_TIMEOUT;
    reply_start = 0;
    return ok;
}

// ---------------------------------------------------------------------------

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    irqSet(IRQ_VBLANK, vblank_handler);
    irqEnable(IRQ_VBLANK);

    videoSetMode(MODE_0_2D);
    videoSetModeSub(MODE_0_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    vramSetBankC(VRAM_C_SUB_BG);
    consoleInit(&top_screen, 0, BgType_Text4bpp, BgSize_T_256x256, 31, 0, true, true);
    consoleInit(&bottom_screen, 0, BgType_Text4bpp, BgSize_T_256x256, 31, 0, false, true);
    consoleSelect(&top_screen);

    printf("DSChat Wi-Fi test\n");
    printf("SSID %s port %d\n", CFG_SSID, CFG_PORT);
    status("Initialising Wi-Fi...\n");

    if (!Wifi_InitDefault(INIT_ONLY | WIFI_DS_MODE_ONLY))
    {
        printf("Wifi_InitDefault failed\n");
        status("Wi-Fi init FAILED\nSTART: exit\n");
        wait_keys(KEY_START);
        return 0;
    }
    print_mac();

    bool joined = false;
    while (1)
    {
        if (!joined || Wifi_AssocStatus() != ASSOCSTATUS_ASSOCIATED)
        {
            if (joined)
            {
                printf("Wi-Fi lost, rejoining\n");
                Wifi_DisconnectAP();
            }
            joined = join_network();
            if (!joined)
            {
                status("Could not join %s\nA: retry  START: exit\n", CFG_SSID);
                if (wait_keys(KEY_A | KEY_START) & KEY_START)
                    return 0;
                Wifi_DisconnectAP();
                continue;
            }
        }

        bool ok = relay_connect() && do_ping() && do_chat(CFG_MSG);
        while (ok)
        {
            status("Reply complete.\n\nA: send again\nB: ping\nSTART: exit\n");
            u32 k = wait_keys(KEY_A | KEY_B | KEY_START);
            if (k & KEY_START)
                return 0;
            if (k & KEY_A)
                ok = do_chat(CFG_MSG);
            else if (k & KEY_B)
                ok = do_ping();
        }

        if (sock >= 0)
        {
            close(sock);
            sock = -1;
        }
        status("Error, see top screen.\nA: reconnect  START: exit\n");
        if (wait_keys(KEY_A | KEY_START) & KEY_START)
            return 0;
    }
}
