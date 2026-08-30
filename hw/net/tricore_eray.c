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
#include "hw/core/qdev-properties.h"
#include "qapi/error.h"
#include "migration/vmstate.h"

#define ERAY_CCSV      0x100
#define ERAY_CCEV      0x104
#define ERAY_SUCC1     0x108
#define ERAY_NEMC      0x10c
#define ERAY_MBSC1     0x110
#define ERAY_NDAT1     0x114
#define ERAY_MBSC0     0x0f0
#define ERAY_NDAT0     0x0f4
#define ERAY_CMD       0x118
#define ERAY_CYCLE     0x11c
#define ERAY_SLOTSTAT  0x120
#define ERAY_MBID      0x124
#define ERAY_MBCTRL    0x128
#define ERAY_MBPENDING 0x12c
#define ERAY_SCHED_CFG 0x130
#define ERAY_STATIC_SLOTS 0x134
#define ERAY_DYNAMIC_START 0x138
#define ERAY_MINISLOT 0x13c
#define ERAY_GUARDIAN 0x140
#define ERAY_CHANNEL 0x144
#define ERAY_SUCC2 0x148
#define ERAY_SUCC3 0x14c
#define ERAY_PRTC1 0x150
#define ERAY_PRTC2 0x154
#define ERAY_FSR 0x158
#define ERAY_MRC 0x15c
#define ERAY_FCL 0x160
#define ERAY_FILTER_ID 0x164
#define ERAY_FILTER_CYCLE 0x168
#define ERAY_MHDS 0x16c
#define ERAY_FSR_TAIL 0x170
#define ERAY_FSR_CRIT 0x174
#define ERAY_IRQ0_MASK 0x178
#define ERAY_IRQ1_MASK 0x17c
#define ERAY_GTU_MICROTICKS 0x180
#define ERAY_GTU_MACROTICKS 0x184
#define ERAY_GTU_CYCLE 0x188
#define ERAY_ACTION_STATIC 0x18c
#define ERAY_ACTION_DYNAMIC 0x190

/* Compact public-fixture header flags: cycle bit plus null/sync/startup. */
#define ERAY_HDR_CYCLE_SHIFT 8
#define ERAY_HDR_CYCLE_MASK  (0x3fu << ERAY_HDR_CYCLE_SHIFT)
#define ERAY_HDR_NULL        BIT(0)
#define ERAY_HDR_SYNC        BIT(1)
#define ERAY_HDR_STARTUP     BIT(2)
#define ERAY_HDR_PUBLIC_MASK (ERAY_HDR_CYCLE_MASK | ERAY_HDR_NULL | \
                              ERAY_HDR_SYNC | ERAY_HDR_STARTUP)

/* CCEV bits used by the portable model.  These map to the public error/event
 * classes; keeping them explicit makes invalid host requests observable in
 * QTests without pretending to emulate undocumented PHY diagnostics. */
#define CCEV_ILLEGAL_COMMAND BIT(0)
#define CCEV_HEADER_ERROR    BIT(1)
#define CCEV_CYCLE_START     BIT(2)
#define CCEV_SLOT_ERROR      BIT(3)
#define CCEV_FIFO_OVERRUN    BIT(4)
#define CCEV_FIFO_CRITICAL   BIT(5)
#define CCEV_FIFO_EMPTY      BIT(6)
#define CCEV_UNLOCK_ERROR    BIT(7)

/* Public register field masks (reserved bits read as zero and ignore writes). */
#define ERAY_SUCC1_MASK 0x03ffffffu
#define ERAY_SUCC2_MASK 0x03ffffffu
#define ERAY_SUCC3_MASK 0x03ffffffu
#define ERAY_PRTC_MASK  0x0fffffffu
#define ERAY_CCEV_MASK  0x000000ffu
#define ERAY_CMD_MASK   0x000000ffu
#define ERAY_CYCLE_MASK 0x0000003fu
#define ERAY_SLOTSTAT_MASK 0xe07fffffu
#define SLOTSTAT_FILTER_REJECT BIT(29)
#define ERAY_MBCTRL_MASK (MBCTRL_COMMIT | MBCTRL_UNLOCK | \
                          MBCTRL_CHANNEL_B | MBCTRL_FIFO_POP)
#define ERAY_GTU_MASK   0x0000ffffu

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
#define CMD_WAKEUP     0x06
#define CMD_FREEZE     0x07
#define CMD_WARMSTART  0x08
#define MBCTRL_COMMIT  BIT(0)
#define MBCTRL_UNLOCK  BIT(1)
#define MBCTRL_CHANNEL_B BIT(2)
#define MBCTRL_FIFO_POP BIT(4)
#define ERAY_CYCLE_NS 1000

