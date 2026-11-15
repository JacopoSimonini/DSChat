#include "check.h"
#include "proto.h"

static proto_parser p;

// Feeds a byte stream in chunks of `step` and collects "TYPE:payload|" for
// every frame. Returns -1 on a protocol error.
static int run(const char *stream, size_t n, size_t step, char *out, size_t cap)
{
    proto_init(&p);
    out[0] = '\0';
    size_t pos = 0;
    while (pos < n)
    {
        size_t k = step < n - pos ? step : n - pos;
        const uint8_t *d = (const uint8_t *)stream + pos;
        size_t off = 0;
        while (off < k)
        {
            size_t used;
            int r = proto_feed(&p, d + off, k - off, &used);
            off += used;
            if (r == PROTO_ERROR)
                return -1;
            if (r == PROTO_FRAME)
            {
                CHECK(strlen(p.payload) == p.len);   // NUL-terminated copy
                snprintf(out + strlen(out), cap - strlen(out), "%s:%s|", p.type, p.payload);
            }
        }
        pos += k;
    }
    return 0;
}

static void test_parser(void)
{
    const char stream[] = "PONG 0014\nDSChat relay 2TEXT 0005\nHelloTEXT 0006\n world"
                          "DONE 0008\nend_turnOKAY 0000\n";
    const char *want = "PONG:DSChat relay 2|TEXT:Hello|TEXT: world|DONE:end_turn|OKAY:|";
    char out[256];
    for (size_t step = 1; step <= sizeof(stream); step++)
    {
        CHECK(run(stream, sizeof(stream) - 1, step, out, sizeof(out)) == 0);
        CHECK_STR(out, want);
    }
}

static void test_parser_errors(void)
{
    char out[64];
    CHECK(run("hello world\n", 12, 3, out, sizeof(out)) == -1);
    CHECK(run("TEXT 9999\n", 10, 10, out, sizeof(out)) == -1);   // > 4096
    CHECK(run("TEXT 00a1\n", 10, 1, out, sizeof(out)) == -1);
    CHECK(run("TExT 0001\nx", 11, 4, out, sizeof(out)) == -1);
    CHECK(run("TEXT 0001 x", 11, 4, out, sizeof(out)) == -1);
    // maximum size is fine
    static char big[PROTO_HDR + PROTO_MAX_PAYLOAD];
    memcpy(big, "TEXT 4096\n", 10);
    memset(big + 10, 'z', PROTO_MAX_PAYLOAD);
    static char bigout[PROTO_MAX_PAYLOAD + 32];
    CHECK(run(big, sizeof(big), 777, bigout, sizeof(bigout)) == 0);
    CHECK(p.len == PROTO_MAX_PAYLOAD);
}

static void test_encode(void)
{
    char buf[64];
    size_t n = proto_encode(buf, sizeof(buf), "SEND", "Hello", 5);
    CHECK(n == 15 && memcmp(buf, "SEND 0005\nHello", 15) == 0);
    n = proto_encode(buf, sizeof(buf), "PING", NULL, 0);
    CHECK(n == 10 && memcmp(buf, "PING 0000\n", 10) == 0);
    CHECK(proto_encode(buf, 12, "SEND", "Hello", 5) == 0);          // no room
    CHECK(proto_encode(buf, sizeof(buf), "SND", "x", 1) == 0);      // bad type
}

static void test_pong(void)
{
    proto_server_info si;
    proto_parse_pong("DSChat relay 2", &si);
    CHECK(si.version == 2 && si.max_msg == 0 && si.boot[0] == '\0');
    proto_parse_pong("DSChat relay 3 max_msg=1024 boot=1a2b3c4d", &si);
    CHECK(si.version == 3 && si.max_msg == 1024);
    CHECK_STR(si.boot, "1a2b3c4d");
    proto_parse_pong("DSChat relay 4 future=x boot=ab max_msg=500", &si);
    CHECK(si.version == 4 && si.max_msg == 500);
    CHECK_STR(si.boot, "ab");
    proto_parse_pong("something else", &si);
    CHECK(si.version == 0);
}

static void test_profiles(void)
{
    char name[17], model[48];
    CHECK(proto_parse_name_model("gemini gemini-3.5-flash-lite", name, sizeof(name),
                                 model, sizeof(model)));
    CHECK_STR(name, "gemini");
    CHECK_STR(model, "gemini-3.5-flash-lite");
    CHECK(!proto_parse_name_model("nospace", name, sizeof(name), model, sizeof(model)));

    proto_profile pr[8];
    int n = proto_parse_plst("* claude claude-haiku-5-5\n+ gemini gemini-3.5-flash-lite\n"
                             "! qwen qwen3.8-flash\n+ ollama gemma3:4b", pr, 8);
    CHECK(n == 4);
    CHECK(pr[0].flag == '*');
    CHECK_STR(pr[0].name, "claude");
    CHECK_STR(pr[1].model, "gemini-3.5-flash-lite");
    CHECK(pr[2].flag == '!');
    CHECK_STR(pr[3].model, "gemma3:4b");
    CHECK(proto_parse_plst("* a m\n+ b n\n+ c o", pr, 2) == 2);   // max respected
    CHECK(proto_parse_plst("", pr, 8) == 0);
    CHECK(proto_parse_plst("garbage\n+ ok model", pr, 8) == 1);
}

static void test_v4(void)
{
    proto_server_info si;
    proto_parse_pong("DSChat relay 4 max_msg=1024 boot=ab voice=1", &si);
    CHECK(si.version == 4 && si.voice && si.max_msg == 1024);
    proto_parse_pong("DSChat relay 4 max_msg=1024 boot=ab voice=0", &si);
    CHECK(!si.voice);
    proto_parse_pong("DSChat relay 3 max_msg=1024 boot=ab", &si);      // old relay
    CHECK(!si.voice);
    proto_parse_pong("DSChat relay 4 voice=1x", &si);                  // only exactly 1
    CHECK(si.voice);   // atoi("1x") == 1: accepted (lenient, like max_msg)

    proto_vtxt v;
    proto_parse_vtxt("ms=2010 audio=3200 bytes=25600 rx=3150\nTell me the capital of France.", &v);
    CHECK(v.stt_ms == 2010 && v.audio_ms == 3200 && v.bytes == 25600 && v.rx_ms == 3150);
    CHECK_STR(v.text, "Tell me the capital of France.");
    proto_parse_vtxt("ms=900 audio=5000 bytes=40000 rx=5000\n", &v);   // nothing heard
    CHECK(v.stt_ms == 900);
    CHECK_STR(v.text, "");
    proto_parse_vtxt("future=1 ms=5\nline one\nline two", &v);        // unknown key, multi-line text
    CHECK(v.stt_ms == 5);
    CHECK_STR(v.text, "line one\nline two");
    proto_parse_vtxt("no newline at all", &v);
    CHECK(v.stt_ms == 0);
    CHECK_STR(v.text, "");
    proto_parse_vtxt("", &v);
    CHECK_STR(v.text, "");
    // v4 frames encode like any other frame (binary-safe payload)
    char out[64];
    const char blk[5] = { 0x10, 0x00, 0x05, 0x00, (char)0xAB };
    CHECK(proto_encode(out, sizeof(out), "VDAT", blk, 5) == 15);
    CHECK(memcmp(out, "VDAT 0005\n", 10) == 0 && memcmp(out + 10, blk, 5) == 0);
    CHECK(proto_encode(out, sizeof(out), "VEND", NULL, 0) == 10);
}

void test_proto(void)
{
    test_v4();
    test_parser();
    test_parser_errors();
    test_encode();
    test_pong();
    test_profiles();
}
