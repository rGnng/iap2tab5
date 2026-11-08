#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the lockdownd client. It waits for the usbmux handshake, connects
 * to port 62078, performs QueryType, StartSession/TLS and StartService for
 * com.apple.afc. */
esp_err_t lockdown_client_start(void);

/* Supplies a lockdownd pair record at runtime (for example from the web
 * upload). The record is also written to /spiffs/pair.plist. */
esp_err_t lockdown_client_set_pair_record(const char * data, size_t len);

typedef struct {
    bool busy;
    bool trusted;
    char message[96];
} lockdown_pair_status_t;
/* Nonblocking: the lockdown task owns the whole Apple trust exchange. */
esp_err_t lockdown_client_request_pair(void);
void lockdown_client_pair_status(lockdown_pair_status_t * out);

const char * lockdown_client_status(void);

/* Re-runs the AFC directory/stat/read probe on the current session. */
esp_err_t lockdown_client_run_afc_probe(void);

/* Copies MediaLibrary.sqlitedb (+ WAL/SHM) from AFC to /sdcard/AFC/ using
 * 8.3 names and logs SHA-256 digests. */
esp_err_t lockdown_client_sync_media(void);

/* Streams the media database (and WAL/SHM) through SHA-256 without SD. */
esp_err_t lockdown_client_hash_media(void);

/* Hashes /sdcard/AFC/MLDB.{SQL,WAL,SHM} to verify a completed copy. */
esp_err_t lockdown_client_verify_sd_media(void);

/* Refreshes the database and incrementally caches all missing artwork. */
esp_err_t lockdown_client_sync_artwork(void);

/* Sync one album's artwork by library album ID. */
esp_err_t lockdown_client_sync_artwork_id(int album_id);

typedef struct {
    bool active;
    int kind;              /* 2 = library sync */
    int phase;             /* 1 = database, 2 = streaming covers, 3 = remaining originals */
    int total;             /* albums with a manifest entry in this run */
    int attempted;
    int copied;
    int failed;
    uint64_t current_id;
    char album[96];
    char artist[96];
} lockdown_artwork_progress_t;

void lockdown_client_artwork_progress(lockdown_artwork_progress_t * out);

/* Decode a JPEG image held in memory into a 360x360 RGB565 cover. Used by the
 * iAP2 Now Playing artwork channel; returns false for unsupported formats. */
bool lockdown_client_decode_cover_mem(const uint8_t * data, size_t len,
                                      uint8_t * cover);

#ifdef __cplusplus
}
#endif
