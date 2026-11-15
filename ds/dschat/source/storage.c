#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <fat.h>

#include "storage.h"

#define MAX_FILE 4096

void storage_init(storage_t *st)
{
    memset(st, 0, sizeof(*st));
    st->sd = fatInitDefault();
    if (st->sd)
    {
        snprintf(st->drive, sizeof(st->drive), "%s", fatGetDefaultDrive());
        snprintf(st->path, sizeof(st->path), "%sdschat/settings.txt", st->drive);
    }
}

int storage_load(storage_t *st, dschat_config *cfg)
{
    static char text[MAX_FILE + 1];
    config_defaults(cfg);
    if (!st->sd)
    {
        st->status = ST_NO_SD;
        snprintf(st->msg, sizeof(st->msg), "no SD card: settings kept in memory only");
        return st->status;
    }
    FILE *f = fopen(st->path, "rb");
    if (f == NULL)
    {
        st->status = ST_MISSING;
        snprintf(st->msg, sizeof(st->msg), "no settings file yet (dschat/settings.txt)");
        return st->status;
    }
    size_t n = fread(text, 1, MAX_FILE + 1, f);
    fclose(f);
    if (n > MAX_FILE)
    {
        st->status = ST_INVALID;
        snprintf(st->msg, sizeof(st->msg), "settings file too big - using defaults");
        return st->status;
    }
    text[n] = '\0';
    if (strlen(text) != n)
    {
        // NUL bytes inside: a damaged file
        st->status = ST_INVALID;
        snprintf(st->msg, sizeof(st->msg), "settings file is damaged - using defaults");
        return st->status;
    }
    char err[64], why[48];
    int problems = config_parse(cfg, text, err, sizeof(err));
    bool any = strstr(text, "=") != NULL;
    if (!any && problems == 0)
    {
        st->status = ST_EMPTY;
        snprintf(st->msg, sizeof(st->msg), "settings file is empty - using defaults");
    }
    else if (!config_validate(cfg, why, sizeof(why)))
    {
        st->status = ST_INVALID;
        snprintf(st->msg, sizeof(st->msg), "settings: %s", problems ? err : why);
    }
    else if (problems)
    {
        st->status = ST_ERRORS;
        snprintf(st->msg, sizeof(st->msg), "settings file: %s", err);
    }
    else
    {
        st->status = ST_LOADED;
        snprintf(st->msg, sizeof(st->msg), "settings loaded from SD");
    }
    return st->status;
}

bool storage_save(storage_t *st, const dschat_config *cfg)
{
    static char text[MAX_FILE], back[MAX_FILE + 1];
    if (!st->sd)
    {
        snprintf(st->save_msg, sizeof(st->save_msg), "no SD card: kept for this session");
        return false;
    }
    size_t len = config_serialise(cfg, text, sizeof(text));
    char dir[48], tmp[48];
    snprintf(dir, sizeof(dir), "%sdschat", st->drive);
    snprintf(tmp, sizeof(tmp), "%sdschat/settings.tmp", st->drive);
    if (mkdir(dir, 0777) != 0 && errno != EEXIST)
    {
        snprintf(st->save_msg, sizeof(st->save_msg), "save failed: mkdir (errno %d)", errno);
        return false;
    }
    const char *step = "create";
    FILE *f = fopen(tmp, "wb");
    if (f != NULL)
    {
        step = "write";
        size_t w = fwrite(text, 1, len, f);
        if (fclose(f) == 0 && w == len)
        {
            step = "rename";
            remove(st->path);                 // FAT rename fails if the target exists
            if (rename(tmp, st->path) == 0)
            {
                step = "read back";
                f = fopen(st->path, "rb");
                if (f != NULL)
                {
                    size_t n = fread(back, 1, sizeof(back), f);
                    fclose(f);
                    if (n == len && memcmp(back, text, len) == 0)
                        step = NULL;
                }
            }
        }
    }
    if (step != NULL)
    {
        snprintf(st->save_msg, sizeof(st->save_msg), "save failed at %s (errno %d)", step, errno);
        return false;
    }
    snprintf(st->save_msg, sizeof(st->save_msg), "saved to dschat/settings.txt");
    return true;
}
