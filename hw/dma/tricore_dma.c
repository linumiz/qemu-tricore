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
#define DMA_STAT_DESC_ERROR BIT(2)
#define DMA_STAT_EOL BIT(3)
#define DMA_CTL_CHAIN BIT(2)

static uint64_t dma_read(void *opaque, hwaddr off, unsigned size)
{
    TriCoreDMAState *s = opaque;
    switch (off) { case DMA_SRC: return s->src; case DMA_DST: return s->dst;
    case DMA_LEN: return s->length; case DMA_CTL: return s->control;
    case DMA_STAT: return s->status; case 0x14: return s->descriptor;
    default: return 0; }
}

static void dma_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    TriCoreDMAState *s = opaque;
    switch (off) {
    case DMA_SRC: s->src = value; break;
    case DMA_DST: s->dst = value; break;
    case DMA_LEN: s->length = value; break;
    case DMA_STAT: s->status &= ~(value & (DMA_STAT_DONE | DMA_STAT_ERROR |
                                             DMA_STAT_DESC_ERROR | DMA_STAT_EOL)); break;
    case 0x14: s->descriptor = value; break;
    case DMA_CTL:
        s->control = value & (DMA_CTL_START | DMA_CTL_IRQ | DMA_CTL_CHAIN);
        if (value & DMA_CTL_START) {
            MemTxResult r = MEMTX_OK;
            unsigned count = 0;
            do {
                uint32_t src = s->src, dst = s->dst, len = s->length, next = 0;
                if ((value & DMA_CTL_CHAIN) && s->descriptor) {
                    uint32_t d[4];
                    r = address_space_read(&address_space_memory, s->descriptor,
                                           MEMTXATTRS_UNSPECIFIED, d, sizeof(d));
                    if (r != MEMTX_OK || !d[2] || d[2] > 16 * 1024 * 1024) {
                        s->status = DMA_STAT_DESC_ERROR;
                        break;
                    }
                    src = le32_to_cpu(d[0]); dst = le32_to_cpu(d[1]);
                    len = le32_to_cpu(d[2]); next = le32_to_cpu(d[3]);
                }
                uint8_t *buf = g_malloc(len);
                r = address_space_read(&address_space_memory, src,
                                       MEMTXATTRS_UNSPECIFIED, buf, len);
                if (r == MEMTX_OK) r = address_space_write(&address_space_memory, dst,
                                                           MEMTXATTRS_UNSPECIFIED, buf, len);
                g_free(buf);
                if (r != MEMTX_OK) break;
                if (!(value & DMA_CTL_CHAIN) || !next) {
                    s->status = DMA_STAT_DONE | ((value & DMA_CTL_CHAIN) ? DMA_STAT_EOL : 0);
                    break;
                }
                s->descriptor = next;
            } while (++count < 256);
            if (count == 256) s->status = DMA_STAT_DESC_ERROR;
            if (r != MEMTX_OK && !(s->status & DMA_STAT_DESC_ERROR)) s->status = DMA_STAT_ERROR;
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
