/*
 * Infineon tc39x SoC System emulation.
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

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/sysbus.h"
#include "hw/core/loader.h"
#include "qemu/units.h"
#include "hw/misc/unimp.h"
#include "hw/core/clock.h"
#include "hw/core/qdev-clock.h"

#include "hw/tricore/tc39xb_soc.h"
#include "hw/tricore/triboard.h"
#include "qemu/log.h"

static uint64_t tc39x_cpu_sfr_read(void *opaque, hwaddr offset,
                                   unsigned size)
{
    TC39XBCPUSFRState *s = opaque;
    switch (offset) {
    case 0x1FE08: return s->boot_pc;
    case 0x1FE60: return s->bootcon;
    case 0x1FE14: return s->syscon;
    default: return 0;
    }
}

static void tc39x_cpu_sfr_write(void *opaque, hwaddr offset,
                                uint64_t value, unsigned size)
{
    TC39XBCPUSFRState *s = opaque;
    switch (offset) {
    case 0x1FE08:
        s->boot_pc = value;
        break;
    case 0x1FE60:
        s->bootcon = value;
        if (!(value & (1u << 24)) && s->id > 0) {
            CPUState *cs = CPU(&s->soc->cpus[s->id]);
            s->soc->cpus[s->id].env.PC = s->boot_pc;
            cs->halted = 0;
            cpu_resume(cs);
        }
        break;
    case 0x1FE14:
        s->syscon = value;
        /* BHALT is bit 24 in CPUx_SYSCON.  Clearing it releases the
         * secondary core after the SSW has programmed CPUx_PC. */
        if (!(value & (1u << 24)) && s->id > 0) {
            CPUState *cs = CPU(&s->soc->cpus[s->id]);
            s->soc->cpus[s->id].env.PC = s->boot_pc;
            cs->halted = 0;
            cpu_resume(cs);
        }
        break;
    default:
        break;
    }
}

static const MemoryRegionOps tc39x_cpu_sfr_ops = {
    .read = tc39x_cpu_sfr_read,
    .write = tc39x_cpu_sfr_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};


