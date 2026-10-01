/* SPDX-License-Identifier: Apache-2.0 */
/* TinyUSB 的固定全局回调符号只能定义一次。本层仅转发给当前注册的应用，
 * 不保存测试结果，不创建线程；调用者停 USB 中断后才能注销回调。 */
#include <rtthread.h>
#include "hal_data.h"
#include "tusb.h"
#include "device/dcd.h"
static uint8_t const *(*device_callback)(void);
static uint8_t const *(*configuration_callback)(uint8_t);
static uint16_t const *(*string_callback)(uint8_t, uint16_t);
static void (*mount_callback)(void);
static void (*sent_callback)(uint8_t);
void hmi_usb_set_callbacks(uint8_t const *(*device)(void),
                           uint8_t const *(*configuration)(uint8_t),
                           uint16_t const *(*string)(uint8_t, uint16_t),
                           void (*mounted)(void),
                           void (*sent)(uint8_t))
{
    device_callback = device;
    configuration_callback = configuration;
    string_callback = string;
    mount_callback = mounted;
    sent_callback = sent;
}
uint8_t const *tud_descriptor_device_cb(void)
{
    if (device_callback)
    {
        return device_callback();
    }
    return NULL;
}
uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    if (configuration_callback)
    {
        return configuration_callback(index);
    }
    return NULL;
}
uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t language)
{
    if (string_callback)
    {
        return string_callback(index, language);
    }
    return NULL;
}
void tud_mount_cb(void)
{
    if (mount_callback)
    {
        mount_callback();
    }
}
void tud_cdc_tx_complete_cb(uint8_t instance)
{
    if (sent_callback)
    {
        sent_callback(instance);
    }
}
void hmi_usb_isr(void)
{
    rt_interrupt_enter();
    dcd_int_handler(0);
    R_BSP_IrqStatusClear(USBFS_INT_IRQn);
    rt_interrupt_leave();
}