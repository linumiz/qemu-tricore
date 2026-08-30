/*
 *  QEMU model of the TriCore ASCLIN UART controller.
 *
 *  Copyright (c) 2017 David Brenken
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "trace.h"
#include "exec/cpu-common.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/core/registerfields.h"
#include "chardev/char-fe.h"
#include "chardev/char-serial.h"
#include "qemu/error-report.h"
#include "migration/vmstate.h"
#include "hw/char/tricore_asclin.h"
#include "hw/core/qdev-properties-system.h"

#include <inttypes.h>

static GPtrArray *asclin_lin_bus;

#define ASCLIN_LIN_GATEWAY_BYTE 0xf1
#define ASCLIN_LIN_GATEWAY_END  0xf2

/*
 * TC3x register offsets are 0x00..0x50, one per 4-byte slot.
 * Enum indices below are offset/4 for TC3x. TC4x offsets start at 0x100
 * and are handled by explicit case labels; state is shared via these
 * enum slots (same bit layout).
 */
enum {
    CLC = 0,
    IOCR,
    ID,
    TXFIFOCON,
    RXFIFOCON,
    BITCON,
    FRAMECON,
    DATCON,
    BRG,
    BRD,
    LINCON,
    LINBTIMER,
    LINHTIMER,
    FLAGS,
    FLAGSSET,
    FLAGSCLEAR,
    FLAGSENABLE,
    TXDATA,
    RXDATA,
    CSR,
    RXDATAD,
    BLOCK_TXDATA_LEN = 24,   /* 0x60/4: custom QEMU xfer length */
    BLOCK_TXDATA_BUF = 25,   /* 0x64/4: custom QEMU xfer buf phys addr */
};

static void asclin_uart_update_parameters(TriCoreASCLINState *s);

/* TC4x register offsets (stride differs completely from TC3x) */
#define TC4X_IOCR          0x100
#define TC4X_TXFIFOCON     0x104
#define TC4X_RXFIFOCON     0x108
#define TC4X_BITCON        0x10C
#define TC4X_FRAMECON      0x110
#define TC4X_DATCON        0x114
#define TC4X_BRG           0x118
#define TC4X_BRD           0x11C
#define TC4X_LINCON        0x120
#define TC4X_LINBTIMER     0x124
#define TC4X_LINHTIMER     0x128
#define TC4X_FLAGS         0x12C
#define TC4X_FLAGSSET      0x130
#define TC4X_FLAGSCLEAR    0x134
#define TC4X_FLAGSENABLE   0x138
#define TC4X_CSR           0x13C
#define TC4X_TXDATA_BASE   0x140   /* TXDATA0..TXDATA7 mirrors 0x140..0x15C */
#define TC4X_TXDATA_LAST   0x15C
#define TC4X_RXDATA_BASE   0x160   /* RXDATA0..RXDATA7 mirrors 0x160..0x17C */
#define TC4X_RXDATA_LAST   0x17C
#define TC4X_RXDATAD       0x180

static void asclin_buffer_reset(TriCoreASCLINState *s)
{
    memset(s->rxbuf, 0, ASCLIN_RX_BUFFER);
    s->rxbufreadidx = 0;
    s->rxbufwriteidx = 0;
}

static uint32_t asclin_buffer_used(TriCoreASCLINState *s)
{
    return ((s->rxbufwriteidx + ASCLIN_RX_BUFFER) - s->rxbufreadidx)
           % ASCLIN_RX_BUFFER;
}

static uint32_t asclin_buffer_free(TriCoreASCLINState *s)
{
    return (ASCLIN_RX_BUFFER - 1) - asclin_buffer_used(s);
}

/*
 * Emit a rising edge on TX/RX/ERR IRQ lines for flag bits in pulse_mask
 * that are currently enabled. IR trigger inputs are edge-sensitive
 * (TC3x TRM 16.5.3, TC4x TRM 41.3.4.2).
 */
