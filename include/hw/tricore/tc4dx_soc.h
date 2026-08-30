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

#include "hw/tricore/tc4x_cpu.h"
#include "hw/tricore/tricore_virt.h"
#include "hw/tricore/tc4x_clock.h"
#include "hw/tricore/tricore_sfr.h"
#include "hw/intc/tricore_ir.h"
#include "hw/timer/tricore_stm.h"
#include "hw/char/tricore_asclin.h"
#include "hw/net/tricore_mcan.h"
#include "hw/tricore/tc_soc.h"

#define TYPE_TC4DX_SOC ("tc4dx-soc")
#define TC4DX_MAX_CPUS 6
#define TC4DX_MAX_ASCLIN 28
#define TC4DX_MAX_MCAN 5
OBJECT_DECLARE_TYPE(TC4DXSoCState, TC4DXSoCClass, TC4DX_SOC)


typedef struct TC4DXSoCState {
    SysBusDevice parent_obj;

    TC4xCPUState cpus[TC4DX_MAX_CPUS];
    MemoryRegion cpu_sfr_alias[TC4DX_MAX_CPUS];

    TriCoreIRState ir;
    TC4xClockState clock;
    TriCoreASCLINState asclin[TC4DX_MAX_ASCLIN];
    TriCoreMCANState mcan[TC4DX_MAX_MCAN];
    CanBusState *canbus;

    Clock *fosc;
} TC4DXSoCState;

typedef struct TC4DXSoCClass {
    DeviceClass parent_class;

    const char *name;
    const char *cpu_type;
    const MemmapEntry *memmap;
    uint32_t num_cpus;
} TC4DXSoCClass;

#endif
