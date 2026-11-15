// Text input being typed on the bottom screen: an edit buffer with a cursor,
// plus the word-wrapped layout used to draw it. Pure C, unit-tested natively.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define INPUT_CAP 1024   // hard cap; the relay's max_msg can only lower it

typedef struct
{
    char buf[INPUT_CAP + 1];   // NUL-terminated
    uint16_t len;
    uint16_t cur;              // cursor: insertion point, 0..len
    uint16_t max;              // current limit (relay max_msg, <= INPUT_CAP)
} input_t;

void input_init(input_t *in, uint16_t max);
// Changes the limit. Existing text is kept even if longer (sending is then
// refused and the counter shows it), so nothing the user typed is lost.
void input_set_max(input_t *in, uint16_t max);
bool input_insert(input_t *in, char c);      // false if full / not printable
bool input_backspace(input_t *in);           // false if nothing to delete
void input_left(input_t *in);
void input_right(input_t *in);
void input_home(input_t *in);
void input_end(input_t *in);
void input_clear(input_t *in);
void input_set(input_t *in, const char *s);  // replace text, cursor at end
bool input_blank(const input_t *in);         // empty or only spaces

// Layout for drawing with `width` columns. Rows are greedy word-wrapped:
// a row breaks after the last space that fits; a word longer than a row is
// split. Every character belongs to exactly one row. If the cursor sits at
// the end of a full row, an extra empty row is counted for it.
typedef struct
{
    uint16_t start;
    uint16_t len;
} input_row;

// Returns the number of rows (>= 1) written to rows[] (at most max_rows) and
// the cursor's row/column.
int input_layout(const input_t *in, int width, input_row *rows, int max_rows,
                 int *cur_row, int *cur_col);
