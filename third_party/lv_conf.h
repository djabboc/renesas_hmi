#ifndef LV_CONF_H
#define LV_CONF_H

/* LVGL 8.3: RGB565 framebuffer, RT-Thread allocator and millisecond clock. */
#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0
#define LV_MEM_CUSTOM 1
#define LV_MEM_CUSTOM_INCLUDE "rtthread.h"
#define LV_MEM_CUSTOM_ALLOC rt_malloc
#define LV_MEM_CUSTOM_FREE rt_free
#define LV_MEM_CUSTOM_REALLOC rt_realloc
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "rtthread.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (rt_tick_get_millisecond())
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0
#define LV_BUILD_EXAMPLES 0
#define LV_USE_DEMO_WIDGETS 0
#define LV_USE_DEMO_BENCHMARK 0
#define LV_USE_DEMO_STRESS 0
#define LV_USE_DEMO_MUSIC 0

#endif