static void asclin_pulse_irq(TriCoreASCLINState *s, uint32_t pulse_mask)
{
    uint32_t fired = pulse_mask & s->regs[FLAGSENABLE];

    if (fired & ASCLIN_TX_INT_MASK) {
        qemu_irq_pulse(s->TXSR);
    }
    if (fired & ASCLIN_RX_INT_MASK) {
        qemu_irq_pulse(s->RXSR);
    }
    if (fired & ASCLIN_ERR_INT_MASK) {
        qemu_irq_pulse(s->EXSR);
    }
}

static bool asclin_lin_mode(TriCoreASCLINState *s)
{
    /* FRAMECON.MODE=3 is LIN (TC2x/TC3x ASCLIN definition). */
    return ((s->regs[FRAMECON] >> 0) & 0x7) == 3;
}

static bool asclin_lin_master(TriCoreASCLINState *s)
{
    /* LINCON.MS (bit 26): 1 = master, 0 = slave. */
    return (s->regs[LINCON] & (1u << 26)) != 0;
}

static bool asclin_lin_pid_valid(uint8_t pid)
{
    uint8_t id = pid & 0x3f;
    unsigned p0 = ((id >> 0) ^ (id >> 1) ^ (id >> 2) ^ (id >> 4)) & 1;
    unsigned p1 = ((id >> 1) ^ (id >> 3) ^ (id >> 4) ^ (id >> 5)) & 1;

    return ((pid >> 6) & 1) == p0 && ((pid >> 7) & 1) == (p1 ^ 1);
}

static void asclin_lin_gateway_emit(TriCoreASCLINState *s, uint8_t byte)
{
    uint8_t record[2] = { ASCLIN_LIN_GATEWAY_BYTE, byte };

    if (s->lin_gateway) {
        qemu_chr_fe_write_all(&s->chr, record, sizeof(record));
    } else {
        qemu_chr_fe_write_all(&s->chr, &byte, 1);
    }
}

static void asclin_lin_gateway_end(TriCoreASCLINState *s)
{
    uint8_t marker = ASCLIN_LIN_GATEWAY_END;

    if (s->lin_gateway && asclin_lin_mode(s)) {
        qemu_chr_fe_write_all(&s->chr, &marker, 1);
    }
}

static void asclin_lin_bus_receive(TriCoreASCLINState *s, uint8_t byte)
{
    if (byte == 0x55) {
        s->lin_sync_seen = true;
        s->lin_pid_seen = false;
    } else if (s->lin_sync_seen && !s->lin_pid_seen) {
        s->lin_pid_seen = true;
        if (!asclin_lin_pid_valid(byte)) {
            qatomic_or(&s->regs[FLAGS], MASK_FLAGS_CE);
            asclin_pulse_irq(s, MASK_FLAGS_CE);
        }
    }

    if (asclin_buffer_free(s) == 0) {
        qatomic_or(&s->regs[FLAGS], MASK_FLAGS_RFO);
        asclin_pulse_irq(s, MASK_FLAGS_RFO);
        return;
    }
    s->rxbuf[s->rxbufwriteidx] = byte;
    s->rxbufwriteidx = (s->rxbufwriteidx + 1) % ASCLIN_RX_BUFFER;
    qatomic_or(&s->regs[FLAGS], MASK_FLAGS_RFL | MASK_FLAGS_RR | MASK_FLAGS_RH);
    asclin_pulse_irq(s, MASK_FLAGS_RFL);
}

/*
 * Retry callback when the chardev backend was busy on the last attempt.
 */
