#include "check.h"
#include "input.h"

static input_t in;

static void type(const char *s)
{
    while (*s)
        input_insert(&in, *s++);
}

static void test_editing(void)
{
    input_init(&in, 1024);
    CHECK(input_blank(&in));
    type("helo");
    CHECK_STR(in.buf, "helo");
    input_left(&in);
    input_insert(&in, 'l');
    CHECK_STR(in.buf, "hello");
    CHECK(in.cur == 4);
    input_end(&in);
    type(" world");
    CHECK_STR(in.buf, "hello world");
    input_home(&in);
    CHECK(!input_backspace(&in));            // nothing before the cursor
    input_right(&in);
    CHECK(input_backspace(&in));
    CHECK_STR(in.buf, "ello world");
    CHECK(in.cur == 0);
    CHECK(!input_insert(&in, '\n'));          // not printable
    CHECK(!input_insert(&in, (char)0xE9));
    input_clear(&in);
    CHECK(in.len == 0 && in.cur == 0 && in.buf[0] == '\0');
    type("   ");
    CHECK(input_blank(&in));
}

static void test_limits(void)
{
    input_init(&in, 5);
    type("abcdefg");
    CHECK_STR(in.buf, "abcde");
    CHECK(!input_insert(&in, 'x'));
    input_set_max(&in, 0);                    // 0 = default cap
    CHECK(in.max == INPUT_CAP);
    input_set_max(&in, 3);                    // lowering keeps the text
    CHECK_STR(in.buf, "abcde");
    CHECK(!input_insert(&in, 'x'));
    input_set_max(&in, 5000);
    CHECK(in.max == INPUT_CAP);
    input_set(&in, "restored message");
    CHECK(in.len == 16 && in.cur == 16);
    // fill to the cap
    input_init(&in, INPUT_CAP);
    for (int i = 0; i < INPUT_CAP + 10; i++)
        input_insert(&in, 'a');
    CHECK(in.len == INPUT_CAP && in.buf[INPUT_CAP] == '\0');
}

static input_row rows[INPUT_CAP + 2];

static void test_layout(void)
{
    int r, c, n;
    input_init(&in, 1024);
    n = input_layout(&in, 32, rows, 64, &r, &c);
    CHECK(n == 1 && r == 0 && c == 0);

    type("The quick brown fox jumps over the lazy dog");
    n = input_layout(&in, 32, rows, 64, &r, &c);
    // row 0 keeps its trailing space: "The quick brown fox jumps over "
    CHECK(n == 2);
    CHECK(rows[0].start == 0 && rows[0].len == 31);
    CHECK(rows[1].start == 31 && rows[1].len == 12);
    CHECK(r == 1 && c == 12);
    input_home(&in);
    input_layout(&in, 32, rows, 64, &r, &c);
    CHECK(r == 0 && c == 0);
    in.cur = 31;                               // first char of row 1
    input_layout(&in, 32, rows, 64, &r, &c);
    CHECK(r == 1 && c == 0);

    // exactly one full row and the cursor at the end -> extra empty row
    input_clear(&in);
    type("0123456789012345678901234567890X");
    n = input_layout(&in, 32, rows, 64, &r, &c);
    CHECK(n == 2 && r == 1 && c == 0 && rows[1].len == 0);

    // long word is split; every character belongs to exactly one row
    input_clear(&in);
    type("a abcdefghijklmnopqrstuvwxyzabcdefghijklmnop b");
    n = input_layout(&in, 32, rows, 64, &r, &c);
    int sum = 0;
    for (int i = 0; i < n; i++)
    {
        CHECK(rows[i].len <= 32);
        CHECK(rows[i].start == sum);
        sum += rows[i].len;
    }
    CHECK(sum == in.len);
    CHECK(rows[0].len == 2);                   // "a " then the long word

    // a word ending exactly at the row end: next row starts with the space
    input_clear(&in);
    type("0123456789012345678901234567890X yes");
    n = input_layout(&in, 32, rows, 64, &r, &c);
    CHECK(n == 2 && rows[0].len == 32 && rows[1].start == 32);

    // max_rows respected
    input_clear(&in);
    for (int i = 0; i < 300; i++)
        input_insert(&in, 'x');
    n = input_layout(&in, 32, rows, 4, &r, &c);
    CHECK(n == 4);
}

void test_input(void)
{
    test_editing();
    test_limits();
    test_layout();
}