static QTAILQ_HEAD(, TriCoreERAYState) eray_bus =
    QTAILQ_HEAD_INITIALIZER(eray_bus);

static void eray_update_irq(TriCoreERAYState *s);
static void eray_schedule(TriCoreERAYState *s);
static void eray_deliver_frame(TriCoreERAYState *s, uint32_t frame_id,
                               const uint8_t *frame);

static bool eray_dynamic_collision(TriCoreERAYState *s, uint32_t frame_id,
                                   uint32_t due_cycle, uint32_t due_slot)
{
    TriCoreERAYState *peer;

    /* Dynamic slots use deterministic collision avoidance in the portable
     * bus.  If two nodes request the same cycle/minislot, the lower public
     * frame ID wins and the loser gets a slot-error event. */
    QTAILQ_FOREACH(peer, &eray_bus, bus_node) {
        /* Only active FlexRay nodes participate in dynamic arbitration;
         * a stopped migration destination must not collide with the source. */
        if (peer == s || peer->ccsv != POC_NORMAL_ACTIVE ||
            !(peer->sched_cfg & 1) || !peer->tx_pending ||
            peer->tx_due_cycle != due_cycle || peer->tx_due_slot != due_slot) {
            continue;
        }
        if (peer->tx_frame_id <= frame_id) {
            return false;
        }
        peer->tx_pending = false;
        peer->shadow_busy = 0;
        peer->ccev |= CCEV_SLOT_ERROR;
        eray_update_irq(peer);
    }
    return true;
}

static void eray_scheduler_cb(void *opaque)
{
    TriCoreERAYState *s = opaque;
    if (s->ccsv == POC_NORMAL_ACTIVE) {
        uint32_t dynamic_slots = s->dynamic_start ? (256 - s->dynamic_start) : 256;
        uint32_t total_slots = MAX(1u, MIN(2048u, s->static_slots + dynamic_slots));
        s->slot_counter = (s->slot_counter + 1) % total_slots;
        if (s->slot_counter >= s->static_slots) {
            /* Dynamic slots advance through minislots; this counter is kept
             * in SaveVM so a migrated scheduler resumes at the same point. */
            s->minislot_counter = (s->minislot_counter + 1) & 0xff;
        } else {
            s->minislot_counter = 0;
        }
        s->cycle = (s->cycle + 1) % MAX(1, s->cycle_length);
        s->slot_status = (s->slot_status & (BIT(31) | SLOTSTAT_FILTER_REJECT)) |
                         (s->cycle << 16) | (s->slot_counter & 0x7ff);
        if (s->slot_counter >= s->static_slots) {
            /* The dynamic action point is a deterministic phase offset in
             * the public model; expose it with the current minislot marker. */
            uint32_t action = s->action_point_dynamic & 0xff;
            s->slot_status |= BIT(30) |
                              (((s->minislot_counter + action) & 0xff) << 8);
        }
        s->ccev |= CCEV_CYCLE_START;
        if (s->tx_pending && s->cycle == s->tx_due_cycle) {
            /* Static frames are virtual-time events at their numbered slot,
             * not merely at cycle rollover.  If the cycle matched before the
             * slot, defer to the next cycle; this keeps every static slot
             * observable while retaining deterministic qtest timing. */
            if (s->tx_frame_id <= s->static_slots &&
                s->slot_counter != s->tx_due_slot) {
                s->tx_due_cycle = (s->cycle + 1) % MAX(1u, s->cycle_length);
                eray_update_irq(s);
                eray_schedule(s);
                return;
            }
            s->slot_status = (s->slot_status & 0xffff0000) |
                             (s->tx_due_slot & 0x7ff);
            eray_deliver_frame(s, s->tx_frame_id, s->tx_frame);
            s->tx_pending = false;
        }
        eray_update_irq(s);
        eray_schedule(s);
    }
}

static void eray_schedule(TriCoreERAYState *s)
{
    timer_mod(s->scheduler, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
              (s->sched_period_ns ? s->sched_period_ns : ERAY_CYCLE_NS));
}

static void eray_update_irq(TriCoreERAYState *s)
{
    qemu_set_irq(s->int0_irq, !!((s->ccev & 0xffff) & ~s->irq0_mask));
    qemu_set_irq(s->int1_irq, !!((s->mbsc0 | s->mbsc1 | s->ndat0 | s->ndat1) &
                                 ~s->irq1_mask));
}

