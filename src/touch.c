#include <rtthread.h>
#include <rtdevice.h>
#include <drivers/i2c.h>
#include <drivers/pin.h>
#include "hal_data.h"

#define GT911_I2C_ADDR_LOW       0x14
#define GT911_I2C_ADDR_HIGH      0x5D
#define GT911_REG_PRODUCT_ID     0x8140
#define GT911_REG_STATUS         0x814E
#define GT911_REG_POINT0         0x814F
#define GT911_MAX_POINTS         5
#define GT911_POINT_SIZE         8

#define GT911_INT_PIN            "p004"
#define GT911_RST_PIN            "p801"

static rt_base_t gt911_int_pin = PIN_NONE;
static rt_base_t gt911_rst_pin = PIN_NONE;

static struct rt_i2c_bus_device *gt911_bus;
static struct rt_semaphore gt911_irq_sem;
static rt_uint16_t gt911_addr;

static rt_err_t gt911_read_regs(rt_uint16_t reg, rt_uint8_t *data, rt_uint16_t len)
{
    rt_uint8_t reg_addr[2];
    struct rt_i2c_msg msgs[2];

    reg_addr[0] = (rt_uint8_t)(reg >> 8);
    reg_addr[1] = (rt_uint8_t)reg;

    msgs[0].addr = gt911_addr;
    msgs[0].flags = RT_I2C_WR;
    msgs[0].len = sizeof(reg_addr);
    msgs[0].buf = reg_addr;

    msgs[1].addr = gt911_addr;
    msgs[1].flags = RT_I2C_RD;
    msgs[1].len = len;
    msgs[1].buf = data;

    return rt_i2c_transfer(gt911_bus, msgs, 2) == 2 ? RT_EOK : -RT_EIO;
}

static rt_err_t gt911_write_reg(rt_uint16_t reg, rt_uint8_t value)
{
    rt_uint8_t data[3];
    struct rt_i2c_msg msg;

    data[0] = (rt_uint8_t)(reg >> 8);
    data[1] = (rt_uint8_t)reg;
    data[2] = value;

    msg.addr = gt911_addr;
    msg.flags = RT_I2C_WR;
    msg.len = sizeof(data);
    msg.buf = data;

    return rt_i2c_transfer(gt911_bus, &msg, 1) == 1 ? RT_EOK : -RT_EIO;
}

static rt_err_t gt911_probe_addr(rt_uint16_t addr)
{
    struct rt_i2c_msg msg;

    msg.addr = addr;
    msg.flags = RT_I2C_WR;
    msg.len = 0;
    msg.buf = RT_NULL;

    return rt_i2c_transfer(gt911_bus, &msg, 1) == 1 ? RT_EOK : -RT_EIO;
}

static void gt911_int_callback(void *args)
{
    RT_UNUSED(args);
    rt_sem_release(&gt911_irq_sem);
}

static void gt911_read_touch_data(void)
{
    rt_uint8_t status;
    rt_uint8_t data[GT911_MAX_POINTS * GT911_POINT_SIZE];
    rt_uint8_t count;
    rt_uint8_t i;

    if (gt911_read_regs(GT911_REG_STATUS, &status, 1) != RT_EOK)
    {
        rt_kprintf("GT911: status read failed\n");
        return;
    }

    if (!(status & 0x80))
    {
        rt_kprintf("GT911: INT falling, status 0x%02X is not ready\n", status);
        return;
    }

    count = status & 0x0F;
    if (count > GT911_MAX_POINTS)
    {
        rt_kprintf("GT911: invalid touch count %u, status 0x%02X\n", count, status);
    }
    else if (count == 0)
    {
        rt_kprintf("GT911: 0 points\n");
    }
    else if (gt911_read_regs(GT911_REG_POINT0, data, count * GT911_POINT_SIZE) != RT_EOK)
    {
        rt_kprintf("GT911: coordinate read failed\n");
    }
    else
    {
        for (i = 0; i < count; i++)
        {
            rt_uint8_t offset = i * GT911_POINT_SIZE;
            rt_uint8_t point_id = data[offset] & 0x0F;
            rt_uint16_t x = data[offset + 1] | ((rt_uint16_t)data[offset + 2] << 8);
            rt_uint16_t y = data[offset + 3] | ((rt_uint16_t)data[offset + 4] << 8);
            rt_uint16_t size = data[offset + 5] | ((rt_uint16_t)data[offset + 6] << 8);

            rt_kprintf("GT911: point %u ID=%u X=%u Y=%u Size=%u\n",
                       i, point_id, x, y, size);
        }
    }

    if (gt911_write_reg(GT911_REG_STATUS, 0x00) != RT_EOK)
    {
        rt_kprintf("GT911: status clear failed\n");
    }
}

