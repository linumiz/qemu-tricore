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
#include "hw/core/qdev-properties.h"
#include "hw/timer/tricore_stm.h"
#include <stdio.h>
#include <inttypes.h>
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/core/ptimer.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include <math.h>

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
#define TC4X_VM1_CMP0   (0x120 / 4)
#define TC4X_VM1_CMP1   (0x124 / 4)
#define TC4X_VM1_CMCON  (0x128 / 4)
#define TC4X_VM1_ICR    (0x12C / 4)
#define TC4X_VM1_ISCR   (0x130 / 4)
#define TC4X_ABS        (0x020 / 4)
#define TC4X_ABS_HI     (0x024 / 4)

static hwaddr stm_tc4x_remap(hwaddr reg_addr)
{
    switch (reg_addr) {
    case TC4X_ABS:       return TIM0;
    case TC4X_ABS_HI:    return TIM6;
    case TC4X_VM1_CMP0:  return CMP0;
    case TC4X_VM1_CMP1:  return CMP1;
    case TC4X_VM1_CMCON: return CMCON;
    case TC4X_VM1_ICR:   return ICR;
    case TC4X_VM1_ISCR:  return ISCR;
    default: break;
    }
    return reg_addr;
}

static uint64_t tricore_stm_get_tim_update_regs(TriCoreSTMState *s,
        int timshift, char bUpdateTIM);

static void tricore_stm_update_irqs(void *opaque)
{
    TriCoreSTMState *s = (TriCoreSTMState *) opaque;

    if (s->regs[ICR] & MASK_ICR_CMP0IR) {
        qemu_irq_raise(s->irq);
    } else {
        qemu_irq_lower(s->irq);
    }
}

#if 0
static void tricore_stm_update_freq(TriCoreSTMState *s)
{
    ptimer_transaction_begin(s->ptimer);
    uint32_t freq = tricore_scu_get_stmclock(s->scu);
    if (freq == 0) {
        freq = RESET_TRICORE_STM_FREQUENCY;
    }
    s->freq_hz = freq;
    ptimer_set_freq(s->ptimer, freq);
    ptimer_transaction_commit(s->ptimer);
}
#endif

#if 0
static void tricore_stm_timer_start(TriCoreSTMState *s)
{
    timer_del(s->timer);

    if (s->regs[CMP0] == 0) {
        return;
    }

    uint32_t mstart = (s->regs[CMCON] & MASK_CMCON_MSTART0) >> 8;
    uint32_t msize  = (s->regs[CMCON] & MASK_CMCON_MSIZE0);
    uint32_t nbits  = msize + 1;
    uint32_t mask   = (nbits >= 32) ? 0xFFFFFFFFu : ((1u << nbits) - 1);

    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    int64_t elapsed = now - s->realtime_base_ns;

    s->counter_offset += muldiv64(elapsed, s->freq_hz, NANOSECONDS_PER_SECOND);
    s->realtime_base_ns = now;
    s->tim_counter = s->counter_offset;

    uint32_t window_val = (uint32_t)(s->tim_counter >> mstart) & mask;
    uint32_t target = s->regs[CMP0] & mask;

    uint32_t delta;
    if (target > window_val) {
        delta = target - window_val;
    } else {
        delta = (mask - window_val) + 1 + target;
    }

    uint64_t timeout_ticks = (uint64_t)delta << mstart;
    int64_t timeout_ns = muldiv64(timeout_ticks,
                                  NANOSECONDS_PER_SECOND, s->freq_hz);

    timer_mod(s->timer, now + timeout_ns);
}
#endif

