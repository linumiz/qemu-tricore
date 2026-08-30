/*
 * CAN common CAN bus emulation support
 *
 * Copyright (c) 2013-2014 Jin Yang
 * Copyright (c) 2014-2018 Pavel Pisa
 *
 * Initial development supported by Google GSoC 2013 from RTEMS project slot
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include "qemu/osdep.h"
#include "chardev/char.h"
#include "qemu/module.h"
#include "qemu/sockets.h"
#include "qapi/error.h"
#include "net/can_emu.h"
#include "qom/object_interfaces.h"
#include "qemu/timer.h"

/* CAN DLC to real data length conversion helpers */

static const uint8_t dlc2len[] = {
    0, 1, 2, 3, 4, 5, 6, 7,
    8, 12, 16, 20, 24, 32, 48, 64
};

/* get data length from can_dlc with sanitized can_dlc */
uint8_t can_dlc2len(uint8_t can_dlc)
{
    return dlc2len[can_dlc & 0x0F];
}

static const uint8_t len2dlc[] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8,                              /* 0 - 8 */
    9, 9, 9, 9,                                             /* 9 - 12 */
    10, 10, 10, 10,                                         /* 13 - 16 */
    11, 11, 11, 11,                                         /* 17 - 20 */
    12, 12, 12, 12,                                         /* 21 - 24 */
    13, 13, 13, 13, 13, 13, 13, 13,                         /* 25 - 32 */
    14, 14, 14, 14, 14, 14, 14, 14,                         /* 33 - 40 */
    14, 14, 14, 14, 14, 14, 14, 14,                         /* 41 - 48 */
    15, 15, 15, 15, 15, 15, 15, 15,                         /* 49 - 56 */
    15, 15, 15, 15, 15, 15, 15, 15                          /* 57 - 64 */
};

/* map the sanitized data length to an appropriate data length code */
uint8_t can_len2dlc(uint8_t len)
{
    if (unlikely(len > 64)) {
        return 0xF;
    }

    return len2dlc[len];
}

struct CanBusState {
    Object object;

    QTAILQ_HEAD(, CanBusClientState) clients;
    /* Virtual-time queue used by controllers that model frame duration. */
    QEMUTimer *event_timer;
    struct {
        CanBusClientState *sender;
        qemu_can_frame frame;
        uint64_t deadline;
    } events[16];
    unsigned event_head, event_count;
};

static ssize_t can_bus_dispatch(CanBusState *bus, CanBusClientState *client,
                                const qemu_can_frame *frames, size_t frames_cnt)
{
    int ret = 0;
    CanBusClientState *peer;
    QTAILQ_FOREACH(peer, &bus->clients, next) {
        if (peer != client && peer->info->can_receive(peer) &&
            peer->info->receive(peer, frames, frames_cnt) > 0) {
            ret = 1;
        }
    }
    return ret;
}

static void can_bus_event_cb(void *opaque)
{
    CanBusState *bus = opaque;
    if (!bus->event_count) {
        return;
    }
    /* Select the earliest due frame; equal deadlines use CAN-ID priority. */
    uint64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    unsigned selected = 0;
    for (unsigned i = 1; i < bus->event_count; i++) {
        unsigned a = (bus->event_head + selected) % ARRAY_SIZE(bus->events);
        unsigned b = (bus->event_head + i) % ARRAY_SIZE(bus->events);
        bool b_due = bus->events[b].deadline <= now;
        bool a_due = bus->events[a].deadline <= now;
        if ((b_due && !a_due) ||
            (b_due == a_due && bus->events[b].deadline < bus->events[a].deadline) ||
            (b_due == a_due && bus->events[b].deadline == bus->events[a].deadline &&
             bus->events[b].frame.can_id < bus->events[a].frame.can_id)) {
            selected = i;
        }
    }
    unsigned index = (bus->event_head + selected) % ARRAY_SIZE(bus->events);
    uint64_t arbitration_deadline = bus->events[index].deadline;
    for (unsigned i = 0; i < bus->event_count; i++) {
        unsigned loser = (bus->event_head + i) % ARRAY_SIZE(bus->events);
        if (i != selected && bus->events[loser].deadline == arbitration_deadline &&
            bus->events[loser].sender->bit_info &&
            bus->events[loser].sender->bit_info->arbitration_lost) {
            bus->events[loser].sender->bit_info->arbitration_lost(
                bus->events[loser].sender);
        }
    }
    can_bus_dispatch(bus, bus->events[index].sender, &bus->events[index].frame, 1);
    for (unsigned i = selected; i + 1 < bus->event_count; i++) {
        unsigned dst = (bus->event_head + i) % ARRAY_SIZE(bus->events);
        unsigned src = (bus->event_head + i + 1) % ARRAY_SIZE(bus->events);
        bus->events[dst] = bus->events[src];
    }
    bus->event_count--;
    if (bus->event_count) {
        timer_mod_ns(bus->event_timer,
                     bus->events[bus->event_head].deadline);
    }
}

