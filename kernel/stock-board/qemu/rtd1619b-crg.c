/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * RTD1619B boot-time clock/reset register model.
 * Reference-source credits: Copyright (C) 2017-2020 Realtek Semiconductor
 * Corporation; author Cheng-Yu Lee <cylee12@realtek.com>. See NOTICE.md.
 * Register behavior is derived from Synology's GPL Realtek clock drivers:
 * clk-rtd1619b-{cc,ic}.c, clk-pll.c, clk-det.c, clk-regmap-gate.c,
 * reset.c and rtk_sb2_sem.c. See CLOCKS.md for evidence and limits.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "system/runstate.h"

#define TYPE_RTD1619B_CRG "rtd1619b-crg"
OBJECT_DECLARE_SIMPLE_TYPE(RtdCrgState, RTD1619B_CRG)

#define REG(s, a) ((s)->regs[(a) / 4])
#define NF(n, f) (((n) << 11) | (f))
#define MNO(m, n, o) (((m) << 4) | ((n) << 12) | ((o) << 17))
#define LOCK_BIT (1U << 20)
/* Functional model latency, not measured silicon. */
#define LOCK_DELAY_NS 50000

static const uint16_t ssc[] = {0x500, 0x520, 0x540, 0x560, 0x5a0, 0x6b0, 0x6e0};
static const uint16_t detectors[] = {
    0x424, 0x428, 0x42c, 0x430, 0x438, 0x43c, 0x440, 0x444, 0x448
};
static const uint16_t detector_pll[] = {
    0x524, 0x544, 0, 0x564, 0x5a4, 0x114, 0x1d0, 0x6b4, 0x6e4
};

typedef struct RtdRate { uint16_t reg; uint32_t value; uint32_t hz; } RtdRate;
#define RATE(r, n, f, hz) {r, NF(n, f), hz}
static const RtdRate rates[] = {
    RATE(0x524, 0x1f, 0, 459000000), RATE(0x524, 0x21, 0, 486000000),
    RATE(0x524, 0x22, 0, 499500000), RATE(0x524, 0x23, 0, 594000000),
    RATE(0x544, 0x1a, 0, 351000000), RATE(0x544, 0x1b, 0, 405000000),
    RATE(0x544, 0x1f, 0, 459000000), RATE(0x544, 0x1f, 0x7b4, 472000000),
    RATE(0x544, 0x22, 0, 499500000), RATE(0x544, 0x27, 0, 567000000),
    RATE(0x544, 0x29, 0, 594000000), RATE(0x564, 29, 0, 432000000),
    RATE(0x5a4, 0x1a, 0x509, 400000000), RATE(0x5a4, 0x1e, 0x2aa, 450000000),
    RATE(0x5a4, 0x22, 0x04b, 500000000), RATE(0x5a4, 0x25, 0x5ed, 550000000),
    RATE(0x5a4, 0x2d, 0x12f, 650000000), RATE(0x5a4, 0x31, 0, 702000000),
    RATE(0x5a4, 0x33, 0, 729000000), RATE(0x5a4, 0x36, 0, 769000000),
    RATE(0x5a4, 0x39, 0, 810000000), RATE(0x5a4, 0x3b, 0, 837000000),
    RATE(0x5a4, 0x3c, 0, 850500000),
    RATE(0x6b4, 0x29, 0x38e, 600000000), RATE(0x6b4, 0x30, 0x6d0, 700000000),
    RATE(0x6b4, 0x33, 0, 729000000), RATE(0x6b4, 0x35, 0, 756000000),
    RATE(0x6b4, 0x3c, 0, 850500000),
    RATE(0x6e4, 0x1b, 0, 405000000), RATE(0x6e4, 0x21, 0, 486000000),
    RATE(0x6e4, 0x22, 0, 499500000), RATE(0x6e4, 0x31, 0, 702000000),
    RATE(0x6e4, 0x38, 0, 796500000), RATE(0x6e4, 0x39, 0, 810000000),
};
static const RtdRate ve_rates[] = {
    {0, MNO(0x0d, 0, 0), 432000000}, {0, MNO(0x11, 0, 0), 540000000},
    {0, MNO(0x26, 1, 0), 553000000}, {0, MNO(0x14, 0, 0), 621000000},
    {0, MNO(0x2b, 1, 0), 621000000}, {0, MNO(0x2d, 1, 0), 648000000},
    {0, MNO(0x2f, 1, 0), 675000000},
};

