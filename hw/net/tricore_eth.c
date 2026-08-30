/*
 * AURIX Ethernet emulation based solely on publicly distributed material.
 *
 * Register offsets, descriptor bit positions and the MDIO/PHY programming
 * model were derived from the public TC2x/TC3x/TC4x User Manuals and from
 * the corresponding Infineon iLLD sources published on GitHub (IfxEth_*,
 * IfxGeth_* and IfxLeth_* register/driver definitions).  Copies of those
 * publicly available iLLD sources are present in this workspace only as a
 * developer convenience; they are not project-provided QEMU source material.
 * This implementation does not use, reproduce or depend on NDA-only manuals,
 * vendor RTL, firmware binaries, or confidential electrical/SerDes details.
 * The HSPHY/SerDes layer is intentionally represented by an abstract PHY.
 */

#include "qemu/osdep.h"
#include "hw/net/tricore_eth.h"
#include "hw/core/irq.h"
#include "hw/net/mii.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "net/eth.h"
#include "net/net.h"
#include "net/checksum.h"
#include "qemu/timer.h"
#include "system/dma.h"

#define ETH_CTRL      0x00
#define ETH_STATUS    0x04
#define ETH_INT_EN    0x08
#define ETH_INT_STAT  0x0c
#define ETH_TX_DESC   0x10
#define ETH_RX_DESC   0x14
#define ETH_MDIO_ADDR 0x18
#define ETH_MDIO_DATA 0x1c
#define ETH_MAC_LOW   0x20
#define ETH_MAC_HIGH  0x24
#define ETH_TX_LEN    0x28
#define ETH_TX_KICK   0x2c
#define ETH_VLAN_CTRL 0x30
#define ETH_CSUM_CTRL 0x34
#define ETH_TX_TS_LO  0x38
#define ETH_TX_TS_HI  0x3c
#define ETH_RX_TS_LO  0x50
#define ETH_RX_TS_HI  0x54
#define ETH_TX_DATA   0x40
#define ETH_RX_LEN    0x44
#define ETH_RX_DATA   0x48
#define ETH_RX_POP    0x4c

#define CTRL_RX_EN BIT(0)
#define CTRL_TX_EN BIT(1)
#define STAT_LINK  BIT(0)
#define STAT_RX_AVAIL BIT(1)
#define INT_RX BIT(0)
#define INT_TX BIT(1)
#define INT_MDIO BIT(2)

static void tricore_eth_update_irq(TriCoreETHState *s)
{
    qemu_set_irq(s->irq, !!(s->int_status & s->int_enable));
}

static bool tricore_eth_can_receive(NetClientState *nc)
{
    TriCoreETHState *s = TRICORE_ETH(qemu_get_nic_opaque(nc));
    return (s->control & CTRL_RX_EN) && !s->rx_len;
}

static bool tricore_eth_rx_descriptor(TriCoreETHState *s,
                                       const uint8_t *buf, size_t len);

static ssize_t tricore_eth_receive(NetClientState *nc, const uint8_t *buf,
                                   size_t len)
{
    TriCoreETHState *s = TRICORE_ETH(qemu_get_nic_opaque(nc));
    if (!tricore_eth_can_receive(nc)) {
        return 0;
    }
    if (tricore_eth_rx_descriptor(s, buf, len)) {
        return len;
    }
    s->rx_len = MIN(len, sizeof(s->rx_buf));
    memcpy(s->rx_buf, buf, s->rx_len);
    s->rx_pos = 0;
    s->status |= STAT_RX_AVAIL;
    s->int_status |= INT_RX;
    tricore_eth_update_irq(s);
    return len;
}

static void tricore_eth_tx(TriCoreETHState *s)
{
    if ((s->control & CTRL_TX_EN) && s->tx_len) {
        qemu_send_packet(qemu_get_queue(s->nic), s->tx_buf, s->tx_len);
        s->tx_len = 0;
        s->int_status |= INT_TX;
        tricore_eth_update_irq(s);
    }
}

/* Process one iLLD four-word descriptor through QEMU system DMA. */
static void tricore_eth_tx_descriptor(TriCoreETHState *s)
{
    uint32_t d[4];
    uint8_t frame[2048];
    if (!s->tx_desc || dma_memory_read(&address_space_memory, s->tx_desc,
                                       d, sizeof(d), MEMTXATTRS_UNSPECIFIED)) return;
    for (int i = 0; i < 4; i++) d[i] = le32_to_cpu(d[i]);
    if (!(d[0] & BIT(31))) return;
    uint32_t len = MIN(d[1] & 0x7ff, sizeof(frame));
    if (!len || dma_memory_read(&address_space_memory, d[2], frame, len,
                                MEMTXATTRS_UNSPECIFIED)) return;
    if (s->vlan_ctrl & BIT(0) && len + 4 <= sizeof(frame) && len >= 14) {
        memmove(frame + 16, frame + 12, len - 12);
        stw_be_p(frame + 12, ETH_P_VLAN);
        stw_be_p(frame + 14, s->vlan_ctrl >> 16);
        len += 4;
    }
    if (s->checksum_ctrl) net_checksum_calculate(frame, len, CSUM_ALL);
    qemu_send_packet(qemu_get_queue(s->nic), frame, len);
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->tx_ts_low = now; s->tx_ts_high = now >> 32;
    d[0] &= ~BIT(31);
    d[0] |= BIT(17); /* completion timestamp/status */
    bool end = d[1] & BIT(25);
    for (int i = 0; i < 4; i++) d[i] = cpu_to_le32(d[i]);
    dma_memory_write(&address_space_memory, s->tx_desc, d, sizeof(d),
                     MEMTXATTRS_UNSPECIFIED);
    s->tx_desc = end ? s->tx_desc : s->tx_desc + 16;
    s->int_status |= INT_TX;
    tricore_eth_update_irq(s);
}

