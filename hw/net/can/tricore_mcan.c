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
FIELD(STATUS, RX_OVERRUN, 2, 1)
FIELD(STATUS, GATEWAY_EVENT, 3, 1)
REG32(TXID, 0x08)
REG32(TXDATAL, 0x0c)
REG32(TXDATAH, 0x10)
REG32(TXCTRL, 0x14)
REG32(RXID, 0x18)
REG32(RXDATAL, 0x1c)
REG32(RXDATAH, 0x20)
REG32(INT_ENABLE, 0x24)
REG32(FIFO_STATUS, 0x28)
REG32(FIFO_CLEAR, 0x2c)
REG32(GATEWAY_TARGET, 0x30)
REG32(GATEWAY_ENABLE, 0x34)
REG32(TX_LEN, 0x38)
REG32(RX_LEN, 0x3c)

/* TC27D MultiCAN+ register locations (iLLD TC27D_UM_V2.2): Node 0 and
 * message object 0.  The compact registers above remain as a QEMU-friendly
 * smoke interface; these aliases let early iLLD code use documented offsets. */
REG32(NODE0_CR, 0x200)
REG32(NODE0_BTR, 0x210)
REG32(NODE1_CR, 0x220)
REG32(NODE2_CR, 0x240)
REG32(NODE3_CR, 0x260)
REG32(MO0_DATAL, 0x910)
REG32(MO0_DATAH, 0x914)
REG32(MO0_AR, 0x918)
REG32(MO0_CTR, 0x91c)
REG32(MO0_AMR, 0x90c)
#define MO_STRIDE 0x20
#define MO_REG(n, off) ((off) + (n) * MO_STRIDE)
#define MCAN_OBJECTS 256

static void tricore_mcan_update_irq(TriCoreMCANState *s)
{
    uint32_t pending = s->regs[R_STATUS / 4] & s->regs[R_INT_ENABLE / 4];
    qemu_set_irq(s->irq[0], pending & R_STATUS_RX_PENDING_MASK);
    qemu_set_irq(s->irq[1], pending & R_STATUS_TX_COMPLETE_MASK);
    qemu_set_irq(s->irq[2], pending & R_STATUS_RX_OVERRUN_MASK);
    qemu_set_irq(s->irq[3], pending & R_STATUS_GATEWAY_EVENT_MASK);
    for (unsigned i = 2; i < 16; i++) {
        qemu_set_irq(s->irq[i], false);
    }
}

static void tricore_mcan_load_rx(TriCoreMCANState *s)
{
    if (!s->rx_fifo_count) {
        s->rx_pending = false;
        s->regs[R_STATUS / 4] &= ~R_STATUS_RX_PENDING_MASK;
        return;
    }
    s->rx_frame = s->rx_fifo[s->rx_fifo_head];
    s->rx_fifo_head = (s->rx_fifo_head + 1) % ARRAY_SIZE(s->rx_fifo);
    s->rx_fifo_count--;
    s->rx_pending = true;
    s->regs[R_RXID / 4] = s->rx_frame.can_id;
    s->regs[R_RXDATAL / 4] = ldl_le_p(&s->rx_frame.data[0]);
    s->regs[R_RXDATAH / 4] = ldl_le_p(&s->rx_frame.data[4]);
    memcpy(s->rx_data, s->rx_frame.data, sizeof(s->rx_data));
    s->rx_len = can_dlc2len(s->rx_frame.can_dlc);
    s->regs[R_RX_LEN / 4] = s->rx_len;
    s->regs[R_STATUS / 4] |= R_STATUS_RX_PENDING_MASK;
}

static bool tricore_mcan_can_receive(CanBusClientState *client)
{
    TriCoreMCANState *s = container_of(client, TriCoreMCANState, bus_client);
    return (s->regs[R_CONTROL / 4] & R_CONTROL_ENABLE_MASK ||
            s->enabled_nodes) && !s->rx_pending;
}

