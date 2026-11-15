#include "check.h"
#include "kbd.h"

static kbd_t k;

static int find_char(char lo)
{
    for (int i = 0; i < k.count; i++)
    {
        if (k.keys[i].type == K_CHAR && k.keys[i].lo == lo)
            return i;
    }
    return -1;
}

static int find_type(int type)
{
    for (int i = 0; i < k.count; i++)
    {
        if (k.keys[i].type == type)
            return i;
    }
    return -1;
}

static void test_layout(void)
{
    kbd_init(&k, 0, 47, 256, 145);
    // every printable ASCII character can be typed (lo or hi of some key)
    for (int c = 0x20; c <= 0x7E; c++)
    {
        int found = c == ' ' && find_type(K_SPACE) >= 0;
        for (int i = 0; i < k.count && !found; i++)
            found = k.keys[i].type == K_CHAR && (k.keys[i].lo == c || k.keys[i].hi == c);
        if (!found)
            fprintf(stderr, "char %c not on the keyboard\n", c);
        CHECK(found);
    }
    // keys stay inside the area and do not overlap; centres hit themselves
    for (int i = 0; i < k.count; i++)
    {
        const kbd_key *a = &k.keys[i];
        CHECK(a->x >= 0 && a->x + a->w <= 256 && a->y >= 47 && a->y + a->h <= 192);
        CHECK(a->w >= 15 && a->h >= 25);          // big enough for a stylus
        CHECK(kbd_hit(&k, a->x + a->w / 2, a->y + a->h / 2) == i);
        CHECK(kbd_hit(&k, a->x, a->y) == i);
        CHECK(kbd_hit(&k, a->x + a->w - 1, a->y + a->h - 1) == i);
    }
    CHECK(kbd_hit(&k, 5, 10) == -1);              // input area, not a key
    // the 1 px gap right of 'q' is a miss
    const kbd_key *q = &k.keys[find_char('q')];
    CHECK(kbd_hit(&k, q->x + q->w, q->y + 5) == -1);
    CHECK(find_type(K_BKSP) >= 0 && find_type(K_SEND) >= 0 && find_type(K_CAPS) >= 0);
    CHECK(find_type(K_LEFT) >= 0 && find_type(K_RIGHT) >= 0);
}

static void test_shift_caps(void)
{
    kbd_init(&k, 0, 47, 256, 145);
    int a = find_char('a'), one = find_char('1'), shift = find_type(K_SHIFT);
    int caps = find_type(K_CAPS), space = find_type(K_SPACE);

    CHECK(kbd_press(&k, a).ch == 'a');
    kbd_press(&k, shift);
    CHECK(kbd_press(&k, a).ch == 'A');
    CHECK(kbd_press(&k, a).ch == 'a');            // shift is one-shot
    kbd_press(&k, shift);
    CHECK(kbd_press(&k, one).ch == '!');
    CHECK(kbd_press(&k, one).ch == '1');

    kbd_press(&k, caps);
    CHECK(k.caps);
    CHECK(kbd_press(&k, a).ch == 'A');
    CHECK(kbd_press(&k, a).ch == 'A');            // caps stays on
    CHECK(kbd_press(&k, one).ch == '1');          // caps: letters only
    kbd_press(&k, shift);
    CHECK(kbd_press(&k, a).ch == 'a');            // shift inverts caps
    kbd_press(&k, caps);
    CHECK(!k.caps);

    kbd_action act = kbd_press(&k, space);
    CHECK(act.type == K_SPACE && act.ch == ' ');
    act = kbd_press(&k, find_type(K_SEND));
    CHECK(act.type == K_SEND && act.ch == 0);
    act = kbd_press(&k, -1);
    CHECK(act.type == 0xFF);
}

void test_kbd(void)
{
    test_layout();
    test_shift_caps();
}
