#ifndef SECURITY_SHA256_H
#define SECURITY_SHA256_H

#include <stddef.h>
#include <stdint.h>

typedef struct
{
  uint32_t state[8];
  uint64_t total_size;
  uint8_t block[64];
  size_t block_size;
} SecuritySha256_Context;

void SecuritySha256_Init(SecuritySha256_Context *context);
void SecuritySha256_Update(SecuritySha256_Context *context,
                           const void *data, size_t size);
void SecuritySha256_Final(SecuritySha256_Context *context,
                          uint8_t digest[32]);
void SecuritySha256_Compute(const void *data, size_t size,
                            uint8_t digest[32]);

#endif /* SECURITY_SHA256_H */
