#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Opens an AFC client on an already running usbmux TCP port. */
esp_err_t afc_client_open(uint16_t port);
void afc_client_close(void);
bool afc_client_is_ready(void);

/* Sends one AFC request and waits for its response. `data` is freed by the
 * caller with free(). Returns the AFC operation code of the response and
 * ESP_OK, or an error. */
esp_err_t afc_client_raw(uint64_t operation, const void * payload,
                         size_t payload_len, uint64_t * response_op,
                         uint8_t ** data, size_t * data_len);

/* Lists one directory as newline separated names. */
esp_err_t afc_client_list(const char * path, char * out, size_t out_len);

/* Reads directory entries without formatting them. */
esp_err_t afc_client_read_dir(const char * path, uint8_t ** data,
                              size_t * data_len);

esp_err_t afc_client_stat(const char * path, uint64_t * size,
                          char * type, size_t type_len);

/* Reads up to max_len bytes from the start of a file. Returns bytes read or
 * -1. */
int afc_client_read_file(const char * path, void * buffer, size_t max_len);

/* Reads a whole file into a 128-byte aligned PSRAM buffer. The caller frees
 * *out with free(). Files exceeding the 8 MiB budget or available contiguous
 * PSRAM return ESP_ERR_INVALID_SIZE; use
 * afc_client_copy_file() to stream those directly to SD. */
esp_err_t afc_client_read_all(const char * path, uint8_t ** out,
                              size_t * out_len);

/* Streams a remote file to a local file in 64 KB chunks and reports its
 * size and SHA-256. */
esp_err_t afc_client_copy_file(const char * remote_path,
                               const char * local_path,
                               uint64_t expected_size,
                               char * sha_out, size_t sha_out_len);

/* Streams a remote file through SHA-256 without writing it locally. */
esp_err_t afc_client_hash_file(const char * remote_path,
                               uint64_t expected_size);

const char * afc_client_status(void);

#ifdef __cplusplus
}
#endif
