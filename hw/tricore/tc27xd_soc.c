/*
 * Infineon tc27x SoC System emulation.
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

#include "hw/tricore/tc27xd_soc.h"
#include "hw/tricore/triboard.h"

static uint64_t tc27x_cpu_ctrl_read(void *opaque, hwaddr offset, unsigned size)
{
    struct TC27XCPUControl *c = opaque;
    if (offset == 0x1FE08) return c->pc;
    if (offset == 0x1FD00) return c->dbgsr;
    return 0;
}

static void tc27x_cpu_ctrl_write(void *opaque, hwaddr offset,
                                 uint64_t value, unsigned size)
{
    struct TC27XCPUControl *c = opaque;
    if (offset == 0x1FE08) {
        c->pc = value << 1;
    } else if (offset == 0x1FD00) {
        c->dbgsr = value;
        if (c->id > 0 && (value & 3) == 2) {
            CPUState *cs = CPU(&c->soc->cpus[c->id]);
            c->soc->cpus[c->id].env.PC = c->pc;
            cs->halted = 0;
            cpu_resume(cs);
        }
    }
}

static const MemoryRegionOps tc27x_cpu_ctrl_ops = {
    .read = tc27x_cpu_ctrl_read,
    .write = tc27x_cpu_ctrl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void tc27x_pmcsr_write(void *opaque, hwaddr offset,
                              uint64_t value, unsigned size)
{
    TC27XDSoCState *s = opaque;
    unsigned id = offset >> 2;
    if (id < 3 && id < TC27XD_SOC_GET_CLASS(s)->num_cpus && !(value & 3) && id > 0) {
        CPUState *cs = CPU(&s->cpus[id]);
        cs->halted = 0;
        cpu_resume(cs);
    }
}

static uint64_t tc27x_pmcsr_read(void *opaque, hwaddr offset, unsigned size)
{
    unsigned id = offset >> 2;
    TC27XDSoCState *s = opaque;
    return (id < 3 && id < TC27XD_SOC_GET_CLASS(s)->num_cpus && id > 0) ? 1 : 0;
}

static const MemoryRegionOps tc27x_pmcsr_ops = {
    .read = tc27x_pmcsr_read,
    .write = tc27x_pmcsr_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void tc27x_dma_isp_sink(void *opaque, int n, int level)
{
    /* TC277 exposes a fourth service-provider slot for DMA.  No DMA device
     * is present yet, but keeping the slot connected prevents it from being
     * confused with a CPU ISP and preserves the documented topology. */
}