static gboolean uart_transmit_watch(void *do_not_use, GIOCondition cond,
                                    void *opaque)
{
    TriCoreASCLINState *s = TRICORE_ASCLIN(opaque);
    int ret;

    s->watch_tag = 0;

    if (s->lin_gateway && asclin_lin_mode(s)) {
        asclin_lin_gateway_emit(s, s->txbuf);
        ret = 1;
    } else {
        ret = qemu_chr_fe_write_all(&s->chr, (uint8_t *)&s->txbuf, 1);
    }
    if (ret <= 0) {
        s->watch_tag = qemu_chr_fe_add_watch(&s->chr, G_IO_OUT | G_IO_HUP,
                                             uart_transmit_watch, s);
        if (!s->watch_tag) {
            goto drained;
        }
        return G_SOURCE_REMOVE;
    }

drained:
    qatomic_or(&s->regs[FLAGS], MASK_FLAGS_TFL | MASK_FLAGS_TC);
    if (asclin_lin_mode(s)) {
        qatomic_or(&s->regs[FLAGS], MASK_FLAGS_TH | MASK_FLAGS_TR);
    }
    asclin_lin_gateway_end(s);
    asclin_pulse_irq(s, MASK_FLAGS_TFL | MASK_FLAGS_TC);
    return G_SOURCE_REMOVE;
}

/*
 * Instant-TX model. Every TXDATA write sends the byte immediately and
 * re-asserts TFL (FIFO level) and TC (transfer complete) with a fresh
 * edge so Zephyr's fifo_fill loop can drain its buffer across callbacks.
 */
static void asclin_txdata_write(TriCoreASCLINState *s, uint32_t value)
{
    int ret;

    /* CLC.DISR gates the module clock and suppresses transfers. */
    if (s->regs[CLC] & 1u) {
        return;
    }

    s->txbuf = value;

    /* FRAMECON.LB feeds transmitted bytes back into the receive FIFO. */
    if (s->regs[FRAMECON] & (1u << 28)) {
        if (asclin_buffer_free(s) == 0) {
            qatomic_or(&s->regs[FLAGS], MASK_FLAGS_TFO);
            asclin_pulse_irq(s, MASK_FLAGS_TFO);
        } else {
            asclin_lin_bus_receive(s, value);
        }
    }

    /* A master starts a header; a slave may transmit only while a response
     * is pending (RR was set by receipt of the preceding header/bytes). */
    if (asclin_lin_mode(s) && asclin_lin_bus &&
        (asclin_lin_master(s) || (s->regs[FLAGS] & MASK_FLAGS_RR))) {
        for (guint i = 0; i < asclin_lin_bus->len; i++) {
            TriCoreASCLINState *peer = g_ptr_array_index(asclin_lin_bus, i);
            if (peer != s && asclin_lin_mode(peer)) {
                asclin_lin_bus_receive(peer, value);
            }
        }
        if (!asclin_lin_master(s)) {
            qatomic_and(&s->regs[FLAGS], ~MASK_FLAGS_RR);
        }
    }

    ret = qemu_chr_fe_write_all(&s->chr, (uint8_t *)&s->txbuf, 1);
    if (ret <= 0) {
        s->watch_tag = qemu_chr_fe_add_watch(&s->chr, G_IO_OUT | G_IO_HUP,
                                             uart_transmit_watch, s);
        if (!s->watch_tag) {
            goto drained;
        }
        return;
    }

drained:
    qatomic_or(&s->regs[FLAGS], MASK_FLAGS_TFL | MASK_FLAGS_TC);
    if (asclin_lin_mode(s)) {
        qatomic_or(&s->regs[FLAGS], MASK_FLAGS_TH | MASK_FLAGS_TR);
    }
    asclin_lin_gateway_end(s);
    asclin_pulse_irq(s, MASK_FLAGS_TFL | MASK_FLAGS_TC);
}

/*
 * Custom QEMU block-TXDATA extension (not real ASCLIN HW).
 * Guest writes xfer length to 0x60, then a physical buffer address to 0x64;
 * QEMU drains the whole buffer through the chardev in one go.
 */
