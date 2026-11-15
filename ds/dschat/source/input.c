#include <string.h>

#include "input.h"

void input_init(input_t *in, uint16_t max)
{
    memset(in, 0, sizeof(*in));
    input_set_max(in, max);
}

void input_set_max(input_t *in, uint16_t max)
{
    if (max == 0 || max > INPUT_CAP)
        max = INPUT_CAP;
    in->max = max;
}

bool input_insert(input_t *in, char c)
{
    if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7E)
        return false;
    if (in->len >= in->max)
        return false;
    memmove(&in->buf[in->cur + 1], &in->buf[in->cur], in->len - in->cur + 1);
    in->buf[in->cur] = c;
    in->len++;
    in->cur++;
    return true;
}

bool input_backspace(input_t *in)
{
    if (in->cur == 0)
        return false;
    memmove(&in->buf[in->cur - 1], &in->buf[in->cur], in->len - in->cur + 1);
    in->len--;
    in->cur--;
    return true;
}

void input_left(input_t *in)
{
    if (in->cur > 0)
        in->cur--;
}

void input_right(input_t *in)
{
    if (in->cur < in->len)
        in->cur++;
}

void input_home(input_t *in)
{
    in->cur = 0;
}

void input_end(input_t *in)
{
    in->cur = in->len;
}

void input_clear(input_t *in)
{
    in->len = 0;
    in->cur = 0;
    in->buf[0] = '\0';
}

void input_set(input_t *in, const char *s)
{
    size_t n = strlen(s);
    if (n > INPUT_CAP)
        n = INPUT_CAP;
    memcpy(in->buf, s, n);
    in->buf[n] = '\0';
    in->len = n;
    in->cur = n;
}

bool input_blank(const input_t *in)
{
    for (int i = 0; i < in->len; i++)
    {
        if (in->buf[i] != ' ')
            return false;
    }
    return true;
}

int input_layout(const input_t *in, int width, input_row *rows, int max_rows,
                 int *cur_row, int *cur_col)
{
    int n = 0;
    int pos = 0;
    int crow = -1, ccol = 0;

    while (pos < in->len && n < max_rows)
    {
        int rest = in->len - pos;
        int len;
        if (rest <= width)
        {
            len = rest;
        }
        else if (in->buf[pos + width] == ' ')
        {
            len = width;                    // the row ends exactly at a word end
        }
        else
        {
            len = width;                    // split a too-long word ...
            for (int i = pos + width - 1; i > pos; i--)
            {
                if (in->buf[i] == ' ')      // ... unless a space lets us wrap
                {
                    len = i - pos + 1;
                    break;
                }
            }
        }
        rows[n].start = pos;
        rows[n].len = len;
        if (crow < 0 && in->cur >= pos && in->cur < pos + len)
        {
            crow = n;
            ccol = in->cur - pos;
        }
        n++;
        pos += len;
    }

    if (n == 0)
    {
        rows[0].start = 0;
        rows[0].len = 0;
        n = 1;
    }

    if (crow < 0)   // cursor at the very end of the text
    {
        input_row *last = &rows[n - 1];
        if (last->len >= width && n < max_rows)
        {
            rows[n].start = in->len;
            rows[n].len = 0;
            n++;
            crow = n - 1;
            ccol = 0;
        }
        else
        {
            crow = n - 1;
            ccol = last->len < width ? last->len : width - 1;
        }
    }

    *cur_row = crow;
    *cur_col = ccol;
    return n;
}