const MemmapEntry tc27xd_soc_memmap[] = {
    [TC27XD_DSPR2]     = { 0x50000000,            120 * KiB },
    [TC27XD_DCACHE2]   = { 0x5001E000,              8 * KiB },
    [TC27XD_DTAG2]     = { 0x500C0000,                0xC00 },
    [TC27XD_PSPR2]     = { 0x50100000,             32 * KiB },
    [TC27XD_PCACHE2]   = { 0x50108000,             16 * KiB },
    [TC27XD_PTAG2]     = { 0x501C0000,               0x1800 },
    [TC27XD_DSPR1]     = { 0x60000000,            120 * KiB },
    [TC27XD_DCACHE1]   = { 0x6001E000,              8 * KiB },
    [TC27XD_DTAG1]     = { 0x600C0000,                0xC00 },
    [TC27XD_PSPR1]     = { 0x60100000,             32 * KiB },
    [TC27XD_PCACHE1]   = { 0x60108000,             16 * KiB },
    [TC27XD_PTAG1]     = { 0x601C0000,               0x1800 },
    [TC27XD_DSPR0]     = { 0x70000000,            112 * KiB },
    [TC27XD_PSPR0]     = { 0x70100000,             24 * KiB },
    [TC27XD_PCACHE0]   = { 0x70106000,              8 * KiB },
    [TC27XD_PTAG0]     = { 0x701C0000,                0xC00 },
    [TC27XD_PFLASH0_C] = { 0x80000000,              2 * MiB },
    [TC27XD_PFLASH1_C] = { 0x80200000,              2 * MiB },
    [TC27XD_OLDA_C]    = { 0x8FE70000,             32 * KiB },
    [TC27XD_BROM_C]    = { 0x8FFF8000,             32 * KiB },
    [TC27XD_LMURAM_C]  = { 0x90000000,             32 * KiB },
    [TC27XD_EMEM_C]    = { 0x9F000000,              1 * MiB },
    [TC27XD_PFLASH0_U] = { 0xA0000000,                  0x0 },
    [TC27XD_PFLASH1_U] = { 0xA0200000,                  0x0 },
    [TC27XD_DFLASH0]   = { 0xAF000000,   1 * MiB + 16 * KiB },
    [TC27XD_DFLASH1]   = { 0xAF110000,             64 * KiB },
    [TC27XD_OLDA_U]    = { 0xAFE70000,                  0x0 },
    [TC27XD_BROM_U]    = { 0xAFFF8000,                  0x0 },
    [TC27XD_LMURAM_U]  = { 0xB0000000,                  0x0 },
    [TC27XD_EMEM_U]    = { 0xBF000000,                  0x0 },
    [TC27XD_PSPRX]     = { 0xC0000000,                  0x0 },
    [TC27XD_DSPRX]     = { 0xD0000000,                  0x0 },

    [TC27XD_VIRT]      = { 0xBF000000,                  0x0 },

    [TC27XD_SFR]       = { 0xF0000000,                  0x0 },
    [TC27XD_STM]       = { 0xF0000000,                  0x0 },
    [TC27XD_ASCLIN]    = { 0xF0000600,                  0x0 },
    [TC27XD_SCU]       = { 0xF0036000,                  0x0 },
    [TC27XD_IRBUS]     = { 0xF0038000,                  0x0 },
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


static void tc27xd_soc_init_memory_mapping(DeviceState *dev_soc)
{
    TC27XDSoCState *s = TC27XD_SOC(dev_soc);
    TC27XDSoCClass *sc = TC27XD_SOC_GET_CLASS(s);

    /* shortcuts to clean up code */
    const MemmapEntry *map = sc->memmap;
    TC27XDSoCCPUMemState *c0 = &s->cpu0mem;
    TC27XDSoCCPUMemState *c1 = &s->cpu1mem;
    TC27XDSoCCPUMemState *c2 = &s->cpu2mem;
    TC27XDSoCFlashMemState *f = &s->flashmem;

    make_ram(&c0->dspr, "CPU0.DSPR", map[TC27XD_DSPR0].base, sc->dspr_size[0]);
    make_ram(&c0->pspr, "CPU0.PSPR", map[TC27XD_PSPR0].base, sc->pspr_size[0]);
    make_ram(&c1->dspr, "CPU1.DSPR", map[TC27XD_DSPR1].base, sc->dspr_size[1]);
    make_ram(&c1->pspr, "CPU1.PSPR", map[TC27XD_PSPR1].base, sc->pspr_size[1]);
    if (sc->num_cpus > 2) {
        make_ram(&c2->dspr, "CPU2.DSPR", map[TC27XD_DSPR2].base, sc->dspr_size[2]);
        make_ram(&c2->pspr, "CPU2.PSPR", map[TC27XD_PSPR2].base, sc->pspr_size[2]);
    }

    /* TODO: Control Cache mapping with Memory Test Unit (MTU) */
    if (sc->num_cpus > 2) {
        make_ram(&c2->dcache, "CPU2.DCACHE", map[TC27XD_DCACHE2].base, map[TC27XD_DCACHE2].size);
        make_ram(&c2->dtag,   "CPU2.DTAG", map[TC27XD_DTAG2].base, map[TC27XD_DTAG2].size);
        make_ram(&c2->pcache, "CPU2.PCACHE", map[TC27XD_PCACHE2].base, map[TC27XD_PCACHE2].size);
        make_ram(&c2->ptag,   "CPU2.PTAG", map[TC27XD_PTAG2].base, map[TC27XD_PTAG2].size);
    }
    make_ram(&c1->dcache, "CPU1.DCACHE", map[TC27XD_DCACHE1].base, map[TC27XD_DCACHE1].size);
    make_ram(&c1->dtag,   "CPU1.DTAG", map[TC27XD_DTAG1].base, map[TC27XD_DTAG1].size);
    make_ram(&c1->pcache, "CPU1.PCACHE", map[TC27XD_PCACHE1].base, map[TC27XD_PCACHE1].size);
    make_ram(&c1->ptag,   "CPU1.PTAG", map[TC27XD_PTAG1].base, map[TC27XD_PTAG1].size);
    make_ram(&c0->pcache, "CPU0.PCACHE", map[TC27XD_PCACHE0].base, map[TC27XD_PCACHE0].size);
    make_ram(&c0->ptag,   "CPU0.PTAG", map[TC27XD_PTAG0].base, map[TC27XD_PTAG0].size);

    /*
     * TriCore QEMU executes CPU0 only, thus it is sufficient to map
     * LOCAL.PSPR/LOCAL.DSPR exclusively onto PSPR0/DSPR0.
     */
    make_alias(&s->psprX, "LOCAL.PSPR", &c0->pspr, map[TC27XD_PSPRX].base);
    make_alias(&s->dsprX, "LOCAL.DSPR", &c0->dspr, map[TC27XD_DSPRX].base);

    make_ram(&f->pflash0_c, "PF0", map[TC27XD_PFLASH0_C].base, sc->pflash0_size);
    if (sc->pflash1_size) {
        make_ram(&f->pflash1_c, "PF1", map[TC27XD_PFLASH1_C].base, sc->pflash1_size);
    }
    make_ram(&f->dflash0,   "DF0", map[TC27XD_DFLASH0].base, map[TC27XD_DFLASH0].size);
    make_ram(&f->dflash1,   "DF1", map[TC27XD_DFLASH1].base, map[TC27XD_DFLASH1].size);
    make_ram(&f->olda_c,    "OLDA", map[TC27XD_OLDA_C].base, map[TC27XD_OLDA_C].size);
    make_rom(&f->brom_c,    "BROM", map[TC27XD_BROM_C].base, map[TC27XD_BROM_C].size);
    make_ram(&f->lmuram_c,  "LMURAM", map[TC27XD_LMURAM_C].base, map[TC27XD_LMURAM_C].size);
    make_ram(&f->emem_c,    "EMEM", map[TC27XD_EMEM_C].base, map[TC27XD_EMEM_C].size);

    make_alias(&f->pflash0_u, "PF0.U",    &f->pflash0_c, map[TC27XD_PFLASH0_U].base);
    if (sc->pflash1_size) {
        make_alias(&f->pflash1_u, "PF1.U",    &f->pflash1_c, map[TC27XD_PFLASH1_U].base);
    }
    make_alias(&f->olda_u,    "OLDA.U",   &f->olda_c, map[TC27XD_OLDA_U].base);
    make_alias(&f->brom_u,    "BROM.U",   &f->brom_c, map[TC27XD_BROM_U].base);
    make_alias(&f->lmuram_u,  "LMURAM.U", &f->lmuram_c, map[TC27XD_LMURAM_U].base);
}

/* TC277 SRC offsets from MODULE_SRC (F0038000), converted to SRN indices. */
#define TC27X_SRC_STM0_SR0      103 /* F0038490 */
#define TC27X_SRC_ASCLIN0_TX    9   /* F0038080 */
#define TC27X_SRC_ASCLIN0_RX    10  /* F0038084 */
#define TC27X_SRC_ASCLIN0_ERR   11  /* F0038088 */

/* Reserved TC277 SRC nodes for later peripheral models. */
#define TC27X_SRC_ETH           175
#define TC27X_SRC_MCAN_BASE     176
#define TC27X_SRC_MCAN_COUNT    16

static void tc27xd_soc_realize(DeviceState *dev_soc, Error **errp)
{
    TC27XDSoCState *s = TC27XD_SOC(dev_soc);
    TC27XDSoCClass *sc = TC27XD_SOC_GET_CLASS(s);
    Error *err = NULL;

    s->irbus = TRICORE_IR(object_new(TYPE_TRICORE_IR));
    s->asclin = TRICORE_ASCLIN(object_new(TYPE_TRICORE_ASCLIN));
    for (unsigned i = 0; i < 3; i++) {
        s->asclin_extra[i] = TRICORE_ASCLIN(object_new(TYPE_TRICORE_ASCLIN));
    }
    s->virt = TRICORE_VIRT(object_new(TYPE_TRICORE_VIRT));
    s->scu = TRICORE_SCU(object_new(TYPE_TRICORE_SCU));
    s->stm = TRICORE_STM(object_new(TYPE_TRICORE_STM));
    s->sfr = TRICORE_SFR(object_new(TYPE_TRICORE_SFR));

    object_property_add_child(OBJECT(dev_soc), "irbus", OBJECT(s->irbus));
    object_property_add_child(OBJECT(dev_soc), "asclin", OBJECT(s->asclin));
    for (unsigned i = 0; i < 3; i++) {
        char *name = g_strdup_printf("asclin%u", i + 1);
        object_property_add_child(OBJECT(dev_soc), name,
                                   OBJECT(s->asclin_extra[i]));
        g_free(name);
    }
    object_property_add_child(OBJECT(dev_soc), "virt", OBJECT(s->virt));
    object_property_add_child(OBJECT(dev_soc), "scu", OBJECT(s->scu));
    object_property_add_child(OBJECT(dev_soc), "stm", OBJECT(s->stm));
    object_property_add_child(OBJECT(dev_soc), "sfr", OBJECT(s->sfr));

    qdev_prop_set_bit(DEVICE(s->irbus), "tc4x-mode", false);
    qdev_prop_set_bit(DEVICE(s->irbus), "tc27x-mode", true);
    qdev_prop_set_uint8(DEVICE(s->irbus), "num-isps", sc->ir_num_isps);
    qdev_prop_set_uint8(DEVICE(s->irbus), "num-cpu-isps", sc->num_cpus);
    qdev_prop_set_uint16(DEVICE(s->irbus), "num-irqs", 512);

    for (unsigned i = 0; i < sc->num_cpus; i++) {
        object_property_set_bool(OBJECT(&s->cpus[i]), "start-powered-off",
                                 i != 0, &error_abort);
        CPU(&s->cpus[i])->cpu_index = i;
        object_property_set_link(OBJECT(&s->cpus[i]), "ir",
                                 OBJECT(s->irbus), &error_abort);
        qdev_realize(DEVICE(&s->cpus[i]), NULL, &err);
        if (err) { error_propagate(errp, err); return; }
    }
    tc27xd_soc_init_memory_mapping(dev_soc);

    MemoryRegion *sysmem = get_system_memory();
    memory_region_init_io(&s->pmcsr_region, OBJECT(s), &tc27x_pmcsr_ops,
                          s, "tc27x-pmcsr", 0x0c);
    memory_region_add_subregion_overlap(sysmem, 0xF00360D4, &s->pmcsr_region,
                                        1);

    /* TC2x/TC27x iLLD default: fPLL=300 MHz, STMDIV=3 => fSTM=100 MHz. */
    Clock *fstm = clock_new(OBJECT(dev_soc), "fstm");
    clock_set_hz(fstm, 100000000);
    qdev_connect_clock_in(DEVICE(s->stm), "fstm", fstm);

    object_property_add_const_link(OBJECT(s->scu), "cpu", OBJECT(&s->cpus[0]));
    qdev_prop_set_chr(DEVICE(s->asclin), "chardev", serial_hd(0));

    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->sfr), &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->scu), &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->stm), &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->irbus), &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->virt), &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->asclin), &error_fatal);
    for (unsigned i = 0; i < 3; i++) {
        DeviceState *extra = DEVICE(s->asclin_extra[i]);
        qdev_prop_set_chr(extra, "chardev", serial_hd(i + 1));
        sysbus_realize_and_unref(SYS_BUS_DEVICE(extra), &error_fatal);
    }

    sysbus_mmio_map(SYS_BUS_DEVICE(s->irbus), 0, 0xF0037000);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->irbus), 1, 0xF0038000);

    for (unsigned i = 0; i < sc->num_cpus; i++) {
        qdev_connect_gpio_out_named(DEVICE(s->irbus), "isp", i,
            qdev_get_gpio_in_named(DEVICE(&s->cpus[i]), "tricore.irq", 0));
    }
    if (sc->ir_num_isps > sc->num_cpus) {
        qdev_connect_gpio_out_named(DEVICE(s->irbus), "isp", sc->num_cpus,
            qemu_allocate_irq(tc27x_dma_isp_sink, s, 0));
    }

    /* TC27D CPU register windows: F881/F883/F8850000. */
    for (unsigned i = 0; i < sc->num_cpus; i++) {
        struct TC27XCPUControl *c = &s->cpu_ctrl[i];
        c->soc = s; c->id = i; c->dbgsr = i ? 1 : 0;
        char *name = g_strdup_printf("tc27x-cpu%u-control", i);
        memory_region_init_io(&c->region, OBJECT(s), &tc27x_cpu_ctrl_ops,
                              c, name, 0x20000);
        g_free(name);
        memory_region_add_subregion_overlap(sysmem,
                                            0xF8810000 + i * 0x20000,
                                            &c->region, 100);
    }

    sysbus_connect_irq(SYS_BUS_DEVICE(s->asclin), 0,
        qdev_get_gpio_in_named(DEVICE(s->irbus), "irq",
                               TC27X_SRC_ASCLIN0_RX));
    sysbus_connect_irq(SYS_BUS_DEVICE(s->asclin), 1,
        qdev_get_gpio_in_named(DEVICE(s->irbus), "irq",
                               TC27X_SRC_ASCLIN0_TX));
    sysbus_connect_irq(SYS_BUS_DEVICE(s->asclin), 2,
        qdev_get_gpio_in_named(DEVICE(s->irbus), "irq",
                               TC27X_SRC_ASCLIN0_ERR));

    sysbus_connect_irq(SYS_BUS_DEVICE(s->stm), 0,
        qdev_get_gpio_in_named(DEVICE(s->irbus), "irq",
                               TC27X_SRC_STM0_SR0));

    memory_region_add_subregion_overlap(sysmem, sc->memmap[TC27XD_SFR].base,
                                        &s->sfr->iomem, -1);
    memory_region_add_subregion(sysmem, sc->memmap[TC27XD_ASCLIN].base,
                                &s->asclin->iomem);
    for (unsigned i = 0; i < 3; i++) {
        memory_region_add_subregion(sysmem,
            sc->memmap[TC27XD_ASCLIN].base + 0x200 * (i + 1),
            &s->asclin_extra[i]->iomem);
        sysbus_connect_irq(SYS_BUS_DEVICE(s->asclin_extra[i]), 0,
            qdev_get_gpio_in_named(DEVICE(s->irbus), "irq",
                                   TC27X_SRC_ASCLIN0_RX + 3 * (i + 1)));
        sysbus_connect_irq(SYS_BUS_DEVICE(s->asclin_extra[i]), 1,
            qdev_get_gpio_in_named(DEVICE(s->irbus), "irq",
                                   TC27X_SRC_ASCLIN0_TX + 3 * (i + 1)));
        sysbus_connect_irq(SYS_BUS_DEVICE(s->asclin_extra[i]), 2,
            qdev_get_gpio_in_named(DEVICE(s->irbus), "irq",
                                   TC27X_SRC_ASCLIN0_ERR + 3 * (i + 1)));
    }
    memory_region_add_subregion(sysmem, sc->memmap[TC27XD_VIRT].base,
                                &s->virt->iomem);
    memory_region_add_subregion(sysmem, sc->memmap[TC27XD_SCU].base,
                                &s->scu->iomem);
    memory_region_add_subregion(sysmem, sc->memmap[TC27XD_STM].base,
                                &s->stm->iomem);
}

