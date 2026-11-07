#include <errno.h>
#include "star_store.h"

#include "esp_log.h"
#include "esp_timer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char * TAG = "stars";

/* One row per track that has ever been toggled. `starred` is the current
 * state; `updated` is the last change. Text fields are quoted so a title with
 * a comma or quote survives a round trip through Excel. */
#define STAR_HEADER \
    "updated,starred,library_id,track_id,album_id,artist_id," \
    "title,artist,album,track_no,year,duration_sec,duration,device\n"
#define STAR_MAX_FIELDS 14
#define STAR_LINE_MAX   1024

static int csv_split(char * line, char ** fields, int max_fields)
{
    int count = 0;
    char * p = line;

    while(count < max_fields && *p != '\0' &&
          *p != '\r' && *p != '\n') {
        char * field = p;
        char * w = p;

        if(*p == '"') {
            p++;
            while(*p != '\0') {
                if(*p == '"') {
                    if(p[1] == '"') {
                        *w++ = '"';
                        p += 2;
                    }
                    else {
                        p++;
                        break;
                    }
                }
                else {
                    *w++ = *p++;
                }
            }
            *w = '\0';
            while(*p != '\0' && *p != ',') p++;
            if(*p == ',') p++;
        }
        else {
            while(*p != '\0' && *p != ',' &&
                  *p != '\r' && *p != '\n') {
                *w++ = *p++;
            }
            char delim = *p;
            *w = '\0';
            if(delim == ',') p++;
        }

        fields[count++] = field;
        if(*p == '\r' || *p == '\n') break;
    }
    return count;
}

static void csv_field(FILE * f, const char * s)
{
    fputc('"', f);
    if(s != NULL) {
        for(const unsigned char * p = (const unsigned char *)s; *p; p++) {
            if(*p == '"') fputc('"', f);
            if(*p == '\r' || *p == '\n') fputc(' ', f);
            else fputc(*p, f);
        }
    }
    fputc('"', f);
}

static void timestamp_now(char * out, size_t out_len)
{
    static int64_t boot_us;
    if(boot_us == 0) boot_us = esp_timer_get_time();

    time_t now = time(NULL);
    struct tm tmv;
    if(now > 1600000000 && localtime_r(&now, &tmv) != NULL) {
        strftime(out, out_len, "%Y-%m-%d %H:%M:%S", &tmv);
    }
    else {
        long long uptime_s = (esp_timer_get_time() - boot_us) / 1000000;
        snprintf(out, out_len, "boot+%llds", uptime_s);
    }
}

/* FATFS has LFN disabled, so the temporary name must stay 8.3 too. */
static void tmp_path_for(const char * path, char * out, size_t out_len)
{
    const char * slash = strrchr(path, '/');
    if(slash != NULL) {
        snprintf(out, out_len, "%.*s/stars.tmp", (int)(slash - path), path);
    }
    else {
        snprintf(out, out_len, "stars.tmp");
    }
}

static void write_row(FILE * f, const char * library_id,
                      const char * device_name,
                      const player_track_t * track, bool starred,
                      const char * updated)
{
    char duration[16];
    unsigned sec = track->duration_sec;
    snprintf(duration, sizeof(duration), "%u:%02u", sec / 60, sec % 60);

    fputs(updated, f);
    fprintf(f, ",%d,", starred ? 1 : 0);
    csv_field(f, library_id);
    fprintf(f, ",%016llX,%016llX,%016llX,",
            (unsigned long long)track->id,
            (unsigned long long)track->album_id,
            (unsigned long long)track->artist_id);
    csv_field(f, track->title);
    fputc(',', f);
    csv_field(f, track->artist);
    fputc(',', f);
    csv_field(f, track->album);
    fprintf(f, ",%u,%u,%u,%s,",
            (unsigned)track->track_no, (unsigned)track->year,
            (unsigned)track->duration_sec, duration);
    csv_field(f, device_name);
    fputc('\n', f);
}

void star_store_load(const char * path,
                     const player_track_t * tracks, int track_count,
                     bool * track_starred)
{
    if(path == NULL || tracks == NULL || track_starred == NULL ||
       track_count <= 0) {
        return;
    }

    FILE * f = fopen(path, "r");
    if(f == NULL) {
        char tmp[160];
        tmp_path_for(path, tmp, sizeof(tmp));
        f = fopen(tmp, "r");
        if(f == NULL) return;
        ESP_LOGW(TAG, "%s missing, recovering %s", path, tmp);
    }

    char line[STAR_LINE_MAX];
    char parse[STAR_LINE_MAX];
    bool header_checked = false;
    int applied = 0;

    while(fgets(line, sizeof(line), f) != NULL) {
        char * start = line;
        if(!header_checked) {
            header_checked = true;
            if((unsigned char)start[0] == 0xEF &&
               (unsigned char)start[1] == 0xBB &&
               (unsigned char)start[2] == 0xBF) {
                start += 3;
            }
            if(strncmp(start, "updated,", 8) == 0) continue;
        }

        memcpy(parse, start, strlen(start) + 1);
        char * fields[STAR_MAX_FIELDS] = {0};
        int n = csv_split(parse, fields, STAR_MAX_FIELDS);
        if(n < 4) continue;

        uint64_t id = strtoull(fields[3], NULL, 16);
        if(id == 0) continue;
        bool starred = atoi(fields[1]) != 0;

        for(int i = 0; i < track_count; i++) {
            if(tracks[i].id == id) {
                track_starred[i] = starred;
                if(starred) applied++;
                break;
            }
        }
    }
    fclose(f);

    ESP_LOGI(TAG, "loaded %d starred track%s from %s", applied,
             applied == 1 ? "" : "s", path);
}