static void eray_command(TriCoreERAYState *s, uint32_t cmd)
{
    s->command = cmd;
    switch (cmd & 0xff) {
    case CMD_CONFIG:
        if (s->ccsv != POC_CONFIG && s->ccsv != POC_HALT) goto invalid;
        s->ccsv = POC_CONFIG;
        break;
    case CMD_READY:
        if (s->ccsv != POC_CONFIG) goto invalid;
        s->ccsv = POC_READY;
        break;
    case CMD_COLDSTART:
        if (s->ccsv != POC_READY && s->ccsv != POC_CONFIG) goto invalid;
        s->ccsv = POC_NORMAL_ACTIVE; s->cycle = 0; eray_schedule(s);
        break;
    case CMD_RUN:
        if (s->ccsv != POC_READY && s->ccsv != POC_HALT) goto invalid;
        s->ccsv = POC_NORMAL_ACTIVE; eray_schedule(s);
        break;
    case CMD_HALT:
        if (s->ccsv != POC_NORMAL_ACTIVE) goto invalid;
        s->ccsv = POC_HALT; timer_del(s->scheduler);
        break;
    case CMD_WAKEUP: s->ccsv = POC_READY; break;
    case CMD_FREEZE: s->ccsv = POC_HALT; timer_del(s->scheduler); break;
    case CMD_WARMSTART:
        if (s->ccsv != POC_READY && s->ccsv != POC_HALT) goto invalid;
        s->ccsv = POC_NORMAL_ACTIVE;
        eray_schedule(s);
        break;
    default: goto invalid;
    }
    eray_update_irq(s);
    return;
invalid:
    /* Public ERAY command handling reports an illegal transition in CCEV. */
    s->ccev |= BIT(0);
    eray_update_irq(s);
}

static void eray_commit_message(TriCoreERAYState *s)
{
    unsigned channel = !!(s->mbctrl & MBCTRL_CHANNEL_B);
    uint32_t offset = (s->mbid & 0xff) * 64;
    uint32_t frame_id = ldl_le_p(&s->msg_data[offset]);
    if (s->unlock_key_ch[channel] != 0xa5) {
        /* The public message-handler sequence requires an unlock write before
         * a commit; expose legacy/direct commits as a protocol error. */
        s->ccev |= CCEV_UNLOCK_ERROR;
        return;
    }
    s->unlock_key_ch[channel] = 0;
    s->unlock_key = 0;
    s->host_busy_ch[channel] = 1;
    s->host_busy = 1;
    uint32_t payload_len = ldl_le_p(&s->msg_data[offset + 4]) & 0x7f;
    uint32_t encoded_payload_len = payload_len;
    uint32_t header_flags = ldl_le_p(&s->msg_data[offset + 8]);
    uint32_t header_cycle = (header_flags & ERAY_HDR_CYCLE_MASK) >>
                            ERAY_HDR_CYCLE_SHIFT;
    bool null_frame = header_flags & ERAY_HDR_NULL;
    bool sync_frame = header_flags & ERAY_HDR_SYNC;
    bool startup_frame = header_flags & ERAY_HDR_STARTUP;
    if (!payload_len) {
        payload_len = 64;
    }
    /* The emulated message RAM intentionally uses a compact public fixture
     * header: word 0 is the 11-bit frame ID, word 1 the payload length, and
     * word 2 carries null/sync/startup flags.  Reject IDs outside the public
     * FlexRay range and lengths that cannot fit this 64-byte model. */
    /* FlexRay header indicators have protocol-level relationships: a null
     * frame carries no payload and cannot be sync/startup, while startup
     * frames are necessarily sync frames.  A non-zero cycle field is an
     * explicit cycle selector and must match the controller's current cycle.
     * These checks are derived from the public ERAY manuals/iLLD headers and
     * keep malformed host fixtures from entering the virtual bus. */
    if ((frame_id & ~0x7ffu) || encoded_payload_len > s->payload_max ||
        encoded_payload_len > sizeof(s->tx_frame) ||
        (header_flags & ~ERAY_HDR_PUBLIC_MASK) ||
        (null_frame && (encoded_payload_len != 0 || sync_frame ||
                        startup_frame)) ||
        (startup_frame && !sync_frame) ||
        (header_cycle && header_cycle != (s->cycle & 0x3f))) {
        s->ccev |= CCEV_HEADER_ERROR;
        s->host_busy_ch[channel] = 0;
        s->host_busy = 0;
        eray_update_irq(s);
        return;
    }
    if (!(s->mbctrl & MBCTRL_COMMIT) || offset + 64 > s->msg_ram_size) {
        s->ccev |= CCEV_HEADER_ERROR;
        return;
    }
    /* The portable bus models a static-slot transfer at the programmed cycle.
     * A/B selection is retained in slot_status while both channels share the
     * same deterministic virtual timebase. */
    uint32_t effective_cycle = header_cycle ? header_cycle : (s->cycle & 0x3f);
    s->slot_status = (frame_id & 0x7ff) | (effective_cycle << 16) |
                     ((payload_len & 0x7f) << 8) |
                     ((s->mbctrl & MBCTRL_CHANNEL_B) ? BIT(31) : 0);
    s->tx_header_flags = header_flags;
    s->slot_status |= header_flags & ERAY_HDR_PUBLIC_MASK;
    if (s->guardian || !(s->channel_mask & ((s->mbctrl & MBCTRL_CHANNEL_B) ? 2 : 1))) {
        s->ccev |= CCEV_SLOT_ERROR; /* bus guardian/channel violation */
        s->host_busy_ch[channel] = 0;
        s->host_busy = 0;
        eray_update_irq(s);
        return;
    }
    if (s->static_slots && frame_id <= s->static_slots) {
        /* Static slots are valid only in the configured static prefix. */
        s->tx_due_slot = frame_id;
    } else if (frame_id >= s->dynamic_start) {
        /* Map each dynamic frame deterministically into the configured
         * minislot range; peers therefore compare the same arbitration key. */
        uint32_t range = MAX(1u, 256u - s->dynamic_start);
        s->tx_due_slot = s->dynamic_start +
                         ((frame_id - s->dynamic_start) % range);
    } else {
        s->ccev |= CCEV_SLOT_ERROR;
        s->host_busy_ch[channel] = 0;
        s->host_busy = 0;
        eray_update_irq(s);
        return;
    }
    /* Static slots are sent in the configured prefix; remaining frame IDs
     * use the dynamic segment and are represented by the minislot marker. */
    if (frame_id >= s->dynamic_start) {
        uint32_t action = s->action_point_dynamic & 0xff;
        s->slot_status |= BIT(30) |
                          (((s->minislot + action) & 0xff) << 8);
    }
    if (s->sched_cfg & 1) {
        /* With scheduling enabled, commit is a host/shadow-buffer request;
         * transmission occurs at the next configured virtual slot. */
        if (frame_id <= s->static_slots &&
            s->slot_counter >= s->tx_due_slot) {
            /* A host request arriving after its static slot is not silently
             * sent in a later slot; report the public slot error instead. */
            s->ccev |= CCEV_SLOT_ERROR;
            s->host_busy_ch[channel] = 0;
            s->host_busy = 0;
            eray_update_irq(s);
            return;
        }
        uint32_t due_cycle = (s->cycle + 1) % MAX(1u, s->cycle_length);
        if (frame_id >= s->dynamic_start &&
            !eray_dynamic_collision(s, frame_id, due_cycle, s->tx_due_slot)) {
            s->ccev |= CCEV_SLOT_ERROR;
            s->host_busy = 0;
            eray_update_irq(s);
            return;
        }
        memcpy(s->tx_frame, &s->msg_data[offset], sizeof(s->tx_frame));
        s->tx_frame_id = frame_id;
        s->tx_due_cycle = due_cycle;
        s->tx_payload_len = payload_len;
        s->tx_pending = true;
        s->mbsc1 |= BIT(s->mbid & 31);
        s->shadow_busy_ch[channel] = 1;
        s->shadow_busy = 1;
        eray_update_irq(s);
        return;
    }
    eray_deliver_frame(s, frame_id, &s->msg_data[offset]);
}

