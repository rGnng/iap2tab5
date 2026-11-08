#include "afc_client.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "mbedtls/sha256.h"
#include "usbmux_host.h"

static const char * TAG = "afc";

#define AFC_MAGIC       "CFA6LPAA"
#define AFC_MAGIC_LEN   8
#define AFC_HDR_LEN     40
#define AFC_RECV_TIMEOUT 15000
#define AFC_MEMORY_FILE_LIMIT (8 * 1024 * 1024)

#define AFC_OP_STATUS        0x01
#define AFC_OP_DATA          0x02
#define AFC_OP_READ_DIR      0x03
#define AFC_OP_GET_FILE_INFO 0x0A
#define AFC_OP_GET_DEVINFO   0x0B
#define AFC_OP_FILE_OPEN     0x0D
#define AFC_OP_FILE_OPEN_RES 0x0E
#define AFC_OP_FILE_READ     0x0F
#define AFC_OP_FILE_CLOSE    0x14

#define AFC_FOPEN_RDONLY     0x00000001

static usbmux_conn_t * s_conn;
static uint64_t s_packet_num;
static char s_status[96] = "idle";

static void set_status(const char * fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_status, sizeof(s_status), fmt, ap);
    va_end(ap);
}

static uint64_t rd_le64(const uint8_t * p)
{
    uint64_t v = 0;
    for(int i = 7; i >= 0; i--) {
        v = (v << 8) | p[i];
    }
    return v;
}

static void wr_le64(uint8_t * p, uint64_t v)
{
    for(int i = 0; i < 8; i++) {
        p[i] = (uint8_t)(v >> (8 * i));
    }
}

static esp_err_t recv_exact(uint8_t * buf, size_t len, uint32_t timeout_ms)
{
    return usbmux_conn_recv_exact(s_conn, buf, len, timeout_ms);
}

