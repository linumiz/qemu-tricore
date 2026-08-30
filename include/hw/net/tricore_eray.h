/*
 * Public-source provenance: this interface reflects only the public AURIX
 * ERAY User Manuals and Infineon IfxEray_* headers published on GitHub.
 * Workspace copies of those iLLDs are kept for reference and are not
 * project-provided source; NDA-only implementation details are excluded.
 */
#ifndef HW_NET_TRICORE_ERAY_H
#define HW_NET_TRICORE_ERAY_H

#include "hw/core/sysbus.h"
#include "qemu/queue.h"
#include "qemu/timer.h"

/* Register/message layout follows the public AURIX ERAY User Manuals and
 * the publicly available Infineon IfxEray_* headers. */
#define TYPE_TRICORE_ERAY "tricore-eray"
OBJECT_DECLARE_SIMPLE_TYPE(TriCoreERAYState, TRICORE_ERAY)

typedef struct TriCoreERAYState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    MemoryRegion msg_ram;
    qemu_irq int0_irq;
    qemu_irq int1_irq;
    uint32_t ccsv, ccev, succ1, nemc, mbsc1, ndat1;
    uint32_t command;
    uint32_t cycle;
    uint32_t slot_status;
    uint32_t mbid, mbctrl;
    QEMUTimer *scheduler;
    QTAILQ_ENTRY(TriCoreERAYState) bus_node;
    uint8_t msg_data[16 * 1024];
} TriCoreERAYState;

#endif
