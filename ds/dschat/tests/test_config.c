#include "check.h"
#include "config.h"

static void test_ipv4(void)
{
    uint8_t b[4];
    CHECK(ipv4_parse("192.168.77.1", b) && b[0] == 192 && b[3] == 1);
    CHECK(ipv4_parse("0.0.0.0", b));
    CHECK(ipv4_parse("255.255.255.255", b));
    const char *bad[] = { "", "1.2.3", "192.0.2.4.5", "256.1.1.1", "1.2.3.-4", "1..2.3",
                          "0192.0.2.4", "1.2.3.04", " 192.0.2.4", "192.0.2.4 ", "a.b.c.d",
                          "192.0.2.4x", "1234.1.1.1", "1.2.3." };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
    {
        if (ipv4_parse(bad[i], b))
            fprintf(stderr, "accepted bad IPv4 '%s'\n", bad[i]);
        CHECK(!ipv4_parse(bad[i], b));
    }
    CHECK(netmask_valid("255.255.255.0"));
    CHECK(netmask_valid("255.255.255.255"));
    CHECK(netmask_valid("255.0.0.0"));
    CHECK(netmask_valid("255.255.252.0"));
    CHECK(!netmask_valid("0.0.0.0"));
    CHECK(!netmask_valid("255.0.255.0"));
    CHECK(!netmask_valid("255.255.255.1"));
}

static void test_wep(void)
{
    uint8_t k[13];
    CHECK(wep_key_bytes("abcde", k) == 5 && k[0] == 'a');
    CHECK(wep_key_bytes("abcdefghijklm", k) == 13);
    CHECK(wep_key_bytes("0123456789", k) == 5 && k[0] == 0x01 && k[4] == 0x89);
    CHECK(wep_key_bytes("0123456789abcdefABCDEF0123", k) == 13 && k[5] == 0xAB && k[12] == 0x23);
    CHECK(wep_key_bytes("", k) == 0);
    CHECK(wep_key_bytes("abcd", k) == 0);
    CHECK(wep_key_bytes("abcdef", k) == 0);
    CHECK(wep_key_bytes("012345678g", k) == 0);                 // 10 chars, not hex
    CHECK(wep_key_bytes("0123456789abcdef0123456789ab", k) == 0);  // 28 hex
    CHECK(wep_key_bytes("abc\x01e", k) == 0);
}

static char buf[2048], err[96];

static void test_roundtrip(void)
{
    dschat_config a, b;
    config_defaults(&a);
    CHECK(a.dhcp && a.relay_port == 4242 && a.relay_host[0] == '\0');
    CHECK_STR(a.ssid, "DSCHAT");
    CHECK(config_validate(&a, err, sizeof(err)));

    snprintf(a.ssid, sizeof(a.ssid), "My Net #1 = home");   // spaces, '#', '='
    a.security = SEC_WEP;
    snprintf(a.wep_key, sizeof(a.wep_key), "0123456789");
    a.dhcp = false;
    snprintf(a.static_ip, 16, "198.51.100.60");
    snprintf(a.static_mask, 16, "255.255.255.0");
    snprintf(a.static_gw, 16, "198.51.100.1");
    snprintf(a.relay_host, 16, "198.51.100.2");
    a.relay_port = 5000;
    snprintf(a.profile, sizeof(a.profile), "gemini");
    size_t n = config_serialise(&a, buf, sizeof(buf));
    CHECK(n > 0 && n < sizeof(buf));
    CHECK(config_parse(&b, buf, err, sizeof(err)) == 0);
    CHECK_STR(b.ssid, "My Net #1 = home");
    CHECK(b.security == SEC_WEP);
    CHECK_STR(b.wep_key, "0123456789");
    CHECK(!b.dhcp);
    CHECK_STR(b.static_ip, "198.51.100.60");
    CHECK_STR(b.static_gw, "198.51.100.1");
    CHECK_STR(b.relay_host, "198.51.100.2");
    CHECK(b.relay_port == 5000);
    CHECK_STR(b.profile, "gemini");
    CHECK(config_validate(&b, err, sizeof(err)));
    CHECK(config_serialise(&a, buf, 100) == 0);              // too small

    // auto relay is written as "auto" and read back as ""
    a.relay_host[0] = '\0';
    config_serialise(&a, buf, sizeof(buf));
    CHECK(strstr(buf, "relay_host = auto\n") != NULL);
    CHECK(config_parse(&b, buf, err, sizeof(err)) == 0 && b.relay_host[0] == '\0');
}

