#include "debug_log.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define DEBUG_LOG_LINES       1024
#define DEBUG_LOG_TEXT_MAX    224   /* keep one line in one entry */
#define DEBUG_LOG_RING_BYTES  (DEBUG_LOG_LINES * sizeof(debug_log_entry_t))

#define DEBUG_LOG_SD_PATH       "/sdcard/debug.log"
#define DEBUG_LOG_SD_OLD_PATH   "/sdcard/debug.old.log"
#define DEBUG_LOG_SD_MAX_BYTES  (4 * 1024 * 1024)
#define DEBUG_LOG_SD_QUEUE_LEN  64

typedef struct {
    uint16_t len;
    char text[DEBUG_LOG_TEXT_MAX];
} debug_log_entry_t;

typedef struct {
    uint32_t seq;
    uint16_t len;
    char text[DEBUG_LOG_TEXT_MAX];
} debug_log_sd_entry_t;

static debug_log_entry_t * s_entries;
static uint32_t s_write_seq;    /* next sequence number */
static uint32_t s_count;        /* valid entries, <= DEBUG_LOG_LINES */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static vprintf_like_t s_prev_vprintf;
static bool s_installed;

static QueueHandle_t s_sd_queue;
static volatile bool s_sd_requested;
static volatile bool s_sd_file_open;
static volatile uint32_t s_sd_lines;
static volatile uint32_t s_sd_errors;

static inline uint32_t ring_oldest(void)
{
    return s_write_seq - s_count;
}

static void push_line(const char * line, size_t len)
{
    if(s_entries == NULL || len == 0) return;

    if(len > DEBUG_LOG_TEXT_MAX - 1) len = DEBUG_LOG_TEXT_MAX - 1;

    portENTER_CRITICAL(&s_lock);
    uint32_t seq = s_write_seq++;
    debug_log_entry_t * entry = &s_entries[seq % DEBUG_LOG_LINES];
    memcpy(entry->text, line, len);
    entry->text[len] = '\0';
    entry->len = (uint16_t)len;
    if(s_count < DEBUG_LOG_LINES) s_count++;
    portEXIT_CRITICAL(&s_lock);

    if(s_sd_queue != NULL && s_sd_requested) {
        debug_log_sd_entry_t sd;
        sd.seq = seq;
        sd.len = (uint16_t)len;
        memcpy(sd.text, line, len);
        sd.text[len] = '\0';
        (void)xQueueSend(s_sd_queue, &sd, 0);
    }
}

/* Split one ESP_LOG call into lines.  ESP_LOG normally emits exactly one
 * line, but multi-line messages are valid too. */
static void push_text(const char * text, size_t len)
{
    size_t start = 0;
    while(start < len) {
        size_t end = start;
        while(end < len && text[end] != '\n') end++;
        size_t line_len = end - start;
        while(line_len > 0 && text[start + line_len - 1] == '\r') line_len--;
        if(line_len > 0) push_line(text + start, line_len);
        start = (end < len) ? end + 1 : end;
    }
}

static int debug_log_vprintf(const char * fmt, va_list args)
{
    va_list copy;
    va_copy(copy, args);
    char line[DEBUG_LOG_TEXT_MAX];
    int n = vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);
    if(n > 0 && s_entries != NULL) {
        size_t len = (size_t)n;
        if(len >= sizeof(line)) len = sizeof(line) - 1;
        push_text(line, len);
    }
    return s_prev_vprintf != NULL ? s_prev_vprintf(fmt, args) : n;
}

/* Copy the in-memory ring to the file so lines logged before the SD card was
 * mounted are preserved.  Returns the last sequence number dumped. */
static uint32_t sd_dump_ring(FILE * f)
{
    char * buf = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(buf == NULL) buf = malloc(4096);
    if(buf == NULL) return debug_log_seq();

    uint32_t since = 0;
    for(int i = 0; i < 256; i++) {
        size_t len = 0;
        uint32_t dropped = 0;
        uint32_t next = debug_log_read_text(since, buf, 4096,
                                            &len, &dropped);
        if(dropped > 0) {
            fprintf(f, "[log] dropped %u lines\n", (unsigned)dropped);
        }
        if(len > 0 && fwrite(buf, 1, len, f) != len) break;
        if(next == since) break;
        since = next;
    }
    free(buf);
    return since;
}

/* Open the SD log.  Keep the first write small and unbuffered so a later
 * hang still leaves a visible marker in the file. */
static FILE * sd_log_open(uint32_t * dumped_seq)
{
    struct stat st;
    if(stat(DEBUG_LOG_SD_PATH, &st) == 0 &&
       st.st_size > DEBUG_LOG_SD_MAX_BYTES) {
        remove(DEBUG_LOG_SD_OLD_PATH);
        rename(DEBUG_LOG_SD_PATH, DEBUG_LOG_SD_OLD_PATH);
    }

    FILE * f = fopen(DEBUG_LOG_SD_PATH, "a");
    if(f == NULL) {
        s_sd_errors++;
        return NULL;
    }
    setvbuf(f, NULL, _IONBF, 0);

    if(fprintf(f, "===== debug log: reset reason %d =====\n",
               (int)esp_reset_reason()) < 0 || fflush(f) != 0) {
        fclose(f);
        s_sd_errors++;
        return NULL;
    }

    *dumped_seq = sd_dump_ring(f);
    if(fflush(f) != 0 || fsync(fileno(f)) != 0) {
        fclose(f);
        s_sd_errors++;
        return NULL;
    }
    s_sd_file_open = true;
    return f;
}

