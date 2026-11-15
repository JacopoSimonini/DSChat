// SPDX-License-Identifier: Zlib
// SPDX-FileNotice: Modified from the original version by the BlocksDS project.
//
// Copyright (C) 2005 Michael Noland (joat)
// Copyright (C) 2005 Jason Rogers (Dovoto)
// Copyright (C) 2005-2015 Dave Murphy (WinterMute)
// Copyright (C) 2023 Antonio Niño Díaz
//
// mictest ARM7: the BlocksDS default ARM7 core (sdk v1.25.0
// sys/arm7/main_core/source/main.c, built with USE_DSWIFI) plus ONE addition:
// a FIFO_USER_01 handler that sets the microphone amplifier gain.
//
// Why: the stock core's micStartRecording() always calls micOn() =
// micSetAmp(PM_AMP_ON, PM_GAIN_160), and no ARM9-callable command changes
// the gain. The ARM9 sends the gain (PM_GAIN_20/40/80/160 = 0..3) right
// after soundMicRecord() has started the recording.

#include <nds.h>
#include <dswifi7.h>

#define FIFO_MIC_GAIN FIFO_USER_01

volatile bool exit_loop = false;

void power_button_callback(void)
{
    exit_loop = true;
}

void vblank_handler(void)
{
    inputGetAndSend();
    Wifi_Update();
}

static void mic_gain_handler(u32 gain, void *userdata)
{
    (void)userdata;
    if (gain <= PM_GAIN_160)
        micSetAmp(PM_AMP_ON, (u8)gain);
}

int main(void)
{
    enableSound();
    readUserSettings();
    ledBlink(0);
    touchInit();

    irqInit();
    fifoInit();

    installWifiFIFO();
    installSoundFIFO();
    installSystemFIFO(); // Sleep mode, storage, firmware...
    if (isDSiMode())
        installCameraFIFO();

    fifoSetValue32Handler(FIFO_MIC_GAIN, mic_gain_handler, 0);   // mictest

    setPowerButtonCB(power_button_callback);
    initClockIRQTimer(LIBNDS_DEFAULT_TIMER_RTC);

    irqSet(IRQ_VBLANK, vblank_handler);
    irqEnable(IRQ_VBLANK);

    while (!exit_loop)
    {
        const uint16_t key_mask = KEY_SELECT | KEY_START | KEY_L | KEY_R;
        uint16_t keys_pressed = ~REG_KEYINPUT;

        if ((keys_pressed & key_mask) == key_mask)
            exit_loop = true;

        swiWaitForVBlank();
    }

    return 0;
}
