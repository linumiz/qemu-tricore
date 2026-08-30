/*
 * AURIX ERAY/FlexRay emulation based solely on public sources.
 *
 * Register offsets, controller-state encodings and message-RAM concepts are
 * derived from the public TC2x/TC3x/TC4x User Manuals and the corresponding
 * Infineon IfxEray_* register and driver headers published on GitHub.  Local
 * iLLD copies in this workspace are developer reference material only; they
 * are not project-provided QEMU source.  No NDA-only manuals, vendor RTL,
 * firmware binaries or confidential PHY details are used here.
 */

#include "qemu/osdep.h"
#include "hw/net/tricore_eray.h"
#include "hw/core/irq.h"
#include "qapi/error.h"
#include "migration/vmstate.h"

#define ERAY_CCSV      0x100
#define ERAY_CCEV      0x104
#define ERAY_SUCC1     0x108
#define ERAY_NEMC      0x10c
#define ERAY_MBSC1     0x110
#define ERAY_NDAT1     0x114
#define ERAY_CMD       0x118
#define ERAY_CYCLE     0x11c
#define ERAY_SLOTSTAT  0x120
#define ERAY_MBID      0x124
#define ERAY_MBCTRL    0x128
#define ERAY_MBPENDING 0x12c
#define ERAY_SCHED_CFG 0x130

#define CCSV_POC_SHIFT 0
#define CCSV_POC_MASK  0x3f
#define POC_CONFIG     0x01
#define POC_READY      0x02
#define POC_NORMAL_ACTIVE 0x0d
#define POC_HALT       0x10
#define CMD_CONFIG     0x01
#define CMD_READY      0x02
#define CMD_COLDSTART  0x03
#define CMD_RUN        0x04
#define CMD_HALT       0x05
#define MBCTRL_COMMIT  BIT(0)
#define MBCTRL_UNLOCK  BIT(1)
#define MBCTRL_CHANNEL_B BIT(2)
#define ERAY_CYCLE_NS 1000

static QTAILQ_HEAD(, TriCoreERAYState) eray_bus =
    QTAILQ_HEAD_INITIALIZER(eray_bus);

static void eray_update_irq(TriCoreERAYState *s);
static void eray_schedule(TriCoreERAYState *s);

static void eray_scheduler_cb(void *opaque)
{
    TriCoreERAYState *s = opaque;
    if (s->ccsv == POC_NORMAL_ACTIVE) {
        s->cycle = (s->cycle + 1) & 0x3f;
        s->slot_status = (s->slot_status & 0x80000000) | (s->cycle << 16);
        s->ccev |= BIT(2); /* cycle start event */
        eray_update_irq(s);
        eray_schedule(s);
    }
}

static void eray_schedule(TriCoreERAYState *s)
{
    timer_mod(s->scheduler, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + ERAY_CYCLE_NS);
}

static void eray_update_irq(TriCoreERAYState *s)
{
    qemu_set_irq(s->int0_irq, !!(s->ccev & 0xffff));
    qemu_set_irq(s->int1_irq, !!(s->mbsc1 | s->ndat1));
}

static void eray_command(TriCoreERAYState *s, uint32_t cmd)
{
    s->command = cmd;
    switch (cmd & 0xff) {
    case CMD_CONFIG: s->ccsv = POC_CONFIG; break;
    case CMD_READY: s->ccsv = POC_READY; break;
    case CMD_COLDSTART: s->ccsv = POC_NORMAL_ACTIVE; s->cycle = 0; eray_schedule(s); break;
    case CMD_RUN: s->ccsv = POC_NORMAL_ACTIVE; eray_schedule(s); break;
    case CMD_HALT: s->ccsv = POC_HALT; timer_del(s->scheduler); break;
    default: s->ccev |= BIT(0); break;
    }
    eray_update_irq(s);
}

static void eray_commit_message(TriCoreERAYState *s)
{
    TriCoreERAYState *peer;
    uint32_t offset = (s->mbid & 0xff) * 64;
    uint32_t frame_id = ldl_le_p(&s->msg_data[offset]);
    if (!(s->mbctrl & MBCTRL_COMMIT) || offset + 64 > sizeof(s->msg_data)) {
        s->ccev |= BIT(1);
        return;
    }
    /* The portable bus models a static-slot transfer at the programmed cycle.
     * A/B selection is retained in slot_status while both channels share the
     * same deterministic virtual timebase. */
    s->slot_status = (frame_id & 0x7ff) | ((s->cycle & 0x3f) << 16) |
                     ((s->mbctrl & MBCTRL_CHANNEL_B) ? BIT(31) : 0);
    QTAILQ_FOREACH(peer, &eray_bus, bus_node) {
        if (peer == s || peer->ccsv != POC_NORMAL_ACTIVE) continue;
        uint32_t peer_off = (s->mbid & 0xff) * 64;
        memcpy(&peer->msg_data[peer_off], &s->msg_data[offset], 64);
        peer->ndat1 |= BIT(s->mbid & 31);
        peer->mbsc1 |= BIT(s->mbid & 31);
        peer->slot_status = s->slot_status;
        eray_update_irq(peer);
    }
    s->mbsc1 |= BIT(s->mbid & 31);
    eray_update_irq(s);
}

