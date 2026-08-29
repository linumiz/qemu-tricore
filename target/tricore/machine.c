#include "qemu/osdep.h"
#include "migration/vmstate.h"
#include "cpu.h"

static int tricore_cpu_post_load(void *opaque, int version_id)
{
    TriCoreCPU *cpu = opaque;
    if (!cpu->parent_obj.halted) {
        cpu_resume(CPU(cpu));
        qemu_cpu_kick(CPU(cpu));
    }
    return 0;
}

static const VMStateDescription vmstate_tricore_env = {
    .name = "tricore-env",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(gpr_a, CPUTriCoreState, 16),
        VMSTATE_UINT32_ARRAY(gpr_d, CPUTriCoreState, 16),
        VMSTATE_UINT32(PSW, CPUTriCoreState),
        VMSTATE_UINT32(PSW_USB_C, CPUTriCoreState),
        VMSTATE_UINT32(PSW_USB_V, CPUTriCoreState),
        VMSTATE_UINT32(PSW_USB_SV, CPUTriCoreState),
        VMSTATE_UINT32(PSW_USB_AV, CPUTriCoreState),
        VMSTATE_UINT32(PSW_USB_SAV, CPUTriCoreState),
        VMSTATE_UINT32(PC, CPUTriCoreState),
        VMSTATE_UINT32(PCXI, CPUTriCoreState),
        VMSTATE_UINT32(FCX, CPUTriCoreState),
        VMSTATE_UINT32(LCX, CPUTriCoreState),
        VMSTATE_UINT32(ICR, CPUTriCoreState),
        VMSTATE_UINT32(BIV, CPUTriCoreState),
        VMSTATE_UINT32(BTV, CPUTriCoreState),
        VMSTATE_UINT32(ISP, CPUTriCoreState),
        VMSTATE_UINT8(tin, CPUTriCoreState),
        VMSTATE_END_OF_LIST()
    }
};

const VMStateDescription vmstate_tricore_cpu = {
    .name = "tricore-cpu",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = tricore_cpu_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(parent_obj, TriCoreCPU, 0, vmstate_cpu_common, CPUState),
        VMSTATE_STRUCT(env, TriCoreCPU, 1, vmstate_tricore_env, CPUTriCoreState),
        VMSTATE_END_OF_LIST()
    }
};