esp_err_t afc_client_raw(uint64_t operation, const void * payload,
                         size_t payload_len, uint64_t * response_op,
                         uint8_t ** data, size_t * data_len)
{
    if(s_conn == NULL || !usbmux_conn_is_open(s_conn)) {
        return ESP_ERR_INVALID_STATE;
    }
    if(response_op != NULL) *response_op = 0;
    if(data != NULL) *data = NULL;
    if(data_len != NULL) *data_len = 0;

    uint64_t packet_num = s_packet_num++;

    /* Path and file-read requests are small: send header and arguments in
     * one mux packet instead of paying for two USB transfers per request. */
    uint8_t header[AFC_HDR_LEN + 512];
    memset(header, 0, AFC_HDR_LEN);
    memcpy(header, AFC_MAGIC, AFC_MAGIC_LEN);
    wr_le64(header + 8, AFC_HDR_LEN + payload_len);
    wr_le64(header + 16, AFC_HDR_LEN + payload_len);
    wr_le64(header + 24, packet_num);
    wr_le64(header + 32, operation);

    size_t first_len = AFC_HDR_LEN;
    if(payload_len > 0 && payload_len <= sizeof(header) - AFC_HDR_LEN) {
        memcpy(header + AFC_HDR_LEN, payload, payload_len);
        first_len += payload_len;
    }
    if(usbmux_conn_send(s_conn, header, first_len) != (int)first_len) {
        set_status("AFC send header failed");
        return ESP_FAIL;
    }
    if(payload_len > sizeof(header) - AFC_HDR_LEN &&
       usbmux_conn_send(s_conn, payload, payload_len) != (int)payload_len) {
        set_status("AFC send payload failed");
        return ESP_FAIL;
    }

    uint8_t rh[AFC_HDR_LEN];
    esp_err_t err = recv_exact(rh, sizeof(rh), AFC_RECV_TIMEOUT);
    if(err != ESP_OK) {
        set_status("AFC response timeout");
        return err;
    }
    if(memcmp(rh, AFC_MAGIC, AFC_MAGIC_LEN) != 0) {
        ESP_LOGW(TAG, "bad AFC magic");
        set_status("bad AFC magic");
        return ESP_ERR_INVALID_RESPONSE;
    }

    uint64_t entire = rd_le64(rh + 8);
    uint64_t this_len = rd_le64(rh + 16);
    uint64_t packet = rd_le64(rh + 24);
    uint64_t op = rd_le64(rh + 32);

    if(entire < AFC_HDR_LEN || this_len < AFC_HDR_LEN ||
       this_len > entire || entire > 16 * 1024 * 1024) {
        ESP_LOGW(TAG, "bad AFC lengths entire=%llu this=%llu",
                 (unsigned long long)entire, (unsigned long long)this_len);
        return ESP_ERR_INVALID_SIZE;
    }
    if(packet != packet_num) {
        ESP_LOGW(TAG, "AFC packet num %llu expected %llu",
                 (unsigned long long)packet, (unsigned long long)packet_num);
    }

    size_t body = (size_t)(entire - AFC_HDR_LEN);
    size_t first = (size_t)(this_len - AFC_HDR_LEN);
    uint8_t * buf = NULL;

    if(body > 0) {
        if(body > 4096) {
            /* The SDMMC driver only takes the multi-block DMA path when a
             * PSRAM buffer is aligned to the L2 cache line (128 bytes on
             * ESP32-P4). A 4-byte aligned buffer silently falls back to
             * single 512-byte writes and caps SD throughput at ~1 MB/s. */
            buf = heap_caps_aligned_alloc(128, body + 1,
                                          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
        else {
            buf = malloc(body + 1);
        }
        if(buf == NULL) {
            ESP_LOGE(TAG, "no memory for AFC body (%u bytes)",
                     (unsigned)body);
            set_status("AFC out of memory");
            return ESP_ERR_NO_MEM;
        }
        if(first > 0 && recv_exact(buf, first, AFC_RECV_TIMEOUT) != ESP_OK) {
            free(buf);
            set_status("AFC response body timeout");
            return ESP_ERR_TIMEOUT;
        }
        size_t got = first;
        while(got < body) {
            int n = usbmux_conn_recv(s_conn, buf + got, body - got,
                                     AFC_RECV_TIMEOUT);
            if(n <= 0) {
                free(buf);
                set_status("AFC response body incomplete");
                return ESP_ERR_TIMEOUT;
            }
            got += (size_t)n;
        }
        buf[body] = '\0';
    }

    if(response_op != NULL) *response_op = op;
    if(data != NULL) *data = buf;
    else free(buf);
    if(data_len != NULL) *data_len = body;
    return ESP_OK;
}

static int dir_iterate(uint8_t * data, size_t len, char * out, size_t out_len)
{
    size_t used = 0;
    size_t pos = 0;
    int count = 0;

    while(pos < len) {
        const char * name = (const char *)data + pos;
        size_t n = strnlen(name, len - pos);
        if(n == 0) break;
        if(used + n + 2 < out_len) {
            memcpy(out + used, name, n);
            used += n;
            out[used++] = '\n';
            out[used] = '\0';
        }
        count++;
        pos += n + 1;
    }
    return count;
}

esp_err_t afc_client_read_dir(const char * path, uint8_t ** data,
                              size_t * data_len)
{
    size_t path_len = strlen(path) + 1;
    uint8_t * payload = malloc(path_len);
    if(payload == NULL) return ESP_ERR_NO_MEM;
    memcpy(payload, path, path_len);

    uint64_t op = 0;
    esp_err_t err = afc_client_raw(AFC_OP_READ_DIR, payload, path_len, &op,
                                   data, data_len);
    free(payload);
    if(err != ESP_OK) return err;

    if(op == AFC_OP_STATUS) {
        uint64_t code = (*data_len >= 8) ? rd_le64(*data) : (uint64_t)-1;
        ESP_LOGW(TAG, "READ_DIR '%s' status %llu", path,
                 (unsigned long long)code);
        free(*data);
        *data = NULL;
        *data_len = 0;
        return ESP_FAIL;
    }
    if(op != AFC_OP_DATA) {
        ESP_LOGW(TAG, "READ_DIR '%s' unexpected op 0x%llx", path,
                 (unsigned long long)op);
        free(*data);
        *data = NULL;
        *data_len = 0;
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

esp_err_t afc_client_list(const char * path, char * out, size_t out_len)
{
    uint8_t * data = NULL;
    size_t len = 0;
    if(out_len > 0) out[0] = '\0';

    esp_err_t err = afc_client_read_dir(path, &data, &len);
    if(err != ESP_OK) return err;

    int count = dir_iterate(data, len, out, out_len);
    free(data);
    ESP_LOGI(TAG, "%s: %d entries", path, count);
    return ESP_OK;
}

esp_err_t afc_client_stat(const char * path, uint64_t * size,
                          char * type, size_t type_len)
{
    size_t path_len = strlen(path) + 1;
    uint8_t * payload = malloc(path_len);
    if(payload == NULL) return ESP_ERR_NO_MEM;
    memcpy(payload, path, path_len);

    uint64_t op = 0;
    uint8_t * data = NULL;
    size_t len = 0;
    esp_err_t err = afc_client_raw(AFC_OP_GET_FILE_INFO, payload, path_len,
                                   &op, &data, &len);
    free(payload);
    if(err != ESP_OK) return err;

    if(op != AFC_OP_DATA) {
        ESP_LOGW(TAG, "GET_FILE_INFO '%s' op 0x%llx", path,
                 (unsigned long long)op);
        free(data);
        return ESP_FAIL;
    }

    if(size != NULL) *size = 0;
    if(type != NULL && type_len > 0) type[0] = '\0';

    size_t pos = 0;
    while(pos < len) {
        const char * key = (const char *)data + pos;
        size_t klen = strnlen(key, len - pos);
        if(klen == 0) break;
        pos += klen + 1;
        if(pos >= len) break;
        const char * value = (const char *)data + pos;
        size_t vlen = strnlen(value, len - pos);
        pos += vlen + 1;

        if(size != NULL && strcmp(key, "st_size") == 0) {
            *size = strtoull(value, NULL, 10);
        }
        else if(type != NULL && strcmp(key, "st_ifmt") == 0) {
            snprintf(type, type_len, "%s", value);
        }
    }

    free(data);
    return ESP_OK;
}

int afc_client_read_file(const char * path, void * buffer, size_t max_len)
{
    size_t path_len = strlen(path) + 1;
    uint8_t * payload = malloc(path_len + 8);
    if(payload == NULL) return -1;
    wr_le64(payload, AFC_FOPEN_RDONLY);
    memcpy(payload + 8, path, path_len);

    uint64_t op = 0;
    uint8_t * data = NULL;
    size_t len = 0;
    esp_err_t err = afc_client_raw(AFC_OP_FILE_OPEN, payload, path_len + 8,
                                   &op, &data, &len);
    free(payload);
    if(err != ESP_OK) return -1;
    if(op != AFC_OP_FILE_OPEN_RES || len < 8) {
        uint64_t code = (len >= 8) ? rd_le64(data) : (uint64_t)-1;
        ESP_LOGW(TAG, "open '%s' failed op=0x%llx code=%llu", path,
                 (unsigned long long)op, (unsigned long long)code);
        free(data);
        return -1;
    }

    uint64_t handle = rd_le64(data);
    free(data);
    ESP_LOGI(TAG, "open '%s' handle=%llu", path, (unsigned long long)handle);

    uint8_t req[16];
    wr_le64(req, handle);
    wr_le64(req + 8, max_len);
    op = 0;
    data = NULL;
    len = 0;
    err = afc_client_raw(AFC_OP_FILE_READ, req, sizeof(req), &op, &data, &len);
    int result = -1;

    if(err == ESP_OK && op == AFC_OP_DATA && len > 0) {
        size_t n = len;
        if(n > max_len) n = max_len;
        memcpy(buffer, data, n);
        result = (int)n;
    }
    else {
        ESP_LOGW(TAG, "read '%s' failed op=0x%llx len=%u", path,
                 (unsigned long long)op, (unsigned)len);
    }
    free(data);

    wr_le64(req, handle);
    op = 0;
    data = NULL;
    len = 0;
    afc_client_raw(AFC_OP_FILE_CLOSE, req, 8, &op, &data, &len);
    free(data);

    return result;
}

esp_err_t afc_client_read_all(const char * remote_path, uint8_t ** out,
                              size_t * out_len)
{
    if(remote_path == NULL || out == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = NULL;
    *out_len = 0;

    uint64_t expected = 0;
    char type[32];
    if(afc_client_stat(remote_path, &expected, type, sizeof(type)) != ESP_OK) {
        expected = 0;
    }
    /* Leave space for display buffers, USB and the JPEG decoder. Callers
     * can use copy_file() for originals larger than the in-memory budget. */
    if(expected > AFC_MEMORY_FILE_LIMIT) return ESP_ERR_INVALID_SIZE;

    size_t cap = expected > 0 ? (size_t)expected + 1 : 256 * 1024;
    if(cap < 64 * 1024) cap = 64 * 1024;
    uint8_t * buf = heap_caps_aligned_alloc(128, cap,
                                            MALLOC_CAP_SPIRAM |
                                            MALLOC_CAP_8BIT);
    if(buf == NULL) return ESP_ERR_INVALID_SIZE;

    size_t path_len = strlen(remote_path) + 1;
    uint8_t * payload = malloc(path_len + 8);
    if(payload == NULL) {
        free(buf);
        return ESP_ERR_NO_MEM;
    }
    wr_le64(payload, AFC_FOPEN_RDONLY);
    memcpy(payload + 8, remote_path, path_len);

    uint64_t op = 0;
    uint8_t * data = NULL;
    size_t len = 0;
    esp_err_t result = afc_client_raw(AFC_OP_FILE_OPEN, payload, path_len + 8,
                                      &op, &data, &len);
    free(payload);
    uint64_t handle = 0;
    bool opened = false;

    if(result != ESP_OK || op != AFC_OP_FILE_OPEN_RES || len < 8) {
        ESP_LOGW(TAG, "read_all open '%s' failed op=0x%llx len=%u",
                 remote_path, (unsigned long long)op, (unsigned)len);
        free(data);
        free(buf);
        return ESP_FAIL;
    }
    handle = rd_le64(data);
    free(data);
    opened = true;

    size_t used = 0;
    for(;;) {
        uint8_t req[16];
        wr_le64(req, handle);
        wr_le64(req + 8, 64 * 1024);

        op = 0;
        data = NULL;
        len = 0;
        result = afc_client_raw(AFC_OP_FILE_READ, req, sizeof(req), &op,
                                &data, &len);
        if(result != ESP_OK) {
            free(data);
            /* Reserve INVALID_SIZE for the caller's SD-streaming fallback;
             * a malformed wire response is not an oversized file. */
            if(result == ESP_ERR_INVALID_SIZE) result = ESP_ERR_INVALID_RESPONSE;
            goto cleanup;
        }
        if(op != AFC_OP_DATA) {
            ESP_LOGW(TAG, "read_all '%s' status op=0x%llx len=%u",
                     remote_path, (unsigned long long)op, (unsigned)len);
            free(data);
            result = ESP_FAIL;
            goto cleanup;
        }
        if(len == 0) {
            free(data);
            break;
        }
        if(len > AFC_MEMORY_FILE_LIMIT - used) {
            free(data);
            result = ESP_ERR_INVALID_SIZE;
            goto cleanup;
        }
        if(used + len > cap) {
            size_t new_cap = cap * 2;
            while(new_cap < used + len) new_cap *= 2;
            uint8_t * nb = heap_caps_aligned_alloc(128, new_cap,
                                                   MALLOC_CAP_SPIRAM |
                                                   MALLOC_CAP_8BIT);
            if(nb == NULL) {
                free(data);
                result = ESP_ERR_INVALID_SIZE;
                goto cleanup;
            }
            memcpy(nb, buf, used);
            heap_caps_free(buf);
            buf = nb;
            cap = new_cap;
        }
        memcpy(buf + used, data, len);
        used += len;
        free(data);
    }

    *out = buf;
    *out_len = used;
    buf = NULL;
    result = ESP_OK;

cleanup:
    if(opened) {
        uint8_t creq[8];
        wr_le64(creq, handle);
        op = 0;
        data = NULL;
        len = 0;
        afc_client_raw(AFC_OP_FILE_CLOSE, creq, 8, &op, &data, &len);
        free(data);
    }
    free(buf);
    if(result != ESP_OK) {
        *out = NULL;
        *out_len = 0;
    }
    return result;
}

esp_err_t afc_client_copy_file(const char * remote_path,
                               const char * local_path,
                               uint64_t expected_size,
                               char * sha_out, size_t sha_out_len)
{
    const size_t chunk = 64 * 1024;

    size_t path_len = strlen(remote_path) + 1;
    uint8_t * payload = malloc(path_len + 8);
    if(payload == NULL) return ESP_ERR_NO_MEM;
    wr_le64(payload, AFC_FOPEN_RDONLY);
    memcpy(payload + 8, remote_path, path_len);

    uint64_t op = 0;
    uint8_t * data = NULL;
    size_t len = 0;
    esp_err_t err = afc_client_raw(AFC_OP_FILE_OPEN, payload, path_len + 8,
                                   &op, &data, &len);
    free(payload);
    if(err != ESP_OK || op != AFC_OP_FILE_OPEN_RES || len < 8) {
        uint64_t code = (len >= 8) ? rd_le64(data) : (uint64_t)-1;
        ESP_LOGW(TAG, "copy open '%s' failed op=0x%llx code=%llu",
                 remote_path, (unsigned long long)op,
                 (unsigned long long)code);
        free(data);
        return ESP_FAIL;
    }
    uint64_t handle = rd_le64(data);
    free(data);

    FILE * out = NULL;
    if(local_path != NULL) {
        out = fopen(local_path, "wb");
        if(out != NULL) {
            /* Pass the AFC buffer straight to FatFs/SDMMC instead of letting
             * stdio copy it through an unaligned FILE buffer. */
            setvbuf(out, NULL, _IONBF, 0);
        }
        if(out == NULL) {
            ESP_LOGE(TAG, "cannot create %s", local_path);
            uint8_t req[8];
            wr_le64(req, handle);
            afc_client_raw(AFC_OP_FILE_CLOSE, req, 8, &op, &data, &len);
            free(data);
            return ESP_FAIL;
        }
    }

    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);

    uint64_t total = 0;
    uint64_t next_log = 4 * 1024 * 1024;
    uint64_t afc_us = 0;
    uint64_t write_us = 0;
    uint64_t close_us = 0;
    uint64_t next_progress_us = 500000;
    esp_err_t result = ESP_OK;

    for(;;) {
        uint8_t req[16];
        wr_le64(req, handle);
        wr_le64(req + 8, chunk);

        op = 0;
        data = NULL;
        len = 0;
        uint64_t t0 = (uint64_t)esp_timer_get_time();
        err = afc_client_raw(AFC_OP_FILE_READ, req, sizeof(req), &op,
                             &data, &len);
        afc_us += (uint64_t)esp_timer_get_time() - t0;
        if((afc_us + write_us) >= next_progress_us) {
            next_progress_us += 500000;
            ESP_LOGI(TAG, "progress %s: %llu bytes afc=%llums sd=%llums",
                     remote_path, (unsigned long long)total,
                     (unsigned long long)(afc_us / 1000),
                     (unsigned long long)(write_us / 1000));
        }
        if(err != ESP_OK) {
            result = err;
            break;
        }
        if(op != AFC_OP_DATA) {
            uint64_t code = (len >= 8) ? rd_le64(data) : (uint64_t)-1;
            ESP_LOGW(TAG, "copy read '%s' status op=0x%llx code=%llu",
                     remote_path, (unsigned long long)op,
                     (unsigned long long)code);
            free(data);
            result = ESP_FAIL;
            break;
        }
        if(len == 0) {
            free(data);
            break;
        }

        if(out != NULL) {
            t0 = (uint64_t)esp_timer_get_time();
            size_t written = fwrite(data, 1, len, out);
            write_us += (uint64_t)esp_timer_get_time() - t0;
            if(written != len) {
                ESP_LOGE(TAG, "write %s failed (%u/%u)", local_path,
                         (unsigned)written, (unsigned)len);
                free(data);
                result = ESP_FAIL;
                break;
            }
        }
        mbedtls_sha256_update(&sha, data, len);
        total += len;
        free(data);

        if(total >= next_log) {
            ESP_LOGI(TAG, "%s %s: %llu/%llu bytes",
                     out ? "copy" : "hash", remote_path,
                     (unsigned long long)total,
                     (unsigned long long)expected_size);
            next_log += 4 * 1024 * 1024;
        }
    }

    if(out != NULL) {
        uint64_t t0 = (uint64_t)esp_timer_get_time();
        if(fclose(out) != 0) result = ESP_FAIL;
        close_us = (uint64_t)esp_timer_get_time() - t0;
    }

    {
        uint8_t req[8];
        wr_le64(req, handle);
        op = 0;
        data = NULL;
        len = 0;
        afc_client_raw(AFC_OP_FILE_CLOSE, req, 8, &op, &data, &len);
        free(data);
    }

    unsigned char digest[32];
    mbedtls_sha256_finish(&sha, digest);
    mbedtls_sha256_free(&sha);

    char hex[65];
    for(int i = 0; i < 32; i++) {
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    }
    hex[64] = '\0';
    if(sha_out != NULL && sha_out_len > 0) {
        snprintf(sha_out, sha_out_len, "%s", hex);
    }

    if(result == ESP_OK && expected_size != 0 && total != expected_size) {
        ESP_LOGW(TAG, "copy %s: size mismatch (%llu != %llu)", remote_path,
                 (unsigned long long)total,
                 (unsigned long long)expected_size);
    }
    if(result == ESP_OK && out != NULL) {
        uint64_t total_us = afc_us + write_us + close_us;
        ESP_LOGI(TAG, "copied %s -> %s: %llu bytes sha256=%s",
                 remote_path, local_path, (unsigned long long)total, hex);
        ESP_LOGI(TAG, "io %llu bytes: afc=%llums sd=%llums close=%llums "
                 "rate=%lluKB/s", (unsigned long long)total,
                 (unsigned long long)(afc_us / 1000),
                 (unsigned long long)(write_us / 1000),
                 (unsigned long long)(close_us / 1000),
                 (unsigned long long)(total * 1000 /
                                      (total_us ? total_us : 1)));
    }
    else if(result == ESP_OK) {
        ESP_LOGI(TAG, "hash %s: %llu bytes sha256=%s", remote_path,
                 (unsigned long long)total, hex);
    }
    else {
        ESP_LOGW(TAG, "copy %s failed after %llu bytes", remote_path,
                 (unsigned long long)total);
    }
    set_status("AFC %s %s", local_path ? "copy" : "hash",
               result == ESP_OK ? "done" : "failed");
    return result;
}

esp_err_t afc_client_hash_file(const char * remote_path,
                               uint64_t expected_size)
{
    return afc_client_copy_file(remote_path, NULL, expected_size, NULL, 0);
}

esp_err_t afc_client_open(uint16_t port)
{
    if(s_conn != NULL) afc_client_close();

    usbmux_conn_t * conn = NULL;
    esp_err_t err = usbmux_connect(port, &conn, 5000);
    if(err != ESP_OK) {
        set_status("AFC connect failed");
        return err;
    }

    s_conn = conn;
    s_packet_num = 0;

    uint64_t op = 0;
    uint8_t * data = NULL;
    size_t len = 0;
    err = afc_client_raw(AFC_OP_GET_DEVINFO, NULL, 0, &op, &data, &len);
    if(err == ESP_OK) {
        ESP_LOGI(TAG, "AFC ready on port %u (devinfo op=0x%llx len=%u)",
                 port, (unsigned long long)op, (unsigned)len);
        set_status("AFC ready on port %u", port);
    }
    free(data);
    return err;
}

void afc_client_close(void)
{
    if(s_conn != NULL) {
        usbmux_conn_close(s_conn);
        s_conn = NULL;
    }
    set_status("idle");
}

bool afc_client_is_ready(void)
{
    return s_conn != NULL && usbmux_conn_is_open(s_conn);
}

const char * afc_client_status(void)
{
    return s_status;
}
