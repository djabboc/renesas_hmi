/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @brief SWD临时采集：官方SSI配置，CPU直接读取FIFO，预热3秒、保存5秒。
 *
 * 本程序在SRAM执行，不运行RT-Thread，不使用DTC、队列、滤波或串口。
 * 保存数据会覆盖原应用RAM；主机必须在退出时复位回原Flash固件。
 * 两段PCM缓冲避开当前MSP栈，代码和报告放在更高地址。
 */
#include "hal_data.h"
#include <stdint.h>
#include <limits.h>

#define CPU_CLOCK_HZ 120000000u
#define WARMUP_FRAMES 144231u
#define RECORD_FRAMES 240384u
#define FIRST_BUFFER_FRAMES 204800u
#define FIRST_BUFFER_ADDRESS 0x20000000u
#define SECOND_BUFFER_ADDRESS 0x20065000u
#define REPORT_MAGIC 0x4649464Fu
#define REPORT_DONE 0x444F4E45u

/* 默认沿用官方极性；主机只有显式选择clock-invert时才编译另一组。 */
#ifndef FIFO_INVERT_BCKP
#define FIFO_INVERT_BCKP 0
#endif

/* 主机读取这个固定格式报告，只有完整采集、无错误时才生成音频。 */
volatile uint32_t fifo_report[24] = {REPORT_MAGIC};

/* 中断保持屏蔽；满足FSP回调参数要求，不在这里处理音频。 */
static void unused_callback(i2s_callback_args_t *arguments)
{
    (void)arguments;
}

/* 引脚复用沿用官方生成表，不在诊断中尝试其他时钟或引脚功能。 */
static fsp_err_t restore_microphone_pin(bsp_io_port_pin_t pin)
{
    for (unsigned index = 0; index < g_bsp_pin_cfg.number_of_pins; ++index)
    {
        if (g_bsp_pin_cfg.p_pin_cfg_data[index].pin == pin)
        {
            return R_IOPORT_PinCfg(&g_ioport_ctrl, pin,
                                  g_bsp_pin_cfg.p_pin_cfg_data[index].pin_cfg);
        }
    }
    return FSP_ERR_INVALID_ARGUMENT;
}

/* FIFO中的24位样本右对齐；不能把最高8位当作符号位。 */
static int32_t signed_pcm24(uint32_t word)
{
    int32_t value = (int32_t)(word & 0x00FFFFFFu);
    if ((word & 0x00800000u) != 0)
    {
        value -= 0x01000000;
    }
    return value;
}

