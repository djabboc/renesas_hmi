/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test-rtc-tick.c
 * @brief 验证 32.768 kHz 晶振驱动的 RTC 走时。
 *
 * 写固定测试日期并检查秒计数；会覆盖日历，不测试断电保持。
 * 阅读顺序：文件末尾线程入口 → run_test → 本文件的硬件辅助函数。
 * 只依赖 RT-Thread、FSP 及所用库，不调用其他测试文件。
 */
#include <rtthread.h>
#include <rtdevice.h>
#include "hal_data.h"
#include <string.h>
#include "r_rtc.h"

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

/* 与 ra_gen/vector_data.c 的事件路由保持一致。 */
#define RTC_TEST_ALARM_IRQ ((IRQn_Type)41)
#define RTC_TEST_CARRY_IRQ ((IRQn_Type)42)
#define RTC_OSCILLATOR_SETTLE_MS 2200u
#define RTC_OBSERVATION_MS 3200u

static rtc_instance_ctrl_t rtc_control;
static volatile unsigned alarm_count;

/* 只统计 RTC 闹钟事件，走时和日期验证由线程完成。 */
static void rtc_event(rtc_callback_args_t *args)
{
    if (args->event == RTC_EVENT_ALARM_IRQ)
    {
        ++alarm_count;
    }
}

/* 按本文件配置打开外设，执行验证；所有退出路径都在返回前清理本次资源。 */
static int run_test(void)
{
    rtc_error_adjustment_cfg_t adjustment_config = {
        .adjustment_mode = RTC_ERROR_ADJUSTMENT_MODE_MANUAL,
        .adjustment_period = RTC_ERROR_ADJUSTMENT_PERIOD_NONE,
        .adjustment_type = RTC_ERROR_ADJUSTMENT_NONE};
    rtc_cfg_t rtc_config = {.clock_source = RTC_CLOCK_SOURCE_SUBCLK,
                            .p_err_cfg = &adjustment_config,
                            .alarm_irq = RTC_TEST_ALARM_IRQ,
                            .alarm_ipl = 12,
                            .periodic_irq = FSP_INVALID_VECTOR,
                            .carry_irq = RTC_TEST_CARRY_IRQ,
                            .carry_ipl = 12,
                            .p_callback = rtc_event};
    /* struct tm：年份自 1900 起计，月份从 0 起计，即 2026-10-01。 */
    rtc_time_t test_time = {.tm_year = 126,
                            .tm_mon = 9,
                            .tm_mday = 1,
                            .tm_hour = 12,
                            .tm_min = 0,
                            .tm_sec = 0,
                            .tm_wday = 4};
    rtc_time_t current_time;

    int result = -RT_ERROR;

    /* 开启低速晶振并等待稳定，再让 RTC 选择 SUBCLK。 */
    R_BSP_RegisterProtectDisable(BSP_REG_PROTECT_CGC);
    if (R_SYSTEM->SOSCCR)
    {
        R_SYSTEM->SOMCR = 0;
        R_SYSTEM->SOSCCR = 0;
    }
    R_BSP_RegisterProtectEnable(BSP_REG_PROTECT_CGC);
    rt_thread_mdelay(RTC_OSCILLATOR_SETTLE_MS);
    if (R_RTC_Open(&rtc_control, &rtc_config) != FSP_SUCCESS)
    {
        return -RT_ERROR;
    }
    if (R_RTC_CalendarTimeSet(&rtc_control, &test_time) != FSP_SUCCESS)
    {
        goto close_rtc;
    }
    /* 本例不配置闹钟；这里只检查本次设置日期后的秒计数。 */
    alarm_count = 0;

    rt_thread_mdelay(RTC_OBSERVATION_MS);
    if (R_RTC_CalendarTimeGet(&rtc_control, &current_time) != FSP_SUCCESS)
    {
        goto close_rtc;
    }
    rt_kprintf("RTC source=32.768k crystal date=%04d-%02d-%02d %02d:%02d:%02d alarm_count=%u\n",
               current_time.tm_year + 1900,
               current_time.tm_mon + 1,
               current_time.tm_mday,
               current_time.tm_hour,
               current_time.tm_min,
               current_time.tm_sec,
               alarm_count);
    if (current_time.tm_sec >= 2 && current_time.tm_sec <= 4 && current_time.tm_year == 126 &&
        current_time.tm_mon == 9 && current_time.tm_mday == 1)
    {
        result = 0;
    }
    else
    {
        result = -RT_ERROR;
    }
close_rtc:
    R_RTC_Close(&rtc_control);
    return result;
}

/* 本文件唯一线程入口：独立完成初始化、验证和清理，然后自然返回。
 * error 保存最终结果，便于调用方在 RT-Thread 线程回收时读取。 */
void test_rtc_tick_thread(void *argument)
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