static uint64_t eray_read(void *opaque, hwaddr off, unsigned size)
{
    TriCoreERAYState *s = opaque;
    switch (off) {
    case ERAY_CCSV: return s->ccsv;
    case ERAY_CCEV: return s->ccev;
    case ERAY_SUCC1: return s->succ1;
    case ERAY_NEMC: return s->nemc;
    case ERAY_MBSC1: return s->mbsc1;
    case ERAY_NDAT1: return s->ndat1;
    case ERAY_CMD: return s->command;
    case ERAY_CYCLE: return s->cycle;
    case ERAY_SLOTSTAT: return s->slot_status;
    case ERAY_MBID: return s->mbid;
    case ERAY_MBCTRL: return s->mbctrl;
    case ERAY_MBPENDING: return s->mbsc1 | s->ndat1;
    case ERAY_SCHED_CFG: return ERAY_CYCLE_NS;
    default: return 0;
    }
}

static void eray_write(void *opaque, hwaddr off, uint64_t value,
                       unsigned size)
{
    TriCoreERAYState *s = opaque;
    switch (off) {
    case ERAY_CCEV: s->ccev &= ~value; break; /* documented W1C status */
    case ERAY_SUCC1: s->succ1 = value; break;
    case ERAY_NEMC: s->nemc = value; break;
    case ERAY_MBSC1: s->mbsc1 &= ~value; break;
    case ERAY_NDAT1: s->ndat1 &= ~value; break;
    case ERAY_CMD: eray_command(s, value); break;
    case ERAY_CYCLE: s->cycle = value & 0xff; break;
    case ERAY_SLOTSTAT: s->slot_status = value; break;
    case ERAY_MBID: s->mbid = value & 0xff; break;
    case ERAY_MBCTRL: s->mbctrl = value; if (value & MBCTRL_COMMIT) eray_commit_message(s); break;
    default: break;
    }
    eray_update_irq(s);
}

static const MemoryRegionOps eray_ops = {
    .read = eray_read, .write = eray_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4, .valid.max_access_size = 4,
};

static void eray_reset(DeviceState *dev)
{
    TriCoreERAYState *s = TRICORE_ERAY(dev);
    s->ccsv = POC_CONFIG;
    s->ccev = s->succ1 = s->nemc = s->mbsc1 = s->ndat1 = 0;
    s->command = s->cycle = s->slot_status = 0;
    s->mbid = s->mbctrl = 0;
    timer_del(s->scheduler);
    memset(s->msg_data, 0, sizeof(s->msg_data));
    eray_update_irq(s);
}

static void eray_init(Object *obj)
{
    TriCoreERAYState *s = TRICORE_ERAY(obj);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->msg_ram);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->int0_irq);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->int1_irq);
}

static void eray_realize(DeviceState *dev, Error **errp)
{
    TriCoreERAYState *s = TRICORE_ERAY(dev);
    memory_region_init_io(&s->iomem, OBJECT(dev), &eray_ops, s,
                          "tricore-eray", 0x1000);
    memory_region_init_ram(&s->msg_ram, OBJECT(dev), "tricore-eray-msg-ram",
                           sizeof(s->msg_data), &error_fatal);
    s->scheduler = timer_new_ns(QEMU_CLOCK_VIRTUAL, eray_scheduler_cb, s);
    QTAILQ_INSERT_TAIL(&eray_bus, s, bus_node);
}

static const VMStateDescription vmstate_eray = {
    .name = TYPE_TRICORE_ERAY, .version_id = 1, .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ccsv, TriCoreERAYState), VMSTATE_UINT32(ccev, TriCoreERAYState),
        VMSTATE_UINT32(succ1, TriCoreERAYState), VMSTATE_UINT32(nemc, TriCoreERAYState),
        VMSTATE_UINT32(mbsc1, TriCoreERAYState), VMSTATE_UINT32(ndat1, TriCoreERAYState),
        VMSTATE_UINT32(command, TriCoreERAYState), VMSTATE_UINT32(cycle, TriCoreERAYState),
        VMSTATE_UINT32(slot_status, TriCoreERAYState), VMSTATE_UINT32(mbid, TriCoreERAYState),
        VMSTATE_UINT32(mbctrl, TriCoreERAYState), VMSTATE_TIMER_PTR(scheduler, TriCoreERAYState),
        VMSTATE_END_OF_LIST()
    }
};

static void eray_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = eray_realize;
    device_class_set_legacy_reset(dc, eray_reset);
    dc->vmsd = &vmstate_eray;
}

static const TypeInfo eray_info = {
    .name = TYPE_TRICORE_ERAY, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(TriCoreERAYState), .instance_init = eray_init,
    .class_init = eray_class_init,
};

static void eray_register_types(void)
{
    type_register_static(&eray_info);
}
type_init(eray_register_types)
