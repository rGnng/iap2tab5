#include "apple_pairing.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/oid.h"
#include "mbedtls/pk.h"
#include "mbedtls/rsa.h"
#include "mbedtls/x509_crt.h"

#define RECORD_CAP 24576
#define PEM_CAP 4096
static const char PREFIX[] = "<?xml version=\"1.0\"?><plist version=\"1.0\"><dict>";
static const char SUFFIX[] = "</dict></plist>";

static int pairing_rng(void * ctx, unsigned char * out, size_t len)
{
    /* RSA prime generation must not starve the idle task/watchdog. */
    vTaskDelay(1);
    return mbedtls_ctr_drbg_random(ctx, out, len);
}

static bool append(char * xml, const char * text)
{
    size_t used = strlen(xml), len = strlen(text);
    if(used + len >= RECORD_CAP) return false;
    memcpy(xml + used, text, len + 1);
    return true;
}

static bool append_pem(char * xml, const char * name, const char * pem)
{
    char key[96];
    snprintf(key, sizeof(key), "<key>%s</key><data>", name);
    if(!append(xml, key)) return false;
    size_t used = strlen(xml), written = 0;
    if(mbedtls_base64_encode((unsigned char *)xml + used, RECORD_CAP - used,
        &written, (const unsigned char *)pem, strlen(pem)) != 0) return false;
    xml[used + written] = 0;
    return append(xml, "</data>");
}

static int certificate(mbedtls_pk_context * subject, mbedtls_pk_context * root,
                       bool ca, mbedtls_ctr_drbg_context * rng, char * pem)
{
    mbedtls_x509write_cert crt;
    mbedtls_x509write_crt_init(&crt);
    const unsigned char serial = 1;
    int rc = mbedtls_x509write_crt_set_serial_raw(&crt, &serial, 1);
    mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&crt, subject);
    mbedtls_x509write_crt_set_issuer_key(&crt, root);
    /* Apple pairing uses empty subject/issuer names and critical constraints. */
    static const unsigned char ca_ext[] = {0x30, 3, 1, 1, 0xff};
    static const unsigned char leaf_ext[] = {0x30, 0};
    if(rc == 0) rc = mbedtls_x509write_crt_set_extension(&crt,
        MBEDTLS_OID_BASIC_CONSTRAINTS, MBEDTLS_OID_SIZE(MBEDTLS_OID_BASIC_CONSTRAINTS),
        1, ca ? ca_ext : leaf_ext, ca ? sizeof(ca_ext) : sizeof(leaf_ext));
    if(rc == 0) rc = mbedtls_x509write_crt_set_subject_key_identifier(&crt);
    if(rc == 0 && !ca) rc = mbedtls_x509write_crt_set_key_usage(&crt,
        MBEDTLS_X509_KU_DIGITAL_SIGNATURE | MBEDTLS_X509_KU_KEY_ENCIPHERMENT);
    if(rc == 0) rc = mbedtls_x509write_crt_set_validity(&crt,
        "20200101000000", "20491231235959");
    if(rc == 0) rc = mbedtls_x509write_crt_pem(&crt, (unsigned char *)pem,
                                              PEM_CAP, pairing_rng, rng);
    mbedtls_x509write_crt_free(&crt);
    return rc;
}

static int uuid(mbedtls_ctr_drbg_context * rng, char out[37])
{
    unsigned char b[16];
    int rc = mbedtls_ctr_drbg_random(rng, b, sizeof(b));
    if(rc != 0) return rc;
    b[6] = (b[6] & 15) | 0x40;
    b[8] = (b[8] & 63) | 0x80;
    snprintf(out, 37, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X",
        b[0],b[1],b[2],b[3],b[4],b[5],b[6],b[7],b[8],b[9],b[10],b[11],b[12],b[13],b[14],b[15]);
    return 0;
}

void apple_pairing_free(apple_pairing_identity_t * identity)
{
    if(identity == NULL) return;
    if(identity->record_xml) memset(identity->record_xml, 0, strlen(identity->record_xml));
    free(identity->record_xml);
    free(identity->request_xml);
    memset(identity, 0, sizeof(*identity));
}

