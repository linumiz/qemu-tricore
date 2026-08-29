#ifndef TC4X_CPU_H
#define TC4X_CPU_H

#include "qemu/osdep.h"
#include "hw/core/clock.h"
#include "hw/core/sysbus.h"
#include "hw/intc/tricore_ir.h"
#include "hw/timer/tricore_stm.h"
#include "qemu/typedefs.h"
#include "qom/object.h"
#include "system/memory.h"
#include "target/tricore/cpu.h"

#define TYPE_TC4X_CPU "tc4x_cpu"
OBJECT_DECLARE_SIMPLE_TYPE(TC4xCPUState, TC4X_CPU)

struct TC4xCPUState {
    SysBusDevice parent_obj;

    TriCoreCPU *tricore;
    TriCoreSTMState stm;
    TriCoreIRState *ir;

    MemoryRegion container;
    MemoryRegion local_container;
    MemoryRegion sfr_stub;
    MemoryRegion dspr;
    MemoryRegion pspr;
    MemoryRegion dlmu;
    MemoryRegion pflash;
    MemoryRegion pflash_alias;

    Clock *fcpu;
    Clock *fstm;

    uint8_t id;
    uint32_t dpsr_size;
    uint32_t dlmu_size;
    uint32_t pflash_size;
    uint32_t boot_pc;
    uint32_t bootcon;
    uint32_t krst0;
    uint32_t krst1;

    char *cpu_type;
    MemoryRegion *board_memory;
    bool start_powered_off;
};

void tc4x_cpu_load_kernel(TriCoreCPU *cpu, const char *kernel_filename,
                          hwaddr mem_base, int mem_size);
void tc4x_cpu_start_core(TC4xCPUState *cpu, hwaddr entry);

#endif /* TC4X_CPU_H */
