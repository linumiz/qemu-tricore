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
#include "qapi/error.h"

#include "qemu/log.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "qemu/main-loop.h"
#include "cpu.h"
#include "qemu/error-report.h"
#include "hw/intc/tricore_irbus.h"

static inline uint32_t ir_sre(TriCoreIRBUSState *s)
{
    return s->tc4x_mode ? IR_SRC_SRE_TC4X : IR_SRC_SRE_TC3X;
}

static inline uint32_t ir_srr(TriCoreIRBUSState *s)
{
    return s->tc4x_mode ? IR_SRC_SRR_TC4X : IR_SRC_SRR_TC3X;
}

static inline uint32_t ir_setr(TriCoreIRBUSState *s)
{
    return s->tc4x_mode ? IR_SRC_SETR_TC4X : IR_SRC_SETR_TC3X;
}

static inline uint32_t ir_clrr(TriCoreIRBUSState *s)
{
    return s->tc4x_mode ? IR_SRC_CLRR_TC4X : IR_SRC_CLRR_TC3X;
}

static const char *get_name_by_src(int srcnum)
{
    switch (srcnum) {
    case IR_SRC_ASCLIN0TX:
        return "SRC_ASCLIN0TX";
    case IR_SRC_ASCLIN0RX:
        return "SRC_ASCLIN0RX";
    case IR_SRC_ASCLIN0EX:
        return "SRC_ASCLIN0EX";
    case IR_SRC_STM0_SR0:
        return "SRC_STM0SR0";
    case IR_SRC_STM0_SR1:
        return "SRC_STM0SR1";
    case IR_SRC_STM1_SR0:
        return "SRC_STM1SR0";
    case IR_SRC_STM1_SR1:
        return "SRC_STM1SR1";
    case IR_SRC_STM2_SR0:
        return "SRC_STM2SR0";
    case IR_SRC_STM2_SR1:
        return "SRC_STM2SR1";
    case IR_SRC_RESET:
        return "RESET";
    default:
        return "";
    }
}

static int reg_addr_to_srcnum(bool tc4x_mode, hwaddr reg_addr)
{
    if (tc4x_mode) {
        switch (reg_addr) {
        /* SRC_STMCPUwSRx: offset 0x020 + w*0x40 + x*4
         * reg_addr = offset / 4 */
        case 0x8 ... 0x17:
            return IR_SRC_STM0_SR0;
        case 0x18 ... 0x1F:
            return IR_SRC_STM1_SR0;
        case 0x28 ... 0x37:
            return IR_SRC_STM2_SR0;
        /* SRC_ASCLINwTX: offset 0x2B0 + w*12 */
        case 0xAC:
            return IR_SRC_ASCLIN0TX;
        case 0xAD:
            return IR_SRC_ASCLIN0RX;
        case 0xAE:
            return IR_SRC_ASCLIN0EX;
        default:
            return -1;
        }
    }

    /* TC2x/TC3x */
    switch (reg_addr) {
    /* TC39XB: ASCLIN0 TX/RX/ERR (IRQ 20/21/22) */
    case 0x14:
        return IR_SRC_ASCLIN0TX;
    case 0x15:
        return IR_SRC_ASCLIN0RX;
    case 0x16:
        return IR_SRC_ASCLIN0EX;
    /* TC27XD: ASCLIN0 TX/RX/ERR at SRC IDX 0x20/0x21/0x22 */
    case 0x20:
        return IR_SRC_ASCLIN0TX;
    case 0x21:
        return IR_SRC_ASCLIN0RX;
    case 0x22:
        return IR_SRC_ASCLIN0EX;
    /* TC39XB: STM0-STM2 (IRQ 192-197) */
    case 0xC0:
        return IR_SRC_STM0_SR0;
    case 0xC1:
        return IR_SRC_STM0_SR1;
    case 0xC2:
        return IR_SRC_STM1_SR0;
    case 0xC3:
        return IR_SRC_STM1_SR1;
    case 0xC4:
        return IR_SRC_STM2_SR0;
    case 0xC5:
        return IR_SRC_STM2_SR1;
    /* TC27XD: STM at SRC IDX 0x124-0x129 */
    case 0x124:
        return IR_SRC_STM0_SR0;
    case 0x125:
        return IR_SRC_STM0_SR1;
    case 0x126:
        return IR_SRC_STM1_SR0;
    case 0x127:
        return IR_SRC_STM1_SR1;
    case 0x128:
        return IR_SRC_STM2_SR0;
    case 0x129:
        return IR_SRC_STM2_SR1;
    /* TC39XB: STM0 SR0/SR1 at SRC offset 0x0C0/0x0C4 -> IDX 0x30/0x31 */
    case 0x30:
        return IR_SRC_STM0_SR0;
    case 0x31:
        return IR_SRC_STM0_SR1;
    case 0x32:
        return IR_SRC_STM1_SR0;
    case 0x33:
        return IR_SRC_STM1_SR1;
    case 0x34:
        return IR_SRC_STM2_SR0;
    case 0x35:
        return IR_SRC_STM2_SR1;
    /* TC39XB: ASCLIN0 TX/RX/ERR at SRC offset 0x200/0x204/0x208 -> IDX 0x80/0x81/0x82 */
    case 0x80:
        return IR_SRC_ASCLIN0TX;
    case 0x81:
        return IR_SRC_ASCLIN0RX;
    case 0x82:
        return IR_SRC_ASCLIN0EX;
    default:
        return -1;
    }
}

