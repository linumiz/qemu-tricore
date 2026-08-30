#ifndef HW_DMA_TRICORE_DMA_H
#define HW_DMA_TRICORE_DMA_H

#include "hw/core/sysbus.h"

#define TYPE_TRICORE_DMA "tricore-dma"
OBJECT_DECLARE_SIMPLE_TYPE(TriCoreDMAState, TRICORE_DMA)

struct TriCoreDMAState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t src, dst, length, control, status, descriptor;
};

#endif
