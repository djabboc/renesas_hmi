/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test-adc.c
 * @brief Arduino A0/P000 的 ADC0 通道 0 测试。
 * sample 只报告读数；low/high 必须分别接 GND/3.3V 后才可判定。
 * 每次使用软件触发的 12 位单次转换，结束关闭 ADC 并恢复引脚复用。
 */
#include "peripheral-test.h"
#include "r_adc.h"

#define ADC_SAMPLE_COUNT 32u
#define ADC_FULL_SCALE 4095u
#define ADC_REFERENCE_MV 3300u
#define ADC_ENDPOINT_TOLERANCE 100u
#define ADC_CONVERSION_TIMEOUT_MS 100u

int test_adc(const char *stage)
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
    if (strcmp(stage, "sample") && strcmp(stage, "low") && strcmp(stage, "high"))
    {
        return -RT_EINVAL;
    }
    if (R_IOPORT_PinCfg(&g_ioport_ctrl, BSP_IO_PORT_00_PIN_00, IOPORT_CFG_ANALOG_ENABLE))
    {
        return -RT_ERROR;
    }
    if (R_ADC_Open(&adc_control, &adc_config))
    {
        goto restore_pin;
    }
    if (R_ADC_ScanCfg(&adc_control, &channel_config))
    {
        goto close_adc;
    }
    for (unsigned sample_index = 0; sample_index < ADC_SAMPLE_COUNT; ++sample_index)
    {
        adc_status_t status;
        if (R_ADC_ScanStart(&adc_control))
        {
            goto close_adc;
        }
        rt_tick_t start = rt_tick_get();
        do
        {
            if (R_ADC_StatusGet(&adc_control, &status))
            {
                goto close_adc;
            }
            if (!status.state)
            {
                break;
            }
            rt_thread_mdelay(1);
        } while (!test_elapsed(start, ADC_CONVERSION_TIMEOUT_MS) && !test_cancelled());
        if (status.state || R_ADC_Read(&adc_control, ADC_CHANNEL_0, &sample_value))
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
    if (strcmp(stage, "low") == 0)
    {
        result = maximum < ADC_ENDPOINT_TOLERANCE ? TEST_PASS : -RT_ERROR;
    }
    else if (strcmp(stage, "high") == 0)
    {
        result = minimum > ADC_FULL_SCALE - ADC_ENDPOINT_TOLERANCE ? TEST_PASS : -RT_ERROR;
    }
    else
    {
        result = TEST_WAIT;
    }
close_adc:
    R_ADC_Close(&adc_control);
restore_pin:
    test_restore_pin(BSP_IO_PORT_00_PIN_00);
    return result;
}
