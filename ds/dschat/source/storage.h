// Settings file on the SD card: fat:/dschat/settings.txt (format: config.c,
// documented in the README). Written safely: temp file, remove old, rename,
// read back and compare - the pattern verified on a real flashcard.
#pragma once

#include <stdbool.h>

#include "config.h"

enum storage_status
{
    ST_NO_SD = 0,       // no FAT/DLDI: settings live in RAM for this session
    ST_MISSING,         // SD ok, no settings file yet
    ST_EMPTY,           // file exists but has no settings
    ST_ERRORS,          // file read, some lines invalid (defaults used for them)
    ST_INVALID,         // file read, but the result can't be used to connect
    ST_LOADED,          // file read and valid
};

typedef struct
{
    bool sd;
    char drive[16];
    char path[48];
    int status;
    char msg[80];        // what happened on load (shown to the user)
    char save_msg[64];   // result of the last save
} storage_t;

void storage_init(storage_t *st);                        // mounts FAT
int storage_load(storage_t *st, dschat_config *cfg);     // always fills cfg
bool storage_save(storage_t *st, const dschat_config *cfg);
