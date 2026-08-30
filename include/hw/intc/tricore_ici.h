#ifndef HW_INTC_TRICORE_ICI_H
#define HW_INTC_TRICORE_ICI_H
#include "hw/core/sysbus.h"
#define TYPE_TRICORE_ICI "tricore-ici"
OBJECT_DECLARE_SIMPLE_TYPE(TriCoreICIState, TRICORE_ICI)
struct TriCoreICIState { SysBusDevice parent_obj; MemoryRegion iomem; qemu_irq irq[6]; uint32_t pending, reset_mask, provider; };
#endif
