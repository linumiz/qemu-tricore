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

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/core/sysbus.h"
#include "qemu/units.h"
#include "hw/core/clock.h"
#include "hw/core/qdev-clock.h"

#include "hw/tricore/tc33x_soc.h"
#include "hw/tricore/triboard.h"


const MemmapEntry tc33xb_soc_memmap[] = {
    [TC33X_DSPR0]     = { 0x70000000,            192 * KiB },
    [TC33X_DCACHE0]   = { 0x7001E000,             16 * KiB },
    [TC33X_DTAG0]     = { 0x700C0000,              6 * KiB },
    [TC33X_PSPR0]     = { 0x70100000,              8 * KiB },
    [TC33X_PCACHE0]   = { 0x70108000,             32 * KiB },
    [TC33X_PTAG0]     = { 0x701C0000,             12 * KiB },

    [TC33X_PFLASH0_C] = { 0x80000000,              2 * MiB },

    [TC33X_OLDA_C]    = { 0x8FE00000,            512 * KiB },
    [TC33X_BROM_C]    = { 0x8FFF0000,             64 * KiB },

    [TC33X_DLMU0_C]   = { 0x90000000,              8 * KiB },
    [TC33X_EMEM]      = { 0x99000000,              1 * MiB },

    [TC33X_PFLASH0_U] = { 0xA0000000,                  0x0 },

    [TC33X_DFLASH0]   = { 0xAF000000,            128 * KiB },
    [TC33X_DFLASH1]   = { 0xAFC00000,            128 * KiB },

    [TC33X_OLDA_U]    = { 0xAFE00000,                  0x0 },
    [TC33X_BROM_U]    = { 0xAFFF0000,                  0x0 },

    [TC33X_DLMU0_U]   = { 0xB0000000,                  0x0 },

    [TC33X_PSPRX]     = { 0xC0000000,                  0x0 },
    [TC33X_DSPRX]     = { 0xD0000000,                  0x0 },

    [TC33X_SFR]       = { 0xF0000000,                  0x0 },
    [TC33X_STM]       = { 0xF0001000,                  0x0 },
    [TC33X_ASCLIN]    = { 0xF0000600,                  0x0 },
    [TC33X_SCU]       = { 0xF0036000,                  0x0 },
    [TC33X_IRBUS]     = { 0xF0037000,                  0x0 },
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


static void tc33x_soc_init_memory_mapping(DeviceState *dev_soc)
{
    TC33XSoCState *s = TC33X_SOC(dev_soc);
    TC33XSoCClass *sc = TC33X_SOC_GET_CLASS(s);

    /* shortcuts to clean up code */
    const MemmapEntry *map = sc->memmap;
    TC33XSoCCPUMemState *c0 = &s->cpu0mem;
    TC33XSoCFlashMemState *f = &s->flashmem;

    make_ram(&c0->dspr, "CPU0.DSPR", map[TC33X_DSPR0].base, map[TC33X_DSPR0].size);
    make_ram(&c0->pspr, "CPU0.PSPR", map[TC33X_PSPR0].base, map[TC33X_PSPR0].size);

    /*
     * TriCore QEMU executes CPU0 only, thus it is sufficient to map
     * LOCAL.PSPR/LOCAL.DSPR exclusively onto PSPR0/DSPR0.
     */
    make_alias(&s->psprX, "LOCAL.PSPR", &c0->pspr, map[TC33X_PSPRX].base);
    make_alias(&s->dsprX, "LOCAL.DSPR", &c0->dspr, map[TC33X_DSPRX].base);

    make_ram(&c0->pflash_c, "PF0", map[TC33X_PFLASH0_C].base, map[TC33X_PFLASH0_C].size);

    make_ram(&c0->dlmu_c, "DLMU0", map[TC33X_DLMU0_C].base, map[TC33X_DLMU0_C].size);

    make_ram(&f->dflash0,   "DF0", map[TC33X_DFLASH0].base, map[TC33X_DFLASH0].size);
    make_ram(&f->dflash1,   "DF1", map[TC33X_DFLASH1].base, map[TC33X_DFLASH1].size);
    make_ram(&f->olda_c,   "OLDA", map[TC33X_OLDA_C].base, map[TC33X_OLDA_C].size);
    make_rom(&f->brom_c,   "BROM", map[TC33X_BROM_C].base, map[TC33X_BROM_C].size);
    make_ram(&f->emem,     "EMEM", map[TC33X_EMEM].base, map[TC33X_EMEM].size);

    make_alias(&c0->pflash_u, "PF0.U", &c0->pflash_c, map[TC33X_PFLASH0_U].base);
    make_alias(&c0->dlmu_u, "DLMU0.U", &c0->dlmu_c, map[TC33X_DLMU0_U].base);

    make_alias(&f->olda_u,  "OLDA.U", &f->olda_c, map[TC33X_OLDA_U].base);
    make_alias(&f->brom_u,  "BROM.U", &f->brom_c, map[TC33X_BROM_U].base);
}

#define TC3X_SRC_STM0_SR0       0xC0
#define TC3X_SRC_ASCLIN0_TX     0x14
#define TC3X_SRC_ASCLIN0_RX     0x15
#define TC3X_SRC_ASCLIN0_ERR    0x16

static void tc33x_soc_realize(DeviceState *dev_soc, Error **errp)
{
    TC33XSoCState *s = TC33X_SOC(dev_soc);
    TC33XSoCClass *sc = TC33X_SOC_GET_CLASS(s);
    Error *err = NULL;

    s->irbus = TRICORE_IR(object_new(TYPE_TRICORE_IR));
    s->asclin = TRICORE_ASCLIN(object_new(TYPE_TRICORE_ASCLIN));
    s->scu = TRICORE_SCU(object_new(TYPE_TRICORE_SCU));
    s->stm = TRICORE_STM(object_new(TYPE_TRICORE_STM));
    s->sfr = TRICORE_SFR(object_new(TYPE_TRICORE_SFR));

    /* Parent all devices so sysbus_realize_and_unref does not free them */
    object_property_add_child(OBJECT(dev_soc), "irbus", OBJECT(s->irbus));
    object_property_add_child(OBJECT(dev_soc), "asclin", OBJECT(s->asclin));
    object_property_add_child(OBJECT(dev_soc), "scu", OBJECT(s->scu));
    object_property_add_child(OBJECT(dev_soc), "stm", OBJECT(s->stm));
    object_property_add_child(OBJECT(dev_soc), "sfr", OBJECT(s->sfr));

    /* IR properties */
    qdev_prop_set_bit(DEVICE(s->irbus), "tc4x-mode", false);
    qdev_prop_set_uint8(DEVICE(s->irbus), "num-isps", 1);
    qdev_prop_set_uint16(DEVICE(s->irbus), "num-irqs", 256);

    /* CPU needs IR link before realize */
    object_property_set_link(OBJECT(&s->cpu), "ir",
                             OBJECT(s->irbus), &error_abort);

    qdev_realize(DEVICE(&s->cpu), NULL, &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    tc33x_soc_init_memory_mapping(dev_soc);

    MemoryRegion *sysmem = get_system_memory();

    /* STM clock: fPLL=300MHz / STMDIV=3 = 100MHz */
    Clock *fstm = clock_new(OBJECT(dev_soc), "fstm");
    clock_set_hz(fstm, 50000000);
    qdev_connect_clock_in(DEVICE(s->stm), "fstm", fstm);

    object_property_add_const_link(OBJECT(s->scu), "cpu", OBJECT(&s->cpu));
    qdev_prop_set_chr(DEVICE(s->asclin), "chardev", serial_hd(0));

    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->sfr), &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->scu), &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->stm), &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->irbus), &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(s->asclin), &error_fatal);

    /* IR MMIO: idx 0 = int_region (F0037000), idx 1 = src_region (F0038000) */
    sysbus_mmio_map(SYS_BUS_DEVICE(s->irbus), 0, 0xF0037000);
    sysbus_mmio_map(SYS_BUS_DEVICE(s->irbus), 1, 0xF0038000);

    /* IR ISP[0] -> CPU tricore.irq */
    qdev_connect_gpio_out_named(DEVICE(s->irbus), "isp", 0,
        qdev_get_gpio_in_named(DEVICE(&s->cpu), "tricore.irq", 0));

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
    memory_region_add_subregion_overlap(sysmem, sc->memmap[TC33X_SFR].base,
                                        &s->sfr->iomem, -1);
    memory_region_add_subregion(sysmem, sc->memmap[TC33X_ASCLIN].base,
                                &s->asclin->iomem);
    memory_region_add_subregion(sysmem, sc->memmap[TC33X_SCU].base,
                                &s->scu->iomem);
    memory_region_add_subregion(sysmem, sc->memmap[TC33X_STM].base,
                                &s->stm->iomem);
}

