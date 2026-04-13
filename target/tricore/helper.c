/*
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
#include "qemu/log.h"
#include "hw/core/registerfields.h"
#include "cpu.h"
#include "exec/cputlb.h"
#include "accel/tcg/cpu-mmu-index.h"
#include "exec/page-protection.h"
#include "exec/target_page.h"
#include "fpu/softfloat-helpers.h"
#include "qemu/qemu-print.h"
#include "system/memory.h"
#include "system/address-spaces.h"

enum {
    TLBRET_DIRTY = -4,
    TLBRET_INVALID = -3,
    TLBRET_NOMATCH = -2,
    TLBRET_BADADDR = -1,
    TLBRET_MATCH = 0
};

static int get_physical_address(CPUTriCoreState *env, hwaddr *physical,
                                int *prot, vaddr address,
                                MMUAccessType access_type, int mmu_idx)
{
    uint32_t seg = extract32(address, 28, 4);

    *physical = address & 0xFFFFFFFF;
    *prot = PAGE_READ | PAGE_WRITE | PAGE_EXEC;

    /*
     * PMA default segment attributes (TC1.6 Vol.1 Table 12, TC1.8 Vol.1):
     * Segments 0xE, 0xF are peripheral space - no code fetch allowed.
     * PMA2 reset value = 0x0000C000 (bits 15:14 = segs F and E).
     */
    if (seg == 0xF || seg == 0xE) {
        *prot = PAGE_READ | PAGE_WRITE;
    }

    return TLBRET_MATCH;
}

hwaddr tricore_cpu_get_phys_page_debug(CPUState *cs, vaddr addr)
{
    TriCoreCPU *cpu = TRICORE_CPU(cs);
    hwaddr phys_addr;
    int prot;
    int mmu_idx = cpu_mmu_index(cs, false);

    if (get_physical_address(&cpu->env, &phys_addr, &prot, addr,
                             MMU_DATA_LOAD, mmu_idx)) {
        return -1;
    }
    return phys_addr;
}

bool tricore_cpu_tlb_fill(CPUState *cs, vaddr address, int size,
                          MMUAccessType access_type, int mmu_idx,
                          bool probe, uintptr_t retaddr)
{
    CPUTriCoreState *env = cpu_env(cs);
    hwaddr physical;
    int prot;

    get_physical_address(env, &physical, &prot, address,
                         access_type, mmu_idx);

    qemu_log_mask(CPU_LOG_MMU, "%s address=0x%" VADDR_PRIx " ret 0 physical "
                  HWADDR_FMT_plx " prot %d access_type %d\n",
                  __func__, address, physical, prot, access_type);

    /* PSE: code fetch from peripheral space (seg E/F) */
    if (access_type == MMU_INST_FETCH && !(prot & PAGE_EXEC)) {
        if (probe) {
            return false;
        }
        raise_exception_sync_internal(env, TRAPC_SYSBUS, TIN4_PSE,
                                      retaddr, 0);
    }

    /*
     * DSE: data access to unmapped memory in non-peripheral segments.
     * Check if any MemoryRegion (RAM or MMIO) covers this address.
     * Skip segments E/F (peripheral space) - QEMU may have gaps in
     * MMIO coverage for unmodeled peripheral registers.
     */
    if (access_type != MMU_INST_FETCH) {
        uint32_t seg = extract32(address, 28, 4);
        if (seg != 0xE && seg != 0xF) {
            MemoryRegionSection section;
            section = memory_region_find(get_system_memory(), physical, 1);
            if (section.mr) {
                memory_region_unref(section.mr);
            } else {
                if (probe) {
                    return false;
                }
                raise_exception_sync_internal(env, TRAPC_SYSBUS, TIN4_DSE,
                                              retaddr, 0);
            }
        }
    }

    tlb_set_page(cs, address & TARGET_PAGE_MASK,
                 physical & TARGET_PAGE_MASK, prot,
                 mmu_idx, TARGET_PAGE_SIZE);
    return true;
}

