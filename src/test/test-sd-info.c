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

close_controller:
    R_SDHI_Close(&g_sdmmc1_ctrl);
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
