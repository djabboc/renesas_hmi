#ifndef HMI_RW007_LWIP_CC_H
#define HMI_RW007_LWIP_CC_H
#include <rtthread.h>
#define BYTE_ORDER LITTLE_ENDIAN
#define PACK_STRUCT_STRUCT __attribute__((packed))
#define PACK_STRUCT_FIELD(x) x
#define PACK_STRUCT_BEGIN
#define PACK_STRUCT_END
#define LWIP_PLATFORM_DIAG(x) do { rt_kprintf x; } while (0)
#define LWIP_PLATFORM_ASSERT(x) do { rt_kprintf("lwIP: %s\n", x); RT_ASSERT(0); } while (0)
unsigned int rw007_net_random(void);
#define LWIP_RAND() rw007_net_random()
#endif
