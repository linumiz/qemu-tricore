/*
 *  QEMU model of the TriCore STM device.
 *
 *  Copyright (c) 2017 David Brenken
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */
#include "qemu/osdep.h"
#include "hw/core/clock.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/timer/tricore_stm.h"
#include "qemu/bitops.h"
#include "qemu/timer.h"

enum {
    CLC,
    RESERVED1,
    ID,
    RESERVED2,
    TIM0,
    TIM1,
    TIM2,
    TIM3,
    TIM4,
    TIM5,
    TIM6,
    CAP,
    CMP0,
    CMP1,
    CMCON,
    ICR,
    ISCR,
    RESERVED3,
    TIM0SV = 0x50 / 4,
    CAPSV,
    RESERVED4,
    OCS = 0xE8 / 4,
    KRSTCLR,
    KRST1,
    KRST0,
    ACCEN1,
    ACCEN0
};

/* TC4x per-CPU STM VM1 register offsets */
#define TC4X_VM1_CMP0 (0x120 / 4)
#define TC4X_VM1_CMP1 (0x124 / 4)
#define TC4X_VM1_CMCON (0x128 / 4)
#define TC4X_VM1_ICR (0x12C / 4)
#define TC4X_VM1_ISCR (0x130 / 4)
#define TC4X_ABS (0x020 / 4)
#define TC4X_ABS_HI (0x024 / 4)

static hwaddr stm_tc4x_remap(hwaddr reg_addr)
{
    switch (reg_addr) {
    case TC4X_ABS:
        return TIM0;
    case TC4X_ABS_HI:
        return CAP;
    case TC4X_VM1_CMP0:
        return CMP0;
    case TC4X_VM1_CMP1:
        return CMP1;
    case TC4X_VM1_CMCON:
        return CMCON;
    case TC4X_VM1_ICR:
        return ICR;
    case TC4X_VM1_ISCR:
        return ISCR;
    default:
        break;
    }
    return reg_addr;
}

static void tricore_stm_tim_update(TriCoreSTMState *s)
{
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->tim_counter += clock_ns_to_ticks(s->fstm, now - s->tim_base_ns);
    s->tim_base_ns = now;
}

static void tricore_stm_timer_update(TriCoreSTMState *s)
{
    uint32_t mstart = (s->regs[CMCON] & MASK_CMCON_MSTART0) >> 8;
    uint32_t msize = (s->regs[CMCON] & MASK_CMCON_MSIZE0);
    uint32_t nbits = msize + 1;
    uint64_t tim_target = s->tim_counter;

    if ((s->regs[ICR] & MASK_ICR_CMP0EN) == 0 && (s->regs[ICR] & MASK_ICR_CMP0IR)) {
        return;
    }

    /* Calculate the target TIM value */
    if (mstart) {
        tim_target = deposit64(tim_target, 0, mstart, 0);
    }
    tim_target = deposit64(tim_target, mstart, nbits, (uint64_t)s->regs[CMP0]);

    /* Wrap around if needed */
    if (tim_target <= s->tim_counter) {
        tim_target += (1ull << (mstart + nbits));
    }

    timer_mod(s->timer,
              s->tim_base_ns +
                  clock_ticks_to_ns(s->fstm, tim_target - s->tim_counter));
}

static void tricore_stm_clock_update(void *opaque, enum ClockEvent event)
{
    TriCoreSTMState *s = (TriCoreSTMState *)opaque;

    if (event == ClockPreUpdate) {
        tricore_stm_tim_update(s);
    } else {
        tricore_stm_timer_update(s);
    }
}

