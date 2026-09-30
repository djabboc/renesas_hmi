/* SPDX-License-Identifier: Apache-2.0 */
#include "peripheral-test.h"
#include "dave_driver.h"
#include "dave_base.h"
#include "test-jpeg-data.h"
void *d1_malloc(size_t size) { return rt_malloc(size); }
void d1_free(void *ptr) { rt_free(ptr); }
static uint16_t bitmap[16*16] BSP_ALIGN_VARIABLE(8);
static uint8_t jpeg_input[4096] BSP_ALIGN_VARIABLE(8);
static volatile jpeg_status_t jpeg_events;
static int g2d_fault;
static void jpeg_event(jpeg_callback_args_t *a) { jpeg_events |= a->status; }
int test_graphics(const char *stage)
{
    int result=-RT_ERROR;
    if(!strcmp(stage,"g2d")) {
        if(g2d_fault) { rt_kprintf("G2D previous hardware timeout: reset required\n");return -RT_ERROR; }
        d2_device *device=d2_opendevice(0);d2_renderbuffer *buffer=NULL;
        if(!device) return -RT_ENOMEM;
        if(d2_inithw(device,0)!=D2_OK) { d2_closedevice(device);return -RT_ERROR; }
        buffer=d2_newrenderbuffer(device,20,20);
        if(!buffer) goto g2d_done;
        memset(bitmap,0,sizeof(bitmap));
        if(d2_selectrenderbuffer(device,buffer)!=D2_OK ||
           d2_framebuffer(device,bitmap,16,16,16,d2_mode_rgb565)!=D2_OK ||
           d2_cliprect(device,0,0,15,15)!=D2_OK ||
           d2_clear(device,0xff0000)!=D2_OK || d2_setcolor(device,0,0x00ff00)!=D2_OK ||
           d2_setalpha(device,255)!=D2_OK || d2_renderbox(device,4*16,4*16,8*16,8*16)!=D2_OK ||
           d2_executerenderbuffer(device,buffer,0)!=D2_OK) goto g2d_done;
        /* D/AVE flush has an unbounded vendor wait. Check its status first:
         * bits 0/1 are rendering/writeback busy, bit 3 is display-list active. */
        rt_tick_t start=rt_tick_get();
        while((uint32_t)d1_getregister(d2_level1interface(device),D1_DAVE2D,0)&0x0b) {
            if(test_elapsed(start,1000) || test_cancelled()) {
                /* Keep DMA-referenced allocations alive on a hardware fault. */
                g2d_fault=1;rt_kprintf("G2D timeout: allocations retained, reset required\n");return -RT_ETIMEOUT;
            }
            rt_thread_mdelay(1);
        }
        d2_flushframe(device);
        result=0;
        for(unsigned y=0;y<16;++y) for(unsigned x=0;x<16;++x) {
            uint16_t expected=(x>=4 && x<12 && y>=4 && y<12)?0x07e0:0xf800;
            if(bitmap[y*16+x]!=expected) result=-RT_ERROR;
        }
        rt_kprintf("G2D hardware red clear + green rectangle: corners=%04X center=%04X\n",bitmap[0],bitmap[8*16+8]);
g2d_done:
        if(buffer) d2_freerenderbuffer(device,buffer);
        d2_deinithw(device);d2_closedevice(device);return result;
    }
    if(!strcmp(stage,"jpeg")) {
        jpeg_cfg_t cfg=g_jpeg0_cfg;uint16_t width=0,height=0;uint32_t lines=0;
        cfg.p_decode_callback=jpeg_event;jpeg_events=0;memset(bitmap,0,sizeof(bitmap));
        if(R_JPEG_Open(&g_jpeg0_ctrl,&cfg)) return -RT_ERROR;
        /* Whole known fixture, padded DMA-readable storage; count mode disabled so
         * header prefetch cannot pause before output is configured. */
        memset(jpeg_input,0,sizeof(jpeg_input));memcpy(jpeg_input,test_jpeg,sizeof(test_jpeg));
        if(R_JPEG_InputBufferSet(&g_jpeg0_ctrl,jpeg_input,0)) goto jpeg_done;
        rt_tick_t start=rt_tick_get();
        while(!(jpeg_events&(JPEG_STATUS_IMAGE_SIZE_READY|JPEG_STATUS_ERROR)) && !test_elapsed(start,1000) && !test_cancelled()) rt_thread_mdelay(1);
        if(!(jpeg_events&JPEG_STATUS_IMAGE_SIZE_READY) || (jpeg_events&JPEG_STATUS_ERROR)) goto jpeg_done;
        if(R_JPEG_DecodeImageSizeGet(&g_jpeg0_ctrl,&width,&height) || width!=16 || height!=16) goto jpeg_done;
        if(R_JPEG_DecodeHorizontalStrideSet(&g_jpeg0_ctrl,16) || R_JPEG_OutputBufferSet(&g_jpeg0_ctrl,bitmap,sizeof(bitmap))) goto jpeg_done;
        start=rt_tick_get();
        while(!(jpeg_events&(JPEG_STATUS_OPERATION_COMPLETE|JPEG_STATUS_ERROR)) && !test_elapsed(start,1000) && !test_cancelled()) rt_thread_mdelay(1);
        if(!(jpeg_events&JPEG_STATUS_OPERATION_COMPLETE) || (jpeg_events&JPEG_STATUS_ERROR)) goto jpeg_done;
        if(R_JPEG_DecodeLinesDecodedGet(&g_jpeg0_ctrl,&lines) || lines!=16) goto jpeg_done;
        result=0;
        /* Fixture is uniform RGB(128,128,128). Allow one quantization step. */
        for(unsigned i=0;i<256;++i) {
            unsigned r=bitmap[i]>>11,g=(bitmap[i]>>5)&63,b=bitmap[i]&31;
            if(r<15 || r>17 || g<31 || g>33 || b<15 || b>17) result=-RT_ERROR;
        }
jpeg_done:
        rt_kprintf("JPEG %ux%u lines=%u status=%X pixel=%04X\n",width,height,lines,jpeg_events,bitmap[0]);
        R_JPEG_Close(&g_jpeg0_ctrl);return result;
    }
    return -RT_EINVAL;
}
