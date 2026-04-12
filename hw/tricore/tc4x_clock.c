/*
 * QEMU TC4x Clockdevice.
 *
 * Copyright (c) 2026 Infineon Technologies AG
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 *
 */

#include "qemu/osdep.h"
#include "hw/core/clock.h"
#include "hw/core/qdev-clock.h"
#include "hw/core/registerfields.h"
#include "hw/core/sysbus.h"
#include "hw/tricore/tc4x_clock.h"
#include "qapi/error.h"
#include "qemu/log.h"

#define UPDATE_DIV_CLK(_clk, _freq, _field_reg, _field_name) clock_update(s->_clk, FIELD_EX32(s->_field_reg, _field_reg, _field_name)*_freq)

static void tc4x_clock_update_sysccu(TC4xClockState *s)
{
    UPDATE_DIV_CLK(fspb, clock_get(s->fsource0), SYSCCUCON0, SPBDIV);
    UPDATE_DIV_CLK(fsri, clock_get(s->fsource0), SYSCCUCON0, SRIDIV);
    UPDATE_DIV_CLK(fstm, clock_get(s->fsource0), SYSCCUCON0, STMDIV);
    UPDATE_DIV_CLK(fsrics, clock_get(s->fsource0), SYSCCUCON0, SRICSDIV);
    UPDATE_DIV_CLK(fgeth, clock_get(s->fsource0), SYSCCUCON1, GETHDIV);
    UPDATE_DIV_CLK(fegtm, clock_get(s->fsource0), SYSCCUCON1, EGTMDIV);
    UPDATE_DIV_CLK(fmcanh, clock_get(s->fsource0), SYSCCUCON1, MCANHDIV);
    UPDATE_DIV_CLK(fleth, clock_get(s->fsource0), SYSCCUCON1, LETHDIV);
    UPDATE_DIV_CLK(fcanxlh, clock_get(s->fsource0), SYSCCUCON1, CANXLHDIV);
}

static void tc4x_clock_update_perccu(TC4xClockState *s) {
    if (FIELD_EX32(s->PERCCUCON0, PERCCUCON0, CLKSELQSPI) == 0) {
        UPDATE_DIV_CLK(fqspi, 0, PERCCUCON0, QSPIDIV);
    } else if (FIELD_EX32(s->PERCCUCON0, PERCCUCON0, CLKSELQSPI) == 1) {
        UPDATE_DIV_CLK(fqspi, clock_get(s->fsource1), PERCCUCON0, QSPIDIV);
    } else {
        UPDATE_DIV_CLK(fqspi, clock_get(s->fsource2), PERCCUCON0, QSPIDIV);
    }

    UPDATE_DIV_CLK(fmcan, clock_get(s->fsource1), PERCCUCON0, MCANDIV);
    UPDATE_DIV_CLK(fasclins, clock_get(s->fsource1), PERCCUCON1, ASCLINSDIV);
    UPDATE_DIV_CLK(fcanxl, clock_get(s->fsource1), PERCCUCON1, CANXLDIV);

    UPDATE_DIV_CLK(fi2c, clock_get(s->fsource2), PERCCUCON0, I2CDIV);
    UPDATE_DIV_CLK(fasclinf, clock_get(s->fsource2), PERCCUCON1, ASCLINFDIV);
}


