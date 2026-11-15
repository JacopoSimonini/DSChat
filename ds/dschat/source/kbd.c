#include <string.h>

#include "kbd.h"

// Rows in half-units; every row sums to ROW_UNITS. Special keys use lo=0.
typedef struct
{
    uint8_t type;
    char lo, hi;
    uint8_t w;          // width in half-units (a letter key is 2)
} key_def;

#define ROW_UNITS 30
#define GAP_KEY(w) { 0xFF, 0, 0, (w) }

static const key_def row0[] = {
    { K_CHAR, '`', '~', 2 }, { K_CHAR, '1', '!', 2 }, { K_CHAR, '2', '@', 2 },
    { K_CHAR, '3', '#', 2 }, { K_CHAR, '4', '$', 2 }, { K_CHAR, '5', '%', 2 },
    { K_CHAR, '6', '^', 2 }, { K_CHAR, '7', '&', 2 }, { K_CHAR, '8', '*', 2 },
    { K_CHAR, '9', '(', 2 }, { K_CHAR, '0', ')', 2 }, { K_CHAR, '-', '_', 2 },
    { K_CHAR, '=', '+', 2 }, { K_BKSP, 0, 0, 4 },
};
static const key_def row1[] = {
    GAP_KEY(2),
    { K_CHAR, 'q', 'Q', 2 }, { K_CHAR, 'w', 'W', 2 }, { K_CHAR, 'e', 'E', 2 },
    { K_CHAR, 'r', 'R', 2 }, { K_CHAR, 't', 'T', 2 }, { K_CHAR, 'y', 'Y', 2 },
    { K_CHAR, 'u', 'U', 2 }, { K_CHAR, 'i', 'I', 2 }, { K_CHAR, 'o', 'O', 2 },
    { K_CHAR, 'p', 'P', 2 }, { K_CHAR, '[', '{', 2 }, { K_CHAR, ']', '}', 2 },
    { K_CHAR, '\\', '|', 2 },
    GAP_KEY(2),
};
static const key_def row2[] = {
    { K_CAPS, 0, 0, 4 },
    { K_CHAR, 'a', 'A', 2 }, { K_CHAR, 's', 'S', 2 }, { K_CHAR, 'd', 'D', 2 },
    { K_CHAR, 'f', 'F', 2 }, { K_CHAR, 'g', 'G', 2 }, { K_CHAR, 'h', 'H', 2 },
    { K_CHAR, 'j', 'J', 2 }, { K_CHAR, 'k', 'K', 2 }, { K_CHAR, 'l', 'L', 2 },
    { K_CHAR, ';', ':', 2 }, { K_CHAR, '\'', '"', 2 },
    { K_SEND, 0, 0, 4 },
};
static const key_def row3[] = {
    { K_SHIFT, 0, 0, 5 },
    { K_CHAR, 'z', 'Z', 2 }, { K_CHAR, 'x', 'X', 2 }, { K_CHAR, 'c', 'C', 2 },
    { K_CHAR, 'v', 'V', 2 }, { K_CHAR, 'b', 'B', 2 }, { K_CHAR, 'n', 'N', 2 },
    { K_CHAR, 'm', 'M', 2 }, { K_CHAR, ',', '<', 2 }, { K_CHAR, '.', '>', 2 },
    { K_CHAR, '/', '?', 2 },
    { K_SHIFT, 0, 0, 5 },
};
static const key_def row4[] = {
    { K_LEFT, 0, 0, 4 }, { K_SPACE, ' ', ' ', 22 }, { K_RIGHT, 0, 0, 4 },
};

static const struct
{
    const key_def *keys;
    int n;
} rows[] = {
    { row0, sizeof(row0) / sizeof(row0[0]) },
    { row1, sizeof(row1) / sizeof(row1[0]) },
    { row2, sizeof(row2) / sizeof(row2[0]) },
    { row3, sizeof(row3) / sizeof(row3[0]) },
    { row4, sizeof(row4) / sizeof(row4[0]) },
};
#define NUM_ROWS (int)(sizeof(rows) / sizeof(rows[0]))

void kbd_init(kbd_t *k, int x0, int y0, int width, int height)
{
    memset(k, 0, sizeof(*k));
    for (int r = 0; r < NUM_ROWS; r++)
    {
        int ya = y0 + r * height / NUM_ROWS;
        int yb = y0 + (r + 1) * height / NUM_ROWS;
        int units = 0;
        for (int i = 0; i < rows[r].n; i++)
        {
            const key_def *d = &rows[r].keys[i];
            int xa = x0 + units * width / ROW_UNITS;
            units += d->w;
            int xb = x0 + units * width / ROW_UNITS;
            if (d->type == 0xFF || k->count >= KBD_MAX_KEYS)
                continue;
            kbd_key *key = &k->keys[k->count++];
            // 1 px gap on the right and bottom of every key
            key->x = xa;
            key->y = ya;
            key->w = xb - xa - 1;
            key->h = yb - ya - 1;
            key->type = d->type;
            key->lo = d->lo;
            key->hi = d->hi;
        }
    }
}

int kbd_hit(const kbd_t *k, int x, int y)
{
    for (int i = 0; i < k->count; i++)
    {
        const kbd_key *key = &k->keys[i];
        if (x >= key->x && x < key->x + key->w && y >= key->y && y < key->y + key->h)
            return i;
    }
    return -1;
}

char kbd_char(const kbd_t *k, const kbd_key *key)
{
    if (key->type == K_SPACE)
        return ' ';
    if (key->type != K_CHAR)
        return 0;
    bool letter = key->lo >= 'a' && key->lo <= 'z';
    bool upper = letter ? (k->shift != k->caps) : k->shift;
    return upper ? key->hi : key->lo;
}

kbd_action kbd_press(kbd_t *k, int index)
{
    kbd_action a = { 0xFF, 0 };
    if (index < 0 || index >= k->count)
        return a;
    const kbd_key *key = &k->keys[index];
    a.type = key->type;
    switch (key->type)
    {
        case K_CHAR:
        case K_SPACE:
            a.ch = kbd_char(k, key);
            k->shift = false;
            break;
        case K_SHIFT:
            k->shift = !k->shift;
            break;
        case K_CAPS:
            k->caps = !k->caps;
            break;
        default:
            break;
    }
    return a;
}
