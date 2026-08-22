// SPDX-License-Identifier: Apache-2.0
// Standalone DES API extracted from mbedTLS (Apache-2.0). Symbol names match
// mbedtls_des_* so the loclass code compiles unchanged. Needed because ESP-IDF
// 6.1 (TF-PSA-Crypto 1.0) removed MBEDTLS_DES_C.

#ifndef POOM_DES_H
#define POOM_DES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MBEDTLS_DES_KEY_SIZE 8

typedef struct mbedtls_des_context {
    uint32_t sk[32]; // DES subkeys
} mbedtls_des_context;

typedef struct mbedtls_des3_context {
    uint32_t sk[96]; // 3DES subkeys
} mbedtls_des3_context;

void mbedtls_des_init(mbedtls_des_context* ctx);
void mbedtls_des_free(mbedtls_des_context* ctx);
void mbedtls_des3_init(mbedtls_des3_context* ctx);
void mbedtls_des3_free(mbedtls_des3_context* ctx);

void mbedtls_des_key_set_parity(unsigned char key[MBEDTLS_DES_KEY_SIZE]);
int mbedtls_des_key_check_key_parity(const unsigned char key[MBEDTLS_DES_KEY_SIZE]);
int mbedtls_des_key_check_weak(const unsigned char key[MBEDTLS_DES_KEY_SIZE]);

void mbedtls_des_setkey(uint32_t SK[32], const unsigned char key[MBEDTLS_DES_KEY_SIZE]);
int mbedtls_des_setkey_enc(mbedtls_des_context* ctx, const unsigned char key[MBEDTLS_DES_KEY_SIZE]);
int mbedtls_des_setkey_dec(mbedtls_des_context* ctx, const unsigned char key[MBEDTLS_DES_KEY_SIZE]);

int mbedtls_des3_set2key_enc(mbedtls_des3_context* ctx, const unsigned char key[MBEDTLS_DES_KEY_SIZE * 2]);
int mbedtls_des3_set2key_dec(mbedtls_des3_context* ctx, const unsigned char key[MBEDTLS_DES_KEY_SIZE * 2]);
int mbedtls_des3_set3key_enc(mbedtls_des3_context* ctx, const unsigned char key[MBEDTLS_DES_KEY_SIZE * 3]);
int mbedtls_des3_set3key_dec(mbedtls_des3_context* ctx, const unsigned char key[MBEDTLS_DES_KEY_SIZE * 3]);

int mbedtls_des_crypt_ecb(mbedtls_des_context* ctx, const unsigned char input[8], unsigned char output[8]);
int mbedtls_des3_crypt_ecb(mbedtls_des3_context* ctx, const unsigned char input[8], unsigned char output[8]);

#ifdef __cplusplus
}
#endif

#endif // POOM_DES_H