struct RtdCrgState {
    SysBusDevice parent_obj;
    MemoryRegion crt, iso, sem;
    uint32_t regs[0x1000 / 4];
    uint32_t iso_reset, iso_gate;
    bool sem_owned;
    int64_t ready_at[ARRAY_SIZE(ssc)];
    MemoryRegion power, power_alias[11], usb_ldo;
    uint32_t power_regs[0x1000 / 4];
    int64_t power_ready[7];
    MemoryRegion scpu_detector;
    uint32_t scpu_det_ctrl, scpu_det_status;
    MemoryRegion sc_wrap, sc_acp;
    uint32_t sc_wrap_regs[0x128 / 4], sc_acp_ctrl;
    MemoryRegion thermal, thermal_latch;
    uint32_t thermal_ctrl[3], thermal_latch_ctrl;
    MemoryRegion wdt, wdt_status;
    uint32_t wdt_regs[6], wdt_reset_count;
    int64_t wdt_started;
    QEMUTimer *wdt_timeout, *wdt_pretimeout;
    qemu_irq wdt_irq;
    bool wdt_irq_level;
};

/* ISO SRAM domains from rtk_gpc.c; last entries are delay and isolation. */
static const unsigned power_offsets[] = {
    0xb00, 0xb20, 0xb60, 0x290, 0x238, 0x260, 0x3b0, 0x28c, 0xfd0,
    0x640, 0x678
};

static uint64_t power_read(void *opaque, hwaddr a, unsigned size)
{
    RtdCrgState *s = opaque;
    for (unsigned i = 0; i < 7; i++) {
        if (a == power_offsets[i] + 0x14 && s->power_ready[i] >= 0 &&
            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) >= s->power_ready[i]) {
            s->power_regs[a / 4] |= 4;
            s->power_ready[i] = -1;
        }
    }
    return s->power_regs[a / 4];
}

static void power_write(void *opaque, hwaddr a, uint64_t v, unsigned size)
{
    RtdCrgState *s = opaque;
    for (unsigned i = 0; i < 7; i++) {
        if (a == power_offsets[i] + 0x14) {
            s->power_regs[a / 4] &= ~(v & 4); /* Completion W1C. */
            return;
        }
        if (a == power_offsets[i] + 0x10) {
            s->power_regs[(a + 4) / 4] = 0;
            s->power_ready[i] = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000;
        }
    }
    s->power_regs[a / 4] = v;
}

