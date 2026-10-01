/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * SB2 invalid-access register subset and read-only RTD1619B A00 identity.
 * Derived from rtk_sb2_inv.c and chip.c. No global bus-fault interception:
 * unknown MMIO still faults rather than being swallowed by this device.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"

#define TYPE_RTD_SB2 "rtd1619b-sb2"
OBJECT_DECLARE_SIMPLE_TYPE(RtdSb2State, RTD_SB2)
struct RtdSb2State {
    SysBusDevice parent_obj;
    MemoryRegion inv, chip;
    qemu_irq irq;
    uint32_t enable, status, timeout;
    MemoryRegion misc;
    qemu_irq misc_irq[3];
    uint32_t misc_inputs, misc_enable, sda[3];
    MemoryRegion sfc, nor, sync;
    uint32_t sfc_regs[11];
    MemoryRegion boot_scratch;
    uint32_t boot_scratch_value;
};

/* SB2+0xd00 is a firmware handoff word read by galcore's HIFI workaround.
 * The driver compares it with 0xdeaddead; do not force that bypass sentinel.
 * This direct-boot profile starts the scratch word at zero, with real R/W
 * storage. It does not represent NPU computation or completion status.
 */
static uint64_t boot_scratch_read(void *opaque, hwaddr a, unsigned size)
{
    return RTD_SB2(opaque)->boot_scratch_value;
}
static void boot_scratch_write(void *opaque, hwaddr a, uint64_t v,
                               unsigned size)
{
    RTD_SB2(opaque)->boot_scratch_value = v;
}
static const MemoryRegionOps boot_scratch_ops = {
    .read = boot_scratch_read, .write = boot_scratch_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static uint64_t sfc_read(void *opaque, hwaddr a, unsigned size)
{
    return RTD_SB2(opaque)->sfc_regs[a / 4];
}
static void sfc_write(void *opaque, hwaddr a, uint64_t v, unsigned size)
{
    RTD_SB2(opaque)->sfc_regs[a / 4] = v;
}
static const MemoryRegionOps sfc_ops = {
    .read = sfc_read, .write = sfc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};
static uint64_t nor_read(void *opaque, hwaddr a, unsigned size)
{
    /* Unpopulated SPI chip select: MISO pulled high, JEDEC ID all FF. */
    return size == 4 ? UINT32_MAX : (1ULL << (size * 8)) - 1;
}
static MemTxResult nor_write(void *opaque, hwaddr a, uint64_t v,
                             unsigned size, MemTxAttrs attrs)
{
    return MEMTX_ERROR; /* No writable NOR backing or host file. */
}
static const MemoryRegionOps nor_ops = {
    .read = nor_read, .write_with_attrs = nor_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};
static uint64_t sync_read(void *opaque, hwaddr a, unsigned size)
{
    return 0; /* No outstanding posted SFC transaction. */
}
static void sync_write(void *opaque, hwaddr a, uint64_t v, unsigned size)
{
    /* All modeled register transactions are synchronous. */
}
static const MemoryRegionOps sync_ops = {
    .read = sync_read, .write = sync_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void misc_update(RtdSb2State *s)
{
    uint32_t active = 0;
    static const uint8_t sources[] = {3, 5, 8, 13, 14, 15, 23, 24, 25, 27, 29};
    static const uint8_t enables[] = {3, 5, 7, 6, 14, 15, 28, 24, 25, 27, 29};
    for (unsigned i = 0; i < ARRAY_SIZE(sources); i++) {
        if ((s->misc_inputs & (1U << sources[i])) &&
            (s->misc_enable & (1U << enables[i]))) {
            active |= 1U << sources[i];
        }
    }
    qemu_set_irq(s->misc_irq[0], !!(active & 0xffffded6));
    qemu_set_irq(s->misc_irq[1], !!(active & 0x28));
    qemu_set_irq(s->misc_irq[2], !!(active & 0x2100));
}
static void misc_input(void *opaque, int source, int level)
{
    RtdSb2State *s = opaque;
    s->misc_inputs = (s->misc_inputs & ~(1U << source)) |
                     (level ? 1U << source : 0);
    misc_update(s);
}
static MemTxResult misc_read(void *opaque, hwaddr a, uint64_t *v,
                             unsigned size, MemTxAttrs attrs)
{
    RtdSb2State *s = opaque;
    *v = 0;
    if (a == 8 || a == 0x80) {
        *v = s->misc_enable;
    } else if (a == 0 || a == 0xc) {
        *v = s->misc_inputs;
    } else if (a >= 0x90 && a <= 0x98) {
        *v = s->sda[(a - 0x90) / 4];
    } else {
        return MEMTX_ERROR;
    }
    return MEMTX_OK;
}
static MemTxResult misc_write(void *opaque, hwaddr a, uint64_t v,
                              unsigned size, MemTxAttrs attrs)
{
    RtdSb2State *s = opaque;
    if (a == 8) {
        /* Per-source unmask/ACK must preserve sibling enables, matching
         * the ISO compatibility subset. See GPIO.md for its limits. */
        s->misc_enable |= v;
    } else if (a == 0x80) {
        s->misc_enable = v;
    } else if (a == 0 || a == 0xc) {
        /* Parent ACK: level remains until the child clears its source. */
    } else if (a >= 0x90 && a <= 0x98) {
        s->sda[(a - 0x90) / 4] = v;
    } else {
        return MEMTX_ERROR;
    }
    misc_update(s);
    return MEMTX_OK;
}
static const MemoryRegionOps misc_ops = {
    .read_with_attrs = misc_read, .write_with_attrs = misc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void sb2_update(RtdSb2State *s)
{
    static const unsigned masks[] = {2, 4, 8, 0x40, 0x100, 0x400};
    bool level = false;
    for (unsigned i = 0; i < ARRAY_SIZE(masks); i++) {
        level |= !!((s->status & (2U << i)) && (s->enable & masks[i]));
    }
    qemu_set_irq(s->irq, level);
}

static uint64_t sb2_read(void *opaque, hwaddr a, unsigned size)
{
    RtdSb2State *s = opaque;
    switch (a) {
    case 0: return s->enable;
    case 4: return s->status;
    case 8: return 0; /* No bus master fault-address input yet. */
    case 12: return s->timeout;
    default:
        g_assert_not_reached();
    }
}

static void sb2_write(void *opaque, hwaddr a, uint64_t v, unsigned size)
{
    RtdSb2State *s = opaque;
    uint32_t *reg = a == 0 ? &s->enable : &s->status;
    uint32_t mask = a == 0 ? 0x54e : 0x7e;
    switch (a) {
    case 0:
    case 4:
        if (v & 1) {
            *reg |= v & mask;
        } else {
            *reg &= ~(v & mask);
        }
        sb2_update(s);
        break;
    case 8:
        break; /* Read-only invalid address. */
    case 12:
        s->timeout = v;
        break;
    }
}

static uint64_t chip_read(void *opaque, hwaddr a, unsigned size)
{
    return a == 0 ? 0x6698 : 0; /* Stark, A00; see vendor chip.c tables. */
}

static void chip_write(void *opaque, hwaddr a, uint64_t v, unsigned size)
{
    qemu_log_mask(LOG_GUEST_ERROR, "rtd1619b-sb2: chip ID is read-only\n");
}

static const MemoryRegionOps sb2_ops = {
    .read = sb2_read, .write = sb2_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};
static const MemoryRegionOps chip_ops = {
    .read = chip_read, .write = chip_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void sb2_reset(DeviceState *dev)
{
    RtdSb2State *s = RTD_SB2(dev);
    s->enable = s->status = s->timeout = 0;
    s->misc_enable = 0;
    s->boot_scratch_value = 0;
    memset(s->sda, 0, sizeof(s->sda));
    memset(s->sfc_regs, 0, sizeof(s->sfc_regs));
    sb2_update(s);
    misc_update(s);
}
static int sb2_post_load(void *opaque, int version)
{
    sb2_update(opaque);
    misc_update(opaque);
    return 0;
}
static const VMStateDescription sb2_vmstate = {
    .name = TYPE_RTD_SB2, .version_id = 4, .minimum_version_id = 4,
    .post_load = sb2_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(enable, RtdSb2State),
        VMSTATE_UINT32(status, RtdSb2State),
        VMSTATE_UINT32(timeout, RtdSb2State),
        VMSTATE_UINT32(misc_inputs, RtdSb2State),
        VMSTATE_UINT32(misc_enable, RtdSb2State),
        VMSTATE_UINT32_ARRAY(sda, RtdSb2State, 3),
        VMSTATE_UINT32_ARRAY(sfc_regs, RtdSb2State, 11),
        VMSTATE_UINT32(boot_scratch_value, RtdSb2State),
        VMSTATE_END_OF_LIST()
    }
};
static void sb2_init(Object *obj)
{
    RtdSb2State *s = RTD_SB2(obj);
    memory_region_init_io(&s->inv, obj, &sb2_ops, s, "rtd-sb2-inv", 16);
    memory_region_init_io(&s->chip, obj, &chip_ops, s, "rtd-chip", 8);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->inv);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->chip);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    memory_region_init_io(&s->misc, obj, &misc_ops, s, "rtd-misc-irq", 0x100);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->misc);
    for (unsigned i = 0; i < 3; i++) {
        sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->misc_irq[i]);
    }
    qdev_init_gpio_in_named(DEVICE(obj), misc_input, "misc-source", 32);
    memory_region_init_io(&s->sfc, obj, &sfc_ops, s, "rtd-sfc", 0x2c);
    memory_region_init_io(&s->nor, obj, &nor_ops, s,
                          "rtd-nor-absent", 0x2000000);
    memory_region_init_io(&s->sync, obj, &sync_ops, s, "rtd-sfc-sync", 4);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->sfc);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->nor);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->sync);
    memory_region_init_io(&s->boot_scratch, obj, &boot_scratch_ops, s,
                         "rtd-sb2-boot-scratch", 4);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->boot_scratch);
}
static void sb2_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->vmsd = &sb2_vmstate;
    device_class_set_legacy_reset(dc, sb2_reset);
}
static const TypeInfo sb2_type = {
    .name = TYPE_RTD_SB2, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RtdSb2State), .instance_init = sb2_init,
    .class_init = sb2_class_init,
};
static void sb2_register_types(void)
{
    type_register_static(&sb2_type);
}
type_init(sb2_register_types)