static void eray_deliver_frame(TriCoreERAYState *s, uint32_t frame_id,
                               const uint8_t *frame)
{
    unsigned channel = !!(s->mbctrl & MBCTRL_CHANNEL_B);
    TriCoreERAYState *peer;
    /* Static and dynamic segments share the same portable representation;
     * the slot marker identifies the segment while delivery remains ordered. */
    QTAILQ_FOREACH(peer, &eray_bus, bus_node) {
        if (peer == s || peer->ccsv != POC_NORMAL_ACTIVE) continue;
        /* The public ERAY channel-selection bits gate reception independently
         * on A and B; do not deliver a frame to a disconnected channel. */
        if (!(peer->channel_mask & ((s->mbctrl & MBCTRL_CHANNEL_B) ? 2 : 1))) {
            continue;
        }
        if (peer->slot_filter && peer->slot_filter != (frame_id & 0x7ff)) {
            peer->slot_status |= SLOTSTAT_FILTER_REJECT;
            eray_update_irq(peer);
            continue;
        }
        if (peer->cycle_filter && peer->cycle_filter != (s->cycle & 0x3f)) {
            peer->slot_status |= SLOTSTAT_FILTER_REJECT;
            eray_update_irq(peer);
            continue;
        }
        if (peer->last_rx_id == (frame_id & 0x7ff) &&
            peer->last_rx_cycle == (s->cycle & 0x3f)) {
            /* A/B copies of one FlexRay frame are one logical reception. */
            continue;
        }
        uint32_t peer_off = (s->mbid & 0xff) * 64;
        /* iLLD configures a contiguous receive FIFO by assigning message
         * buffers from FFB through FCL.  Keep a deterministic head/count in
         * FSR while retaining the normal NDAT/MBSC indication. */
        if (peer->fifo_depth) {
            uint32_t count = (peer->fifo_status >> 8) & 0xff;
            uint32_t head = peer->fifo_status & 0xff;
            if (count < peer->fifo_depth) {
                uint32_t fifo_off = ((peer->fifo_start + head) & 0xff) * 64;
                memcpy(&peer->msg_data[fifo_off], frame, 64);
                head = (head + 1) % peer->fifo_depth;
                count++;
                peer->fifo_status = head | (count << 8);
                peer->fifo_tail = (peer->fifo_start + count - 1) & 0xff;
                if (peer->fifo_critical && count >= peer->fifo_critical) {
                    peer->ccev |= BIT(5); /* FIFO critical level */
                }
            } else {
                peer->ccev |= BIT(4); /* receive FIFO overrun */
            }
        }
        memcpy(&peer->msg_data[peer_off], frame, 64);
        if (s->mbctrl & MBCTRL_CHANNEL_B) {
            peer->ndat1 |= BIT(s->mbid & 31);
            peer->mbsc1 |= BIT(s->mbid & 31);
        } else {
            peer->ndat0 |= BIT(s->mbid & 31);
            peer->mbsc0 |= BIT(s->mbid & 31);
        }
        peer->last_rx_id = frame_id & 0x7ff;
        peer->last_rx_cycle = s->cycle & 0x3f;
        peer->last_rx_channel = (s->mbctrl & MBCTRL_CHANNEL_B) ? 1 : 0;
        peer->slot_status = s->slot_status;
        eray_update_irq(peer);
    }
    if (s->mbctrl & MBCTRL_CHANNEL_B) {
        s->mbsc1 |= BIT(s->mbid & 31);
    } else {
        s->mbsc0 |= BIT(s->mbid & 31);
    }
    s->host_busy_ch[channel] = 0;
    s->shadow_busy_ch[channel] = 0;
    s->host_busy = 0;
    s->shadow_busy = 0;
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
    case ERAY_MBSC0: return s->mbsc0;
    case ERAY_NDAT0: return s->ndat0;
    case ERAY_CMD: return s->command;
    case ERAY_CYCLE: return s->cycle;
    case ERAY_SLOTSTAT: return s->slot_status;
    case ERAY_MBID: return s->mbid;
    case ERAY_MBCTRL: return s->mbctrl;
    case ERAY_MBPENDING: return s->mbsc0 | s->mbsc1 | s->ndat0 | s->ndat1;
    case ERAY_SCHED_CFG: return s->sched_cfg | ((s->sched_period_ns / 1000) << 8);
    case ERAY_STATIC_SLOTS: return s->static_slots;
    case ERAY_DYNAMIC_START: return s->dynamic_start;
    case ERAY_MINISLOT: return s->minislot;
    case ERAY_GUARDIAN: return s->guardian;
    case ERAY_CHANNEL: return s->channel_mask;
    case ERAY_SUCC2: return s->succ2;
    case ERAY_SUCC3: return s->succ3;
    case ERAY_PRTC1: return s->prtc1;
    case ERAY_PRTC2: return s->prtc2;
    case ERAY_FSR: return s->fifo_status;
    case ERAY_MRC: return s->fifo_start;
    case ERAY_FCL: return s->fifo_depth;
    case ERAY_FILTER_ID: return s->slot_filter;
    case ERAY_FILTER_CYCLE: return s->cycle_filter;
    case ERAY_MHDS: return s->host_busy | (s->shadow_busy << 1) |
                            ((s->unlock_key & 0xff) << 8) |
                            (s->host_busy_ch[1] << 16) |
                            (s->shadow_busy_ch[1] << 17) |
                            ((s->unlock_key_ch[1] & 0xff) << 24);
    case ERAY_FSR_TAIL: return s->fifo_tail;
    case ERAY_FSR_CRIT: return s->fifo_critical;
    case ERAY_IRQ0_MASK: return s->irq0_mask;
    case ERAY_IRQ1_MASK: return s->irq1_mask;
    case ERAY_GTU_MICROTICKS: return s->gtu_microticks;
    case ERAY_GTU_MACROTICKS: return s->gtu_macroticks;
    case ERAY_GTU_CYCLE: return s->cycle_length;
    case ERAY_ACTION_STATIC: return s->action_point_static;
    case ERAY_ACTION_DYNAMIC: return s->action_point_dynamic;
    default: return 0;
    }
}