static const MemoryRegionOps power_ops = {
    .read = power_read, .write = power_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static bool paired(hwaddr a)
{
    switch (a) {
    case 0: case 4: case 8: case 0xc: case 0x68: case 0x90:
    case 0x50: case 0x54: case 0x58: case 0x5c: case 0x8c:
        return true;
    default: return false;
    }
}

static void wdt_update(RtdCrgState *s)
{
    timer_del(s->wdt_timeout);
    timer_del(s->wdt_pretimeout);
    if ((s->wdt_regs[0] & 0xff) == 0xff) {
        timer_mod(s->wdt_timeout, s->wdt_started +
                  (uint64_t)s->wdt_regs[3] * 1000000000 / 27000000);
        if ((s->wdt_regs[0] & 0x80000000) && s->wdt_regs[2]) {
            timer_mod(s->wdt_pretimeout, s->wdt_started +
                      (uint64_t)s->wdt_regs[2] * 1000000000 / 27000000);
        }
    }
    if (!(s->wdt_regs[0] & 0x80000000)) {
        s->wdt_irq_level = false;
    }
    qemu_set_irq(s->wdt_irq, s->wdt_irq_level);
}

static void wdt_expired(void *opaque)
{
    RtdCrgState *s = opaque;
    s->wdt_reset_count = (s->wdt_reset_count + 1) & 15;
    qemu_system_reset_request(SHUTDOWN_CAUSE_GUEST_RESET);
}
static void wdt_preexpired(void *opaque)
{
    RtdCrgState *s = opaque;
    s->wdt_irq_level = true;
    qemu_set_irq(s->wdt_irq, 1);
}
static uint64_t wdt_read(void *opaque, hwaddr a, unsigned size)
{
    return a == 4 ? 0 : RTD1619B_CRG(opaque)->wdt_regs[a / 4];
}
static void wdt_write(void *opaque, hwaddr a, uint64_t v, unsigned size)
{
    RtdCrgState *s = opaque;
    bool starting = a == 0 && (v & 0xff) == 0xff &&
                    (s->wdt_regs[0] & 0xff) != 0xff;
    if (a == 4) {
        if (v & 1) {
            s->wdt_started = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
            s->wdt_irq_level = false;
        }
    } else {
        s->wdt_regs[a / 4] = v;
    }
    if (starting) {
        s->wdt_started = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    }
    wdt_update(s);
}
static MemTxResult wdt_status_read(void *opaque, hwaddr a, uint64_t *v,
                                   unsigned size, MemTxAttrs attrs)
{
    RtdCrgState *s = opaque;
    if (a == 0x18) {
        *v = 0; /* No modeled IP assertion causes. */
    } else if (a == 0x20) {
        *v = s->wdt_reset_count;
    } else {
        return MEMTX_ERROR;
    }
    return MEMTX_OK;
}
static const MemoryRegionOps wdt_ops = {
    .read = wdt_read, .write = wdt_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};
static MemTxResult wdt_status_write(void *opaque, hwaddr a, uint64_t v,
                                    unsigned size, MemTxAttrs attrs)
{
    return a == 0x18 || a == 0x20 ? MEMTX_OK : MEMTX_ERROR;
}
static const MemoryRegionOps wdt_status_ops = {
    .read_with_attrs = wdt_status_read,
    .write_with_attrs = wdt_status_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static MemTxResult thermal_read(void *opaque, hwaddr a, uint64_t *v,
                                unsigned size, MemTxAttrs attrs)
{
    RtdCrgState *s = opaque;
    if (a < 12) {
        *v = s->thermal_ctrl[a / 4];
    } else if (a == 0x40) {
        *v = 25 * 1024; /* Synthetic 25 C environment; signed Q10 format. */
    } else if (a == 0x44) {
        *v = 0;
    } else {
        return MEMTX_ERROR;
    }
    return MEMTX_OK;
}

static MemTxResult thermal_write(void *opaque, hwaddr a, uint64_t v,
                                 unsigned size, MemTxAttrs attrs)
{
    RtdCrgState *s = opaque;
    if (a < 12) {
        s->thermal_ctrl[a / 4] = v;
    } else if (a != 0x40 && a != 0x44) {
        return MEMTX_ERROR;
    }
    return MEMTX_OK;
}

static uint64_t thermal_latch_read(void *opaque, hwaddr a, unsigned size)
{
    return RTD1619B_CRG(opaque)->thermal_latch_ctrl;
}
static void thermal_latch_write(void *opaque, hwaddr a, uint64_t v,
                                unsigned size)
{
    RTD1619B_CRG(opaque)->thermal_latch_ctrl = v;
}
static const MemoryRegionOps thermal_ops = {
    .read_with_attrs = thermal_read, .write_with_attrs = thermal_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};
static const MemoryRegionOps thermal_latch_ops = {
    .read = thermal_latch_read, .write = thermal_latch_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

/* Clock gating controls used by rtk_sc_wrap.c; no CPU timing modulation. */
static MemTxResult sc_wrap_read(void *opaque, hwaddr a, uint64_t *v,
                                unsigned size, MemTxAttrs attrs)
{
    RtdCrgState *s = opaque;
    if (a != 0 && a != 0x24 && a != 0x30 && a != 0x100 && a != 0x124) {
        return MEMTX_ERROR;
    }
    *v = s->sc_wrap_regs[a / 4];
    return MEMTX_OK;
}

static MemTxResult sc_wrap_write(void *opaque, hwaddr a, uint64_t v,
                                 unsigned size, MemTxAttrs attrs)
{
    RtdCrgState *s = opaque;
    if (a != 0 && a != 0x24 && a != 0x30 && a != 0x100 && a != 0x124) {
        return MEMTX_ERROR;
    }
    s->sc_wrap_regs[a / 4] = v;
    return MEMTX_OK;
}

static const MemoryRegionOps sc_wrap_ops = {
    .read_with_attrs = sc_wrap_read, .write_with_attrs = sc_wrap_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

/* Vendor pcie-rtd.c acp_init configures the SCPU coherent DMA path.
 * QEMU DMA already shares coherent guest RAM; preserve configuration/readback,
 * without inventing cache timing or hardware completion indications. */
static uint64_t sc_acp_read(void *opaque, hwaddr a, unsigned size)
{
    return ((RtdCrgState *)opaque)->sc_acp_ctrl;
}
static void sc_acp_write(void *opaque, hwaddr a, uint64_t v, unsigned size)
{
    ((RtdCrgState *)opaque)->sc_acp_ctrl = v;
}
static const MemoryRegionOps sc_acp_ops = {
    .read = sc_acp_read, .write = sc_acp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static int detector_index(hwaddr a)
{
    for (unsigned i = 0; i < ARRAY_SIZE(detectors); i++) {
        if (a == detectors[i]) {
            return i;
        }
    }
    return -1;
}

static bool known(hwaddr a)
{
    if (paired(a) || detector_index(a) >= 0) {
        return true;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(ssc); i++) {
        if (a == ssc[i] || a == ssc[i] + 4 || a == ssc[i] + 0x1c) {
            return true;
        }
    }
    switch (a) {
    case 0x18: case 0x30: case 0x4c: case 0x108: case 0x1b0:
    case 0x114: case 0x118: case 0x120: case 0x124: case 0x128:
    case 0x130: case 0x134: case 0x1c0: case 0x1c4: case 0x1c8:
    case 0x1d0: case 0x1d4: case 0x1d8:
    case 0x454: case 0x458: case 0x464:
    case 0x624: case 0x628: case 0x62c: case 0x630:
    case 0x634: case 0x638: case 0x63c: case 0x640: case 0x644:
        return true;
    default: return false;
    }
}

static bool pll_ready(RtdCrgState *s, unsigned i)
{
    if (s->ready_at[i] >= 0 &&
        qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) >= s->ready_at[i]) {
        REG(s, ssc[i] + 0x1c) = LOCK_BIT;
        s->ready_at[i] = -1;
        qemu_log_mask(LOG_UNIMP, "rtd1619b-crg: PLL +0x%x locked\n", ssc[i]);
    }
    return REG(s, ssc[i] + 0x1c) & LOCK_BIT;
}

static MemTxResult scpu_det_read(void *opaque, hwaddr a, uint64_t *v,
                                 unsigned size, MemTxAttrs attrs)
{
    RtdCrgState *s = opaque;
    *v = a == 0 ? s->scpu_det_ctrl : s->scpu_det_status;
    return a == 0 || a == 8 ? MEMTX_OK : MEMTX_ERROR;
}
static MemTxResult scpu_det_write(void *opaque, hwaddr a, uint64_t v,
                                  unsigned size, MemTxAttrs attrs)
{
    RtdCrgState *s = opaque;
    if (a == 8) {
        return MEMTX_OK; /* Read-only sampled status. */
    }
    if (a != 0) {
        return MEMTX_ERROR;
    }
    s->scpu_det_ctrl = v;
    s->scpu_det_status = 0;
    if ((v & 0x30000) == 0x30000) {
        uint32_t nf = REG(s, 0x504);
        uint64_t hz = pll_ready(s, 0) ?
            (((nf >> 11) & 0xff) + 3ULL) * 27000000 +
            (nf & 0x7ff) * 27000000ULL / 2048 : 0;
        s->scpu_det_status = 1 | ((hz / 100000) << 1);
    }
    return MEMTX_OK;
}
static const MemoryRegionOps scpu_det_ops = {
    .read_with_attrs = scpu_det_read, .write_with_attrs = scpu_det_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static uint32_t detector_rate(RtdCrgState *s, int index)
{
    unsigned a = detector_pll[index];
    uint32_t val, rate = 0;
    /* No ACPU clock source is modeled: its detector reports no input edges. */
    if (!a) {
        return 0;
    }
    if (a == 0x114 || a == 0x1d0) {
        val = REG(s, a) & 0xe3ff0;
        if (!(REG(s, a + 4) & 1)) {
            return 0;
        }
        for (unsigned i = 0; i < ARRAY_SIZE(ve_rates); i++) {
            if (ve_rates[i].value == val) {
                return ve_rates[i].hz;
            }
        }
        return 0;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(ssc); i++) {
        if (a == ssc[i] + 4 && !pll_ready(s, i)) {
            return 0;
        }
    }
    val = REG(s, a) & 0x7ffff;
    for (unsigned i = 0; i < ARRAY_SIZE(rates); i++) {
        if (rates[i].reg == a && rates[i].value == val) {
            rate = rates[i].hz;
            break;
        }
    }
    unsigned pll = a == 0x564 ? 0x120 : a == 0x5a4 ? 0x1c0 :
                   a == 0x6b4 ? 0x1c8 : a == 0x6e4 ? 0x1d8 : 0;
    if (pll) {
        rate /= ((REG(s, pll) >> 22) & 7) + 1;
    }
    if (a == 0x564 && !(REG(s, 0x128) & 1)) {
        return 0;
    }
    if (a == 0x5a4 && !(REG(s, 0x1c4) & 1)) {
        return 0;
    }
    return rate;
}

static MemTxResult crt_read(void *opaque, hwaddr a, uint64_t *v,
                            unsigned size, MemTxAttrs attrs)
{
    RtdCrgState *s = opaque;
    if (!known(a)) {
        qemu_log_mask(LOG_UNIMP, "rtd1619b-crg: unsupported CRT read +0x%"
                      HWADDR_PRIx "\n", a);
        *v = 0;
        return MEMTX_ERROR;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(ssc); i++) {
        if (a == ssc[i] + 0x1c) {
            pll_ready(s, i);
        }
    }
    *v = REG(s, a);
    return MEMTX_OK;
}

static MemTxResult crt_write(void *opaque, hwaddr a, uint64_t value,
                             unsigned size, MemTxAttrs attrs)
{
    RtdCrgState *s = opaque;
    uint32_t v = value;
    if (!known(a)) {
        qemu_log_mask(LOG_UNIMP, "rtd1619b-crg: unsupported CRT write +0x%"
                      HWADDR_PRIx "\n", a);
        return MEMTX_ERROR;
    }
    if (paired(a)) {
        uint32_t mask = (v >> 1) & 0x55555555;
        REG(s, a) = (REG(s, a) & ~mask) | (v & mask);
        return MEMTX_OK;
    }
    int det = detector_index(a);
    if (det >= 0) {
        REG(s, a) = v & 3;
        if ((v & 3) == 3) {
            uint32_t count = detector_rate(s, det) / 100000;
            REG(s, a) |= (1U << 30) | ((count << 13) & 0x3fffc000);
        }
        return MEMTX_OK;
    }
    for (unsigned i = 0; i < ARRAY_SIZE(ssc); i++) {
        if (a == ssc[i] + 0x1c) {
            return MEMTX_OK; /* Read-only status. */
        }
        if (a == ssc[i]) {
            REG(s, a) = v;
            REG(s, a + 0x1c) = 0;
            s->ready_at[i] = (v & 7) == 5 ?
                qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + LOCK_DELAY_NS : -1;
            qemu_log_mask(LOG_UNIMP,
                          "rtd1619b-crg: PLL +0x%x ctrl=0x%x nf=0x%x\n",
                          ssc[i], v, REG(s, a + 4));
            return MEMTX_OK;
        }
        if (a == ssc[i] + 4) {
            REG(s, ssc[i] + 0x1c) = 0;
            s->ready_at[i] = -1;
        }
    }
    REG(s, a) = v;
    return MEMTX_OK;
}

static uint64_t iso_read(void *opaque, hwaddr a, unsigned size)
{
    RtdCrgState *s = opaque;
    return a == 0 ? s->iso_reset : s->iso_gate;
}
static void iso_write(void *opaque, hwaddr a, uint64_t v, unsigned size)
{
    RtdCrgState *s = opaque;
    if (a == 0) {
        s->iso_reset = v;
    } else {
        s->iso_gate = v;
    }
}
static uint64_t sem_read(void *opaque, hwaddr a, unsigned size)
{
    RtdCrgState *s = opaque;
    bool available = !s->sem_owned;
    s->sem_owned = true;
    return available;
}
static void sem_write(void *opaque, hwaddr a, uint64_t v, unsigned size)
{
    RtdCrgState *s = opaque;
    if (!v) {
        s->sem_owned = false;
    }
}

static const MemoryRegionOps crt_ops = {
    .read_with_attrs = crt_read, .write_with_attrs = crt_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};
static const MemoryRegionOps iso_ops = {
    .read = iso_read, .write = iso_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};
static const MemoryRegionOps sem_ops = {
    .read = sem_read, .write = sem_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void crg_reset(DeviceState *dev)
{
    RtdCrgState *s = RTD1619B_CRG(dev);
    memset(s->power_regs, 0, sizeof(s->power_regs));
    for (unsigned i = 0; i < 7; i++) {
        s->power_regs[(power_offsets[i] + 0x10) / 4] = 1;
        s->power_ready[i] = -1;
    }
    s->power_regs[0xfd0 / 4] = 0x740b;
    s->scpu_det_ctrl = s->scpu_det_status = 0;
    memset(s->sc_wrap_regs, 0, sizeof(s->sc_wrap_regs));
    s->sc_acp_ctrl = 0;
    memset(s->thermal_ctrl, 0, sizeof(s->thermal_ctrl));
    s->thermal_latch_ctrl = 0;
    memset(s->wdt_regs, 0, sizeof(s->wdt_regs));
    s->wdt_regs[0] = 0xa5;
    s->wdt_irq_level = false;
    wdt_update(s);
    memset(s->regs, 0, sizeof(s->regs));
    for (unsigned a = 0; a < 0x1000; a += 4) {
        if (paired(a)) {
            REG(s, a) = 0x55555555;
        }
    }
    for (unsigned i = 0; i < ARRAY_SIZE(ssc); i++) {
        REG(s, ssc[i]) = 5;
        REG(s, ssc[i] + 0x1c) = LOCK_BIT;
        s->ready_at[i] = -1;
    }
    /* Representative boot-firmware state, not measured silicon reset values. */
    REG(s, 0x504) = NF(60, 0);       /* SCPU 1.7 GHz; DT requests 1.6 GHz. */
    REG(s, 0x524) = NF(0x21, 0);     /* BUS 486 MHz */
    REG(s, 0x544) = NF(0x22, 0);     /* DCSB 499.5 MHz */
    REG(s, 0x564) = NF(29, 0);       /* DDSA 432 MHz */
    REG(s, 0x5a4) = NF(0x1a, 0x509); /* GPU 400 MHz */
    REG(s, 0x6b4) = NF(0x29, 0x38e); /* NPU 600 MHz */
    REG(s, 0x6e4) = NF(0x1b, 0);     /* HIFI 405 MHz */
    REG(s, 0x114) = REG(s, 0x1d0) = MNO(0xd, 0, 0);
    REG(s, 0x118) = REG(s, 0x128) = REG(s, 0x1c4) = REG(s, 0x1d4) = 3;
    REG(s, 0x134) = 5;
    REG(s, 0x454) = REG(s, 0x458) = REG(s, 0x464) = UINT32_MAX;
    s->iso_reset = UINT32_MAX;
    s->iso_gate = 0x7ffff;
    s->sem_owned = false;
}

static void crg_init(Object *obj)
{
    RtdCrgState *s = RTD1619B_CRG(obj);
    memory_region_init_io(&s->crt, obj, &crt_ops, s, "rtd1619b-crt", 0x1000);
    memory_region_init_io(&s->iso, obj, &iso_ops, s,
                          "rtd1619b-iso-clock-reset", 8);
    memory_region_init_io(&s->sem, obj, &sem_ops, s, "rtd1619b-sb2-lock", 4);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->crt);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iso);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->sem);
    memory_region_init_io(&s->power, obj, &power_ops, s, "rtd-sram", 0x1000);
    for (unsigned i = 0; i < ARRAY_SIZE(power_offsets); i++) {
        memory_region_init_alias(&s->power_alias[i], obj, "rtd-sram-window",
                                 &s->power, power_offsets[i],
                                 i < 7 ? 0x18 : i == 9 ? 8 : 4);
        sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->power_alias[i]);
    }
    memory_region_init_io(&s->scpu_detector, obj, &scpu_det_ops, s,
                          "rtd-scpu-clock-detector", 12);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->scpu_detector);
    memory_region_init_io(&s->sc_wrap, obj, &sc_wrap_ops, s,
                          "rtd-scpu-wrapper", 0x128);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->sc_wrap);
    memory_region_init_io(&s->thermal, obj, &thermal_ops, s,
                          "rtd-thermal", 0x48);
    memory_region_init_io(&s->thermal_latch, obj, &thermal_latch_ops, s,
                          "rtd-thermal-latch", 4);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->thermal);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->thermal_latch);
    s->wdt_timeout = timer_new_ns(QEMU_CLOCK_VIRTUAL, wdt_expired, s);
    s->wdt_pretimeout = timer_new_ns(QEMU_CLOCK_VIRTUAL, wdt_preexpired, s);
    memory_region_init_io(&s->wdt, obj, &wdt_ops, s, "rtd-watchdog", 0x18);
    memory_region_init_io(&s->wdt_status, obj, &wdt_status_ops, s,
                          "rtd-watchdog-status", 0x24);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->wdt);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->wdt_status);
    memory_region_init_alias(&s->usb_ldo, obj, "rtd-usb-ldo",
                             &s->power, 0xfb0, 4);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->usb_ldo);
    memory_region_init_io(&s->sc_acp, obj, &sc_acp_ops, s, "rtd-scpu-acp", 4);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->sc_acp);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->wdt_irq);
}

