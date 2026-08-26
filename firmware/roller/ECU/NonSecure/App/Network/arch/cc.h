#ifndef ECU_LWIP_ARCH_CC_H
#define ECU_LWIP_ARCH_CC_H

#include <stdint.h>

typedef int sys_prot_t;

#define BYTE_ORDER LITTLE_ENDIAN
#define LWIP_PROVIDE_ERRNO 1
uint32_t ECU_LwipRandom(void);
#define LWIP_RAND() ECU_LwipRandom()

#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_STRUCT __attribute__((packed))
#define PACK_STRUCT_END
#define PACK_STRUCT_FIELD(x) x

void ECU_LwipAssert(const char *file, uint32_t line);
#define LWIP_PLATFORM_ASSERT(message) \
  do { (void)(message); ECU_LwipAssert(__FILE__, (uint32_t)__LINE__); } while (0)
#define LWIP_PLATFORM_DIAG(message) do { (void)0; } while (0)

#endif /* ECU_LWIP_ARCH_CC_H */
