/* SPDX-License-Identifier: Apache-2.0 */
#include "peripheral-test.h"
#include "rw007-internet.h"
/* FSP 3.5 exposes these helpers from r_ether_phy.c. Clause 22 registers. */
extern uint32_t ether_phy_read(ether_phy_instance_ctrl_t *, uint32_t);
extern void ether_phy_write(ether_phy_instance_ctrl_t *, uint32_t, uint32_t);
static ether_cfg_t ecfg;
static ether_phy_api_t phy_api;
static ether_phy_instance_t phy;
static uint8_t mac[6] = {0x02,0x48,0x4d,0x49,0x00,0x01};
static ether_instance_descriptor_t rx_desc[4] BSP_ALIGN_VARIABLE(16);
static ether_instance_descriptor_t tx_desc[2] BSP_ALIGN_VARIABLE(16);
static uint8_t buffers[6][1536] BSP_ALIGN_VARIABLE(32);
static uint8_t *buffer_ptrs[6];
static uint8_t tx[1514] BSP_ALIGN_VARIABLE(4), rx[1536] BSP_ALIGN_VARIABLE(4);
static volatile unsigned eth_irqs;
static void eth_callback(ether_callback_args_t *a) { RT_UNUSED(a); ++eth_irqs; }
/* FSP expects negotiated link. In a local loop only, explicitly supply the
 * forced 100/full MAC mode; success still requires actual DMA frame receipt. */