#if 0
static void tricore_stm_timer_start(TriCoreSTMState *s)
{
    timer_del(s->timer);
 
    if (s->regs[CMP0] == 0) {
        return;
    }
 
    uint32_t mstart = (s->regs[CMCON] & MASK_CMCON_MSTART0) >> 8;
    uint32_t msize  = (s->regs[CMCON] & MASK_CMCON_MSIZE0);
    uint32_t nbits  = msize + 1;
    uint32_t mask   = (nbits >= 32) ? 0xFFFFFFFFu : ((1u << nbits) - 1);
 
    tricore_stm_get_tim_update_regs(s, 0, 0);
 
    uint32_t window_val = (uint32_t)(s->tim_counter >> mstart) & mask;
    uint32_t target = s->regs[CMP0] & mask;
 
    uint32_t delta;
    if (target > window_val) {
        delta = target - window_val;
    } else {
        delta = (mask - window_val) + 1 + target;
    }
 
    uint64_t timeout_ticks = (uint64_t)delta << mstart;
    int64_t timeout_ns = muldiv64(timeout_ticks,
                                  NANOSECONDS_PER_SECOND, s->freq_hz);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
 
    timer_mod(s->timer, now + timeout_ns);
}

static void tricore_stm_timer_start(TriCoreSTMState *s)
{
    timer_del(s->timer);

    if (s->regs[CMP0] == 0) {
        return;
    }

    uint32_t mstart = (s->regs[CMCON] & MASK_CMCON_MSTART0) >> 8;
    uint32_t msize  = (s->regs[CMCON] & MASK_CMCON_MSIZE0);
    uint32_t nbits  = msize + 1;
    uint32_t mask   = (nbits >= 32) ? 0xFFFFFFFFu : ((1u << nbits) - 1);

    tricore_stm_get_tim_update_regs(s, 0, 0);

    uint32_t window_val = (uint32_t)(s->tim_counter >> mstart) & mask;
    uint32_t target = s->regs[CMP0] & mask;

    uint32_t delta;
#if 0
    if (target > window_val) {
        delta = target - window_val;
    } else {
        delta = (mask - window_val) + 1 + target;
        /* Counter already passed the target. If the wrap-around delta
         * exceeds half the compare window, the target was just missed
         * rather than being far in the future. Fire immediately. */
        if (delta > (mask >> 1)) {
            delta = 1;
        }
    }
#endif

    if (target > window_val) {
        delta = target - window_val;
    } else {
        delta = (mask - window_val) + 1 + target;
    }

    uint64_t timeout_ticks = (uint64_t)delta << mstart;
    int64_t timeout_ns = muldiv64(timeout_ticks,
                                  NANOSECONDS_PER_SECOND, s->freq_hz);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

#if 1
    error_report("STM timer_start: CMP0=0x%x counter=0x%x delta=0x%x "
                 "timeout_ms=%ld freq=%u",
                 target, window_val, delta,
                 (long)(timeout_ns / 1000000), s->freq_hz);
#endif

    timer_mod(s->timer, now + timeout_ns);
}

static void tricore_stm_timer_start(TriCoreSTMState *s)
{
    timer_del(s->timer);

    if (s->regs[CMP0] == 0) {
        return;
    }

    uint32_t mstart = (s->regs[CMCON] & MASK_CMCON_MSTART0) >> 8;
    uint32_t msize  = (s->regs[CMCON] & MASK_CMCON_MSIZE0);
    uint32_t nbits  = msize + 1;
    uint32_t mask   = (nbits >= 32) ? 0xFFFFFFFFu : ((1u << nbits) - 1);

    tricore_stm_get_tim_update_regs(s, 0, 0);

    uint32_t window_val = (uint32_t)(s->tim_counter >> mstart) & mask;
    uint32_t target = s->regs[CMP0] & mask;

    uint32_t delta;
    if (target > window_val) {
        delta = target - window_val;
    } else {
        delta = (mask - window_val) + 1 + target;
    }

    uint64_t timeout_ticks = (uint64_t)delta << mstart;
    int64_t timeout_ns = muldiv64(timeout_ticks,
                                  NANOSECONDS_PER_SECOND, s->freq_hz);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    timer_mod(s->timer, now + timeout_ns);
}
#endif

