/*
 *  TriCore emulation for qemu: main translation routines.
 *
 *  Copyright (c) 2012-2014 Bastian Koppelmann C-Lab/University Paderborn
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev.h"
#include "qapi/error.h"
#include "cpu.h"
#include "exec/cpu-interrupt.h"
#include "exec/translation-block.h"
#include "tcg/debug-assert.h"
#include "accel/tcg/cpu-ops.h"
#include "cpu-qom.h"

static inline void set_feature(CPUTriCoreState *env, int feature)
{
    env->features |= 1ULL << feature;
}

static const gchar *tricore_gdb_arch_name(CPUState *cs)
{
    TriCoreCPU *cpu = TRICORE_CPU(cs);
    CPUTriCoreState *env = &cpu->env;
    if (tricore_has_feature(env, TRICORE_FEATURE_18))  return g_strdup("TriCore:V1_8");
    if (tricore_has_feature(env, TRICORE_FEATURE_162)) return g_strdup("TriCore:V1_6_2");
    if (tricore_has_feature(env, TRICORE_FEATURE_161)) return g_strdup("TriCore:V1_6_1");
    if (tricore_has_feature(env, TRICORE_FEATURE_16)) return g_strdup("TriCore:V1_6");
    if (tricore_has_feature(env, TRICORE_FEATURE_131)) return g_strdup("TriCore:V1_3_1");
    if (tricore_has_feature(env, TRICORE_FEATURE_13)) return g_strdup("TriCore:V1_3");
    return "tricore";
}

static void tricore_cpu_set_pc(CPUState *cs, vaddr value)
{
    cpu_env(cs)->PC = value & ~1;
}

static vaddr tricore_cpu_get_pc(CPUState *cs)
{
    return cpu_env(cs)->PC;
}

static TCGTBCPUState tricore_get_tb_cpu_state(CPUState *cs)
{
    CPUTriCoreState *env = cpu_env(cs);

    return (TCGTBCPUState){
        .pc = env->PC,
        .flags = FIELD_DP32(0, TB_FLAGS, PRIV, extract32(env->PSW, 10, 2)),
    };
}

static void tricore_cpu_synchronize_from_tb(CPUState *cs,
                                            const TranslationBlock *tb)
{
    tcg_debug_assert(!tcg_cflags_has(cs, CF_PCREL));
    cpu_env(cs)->PC = tb->pc;
}

static void tricore_restore_state_to_opc(CPUState *cs,
                                         const TranslationBlock *tb,
                                         const uint64_t *data)
{
    cpu_env(cs)->PC = data[0];
}

static void tricore_cpu_reset_hold(Object *obj, ResetType type)
{
    CPUState *cs = CPU(obj);
    TriCoreCPUClass *tcc = TRICORE_CPU_GET_CLASS(obj);

    TriCoreCPU *cpu = TRICORE_CPU(obj);
    CPUTriCoreState *env = &cpu->env;

    /* FORCE BIV to your linked address (0x80000100) */
    env->BIV = 0x80000100; 

    /* FORCE FCX to a valid CSA memory block (0xD000A000) */
    /* This prevents the "Upper Context Save" crash in do_interrupt */
    env->FCX = 0x000D0280; // Example Link Word for 0xD000A000
    
    /* FORCE ISP (Interrupt Stack) */
    env->ISP = 0xD0008000;

    /* Ensure Interrupts are globally enabled in PSW */
    env->PSW |= MASK_PSW_IE; 

    if (tcc->parent_phases.hold) {
        tcc->parent_phases.hold(obj, type);
    }

    cpu_state_reset(cpu_env(cs));
}

static bool tricore_cpu_has_work(CPUState *cs)
{
    TriCoreCPU *cpu = TRICORE_CPU(cs);
    CPUTriCoreState *env = &cpu->env;

    return cpu_test_interrupt(cs, CPU_INTERRUPT_HARD | CPU_INTERRUPT_NMI) ||
           (icr_get_ie(env) && FIELD_EX32(env->ICR, ICR, PIPN) != 0);
}

static int tricore_cpu_mmu_index(CPUState *cs, bool ifetch)
{
    return 0;
}

static void tricore_cpu_finalizefn(Object *obj)
{
}

