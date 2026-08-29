#include "qemu/osdep.h"
#include "hw/core/clock.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/intc/tricore_ir.h"
#include "hw/timer/tricore_stm.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/typedefs.h"
#include "qemu/units.h"
#include "qom/object.h"
#include "elf.h"
#include "tc4x_cpu.h"
#include "cpu-qom.h"
#include "cpu.h"
#include "system/memory.h"
#include "system/reset.h"
#include <stdbool.h>
#include <stdlib.h>

#define TC4X_PFLASH_C_BASE  0x80000000ULL
#define TC4X_PFLASH_NC_BASE 0xA0000000ULL
#define TC4X_PFLASH_STRIDE  0x00400000ULL

static uint64_t tc4x_cpu_sfr_stub_read(void *opaque, hwaddr offset,
                                       unsigned size)
{
    TC4xCPUState *s = opaque;

    switch (offset) {
    case 0xD000: return s->krst0;
    case 0xD004: return s->krst1;
    case 0x1FE08: return s->boot_pc;
    case 0x1FE60: return s->bootcon;
    default: break;
    }
    return 0;
}

static void tc4x_cpu_sfr_stub_write(void *opaque, hwaddr offset,
                                    uint64_t value, unsigned size)
{
    TC4xCPUState *s = opaque;

    switch (offset) {
    case 0xD000:
        s->krst0 = value;
        break;
    case 0xD004:
        s->krst1 = value;
        break;
    case 0x1FE08:
        s->boot_pc = value;
        break;
    case 0x1FE60:
        s->bootcon = value;
        if (!(value & 1)) {
            tc4x_cpu_start_core(s, s->boot_pc);
        }
        break;
    default:
        break;
    }
}

static const MemoryRegionOps tc4x_cpu_sfr_stub_ops = {
    .read = tc4x_cpu_sfr_stub_read,
    .write = tc4x_cpu_sfr_stub_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
};

static void tc4x_cpu_instance_init(Object *obj)
{
    TC4xCPUState *s = TC4X_CPU(obj);
    Error *errp = NULL;

    /* Can't init the cpu here, we don't yet know which model to use */
    memory_region_init(&s->container, obj, "tc4x_cpu-container", UINT64_MAX);
    memory_region_init(&s->local_container, obj, "tc4x_cpu-local-container",
                       UINT64_MAX);

    object_initialize_child(obj, "stm ", &s->stm, TYPE_TRICORE_STM);
    object_property_set_bool(OBJECT(&s->stm), "tc4x-mode", true, &errp);
    if (errp != NULL) {
        return;
    }

    s->fstm = qdev_init_clock_in(DEVICE(obj), "fstm", NULL, NULL, 0);
    s->fcpu = qdev_init_clock_in(DEVICE(obj), "fcpu", NULL, NULL, 0);
}

