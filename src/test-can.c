/* SPDX-License-Identifier: Apache-2.0 */
#include "peripheral-test.h"
static volatile unsigned can_rx, can_tx, can_errors;
static can_frame_t received;
static void callback(can_callback_args_t *a)
{
    if (a->event == CAN_EVENT_RX_COMPLETE) { received = a->frame; ++can_rx; }
    else if (a->event == CAN_EVENT_TX_COMPLETE) ++can_tx;
    else ++can_errors;
}
int test_can(const char *stage)
{
    int loop = !strcmp(stage,"loop"), result = -RT_ERROR;
    can_cfg_t cfg = g_can0_cfg;
    can_extended_cfg_t ext = *(const can_extended_cfg_t *)cfg.p_extend;
    can_mailbox_t boxes[4] = {
        { .mailbox_id=0x321, .id_mode=CAN_ID_MODE_STANDARD, .mailbox_type=CAN_MAILBOX_TRANSMIT, .frame_type=CAN_FRAME_TYPE_DATA },
        { .mailbox_id=0x321, .id_mode=CAN_ID_MODE_STANDARD, .mailbox_type=CAN_MAILBOX_RECEIVE, .frame_type=CAN_FRAME_TYPE_DATA },
        { .mailbox_id=0x322, .id_mode=CAN_ID_MODE_STANDARD, .mailbox_type=CAN_MAILBOX_RECEIVE, .frame_type=CAN_FRAME_TYPE_DATA },
        { .mailbox_id=0x323, .id_mode=CAN_ID_MODE_STANDARD, .mailbox_type=CAN_MAILBOX_RECEIVE, .frame_type=CAN_FRAME_TYPE_DATA }
    };
    uint32_t masks[1] = {0x1fffffff};
    /* CANMCLK = 24 MHz, 4 * (1+9+2) = 48 clocks/bit => 500 kbit/s. */
    can_bit_timing_cfg_t timing = {4,9,2,1};
    can_frame_t sent = {.id=0x321,.id_mode=CAN_ID_MODE_STANDARD,.type=CAN_FRAME_TYPE_DATA,.data_length_code=8};
    if (!loop && strcmp(stage,"bus")) return -RT_EINVAL;
    ext.mailbox_count=4; ext.p_mailbox=boxes; ext.p_mailbox_mask=masks;
    cfg.p_extend=&ext; cfg.p_bit_timing=&timing; cfg.p_callback=callback;
    can_rx=can_tx=can_errors=0;
    if (R_CAN_Open(&g_can0_ctrl,&cfg)) return -RT_ERROR;
    if (loop && R_CAN_ModeTransition(&g_can0_ctrl,CAN_OPERATION_MODE_NORMAL,CAN_TEST_MODE_LOOPBACK_INTERNAL)) goto done;
    rt_kprintf("CAN 500k %s: TX ID=321, expected RX ID=%s same 8 bytes\n",stage,loop?"321":"322");
    for (unsigned n=0;n<(loop?8u:1u);++n) {
        for (unsigned i=0;i<8;++i) sent.data[i]=(uint8_t)(0x10*n+i);
        can_rx=can_tx=0;
        if (R_CAN_Write(&g_can0_ctrl,0,&sent)) goto done;
        rt_tick_t start=rt_tick_get();
        while ((!can_rx || !can_tx) && !can_errors && !test_cancelled() && !test_elapsed(start,loop?1000:10000)) rt_thread_mdelay(1);
        if (!can_rx || !can_tx || can_errors || received.id != (loop?0x321u:0x322u) ||
            received.id_mode != CAN_ID_MODE_STANDARD || received.type != CAN_FRAME_TYPE_DATA ||
            received.data_length_code != 8 || memcmp(sent.data,received.data,8)) goto done;
    }
    result=0;
done:
    rt_kprintf("CAN tx=%u rx=%u errors=%u\n",can_tx,can_rx,can_errors);
    R_CAN_Close(&g_can0_ctrl);
    return result;
}