static void tc4x_clock_update_freq(TC4xClockState *s)
{
    uint32_t fsource0 = clock_get(s->fsource0);
    uint32_t fsource1 = clock_get(s->fsource1);
    uint32_t fsource2 = clock_get(s->fsource2);
    uint32_t fsource3 = clock_get(s->fsource3);

    switch (s->clksels) {
    case TC4X_CLOCK_CLKSEL_PLL:
        if (FIELD_EX32(s->SYSPLLCON0, SYSPLLCON0, PLLPWR) == 1) {
            clock_set_hz(
                s->fsource0,
                clock_get_hz(s->fosc) /
                    (FIELD_EX32(s->SYSPLLCON0, SYSPLLCON0, PDIV) + 1) *
                    (FIELD_EX32(s->SYSPLLCON0, SYSPLLCON0, NDIV) + 1) /
                    (FIELD_EX32(s->SYSPLLCON1, SYSPLLCON1, K2DIV) + 1));
        } else {
            clock_set_hz(s->fsource0, 0);
        }
        break;
    case TC4X_CLOCK_CLKSEL_BACK:
        clock_set_hz(s->fsource0, 100000000);
        break;
    case TC4X_CLOCK_CLKSEL_RAMP:
        if (s->rampfstat == TC4X_CLOCK_RAMP_FSTAT_AT_BASE) {
            clock_set_hz(s->fsource0, 100000000);
        } else if (s->rampfstat == TC4X_CLOCK_RAMP_FSTAT_AT_TOP) {
            clock_set_hz(s->fsource0,
                         1000000 * FIELD_EX32(s->RAMPCON0, RAMPCON0, UFL));
        }
        break;
    }
    switch (s->clkselp) {
    case TC4X_CLOCK_CLKSEL_PLL:
        if (FIELD_EX32(s->PERPLLCON0, PERPLLCON0, PLLPWR) == 1) {
            clock_set_hz(
                s->fsource1,
                clock_get_hz(s->fosc) /
                    (FIELD_EX32(s->PERPLLCON0, PERPLLCON0, PDIV) + 1) *
                    (FIELD_EX32(s->PERPLLCON0, PERPLLCON0, NDIV) + 1) /
                    (FIELD_EX32(s->PERPLLCON1, PERPLLCON1, K2DIV) + 1));
            clock_set_hz(
                s->fsource2,
                clock_get_hz(s->fosc) /
                    (FIELD_EX32(s->PERPLLCON0, PERPLLCON0, PDIV) + 1) *
                    (FIELD_EX32(s->PERPLLCON0, PERPLLCON0, NDIV) + 1) /
                    (FIELD_EX32(s->PERPLLCON1, PERPLLCON1, K3DIV) + 1));
            clock_set_hz(
                s->fsource3,
                clock_get_hz(s->fosc) /
                    (FIELD_EX32(s->PERPLLCON0, PERPLLCON0, PDIV) + 1) *
                    (FIELD_EX32(s->PERPLLCON0, PERPLLCON0, NDIV) + 1) /
                    (FIELD_EX32(s->PERPLLCON1, PERPLLCON1, K4DIV) + 1));
        } else {
            clock_set_hz(s->fsource1, 0);
            clock_set_hz(s->fsource2, 0);
            clock_set_hz(s->fsource3, 0);
        }
        break;
    case TC4X_CLOCK_CLKSEL_BACK:
        clock_set_hz(s->fsource1, 100000000);
        clock_set_hz(s->fsource2, 100000000);
        clock_set_hz(s->fsource3, 100000000);
        break;
    default:
        break;
    }

    if (fsource0 != clock_get(s->fsource0)) {
        clock_propagate(s->fsource0);
    }
    if (fsource1 != clock_get(s->fsource1)) {
        clock_propagate(s->fsource1);
    }
    if (fsource2 != clock_get(s->fsource2)) {
        clock_propagate(s->fsource2);
    }
    if (fsource3 != clock_get(s->fsource3)) {
        clock_propagate(s->fsource3);
    }
}

