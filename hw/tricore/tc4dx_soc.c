/*
 * Infineon TC4Dx SoC System emulation.
 *
 * Copyright (c) 2026 Parthiban Nallathambi <parthiban@linumiz.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/sysbus.h"
#include "hw/core/loader.h"
#include "qemu/units.h"
#include "hw/misc/unimp.h"

#include "hw/tricore/tc4dx_soc.h"
#include "hw/tricore/triboard.h"

const MemmapEntry tc4dx_soc_memmap[] = {
    [TC4DX_DSPR0]      = { 0x70000000,            240 * KiB },
    [TC4DX_PSPR0]      = { 0x70100000,             64 * KiB },
    [TC4DX_DSPR1]      = { 0x60000000,            240 * KiB },
    [TC4DX_PSPR1]      = { 0x60100000,             64 * KiB },
    [TC4DX_DSPR2]      = { 0x50000000,            240 * KiB },
    [TC4DX_PSPR2]      = { 0x50100000,             64 * KiB },
    [TC4DX_DSPR3]      = { 0x40000000,            240 * KiB },
    [TC4DX_PSPR3]      = { 0x40100000,             64 * KiB },
    [TC4DX_DSPR4]      = { 0x30000000,            240 * KiB },
    [TC4DX_PSPR4]      = { 0x30100000,             64 * KiB },
    [TC4DX_DSPR5]      = { 0x20000000,            240 * KiB },
    [TC4DX_PSPR5]      = { 0x20100000,             64 * KiB },
    [TC4DX_DSPRCS]     = { 0x10000000,            240 * KiB },
    [TC4DX_PSPRCS]     = { 0x10100000,             64 * KiB },

    [TC4DX_PFLASH0_C]  = { 0x80000000,              4 * MiB },
    [TC4DX_PFLASH1_C]  = { 0x80400000,              4 * MiB },
    [TC4DX_PFLASH2_C]  = { 0x80800000,              2 * MiB },
    [TC4DX_PFLASH3_C]  = { 0x80A00000,              4 * MiB },
    [TC4DX_PFLASH4_C]  = { 0x80E00000,              4 * MiB },
    [TC4DX_PFLASH5_C]  = { 0x81200000,              2 * MiB },
    [TC4DX_FLASHCS_C]  = { 0x84000000,              1 * MiB },

    [TC4DX_DLMU0]      = { 0x90000000,            512 * KiB },
    [TC4DX_DLMU1]      = { 0x90080000,            512 * KiB },
    [TC4DX_DLMU2]      = { 0x90100000,            512 * KiB },
    [TC4DX_DLMU3]      = { 0x90180000,            512 * KiB },
    [TC4DX_DLMU4]      = { 0x90200000,            512 * KiB },
    [TC4DX_DLMU5]      = { 0x90280000,            512 * KiB },

    [TC4DX_PSPRX]      = { 0xC0000000,                  0x0 },
    [TC4DX_DSPRX]      = { 0xD0000000,                  0x0 },

    [TC4DX_VIRT]       = { 0xBF000000,                  0x0 },
    [TC4DX_SFR]        = { 0xF0000000,                  0x0 },
    [TC4DX_STM]        = { 0xF8800000,                  0x0 },
    [TC4DX_ASCLIN]     = { 0xF46C0000,                  0x0 },
    [TC4DX_SCU]        = { 0xF0064000,                  0x0 },
    [TC4DX_IRBUS]      = { 0xF4432000,                  0x0 },
};

static void make_ram(MemoryRegion *mr, const char *name,
                     hwaddr base, hwaddr size)
{
    memory_region_init_ram(mr, NULL, name, size, &error_fatal);
    memory_region_add_subregion(get_system_memory(), base, mr);
}

static void make_alias(MemoryRegion *mr, const char *name,
                       MemoryRegion *orig, hwaddr base)
{
    memory_region_init_alias(mr, NULL, name, orig, 0,
                             memory_region_size(orig));
    memory_region_add_subregion(get_system_memory(), base, mr);
}

static void tc4dx_soc_init_memory_mapping(DeviceState *dev_soc)
{
    TC4DXSoCState *s = TC4DX_SOC(dev_soc);
    TC4DXSoCClass *sc = TC4DX_SOC_GET_CLASS(s);
    const MemmapEntry *map = sc->memmap;

    TC4DXSoCCPUMemState *c0 = &s->cpu0mem;
    TC4DXSoCCPUMemState *c1 = &s->cpu1mem;
    TC4DXSoCCPUMemState *c2 = &s->cpu2mem;
    TC4DXSoCCPUMemState *c3 = &s->cpu3mem;
    TC4DXSoCCPUMemState *c4 = &s->cpu4mem;
    TC4DXSoCCPUMemState *c5 = &s->cpu5mem;
    TC4DXSoCCPUMemState *ccs = &s->cpucsmem;

    make_ram(&c0->dspr, "CPU0.DSPR", map[TC4DX_DSPR0].base, map[TC4DX_DSPR0].size);
    make_ram(&c0->pspr, "CPU0.PSPR", map[TC4DX_PSPR0].base, map[TC4DX_PSPR0].size);
    make_ram(&c1->dspr, "CPU1.DSPR", map[TC4DX_DSPR1].base, map[TC4DX_DSPR1].size);
    make_ram(&c1->pspr, "CPU1.PSPR", map[TC4DX_PSPR1].base, map[TC4DX_PSPR1].size);
    make_ram(&c2->dspr, "CPU2.DSPR", map[TC4DX_DSPR2].base, map[TC4DX_DSPR2].size);
    make_ram(&c2->pspr, "CPU2.PSPR", map[TC4DX_PSPR2].base, map[TC4DX_PSPR2].size);
    make_ram(&c3->dspr, "CPU3.DSPR", map[TC4DX_DSPR3].base, map[TC4DX_DSPR3].size);
    make_ram(&c3->pspr, "CPU3.PSPR", map[TC4DX_PSPR3].base, map[TC4DX_PSPR3].size);
    make_ram(&c4->dspr, "CPU4.DSPR", map[TC4DX_DSPR4].base, map[TC4DX_DSPR4].size);
    make_ram(&c4->pspr, "CPU4.PSPR", map[TC4DX_PSPR4].base, map[TC4DX_PSPR4].size);
    make_ram(&c5->dspr, "CPU5.DSPR", map[TC4DX_DSPR5].base, map[TC4DX_DSPR5].size);
    make_ram(&c5->pspr, "CPU5.PSPR", map[TC4DX_PSPR5].base, map[TC4DX_PSPR5].size);
    make_ram(&ccs->dspr, "CPUCS.DSPR", map[TC4DX_DSPRCS].base, map[TC4DX_DSPRCS].size);
    make_ram(&ccs->pspr, "CPUCS.PSPR", map[TC4DX_PSPRCS].base, map[TC4DX_PSPRCS].size);

    make_alias(&s->psprX, "LOCAL.PSPR", &c0->pspr, map[TC4DX_PSPRX].base);
    make_alias(&s->dsprX, "LOCAL.DSPR", &c0->dspr, map[TC4DX_DSPRX].base);

    make_ram(&c0->pflash_c, "PF0", map[TC4DX_PFLASH0_C].base, map[TC4DX_PFLASH0_C].size);
    make_ram(&c1->pflash_c, "PF1", map[TC4DX_PFLASH1_C].base, map[TC4DX_PFLASH1_C].size);
    make_ram(&c2->pflash_c, "PF2", map[TC4DX_PFLASH2_C].base, map[TC4DX_PFLASH2_C].size);
    make_ram(&c3->pflash_c, "PF3", map[TC4DX_PFLASH3_C].base, map[TC4DX_PFLASH3_C].size);
    make_ram(&c4->pflash_c, "PF4", map[TC4DX_PFLASH4_C].base, map[TC4DX_PFLASH4_C].size);
    make_ram(&c5->pflash_c, "PF5", map[TC4DX_PFLASH5_C].base, map[TC4DX_PFLASH5_C].size);
    make_ram(&s->flashmem.flashcs, "PFCS", map[TC4DX_FLASHCS_C].base, map[TC4DX_FLASHCS_C].size);

    make_ram(&c0->dlmu, "DLMU0", map[TC4DX_DLMU0].base, map[TC4DX_DLMU0].size);
    make_ram(&c1->dlmu, "DLMU1", map[TC4DX_DLMU1].base, map[TC4DX_DLMU1].size);
    make_ram(&c2->dlmu, "DLMU2", map[TC4DX_DLMU2].base, map[TC4DX_DLMU2].size);
    make_ram(&c3->dlmu, "DLMU3", map[TC4DX_DLMU3].base, map[TC4DX_DLMU3].size);
    make_ram(&c4->dlmu, "DLMU4", map[TC4DX_DLMU4].base, map[TC4DX_DLMU4].size);
    make_ram(&c5->dlmu, "DLMU5", map[TC4DX_DLMU5].base, map[TC4DX_DLMU5].size);
}

static void tc4dx_soc_realize(DeviceState *dev_soc, Error **errp)
{
    TC4DXSoCState *s = TC4DX_SOC(dev_soc);
    TC4DXSoCClass *sc = TC4DX_SOC_GET_CLASS(s);
    Error *err = NULL;

    qdev_realize(DEVICE(&s->cpu), NULL, &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    tc4dx_soc_init_memory_mapping(dev_soc);

    MemoryRegion *sysmem = get_system_memory();

    s->cpu_irq = tricore_cpu_ir_init(&s->cpu);

    s->irbus = TRICORE_IRBUS(object_new(TYPE_TRICORE_IRBUS));
    s->asclin = TRICORE_ASCLIN(object_new(TYPE_TRICORE_ASCLIN));
    s->virt = TRICORE_VIRT(object_new(TYPE_TRICORE_VIRT));
    s->scu = TRICORE_SCU(object_new(TYPE_TRICORE_SCU));
    s->stm = TRICORE_STM(object_new(TYPE_TRICORE_STM));
    s->sfr = TRICORE_SFR(object_new(TYPE_TRICORE_SFR));

    object_property_add_const_link(OBJECT(s->irbus), "cpu", OBJECT(&s->cpu));
    object_property_add_const_link(OBJECT(s->scu), "cpu", OBJECT(&s->cpu));
    object_property_add_const_link(OBJECT(s->stm), "scu", OBJECT(s->scu));
    qdev_prop_set_chr(DEVICE(s->asclin), "chardev", serial_hd(0));
    qdev_prop_set_bit(DEVICE(s->stm), "tc4x-mode", true);
    qdev_prop_set_bit(DEVICE(s->irbus), "tc4x-mode", true);

    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->sfr), &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->scu), &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->stm), &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->irbus), &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->virt), &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->asclin), &error_fatal);

    sysbus_connect_irq(SYS_BUS_DEVICE(s->irbus), 0, s->cpu_irq[0]);
    for (int i = 0; i < IR_SRC_COUNT; i++) {
        s->irq[i] = qdev_get_gpio_in(DEVICE(s->irbus), i);
    }

    sysbus_connect_irq(SYS_BUS_DEVICE(s->asclin), 0, s->irq[IR_SRC_ASCLIN0RX]);
    sysbus_connect_irq(SYS_BUS_DEVICE(s->asclin), 1, s->irq[IR_SRC_ASCLIN0TX]);
    sysbus_connect_irq(SYS_BUS_DEVICE(s->asclin), 2, s->irq[IR_SRC_ASCLIN0EX]);
    sysbus_connect_irq(SYS_BUS_DEVICE(s->stm), 0, s->irq[IR_SRC_STM0_SR0]);
    sysbus_connect_irq(SYS_BUS_DEVICE(s->scu), 0, s->irq[IR_SRC_RESET]);

    memory_region_add_subregion_overlap(sysmem, sc->memmap[TC4DX_SFR].base, &s->sfr->iomem, -1);
    memory_region_add_subregion_overlap(sysmem, sc->memmap[TC4DX_IRBUS].base, &s->irbus->srvcontrolregs, 0);
    memory_region_add_subregion_overlap(sysmem, 0xF4430000, &s->irbus->intregs, 0);  /* IR INT regs (LWSR) */
    memory_region_add_subregion_overlap(sysmem, sc->memmap[TC4DX_ASCLIN].base, &s->asclin->iomem, 1);
    memory_region_add_subregion_overlap(sysmem, sc->memmap[TC4DX_VIRT].base, &s->virt->iomem, 1);
    memory_region_add_subregion_overlap(sysmem, sc->memmap[TC4DX_SCU].base, &s->scu->iomem, 1);
    memory_region_add_subregion_overlap(sysmem, sc->memmap[TC4DX_STM].base, &s->stm->iomem, 1);
}

