/*
 * Infineon tc33x SoC System emulation.
 *
 * Copyright (c) 2020 Andreas Konopik <andreas.konopik@efs-auto.de>
 * Copyright (c) 2020 David Brenken <david.brenken@efs-auto.de>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#ifndef TC33X_SOC_H
#define TC33X_SOC_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#include "hw/tricore/tricore_scu.h"
#include "hw/tricore/tricore_sfr.h"
#include "hw/intc/tricore_ir.h"
#include "hw/timer/tricore_stm.h"
#include "hw/char/tricore_asclin.h"
#include "hw/tricore/tc_soc.h"

#define TYPE_TC33X_SOC ("tc33x-soc")
OBJECT_DECLARE_TYPE(TC33XSoCState, TC33XSoCClass, TC33X_SOC)

typedef struct TC33XSoCCPUMemState {

    MemoryRegion dspr;
    MemoryRegion pspr;

    MemoryRegion pflash_c;
    MemoryRegion pflash_u;
    MemoryRegion dlmu_c;
    MemoryRegion dlmu_u;

} TC33XSoCCPUMemState;

#define TC33X_MEMDEV_CPU(n) \
    TC33X_DSPR##n,      \
    TC33X_DCACHE##n,    \
    TC33X_DTAG##n,      \
    TC33X_PSPR##n,      \
    TC33X_PCACHE##n,    \
    TC33X_PTAG##n,      \
    TC33X_DLMU##n##_U,  \
    TC33X_DLMU##n##_C,  \
    TC33X_PFLASH##n##_U,\
    TC33X_PFLASH##n##_C
    


typedef struct TC33XSoCFlashMemState {
    MemoryRegion dflash0;
    MemoryRegion dflash1;
    MemoryRegion olda_c;
    MemoryRegion olda_u;
    MemoryRegion brom_c;
    MemoryRegion brom_u;
    MemoryRegion emem;

} TC33XSoCFlashMemState;

typedef struct TC33XSoCState {
    /*< private >*/
    SysBusDevice parent_obj;

    /*< public >*/
    TriCoreCPU cpu;

    MemoryRegion dsprX;
    MemoryRegion psprX;

    TC33XSoCCPUMemState cpu0mem;
    TC33XSoCFlashMemState flashmem;
    
    TriCoreIRState *irbus;
    TriCoreSCUState *scu;
    TriCoreSTMState *stm;
    TriCoreSFRState *sfr;
    TriCoreASCLINState *asclin;

    qemu_irq irq[256];
    qemu_irq *cpu_irq;


} TC33XSoCState;

typedef struct TC33XSoCClass {
    DeviceClass parent_class;

    const char *name;
    const char *cpu_type;
    const MemmapEntry *memmap;
    uint32_t num_cpus;
} TC33XSoCClass;

enum {
    TC33X_MEMDEV_CPU(0),
    
    TC33X_OLDA_C,
    TC33X_OLDA_U,
    TC33X_BROM_C,
    TC33X_BROM_U,

    // no LMU for TC33x

    TC33X_EMEM,

    TC33X_DFLASH0,
    TC33X_DFLASH1,
    TC33X_PSPRX,
    TC33X_DSPRX,
    TC33X_SFR,
    TC33X_IRBUS,
    TC33X_SCU,
    TC33X_STM,
    TC33X_ASCLIN,
};

#endif
