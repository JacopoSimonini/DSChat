// Prints the on-screen keyboard key centres (from the real kbd.c) as JSON,
// so the melonDS driver taps exactly where the app expects.
#include <stdio.h>
#include "kbd.h"
int main(void)
{
    kbd_t k;
    kbd_init(&k, 0, 47, 256, 145);
    printf("{");
    for (int i = 0; i < k.count; i++)
    {
        const kbd_key *x = &k.keys[i];
        const char *name = x->type == K_BKSP ? "BKSP" : x->type == K_SEND ? "SEND"
                         : x->type == K_SHIFT ? "SHIFT" : x->type == K_CAPS ? "CAPS"
                         : x->type == K_LEFT ? "LEFT" : x->type == K_RIGHT ? "RIGHT"
                         : x->type == K_SPACE ? "SPACE" : NULL;
        char buf[8];
        if (!name)
        {
            snprintf(buf, sizeof buf, "c%d", x->lo);
            name = buf;
        }
        if (x->type == K_SHIFT && i > 40)
            continue;                    // second Shift key: keep the first
        printf("%s\"%s\": [%d, %d]", i ? ", " : "", name, x->x + x->w / 2, x->y + x->h / 2);
    }
    printf("}\n");
    return 0;
}