static void tricore_stm_timer_start(TriCoreSTMState *s)
{
    timer_del(s->timer);

    if (s->regs[CMP0] == 0) {
        //error_report("STM timer_start: CMP0=0, skip");
        return;
    }

    uint32_t mstart = (s->regs[CMCON] & MASK_CMCON_MSTART0) >> 8;
    uint32_t msize  = (s->regs[CMCON] & MASK_CMCON_MSIZE0);
    uint32_t nbits  = msize + 1;
    uint32_t mask   = (nbits >= 32) ? 0xFFFFFFFFu : ((1u << nbits) - 1);

    tricore_stm_get_tim_update_regs(s, 0, 0);

    uint32_t window_val = (uint32_t)(s->tim_counter >> mstart) & mask;
    uint32_t target = s->regs[CMP0] & mask;

    uint32_t delta;
    if (target > window_val) {
        delta = target - window_val;
    } else {
        delta = (mask - window_val) + 1 + target;
    }

    uint64_t timeout_ticks = (uint64_t)delta << mstart;
    int64_t timeout_ns = muldiv64(timeout_ticks,
                                  NANOSECONDS_PER_SECOND, s->freq_hz);
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

#if 0
    error_report("STM timer_start: CMP0=0x%x counter=0x%x delta=0x%x "
                 "timeout_ms=%ld freq=%u ICR=0x%x",
                 target, window_val, delta,
                 (long)(timeout_ns / 1000000), s->freq_hz, s->regs[ICR]);
#endif

    timer_mod(s->timer, now + timeout_ns);
}

static void tricore_stm_write(void *opaque, hwaddr offset, uint64_t value,
        unsigned size)
{
    TriCoreSTMState *s = (TriCoreSTMState *) opaque;
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
    case CMP0:
         s->regs[reg_addr] = value;
#if 0
         error_report("STM CMP0 write: val=0x%x counter=0x%lx freq=%u",
                      (uint32_t)value, (unsigned long)s->tim_counter, s->freq_hz);
#endif
         tricore_stm_timer_start(s);
         break;
    case CMP1:
    case ICR:
#if 0
         error_report("STM ICR write: val=0x%lx CMP0EN=%d",
                      (unsigned long)value, !!(value & MASK_ICR_CMP0EN));
#endif
         s->regs[reg_addr] = value;
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
        tricore_stm_timer_start(s);
        break;
    case ISCR:
#if 0
        error_report("STM ISCR write: val=0x%lx ICR_before=0x%x",
                     (unsigned long)value, s->regs[ICR]);
#endif

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
        tricore_stm_update_irqs(opaque);

#if 0
        error_report("STM ISCR write: val=0x%lx ICR_after=0x%x",
                     (unsigned long)value, s->regs[ICR]);
#endif
        break;
    default:
        break;
    }

    tricore_stm_update_irqs(opaque);
}

static uint64_t tricore_stm_get_tim_update_regs(TriCoreSTMState *s,
        int timshift, char bUpdateTIM)
{
    int64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) - s->realtime_base_ns;
 
    s->tim_counter = muldiv64(now_ns, s->freq_hz, NANOSECONDS_PER_SECOND);
 
    uint64_t r = (uint32_t)(s->tim_counter >> timshift);
 
    if (bUpdateTIM) {
        s->regs[CAP] = (uint32_t)(s->tim_counter >> 32);
    }
    return r;
}

#if 0
static uint64_t tricore_stm_get_tim_update_regs(TriCoreSTMState *s,
        int timshift, char bUpdateTIM)
{
    int64_t now_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - s->realtime_base_ns;
    uint64_t delta = muldiv64(now_ns, s->freq_hz, NANOSECONDS_PER_SECOND);

    s->tim_counter = s->counter_offset + delta;

    error_report("STM read: offset=0x%lx delta=0x%lx total=0x%lx shift=%d",
                 (unsigned long)s->counter_offset,
                 (unsigned long)delta,
                 (unsigned long)s->tim_counter, timshift);

    uint64_t r = (uint32_t)(s->tim_counter >> timshift);

    if (bUpdateTIM) {
        s->regs[CAP] = (uint32_t)(s->tim_counter >> 32);
    }

    return r;
}
#endif

