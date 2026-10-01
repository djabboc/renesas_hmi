/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test-can-loop.c
 * @brief 验证 CAN0 内部回环收发。
 *
 * 无需对端；500 kbit/s，邮箱发送 0x321 并比对数据，不验证外部收发器。
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

#define CAN_REQUEST_ID 0x321u
#define CAN_RESPONSE_ID 0x322u
#define CAN_PAYLOAD_BYTES 8u
#define CAN_LOOP_PATTERNS 8u
#define CAN_LOOP_TIMEOUT_MS 1000u

static volatile unsigned can_rx_count;
static volatile unsigned can_tx_count;
static volatile unsigned can_error_count;
/* RX 回调先复制帧，再更新完成计数；工作线程见到计数后读取帧。 */
static can_frame_t received_frame;

/* CAN 中断复制收到的帧并记录事件，线程随后核对帧内容。 */
static void can_event_callback(can_callback_args_t *arguments)
{
    if (arguments->event == CAN_EVENT_RX_COMPLETE)
    {
        received_frame = arguments->frame;
        ++can_rx_count;
    }
    else if (arguments->event == CAN_EVENT_TX_COMPLETE)
    {
        ++can_tx_count;
    }
    else
    {
        ++can_error_count;
    }
}

/* 按本文件配置打开外设，执行验证；所有退出路径都在返回前清理本次资源。 */
static int run_test(void)
{

    int result = -RT_ERROR;
    can_cfg_t can_config = g_can0_cfg;
    can_extended_cfg_t mailbox_config = *(const can_extended_cfg_t *)can_config.p_extend;
    /* 邮箱 0 发送；邮箱 1 接收内部回环，邮箱 2 接收外部对端回复。 */
    can_mailbox_t mailboxes[4] = {{.mailbox_id = CAN_REQUEST_ID,
                                   .id_mode = CAN_ID_MODE_STANDARD,
                                   .mailbox_type = CAN_MAILBOX_TRANSMIT,
                                   .frame_type = CAN_FRAME_TYPE_DATA},
                                  {.mailbox_id = CAN_REQUEST_ID,
                                   .id_mode = CAN_ID_MODE_STANDARD,
                                   .mailbox_type = CAN_MAILBOX_RECEIVE,
                                   .frame_type = CAN_FRAME_TYPE_DATA},
                                  {.mailbox_id = CAN_RESPONSE_ID,
                                   .id_mode = CAN_ID_MODE_STANDARD,
                                   .mailbox_type = CAN_MAILBOX_RECEIVE,
                                   .frame_type = CAN_FRAME_TYPE_DATA},
                                  {.mailbox_id = 0x323,
                                   .id_mode = CAN_ID_MODE_STANDARD,
                                   .mailbox_type = CAN_MAILBOX_RECEIVE,
                                   .frame_type = CAN_FRAME_TYPE_DATA}};
    uint32_t mailbox_masks[1] = {0x1fffffff};
    /* CANMCLK = 24 MHz, 4 * (1+9+2) = 48 clocks/bit => 500 kbit/s. */
    can_bit_timing_cfg_t bit_timing = {
        .baud_rate_prescaler = 4,
        .time_segment_1 = 9,
        .time_segment_2 = 2,
        .synchronization_jump_width = 1,
    };
    can_frame_t transmit_frame = {.id = CAN_REQUEST_ID,
                                  .id_mode = CAN_ID_MODE_STANDARD,
                                  .type = CAN_FRAME_TYPE_DATA,
                                  .data_length_code = CAN_PAYLOAD_BYTES};

    mailbox_config.mailbox_count = 4;
    mailbox_config.p_mailbox = mailboxes;
    mailbox_config.p_mailbox_mask = mailbox_masks;
    can_config.p_extend = &mailbox_config;
    can_config.p_bit_timing = &bit_timing;
    can_config.p_callback = can_event_callback;
    can_rx_count = 0;
    can_tx_count = 0;
    can_error_count = 0;
    if (R_CAN_Open(&g_can0_ctrl, &can_config) != FSP_SUCCESS)
    {
        return -RT_ERROR;
    }
    if (R_CAN_ModeTransition(&g_can0_ctrl,
                             CAN_OPERATION_MODE_NORMAL,
                             CAN_TEST_MODE_LOOPBACK_INTERNAL) != FSP_SUCCESS)
    {
        goto close_can;
    }
    rt_kprintf("CAN 500k %s: TX ID=321, expected RX ID=%s same 8 bytes\n", "loop", "321");
    for (unsigned frame_index = 0; frame_index < (CAN_LOOP_PATTERNS); ++frame_index)
    {
        for (unsigned byte_index = 0; byte_index < CAN_PAYLOAD_BYTES; ++byte_index)
        {
            transmit_frame.data[byte_index] = (uint8_t)(0x10 * frame_index + byte_index);
        }
        can_rx_count = 0;
        can_tx_count = 0;
        if (R_CAN_Write(&g_can0_ctrl, 0, &transmit_frame) != FSP_SUCCESS)
        {
            goto close_can;
        }
        rt_tick_t start = rt_tick_get();
        while ((!can_rx_count || !can_tx_count) && !can_error_count && !test_cancelled() &&
               !test_elapsed(start, CAN_LOOP_TIMEOUT_MS))
        {
            rt_thread_mdelay(1);
        }
        if (!can_rx_count || !can_tx_count || can_error_count ||
            received_frame.id != (CAN_REQUEST_ID) ||
            received_frame.id_mode != CAN_ID_MODE_STANDARD ||
            received_frame.type != CAN_FRAME_TYPE_DATA ||
            received_frame.data_length_code != CAN_PAYLOAD_BYTES ||
            memcmp(transmit_frame.data, received_frame.data, CAN_PAYLOAD_BYTES) != 0)
        {
            goto close_can;
        }
    }
    result = 0;
close_can:
    rt_kprintf("CAN tx=%u rx=%u errors=%u\n", can_tx_count, can_rx_count, can_error_count);
    R_CAN_Close(&g_can0_ctrl);
    return result;
}

/* 本文件唯一线程入口：独立完成初始化、验证和清理，然后自然返回。
 * error 保存最终结果，便于调用方在 RT-Thread 线程回收时读取。 */
void test_can_loop_thread(void *argument)
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