/* TC4x SRC.VM field: bits 10:8 */
#define IR_SRC_VM_SHIFT  8
#define IR_SRC_VM_MASK   0x700

static void irq_evaluate(void *opaque)
{
    TriCoreIRBUSState *pv = opaque;
    CPUTriCoreState *env = &((TriCoreCPU *) (pv->cpu))->env;
    uint32_t sre_mask = ir_sre(pv);
    uint32_t srr_mask = ir_srr(pv);

    for (uint32_t srcnum = 0; srcnum < IR_SRC_COUNT; srcnum++) {
        uint32_t src_reg = pv->src_control_reg[srcnum];

        if ((src_reg & srr_mask) && ((src_reg & sre_mask) || (srcnum == IR_SRC_RESET))) {

            if (qemu_loglevel_mask(CPU_LOG_INT)) {
                qemu_log("tricore_irbus: SRC #%d (%s) (SRPN %d) triggered\n",
                srcnum, get_name_by_src(srcnum), (src_reg & IR_SRC_SRPN));
            }

            /*
             * Do NOT auto-clear SRR here. On real HW, SRR is cleared
             * on ISP acknowledge. In QEMU level-triggered model, SRR
             * is cleared when the peripheral deasserts (irq_handler
             * level=0). Clearing here causes a race: any subsequent
             * irq_evaluate call from another SRC finds nothing pending
             * and lowers parent_irq before the CPU takes the interrupt.
             */

            env->ICR = (env->ICR & (~MASK_ICR_PIPN)) |
                ((src_reg & IR_SRC_SRPN) << 16);

            if (pv->tc4x_mode) {
                uint32_t vm = (src_reg & IR_SRC_VM_MASK) >> IR_SRC_VM_SHIFT;
                uint32_t src_idx = pv->src_regaddr[srcnum];
                pv->lwsr[vm] = (1u << 31)
                             | ((uint32_t)src_idx << 16)
                             | (1u << 12)
                             | (src_reg & IR_SRC_SRPN);
                pv->lasr = pv->lwsr[vm];
            } else {
                pv->lasr = (1u << 31)
                         | ((uint32_t)pv->src_regaddr[srcnum] << 16)
                         | (src_reg & IR_SRC_SRPN);
            }

            qemu_irq_raise(pv->parent_irq);
            return;
        }
    }

    env->ICR &= ~MASK_ICR_PIPN;

    if (qemu_loglevel_mask(CPU_LOG_INT)) {
        qemu_log("tricore_irbus: lower irq line\n");
    }
    qemu_irq_lower(pv->parent_irq);
}

static void irq_handler(void *opaque, int srcnum, int level)
{
    TriCoreIRBUSState *pv = opaque;
    uint32_t src_reg = pv->src_control_reg[srcnum];
    uint32_t srr_mask = ir_srr(pv);

    if (level) {
        if (src_reg & srr_mask) {
            return;
        }
        src_reg |= srr_mask;
    } else {
        src_reg &= ~srr_mask;
    }
    pv->src_control_reg[srcnum] = src_reg;

    if (qemu_loglevel_mask(CPU_LOG_INT)) {
        qemu_log("tricore_irbus: SRC #%d (%s) level %d\n",
        srcnum, get_name_by_src(srcnum), level);
    }

    irq_evaluate(opaque);
}

static uint64_t tricore_irbus_srvcontrolregs_read(void *opaque, hwaddr offset,
        unsigned size)
{
    TriCoreIRBUSState *s = (TriCoreIRBUSState *) opaque;
    hwaddr reg_addr = offset >> 2;
    int srcnum = reg_addr_to_srcnum(s->tc4x_mode, reg_addr);

    if (srcnum >= 0) {
        return s->src_control_reg[srcnum];
    }

    return 0;
}