static fsp_err_t forced_link(ether_phy_ctrl_t *c) { RT_UNUSED(c); return FSP_SUCCESS; }
static fsp_err_t forced_ability(ether_phy_ctrl_t *c, uint32_t *speed, uint32_t *l, uint32_t *r)
{ RT_UNUSED(c); *speed = ETHER_PHY_LINK_SPEED_100F; *l = *r = 0; return FSP_SUCCESS; }
static int eth_open(int loop)
{
    unsigned i;
    fsp_err_t err;
    const bsp_unique_id_t *uid=R_BSP_UniqueIdGet();
    uint32_t hash=2166136261u;
    for(i=0;i<sizeof(uid->unique_id_bytes);++i) hash=(hash^uid->unique_id_bytes[i])*16777619u;
    for(i=0;i<4;++i) mac[i+2]=(uint8_t)(hash>>(i*8));
    R_IOPORT_PinCfg(&g_ioport_ctrl, BSP_IO_PORT_04_PIN_00,
                   IOPORT_CFG_PORT_DIRECTION_OUTPUT | IOPORT_CFG_PORT_OUTPUT_LOW);
    rt_thread_mdelay(20);
    R_IOPORT_PinWrite(&g_ioport_ctrl, BSP_IO_PORT_04_PIN_00, BSP_IO_LEVEL_HIGH);
    rt_thread_mdelay(100);
    phy_api = g_ether_phy_on_ether_phy;
    if (loop) { phy_api.linkStatusGet = forced_link; phy_api.linkPartnerAbilityGet = forced_ability; }
    phy = g_ether_phy0; phy.p_api = &phy_api;
    ecfg = g_ether0_cfg;
    ecfg.p_ether_phy_instance = &phy; ecfg.p_callback = eth_callback;
    ecfg.p_mac_address = mac;
    ecfg.p_rx_descriptors = rx_desc; ecfg.p_tx_descriptors = tx_desc;
    ecfg.num_rx_descriptors = 4; ecfg.num_tx_descriptors = 2;
    for (i = 0; i < 6; ++i) buffer_ptrs[i] = buffers[i];
    ecfg.pp_ether_buffers = buffer_ptrs;
    eth_irqs = 0;
    err = R_ETHER_Open(&g_ether0_ctrl, &ecfg);
    if (err) { rt_kprintf("ETH open=%d\n", err); return -RT_ERROR; }
    rt_kprintf("ETH MAC=%02X:%02X:%02X:%02X:%02X:%02X\n",mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    return 0;
}
static int eth_link(unsigned timeout)
{
    rt_tick_t start = rt_tick_get();
    do {
        R_ETHER_LinkProcess(&g_ether0_ctrl);
        if (g_ether0_ctrl.link_establish_status == ETHER_LINK_ESTABLISH_STATUS_UP) return 0;
        rt_thread_mdelay(20);
    } while (!test_elapsed(start, timeout) && !test_cancelled());
    return -RT_ETIMEOUT;
}
static rt_err_t eth_exchange(const void *frame, rt_uint16_t length)
{
    uint32_t size;
    rt_tick_t start = rt_tick_get();
    fsp_err_t err;
    if (test_cancelled()) return -RT_EINTR;
    R_ETHER_LinkProcess(&g_ether0_ctrl);
    if (g_ether0_ctrl.link_establish_status != ETHER_LINK_ESTABLISH_STATUS_UP) return -RT_EIO;
    if (length) {
        do {
            err = R_ETHER_Write(&g_ether0_ctrl, (void *)frame, length);
            if (!err) break;
            rt_thread_mdelay(1);
        } while (!test_elapsed(start, 100) && !test_cancelled());
        if (err) return -RT_EIO;
    }
    /* Bounded drain so traffic cannot starve timeouts or cancellation. */
    for (unsigned n = 0; n < 8; ++n) {
        size = sizeof(rx);
        err = R_ETHER_Read(&g_ether0_ctrl, rx, &size);
        if (err || !size) break;
        rw007_net_input(rx, (rt_uint16_t)size);
    }
    return RT_EOK;
}
int test_eth(const char *stage)
{
    int loop = !strcmp(stage, "mac") || !strcmp(stage, "phyloop");
    int result = -RT_ERROR;
    uint32_t id1, id2, size, speed = 0, pause1 = 0, pause2 = 0;
    unsigned i, n;
    if (!loop && strcmp(stage,"phy") && strcmp(stage,"link") && strcmp(stage,"lwip")) return -RT_EINVAL;
    if (eth_open(loop)) return -RT_ERROR;
    id1 = ether_phy_read(&g_ether_phy0_ctrl, 2); id2 = ether_phy_read(&g_ether_phy0_ctrl, 3);
    rt_kprintf("ETH PHY id=%04X:%04X BMCR=%04X BMSR=%04X\n", id1,id2,
        ether_phy_read(&g_ether_phy0_ctrl,0), ether_phy_read(&g_ether_phy0_ctrl,1));
    if ((id1 == 0 && id2 == 0) || id1 == 0xffff || id2 == 0xffff) goto done;
    if (!strcmp(stage,"phy")) { result = 0; goto done; }
    if (loop) {
        /* BMCR: 100 Mbps, full duplex, autoneg off; bit14 enables PHY loop. */
        ether_phy_write(&g_ether_phy0_ctrl, 0, !strcmp(stage,"phyloop") ? 0x6100 : 0x2100);
        rt_thread_mdelay(50);
        if (eth_link(500)) goto done;
        if (!strcmp(stage,"mac")) R_ETHERC0->ECMR_b.ILB = 1;
        for (n = 0; n < 12 && !test_cancelled(); ++n) {
            unsigned length = n % 3 == 0 ? 60 : n % 3 == 1 ? 512 : 1514;
            memcpy(tx, mac, 6); memcpy(tx + 6, mac, 6);
            tx[12] = 0x88; tx[13] = 0xb5;
            for (i = 14; i < length; ++i) tx[i] = (uint8_t)(i ^ (n * 31));
            if (R_ETHER_Write(&g_ether0_ctrl, tx, length)) goto done;
            rt_tick_t start = rt_tick_get();
            do {
                size = sizeof(rx);
                if (R_ETHER_Read(&g_ether0_ctrl, rx, &size) == FSP_SUCCESS && size) break;
                size = 0; rt_thread_mdelay(1);
            } while (!test_elapsed(start, 500) && !test_cancelled());
            if (size != length || memcmp(tx, rx, length)) {
                rt_kprintf("ETH loop frame=%u expected=%u received=%u\n", n,length,size); goto done;
            }
        }
        result = test_cancelled() ? -RT_EINTR : 0;
        rt_kprintf("ETH %s verified=%u/12 frames (60/512/1514 bytes) irq=%u\n", stage,n,eth_irqs);
    } else {
        if (eth_link(6000)) { rt_kprintf("ETH no physical link; connect router LAN cable\n"); result = TEST_SKIP; goto done; }
        R_ETHER_PHY_LinkPartnerAbilityGet(&g_ether_phy0_ctrl,&speed,&pause1,&pause2);
        rt_kprintf("ETH physical link UP speed/duplex enum=%u ECMR=%08X\n", speed,R_ETHERC0->ECMR);
        result = !strcmp(stage,"lwip") ? rw007_internet_test(mac, eth_exchange) : 0;
    }
done:
    R_ETHERC0->ECMR_b.ILB = 0;
    if (loop) ether_phy_write(&g_ether_phy0_ctrl, 0, 0x1200);
    R_ETHER_Close(&g_ether0_ctrl);
    return result;
}
