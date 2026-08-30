#ifndef HW_DMA_TRICORE_DMA_H
#define HW_DMA_TRICORE_DMA_H

#include "hw/core/sysbus.h"

#define TYPE_TRICORE_DMA "tricore-dma"
OBJECT_DECLARE_SIMPLE_TYPE(TriCoreDMAState, TRICORE_DMA)
enum TriCoreDMARequest {
    TRICORE_DMA_REQ_ASCLIN0 = 1, TRICORE_DMA_REQ_MCAN0 = 16,
    TRICORE_DMA_REQ_ERAY0 = 32, TRICORE_DMA_REQ_ETH = 48,
};
void tricore_dma_request(TriCoreDMAState *s, uint32_t request);
/* Public request matrix IDs shared by TC2x/TC3x/TC4x profiles. */
extern const uint8_t tricore_dma_request_matrix[3][4];

struct TriCoreDMAState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t src, dst, length, control, status, descriptor, request;
    uint32_t accen, error_enable, priority;
};

#endif