static void tricore_cpu_realizefn(DeviceState *dev, Error **errp)
{
    CPUState *cs = CPU(dev);
    TriCoreCPU *cpu = TRICORE_CPU(dev);
    TriCoreCPUClass *tcc = TRICORE_CPU_GET_CLASS(dev);
    CPUTriCoreState *env = &cpu->env;
    Error *local_err = NULL;

    cpu_exec_realizefn(cs, &local_err);
    if (local_err != NULL) {
        error_propagate(errp, local_err);
        return;
    }

    /* Some features automatically imply others */
    if (tricore_has_feature(env, TRICORE_FEATURE_18)) {
        set_feature(env, TRICORE_FEATURE_162);
    }
    if (tricore_has_feature(env, TRICORE_FEATURE_162)) {
        set_feature(env, TRICORE_FEATURE_161);
    }

    if (tricore_has_feature(env, TRICORE_FEATURE_161)) {
        set_feature(env, TRICORE_FEATURE_16);
    }

    if (tricore_has_feature(env, TRICORE_FEATURE_16)) {
        set_feature(env, TRICORE_FEATURE_131);
    }
    if (tricore_has_feature(env, TRICORE_FEATURE_131)) {
        set_feature(env, TRICORE_FEATURE_13);
    }
    cpu_reset(cs);
    qemu_init_vcpu(cs);

    tcc->parent_realize(dev, errp);
}

static void tricore_cpu_set_irq(void *opaque, int irq, int level)
{
    TriCoreCPU *cpu = TRICORE_CPU(opaque);
    CPUTriCoreState *env = &cpu->env;
    CPUState *cs = CPU(cpu);

    if (level) {
        env->ICR = FIELD_DP32(env->ICR, ICR, PIPN,
                              FIELD_EX32(cpu->ir->lwsr[0], LWSR, PN));
        cpu_interrupt(cs, CPU_INTERRUPT_HARD);
    } else {
        env->ICR = FIELD_DP32(env->ICR, ICR, PIPN, 0);
        cpu_reset_interrupt(cs, CPU_INTERRUPT_HARD);
    }
}

static void tricore_cpu_set_nmi(void* opaque, int irq, int level){
    TriCoreCPU *cpu = TRICORE_CPU(opaque);
    CPUState *cs = CPU(cpu);

    if (level) {
        cpu_interrupt(cs, CPU_INTERRUPT_NMI);
    } else {
        cpu_reset_interrupt(cs, CPU_INTERRUPT_NMI);
    }
}

static void tricore_cpu_initfn(Object *obj)
{
    TriCoreCPU *cpu      = TRICORE_CPU(obj);

    qdev_init_gpio_in_named(DEVICE(cpu), tricore_cpu_set_irq, "tricore.irq", 1);
    qdev_init_gpio_in_named(DEVICE(cpu), tricore_cpu_set_nmi, "tricore.nmi", 1);
}

static ObjectClass *tricore_cpu_class_by_name(const char *cpu_model)
{
    ObjectClass *oc;
    char *typename;
    char **cpuname;
    const char *cpunamestr;

    cpuname = g_strsplit(cpu_model, ",", 1);
    cpunamestr = cpuname[0];
    typename = g_strdup_printf(TRICORE_CPU_TYPE_NAME("%s"), cpunamestr);
    oc = object_class_by_name(typename);
    g_strfreev(cpuname);
    g_free(typename);

    return oc;
}

static void tc2x_initfn(Object *obj)
{
    TriCoreCPU *cpu = TRICORE_CPU(obj);

    set_feature(&cpu->env, TRICORE_FEATURE_161);
}

static void tc3x_initfn(Object *obj)
{
    TriCoreCPU *cpu = TRICORE_CPU(obj);

    set_feature(&cpu->env, TRICORE_FEATURE_162);
}

static void tc4x_initfn(Object *obj)
{
    TriCoreCPU *cpu = TRICORE_CPU(obj);

    set_feature(&cpu->env, TRICORE_FEATURE_18);
}

static void tricore_cpu_post_init(Object *obj)
{
    // TriCoreCPU *cpu = TRICORE_CPU(obj);
}

static G_NORETURN
void tricore_cpu_do_transaction_failed(CPUState *cs, hwaddr physaddr,
                                       vaddr addr, unsigned size,
                                       MMUAccessType access_type,
                                       int mmu_idx, MemTxAttrs attrs,
                                       MemTxResult response,
                                       uintptr_t retaddr)
{
    CPUTriCoreState *env = cpu_env(cs);
    uint8_t tin = (access_type == MMU_INST_FETCH) ? TIN4_PSE : TIN4_DSE;

    tricore_raise_exception(env, TRAPC_SYSBUS, tin, retaddr);
}

#include "hw/core/sysemu-cpu-ops.h"

static const struct SysemuCPUOps tricore_sysemu_ops = {
    .has_work = tricore_cpu_has_work,
    .get_phys_page_debug = tricore_cpu_get_phys_page_debug,
};