static void debug_log_sd_task(void * arg)
{
    (void)arg;
    debug_log_sd_entry_t entry;
    FILE * f = NULL;
    uint32_t dumped_seq = 0;

    for(;;) {
        BaseType_t got = xQueueReceive(s_sd_queue, &entry,
                                       pdMS_TO_TICKS(500));
        if(!s_sd_requested) {
            if(f != NULL) {
                fflush(f);
                fclose(f);
                f = NULL;
                s_sd_file_open = false;
                dumped_seq = 0;
            }
            continue;
        }
        if(f == NULL) {
            f = sd_log_open(&dumped_seq);
            if(f == NULL) continue;
        }
        if(got == pdTRUE) {
            if((int32_t)(entry.seq - dumped_seq) >= 0) {
                if(fwrite(entry.text, 1, entry.len, f) == entry.len &&
                   fputc('\n', f) != EOF) {
                    s_sd_lines++;
                }
                else {
                    s_sd_errors++;
                }
            }
            if(fflush(f) != 0 || fsync(fileno(f)) != 0 || ferror(f)) {
                s_sd_errors++;
                fclose(f);
                f = NULL;
                s_sd_file_open = false;
                dumped_seq = 0;
            }
        }
        else {
            fflush(f);
        }
    }
}

void debug_log_init(void)
{
    if(s_installed) return;

    s_entries = heap_caps_calloc(DEBUG_LOG_LINES, sizeof(debug_log_entry_t),
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(s_entries == NULL) {
        s_entries = heap_caps_calloc(DEBUG_LOG_LINES, sizeof(debug_log_entry_t),
                                     MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if(s_entries == NULL) {
        ESP_LOGW("debug", "no memory for log mirror (%u bytes)",
                 (unsigned)DEBUG_LOG_RING_BYTES);
        return;
    }

    s_prev_vprintf = esp_log_set_vprintf(debug_log_vprintf);
    s_installed = true;

    s_sd_queue = xQueueCreate(DEBUG_LOG_SD_QUEUE_LEN,
                              sizeof(debug_log_sd_entry_t));
    if(s_sd_queue == NULL) {
        ESP_LOGW("debug", "no memory for SD log queue");
    }
    else if(xTaskCreate(debug_log_sd_task, "debug_sd", 8192, NULL, 3,
                        NULL) != pdPASS) {
        ESP_LOGW("debug", "SD log task creation failed");
        vQueueDelete(s_sd_queue);
        s_sd_queue = NULL;
    }

    ESP_LOGI("debug", "log mirror ready: %u lines x %u bytes",
             (unsigned)DEBUG_LOG_LINES, (unsigned)DEBUG_LOG_TEXT_MAX);
}

void debug_log_set_sd_enabled(bool enabled)
{
    if(s_sd_requested == enabled) return;
    s_sd_requested = enabled;
    ESP_LOGI("debug", "SD log %s", enabled ? "enabled" : "disabled");
}

bool debug_log_sd_enabled(void)
{
    return s_sd_requested;
}

bool debug_log_sd_file_open(void)
{
    return s_sd_file_open;
}

uint32_t debug_log_sd_lines(void)
{
    return s_sd_lines;
}

uint32_t debug_log_sd_errors(void)
{
    return s_sd_errors;
}

uint32_t debug_log_seq(void)
{
    portENTER_CRITICAL(&s_lock);
    uint32_t seq = s_write_seq;
    portEXIT_CRITICAL(&s_lock);
    return seq;
}

/* Sequence numbers are 32-bit and may wrap.  Return true when a is before b. */
static inline bool seq_before(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) < 0;
}

static uint32_t read_locked(uint32_t since, char * out, size_t cap,
                            size_t * out_len, uint32_t * dropped,
                            bool sse)
{
    uint32_t oldest = ring_oldest();
    uint32_t latest = s_write_seq;
    *dropped = 0;

    if(seq_before(since, oldest)) {
        *dropped = oldest - since;
        since = oldest;
    }
    if(seq_before(latest, since)) since = latest;

    size_t n = 0;

    if(*dropped > 0 && sse && cap > 48) {
        int wrote = snprintf(out, cap, "data: [log] dropped %u lines\n\n",
                             (unsigned)*dropped);
        if(wrote > 0) n = (size_t)wrote;
    }

    uint32_t seq = since;
    while(seq != latest) {
        debug_log_entry_t * entry = &s_entries[seq % DEBUG_LOG_LINES];
        size_t need = sse ? (6 + entry->len + 2) : (entry->len + 1);
        if(n + need > cap) break;

        if(sse) {
            memcpy(out + n, "data: ", 6);
            n += 6;
        }
        memcpy(out + n, entry->text, entry->len);
        n += entry->len;
        out[n++] = '\n';
        if(sse) out[n++] = '\n';
        seq++;
    }

    *out_len = n;
    return seq;
}

uint32_t debug_log_read_text(uint32_t since, char * out, size_t cap,
                             size_t * out_len, uint32_t * dropped)
{
    portENTER_CRITICAL(&s_lock);
    uint32_t next = read_locked(since, out, cap, out_len, dropped, false);
    portEXIT_CRITICAL(&s_lock);
    return next;
}

uint32_t debug_log_read_sse(uint32_t since, char * out, size_t cap,
                            size_t * out_len, uint32_t * dropped)
{
    portENTER_CRITICAL(&s_lock);
    uint32_t next = read_locked(since, out, cap, out_len, dropped, true);
    portEXIT_CRITICAL(&s_lock);
    return next;
}