static void tc4dx_soc_init(Object *obj)
{
    TC4DXSoCState *s = TC4DX_SOC(obj);
    TC4DXSoCClass *sc = TC4DX_SOC_GET_CLASS(s);

    object_initialize_child(obj, "tc37x", &s->cpu, sc->cpu_type);
}

static void tc4dx_soc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = tc4dx_soc_realize;
}

static void tc4d7_soc_class_init(ObjectClass *oc, const void *data)
{
    TC4DXSoCClass *sc = TC4DX_SOC_CLASS(oc);

    sc->name         = "tc4dx-soc";
    sc->cpu_type     = TRICORE_CPU_TYPE_NAME("tc37x");
    sc->memmap       = tc4dx_soc_memmap;
    sc->num_cpus     = 1;
}

static const TypeInfo tc4dx_soc_types[] = {
    {
        .name          = "tc4d7-soc",
        .parent        = TYPE_TC4DX_SOC,
        .class_init    = tc4d7_soc_class_init,
    }, {
        .name          = TYPE_TC4DX_SOC,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(TC4DXSoCState),
        .instance_init = tc4dx_soc_init,
        .class_size    = sizeof(TC4DXSoCClass),
        .class_init    = tc4dx_soc_class_init,
        .abstract      = true,
    },
};

DEFINE_TYPES(tc4dx_soc_types)