static const TCGCPUOps tricore_tcg_ops = {
    /* MTTCG not yet supported: require strict ordering */
    .guest_default_memory_order = TCG_MO_ALL,
    .mttcg_supported = false,
    .initialize = tricore_tcg_init,
    .translate_code = tricore_translate_code,
    .get_tb_cpu_state = tricore_get_tb_cpu_state,
    .synchronize_from_tb = tricore_cpu_synchronize_from_tb,
    .restore_state_to_opc = tricore_restore_state_to_opc,
    .mmu_index = tricore_cpu_mmu_index,
    .tlb_fill = tricore_cpu_tlb_fill,
    .pointer_wrap = cpu_pointer_wrap_uint32,
    .cpu_exec_interrupt = tricore_cpu_exec_interrupt,
    .cpu_exec_halt = tricore_cpu_has_work,
    .cpu_exec_reset = cpu_reset,
    .do_interrupt = tricore_cpu_do_interrupt,
    .do_transaction_failed = tricore_cpu_do_transaction_failed,
};

static const Property tricore_properties[] = {
    DEFINE_PROP_LINK("ir", TriCoreCPU, ir, TYPE_TRICORE_IR, TriCoreIRState *),
};

static void tricore_cpu_class_init(ObjectClass *c, const void *data)
{
    TriCoreCPUClass *mcc = TRICORE_CPU_CLASS(c);
    CPUClass *cc = CPU_CLASS(c);
    DeviceClass *dc = DEVICE_CLASS(c);
    ResettableClass *rc = RESETTABLE_CLASS(c);

    device_class_set_props(dc, tricore_properties);
    device_class_set_parent_realize(dc, tricore_cpu_realizefn,
                                    &mcc->parent_realize);
    resettable_class_set_parent_phases(rc, NULL, tricore_cpu_reset_hold, NULL,
                                       &mcc->parent_phases);
    cc->class_by_name = tricore_cpu_class_by_name;

    cc->gdb_read_register = tricore_cpu_gdb_read_register;
    cc->gdb_write_register = tricore_cpu_gdb_write_register;
    cc->gdb_num_core_regs = 44;
    cc->gdb_arch_name = tricore_gdb_arch_name;

    cc->dump_state = tricore_cpu_dump_state;
    cc->set_pc = tricore_cpu_set_pc;
    cc->get_pc = tricore_cpu_get_pc;
    cc->sysemu_ops = &tricore_sysemu_ops;
    cc->tcg_ops = &tricore_tcg_ops;
}

static void tricore_cpu_instance_init(Object *obj)
{
    TriCoreCPUClass *acc = TRICORE_CPU_GET_CLASS(obj);

    acc->info->initfn(obj);
    tricore_cpu_post_init(obj);
}

static void cpu_register_class_init(ObjectClass *oc, const void *data)
{
    TriCoreCPUClass *acc = TRICORE_CPU_CLASS(oc);
    CPUClass *cc = CPU_CLASS(acc);

    acc->info = data;
    if (acc->info->deprecation_note) {
        cc->deprecation_note = acc->info->deprecation_note;
    }
}

void tricore_cpu_register(const TriCoreCPUInfo *info)
{
    TypeInfo type_info = {
        .parent = TYPE_TRICORE_CPU,
        .instance_init = tricore_cpu_instance_init,
        .class_init = info->class_init ?: cpu_register_class_init,
        .class_data = info,
    };

    type_info.name = g_strdup_printf("%s-" TYPE_TRICORE_CPU, info->name);
    type_register_static(&type_info);
    g_free((void *)type_info.name);
}

static const TypeInfo tricore_cpu_type_info = {
    .name = TYPE_TRICORE_CPU,
    .parent = TYPE_CPU,
    .instance_size = sizeof(TriCoreCPU),
    .instance_align = __alignof__(TriCoreCPU),
    .instance_init = tricore_cpu_initfn,
    .instance_finalize = tricore_cpu_finalizefn,
    .abstract = true,
    .class_size = sizeof(TriCoreCPUClass),
    .class_init = tricore_cpu_class_init,
};

static void tricore_base_cpu_register_types(void)
{
    type_register_static(&tricore_cpu_type_info);
}

static void tricore_class_init(ObjectClass *oc, const void *data)
{
    TriCoreCPUClass *acc = TRICORE_CPU_CLASS(oc);
    CPUClass *cc = CPU_CLASS(oc);

    acc->info = data;
    cc->tcg_ops = &tricore_tcg_ops;
}

static const TriCoreCPUInfo tricore_cpus[] = {
    { .name = "tc4x", .initfn = tc4x_initfn, .class_init = tricore_class_init },
    { .name = "tc3x", .initfn = tc3x_initfn, .class_init = tricore_class_init },
    { .name = "tc2x", .initfn = tc2x_initfn, .class_init = tricore_class_init },
};

static void tricore_cpu_register_types(void)
{
    size_t i;

    for (i = 0; i < ARRAY_SIZE(tricore_cpus); ++i) {
        tricore_cpu_register(&tricore_cpus[i]);
    }
}

type_init(tricore_base_cpu_register_types)
type_init(tricore_cpu_register_types)