static void tc4x_clock_write(void *opaque, hwaddr offset, uint64_t value,
                             unsigned size)
{
    TC4xClockState *s = (TC4xClockState *)opaque;
    uint32_t id = offset / 4;

    switch (id) {
    case R_OSCCON:
        memcpy((uint8_t *)&s->OSCCON + (offset & 0x3), &value, size);
        s->insel = FIELD_EX32(s->OSCCON, OSCCON, INSEL);
        break;
    case R_OSCMON0:
        memcpy((uint8_t *)&s->OSCMON0 + (offset & 0x3), &value, size);
        break;
    case R_OSCMON1:
        memcpy((uint8_t *)&s->OSCMON1 + (offset & 0x3), &value, size);
        break;
    case R_CCUCON:
        memcpy((uint8_t *)&s->CCUCON + (offset & 0x3), &value, size);
        s->clksels = FIELD_EX32(s->CCUCON, CCUCON, CLKSELS);
        s->clkselp = FIELD_EX32(s->CCUCON, CCUCON, CLKSELP);
        tc4x_clock_update_freq(s);
        break;
    case R_SYSPLLCON0:
        memcpy((uint8_t *)&s->SYSPLLCON0 + (offset & 0x3), &value, size);
        tc4x_clock_update_freq(s);
        break;
    case R_SYSPLLCON1:
        memcpy((uint8_t *)&s->SYSPLLCON1 + (offset & 0x3), &value, size);
        tc4x_clock_update_freq(s);
        break;
    case R_SYSPLLCON2:
        memcpy((uint8_t *)&s->SYSPLLCON2 + (offset & 0x3), &value, size);
        tc4x_clock_update_freq(s);
        break;
    case R_PERPLLCON0:
        memcpy((uint8_t *)&s->PERPLLCON0 + (offset & 0x3), &value, size);
        tc4x_clock_update_freq(s);
        break;
    case R_PERPLLCON1:
        memcpy((uint8_t *)&s->PERPLLCON1 + (offset & 0x3), &value, size);
        tc4x_clock_update_freq(s);
        break;
    case R_PERPLLCON2:
        memcpy((uint8_t *)&s->PERPLLCON2 + (offset & 0x3), &value, size);
        tc4x_clock_update_freq(s);
        break;
    case R_RAMPCON0:
        memcpy((uint8_t *)&s->RAMPCON0 + (offset & 0x3), &value, size);
        tc4x_clock_update_freq(s);
        s->RAMPCON0 &= ~R_RAMPCON0_CMD_MASK;
        break;
    case R_SYSCCUCON0:
        memcpy((uint8_t *)&s->SYSCCUCON0 + (offset & 0x3), &value, size);
        tc4x_clock_update_sysccu(s);
        break;
        case R_SYSCCUCON1:
        memcpy((uint8_t *)&s->SYSCCUCON1 + (offset & 0x3), &value, size);
        tc4x_clock_update_sysccu(s);
        break;
    case R_PERCCUCON0:
        memcpy((uint8_t *)&s->PERCCUCON0 + (offset & 0x3), &value, size);
        tc4x_clock_update_perccu(s);
        break;
    case R_PERCCUCON1:
        memcpy((uint8_t *)&s->PERCCUCON1 + (offset & 0x3), &value, size);
        tc4x_clock_update_perccu(s);
        break;
    default:
        break;
    }
}