static bool tricore_eth_rx_descriptor(TriCoreETHState *s,
                                       const uint8_t *buf, size_t len)
{
    uint32_t d[4];
    if (!s->rx_desc || dma_memory_read(&address_space_memory, s->rx_desc,
                                       d, sizeof(d), MEMTXATTRS_UNSPECIFIED)) return false;
    for (int i = 0; i < 4; i++) d[i] = le32_to_cpu(d[i]);
    if (!(d[0] & BIT(31))) return false;
    uint32_t copied = MIN(MIN(len, d[1] & 0x7ff), 2048u);
    if (dma_memory_write(&address_space_memory, d[2], buf, copied,
                         MEMTXATTRS_UNSPECIFIED)) return false;
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->rx_ts_low = now; s->rx_ts_high = now >> 32;
    bool end = d[1] & BIT(25);
    d[0] = BIT(8) | BIT(9) | (copied << 16); /* LS, FS, frame length */
    for (int i = 0; i < 4; i++) d[i] = cpu_to_le32(d[i]);
    dma_memory_write(&address_space_memory, s->rx_desc, d, sizeof(d),
                     MEMTXATTRS_UNSPECIFIED);
    s->rx_desc = end ? s->rx_desc : s->rx_desc + 16;
    s->int_status |= INT_RX;
    tricore_eth_update_irq(s);
    return true;
}

static uint64_t tricore_eth_read(void *opaque, hwaddr off, unsigned size)
{
    TriCoreETHState *s = opaque;
    switch (off) {
    case ETH_CTRL: return s->control;
    case ETH_STATUS: return s->status | STAT_LINK;
    case ETH_INT_EN: return s->int_enable;
    case ETH_INT_STAT: return s->int_status;
    case ETH_TX_DESC: return s->tx_desc;
    case ETH_RX_DESC: return s->rx_desc;
    case ETH_MDIO_ADDR: return s->mdio_addr;
    case ETH_MDIO_DATA: return s->mdio_data;
    case ETH_MAC_LOW: return s->mac_low;
    case ETH_MAC_HIGH: return s->mac_high;
    case ETH_TX_LEN: return s->tx_len;
    case ETH_VLAN_CTRL: return s->vlan_ctrl;
    case ETH_CSUM_CTRL: return s->checksum_ctrl;
    case ETH_TX_TS_LO: return s->tx_ts_low;
    case ETH_TX_TS_HI: return s->tx_ts_high;
    case ETH_RX_TS_LO: return s->rx_ts_low;
    case ETH_RX_TS_HI: return s->rx_ts_high;
    case ETH_RX_LEN: return s->rx_len;
    case ETH_RX_DATA:
        return s->rx_pos < s->rx_len ? s->rx_buf[s->rx_pos++] : 0;
    default: return 0;
    }
}

static void tricore_eth_write(void *opaque, hwaddr off, uint64_t value,
                              unsigned size)
{
    TriCoreETHState *s = opaque;
    switch (off) {
    case ETH_CTRL: s->control = value; break;
    case ETH_INT_EN: s->int_enable = value; break;
    case ETH_INT_STAT: s->int_status &= ~value; break;
    case ETH_TX_DESC: s->tx_desc = value; break;
    case ETH_RX_DESC: s->rx_desc = value; break;
    case ETH_MDIO_ADDR: s->mdio_addr = value & 0x1f; s->mdio_data = s->phy_regs[s->mdio_addr]; break;
    case ETH_MDIO_DATA:
        s->mdio_data = value & 0xffff;
        s->phy_regs[s->mdio_addr] = s->mdio_data;
        s->int_status |= INT_MDIO;
        break;
    case ETH_MAC_LOW: s->mac_low = value; break;
    case ETH_MAC_HIGH: s->mac_high = value & 0xffff; break;
    case ETH_TX_LEN: s->tx_len = MIN(value, sizeof(s->tx_buf)); tricore_eth_tx(s); break;
    case ETH_TX_KICK: tricore_eth_tx_descriptor(s); break;
    case ETH_VLAN_CTRL: s->vlan_ctrl = value; break;
    case ETH_CSUM_CTRL: s->checksum_ctrl = value; break;
    case ETH_TX_DATA:
        if (s->tx_len < sizeof(s->tx_buf)) s->tx_buf[s->tx_len++] = value;
        break;
    case ETH_RX_POP:
        s->rx_len = 0; s->rx_pos = 0; s->status &= ~STAT_RX_AVAIL;
        s->int_status &= ~INT_RX;
        break;
    default: break;
    }
    tricore_eth_update_irq(s);
}

