#ifndef HMI_LV_RT_THREAD_CONF_H
#define HMI_LV_RT_THREAD_CONF_H

/* Upstream LVGL detects __RTTHREAD__ before loading lv_conf.h.
 * Use the vendored configuration without requiring an RT-Thread LVGL package.
 * The path is resolved from lvgl/src/lv_conf_internal.h. */
#undef LV_CONF_INCLUDE_SIMPLE
#define LV_CONF_PATH ../../lv_conf.h

#endif
