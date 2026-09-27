#include <rtthread.h>
#include <rtdevice.h>
#include <drivers/pin.h>
#include "hal_data.h"

#define GT911_INT_PIN       "p004"
#define GT911_RST_PIN       "p801"

static rt_base_t gt911_int_pin = PIN_NONE;
static rt_base_t gt911_rst_pin = PIN_NONE;

static struct rt_semaphore gt911_irq_sem;
static volatile rt_uint32_t gt911_irq_time_ms;

static void gt911_int_callback(void *args)
{
    RT_UNUSED(args);
    gt911_irq_time_ms = rt_tick_get_millisecond();
    rt_sem_release(&gt911_irq_sem);
}

static void gt911_gpio_irq_thread(void *parameter)
{
    RT_UNUSED(parameter);

    gt911_int_pin = rt_pin_get(GT911_INT_PIN);
    gt911_rst_pin = rt_pin_get(GT911_RST_PIN);

    if ((gt911_int_pin < 0) || (gt911_rst_pin < 0))
    {
        rt_kprintf("GT911 GPIO: invalid INT/RST pin mapping\n");
        return;
    }

    rt_pin_mode(gt911_int_pin, PIN_MODE_OUTPUT);
    rt_pin_mode(gt911_rst_pin, PIN_MODE_OUTPUT);

    rt_pin_write(gt911_rst_pin, PIN_LOW);
    rt_pin_write(gt911_int_pin, PIN_HIGH); /* Select I2C address 0x14. */

    rt_thread_mdelay(10);
    rt_pin_write(gt911_rst_pin, PIN_HIGH);
    rt_thread_mdelay(100);

    /* 瑞萨 FSPIOPORT 驱动 API：配置芯片 GPIO 引脚 */
    if (R_IOPORT_PinCfg(&g_ioport_ctrl, gt911_int_pin,
                        IOPORT_CFG_IRQ_ENABLE | IOPORT_CFG_PORT_DIRECTION_INPUT) != FSP_SUCCESS)
    {
        rt_kprintf("GT911 GPIO: INT input/IRQ configuration failed\n");
        return;
    }

    if (rt_pin_attach_irq(gt911_int_pin, PIN_IRQ_MODE_FALLING,
                          gt911_int_callback, RT_NULL) != RT_EOK)
    {
        rt_kprintf("GT911 GPIO: falling-edge IRQ attach failed\n");
        return;
    }

    if (rt_pin_irq_enable(gt911_int_pin, RT_TRUE) != RT_EOK)
    {
        rt_kprintf("GT911 GPIO: IRQ enable failed\n");
        return;
    }

    rt_kprintf("GT911 GPIO: reset complete; waiting for INT falling edge\n");

    while (1)
    {
        if (rt_sem_take(&gt911_irq_sem, RT_WAITING_FOREVER) == RT_EOK)
        {
            rt_kprintf("GT911 GPIO: INT at %u ms\n", gt911_irq_time_ms);
        }
    }
}

static int gt911_gpio_irq_start(void)
{
    rt_thread_t thread;

    if (rt_sem_init(&gt911_irq_sem, "gtirq", 0, RT_IPC_FLAG_FIFO) != RT_EOK)
    {
        rt_kprintf("GT911 GPIO: semaphore initialization failed\n");
        return -RT_ERROR;
    }

    thread = rt_thread_create("gtirq", gt911_gpio_irq_thread, RT_NULL,
                              1024, 20, 10);
    if (thread == RT_NULL)
    {
        rt_sem_detach(&gt911_irq_sem);
        rt_kprintf("GT911 GPIO: thread creation failed\n");
        return -RT_ERROR;
    }

    return rt_thread_startup(thread);
}

INIT_APP_EXPORT(gt911_gpio_irq_start);
