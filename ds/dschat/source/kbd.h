// On-screen QWERTY keyboard: layout, hit-testing and shift/caps logic.
// Drawing lives in ui.c. Pure C, unit-tested natively.
#pragma once

#include <stdbool.h>
#include <stdint.h>

enum kbd_type
{
    K_CHAR = 0,
    K_BKSP,
    K_SEND,     // Enter: send (shown as "Stop" while a reply is pending)
    K_SHIFT,    // one-shot shift
    K_CAPS,     // caps lock (letters only)
    K_LEFT,     // move the cursor
    K_RIGHT,
    K_SPACE,
};

typedef struct
{
    int16_t x, y, w, h;
    uint8_t type;
    char lo, hi;        // K_CHAR: unshifted / shifted character
} kbd_key;

#define KBD_MAX_KEYS 64

typedef struct
{
    kbd_key keys[KBD_MAX_KEYS];
    int count;
    bool shift;
    bool caps;
} kbd_t;

// US layout, 5 rows, filling the rectangle (x0, y0, width, height).
void kbd_init(kbd_t *k, int x0, int y0, int width, int height);
// Index of the key under (x, y), or -1 (gaps between keys count as misses).
int kbd_hit(const kbd_t *k, int x, int y);
// Character a K_CHAR key types in the current shift/caps state.
char kbd_char(const kbd_t *k, const kbd_key *key);

// Result of pressing a key.
typedef struct
{
    uint8_t type;       // K_* of the key
    char ch;            // for K_CHAR and K_SPACE: the character to insert
} kbd_action;

// Applies a key press: updates shift/caps and returns what the app must do.
// Shift is released after one character.
kbd_action kbd_press(kbd_t *k, int index);
