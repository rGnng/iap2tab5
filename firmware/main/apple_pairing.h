#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* Caller frees both strings. Only request_xml may be sent to the device;
 * record_xml includes private keys and must stay in local storage. */
typedef struct {
    char * request_xml;
    char * record_xml;
} apple_pairing_identity_t;
esp_err_t apple_pairing_generate(const uint8_t * public_key, size_t len,
                                  apple_pairing_identity_t * out);
void apple_pairing_free(apple_pairing_identity_t * identity);
