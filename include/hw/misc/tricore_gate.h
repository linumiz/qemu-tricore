#ifndef HW_MISC_TRICORE_GATE_H
#define HW_MISC_TRICORE_GATE_H

#include "hw/core/sysbus.h"

#define TYPE_TRICORE_GATE "tricore-gate"
OBJECT_DECLARE_SIMPLE_TYPE(TriCoreGateState, TRICORE_GATE)

struct TriCoreGateState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t clock_enable;
    uint32_t reset_request;
    uint32_t status;
};

#endif
