/* SPDX-License-Identifier: Apache-2.0 */
/* raw lwIP 的 RT-Thread 时间、随机数和一次性初始化适配。
 * 调用者必须串行使用 NO_SYS 协议栈；此处不创建线程或网卡。 */
#include <rtthread.h>
#include "lwip/init.h"
#include "lwip/arch.h"

u32_t sys_now(void)
{
    return (u32_t)((rt_uint64_t)rt_tick_get() * 1000 / RT_TICK_PER_SECOND);
}
unsigned int rw007_net_random(void)
{
    static unsigned int state;
    if (state == 0)
    {
        state = 0x7865a21u ^ rt_tick_get();
    }
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}
void hmi_lwip_initialize(void)
{
    static rt_bool_t initialized;
    if (!initialized)
    {
        lwip_init();
        initialized = RT_TRUE;
    }
}
