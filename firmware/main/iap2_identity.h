#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Apple Accessories P-256 identity from ../iap2cert. The certificate is a
 * PKCS#7 SignedData blob, exactly as expected in iAP2 message 0xAA01. The
 * private key is converted to SEC1 DER for mbedtls_pk_parse_key(). */
extern const uint8_t iap2_certificate_p7b[];
extern const size_t iap2_certificate_p7b_len;
extern const uint8_t iap2_identity_sec1[];
extern const size_t iap2_identity_sec1_len;

#ifdef __cplusplus
}
#endif
