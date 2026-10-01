/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test-touch.c
 * @brief GT911 身份及有限时长触点测试；不依赖 LVGL。
 * 复位脚 P801、INT P004，I2C1/P202/P203 与 Arduino I2C 共用。
 * 每次读取完整触点帧后清除就绪位，释放触摸控制器的下一帧上报。
 */
#include "peripheral-test.h"

#define GT911_REG_ID 0x8140u
#define GT911_REG_STATUS 0x814eu
#define GT911_REG_POINTS 0x814fu
#define GT911_DATA_READY 0x80u
#define GT911_POINT_COUNT_MASK 0x0fu
#define GT911_MAX_POINTS 5u
#define GT911_POINT_BYTES 8u
#define TOUCH_WINDOW_MS 15000u

static struct rt_i2c_bus_device *touch_bus;
static uint16_t touch_address;
/* GT911 寄存器地址大端发送，数据中的坐标值则为小端。 */
static int touch_read(uint16_t register_address, uint8_t *data, uint16_t length)
{
    uint8_t register_bytes[2] = {(uint8_t)(register_address >> 8), (uint8_t)register_address};
    struct rt_i2c_msg messages[2] = {
        {.addr = touch_address, .flags = RT_I2C_WR, .buf = register_bytes, .len = 2},
        {.addr = touch_address, .flags = RT_I2C_RD, .buf = data, .len = length}};
    return rt_i2c_transfer(touch_bus, messages, 2) == 2 ? 0 : -RT_EIO;
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
    if (memcmp(identity, "911", 3) || width != 480 || height != 272)
    {
        return -RT_ERROR;
    }
    return TEST_PASS;
}

/* 只统计合法触点；位置与多指操作仍需人工对照，结束返回 WAIT。 */
static int observe_touch_points(void)
{
    uint8_t status;
    uint8_t points[GT911_MAX_POINTS * GT911_POINT_BYTES];
    /* 寄存器地址高字节在前，写 0 清除本帧就绪标志。 */
    uint8_t clear_status[] = {GT911_REG_STATUS >> 8, GT911_REG_STATUS & 0xff, 0};
    unsigned observed_points = 0;
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
                ++observed_points;
            }
            rt_kprintf("\n");
        }
        rt_thread_mdelay(10);
    }
    rt_kprintf("TOUCH observed_points=%u; position/multitouch accuracy requires interaction\n",
               observed_points);
    return TEST_WAIT;
}

int test_touch(const char *stage)
{
    int result;

    if (strcmp(stage, "info") != 0 && strcmp(stage, "points") != 0)
    {
        return -RT_EINVAL;
    }
    result = initialize_touch();
    if (result != TEST_PASS || strcmp(stage, "info") == 0)
    {
        return result;
    }
    return observe_touch_points();
}
