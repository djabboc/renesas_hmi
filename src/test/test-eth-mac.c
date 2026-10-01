/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file test-eth-mac.c
 * @brief 验证以太网 MAC 内部回环。
 *
 * 无需网线；固定 100M 全双工，发送并逐字节比对 12 帧，结束恢复自动协商。
 * 阅读顺序：文件末尾线程入口 → run_test → 本文件的硬件辅助函数。
 * 只依赖 RT-Thread、FSP 及所用库，不调用其他测试文件。
 */
#include <rtthread.h>
#include <rtdevice.h>
#include "hal_data.h"
#include <string.h>

/* 结果：0=断言通过，1=等待人工观察，2=缺少测试条件，负值=错误。 */
enum
{
    TEST_PASS = 0,
    TEST_WAIT = 1,
    TEST_SKIP = 2
};
#define TEST_ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

/* 参数可为 NULL；提供 RT-Thread 事件对象时，bit0 表示请求协作退出。
 * 不清除事件位，让初始化、收发和清理路径都能看见同一次停止请求。 */
static int test_cancelled(void)
{
    rt_event_t stop_event = (rt_event_t)rt_thread_self()->parameter;
    rt_uint32_t received = 0;
    if (stop_event == RT_NULL)
    {
        return 0;
    }
    return rt_event_recv(stop_event, 1, RT_EVENT_FLAG_OR, 0, &received) == RT_EOK;
}

/* 无符号 tick 差值允许系统计时回绕；等待始终有上限。 */
static int test_elapsed(rt_tick_t start, unsigned milliseconds)
{
    return (rt_tick_t)(rt_tick_get() - start) >= rt_tick_from_millisecond(milliseconds);
}

/* FSP 3.5 在 r_ether_phy.c 导出这两个 Clause 22 MDIO 辅助函数。 */
extern uint32_t ether_phy_read(ether_phy_instance_ctrl_t *control, uint32_t register_number);
extern void
ether_phy_write(ether_phy_instance_ctrl_t *control, uint32_t register_number, uint32_t value);

#define PHY_REG_CONTROL 0u
#define PHY_REG_STATUS 1u
#define PHY_REG_ID_HIGH 2u
#define PHY_REG_ID_LOW 3u
#define PHY_CONTROL_SPEED_100M (1u << 13)
#define PHY_CONTROL_AUTONEG (1u << 12)
#define PHY_CONTROL_RESTART_AUTONEG (1u << 9)
#define PHY_CONTROL_FULL_DUPLEX (1u << 8)
#define ETH_RX_DESCRIPTOR_COUNT 4u
#define ETH_TX_DESCRIPTOR_COUNT 2u
#define ETH_BUFFER_COUNT (ETH_RX_DESCRIPTOR_COUNT + ETH_TX_DESCRIPTOR_COUNT)
#define ETH_LOOP_FRAME_COUNT 12u
#define ETH_FRAME_TIMEOUT_MS 500u

static ether_cfg_t ethernet_config;
static ether_phy_api_t selected_phy_api;
static ether_phy_instance_t selected_phy;
static uint8_t mac_address[6] = {0x02, 0x48, 0x4d, 0x49, 0x00, 0x01};
static ether_instance_descriptor_t rx_descriptors[ETH_RX_DESCRIPTOR_COUNT] BSP_ALIGN_VARIABLE(16);
static ether_instance_descriptor_t tx_descriptors[ETH_TX_DESCRIPTOR_COUNT] BSP_ALIGN_VARIABLE(16);
static uint8_t dma_buffers[ETH_BUFFER_COUNT][1536] BSP_ALIGN_VARIABLE(32);
static uint8_t *dma_buffer_pointers[ETH_BUFFER_COUNT];
static uint8_t transmit_frame[1514] BSP_ALIGN_VARIABLE(4);
static uint8_t receive_frame[1536] BSP_ALIGN_VARIABLE(4);
static volatile unsigned ethernet_irq_count;

/* MAC 中断只累加计数，收帧和数据比较在测试线程执行。 */
static void ethernet_callback(ether_callback_args_t *arguments)
{
    RT_UNUSED(arguments);
    ++ethernet_irq_count;
}

/* 内部回环没有对端协商。仅在回环阶段向 FSP 提供固定链路状态，
 * PASS 仍由后续实际接收长度/数据比对决定。
 */
static fsp_err_t loopback_link_status(ether_phy_ctrl_t *control)
{
    RT_UNUSED(control);
    return FSP_SUCCESS;
}

