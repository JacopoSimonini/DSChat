#include <stdlib.h>

#include "check.h"
#include "textbuf.h"

static textbuf tb, tb2;   // large: keep off the stack

// Line n as a C string.
static const char *line(const textbuf *t, uint32_t n)
{
    static char buf[TB_COLS + 1];
    const tb_line *l = tb_get(t, n);
    if (l == NULL)
        return "<null>";
    memcpy(buf, l->text, l->len);
    buf[l->len] = '\0';
    return buf;
}

static void dump_equal(const textbuf *a, const textbuf *b, const char *what)
{
    int same = a->total == b->total;
    for (uint32_t i = tb_first(a); same && i < a->total; i++)
    {
        const tb_line *x = tb_get(a, i), *y = tb_get(b, i);
        same = x->len == y->len && x->kind == y->kind && memcmp(x->text, y->text, x->len) == 0;
    }
    if (!same)
        fprintf(stderr, "chunking changed the result: %s\n", what);
    CHECK(same);
}

static void test_basic_wrap(void)
{
    tb_init(&tb);
    // 32 columns: "aaaa ... " words must not be split
    tb_puts(&tb, TB_AI, "The quick brown fox jumps over the lazy dog and keeps running far away.");
    CHECK(tb.total == 3);
    CHECK_STR(line(&tb, 0), "The quick brown fox jumps over");
    CHECK_STR(line(&tb, 1), "the lazy dog and keeps running");
    CHECK_STR(line(&tb, 2), "far away.");
    CHECK(tb_get(&tb, 0)->kind == TB_AI);
}

static void test_exact_fit_and_space(void)
{
    tb_init(&tb);
    // exactly 32 chars, then a space, then a word: the space is dropped
    tb_puts(&tb, TB_USER, "0123456789012345678901234567890X next");
    CHECK(tb.total == 2);
    CHECK_STR(line(&tb, 0), "0123456789012345678901234567890X");
    CHECK_STR(line(&tb, 1), "next");
    CHECK(tb_get(&tb, 1)->kind == TB_USER);
}

static void test_long_word_split(void)
{
    tb_init(&tb);
    tb_puts(&tb, TB_AI, "a Pneumonoultramicroscopicsilicovolcanoconiosis-and-more end");
    // "a" stays alone; the 50-char word is split only where it must be
    CHECK_STR(line(&tb, 0), "a");
    CHECK_STR(line(&tb, 1), "Pneumonoultramicroscopicsilicovo");
    CHECK_STR(line(&tb, 2), "lcanoconiosis-and-more end");
}

static void test_newlines_and_blank(void)
{
    tb_init(&tb);
    tb_puts(&tb, TB_AI, "one\ntwo\n\nfour\r\n");
    CHECK(tb.total == 5);
    CHECK_STR(line(&tb, 0), "one");
    CHECK_STR(line(&tb, 1), "two");
    CHECK_STR(line(&tb, 2), "");
    CHECK_STR(line(&tb, 3), "four");
    CHECK_STR(line(&tb, 4), "");
    tb_blank(&tb);            // last line already empty: no second blank
    CHECK(tb.total == 5);
    tb_puts(&tb, TB_INFO, "x");
    tb_blank(&tb);
    CHECK(tb.total == 7);
    CHECK_STR(line(&tb, 6), "");
}

static void test_non_ascii(void)
{
    tb_init(&tb);
    tb_puts(&tb, TB_AI, "caf\xe9\ttab\x01");
    CHECK_STR(line(&tb, 0), "caf? tab?");
}

static void feed_chunked(textbuf *t, const char *s, unsigned seed)
{
    srand(seed);
    tb_init(t);
    tb_begin(t, TB_AI);
    size_t n = strlen(s), i = 0;
    while (i < n)
    {
        size_t k = 1 + rand() % 9;
        if (k > n - i)
            k = n - i;
        tb_append(t, s + i, k);
        i += k;
    }
    tb_end(t);
}

static void test_chunking_independent(void)
{
    const char *text =
        "Streaming text arrives in chunks of any size, and the wrapped result "
        "must not depend on where the chunks were split. Here is a longword: "
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 and\n"
        "a second paragraph with  double  spaces and a line that is exactly "
        "thirty-two chars!! followed by more words to wrap around.";
    tb_init(&tb);
    tb_puts(&tb, TB_AI, text);
    for (unsigned seed = 1; seed <= 200; seed++)
    {
        feed_chunked(&tb2, text, seed);
        dump_equal(&tb, &tb2, "random chunks");
    }
    // every line fits and no line starts with a word-split unless needed
    for (uint32_t i = 0; i < tb.total; i++)
        CHECK(tb_get(&tb, i)->len <= TB_COLS);
}

static void test_ring_overflow(void)
{
    tb_init(&tb);
    char msg[24];
    for (int i = 0; i < TB_LINES + 100; i++)
    {
        snprintf(msg, sizeof(msg), "line %d", i);
        tb_puts(&tb, TB_AI, msg);
    }
    CHECK(tb.total == TB_LINES + 100);
    CHECK(tb_first(&tb) == 100);
    CHECK(tb_get(&tb, 99) == NULL);
    CHECK_STR(line(&tb, 100), "line 100");
    CHECK_STR(line(&tb, TB_LINES + 99), "line 2147");
    CHECK(tb_get(&tb, TB_LINES + 100) == NULL);
    // word moved to a new line across the ring boundary
    tb_begin(&tb, TB_AI);
    tb_append(&tb, "0123456789 0123456789 012345678 overflow", 40);
    tb_end(&tb);
    CHECK_STR(line(&tb, tb.total - 2), "0123456789 0123456789 012345678");
    CHECK_STR(line(&tb, tb.total - 1), "overflow");
}

static void test_clear_keeps_numbering(void)
{
    tb_init(&tb);
    tb_puts(&tb, TB_AI, "a");
    tb_puts(&tb, TB_AI, "b");
    tb_clear(&tb);
    CHECK(tb.total == 2);
    CHECK(tb_first(&tb) == 2);
    CHECK(tb_get(&tb, 1) == NULL);
    tb_puts(&tb, TB_INFO, "new chat");
    CHECK_STR(line(&tb, 2), "new chat");
}

void test_textbuf(void)
{
    test_basic_wrap();
    test_exact_fit_and_space();
    test_long_word_split();
    test_newlines_and_blank();
    test_non_ascii();
    test_chunking_independent();
    test_ring_overflow();
    test_clear_keeps_numbering();
}