const MemmapEntry tc39xb_soc_memmap[] = {
    [TC39XB_DSPR5]     = { 0x10000000,             240 * KiB },
    [TC39XB_DCACHE5]   = { 0x10018000,             16 * KiB },
    [TC39XB_DTAG5]     = { 0x100C0000,              6 * KiB },
    [TC39XB_PSPR5]     = { 0x10100000,             64 * KiB },
    [TC39XB_PCACHE5]   = { 0x10108000,             32 * KiB },
    [TC39XB_PTAG5]     = { 0x101C0000,             12 * KiB },

    [TC39XB_DSPR4]     = { 0x30000000,             240 * KiB },
    [TC39XB_DCACHE4]   = { 0x30018000,             16 * KiB },
    [TC39XB_DTAG4]     = { 0x300C0000,              6 * KiB },
    [TC39XB_PSPR4]     = { 0x30100000,             64 * KiB },
    [TC39XB_PCACHE4]   = { 0x30108000,             32 * KiB },
    [TC39XB_PTAG4]     = { 0x301C0000,             12 * KiB },

    [TC39XB_DSPR3]     = { 0x40000000,             240 * KiB },
    [TC39XB_DCACHE3]   = { 0x40018000,             16 * KiB },
    [TC39XB_DTAG3]     = { 0x400C0000,              6 * KiB },
    [TC39XB_PSPR3]     = { 0x40100000,             64 * KiB },
    [TC39XB_PCACHE3]   = { 0x40108000,             32 * KiB },
    [TC39XB_PTAG3]     = { 0x401C0000,             12 * KiB },

    [TC39XB_DSPR2]     = { 0x50000000,             240 * KiB },
    [TC39XB_DCACHE2]   = { 0x5001E000,             16 * KiB },
    [TC39XB_DTAG2]     = { 0x500C0000,              6 * KiB },
    [TC39XB_PSPR2]     = { 0x50100000,             64 * KiB },
    [TC39XB_PCACHE2]   = { 0x50108000,             32 * KiB },
    [TC39XB_PTAG2]     = { 0x501C0000,             12 * KiB },

    [TC39XB_DSPR1]     = { 0x60000000,             240 * KiB },
    [TC39XB_DCACHE1]   = { 0x6001E000,             16 * KiB },
    [TC39XB_DTAG1]     = { 0x600C0000,              6 * KiB },
    [TC39XB_PSPR1]     = { 0x60100000,             64 * KiB },
    [TC39XB_PCACHE1]   = { 0x60108000,             32 * KiB },
    [TC39XB_PTAG1]     = { 0x601C0000,             12 * KiB },

    [TC39XB_DSPR0]     = { 0x70000000,             240 * KiB },
    [TC39XB_DCACHE0]   = { 0x7001E000,             16 * KiB },
    [TC39XB_DTAG0]     = { 0x700C0000,              6 * KiB },
    [TC39XB_PSPR0]     = { 0x70100000,             64 * KiB },
    [TC39XB_PCACHE0]   = { 0x70108000,             32 * KiB },
    [TC39XB_PTAG0]     = { 0x701C0000,             12 * KiB },

    [TC39XB_PFLASH0_C] = { 0x80000000,              3 * MiB },
    [TC39XB_PFLASH1_C] = { 0x80300000,              3 * MiB },
    [TC39XB_PFLASH2_C] = { 0x80600000,              3 * MiB },
    [TC39XB_PFLASH3_C] = { 0x80900000,              3 * MiB },
    [TC39XB_PFLASH4_C] = { 0x80C00000,              3 * MiB },
    [TC39XB_PFLASH5_C] = { 0x80F00000,              1 * MiB },

    [TC39XB_OLDA_C]    = { 0x8FE00000,            512 * KiB },
    [TC39XB_BROM_C]    = { 0x8FFF0000,             64 * KiB },

    [TC39XB_DLMU0_C]   = { 0x90000000,             64 * KiB },
    [TC39XB_DLMU1_C]   = { 0x90010000,             64 * KiB },
    [TC39XB_DLMU2_C]   = { 0x90020000,             64 * KiB },
    [TC39XB_DLMU3_C]   = { 0x90030000,             64 * KiB },
    [TC39XB_LMU0_C]    = { 0x90040000,            256 * KiB },
    [TC39XB_LMU1_C]    = { 0x90080000,            256 * KiB },
    [TC39XB_LMU2_C]    = { 0x900c0000,            256 * KiB },
    [TC39XB_DLMU4_C]   = { 0x90100000,             64 * KiB },
    [TC39XB_DLMU5_C]   = { 0x90110000,             64 * KiB },
    [TC39XB_EMEM]      = { 0x99000000,              4 * MiB },

    [TC39XB_PFLASH0_U] = { 0xA0000000,                  0x0 },
    [TC39XB_PFLASH1_U] = { 0xA0300000,                  0x0 },
    [TC39XB_PFLASH2_U] = { 0xA0600000,                  0x0 },
    [TC39XB_PFLASH3_U] = { 0xA0900000,                  0x0 },
    [TC39XB_PFLASH4_U] = { 0xA0C00000,                  0x0 },
    [TC39XB_PFLASH5_U] = { 0xA0F00000,                  0x0 },

    [TC39XB_DFLASH0]   = { 0xAF000000,              1 * MiB },
    [TC39XB_DFLASH1]   = { 0xAFC00000,            128 * KiB },

    [TC39XB_OLDA_U]    = { 0xAFE00000,                  0x0 },
    [TC39XB_BROM_U]    = { 0xAFFF0000,                  0x0 },

    [TC39XB_DLMU0_U]   = { 0xB0000000,                  0x0 },
    [TC39XB_DLMU1_U]   = { 0xB0010000,                  0x0 },
    [TC39XB_DLMU2_U]   = { 0xB0020000,                  0x0 },
    [TC39XB_DLMU3_U]   = { 0xB0030000,                  0x0 },
    [TC39XB_LMU0_U]    = { 0xB0040000,                  0x0 },
    [TC39XB_LMU1_U]    = { 0xB0080000,                  0x0 },
    [TC39XB_LMU2_U]    = { 0xB00C0000,                  0x0 },
    [TC39XB_DLMU4_U]   = { 0xB0100000,                  0x0 },
    [TC39XB_DLMU5_U]   = { 0xB0110000,                  0x0 },

    [TC39XB_VIRT]      = { 0xBF000000,                  0x0 },

    [TC39XB_PSPRX]     = { 0xC0000000,                  0x0 },
    [TC39XB_DSPRX]     = { 0xD0000000,                  0x0 },

    [TC39XB_SFR]       = { 0xF0000000,                  0x0 },
    [TC39XB_STM]       = { 0xF0001000,                  0x0 },
    [TC39XB_ASCLIN]    = { 0xF0000600,                  0x0 },
    [TC39XB_SCU]       = { 0xF0036000,                  0x0 },
    [TC39XB_IRBUS]     = { 0xF0037000,                  0x0 },
};

