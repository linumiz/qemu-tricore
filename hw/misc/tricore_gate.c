#include "qemu/osdep.h"
#include "hw/misc/tricore_gate.h"

/* The public SCU/CCU manuals expose gate and reset request bitmaps.  This
 * model keeps those bitmaps visible to firmware; peripherals consume the
 * same state through the SoC integration layer. */
static uint64_t gate_read(void *opaque, hwaddr off, unsigned size)
{
    TriCoreGateState *s = opaque;
    switch (off) {
    case 0x00: return s->clock_enable;
    case 0x04: return s->reset_request;
    case 0x08: return s->status;
    default: return 0;
    }
}

static void gate_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    TriCoreGateState *s = opaque;
    uint32_t v = value;
    switch (off) {
    case 0x00:
        s->clock_enable = v;
        s->status = (s->status & ~0xffff0000u) | ((v & 0xffffu) << 16);
        break;
    case 0x04:
        /* Reset requests are write-one-to-pulse and do not latch forever. */
        s->reset_request |= v;
        s->status |= v;
        break;
    case 0x08:
        /* Status is write-one-to-clear, matching SCU event registers. */
        s->status &= ~v;
        s->reset_request &= ~v;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps gate_ops = {
    .read = gate_read, .write = gate_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4, .valid.max_access_size = 4,
};

static void gate_init(Object *obj)
{
    TriCoreGateState *s = TRICORE_GATE(obj);
    memory_region_init_io(&s->iomem, obj, &gate_ops, s,
                          "tricore-clock-reset-gate", 0x100);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    s->clock_enable = UINT32_MAX;
}

static void gate_reset(Object *obj, ResetType type)
{
    TriCoreGateState *s = TRICORE_GATE(obj);
    s->clock_enable = UINT32_MAX;
    s->reset_request = 0;
    s->status = 0;
}

static void gate_class_init(ObjectClass *klass, const void *data)
{
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    rc->phases.hold = gate_reset;
}

static const TypeInfo gate_type = {
    .name = TYPE_TRICORE_GATE, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(TriCoreGateState), .instance_init = gate_init,
    .class_init = gate_class_init,
};

static void gate_register_types(void)
{
    type_register_static(&gate_type);
}

type_init(gate_register_types)