static void tricore_irbus_srvcontrolregs_write(void *opaque, hwaddr offset,
        uint64_t value, unsigned size)
{
    TriCoreIRBUSState *s = (TriCoreIRBUSState *) opaque;
    hwaddr reg_addr = offset >> 2;
    int srcnum = reg_addr_to_srcnum(s->tc4x_mode, reg_addr);

    if (srcnum < 0) {
        if (qemu_loglevel_mask(CPU_LOG_INT)) {
            qemu_log("tricore_irbus: write to unmapped SRC offset 0x"
                     HWADDR_FMT_plx "\n", offset);
        }
        return;
    }

    /* track the actual SRC register offset for LWSR.ID */
    s->src_regaddr[srcnum] = (uint16_t)reg_addr;

    uint32_t srcc = s->src_control_reg[srcnum];
    uint32_t setr_mask = ir_setr(s);
    uint32_t clrr_mask = ir_clrr(s);
    uint32_t srr_mask = ir_srr(s);

    memcpy(((void *) &srcc) + (offset & 0x3), &value, size);

    bool setr = srcc & setr_mask;
    bool clrr = srcc & clrr_mask;

    if (setr && !clrr) {
        srcc |= srr_mask;
    } else if (clrr && !setr) {
        srcc &= ~srr_mask;
    }
    srcc &= ~(setr_mask | clrr_mask);

    s->src_control_reg[srcnum] = srcc;

    if (qemu_loglevel_mask(CPU_LOG_INT)) {
        qemu_log("tricore_irbus: SRC %s now %s (SRPN %d)\n",
        get_name_by_src(srcnum),
        (srcc & ir_sre(s)) ? "enabled" : "disabled",
        (srcc & IR_SRC_SRPN));
    }

    irq_evaluate(opaque);
}


static const MemoryRegionOps tricore_irbus_srvcontrolregs_ops = {
        .read = tricore_irbus_srvcontrolregs_read,
        .write = tricore_irbus_srvcontrolregs_write,
        .valid = {.min_access_size = 1, .max_access_size = 4, },
        .endianness = DEVICE_LITTLE_ENDIAN, };

/*
 * IR INT registers (LWSR, LASR, VMEN etc.)
 * LWSR offset: 0x0C00 + z*0x34 + y*4 (z=ICU, y=VM)
 * For single-CPU QEMU: z=0 only
 */
#define IR_INT_LWSR_BASE   0x0C00
#define IR_INT_ICU_STRIDE  0x34
#define IR_INT_LASR_OFF    0x20
#define IR_INT_VMEN_OFF    0x30
#define IR_INT_ID_OFF      0x08

/* Module ID: IR module number 0x00B9, type 0xC0, rev 0x13 */
#define IR_INT_MOD_ID      0x00B9C013

static uint64_t tricore_irbus_intregs_read(void *opaque, hwaddr offset,
        unsigned size)
{
    TriCoreIRBUSState *s = (TriCoreIRBUSState *) opaque;

    if (offset == IR_INT_ID_OFF) {
        return IR_INT_MOD_ID;
    }

    /* TC3x LASR: 0x200 + ICU*0x10 + 0x04 */
    if (!s->tc4x_mode && offset >= 0x200 && offset < 0x280) {
        return s->lasr;
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

static void tricore_irbus_intregs_write(void *opaque, hwaddr offset,
        uint64_t value, unsigned size)
{
    /* LWSR/LASR are read-only from software side */
}

static const MemoryRegionOps tricore_irbus_intregs_ops = {
        .read = tricore_irbus_intregs_read,
        .write = tricore_irbus_intregs_write,
        .valid = {.min_access_size = 4, .max_access_size = 4, },
        .endianness = DEVICE_LITTLE_ENDIAN, };


static void tricore_irbus_init(Object *obj)
{
    TriCoreIRBUSState *pv = TRICORE_IRBUS(obj);

    qdev_init_gpio_in(DEVICE(pv), irq_handler, IR_SRC_COUNT);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &pv->parent_irq);
    memory_region_init_io(&pv->srvcontrolregs, OBJECT(pv),
            &tricore_irbus_srvcontrolregs_ops, pv, "tricore_irbus.src", 0x4000);
    memory_region_init_io(&pv->intregs, OBJECT(pv),
            &tricore_irbus_intregs_ops, pv, "tricore_irbus.int", 0x1000);
}

static void tricore_irbus_realize(DeviceState *dev, Error **errp)
{
    struct TriCoreIRBUSState *pv = TRICORE_IRBUS(dev);
    Error *err = NULL;

    pv->cpu = object_property_get_link(OBJECT(dev), "cpu", &err);
    if (!pv->cpu) {
        error_setg(errp, "tricore,irbus: CPU link not found");
        return;
    }
}

static const Property tricore_irbus_properties[] = {
    DEFINE_PROP_BOOL("tc4x-mode", TriCoreIRBUSState, tc4x_mode, false),
};

static void tricore_irbus_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->user_creatable = false;
    dc->realize = tricore_irbus_realize;
    device_class_set_props(dc, tricore_irbus_properties);
}

static TypeInfo tricore_irbus_info = { .name = "tricore_irbus", .parent =
        TYPE_SYS_BUS_DEVICE, .instance_size = sizeof(TriCoreIRBUSState),
        .instance_init = tricore_irbus_init, .class_init =
                tricore_irbus_class_init, };

static void tricore_irbus_register(void)
{
    type_register_static(&tricore_irbus_info);
}

type_init(tricore_irbus_register)
