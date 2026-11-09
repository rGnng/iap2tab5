#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static inline uint32_t store_crc32(uint32_t crc, const void *data, size_t len)
{
    const uint8_t *p = data;
    crc = ~crc;
    while(len--)
    {
        crc ^= *p++;
        for(int i = 0; i < 8; i++)
            crc = (crc >> 1) ^ ((0u - (crc & 1u)) & 0xedb88320u);
    }
    return ~crc;
}

/* FATFS rename does not replace a file. Keep a recoverable prior generation. */
static inline bool store_replace(const char *path, const char *temporary)
{
    char backup[320];
    snprintf(backup, sizeof(backup), "%s.bak", path);
    struct stat st;
    bool moved = false;
    if(stat(path, &st) == 0)
    {
        remove(backup);
        if(rename(path, backup) != 0)
            return false;
        moved = true;
    }
    if(rename(temporary, path) == 0)
        return true;
    if(moved)
        rename(backup, path);
    return false;
}

static inline bool store_write_atomic(const char *path, const void *data, size_t size)
{
    char temporary[320];
    snprintf(temporary, sizeof(temporary), "%s.tmp", path);
    FILE *file = fopen(temporary, "wb");
    if(!file)
        return false;
    bool ok = fwrite(data, 1, size, file) == size;
    if(fflush(file) != 0 || fsync(fileno(file)) != 0)
        ok = false;
    if(fclose(file) != 0)
        ok = false;
    if(ok)
        ok = store_replace(path, temporary);
    if(!ok)
        remove(temporary);
    return ok;
}
