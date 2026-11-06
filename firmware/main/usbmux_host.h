#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct usbmux_conn usbmux_conn_t;

/* Registers a USB host client that looks for the iPod, claims the Apple
 * usbmux interface and performs the usbmux v2 version/setup handshake.
 * The TCP stream layer below is used by the lockdownd/AFC client. */
esp_err_t usbmux_host_start(void);

bool usbmux_host_is_ready(void);

/* Blocks until the mux handshake has completed and the device is still
 * present. Returns false on timeout. */
bool usbmux_host_wait_ready(uint32_t timeout_ms);

/* Opens a TCP-like stream to the given port on the device (for example
 * 62078 for lockdownd). The returned connection must be closed with
 * usbmux_conn_close(). */
esp_err_t usbmux_connect(uint16_t port, usbmux_conn_t ** out,
                         uint32_t timeout_ms);

/* Returns the number of bytes sent, or -1 on error. */
int usbmux_conn_send(usbmux_conn_t * conn, const void * data, size_t len);

/* Returns >0 bytes received, 0 on timeout, -1 if the stream was closed. */
int usbmux_conn_recv(usbmux_conn_t * conn, void * data, size_t len,
                     uint32_t timeout_ms);

/* Reads exactly len bytes. ESP_OK, ESP_ERR_TIMEOUT or ESP_FAIL if closed. */
esp_err_t usbmux_conn_recv_exact(usbmux_conn_t * conn, void * data, size_t len,
                                 uint32_t timeout_ms);

bool usbmux_conn_is_open(const usbmux_conn_t * conn);

void usbmux_conn_close(usbmux_conn_t * conn);

const char * usbmux_host_status(void);

/* Logs every USB configuration descriptor of the connected iPod. Useful for
 * bringing up the iAP2 configuration (config 2). */
esp_err_t usbmux_host_dump_configs(void);

#ifdef __cplusplus
}
#endif
