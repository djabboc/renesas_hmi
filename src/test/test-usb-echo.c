/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test-usb-echo.c
 * @brief 验证系统 USB CDC 的二进制回显。
 *
 * Con4 连接电脑；枚举后在新串口回显 30 秒，由主机逐字节比对。
 * 阅读顺序：文件末尾线程入口 → run_test → 本文件的硬件辅助函数。
 * 只依赖 RT-Thread、FSP 及所用库，不调用其他测试文件。
 */
#include <rtthread.h>
#include <rtdevice.h>
#include "hal_data.h"
#include <string.h>
#include "tusb.h"
#include "device/dcd.h"

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

/* TinyUSB 回调由平台适配层转发；注册的函数和描述符均属于本文件。 */
extern void hmi_usb_set_callbacks(uint8_t const *(*device)(void),
                                  uint8_t const *(*configuration)(uint8_t),
                                  uint16_t const *(*string)(uint8_t, uint16_t),
                                  void (*mounted)(void),
                                  void (*sent)(uint8_t));

extern void hmi_usb_set_observers(void (*irq)(void), void (*event)(uint32_t));

#define USB_ROOT_PORT 0u
#define USB_MOUNT_TIMEOUT_MS 15000u
#define USB_ECHO_WINDOW_MS 30000u

static volatile unsigned mount_count;
static volatile unsigned tx_completion_count;
static volatile unsigned irq_count;
static volatile unsigned reset_count;
static volatile unsigned setup_count;
static unsigned device_request_count;
static unsigned configuration_request_count;

/* 中断/事件回调只计数；串口输出放在线程内，避免拖慢枚举时序。 */
static void app_usb_irq(void)
{
    ++irq_count;
}

static void app_usb_event(uint32_t event)
{
    if (event == DCD_EVENT_BUS_RESET)
    {
        ++reset_count;
    }
    else if (event == DCD_EVENT_SETUP_RECEIVED)
    {
        ++setup_count;
    }
}

/* 在停模块时钟前读取寄存器。GPIO 高电平与 USB 控制器的 VBSTS 分开记录。
 * DPRPU=1 表示已打开 D+ 上拉；reset/setup 用于确认主机是否开始枚举。 */
static void print_usb_diagnostics(const char *phase)
{
    rt_kprintf("USB %s: VBUS_pin=%u VBSTS=%u DPRPU=%u UCK=%02X SYSCFG=%04X\n",
               phase,
               (unsigned)rt_pin_read(BSP_IO_PORT_04_PIN_07),
               (unsigned)R_USB_FS0->INTSTS0_b.VBSTS,
               (unsigned)R_USB_FS0->SYSCFG_b.DPRPU,
               (unsigned)R_SYSTEM->SCKDIVCR2,
               (unsigned)R_USB_FS0->SYSCFG);
    rt_kprintf("USB %s: SYSSTS0=%04X INTSTS0=%04X INTENB0=%04X USBADDR=%04X NVIC=%u\n",
               phase,
               (unsigned)R_USB_FS0->SYSSTS0,
               (unsigned)R_USB_FS0->INTSTS0,
               (unsigned)R_USB_FS0->INTENB0,
               (unsigned)R_USB_FS0->USBADDR,
               (unsigned)NVIC_GetEnableIRQ(USBFS_INT_IRQn));
    rt_kprintf("USB %s: irq=%u reset=%u setup=%u desc_device=%u desc_config=%u\n",
               phase, irq_count, reset_count, setup_count,
               device_request_count, configuration_request_count);
}

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
/* 返回本文件的 USB 设备描述符，生命周期覆盖枚举全过程。 */
static uint8_t const *app_tud_descriptor_device_cb(void)
{
    ++device_request_count;
    return (const uint8_t *)&device_descriptor;
}

/* 返回本文件的 CDC 接口和端点配置描述符。 */
static uint8_t const *app_tud_descriptor_configuration_cb(uint8_t index)
{
    RT_UNUSED(index);
    ++configuration_request_count;
    return config_descriptor;
}

/* 将产品字符串编码为 USB UTF-16LE 描述符，并检查索引范围。 */
static uint16_t const *app_tud_descriptor_string_cb(uint8_t index, uint16_t language)
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

/* 主机完成 SET_CONFIGURATION 后计数；VBUS 有电不等于枚举完成。 */
static void app_tud_mount_cb(void)
{
    ++mount_count;
}

/* 统计 USB 发送完成回调；主机仍需核对回显字节内容。 */
static void app_tud_cdc_tx_complete_cb(uint8_t instance)
{
    RT_UNUSED(instance);
    ++tx_completion_count;
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

/* 按本文件配置打开外设，执行验证；所有退出路径都在返回前清理本次资源。 */
static int run_test(void)
{
    unsigned received_bytes = 0;
    unsigned queued_bytes = 0;
    int result = TEST_SKIP;

    if (R_IOPORT_PinCfg(&g_ioport_ctrl,
                        BSP_IO_PORT_04_PIN_07,
                        IOPORT_CFG_PERIPHERAL_PIN | IOPORT_PERIPHERAL_USB_FS) != FSP_SUCCESS)
    {
        return -RT_ERROR;
    }
    rt_kprintf("USB system connector VBUS=%u; debug USB/COM8 is separate\n",
               (unsigned)rt_pin_read(BSP_IO_PORT_04_PIN_07));
    R_BSP_RegisterProtectDisable(BSP_REG_PROTECT_OM_LPC_BATT);
    R_BSP_MODULE_START(FSP_IP_USBFS, 0);
    R_BSP_RegisterProtectEnable(BSP_REG_PROTECT_OM_LPC_BATT);
    R_BSP_IrqCfg(USBFS_INT_IRQn, 12, NULL);
    irq_count = 0;
    reset_count = 0;
    setup_count = 0;
    device_request_count = 0;
    configuration_request_count = 0;
    hmi_usb_set_observers(app_usb_irq, app_usb_event);
    mount_count = 0;
    tx_completion_count = 0;
    hmi_usb_set_callbacks(app_tud_descriptor_device_cb,
                          app_tud_descriptor_configuration_cb,
                          app_tud_descriptor_string_cb,
                          app_tud_mount_cb,
                          app_tud_cdc_tx_complete_cb);
    if (!tud_init(USB_ROOT_PORT))
    {
        result = -RT_ERROR;
        goto close_usb;
    }
    print_usb_diagnostics("start");
    rt_kprintf("USB waiting up to 15 seconds for host enumeration\n");
    result = wait_for_usb_mount();
    print_usb_diagnostics("enumeration");
    if (result == TEST_SKIP && !test_cancelled())
    {
        rt_kprintf("USB enumeration incomplete; capture diagnostics above (SKIP is not PASS)\n");
    }
    if (result == TEST_PASS)
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
    hmi_usb_set_callbacks(NULL, NULL, NULL, NULL, NULL);
    hmi_usb_set_observers(NULL, NULL);
    test_restore_pin(BSP_IO_PORT_04_PIN_07);
    return result;
}

/* 本文件唯一线程入口：独立完成初始化、验证和清理，然后自然返回。
 * error 保存最终结果，便于调用方在 RT-Thread 线程回收时读取。 */
void test_usb_echo_thread(void *argument)
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