static uint64_t tc4x_clock_read(void *opaque, hwaddr offset, unsigned size)
{
    TC4xClockState *s = (TC4xClockState *)opaque;
    uint32_t id = offset / 4;
    uint32_t value = 0, temp = 0;

    switch (id) {
    case R_ID:
        value = 0x00EEC013;
        break;
    case R_OSCCON:
        memcpy(&value, (uint8_t *)&s->OSCCON + (offset & 0x3), size);
        break;
    case R_OSCMON0:
        memcpy(&value, (uint8_t *)&s->OSCMON0 + (offset & 0x3), size);
        break;
    case R_OSCMON1:
        memcpy(&value, (uint8_t *)&s->OSCMON1 + (offset & 0x3), size);
        break;
    case R_SYSPLLCON0:
        memcpy(&value, (uint8_t *)&s->SYSPLLCON0 + (offset & 0x3), size);
        break;
    case R_SYSPLLCON1:
        memcpy(&value, (uint8_t *)&s->SYSPLLCON1 + (offset & 0x3), size);
        break;
    case R_SYSPLLCON2:
        memcpy(&value, (uint8_t *)&s->SYSPLLCON2 + (offset & 0x3), size);
        break;
    case R_SYSPLLSTAT:
        if (FIELD_EX32(s->SYSPLLCON0, SYSPLLCON0, PLLPWR) == 1) {
            temp = R_SYSPLLSTAT_PLLLOCK_MASK | R_SYSPLLCON0_PLLPWR_MASK;
        }
        memcpy(&value, (uint8_t *)&temp + (offset & 0x3), size);
        break;
    case R_PERPLLCON0:
        memcpy(&value, (uint8_t *)&s->PERPLLCON0 + (offset & 0x3), size);
        break;
    case R_PERPLLCON1:
        memcpy(&value, (uint8_t *)&s->PERPLLCON1 + (offset & 0x3), size);
        break;
    case R_PERPLLCON2:
        memcpy(&value, (uint8_t *)&s->PERPLLCON2 + (offset & 0x3), size);
        break;
    case R_PERPLLSTAT:
        if (FIELD_EX32(s->PERPLLCON0, PERPLLCON0, PLLPWR) == 1) {
            temp = R_PERPLLSTAT_PLLLOCK_MASK | R_PERPLLCON0_PLLPWR_MASK;
        }
        memcpy(&value, (uint8_t *)&temp + (offset & 0x3), size);
        break;
    case R_CCUCON:
        temp = FIELD_DP32(0, CCUCON, CLKSELS, s->clksels) |
               FIELD_DP32(0, CCUCON, CLKSELP, s->clkselp);
        memcpy(&value, (uint8_t *)&temp + (offset & 0x3), size);
        break;
    case R_CCUSTAT:
        temp = FIELD_DP32(0, CCUSTAT, CLKSELS, s->clksels) |
               FIELD_DP32(0, CCUSTAT, CLKSELP, s->clkselp) |
               FIELD_DP32(0, CCUSTAT, LCK, 0);
        memcpy(&value, (uint8_t *)&temp + (offset & 0x3), size);
        break;
    case R_RAMPCON0:
        memcpy(&value, (uint8_t *)&s->RAMPCON0 + (offset & 0x3), size);
        break;
    case R_SYSCCUCON0:
        memcpy(&value, (uint8_t *)&s->SYSCCUCON0 + (offset & 0x3), size);
        break;
    case R_SYSCCUCON1:
        memcpy(&value, (uint8_t *)&s->SYSCCUCON1 + (offset & 0x3), size);
        break;
    case R_PERCCUCON0:
        memcpy(&value, (uint8_t *)&s->PERCCUCON0 + (offset & 0x3), size);
        break;
    case R_PERCCUCON1:
        memcpy(&value, (uint8_t *)&s->PERCCUCON1 + (offset & 0x3), size);
        break;
    case R_RAMPSTAT:
        temp = R_RAMPSTAT_FLLLOCK_MASK |
               FIELD_DP32(0, RAMPSTAT, ACTIVE,
                          FIELD_EX32(s->RAMPCON0, RAMPCON0, PWR)) |
               FIELD_DP32(0, RAMPSTAT, FSTAT, s->rampfstat);
        memcpy(&value, (uint8_t *)&temp + (offset & 0x3), size);
        break;
    default:
        break;
    }

    return value;
}

