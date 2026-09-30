/* SPDX-License-Identifier: Apache-2.0 */
#include "peripheral-test.h"
#include "r_adc.h"
static const bsp_io_port_pin_t leds[] = {BSP_IO_PORT_02_PIN_09,BSP_IO_PORT_02_PIN_10,BSP_IO_PORT_02_PIN_04};
static const bsp_io_port_pin_t keys[] = {BSP_IO_PORT_00_PIN_05,BSP_IO_PORT_00_PIN_06,BSP_IO_PORT_00_PIN_07};
int test_gpio(const char *stage)
{
    unsigned i;
    if (!strcmp(stage,"led")) {
        for (i=0;i<3;++i) {
            rt_kprintf("LED %u pin=P%u%02u active=%u\n",i,leds[i]>>8,leds[i]&255,i==2?1:0);
            rt_pin_mode(leds[i],PIN_MODE_OUTPUT);
            for (unsigned n=0;n<4 && !test_cancelled();++n) {
                rt_pin_write(leds[i],n&1); rt_thread_mdelay(180);
            }
            rt_pin_write(leds[i],i==2?0:1); test_restore_pin(leds[i]);
        }
        rt_kprintf("LED output sequence finished; visual confirmation required\n");
        return TEST_WAIT;
    }
    if (!strcmp(stage,"inputs") || !strcmp(stage,"keys")) {
        unsigned pressed[3]={0},released[3]={0};
        int stable[3],candidate[3]; unsigned consecutive[3]={0}; rt_tick_t down[3]={0};
        for (i=0;i<3;++i) { rt_pin_mode(keys[i],PIN_MODE_INPUT_PULLUP); stable[i]=candidate[i]=rt_pin_read(keys[i]); }
        rt_kprintf("KEY levels P005=%d P006=%d P007=%d (pressed=0)\n",stable[0],stable[1],stable[2]);
        if (!strcmp(stage,"inputs")) {
            for(i=0;i<3;++i) test_restore_pin(keys[i]);
            return TEST_WAIT;
        }
        rt_tick_t start=rt_tick_get();
        while (!test_elapsed(start,15000) && !test_cancelled()) {
            for (i=0;i<3;++i) {
                int value=rt_pin_read(keys[i]);
                if (value!=candidate[i]) { candidate[i]=value; consecutive[i]=0; }
                if (++consecutive[i] > 4) consecutive[i]=4;
                if (consecutive[i]==4 && stable[i]!=value) {
                    stable[i]=value;
                    if (!value) { ++pressed[i]; down[i]=rt_tick_get(); rt_kprintf("KEY %u DOWN\n",i); }
                    else { ++released[i]; rt_kprintf("KEY %u UP %s\n",i,test_elapsed(down[i],800)?"LONG":"SHORT"); }
                }
            }
            rt_thread_mdelay(5);
        }
        for(i=0;i<3;++i) {
            rt_kprintf("KEY %u press=%u release=%u\n",i,pressed[i],released[i]);
            test_restore_pin(keys[i]);
        }
        return pressed[0]&&pressed[1]&&pressed[2]&&released[0]&&released[1]&&released[2]?0:TEST_WAIT;
    }
    if (!strcmp(stage,"loop")) {
        /* Arduino D2=P008 -> D9=P009, through 1k resistor. */
        int result=0;
        rt_pin_mode(BSP_IO_PORT_00_PIN_09,PIN_MODE_INPUT_PULLUP);
        rt_pin_mode(BSP_IO_PORT_00_PIN_08,PIN_MODE_OUTPUT);
        for(i=0;i<16;++i) {
            rt_pin_write(BSP_IO_PORT_00_PIN_08,i&1);rt_thread_mdelay(2);
            if (rt_pin_read(BSP_IO_PORT_00_PIN_09)!=(int)(i&1)) result=-RT_ERROR;
        }
        test_restore_pin(BSP_IO_PORT_00_PIN_08);test_restore_pin(BSP_IO_PORT_00_PIN_09);
        return result;
    }
    return -RT_EINVAL;
}
int test_adc(const char *stage)
{
    adc_instance_ctrl_t ctrl={0};
    adc_extended_cfg_t ext={.window_a_irq=FSP_INVALID_VECTOR,.window_b_irq=FSP_INVALID_VECTOR};
    adc_cfg_t cfg={.unit=0,.mode=ADC_MODE_SINGLE_SCAN,.resolution=ADC_RESOLUTION_12_BIT,
        .alignment=ADC_ALIGNMENT_RIGHT,.trigger=ADC_TRIGGER_SOFTWARE,.scan_end_irq=FSP_INVALID_VECTOR,
        .scan_end_b_irq=FSP_INVALID_VECTOR,.p_extend=&ext};
    adc_channel_cfg_t channels={.scan_mask=1};
    uint16_t value,min=4095,max=0; unsigned sum=0;
    int result=-RT_ERROR;
    if(strcmp(stage,"sample") && strcmp(stage,"low") && strcmp(stage,"high")) return -RT_EINVAL;
    if(R_IOPORT_PinCfg(&g_ioport_ctrl,BSP_IO_PORT_00_PIN_00,IOPORT_CFG_ANALOG_ENABLE)) return -RT_ERROR;
    if(R_ADC_Open(&ctrl,&cfg)) goto restore;
    if(R_ADC_ScanCfg(&ctrl,&channels)) goto done;
    for(unsigned n=0;n<32;++n) {
        adc_status_t status;
        if(R_ADC_ScanStart(&ctrl)) goto done;
        rt_tick_t start=rt_tick_get();
        do { if(R_ADC_StatusGet(&ctrl,&status)) goto done; if(!status.state) break;rt_thread_mdelay(1); }
        while(!test_elapsed(start,100) && !test_cancelled());
        if(status.state || R_ADC_Read(&ctrl,ADC_CHANNEL_0,&value)) goto done;
        if(value<min) min=value;
        if(value>max) max=value;
        sum+=value;
    }
    rt_kprintf("ADC A0/P000 n=32 min=%u max=%u avg=%u approx_mV=%u (Vref assumed 3300mV)\n",min,max,sum/32,(sum/32)*3300/4095);
    result=!strcmp(stage,"low")?(max<100?0:-RT_ERROR):!strcmp(stage,"high")?(min>3995?0:-RT_ERROR):TEST_WAIT;
done:
    R_ADC_Close(&ctrl);
restore:
    test_restore_pin(BSP_IO_PORT_00_PIN_00);
    return result;
}
