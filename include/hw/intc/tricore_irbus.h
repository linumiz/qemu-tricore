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

#ifndef HW_TRICORE_IRBUS_H
#define HW_TRICORE_IRBUS_H


#define TYPE_TRICORE_IRBUS "tricore_irbus"
#define TRICORE_IRBUS(obj) \
    OBJECT_CHECK(TriCoreIRBUSState, (obj), TYPE_TRICORE_IRBUS)

#define IR_SRC_COUNT 256

#define IR_SRC_SRPN 0xFF

/* TC2x/TC3x SRC bit positions */
#define IR_SRC_SRE_TC3X  (1 << 10)
#define IR_SRC_SRR_TC3X  (1 << 24)
#define IR_SRC_SETR_TC3X (1 << 26)
#define IR_SRC_CLRR_TC3X (1 << 25)

/* TC4x SRC bit positions */
#define IR_SRC_SRE_TC4X  (1 << 25)
#define IR_SRC_SRR_TC4X  (1 << 26)
#define IR_SRC_SETR_TC4X (1 << 28)
#define IR_SRC_CLRR_TC4X (1 << 27)

/* default to TC3x for backward compat */
#define IR_SRC_SRE  IR_SRC_SRE_TC3X
#define IR_SRC_SRR  IR_SRC_SRR_TC3X

/* internal srcnum indices */
#define IR_SRC_ASCLIN0TX     9
#define IR_SRC_ASCLIN0RX    10
#define IR_SRC_ASCLIN0EX    11
#define IR_SRC_STM0_SR0    103
#define IR_SRC_STM0_SR1    104
#define IR_SRC_STM1_SR0    105
#define IR_SRC_STM1_SR1    106
#define IR_SRC_STM2_SR0    107
#define IR_SRC_STM2_SR1    108
#define IR_SRC_RESET       254



typedef struct TriCoreIRBUSState {
    SysBusDevice parent_obj;
    void *cpu;
    MemoryRegion srvcontrolregs;
    MemoryRegion intregs;
    uint32_t src_control_reg[IR_SRC_COUNT];
    uint16_t src_regaddr[IR_SRC_COUNT];
    uint8_t interruptstatusregs[IR_SRC_COUNT];
    uint32_t lwsr[8];
    uint32_t lasr;
    qemu_irq parent_irq;
    bool tc4x_mode;
} TriCoreIRBUSState;

#endif
