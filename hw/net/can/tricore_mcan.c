#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/registerfields.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "qemu/log.h"
#include "hw/net/tricore_mcan.h"

REG32(CONTROL, 0x00)
FIELD(CONTROL, ENABLE, 0, 1)
FIELD(CONTROL, LOOPBACK, 1, 1)
REG32(STATUS, 0x04)
FIELD(STATUS, RX_PENDING, 0, 1)
FIELD(STATUS, TX_COMPLETE, 1, 1)
REG32(TXID, 0x08)
REG32(TXDATAL, 0x0c)
REG32(TXDATAH, 0x10)
REG32(TXCTRL, 0x14)
REG32(RXID, 0x18)
REG32(RXDATAL, 0x1c)
REG32(RXDATAH, 0x20)
REG32(INT_ENABLE, 0x24)

static void tricore_mcan_update_irq(TriCoreMCANState *s)
{
    bool active = (s->regs[R_STATUS / 4] & s->regs[R_INT_ENABLE / 4]) != 0;
    qemu_set_irq(s->irq, active);
}

static bool tricore_mcan_can_receive(CanBusClientState *client)
{
    TriCoreMCANState *s = container_of(client, TriCoreMCANState, bus_client);
    return (s->regs[R_CONTROL / 4] & R_CONTROL_ENABLE_MASK) && !s->rx_pending;
}

static ssize_t tricore_mcan_receive(CanBusClientState *client,
                                    const qemu_can_frame *frames,
                                    size_t frames_cnt)
{
    TriCoreMCANState *s = container_of(client, TriCoreMCANState, bus_client);

    if (!frames_cnt || s->rx_pending) {
        return 0;
    }
    s->rx_frame = frames[0];
    s->rx_pending = true;
    s->regs[R_RXID / 4] = s->rx_frame.can_id;
    s->regs[R_RXDATAL / 4] = ldl_le_p(&s->rx_frame.data[0]);
    s->regs[R_RXDATAH / 4] = ldl_le_p(&s->rx_frame.data[4]);
    s->regs[R_STATUS / 4] |= R_STATUS_RX_PENDING_MASK;
    tricore_mcan_update_irq(s);
    return 1;
}

static CanBusClientInfo tricore_mcan_bus_info = {
    .can_receive = tricore_mcan_can_receive,
    .receive = tricore_mcan_receive,
};

static void tricore_mcan_send(TriCoreMCANState *s)
{
    qemu_can_frame frame = { 0 };
    frame.can_id = s->regs[R_TXID / 4];
    frame.can_dlc = 8;
    stl_le_p(&frame.data[0], s->regs[R_TXDATAL / 4]);
    stl_le_p(&frame.data[4], s->regs[R_TXDATAH / 4]);
    if (s->regs[R_CONTROL / 4] & R_CONTROL_LOOPBACK_MASK) {
        tricore_mcan_receive(&s->bus_client, &frame, 1);
    } else if (s->canbus) {
        can_bus_client_send(&s->bus_client, &frame, 1);
    }
    s->regs[R_STATUS / 4] |= R_STATUS_TX_COMPLETE_MASK;
    tricore_mcan_update_irq(s);
}

static uint64_t tricore_mcan_read(void *opaque, hwaddr addr, unsigned size)
{
    TriCoreMCANState *s = opaque;
    uint32_t index = addr >> 2;
    if (index >= ARRAY_SIZE(s->regs)) {
        return 0;
    }
    if (addr == R_RXDATAL) {
        return s->regs[index];
    }
    if (addr == R_RXDATAH) {
        uint32_t value = s->regs[index];
        s->rx_pending = false;
        s->regs[R_STATUS / 4] &= ~R_STATUS_RX_PENDING_MASK;
        tricore_mcan_update_irq(s);
        return value;
    }
    return s->regs[index];
}

static void tricore_mcan_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned size)
{
    TriCoreMCANState *s = opaque;
    uint32_t index = addr >> 2;
    if (index >= ARRAY_SIZE(s->regs)) {
        return;
    }
    if (addr == R_STATUS) {
        s->regs[index] &= ~(uint32_t)value;
    } else if (addr == R_INT_ENABLE || addr == R_CONTROL ||
               addr == R_TXID || addr == R_TXDATAL || addr == R_TXDATAH) {
        s->regs[index] = value;
    } else if (addr == R_TXCTRL && (value & 1)) {
        tricore_mcan_send(s);
    }
    tricore_mcan_update_irq(s);
}

static const MemoryRegionOps tricore_mcan_ops = {
    .read = tricore_mcan_read,
    .write = tricore_mcan_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void tricore_mcan_realize(DeviceState *dev, Error **errp)
{
    TriCoreMCANState *s = TRICORE_MCAN(dev);
    s->bus_client.info = &tricore_mcan_bus_info;
    if (s->canbus && can_bus_insert_client(s->canbus, &s->bus_client) < 0) {
        error_setg(errp, "unable to attach MultiCAN node to CAN bus");
    }
}

static void tricore_mcan_unrealize(DeviceState *dev)
{
    TriCoreMCANState *s = TRICORE_MCAN(dev);
    if (s->canbus) {
        can_bus_remove_client(&s->bus_client);
    }
}

static void tricore_mcan_init(Object *obj)
{
    TriCoreMCANState *s = TRICORE_MCAN(obj);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    memory_region_init_io(&s->iomem, obj, &tricore_mcan_ops, s,
                          TYPE_TRICORE_MCAN, 0x100);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const VMStateDescription vmstate_tricore_mcan = {
    .name = TYPE_TRICORE_MCAN,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, TriCoreMCANState, 16),
        VMSTATE_UINT32(rx_frame.can_id, TriCoreMCANState),
        VMSTATE_UINT8(rx_frame.can_dlc, TriCoreMCANState),
        VMSTATE_UINT8(rx_frame.flags, TriCoreMCANState),
        VMSTATE_UINT8_ARRAY(rx_frame.data, TriCoreMCANState, 64),
        VMSTATE_BOOL(rx_pending, TriCoreMCANState),
        VMSTATE_END_OF_LIST()
    },
};

static Property tricore_mcan_props[] = {
    DEFINE_PROP_LINK("canbus", TriCoreMCANState, canbus, TYPE_CAN_BUS,
                     CanBusState *),
};

static void tricore_mcan_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = tricore_mcan_realize;
    dc->unrealize = tricore_mcan_unrealize;
    dc->vmsd = &vmstate_tricore_mcan;
    device_class_set_props_n(dc, tricore_mcan_props,
                             ARRAY_SIZE(tricore_mcan_props));
}

static const TypeInfo tricore_mcan_info = {
    .name = TYPE_TRICORE_MCAN,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(TriCoreMCANState),
    .instance_init = tricore_mcan_init,
    .class_init = tricore_mcan_class_init,
};

static void tricore_mcan_register_types(void)
{
    type_register_static(&tricore_mcan_info);
}

type_init(tricore_mcan_register_types)