static void can_bus_instance_init(Object *object)
{
    CanBusState *bus = (CanBusState *)object;

    QTAILQ_INIT(&bus->clients);
    bus->event_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, can_bus_event_cb, bus);
}

int can_bus_insert_client(CanBusState *bus, CanBusClientState *client)
{
    client->bus = bus;
    QTAILQ_INSERT_TAIL(&bus->clients, client, next);
    return 0;
}

int can_bus_remove_client(CanBusClientState *client)
{
    CanBusState *bus = client->bus;
    if (bus == NULL) {
        return 0;
    }

    QTAILQ_REMOVE(&bus->clients, client, next);
    client->bus = NULL;
    return 1;
}

ssize_t can_bus_client_send(CanBusClientState *client,
             const struct qemu_can_frame *frames, size_t frames_cnt)
{
    CanBusState *bus = client->bus;
    if (bus == NULL) {
        return -1;
    }

    return can_bus_dispatch(bus, client, frames, frames_cnt);
}

ssize_t can_bus_client_send_timed(CanBusClientState *client,
                                  const qemu_can_frame *frames,
                                  size_t frames_cnt, uint64_t delay_ns)
{
    CanBusState *bus = client->bus;
    if (!bus || !frames_cnt || frames_cnt > 1 ||
        bus->event_count == ARRAY_SIZE(bus->events)) {
        return -1;
    }
    /* The scheduler is deliberately transport-level: controllers provide the
     * delay computed from their nominal/data bit timing, while legacy users
     * continue to use atomic can_bus_client_send(). */
    unsigned slot = (bus->event_head + bus->event_count) % ARRAY_SIZE(bus->events);
    bus->events[slot].sender = client;
    bus->events[slot].frame = frames[0];
    bus->events[slot].deadline = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delay_ns;
    if (!bus->event_count) {
        timer_mod_ns(bus->event_timer, bus->events[slot].deadline);
    }
    bus->event_count++;
    return 0;
}

ssize_t can_bus_client_send_bits(CanBusClientState *client,
                                 const uint8_t *bits, size_t bit_count,
                                 uint64_t bit_time_ns, uint64_t start_time_ns)
{
    /* This path exposes sampled bus levels without changing legacy frames. */
    CanBusState *bus = client->bus;
    if (!bus || !bits || !bit_count || !bit_time_ns) {
        return -1;
    }
    for (size_t i = 0; i < bit_count; i++) {
        CanBusBitSample sample = {
            .level = (bits[i / 8] >> (i % 8)) & 1,
            .timestamp_ns = start_time_ns + i * bit_time_ns,
        };
        CanBusClientState *peer;
        QTAILQ_FOREACH(peer, &bus->clients, next) {
            if (peer != client && peer->bit_info && peer->bit_info->sample) {
                peer->bit_info->sample(peer, &sample);
            }
        }
    }
    return bit_count;
}

bool can_bus_wired_and(const bool *drives, size_t drive_count)
{
    /* CAN's dominant-low electrical rule is represented as boolean AND. */
    bool level = true;
    for (size_t i = 0; i < drive_count; i++) {
        level &= drives[i];
    }
    return level;
}