static void tricore_stm_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size)
{
    TriCoreSTMState *s = (TriCoreSTMState *)opaque;
    hwaddr reg_addr = offset >> 2;

    if (s->tc4x_mode) {
        reg_addr = stm_tc4x_remap(reg_addr);
    }

    value = value << ((offset & 0x3) * 0x8);

    if (size == 1) {
        uint32_t old_value = s->regs[reg_addr];
        uint32_t shifter = offset & 3;
        old_value &= ~(0xFF << (shifter * 8));
        value |= old_value;
    }
    if (size == 2) {
        uint32_t old_value = s->regs[reg_addr];
        uint32_t shifter = offset & 3;
        old_value &= ~(0xFFFF << (shifter * 8));
        value |= old_value;
    }

    switch (reg_addr) {
    case CLC:
    case ID:
    case TIM0:
    case TIM1:
    case TIM2:
    case TIM3:
    case TIM4:
    case TIM5:
    case TIM6:
    case CAP:
        s->regs[reg_addr] = value;
        break;
    case CMP0:
        s->regs[reg_addr] = value;
        tricore_stm_tim_update(s);
        tricore_stm_timer_update(s);
        break;
    case CMP1:
    case ICR:
        s->regs[reg_addr] = value;
        tricore_stm_timer_update(s);
        break;
    case TIM0SV:
    case CAPSV:
    case OCS:
    case KRSTCLR:
    case KRST1:
    case KRST0:
    case ACCEN1:
    case ACCEN0:
        s->regs[reg_addr] = value;
        break;
    case CMCON:
        s->regs[reg_addr] = value;
        tricore_stm_tim_update(s);
        tricore_stm_timer_update(s);
        break;
    case ISCR:
        if (value & MASK_ISCR_CMP0IRR) {
            qatomic_and(&s->regs[ICR], ~MASK_ICR_CMP0IR);
        }
        if (value & MASK_ISCR_CMP1IRR) {
            qatomic_and(&s->regs[ICR], ~MASK_ICR_CMP1IR);
        }
        if (value & MASK_ISCR_CMP0IRS) {
            qatomic_or(&s->regs[ICR], MASK_ICR_CMP0IR);
        }
        if (value & MASK_ISCR_CMP1IRS) {
            qatomic_or(&s->regs[ICR], MASK_ICR_CMP1IR);
        }
        tricore_stm_timer_update(s);
        break;
    default:
        break;
    }
}

static uint64_t tricore_stm_read(void *opaque, hwaddr offset, unsigned size)
{
    TriCoreSTMState *s = (TriCoreSTMState *)opaque;
    uint64_t r = 0x0;
    hwaddr reg_addr = offset >> 2;

    if (s->tc4x_mode) {
        reg_addr = stm_tc4x_remap(reg_addr);
    }

    if ((reg_addr >= TIM0 && reg_addr <= TIM6) || reg_addr == TIM0SV) {
        tricore_stm_tim_update(s);
        s->regs[CAP] = (uint32_t)(s->tim_counter >> 32);
    }

    switch (reg_addr) {
    case CLC:
    case ID:
        r = s->regs[reg_addr];
        break;
    case TIM0:
    case TIM1:
    case TIM2:
    case TIM3:
    case TIM4:
    case TIM5:
    case TIM6:
        r = s->tim_counter << (reg_addr - TIM0) * 4;
        break;
    case CAP:
        r = s->regs[CAP];
        break;
    case ISCR:
        r = 0;
        break;
    case CMP0:
    case CMP1:
    case CMCON:
    case ICR:
        r = s->regs[reg_addr];
        break;
    case TIM0SV:
        r = s->tim_counter;
        break;
    case CAPSV:
        r = s->regs[CAP];
        break;
    case OCS:
    case KRSTCLR:
    case KRST1:
    case KRST0:
    case ACCEN1:
    case ACCEN0:
        r = s->regs[reg_addr];
        break;
    default:
        r = 0x0;
        break;
    }

    return r;
}


