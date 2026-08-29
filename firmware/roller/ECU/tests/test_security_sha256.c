#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "security_sha256.h"

static int check_digest(const void *data, size_t size,
                        const uint8_t expected[32])
{
  uint8_t digest[32];
  SecuritySha256_Compute(data, size, digest);
  return memcmp(digest, expected, sizeof(digest)) == 0;
}

int main(void)
{
  static const uint8_t empty_digest[32] = {
    0xe3,0xb0,0xc4,0x42,0x98,0xfc,0x1c,0x14,
    0x9a,0xfb,0xf4,0xc8,0x99,0x6f,0xb9,0x24,
    0x27,0xae,0x41,0xe4,0x64,0x9b,0x93,0x4c,
    0xa4,0x95,0x99,0x1b,0x78,0x52,0xb8,0x55
  };
  static const uint8_t abc_digest[32] = {
    0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,
    0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
    0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,
    0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad
  };
  SecuritySha256_Context context;
  uint8_t split_digest[32];

  if (!check_digest(NULL, 0U, empty_digest) ||
      !check_digest("abc", 3U, abc_digest))
  {
    fputs("SHA-256 known-vector failure\n", stderr);
    return 1;
  }
  SecuritySha256_Init(&context);
  SecuritySha256_Update(&context, "a", 1U);
  SecuritySha256_Update(&context, "bc", 2U);
  SecuritySha256_Final(&context, split_digest);
  if (memcmp(split_digest, abc_digest, sizeof(split_digest)) != 0)
  {
    fputs("SHA-256 incremental failure\n", stderr);
    return 1;
  }
  puts("SHA-256 tests: PASS");
  return 0;
}