static void copy_text(char * dst, size_t dst_len, const char * src)
{
    if(dst_len == 0) return;
    snprintf(dst, dst_len, "%s", src != NULL ? src : "");
}

/* Re-emit an existing CSV row in the current column layout. This keeps an
 * older file (13 columns, no device) readable and upgrades it on first write. */
static void write_existing_row(FILE * out, char ** fields, int n)
{
    if(n < 4) return;

    player_track_t tr;
    memset(&tr, 0, sizeof(tr));
    tr.id = strtoull(fields[3], NULL, 16);
    if(n > 4) tr.album_id = strtoull(fields[4], NULL, 16);
    if(n > 5) tr.artist_id = strtoull(fields[5], NULL, 16);
    if(n > 6) copy_text(tr.title, sizeof(tr.title), fields[6]);
    if(n > 7) copy_text(tr.artist, sizeof(tr.artist), fields[7]);
    if(n > 8) copy_text(tr.album, sizeof(tr.album), fields[8]);
    if(n > 9) tr.track_no = (uint16_t)atoi(fields[9]);
    if(n > 10) tr.year = (uint16_t)atoi(fields[10]);
    if(n > 11) tr.duration_sec = (uint32_t)strtoul(fields[11], NULL, 10);

    bool starred = n > 1 && atoi(fields[1]) != 0;
    const char * updated = n > 0 ? fields[0] : "";
    const char * library_id = n > 2 ? fields[2] : "";
    const char * device = n > 13 ? fields[13] : "";
    write_row(out, library_id, device, &tr, starred, updated);
}

void star_store_update(const char * path, const char * library_id,
                       const char * device_name,
                       const player_track_t * track, bool starred)
{
    if(path == NULL || track == NULL) return;

    char tmp[160];
    tmp_path_for(path, tmp, sizeof(tmp));

    char updated[32];
    timestamp_now(updated, sizeof(updated));

    FILE * in = fopen(path, "r");
    FILE * out = fopen(tmp, "w");
    if(out == NULL) {
        ESP_LOGW(TAG, "cannot write %s: %s", tmp, strerror(errno));
        if(in != NULL) fclose(in);
        return;
    }

    fputs(STAR_HEADER, out);

    bool wrote = false;
    if(in != NULL) {
        char line[STAR_LINE_MAX];
        char parse[STAR_LINE_MAX];
        bool header_checked = false;
        while(fgets(line, sizeof(line), in) != NULL) {
            char * start = line;
            if(!header_checked) {
                header_checked = true;
                if((unsigned char)start[0] == 0xEF &&
                   (unsigned char)start[1] == 0xBB &&
                   (unsigned char)start[2] == 0xBF) {
                    start += 3;
                }
                if(strncmp(start, "updated,", 8) == 0) continue;
            }

            memcpy(parse, start, strlen(start) + 1);
            char * fields[STAR_MAX_FIELDS] = {0};
            int n = csv_split(parse, fields, STAR_MAX_FIELDS);
            if(n >= 4) {
                uint64_t id = strtoull(fields[3], NULL, 16);
                if(id == track->id) {
                    if(!wrote) {
                        write_row(out, library_id, device_name, track,
                                  starred, updated);
                        wrote = true;
                    }
                    continue;
                }
                write_existing_row(out, fields, n);
            }
        }
        fclose(in);
    }

    if(!wrote) {
        write_row(out, library_id, device_name, track, starred, updated);
    }

    fflush(out);
    fsync(fileno(out));
    fclose(out);

    remove(path);
    if(rename(tmp, path) != 0) {
        ESP_LOGW(TAG, "rename %s -> %s failed", tmp, path);
    }
    else {
        ESP_LOGI(TAG, "%s %016llX '%s' -> %s", starred ? "star" : "unstar",
                 (unsigned long long)track->id, track->title, updated);
    }
}

void star_store_clear(const char * path)
{
    if(path == NULL) return;
    char tmp[160];
    tmp_path_for(path, tmp, sizeof(tmp));
    int main_removed = remove(path);
    int tmp_removed = remove(tmp);
    ESP_LOGI(TAG, "cleared %s (main=%d tmp=%d)", path,
             main_removed, tmp_removed);
}