/*
 * Initialize the auxiliary ROM region @mr and map it into
 * the memory map at @base.
 */
static void make_rom(MemoryRegion *mr, const char *name,
                     hwaddr base, hwaddr size)
{
    memory_region_init_rom(mr, NULL, name, size, &error_fatal);
    memory_region_add_subregion(get_system_memory(), base, mr);
}

/*
 * Initialize the auxiliary RAM region @mr and map it into
 * the memory map at @base.
 */
static void make_ram(MemoryRegion *mr, const char *name,
                     hwaddr base, hwaddr size)
{
    memory_region_init_ram(mr, NULL, name, size, &error_fatal);
    memory_region_add_subregion(get_system_memory(), base, mr);
}

/*
 * Create an alias of an entire original MemoryRegion @orig
 * located at @base in the memory map.
 */
static void make_alias(MemoryRegion *mr, const char *name,
                           MemoryRegion *orig, hwaddr base)
{
    memory_region_init_alias(mr, NULL, name, orig, 0,
                             memory_region_size(orig));
    memory_region_add_subregion(get_system_memory(), base, mr);
}


static void tc39x_soc_init_memory_mapping(DeviceState *dev_soc)
{
    TC39XBSoCState *s = TC39XB_SOC(dev_soc);
    TC39XBSoCClass *sc = TC39XB_SOC_GET_CLASS(s);

    /* shortcuts to clean up code */
    const MemmapEntry *map = sc->memmap;
    TC39XBSoCCPUMemState *c0 = &s->cpu0mem;
    TC39XBSoCCPUMemState *c1 = &s->cpu1mem;
    TC39XBSoCCPUMemState *c2 = &s->cpu2mem;
    TC39XBSoCCPUMemState *c3 = &s->cpu3mem;
    TC39XBSoCCPUMemState *c4 = &s->cpu4mem;
    TC39XBSoCCPUMemState *c5 = &s->cpu5mem;
    TC39XBSoCFlashMemState *f = &s->flashmem;

    make_ram(&c0->dspr, "CPU0.DSPR", map[TC39XB_DSPR0].base, map[TC39XB_DSPR0].size);
    make_ram(&c0->pspr, "CPU0.PSPR", map[TC39XB_PSPR0].base, map[TC39XB_PSPR0].size);
    make_ram(&c1->dspr, "CPU1.DSPR", map[TC39XB_DSPR1].base, map[TC39XB_DSPR1].size);
    make_ram(&c1->pspr, "CPU1.PSPR", map[TC39XB_PSPR1].base, map[TC39XB_PSPR1].size);
    make_ram(&c2->dspr, "CPU2.DSPR", map[TC39XB_DSPR2].base, map[TC39XB_DSPR2].size);
    make_ram(&c2->pspr, "CPU2.PSPR", map[TC39XB_PSPR2].base, map[TC39XB_PSPR2].size);
    make_ram(&c3->dspr, "CPU3.DSPR", map[TC39XB_DSPR3].base, map[TC39XB_DSPR3].size);
    make_ram(&c3->pspr, "CPU3.PSPR", map[TC39XB_PSPR3].base, map[TC39XB_PSPR3].size);
    make_ram(&c4->dspr, "CPU4.DSPR", map[TC39XB_DSPR4].base, map[TC39XB_DSPR4].size);
    make_ram(&c4->pspr, "CPU4.PSPR", map[TC39XB_PSPR4].base, map[TC39XB_PSPR4].size);
    make_ram(&c5->dspr, "CPU5.DSPR", map[TC39XB_DSPR5].base, map[TC39XB_DSPR5].size);
    make_ram(&c5->pspr, "CPU5.PSPR", map[TC39XB_PSPR5].base, map[TC39XB_PSPR5].size);

    /*
     * TriCore QEMU executes CPU0 only, thus it is sufficient to map
     * LOCAL.PSPR/LOCAL.DSPR exclusively onto PSPR0/DSPR0.
     */
    make_alias(&s->psprX, "LOCAL.PSPR", &c0->pspr, map[TC39XB_PSPRX].base);
    make_alias(&s->dsprX, "LOCAL.DSPR", &c0->dspr, map[TC39XB_DSPRX].base);

    make_ram(&c0->pflash_c, "PF0", map[TC39XB_PFLASH0_C].base, map[TC39XB_PFLASH0_C].size);
    make_ram(&c1->pflash_c, "PF1", map[TC39XB_PFLASH1_C].base, map[TC39XB_PFLASH1_C].size);
    make_ram(&c2->pflash_c, "PF2", map[TC39XB_PFLASH2_C].base, map[TC39XB_PFLASH2_C].size);
    make_ram(&c3->pflash_c, "PF3", map[TC39XB_PFLASH3_C].base, map[TC39XB_PFLASH3_C].size);
    make_ram(&c4->pflash_c, "PF4", map[TC39XB_PFLASH4_C].base, map[TC39XB_PFLASH4_C].size);
    make_ram(&c5->pflash_c, "PF5", map[TC39XB_PFLASH5_C].base, map[TC39XB_PFLASH5_C].size);

    make_ram(&c0->dlmu_c, "DLMU0", map[TC39XB_DLMU0_C].base, map[TC39XB_DLMU0_C].size);
    make_ram(&c1->dlmu_c, "DLMU1", map[TC39XB_DLMU1_C].base, map[TC39XB_DLMU1_C].size);
    make_ram(&c2->dlmu_c, "DLMU2", map[TC39XB_DLMU2_C].base, map[TC39XB_DLMU2_C].size);
    make_ram(&c3->dlmu_c, "DLMU3", map[TC39XB_DLMU3_C].base, map[TC39XB_DLMU3_C].size);
    make_ram(&c4->dlmu_c, "DLMU4", map[TC39XB_DLMU4_C].base, map[TC39XB_DLMU4_C].size);
    make_ram(&c5->dlmu_c, "DLMU5", map[TC39XB_DLMU5_C].base, map[TC39XB_DLMU5_C].size);

    make_ram(&f->dflash0,   "DF0", map[TC39XB_DFLASH0].base, map[TC39XB_DFLASH0].size);
    make_ram(&f->dflash1,   "DF1", map[TC39XB_DFLASH1].base, map[TC39XB_DFLASH1].size);
    make_ram(&f->olda_c,   "OLDA", map[TC39XB_OLDA_C].base, map[TC39XB_OLDA_C].size);
    make_rom(&f->brom_c,   "BROM", map[TC39XB_BROM_C].base, map[TC39XB_BROM_C].size);
    make_ram(&f->lmu0_c,   "LMU0", map[TC39XB_LMU0_C].base, map[TC39XB_LMU0_C].size);
    make_ram(&f->lmu1_c,   "LMU1", map[TC39XB_LMU1_C].base, map[TC39XB_LMU1_C].size);
    make_ram(&f->lmu2_c,   "LMU2", map[TC39XB_LMU2_C].base, map[TC39XB_LMU2_C].size);
    make_ram(&f->emem,     "EMEM", map[TC39XB_EMEM].base, map[TC39XB_EMEM].size);

    make_alias(&c0->pflash_u, "PF0.U", &c0->pflash_c, map[TC39XB_PFLASH0_U].base);
    make_alias(&c1->pflash_u, "PF1.U", &c1->pflash_c, map[TC39XB_PFLASH1_U].base);
    make_alias(&c2->pflash_u, "PF2.U", &c2->pflash_c, map[TC39XB_PFLASH2_U].base);
    make_alias(&c3->pflash_u, "PF3.U", &c3->pflash_c, map[TC39XB_PFLASH3_U].base);
    make_alias(&c4->pflash_u, "PF4.U", &c4->pflash_c, map[TC39XB_PFLASH4_U].base);
    make_alias(&c5->pflash_u, "PF5.U", &c5->pflash_c, map[TC39XB_PFLASH5_U].base);
    make_alias(&c0->dlmu_u, "DLMU0.U", &c0->dlmu_c, map[TC39XB_DLMU0_U].base);
    make_alias(&c1->dlmu_u, "DLMU1.U", &c1->dlmu_c, map[TC39XB_DLMU1_U].base);
    make_alias(&c2->dlmu_u, "DLMU2.U", &c2->dlmu_c, map[TC39XB_DLMU2_U].base);
    make_alias(&c3->dlmu_u, "DLMU3.U", &c3->dlmu_c, map[TC39XB_DLMU3_U].base);
    make_alias(&c4->dlmu_u, "DLMU4.U", &c4->dlmu_c, map[TC39XB_DLMU4_U].base);
    make_alias(&c5->dlmu_u, "DLMU5.U", &c5->dlmu_c, map[TC39XB_DLMU5_U].base);

    make_alias(&f->olda_u,  "OLDA.U", &f->olda_c, map[TC39XB_OLDA_U].base);
    make_alias(&f->brom_u,  "BROM.U", &f->brom_c, map[TC39XB_BROM_U].base);
    make_alias(&f->lmu0_u,  "LMU0.U", &f->lmu0_c, map[TC39XB_LMU0_U].base);
    make_alias(&f->lmu1_u,  "LMU1.U", &f->lmu1_c, map[TC39XB_LMU1_U].base);
    make_alias(&f->lmu2_u,  "LMU2.U", &f->lmu2_c, map[TC39XB_LMU2_U].base);
}

