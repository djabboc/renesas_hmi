/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test-sd-info.c
 * @brief 读取 TF 卡容量、扇区尺寸和写保护信息。
 *
 * 插入 TF 卡；本例只读取卡信息，不读写文件；无卡返回 SKIP。
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

#define SD_SECTOR_BYTES 512u

/* 卡座 CD 接 P405，而不是 SDHI1 的专用 CD 引脚；低电平表示插卡。
 * 每个例程自行配置和检测，不能把 CARD_DETECT_NONE 的状态当成插卡证据。 */
#define SD_CARD_DETECT_PIN BSP_IO_PORT_04_PIN_05
static const bsp_io_port_pin_t sd_bus_pins[] = {
    BSP_IO_PORT_05_PIN_00, BSP_IO_PORT_05_PIN_01, BSP_IO_PORT_05_PIN_02,
    BSP_IO_PORT_05_PIN_03, BSP_IO_PORT_05_PIN_04, BSP_IO_PORT_05_PIN_05};

static int card_is_inserted(void)
{
    bsp_io_level_t level;
    if (R_IOPORT_PinRead(&g_ioport_ctrl, SD_CARD_DETECT_PIN, &level) != FSP_SUCCESS)
    {
        return -RT_EIO;
    }
    if (level == BSP_IO_LEVEL_LOW)
    {
        return 1;
    }
    return 0;
}

/* 配置六根总线引脚与独立检测输入；不重新打开整个 IOPORT。 */
static int configure_sd_pins(void)
{
    for (unsigned index = 0; index < sizeof(sd_bus_pins) / sizeof(sd_bus_pins[0]); ++index)
    {
        if (R_IOPORT_PinCfg(&g_ioport_ctrl, sd_bus_pins[index],
                            IOPORT_CFG_DRIVE_HIGH | IOPORT_CFG_PERIPHERAL_PIN |
                                IOPORT_PERIPHERAL_SDHI_MMC) != FSP_SUCCESS)
        {
            return -RT_ERROR;
        }
    }
    if (R_IOPORT_PinCfg(&g_ioport_ctrl, SD_CARD_DETECT_PIN,
                        IOPORT_CFG_PORT_DIRECTION_INPUT | IOPORT_CFG_PULLUP_ENABLE) != FSP_SUCCESS)
    {
        return -RT_ERROR;
    }
    /* 等待输入稳定；卡应在命令开始前插稳。 */
    rt_thread_mdelay(20);
    return RT_EOK;
}

/* 退出时恢复生成配置；未在生成表中定义的 P405 恢复为普通输入。 */
static void restore_sd_pin(bsp_io_port_pin_t pin)
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

static void restore_sd_pins(void)
{
    for (unsigned index = 0; index < sizeof(sd_bus_pins) / sizeof(sd_bus_pins[0]); ++index)
    {
        restore_sd_pin(sd_bus_pins[index]);
    }
    restore_sd_pin(SD_CARD_DETECT_PIN);
}

static sdmmc_device_t card_info;
static sdmmc_cfg_t sd_config;
static volatile unsigned transfer_events;
/* SDHI 回调只记录硬件事件；本例不发起块数据传输。 */

static void sd_callback(sdmmc_callback_args_t *arguments)
{
    transfer_events |= arguments->event;
}

/* 独立打开 SDHI、查询卡信息，再关闭控制器；不写介质。 */

static int run_test(void)
{
    int inserted;
    fsp_err_t error;
    int result = -RT_ERROR;

    if (configure_sd_pins() != RT_EOK)
    {
        restore_sd_pins();
        return -RT_ERROR;
    }
    inserted = card_is_inserted();
    rt_kprintf("SD P405 detect=%d (1=inserted, 0=absent)\n", inserted);
    if (inserted < 0)
    {
        restore_sd_pins();
        return -RT_EIO;
    }
    if (inserted == 0)
    {
        restore_sd_pins();
        return TEST_SKIP;
    }
    sd_config = g_sdmmc1_cfg;
    /* 用 GPIO 检测卡；禁用未接线的 SDHI 专用 CD 检测。 */
    sd_config.card_detect = SDMMC_CARD_DETECT_NONE;
    sd_config.p_callback = sd_callback;
    transfer_events = 0;
    error = R_SDHI_Open(&g_sdmmc1_ctrl, &sd_config);
    if (error != FSP_SUCCESS)
    {
        rt_kprintf("SD open error=%d\n", error);
        restore_sd_pins();
        return -RT_ERROR;
    }
    error = R_SDHI_MediaInit(&g_sdmmc1_ctrl, &card_info);
    if (error != FSP_SUCCESS)
    {
        rt_kprintf("SD media init error=%d\n", error);
        goto close_controller;
    }
    rt_kprintf("SD sectors=%u bytes/sector=%u clock=%u protected=%u\n",
               card_info.sector_count,
               card_info.sector_size_bytes,
               card_info.clock_rate,
               card_info.write_protected);
    if (card_info.sector_size_bytes != SD_SECTOR_BYTES)
    {
        goto close_controller;
    }
    result = TEST_PASS;

close_controller:
    R_SDHI_Close(&g_sdmmc1_ctrl);
    restore_sd_pins();
    return result;
}

/* 本文件唯一线程入口：独立完成初始化、验证和清理，然后自然返回。
 * error 保存最终结果，便于调用方在 RT-Thread 线程回收时读取。 */
void test_sd_info_thread(void *argument)
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
