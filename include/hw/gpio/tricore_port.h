#ifndef HW_GPIO_TRICORE_PORT_H
#define HW_GPIO_TRICORE_PORT_H
#include "hw/core/sysbus.h"
#define TYPE_TRICORE_PORT "tricore-port"
OBJECT_DECLARE_SIMPLE_TYPE(TriCorePortState, TRICORE_PORT)
struct TriCorePortState { SysBusDevice parent_obj; MemoryRegion iomem; qemu_irq out[16]; uint32_t in, out_latch, dir, pdisc, palt; };
#endif