/* 内部回环没有真实对端；向 FSP 提供固定的 100M 全双工配置。 */
static fsp_err_t loopback_link_ability(ether_phy_ctrl_t *control,
                                       uint32_t *speed,
                                       uint32_t *local_pause,
                                       uint32_t *remote_pause)
{
    RT_UNUSED(control);
    *speed = ETHER_PHY_LINK_SPEED_100F;
    *local_pause = 0;
    *remote_pause = 0;
    return FSP_SUCCESS;
}

/* 根据 MCU 唯一 ID 派生本地单播 MAC，避免测试板使用同一个固定地址。 */
static void derive_mac_address(void)
{
    const bsp_unique_id_t *unique_id = R_BSP_UniqueIdGet();
    uint32_t hash = 2166136261u;

    /* FNV-1a 压缩芯片 ID；首字节 02 保持本地管理、单播属性。 */
    for (unsigned index = 0; index < sizeof(unique_id->unique_id_bytes); ++index)
    {
        hash = (hash ^ unique_id->unique_id_bytes[index]) * 16777619u;
    }
    for (unsigned index = 0; index < 4; ++index)
    {
        mac_address[index + 2] = (uint8_t)(hash >> (index * 8));
    }
}

/* 复位 PHY，将本文件的回调、DMA 描述符和缓冲交给 FSP，再打开 MAC。 */
static int ethernet_open(void)
{
    fsp_err_t error;

    derive_mac_address();
    /* P400 是 PHY 低有效复位；释放后留出启动时间再访问 MDIO。 */
    R_IOPORT_PinCfg(&g_ioport_ctrl,
                    BSP_IO_PORT_04_PIN_00,
                    IOPORT_CFG_PORT_DIRECTION_OUTPUT | IOPORT_CFG_PORT_OUTPUT_LOW);
    rt_thread_mdelay(20);
    R_IOPORT_PinWrite(&g_ioport_ctrl, BSP_IO_PORT_04_PIN_00, BSP_IO_LEVEL_HIGH);
    rt_thread_mdelay(100);

    selected_phy_api = g_ether_phy_on_ether_phy;

    selected_phy_api.linkStatusGet = loopback_link_status;
    selected_phy_api.linkPartnerAbilityGet = loopback_link_ability;

    selected_phy = g_ether_phy0;
    selected_phy.p_api = &selected_phy_api;
    ethernet_config = g_ether0_cfg;
    ethernet_config.p_ether_phy_instance = &selected_phy;
    ethernet_config.p_callback = ethernet_callback;
    ethernet_config.p_mac_address = mac_address;
    ethernet_config.p_rx_descriptors = rx_descriptors;
    ethernet_config.p_tx_descriptors = tx_descriptors;
    ethernet_config.num_rx_descriptors = ETH_RX_DESCRIPTOR_COUNT;
    ethernet_config.num_tx_descriptors = ETH_TX_DESCRIPTOR_COUNT;
    for (unsigned index = 0; index < ETH_BUFFER_COUNT; ++index)
    {
        dma_buffer_pointers[index] = dma_buffers[index];
    }
    ethernet_config.pp_ether_buffers = dma_buffer_pointers;
    ethernet_irq_count = 0;
    error = R_ETHER_Open(&g_ether0_ctrl, &ethernet_config);
    if (error != FSP_SUCCESS)
    {
        rt_kprintf("ETH open=%d\n", error);
        return -RT_ERROR;
    }
    rt_kprintf("ETH MAC=%02X:%02X:%02X:%02X:%02X:%02X\n",
               mac_address[0],
               mac_address[1],
               mac_address[2],
               mac_address[3],
               mac_address[4],
               mac_address[5]);
    return TEST_PASS;
}

/* 周期调用链路状态机；协商成功、超时或取消时结束等待。 */
static int wait_for_ethernet_link(unsigned timeout_ms)
{
    rt_tick_t start = rt_tick_get();
    do
    {
        R_ETHER_LinkProcess(&g_ether0_ctrl);
        if (g_ether0_ctrl.link_establish_status == ETHER_LINK_ESTABLISH_STATUS_UP)
        {
            return TEST_PASS;
        }
        rt_thread_mdelay(20);
    } while (!test_elapsed(start, timeout_ms) && !test_cancelled());
    return -RT_ETIMEOUT;
}