static void tc4x_clock_reset(Object *obj, ResetType type)
{
    TC4xClockState *s = TC4X_CLOCK(obj);

    s->insel = TC4X_CLOCK_INSEL_OSC;
    s->clksels = TC4X_CLOCK_CLKSEL_BACK;
    s->clkselp = TC4X_CLOCK_CLKSEL_BACK;
    s->rampfstat = TC4X_CLOCK_RAMP_FSTAT_AT_BASE;

    s->OSCCON = 0x01000723;
    s->OSCMON0 = 0;
    s->OSCMON1 = 0;

    s->RAMPCON0 = 0x00000190;

    s->SYSPLLCON0 = 0;
    s->SYSPLLCON1 = 0x0000070F;
    s->SYSPLLCON2 = 0;

    s->PERPLLCON0 = 0;
    s->PERPLLCON1 = 0x0007070F;
    s->PERPLLCON2 = 0;

    s->SYSCCUCON0 = 0x02111102;
    s->SYSCCUCON1 = 0x02420004;
    s->PERCCUCON0 = 0x53101015;
    s->PERCCUCON1 = 0x15511201;
}

static const MemoryRegionOps tc4x_clock_ops = {
    .read = tc4x_clock_read,
    .write = tc4x_clock_write,
    .valid = { .min_access_size = 1, .max_access_size = 4, }, 
    .endianness = DEVICE_LITTLE_ENDIAN
};

static void tc4x_clock_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    TC4xClockState *s = TC4X_CLOCK(obj);

    tc4x_clock_reset(OBJECT(s), RESET_TYPE_COLD);

    /* map memory */
    memory_region_init_io(&s->iomem, OBJECT(s), &tc4x_clock_ops, s,
                          "tc4x-clock", 0x1000);

    s->fsource0 = clock_new(OBJECT(s), "fsource0");
    s->fsource1 = clock_new(OBJECT(s), "fsource1");
    s->fsource2 = clock_new(OBJECT(s), "fsource2");
    s->fsource3 = clock_new(OBJECT(s), "fsource3");
    s->fosc = qdev_init_clock_in(DEVICE(s), "fosc", NULL, NULL, 0);
    s->fspb = qdev_init_clock_out(DEVICE(s), "fspb");
    s->fsri = qdev_init_clock_out(DEVICE(s), "fsri");
    s->fstm = qdev_init_clock_out(DEVICE(s), "fstm");
    s->fsrics = qdev_init_clock_out(DEVICE(s), "fsrics");
    s->fgeth = qdev_init_clock_out(DEVICE(s), "fgeth");
    s->fegtm = qdev_init_clock_out(DEVICE(s), "fegtm");
    s->fmcanh = qdev_init_clock_out(DEVICE(s), "fmcanh");
    s->fleth = qdev_init_clock_out(DEVICE(s), "fleth");
    s->fcanxlh = qdev_init_clock_out(DEVICE(s), "fcanxlh");
    s->fmcan = qdev_init_clock_out(DEVICE(s), "fmcan");
    s->fqspi = qdev_init_clock_out(DEVICE(s), "fqspi");
    s->fi2c = qdev_init_clock_out(DEVICE(s), "fi2c");
    s->fasclinf = qdev_init_clock_out(DEVICE(s), "fasclinf");
    s->fasclins = qdev_init_clock_out(DEVICE(s), "fasclins");
    s->fcanxl = qdev_init_clock_out(DEVICE(s), "fcanxl");

    sysbus_init_mmio(sbd, &s->iomem);
}

static void tc4x_clock_realize(DeviceState *dev, Error **errp)
{
    TC4xClockState *s = TC4X_CLOCK(dev);

    if (!clock_has_source(s->fosc)) {
        error_setg(errp, "tc4x_clock: fosc must be connected");
        return;
    }

    tc4x_clock_update_freq(s);
    tc4x_clock_update_sysccu(s);
    tc4x_clock_update_perccu(s);
}

static void tc4x_clock_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    rc->phases.hold = tc4x_clock_reset;
    dc->realize = tc4x_clock_realize;
}

static const TypeInfo tc4x_clock_info = {
    .name = TYPE_TC4X_CLOCK,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(TC4xClockState),
    .instance_init = tc4x_clock_init,
    .class_init = tc4x_clock_class_init,
};

static void tc4x_clock_register_types(void)
{
    type_register_static(&tc4x_clock_info);
}

type_init(tc4x_clock_register_types)
