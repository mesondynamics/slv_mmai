#include "security_sha256.h"

#include <string.h>

#define ROTR32(value, bits) (((value) >> (bits)) | ((value) << (32U - (bits))))

static const uint32_t sha256_round_constants[64] = {
  0x428a2f98UL,0x71374491UL,0xb5c0fbcfUL,0xe9b5dba5UL,
  0x3956c25bUL,0x59f111f1UL,0x923f82a4UL,0xab1c5ed5UL,
  0xd807aa98UL,0x12835b01UL,0x243185beUL,0x550c7dc3UL,
  0x72be5d74UL,0x80deb1feUL,0x9bdc06a7UL,0xc19bf174UL,
  0xe49b69c1UL,0xefbe4786UL,0x0fc19dc6UL,0x240ca1ccUL,
  0x2de92c6fUL,0x4a7484aaUL,0x5cb0a9dcUL,0x76f988daUL,
  0x983e5152UL,0xa831c66dUL,0xb00327c8UL,0xbf597fc7UL,
  0xc6e00bf3UL,0xd5a79147UL,0x06ca6351UL,0x14292967UL,
  0x27b70a85UL,0x2e1b2138UL,0x4d2c6dfcUL,0x53380d13UL,
  0x650a7354UL,0x766a0abbUL,0x81c2c92eUL,0x92722c85UL,
  0xa2bfe8a1UL,0xa81a664bUL,0xc24b8b70UL,0xc76c51a3UL,
  0xd192e819UL,0xd6990624UL,0xf40e3585UL,0x106aa070UL,
  0x19a4c116UL,0x1e376c08UL,0x2748774cUL,0x34b0bcb5UL,
  0x391c0cb3UL,0x4ed8aa4aUL,0x5b9cca4fUL,0x682e6ff3UL,
  0x748f82eeUL,0x78a5636fUL,0x84c87814UL,0x8cc70208UL,
  0x90befffaUL,0xa4506cebUL,0xbef9a3f7UL,0xc67178f2UL
};

static uint32_t SecuritySha256_LoadBe32(const uint8_t *data)
{
  return ((uint32_t)data[0] << 24U) | ((uint32_t)data[1] << 16U) |
         ((uint32_t)data[2] << 8U) | data[3];
}

static void SecuritySha256_StoreBe32(uint8_t *data, uint32_t value)
{
  data[0] = (uint8_t)(value >> 24U);
  data[1] = (uint8_t)(value >> 16U);
  data[2] = (uint8_t)(value >> 8U);
  data[3] = (uint8_t)value;
}

static void SecuritySha256_Transform(SecuritySha256_Context *context,
                                     const uint8_t block[64])
{
  uint32_t words[64];
  uint32_t a, b, c, d, e, f, g, h;
  uint32_t index;

  for (index = 0U; index < 16U; ++index)
  {
    words[index] = SecuritySha256_LoadBe32(&block[index * 4U]);
  }
  for (; index < 64U; ++index)
  {
    uint32_t s0 = ROTR32(words[index - 15U], 7U) ^
                  ROTR32(words[index - 15U], 18U) ^
                  (words[index - 15U] >> 3U);
    uint32_t s1 = ROTR32(words[index - 2U], 17U) ^
                  ROTR32(words[index - 2U], 19U) ^
                  (words[index - 2U] >> 10U);
    words[index] = words[index - 16U] + s0 + words[index - 7U] + s1;
  }
  a = context->state[0]; b = context->state[1];
  c = context->state[2]; d = context->state[3];
  e = context->state[4]; f = context->state[5];
  g = context->state[6]; h = context->state[7];
  for (index = 0U; index < 64U; ++index)
  {
    uint32_t sum1 = ROTR32(e, 6U) ^ ROTR32(e, 11U) ^ ROTR32(e, 25U);
    uint32_t choose = (e & f) ^ ((~e) & g);
    uint32_t temporary1 = h + sum1 + choose +
                          sha256_round_constants[index] + words[index];
    uint32_t sum0 = ROTR32(a, 2U) ^ ROTR32(a, 13U) ^ ROTR32(a, 22U);
    uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    uint32_t temporary2 = sum0 + majority;
    h = g; g = f; f = e; e = d + temporary1;
    d = c; c = b; b = a; a = temporary1 + temporary2;
  }
  context->state[0] += a; context->state[1] += b;
  context->state[2] += c; context->state[3] += d;
  context->state[4] += e; context->state[5] += f;
  context->state[6] += g; context->state[7] += h;
  memset(words, 0, sizeof(words));
}

void SecuritySha256_Init(SecuritySha256_Context *context)
{
  static const uint32_t initial[8] = {
    0x6a09e667UL,0xbb67ae85UL,0x3c6ef372UL,0xa54ff53aUL,
    0x510e527fUL,0x9b05688cUL,0x1f83d9abUL,0x5be0cd19UL
  };
  if (context == NULL) { return; }
  memcpy(context->state, initial, sizeof(initial));
  context->total_size = 0U;
  context->block_size = 0U;
  memset(context->block, 0, sizeof(context->block));
}

void SecuritySha256_Update(SecuritySha256_Context *context,
                           const void *data, size_t size)
{
  const uint8_t *bytes = (const uint8_t *)data;
  size_t amount;

  if ((context == NULL) || ((data == NULL) && (size != 0U))) { return; }
  context->total_size += size;
  while (size != 0U)
  {
    amount = sizeof(context->block) - context->block_size;
    if (amount > size) { amount = size; }
    memcpy(&context->block[context->block_size], bytes, amount);
    context->block_size += amount;
    bytes += amount;
    size -= amount;
    if (context->block_size == sizeof(context->block))
    {
      SecuritySha256_Transform(context, context->block);
      context->block_size = 0U;
    }
  }
}

void SecuritySha256_Final(SecuritySha256_Context *context,
                          uint8_t digest[32])
{
  uint64_t bit_size;
  uint32_t index;

  if ((context == NULL) || (digest == NULL)) { return; }
  bit_size = context->total_size * 8U;
  context->block[context->block_size++] = 0x80U;
  if (context->block_size > 56U)
  {
    memset(&context->block[context->block_size], 0,
           sizeof(context->block) - context->block_size);
    SecuritySha256_Transform(context, context->block);
    context->block_size = 0U;
  }
  memset(&context->block[context->block_size], 0, 56U - context->block_size);
  for (index = 0U; index < 8U; ++index)
  {
    context->block[63U - index] = (uint8_t)(bit_size >> (index * 8U));
  }
  SecuritySha256_Transform(context, context->block);
  for (index = 0U; index < 8U; ++index)
  {
    SecuritySha256_StoreBe32(&digest[index * 4U], context->state[index]);
  }
  memset(context, 0, sizeof(*context));
}

void SecuritySha256_Compute(const void *data, size_t size,
                            uint8_t digest[32])
{
  SecuritySha256_Context context;
  SecuritySha256_Init(&context);
  SecuritySha256_Update(&context, data, size);
  SecuritySha256_Final(&context, digest);
}
