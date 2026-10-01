/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test-sd-read.c
 * @brief 只读验证 TF 卡原始扇区。
 *
 * 独立初始化 SDHI1，读取扇区 0 两次并比对，不改动卡内数据。
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

#define SD_SECTOR_BYTES 512u
#define SD_TRANSFER_TIMEOUT_MS 2000u

static sdmmc_device_t card_info;
static sdmmc_cfg_t sd_config;
static volatile unsigned transfer_events;

/* SDHI 中断累积完成、错误和拔卡事件，线程等待并判定。 */
static void sd_callback(sdmmc_callback_args_t *arguments)
{
    transfer_events |= arguments->event;
}

/* 完成、传输错误和拔卡均结束等待；错误优先于完成位判定。 */
static int wait_for_sd_transfer(void)
{
    rt_tick_t start = rt_tick_get();
    while (!(transfer_events & (SDMMC_EVENT_TRANSFER_COMPLETE | SDMMC_EVENT_TRANSFER_ERROR |
                                SDMMC_EVENT_CARD_REMOVED)) &&
           !test_elapsed(start, SD_TRANSFER_TIMEOUT_MS) && !test_cancelled())
    {
        rt_thread_mdelay(1);
    }
    if ((transfer_events & SDMMC_EVENT_TRANSFER_COMPLETE) &&
        !(transfer_events & (SDMMC_EVENT_TRANSFER_ERROR | SDMMC_EVENT_CARD_REMOVED)))
    {
        return 0;
    }
    else
    {
        return -RT_EIO;
    }
}

/* 打开 SDHI、初始化介质、只读两次扇区 0，最后关闭控制器。 */
static int run_test(void)
{
    sdmmc_status_t status;
    int result = -RT_ERROR;

    sd_config = g_sdmmc1_cfg;
    sd_config.p_callback = sd_callback;
    if (R_SDHI_Open(&g_sdmmc1_ctrl, &sd_config) != FSP_SUCCESS)
    {
        return -RT_ERROR;
    }
    if (R_SDHI_StatusGet(&g_sdmmc1_ctrl, &status) != FSP_SUCCESS)
    {
        goto close_controller;
    }
    rt_kprintf("SD card_inserted=%u\n", status.card_inserted);
    if (!status.card_inserted)
    {
        result = TEST_SKIP;
        goto close_controller;
    }
    if (R_SDHI_MediaInit(&g_sdmmc1_ctrl, &card_info) != FSP_SUCCESS)
    {
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

    /* 两个 DMA 缓冲都按四字节对齐；每次传输结束后才能比较数据。 */
    uint8_t first_sector[SD_SECTOR_BYTES] BSP_ALIGN_VARIABLE(4);
    uint8_t second_sector[SD_SECTOR_BYTES] BSP_ALIGN_VARIABLE(4);
    transfer_events = 0;
    if (R_SDHI_Read(&g_sdmmc1_ctrl, first_sector, 0, 1) != FSP_SUCCESS ||
        wait_for_sd_transfer() != RT_EOK)
    {
        result = -RT_EIO;
        goto close_controller;
    }
    transfer_events = 0;
    if (R_SDHI_Read(&g_sdmmc1_ctrl, second_sector, 0, 1) != FSP_SUCCESS ||
        wait_for_sd_transfer() != RT_EOK)
    {
        result = -RT_EIO;
        goto close_controller;
    }
    if (memcmp(first_sector, second_sector, SD_SECTOR_BYTES) != 0)
    {
        result = -RT_ERROR;
        goto close_controller;
    }
    rt_kprintf("SD sector0 repeat-read matched, signature=%02X%02X\n",
               first_sector[510],
               first_sector[511]);
    result = TEST_PASS;

close_controller:
    R_SDHI_Close(&g_sdmmc1_ctrl);
    return result;
}

/* 本文件唯一线程入口：独立完成初始化、验证和清理，然后自然返回。
 * error 保存最终结果，便于调用方在 RT-Thread 线程回收时读取。 */
void test_sd_read_thread(void *argument)
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