static void tc27xd_soc_reset(DeviceState *dev_soc)
{
    TC27XDSoCState *s = TC27XD_SOC(dev_soc);
    TC27XDSoCClass *sc = TC27XD_SOC_GET_CLASS(s);
    for (unsigned i = 0; i < sc->num_cpus; i++) {
        cpu_state_reset(&s->cpus[i].env);
    }
}

static void tc27xd_soc_init(Object *obj)
{
    TC27XDSoCState *s = TC27XD_SOC(obj);
    TC27XDSoCClass *sc = TC27XD_SOC_GET_CLASS(s);

    for (unsigned i = 0; i < sc->num_cpus; i++) {
        char *name = g_strdup_printf("tc27x-cpu%u", i);
        object_initialize_child(obj, name, &s->cpus[i], sc->cpu_type);
        g_free(name);
    }
}

static void tc27xd_soc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = tc27xd_soc_realize;
    dc->legacy_reset = tc27xd_soc_reset;
}

static void tc277d_soc_class_init(ObjectClass *oc, const void *data)
{
    TC27XDSoCClass *sc = TC27XD_SOC_CLASS(oc);

    sc->name         = "tc277d-soc";
    sc->cpu_type     = TRICORE_CPU_TYPE_NAME("tc2x");
    sc->memmap       = tc27xd_soc_memmap;
    sc->num_cpus     = 3;
    sc->ir_num_isps  = 4;
    sc->dspr_size[0] = 112 * KiB;
    sc->dspr_size[1] = 120 * KiB;
    sc->dspr_size[2] = 120 * KiB;
    sc->pspr_size[0] = 24 * KiB;
    sc->pspr_size[1] = 32 * KiB;
    sc->pspr_size[2] = 32 * KiB;
    sc->pflash0_size = 2 * MiB;
    sc->pflash1_size = 2 * MiB;
}

