/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test-touch-info.c
 * @brief 读取 GT911 型号和坐标范围。
 *
 * 独立复位 P801/P004，尝试 0x14/0x5D；要求型号 911、坐标范围 480×272。
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

#define GT911_REG_ID 0x8140u

static struct rt_i2c_bus_device *touch_bus;
static uint16_t touch_address;
/* GT911 寄存器地址大端发送，数据中的坐标值则为小端。 */
static int touch_read(uint16_t register_address, uint8_t *data, uint16_t length)
{
    uint8_t register_bytes[2] = {(uint8_t)(register_address >> 8), (uint8_t)register_address};
    struct rt_i2c_msg messages[2] = {
        {.addr = touch_address, .flags = RT_I2C_WR, .buf = register_bytes, .len = 2},
        {.addr = touch_address, .flags = RT_I2C_RD, .buf = data, .len = length}};
    if (rt_i2c_transfer(touch_bus, messages, 2) == 2)
    {
        return 0;
    }
    else
    {
        return -RT_EIO;
    }
}

/* 按现有板卡时序复位，尝试 GT911 两个合法地址并核对屏幕范围。 */
static int initialize_touch(void)
{
    uint8_t identity[11];
    touch_bus = (struct rt_i2c_bus_device *)rt_device_find("i2c1");
    if (!touch_bus)
    {
        return -RT_ENOSYS;
    }
    rt_pin_mode(BSP_IO_PORT_08_PIN_01, PIN_MODE_OUTPUT);
    rt_pin_write(BSP_IO_PORT_08_PIN_01, 0);
    rt_pin_mode(BSP_IO_PORT_00_PIN_04, PIN_MODE_OUTPUT);
    rt_pin_write(BSP_IO_PORT_00_PIN_04, 1);
    rt_thread_mdelay(10);
    rt_pin_write(BSP_IO_PORT_08_PIN_01, 1);
    rt_thread_mdelay(100);
    rt_pin_mode(BSP_IO_PORT_00_PIN_04, PIN_MODE_INPUT);
    touch_address = 0x14;
    if (touch_read(GT911_REG_ID, identity, sizeof(identity)))
    {
        touch_address = 0x5d;
        if (touch_read(GT911_REG_ID, identity, sizeof(identity)))
        {
            return -RT_EIO;
        }
    }
    unsigned width = identity[6] | identity[7] << 8;
    unsigned height = identity[8] | identity[9] << 8;
    rt_kprintf("TOUCH addr=%02X id=%c%c%c%c range=%ux%u\n",
               touch_address,
               identity[0],
               identity[1],
               identity[2],
               identity[3],
               width,
               height);
    if (memcmp(identity, "911", 3) != 0 || width != 480 || height != 272)
    {
        return -RT_ERROR;
    }
    return TEST_PASS;
}

/* 结束后恢复触摸引脚复用，便于随后独立启动其他例程。 */
static void test_restore_pin(bsp_io_port_pin_t pin)
{
    for (unsigned index = 0; index < g_bsp_pin_cfg.number_of_pins; ++index)
    {
        if (g_bsp_pin_cfg.p_pin_cfg_data[index].pin == pin)
        {
            R_IOPORT_PinCfg(&g_ioport_ctrl, pin, g_bsp_pin_cfg.p_pin_cfg_data[index].pin_cfg);
            return;
        }
    }
    R_IOPORT_PinCfg(&g_ioport_ctrl, pin, IOPORT_CFG_PORT_DIRECTION_INPUT);
}

/* 按本文件配置打开外设，执行验证；所有退出路径都在返回前清理本次资源。 */
static int run_test(void)
{
    int result;

    result = initialize_touch();

    return result;
}

/* 本文件唯一线程入口：独立完成初始化、验证和清理，然后自然返回。
 * error 保存最终结果，便于调用方在 RT-Thread 线程回收时读取。 */
void test_touch_info_thread(void *argument)
{
    int result;
    RT_UNUSED(argument);
    result = run_test();
    test_restore_pin(BSP_IO_PORT_00_PIN_04);
    test_restore_pin(BSP_IO_PORT_08_PIN_01);
    if (test_cancelled())
    {
        result = -RT_EINTR;
    }
    rt_thread_self()->error = result;
}
