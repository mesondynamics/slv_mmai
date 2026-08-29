#ifndef SECURITY_CRYPTO_H
#define SECURITY_CRYPTO_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SECURITY_CRYPTO_P256_SIZE 32U

bool SecurityCrypto_Init(void);
bool SecurityCrypto_RandomDigest(uint8_t digest[SECURITY_CRYPTO_P256_SIZE]);
bool SecurityCrypto_VerifyP256(
    const uint8_t public_key[2U * SECURITY_CRYPTO_P256_SIZE],
    const uint8_t digest[SECURITY_CRYPTO_P256_SIZE],
    const uint8_t signature[2U * SECURITY_CRYPTO_P256_SIZE]);

#ifdef __cplusplus
}
#endif

#endif /* SECURITY_CRYPTO_H */