void fpu_set_state(CPUTriCoreState *env)
{
    switch (extract32(env->PSW, 24, 2)) {
    case 0:
        set_float_rounding_mode(float_round_nearest_even, &env->fp_status);
        break;
    case 1:
        set_float_rounding_mode(float_round_up, &env->fp_status);
        break;
    case 2:
        set_float_rounding_mode(float_round_down, &env->fp_status);
        break;
    case 3:
        set_float_rounding_mode(float_round_to_zero, &env->fp_status);
        break;
    }

    set_flush_inputs_to_zero(1, &env->fp_status);
    set_flush_to_zero(1, &env->fp_status);
    set_float_detect_tininess(float_tininess_before_rounding, &env->fp_status);
    set_float_ftz_detection(float_ftz_before_rounding, &env->fp_status);
    set_default_nan_mode(1, &env->fp_status);
    /* Default NaN pattern: sign bit clear, frac msb set */
    set_float_default_nan_pattern(0b01000000, &env->fp_status);
}

uint32_t psw_read(CPUTriCoreState *env)
{
    /* clear all USB bits */
    env->PSW &= 0x7ffffff;
    /* now set them from the cache */
    env->PSW |= ((env->PSW_USB_C != 0) << 31);
    env->PSW |= ((env->PSW_USB_V   & (1 << 31))  >> 1);
    env->PSW |= ((env->PSW_USB_SV  & (1 << 31))  >> 2);
    env->PSW |= ((env->PSW_USB_AV  & (1 << 31))  >> 3);
    env->PSW |= ((env->PSW_USB_SAV & (1 << 31))  >> 4);

    return env->PSW;
}

void psw_write(CPUTriCoreState *env, uint32_t val)
{
    env->PSW_USB_C = (val & MASK_USB_C);
    env->PSW_USB_V = (val & MASK_USB_V) << 1;
    env->PSW_USB_SV = (val & MASK_USB_SV) << 2;
    env->PSW_USB_AV = (val & MASK_USB_AV) << 3;
    env->PSW_USB_SAV = (val & MASK_USB_SAV) << 4;
    env->PSW = val;

    fpu_set_state(env);
}

#define FIELD_GETTER_WITH_FEATURE(NAME, REG, FIELD, FEATURE)     \
uint32_t NAME(CPUTriCoreState *env)                             \
{                                                                \
    if (tricore_has_feature(env, TRICORE_FEATURE_##FEATURE)) {   \
        return FIELD_EX32(env->REG, REG, FIELD ## _ ## FEATURE); \
    }                                                            \
    return FIELD_EX32(env->REG, REG, FIELD ## _13);              \
}

#define FIELD_GETTER(NAME, REG, FIELD)       \
uint32_t NAME(CPUTriCoreState *env)         \
{                                            \
    return FIELD_EX32(env->REG, REG, FIELD); \
}

#define FIELD_SETTER_WITH_FEATURE(NAME, REG, FIELD, FEATURE)              \
void NAME(CPUTriCoreState *env, uint32_t val)                            \
{                                                                         \
    if (tricore_has_feature(env, TRICORE_FEATURE_##FEATURE)) {            \
        env->REG = FIELD_DP32(env->REG, REG, FIELD ## _ ## FEATURE, val); \
    }                                                                     \
    env->REG = FIELD_DP32(env->REG, REG, FIELD ## _13, val);              \
}

#define FIELD_SETTER(NAME, REG, FIELD)                \
void NAME(CPUTriCoreState *env, uint32_t val)        \
{                                                     \
    env->REG = FIELD_DP32(env->REG, REG, FIELD, val); \
}

FIELD_GETTER_WITH_FEATURE(pcxi_get_pcpn, PCXI, PCPN, 161)
FIELD_SETTER_WITH_FEATURE(pcxi_set_pcpn, PCXI, PCPN, 161)
FIELD_GETTER_WITH_FEATURE(pcxi_get_pie, PCXI, PIE, 161)
FIELD_SETTER_WITH_FEATURE(pcxi_set_pie, PCXI, PIE, 161)
FIELD_GETTER_WITH_FEATURE(pcxi_get_ul, PCXI, UL, 161)
FIELD_SETTER_WITH_FEATURE(pcxi_set_ul, PCXI, UL, 161)
FIELD_GETTER(pcxi_get_pcxs, PCXI, PCXS)
FIELD_GETTER(pcxi_get_pcxo, PCXI, PCXO)

FIELD_GETTER_WITH_FEATURE(icr_get_ie, ICR, IE, 161)
FIELD_SETTER_WITH_FEATURE(icr_set_ie, ICR, IE, 161)
FIELD_GETTER(icr_get_ccpn, ICR, CCPN)
FIELD_SETTER(icr_set_ccpn, ICR, CCPN)