ssize_t can_bus_arbitrate_bits(const uint8_t *streams, size_t sender_count,
                               size_t stream_stride, size_t bit_count)
{
    /* Remove senders that transmit recessive while another sender is dominant. */
    if (!streams || !sender_count || !stream_stride || !bit_count) {
        return -1;
    }
    bool active[16];
    if (sender_count > ARRAY_SIZE(active)) {
        return -1;
    }
    memset(active, true, sizeof(active));
    size_t remaining = sender_count;
    for (size_t bit = 0; bit < bit_count && remaining > 1; bit++) {
        bool drives[16];
        for (size_t sender = 0; sender < sender_count; sender++) {
            drives[sender] = !active[sender] ||
                ((streams[sender * stream_stride + bit / 8] >> (bit % 8)) & 1);
        }
        bool bus_level = can_bus_wired_and(drives, sender_count);
        for (size_t sender = 0; sender < sender_count; sender++) {
            if (active[sender] && drives[sender] && !bus_level) {
                active[sender] = false;
                remaining--;
            }
        }
    }
    for (size_t sender = 0; sender < sender_count; sender++) {
        if (active[sender]) {
            return sender;
        }
    }
    return -1;
}

ssize_t can_bus_arbitrate_clients(CanBusClientState *const *senders,
                                  const uint8_t *streams, size_t sender_count,
                                  size_t stream_stride, size_t bit_count)
{
    /* Convert the winning bitstream into per-client loss notifications. */
    ssize_t winner = can_bus_arbitrate_bits(streams, sender_count,
                                             stream_stride, bit_count);
    if (winner < 0 || !senders) {
        return winner;
    }
    for (size_t i = 0; i < sender_count; i++) {
        if ((ssize_t)i != winner && senders[i] && senders[i]->bit_info &&
            senders[i]->bit_info->arbitration_lost) {
            senders[i]->bit_info->arbitration_lost(senders[i]);
        }
    }
    return winner;
}

ssize_t can_bus_sample_clients(CanBusClientState *const *senders,
                               size_t sender_count, const uint8_t *streams,
                               size_t stream_stride, size_t bit_count,
                               uint64_t bit_time_ns, uint64_t start_time_ns)
{
    /* Sample all senders at the same virtual instant and broadcast one bus level. */
    if (!senders || !streams || !sender_count || sender_count > 16 ||
        !stream_stride || !bit_count || !bit_time_ns) {
        return -1;
    }
    for (size_t bit = 0; bit < bit_count; bit++) {
        bool drives[16];
        for (size_t i = 0; i < sender_count; i++) {
            drives[i] = (streams[i * stream_stride + bit / 8] >> (bit % 8)) & 1;
        }
        CanBusBitSample sample = {
            .level = can_bus_wired_and(drives, sender_count),
            .timestamp_ns = start_time_ns + bit * bit_time_ns,
        };
        for (size_t i = 0; i < sender_count; i++) {
            if (senders[i]->bit_info && senders[i]->bit_info->sample) {
                senders[i]->bit_info->sample(senders[i], &sample);
            }
        }
    }
    return bit_count;
}

int can_bus_filter_match(struct qemu_can_filter *filter, qemu_canid_t can_id)
{
    int m;
    if (((can_id | filter->can_mask) & QEMU_CAN_ERR_FLAG)) {
        return (filter->can_mask & QEMU_CAN_ERR_FLAG) != 0;
    }
    m = (can_id & filter->can_mask) == (filter->can_id & filter->can_mask);
    return filter->can_id & QEMU_CAN_INV_FILTER ? !m : m;
}

int can_bus_client_set_filters(CanBusClientState *client,
             const struct qemu_can_filter *filters, size_t filters_cnt)
{
    return 0;
}


static bool can_bus_can_be_deleted(UserCreatable *uc)
{
    return false;
}

static void can_bus_class_init(ObjectClass *klass,
                               const void *class_data G_GNUC_UNUSED)
{
    UserCreatableClass *uc_klass = USER_CREATABLE_CLASS(klass);

    uc_klass->can_be_deleted = can_bus_can_be_deleted;
}

static const TypeInfo can_bus_info = {
    .parent = TYPE_OBJECT,
    .name = TYPE_CAN_BUS,
    .instance_size = sizeof(CanBusState),
    .instance_init = can_bus_instance_init,
    .class_init = can_bus_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { TYPE_USER_CREATABLE },
        { }
    }
};

static void can_bus_register_types(void)
{
    type_register_static(&can_bus_info);
}

type_init(can_bus_register_types);
