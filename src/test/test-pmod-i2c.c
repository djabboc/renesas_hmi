/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test-pmod-i2c.c
 * @brief 只读探测外部 I2C 夹具地址 0x50。
 *
 * 使用 I2C1，与 GT911 共用总线；夹具必须兼容当前地址单字节读；无响应返回 SKIP。
 * 阅读顺序：文件末尾线程入口 → run_test → 本文件的硬件辅助函数。
 * 只依赖 RT-Thread、FSP 及所用库，不调用其他测试文件。
 */
#include <rtthread.h>
#include <rtdevice.h>
#include "hal_data.h"
#include <string.h>

/* 结果：0=断言通过，1=等待人工观察，2=缺少测试条件，负值=错误。 */
enum
{
    TEST_PASS = 0,
    TEST_WAIT = 1,
    TEST_SKIP = 2
};

/* 参数可为 NULL；提供 RT-Thread 事件对象时，bit0 表示请求协作退出。
 * 不清除事件位，让初始化、收发和清理路径都能看见同一次停止请求。 */
static int test_cancelled(void)
{
    rt_event_t stop_event = (rt_event_t)rt_thread_self()->parameter;
    rt_uint32_t received = 0;
    if (stop_event == RT_NULL)
    {
        return 0;
    }
    return rt_event_recv(stop_event, 1, RT_EVENT_FLAG_OR, 0, &received) == RT_EOK;
}

#define PMOD_I2C_FIXTURE_ADDRESS 0x50u

/* 只读取已知夹具地址；ACK 通过不代表外接设备的内容正确。 */
static int run_test(void)
{
    struct rt_i2c_bus_device *bus = (struct rt_i2c_bus_device *)rt_device_find("i2c1");
    uint8_t byte;
    if (!bus)
    {
        return -RT_ENOSYS;
    }
    /* 固定地址 0x50，只读当前字节；与 GT911 共用总线，不做写扫描。 */
    if (rt_i2c_master_recv(bus, PMOD_I2C_FIXTURE_ADDRESS, 0, &byte, 1) != 1)
    {
        rt_kprintf("I2C no response at external fixture 0x50\n");
        return TEST_SKIP;
    }
    rt_kprintf("I2C external 0x50 byte=%02X; device content not checked\n", byte);
    return 0;
}

/* 本文件唯一线程入口：独立完成初始化、验证和清理，然后自然返回。
 * error 保存最终结果，便于调用方在 RT-Thread 线程回收时读取。 */
void test_pmod_i2c_thread(void *argument)
{
    int result;
    RT_UNUSED(argument);
    result = run_test();
    if (test_cancelled())
    {
        result = -RT_EINTR;
    }
    rt_thread_self()->error = result;
}
