/* SPDX-License-Identifier: Apache-2.0 */
#include "peripheral-test.h"
#include "r_rtc.h"
static rtc_instance_ctrl_t rtc;
static volatile unsigned alarms;
static void rtc_event(rtc_callback_args_t *args) { if(args->event==RTC_EVENT_ALARM_IRQ) ++alarms; }
int test_rtc(const char *stage)
{
    rtc_error_adjustment_cfg_t adjust={.adjustment_mode=RTC_ERROR_ADJUSTMENT_MODE_MANUAL,
        .adjustment_period=RTC_ERROR_ADJUSTMENT_PERIOD_NONE,.adjustment_type=RTC_ERROR_ADJUSTMENT_NONE};
    rtc_cfg_t cfg={.clock_source=RTC_CLOCK_SOURCE_SUBCLK,.p_err_cfg=&adjust,.alarm_irq=(IRQn_Type)41,
        .alarm_ipl=12,.periodic_irq=FSP_INVALID_VECTOR,.carry_irq=(IRQn_Type)42,.carry_ipl=12,.p_callback=rtc_event};
    rtc_time_t tm={.tm_year=126,.tm_mon=9,.tm_mday=1,.tm_hour=12,.tm_min=0,.tm_sec=0,.tm_wday=4},now;
    rtc_alarm_time_t alarm={.sec_match=true};
    int result=-RT_ERROR;
    if(strcmp(stage,"tick") && strcmp(stage,"alarm")) return -RT_EINVAL;
    R_BSP_RegisterProtectDisable(BSP_REG_PROTECT_CGC);
    if(R_SYSTEM->SOSCCR) { R_SYSTEM->SOMCR=0; R_SYSTEM->SOSCCR=0; }
    R_BSP_RegisterProtectEnable(BSP_REG_PROTECT_CGC);
    rt_thread_mdelay(2200);
    if(R_RTC_Open(&rtc,&cfg)) return -RT_ERROR;
    if(R_RTC_CalendarTimeSet(&rtc,&tm)) goto done;
    alarms=0;
    if(!strcmp(stage,"alarm")) { alarm.time=tm; alarm.time.tm_sec=2; if(R_RTC_CalendarAlarmSet(&rtc,&alarm)) goto done; }
    rt_thread_mdelay(3200);
    if(R_RTC_CalendarTimeGet(&rtc,&now)) goto done;
    rt_kprintf("RTC source=32.768k crystal date=%04d-%02d-%02d %02d:%02d:%02d alarm_count=%u\n",
        now.tm_year+1900,now.tm_mon+1,now.tm_mday,now.tm_hour,now.tm_min,now.tm_sec,alarms);
    result=now.tm_sec>=2 && now.tm_sec<=4 && now.tm_year==126 && now.tm_mon==9 && now.tm_mday==1 &&
        (strcmp(stage,"alarm") || alarms==1)?0:-RT_ERROR;
done:
    R_RTC_Close(&rtc);
    return result;
}
