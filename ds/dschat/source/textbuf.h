// Conversation scrollback: a fixed-size ring buffer of already word-wrapped
// lines. Text is appended incrementally (streamed reply chunks) and wrapped
// as it arrives: a word is never split unless it is longer than a line.
// Pure C (no libnds), unit-tested natively in ../tests.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define TB_COLS  32      // characters per line (256 px / 8 px font)
#define TB_LINES 2048    // ring capacity in wrapped lines (~70 KB)

enum tb_kind
{
    TB_AI = 0,           // AI reply text
    TB_USER,             // what the user sent
    TB_INFO,             // status notices from the app
    TB_ERROR,            // relay / network errors
};

typedef struct
{
    char text[TB_COLS];  // not NUL-terminated
    uint8_t len;
    uint8_t kind;
} tb_line;

typedef struct
{
    tb_line lines[TB_LINES];
    uint32_t total;      // lines ever created; line numbers are absolute
    uint32_t base;       // lines before this were cleared (tb_clear)
    uint8_t kind;        // kind of the paragraph being appended
    bool open;           // the last line still receives text
} textbuf;

void tb_init(textbuf *tb);
// Forgets all lines; numbering continues (old line numbers become invalid).
void tb_clear(textbuf *tb);

// Starts a paragraph of the given kind on a new line.
void tb_begin(textbuf *tb, uint8_t kind);
// Appends text to the open paragraph ('\n' starts a new line). Bytes outside
// printable ASCII become '?'. Opens a paragraph of the last kind if needed.
void tb_append(textbuf *tb, const char *s, size_t n);
// Ends the paragraph: the next text starts on a new line.
void tb_end(textbuf *tb);
// One complete paragraph.
void tb_puts(textbuf *tb, uint8_t kind, const char *s);
// An empty separator line, unless the last line is already empty.
void tb_blank(textbuf *tb);

// Absolute number of the oldest line still stored, and one past the newest.
uint32_t tb_first(const textbuf *tb);
static inline uint32_t tb_end_line(const textbuf *tb) { return tb->total; }
// Line by absolute number; NULL if it was overwritten or does not exist yet.
const tb_line *tb_get(const textbuf *tb, uint32_t n);
