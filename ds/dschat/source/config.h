// All user-changeable settings, in one struct.
//
// Sources, in this order: built-in defaults (make variables, dschat_build.h)
// -> the settings file on the SD card (storage.c) -> the Settings screen.
// The file format is plain "key = value" text, see config_serialise() and the
// README. Pure C (no libnds): parsing, serialising and validation are
// unit-tested natively in ../tests.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { SEC_OPEN = 0, SEC_WEP = 1 };
enum { VOICE_EN = 0, VOICE_IT = 1 };
// "en" / "it" (the code sent to the relay in VBEG)
const char *config_voice_code(int lang);

typedef struct
{
    char ssid[33];          // access point to join
    uint8_t security;       // SEC_OPEN or SEC_WEP (the DS Lite can't do WPA)
    char wep_key[27];       // as typed: 5/13 ASCII chars or 10/26 hex digits
    bool dhcp;              // false = static_ip/mask/gw below
    char static_ip[16];
    char static_mask[16];
    char static_gw[16];
    char relay_host[16];    // "" = the gateway (auto)
    uint16_t relay_port;
    char profile[17];       // last used relay profile ("" = relay default)
    uint8_t voice_lang;     // VOICE_EN / VOICE_IT: language of voice input
    const char *source;     // where the values came from (shown in About)
} dschat_config;

// Built-in defaults (from the make variables).
void config_defaults(dschat_config *c);

// Parses a settings file on top of the defaults. Bad lines or values keep
// the default for that key. Returns the number of problems; the first one is
// described in err ("line 4: bad IPv4 address for static_ip").
int config_parse(dschat_config *c, const char *text, char *err, size_t err_size);

// Writes the settings file (with comments). Returns its length, 0 if `cap`
// is too small.
size_t config_serialise(const dschat_config *c, char *out, size_t cap);

// Is the configuration usable for connecting? If not, why (short text).
bool config_validate(const dschat_config *c, char *why, size_t why_size);

// Strict dotted-quad IPv4 ("a.b.c.d", 0-255, no leading zeros, no spaces).
bool ipv4_parse(const char *s, uint8_t out[4]);
// A netmask: valid IPv4 with contiguous one bits (0.0.0.0 not allowed).
bool netmask_valid(const char *s);
// WEP key: 5 or 13 ASCII characters, or 10 or 26 hex digits. Writes the key
// bytes (5 or 13) to out and returns their count, or 0 if invalid.
int wep_key_bytes(const char *key, uint8_t out[13]);
