#include <string.h>

#include "textbuf.h"

void tb_init(textbuf *tb)
{
    memset(tb, 0, sizeof(*tb));
}

void tb_clear(textbuf *tb)
{
    // Keep the numbering monotonic so views holding line numbers stay valid.
    uint32_t total = tb->total;
    tb_init(tb);
    tb->total = total;
    tb->base = total;
}

static tb_line *last_line(textbuf *tb)
{
    return &tb->lines[(tb->total - 1) % TB_LINES];
}

static tb_line *new_line(textbuf *tb)
{
    tb_line *l = &tb->lines[tb->total % TB_LINES];
    tb->total++;
    l->len = 0;
    l->kind = tb->kind;
    return l;
}

void tb_begin(textbuf *tb, uint8_t kind)
{
    tb->kind = kind;
    new_line(tb);
    tb->open = true;
}

void tb_end(textbuf *tb)
{
    tb->open = false;
}

static void trim_right(tb_line *l)
{
    while (l->len > 0 && l->text[l->len - 1] == ' ')
        l->len--;
}

static void put_char(textbuf *tb, char c)
{
    if (c == '\r')
        return;
    if (c == '\n')
    {
        new_line(tb);
        return;
    }
    if (c == '\t')
        c = ' ';
    if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7E)
        c = '?';

    tb_line *l = last_line(tb);

    if (c == ' ')
    {
        if (l->len < TB_COLS)
            l->text[l->len++] = ' ';
        else
            new_line(tb);           // the space is the wrap point: drop it
        return;
    }

    if (l->len < TB_COLS)
    {
        l->text[l->len++] = c;
        return;
    }

    // The line is full and a word continues: move that word to a new line,
    // unless it fills the whole line already (then it must be split).
    int sp = -1;
    for (int i = l->len - 1; i >= 0; i--)
    {
        if (l->text[i] == ' ')
        {
            sp = i;
            break;
        }
    }

    if (sp >= 0)
    {
        char word[TB_COLS];
        int n = l->len - sp - 1;
        memcpy(word, &l->text[sp + 1], n);
        l->len = sp;
        trim_right(l);
        tb_line *nl = new_line(tb);
        memcpy(nl->text, word, n);
        nl->len = n;
        nl->text[nl->len++] = c;
    }
    else
    {
        tb_line *nl = new_line(tb);
        nl->text[nl->len++] = c;
    }
}

void tb_append(textbuf *tb, const char *s, size_t n)
{
    if (!tb->open)
        tb_begin(tb, tb->kind);
    for (size_t i = 0; i < n; i++)
        put_char(tb, s[i]);
}

void tb_puts(textbuf *tb, uint8_t kind, const char *s)
{
    tb_begin(tb, kind);
    tb_append(tb, s, strlen(s));
    tb_end(tb);
}

void tb_blank(textbuf *tb)
{
    if (tb->total > 0 && last_line(tb)->len == 0)
    {
        tb->open = false;
        return;
    }
    new_line(tb);
    tb->open = false;
}

uint32_t tb_first(const textbuf *tb)
{
    uint32_t first = tb->total > TB_LINES ? tb->total - TB_LINES : 0;
    return first > tb->base ? first : tb->base;
}

const tb_line *tb_get(const textbuf *tb, uint32_t n)
{
    if (n < tb_first(tb) || n >= tb->total)
        return NULL;
    return &tb->lines[n % TB_LINES];
}