static uint64_t tricore_stm_read(void *opaque, hwaddr offset, unsigned size)
{
    TriCoreSTMState *s = (TriCoreSTMState *) opaque;
    uint64_t r = 0x0;
    hwaddr reg_addr = offset >> 2;

#if 0
    error_report("STM read: offset=0x%lx size=%d reg_addr=0x%lx tc4x=%d",
                 (unsigned long)offset, size, (unsigned long)reg_addr, s->tc4x_mode);
#endif

    if (s->tc4x_mode) {
        reg_addr = stm_tc4x_remap(reg_addr);
    }

    switch (reg_addr) {
    case CLC:
    case ID:
    case TIM0:
        r = tricore_stm_get_tim_update_regs(s, 0, 1);
        if (s->tc4x_mode && size == 8) {
            r |= ((uint64_t)s->regs[CAP]) << 32;
        }
        break;
    case TIM1:
        r = tricore_stm_get_tim_update_regs(s, 4, 1);
        break;
    case TIM2:
        r = tricore_stm_get_tim_update_regs(s, 8, 1);
        break;
    case TIM3:
        r = tricore_stm_get_tim_update_regs(s, 12, 1);
        break;
    case TIM4:
        if (s->tc4x_mode) {
            r = tricore_stm_get_tim_update_regs(s, 0, 1);
        } else {
            r = tricore_stm_get_tim_update_regs(s, 16, 1);
        }
        break;
    case TIM5:
        r = tricore_stm_get_tim_update_regs(s, 20, 1);
        break;
    case TIM6:
        r = tricore_stm_get_tim_update_regs(s, 32, 1);
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
        r = tricore_stm_get_tim_update_regs(s, 0, 0);
#if 0
        r = 0x0;
        s->regs[CAP] = (uint32_t) (s->tim_counter >> 32);
#endif
        break;
    case CAPSV:
        tricore_stm_get_tim_update_regs(s, 0, 0);
        r = (uint32_t)(s->tim_counter >> 32);
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

#if 0
static void tricore_stm_timer_hit(void *opaque)
{
    TriCoreSTMState *s = (TriCoreSTMState *) opaque;

    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    int64_t elapsed = now - s->realtime_base_ns;

    s->counter_offset += muldiv64(elapsed, s->freq_hz, NANOSECONDS_PER_SECOND);
    s->realtime_base_ns = now;

    error_report("STM timer_hit: counter_offset=0x%lx elapsed_ns=%ld",
                 (unsigned long)s->counter_offset, (long)elapsed);

    qatomic_or(&s->regs[ICR], MASK_ICR_CMP0IR);
    tricore_stm_update_irqs(opaque);
}

static void tricore_stm_timer_hit(void *opaque)
{
    TriCoreSTMState *s = (TriCoreSTMState *) opaque;
    qatomic_or(&s->regs[ICR], MASK_ICR_CMP0IR);
    tricore_stm_update_irqs(opaque);
}

static void tricore_stm_timer_hit(void *opaque)
{
    TriCoreSTMState *s = (TriCoreSTMState *) opaque;
//    error_report("STM timer_hit fired");
    qatomic_or(&s->regs[ICR], MASK_ICR_CMP0IR);
    tricore_stm_update_irqs(opaque);
}

static void tricore_stm_timer_hit(void *opaque)
{
    TriCoreSTMState *s = (TriCoreSTMState *) opaque;

    if (!(s->regs[ICR] & MASK_ICR_CMP0EN)) {
        return;
    }

    qatomic_or(&s->regs[ICR], MASK_ICR_CMP0IR);
    tricore_stm_update_irqs(opaque);
}

static void tricore_stm_timer_hit(void *opaque)
{
    TriCoreSTMState *s = (TriCoreSTMState *) opaque;

    if (!(s->regs[ICR] & MASK_ICR_CMP0EN)) {
        return;
    }

    /* Rebase so ISR sees counter at match point, not wall-clock now */
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->realtime_base_ns = now;
    s->tim_counter = (uint64_t)s->regs[CMP0];

    qatomic_or(&s->regs[ICR], MASK_ICR_CMP0IR);
    tricore_stm_update_irqs(opaque);
}

static void tricore_stm_timer_hit(void *opaque)
{
    TriCoreSTMState *s = (TriCoreSTMState *) opaque;

    if (!(s->regs[ICR] & MASK_ICR_CMP0EN)) {
        return;
    }

    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->realtime_base_ns = now - muldiv64((uint64_t)s->regs[CMP0],
                                          NANOSECONDS_PER_SECOND,
                                          s->freq_hz);

    qatomic_or(&s->regs[ICR], MASK_ICR_CMP0IR);
    tricore_stm_update_irqs(opaque);
}

static void tricore_stm_timer_hit(void *opaque)
{
    TriCoreSTMState *s = (TriCoreSTMState *) opaque;

    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->realtime_base_ns = now - muldiv64((uint64_t)s->regs[CMP0],
                                          NANOSECONDS_PER_SECOND,
                                          s->freq_hz);

    qatomic_or(&s->regs[ICR], MASK_ICR_CMP0IR);
    tricore_stm_update_irqs(opaque);
}

static void tricore_stm_timer_hit(void *opaque)
{
    TriCoreSTMState *s = (TriCoreSTMState *) opaque;

    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->realtime_base_ns = now - muldiv64((uint64_t)s->regs[CMP0],
                                          NANOSECONDS_PER_SECOND,
                                          s->freq_hz);

    qatomic_or(&s->regs[ICR], MASK_ICR_CMP0IR);
    tricore_stm_update_irqs(opaque);
}
#endif

static void tricore_stm_timer_hit(void *opaque)
{
    TriCoreSTMState *s = (TriCoreSTMState *) opaque;

#if 0
    error_report("STM timer_hit: CMP0=0x%x ICR=0x%x CMP0EN=%d",
                 s->regs[CMP0], s->regs[ICR],
                 !!(s->regs[ICR] & MASK_ICR_CMP0EN));
#endif

    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    s->realtime_base_ns = now - muldiv64((uint64_t)s->regs[CMP0],
                                          NANOSECONDS_PER_SECOND,
                                          s->freq_hz);

    qatomic_or(&s->regs[ICR], MASK_ICR_CMP0IR);
    tricore_stm_update_irqs(opaque);
}

static void tricore_stm_realize(DeviceState *dev, Error **errp)
{
    TriCoreSTMState *s = TRICORE_STM(dev);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    Error *err = NULL;

    s->scu = (TriCoreSCUState *)object_property_get_link(OBJECT(dev), "scu", &err);
    if (!s->scu) {
        error_setg(errp, "tricore_stm: scu link not found");
        return;
    }

    s->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                            tricore_stm_timer_hit, s);
    s->freq_hz = tricore_scu_get_stmclock(s->scu);
    if (s->freq_hz == 0) {
        s->freq_hz = RESET_TRICORE_STM_FREQUENCY;
    }
    s->realtime_base_ns = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static void tricore_stm_init(Object *obj)
{
    TriCoreSTMState *s = TRICORE_STM(obj);
    memory_region_init_io(&s->iomem, OBJECT(s), &tricore_stm_ops, s,
            "tricore_stm", 0x200);
    s->tim_counter = 0x0;
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
        .name = TYPE_TRICORE_STM, .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(TriCoreSTMState), .instance_init =
                tricore_stm_init, .class_init = tricore_stm_class_init, };

static void tricore_stm_register_types(void)
{
    type_register_static(&tricore_stm_info);
}

type_init(tricore_stm_register_types)
