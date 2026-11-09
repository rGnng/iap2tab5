#include "lockdown_client.h"

#include <errno.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#include "afc_client.h"
#include "apple_pairing.h"
#include "app_config.h"
#include "player.h"
#include "thumb_cache.h"
#include "jpeg_thumbnail.h"
#include "media_index.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "driver/jpeg_decode.h"
#include "scaled_tjpgd.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "freertos/task.h"
#include "bsp/esp-bsp.h"
#include "lvgl.h"
#include "src/image/lv_image_decoder_private.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/pk.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "usbmux_host.h"

static const char * TAG = "lockdown";

#define LOCKDOWN_PORT   62078
#define MAX_PLIST_LEN   (128 * 1024)
#define MAX_PAIR_LEN    (256 * 1024)
#define PAIR_PATH_SD    "/sdcard/pair.plist"
#define PAIR_PATH_SPIFS "/spiffs/pair.plist"

static usbmux_conn_t * s_conn;
static char s_status[96] = "idle";

static volatile bool s_art_active;
static volatile int s_art_kind;
static volatile int s_art_phase;
static volatile int s_art_total;
static volatile int s_art_attempted;
static volatile int s_art_copied;
static volatile int s_art_failed;
static volatile uint64_t s_art_current;
static char s_art_album[96];
static char s_art_artist[96];

#define SLOW_ALBUM_MS 1000
static uint32_t s_last_bytes;
static int s_last_w;
static int s_last_h;
static int s_last_read_ms;
static int s_last_decode_ms;
static int s_last_write_ms;
static char s_last_kind[24] = "?";

static void log_safe(char * dst, size_t dst_len, const char * src)
{
    size_t j = 0;
    if(src == NULL) src = "";
    for(size_t i = 0; src[i] != '\0' && j + 1 < dst_len; i++) {
        unsigned char c = (unsigned char)src[i];
        if(c == '"' || c == '\n' || c == '\r' || c == '\t' || c < 0x20) {
            c = ' ';
        }
        dst[j++] = (char)c;
    }
    dst[j] = '\0';
}

static void cache_slow_log_begin(bool truncate)
{
    char path[160];
    snprintf(path, sizeof(path), "%s/slow.log",
             app_config_library_dir());
    FILE * f = fopen(path, truncate ? "w" : "a");
    if(f == NULL) return;
    setvbuf(f, NULL, _IONBF, 0);
    if(truncate) {
        char ts[32];
        time_t now = time(NULL);
        struct tm tmv;
        localtime_r(&now, &tmv);
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);
        fprintf(f, "# T5 artwork cache slow log\n"
                   "# started=%s\n"
                   "# threshold=%d ms\n"
                   "# time album total_ms result image bytes kind read_ms "
                   "decode_ms write_ms \"album\" / \"artist\"\n",
                ts, SLOW_ALBUM_MS);
    }
    fclose(f);
}

static void art_set_info(int w, int h, const char * kind)
{
    s_last_w = w;
    s_last_h = h;
    snprintf(s_last_kind, sizeof(s_last_kind), "%s", kind);
}

static void cache_slow_log_add(uint64_t id, const char * album,
                               const char * artist, uint64_t total_ms,
                               bool ok)
{
    char path[160];
    snprintf(path, sizeof(path), "%s/slow.log",
             app_config_library_dir());
    FILE * f = fopen(path, "a");
    if(f == NULL) return;
    setvbuf(f, NULL, _IONBF, 0);

    char ts[32];
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tmv);

    char safe_album[96];
    char safe_artist[96];
    log_safe(safe_album, sizeof(safe_album), album);
    log_safe(safe_artist, sizeof(safe_artist), artist);

    fprintf(f, "%s album=%llu total=%llums result=%s image=%dx%d "
               "bytes=%u kind=%s read=%dms decode=%dms write=%dms "
               "\"%s\" / \"%s\"\n",
            ts, (unsigned long long)id, (unsigned long long)total_ms,
            ok ? "ok" : "failed", s_last_w, s_last_h,
            (unsigned)s_last_bytes, s_last_kind,
            s_last_read_ms, s_last_decode_ms, s_last_write_ms,
            safe_album, safe_artist);
    fclose(f);
}

static media_index_t s_afc_index;
static const media_index_album_t *afc_album_slot(uint64_t slot)
{
    return media_index_album(&s_afc_index,thumb_cache_pid((uint32_t)slot));
}
static bool afc_artwork_remote(uint64_t slot,char *out,size_t size)
{
    const media_index_album_t *a=afc_album_slot(slot);
    if(!a || !a->remote[0]) return false;
    snprintf(out,size,"%s",a->remote);return true;
}
static esp_err_t afc_write_thumb(uint32_t slot,const uint8_t *data)
{
    const media_index_album_t *a=afc_album_slot(slot);
    esp_err_t result=a && thumb_cache_has(slot)
        ? thumb_cache_replace(a->pid,a->artwork_version,data)
        : thumb_cache_write(slot,data);
    if(result==ESP_OK && a && thumb_cache_pid(slot)==a->pid) thumb_cache_version(slot,a->artwork_version);
    return result;
}
static void artwork_set_current(uint64_t id)
{
    s_art_current = id;
    const media_index_album_t *album=afc_album_slot(id);
    snprintf(s_art_album,sizeof(s_art_album),"%s",album?album->title:"");
    snprintf(s_art_artist,sizeof(s_art_artist),"%s",album?album->artist:"");
}

static SemaphoreHandle_t s_pair_lock;
static char * s_pair_ram;
static size_t s_pair_len;
static _Atomic bool s_pair_requested;
static lockdown_pair_status_t s_pair_status;

static void pair_state(bool busy, bool trusted, const char * message)
{
    xSemaphoreTake(s_pair_lock, portMAX_DELAY);
    s_pair_status.busy = busy;
    s_pair_status.trusted = trusted;
    snprintf(s_pair_status.message, sizeof(s_pair_status.message), "%s", message);
    xSemaphoreGive(s_pair_lock);
}

void lockdown_client_pair_status(lockdown_pair_status_t * out)
{
    memset(out, 0, sizeof(*out));
    if(!s_pair_lock) return;
    xSemaphoreTake(s_pair_lock, portMAX_DELAY);
    *out = s_pair_status;
    xSemaphoreGive(s_pair_lock);
}

esp_err_t lockdown_client_request_pair(void)
{
    if(!s_pair_lock || !usbmux_host_is_ready() || app_config_iap2_mode() ||
       s_art_active || afc_client_is_ready())
        return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_pair_lock, portMAX_DELAY);
    if(s_pair_status.busy || s_pair_status.trusted) {
        xSemaphoreGive(s_pair_lock);
        return ESP_ERR_INVALID_STATE;
    }
    s_pair_status.busy = true;
    snprintf(s_pair_status.message, sizeof(s_pair_status.message), "Preparing trust...");
    s_pair_requested = true;
    xSemaphoreGive(s_pair_lock);
    return ESP_OK;
}

static mbedtls_ssl_context s_ssl;
static mbedtls_ssl_config s_ssl_conf;
static mbedtls_x509_crt s_client_crt;
static mbedtls_pk_context s_client_key;
static mbedtls_entropy_context s_entropy;
static mbedtls_ctr_drbg_context s_ctr_drbg;
static bool s_tls_active;

static const char QUERY_TYPE_XML[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
    "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
    "<plist version=\"1.0\">\n"
    "<dict>\n"
    "  <key>Label</key><string>tab5-afc</string>\n"
    "  <key>Request</key><string>QueryType</string>\n"
    "</dict>\n"
    "</plist>\n";

static void set_status(const char * fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_status, sizeof(s_status), fmt, ap);
    va_end(ap);
}