static const MemoryRegionOps tricore_eth_ops = {
    .read = tricore_eth_read, .write = tricore_eth_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1, .valid.max_access_size = 4,
};

static NetClientInfo tricore_eth_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = tricore_eth_can_receive,
    .receive = tricore_eth_receive,
};

static void tricore_eth_reset(DeviceState *dev)
{
    TriCoreETHState *s = TRICORE_ETH(dev);
    s->control = 0; s->status = 0; s->int_enable = 0; s->int_status = 0;
    s->tx_desc = s->rx_desc = 0; s->tx_len = s->rx_len = s->rx_pos = 0;
    s->vlan_ctrl = s->checksum_ctrl = 0;
    s->tx_ts_low = s->tx_ts_high = s->rx_ts_low = s->rx_ts_high = 0;
    s->mdio_addr = 1; s->mdio_data = MII_BMSR_LINK_ST | MII_BMSR_AUTONEG;
    memset(s->phy_regs, 0, sizeof(s->phy_regs));
    s->phy_regs[MII_BMCR] = MII_BMCR_AUTOEN | MII_BMCR_FD | MII_BMCR_SPEED100;
    s->phy_regs[MII_BMSR] = s->mdio_data;
    tricore_eth_update_irq(s);
}

static void tricore_eth_init(Object *obj)
{
    TriCoreETHState *s = TRICORE_ETH(obj);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static void tricore_eth_realize(DeviceState *dev, Error **errp)
{
    TriCoreETHState *s = TRICORE_ETH(dev);
    memory_region_init_io(&s->iomem, OBJECT(dev), &tricore_eth_ops, s,
                          "tricore-eth", 0x1000);
    s->nic = qemu_new_nic(&tricore_eth_net_info, &s->conf,
                          object_get_typename(OBJECT(dev)), dev->id,
                          &dev->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
}

static const Property tricore_eth_props[] = { DEFINE_NIC_PROPERTIES(TriCoreETHState, conf) };
static const VMStateDescription vmstate_tricore_eth = {
    .name = TYPE_TRICORE_ETH, .version_id = 1, .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(control, TriCoreETHState), VMSTATE_UINT32(status, TriCoreETHState),
        VMSTATE_UINT32(int_enable, TriCoreETHState), VMSTATE_UINT32(int_status, TriCoreETHState),
        VMSTATE_UINT32(tx_desc, TriCoreETHState), VMSTATE_UINT32(rx_desc, TriCoreETHState),
        VMSTATE_UINT32(mdio_addr, TriCoreETHState), VMSTATE_UINT32(mdio_data, TriCoreETHState),
        VMSTATE_UINT32(mac_low, TriCoreETHState), VMSTATE_UINT32(mac_high, TriCoreETHState),
        VMSTATE_UINT32(vlan_ctrl, TriCoreETHState), VMSTATE_UINT32(checksum_ctrl, TriCoreETHState),
        VMSTATE_UINT32(tx_ts_low, TriCoreETHState), VMSTATE_UINT32(tx_ts_high, TriCoreETHState),
        VMSTATE_UINT32(rx_ts_low, TriCoreETHState), VMSTATE_UINT32(rx_ts_high, TriCoreETHState),
        VMSTATE_UINT16(tx_len, TriCoreETHState), VMSTATE_UINT16(rx_len, TriCoreETHState),
        VMSTATE_UINT16(rx_pos, TriCoreETHState), VMSTATE_UINT8_ARRAY(tx_buf, TriCoreETHState, 2048),
        VMSTATE_UINT8_ARRAY(rx_buf, TriCoreETHState, 2048), VMSTATE_UINT16_ARRAY(phy_regs, TriCoreETHState, 32),
        VMSTATE_END_OF_LIST()
    }
};

static void tricore_eth_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
    dc->realize = tricore_eth_realize;
    device_class_set_legacy_reset(dc, tricore_eth_reset);
    device_class_set_props(dc, tricore_eth_props);
    dc->vmsd = &vmstate_tricore_eth;
}

static const TypeInfo tricore_eth_info = {
    .name = TYPE_TRICORE_ETH, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(TriCoreETHState), .instance_init = tricore_eth_init,
    .class_init = tricore_eth_class_init,
};

/* Keep GETH and LETH as distinct QOM devices even though their deterministic
 * packet engine is shared; this lets SoC code and guests select the proper
 * documented peripheral without duplicating transport logic. */
static const TypeInfo tricore_geth_info = {
    .name = TYPE_TRICORE_GETH, .parent = TYPE_TRICORE_ETH,
};
static const TypeInfo tricore_leth_info = {
    .name = TYPE_TRICORE_LETH, .parent = TYPE_TRICORE_ETH,
};

static void tricore_eth_register_types(void)
{
    type_register_static(&tricore_eth_info);
    type_register_static(&tricore_geth_info);
    type_register_static(&tricore_leth_info);
}

type_init(tricore_eth_register_types)