static void asclin_txdata_block(TriCoreASCLINState *s, uint32_t phys_addr)
{
    uint32_t xfer_len = s->regs[BLOCK_TXDATA_LEN];
    uint8_t *buf;
    int ret;

    if (xfer_len == 0) {
        return;
    }

    buf = g_malloc(xfer_len);
    cpu_physical_memory_read(phys_addr, buf, xfer_len);
    ret = qemu_chr_fe_write_all(&s->chr, buf, xfer_len);
    g_free(buf);

    if (ret > 0) {
        qatomic_or(&s->regs[FLAGS], MASK_FLAGS_TFL | MASK_FLAGS_TC);
        asclin_pulse_irq(s, MASK_FLAGS_TFL | MASK_FLAGS_TC);
    }
}

static void asclin_txfifocon_write(TriCoreASCLINState *s, uint32_t value)
{
    /* FILL is rh; never writable */
    s->regs[TXFIFOCON] = value & ~ASCLIN_FILL_MASK;
}

static uint32_t asclin_txfifocon_read(TriCoreASCLINState *s)
{
    /* Instant-TX: FILL always 0 so Zephyr fifo_fill loop (FILL<16) runs */
    return s->regs[TXFIFOCON] & ~ASCLIN_FILL_MASK;
}

static void asclin_rxfifocon_write(TriCoreASCLINState *s, uint32_t value)
{
    s->regs[RXFIFOCON] = value & ~ASCLIN_FILL_MASK;

    if (value & MASK_RXFIFOCON_FLUSH) {
        asclin_buffer_reset(s);
    }
    if (value & MASK_RXFIFOCON_ENI) {
        qemu_chr_fe_accept_input(&s->chr);
    }
}

static uint32_t asclin_rxfifocon_read(TriCoreASCLINState *s)
{
    uint32_t used = asclin_buffer_used(s);
    uint32_t fill = used > ASCLIN_HW_FIFO_DEPTH ? ASCLIN_HW_FIFO_DEPTH : used;

    return (s->regs[RXFIFOCON] & ~ASCLIN_FILL_MASK) |
           (fill << ASCLIN_FILL_SHIFT);
}

static uint32_t asclin_rxdata_read(TriCoreASCLINState *s, bool peek)
{
    uint32_t r;

    if (s->rxbufreadidx == s->rxbufwriteidx) {
        return 0;
    }

    r = s->rxbuf[s->rxbufreadidx];
    if (!peek) {
        s->rxbufreadidx = (s->rxbufreadidx + 1) % ASCLIN_RX_BUFFER;
        if (s->rxbufreadidx == s->rxbufwriteidx) {
            qatomic_and(&s->regs[FLAGS], ~MASK_FLAGS_RFL);
        }
    }
    return r;
}

static uint32_t asclin_csr_read(TriCoreASCLINState *s)
{
    uint32_t csr = s->regs[CSR];

    if (csr & 0x1F) {
        csr |= (1u << 31);
    }
    return csr;
}

static void asclin_flagsset_write(TriCoreASCLINState *s, uint32_t value)
{
    qatomic_or(&s->regs[FLAGS], value);
    asclin_pulse_irq(s, value);
}

static void asclin_flagsclear_write(TriCoreASCLINState *s, uint32_t value)
{
    qatomic_and(&s->regs[FLAGS], ~value);
}

/*
 * FLAGSENABLE write: newly-enabled bits whose FLAG is already set must
 * produce a rising edge on the aggregated line (the AND gate closes
 * then opens).
 */
static void asclin_flagsenable_write(TriCoreASCLINState *s, uint32_t value)
{
    uint32_t old_en = s->regs[FLAGSENABLE];
    uint32_t newly_enabled = value & ~old_en;

    s->regs[FLAGSENABLE] = value;
    asclin_pulse_irq(s, newly_enabled & s->regs[FLAGS]);
}

