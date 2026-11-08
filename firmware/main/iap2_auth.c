#include "iap2_auth.h"

#include <string.h>

#include "esp_log.h"
#include "iap2_identity.h"
#include "mbedtls/asn1.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/pk.h"

static const char * TAG = "iap2_auth";

static mbedtls_pk_context s_key;
static mbedtls_entropy_context s_entropy;
static mbedtls_ctr_drbg_context s_drbg;
static bool s_ready;

static void log_mbedtls(const char * what, int ret)
{
    char buf[128] = {0};
    mbedtls_strerror(ret, buf, sizeof(buf));
    ESP_LOGE(TAG, "%s: -0x%04x %s", what, -ret, buf);
}

esp_err_t iap2_auth_init(void)
{
    if(s_ready) return ESP_OK;

    mbedtls_pk_init(&s_key);
    mbedtls_entropy_init(&s_entropy);
    mbedtls_ctr_drbg_init(&s_drbg);

    int ret = mbedtls_ctr_drbg_seed(&s_drbg, mbedtls_entropy_func, &s_entropy,
                                    (const unsigned char *)"iap2-auth", 9);
    if(ret != 0) {
        log_mbedtls("ctr_drbg_seed", ret);
        return ESP_FAIL;
    }

    /* PKCS#8 can omit the public point. mbedTLS needs an RNG to derive it
     * from the private scalar, even though the key is not encrypted. */
    ret = mbedtls_pk_parse_key(&s_key, iap2_identity_sec1,
                               iap2_identity_sec1_len, NULL, 0,
                               mbedtls_ctr_drbg_random, &s_drbg);
    if(ret != 0) {
        log_mbedtls("pk_parse_key", ret);
        return ESP_FAIL;
    }
    if(mbedtls_pk_get_type(&s_key) != MBEDTLS_PK_ECKEY) {
        ESP_LOGE(TAG, "embedded key is not an EC key");
        return ESP_FAIL;
    }

    s_ready = true;
    ESP_LOGI(TAG, "MFi identity loaded: certificate %u bytes, key %u bytes",
             (unsigned)iap2_certificate_p7b_len,
             (unsigned)iap2_identity_sec1_len);
    return ESP_OK;
}

const uint8_t * iap2_auth_certificate(size_t * len)
{
    if(len != NULL) *len = iap2_certificate_p7b_len;
    return iap2_certificate_p7b;
}

static esp_err_t sign_der(const uint8_t * challenge, size_t challenge_len,
                          uint8_t * sig, size_t * sig_len)
{
    if(!s_ready && iap2_auth_init() != ESP_OK) return ESP_FAIL;
    if(challenge_len != 32) {
        ESP_LOGE(TAG, "challenge must be 32 bytes, got %u",
                 (unsigned)challenge_len);
        return ESP_ERR_INVALID_SIZE;
    }

    int ret = mbedtls_pk_sign(&s_key, MBEDTLS_MD_SHA256, challenge,
                              challenge_len, sig, *sig_len, sig_len,
                              mbedtls_ctr_drbg_random, &s_drbg);
    if(ret != 0) {
        log_mbedtls("pk_sign", ret);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t iap2_auth_sign(const uint8_t * challenge, size_t challenge_len,
                         uint8_t signature[64])
{
    uint8_t der[80];
    size_t der_len = sizeof(der);
    esp_err_t err = sign_der(challenge, challenge_len, der, &der_len);
    if(err != ESP_OK) return err;

    /* Convert DER SEQUENCE { INTEGER r, INTEGER s } to raw r||s. */
    unsigned char * p = der;
    const unsigned char * end = der + der_len;
    size_t seq_len = 0;
    mbedtls_mpi r;
    mbedtls_mpi s;
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);

    int ret = mbedtls_asn1_get_tag(&p, end, &seq_len,
                                   MBEDTLS_ASN1_CONSTRUCTED |
                                       MBEDTLS_ASN1_SEQUENCE);
    if(ret == 0) {
        ret = mbedtls_asn1_get_mpi(&p, end, &r);
    }
    if(ret == 0) {
        ret = mbedtls_asn1_get_mpi(&p, end, &s);
    }
    if(ret == 0) {
        ret = mbedtls_mpi_write_binary(&r, signature, 32);
    }
    if(ret == 0) {
        ret = mbedtls_mpi_write_binary(&s, signature + 32, 32);
    }

    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&s);

    if(ret != 0) {
        log_mbedtls("ecdsa der->raw", ret);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t iap2_auth_self_test(void)
{
    if(iap2_auth_init() != ESP_OK) return ESP_FAIL;

    static const uint8_t challenge[32] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
        0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87,
        0x98, 0xa9, 0xba, 0xcb, 0xdc, 0xed, 0xfe, 0x0f,
    };

    uint8_t der[80];
    size_t der_len = sizeof(der);
    esp_err_t err = sign_der(challenge, sizeof(challenge), der, &der_len);
    if(err != ESP_OK) return err;

    int ret = mbedtls_pk_verify(&s_key, MBEDTLS_MD_SHA256, challenge,
                                sizeof(challenge), der, der_len);
    if(ret != 0) {
        log_mbedtls("pk_verify", ret);
        return ESP_FAIL;
    }

    uint8_t raw[64];
    err = iap2_auth_sign(challenge, sizeof(challenge), raw);
    if(err != ESP_OK) return err;

    char hex[129];
    for(int i = 0; i < 64; i++) {
        snprintf(hex + i * 2, 3, "%02x", raw[i]);
    }
    hex[128] = '\0';
    ESP_LOGI(TAG, "MFi self-test OK, challenge signature (r||s): %s", hex);
    return ESP_OK;
}