static int crg_post_load(void *opaque, int version_id)
{
    RtdCrgState *s = opaque;
    qemu_set_irq(s->wdt_irq, s->wdt_irq_level);
    return 0;
}

static void crg_finalize(Object *obj)
{
    RtdCrgState *s = RTD1619B_CRG(obj);
    timer_free(s->wdt_timeout);
    timer_free(s->wdt_pretimeout);
}

static const VMStateDescription crg_vmstate = {
    .name = TYPE_RTD1619B_CRG,
    .version_id = 7, .minimum_version_id = 7,
    .post_load = crg_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, RtdCrgState, 0x1000 / 4),
        VMSTATE_UINT32(iso_reset, RtdCrgState),
        VMSTATE_UINT32(iso_gate, RtdCrgState),
        VMSTATE_BOOL(sem_owned, RtdCrgState),
        VMSTATE_INT64_ARRAY(ready_at, RtdCrgState, 7),
        VMSTATE_UINT32_ARRAY(power_regs, RtdCrgState, 0x1000 / 4),
        VMSTATE_INT64_ARRAY(power_ready, RtdCrgState, 7),
        VMSTATE_UINT32(scpu_det_ctrl, RtdCrgState),
        VMSTATE_UINT32(scpu_det_status, RtdCrgState),
        VMSTATE_UINT32_ARRAY(sc_wrap_regs, RtdCrgState, 0x128 / 4),
        VMSTATE_UINT32(sc_acp_ctrl, RtdCrgState),
        VMSTATE_UINT32_ARRAY(thermal_ctrl, RtdCrgState, 3),
        VMSTATE_UINT32(thermal_latch_ctrl, RtdCrgState),
        VMSTATE_UINT32_ARRAY(wdt_regs, RtdCrgState, 6),
        VMSTATE_UINT32(wdt_reset_count, RtdCrgState),
        VMSTATE_INT64(wdt_started, RtdCrgState),
        VMSTATE_BOOL(wdt_irq_level, RtdCrgState),
        VMSTATE_TIMER_PTR(wdt_timeout, RtdCrgState),
        VMSTATE_TIMER_PTR(wdt_pretimeout, RtdCrgState),
        VMSTATE_END_OF_LIST()
    }
};
static void crg_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->vmsd = &crg_vmstate;
    device_class_set_legacy_reset(dc, crg_reset);
}
static const TypeInfo crg_type = {
    .name = TYPE_RTD1619B_CRG, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RtdCrgState), .instance_init = crg_init,
    .instance_finalize = crg_finalize,
    .class_init = crg_class_init,
};
static void crg_register_types(void)
{
    type_register_static(&crg_type);
}
type_init(crg_register_types)
