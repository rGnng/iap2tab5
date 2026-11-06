#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Loads the embedded Apple Accessories P-256 identity. */
esp_err_t iap2_auth_init(void);

const uint8_t * iap2_auth_certificate(size_t * len);

/* Signs the 32-byte iAP2 challenge exactly as the MFi 3.0 path expects:
 * ECDSA P-256 over SHA-256(challenge) with raw r||s output. */
esp_err_t iap2_auth_sign(const uint8_t * challenge, size_t challenge_len,
                         uint8_t signature[64]);

/* Runs a sign+verify self-test against the embedded key. */
esp_err_t iap2_auth_self_test(void);

#ifdef __cplusplus
}
#endif