#define TC3X_SRC_STM0_SR0       0xC0
#define TC3X_SRC_ASCLIN0_TX     0x14
#define TC3X_SRC_ASCLIN0_RX     0x15
#define TC3X_SRC_ASCLIN0_ERR    0x16
/* Public TC3x ERAY0/ERAY1 service-request slots. */
#define TC3X_SRC_ERAY0_INT0     160
#define TC3X_SRC_ERAY0_INT1     161
#define TC3X_SRC_ERAY1_INT0     162
#define TC3X_SRC_ERAY1_INT1     163
/* Generation-specific ERAY service-request table from the public TC3x
 * User Manual / IfxEray headers. */
static const uint16_t tc3x_eray_src[2][2] = {
    { TC3X_SRC_ERAY0_INT0, TC3X_SRC_ERAY0_INT1 },
    { TC3X_SRC_ERAY1_INT0, TC3X_SRC_ERAY1_INT1 },
};

static void tc39x_soc_realize(DeviceState *dev_soc, Error **errp)
{
    TC39XBSoCState *s = TC39XB_SOC(dev_soc);
    TC39XBSoCClass *sc = TC39XB_SOC_GET_CLASS(s);
    Error *err = NULL;

    s->irbus = TRICORE_IR(object_new(TYPE_TRICORE_IR));
    s->asclin = TRICORE_ASCLIN(object_new(TYPE_TRICORE_ASCLIN));
    for (unsigned i = 0; i < 11; i++) {
        s->asclin_extra[i] = TRICORE_ASCLIN(object_new(TYPE_TRICORE_ASCLIN));
    }
    s->virt = TRICORE_VIRT(object_new(TYPE_TRICORE_VIRT));
    s->scu = TRICORE_SCU(object_new(TYPE_TRICORE_SCU));
    s->stm = TRICORE_STM(object_new(TYPE_TRICORE_STM));
    s->sfr = TRICORE_SFR(object_new(TYPE_TRICORE_SFR));
    for (unsigned i = 0; i < 3; i++) {
        s->mcan[i] = TRICORE_MCAN(object_new(TYPE_TRICORE_MCAN));
    }
    s->eth = TRICORE_ETH(object_new(TYPE_TRICORE_GETH));
    for (unsigned i = 0; i < 2; i++) {
        s->eray[i] = TRICORE_ERAY(object_new(TYPE_TRICORE_ERAY));
        /* TC3x public profile exposes a 16 KiB ERAY message window. */
        qdev_prop_set_uint32(DEVICE(s->eray[i]), "message-ram-size", 0x4000);
        qdev_prop_set_uint32(DEVICE(s->eray[i]), "payload-max", 64);
    }

    /* Parent all devices so sysbus_realize_and_unref does not free them */
    object_property_add_child(OBJECT(dev_soc), "irbus", OBJECT(s->irbus));
    object_property_add_child(OBJECT(dev_soc), "asclin", OBJECT(s->asclin));
    for (unsigned i = 0; i < 11; i++) {
        char *name = g_strdup_printf("asclin%u", i + 1);
        object_property_add_child(OBJECT(dev_soc), name,
                                   OBJECT(s->asclin_extra[i]));
        g_free(name);
    }
    object_property_add_child(OBJECT(dev_soc), "virt", OBJECT(s->virt));
    object_property_add_child(OBJECT(dev_soc), "scu", OBJECT(s->scu));
    object_property_add_child(OBJECT(dev_soc), "stm", OBJECT(s->stm));
    object_property_add_child(OBJECT(dev_soc), "sfr", OBJECT(s->sfr));
    for (unsigned i = 0; i < 3; i++) {
        char *name = g_strdup_printf("mcan%u", i);
        object_property_add_child(OBJECT(dev_soc), name,
                                  OBJECT(s->mcan[i]));
        g_free(name);
    }
    object_property_add_child(OBJECT(dev_soc), "eth", OBJECT(s->eth));
    for (unsigned i = 0; i < 2; i++) {
        char *name = g_strdup_printf("eray%u", i);
        object_property_add_child(OBJECT(dev_soc), name, OBJECT(s->eray[i]));
        g_free(name);
    }

    /* IR properties */
    qdev_prop_set_bit(DEVICE(s->irbus), "tc4x-mode", false);
    qdev_prop_set_uint8(DEVICE(s->irbus), "num-isps", 6);
    qdev_prop_set_uint16(DEVICE(s->irbus), "num-irqs", 1024);

    /* CPU needs IR link before realize */
    object_property_set_link(OBJECT(&s->cpus[0]), "ir",
                             OBJECT(s->irbus), &error_abort);

    qdev_realize(DEVICE(&s->cpus[0]), NULL, &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    for (unsigned i = 1; i < 6; i++) {
        DeviceState *core = DEVICE(&s->cpus[i]);
        object_property_set_link(OBJECT(core), "ir", OBJECT(s->irbus),
                                 &error_abort);
        object_property_set_bool(OBJECT(core), "start-powered-off", true,
                                 &error_abort);
        if (!qdev_realize(core, NULL, &err)) {
            error_propagate(errp, err);
            return;
        }
    }

    tc39x_soc_init_memory_mapping(dev_soc);

    MemoryRegion *sysmem = get_system_memory();

    /* TC3x iLLD default: fPLL=300 MHz, STMDIV=3 => fSTM=100 MHz. */
    Clock *fstm = clock_new(OBJECT(dev_soc), "fstm");
    clock_set_hz(fstm, 100000000);
    qdev_connect_clock_in(DEVICE(s->stm), "fstm", fstm);

    object_property_add_const_link(OBJECT(s->scu), "cpu", OBJECT(&s->cpus[0]));
    qdev_prop_set_chr(DEVICE(s->asclin), "chardev", serial_hd(0));
    if (s->canbus) {
        for (unsigned i = 0; i < 3; i++) {
            object_property_set_link(OBJECT(s->mcan[i]), "canbus",
                                     OBJECT(s->canbus), &error_abort);
        }
    }

    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->sfr), &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->scu), &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->stm), &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->irbus), &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->virt), &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->asclin), &error_fatal);
    for (unsigned i = 0; i < 3; i++) {
        sysbus_realize_and_unref(SYS_BUS_DEVICE(s->mcan[i]), &error_fatal);
    }
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->eth), &error_fatal);
    for (unsigned i = 0; i < 2; i++) {
        sysbus_realize_and_unref(SYS_BUS_DEVICE(s->eray[i]), &error_fatal);
    }
    s->dma = TRICORE_DMA(object_new(TYPE_TRICORE_DMA));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->dma), &error_fatal);
    for (unsigned i = 0; i < 11; i++) {
        DeviceState *extra = DEVICE(s->asclin_extra[i]);
        qdev_prop_set_chr(extra, "chardev", serial_hd(i + 1));
        sysbus_realize_and_unref(SYS_BUS_DEVICE(extra), &error_fatal);
    }

    /* TC3x CPU-local SFRs (including CPUx_KRST0/KRST1) are not modeled yet.
     * Map the documented local window explicitly so startup accesses are
     * visible as unimplemented instead of falling through an unmapped hole. */
    /* TC397B has a hole before CPU5's local window (CPU5 is at F88C0000,
     * unlike the 0x20000-stepped CPU0..CPU4 windows). */
    static const hwaddr cpu_sfr_base[] = {
        0xF8800000, 0xF8820000, 0xF8840000,
        0xF8860000, 0xF8880000, 0xF88C0000,
    };
    for (unsigned i = 0; i < 6; i++) {
        TC39XBCPUSFRState *sfr = &s->cpu_sfr[i];
        sfr->soc = s;
        sfr->id = i;
        sfr->bootcon = i ? 1 : 0;
        sfr->syscon = i ? (1u << 24) : 0;
        char *name = g_strdup_printf("tc39x-cpu%u-local-sfr", i);
        memory_region_init_io(&sfr->region, OBJECT(s), &tc39x_cpu_sfr_ops,
                              sfr, name, 0x20000);
        g_free(name);
        memory_region_add_subregion(sysmem, cpu_sfr_base[i], &sfr->region);
    }

    /* IR MMIO: idx 0 = int_region (F0037000), idx 1 = src_region (F0038000) */
    sysbus_mmio_map(SYS_BUS_DEVICE(s->irbus), 0, 0xF0037000);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->irbus), 1, 0xF0038000);

    /* IR ISP[0] -> CPU tricore.irq */
    qdev_connect_gpio_out_named(DEVICE(s->irbus), "isp", 0,
        qdev_get_gpio_in_named(DEVICE(&s->cpus[0]), "tricore.irq", 0));
    for (unsigned i = 1; i < 6; i++) {
        qdev_connect_gpio_out_named(DEVICE(s->irbus), "isp", i,
            qdev_get_gpio_in_named(DEVICE(&s->cpus[i]), "tricore.irq", 0));
    }

    /* ASCLIN0: sysbus 0=RXSR, 1=TXSR, 2=EXSR */
    sysbus_connect_irq(SYS_BUS_DEVICE(s->asclin), 0,
        qdev_get_gpio_in_named(DEVICE(s->irbus), "irq",
                               TC3X_SRC_ASCLIN0_RX));
    sysbus_connect_irq(SYS_BUS_DEVICE(s->asclin), 1,
        qdev_get_gpio_in_named(DEVICE(s->irbus), "irq",
                               TC3X_SRC_ASCLIN0_TX));
    sysbus_connect_irq(SYS_BUS_DEVICE(s->asclin), 2,
        qdev_get_gpio_in_named(DEVICE(s->irbus), "irq",
                               TC3X_SRC_ASCLIN0_ERR));

    /* STM0 SR0 */
    sysbus_connect_irq(SYS_BUS_DEVICE(s->stm), 0,
        qdev_get_gpio_in_named(DEVICE(s->irbus), "irq",
                               TC3X_SRC_STM0_SR0));

   /* SFR is a catch-all - low priority so specific devices take precedence */
    memory_region_add_subregion_overlap(sysmem, sc->memmap[TC39XB_SFR].base,
                                        &s->sfr->iomem, -1);
    memory_region_add_subregion(sysmem, sc->memmap[TC39XB_ASCLIN].base,
                                &s->asclin->iomem);
    /* TC3xx MCMCAN0 control window (iLLD/User Manual base). */
    static const hwaddr mcan_base[3] = {
        0xF0200000, 0xF0210000, 0xF0220000,
    };
    for (unsigned i = 0; i < 3; i++) {
        memory_region_add_subregion(sysmem, mcan_base[i],
                                    &s->mcan[i]->iomem);
        memory_region_add_subregion(sysmem, mcan_base[i] + 0x3000,
                                    &s->mcan[i]->msg_ram);
        /* MCMCAN0..2 expose 16 service requests each.  Keep the controller
         * channels independent while routing them through the shared IR. */
        for (unsigned irq = 0; irq < 16; irq++) {
            sysbus_connect_irq(SYS_BUS_DEVICE(s->mcan[i]), irq,
                qdev_get_gpio_in_named(DEVICE(s->irbus), "irq",
                                       176 + i * 16 + irq));
        }
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(s->dma), 0, 0xF0010000);
    memory_region_add_subregion(sysmem, 0xF001D000, &s->eth->iomem);
    sysbus_connect_irq(SYS_BUS_DEVICE(s->eth), 0,
        qdev_get_gpio_in_named(DEVICE(s->irbus), "irq", 175));
    for (unsigned i = 0; i < 2; i++) {
        hwaddr base = i ? 0xF0017000 : 0xF001C000;
        memory_region_add_subregion(sysmem, base, &s->eray[i]->iomem);
        memory_region_add_subregion(sysmem, base + 0x1000,
                                    &s->eray[i]->msg_ram);
        sysbus_connect_irq(SYS_BUS_DEVICE(s->eray[i]), 0,
            qdev_get_gpio_in_named(DEVICE(s->irbus), "irq",
                                   tc3x_eray_src[i][0]));
        sysbus_connect_irq(SYS_BUS_DEVICE(s->eray[i]), 1,
            qdev_get_gpio_in_named(DEVICE(s->irbus), "irq",
                                   tc3x_eray_src[i][1]));
    }
    for (unsigned i = 0; i < 11; i++) {
        memory_region_add_subregion(sysmem,
            sc->memmap[TC39XB_ASCLIN].base + 0x200 * (i + 1),
            &s->asclin_extra[i]->iomem);
        sysbus_connect_irq(SYS_BUS_DEVICE(s->asclin_extra[i]), 0,
            qdev_get_gpio_in_named(DEVICE(s->irbus), "irq",
                                   TC3X_SRC_ASCLIN0_RX + 3 * (i + 1)));
        sysbus_connect_irq(SYS_BUS_DEVICE(s->asclin_extra[i]), 1,
            qdev_get_gpio_in_named(DEVICE(s->irbus), "irq",
                                   TC3X_SRC_ASCLIN0_TX + 3 * (i + 1)));
        sysbus_connect_irq(SYS_BUS_DEVICE(s->asclin_extra[i]), 2,
            qdev_get_gpio_in_named(DEVICE(s->irbus), "irq",
                                   TC3X_SRC_ASCLIN0_ERR + 3 * (i + 1)));
    }
    memory_region_add_subregion(sysmem, sc->memmap[TC39XB_VIRT].base,
                                &s->virt->iomem);
    memory_region_add_subregion(sysmem, sc->memmap[TC39XB_SCU].base,
                                &s->scu->iomem);
    memory_region_add_subregion(sysmem, sc->memmap[TC39XB_STM].base,
                                &s->stm->iomem);
}

