/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test-rtc.c
 * @brief 32.768kHz 晶振走时及 RTC 闹钟测试。
 *
 * 每次覆盖日历为固定测试日期，结束关闭 RTC；这不是日历应用。
 * 板卡 VBATT 接 3.3V，无独立电池，不测试断电保持。
 */
#include "peripheral-test.h"
#include "r_rtc.h"

/* 与 ra_gen/vector_data.c 的事件路由保持一致。 */
#define RTC_TEST_ALARM_IRQ ((IRQn_Type)41)
#define RTC_TEST_CARRY_IRQ ((IRQn_Type)42)
#define RTC_OSCILLATOR_SETTLE_MS 2200u
#define RTC_OBSERVATION_MS 3200u

static rtc_instance_ctrl_t rtc_control;
static volatile unsigned alarm_count;

static void rtc_event(rtc_callback_args_t *args)
{
    if (args->event == RTC_EVENT_ALARM_IRQ)
    {
        ++alarm_count;
    }
}

int test_rtc(const char *stage)
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
    rtc_alarm_time_t alarm_config = {.sec_match = true};
    int result = -RT_ERROR;
    if (strcmp(stage, "tick") && strcmp(stage, "alarm"))
    {
        return -RT_EINVAL;
    }
    /* 开启低速晶振并等待稳定，再让 RTC 选择 SUBCLK。 */
    R_BSP_RegisterProtectDisable(BSP_REG_PROTECT_CGC);
    if (R_SYSTEM->SOSCCR)
    {
        R_SYSTEM->SOMCR = 0;
        R_SYSTEM->SOSCCR = 0;
    }
    R_BSP_RegisterProtectEnable(BSP_REG_PROTECT_CGC);
    rt_thread_mdelay(RTC_OSCILLATOR_SETTLE_MS);
    if (R_RTC_Open(&rtc_control, &rtc_config))
    {
        return -RT_ERROR;
    }
    if (R_RTC_CalendarTimeSet(&rtc_control, &test_time))
    {
        goto close_rtc;
    }
    /* 只匹配秒字段，在观察窗口内第 2 秒应触发且只触发一次。 */
    alarm_count = 0;
    if (strcmp(stage, "alarm") == 0)
    {
        alarm_config.time = test_time;
        alarm_config.time.tm_sec = 2;
        if (R_RTC_CalendarAlarmSet(&rtc_control, &alarm_config))
        {
            goto close_rtc;
        }
    }
    rt_thread_mdelay(RTC_OBSERVATION_MS);
    if (R_RTC_CalendarTimeGet(&rtc_control, &current_time))
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
    result = current_time.tm_sec >= 2 && current_time.tm_sec <= 4 && current_time.tm_year == 126 &&
                     current_time.tm_mon == 9 && current_time.tm_mday == 1 &&
                     (strcmp(stage, "alarm") || alarm_count == 1)
                 ? 0
                 : -RT_ERROR;
close_rtc:
    R_RTC_Close(&rtc_control);
    return result;
}