static void tc4x_cpu_realize(DeviceState *dev, Error **errp)
{
    TC4xCPUState *s = TC4X_CPU(dev);
    Error *err = NULL;

    if (!s->board_memory) {
        error_setg(errp, "memory property was not set");
        return;
    }

    /* fcpu must be connected; fstm is optional */
    if (!clock_has_source(s->fcpu)) {
        error_setg(errp, "tricore: fcpu must be connected");
        return;
    }
    if (!clock_has_source(s->fstm)) {
        error_setg(errp, "tricore: fstm must be connected");
        return;
    }

    memory_region_init_alias(&s->board_memory_alias, OBJECT(s),
                             "tc4x-board-memory", s->board_memory, 0,
                             memory_region_size(s->board_memory));
    memory_region_add_subregion_overlap(&s->local_container, 0,
                                        &s->board_memory_alias, -1);
    /* TODO: add mirror mememory */

    s->tricore = TRICORE_CPU(
        object_new_with_props(s->cpu_type, OBJECT(s), "cpu", &err, NULL));
    if (err != NULL) {
        error_propagate(errp, err);
        return;
    }
    /* Nested CPUs must have stable distinct indices; migration registration
     * uses this value to distinguish the per-core CPU state streams. */
    CPU(s->tricore)->cpu_index = s->id;

    object_property_set_link(OBJECT(s->tricore), "memory",
                             OBJECT(&s->local_container), &error_abort);
    object_property_set_link(OBJECT(s->tricore), "ir", OBJECT(s->ir),
                             &error_abort);
    object_property_set_bool(OBJECT(s->tricore), "start-powered-off",
                             s->start_powered_off, &error_abort);
    /* TODO: Connect STM for TC1.8 csfr */

    if (!qdev_realize(DEVICE(s->tricore), NULL, errp)) {
        return;
    }

    qdev_connect_clock_in(DEVICE(&s->stm), "fstm", s->fstm);
    qdev_prop_set_bit(DEVICE(&s->stm), "tc4x-mode", true);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->stm), errp)) {
        return;
    }
    memory_region_add_subregion(
        &s->container, 0xF8800000 + (0x40000 * s->id),
        sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->stm), 0));

    memory_region_init_io(&s->sfr_stub, OBJECT(s), &tc4x_cpu_sfr_stub_ops, s,
                          "tc4x_cpu_sfr_stub", 0x40000);
    memory_region_add_subregion_overlap(&s->container,
                                        0xF8800000 + (0x40000 * s->id),
                                        &s->sfr_stub, -100);

    sysbus_pass_irq(SYS_BUS_DEVICE(dev), SYS_BUS_DEVICE(&s->stm));
    qdev_pass_gpios(DEVICE(s->tricore), dev, "tricore.irq");
    qdev_pass_gpios(DEVICE(s->tricore), dev, "tricore.nmi");

    char *pflashname = g_strdup_printf("CPU%d_PFLASH", s->id);
    memory_region_init_ram(&s->pflash, NULL, pflashname, s->pflash_size, errp);
    free(pflashname);
    if (*errp) {
        return;
    }
    memory_region_add_subregion(
        &s->container, TC4X_PFLASH_C_BASE + (s->id * TC4X_PFLASH_STRIDE),
        &s->pflash);
    char *pflash_alias_name = g_strdup_printf("CPU%d_PFLASH_ALIAS", s->id);
    memory_region_init_alias(&s->pflash_alias, OBJECT(s), pflash_alias_name,
                             &s->pflash, 0, s->pflash_size);
    free(pflash_alias_name);
    memory_region_add_subregion(
        &s->container, TC4X_PFLASH_NC_BASE + (s->id * TC4X_PFLASH_STRIDE),
        &s->pflash_alias);

    char *dsprname = g_strdup_printf("CPU%d_DSPR", s->id);
    memory_region_init_ram(&s->dspr, NULL, dsprname, s->dpsr_size, errp);
    free(dsprname);
    if (*errp) {
        return;
    }
    memory_region_add_subregion(&s->container,
                                0x70000000 - (s->id * 0x10000000), &s->dspr);

    char *psprname = g_strdup_printf("CPU%d_PSPR", s->id);
    memory_region_init_ram(&s->pspr, NULL, psprname, 65 * KiB, errp);
    free(psprname);
    if (*errp) {
        return;
    }
    memory_region_add_subregion(&s->container,
                                0x70100000 - (s->id * 0x10000000), &s->pspr);

    if (s->dlmu_size > 0) {
        char *dlmuname = g_strdup_printf("CPU%d_DLMU", s->id);
        memory_region_init_ram(&s->dlmu, NULL, dlmuname, s->dlmu_size, errp);
        free(dlmuname);
        if (*errp) {
            return;
        }
        memory_region_add_subregion(&s->container,
                                    0x90000000 + (s->id * 512 * KiB), &s->dlmu);
    }

    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->container);
}

