// DSChat wire protocol (docs/PROTOCOL.md): incremental frame parser, frame
// encoder and payload parsers. Pure C, unit-tested natively.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PROTO_HDR          10
#define PROTO_MAX_PAYLOAD  4096

typedef struct
{
    uint8_t hdr[PROTO_HDR];
    uint16_t hlen;          // header bytes received
    uint16_t need;          // payload length announced by the header
    uint16_t got;           // payload bytes received
    bool in_payload;
    // Completed frame (valid after proto_feed() returned PROTO_FRAME):
    char type[5];
    char payload[PROTO_MAX_PAYLOAD + 1];   // NUL-terminated copy
    uint16_t len;
} proto_parser;

enum
{
    PROTO_ERROR = -1,       // malformed header or too long: drop the connection
    PROTO_NEED_MORE = 0,    // all input consumed, no complete frame yet
    PROTO_FRAME = 1,        // a frame is ready; *used tells how much was eaten
};

void proto_init(proto_parser *p);
// Feeds bytes. Stops right after a complete frame so the caller can handle
// it, then call again with the rest (data + *used, n - *used).
int proto_feed(proto_parser *p, const uint8_t *data, size_t n, size_t *used);

// Writes header + payload to out. Returns the frame size, or 0 if it does
// not fit or the payload is too long.
size_t proto_encode(char *out, size_t cap, const char *type, const char *payload,
                    size_t len);

// PONG payload: "DSChat relay <version>[ key=value ...]" (v3 adds max_msg
// and boot, v4 adds voice
// and boot). Unknown fields are ignored; missing ones stay 0 / "".
typedef struct
{
    int version;            // 0 = not a DSChat relay
    int max_msg;
    char boot[17];
    bool voice;             // v4: "voice=1" = VBEG/VDAT/VEND transcription available
} proto_server_info;

void proto_parse_pong(const char *payload, proto_server_info *out);

// "<name> <model>" (PGET INFO and PSET OKAY payloads).
bool proto_parse_name_model(const char *payload, char *name, size_t name_size,
                            char *model, size_t model_size);

// PLST INFO payload: lines "<flag> <name> <model>".
typedef struct
{
    char flag;              // '*' current, '+' ready, '!' no API key
    char name[17];
    char model[48];
} proto_profile;

int proto_parse_plst(const char *payload, proto_profile *out, int max);

// VTXT payload (v4): a first line of key=value stats, then the transcript
// (may be empty): "ms=<transcription ms> audio=<ms> bytes=<n> rx=<ms>\n<text>".
typedef struct
{
    uint32_t stt_ms;        // transcription time on the laptop
    uint32_t audio_ms;      // audio the relay decoded
    uint32_t bytes;         // ADPCM bytes it received
    uint32_t rx_ms;         // first to last VDAT at the relay
    const char *text;       // points into the payload; "" when nothing heard
} proto_vtxt;

void proto_parse_vtxt(const char *payload, proto_vtxt *out);
