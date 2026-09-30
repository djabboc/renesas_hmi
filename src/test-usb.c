/* SPDX-License-Identifier: Apache-2.0 */
#include "peripheral-test.h"
#include "tusb.h"
#include "device/dcd.h"
static volatile unsigned mounts,completions;
static const tusb_desc_device_t device_descriptor={
    .bLength=sizeof(tusb_desc_device_t),.bDescriptorType=TUSB_DESC_DEVICE,.bcdUSB=0x0200,
    .bDeviceClass=TUSB_CLASS_MISC,.bDeviceSubClass=MISC_SUBCLASS_COMMON,.bDeviceProtocol=MISC_PROTOCOL_IAD,
    .bMaxPacketSize0=64,.idVendor=0xcafe,.idProduct=0x4001,.bcdDevice=0x0100,
    .iManufacturer=1,.iProduct=2,.iSerialNumber=3,.bNumConfigurations=1
};
static const uint8_t config_descriptor[]={
    TUD_CONFIG_DESCRIPTOR(1,2,0,TUD_CONFIG_DESC_LEN+TUD_CDC_DESC_LEN,0,100),
    TUD_CDC_DESCRIPTOR(0,4,0x81,8,0x02,0x82,64)
};
uint8_t const *tud_descriptor_device_cb(void) { return (const uint8_t *)&device_descriptor; }
uint8_t const *tud_descriptor_configuration_cb(uint8_t index) { RT_UNUSED(index);return config_descriptor; }
uint16_t const *tud_descriptor_string_cb(uint8_t index,uint16_t language)
{
    static uint16_t string[32];
    static const char *const strings[]={"","HMI test","HMI USB CDC Echo","HMI-RA6M3-TEST","Test serial"};
    unsigned length;
    RT_UNUSED(language);
    if(index==0) { string[0]=(TUSB_DESC_STRING<<8)|4;string[1]=0x0409;return string; }
    if(index>=sizeof(strings)/sizeof(strings[0])) return NULL;
    length=strlen(strings[index]);if(length>31) length=31;
    for(unsigned i=0;i<length;++i) string[i+1]=(uint8_t)strings[index][i];
    string[0]=(uint16_t)((TUSB_DESC_STRING<<8)|(2*length+2));return string;
}
void tud_mount_cb(void) { ++mounts; }
void tud_cdc_tx_complete_cb(uint8_t instance) { RT_UNUSED(instance);++completions; }
void hmi_usb_isr(void)
{
    rt_interrupt_enter();
    dcd_int_handler(0);
    R_BSP_IrqStatusClear((IRQn_Type)40);
    rt_interrupt_leave();
}
int test_usb(const char *stage)
{
    uint8_t data[64];unsigned received=0,queued=0;
    int result=TEST_SKIP;
    if(strcmp(stage,"probe") && strcmp(stage,"echo")) return -RT_EINVAL;
    if(R_IOPORT_PinCfg(&g_ioport_ctrl,BSP_IO_PORT_04_PIN_07,
        IOPORT_CFG_PERIPHERAL_PIN|IOPORT_PERIPHERAL_USB_FS)) return -RT_ERROR;
    rt_kprintf("USB system connector VBUS=%u; debug USB/COM8 is separate\n",(unsigned)rt_pin_read(BSP_IO_PORT_04_PIN_07));
    R_BSP_RegisterProtectDisable(BSP_REG_PROTECT_OM_LPC_BATT);
    R_BSP_MODULE_START(FSP_IP_USBFS,0);
    R_BSP_RegisterProtectEnable(BSP_REG_PROTECT_OM_LPC_BATT);
    R_BSP_IrqCfg((IRQn_Type)40,12,NULL);
    mounts=completions=0;
    if(!tud_init(0)) { result=-RT_ERROR;goto done; }
    rt_tick_t start=rt_tick_get();
    while(!test_elapsed(start,5000) && !test_cancelled()) {
        tud_task_ext(0,false);
        if(tud_mounted()) { result=0;break; }
        rt_thread_mdelay(1);
    }
    if(result==0 && !strcmp(stage,"echo")) {
        start=rt_tick_get();result=TEST_WAIT;
        rt_kprintf("USB CDC configured, echo window 30s. Use the NEW COM port.\n");
        while(!test_elapsed(start,30000) && !test_cancelled()) {
            tud_task_ext(0,false);
            if(tud_cdc_available() && tud_cdc_write_available()>=sizeof(data)) {
                uint32_t count=tud_cdc_read(data,sizeof(data));received+=count;
                uint32_t written=tud_cdc_write(data,count);queued+=written;
                if(written!=count) { result=-RT_EIO;break; }
                tud_cdc_write_flush();
            }
            rt_thread_mdelay(1);
        }
    }
    rt_kprintf("USB mounted_events=%u rx=%u tx_queued=%u tx_complete_events=%u\n",mounts,received,queued,completions);
done:
    tud_deinit(0);NVIC_DisableIRQ((IRQn_Type)40);NVIC_ClearPendingIRQ((IRQn_Type)40);
    R_BSP_MODULE_STOP(FSP_IP_USBFS,0);
    test_restore_pin(BSP_IO_PORT_04_PIN_07);
    return result;
}
