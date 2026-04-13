
#include "qemu/osdep.h"
#include "exec/cpu-interrupt.h"
#include "hw/intc/tricore_ir.h"
#include "qemu/bitops.h"
#include "qemu/plugin.h"
#include "qemu/typedefs.h"
#include "accel/tcg/cpu-ldst.h"
#include "accel/tcg/getpc.h"
#include "cpu-qom.h"
#include "cpu.h"
#include "target/riscv/cpu_bits.h"

static bool tricore_cpu_interruptable(CPUTriCoreState *env)
{
    uint32_t ie_mask = env->features & TRICORE_FEATURE_16 ? BIT(15) : BIT(8);

    if ((env->ICR & ie_mask) &&
        extract32(env->ICR, 16, 8) > extract32(env->ICR, 0, 8)) {
        return true;
    }
    return false;
}

void tricore_cpu_do_interrupt(CPUState *cs)
{
    ArchCPU *cpu = TRICORE_CPU(cs);
    CPUArchState *env = &cpu->env;
    uint64_t last_pc = env->PC;
    uint32_t temp_FCX = env->FCX;
    uint32_t pipn = get_field(env->ICR, MASK_ICR_PIPN);
    uint16_t irq_id = get_field(cpu->ir->lwsr[0], R_LWSR_ID_MASK);
    const uint64_t pie_mask = env->features & TRICORE_FEATURE_161 ?
                                  R_PCXI_PIE_161_MASK :
                                  R_PCXI_PIE_13_MASK;
    const uint64_t pcpn_mask = env->features & TRICORE_FEATURE_161 ?
                                   R_PCXI_PCPN_161_MASK :
                                   R_PCXI_PCPN_13_MASK;
    const uint64_t ie_mask = env->features & TRICORE_FEATURE_161 ?
                                 R_ICR_IE_161_MASK :
                                 R_ICR_IE_13_MASK;
    const uint64_t ul_mask = env->features & TRICORE_FEATURE_161 ?
                                 R_PCXI_UL_161_MASK :
                                 R_PCXI_UL_13_MASK;

    if (cs->exception_index > EXCP_IRQ) {
        cpu_abort(cs, "Unhandled exception 0x%x\n", cs->exception_index);
        return;
    }

    /* Skip the saving of the context if it's an FCU trap */
    if (cs->exception_index != EXCP_CTX_MNG && env->tin != TIN3_FCU) {
        uint32_t ea = ((env->FCX & MASK_FCX_FCXS) << 12) +
                      ((env->FCX & MASK_FCX_FCXO) << 6);

        uint32_t new_FCX = cpu_ldl_le_data(env, ea);

        tricore_store_context_upper(env, ea);
        env->PCXI = (temp_FCX & 0xFFFFF) | ul_mask;
        env->FCX = new_FCX;
        env->gpr_a[11] =
            cs->exception_index == EXCP_SYSCALL ? env->PC + 4 : env->PC;
    }

    env->gpr_d[15] = cs->exception_index == EXCP_IRQ ? 0 : env->tin;
    env->gpr_a[10] = psw_read(env) & MASK_PSW_IS ? env->gpr_a[10] : env->ISP;
    env->PSW = set_field(env->PSW, MASK_PSW_IO, TRICORE_PRIV_SM);
    env->PSW = set_field(env->PSW, MASK_PSW_PRS, 0);
    env->PSW = set_field(env->PSW, MASK_PSW_PRS2, 0);
    env->PSW = set_field(env->PSW, MASK_PSW_CDC, 0);
    env->PSW = set_field(env->PSW, MASK_PSW_CDE, 1);
    env->PSW =
        set_field(env->PSW, MASK_PSW_S, !!(env->SYSCON & MASK_SYSCON_IS));
    env->PSW = set_field(env->PSW, MASK_PSW_GW, 0);

    uint32_t ie = get_field(env->ICR, ie_mask);
    env->ICR = set_field(env->ICR, ie_mask, 0);

    /* Skip PCXI update if it's an FCU trap */
    if (cs->exception_index != EXCP_CTX_MNG && env->tin != TIN3_FCU) {
        env->PCXI = set_field(env->PCXI, pie_mask, ie);
        env->PCXI =
            set_field(env->PCXI, pcpn_mask, get_field(env->ICR, MASK_ICR_CCPN));
    }
    /* Update ICR.CCPN if it's an IRQ */
    if (cs->exception_index == EXCP_IRQ) {
        env->ICR = set_field(env->ICR, MASK_ICR_CCPN,
                             get_field(env->ICR, MASK_ICR_PIPN));
    }

    /* Calculate the next PC */
    if (cs->exception_index == EXCP_IRQ) {
        uint32_t v_offset =
            (env->BIV & MASK_BIV_VSS) == 0 ? pipn << 5 : pipn << 3;
        env->PC = (env->BIV & 0xFFFFFFFE) | v_offset;

    } else {
        env->PC = (env->BTV & 0xFFFFFFFE) | (cs->exception_index << 5);
    }

    /* Raise a trap if context list is depleted */
    if (temp_FCX == env->LCX) {
        tricore_raise_exception(env, TRAPC_CTX_MNG, TIN3_FCU, GETPC());
    }

    if (cs->exception_index == EXCP_IRQ) {
        tricore_ir_irq_acknowledge(cpu->ir, irq_id, 0);
        qemu_plugin_vcpu_interrupt_cb(cs, last_pc);
    } else {
        qemu_plugin_vcpu_exception_cb(cs, last_pc);
    }
}

bool tricore_cpu_exec_interrupt(CPUState *cs, int interrupt_request)
{
    ArchCPU *cpu = TRICORE_CPU(cs);
    CPUArchState *env = &cpu->env;

    if (interrupt_request & CPU_INTERRUPT_HARD) {
        if (tricore_cpu_interruptable(env)) {
            cs->exception_index = EXCP_IRQ;
            tricore_cpu_do_interrupt(cs);
            return true;
        }
    }
    if (interrupt_request & CPU_INTERRUPT_NMI) {
        cs->exception_index = EXCP_NMI;
        tricore_cpu_do_interrupt(cs);
        return true;
    }
    return false;
}