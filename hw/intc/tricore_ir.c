/*
 * QEMU TriCore Interrupt Router Bus.
 *
 * Copyright (c) 2017 David Brenken <david.brenken@efs-auto.de>
 * Copyright (c) 2026 Parthiban Nallathambi <parthiban@linumiz.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev.h"
#include "hw/core/registerfields.h"
#include "hw/core/sysbus.h"
#include "hw/intc/tricore_ir.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/typedefs.h"
#include "glib.h"


static void irq_evaluate(void *opaque)
{
    TriCoreIRState *pv = opaque;
    uint16_t tos_irq[8] = { 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
                            0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF };
    uint8_t tos_priority[8] = { 0, 0, 0, 0, 0, 0, 0, 0 };

    for (uint32_t srcnum = 0; srcnum < pv->num_irqs; srcnum++) {
        uint32_t src_reg = pv->src_regs[srcnum];
        uint8_t priority = FIELD_EX32(src_reg, SRC, SRPN);
        uint8_t tos = pv->tc4x_mode ? FIELD_EX32(src_reg, SRC_TC4X, TOS) :
                                      FIELD_EX32(src_reg, SRC_TC3X, TOS);

        if ((src_reg & R_SRC_SRR_MASK) &&
            (pv->tc4x_mode ? (src_reg & R_SRC_TC4X_SRE_MASK) :
                             (src_reg & R_SRC_TC3X_SRE_MASK))) {
            if (qemu_loglevel_mask(CPU_LOG_INT)) {
                qemu_log("tricore_ir: pending irq #%d (priority %d, TOS %d)\n",
                         srcnum, priority, tos);
            }
            tos_priority[tos] = priority;
            tos_irq[tos] = srcnum;
        }
    }

    uint8_t tos_idx;
    for (tos_idx = 0; tos_idx < pv->num_isps; tos_idx++) {
        if (tos_irq[tos_idx] == 0xFFFF) {
            pv->lwsr[tos_idx] = 0;
            if (qemu_loglevel_mask(CPU_LOG_INT)) {
                qemu_log("tricore_ir: lower TOS %d irq line\n", tos_idx);
            }
            qemu_irq_lower(pv->isp_irqs[tos_idx]);
        } else {
            pv->lwsr[tos_idx] = FIELD_DP32(0, LWSR, STAT, 1) |
                                FIELD_DP32(0, LWSR, ID, tos_irq[tos_idx]) |
                                FIELD_DP32(0, LWSR, VALID, 1) |
                                FIELD_DP32(0, LWSR, PN, tos_priority[tos_idx]);

            if (qemu_loglevel_mask(CPU_LOG_INT)) {
                qemu_log("tricore_ir: raise TOS %d irq line (irq: %d, "
                         "priority: %d)\n",
                         tos_idx, tos_irq[tos_idx], tos_priority[tos_idx]);
            }
            qemu_irq_raise(pv->isp_irqs[tos_idx]);
        }
    }
}

static void irq_handler(void *opaque, int srcnum, int level)
{
    TriCoreIRState *pv = opaque;
    uint32_t src_reg = pv->src_regs[srcnum];

    if (level) {
        if (src_reg & R_SRC_SRR_MASK) {
            src_reg |= R_SRC_IOV_MASK;
            if (qemu_loglevel_mask(CPU_LOG_INT)) {
                qemu_log("tricore_ir: SRC #%d overflow\n", srcnum);
            }
        }
        src_reg |= R_SRC_SRR_MASK;
    }
    pv->src_regs[srcnum] = src_reg;

    if (qemu_loglevel_mask(CPU_LOG_INT)) {
        qemu_log("tricore_ir: SRC #%d level %d\n", srcnum, level);
    }

    irq_evaluate(opaque);
}

void tricore_ir_irq_acknowledge(TriCoreIRState *s, uint16_t irq, uint8_t vm)
{
    uint32_t src_reg;

    if (!s || irq >= s->num_irqs || vm >= ARRAY_SIZE(s->lwsr)) {
        return;
    }

    if (qemu_loglevel_mask(CPU_LOG_INT)) {
        qemu_log("tricore_ir: Acknowledge IRQ %d for VM %d\n", irq, vm);
    }

    src_reg = s->src_regs[irq];
    if (s->tc4x_mode && FIELD_EX32(src_reg, SRC, VM) != vm) {
        return;
    }

    src_reg &= ~R_SRC_SRR_MASK;
    s->src_regs[irq] = src_reg;

    if (FIELD_EX32(s->lwsr[0], LWSR, ID) == irq) {
        s->lwsr[0] &= ~R_LWSR_STAT_MASK;
    }

    irq_evaluate(s);
}

static uint64_t tricore_ir_src_regs_read(void *opaque, hwaddr offset,
                                         unsigned size)
{
    TriCoreIRState *s = (TriCoreIRState *)opaque;
    hwaddr srcnum = offset >> 2;

    if (srcnum < s->num_irqs) {
        return s->src_regs[srcnum];
    }

    return 0;
}

static void tricore_ir_src_regs_write(void *opaque, hwaddr offset,
                                      uint64_t value, unsigned size)
{
    TriCoreIRState *s = (TriCoreIRState *)opaque;
    hwaddr srcnum = offset >> 2;

    if (srcnum >= s->num_irqs) {
        if (qemu_loglevel_mask(CPU_LOG_INT)) {
            qemu_log(
                "tricore_ir: write to unmapped SRC offset 0x" HWADDR_FMT_plx
                "\n",
                offset);
        }
        return;
    }

    uint32_t srcc = (value & ~(R_SRC_SETR_MASK | R_SRC_CLRR_MASK));
    bool setr = value & R_SRC_SETR_MASK;
    bool clrr = value & R_SRC_CLRR_MASK;

    if (setr && !clrr) {
        srcc |= R_SRC_SRR_MASK;
    } else if (clrr && !setr) {
        srcc &= ~R_SRC_SRR_MASK;
    }

    s->src_regs[srcnum] = srcc;

    if (qemu_loglevel_mask(CPU_LOG_INT)) {
        qemu_log("tricore_ir: SRC %lu now %s (SRPN %d)\n", srcnum,
                 (srcc &
                  (s->tc4x_mode ? R_SRC_TC4X_SRE_MASK : R_SRC_TC3X_SRE_MASK)) ?
                     "enabled" :
                     "disabled",
                 FIELD_EX32(srcc, SRC, SRPN));
    }

    irq_evaluate(opaque);
}


static const MemoryRegionOps tricore_ir_src_regs_ops = {
        .read = tricore_ir_src_regs_read,
        .write = tricore_ir_src_regs_write,
        .valid = {.min_access_size = 1, .max_access_size = 4, },
        .endianness = DEVICE_LITTLE_ENDIAN, };

/*
 * IR INT registers (LWSR, LASR, VMEN etc.)
 * LWSR offset: 0x0C00 + z*0x34 + y*4 (z=ICU, y=VM)
 * For single-CPU QEMU: z=0 only
 */