static uint64_t uart_read(void *opaque, hwaddr offset, unsigned size)
{
    TriCoreASCLINState *s = opaque;
    hwaddr reg_addr = offset >> 2;

    /* TC4x TXDATA mirror range reads as 0 */
    if (offset >= TC4X_TXDATA_BASE && offset <= TC4X_TXDATA_LAST) {
        return 0;
    }
    /* TC4x RXDATA mirror range: dequeue like RXDATA */
    if (offset >= TC4X_RXDATA_BASE && offset <= TC4X_RXDATA_LAST) {
        return asclin_rxdata_read(s, false);
    }

    switch (reg_addr) {
    /* TC3x */
    case CLC:
    case IOCR:
    case ID:
    case BITCON:
    case FRAMECON:
    case DATCON:
    case BRG:
    case BRD:
    case LINCON:
    case LINBTIMER:
    case LINHTIMER:
    case FLAGS:
    case FLAGSENABLE:
        return s->regs[reg_addr];
    case TXFIFOCON:
        return asclin_txfifocon_read(s);
    case RXFIFOCON:
        return asclin_rxfifocon_read(s);
    case FLAGSSET:
    case FLAGSCLEAR:
        return 0;
    case TXDATA:
        return 0;
    case RXDATA:
        return asclin_rxdata_read(s, false);
    case CSR:
        return asclin_csr_read(s);
    case RXDATAD:
        return asclin_rxdata_read(s, true);

    /* TC4x */
    case TC4X_IOCR / 4:
        return s->regs[IOCR];
    case TC4X_TXFIFOCON / 4:
        return asclin_txfifocon_read(s);
    case TC4X_RXFIFOCON / 4:
        return asclin_rxfifocon_read(s);
    case TC4X_BITCON / 4:
        return s->regs[BITCON];
    case TC4X_FRAMECON / 4:
        return s->regs[FRAMECON];
    case TC4X_DATCON / 4:
        return s->regs[DATCON];
    case TC4X_BRG / 4:
        return s->regs[BRG];
    case TC4X_BRD / 4:
        return s->regs[BRD];
    case TC4X_LINCON / 4:
        return s->regs[LINCON];
    case TC4X_LINBTIMER / 4:
        return s->regs[LINBTIMER];
    case TC4X_LINHTIMER / 4:
        return s->regs[LINHTIMER];
    case TC4X_FLAGS / 4:
        return s->regs[FLAGS];
    case TC4X_FLAGSSET / 4:
    case TC4X_FLAGSCLEAR / 4:
        return 0;
    case TC4X_FLAGSENABLE / 4:
        return s->regs[FLAGSENABLE];
    case TC4X_CSR / 4:
        return asclin_csr_read(s);
    case TC4X_RXDATAD / 4:
        return asclin_rxdata_read(s, true);

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
            "asclin_uart: read unknown reg offset 0x%" HWADDR_PRIx "\n",
            offset);
        return 0;
    }
}

