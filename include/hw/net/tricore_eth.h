#ifndef HW_NET_TRICORE_ETH_H
#define HW_NET_TRICORE_ETH_H

#include "hw/core/sysbus.h"
#include "net/net.h"

#define TYPE_TRICORE_ETH "tricore-eth"
OBJECT_DECLARE_SIMPLE_TYPE(TriCoreETHState, TRICORE_ETH)

typedef struct TriCoreETHState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    NICState *nic;
    NICConf conf;
    uint32_t control, status, int_enable, int_status;
    uint32_t tx_desc, rx_desc, mdio_addr, mdio_data;
    uint32_t mac_low, mac_high;
    uint32_t vlan_ctrl, checksum_ctrl;
    uint8_t tx_buf[2048];
    uint8_t rx_buf[2048];
    uint16_t tx_len, rx_len, rx_pos;
    uint16_t phy_regs[32];
} TriCoreETHState;

#endif
