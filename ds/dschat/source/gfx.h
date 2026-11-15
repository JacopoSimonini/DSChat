// Both screens as 256x192 16-bit bitmaps, text drawn with the libnds
// built-in 8x8 font (32 columns x 24 rows).
//
// The bottom screen is double-buffered: everything is drawn into a buffer in
// main RAM, rows that changed are remembered, and gfx_present() copies only
// those rows to VRAM right after VBlank starts. Drawing straight into VRAM
// (clear the keyboard, then redraw every key) used to show the half-drawn
// keyboard for one frame on every key press and release - the "dimming
// flicker" seen on the real DS Lite.
#pragma once

#include <nds.h>

#define SCREEN_W 256
#define SCREEN_H 192
#define FONT_W 8
#define FONT_H 8

#define C(r, g, b) ((u16)(RGB15(r, g, b) | BIT(15)))

// Palette (r, g, b in 0..31)
#define COL_BG        C(1, 2, 4)
#define COL_TEXT      C(29, 29, 29)    // AI text
#define COL_USER      C(12, 28, 31)    // what the user sent
#define COL_INFO      C(25, 23, 12)    // app notices
#define COL_ERROR     C(31, 11, 9)
#define COL_DIM       C(14, 15, 17)
#define COL_BAR       C(4, 7, 14)      // status bar
#define COL_BAR_OK    C(7, 22, 9)
#define COL_BAR_WARN  C(28, 20, 4)
#define COL_BAR_BAD   C(29, 8, 6)
#define COL_KEY       C(7, 9, 13)
#define COL_KEY_EDGE  C(12, 14, 19)
#define COL_KEY_DOWN  C(28, 28, 20)
#define COL_KEY_ON    C(9, 17, 28)     // active shift / caps
#define COL_KEY_SEND  C(6, 18, 8)
#define COL_KEY_STOP  C(24, 6, 5)
#define COL_INPUT_BG  C(0, 0, 0)
#define COL_CURSOR    C(31, 31, 31)
#define COL_SEL       C(9, 17, 28)
// Bottom-screen header bar (hint + counter): high contrast for the dim DS
// Lite LCD - black text on a near-white bar. The top status bar is unchanged.
#define COL_HDR_BG    C(30, 30, 29)
#define COL_HDR_TEXT  C(0, 0, 0)
#define COL_HDR_ALERT C(22, 0, 0)
// State chip of the top status bar: white on a dark colour
#define COL_ST_TEXT   C(31, 31, 31)
#define COL_ST_OK     C(0, 11, 2)
#define COL_ST_BUSY   C(3, 5, 12)
#define COL_ST_BAD    C(19, 0, 0)

void gfx_init(void);
// Copy the changed rows of the bottom back buffer to VRAM. Call right after
// VBlank has started (first thing in the main loop).
void gfx_present(void);

typedef struct
{
    uint32_t presents;       // copies done
    uint32_t rows;           // rows copied in total
    uint32_t last_rows;      // rows in the last copy
    uint32_t overruns;       // copies that were still running when VBlank ended
    uint32_t max_lines;      // longest copy, in scanlines (VBlank = 71 lines)
} gfx_stats_t;
void gfx_get_stats(gfx_stats_t *s);
u16 *gfx_top(void);
u16 *gfx_bottom(void);

void gfx_fill(u16 *fb, int x, int y, int w, int h, u16 color);
void gfx_char(u16 *fb, int x, int y, char c, u16 fg, u16 bg);
// Draws up to n characters of s (stops at NUL). Returns columns drawn.
int gfx_text(u16 *fb, int x, int y, const char *s, int n, u16 fg, u16 bg);
// Like gfx_text but pads with bg up to `cols` columns.
void gfx_text_cols(u16 *fb, int x, int y, const char *s, int cols, u16 fg, u16 bg);
// Text scaled up `scale` times (big, readable digits for IP / port entry).
void gfx_text_big(u16 *fb, int x, int y, const char *s, int scale, u16 fg, u16 bg);
