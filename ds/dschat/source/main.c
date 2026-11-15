// DSChat for the Nintendo DS (Lite): chat with an AI through the laptop relay.
// Top screen: conversation. Bottom screen: input box + touch keyboard.
// Protocol: docs/PROTOCOL.md. Settings: config.c + storage.c (SD card).

#include <nds.h>

#include "app.h"
#include "config.h"
#include "gfx.h"
#include "net.h"
#include "textbuf.h"

static volatile uint32_t frames;     // VBlank counter (60 Hz)
static dschat_config cfg;

static void vblank_handler(void)
{
    frames++;
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    irqSet(IRQ_VBLANK, vblank_handler);
    irqEnable(IRQ_VBLANK);
    gfx_init();
    keysSetRepeat(20, 4);

    app_init(&cfg);              // mounts the SD card and loads the settings
    ui_render();
    gfx_present();
    app.wifi_ok = net_init(&cfg);
    if (!app.wifi_ok)
        app_say(TB_ERROR, "Wi-Fi init FAILED. Restart the DS.");
    app_boot(frames);            // auto-connect, or open the settings

    // Everything is polled from here: the UI never waits on the network.
    while (1)
    {
        cothread_yield_irq(IRQ_VBLANK);
        gfx_present();           // bottom screen: copy last frame's changes in VBlank
        uint32_t now = frames;
        app_input(now);
        net_poll(now);
        app_tick(now);
        ui_render();
    }
}