static void eray_write(void *opaque, hwaddr off, uint64_t value,
                       unsigned size)
{
    TriCoreERAYState *s = opaque;
    switch (off) {
    /* Public ERAY status/event registers use write-one-to-clear (W1C). */
    case ERAY_CCEV: s->ccev &= ~(value & ERAY_CCEV_MASK); break;
    case ERAY_SUCC1: s->succ1 = value & ERAY_SUCC1_MASK; break;
    case ERAY_NEMC: s->nemc = value & 0x00ffffffu; break;
    case ERAY_MBSC1: s->mbsc1 &= ~value; break; /* W1C */
    case ERAY_NDAT1: s->ndat1 &= ~value; break; /* W1C */
    case ERAY_MBSC0: s->mbsc0 &= ~value; break; /* W1C */
    case ERAY_NDAT0: s->ndat0 &= ~value; break; /* W1C */
    case ERAY_CMD: eray_command(s, value & ERAY_CMD_MASK); break;
    case ERAY_CYCLE: s->cycle = (value & ERAY_CYCLE_MASK) %
                                  MAX(1u, s->cycle_length); break;
    case ERAY_SLOTSTAT: s->slot_status = value & ERAY_SLOTSTAT_MASK; break;
    case ERAY_MBID: s->mbid = value & 0xff; break;
    case ERAY_MBCTRL:
        s->mbctrl = value & ERAY_MBCTRL_MASK;
        if (value & MBCTRL_UNLOCK) {
            s->unlock_key = 0xa5;
            s->unlock_key_ch[(value & MBCTRL_CHANNEL_B) != 0] = 0xa5;
        }
        if (value & MBCTRL_COMMIT) {
            eray_commit_message(s);
        }
        if (value & MBCTRL_FIFO_POP) {
            uint32_t count = (s->fifo_status >> 8) & 0xff;
            if (!count) {
                s->ccev |= CCEV_FIFO_EMPTY;
            } else {
                count--;
                s->fifo_status = (s->fifo_status & 0xff) | (count << 8);
                s->fifo_tail = (s->fifo_tail + 1) & 0xff;
            }
        }
        break;
    case ERAY_STATIC_SLOTS: s->static_slots = value & 0x7ff; break;
    case ERAY_DYNAMIC_START: s->dynamic_start = value & 0x7ff; break;
    case ERAY_MINISLOT: s->minislot = value & 0xff; break;
    case ERAY_GUARDIAN: s->guardian = value & 1; break;
    case ERAY_CHANNEL: s->channel_mask = value & 3; break;
    case ERAY_SCHED_CFG:
        s->sched_cfg = value & 1;
        s->sched_period_ns = ((uint64_t)((value >> 8) & 0xffff)) * 1000;
        if (!s->sched_period_ns) {
            s->sched_period_ns = ERAY_CYCLE_NS;
        }
        break;
    case ERAY_SUCC2: s->succ2 = value & ERAY_SUCC2_MASK; break;
    case ERAY_SUCC3: s->succ3 = value & ERAY_SUCC3_MASK; break;
    case ERAY_PRTC1: s->prtc1 = value & ERAY_PRTC_MASK; break;
    case ERAY_PRTC2: s->prtc2 = value & ERAY_PRTC_MASK; break;
    case ERAY_MRC: s->fifo_start = value & 0xff; break;
    case ERAY_FCL: s->fifo_depth = value & 0xff; break;
    case ERAY_FSR: s->fifo_status &= ~value; break; /* status W1C */
    case ERAY_MHDS:
        s->host_busy = s->shadow_busy = 0;
        s->host_busy_ch[0] = s->host_busy_ch[1] = 0;
        s->shadow_busy_ch[0] = s->shadow_busy_ch[1] = 0;
        break;
    case ERAY_FSR_TAIL: s->fifo_tail = value & 0xff; break;
    case ERAY_FSR_CRIT: s->fifo_critical = value & 0xff; break;
    case ERAY_IRQ0_MASK: s->irq0_mask = value & 0xffff; break;
    case ERAY_IRQ1_MASK: s->irq1_mask = value; break;
    case ERAY_GTU_MICROTICKS:
        s->gtu_microticks = value & ERAY_GTU_MASK;
        s->sched_period_ns = (uint64_t)MAX(1u, s->gtu_microticks) *
                             MAX(1u, s->gtu_macroticks) * 1000;
        break;
    case ERAY_GTU_MACROTICKS:
        s->gtu_macroticks = value & ERAY_GTU_MASK;
        s->sched_period_ns = (uint64_t)MAX(1u, s->gtu_microticks) *
                             MAX(1u, s->gtu_macroticks) * 1000;
        break;
    case ERAY_GTU_CYCLE:
        s->cycle_length = MIN(64u, MAX(1u, value & 0x3f));
        s->cycle %= s->cycle_length;
        break;
    case ERAY_ACTION_STATIC: s->action_point_static = value & 0xffff; break;
    case ERAY_ACTION_DYNAMIC: s->action_point_dynamic = value & 0xffff; break;
    case ERAY_FILTER_ID: s->slot_filter = value & 0x7ff; break;
    case ERAY_FILTER_CYCLE: s->cycle_filter = value & 0x3f; break;
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
    s->ccev = s->succ1 = s->succ2 = s->succ3 = s->nemc = 0;
    s->prtc1 = s->prtc2 = 0;
    s->gtu_microticks = 1;
    s->gtu_macroticks = 1;
    s->cycle_length = 64;
    s->action_point_static = s->action_point_dynamic = 0;
    s->mbsc0 = s->mbsc1 = s->ndat0 = s->ndat1 = 0;
    s->command = s->cycle = s->slot_status = 0;
    s->mbid = s->mbctrl = 0;
    s->static_slots = 64; s->dynamic_start = 65; s->minislot = 1;
    s->guardian = 0; s->channel_mask = 3;
    s->fifo_start = s->fifo_depth = s->fifo_status = 0;
    s->fifo_tail = s->fifo_critical = 0;
    s->host_busy = s->shadow_busy = s->unlock_key = 0;
    memset(s->host_busy_ch, 0, sizeof(s->host_busy_ch));
    memset(s->shadow_busy_ch, 0, sizeof(s->shadow_busy_ch));
    memset(s->unlock_key_ch, 0, sizeof(s->unlock_key_ch));
    s->irq0_mask = s->irq1_mask = 0;
    s->slot_filter = s->cycle_filter = 0;
    s->last_rx_id = s->last_rx_cycle = 0;
    s->last_rx_channel = 0;
    s->sched_cfg = s->tx_frame_id = s->tx_due_cycle = s->tx_due_slot = 0;
    s->slot_counter = s->minislot_counter = 0;
    s->tx_header_flags = 0;
    s->sched_period_ns = ERAY_CYCLE_NS;
    s->tx_pending = false;
    s->tx_payload_len = 0;
    memset(s->tx_frame, 0, sizeof(s->tx_frame));
    timer_del(s->scheduler);
    memset(s->msg_data, 0, sizeof(s->msg_data));
    eray_update_irq(s);
}

static void eray_init(Object *obj)
{
    TriCoreERAYState *s = TRICORE_ERAY(obj);
    s->payload_max = 64;
    s->msg_ram_size = sizeof(s->msg_data);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->msg_ram);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->int0_irq);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->int1_irq);
}