#define IR_INT_LWSR_BASE 0x0C00
#define IR_INT_ICU_STRIDE 0x34
#define IR_INT_LASR_OFF 0x20
#define IR_INT_VMEN_OFF 0x30
#define IR_INT_ID_OFF 0x08

/* Module ID: IR module number 0x00B9, type 0xC0, rev 0x13 */
#define IR_INT_MOD_ID 0x00B9C013

static uint64_t tricore_ir_intregs_read(void *opaque, hwaddr offset,
                                        unsigned size)
{
    TriCoreIRState *s = (TriCoreIRState *)opaque;

    if (offset == IR_INT_ID_OFF) {
        return IR_INT_MOD_ID;
    }

    /* LWSR: 0x0C00 + z*0x34 + y*4 */
    if (offset >= IR_INT_LWSR_BASE &&
        offset < IR_INT_LWSR_BASE + IR_INT_ICU_STRIDE) {
        uint32_t y = (offset - IR_INT_LWSR_BASE) >> 2;
        if (y < 8) {
            return s->lwsr[y];
        }
    }

    /* VMEN: 0x0C30 for ICU0 */
    if (offset == IR_INT_LWSR_BASE + IR_INT_VMEN_OFF) {
        return 0xFF;
    }

    /* LASR: 0x0C20 for ICU0 */
    if (offset == IR_INT_LWSR_BASE + 0x20) {
        return s->lasr;
    }

    return 0;
}

static void tricore_ir_intregs_write(void *opaque, hwaddr offset,
                                     uint64_t value, unsigned size)
{
    /* LWSR/LASR are read-only from software side */
}

static const MemoryRegionOps tricore_ir_intregs_ops = {
        .read = tricore_ir_intregs_read,
        .write = tricore_ir_intregs_write,
        .valid = {.min_access_size = 4, .max_access_size = 4, },
        .endianness = DEVICE_LITTLE_ENDIAN, };


static void tricore_ir_init(Object *obj)
{
    TriCoreIRState *pv = TRICORE_IR(obj);

    memory_region_init_io(&pv->src_region, OBJECT(pv), &tricore_ir_src_regs_ops,
                          pv, "tricore_ir.src", 0x4000);
    memory_region_init_io(&pv->int_region, OBJECT(pv), &tricore_ir_intregs_ops,
                          pv, "tricore_ir.int", 0x1000);
}

static void tricore_ir_realize(DeviceState *dev, Error **errp)
{
    struct TriCoreIRState *pv = TRICORE_IR(dev);

    pv->src_regs = g_malloc0_n(pv->num_irqs, sizeof(uint32_t));
    pv->isp_irqs = g_malloc_n(pv->num_isps, sizeof(qemu_irq));
    qdev_init_gpio_in_named(DEVICE(pv), irq_handler, "irq", pv->num_irqs);
    qdev_init_gpio_out_named(DEVICE(pv), pv->isp_irqs, "isp", pv->num_isps);

    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &pv->int_region);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &pv->src_region);
}

static const Property tricore_ir_properties[] = {
    DEFINE_PROP_BOOL("tc4x-mode", TriCoreIRState, tc4x_mode, false),
    DEFINE_PROP_UINT8("num-isps", TriCoreIRState, num_isps, 1),
    DEFINE_PROP_UINT16("num-irqs", TriCoreIRState, num_irqs, 256),
};

static void tricore_ir_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->user_creatable = false;
    dc->realize = tricore_ir_realize;
    device_class_set_props(dc, tricore_ir_properties);
}

static TypeInfo tricore_ir_info = {
    .name = "tricore_ir",
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(TriCoreIRState),
    .instance_init = tricore_ir_init,
    .class_init = tricore_ir_class_init,
};

static void tricore_ir_register(void)
{
    type_register_static(&tricore_ir_info);
}

type_init(tricore_ir_register)
