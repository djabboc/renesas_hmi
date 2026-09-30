#include <rtthread.h>
#include "hal_data.h"
#include <rtdevice.h>
#include "test-profile.h"

#define LED_PIN    BSP_IO_PORT_02_PIN_09 /* Onboard LED pins */

void hal_entry(void)
{
    rt_kprintf("\nHello RT-Thread!\n");

    while (1)
    {
#if !HMI_TEST_SUITE
        rt_pin_write(LED_PIN, PIN_HIGH);
        rt_thread_mdelay(500);
        rt_pin_write(LED_PIN, PIN_LOW);
        rt_thread_mdelay(500);
#else
        rt_thread_mdelay(1000);
#endif
    }
}
