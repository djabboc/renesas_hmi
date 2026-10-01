/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test-usb.c
 * @brief 系统 USBFS 的 CDC 枚举及有限时长二进制回显。
 *
 * 使用 Con4/P407 VBUS，与 ART-Link 调试串口独立。
 * TinyUSB 以无 OS 轮询模式运行；仅 ptest 线程调用 tud_task。
 */
#include "peripheral-test.h"
#include "tusb.h"
#include "device/dcd.h"

#define USB_ROOT_PORT 0u
#define USB_MOUNT_TIMEOUT_MS 5000u
#define USB_ECHO_WINDOW_MS 30000u

static volatile unsigned mount_count;
static volatile unsigned tx_completion_count;

/* CAFE:4001 仅为开发测试标识；CDC 使用两个接口和三个端点。 */
static const tusb_desc_device_t device_descriptor = {.bLength = sizeof(tusb_desc_device_t),
                                                     .bDescriptorType = TUSB_DESC_DEVICE,
                                                     .bcdUSB = 0x0200,
                                                     .bDeviceClass = TUSB_CLASS_MISC,
                                                     .bDeviceSubClass = MISC_SUBCLASS_COMMON,
                                                     .bDeviceProtocol = MISC_PROTOCOL_IAD,
                                                     .bMaxPacketSize0 = 64,
                                                     .idVendor = 0xcafe,
                                                     .idProduct = 0x4001,
                                                     .bcdDevice = 0x0100,
                                                     .iManufacturer = 1,
                                                     .iProduct = 2,
                                                     .iSerialNumber = 3,
                                                     .bNumConfigurations = 1};
static const uint8_t config_descriptor[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN, 0, 100),
    TUD_CDC_DESCRIPTOR(0, 4, 0x81, 8, 0x02, 0x82, 64)};
uint8_t const *tud_descriptor_device_cb(void)
{
    return (const uint8_t *)&device_descriptor;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
    RT_UNUSED(index);
    return config_descriptor;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t language)
{
    static uint16_t string_descriptor[32];
    static const char *const string_table[] = {
        "", "HMI test", "HMI USB CDC Echo", "HMI-RA6M3-TEST", "Test serial"};
    unsigned length;
    RT_UNUSED(language);
    /* 索引 0 返回语言 ID；其他字符串按 USB UTF-16LE 描述符编码。 */
    if (index == 0)
    {
        string_descriptor[0] = (TUSB_DESC_STRING << 8) | 4;
        string_descriptor[1] = 0x0409;
        return string_descriptor;
    }
    if (index >= sizeof(string_table) / sizeof(string_table[0]))
    {
        return NULL;
    }
    length = strlen(string_table[index]);
    if (length > 31)
    {
        length = 31;
    }
    for (unsigned character_index = 0; character_index < length; ++character_index)
    {
        string_descriptor[character_index + 1] = (uint8_t)string_table[index][character_index];
    }
    string_descriptor[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * length + 2));
    return string_descriptor;
}

void tud_mount_cb(void)
{
    ++mount_count;
}

void tud_cdc_tx_complete_cb(uint8_t instance)
{
    RT_UNUSED(instance);
    ++tx_completion_count;
}

void hmi_usb_isr(void)
{
    rt_interrupt_enter();
    dcd_int_handler(USB_ROOT_PORT);
    R_BSP_IrqStatusClear(USBFS_INT_IRQn);
    rt_interrupt_leave();
}

/* 仅等待主机完成配置；VBUS=1 不能替代枚举完成判定。 */
static int wait_for_usb_mount(void)
{
    rt_tick_t start = rt_tick_get();
    while (!test_elapsed(start, USB_MOUNT_TIMEOUT_MS) && !test_cancelled())
    {
        tud_task_ext(0, false);
        if (tud_mounted())
        {
            return TEST_PASS;
        }
        rt_thread_mdelay(1);
    }
    return TEST_SKIP;
}

/* 先确认 TX FIFO 有空间再读 RX，避免接收后无法完整排队发送。
 * 设备只核对排队字节数；主机必须进一步逐字节核对回显内容。
 */
static int run_usb_echo(unsigned *received_bytes, unsigned *queued_bytes)
{
    uint8_t echo_buffer[64];

    rt_tick_t start = rt_tick_get();
    int result = TEST_WAIT;
    rt_kprintf("USB CDC configured, echo window 30s. Use the NEW COM port.\n");
    while (!test_elapsed(start, USB_ECHO_WINDOW_MS) && !test_cancelled())
    {
        tud_task_ext(0, false);
        if (tud_cdc_available() && tud_cdc_write_available() >= sizeof(echo_buffer))
        {
            uint32_t read_count = tud_cdc_read(echo_buffer, sizeof(echo_buffer));
            *received_bytes += read_count;
            uint32_t written_count = tud_cdc_write(echo_buffer, read_count);
            *queued_bytes += written_count;
            if (written_count != read_count)
            {
                result = -RT_EIO;
                break;
            }
            tud_cdc_write_flush();
        }
        rt_thread_mdelay(1);
    }
    return result;
}

int test_usb(const char *stage)
{
    unsigned received_bytes = 0;
    unsigned queued_bytes = 0;
    int result = TEST_SKIP;
    if (strcmp(stage, "probe") && strcmp(stage, "echo"))
    {
        return -RT_EINVAL;
    }
    if (R_IOPORT_PinCfg(&g_ioport_ctrl,
                        BSP_IO_PORT_04_PIN_07,
                        IOPORT_CFG_PERIPHERAL_PIN | IOPORT_PERIPHERAL_USB_FS))
    {
        return -RT_ERROR;
    }
    rt_kprintf("USB system connector VBUS=%u; debug USB/COM8 is separate\n",
               (unsigned)rt_pin_read(BSP_IO_PORT_04_PIN_07));
    R_BSP_RegisterProtectDisable(BSP_REG_PROTECT_OM_LPC_BATT);
    R_BSP_MODULE_START(FSP_IP_USBFS, 0);
    R_BSP_RegisterProtectEnable(BSP_REG_PROTECT_OM_LPC_BATT);
    R_BSP_IrqCfg(USBFS_INT_IRQn, 12, NULL);
    mount_count = 0;
    tx_completion_count = 0;
    if (!tud_init(USB_ROOT_PORT))
    {
        result = -RT_ERROR;
        goto close_usb;
    }
    result = wait_for_usb_mount();
    if (result == TEST_PASS && strcmp(stage, "echo") == 0)
    {
        result = run_usb_echo(&received_bytes, &queued_bytes);
    }

    rt_kprintf("USB mounted_events=%u rx=%u tx_queued=%u tx_complete_events=%u\n",
               mount_count,
               received_bytes,
               queued_bytes,
               tx_completion_count);
close_usb:
    /* 先断开设备/停中断，再停模块时钟，最后恢复 VBUS 引脚。 */
    tud_deinit(USB_ROOT_PORT);
    NVIC_DisableIRQ(USBFS_INT_IRQn);
    NVIC_ClearPendingIRQ(USBFS_INT_IRQn);
    R_BSP_MODULE_STOP(FSP_IP_USBFS, 0);
    test_restore_pin(BSP_IO_PORT_04_PIN_07);
    return result;
}
