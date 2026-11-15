#include <string.h>

#include "gfx.h"

static u16 *fb_top;          // VRAM, drawn directly (only line overwrites)
static u16 *vram_bottom;     // VRAM of the bottom screen
static u16 back[SCREEN_W * SCREEN_H] __attribute__((aligned(32)));   // bottom back buffer
static u8 dirty_rows[SCREEN_H];
static bool any_dirty;
static gfx_stats_t stats;
static const u8 *font;        // 1 bpp, 8 bytes per glyph, bit 0 = leftmost pixel
static unsigned font_first, font_count;

static inline void mark(u16 *fb, int y, int h)
{
    if (fb != back)
        return;
    for (int i = 0; i < h; i++)
        dirty_rows[y + i] = 1;
    any_dirty = true;
}

void gfx_init(void)
{
    videoSetMode(MODE_5_2D);
    videoSetModeSub(MODE_5_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    vramSetBankC(VRAM_C_SUB_BG);
    int top = bgInit(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
    int bottom = bgInitSub(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
    fb_top = bgGetGfxPtr(top);
    vram_bottom = bgGetGfxPtr(bottom);

    // Reuse the libnds console font without creating a console.
    const PrintConsole *def = consoleGetDefault();
    font = def->font.gfx;
    font_first = def->font.asciiOffset;
    font_count = def->font.numChars;

    gfx_fill(fb_top, 0, 0, SCREEN_W, SCREEN_H, COL_BG);
    gfx_fill(back, 0, 0, SCREEN_W, SCREEN_H, COL_BG);
    gfx_present();
}

u16 *gfx_top(void)
{
    return fb_top;
}

u16 *gfx_bottom(void)
{
    return back;
}

void gfx_present(void)
{
    if (!any_dirty)
        return;
    any_dirty = false;
    int start_line = REG_VCOUNT;
    uint32_t rows = 0;
    // Copy each run of dirty rows (whole rows are contiguous in memory).
    for (int y = 0; y < SCREEN_H;)
    {
        if (!dirty_rows[y])
        {
            y++;
            continue;
        }
        int y0 = y;
        while (y < SCREEN_H && dirty_rows[y])
            dirty_rows[y++] = 0;
        size_t bytes = (size_t)(y - y0) * SCREEN_W * sizeof(u16);
        DC_FlushRange(&back[y0 * SCREEN_W], bytes);
        dmaCopyWords(3, &back[y0 * SCREEN_W], &vram_bottom[y0 * SCREEN_W], bytes);
        rows += y - y0;
    }
    int end_line = REG_VCOUNT;
    // VCOUNT runs 0-262; 192-262 is VBlank. Started in VBlank and now in the
    // visible part = the copy outlasted VBlank (could tear, never "dim").
    int lines = end_line - start_line;
    if (lines < 0)
        lines += 263;
    if ((uint32_t)lines > stats.max_lines)
        stats.max_lines = lines;
    if (start_line >= 192 && end_line < 192)
        stats.overruns++;
    stats.presents++;
    stats.rows += rows;
    stats.last_rows = rows;
}

void gfx_get_stats(gfx_stats_t *s)
{
    *s = stats;
}

void gfx_fill(u16 *fb, int x, int y, int w, int h, u16 color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > SCREEN_W) w = SCREEN_W - x;
    if (y + h > SCREEN_H) h = SCREEN_H - y;
    if (w <= 0 || h <= 0)
        return;
    mark(fb, y, h);
    u16 *row = fb + y * SCREEN_W + x;
    for (int j = 0; j < h; j++, row += SCREEN_W)
    {
        for (int i = 0; i < w; i++)
            row[i] = color;
    }
}

void gfx_char(u16 *fb, int x, int y, char c, u16 fg, u16 bg)
{
    if (x < 0 || y < 0 || x + FONT_W > SCREEN_W || y + FONT_H > SCREEN_H)
        return;
    mark(fb, y, FONT_H);
    unsigned idx = (unsigned char)c - font_first;
    if (idx >= font_count)
        idx = '?' - font_first;
    const u8 *g = font + idx * 8;
    u16 *row = fb + y * SCREEN_W + x;
    for (int r = 0; r < FONT_H; r++, row += SCREEN_W)
    {
        u8 bits = g[r];
        for (int i = 0; i < FONT_W; i++)
            row[i] = (bits >> i) & 1 ? fg : bg;
    }
}

int gfx_text(u16 *fb, int x, int y, const char *s, int n, u16 fg, u16 bg)
{
    int i = 0;
    for (; i < n && s[i]; i++)
        gfx_char(fb, x + i * FONT_W, y, s[i], fg, bg);
    return i;
}

void gfx_text_cols(u16 *fb, int x, int y, const char *s, int cols, u16 fg, u16 bg)
{
    int n = gfx_text(fb, x, y, s, cols, fg, bg);
    if (n < cols)
        gfx_fill(fb, x + n * FONT_W, y, (cols - n) * FONT_W, FONT_H, bg);
}

void gfx_text_big(u16 *fb, int x, int y, const char *s, int scale, u16 fg, u16 bg)
{
    for (; *s; s++, x += FONT_W * scale)
    {
        unsigned idx = (unsigned char)*s - font_first;
        if (idx >= font_count)
            idx = '?' - font_first;
        const u8 *g = font + idx * 8;
        for (int r = 0; r < FONT_H; r++)
        {
            for (int i = 0; i < FONT_W; i++)
                gfx_fill(fb, x + i * scale, y + r * scale, scale, scale,
                         (g[r] >> i) & 1 ? fg : bg);
        }
    }
}