static void uart_write(void *opaque, hwaddr offset, uint64_t value,
                       unsigned size)
{
    TriCoreASCLINState *s = opaque;
    hwaddr reg_addr = offset >> 2;
    uint32_t val = (uint32_t)value;
    bool old_lin_mode = asclin_lin_mode(s);

    /* TC4x TXDATA mirror writes enqueue data */
    if (offset >= TC4X_TXDATA_BASE && offset <= TC4X_TXDATA_LAST) {
        asclin_txdata_write(s, val);
        return;
    }
    /* TC4x RXDATA mirror writes return BE on HW; ignore */
    if (offset >= TC4X_RXDATA_BASE && offset <= TC4X_RXDATA_LAST) {
        return;
    }

    switch (reg_addr) {
    /* TC3x */
    case CLC:
    case IOCR:
    case ID:
    case BITCON:
    case FRAMECON:
    case DATCON:
    case BRG:
    case BRD:
    case LINCON:
    case LINBTIMER:
    case LINHTIMER:
        s->regs[reg_addr] = val;
        break;
    case TXFIFOCON:
        asclin_txfifocon_write(s, val);
        break;
    case RXFIFOCON:
        asclin_rxfifocon_write(s, val);
        break;
    case FLAGS:
        /* rh on TC3x; BE on write on TC4x; ignore direct writes */
        break;
    case FLAGSSET:
        asclin_flagsset_write(s, val);
        break;
    case FLAGSCLEAR:
        asclin_flagsclear_write(s, val);
        break;
    case FLAGSENABLE:
        asclin_flagsenable_write(s, val);
        break;
    case TXDATA:
        asclin_txdata_write(s, val);
        break;
    case RXDATA:
    case RXDATAD:
        /* read-only on HW */
        break;
    case CSR:
        s->regs[CSR] = val;
        break;

    /* Custom QEMU block-TXDATA interface */
    case BLOCK_TXDATA_LEN:
        s->regs[BLOCK_TXDATA_LEN] = val;
        break;
    case BLOCK_TXDATA_BUF:
        if (s->block_tx_enabled) {
            asclin_txdata_block(s, val);
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                "asclin_uart: QEMU block-TX extension disabled\n");
        }
        break;

    /* TC4x */
    case TC4X_IOCR / 4:
        s->regs[IOCR] = val;
        break;
    case TC4X_TXFIFOCON / 4:
        asclin_txfifocon_write(s, val);
        break;
    case TC4X_RXFIFOCON / 4:
        asclin_rxfifocon_write(s, val);
        break;
    case TC4X_BITCON / 4:
        s->regs[BITCON] = val;
        break;
    case TC4X_FRAMECON / 4:
        s->regs[FRAMECON] = val;
        break;
    case TC4X_DATCON / 4:
        s->regs[DATCON] = val;
        break;
    case TC4X_BRG / 4:
        s->regs[BRG] = val;
        break;
    case TC4X_BRD / 4:
        s->regs[BRD] = val;
        break;
    case TC4X_LINCON / 4:
        s->regs[LINCON] = val;
        break;
    case TC4X_LINBTIMER / 4:
        s->regs[LINBTIMER] = val;
        break;
    case TC4X_LINHTIMER / 4:
        s->regs[LINHTIMER] = val;
        break;
    case TC4X_FLAGS / 4:
        /* BE on write; ignore */
        break;
    case TC4X_FLAGSSET / 4:
        asclin_flagsset_write(s, val);
        break;
    case TC4X_FLAGSCLEAR / 4:
        asclin_flagsclear_write(s, val);
        break;
    case TC4X_FLAGSENABLE / 4:
        asclin_flagsenable_write(s, val);
        break;
    case TC4X_CSR / 4:
        s->regs[CSR] = val;
        break;

    default:
        qemu_log_mask(LOG_GUEST_ERROR,
            "asclin_uart: write unknown reg offset 0x%" HWADDR_PRIx "\n",
            offset);
        break;
    }

    /* Keep the host chardev framing synchronized with guest configuration. */
    if (reg_addr == BITCON || reg_addr == BRG || reg_addr == FRAMECON ||
        reg_addr == DATCON || reg_addr == TC4X_BITCON / 4 ||
        reg_addr == TC4X_BRG / 4 || reg_addr == TC4X_FRAMECON / 4 ||
        reg_addr == TC4X_DATCON / 4) {
        asclin_uart_update_parameters(s);
    }
    if (reg_addr == FRAMECON || reg_addr == TC4X_FRAMECON / 4) {
        if (old_lin_mode && !asclin_lin_mode(s)) {
            qatomic_and(&s->regs[FLAGS],
                        ~(MASK_FLAGS_TH | MASK_FLAGS_TR |
                          MASK_FLAGS_RH | MASK_FLAGS_RR));
            s->lin_sync_seen = false;
            s->lin_pid_seen = false;
        }
    }
}

static const MemoryRegionOps asclin_uart_mmio_ops = {
    .read = uart_read,
    .write = uart_write,
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .endianness = DEVICE_LITTLE_ENDIAN,
};