static void gt911_demo_thread(void *parameter)
{
    rt_uint8_t id[4];
    rt_err_t result;

    RT_UNUSED(parameter);

    gt911_int_pin = rt_pin_get(GT911_INT_PIN);
    gt911_rst_pin = rt_pin_get(GT911_RST_PIN);
    if ((gt911_int_pin < 0) || (gt911_rst_pin < 0))
    {
        rt_kprintf("GT911: invalid INT/RST pin mapping\n");
        return;
    }

    rt_pin_mode(gt911_int_pin, PIN_MODE_OUTPUT);
    rt_pin_mode(gt911_rst_pin, PIN_MODE_OUTPUT);
    rt_pin_write(gt911_rst_pin, PIN_LOW);
    rt_pin_write(gt911_int_pin, PIN_HIGH);
    rt_thread_mdelay(10);
    rt_pin_write(gt911_rst_pin, PIN_HIGH);
    rt_thread_mdelay(100);
    if (R_IOPORT_PinCfg(&g_ioport_ctrl, gt911_int_pin,
                        IOPORT_CFG_IRQ_ENABLE | IOPORT_CFG_PORT_DIRECTION_INPUT) != FSP_SUCCESS)
    {
        rt_kprintf("GT911: INT IRQ pin configuration failed\n");
        return;
    }

    gt911_bus = rt_i2c_bus_device_find("i2c1");
    if (gt911_bus == RT_NULL)
    {
        rt_kprintf("GT911: I2C bus i2c1 not found\n");
        return;
    }

    if (gt911_probe_addr(GT911_I2C_ADDR_HIGH) == RT_EOK)
    {
        gt911_addr = GT911_I2C_ADDR_HIGH;
    }
    else if (gt911_probe_addr(GT911_I2C_ADDR_LOW) == RT_EOK)
    {
        gt911_addr = GT911_I2C_ADDR_LOW;
    }
    else
    {
        rt_kprintf("GT911: no I2C response at 0x14 or 0x5D\n");
        return;
    }

    result = gt911_read_regs(GT911_REG_PRODUCT_ID, id, sizeof(id));
    if ((result != RT_EOK) || (id[0] != '9') || (id[1] != '1') ||
        (id[2] != '1') || (id[3] != 0))
    {
        rt_kprintf("GT911: product ID check failed at 0x%02X\n", gt911_addr);
        return;
    }

    rt_kprintf("GT911: ID 911 at 0x%02X\n", gt911_addr);
    if (rt_pin_attach_irq(gt911_int_pin, PIN_IRQ_MODE_FALLING,
                          gt911_int_callback, RT_NULL) != RT_EOK)
    {
        rt_kprintf("GT911: INT falling-edge setup failed\n");
        return;
    }

    if (rt_pin_irq_enable(gt911_int_pin, RT_TRUE) != RT_EOK)
    {
        rt_kprintf("GT911: INT IRQ enable failed\n");
        return;
    }

    rt_kprintf("GT911: INT falling-edge demo ready; touch the screen\n");
    if (rt_pin_read(gt911_int_pin) == PIN_LOW)
    {
        rt_sem_release(&gt911_irq_sem);
    }

    while (1)
    {
        if (rt_sem_take(&gt911_irq_sem, RT_WAITING_FOREVER) == RT_EOK)
        {
            gt911_read_touch_data();
        }
    }
}

static int gt911_demo_start(void)
{
    rt_thread_t thread;

    if (rt_sem_init(&gt911_irq_sem, "gt911", 0, RT_IPC_FLAG_FIFO) != RT_EOK)
    {
        rt_kprintf("GT911: semaphore initialization failed\n");
        return -RT_ERROR;
    }

    thread = rt_thread_create("gt911", gt911_demo_thread, RT_NULL,
                              2048, 20, 10);
    if (thread == RT_NULL)
    {
        rt_sem_detach(&gt911_irq_sem);
        rt_kprintf("GT911: worker thread creation failed\n");
        return -RT_ERROR;
    }

    return rt_thread_startup(thread);
}

// INIT_APP_EXPORT(gt911_demo_start);