static void wr_be32(uint8_t * p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t rd_be32(const uint8_t * p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

/* ------------------------------------------------------------------ */
/* Pair record handling                                                */
/* ------------------------------------------------------------------ */

static int b64_val(char c)
{
    if(c >= 'A' && c <= 'Z') return c - 'A';
    if(c >= 'a' && c <= 'z') return c - 'a' + 26;
    if(c >= '0' && c <= '9') return c - '0' + 52;
    if(c == '+') return 62;
    if(c == '/') return 63;
    return -1;
}

static uint8_t * b64_decode(const char * in, size_t in_len, size_t * out_len)
{
    uint8_t * out = malloc(in_len * 3 / 4 + 8);
    if(out == NULL) return NULL;

    size_t o = 0;
    int quad[4];
    int qn = 0;

    for(size_t i = 0; i < in_len; i++) {
        char c = in[i];
        int v;
        if(c == '=') {
            v = -2;
        }
        else {
            v = b64_val(c);
            if(v < 0) continue;   /* whitespace / newlines */
        }
        quad[qn++] = v;
        if(qn == 4) {
            if(quad[0] >= 0 && quad[1] >= 0) {
                out[o++] = (uint8_t)((quad[0] << 2) | (quad[1] >> 4));
            }
            if(quad[1] >= 0 && quad[2] >= 0) {
                out[o++] = (uint8_t)(((quad[1] & 0x0F) << 4) |
                                     (quad[2] >> 2));
            }
            if(quad[2] >= 0 && quad[3] >= 0) {
                out[o++] = (uint8_t)(((quad[2] & 0x03) << 6) | quad[3]);
            }
            qn = 0;
        }
    }

    out[o] = '\0';
    *out_len = o;
    return out;
}

static bool plist_get_string(const char * xml, const char * key,
                             char * out, size_t out_len)
{
    char pattern[96];
    snprintf(pattern, sizeof(pattern), "<key>%s</key>", key);

    const char * p = strstr(xml, pattern);
    if(p == NULL) return false;
    p += strlen(pattern);

    while(*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if(strncmp(p, "<string>", 8) != 0) return false;
    p += 8;

    const char * end = strstr(p, "</string>");
    if(end == NULL) return false;

    size_t n = (size_t)(end - p);
    if(n >= out_len) n = out_len - 1;
    memcpy(out, p, n);
    out[n] = '\0';
    return true;
}

static bool plist_get_bool(const char * xml, const char * key)
{
    char pattern[96];
    snprintf(pattern, sizeof(pattern), "<key>%s</key>", key);

    const char * p = strstr(xml, pattern);
    if(p == NULL) return false;
    p += strlen(pattern);

    while(*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return strncmp(p, "<true/>", 7) == 0;
}

static bool plist_get_uint(const char * xml, const char * key, uint64_t * out)
{
    char pattern[96];
    snprintf(pattern, sizeof(pattern), "<key>%s</key>", key);

    const char * p = strstr(xml, pattern);
    if(p == NULL) return false;
    p += strlen(pattern);

    while(*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    if(strncmp(p, "<integer>", 9) != 0) return false;
    p += 9;

    char * end = NULL;
    unsigned long long v = strtoull(p, &end, 10);
    if(end == NULL || strncmp(end, "</integer>", 10) != 0) return false;
    *out = (uint64_t)v;
    return true;
}

static uint8_t * plist_get_data(const char * xml, const char * key,
                                size_t * out_len)
{
    char pattern[96];
    snprintf(pattern, sizeof(pattern), "<key>%s</key>", key);

    const char * p = strstr(xml, pattern);
    if(p == NULL) return NULL;
    p += strlen(pattern);

    p = strstr(p, "<data>");
    if(p == NULL) return NULL;
    p += 6;

    const char * end = strstr(p, "</data>");
    if(end == NULL) return NULL;

    return b64_decode(p, (size_t)(end - p), out_len);
}

static char * read_file(const char * path, size_t * out_len)
{
    FILE * f = fopen(path, "rb");
    if(f == NULL) return NULL;

    if(fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long size = ftell(f);
    if(size <= 0 || size > MAX_PAIR_LEN) {
        fclose(f);
        return NULL;
    }
    rewind(f);

    char * buf = heap_caps_malloc((size_t)size + 1, MALLOC_CAP_SPIRAM);
    if(buf == NULL) {
        fclose(f);
        return NULL;
    }

    size_t n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if(n != (size_t)size) {
        free(buf);
        return NULL;
    }
    buf[n] = '\0';
    *out_len = n;
    return buf;
}

static char * load_pair_record(size_t * out_len)
{
    if(s_pair_lock != NULL &&
       xSemaphoreTake(s_pair_lock, pdMS_TO_TICKS(1000)) == pdTRUE) {
        if(s_pair_ram != NULL) {
            char * copy = heap_caps_malloc(s_pair_len + 1, MALLOC_CAP_SPIRAM);
            if(copy != NULL) {
                memcpy(copy, s_pair_ram, s_pair_len + 1);
                *out_len = s_pair_len;
            }
            xSemaphoreGive(s_pair_lock);
            return copy;
        }
        xSemaphoreGive(s_pair_lock);
    }

    char * buf = read_file(PAIR_PATH_SPIFS, out_len);
    if(buf == NULL) buf = read_file(PAIR_PATH_SPIFS ".bak", out_len);
    if(buf == NULL) buf = read_file(PAIR_PATH_SD, out_len);
    return buf;
}

esp_err_t lockdown_client_set_pair_record(const char * data, size_t len)
{
    if(data == NULL || len < 100 || len > MAX_PAIR_LEN) {
        return ESP_ERR_INVALID_ARG;
    }
    if(s_pair_lock == NULL) return ESP_ERR_INVALID_STATE;

    char * copy = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
    if(copy == NULL) return ESP_ERR_NO_MEM;
    memcpy(copy, data, len);
    copy[len] = '\0';

    if(strstr(copy, "<plist") == NULL) {
        free(copy);
        return ESP_ERR_INVALID_ARG;
    }

    /* Publish only after a complete durable write. Keep the previous identity
     * on any failure; uploading or retrying trust must not erase a working pair. */
    xSemaphoreTake(s_pair_lock, portMAX_DELAY);
    const char * tmp = PAIR_PATH_SPIFS ".tmp";
    const char * backup = PAIR_PATH_SPIFS ".bak";
    FILE * f = fopen(tmp, "wb");
    bool ok = f != NULL;
    if(f) {
        ok = fwrite(copy, 1, len, f) == len;
        if(fclose(f) != 0) ok = false;
    }
    bool moved = false;
    if(ok) {
        struct stat st;
        if(stat(PAIR_PATH_SPIFS, &st) == 0) {
            remove(backup);
            moved = rename(PAIR_PATH_SPIFS, backup) == 0;
            ok = moved;
        }
    }
    if(ok) ok = rename(tmp, PAIR_PATH_SPIFS) == 0;
    if(!ok) {
        if(moved) rename(backup, PAIR_PATH_SPIFS);
        remove(tmp);
        xSemaphoreGive(s_pair_lock);
        free(copy);
        return ESP_FAIL;
    }
    free(s_pair_ram);
    s_pair_ram = copy;
    s_pair_len = len;
    xSemaphoreGive(s_pair_lock);

    set_status("pair record uploaded");
    ESP_LOGI(TAG, "pair record uploaded (%u bytes)", (unsigned)len);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Lockdown property-list transport                                    */
/* ------------------------------------------------------------------ */

static esp_err_t send_plist_plain(usbmux_conn_t * conn, const char * xml)
{
    size_t len = strlen(xml);
    uint8_t header[4];
    wr_be32(header, (uint32_t)len);

    if(usbmux_conn_send(conn, header, sizeof(header)) != sizeof(header)) {
        return ESP_FAIL;
    }
    if(usbmux_conn_send(conn, xml, len) != (int)len) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t recv_plist_plain(usbmux_conn_t * conn, char ** out)
{
    uint8_t header[4];
    esp_err_t err = usbmux_conn_recv_exact(conn, header, sizeof(header), 5000);
    if(err != ESP_OK) {
        ESP_LOGW(TAG, "no plist length: %s", esp_err_to_name(err));
        return err;
    }

    uint32_t len = rd_be32(header);
    if(len == 0 || len > MAX_PLIST_LEN) {
        ESP_LOGW(TAG, "invalid plist length %u", (unsigned)len);
        return ESP_ERR_INVALID_SIZE;
    }

    char * buf = malloc(len + 1);
    if(buf == NULL) return ESP_ERR_NO_MEM;

    err = usbmux_conn_recv_exact(conn, buf, len, 10000);
    if(err != ESP_OK) {
        free(buf);
        return err;
    }
    buf[len] = '\0';
    *out = buf;
    return ESP_OK;
}

static esp_err_t tls_send_all(const void * data, size_t len)
{
    const uint8_t * p = (const uint8_t *)data;
    size_t sent = 0;
    TickType_t start = xTaskGetTickCount();

    while(sent < len) {
        int n = mbedtls_ssl_write(&s_ssl, p + sent, len - sent);
        if(n == MBEDTLS_ERR_SSL_WANT_READ ||
           n == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if((xTaskGetTickCount() - start) > pdMS_TO_TICKS(30000)) {
                return ESP_ERR_TIMEOUT;
            }
            continue;
        }
        if(n <= 0) return ESP_FAIL;
        sent += (size_t)n;
    }
    return ESP_OK;
}

static esp_err_t tls_recv_exact(void * data, size_t len)
{
    uint8_t * p = (uint8_t *)data;
    size_t got = 0;
    TickType_t start = xTaskGetTickCount();

    while(got < len) {
        int n = mbedtls_ssl_read(&s_ssl, p + got, len - got);
        if(n == MBEDTLS_ERR_SSL_WANT_READ ||
           n == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if((xTaskGetTickCount() - start) > pdMS_TO_TICKS(30000)) {
                return ESP_ERR_TIMEOUT;
            }
            continue;
        }
        if(n <= 0) return ESP_FAIL;
        got += (size_t)n;
    }
    return ESP_OK;
}

static esp_err_t send_plist_tls(const char * xml)
{
    size_t len = strlen(xml);
    uint8_t header[4];
    wr_be32(header, (uint32_t)len);

    esp_err_t err = tls_send_all(header, sizeof(header));
    if(err != ESP_OK) return err;
    return tls_send_all(xml, len);
}

static esp_err_t recv_plist_tls(char ** out)
{
    uint8_t header[4];
    esp_err_t err = tls_recv_exact(header, sizeof(header));
    if(err != ESP_OK) {
        ESP_LOGW(TAG, "TLS plist length failed: %s", esp_err_to_name(err));
        return err;
    }

    uint32_t len = rd_be32(header);
    if(len == 0 || len > MAX_PLIST_LEN) return ESP_ERR_INVALID_SIZE;

    char * buf = malloc(len + 1);
    if(buf == NULL) return ESP_ERR_NO_MEM;

    err = tls_recv_exact(buf, len);
    if(err != ESP_OK) {
        free(buf);
        return err;
    }
    buf[len] = '\0';
    *out = buf;
    return ESP_OK;
}

static esp_err_t send_plist(const char * xml)
{
    if(s_tls_active) return send_plist_tls(xml);
    return send_plist_plain(s_conn, xml);
}

static esp_err_t recv_plist(char ** out)
{
    if(s_tls_active) return recv_plist_tls(out);
    return recv_plist_plain(s_conn, out);
}

/* ------------------------------------------------------------------ */
/* TLS                                                                 */
/* ------------------------------------------------------------------ */

static void tls_stop(void)
{
    if(s_tls_active) {
        mbedtls_ssl_close_notify(&s_ssl);
    }
    mbedtls_ssl_free(&s_ssl);
    mbedtls_ssl_config_free(&s_ssl_conf);
    mbedtls_x509_crt_free(&s_client_crt);
    mbedtls_pk_free(&s_client_key);
    mbedtls_ctr_drbg_free(&s_ctr_drbg);
    mbedtls_entropy_free(&s_entropy);
    s_tls_active = false;
}

static int tls_send_cb(void * ctx, const unsigned char * buf, size_t len)
{
    int n = usbmux_conn_send((usbmux_conn_t *)ctx, buf, len);
    if(n <= 0) return MBEDTLS_ERR_SSL_INTERNAL_ERROR;
    return n;
}

static int tls_recv_cb(void * ctx, unsigned char * buf, size_t len)
{
    int n = usbmux_conn_recv((usbmux_conn_t *)ctx, buf, len, 15000);
    if(n < 0) return MBEDTLS_ERR_SSL_CONN_EOF;
    if(n == 0) return MBEDTLS_ERR_SSL_WANT_READ;
    return n;
}

static void tls_log_error(const char * what, int ret)
{
    char errbuf[128] = {0};
    mbedtls_strerror(ret, errbuf, sizeof(errbuf));
    ESP_LOGE(TAG, "%s failed: -0x%04x %s", what, -ret, errbuf);
}

static esp_err_t tls_start(usbmux_conn_t * conn, const uint8_t * cert,
                           size_t cert_len, const uint8_t * key,
                           size_t key_len)
{
    tls_stop();
    mbedtls_ssl_init(&s_ssl);
    mbedtls_ssl_config_init(&s_ssl_conf);
    mbedtls_x509_crt_init(&s_client_crt);
    mbedtls_pk_init(&s_client_key);
    mbedtls_entropy_init(&s_entropy);
    mbedtls_ctr_drbg_init(&s_ctr_drbg);

    int ret = mbedtls_ctr_drbg_seed(&s_ctr_drbg, mbedtls_entropy_func,
                                    &s_entropy,
                                    (const unsigned char *)"tab5-lockdown",
                                    13);
    if(ret != 0) {
        tls_log_error("ctr_drbg_seed", ret);
        return ESP_FAIL;
    }

    /* Pair records contain PEM data in the RootCertificate and
     * RootPrivateKey fields; libimobiledevice uses these as the client
     * identity for the lockdown TLS session. */
    ret = mbedtls_x509_crt_parse(&s_client_crt, cert, cert_len + 1);
    if(ret != 0) {
        tls_log_error("crt_parse", ret);
        return ESP_FAIL;
    }

    ret = mbedtls_pk_parse_key(&s_client_key, key, key_len + 1,
                               NULL, 0, mbedtls_ctr_drbg_random, &s_ctr_drbg);
    if(ret != 0) {
        tls_log_error("pk_parse_key", ret);
        return ESP_FAIL;
    }

    ret = mbedtls_ssl_config_defaults(&s_ssl_conf, MBEDTLS_SSL_IS_CLIENT,
                                      MBEDTLS_SSL_TRANSPORT_STREAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    if(ret != 0) {
        tls_log_error("ssl_config_defaults", ret);
        return ESP_FAIL;
    }

    mbedtls_ssl_conf_authmode(&s_ssl_conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&s_ssl_conf, mbedtls_ctr_drbg_random, &s_ctr_drbg);
    mbedtls_ssl_conf_own_cert(&s_ssl_conf, &s_client_crt, &s_client_key);
    mbedtls_ssl_conf_min_tls_version(&s_ssl_conf,
                                     MBEDTLS_SSL_VERSION_TLS1_2);

    ret = mbedtls_ssl_setup(&s_ssl, &s_ssl_conf);
    if(ret != 0) {
        tls_log_error("ssl_setup", ret);
        return ESP_FAIL;
    }

    mbedtls_ssl_set_bio(&s_ssl, conn, tls_send_cb, tls_recv_cb, NULL);

    TickType_t start = xTaskGetTickCount();
    for(;;) {
        ret = mbedtls_ssl_handshake(&s_ssl);
        if(ret == 0) break;
        if(ret == MBEDTLS_ERR_SSL_WANT_READ ||
           ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            if((xTaskGetTickCount() - start) > pdMS_TO_TICKS(30000)) {
                ESP_LOGE(TAG, "TLS handshake timeout");
                return ESP_ERR_TIMEOUT;
            }
            continue;
        }
        tls_log_error("ssl_handshake", ret);
        return ESP_FAIL;
    }

    s_tls_active = true;
    ESP_LOGI(TAG, "TLS established: %s / %s",
             mbedtls_ssl_get_version(&s_ssl),
             mbedtls_ssl_get_ciphersuite(&s_ssl));
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* Lockdown requests                                                   */
/* ------------------------------------------------------------------ */

static esp_err_t query_type(void)
{
    ESP_LOGI(TAG, "connected to lockdownd, sending QueryType");

    esp_err_t err = send_plist(QUERY_TYPE_XML);
    if(err != ESP_OK) {
        set_status("QueryType send failed");
        return err;
    }

    char * xml = NULL;
    err = recv_plist(&xml);
    if(err != ESP_OK) {
        set_status("QueryType response failed");
        return err;
    }

    ESP_LOGI(TAG, "QueryType reply:\n%s", xml);

    char type[64] = "?";
    plist_get_string(xml, "Type", type, sizeof(type));
    free(xml);

    if(strcmp(type, "com.apple.mobile.lockdown") != 0) {
        ESP_LOGW(TAG, "unexpected lockdownd type '%s'", type);
        set_status("unexpected lockdownd type");
        return ESP_FAIL;
    }

    set_status("lockdownd ready");
    return ESP_OK;
}

static esp_err_t pair_public_key(uint8_t ** key, size_t * len)
{
    const char * request = "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict>"
        "<key>Label</key><string>Tab5</string><key>Request</key><string>GetValue</string>"
        "<key>Key</key><string>DevicePublicKey</string></dict></plist>";
    esp_err_t err = send_plist(request);
    char * reply = NULL;
    if(err == ESP_OK) err = recv_plist(&reply);
    if(err == ESP_OK) {
        *key = plist_get_data(reply, "Value", len);
        if(!*key || !*len) err = ESP_ERR_INVALID_RESPONSE;
    }
    free(reply);
    return err;
}

static esp_err_t pair_with_apple(void)
{
    pair_state(true, false, "Creating this Tab5's pairing identity...");
    uint8_t * key = NULL;
    size_t key_len = 0;
    apple_pairing_identity_t identity = {0};
    esp_err_t err = pair_public_key(&key, &key_len);
    if(err == ESP_OK) err = apple_pairing_generate(key, key_len, &identity);
    if(err != ESP_OK) {
        pair_state(false, false, "Could not create trust. Unlock the device and try again.");
        free(key);
        return err;
    }
    /* Key generation can outlast lockdownd's idle timeout. Reopen and verify
     * the same device's public key before sending the new pairing request. */
    usbmux_conn_close(s_conn);
    s_conn = NULL;
    err = usbmux_connect(LOCKDOWN_PORT, &s_conn, 5000);
    uint8_t * check = NULL;
    size_t check_len = 0;
    if(err == ESP_OK) err = query_type();
    if(err == ESP_OK) err = pair_public_key(&check, &check_len);
    if(err == ESP_OK && (check_len != key_len || memcmp(check, key, key_len)))
        err = ESP_ERR_INVALID_STATE;
    free(check); free(key);
    if(err != ESP_OK) {
        pair_state(false, false, "Connection changed. Reconnect your iPod and try again.");
        apple_pairing_free(&identity);
        return err;
    }
    pair_state(true, false, "Unlock the device and tap Trust on its screen.");
    TickType_t started = xTaskGetTickCount();
    bool finished = false;
    while(usbmux_host_is_ready() &&
          xTaskGetTickCount() - started < pdMS_TO_TICKS(120000)) {
        char * reply = NULL;
        err = send_plist(identity.request_xml);
        if(err == ESP_OK) err = recv_plist(&reply);
        if(err != ESP_OK) { free(reply); break; }
        char error[96] = "";
        if(plist_get_string(reply, "Error", error, sizeof(error))) {
            free(reply);
            if(strcmp(error, "PairingDialogResponsePending") == 0 ||
               strcmp(error, "PasswordProtected") == 0) {
                pair_state(true, false, strcmp(error, "PasswordProtected") == 0 ?
                    "Unlock the device to continue pairing." :
                    "Tap Trust on the device, then enter its passcode.");
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            pair_state(false, false, strcmp(error, "UserDeniedPairing") == 0 ?
                "Trust was declined. Tap Pair / Trust to try again." :
                "Could not pair. Unlock the device and try again.");
            err = ESP_FAIL;
            finished = true;
            break;
        }
        char request[32] = "";
        bool valid = plist_get_string(reply, "Request", request, sizeof(request)) &&
                     strcmp(request, "Pair") == 0;
        if(!valid) { free(reply); err = ESP_ERR_INVALID_RESPONSE; break; }
        /* Preserve Apple's escrow bag in the local record, never in logs. */
        const char * bag = strstr(reply, "<key>EscrowBag</key>");
        if(bag) {
            const char * end = strstr(bag, "</data>");
            if(end) {
                size_t extra = (size_t)(end + 7 - bag);
                char * tail = strstr(identity.record_xml, "</dict></plist>");
                size_t prefix = (size_t)(tail - identity.record_xml);
                char * record = malloc(prefix + extra + 16);
                if(!record) { free(reply); err = ESP_ERR_NO_MEM; break; }
                memcpy(record, identity.record_xml, prefix);
                memcpy(record + prefix, bag, extra);
                strcpy(record + prefix + extra, "</dict></plist>");
                free(identity.record_xml);
                identity.record_xml = record;
            }
        }
        free(reply);
        err = lockdown_client_set_pair_record(identity.record_xml, strlen(identity.record_xml));
        pair_state(false, err == ESP_OK, err == ESP_OK ?
            "" :
            "Trust approved, but saving failed. Tap Pair / Trust to retry.");
        finished = true;
        break;
    }
    if(!finished) {
        pair_state(false, false, usbmux_host_is_ready() ?
            "Trust timed out. Unlock the device and try again." :
            "Device disconnected. Reconnect and try again.");
        err = ESP_ERR_TIMEOUT;
    }
    apple_pairing_free(&identity);
    return err;
}

static esp_err_t start_session(const char * pair)
{
    char host_id[80] = {0};
    char buid[80] = {0};

    if(!plist_get_string(pair, "HostID", host_id, sizeof(host_id)) ||
       host_id[0] == '\0') {
        ESP_LOGE(TAG, "pair record has no HostID");
        set_status("pair record invalid");
        return ESP_ERR_INVALID_ARG;
    }
    plist_get_string(pair, "SystemBUID", buid, sizeof(buid));

    char xml[1024];
    snprintf(xml, sizeof(xml),
             "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
             "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
             "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
             "<plist version=\"1.0\"><dict>\n"
             "<key>Label</key><string>tab5-afc</string>\n"
             "<key>Request</key><string>StartSession</string>\n"
             "<key>HostID</key><string>%s</string>\n"
             "<key>SystemBUID</key><string>%s</string>\n"
             "</dict></plist>\n", host_id, buid);

    ESP_LOGI(TAG, "sending StartSession (HostID %s)", host_id);

    esp_err_t err = send_plist(xml);
    if(err != ESP_OK) {
        set_status("StartSession send failed");
        return err;
    }

    char * resp = NULL;
    err = recv_plist(&resp);
    if(err != ESP_OK) {
        set_status("StartSession response failed");
        return err;
    }

    if(strstr(resp, "<key>Error</key>") != NULL) {
        char error[96] = "?";
        plist_get_string(resp, "Error", error, sizeof(error));
        ESP_LOGE(TAG, "StartSession error: %s", error);
        ESP_LOGD(TAG, "StartSession reply:\n%s", resp);
        set_status("StartSession: %s", error);
        free(resp);
        return ESP_FAIL;
    }

    char session_id[128] = "";
    plist_get_string(resp, "SessionID", session_id, sizeof(session_id));
    bool ssl = plist_get_bool(resp, "EnableSessionSSL");
    free(resp);

    ESP_LOGI(TAG, "StartSession OK, SessionID=%s, SSL=%d", session_id,
             (int)ssl);
    set_status("session active");

    if(ssl) {
        size_t cert_len = 0;
        size_t key_len = 0;
        uint8_t * cert = plist_get_data(pair, "RootCertificate", &cert_len);
        uint8_t * key = plist_get_data(pair, "RootPrivateKey", &key_len);
        ESP_LOGI(TAG, "pair cert=%u bytes key=%u bytes", (unsigned)cert_len,
                 (unsigned)key_len);
        if(cert == NULL || key == NULL || cert_len == 0 || key_len == 0) {
            free(cert);
            free(key);
            ESP_LOGE(TAG, "pair record has no RootCertificate/RootPrivateKey");
            set_status("pair record has no TLS identity");
            return ESP_ERR_INVALID_ARG;
        }

        set_status("TLS handshake");
        err = tls_start(s_conn, cert, cert_len, key, key_len);
        free(cert);
        free(key);
        if(err != ESP_OK) {
            set_status("TLS handshake failed");
            return err;
        }
    }

    set_status("lockdown session active");
    return ESP_OK;
}

static esp_err_t start_service(const char * service, uint16_t * port,
                               bool * ssl)
{
    char xml[640];
    snprintf(xml, sizeof(xml),
             "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
             "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
             "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
             "<plist version=\"1.0\"><dict>\n"
             "<key>Label</key><string>tab5-afc</string>\n"
             "<key>Request</key><string>StartService</string>\n"
             "<key>Service</key><string>%s</string>\n"
             "</dict></plist>\n", service);

    ESP_LOGI(TAG, "starting service %s", service);

    esp_err_t err = send_plist(xml);
    if(err != ESP_OK) return err;

    char * resp = NULL;
    err = recv_plist(&resp);
    if(err != ESP_OK) return err;

    if(strstr(resp, "<key>Error</key>") != NULL) {
        char error[96] = "?";
        plist_get_string(resp, "Error", error, sizeof(error));
        ESP_LOGE(TAG, "StartService %s error: %s", service, error);
        set_status("StartService: %s", error);
        free(resp);
        return ESP_FAIL;
    }

    uint64_t value = 0;
    if(!plist_get_uint(resp, "Port", &value) || value == 0 || value > 65535) {
        ESP_LOGE(TAG, "StartService %s returned no Port", service);
        ESP_LOGD(TAG, "StartService reply:\n%s", resp);
        free(resp);
        return ESP_ERR_INVALID_RESPONSE;
    }

    *port = (uint16_t)value;
    *ssl = plist_get_bool(resp, "EnableServiceSSL");
    free(resp);

    ESP_LOGI(TAG, "service %s on port %u (ssl=%d)", service,
             (unsigned)*port, (int)*ssl);
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* AFC bring-up                                                        */
/* ------------------------------------------------------------------ */

static void afc_probe(void)
{
    char list[2048];

    if(afc_client_list("/", list, sizeof(list)) == ESP_OK) {
        ESP_LOGI(TAG, "AFC / entries:\n%s", list);
    }

    if(afc_client_list("/iTunes_Control/iTunes", list, sizeof(list)) ==
       ESP_OK) {
        ESP_LOGI(TAG, "AFC /iTunes_Control/iTunes entries:\n%s", list);
    }

    static const char * files[] = {
        "/iTunes_Control/iTunes/MediaLibrary.sqlitedb",
        "/iTunes_Control/iTunes/MediaLibrary.sqlitedb-wal",
        "/iTunes_Control/iTunes/MediaLibrary.sqlitedb-shm",
    };

    for(size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        uint64_t size = 0;
        char type[32] = "";
        esp_err_t err = afc_client_stat(files[i], &size, type, sizeof(type));
        if(err == ESP_OK) {
            ESP_LOGI(TAG, "%s: type=%s size=%llu", files[i], type,
                     (unsigned long long)size);
        }
        else {
            ESP_LOGW(TAG, "%s: stat failed", files[i]);
        }
    }

    const char * ml = "/iTunes_Control/iTunes/MediaLibrary.sqlitedb";
    uint8_t head[16];
    int n = afc_client_read_file(ml, head, sizeof(head));
    if(n > 0) {
        ESP_LOGI(TAG, "read %d bytes from MediaLibrary.sqlitedb OK", n);
    }
    else {
        ESP_LOGW(TAG, "MediaLibrary.sqlitedb read failed");
    }
}

/* ------------------------------------------------------------------ */
/* Main task                                                           */
/* ------------------------------------------------------------------ */

static void lockdown_task(void * arg)
{
    (void)arg;

    for(;;) {
        if(!usbmux_host_wait_ready(60000)) {
            s_pair_requested = false;
            pair_state(false, false, "Connect and unlock your iPod to pair.");
            set_status("waiting for iPod");
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(100));

        usbmux_conn_t * conn = NULL;
        esp_err_t err = usbmux_connect(LOCKDOWN_PORT, &conn, 5000);
        if(err != ESP_OK) {
            ESP_LOGW(TAG, "lockdownd connect failed: %s",
                     esp_err_to_name(err));
            set_status("lockdownd connect failed");
            if(atomic_exchange(&s_pair_requested, false))
                pair_state(false, false, "Could not connect. Unlock the device and retry pairing.");
            vTaskDelay(pdMS_TO_TICKS(3000));
            continue;
        }
        s_conn = conn;

        if(query_type() != ESP_OK) {
            if(atomic_exchange(&s_pair_requested, false))
                pair_state(false, false, "Connection failed. Reconnect and retry pairing.");
            goto cleanup;
        }

        if(atomic_exchange(&s_pair_requested, false)) {
            if(pair_with_apple() != ESP_OK) goto cleanup;
        }

        /* StartSession needs the host pair record. It can arrive from the
         * SD card, the internal SPIFFS storage, or the web upload. */
        char * pair = NULL;
        size_t pair_len = 0;
        for(;;) {
            pair = load_pair_record(&pair_len);
            if(pair != NULL) break;
            if(s_pair_requested) goto cleanup;
            set_status("Pair / Trust to connect");
            if(!usbmux_host_is_ready()) break;
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        if(pair == NULL) goto cleanup;

        ESP_LOGI(TAG, "using pair record (%u bytes)", (unsigned)pair_len);
        err = start_session(pair);
        free(pair);
        if(err != ESP_OK) {
            pair_state(false, false, "Trust needs renewing. Unlock the device and tap Pair / Trust.");
            goto cleanup;
        }
        pair_state(false, true, "");

        uint16_t afc_port = 0;
        bool afc_ssl = false;
        if(start_service("com.apple.afc", &afc_port, &afc_ssl) == ESP_OK) {
            if(!afc_ssl) {
                if(afc_client_open(afc_port) == ESP_OK) {
                    afc_probe();
                    set_status("AFC ready");
                }
                else {
                    set_status("AFC connect failed");
                }
            }
            else {
                ESP_LOGW(TAG, "AFC requested SSL; use the TLS service path");
                set_status("AFC SSL not implemented");
            }
        }

        if(afc_client_is_ready()) {
            /* lockdownd is only needed to start the service. Closing it
             * here avoids the device's idle-session reset while a long AFC
             * copy is running; the AFC connection stays valid. */
            ESP_LOGI(TAG, "closing lockdownd; holding AFC only");
            tls_stop();
            usbmux_conn_close(s_conn);
            s_conn = NULL;
            set_status("AFC ready");

            while(usbmux_host_is_ready() && afc_client_is_ready() && !s_pair_requested) {
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            ESP_LOGW(TAG, "AFC connection closed");
        }
        else {
            ESP_LOGW(TAG, "AFC not ready; dropping session");
        }

cleanup:
        afc_client_close();
        tls_stop();
        if(s_conn != NULL) {
            usbmux_conn_close(s_conn);
            s_conn = NULL;
        }
        set_status("disconnected");
        if(!usbmux_host_is_ready()) pair_state(false, false, "Reconnect your device to continue.");
        vTaskDelay(pdMS_TO_TICKS(s_pair_requested ? 50 : 3000));
    }
}


static const char * artwork_dir(void)
{
    return app_config_artwork_dir();
}

#define ART_THUMB_SIZE 54
#define ART_COVER_SIZE 360

static uint8_t * s_art_buf;
static uint8_t * s_cover_buf;

static void art_sample_rect(const lv_area_t * area, const uint8_t * data,
                            uint32_t stride, lv_color_format_t cf,
                            uint8_t * dst, int tw, int th, int iw, int ih)
{
    int bpp;
    switch(cf) {
        case LV_COLOR_FORMAT_RGB565:
            bpp = 2;
            break;
        case LV_COLOR_FORMAT_RGB888:
            bpp = 3;
            break;
        case LV_COLOR_FORMAT_ARGB8888:
        case LV_COLOR_FORMAT_XRGB8888:
            bpp = 4;
            break;
        default:
            return;
    }

    int tx1 = (int)((int64_t)area->x1 * tw / iw);
    int tx2 = (int)((((int64_t)(area->x2 + 1) * tw) + iw - 1) / iw) - 1;
    int ty1 = (int)((int64_t)area->y1 * th / ih);
    int ty2 = (int)((((int64_t)(area->y2 + 1) * th) + ih - 1) / ih) - 1;
    if(tx1 < 0) tx1 = 0;
    if(ty1 < 0) ty1 = 0;
    if(tx2 >= tw) tx2 = tw - 1;
    if(ty2 >= th) ty2 = th - 1;
    if(tx1 > tx2 || ty1 > ty2) return;

    uint16_t * out = (uint16_t *)dst;
    for(int ty = ty1; ty <= ty2; ty++) {
        int sy = (int)((((int64_t)ty * 2 + 1) * ih) / (2 * th));
        if(sy < area->y1 || sy > area->y2) continue;
        const uint8_t * row = data + (size_t)(sy - area->y1) * stride;
        for(int tx = tx1; tx <= tx2; tx++) {
            int sx = (int)((((int64_t)tx * 2 + 1) * iw) / (2 * tw));
            if(sx < area->x1 || sx > area->x2) continue;
            const uint8_t * p = row + (size_t)(sx - area->x1) * bpp;
            uint8_t r, g, b;
            if(cf == LV_COLOR_FORMAT_RGB565) {
                uint16_t v = (uint16_t)(p[0] | (p[1] << 8));
                uint8_t r5 = (v >> 11) & 0x1F;
                uint8_t g6 = (v >> 5) & 0x3F;
                uint8_t b5 = v & 0x1F;
                r = (uint8_t)((r5 << 3) | (r5 >> 2));
                g = (uint8_t)((g6 << 2) | (g6 >> 4));
                b = (uint8_t)((b5 << 3) | (b5 >> 2));
            }
            else {
                b = p[0];
                g = p[1];
                r = p[2];
            }
            out[ty * tw + tx] = (uint16_t)(((r & 0xF8) << 8) |
                                           ((g & 0xFC) << 3) |
                                           (b >> 3));
        }
    }
}

/* Decode the source image into the packed RGB565 thumb/cover buffers.
 * TJPGD returns partial MCU areas, PNG decoders return the full image; both
 * are sampled directly so LVGL never has to transform a partially decoded
 * JPEG. */
static bool art_decode_sampled(const char * lv_src, uint8_t * thumb,
                               uint8_t * cover)
{
    lv_image_decoder_dsc_t dsc;
    if(lv_image_decoder_open(&dsc, lv_src, NULL) != LV_RESULT_OK) {
        ESP_LOGW(TAG, "art %s: decoder open failed", lv_src);
        return false;
    }

    int iw = dsc.header.w;
    int ih = dsc.header.h;
    if(iw > 0 && ih > 0) {
        s_last_bytes = 0;
        art_set_info(iw, ih, "lvgl");
    }
    if(iw <= 0 || ih <= 0) {
        ESP_LOGW(TAG, "art %s: bad size %dx%d", lv_src, iw, ih);
        lv_image_decoder_close(&dsc);
        return false;
    }

    lv_area_t full = { 0, 0, iw - 1, ih - 1 };
    bool got = false;

    if(dsc.decoded != NULL && dsc.decoded->data != NULL) {
        const lv_draw_buf_t * db = dsc.decoded;
        art_sample_rect(&full, db->data, db->header.stride, db->header.cf,
                        thumb, ART_THUMB_SIZE, ART_THUMB_SIZE, iw, ih);
        if(cover != NULL) {
            art_sample_rect(&full, db->data, db->header.stride,
                            db->header.cf, cover, ART_COVER_SIZE,
                            ART_COVER_SIZE, iw, ih);
        }
        got = true;
    }
    else {
        lv_area_t area = { LV_COORD_MIN, LV_COORD_MIN,
                           LV_COORD_MIN, LV_COORD_MIN };
        int guard = ((iw + 7) / 8 + 2) * ((ih + 7) / 8 + 2);
        while(guard-- > 0) {
            if(lv_image_decoder_get_area(&dsc, &full, &area) != LV_RESULT_OK) {
                break;
            }
            const lv_draw_buf_t * db = dsc.decoded;
            if(db == NULL || db->data == NULL) break;
            art_sample_rect(&area, db->data, db->header.stride,
                            db->header.cf, thumb, ART_THUMB_SIZE,
                            ART_THUMB_SIZE, iw, ih);
            if(cover != NULL) {
                art_sample_rect(&area, db->data, db->header.stride,
                                db->header.cf, cover, ART_COVER_SIZE,
                                ART_COVER_SIZE, iw, ih);
            }
            got = true;
            if(area.x2 >= iw - 1 && area.y2 >= ih - 1) break;
        }
    }

    lv_image_decoder_close(&dsc);
    if(got) {
        ESP_LOGI(TAG, "art %s: %dx%d decoded", lv_src, iw, ih);
    }
    else {
        ESP_LOGW(TAG, "art %s: no pixel data", lv_src);
    }
    return got;
}

/* Scaled software fallback. The stock LVGL TJPGD decoder always decodes at
 * 1:1; a 3000x3000 cover would take ~100 s even though the final .cov is only
 * 360x360. Decode just large enough for the cover and sample each MCU as it
 * comes out of the decoder. */
typedef struct {
    FILE * f;
    const uint8_t * data;
    size_t len;
    size_t pos;
    uint8_t * thumb;
    uint8_t * cover;
    int out_w;
    int out_h;
} tjpgd_ctx_t;

static size_t tjpgd_input(JDEC * jd, uint8_t * buff, size_t ndata)
{
    tjpgd_ctx_t * ctx = (tjpgd_ctx_t *)jd->device;
    if(ctx->data != NULL) {
        size_t avail = ctx->len > ctx->pos ? ctx->len - ctx->pos : 0;
        if(ndata > avail) ndata = avail;
        if(buff != NULL && ndata > 0) {
            memcpy(buff, ctx->data + ctx->pos, ndata);
        }
        ctx->pos += ndata;
        return ndata;
    }
    if(buff != NULL) {
        return fread(buff, 1, ndata, ctx->f);
    }
    if(fseek(ctx->f, (long)ndata, SEEK_CUR) != 0) return 0;
    return ndata;
}

static int64_t s_tjpgd_yield_us;

static void tjpgd_yield_if_needed(void)
{
    int64_t now = esp_timer_get_time();
    if(now - s_tjpgd_yield_us >= 500000) {
        s_tjpgd_yield_us = now;
        vTaskDelay(1);
    }
}

static int tjpgd_output(JDEC * jd, void * bitmap, JRECT * rect)
{
    tjpgd_ctx_t * ctx = (tjpgd_ctx_t *)jd->device;
    tjpgd_yield_if_needed();
    int bw = (int)rect->right - (int)rect->left + 1;
    lv_area_t area = { rect->left, rect->top, rect->right, rect->bottom };
    uint32_t stride = (uint32_t)bw * 2;
    art_sample_rect(&area, (const uint8_t *)bitmap, stride,
                    LV_COLOR_FORMAT_RGB565, ctx->thumb,
                    ART_THUMB_SIZE, ART_THUMB_SIZE, ctx->out_w, ctx->out_h);
    if(ctx->cover != NULL) {
        art_sample_rect(&area, (const uint8_t *)bitmap, stride,
                        LV_COLOR_FORMAT_RGB565, ctx->cover,
                        ART_COVER_SIZE, ART_COVER_SIZE, ctx->out_w, ctx->out_h);
    }
    return 1;
}

static bool tjpgd_decode_sampled(const char * src, uint8_t * thumb,
                                 uint8_t * cover, int target)
{
    FILE * f = fopen(src, "rb");
    if(f == NULL) return false;
    /* TJPGD asks for 512 bytes at a time. An aligned stdio buffer batches
     * these into efficient SD reads for oversized originals. */
    uint8_t * input = heap_caps_aligned_alloc(128, 64 * 1024,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(input != NULL) setvbuf(f, (char *)input, _IOFBF, 64 * 1024);

    void * pool = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(pool == NULL) {
        fclose(f);
        heap_caps_free(input);
        return false;
    }

    s_tjpgd_yield_us = esp_timer_get_time();
    tjpgd_ctx_t ctx = {
        .f = f,
        .thumb = thumb,
        .cover = cover,
    };
    JDEC jd;
    JRESULT rc = tab5_jd_prepare(&jd, tjpgd_input, pool, 4096, &ctx);
    if(rc != JDR_OK) {
        heap_caps_free(pool);
        fclose(f);
        heap_caps_free(input);
        return false;
    }

    int max_dim = jd.width > jd.height ? jd.width : jd.height;
    uint8_t scale = 0;
    while(scale < 3 && (max_dim >> (scale + 1)) >= target) {
        scale++;
    }
    ctx.out_w = jd.width >> scale;
    ctx.out_h = jd.height >> scale;
    if(ctx.out_w <= 0 || ctx.out_h <= 0) {
        heap_caps_free(pool);
        fclose(f);
        heap_caps_free(input);
        return false;
    }

    {
        struct stat st;
        s_last_bytes = stat(src, &st) == 0 ? (uint32_t)st.st_size : 0;
        char kind[24];
        snprintf(kind, sizeof(kind), "sw 1/%u", 1u << scale);
        art_set_info(jd.width, jd.height, kind);
    }
    rc = tab5_jd_decomp(&jd, tjpgd_output, scale);
    heap_caps_free(pool);
    fclose(f);
    heap_caps_free(input);
    if(rc != JDR_OK) return false;
    ESP_LOGI(TAG, "art %s: sw %ux%u scale=1/%u decoded",
             src, (unsigned)jd.width, (unsigned)jd.height, 1u << scale);
    return true;
}

static bool tjpgd_decode_sampled_mem(const uint8_t * data, size_t len,
                                     uint8_t * thumb, uint8_t * cover,
                                     int target)
{
    if(data == NULL || len == 0) return false;

    void * pool = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(pool == NULL) return false;

    s_tjpgd_yield_us = esp_timer_get_time();
    tjpgd_ctx_t ctx = {
        .data = data,
        .len = len,
        .thumb = thumb,
        .cover = cover,
    };
    JDEC jd;
    JRESULT rc = tab5_jd_prepare(&jd, tjpgd_input, pool, 4096, &ctx);
    if(rc != JDR_OK) {
        heap_caps_free(pool);
        return false;
    }

    int max_dim = jd.width > jd.height ? jd.width : jd.height;
    uint8_t scale = 0;
    while(scale < 3 && (max_dim >> (scale + 1)) >= target) {
        scale++;
    }
    ctx.out_w = jd.width >> scale;
    ctx.out_h = jd.height >> scale;
    if(ctx.out_w <= 0 || ctx.out_h <= 0) {
        heap_caps_free(pool);
        return false;
    }

    s_last_bytes = (uint32_t)len;
    {
        char kind[24];
        snprintf(kind, sizeof(kind), "sw 1/%u", 1u << scale);
        art_set_info(jd.width, jd.height, kind);
    }
    rc = tab5_jd_decomp(&jd, tjpgd_output, scale);
    heap_caps_free(pool);
    if(rc != JDR_OK) return false;
    ESP_LOGI(TAG, "art mem: sw %ux%u scale=1/%u decoded",
             (unsigned)jd.width, (unsigned)jd.height, 1u << scale);
    return true;
}

/* ESP32-P4 hardware JPEG decoder. This is dramatically faster than the
 * software TJPGD path for the full-size artwork originals. The decoder pads
 * the output to the JPEG MCU size, so the row stride is recovered from the
 * returned output length. */
static jpeg_decoder_handle_t s_hw_jpeg;
static int s_hw_jpeg_state; /* 0 = not tried, 1 = ready, -1 = unavailable */

static bool hw_jpeg_engine_ready(void)
{
    if(s_hw_jpeg_state == 1) return true;
    if(s_hw_jpeg_state < 0) return false;

    jpeg_decode_engine_cfg_t cfg = {
        .intr_priority = 0,
        .timeout_ms = 3000,
    };
    esp_err_t err = jpeg_new_decoder_engine(&cfg, &s_hw_jpeg);
    if(err != ESP_OK) {
        /* A transient internal-DMA shortage can clear once the current
         * download finishes; retry on the next album instead of giving up. */
        ESP_LOGW(TAG, "hw jpeg: engine init failed (%s), will retry",
                 esp_err_to_name(err));
        return false;
    }
    s_hw_jpeg_state = 1;
    ESP_LOGI(TAG, "hw jpeg: decoder ready");
    return true;
}

static bool hw_jpeg_decode_sampled(const char * src, uint8_t * thumb,
                                   uint8_t * cover)
{
    if(!hw_jpeg_engine_ready()) return false;

    bool ok = false;
    FILE * f = NULL;
    uint8_t * tx = NULL;
    uint8_t * rx = NULL;
    size_t tx_size = 0;
    size_t rx_size = 0;
    jpeg_decode_picture_info_t info;

    f = fopen(src, "rb");
    if(f == NULL) return false;
    if(fseek(f, 0, SEEK_END) != 0) goto out;
    long fsize = ftell(f);
    if(fsize <= 0 || fsize > 8 * 1024 * 1024) goto out;
    if(fseek(f, 0, SEEK_SET) != 0) goto out;

    {
        jpeg_decode_memory_alloc_cfg_t tx_cfg = {
            .buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER,
        };
        tx = jpeg_alloc_decoder_mem((size_t)fsize, &tx_cfg, &tx_size);
    }
    if(tx == NULL) goto out;
    if(fread(tx, 1, (size_t)fsize, f) != (size_t)fsize) goto out;
    fclose(f);
    f = NULL;

    if(jpeg_decoder_get_info(tx, (uint32_t)fsize, &info) != ESP_OK) goto out;
    if(info.width == 0 || info.height == 0) goto out;
    s_last_bytes = (uint32_t)fsize;
    art_set_info((int)info.width, (int)info.height, "hw");

    /* Keep the transient decode buffers inside the PSRAM budget. */
    uint64_t out_need = (uint64_t)(info.width + 16) *
                        (uint64_t)(info.height + 16) * 2;
    if(out_need > 6 * 1024 * 1024) goto out;

    {
        jpeg_decode_memory_alloc_cfg_t rx_cfg = {
            .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
        };
        rx = jpeg_alloc_decoder_mem((size_t)out_need, &rx_cfg, &rx_size);
    }
    if(rx == NULL) goto out;

    {
        jpeg_decode_cfg_t dcfg = {
            .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
            .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
            .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
        };
        uint32_t out_size = 0;
        if(jpeg_decoder_process(s_hw_jpeg, &dcfg, tx, (uint32_t)fsize,
                                rx, (uint32_t)rx_size, &out_size) != ESP_OK) {
            goto out;
        }
        if(out_size < (uint64_t)info.width * info.height * 2) goto out;

        int stride = 0;
        int ph0 = ((int)info.width + 7) & ~7;
        for(int ph = ph0; ph <= ph0 + 16; ph += 8) {
            if(out_size % (uint32_t)(ph * 2) != 0) continue;
            int pv = (int)(out_size / (uint32_t)(ph * 2));
            if(pv >= (int)info.height && pv <= (int)info.height + 16) {
                stride = ph * 2;
                break;
            }
        }
        if(stride == 0) stride = (((int)info.width + 15) & ~15) * 2;

        lv_area_t full = { 0, 0, (int32_t)info.width - 1,
                           (int32_t)info.height - 1 };
        art_sample_rect(&full, rx, (uint32_t)stride,
                        LV_COLOR_FORMAT_RGB565, thumb,
                        ART_THUMB_SIZE, ART_THUMB_SIZE,
                        (int)info.width, (int)info.height);
        if(cover != NULL) {
            art_sample_rect(&full, rx, (uint32_t)stride,
                            LV_COLOR_FORMAT_RGB565, cover,
                            ART_COVER_SIZE, ART_COVER_SIZE,
                            (int)info.width, (int)info.height);
        }
        ok = true;
        ESP_LOGI(TAG, "art %s: hw %ux%u decoded (%u bytes)",
                 src, (unsigned)info.width, (unsigned)info.height,
                 (unsigned)out_size);
    }

out:
    if(f != NULL) fclose(f);
    free(tx);
    free(rx);
    return ok;
}

static bool hw_jpeg_decode_mem(const uint8_t * data, size_t len,
                               uint8_t * thumb, uint8_t * cover)
{
    if(!hw_jpeg_engine_ready() || data == NULL || len == 0) return false;

    jpeg_decode_picture_info_t info;
    if(jpeg_decoder_get_info(data, (uint32_t)len, &info) != ESP_OK) return false;
    if(info.width == 0 || info.height == 0) return false;
    s_last_bytes = (uint32_t)len;
    art_set_info((int)info.width, (int)info.height, "hw");

    uint64_t out_need = (uint64_t)(info.width + 16) *
                        (uint64_t)(info.height + 16) * 2;
    if(out_need > 6 * 1024 * 1024) return false;

    jpeg_decode_memory_alloc_cfg_t rx_cfg = {
        .buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER,
    };
    size_t rx_size = 0;
    uint8_t * rx = jpeg_alloc_decoder_mem((size_t)out_need, &rx_cfg, &rx_size);
    if(rx == NULL) return false;

    bool ok = false;
    jpeg_decode_cfg_t dcfg = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint32_t out_size = 0;
    if(jpeg_decoder_process(s_hw_jpeg, &dcfg, data, (uint32_t)len,
                            rx, (uint32_t)rx_size, &out_size) == ESP_OK &&
       out_size >= (uint64_t)info.width * info.height * 2) {
        int stride = 0;
        int ph0 = ((int)info.width + 7) & ~7;
        for(int ph = ph0; ph <= ph0 + 16; ph += 8) {
            if(out_size % (uint32_t)(ph * 2) != 0) continue;
            int pv = (int)(out_size / (uint32_t)(ph * 2));
            if(pv >= (int)info.height && pv <= (int)info.height + 16) {
                stride = ph * 2;
                break;
            }
        }
        if(stride == 0) stride = (((int)info.width + 15) & ~15) * 2;

        lv_area_t full = { 0, 0, (int32_t)info.width - 1,
                           (int32_t)info.height - 1 };
        art_sample_rect(&full, rx, (uint32_t)stride,
                        LV_COLOR_FORMAT_RGB565, thumb,
                        ART_THUMB_SIZE, ART_THUMB_SIZE,
                        (int)info.width, (int)info.height);
        if(cover != NULL) {
            art_sample_rect(&full, rx, (uint32_t)stride,
                            LV_COLOR_FORMAT_RGB565, cover,
                            ART_COVER_SIZE, ART_COVER_SIZE,
                            (int)info.width, (int)info.height);
        }
        ok = true;
        ESP_LOGI(TAG, "art mem: hw %ux%u decoded (%u bytes)",
                 (unsigned)info.width, (unsigned)info.height,
                 (unsigned)out_size);
    }

    free(rx);
    return ok;
}

static bool art_write_file(const char * path, const uint8_t * data, size_t size)
{
    FILE * f = fopen(path, "wb");
    if(f == NULL) {
        ESP_LOGW(TAG, "art write %s: fopen errno=%d", path, errno);
        return false;
    }
    setvbuf(f, NULL, _IONBF, 0);
    size_t written = fwrite(data, 1, size, f);
    fclose(f);
    if(written != size) {
        ESP_LOGW(TAG, "art write %s: %u/%u errno=%d", path,
                 (unsigned)written, (unsigned)size, errno);
        return false;
    }
    return true;
}

static bool art_buf_ensure(void)
{
    if(s_art_buf != NULL) return true;
    const size_t thumb_size = (size_t)ART_THUMB_SIZE * ART_THUMB_SIZE * 2;
    s_art_buf = heap_caps_aligned_alloc(128, thumb_size,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(s_art_buf == NULL) s_art_buf = lv_malloc(thumb_size);
    return s_art_buf != NULL;
}

bool lockdown_client_decode_cover_mem(const uint8_t * data, size_t len,
                                      uint8_t * cover)
{
    if(data == NULL || cover == NULL || len < 3) return false;
    bool jpeg = data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF;
    if(!jpeg) return false;
    if(!art_buf_ensure()) return false;
    if(hw_jpeg_decode_mem(data, len, s_art_buf, cover)) return true;
    return tjpgd_decode_sampled_mem(data, len, s_art_buf, cover,
                                    ART_COVER_SIZE);
}

/* Both the P4 JPEG engine and our TJPGD/LVGL fallback only support baseline
 * JPEG. Inspect markers without reading the compressed pixels, so an
 * unsupported original does not trigger three full reads on every sync. */
static bool jpeg_file_is_progressive(const char * path)
{
    FILE * f = fopen(path, "rb");
    if(f == NULL) return false;
    bool progressive = false;
    if(fgetc(f) != 0xff || fgetc(f) != 0xd8) goto out;
    for(;;) {
        if(fgetc(f) != 0xff) break;
        int marker;
        do { marker = fgetc(f); } while(marker == 0xff);
        if(marker < 0 || marker == 0 || marker == 0xda || marker == 0xd9) break;
        if(marker == 0x01 || (marker >= 0xd0 && marker <= 0xd7)) continue;
        int hi = fgetc(f), lo = fgetc(f);
        if(hi < 0 || lo < 0) break;
        unsigned length = (unsigned)((hi << 8) | lo);
        if(length < 2) break;
        if(marker == 0xc0) break;
        if(marker == 0xc2) {
            uint8_t frame[6];
            progressive = length >= 8 && fread(frame, 1, sizeof(frame), f) == sizeof(frame) &&
                frame[0] == 8 && (frame[1] || frame[2]) && (frame[3] || frame[4]) &&
                (frame[5] == 1 || frame[5] == 3) && length == 8u + 3u * frame[5];
            break;
        }
        if(fseek(f, (long)length - 2, SEEK_CUR) != 0) break;
    }
out:
    fclose(f);
    return progressive;
}

static bool make_art_cache(const char * src, const char * thumb_dst,
                           const char * cover_dst)
{
    /* Decoding and file I/O here do not touch LVGL widgets, so the display
     * mutex is intentionally not held: the UI stays responsive during sync. */
    char lv_src[80];
    snprintf(lv_src, sizeof(lv_src), "A:%s", src);
    struct stat src_st;
    if(stat(src, &src_st) == 0) s_last_bytes = (uint32_t)src_st.st_size;
    const char * ext = strrchr(src, '.');
    bool is_jpeg = ext != NULL && (strcasecmp(ext, ".jpg") == 0 ||
                                   strcasecmp(ext, ".jpeg") == 0);
    bool progressive = is_jpeg && jpeg_file_is_progressive(src);

    const size_t thumb_size = (size_t)ART_THUMB_SIZE * ART_THUMB_SIZE * 2;
    const size_t cover_size = (size_t)ART_COVER_SIZE * ART_COVER_SIZE * 2;
    /* Keep the decode/sample buffers out of the small internal DMA heap: the
     * SDMMC driver needs internal DMA-capable blocks for every transfer. */
    if(!art_buf_ensure()) {
        ESP_LOGW(TAG, "art %s: cache buffer alloc failed", src);
        return false;
    }
    if(cover_dst != NULL && s_cover_buf == NULL) {
        s_cover_buf = heap_caps_aligned_alloc(128, cover_size,
                                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if(s_cover_buf == NULL) s_cover_buf = lv_malloc(cover_size);
    }
    if(s_art_buf == NULL || (cover_dst != NULL && s_cover_buf == NULL)) {
        ESP_LOGW(TAG, "art %s: cache buffer alloc failed", src);
        return false;
    }

    memset(s_art_buf, 0, thumb_size);
    if(s_cover_buf != NULL) memset(s_cover_buf, 0, cover_size);

    bool decoded = false;
    int target = cover_dst != NULL ? ART_COVER_SIZE : ART_THUMB_SIZE;
    if(progressive) {
        decoded = jpeg_thumbnail_decode(src, s_art_buf, ART_THUMB_SIZE);
        if(decoded && cover_dst != NULL)
            decoded = jpeg_thumbnail_decode(src, s_cover_buf, ART_COVER_SIZE);
        art_set_info(0, 0, "software-jpeg");
    }
    else if(is_jpeg) {
        if(cover_dst == NULL) {
            /* Thumbnail-only AFC sync: decode just enough pixels for the
             * 54x54 list thumbnail (usually 1/8 scale) instead of asking the
             * hardware decoder for a full-size RGB565 frame. */
            decoded = tjpgd_decode_sampled(src, s_art_buf, NULL, target);
            if(!decoded) {
                decoded = hw_jpeg_decode_sampled(src, s_art_buf, NULL);
            }
        }
        else {
            decoded = hw_jpeg_decode_sampled(src, s_art_buf, s_cover_buf);
            if(!decoded) {
                decoded = tjpgd_decode_sampled(src, s_art_buf, s_cover_buf,
                                               target);
            }
        }
    }
    if(!decoded && is_jpeg && !progressive) {
        /* Some valid baseline sampling layouts are unsupported by both fast
         * decoders. Use the same bounded software path before LVGL retries. */
        decoded = jpeg_thumbnail_decode(src, s_art_buf, ART_THUMB_SIZE);
        if(decoded && cover_dst != NULL)
            decoded = jpeg_thumbnail_decode(src, s_cover_buf, ART_COVER_SIZE);
        if(decoded) art_set_info(0, 0, "software-jpeg");
    }
    if(!decoded) {
        decoded = art_decode_sampled(lv_src, s_art_buf,
                                     cover_dst != NULL ? s_cover_buf : NULL);
    }
    bool ok = decoded &&
              (thumb_dst == NULL ||
               art_write_file(thumb_dst, s_art_buf, thumb_size));
    if(ok && cover_dst != NULL) {
        if(!art_write_file(cover_dst, s_cover_buf, cover_size)) {
            ESP_LOGW(TAG, "art %s: cover write failed", src);
        }
    }

    if(ok) {
        if(thumb_dst != NULL) {
            ESP_LOGI(TAG, "art %s: wrote %s%s%s", src, thumb_dst,
                     cover_dst != NULL ? " + " : "",
                     cover_dst != NULL ? cover_dst : "");
        }
        else {
            ESP_LOGI(TAG, "art %s: decoded for packed cache", src);
        }
    }
    return ok;
}

static bool existing_artwork(uint64_t id, char * out, size_t out_len)
{
    static const char * exts[] = { "jpg", "png", "wbp", "gif" };
    for(size_t i = 0; i < sizeof(exts) / sizeof(exts[0]); i++) {
        snprintf(out, out_len, "%s/%llu.%s", artwork_dir(),
                 (unsigned long long)id, exts[i]);
        FILE * f = fopen(out, "rb");
        if(f != NULL) {
            fclose(f);
            return true;
        }
    }
    out[0] = '\0';
    return false;
}

void lockdown_client_artwork_progress(lockdown_artwork_progress_t * out)
{
    if(out == NULL) return;
    out->active = s_art_active;
    out->kind = s_art_kind;
    out->phase = s_art_phase;
    out->total = s_art_total;
    out->attempted = s_art_attempted;
    out->copied = s_art_copied;
    out->failed = s_art_failed;
    out->current_id = s_art_current;
    snprintf(out->album, sizeof(out->album), "%s", s_art_album);
    snprintf(out->artist, sizeof(out->artist), "%s", s_art_artist);
}

static bool artwork_cache_ready(uint64_t id)
{
    /* AFC owns only the packed 54x54 list/artist thumbnail. The 360x360 Now
     * Playing cover comes through the iAP2 artwork channel. */
    const media_index_album_t *a=afc_album_slot(id);
    return thumb_cache_matches((uint32_t)id,a?a->artwork_version:0);
}

static const char * artwork_ext_mem(const uint8_t * data, size_t len)
{
    if(len >= 3 && data[0] == 0xFF && data[1] == 0xD8 &&
       data[2] == 0xFF) {
        return "jpg";
    }
    if(len >= 4 && data[0] == 0x89 && data[1] == 'P' &&
       data[2] == 'N' && data[3] == 'G') {
        return "png";
    }
    if(len >= 4 && data[0] == 'G' && data[1] == 'I' &&
       data[2] == 'F' && data[3] == '8') {
        return "gif";
    }
    if(len >= 12 && memcmp(data, "RIFF", 4) == 0 &&
       memcmp(data + 8, "WEBP", 4) == 0) {
        return "wbp";
    }
    return NULL;
}

static bool write_mem_file(const char * path, const uint8_t * data, size_t len)
{
    FILE * f = fopen(path, "wb");
    if(f == NULL) return false;
    setvbuf(f, NULL, _IONBF, 0);
    size_t n = fwrite(data, 1, len, f);
    fclose(f);
    return n == len;
}

static bool artwork_download_large(uint64_t id, const char * remote)
{
    char tmp[96], final[96], type[32];
    uint64_t expected = 0;
    if(afc_client_stat(remote, &expected, type, sizeof(type)) != ESP_OK || !expected)
        return false;
    snprintf(tmp, sizeof(tmp), "%s/%llu.art", artwork_dir(), (unsigned long long)id);
    /* Only one 64 KiB transfer buffer is needed; the fallback decoder reads
     * from SD instead of retaining a multi-megabyte compressed file in RAM. */
    bool ok = afc_client_copy_file(remote, tmp, expected, NULL, 0) == ESP_OK;
    struct stat st;
    ok = ok && stat(tmp, &st) == 0 && (uint64_t)st.st_size == expected;
    const char * ext = NULL;
    if(ok) {
        FILE * f = fopen(tmp, "rb");
        if(f != NULL) {
            uint8_t head[12];
            size_t n = fread(head, 1, sizeof(head), f);
            fclose(f);
            ext = artwork_ext_mem(head, n);
        }
    }
    if(ext != NULL) {
        snprintf(final, sizeof(final), "%s/%llu.%s", artwork_dir(),
                 (unsigned long long)id, ext);
        ok = rename(tmp, final) == 0;
        if(ok) thumb_cache_set_original((uint32_t)id);
    }
    else ok = false;
    if(!ok) remove(tmp);
    s_last_bytes = (uint32_t)expected;
    art_set_info(0, 0, "sd-stream");
    return ok;
}

/* One-pass path: read the original over AFC into PSRAM, decode only as much
 * as the 54x54 thumbnail needs, and append it to the packed cache. No original
 * file is written to SD and no per-album .thm file is created. */
static bool artwork_stream_one(uint64_t id)
{
    s_last_read_ms = s_last_decode_ms = s_last_write_ms = 0;
    s_last_bytes = 0;
    if(artwork_cache_ready(id)) return true;
    if(thumb_cache_original(id)) return true;

    char remote[160];
    if(!afc_artwork_remote(id, remote, sizeof(remote))) return false;

    uint8_t * data = NULL;
    size_t len = 0;
    uint64_t t0 = (uint64_t)esp_timer_get_time();
    esp_err_t err = afc_client_read_all(remote, &data, &len);
    s_last_read_ms = (int)(((uint64_t)esp_timer_get_time() - t0) / 1000);
    if(err == ESP_ERR_INVALID_SIZE) {
        ESP_LOGI(TAG, "artwork %llu: streaming large original to SD", (unsigned long long)id);
        bool ok = artwork_download_large(id, remote);
        s_last_read_ms = (int)(((uint64_t)esp_timer_get_time() - t0) / 1000);
        return ok;
    }
    if(err != ESP_OK) {
        s_last_bytes = 0;
        art_set_info(0, 0, "afc-read");
        ESP_LOGW(TAG, "artwork %llu: AFC read failed: %s",
                 (unsigned long long)id, esp_err_to_name(err));
        return false;
    }
    s_last_bytes = (uint32_t)len;

    bool ok = false;
    bool is_jpeg = len >= 3 && data[0] == 0xFF && data[1] == 0xD8 &&
                   data[2] == 0xFF;
    if(is_jpeg) {
        if(art_buf_ensure()) {
            t0 = (uint64_t)esp_timer_get_time();
            bool decoded = hw_jpeg_decode_mem(data, len, s_art_buf, NULL);
            if(!decoded) {
                decoded = tjpgd_decode_sampled_mem(data, len, s_art_buf, NULL,
                                                   ART_THUMB_SIZE);
            }
            s_last_decode_ms =
                (int)(((uint64_t)esp_timer_get_time() - t0) / 1000);
            if(decoded) {
                t0 = (uint64_t)esp_timer_get_time();
                ok = afc_write_thumb((uint32_t)id, s_art_buf) == ESP_OK;
                s_last_write_ms =
                    (int)(((uint64_t)esp_timer_get_time() - t0) / 1000);
            }
        }
    }
    else {
        art_set_info(0, 0, "non-jpeg");
    }

    if(!ok) {
        /* Non-JPEG, progressive, or an image the decoder rejected: keep the
         * original on SD and let the conversion phase retry it. */
        const char * ext = artwork_ext_mem(data, len);
        if(ext != NULL) {
            char tmp[64];
            char final[64];
            snprintf(tmp, sizeof(tmp), "%s/%llu.art", artwork_dir(),
                     (unsigned long long)id);
            t0 = (uint64_t)esp_timer_get_time();
            if(write_mem_file(tmp, data, len)) {
                snprintf(final, sizeof(final), "%s/%llu.%s", artwork_dir(),
                         (unsigned long long)id, ext);
                remove(final);
                if(rename(tmp, final) == 0) {
                    thumb_cache_set_original((uint32_t)id);
                    ok = true;
                }
                else {
                    remove(tmp);
                }
            }
            s_last_write_ms +=
                (int)(((uint64_t)esp_timer_get_time() - t0) / 1000);
        }
        if(!ok) {
            ESP_LOGW(TAG, "artwork %llu: stream decode failed (%u bytes)",
                     (unsigned long long)id, (unsigned)len);
        }
    }

    free(data);
    return ok;
}

/* Phase 3 fallback: decode an original that the one-pass path could not
 * handle and write its slot into the packed thumbnail cache. */
static bool artwork_convert_one(uint64_t id)
{
    if(artwork_cache_ready(id)) {
        if(thumb_cache_original(id)) {
            char original[64];
            if(existing_artwork(id, original, sizeof(original))) remove(original);
        }
        return true;
    }

    char original[64];
    if(!existing_artwork(id, original, sizeof(original))) return false;

    if(!make_art_cache(original, NULL, NULL)) {
        ESP_LOGW(TAG, "artwork %llu: cache failed", (unsigned long long)id);
        return false;
    }
    if(afc_write_thumb((uint32_t)id, s_art_buf) != ESP_OK) {
        ESP_LOGW(TAG, "artwork %llu: pack write failed", (unsigned long long)id);
        return false;
    }
    remove(original);
    return true;
}

esp_err_t lockdown_client_sync_artwork(void)
{
    if(!afc_client_is_ready()) {
        set_status("AFC not ready for artwork");
        s_art_active = false;
        return ESP_ERR_INVALID_STATE;
    }
    if(!app_config_sd_ensure()) {
        set_status("SD not available");
        s_art_active = false;
        return ESP_ERR_INVALID_STATE;
    }
    mkdir(artwork_dir(), 0777);
    cache_slow_log_begin(true);

    int total = 0;
    int planned = 0;

    uint64_t sd_total = 0, sd_free = 0;
    esp_vfs_fat_info("/sdcard", &sd_total, &sd_free);

    s_art_kind = 2;
    s_art_total = planned;
    s_art_current = 0;
    s_art_active = true;

    ESP_LOGI(TAG, "artwork sync start (albums=%d, planned=%d, "
             "sd free=%llu/%llu)", total, planned,
             (unsigned long long)(sd_free / (1024 * 1024)),
             (unsigned long long)(sd_total / (1024 * 1024)));

    /* Refresh and verify the database snapshot before artwork work. */
    s_art_phase = 1;
    artwork_set_current(0);
    s_art_total = 1;
    s_art_attempted = 0;
    s_art_copied = 0;
    s_art_failed = 0;
    {
        set_status("database sync");
        esp_err_t db_err = lockdown_client_sync_media();
        s_art_attempted = 1;
        s_art_copied = (db_err == ESP_OK) ? 1 : 0;
        if(db_err != ESP_OK) {
            ESP_LOGW(TAG, "database sync failed: %s", esp_err_to_name(db_err));
            s_art_failed = 1;
            s_art_active = false;
            set_status("Database copy failed. Check the connection and retry.");
            return db_err;
        }
        set_status("AFC ready");
    }

    char import_error[160],index_path[192];
    if(!media_index_import(&s_afc_index,"/sdcard/AFC/MLDB.SQL","/sdcard/AFC/MLDB.WAL",
                           app_config_library_id(),import_error,sizeof(import_error))) {
        ESP_LOGE(TAG,"AFC metadata import failed: %s",import_error);
        s_art_active=false;s_art_failed=1;set_status("%s",import_error);return ESP_FAIL;
    }
    snprintf(index_path,sizeof(index_path),"%s/afc_index.bin",app_config_library_dir());
    if(!media_index_save(&s_afc_index,index_path)) {
        s_art_active=false;s_art_failed=1;set_status("AFC metadata save failed");return ESP_FAIL;
    }
    player_library_refresh_afc();total=s_afc_index.album_count;
    ESP_LOGI(TAG,"AFC index refreshed: %u albums, %u tracks",(unsigned)s_afc_index.album_count,(unsigned)s_afc_index.track_count);
    free(s_afc_index.tracks);s_afc_index.tracks=NULL;s_afc_index.track_count=0;
    for(uint32_t i=0;i<s_afc_index.album_count;i++) {
        const media_index_album_t *a=&s_afc_index.albums[i];
        if(!a->remote[0]) continue;
        uint32_t slot=thumb_cache_slot(a->pid,true);
        if(!slot) { s_art_active=false;set_status("Thumbnail index is full");return ESP_FAIL; }
        planned++;
        if(thumb_cache_has(slot) && !thumb_cache_matches(slot,a->artwork_version)) {
            static const char *extensions[]={"jpg","jpeg","png","wbp","webp","gif"};
            for(unsigned e=0;e<sizeof(extensions)/sizeof(extensions[0]);e++) {
                char old[192];snprintf(old,sizeof(old),"%s/%u.%s",artwork_dir(),(unsigned)slot,extensions[e]);remove(old);
            }
        }
    }

    /* Read/decode/write missing covers in one pass; reuse existing slots. */
    s_art_phase = 2;
    s_art_total = planned;
    int attempted = 0, copied = 0, failed = 0;
    s_art_attempted = 0;
    s_art_copied = 0;
    s_art_failed = 0;

    /* One directory pass builds the packed-thumb bitmap and the original
     * index. Legacy .thm files are imported once and then removed. */
    set_status("indexing artwork cache");
    thumb_cache_scan(artwork_dir(), (uint32_t)total);
    set_status("AFC ready");

    uint64_t read_ms = 0, decode_ms = 0, write_ms = 0, read_bytes = 0;
    int reused = 0;
    for(uint32_t i = 0; i < s_afc_index.album_count; i++) {
        uint64_t id = thumb_cache_slot(s_afc_index.albums[i].pid,false);
        char remote[160];
        if(!afc_artwork_remote(id, remote, sizeof(remote))) continue;

        artwork_set_current(id);
        attempted++;
        s_art_attempted = attempted;
        if(artwork_cache_ready(id)) reused++;
        uint64_t t0 = (uint64_t)esp_timer_get_time();
        bool streamed = artwork_stream_one(id);
        read_ms += s_last_read_ms;
        decode_ms += s_last_decode_ms;
        write_ms += s_last_write_ms;
        read_bytes += s_last_bytes;
        uint64_t elapsed_ms =
            ((uint64_t)esp_timer_get_time() - t0) / 1000;
        if(!streamed || elapsed_ms >= SLOW_ALBUM_MS) {
            cache_slow_log_add(id, s_art_album, s_art_artist,
                               elapsed_ms, streamed);
        }
        if(elapsed_ms > 50) vTaskDelay(1);
        if(streamed) {
            copied++;
            s_art_copied = copied;
            if((copied % 10) == 0) {
                ESP_LOGI(TAG, "stream: %d ready, %d failed", copied, failed);
            }
        }
        else {
            failed++;
            s_art_failed = failed;
        }
    }
    ESP_LOGI(TAG, "download phase done: %d ready, %d failed", copied, failed);
    ESP_LOGI(TAG, "artwork IO: reused=%d read=%llums decode=%llums write=%llums bytes=%llu",
             reused, (unsigned long long)read_ms, (unsigned long long)decode_ms,
             (unsigned long long)write_ms, (unsigned long long)read_bytes);

    /* Phase 3: decode everything that landed and make the thumbnails. */
    s_art_phase = 3;
    attempted = copied = failed = 0;
    s_art_attempted = 0;
    s_art_copied = 0;
    s_art_failed = 0;
    for(uint32_t i = 0; i < s_afc_index.album_count; i++) {
        uint64_t id = thumb_cache_slot(s_afc_index.albums[i].pid,false);
        char remote[160];
        if(!afc_artwork_remote(id, remote, sizeof(remote))) continue;

        artwork_set_current(id);
        attempted++;
        s_art_attempted = attempted;
        bool had_original = thumb_cache_original((uint32_t)id);
        s_last_read_ms = 0;
        s_last_write_ms = 0;
        uint64_t t0 = (uint64_t)esp_timer_get_time();
        bool converted = artwork_convert_one(id);
        uint64_t elapsed_ms =
            ((uint64_t)esp_timer_get_time() - t0) / 1000;
        if((!converted && had_original) || elapsed_ms >= SLOW_ALBUM_MS) {
            if(!converted) s_last_decode_ms = (int)elapsed_ms;
            cache_slow_log_add(id, s_art_album, s_art_artist,
                               elapsed_ms, converted);
        }
        if(elapsed_ms > 50) vTaskDelay(1);
        if(converted) {
            copied++;
            s_art_copied = copied;
            if((copied % 10) == 0) {
                ESP_LOGI(TAG, "convert: %d ready, %d failed", copied, failed);
            }
        }
        else {
            failed++;
            s_art_failed = failed;
        }
    }

    thumb_cache_sync();
    player_library_refresh_afc();
    s_art_active = false;
    ESP_LOGI(TAG, "artwork sync done: %d cached, %d failed (attempted %d)",
             copied, failed, attempted);
    set_status("artwork %d cached, %d failed", copied, failed);
    return copied > 0 || planned == 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t lockdown_client_sync_artwork_id(int album_id)
{
    if(album_id < 1 || album_id > player_album_count()) {
        return ESP_ERR_INVALID_ARG;
    }
    if(!afc_client_is_ready()) {
        set_status("AFC not ready for artwork");
        return ESP_ERR_INVALID_STATE;
    }
    if(!app_config_sd_ensure()) {
        set_status("SD not available");
        return ESP_ERR_INVALID_STATE;
    }
    mkdir(artwork_dir(), 0777);

    if(!s_afc_index.library_id[0]) {
        char path[192];snprintf(path,sizeof(path),"%s/afc_index.bin",app_config_library_dir());
        if(!media_index_load(&s_afc_index,path,app_config_library_id())) return ESP_ERR_NOT_FOUND;
        free(s_afc_index.tracks);s_afc_index.tracks=NULL;s_afc_index.track_count=0;
    }
    player_album_t album;
    if(!player_album_get(album_id-1,&album)) return ESP_ERR_NOT_FOUND;
    uint32_t slot=thumb_cache_slot(album.album_pid,true);
    if(!slot) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "artwork sync id=%d", album_id);
    artwork_set_current(slot);
    thumb_cache_scan(artwork_dir(), (uint32_t)player_album_count());
    uint64_t t0 = (uint64_t)esp_timer_get_time();
    bool ok = artwork_stream_one(slot);
    if(ok && !artwork_cache_ready(slot)) {
        ok = artwork_convert_one(slot);
    }
    uint64_t elapsed_ms = ((uint64_t)esp_timer_get_time() - t0) / 1000;
    if(!ok || elapsed_ms >= SLOW_ALBUM_MS) {
        cache_slow_log_add((uint64_t)album_id, s_art_album, s_art_artist,
                           elapsed_ms, ok);
    }
    thumb_cache_sync();
    ESP_LOGI(TAG, "artwork sync id=%d: %s", album_id, ok ? "ok" : "failed");
    set_status("artwork id %d %s", album_id, ok ? "ok" : "failed");
    return ok ? ESP_OK : ESP_FAIL;
}

esp_err_t lockdown_client_run_afc_probe(void)
{
    if(!afc_client_is_ready()) return ESP_ERR_INVALID_STATE;
    afc_probe();
    return ESP_OK;
}

esp_err_t lockdown_client_sync_media(void)
{
    if(!afc_client_is_ready()) {
        set_status("AFC not ready");
        return ESP_ERR_INVALID_STATE;
    }
    if(!app_config_sd_ensure()) {
        set_status("SD not available");
        return ESP_ERR_INVALID_STATE;
    }

    mkdir("/sdcard/AFC", 0777);

    static const struct {
        const char * remote;
        const char * local;
    } files[] = {
        { "/iTunes_Control/iTunes/MediaLibrary.sqlitedb",
          "/sdcard/AFC/MLDB.SQL" },
        { "/iTunes_Control/iTunes/MediaLibrary.sqlitedb-wal",
          "/sdcard/AFC/MLDB.WAL" },
        { "/iTunes_Control/iTunes/MediaLibrary.sqlitedb-shm",
          "/sdcard/AFC/MLDB.SHM" },
    };

    set_status("copying media DB");
    ESP_LOGI(TAG, "copying media library to /sdcard/AFC");

    esp_err_t result = ESP_OK;
    for(size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        uint64_t size = 0;
        char type[32] = "";
        if(afc_client_stat(files[i].remote, &size, type, sizeof(type)) !=
           ESP_OK) {
            size = 0;
        }
        char afc_sha[65] = "";
        if(afc_client_copy_file(files[i].remote, files[i].local, size,
                                afc_sha, sizeof(afc_sha)) != ESP_OK) {
            result = ESP_FAIL;
            continue;
        }

        uint64_t local_size = 0;
        char local_sha[65] = "";
        if(app_config_hash_file(files[i].local, &local_size, local_sha,
                                sizeof(local_sha)) != ESP_OK) {
            ESP_LOGW(TAG, "cannot re-read %s", files[i].local);
            result = ESP_FAIL;
            continue;
        }

        bool same = local_size == size && strcmp(afc_sha, local_sha) == 0;
        ESP_LOGI(TAG, "%s: local %llu bytes sha256=%s %s", files[i].local,
                 (unsigned long long)local_size, local_sha,
                 same ? "VERIFIED" : "MISMATCH");
        if(!same) result = ESP_FAIL;
    }

    if(result == ESP_OK) {
        set_status("media DB copied to SD");
        ESP_LOGI(TAG, "media library copy complete");
    }
    else {
        set_status("media DB copy failed");
    }
    return result;
}

esp_err_t lockdown_client_verify_sd_media(void)
{
    static const char * files[] = {
        "/sdcard/AFC/MLDB.SQL",
        "/sdcard/AFC/MLDB.WAL",
        "/sdcard/AFC/MLDB.SHM",
    };

    if(!app_config_sd_ready()) {
        set_status("SD not available");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = ESP_OK;
    for(size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        uint64_t size = 0;
        char sha[65] = "";
        esp_err_t err = app_config_hash_file(files[i], &size, sha, sizeof(sha));
        if(err != ESP_OK) {
            ESP_LOGW(TAG, "verify %s: %s", files[i], esp_err_to_name(err));
            result = ESP_FAIL;
            continue;
        }
        ESP_LOGI(TAG, "%s: %llu bytes sha256=%s", files[i],
                 (unsigned long long)size, sha);
    }
    set_status(result == ESP_OK ? "SD media verified" : "SD media missing");
    return result;
}

esp_err_t lockdown_client_hash_media(void)
{
    if(!afc_client_is_ready()) {
        set_status("AFC not ready");
        return ESP_ERR_INVALID_STATE;
    }

    static const char * files[] = {
        "/iTunes_Control/iTunes/MediaLibrary.sqlitedb",
        "/iTunes_Control/iTunes/MediaLibrary.sqlitedb-wal",
        "/iTunes_Control/iTunes/MediaLibrary.sqlitedb-shm",
    };

    set_status("hashing media DB");
    ESP_LOGI(TAG, "hashing media library over AFC");

    esp_err_t result = ESP_OK;
    for(size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        uint64_t size = 0;
        char type[32] = "";
        afc_client_stat(files[i], &size, type, sizeof(type));
        if(afc_client_hash_file(files[i], size) != ESP_OK) {
            result = ESP_FAIL;
        }
    }

    set_status(result == ESP_OK ? "media DB hashed" : "media DB hash failed");
    return result;
}

esp_err_t lockdown_client_start(void)
{
    s_pair_lock = xSemaphoreCreateMutex();
    if(s_pair_lock == NULL) return ESP_ERR_NO_MEM;

    if(xTaskCreateWithCaps(lockdown_task, "lockdown", 24576, NULL, 4, NULL,
                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    /* Reserve the JPEG engine's internal DMA descriptors before the USB/AFC
     * path consumes the last internal blocks. */
    hw_jpeg_engine_ready();
    return ESP_OK;
}

const char * lockdown_client_status(void)
{
    return s_status;
}
