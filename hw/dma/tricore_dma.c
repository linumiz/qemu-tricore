#include "qemu/osdep.h"
#include "hw/dma/tricore_dma.h"
#include "system/address-spaces.h"
#include "system/memory.h"
#include "hw/core/irq.h"

/* Public AURIX DMA channel model: a programmed move-engine request is
 * completed deterministically and raises the channel completion interrupt. */
#define DMA_SRC 0x00
#define DMA_DST 0x04
#define DMA_LEN 0x08
#define DMA_CTL 0x0c
#define DMA_STAT 0x10
#define DMA_CTL_START BIT(0)
#define DMA_CTL_IRQ BIT(1)
#define DMA_STAT_DONE BIT(0)
#define DMA_STAT_ERROR BIT(1)

static uint64_t dma_read(void *opaque, hwaddr off, unsigned size)
{
    TriCoreDMAState *s = opaque;
    switch (off) { case DMA_SRC: return s->src; case DMA_DST: return s->dst;
    case DMA_LEN: return s->length; case DMA_CTL: return s->control;
    case DMA_STAT: return s->status; default: return 0; }
}

static void dma_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    TriCoreDMAState *s = opaque;
    switch (off) {
    case DMA_SRC: s->src = value; break;
    case DMA_DST: s->dst = value; break;
    case DMA_LEN: s->length = value; break;
    case DMA_STAT: s->status &= ~(value & (DMA_STAT_DONE | DMA_STAT_ERROR)); break;
    case DMA_CTL:
        s->control = value & (DMA_CTL_START | DMA_CTL_IRQ);
        if (value & DMA_CTL_START) {
            uint8_t *buf = g_malloc(s->length);
            MemTxResult r = address_space_read(&address_space_memory, s->src,
                                               MEMTXATTRS_UNSPECIFIED, buf, s->length);
            if (r == MEMTX_OK) r = address_space_write(&address_space_memory, s->dst,
                                                       MEMTXATTRS_UNSPECIFIED, buf, s->length);
            g_free(buf);
            s->status = (r == MEMTX_OK) ? DMA_STAT_DONE : DMA_STAT_ERROR;
            if ((value & DMA_CTL_IRQ) && r == MEMTX_OK) {
                qemu_set_irq(s->irq, 1);
                qemu_set_irq(s->irq, 0);
            }
        }
        break;
    default: break;
    }
}

static const MemoryRegionOps dma_ops = { .read = dma_read, .write = dma_write,
    .endianness = DEVICE_LITTLE_ENDIAN, .valid.min_access_size = 4,
    .valid.max_access_size = 4 };

static void dma_init(Object *obj)
{
    TriCoreDMAState *s = TRICORE_DMA(obj);
    memory_region_init_io(&s->iomem, obj, &dma_ops, s, "tricore-dma", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static const TypeInfo dma_type = { .name = TYPE_TRICORE_DMA,
    .parent = TYPE_SYS_BUS_DEVICE, .instance_size = sizeof(TriCoreDMAState),
    .instance_init = dma_init };
static void dma_register_types(void) { type_register_static(&dma_type); }
type_init(dma_register_types)
