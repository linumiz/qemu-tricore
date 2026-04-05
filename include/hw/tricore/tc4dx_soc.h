/*
 * Infineon TC4Dx SoC System emulation.
 *
 * Copyright (c) 2026 Parthiban Nallathambi <parthiban@linumiz.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef TC4DX_SOC_H
#define TC4DX_SOC_H

#include "hw/core/sysbus.h"
#include "target/tricore/cpu.h"
#include "qom/object.h"

#include "hw/tricore/tricore.h"
#include "hw/tricore/tricore_virt.h"
#include "hw/tricore/tricore_ir.h"
#include "hw/tricore/tricore_scu.h"
#include "hw/tricore/tricore_sfr.h"
#include "hw/intc/tricore_irbus.h"
#include "hw/timer/tricore_stm.h"
#include "hw/char/tricore_asclin.h"
#include "hw/tricore/tc_soc.h"

#define TYPE_TC4DX_SOC ("tc4dx-soc")
OBJECT_DECLARE_TYPE(TC4DXSoCState, TC4DXSoCClass, TC4DX_SOC)

typedef struct TC4DXSoCCPUMemState {
    MemoryRegion dspr;
    MemoryRegion pspr;
    MemoryRegion dlmu;
    MemoryRegion pflash_c;
    MemoryRegion pflash_u;
} TC4DXSoCCPUMemState;

typedef struct TC4DXSoCFlashMemState {
    MemoryRegion flashcs;
} TC4DXSoCFlashMemState;

typedef struct TC4DXSoCState {
    SysBusDevice parent_obj;

    TriCoreCPU cpu;

    MemoryRegion dsprX;
    MemoryRegion psprX;

    TC4DXSoCCPUMemState cpu0mem;
    TC4DXSoCCPUMemState cpu1mem;
    TC4DXSoCCPUMemState cpu2mem;
    TC4DXSoCCPUMemState cpu3mem;
    TC4DXSoCCPUMemState cpu4mem;
    TC4DXSoCCPUMemState cpu5mem;
    TC4DXSoCCPUMemState cpucsmem;
    TC4DXSoCFlashMemState flashmem;

    TriCoreIRBUSState *irbus;
    TriCoreVIRTState *virt;
    TriCoreSCUState *scu;
    TriCoreSTMState *stm;
    TriCoreSFRState *sfr;
    TriCoreASCLINState *asclin;

    qemu_irq irq[IR_SRC_COUNT];
    qemu_irq *cpu_irq;
} TC4DXSoCState;

typedef struct TC4DXSoCClass {
    DeviceClass parent_class;

    const char *name;
    const char *cpu_type;
    const MemmapEntry *memmap;
    uint32_t num_cpus;
} TC4DXSoCClass;

enum {
    TC4DX_DSPR0,
    TC4DX_PSPR0,
    TC4DX_DSPR1,
    TC4DX_PSPR1,
    TC4DX_DSPR2,
    TC4DX_PSPR2,
    TC4DX_DSPR3,
    TC4DX_PSPR3,
    TC4DX_DSPR4,
    TC4DX_PSPR4,
    TC4DX_DSPR5,
    TC4DX_PSPR5,
    TC4DX_DSPRCS,
    TC4DX_PSPRCS,

    TC4DX_PFLASH0_C,
    TC4DX_PFLASH1_C,
    TC4DX_PFLASH2_C,
    TC4DX_PFLASH3_C,
    TC4DX_PFLASH4_C,
    TC4DX_PFLASH5_C,
    TC4DX_FLASHCS_C,

    TC4DX_DLMU0,
    TC4DX_DLMU1,
    TC4DX_DLMU2,
    TC4DX_DLMU3,
    TC4DX_DLMU4,
    TC4DX_DLMU5,

    TC4DX_PSPRX,
    TC4DX_DSPRX,
    TC4DX_VIRT,
    TC4DX_SFR,
    TC4DX_STM,
    TC4DX_ASCLIN,
    TC4DX_SCU,
    TC4DX_IRBUS,
};

#endif