static const Property tc4x_cpu_properties[] = {
    DEFINE_PROP_STRING("cpu-type", TC4xCPUState, cpu_type),
    DEFINE_PROP_UINT8("cpu-id", TC4xCPUState, id, 0),
    DEFINE_PROP_LINK("memory", TC4xCPUState, board_memory, TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_LINK("ir", TC4xCPUState, ir, TYPE_TRICORE_IR, TriCoreIRState *),
    DEFINE_PROP_BOOL("start-powered-off", TC4xCPUState, start_powered_off,
                     false),
    DEFINE_PROP_UINT32("dspr-size", TC4xCPUState, dpsr_size, 240 * KiB),
    DEFINE_PROP_UINT32("dlmu-size", TC4xCPUState, dlmu_size, 512 * KiB),
    DEFINE_PROP_UINT32("pflash-size", TC4xCPUState, pflash_size, 4 * MiB),
};

static int tc4x_cpu_post_load(void *opaque, int version_id)
{
    TC4xCPUState *s = opaque;
    if (s->tricore && (s->migration_running || s->id > 0)) {
        CPU(s->tricore)->halted = 0;
        cpu_resume(CPU(s->tricore));
        qemu_cpu_kick(CPU(s->tricore));
    }
    return 0;
}

static int tc4x_cpu_pre_save(void *opaque)
{
    TC4xCPUState *s = opaque;
    s->migration_running = s->tricore && !CPU(s->tricore)->halted;
    return 0;
}

static const VMStateDescription vmstate_tc4x_cpu = {
    .name = "tc4x_cpu",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = tc4x_cpu_pre_save,
    .post_load = tc4x_cpu_post_load,
    .fields = (const VMStateField[]){ VMSTATE_CLOCK(fstm, TC4xCPUState),
                                      VMSTATE_CLOCK(fcpu, TC4xCPUState),
                                      VMSTATE_UINT32(bootcon, TC4xCPUState),
                                      VMSTATE_UINT32(boot_pc, TC4xCPUState),
                                      VMSTATE_UINT32(krst0, TC4xCPUState),
                                      VMSTATE_UINT32(krst1, TC4xCPUState),
                                      VMSTATE_UINT8(migration_running, TC4xCPUState),
                                      VMSTATE_END_OF_LIST() }
};

static void tc4x_cpu_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = tc4x_cpu_realize;
    dc->vmsd = &vmstate_tc4x_cpu;
    device_class_set_props(dc, tc4x_cpu_properties);
}

static const TypeInfo tc4x_cpu_info = {
    .name = TYPE_TC4X_CPU,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(TC4xCPUState),
    .instance_init = tc4x_cpu_instance_init,
    .class_init = tc4x_cpu_class_init,
};

static void tc4x_cpu_reset(void *opaque)
{
    TriCoreCPU *cpu = opaque;

    cpu_reset(CPU(cpu));
}

void tc4x_cpu_load_kernel(TriCoreCPU *cpu, const char *kernel_filename,
                          hwaddr mem_base, int mem_size)
{
    ssize_t image_size;
    uint64_t entry;
    AddressSpace *as;
    CPUState *cs = CPU(cpu);
    CPUTriCoreState *s = &cpu->env;


    as = cpu_get_address_space(cs, 0);

    if (kernel_filename) {
        image_size =
            load_elf_as(kernel_filename, NULL, NULL, NULL, &entry, NULL, NULL,
                        NULL, ELFDATA2LSB, EM_TRICORE, 1, 0, as);
        if (image_size < 0) {
            image_size = load_image_targphys_as(kernel_filename, mem_base,
                                                mem_size, as, NULL);
        }
        if (image_size < 0) {
            error_report("Could not load kernel '%s'", kernel_filename);
            exit(1);
        }
    }

    /* CPU objects (unlike devices) are not automatically reset on system
     * reset, so we must always register a handler to do so. Unlike
     * A-profile CPUs, we don't need to do anything special in the
     * handler to arrange that it starts correctly.
     * This is arguably the wrong place to do this, but it matches the
     * way A-profile does it. Note that this means that every M profile
     * board must call this function!
     */
    qemu_register_reset(tc4x_cpu_reset, cpu);
    s->PC = entry;
}

void tc4x_cpu_start_core(TC4xCPUState *cpu, hwaddr entry)
{
    CPUState *cs;

    if (!cpu || !cpu->tricore) {
        return;
    }

    cs = CPU(cpu->tricore);
    cpu->tricore->env.PC = entry;
    cs->halted = 0;
    qemu_log_mask(CPU_LOG_EXEC, "tc4x: starting CPU%u at PC 0x%08" PRIx64 "\n",
                  cpu->id, (uint64_t)entry);
    cpu_resume(cs);
}


static void tc4x_cpu_register_types(void)
{
    type_register_static(&tc4x_cpu_info);
}

type_init(tc4x_cpu_register_types)
