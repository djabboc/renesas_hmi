/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test-adc-sample.c
 * @brief 报告 Arduino A0 的 ADC 原始采样值。
 *
 * A0/P000 对应 ADC0 通道 0；采样 32 次；悬空读数不能证明精度。
 * 阅读顺序：文件末尾线程入口 → run_test → 本文件的硬件辅助函数。
 * 只依赖 RT-Thread、FSP 及所用库，不调用其他测试文件。
 */
#include <rtthread.h>
#include <rtdevice.h>
#include "hal_data.h"
#include <string.h>
#include "r_adc.h"

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

/* 恢复 FSP 生成的复用配置；未配置的引脚退回高阻输入。 */
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

#define ADC_SAMPLE_COUNT 32u
#define ADC_FULL_SCALE 4095u
#define ADC_REFERENCE_MV 3300u
#define ADC_CONVERSION_TIMEOUT_MS 100u

/* 按本文件配置打开外设，执行验证；所有退出路径都在返回前清理本次资源。 */
static int run_test(void)
{
    adc_instance_ctrl_t adc_control = {0};
    adc_extended_cfg_t adc_extension = {.window_a_irq = FSP_INVALID_VECTOR,
                                        .window_b_irq = FSP_INVALID_VECTOR};
    adc_cfg_t adc_config = {.unit = 0,
                            .mode = ADC_MODE_SINGLE_SCAN,
                            .resolution = ADC_RESOLUTION_12_BIT,
                            .alignment = ADC_ALIGNMENT_RIGHT,
                            .trigger = ADC_TRIGGER_SOFTWARE,
                            .scan_end_irq = FSP_INVALID_VECTOR,
                            .scan_end_b_irq = FSP_INVALID_VECTOR,
                            .p_extend = &adc_extension};
    adc_channel_cfg_t channel_config = {.scan_mask = 1};
    uint16_t sample_value;
    uint16_t minimum = ADC_FULL_SCALE;
    uint16_t maximum = 0;
    unsigned sample_sum = 0;
    int result = -RT_ERROR;

    if (R_IOPORT_PinCfg(&g_ioport_ctrl, BSP_IO_PORT_00_PIN_00, IOPORT_CFG_ANALOG_ENABLE) !=
        FSP_SUCCESS)
    {
        return -RT_ERROR;
    }
    if (R_ADC_Open(&adc_control, &adc_config) != FSP_SUCCESS)
    {
        goto restore_pin;
    }
    if (R_ADC_ScanCfg(&adc_control, &channel_config) != FSP_SUCCESS)
    {
        goto close_adc;
    }
    for (unsigned sample_index = 0; sample_index < ADC_SAMPLE_COUNT; ++sample_index)
    {
        adc_status_t status;
        if (R_ADC_ScanStart(&adc_control) != FSP_SUCCESS)
        {
            goto close_adc;
        }
        rt_tick_t start = rt_tick_get();
        do
        {
            if (R_ADC_StatusGet(&adc_control, &status) != FSP_SUCCESS)
            {
                goto close_adc;
            }
            if (!status.state)
            {
                break;
            }
            rt_thread_mdelay(1);
        } while (!test_elapsed(start, ADC_CONVERSION_TIMEOUT_MS) && !test_cancelled());
        if (status.state || R_ADC_Read(&adc_control, ADC_CHANNEL_0, &sample_value) != FSP_SUCCESS)
        {
            goto close_adc;
        }
        if (sample_value < minimum)
        {
            minimum = sample_value;
        }
        if (sample_value > maximum)
        {
            maximum = sample_value;
        }
        sample_sum += sample_value;
    }
    rt_kprintf("ADC A0/P000 n=32 min=%u max=%u avg=%u approx_mV=%u (Vref assumed 3300mV)\n",
               minimum,
               maximum,
               sample_sum / ADC_SAMPLE_COUNT,
               (sample_sum / ADC_SAMPLE_COUNT) * ADC_REFERENCE_MV / ADC_FULL_SCALE);
    /* 端点允许 100 个 LSB 的板级误差；悬空 sample 不据此判定硬件通过。 */

    result = TEST_WAIT;

close_adc:
    R_ADC_Close(&adc_control);
restore_pin:
    test_restore_pin(BSP_IO_PORT_00_PIN_00);
    return result;
}

/* 本文件唯一线程入口：独立完成初始化、验证和清理，然后自然返回。
 * error 保存最终结果，便于调用方在 RT-Thread 线程回收时读取。 */
void test_adc_sample_thread(void *argument)
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