static ssize_t tricore_mcan_receive(CanBusClientState *client,
                                    const qemu_can_frame *frames,
                                    size_t frames_cnt)
{
    TriCoreMCANState *s = container_of(client, TriCoreMCANState, bus_client);

    if (!frames_cnt) {
        return 0;
    }
    if (s->rx_fifo_count == ARRAY_SIZE(s->rx_fifo)) {
        s->regs[R_STATUS / 4] |= R_STATUS_RX_OVERRUN_MASK;
        tricore_mcan_update_irq(s);
        return 0;
    }
    unsigned object = 0;
    while (object < MCAN_OBJECTS && ((frames[0].can_id ^
             s->regs[MO_REG(object, R_MO0_AR) / 4]) &
            ~s->regs[MO_REG(object, R_MO0_AMR) / 4]) != 0) {
        object++;
    }
    if (object == MCAN_OBJECTS) {
        return 0;
    }
    s->rx_fifo[s->rx_fifo_tail] = frames[0];
    s->rx_fifo_tail = (s->rx_fifo_tail + 1) % ARRAY_SIZE(s->rx_fifo);
    s->rx_fifo_count++;
    if (!s->rx_pending) {
        tricore_mcan_load_rx(s);
    }
    s->regs[R_RXID / 4] = s->rx_frame.can_id;
    s->regs[R_RXDATAL / 4] = ldl_le_p(&s->rx_frame.data[0]);
    s->regs[R_RXDATAH / 4] = ldl_le_p(&s->rx_frame.data[4]);
    s->regs[MO_REG(object, R_MO0_AR) / 4] = s->rx_frame.can_id;
    s->regs[MO_REG(object, R_MO0_DATAL) / 4] = s->regs[R_RXDATAL / 4];
    s->regs[MO_REG(object, R_MO0_DATAH) / 4] = s->regs[R_RXDATAH / 4];
    if ((s->regs[R_GATEWAY_ENABLE / 4] & 1) &&
        s->regs[R_GATEWAY_TARGET / 4] < MCAN_OBJECTS) {
        unsigned target = s->regs[R_GATEWAY_TARGET / 4];
        s->regs[MO_REG(target, R_MO0_AR) / 4] = s->rx_frame.can_id;
        s->regs[MO_REG(target, R_MO0_DATAL) / 4] = s->regs[R_RXDATAL / 4];
        s->regs[MO_REG(target, R_MO0_DATAH) / 4] = s->regs[R_RXDATAH / 4];
        s->regs[R_STATUS / 4] |= R_STATUS_GATEWAY_EVENT_MASK;
    }
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
    if (s->regs[R_TXCTRL / 4] & BIT(1)) {
        /* QEMU's CAN transport carries FD/BRS metadata and up to 64 bytes. */
        s->tx_len = MIN(s->regs[R_TX_LEN / 4], 64u);
        frame.can_dlc = can_len2dlc(s->tx_len);
        frame.flags = QEMU_CAN_FRMF_TYPE_FD;
        if (s->regs[R_TXCTRL / 4] & BIT(2)) {
            frame.flags |= QEMU_CAN_FRMF_BRS;
        }
    } else {
        s->tx_len = 8;
    }
    memcpy(frame.data, s->tx_data, sizeof(frame.data));
    stl_le_p(&frame.data[0], s->regs[R_TXDATAL / 4]);
    stl_le_p(&frame.data[4], s->regs[R_TXDATAH / 4]);
    if (s->regs[R_CONTROL / 4] & R_CONTROL_LOOPBACK_MASK) {
        tricore_mcan_receive(&s->bus_client, &frame, 1);
    } else if (s->canbus) {
        if (s->regs[R_TXCTRL / 4] & BIT(3)) {
            /* QEMU-specific timing mode turns the documented BTR fields into
             * a virtual transfer delay; atomic mode remains the default for
             * compatibility with existing CAN devices. */
            uint32_t btr = s->regs[R_NODE0_BTR / 4];
            uint32_t brp = (btr & 0x3f) + 1;
            uint32_t tseg1 = ((btr >> 8) & 0xff) + 1;
            uint32_t tseg2 = ((btr >> 16) & 0x0f) + 1;
            uint32_t bitrate = 100000000u / (brp * (1 + tseg1 + tseg2));
            uint32_t bits = 47 + 8 * can_dlc2len(frame.can_dlc);
            uint64_t delay_ns = bitrate ? ((uint64_t)bits * 1000000000ull) /
                                             bitrate : 0;
            can_bus_client_send_timed(&s->bus_client, &frame, 1, delay_ns);
        } else {
            can_bus_client_send(&s->bus_client, &frame, 1);
        }
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
    if (addr >= 0x80 && addr < 0xc0 && (addr & 3) == 0) {
        return ldl_le_p(&s->rx_data[addr - 0x80]);
    }
    if (addr == R_FIFO_STATUS) {
        return s->rx_fifo_count | (s->rx_pending ? BIT(8) : 0);
    }
    if (addr == R_RXDATAH) {
        uint32_t value = s->regs[index];
        tricore_mcan_load_rx(s);
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
    } else if (addr == R_FIFO_CLEAR && (value & 1)) {
        s->rx_fifo_head = s->rx_fifo_tail = s->rx_fifo_count = 0;
        s->rx_pending = false;
        s->regs[R_STATUS / 4] &= ~R_STATUS_RX_PENDING_MASK;
    } else if (addr >= 0x40 && addr < 0x80 && (addr & 3) == 0) {
        stl_le_p(&s->tx_data[addr - 0x40], value);
    } else if (addr == R_INT_ENABLE || addr == R_CONTROL ||
               addr == R_TXID || addr == R_TXDATAL || addr == R_TXDATAH ||
               addr == R_TX_LEN ||
               addr == R_GATEWAY_TARGET || addr == R_GATEWAY_ENABLE ||
               (addr >= R_MO0_AR && addr < R_MO0_AR + MCAN_OBJECTS * MO_STRIDE &&
                ((addr - R_MO0_AR) % MO_STRIDE) == 0) ||
               (addr >= R_MO0_AMR && addr < R_MO0_AMR + MCAN_OBJECTS * MO_STRIDE &&
                ((addr - R_MO0_AMR) % MO_STRIDE) == 0) ||
               (addr >= R_MO0_DATAL && addr < R_MO0_DATAL + MCAN_OBJECTS * MO_STRIDE &&
                ((addr - R_MO0_DATAL) % MO_STRIDE) == 0) ||
               (addr >= R_MO0_DATAH && addr < R_MO0_DATAH + MCAN_OBJECTS * MO_STRIDE &&
                ((addr - R_MO0_DATAH) % MO_STRIDE) == 0)) {
        s->regs[index] = value;
    } else if (addr == R_TXCTRL && (value & 1)) {
        tricore_mcan_send(s);
    } else if (addr >= R_MO0_CTR && addr < R_MO0_CTR + MCAN_OBJECTS * MO_STRIDE &&
               ((addr - R_MO0_CTR) % MO_STRIDE) == 0 && (value & 1)) {
        unsigned object = (addr - R_MO0_CTR) / MO_STRIDE;
        s->regs[R_TXID / 4] = s->regs[MO_REG(object, R_MO0_AR) / 4];
        s->regs[R_TXDATAL / 4] = s->regs[MO_REG(object, R_MO0_DATAL) / 4];
        s->regs[R_TXDATAH / 4] = s->regs[MO_REG(object, R_MO0_DATAH) / 4];
        tricore_mcan_send(s);
    } else if (addr == R_NODE0_CR || addr == R_NODE1_CR ||
               addr == R_NODE2_CR || addr == R_NODE3_CR) {
        /* INIT/CCE sequencing is intentionally collapsed to the enable bit
         * for this first register-oriented implementation. */
        unsigned node = (addr - R_NODE0_CR) / 0x20;
        s->regs[index] = value;
        if (value) {
            s->enabled_nodes |= 1u << node;
        } else {
            s->enabled_nodes &= ~(1u << node);
        }
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
    for (unsigned i = 0; i < 16; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq[i]);
    }
    memory_region_init_io(&s->iomem, obj, &tricore_mcan_ops, s,
                          TYPE_TRICORE_MCAN, 0x3000);
    /* Reset value accepts every identifier until firmware programs a mask. */
    for (unsigned i = 0; i < 4; i++) {
        s->regs[MO_REG(i, R_MO0_AMR) / 4] = UINT32_MAX;
    }
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}

static const VMStateDescription vmstate_tricore_mcan = {
    .name = TYPE_TRICORE_MCAN,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_BUFFER_UNSAFE_INFO(regs, TriCoreMCANState, 1,
                                   vmstate_info_buffer,
                                   sizeof(((TriCoreMCANState *)0)->regs)),
        VMSTATE_UINT32(rx_frame.can_id, TriCoreMCANState),
        VMSTATE_UINT8(rx_frame.can_dlc, TriCoreMCANState),
        VMSTATE_UINT8(rx_frame.flags, TriCoreMCANState),
        VMSTATE_UINT8_ARRAY(rx_frame.data, TriCoreMCANState, 64),
        VMSTATE_BOOL(rx_pending, TriCoreMCANState),
        VMSTATE_UINT8(enabled_nodes, TriCoreMCANState),
        VMSTATE_BUFFER_UNSAFE_INFO(rx_fifo, TriCoreMCANState, 1,
                                   vmstate_info_buffer,
                                   sizeof(((TriCoreMCANState *)0)->rx_fifo)),
        VMSTATE_UINT8(rx_fifo_head, TriCoreMCANState),
        VMSTATE_UINT8(rx_fifo_tail, TriCoreMCANState),
        VMSTATE_UINT8(rx_fifo_count, TriCoreMCANState),
        VMSTATE_UINT8_ARRAY(tx_data, TriCoreMCANState, 64),
        VMSTATE_UINT8_ARRAY(rx_data, TriCoreMCANState, 64),
        VMSTATE_UINT8(tx_len, TriCoreMCANState),
        VMSTATE_UINT8(rx_len, TriCoreMCANState),
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