static void uart_rx(void *opaque, const uint8_t *buf, int size)
{
    TriCoreASCLINState *s = opaque;

    while (size > 0) {
        if (s->lin_gateway && asclin_lin_mode(s)) {
            if (s->lin_gateway_rx_type == ASCLIN_LIN_GATEWAY_BYTE) {
                asclin_lin_bus_receive(s, *buf++);
                s->lin_gateway_rx_type = 0;
                size--;
                continue;
            }
            s->lin_gateway_rx_type = (*buf++ == ASCLIN_LIN_GATEWAY_BYTE) ?
                                      ASCLIN_LIN_GATEWAY_BYTE : 0;
            size--;
            continue;
        }
        if (asclin_buffer_free(s) == 0) {
            error_report(
                "asclin_uart: RX buffer overflowed, %d bytes dropped", size);
            qatomic_or(&s->regs[FLAGS], MASK_FLAGS_RFO);
            asclin_pulse_irq(s, MASK_FLAGS_RFO);
            break;
        }
        s->rxbuf[s->rxbufwriteidx] = *buf++;
        s->rxbufwriteidx = (s->rxbufwriteidx + 1) % ASCLIN_RX_BUFFER;
        size--;
    }

    if (s->rxbufreadidx != s->rxbufwriteidx) {
        qatomic_or(&s->regs[FLAGS], MASK_FLAGS_RFL);
        if (asclin_lin_mode(s)) {
            qatomic_or(&s->regs[FLAGS], MASK_FLAGS_RH | MASK_FLAGS_RR);
        }
        asclin_pulse_irq(s, MASK_FLAGS_RFL);
    }
}

static int uart_can_rx(void *opaque)
{
    TriCoreASCLINState *s = TRICORE_ASCLIN(opaque);

    if (!(s->regs[CLC] & 1u) &&
        (s->regs[RXFIFOCON] & MASK_RXFIFOCON_ENI) &&
        asclin_buffer_free(s) > 0) {
        return 1;
    }
    return 0;
}

static void uart_event(void *opaque, QEMUChrEvent event)
{
}

static void asclin_uart_reset(DeviceState *d)
{
    TriCoreASCLINState *s = TRICORE_ASCLIN(d);
    int i;

    for (i = 0; i < ASCLIN_R_MAX; i++) {
        s->regs[i] = 0;
    }
    asclin_buffer_reset(s);
    s->lin_sync_seen = false;
    s->lin_pid_seen = false;
    s->lin_gateway_rx_type = 0;
}

static void asclin_uart_update_parameters(TriCoreASCLINState *s)
{
    QEMUSerialSetParams ssp;
    uint32_t bitcon = s->regs[BITCON];
    uint32_t brg = s->regs[BRG];
    uint32_t framecon = s->regs[FRAMECON];
    uint32_t prescaler = (bitcon & 0xfff) + 1;
    uint32_t oversampling = ((bitcon >> 16) & 0xf) + 1;
    uint32_t denominator = brg & 0xfff;
    uint32_t numerator = (brg >> 16) & 0xfff;
    uint32_t baudrate = 115200;

    /* The TC2x/TC3x iLLD derives baud from a 100 MHz peripheral clock. */
    if (denominator != 0 && numerator != 0) {
        uint64_t rate = 100000000ULL * numerator;
        rate /= denominator * prescaler * oversampling;
        if (rate != 0 && rate <= UINT32_MAX) {
            baudrate = rate;
        }
    }

    ssp.speed = baudrate;
    ssp.data_bits = (s->regs[DATCON] & 0xf) + 1;
    if (ssp.data_bits < 5 || ssp.data_bits > 8) {
        ssp.data_bits = 8;
    }
    ssp.parity = (framecon & (1u << 30)) ?
                 ((framecon & (1u << 31)) ? 'O' : 'E') : 'N';
    ssp.stop_bits = ((framecon >> 9) & 0x7) ? 2 : 1;
    qemu_chr_fe_ioctl(&s->chr, CHR_IOCTL_SERIAL_SET_PARAMS, &ssp);
}

