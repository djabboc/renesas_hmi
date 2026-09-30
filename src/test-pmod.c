/* SPDX-License-Identifier: Apache-2.0 */
#include "peripheral-test.h"
static volatile unsigned spi_done,spi_error;
static volatile unsigned irq_count;
static void pmod_irq_callback(external_irq_callback_args_t *a) { RT_UNUSED(a);++irq_count; }
static void spi_callback(spi_callback_args_t *a)
{ if(a->event==SPI_EVENT_TRANSFER_COMPLETE) spi_done=1;else spi_error=1; }
int test_pmod(const char *stage)
{
    if(!strcmp(stage,"irq0") || !strcmp(stage,"irq1")) {
        int first=!strcmp(stage,"irq0"),result=-RT_ERROR;
        const external_irq_instance_t *irq=first?&g_external_irq11:&g_external_irq10;
        bsp_io_port_pin_t input=first?BSP_IO_PORT_07_PIN_08:BSP_IO_PORT_07_PIN_09;
        bsp_io_port_pin_t output=first?BSP_IO_PORT_02_PIN_11:BSP_IO_PORT_07_PIN_10;
        external_irq_cfg_t cfg=*irq->p_cfg;
        cfg.trigger=EXTERNAL_IRQ_TRIG_BOTH_EDGE;cfg.p_callback=pmod_irq_callback;
        rt_pin_mode(output,PIN_MODE_OUTPUT);rt_pin_write(output,0);
        R_IOPORT_PinCfg(&g_ioport_ctrl,input,IOPORT_CFG_PORT_DIRECTION_INPUT|IOPORT_CFG_IRQ_ENABLE|IOPORT_CFG_PULLUP_ENABLE);
        if(R_ICU_ExternalIrqOpen(irq->p_ctrl,&cfg)) goto irq_pins;
        if(R_ICU_ExternalIrqEnable(irq->p_ctrl)) goto irq_close;
        rt_thread_mdelay(10);irq_count=0;
        for(unsigned i=0;i<16 && !test_cancelled();++i) { rt_pin_write(output,(i+1)&1);rt_thread_mdelay(10); }
        result=irq_count==16?0:-RT_ERROR;
        rt_kprintf("PMOD %s GPIO->IRQ edges=%u expected=16\n",stage,irq_count);
        R_ICU_ExternalIrqDisable(irq->p_ctrl);
irq_close:
        R_ICU_ExternalIrqClose(irq->p_ctrl);
irq_pins:
        test_restore_pin(output);test_restore_pin(input);return result;
    }
    if(!strcmp(stage,"i2c")) {
        struct rt_i2c_bus_device *bus=(struct rt_i2c_bus_device *)rt_device_find("i2c1");
        uint8_t byte;
        if(!bus) return -RT_ENOSYS;
        /* Explicit read at documented fixture address, never broad write scanning. */
        if(rt_i2c_master_recv(bus,0x50,0,&byte,1)!=1) { rt_kprintf("I2C no response at external fixture 0x50\n");return TEST_SKIP; }
        rt_kprintf("I2C external 0x50 byte=%02X; device content not checked\n",byte);return 0;
    }
    const spi_instance_t *spi;
    bsp_io_port_pin_t cs;
    if(!strcmp(stage,"spi0")) { spi=&g_sci_spi6;cs=BSP_IO_PORT_03_PIN_07; }
    else if(!strcmp(stage,"spi1")) { spi=&g_sci_spi7;cs=BSP_IO_PORT_06_PIN_11; }
    else if(!strcmp(stage,"arduino")) { spi=&g_sci_spi4;cs=BSP_IO_PORT_07_PIN_12; }
    else return -RT_EINVAL;
    spi_cfg_t cfg=*spi->p_cfg;
    sci_spi_extended_cfg_t ext=*(const sci_spi_extended_cfg_t *)cfg.p_extend;
    uint8_t tx[64] BSP_ALIGN_VARIABLE(4),rx[64] BSP_ALIGN_VARIABLE(4);
    int result=-RT_ERROR;
    cfg.p_callback=spi_callback;cfg.p_extend=&ext;
    if(R_SCI_SPI_CalculateBitrate(1000000,&ext.clk_div,false)) return -RT_ERROR;
    rt_pin_mode(cs,PIN_MODE_OUTPUT);rt_pin_write(cs,1);
    if(R_SCI_SPI_Open(spi->p_ctrl,&cfg)) goto pins;
    for(unsigned n=0;n<8;++n) {
        for(unsigned i=0;i<sizeof(tx);++i) tx[i]=(uint8_t)(i*17+n*29);
        memset(rx,0,sizeof(rx));spi_done=spi_error=0;rt_pin_write(cs,0);
        if(R_SCI_SPI_WriteRead(spi->p_ctrl,tx,rx,sizeof(tx),SPI_BIT_WIDTH_8_BITS)) goto done;
        rt_tick_t start=rt_tick_get();
        while(!spi_done && !spi_error && !test_elapsed(start,500) && !test_cancelled()) rt_thread_mdelay(1);
        rt_pin_write(cs,1);
        if(!spi_done || spi_error || memcmp(tx,rx,sizeof(tx))) goto done;
    }
    rt_kprintf("PMOD %s MOSI/MISO loop 8x64 bytes MATCH\n",stage);result=0;
done:
    R_SCI_SPI_Close(spi->p_ctrl);
pins:
    rt_pin_write(cs,1);test_restore_pin(cs);return result;
}
