/*
 * Infineon TC4Dx SoC System emulation.
 *
 * Copyright (c) 2026 Parthiban Nallathambi <parthiban@linumiz.com>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev.h"
#include "hw/core/sysbus.h"
#include "hw/misc/unimp.h"
#include "qapi/error.h"
#include "qemu/units.h"

#include "hw/tricore/tc4x_cpu.h"
#include "hw/tricore/tc4dx_soc.h"
#include "hw/tricore/triboard.h"
#include "qom/object.h"

/* Public TC4Dx SRC service-request slots for ERAY0 and ERAY1. */
#define TC4DX_SRC_ERAY0_INT0 720
#define TC4DX_SRC_ERAY0_INT1 721
#define TC4DX_SRC_ERAY1_INT0 722
#define TC4DX_SRC_ERAY1_INT1 723
/* Generation-specific ERAY service-request table from the public TC4x
 * User Manual / IfxEray headers. */
static const uint16_t tc4dx_eray_src[2][2] = {
    { TC4DX_SRC_ERAY0_INT0, TC4DX_SRC_ERAY0_INT1 },
    { TC4DX_SRC_ERAY1_INT0, TC4DX_SRC_ERAY1_INT1 },
};

static uint64_t tc4dx_cre_read(void *opaque, hwaddr offset, unsigned size)
{
    TC4DXSoCState *s = opaque;
    return offset == 0 ? s->cre_control :
           offset == 4 ? s->cre_status : 0;
}

static void tc4dx_cre_write(void *opaque, hwaddr offset, uint64_t value,
                            unsigned size)
{
    TC4DXSoCState *s = opaque;
    if (offset == 0) {
        s->cre_control = value;
        s->cre_status = value ? 1 : 0;
    } else if (offset == 4) {
        s->cre_status &= ~value;
    }
}

static const MemoryRegionOps tc4dx_cre_ops = {
    .read = tc4dx_cre_read, .write = tc4dx_cre_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4, .valid.max_access_size = 4,
};

