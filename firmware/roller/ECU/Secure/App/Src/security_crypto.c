#include "security_crypto.h"

#include <string.h>

#include "stm32h5xx_hal.h"

/* NIST P-256 constants, big-endian, from FIPS 186 and ST's H5 PKA example. */
static const uint8_t p256_prime[32] = {
  0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x01,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0xff,0xff,0xff,0xff,
  0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff
};
static const uint8_t p256_abs_a[32] = {
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
  0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x03
};
static const uint8_t p256_gx[32] = {
  0x6b,0x17,0xd1,0xf2,0xe1,0x2c,0x42,0x47,
  0xf8,0xbc,0xe6,0xe5,0x63,0xa4,0x40,0xf2,
  0x77,0x03,0x7d,0x81,0x2d,0xeb,0x33,0xa0,
  0xf4,0xa1,0x39,0x45,0xd8,0x98,0xc2,0x96
};
static const uint8_t p256_gy[32] = {
  0x4f,0xe3,0x42,0xe2,0xfe,0x1a,0x7f,0x9b,
  0x8e,0xe7,0xeb,0x4a,0x7c,0x0f,0x9e,0x16,
  0x2b,0xce,0x33,0x57,0x6b,0x31,0x5e,0xce,
  0xcb,0xb6,0x40,0x68,0x37,0xbf,0x51,0xf5
};
static const uint8_t p256_order[32] = {
  0xff,0xff,0xff,0xff,0x00,0x00,0x00,0x00,
  0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
  0xbc,0xe6,0xfa,0xad,0xa7,0x17,0x9e,0x84,
  0xf3,0xb9,0xca,0xc2,0xfc,0x63,0x25,0x51
};

static RNG_HandleTypeDef security_rng;
static PKA_HandleTypeDef security_pka;
static uint8_t security_crypto_ready;

bool SecurityCrypto_Init(void)
{
  RCC_OscInitTypeDef oscillator = {0};
  RCC_PeriphCLKInitTypeDef peripheral_clock = {0};

  if (security_crypto_ready != 0U) { return true; }

  oscillator.OscillatorType = RCC_OSCILLATORTYPE_HSI48;
  oscillator.HSI48State = RCC_HSI48_ON;
  oscillator.PLL.PLLState = RCC_PLL_NONE;
  if (HAL_RCC_OscConfig(&oscillator) != HAL_OK) { return false; }

  peripheral_clock.PeriphClockSelection = RCC_PERIPHCLK_RNG;
  peripheral_clock.RngClockSelection = RCC_RNGCLKSOURCE_HSI48;
  if (HAL_RCCEx_PeriphCLKConfig(&peripheral_clock) != HAL_OK) { return false; }

  __HAL_RCC_RNG_CLK_ENABLE();
  security_rng.Instance = RNG;
  security_rng.Init.ClockErrorDetection = RNG_CED_ENABLE;
  if (HAL_RNG_Init(&security_rng) != HAL_OK) { return false; }

  __HAL_RCC_PKA_CLK_ENABLE();
  security_pka.Instance = PKA;
  if (HAL_PKA_Init(&security_pka) != HAL_OK)
  {
    (void)HAL_RNG_DeInit(&security_rng);
    return false;
  }
  security_crypto_ready = 1U;
  return true;
}

bool SecurityCrypto_RandomDigest(uint8_t digest[SECURITY_CRYPTO_P256_SIZE])
{
  uint32_t word;
  uint32_t index;

  if ((digest == NULL) || !SecurityCrypto_Init()) { return false; }
  for (index = 0U; index < SECURITY_CRYPTO_P256_SIZE; index += sizeof(word))
  {
    if (HAL_RNG_GenerateRandomNumber(&security_rng, &word) != HAL_OK)
    {
      memset(digest, 0, SECURITY_CRYPTO_P256_SIZE);
      return false;
    }
    memcpy(&digest[index], &word, sizeof(word));
  }
  return true;
}

bool SecurityCrypto_VerifyP256(
    const uint8_t public_key[2U * SECURITY_CRYPTO_P256_SIZE],
    const uint8_t digest[SECURITY_CRYPTO_P256_SIZE],
    const uint8_t signature[2U * SECURITY_CRYPTO_P256_SIZE])
{
  PKA_ECDSAVerifInTypeDef input = {0};

  if ((public_key == NULL) || (digest == NULL) || (signature == NULL) ||
      !SecurityCrypto_Init())
  {
    return false;
  }
  input.primeOrderSize = sizeof(p256_order);
  input.modulusSize = sizeof(p256_prime);
  input.coefSign = 1U;
  input.coef = p256_abs_a;
  input.modulus = p256_prime;
  input.basePointX = p256_gx;
  input.basePointY = p256_gy;
  input.pPubKeyCurvePtX = &public_key[0];
  input.pPubKeyCurvePtY = &public_key[SECURITY_CRYPTO_P256_SIZE];
  input.RSign = &signature[0];
  input.SSign = &signature[SECURITY_CRYPTO_P256_SIZE];
  input.hash = digest;
  input.primeOrder = p256_order;

  if (HAL_PKA_ECDSAVerif(&security_pka, &input, 1000U) != HAL_OK)
  {
    return false;
  }
  return HAL_PKA_ECDSAVerif_IsValidSignature(&security_pka) == SET;
}
