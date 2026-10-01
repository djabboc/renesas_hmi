/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test-touch-irq.c
 * @brief 验证 GT911 的触摸中断及完整触点读取。
 *
 * P004/IRQ9 下降沿只做计数；线程轮询读帧供对照，结束关闭 IRQ。
 * 读到触点但没有中断时返回 FAIL；无人触摸时返回 WAIT。
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

/* 无符号 tick 差值允许系统计时回绕；等待始终有上限。 */
static int test_elapsed(rt_tick_t start, unsigned milliseconds)
{
    return (rt_tick_t)(rt_tick_get() - start) >= rt_tick_from_millisecond(milliseconds);
}

#define GT911_REG_ID 0x8140u
#define GT911_REG_STATUS 0x814eu
#define GT911_REG_POINTS 0x814fu
#define GT911_DATA_READY 0x80u
#define GT911_POINT_COUNT_MASK 0x0fu
#define GT911_MAX_POINTS 5u
#define GT911_POINT_BYTES 8u
#define TOUCH_WINDOW_MS 15000u

/* 中断只计数；I2C 读帧和清状态在测试线程中完成。 */
static volatile unsigned touch_irq_count;
static void touch_irq_callback(external_irq_callback_args_t *arguments)
{
    RT_UNUSED(arguments);
    touch_irq_count++;
}
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
    /* 只修改 RST 和 INT；本工程的 rt_pin_mode 会重新打开整个 IOPORT，
     * 可能把刚设置的另一个引脚还原，破坏复位期间的地址选择电平。 */
    if (R_IOPORT_PinCfg(&g_ioport_ctrl,
                        BSP_IO_PORT_08_PIN_01,
                        IOPORT_CFG_PORT_DIRECTION_OUTPUT | IOPORT_CFG_PORT_OUTPUT_LOW) !=
        FSP_SUCCESS)
    {
        return -RT_ERROR;
    }
    if (R_IOPORT_PinCfg(&g_ioport_ctrl,
                        BSP_IO_PORT_00_PIN_04,
                        IOPORT_CFG_PORT_DIRECTION_OUTPUT | IOPORT_CFG_PORT_OUTPUT_HIGH) !=
        FSP_SUCCESS)
    {
        return -RT_ERROR;
    }
    rt_thread_mdelay(10);
    if (R_IOPORT_PinWrite(&g_ioport_ctrl, BSP_IO_PORT_08_PIN_01, BSP_IO_LEVEL_HIGH) !=
        FSP_SUCCESS)
    {
        return -RT_ERROR;
    }
    rt_thread_mdelay(100);
    /* 普通输入模式会清除 ISEL；必须同时保留 IRQ 输入使能。 */
    if (R_IOPORT_PinCfg(&g_ioport_ctrl,
                        BSP_IO_PORT_00_PIN_04,
                        IOPORT_CFG_PORT_DIRECTION_INPUT | IOPORT_CFG_IRQ_ENABLE) != FSP_SUCCESS)
    {
        return -RT_ERROR;
    }
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

/* 只统计合法触点；位置与多指操作仍需人工对照，结束返回 WAIT。 */
static int observe_touch_points(unsigned *observed_points)
{
    uint8_t status;
    uint8_t points[GT911_MAX_POINTS * GT911_POINT_BYTES];
    /* 寄存器地址高字节在前，写 0 清除本帧就绪标志。 */
    uint8_t clear_status[] = {GT911_REG_STATUS >> 8, GT911_REG_STATUS & 0xff, 0};
    *observed_points = 0;
    rt_tick_t start = rt_tick_get();
    while (!test_elapsed(start, TOUCH_WINDOW_MS) && !test_cancelled())
    {
        if (touch_read(GT911_REG_STATUS, &status, 1))
        {
            return -RT_EIO;
        }
        if (status & GT911_DATA_READY)
        {
            unsigned count = status & GT911_POINT_COUNT_MASK;
            if (count > GT911_MAX_POINTS)
            {
                return -RT_ERROR;
            }
            if (count && touch_read(GT911_REG_POINTS, points, count * GT911_POINT_BYTES))
            {
                return -RT_EIO;
            }
            if (rt_i2c_master_send(touch_bus, touch_address, 0, clear_status, 3) != 3)
            {
                return -RT_EIO;
            }
            rt_kprintf("TOUCH N=%u", count);
            for (unsigned point_index = 0; point_index < count; ++point_index)
            {
                unsigned x = points[point_index * GT911_POINT_BYTES + 1] |
                             points[point_index * GT911_POINT_BYTES + 2] << 8;
                unsigned y = points[point_index * GT911_POINT_BYTES + 3] |
                             points[point_index * GT911_POINT_BYTES + 4] << 8;
                if (x >= 480 || y >= 272)
                {
                    return -RT_ERROR;
                }
                rt_kprintf(" id=%u (%u,%u)", points[point_index * GT911_POINT_BYTES], x, y);
                *observed_points += 1;
            }
            rt_kprintf("\n");
        }
        rt_thread_mdelay(10);
    }
    rt_kprintf("TOUCH observed_points=%u; position/multitouch accuracy requires interaction\n",
               *observed_points);
    return TEST_WAIT;
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
    static external_irq_cfg_t irq_config;
    unsigned observed_points = 0;
    int result;

    result = initialize_touch();
    if (result != TEST_PASS)
    {
        return result;
    }
    touch_irq_count = 0;
    /* 本工程的 GPIO IRQ 接口只保存 mode，没有据此修改硬件触发方式。
     * 独立复制 IRQ9 配置，明确选择下降沿；不修改 FSP 生成文件。 */
    irq_config = g_external_irq9_cfg;
    irq_config.trigger = EXTERNAL_IRQ_TRIG_FALLING;
    irq_config.p_callback = touch_irq_callback;
    if (R_ICU_ExternalIrqOpen(&g_external_irq9_ctrl, &irq_config) != FSP_SUCCESS)
    {
        return -RT_ERROR;
    }
    if (R_ICU_ExternalIrqEnable(&g_external_irq9_ctrl) != FSP_SUCCESS)
    {
        R_ICU_ExternalIrqClose(&g_external_irq9_ctrl);
        return -RT_ERROR;
    }
    rt_kprintf("TOUCH P004/IRQ9 falling-edge enabled; touch within 15 seconds\n");
    result = observe_touch_points(&observed_points);
    if (R_ICU_ExternalIrqDisable(&g_external_irq9_ctrl) != FSP_SUCCESS)
    {
        result = -RT_ERROR;
    }
    if (R_ICU_ExternalIrqClose(&g_external_irq9_ctrl) != FSP_SUCCESS)
    {
        result = -RT_ERROR;
    }
    rt_kprintf("TOUCH falling-edge IRQ count=%u; touch the panel to generate edges\n",
               touch_irq_count);
    /* 轮询有数据不代表 IRQ 有效；必须同时看到触点和真实中断。
     * 无人操作仍为 WAIT，避免把缺少验收动作当成硬件故障。 */
    if (result == TEST_WAIT && observed_points > 0)
    {
        if (touch_irq_count == 0)
        {
            rt_kprintf("TOUCH IRQ FAIL: points received but no interrupt detected\n");
            result = -RT_ERROR;
        }
        else
        {
            result = TEST_PASS;
        }
    }
    return result;
}

/* 本文件唯一线程入口：独立完成初始化、验证和清理，然后自然返回。
 * error 保存最终结果，便于调用方在 RT-Thread 线程回收时读取。 */
void test_touch_irq_thread(void *argument)
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
