/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test-can.c
 * @brief CAN0/XL2551 的内部回环与外部请求/应答测试。
 *
 * 内部回环不验证收发器；bus 需 500k 对端 ACK，并回复 ID 0x322。
 * ISR 只保存帧和计数，工作线程等待、校验并关闭控制器。
 */
#include "peripheral-test.h"

#define CAN_REQUEST_ID 0x321u
#define CAN_RESPONSE_ID 0x322u
#define CAN_PAYLOAD_BYTES 8u
#define CAN_LOOP_PATTERNS 8u
#define CAN_LOOP_TIMEOUT_MS 1000u
#define CAN_BUS_TIMEOUT_MS 10000u

static volatile unsigned can_rx_count;
static volatile unsigned can_tx_count;
static volatile unsigned can_error_count;
/* RX 回调先复制帧，再更新完成计数；工作线程见到计数后读取帧。 */
static can_frame_t received_frame;

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

int test_can(const char *stage)
{
    int use_loopback = strcmp(stage, "loop") == 0;
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
    if (!use_loopback && strcmp(stage, "bus"))
    {
        return -RT_EINVAL;
    }
    mailbox_config.mailbox_count = 4;
    mailbox_config.p_mailbox = mailboxes;
    mailbox_config.p_mailbox_mask = mailbox_masks;
    can_config.p_extend = &mailbox_config;
    can_config.p_bit_timing = &bit_timing;
    can_config.p_callback = can_event_callback;
    can_rx_count = 0;
    can_tx_count = 0;
    can_error_count = 0;
    if (R_CAN_Open(&g_can0_ctrl, &can_config))
    {
        return -RT_ERROR;
    }
    if (use_loopback && R_CAN_ModeTransition(&g_can0_ctrl,
                                             CAN_OPERATION_MODE_NORMAL,
                                             CAN_TEST_MODE_LOOPBACK_INTERNAL))
    {
        goto close_can;
    }
    rt_kprintf("CAN 500k %s: TX ID=321, expected RX ID=%s same 8 bytes\n",
               stage,
               use_loopback ? "321" : "322");
    for (unsigned frame_index = 0; frame_index < (use_loopback ? CAN_LOOP_PATTERNS : 1u);
         ++frame_index)
    {
        for (unsigned byte_index = 0; byte_index < CAN_PAYLOAD_BYTES; ++byte_index)
        {
            transmit_frame.data[byte_index] = (uint8_t)(0x10 * frame_index + byte_index);
        }
        can_rx_count = 0;
        can_tx_count = 0;
        if (R_CAN_Write(&g_can0_ctrl, 0, &transmit_frame))
        {
            goto close_can;
        }
        rt_tick_t start = rt_tick_get();
        while ((!can_rx_count || !can_tx_count) && !can_error_count && !test_cancelled() &&
               !test_elapsed(start, use_loopback ? CAN_LOOP_TIMEOUT_MS : CAN_BUS_TIMEOUT_MS))
        {
            rt_thread_mdelay(1);
        }
        if (!can_rx_count || !can_tx_count || can_error_count ||
            received_frame.id != (use_loopback ? CAN_REQUEST_ID : CAN_RESPONSE_ID) ||
            received_frame.id_mode != CAN_ID_MODE_STANDARD ||
            received_frame.type != CAN_FRAME_TYPE_DATA ||
            received_frame.data_length_code != CAN_PAYLOAD_BYTES ||
            memcmp(transmit_frame.data, received_frame.data, CAN_PAYLOAD_BYTES))
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