static void eray_realize(DeviceState *dev, Error **errp)
{
    TriCoreERAYState *s = TRICORE_ERAY(dev);
    s->msg_ram_size = MIN((uint32_t)sizeof(s->msg_data),
                          MAX(64u, s->msg_ram_size));
    s->payload_max = MIN((uint32_t)sizeof(s->tx_frame),
                         MAX(1u, s->payload_max));
    g_autofree char *ram_name = g_strdup_printf("tricore-eray-msg-ram-%p", s);
    memory_region_init_io(&s->iomem, OBJECT(dev), &eray_ops, s,
                          "tricore-eray", 0x1000);
    /* Use the state-owned buffer as RAM backing so firmware writes are visible
     * to the message handler and the same bytes are included in migration. */
    memory_region_init_ram_ptr(&s->msg_ram, OBJECT(dev), ram_name,
                               s->msg_ram_size, s->msg_data);
    s->scheduler = timer_new_ns(QEMU_CLOCK_VIRTUAL, eray_scheduler_cb, s);
    QTAILQ_INSERT_TAIL(&eray_bus, s, bus_node);
}

static const Property eray_properties[] = {
    DEFINE_PROP_UINT32("payload-max", TriCoreERAYState, payload_max, 64),
    DEFINE_PROP_UINT32("message-ram-size", TriCoreERAYState, msg_ram_size,
                       sizeof(((TriCoreERAYState *)0)->msg_data)),
};