static void tc4dx_soc_realize(DeviceState *dev_soc, Error **errp)
{
    TC4DXSoCState *s = TC4DX_SOC(dev_soc);
    DeviceState *cpu, *dev;
    SysBusDevice *busdev;
    MemoryRegion *system_memory = get_system_memory();
    int i;

    /* Minimal TC4x CRE control/status block.  Routing itself is represented
     * by the per-channel SRC/DRE IRQ wiring below. */
    memory_region_init_io(&s->cre_region, OBJECT(s), &tc4dx_cre_ops, s,
                          "tc4x-cre", 0x1000);
    memory_region_add_subregion(system_memory, 0xF4700000, &s->cre_region);

    if (!clock_has_source(s->fosc)) {
        error_setg(errp, "osc clock must be wired up by the board code");
        return;
    }

    /* IR controller */
    dev = DEVICE(&s->ir);
    qdev_prop_set_bit(dev, "tc4x-mode", true);
    qdev_prop_set_uint8(dev, "num-isps", TC4DX_MAX_CPUS);
    qdev_prop_set_uint16(dev, "num-irqs", 2048);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->ir), errp)) {
        return;
    }
    busdev = SYS_BUS_DEVICE(dev);
    sysbus_mmio_map(busdev, 0, 0xF4430000);
    sysbus_mmio_map(busdev, 1, 0xF4432000);

    /* Clock controller */
    dev = DEVICE(&s->clock);
    qdev_connect_clock_in(dev, "fosc", s->fosc);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->clock), errp)) {
        return;
    }
    busdev = SYS_BUS_DEVICE(dev);
    sysbus_mmio_map(busdev, 0, 0xF0064000);

    for (i = 0; i < TC4DX_MAX_CPUS; i++) {
        memory_region_add_subregion(system_memory, 0, &s->cpus[i].container);
        cpu = DEVICE(&s->cpus[i]);
        qdev_prop_set_string(cpu, "cpu-type", TRICORE_CPU_TYPE_NAME("tc4x"));
        qdev_prop_set_uint8(cpu, "cpu-id", i);
        qdev_prop_set_bit(cpu, "start-powered-off", i != 0);
        object_property_set_link(OBJECT(cpu), "ir", OBJECT(&s->ir), &error_abort);
        qdev_connect_clock_in(cpu, "fstm", s->clock.fstm);
        qdev_connect_clock_in(cpu, "fcpu", s->clock.fsri);
        object_property_set_link(OBJECT(cpu), "memory", OBJECT(system_memory),
                                 &error_abort);
        object_property_set_link(OBJECT(cpu), "ir", OBJECT(&s->ir),
                                 &error_abort);
        if (!sysbus_realize(SYS_BUS_DEVICE(cpu), errp)) {
            return;
        }
        if (i > 0) {
            s->cpus[i].bootcon = 1;
        }
        if (i > 0) {
            /* Secondary TC4x cores reset in boot-halt mode.  The SSW reads
             * BOOTCON.BHALT before issuing the release write. */
            s->cpus[i].bootcon = 1;
        }
        qdev_connect_gpio_out_named(
            DEVICE(&s->ir), "isp", i,
            qdev_get_gpio_in_named(cpu, "tricore.irq", 0));
        sysbus_connect_irq(
            SYS_BUS_DEVICE(cpu), 0,
            qdev_get_gpio_in_named(DEVICE(&s->ir), "irq", 8 + 0x10 * i + 2));
        if (i > 0) {
            char *name = g_strdup_printf("CPU%d_SFR_ALIAS", i);
            memory_region_init_alias(&s->cpu_sfr_alias[i], OBJECT(s), name,
                                     &s->cpus[i].sfr_stub, 0, 0x40000);
            g_free(name);
            memory_region_add_subregion_overlap(
                system_memory, 0xF8800000 + (i * 0x40000),
                &s->cpu_sfr_alias[i], 1);
        }
    }

    create_unimplemented_device("tc4x-sfr", 0xF0000000, 0x400000);

    for (i = 0; i < TC4DX_MAX_ASCLIN; i++) {
        dev = DEVICE(&s->asclin[i]);
        qdev_prop_set_chr(DEVICE(&s->asclin[i]), "chardev", serial_hd(i));
        if (!sysbus_realize(SYS_BUS_DEVICE(&s->asclin[i]), errp)) {
            return;
        }
        busdev = SYS_BUS_DEVICE(dev);
        sysbus_mmio_map(busdev, 0, 0xF46C0000 + 0x200 * i);
        sysbus_connect_irq(
            busdev, 0,
            qdev_get_gpio_in_named(DEVICE(&s->ir), "irq", 173 + i * 3));
        sysbus_connect_irq(
            busdev, 1,
            qdev_get_gpio_in_named(DEVICE(&s->ir), "irq", 172 + i * 3));
        sysbus_connect_irq(
            busdev, 2,
            qdev_get_gpio_in_named(DEVICE(&s->ir), "irq", 174 + i * 3));
    }

    /* TC4Dx MCMCAN control windows; each channel has private message state. */
    static const hwaddr mcan_base[TC4DX_MAX_MCAN] = {
        0xF4710000, 0xF4730000, 0xF4750000, 0xF4770000, 0xF4790000,
    };
    for (i = 0; i < TC4DX_MAX_MCAN; i++) {
        dev = DEVICE(&s->mcan[i]);
        if (s->canbus) {
            object_property_set_link(OBJECT(dev), "canbus",
                                     OBJECT(s->canbus), &error_abort);
        }
        if (!sysbus_realize(SYS_BUS_DEVICE(dev), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, mcan_base[i]);
        sysbus_mmio_map(SYS_BUS_DEVICE(dev), 1, mcan_base[i] + 0x3000);
        /* Keep all documented service-request outputs visible at the TC4x
         * interrupt router; the controller core assigns event meaning. */
        for (unsigned irq = 0; irq < 16; irq++) {
            sysbus_connect_irq(SYS_BUS_DEVICE(dev), irq,
                qdev_get_gpio_in_named(DEVICE(&s->ir), "irq",
                                       700 + i * 16 + irq));
        }
    }

    /* The TC4x XGMAC/EDMA front-end shares the common deterministic model. */
    dev = DEVICE(&s->eth);
    if (!sysbus_realize(SYS_BUS_DEVICE(dev), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, 0xF9000000);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0,
                       qdev_get_gpio_in_named(DEVICE(&s->ir), "irq", 700));

    /* TC4Dx LETH0 has a separate register window and service request group. */
    dev = DEVICE(&s->leth);
    if (!sysbus_realize(SYS_BUS_DEVICE(dev), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, 0xF9400000);
    sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0,
                       qdev_get_gpio_in_named(DEVICE(&s->ir), "irq", 710));

    /* TC4Dx provides two ERAY instances at the public IfxEray base addresses. */
    static const hwaddr eray_base[2] = { 0xF441C000, 0xF441D000 };
    for (i = 0; i < 2; i++) {
        dev = DEVICE(&s->eray[i]);
        /* TC4x public profile uses a 16 KiB message window and the portable
         * 64-byte payload fixture. */
        qdev_prop_set_uint32(dev, "message-ram-size", 0x4000);
        qdev_prop_set_uint32(dev, "payload-max", 64);
        if (!sysbus_realize(SYS_BUS_DEVICE(dev), errp)) {
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, eray_base[i]);
        /* Keep each message RAM outside the adjacent ERAY register window. */
        sysbus_mmio_map(SYS_BUS_DEVICE(dev), 1, eray_base[i] + 0x2000);
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), 0,
                           qdev_get_gpio_in_named(DEVICE(&s->ir), "irq",
                                                  tc4dx_eray_src[i][0]));
        sysbus_connect_irq(SYS_BUS_DEVICE(dev), 1,
                           qdev_get_gpio_in_named(DEVICE(&s->ir), "irq",
                                                  tc4dx_eray_src[i][1]));
    }
    dev = DEVICE(&s->dma);
    if (!sysbus_realize(SYS_BUS_DEVICE(dev), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, 0xF0000000);
    dev = DEVICE(&s->port);
    if (!sysbus_realize(SYS_BUS_DEVICE(dev), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, 0xF000A000);
    dev = DEVICE(&s->ici);
    if (!sysbus_realize(SYS_BUS_DEVICE(dev), errp)) return;
    sysbus_mmio_map(SYS_BUS_DEVICE(dev), 0, 0xF000B000);
    /* Route each ICI output into the TC4x IR source bank.  Keeping this
     * connection in the SoC (rather than the test board) makes firmware-visible
     * inter-core requests follow the same ISP path as peripheral interrupts. */
    for (unsigned i = 0; i < 6; i++)
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->ici), i,
            qdev_get_gpio_in_named(DEVICE(&s->ir), "irq", 720 + i));
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->gate), errp)) {
        return;
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->gate), 0, 0xF000B400);
}