static void tricore_stm_reset(DeviceState *dev)
{
    TriCoreSTMState *s = TRICORE_STM(dev);

    s->regs[CLC] = RESET_TRICORE_STM_CLC;
    s->regs[ID] = RESET_TRICORE_STM_ID;
    s->regs[TIM0] = RESET_TRICORE_STM_TIM0;
    s->regs[TIM1] = RESET_TRICORE_STM_TIM1;
    s->regs[TIM2] = RESET_TRICORE_STM_TIM2;
    s->regs[TIM3] = RESET_TRICORE_STM_TIM3;
    s->regs[TIM4] = RESET_TRICORE_STM_TIM4;
    s->regs[TIM5] = RESET_TRICORE_STM_TIM5;
    s->regs[TIM6] = RESET_TRICORE_STM_TIM6;
    s->regs[CAP] = RESET_TRICORE_STM_CAP;
    s->regs[CMP0] = RESET_TRICORE_STM_CMP0;
    s->regs[CMP1] = RESET_TRICORE_STM_CMP1;
    s->regs[CMCON] = RESET_TRICORE_STM_CMCON;
    s->regs[ICR] = RESET_TRICORE_STM_ICR;
    s->regs[ISCR] = RESET_TRICORE_STM_ISCR;
    s->regs[TIM0SV] = RESET_TRICORE_STM_TIM0SV;
    s->regs[CAPSV] = RESET_TRICORE_STM_CAPSV;
    s->regs[OCS] = RESET_TRICORE_STM_OCS;
    s->regs[KRSTCLR] = RESET_TRICORE_STM_KRSTCLR;
    s->regs[KRST1] = RESET_TRICORE_STM_KRST1;
    s->regs[KRST0] = RESET_TRICORE_STM_KRST0;
    s->regs[ACCEN1] = RESET_TRICORE_STM_ACCEN1;
    s->regs[ACCEN0] = RESET_TRICORE_STM_ACCEN0;
}

static const MemoryRegionOps tricore_stm_ops = {
        .read = tricore_stm_read, .write = tricore_stm_write, .valid = {
                .min_access_size = 4, .max_access_size = 8, }, .endianness =
                DEVICE_LITTLE_ENDIAN, };

static void tricore_stm_timer_hit(void *opaque)
{
    TriCoreSTMState *s = (TriCoreSTMState *)opaque;

    qatomic_or(&s->regs[ICR], MASK_ICR_CMP0IR);

    if (s->regs[ICR] & MASK_ICR_CMP0EN) {
        qemu_irq_pulse(s->irq);
    }

    tricore_stm_tim_update(s);
    tricore_stm_timer_update(s);
}

static void tricore_stm_realize(DeviceState *dev, Error **errp)
{
    TriCoreSTMState *s = TRICORE_STM(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, tricore_stm_timer_hit, s);
    s->tim_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->tim_counter = 0x0;
    /* Trigger timer for 0 compare */
    s->regs[ICR] = MASK_ICR_CMP1IR | MASK_ICR_CMP0IR;
    /* TODO: qemu_irq_pulse(s->irq); */

    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static void tricore_stm_init(Object *obj)
{
    TriCoreSTMState *s = TRICORE_STM(obj);
    memory_region_init_io(&s->iomem, OBJECT(s), &tricore_stm_ops, s,
                          "tricore_stm", 0x200);
    s->fstm = qdev_init_clock_in(DEVICE(s), "fstm", tricore_stm_clock_update, s,
                                 ClockPreUpdate | ClockUpdate);
}

static const Property tricore_stm_properties[] = {
    DEFINE_PROP_BOOL("tc4x-mode", TriCoreSTMState, tc4x_mode, false),
};

static void tricore_stm_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->legacy_reset = tricore_stm_reset;
    dc->realize = tricore_stm_realize;
    device_class_set_props(dc, tricore_stm_properties);
}

static const TypeInfo tricore_stm_info = {
    .name = TYPE_TRICORE_STM,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(TriCoreSTMState),
    .instance_init = tricore_stm_init,
    .class_init = tricore_stm_class_init,
};

static void tricore_stm_register_types(void)
{
    type_register_static(&tricore_stm_info);
}

type_init(tricore_stm_register_types)