static void eray_unrealize(DeviceState *dev)
{
    TriCoreERAYState *s = TRICORE_ERAY(dev);

    /* A QTest instance may create and destroy several machines in one
     * process.  Remove the node from the portable in-process bus on teardown
     * so a later machine cannot receive frames through a stale peer pointer. */
    QTAILQ_REMOVE(&eray_bus, s, bus_node);
    timer_free(s->scheduler);
    s->scheduler = NULL;
}

static const VMStateDescription vmstate_eray = {
    .name = TYPE_TRICORE_ERAY, .version_id = 1, .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(ccsv, TriCoreERAYState), VMSTATE_UINT32(ccev, TriCoreERAYState),
        VMSTATE_UINT32(succ1, TriCoreERAYState), VMSTATE_UINT32(succ2, TriCoreERAYState),
        VMSTATE_UINT32(succ3, TriCoreERAYState), VMSTATE_UINT32(nemc, TriCoreERAYState),
        VMSTATE_UINT32(prtc1, TriCoreERAYState), VMSTATE_UINT32(prtc2, TriCoreERAYState),
        VMSTATE_UINT32(gtu_microticks, TriCoreERAYState),
        VMSTATE_UINT32(gtu_macroticks, TriCoreERAYState),
        VMSTATE_UINT32(cycle_length, TriCoreERAYState),
        VMSTATE_UINT32(action_point_static, TriCoreERAYState),
        VMSTATE_UINT32(action_point_dynamic, TriCoreERAYState),
        VMSTATE_UINT32(mbsc0, TriCoreERAYState), VMSTATE_UINT32(mbsc1, TriCoreERAYState),
        VMSTATE_UINT32(ndat0, TriCoreERAYState), VMSTATE_UINT32(ndat1, TriCoreERAYState),
        VMSTATE_UINT32(command, TriCoreERAYState), VMSTATE_UINT32(cycle, TriCoreERAYState),
        VMSTATE_UINT32(slot_status, TriCoreERAYState), VMSTATE_UINT32(mbid, TriCoreERAYState),
        VMSTATE_UINT32(mbctrl, TriCoreERAYState), VMSTATE_TIMER_PTR(scheduler, TriCoreERAYState),
        VMSTATE_UINT32(payload_max, TriCoreERAYState),
        VMSTATE_UINT32(msg_ram_size, TriCoreERAYState),
        VMSTATE_UINT32(static_slots, TriCoreERAYState), VMSTATE_UINT32(dynamic_start, TriCoreERAYState),
        VMSTATE_UINT32(minislot, TriCoreERAYState), VMSTATE_UINT32(guardian, TriCoreERAYState),
        VMSTATE_UINT32(channel_mask, TriCoreERAYState),
        VMSTATE_UINT32(fifo_start, TriCoreERAYState), VMSTATE_UINT32(fifo_depth, TriCoreERAYState),
        VMSTATE_UINT32(fifo_status, TriCoreERAYState),
        VMSTATE_UINT32(fifo_tail, TriCoreERAYState), VMSTATE_UINT32(fifo_critical, TriCoreERAYState),
        VMSTATE_UINT32(host_busy, TriCoreERAYState), VMSTATE_UINT32(shadow_busy, TriCoreERAYState),
        VMSTATE_UINT32(unlock_key, TriCoreERAYState),
        /* Channel-A/B ownership and pending indicators are migrated as one
         * atomic group so a destination cannot observe a half-restored bus. */
        VMSTATE_UINT32_ARRAY(host_busy_ch, TriCoreERAYState, 2),
        VMSTATE_UINT32_ARRAY(shadow_busy_ch, TriCoreERAYState, 2),
        VMSTATE_UINT32_ARRAY(unlock_key_ch, TriCoreERAYState, 2),
        VMSTATE_UINT32(irq0_mask, TriCoreERAYState), VMSTATE_UINT32(irq1_mask, TriCoreERAYState),
        VMSTATE_UINT32(slot_filter, TriCoreERAYState),
        VMSTATE_UINT32(cycle_filter, TriCoreERAYState),
        VMSTATE_UINT32(last_rx_id, TriCoreERAYState),
        VMSTATE_UINT32(last_rx_cycle, TriCoreERAYState),
        VMSTATE_UINT8(last_rx_channel, TriCoreERAYState),
        VMSTATE_UINT32(sched_cfg, TriCoreERAYState),
        VMSTATE_UINT64(sched_period_ns, TriCoreERAYState),
        VMSTATE_UINT32(slot_counter, TriCoreERAYState),
        VMSTATE_UINT32(minislot_counter, TriCoreERAYState),
        VMSTATE_UINT32(tx_frame_id, TriCoreERAYState),
        VMSTATE_UINT32(tx_due_cycle, TriCoreERAYState),
        VMSTATE_UINT32(tx_due_slot, TriCoreERAYState),
        VMSTATE_UINT32(tx_payload_len, TriCoreERAYState),
        VMSTATE_UINT32(tx_header_flags, TriCoreERAYState),
        VMSTATE_BOOL(tx_pending, TriCoreERAYState),
        VMSTATE_UINT8_ARRAY(tx_frame, TriCoreERAYState, 64),
        VMSTATE_UINT8_ARRAY(msg_data, TriCoreERAYState, sizeof(((TriCoreERAYState *)0)->msg_data)),
        VMSTATE_END_OF_LIST()
    }
};

static void eray_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = eray_realize;
    dc->unrealize = eray_unrealize;
    device_class_set_legacy_reset(dc, eray_reset);
    dc->vmsd = &vmstate_eray;
    device_class_set_props(dc, eray_properties);
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