static void test_corrupt(void)
{
    dschat_config c;
    // empty / comments only -> defaults, no problems
    CHECK(config_parse(&c, "", err, sizeof(err)) == 0);
    CHECK_STR(c.ssid, "DSCHAT");
    CHECK(config_parse(&c, "# nothing\n\n   \n", err, sizeof(err)) == 0);

    // bad values keep the default for that key, others still apply
    int p = config_parse(&c,
                         "ssid = Home\r\n"
                         "ip = static\n"
                         "static_ip = 192.168.1.300\n"
                         "relay_port = 99999\n"
                         "garbage line\n"
                         "colour = blue\n"
                         "security = wpa\n", err, sizeof(err));
    CHECK(p == 5);
    CHECK_STR(err, "line 3: bad IPv4 address for static_ip");
    CHECK_STR(c.ssid, "Home");                   // CRLF tolerated
    CHECK(!c.dhcp);
    CHECK_STR(c.static_ip, "192.168.77.60");     // default kept
    CHECK(c.relay_port == 4242);
    CHECK(c.security == SEC_OPEN);

    // binary junk, overlong line, missing '='
    static char junk[600];
    memset(junk, 'x', sizeof(junk) - 1);
    junk[sizeof(junk) - 1] = '\0';
    CHECK(config_parse(&c, junk, err, sizeof(err)) == 1);
    CHECK(config_parse(&c, "ssid = a\x01b\n", err, sizeof(err)) == 1);
    CHECK(config_parse(&c, "\xff\xfe\x00", err, sizeof(err)) == 1);
    CHECK(config_parse(&c, "ssid =\n", err, sizeof(err)) == 1);
    CHECK(config_parse(&c, "ssid = 123456789012345678901234567890123\n", err, sizeof(err)) == 1);
    CHECK(config_parse(&c, "profile = Bad Name\n", err, sizeof(err)) == 1);
    CHECK(config_parse(&c, "relay_host = auto\nrelay_port = 80\n", err, sizeof(err)) == 0
          && c.relay_port == 80);
}

static void test_validate(void)
{
    dschat_config c;
    config_defaults(&c);
    c.security = SEC_WEP;
    CHECK(!config_validate(&c, err, sizeof(err)));          // no key
    CHECK_STR(err, "WEP key missing");
    snprintf(c.wep_key, sizeof(c.wep_key), "abcd");
    CHECK(!config_validate(&c, err, sizeof(err)));          // bad key
    CHECK_STR(err, "WEP key: 5/13 chars, 10/26 hex");
    snprintf(c.wep_key, sizeof(c.wep_key), "0123456789abcdef0123456789");  // 26 hex
    CHECK(config_validate(&c, err, sizeof(err)));
    snprintf(c.wep_key, sizeof(c.wep_key), "abcde");
    CHECK(config_validate(&c, err, sizeof(err)));
    c.dhcp = false;
    snprintf(c.static_ip, 16, "192.168.77.60");
    snprintf(c.static_gw, 16, "198.51.100.1");
    CHECK(!config_validate(&c, err, sizeof(err)));          // gw outside subnet
    CHECK_STR(err, "gateway not in the IP's subnet");
    snprintf(c.static_gw, 16, "192.168.77.60");
    CHECK(!config_validate(&c, err, sizeof(err)));          // same as IP
    snprintf(c.static_gw, 16, "192.168.77.1");
    CHECK(config_validate(&c, err, sizeof(err)));
    c.ssid[0] = '\0';
    CHECK(!config_validate(&c, err, sizeof(err)));
    CHECK_STR(err, "no network (SSID) chosen");
}

static void test_voice_lang(void)
{
    dschat_config c, d;
    config_defaults(&c);
    CHECK(c.voice_lang == VOICE_EN);
    CHECK_STR(config_voice_code(c.voice_lang), "en");
    c.voice_lang = VOICE_IT;
    size_t n = config_serialise(&c, buf, sizeof(buf));
    CHECK(n > 0 && strstr(buf, "voice_lang = it\n") != NULL);
    CHECK(config_parse(&d, buf, err, sizeof(err)) == 0);
    CHECK(d.voice_lang == VOICE_IT);
    CHECK_STR(config_voice_code(d.voice_lang), "it");
    CHECK(config_parse(&d, "voice_lang = fr\n", err, sizeof(err)) == 1);
    CHECK(strstr(err, "voice_lang must be en or it") != NULL);
    CHECK(d.voice_lang == VOICE_EN);                    // bad value keeps the default
    CHECK(config_parse(&d, "voice_lang = en\n", err, sizeof(err)) == 0 && d.voice_lang == VOICE_EN);
    // a 1.0 file (no voice_lang line) still loads, default English
    CHECK(config_parse(&d, "ssid = DSCHAT\nprofile = gemini\n", err, sizeof(err)) == 0);
    CHECK(d.voice_lang == VOICE_EN);
}

void test_config(void)
{
    test_voice_lang();
    test_ipv4();
    test_wep();
    test_roundtrip();
    test_corrupt();
    test_validate();
}