void capture_fifo(void)
{
    timer_cfg_t timer_config = g_timer_cfg;
    i2s_cfg_t microphone_config = g_i2s0_cfg;
    uint32_t unused_receive_buffer[2];
    int32_t minimum = INT32_MAX;
    int32_t maximum = INT32_MIN;
    uint32_t right_peak = 0;
    unsigned frame = 0;
    unsigned saved = 0;
    int timer_opened = 0;
    int ssi_opened = 0;

    __disable_irq();
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
    fifo_report[1] = 1;
    fifo_report[2] = restore_microphone_pin(BSP_IO_PORT_04_PIN_03);
    fifo_report[3] = restore_microphone_pin(BSP_IO_PORT_04_PIN_04);
    fifo_report[4] = restore_microphone_pin(BSP_IO_PORT_04_PIN_06);
    if (fifo_report[2] != FSP_SUCCESS || fifo_report[3] != FSP_SUCCESS ||
        fifo_report[4] != FSP_SUCCESS)
    {
        goto finish;
    }

    /* 时钟、PCM24、32位时隙和WS设置先沿用官方配置。
     * 取消DTC接收子驱动；CPU随后直接排空FIFO。 */
    microphone_config.p_transfer_rx = NULL;
    microphone_config.p_callback = unused_callback;
    fifo_report[5] = R_GPT_Open(&g_timer_ctrl, &timer_config);
    if (fifo_report[5] != FSP_SUCCESS)
    {
        goto finish;
    }
    timer_opened = 1;
    fifo_report[6] = R_GPT_Start(&g_timer_ctrl);
    if (fifo_report[6] != FSP_SUCCESS)
    {
        goto finish;
    }
    fifo_report[7] = R_SSI_Open(&g_i2s0_ctrl, &microphone_config);
    if (fifo_report[7] != FSP_SUCCESS)
    {
        goto finish;
    }
    ssi_opened = 1;

    /* Open已经配置SSI，但还没有启用接收/发送。
     * 手册禁止在IIRQ=0的通信状态修改BCKP，必须先确认空闲。
     * 对照组仅翻转位时钟极性，频率和数据格式保持一致。
     * 报告保存修改前值及改动掩码，主机必须核对实际寄存器。 */
    fifo_report[21] = R_SSI0->SSICR;
    if ((fifo_report[21] & 3u) != 0 || R_SSI0->SSISR_b.IIRQ == 0)
    {
        fifo_report[8] = FSP_ERR_IN_USE;
        goto finish;
    }
#if FIFO_INVERT_BCKP
    R_SSI0->SSICR = fifo_report[21] ^ R_SSI0_SSICR_BCKP_Msk;
    fifo_report[22] = R_SSI0_SSICR_BCKP_Msk;
#else
    fifo_report[22] = 0;
#endif
    fifo_report[8] = R_SSI_Read(&g_i2s0_ctrl, unused_receive_buffer,
                              sizeof(unused_receive_buffer));
    if (fifo_report[8] != FSP_SUCCESS)
    {
        goto finish;
    }

    fifo_report[9] = R_SSI0->SSICR;
    fifo_report[10] = R_SSI0->SSIOFR;
    fifo_report[11] = DWT->CYCCNT;
    fifo_report[1] = 2;
    /* 从现在起不调用依赖原应用RAM的驱动或内核。
     * 每次成对读出左右字，保存前先丢弃3秒启动段。
     * PCM24固定除以256转PCM16，全速保存，不滤波、不降采样。 */
    while (frame < WARMUP_FRAMES + RECORD_FRAMES)
    {
        if (R_SSI0->SSISR_b.ROIRQ != 0)
        {
            fifo_report[18] = 1;
            break;
        }
        if ((uint32_t)(DWT->CYCCNT - fifo_report[11]) > CPU_CLOCK_HZ * 10u)
        {
            fifo_report[18] = 2;
            break;
        }
        if (R_SSI0->SSIFSR_b.RDC < 2)
        {
            continue;
        }
        int32_t left = signed_pcm24(R_SSI0->SSIFRDR);
        int32_t right = signed_pcm24(R_SSI0->SSIFRDR);
        if (frame == WARMUP_FRAMES)
        {
            fifo_report[12] = DWT->CYCCNT;
        }
        if (frame >= WARMUP_FRAMES)
        {
            volatile int16_t *destination;
            if (saved < FIRST_BUFFER_FRAMES)
            {
                destination = (volatile int16_t *)FIRST_BUFFER_ADDRESS;
                destination[saved] = (int16_t)(left / 256);
            }
            else
            {
                destination = (volatile int16_t *)SECOND_BUFFER_ADDRESS;
                destination[saved - FIRST_BUFFER_FRAMES] = (int16_t)(left / 256);
            }
            if (left < minimum)
            {
                minimum = left;
            }
            if (left > maximum)
            {
                maximum = left;
            }
            if (right < 0)
            {
                right = -right;
            }
            if ((uint32_t)right > right_peak)
            {
                right_peak = (uint32_t)right;
            }
            ++saved;
        }
        ++frame;
    }
    fifo_report[13] = DWT->CYCCNT;
    fifo_report[14] = frame;
    fifo_report[15] = saved;
    fifo_report[16] = (uint32_t)minimum;
    fifo_report[17] = (uint32_t)maximum;
    fifo_report[19] = right_peak;
    fifo_report[20] = R_SSI0->SSISR;
    fifo_report[1] = 3;

finish:
    /* 原应用的控制对象可能已被录音覆盖，因此直接停硬件。
     * 主机随后复位，完整重建原应用和所有驱动对象。 */
    if (ssi_opened)
    {
        R_SSI0->SSICR = 0;
        R_SSI0->SSIFCR = 0;
    }
    if (timer_opened)
    {
        R_GPT1->GTCR_b.CST = 0;
    }
    fifo_report[23] = REPORT_DONE;
    __BKPT(0);
    for (;;)
    {
        __NOP();
    }
}
