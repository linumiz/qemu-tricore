/*
 * Infineon TriBoard System emulation.
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
#include "hw/core/clock.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-clock.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "elf.h"

#include "hw/tricore/tc27xd_soc.h"
#include "hw/tricore/tc39xb_soc.h"
#include "hw/tricore/tc4dx_soc.h"
#include "hw/tricore/triboard.h"
#include "system/address-spaces.h"

static void tricore_load_kernel(TriCoreCPU *cpu, const char *kernel_filename)
{
    uint64_t entry;
    long kernel_size;
    CPUTriCoreState *env;

    kernel_size = load_elf(kernel_filename, NULL, NULL, NULL, &entry, NULL,
                           NULL, NULL, ELFDATA2LSB, EM_TRICORE, 1, 0);
    if (kernel_size <= 0) {
        error_report("no kernel file '%s'", kernel_filename);
        exit(1);
    }
    env = &cpu->env;
    env->PC = entry;
}

typedef struct TC277LoadContext {
    TC27XDSoCState *soc;
    uint64_t core_start[3];
} TC277LoadContext;
static TC277LoadContext *tc277_load_context;

static void tc277_capture_symbol(const char *name, int info, uint64_t value,
                                 uint64_t size)
{
    TC277LoadContext *ctx = tc277_load_context;
    if (!ctx) return;
    if (!strcmp(name, "_Core1_start")) ctx->core_start[1] = value;
    if (!strcmp(name, "_Core2_start")) ctx->core_start[2] = value;
}

static void tc277_load_multicore_kernel(TC27XDSoCState *soc,
                                        const char *kernel_filename)
{
    TC277LoadContext ctx = { .soc = soc };
    uint64_t entry;
    tc277_load_context = &ctx;
    if (load_elf_ram_sym(kernel_filename, NULL, NULL, NULL, &entry, NULL,
                         NULL, NULL, ELFDATA2LSB, EM_TRICORE, 1, 0,
                         &address_space_memory, false, tc277_capture_symbol) < 0) {
        error_report("no kernel file '%s'", kernel_filename);
        exit(1);
    }
    tc277_load_context = NULL;
    soc->cpus[0].env.PC = entry;
    for (unsigned i = 1; i < 3; i++) {
        if (ctx.core_start[i]) {
            soc->cpus[i].env.PC = ctx.core_start[i];
            CPUState *cs = CPU(&soc->cpus[i]);
            cs->halted = 0;
            cpu_resume(cs);
        }
    }
}

static void triboard_machine_tc4d7_init(MachineState *machine)
{
    DeviceState *dev;
    Clock *fosc;

    /* This clock doesn't need migration because it is fixed-frequency */
    fosc = clock_new(OBJECT(machine), "fosc");
    clock_set_hz(fosc, 25000000);

    dev = qdev_new("tc4d7-soc");
    object_property_add_child(OBJECT(machine), "soc", OBJECT(dev));
    qdev_connect_clock_in(dev, "fosc", fosc);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(dev), &error_fatal);

    if (machine->kernel_filename) {
        tc4x_cpu_load_kernel(TC4DX_SOC(dev)->cpus[0].tricore,
                             machine->kernel_filename, 0, 4 * MiB);
    }
}

static void triboard_machine_tc4d7_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->init = triboard_machine_tc4d7_init;
    mc->desc = "Infineon AURIX Kit TC4D7 Lite";
    mc->default_cpu_type = TRICORE_CPU_TYPE_NAME("tc4x");
}


static void triboard_machine_tc27xd_init(MachineState *machine)
{
    TriBoardMachineState *ms = TRIBOARD_MACHINE(machine);
    TriBoardMachineClass *amc = TRIBOARD_MACHINE_GET_CLASS(machine);

    object_initialize_child(OBJECT(machine), "tc27xd_soc", &ms->tc27xd_soc,
                            amc->soc_name);
    sysbus_realize(SYS_BUS_DEVICE(&ms->tc27xd_soc), &error_fatal);

    if (machine->kernel_filename) {
        tc277_load_multicore_kernel(&ms->tc27xd_soc,
                                    machine->kernel_filename);
    }
}

static void triboard_machine_tc39xb_init(MachineState *machine)
{
    TriBoardMachineState *ms = TRIBOARD_MACHINE(machine);
    TriBoardMachineClass *amc = TRIBOARD_MACHINE_GET_CLASS(machine);

    object_initialize_child(OBJECT(machine), "tc39xb_soc", &ms->tc39xb_soc,
                            amc->soc_name);
    sysbus_realize(SYS_BUS_DEVICE(&ms->tc39xb_soc), &error_fatal);

    if (machine->kernel_filename) {
        tricore_load_kernel(&ms->tc39xb_soc.cpus[0], machine->kernel_filename);
    }
}

static void triboard_machine_tc277d_class_init(ObjectClass *oc,
                                               const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    TriBoardMachineClass *amc = TRIBOARD_MACHINE_CLASS(oc);

    mc->init = triboard_machine_tc27xd_init;
    mc->desc = "Infineon AURIX TriBoard TC277 (D-Step)";
    mc->max_cpus = 1;
    amc->soc_name = "tc277d-soc";
};

static void triboard_machine_tc397b_class_init(ObjectClass *oc,
                                               const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    TriBoardMachineClass *amc = TRIBOARD_MACHINE_CLASS(oc);

    mc->init = triboard_machine_tc39xb_init;
    mc->desc = "Infineon AURIX TriBoard TC397 (B-Step)";
    mc->max_cpus = 6;
    amc->soc_name = "tc397b-soc";
};

static const TypeInfo triboard_machine_types[] = {
    {
        .name = TYPE_TRIBOARD_MACHINE,
        .parent = TYPE_MACHINE,
        .instance_size = sizeof(TriBoardMachineState),
        .class_size = sizeof(TriBoardMachineClass),
        .abstract = true,
    },
    {
        .name = MACHINE_TYPE_NAME("KIT_AURIX_TC277_TRB"),
        .parent = TYPE_TRIBOARD_MACHINE,
        .class_init = triboard_machine_tc277d_class_init,
    },
    {
        .name = MACHINE_TYPE_NAME("KIT_AURIX_TC397B_TRB"),
        .parent = TYPE_TRIBOARD_MACHINE,
        .class_init = triboard_machine_tc397b_class_init,
    },
    {
        .name = MACHINE_TYPE_NAME("KIT_A3G_TC4D7_LITE"),
        .parent = TYPE_MACHINE,
        .class_init = triboard_machine_tc4d7_class_init,
    },
};

DEFINE_TYPES(triboard_machine_types)
