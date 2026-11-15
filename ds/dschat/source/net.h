// Wi-Fi + relay connection as a state machine polled from the main loop
// (never blocks). Joins the AP, connects TCP, says hello (PING + PGET),
// keeps the connection alive, detects loss and reconnects with backoff.
//
// Requests are pipelined in order (the relay answers strictly in order), so
// the app can send at any time while online; replies are matched to the
// request queue and handed to the app through the callbacks below.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"
#include "proto.h"

enum net_state
{
    NS_OFF = 0,          // Wi-Fi could not be initialised
    NS_SCAN,             // looking for the SSID
    NS_ASSOC,            // associating + DHCP
    NS_CONNECT,          // TCP connect to the relay
    NS_HELLO,            // PING / PGET exchange
    NS_ONLINE,
    NS_RETRY,            // waiting before the next attempt
    NS_RADIO_RESET,      // Wi-Fi hardware power-cycled (radio seemed stuck)
    NS_IDLE,             // not started / stopped (settings, before the first start)
};

// Stages of "Test connection" (net_start(cfg, true)), reported in order
// through app_net_test(); a failing stage ends the test.
enum test_stage { TS_FOUND = 1, TS_JOINED, TS_IP, TS_RELAY, TS_DONE };

typedef struct
{
    char mac[18];
    char ip[16];
    char relay[24];      // "a.b.c.d:port"
    bool static_ip;
    char wifi[40];       // DSWiFi packet counters: queued / sent / rejected TX, RX
    char stats[40];      // conns, bytes sent/received, queue, pending tx, last send()/errno
} net_info_t;

bool net_init(const dschat_config *cfg);      // false: Wi-Fi init failed; then NS_IDLE
// Start connecting with these settings (they must stay valid). test = one
// attempt with stage reports and no retries; it stops by itself.
void net_start(const dschat_config *cfg, bool test);
// Close the connection and leave the AP; no callbacks. NS_IDLE afterwards.
void net_stop(void);
// Before powering off: flush pending frames, close, leave the AP, radio off.
void net_shutdown(void);
int net_failures(void);                       // consecutive failed attempts
void net_poll(uint32_t now);                  // call once per frame
int net_state(void);
int net_scan_count(void);                     // APs seen while scanning
bool net_online(void);
int net_retry_seconds(uint32_t now);          // NS_RETRY: seconds left
const proto_server_info *net_server(void);    // valid once online
void net_get_info(net_info_t *info);
// DSWiFi packet counters: queued for TX, sent by the hardware, received.
void net_wifi_counters(uint32_t *q, uint32_t *t, uint32_t *r);

// Queue a request (tag is returned with its reply frames). SEND replies
// arrive as several frames (TEXT..., then DONE or FAIL); every other request
// gets exactly one frame (VEND may take long: same timeout and LIVE probing
// as SEND). Returns false if offline or the queue is full.
bool net_request(int tag, const char *type, const char *payload, size_t len);
// A frame that gets no reply (v4 VBEG / VDAT). False if offline or the TX
// buffer is full (try again next frame).
bool net_send_noreply(const char *type, const void *payload, size_t len);
// Bytes written by the app but not yet handed to the TCP stack.
size_t net_tx_pending(void);
// Upload pacing: at most this many bytes to send() per frame (0 = no limit;
// default 1260 = one TCP segment, see net.c).
void net_set_tx_pace(int bytes_per_frame);
// Protocol v3 cancel: sends CNCL (no reply of its own). Only if online.
bool net_cancel(void);
// Drop the connection and rejoin Wi-Fi from scratch.
void net_reconnect(void);
// The console woke up (lid opened): check the connection right away.
void net_wake(uint32_t now);

// Implemented by the app:
void app_net_online(const proto_server_info *si, const char *profile, const char *model);
void app_net_lost(const char *reason);
void app_net_frame(int tag, const char *type, const char *payload, int len);
void app_net_notice(const char *text);        // one-off progress / errors
void app_net_test(int stage, bool ok, const char *msg);   // test connection