esp_err_t apple_pairing_generate(const uint8_t * public_key, size_t len,
                                  apple_pairing_identity_t * out)
{
    if(!out || !public_key || !len) return ESP_ERR_INVALID_ARG;
    memset(out, 0, sizeof(*out));
    esp_err_t result = ESP_FAIL;
    mbedtls_pk_context root, host, device;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context rng;
    mbedtls_pk_init(&root); mbedtls_pk_init(&host); mbedtls_pk_init(&device);
    mbedtls_entropy_init(&entropy); mbedtls_ctr_drbg_init(&rng);
    char * pem = calloc(1, PEM_CAP);
    unsigned char * key = calloc(1, len + 1);
    out->record_xml = calloc(1, RECORD_CAP);
    out->request_xml = calloc(1, RECORD_CAP);
    if(!pem || !key || !out->record_xml || !out->request_xml) {
        result = ESP_ERR_NO_MEM;
        goto done;
    }
    memcpy(key, public_key, len);
    if(mbedtls_pk_parse_public_key(&device, key, len + 1) != 0 ||
       !mbedtls_pk_can_do(&device, MBEDTLS_PK_RSA)) goto done;
    if(mbedtls_ctr_drbg_seed(&rng, mbedtls_entropy_func, &entropy,
        (const unsigned char *)"tab5-apple-pair", 15) != 0) goto done;
    if(mbedtls_pk_setup(&root, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)) != 0 ||
       mbedtls_pk_setup(&host, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)) != 0) goto done;
    if(mbedtls_rsa_gen_key(mbedtls_pk_rsa(root), pairing_rng, &rng, 2048, 65537) != 0 ||
       mbedtls_rsa_gen_key(mbedtls_pk_rsa(host), pairing_rng, &rng, 2048, 65537) != 0) goto done;
    char host_id[37], buid[37], ids[256];
    if(uuid(&rng, host_id) != 0 || uuid(&rng, buid) != 0) goto done;
    snprintf(ids, sizeof(ids), "<key>HostID</key><string>%s</string>"
        "<key>SystemBUID</key><string>%s</string>", host_id, buid);
    append(out->record_xml, PREFIX);
    append(out->record_xml, ids);
    append(out->request_xml, PREFIX);
    append(out->request_xml, "<key>Label</key><string>Tab5</string>"
        "<key>Request</key><string>Pair</string>"
        "<key>ProtocolVersion</key><string>2</string>"
        "<key>PairingOptions</key><dict><key>ExtendedPairingErrors</key><true/></dict>"
        "<key>PairRecord</key><dict>");
    append(out->request_xml, ids);
    mbedtls_pk_context * subjects[] = {&root, &host, &device};
    const char * names[] = {"RootCertificate", "HostCertificate", "DeviceCertificate"};
    for(int i = 0; i < 3; i++) {
        if(certificate(subjects[i], &root, i == 0, &rng, pem) != 0 ||
           !append_pem(out->record_xml, names[i], pem) ||
           !append_pem(out->request_xml, names[i], pem)) goto done;
    }
    if(!append(out->request_xml, "</dict>") || !append(out->request_xml, SUFFIX)) goto done;
    /* Private keys are deliberately appended only to the local record. */
    if(mbedtls_pk_write_key_pem(&root, (unsigned char *)pem, PEM_CAP) != 0 ||
       !append_pem(out->record_xml, "RootPrivateKey", pem) ||
       mbedtls_pk_write_key_pem(&host, (unsigned char *)pem, PEM_CAP) != 0 ||
       !append_pem(out->record_xml, "HostPrivateKey", pem) ||
       !append(out->record_xml, SUFFIX)) goto done;
    result = ESP_OK;
done:
    if(pem) memset(pem, 0, PEM_CAP);
    free(pem); free(key);
    mbedtls_pk_free(&device); mbedtls_pk_free(&host); mbedtls_pk_free(&root);
    mbedtls_ctr_drbg_free(&rng); mbedtls_entropy_free(&entropy);
    if(result != ESP_OK) apple_pairing_free(out);
    return result;
}