/* 通过 MDIO 读取 PHY 身份；全零或全 FF 表示管理接口没有有效应答。 */
static int verify_phy_identity(void)
{
    uint32_t id_high = ether_phy_read(&g_ether_phy0_ctrl, PHY_REG_ID_HIGH);
    uint32_t id_low = ether_phy_read(&g_ether_phy0_ctrl, PHY_REG_ID_LOW);

    rt_kprintf("ETH PHY id=%04X:%04X BMCR=%04X BMSR=%04X\n",
               id_high,
               id_low,
               ether_phy_read(&g_ether_phy0_ctrl, PHY_REG_CONTROL),
               ether_phy_read(&g_ether_phy0_ctrl, PHY_REG_STATUS));
    /* 全零/全 FF 通常表示 MDIO 未正常应答，不能作为有效身份。 */
    if ((id_high == 0 && id_low == 0) || id_high == 0xffff || id_low == 0xffff)
    {
        return -RT_ERROR;
    }
    return TEST_PASS;
}

/* 发送 60/512/1514 字节变化载荷，比较实际收到的长度和每个字节。 */
static int verify_loopback_frames(void)
{
    static const unsigned frame_lengths[] = {60, 512, 1514};
    unsigned verified_frames;
    uint32_t phy_control = PHY_CONTROL_SPEED_100M | PHY_CONTROL_FULL_DUPLEX;

    /* 关闭自动协商，固定 100M 全双工；PHY 回环还须设置 BMCR bit14。 */

    ether_phy_write(&g_ether_phy0_ctrl, PHY_REG_CONTROL, phy_control);
    rt_thread_mdelay(50);
    if (wait_for_ethernet_link(ETH_FRAME_TIMEOUT_MS) != TEST_PASS)
    {
        return -RT_ERROR;
    }

    R_ETHERC0->ECMR_b.ILB = 1;

    for (verified_frames = 0; verified_frames < ETH_LOOP_FRAME_COUNT && !test_cancelled();
         ++verified_frames)
    {
        unsigned length = frame_lengths[verified_frames % TEST_ARRAY_SIZE(frame_lengths)];
        uint32_t received_bytes;
        rt_tick_t start;

        /* 0x88B5 是本地实验 EtherType；变化载荷可检出旧帧或截断帧。 */
        memcpy(transmit_frame, mac_address, 6);
        memcpy(transmit_frame + 6, mac_address, 6);
        transmit_frame[12] = 0x88;
        transmit_frame[13] = 0xb5;
        for (unsigned index = 14; index < length; ++index)
        {
            transmit_frame[index] = (uint8_t)(index ^ (verified_frames * 31));
        }
        if (R_ETHER_Write(&g_ether0_ctrl, transmit_frame, length) != FSP_SUCCESS)
        {
            return -RT_ERROR;
        }
        start = rt_tick_get();
        do
        {
            received_bytes = sizeof(receive_frame);
            if (R_ETHER_Read(&g_ether0_ctrl, receive_frame, &received_bytes) == FSP_SUCCESS &&
                received_bytes != 0)
            {
                break;
            }
            received_bytes = 0;
            rt_thread_mdelay(1);
        } while (!test_elapsed(start, ETH_FRAME_TIMEOUT_MS) && !test_cancelled());
        if (received_bytes != length || memcmp(transmit_frame, receive_frame, length) != 0)
        {
            rt_kprintf("ETH loop frame=%u expected=%u received=%u\n",
                       verified_frames,
                       length,
                       received_bytes);
            return -RT_ERROR;
        }
    }
    rt_kprintf("ETH %s verified=%u/12 frames (60/512/1514 bytes) irq=%u\n",
               "mac",
               verified_frames,
               ethernet_irq_count);
    if (test_cancelled())
    {
        return -RT_EINTR;
    }
    else
    {
        return TEST_PASS;
    }
}

/* 按本文件配置打开外设，执行验证；所有退出路径都在返回前清理本次资源。 */
static int run_test(void)
{
    int result;

    if (ethernet_open() != TEST_PASS)
    {
        return -RT_ERROR;
    }
    result = verify_phy_identity();
    if (result == TEST_PASS)
    {
        result = verify_loopback_frames();
    }

    /* 任一阶段退出均关闭 MAC；回环模式还需恢复 PHY 自动协商。 */
    R_ETHERC0->ECMR_b.ILB = 0;

    ether_phy_write(
        &g_ether_phy0_ctrl, PHY_REG_CONTROL, PHY_CONTROL_AUTONEG | PHY_CONTROL_RESTART_AUTONEG);

    R_ETHER_Close(&g_ether0_ctrl);
    return result;
}

/* 本文件唯一线程入口：独立完成初始化、验证和清理，然后自然返回。
 * error 保存最终结果，便于调用方在 RT-Thread 线程回收时读取。 */
void test_eth_mac_thread(void *argument)
{
    int result;
    RT_UNUSED(argument);
    result = run_test();
    if (test_cancelled())
    {
        result = -RT_EINTR;
    }
    rt_thread_self()->error = result;
}