static void tc33x_soc_init(Object *obj)
{
    TC33XSoCState *s = TC33X_SOC(obj);
    TC33XSoCClass *sc = TC33X_SOC_GET_CLASS(s);

    object_initialize_child(obj, "tc37x", &s->cpu, sc->cpu_type);
}

static void tc33x_soc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = tc33x_soc_realize;
}

static void tc337_soc_class_init(ObjectClass *oc, const void *data)
{
    TC33XSoCClass *sc = TC33X_SOC_CLASS(oc);

    sc->name         = "tc33xb-soc";
    sc->cpu_type     = TRICORE_CPU_TYPE_NAME("tc3x");
    sc->memmap       = tc33xb_soc_memmap;
    sc->num_cpus     = 1;
}

static const TypeInfo tc33x_soc_types[] = {
    {
        .name          = "tc337-soc",
        .parent        = TYPE_TC33X_SOC,
        .class_init    = tc337_soc_class_init,
    }, {
        .name          = TYPE_TC33X_SOC,
        .parent        = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(TC33XSoCState),
        .instance_init = tc33x_soc_init,
        .class_size    = sizeof(TC33XSoCClass),
        .class_init    = tc33x_soc_class_init,
        .abstract      = true,
    },
};

DEFINE_TYPES(tc33x_soc_types)