static void tc4dx_soc_init(Object *obj)
{
    TC4DXSoCState *s = TC4DX_SOC(obj);

    for (unsigned i = 0; i < TC4DX_MAX_CPUS; i++) {
        char *name = g_strdup_printf("tc4x-cpu%u", i);
        object_initialize_child(obj, name, &s->cpus[i], TYPE_TC4X_CPU);
        g_free(name);
    }
    object_initialize_child(obj, "ir", &s->ir, TYPE_TRICORE_IR);
    object_initialize_child(obj, "clock", &s->clock, TYPE_TC4X_CLOCK);
    for (unsigned i = 0; i < TC4DX_MAX_ASCLIN; i++) {
        char *name = g_strdup_printf("asclin%u", i);
        object_initialize_child(obj, name, &s->asclin[i], TYPE_TRICORE_ASCLIN);
        g_free(name);
    }
    for (unsigned i = 0; i < TC4DX_MAX_MCAN; i++) {
        char *name = g_strdup_printf("mcan%u", i);
        object_initialize_child(obj, name, &s->mcan[i], TYPE_TRICORE_MCAN);
        g_free(name);
    }
    object_initialize_child(obj, "eth", &s->eth, TYPE_TRICORE_GETH);
    object_initialize_child(obj, "leth", &s->leth, TYPE_TRICORE_LETH);
    /* These embedded children are realized below; initialize them here so
     * DEVICE() receives a valid QOM object during TC4x board startup. */
    object_initialize_child(obj, "dma", &s->dma, TYPE_TRICORE_DMA);
    object_initialize_child(obj, "port", &s->port, TYPE_TRICORE_PORT);
    object_initialize_child(obj, "ici", &s->ici, TYPE_TRICORE_ICI);
    object_initialize_child(obj, "gate", &s->gate, TYPE_TRICORE_GATE);
    for (unsigned i = 0; i < 2; i++) {
        char *name = g_strdup_printf("eray%u", i);
        object_initialize_child(obj, name, &s->eray[i], TYPE_TRICORE_ERAY);
        g_free(name);
    }

    s->fosc = qdev_init_clock_in(DEVICE(s), "fosc", NULL, NULL, 0);
}

static Property tc4dx_soc_props[] = {
    DEFINE_PROP_LINK("canbus", TC4DXSoCState, canbus,
                     TYPE_CAN_BUS, CanBusState *),
};

static void tc4dx_soc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = tc4dx_soc_realize;
    device_class_set_props_n(dc, tc4dx_soc_props,
                             ARRAY_SIZE(tc4dx_soc_props));
}

static void tc4d7_soc_class_init(ObjectClass *oc, const void *data)
{
    TC4DXSoCClass *sc = TC4DX_SOC_CLASS(oc);

    sc->name         = "tc4dx-soc";
}

static const TypeInfo tc4dx_soc_types[] = {
    {
        .name = "tc4d7-soc",
        .parent = TYPE_TC4DX_SOC,
        .class_init = tc4d7_soc_class_init,
    },
    {
        .name = TYPE_TC4DX_SOC,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(TC4DXSoCState),
        .instance_init = tc4dx_soc_init,
        .class_size = sizeof(TC4DXSoCClass),
        .class_init = tc4dx_soc_class_init,
        .abstract = true,
    },
};

DEFINE_TYPES(tc4dx_soc_types)