static void asclin_uart_realize(DeviceState *dev, Error **errp)
{
    TriCoreASCLINState *s = TRICORE_ASCLIN(dev);

    if (!asclin_lin_bus) {
        asclin_lin_bus = g_ptr_array_new();
    }
    g_ptr_array_add(asclin_lin_bus, s);

    qemu_chr_fe_set_handlers(&s->chr, uart_can_rx, uart_rx, uart_event, NULL,
                             s, NULL, true);
}

static void asclin_uart_unrealize(DeviceState *dev)
{
    TriCoreASCLINState *s = TRICORE_ASCLIN(dev);

    if (asclin_lin_bus) {
        g_ptr_array_remove_fast(asclin_lin_bus, s);
    }
    qemu_chr_fe_set_handlers(&s->chr, NULL, NULL, NULL, NULL,
                             NULL, NULL, false);
}

static void asclin_uart_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    TriCoreASCLINState *s = TRICORE_ASCLIN(obj);

    memory_region_init_io(&s->iomem, obj, &asclin_uart_mmio_ops, s, "uart",
                          0x200);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->RXSR);
    sysbus_init_irq(sbd, &s->TXSR);
    sysbus_init_irq(sbd, &s->EXSR);
    s->rxbufreadidx = 0;
    s->rxbufwriteidx = 0;

    asclin_uart_update_parameters(s);
}

static int asclin_uart_post_load(void *opaque, int version_id)
{
    TriCoreASCLINState *s = TRICORE_ASCLIN(opaque);

    if (s->regs[FLAGS] & MASK_FLAGS_TFL) {
        s->watch_tag = qemu_chr_fe_add_watch(&s->chr, G_IO_OUT | G_IO_HUP,
                                             uart_transmit_watch, s);
    }
    asclin_uart_update_parameters(s);
    return 0;
}

static const VMStateDescription vmstate_asclin_uart = {
    .name = "asclin-uart",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, TriCoreASCLINState, ASCLIN_R_MAX),
        VMSTATE_UINT32(txbuf, TriCoreASCLINState),
        VMSTATE_UINT8_ARRAY(rxbuf, TriCoreASCLINState, ASCLIN_RX_BUFFER),
        VMSTATE_UINT32(rxbufwriteidx, TriCoreASCLINState),
        VMSTATE_UINT32(rxbufreadidx, TriCoreASCLINState),
        VMSTATE_BOOL(block_tx_enabled, TriCoreASCLINState),
        VMSTATE_BOOL(lin_gateway, TriCoreASCLINState),
        VMSTATE_UINT8(lin_gateway_rx_type, TriCoreASCLINState),
        VMSTATE_BOOL(lin_sync_seen, TriCoreASCLINState),
        VMSTATE_BOOL(lin_pid_seen, TriCoreASCLINState),
        VMSTATE_END_OF_LIST()
    },
    .post_load = asclin_uart_post_load,
};

static const Property asclin_uart_properties[] = {
    DEFINE_PROP_CHR("chardev", TriCoreASCLINState, chr),
    DEFINE_PROP_BOOL("block-tx-enabled", TriCoreASCLINState,
                     block_tx_enabled, true),
    DEFINE_PROP_BOOL("lin-gateway", TriCoreASCLINState, lin_gateway, false),
};

static void asclin_uart_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = asclin_uart_realize;
    dc->unrealize = asclin_uart_unrealize;
    dc->legacy_reset = asclin_uart_reset;
    dc->vmsd = &vmstate_asclin_uart;
    device_class_set_props(dc, asclin_uart_properties);
}

static const TypeInfo asclin_uart_info = {
    .name = TYPE_TRICORE_ASCLIN,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(TriCoreASCLINState),
    .instance_init = asclin_uart_init,
    .class_init = asclin_uart_class_init,
};

static void asclin_uart_register_types(void)
{
    type_register_static(&asclin_uart_info);
}

type_init(asclin_uart_register_types)
