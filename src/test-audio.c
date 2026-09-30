/* SPDX-License-Identifier: Apache-2.0 */
#include "peripheral-test.h"
#define AUDIO_FRAMES 8192
static volatile unsigned rx_done, audio_position;
static const int32_t *play_data;
static unsigned play_count;
static void mic_callback(i2s_callback_args_t *a) { if(a->event==I2S_EVENT_RX_FULL) rx_done=1; }
static int capture(int32_t *data)
{
    i2s_cfg_t cfg=g_i2s0_cfg;
    timer_cfg_t clock=g_timer_cfg;
    int result=-RT_ERROR;
    /* GPT1A internally feeds SSI; 120MHz/117/64 = 16025.64 stereo frames/s. */
    clock.period_counts=117;clock.duty_cycle_counts=58;
    cfg.p_callback=mic_callback;
    memset(data,0,AUDIO_FRAMES*8);rx_done=0;
    if(R_GPT_Open(&g_timer_ctrl,&clock)) return -RT_ERROR;
    if(R_GPT_Start(&g_timer_ctrl)) goto timer_close;
    if(R_SSI_Open(&g_i2s0_ctrl,&cfg)) goto timer_close;
    if(R_SSI_Read(&g_i2s0_ctrl,data,AUDIO_FRAMES*8)==FSP_SUCCESS) {
        rt_tick_t start=rt_tick_get();
        while(!rx_done && !test_elapsed(start,2000) && !test_cancelled()) rt_thread_mdelay(1);
        if(rx_done) result=0;
    }
    R_SSI_Stop(&g_i2s0_ctrl);rt_thread_mdelay(3);R_SSI_Close(&g_i2s0_ctrl);
timer_close:
    R_GPT_Stop(&g_timer_ctrl);R_GPT_Close(&g_timer_ctrl);return result;
}
static void audio_tick(timer_callback_args_t *a)
{
    int sample=0;
    RT_UNUSED(a);
    if(audio_position<play_count) {
        if(play_data) sample=((int32_t)((uint32_t)play_data[audio_position*2]<<8)>>16)/8;
        else sample=(audio_position%32<16)?1200:-1200;
        ++audio_position;
    }
    /* Center both bridge legs at 50%, with a limited differential swing. */
    if(sample>4000) sample=4000;
    if(sample< -4000) sample=-4000;
    int delta=sample*750/32768;
    R_GPT_DutyCycleSet(&g_timer6_ctrl,750+delta,GPT_IO_PIN_GTIOCA);
    R_GPT_DutyCycleSet(&g_timer6_ctrl,750-delta,GPT_IO_PIN_GTIOCB);
}
static int play(const int32_t *data)
{
    timer_cfg_t carrier=g_timer6_cfg,sampler=g_timer2_cfg;
    int result=-RT_ERROR;
    carrier.period_counts=1500;carrier.duty_cycle_counts=750;
    sampler.period_counts=7488;sampler.duty_cycle_counts=3744;sampler.p_callback=audio_tick;
    test_restore_pin(BSP_IO_PORT_07_PIN_02);test_restore_pin(BSP_IO_PORT_07_PIN_03);
    play_data=data;play_count=data?AUDIO_FRAMES:8000;audio_position=0;
    if(R_GPT_Open(&g_timer6_ctrl,&carrier)) goto pins;
    if(R_GPT_Start(&g_timer6_ctrl)) goto carrier_close;
    if(R_GPT_Open(&g_timer2_ctrl,&sampler)) goto carrier_close;
    if(R_GPT_Start(&g_timer2_ctrl)==FSP_SUCCESS) {
        rt_tick_t start=rt_tick_get();
        while(audio_position<play_count && !test_elapsed(start,1500) && !test_cancelled()) rt_thread_mdelay(1);
        result=audio_position==play_count?TEST_WAIT:-RT_ETIMEOUT;
    }
    R_GPT_Stop(&g_timer2_ctrl);R_GPT_Close(&g_timer2_ctrl);
carrier_close:
    R_GPT_Stop(&g_timer6_ctrl);R_GPT_Close(&g_timer6_ctrl);
pins:
    rt_pin_mode(BSP_IO_PORT_07_PIN_02,PIN_MODE_OUTPUT);rt_pin_write(BSP_IO_PORT_07_PIN_02,0);
    rt_pin_mode(BSP_IO_PORT_07_PIN_03,PIN_MODE_OUTPUT);rt_pin_write(BSP_IO_PORT_07_PIN_03,0);
    rt_kprintf("AUDIO output samples=%u/%u; listening confirmation required\n",audio_position,play_count);
    play_data=NULL;return result;
}
int test_audio(const char *stage)
{
    int result;int32_t *data;
    if(!strcmp(stage,"tone")) return play(NULL);
    if(strcmp(stage,"mic") && strcmp(stage,"replay")) return -RT_EINVAL;
    data=rt_malloc(AUDIO_FRAMES*8);if(!data) return -RT_ENOMEM;
    rt_kprintf("MIC capture 8192 stereo frames at ~16026Hz; microphone on LEFT\n");
    result=capture(data);
    if(!result) {
        int32_t min=0x7fffffff,max=(-2147483647-1);unsigned changed=0;
        for(unsigned i=0;i<AUDIO_FRAMES;++i) {
            /* SSI PDTA right-justifies 24-bit words: explicitly sign extend. */
            int32_t sample=(int32_t)((uint32_t)data[i*2]<<8)>>8;
            if(sample<min) min=sample;
            if(sample>max) max=sample;
            if(i && data[i*2]!=data[(i-1)*2]) ++changed;
        }
        rt_kprintf("MIC DMA complete left min=%d max=%d changed=%u/%u; acoustic response needs speaking test\n",min,max,changed,AUDIO_FRAMES-1);
        if(!changed) result=-RT_ERROR;
        else if(!strcmp(stage,"replay")) result=play(data);
    }
    rt_free(data);return result;
}