static void tc39x_soc_init(Object *obj)
{
    TC39XBSoCState *s = TC39XB_SOC(obj);
    TC39XBSoCClass *sc = TC39XB_SOC_GET_CLASS(s);

    object_initialize_child(obj, "tc37x-cpu0", &s->cpus[0], sc->cpu_type);
    for (unsigned i = 1; i < 6; i++) {
        char *name = g_strdup_printf("tc37x-cpu%u", i);
        object_initialize_child(obj, name, &s->cpus[i], sc->cpu_type);
        g_free(name);
    }
}

static Property tc39xb_soc_props[] = {
    DEFINE_PROP_LINK("canbus", TC39XBSoCState, canbus,
                     TYPE_CAN_BUS, CanBusState *),
};

static void tc39x_soc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = tc39x_soc_realize;
    device_class_set_props_n(dc, tc39xb_soc_props,
                             ARRAY_SIZE(tc39xb_soc_props));
}

static void tc397b_soc_class_init(ObjectClass *oc, const void *data)
{
    TC39XBSoCClass *sc = TC39XB_SOC_CLASS(oc);

    sc->name         = "tc39xb-soc";
    sc->cpu_type     = TRICORE_CPU_TYPE_NAME("tc3x");
    sc->memmap       = tc39xb_soc_memmap;
    sc->num_cpus     = 6;
}

static const TypeInfo tc39x_soc_types[] = {
    {
        .name          = "tc397b-soc",
        .parent        = TYPE_TC39XB_SOC,
        .class_init    = tc397b_soc_class_init,
    }, {
        .name          = TYPE_TC39XB_SOC,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(TC39XBSoCState),
        .instance_init = tc39x_soc_init,
        .class_size    = sizeof(TC39XBSoCClass),
        .class_init    = tc39x_soc_class_init,
        .abstract      = true,
    },
};

DEFINE_TYPES(tc39x_soc_types)
