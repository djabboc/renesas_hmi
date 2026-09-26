#include <rtthread.h>
#include "hal_data.h"
#include <rtdevice.h>

#define LOG_TAG "example"   // 本模块的日志标签
#define LOG_LVL LOG_LVL_DBG // 本模块的日志级别

#include <ulog.h> // 先定义宏，再包含头文件

#define LED_PIN    BSP_IO_PORT_02_PIN_09 /* Onboard LED pins */

void hal_entry(void)
{
    rt_kprintf("\nHello RT-Thread!\n");

    while (1)
    {
        rt_pin_write(LED_PIN, PIN_HIGH);
        rt_thread_mdelay(500);
        rt_pin_write(LED_PIN, PIN_LOW);
        rt_thread_mdelay(500);

        LOG_I("HELLO WORLD!"); // 测试 ulog
    }
}
