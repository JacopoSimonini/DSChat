#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "proto.h"

void proto_init(proto_parser *p)
{
    p->hlen = 0;
    p->need = 0;
    p->got = 0;
    p->in_payload = false;
    p->type[0] = '\0';
    p->payload[0] = '\0';
    p->len = 0;
}

static bool header_ok(const uint8_t *h, uint16_t *len)
{
    for (int i = 0; i < 4; i++)
    {
        if (h[i] < 'A' || h[i] > 'Z')
            return false;
    }
    if (h[4] != ' ' || h[9] != '\n')
        return false;
    unsigned v = 0;
    for (int i = 5; i < 9; i++)
    {
        if (h[i] < '0' || h[i] > '9')
            return false;
        v = v * 10 + (h[i] - '0');
    }
    if (v > PROTO_MAX_PAYLOAD)
        return false;
    *len = v;
    return true;
}

static void finish(proto_parser *p)
{
    memcpy(p->type, p->hdr, 4);
    p->type[4] = '\0';
    p->payload[p->got] = '\0';
    p->len = p->got;
    p->hlen = 0;
    p->got = 0;
    p->need = 0;
    p->in_payload = false;
}

int proto_feed(proto_parser *p, const uint8_t *data, size_t n, size_t *used)
{
    size_t i = 0;
    while (i < n)
    {
        if (!p->in_payload)
        {
            p->hdr[p->hlen++] = data[i++];
            if (p->hlen < PROTO_HDR)
                continue;
            if (!header_ok(p->hdr, &p->need))
            {
                *used = i;
                return PROTO_ERROR;
            }
            p->got = 0;
            if (p->need == 0)
            {
                finish(p);
                *used = i;
                return PROTO_FRAME;
            }
            p->in_payload = true;
            continue;
        }
        size_t take = p->need - p->got;
        if (take > n - i)
            take = n - i;
        memcpy(&p->payload[p->got], &data[i], take);
        p->got += take;
        i += take;
        if (p->got == p->need)
        {
            finish(p);
            *used = i;
            return PROTO_FRAME;
        }
    }
    *used = i;
    return PROTO_NEED_MORE;
}

size_t proto_encode(char *out, size_t cap, const char *type, const char *payload,
                    size_t len)
{
    if (len > PROTO_MAX_PAYLOAD || cap < PROTO_HDR + len || strlen(type) != 4)
        return 0;
    char hdr[PROTO_HDR + 1];
    snprintf(hdr, sizeof(hdr), "%.4s %04u\n", type, (unsigned)len);
    memcpy(out, hdr, PROTO_HDR);
    if (len)
        memcpy(out + PROTO_HDR, payload, len);
    return PROTO_HDR + len;
}

static void copy_field(char *dst, size_t size, const char *src, size_t n)
{
    if (size == 0)
        return;
    if (n >= size)
        n = size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void proto_parse_pong(const char *payload, proto_server_info *out)
{
    memset(out, 0, sizeof(*out));
    static const char prefix[] = "DSChat relay ";
    if (strncmp(payload, prefix, sizeof(prefix) - 1) != 0)
        return;
    const char *s = payload + sizeof(prefix) - 1;
    out->version = atoi(s);
    // key=value fields after the version, separated by spaces
    while ((s = strchr(s, ' ')) != NULL)
    {
        s++;
        size_t n = strcspn(s, " \n");
        if (strncmp(s, "max_msg=", 8) == 0)
            out->max_msg = atoi(s + 8);
        else if (strncmp(s, "boot=", 5) == 0)
            copy_field(out->boot, sizeof(out->boot), s + 5, n - 5);
        else if (strncmp(s, "voice=", 6) == 0)
            out->voice = atoi(s + 6) == 1;
    }
}

void proto_parse_vtxt(const char *payload, proto_vtxt *out)
{
    memset(out, 0, sizeof(*out));
    const char *nl = strchr(payload, '\n');
    size_t head = nl ? (size_t)(nl - payload) : strlen(payload);
    char line[96];
    copy_field(line, sizeof(line), payload, head);
    for (char *tok = strtok(line, " "); tok != NULL; tok = strtok(NULL, " "))
    {
        char *eq = strchr(tok, '=');
        if (eq == NULL)
            continue;
        *eq = '\0';
        uint32_t v = (uint32_t)strtoul(eq + 1, NULL, 10);
        if (strcmp(tok, "ms") == 0) out->stt_ms = v;
        else if (strcmp(tok, "audio") == 0) out->audio_ms = v;
        else if (strcmp(tok, "bytes") == 0) out->bytes = v;
        else if (strcmp(tok, "rx") == 0) out->rx_ms = v;
    }
    out->text = nl ? nl + 1 : "";
}

bool proto_parse_name_model(const char *payload, char *name, size_t name_size,
                            char *model, size_t model_size)
{
    const char *sp = strchr(payload, ' ');
    if (sp == NULL || sp == payload)
        return false;
    copy_field(name, name_size, payload, sp - payload);
    copy_field(model, model_size, sp + 1, strcspn(sp + 1, "\n"));
    return true;
}

int proto_parse_plst(const char *payload, proto_profile *out, int max)
{
    int count = 0;
    const char *line = payload;
    while (*line && count < max)
    {
        size_t n = strcspn(line, "\n");
        // "<flag> <name> <model>"
        if (n >= 3 && line[1] == ' ')
        {
            const char *name = line + 2;
            const char *end = line + n;
            const char *sp = memchr(name, ' ', end - name);
            if (sp != NULL && sp > name)
            {
                proto_profile *p = &out[count++];
                p->flag = line[0];
                copy_field(p->name, sizeof(p->name), name, sp - name);
                copy_field(p->model, sizeof(p->model), sp + 1, end - sp - 1);
            }
        }
        line += n;
        if (*line == '\n')
            line++;
    }
    return count;
}
