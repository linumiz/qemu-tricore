/*
 * QEMU TriCore ASCLIN device.
 *
 * Copyright (c) 2017 David Brenken <david.brenken@efs-auto.de>
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#ifndef ASCLIN_UART_H
#define ASCLIN_UART_H

#include "hw/core/sysbus.h"
#include "qapi/error.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "chardev/char-fe.h"
#include "hw/core/ptimer.h"
#include "qemu/timer.h"

enum {
    STAT_THRE = (1 << 0), STAT_RX_EVT = (1 << 1), STAT_TX_EVT = (1 << 2),
};

enum {
    CTRL_RX_IRQ_EN = (1 << 0),
    CTRL_TX_IRQ_EN = (1 << 1),
    CTRL_THRU_EN = (1 << 2),
};

enum {
    DBG_BREAK_EN = (1 << 0),
};

/* FLAGS bits. TC3x and TC4x share layout except TC4x adds
 * TFE(10), OMT(11), SWTRG(12) which are unused by the current driver.
 */
#define MASK_FLAGS_TH    (1u << 0)
#define MASK_FLAGS_TR    (1u << 1)
#define MASK_FLAGS_RH    (1u << 2)
#define MASK_FLAGS_RR    (1u << 3)
#define MASK_FLAGS_FED   (1u << 5)
#define MASK_FLAGS_RED   (1u << 6)
#define MASK_FLAGS_TFE   (1u << 10)
#define MASK_FLAGS_OMT   (1u << 11)
#define MASK_FLAGS_PE    (1u << 16)
#define MASK_FLAGS_TC    (1u << 17)
#define MASK_FLAGS_FE    (1u << 18)
#define MASK_FLAGS_HT    (1u << 19)
#define MASK_FLAGS_RT    (1u << 20)
#define MASK_FLAGS_BD    (1u << 21)
#define MASK_FLAGS_LP    (1u << 22)
#define MASK_FLAGS_LA    (1u << 23)
#define MASK_FLAGS_LC    (1u << 24)
#define MASK_FLAGS_CE    (1u << 25)
#define MASK_FLAGS_RFO   (1u << 26)
#define MASK_FLAGS_RFU   (1u << 27)
#define MASK_FLAGS_RFL   (1u << 28)
#define MASK_FLAGS_TFO   (1u << 30)
#define MASK_FLAGS_TFL   (1u << 31)
#define MASK_FLAGS_LIN_BREAK MASK_FLAGS_BD

#define MASK_RXFIFOCON_FLUSH 0x1
#define MASK_RXFIFOCON_ENI   0x2
#define MASK_RXFIFOCON_OUTW  0xC0

#define MASK_FLAGSENABLE_RFLE (1u << 28)
#define MASK_FLAGSENABLE_TFLE (1u << 31)

/* Per-line FLAGS bit masks (which flag drives which interrupt line) */
#define ASCLIN_TX_INT_MASK  (MASK_FLAGS_TH | MASK_FLAGS_TR | MASK_FLAGS_TFL)
#define ASCLIN_RX_INT_MASK  (MASK_FLAGS_RH | MASK_FLAGS_RR | MASK_FLAGS_RFL)
#define ASCLIN_ERR_INT_MASK (MASK_FLAGS_FED | MASK_FLAGS_RED | \
                             MASK_FLAGS_TFE | MASK_FLAGS_OMT | \
                             MASK_FLAGS_PE  | MASK_FLAGS_TC  | \
                             MASK_FLAGS_FE  | MASK_FLAGS_HT  | \
                             MASK_FLAGS_RT  | MASK_FLAGS_BD  | \
                             MASK_FLAGS_LP  | MASK_FLAGS_LA  | \
                             MASK_FLAGS_LC  | MASK_FLAGS_CE  | \
                             MASK_FLAGS_RFO | MASK_FLAGS_RFU | \
                             MASK_FLAGS_TFO)

/* HW FIFO depth is 16; FILL field is [20:16] in TX/RXFIFOCON */
#define ASCLIN_HW_FIFO_DEPTH 16
#define ASCLIN_FILL_SHIFT    16
#define ASCLIN_FILL_MASK     (0x1Fu << ASCLIN_FILL_SHIFT)

#define ASCLIN_R_MAX 27
#define ASCLIN_RX_BUFFER 8192
#define ASCLIN_RX_FIFO_MASK (ASCLIN_HW_FIFO_DEPTH - 1)

#define TYPE_TRICORE_ASCLIN "tricore_asclin"
#define TRICORE_ASCLIN(obj) \
    OBJECT_CHECK(TriCoreASCLINState, (obj), TYPE_TRICORE_ASCLIN)

struct TriCoreASCLINState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    CharFrontend chr;
    qemu_irq RXSR;
    qemu_irq TXSR;
    qemu_irq EXSR;
    guint watch_tag;
    uint32_t regs[ASCLIN_R_MAX];
    uint32_t txbuf;
    uint8_t rxbuf[ASCLIN_RX_BUFFER];
    uint32_t rxbufwriteidx;
    uint32_t rxbufreadidx;
    bool block_tx_enabled;
    bool lin_gateway;
    uint8_t lin_gateway_rx_type;
    bool lin_sync_seen;
    bool lin_pid_seen;
    uint8_t lin_data_count;
    uint16_t lin_checksum_sum;
    uint8_t lin_response_length;
    bool lin_checksum_enhanced;
    ptimer_state *ptimer;
    QEMUTimer *lin_timeout_timer;
    bool lin_timeout_response;
    QEMUBH *bh;
};
typedef struct TriCoreASCLINState TriCoreASCLINState;


#endif
