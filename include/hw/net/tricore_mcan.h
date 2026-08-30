#ifndef HW_TRICORE_MCAN_H
#define HW_TRICORE_MCAN_H

#include "hw/core/sysbus.h"
#include "net/can_emu.h"

#define TYPE_TRICORE_MCAN "tricore-mcan"
OBJECT_DECLARE_SIMPLE_TYPE(TriCoreMCANState, TRICORE_MCAN)

struct TriCoreMCANState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq[16];
    CanBusClientState bus_client;
    CanBusState *canbus;
    uint32_t regs[0x3000 / 4];
    qemu_can_frame rx_frame;
    bool rx_pending;
    uint8_t enabled_nodes;
    qemu_can_frame rx_fifo[8];
    uint8_t rx_fifo_head, rx_fifo_tail, rx_fifo_count;
};

#endif
