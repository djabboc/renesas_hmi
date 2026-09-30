/* SPDX-License-Identifier: Apache-2.0 */
#include "peripheral-test.h"
static volatile unsigned lcd_frames;
static void display_event(display_callback_args_t *a) { RT_UNUSED(a); ++lcd_frames; }
int test_lcd(const char *stage)
{
    static const uint16_t colors[]={0xf800,0x07e0,0x001f,0xffff,0};
    static display_cfg_t cfg;
    cfg=g_display0_cfg;
    uint16_t *pixels=(uint16_t *)fb_background[0];
    int result=-RT_ERROR,pwm=0;
    if(strcmp(stage,"colors") && strcmp(stage,"backlight")) return -RT_EINVAL;
    cfg.output.htiming.total_cyc=531;cfg.output.htiming.back_porch=43;cfg.output.htiming.sync_width=2;
    cfg.output.vtiming.total_cyc=292;cfg.output.vtiming.back_porch=12;cfg.output.vtiming.sync_width=2;
    cfg.p_callback=display_event;lcd_frames=0;
    rt_pin_mode(BSP_IO_PORT_01_PIN_05,PIN_MODE_OUTPUT);rt_pin_write(BSP_IO_PORT_01_PIN_05,0);
    rt_pin_mode(BSP_IO_PORT_01_PIN_00,PIN_MODE_OUTPUT);rt_pin_write(BSP_IO_PORT_01_PIN_00,0);
    memset(pixels,0,DISPLAY_BUFFER_STRIDE_BYTES_INPUT0*272);
    if(R_GLCDC_Open(&g_display0_ctrl,&cfg)) return -RT_ERROR;
    if(R_GLCDC_Start(&g_display0_ctrl)) goto done;
    rt_thread_mdelay(150);rt_pin_write(BSP_IO_PORT_01_PIN_05,1);rt_thread_mdelay(10);rt_pin_write(BSP_IO_PORT_01_PIN_00,1);
    for(unsigned n=0;n<5 && !test_cancelled();++n) {
        for(unsigned y=0;y<272;++y) for(unsigned x=0;x<480;++x) pixels[y*DISPLAY_BUFFER_STRIDE_PIXELS_INPUT0+x]=colors[n];
        rt_kprintf("LCD color=%04X\n",colors[n]);rt_thread_mdelay(350);
    }
    if(!strcmp(stage,"backlight")) {
        memset(pixels,0xff,DISPLAY_BUFFER_STRIDE_BYTES_INPUT0*272);
        test_restore_pin(BSP_IO_PORT_01_PIN_00);
        if(R_GPT_Open(&g_timer5_ctrl,&g_timer5_cfg)) goto done;
        pwm=1;if(R_GPT_Start(&g_timer5_ctrl)) goto done;
        for(unsigned n=0;n<=10 && !test_cancelled();++n) {
            unsigned pct=n<=5?n*20:(10-n)*20;
            if(R_GPT_DutyCycleSet(&g_timer5_ctrl,g_timer5_cfg.period_counts*pct/100,GPT_IO_PIN_GTIOCA)) goto done;
            rt_kprintf("LCD brightness=%u%%\n",pct);rt_thread_mdelay(250);
        }
    }
    rt_kprintf("LCD interrupts=%u; color/brightness require visual confirmation\n",lcd_frames);
    result=lcd_frames?TEST_WAIT:-RT_ERROR;
done:
    if(pwm) { R_GPT_Stop(&g_timer5_ctrl);R_GPT_Close(&g_timer5_ctrl); }
    rt_pin_mode(BSP_IO_PORT_01_PIN_00,PIN_MODE_OUTPUT);rt_pin_write(BSP_IO_PORT_01_PIN_00,0);
    rt_pin_write(BSP_IO_PORT_01_PIN_05,0);
    rt_tick_t start=rt_tick_get();fsp_err_t err;
    do { err=R_GLCDC_Stop(&g_display0_ctrl);if(!err) break;rt_thread_mdelay(1); } while(!test_elapsed(start,100));
    /* Stop requests take effect at a frame boundary. Do not leave a live
     * controller holding a dead configuration pointer after this function. */
    start=rt_tick_get();
    do { err=R_GLCDC_Close(&g_display0_ctrl);if(!err) break;rt_thread_mdelay(1); }
    while(!test_elapsed(start,100));
    if(err) { rt_kprintf("LCD close=%d; reset before further display tests\n",err);result=-RT_ERROR; }
    return result;
}
static struct rt_i2c_bus_device *touch_bus;
static uint16_t touch_addr;
static int touch_read(uint16_t reg,uint8_t *data,uint16_t length)
{
    uint8_t index[2]={(uint8_t)(reg>>8),(uint8_t)reg};
    struct rt_i2c_msg msgs[2]={{.addr=touch_addr,.flags=RT_I2C_WR,.buf=index,.len=2},
                             {.addr=touch_addr,.flags=RT_I2C_RD,.buf=data,.len=length}};
    return rt_i2c_transfer(touch_bus,msgs,2)==2?0:-RT_EIO;
}
int test_touch(const char *stage)
{
    uint8_t identity[11],status,points[40],ack[]={0x81,0x4e,0};unsigned frames=0;
    if(strcmp(stage,"info") && strcmp(stage,"points")) return -RT_EINVAL;
    touch_bus=(struct rt_i2c_bus_device *)rt_device_find("i2c1");if(!touch_bus) return -RT_ENOSYS;
    rt_pin_mode(BSP_IO_PORT_08_PIN_01,PIN_MODE_OUTPUT);rt_pin_write(BSP_IO_PORT_08_PIN_01,0);
    rt_pin_mode(BSP_IO_PORT_00_PIN_04,PIN_MODE_OUTPUT);rt_pin_write(BSP_IO_PORT_00_PIN_04,1);
    rt_thread_mdelay(10);rt_pin_write(BSP_IO_PORT_08_PIN_01,1);rt_thread_mdelay(100);
    rt_pin_mode(BSP_IO_PORT_00_PIN_04,PIN_MODE_INPUT);
    touch_addr=0x14;
    if(touch_read(0x8140,identity,sizeof(identity))) { touch_addr=0x5d;if(touch_read(0x8140,identity,sizeof(identity))) return -RT_EIO; }
    unsigned width=identity[6]|identity[7]<<8,height=identity[8]|identity[9]<<8;
    rt_kprintf("TOUCH addr=%02X id=%c%c%c%c range=%ux%u\n",touch_addr,identity[0],identity[1],identity[2],identity[3],width,height);
    if(memcmp(identity,"911",3) || width!=480 || height!=272) return -RT_ERROR;
    if(!strcmp(stage,"info")) return 0;
    rt_tick_t start=rt_tick_get();
    while(!test_elapsed(start,15000) && !test_cancelled()) {
        if(touch_read(0x814e,&status,1)) return -RT_EIO;
        if(status&0x80) {
            unsigned count=status&0x0f;if(count>5) return -RT_ERROR;
            if(count && touch_read(0x814f,points,count*8)) return -RT_EIO;
            if(rt_i2c_master_send(touch_bus,touch_addr,0,ack,3)!=3) return -RT_EIO;
            rt_kprintf("TOUCH N=%u",count);
            for(unsigned i=0;i<count;++i) {
                unsigned x=points[i*8+1]|points[i*8+2]<<8,y=points[i*8+3]|points[i*8+4]<<8;
                if(x>=480 || y>=272) return -RT_ERROR;
                rt_kprintf(" id=%u (%u,%u)",points[i*8],x,y);++frames;
            }
            rt_kprintf("\n");
        }
        rt_thread_mdelay(10);
    }
    rt_kprintf("TOUCH observed_points=%u; position/multitouch accuracy requires interaction\n",frames);
    return TEST_WAIT;
}
