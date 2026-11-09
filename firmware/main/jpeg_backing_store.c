/* Spill progressive JPEG coefficients that cannot fit in PSRAM. The decoder
 * closes temporary files on both success and error; managed sources stay intact. */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <ctype.h>
#include <jpeglib.h>
#include <jerror.h>
#include "jmemsys.h"
#ifdef ESP_PLATFORM
#include "esp_timer.h"
#endif
static void backing_read(
    j_common_ptr cinfo, backing_store_ptr store, void *buffer, long offset, long count)
{
    int fd = fileno(store->temp_file);
    if(lseek(fd, offset, SEEK_SET) < 0)
        ERREXIT(cinfo, JERR_TFILE_SEEK);
    uint8_t *p = buffer;
    while(count > 0)
    {
        ssize_t n = read(fd, p, (size_t)count);
        if(n < 0 && errno == EINTR)
            continue;
        if(n <= 0)
            ERREXIT(cinfo, JERR_TFILE_READ);
        count -= n;
        p += n;
    }
}
static void backing_write(
    j_common_ptr cinfo, backing_store_ptr store, void *buffer, long offset, long count)
{
    int fd = fileno(store->temp_file);
    if(lseek(fd, offset, SEEK_SET) < 0)
        ERREXIT(cinfo, JERR_TFILE_SEEK);
    const uint8_t *p = buffer;
    while(count > 0)
    {
        ssize_t n = write(fd, p, (size_t)count);
        if(n < 0 && errno == EINTR)
            continue;
        if(n <= 0)
            ERREXIT(cinfo, JERR_TFILE_WRITE);
        count -= n;
        p += n;
    }
}
static void backing_close(j_common_ptr cinfo, backing_store_ptr store)
{
    (void)cinfo;
    if(store->temp_file)
        fclose(store->temp_file);
    store->temp_file = NULL;
    remove(store->temp_name);
}
void __wrap_jpeg_open_backing_store(j_common_ptr cinfo, backing_store_ptr store, long total)
{
    (void)total;
    static uint32_t sequence;
#ifdef ESP_PLATFORM
    uint32_t stamp = (uint32_t)esp_timer_get_time();
    const char *root = "/sdcard";
#else
    uint32_t stamp = (uint32_t)getpid();
    const char *root = "/tmp";
#endif
    int fd = -1;
    for(unsigned attempt = 0; attempt < 8 && fd < 0; attempt++)
    {
        uint32_t serial = __atomic_add_fetch(&sequence, 1, __ATOMIC_RELAXED);
        snprintf(store->temp_name, sizeof(store->temp_name), "%s/.jpeg-%08x-%08x.tmp", root,
            (unsigned)stamp, (unsigned)serial);
        fd = open(store->temp_name, O_CREAT | O_EXCL | O_RDWR, 0600);
    }
    if(fd < 0)
        ERREXIT(cinfo, JERR_TFILE_CREATE);
    store->temp_file = fdopen(fd, "w+b");
    if(!store->temp_file)
    {
        close(fd);
        remove(store->temp_name);
        ERREXIT(cinfo, JERR_TFILE_CREATE);
    }
    store->read_backing_store = backing_read;
    store->write_backing_store = backing_write;
    store->close_backing_store = backing_close;
}
void jpeg_thumbnail_cleanup(void)
{
    /* Boot only, before decoder workers start. Recover scratch space from an
     * interrupted decode without touching user covers or other temp files. */
    DIR *directory = opendir("/sdcard");
    if(!directory)
        return;
    struct dirent *entry;
    while((entry = readdir(directory)))
    {
        const char *name = entry->d_name;
        if(strlen(name) != 27 || strncmp(name, ".jpeg-", 6) || name[14] != '-'
            || strcmp(name + 23, ".tmp"))
            continue;
        bool valid = true;
        for(unsigned i = 6; i < 23; i++)
            if(i != 14 && !isxdigit((unsigned char)name[i]))
                valid = false;
        if(!valid)
            continue;
        char path[80];
        snprintf(path, sizeof(path), "/sdcard/%.27s", name);
        remove(path);
    }
    closedir(directory);
}
