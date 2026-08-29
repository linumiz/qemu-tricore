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

#ifndef HW_TRICORE_IR_H
#define HW_TRICORE_IR_H

#include "hw/core/sysbus.h"
#include "hw/core/registerfields.h"

#define TYPE_TRICORE_IR "tricore_ir"
#define TRICORE_IR(obj) \
    OBJECT_CHECK(TriCoreIRState, (obj), TYPE_TRICORE_IR)

FIELD(SRC, SRPN, 0, 8)
FIELD(SRC, VM, 8, 3)
FIELD(SRC, SRR, 24, 1)
FIELD(SRC, CLRR, 25, 1)
FIELD(SRC, SETR, 26, 1)
FIELD(SRC, IOV, 27, 1)
FIELD(SRC, IOVCLR, 28, 1)

/* TC2x/TC3x SRC bit layout */
FIELD(SRC_TC3X, SRE, 10, 1)
FIELD(SRC_TC3X, TOS, 11, 3)
FIELD(SRC_TC3X, SWS, 29, 1)
FIELD(SRC_TC3X, SWSCLR, 30, 1)

/* TC4x SRC bit layout */
FIELD(SRC_TC4X, TOS, 12, 4)
FIELD(SRC_TC4X, SRE, 23, 1)

FIELD(LWSR, PN, 0, 8)
FIELD(LWSR, VM, 8, 3)
FIELD(LWSR, VALID, 12, 1)
FIELD(LWSR, INVALID, 13, 1)
FIELD(LWSR, ID, 16, 9)
FIELD(LWSR, CS, 27, 1)
FIELD(LWSR, STAT, 31, 1)

FIELD(LASR, PN, 0, 8)
FIELD(LASR, ECC, 8, 6)
FIELD(LASR, ID, 16, 11)
FIELD(LASR, CS, 27, 1)
FIELD(LASR, VM, 28, 3)
FIELD(LASR, ENTER, 31, 1)

typedef struct TriCoreIRState {
    SysBusDevice parent_obj;
    
    MemoryRegion src_region;
    MemoryRegion int_region;

    uint32_t *src_regs;
    uint32_t lwsr[8];
    uint32_t lasr;

    qemu_irq *isp_irqs;
    
    bool tc4x_mode;
    uint8_t num_isps;
    uint16_t num_irqs;
} TriCoreIRState;

void tricore_ir_irq_acknowledge(TriCoreIRState *s, uint16_t irq, uint8_t vm);

#endif