static void tc26b_soc_class_init(ObjectClass *oc, const void *data)
{
    TC27XDSoCClass *sc = TC27XD_SOC_CLASS(oc);
    tc277d_soc_class_init(oc, data);
    sc->name = "tc26b-soc";
    sc->num_cpus = 2;
    sc->ir_num_isps = 2;
    sc->dspr_size[0] = 72 * KiB;
    sc->dspr_size[1] = 120 * KiB;
    sc->pspr_size[0] = 16 * KiB;
    sc->pspr_size[1] = 32 * KiB;
    sc->dspr_size[2] = sc->pspr_size[2] = 0;
    sc->pflash0_size = 1536 * KiB;
    sc->pflash1_size = 0;
}

static void tc29b_soc_class_init(ObjectClass *oc, const void *data)
{
    TC27XDSoCClass *sc = TC27XD_SOC_CLASS(oc);
    tc277d_soc_class_init(oc, data);
    sc->name = "tc29b-soc";
    sc->dspr_size[0] = 120 * KiB;
    sc->dspr_size[1] = 240 * KiB;
    sc->dspr_size[2] = 240 * KiB;
    sc->pspr_size[0] = sc->pspr_size[1] = sc->pspr_size[2] = 32 * KiB;
}

static const TypeInfo tc27xd_soc_types[] = {
    {
        .name          = "tc277d-soc",
        .parent        = TYPE_TC27XD_SOC,
        .class_init    = tc277d_soc_class_init,
    }, {
        .name          = "tc26b-soc",
        .parent        = TYPE_TC27XD_SOC,
        .class_init    = tc26b_soc_class_init,
    }, {
        .name          = "tc29b-soc",
        .parent        = TYPE_TC27XD_SOC,
        .class_init    = tc29b_soc_class_init,
    }, {
        .name          = TYPE_TC27XD_SOC,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(TC27XDSoCState),
        .instance_init = tc27xd_soc_init,
        .class_size    = sizeof(TC27XDSoCClass),
        .class_init    = tc27xd_soc_class_init,
        .abstract      = true,
    },
};

DEFINE_TYPES(tc27xd_soc_types)
