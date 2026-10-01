/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Realtek SATA wrapper/MDIO protocol; analog calibration is not modeled. */
#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"

#define TYPE_RTD_SATA "rtd1619b-sata-phy"
OBJECT_DECLARE_SIMPLE_TYPE(RtdSataState, RTD_SATA)
struct RtdSataState {
    SysBusDevice parent_obj;
    MemoryRegion mmio, phyctl;
    uint32_t regs[0x100 / 4], ctl;
    uint16_t mdio[2 * 3 * 64];
};

static bool sata_known(hwaddr a)
{
    switch (a) {
    case 0x18: case 0x20: case 0x28: case 0x2c:
    case 0x60: case 0x64: case 0x68: case 0xf0:
        return true;
    default:
        return false;
    }
}

static MemTxResult sata_read(void *opaque, hwaddr a, uint64_t *v,
                             unsigned size, MemTxAttrs attrs)
{
    RtdSataState *s = opaque;
    if (!sata_known(a)) {
        return MEMTX_ERROR;
    }
    *v = s->regs[a / 4];
    return MEMTX_OK;
}

static MemTxResult sata_write(void *opaque, hwaddr a, uint64_t v,
                              unsigned size, MemTxAttrs attrs)
{
    RtdSataState *s = opaque;
    if (!sata_known(a)) {
        return MEMTX_ERROR;
    }
    if (a == 0x64 && v > 1) {
        return MEMTX_ERROR;
    }
    if (a == 0x60 && (v & 0x10)) {
        unsigned gen = (v >> 14) & 3;
        unsigned reg = (v >> 8) & 63;
        unsigned port = s->regs[0x64 / 4];
        if (gen > 2) {
            return MEMTX_ERROR;
        }
        unsigned index = (port * 3 + gen) * 64 + reg;
        if (v & 1) {
            s->mdio[index] = v >> 16;
        } else {
            v = (v & 0xffff) | ((uint32_t)s->mdio[index] << 16);
        }
        v &= ~0x80U; /* Untimed transaction completed, MDIO not busy. */
    }
    s->regs[a / 4] = v;
    return MEMTX_OK;
}

static uint64_t sata_ctl_read(void *opaque, hwaddr a, unsigned size)
{
    return RTD_SATA(opaque)->ctl;
}

static void sata_ctl_write(void *opaque, hwaddr a, uint64_t v, unsigned size)
{
    RTD_SATA(opaque)->ctl = v;
}

static const MemoryRegionOps sata_ops = {
    .read_with_attrs = sata_read, .write_with_attrs = sata_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};
static const MemoryRegionOps sata_ctl_ops = {
    .read = sata_ctl_read, .write = sata_ctl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void sata_reset(DeviceState *dev)
{
    RtdSataState *s = RTD_SATA(dev);
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->mdio, 0, sizeof(s->mdio));
    s->ctl = 0;
}

static void sata_init(Object *obj)
{
    RtdSataState *s = RTD_SATA(obj);
    memory_region_init_io(&s->mmio, obj, &sata_ops, s, "rtd-sata", 0x100);
    memory_region_init_io(&s->phyctl, obj, &sata_ctl_ops, s, "rtd-sata-ctl", 4);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->phyctl);
}

static const VMStateDescription sata_vmstate = {
    .name = TYPE_RTD_SATA, .version_id = 1, .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, RtdSataState, 0x100 / 4),
        VMSTATE_UINT16_ARRAY(mdio, RtdSataState, 2 * 3 * 64),
        VMSTATE_UINT32(ctl, RtdSataState),
        VMSTATE_END_OF_LIST()
    }
};

static void sata_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->vmsd = &sata_vmstate;
    device_class_set_legacy_reset(dc, sata_reset);
}

static const TypeInfo sata_type = {
    .name = TYPE_RTD_SATA, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RtdSataState), .instance_init = sata_init,
    .class_init = sata_class_init,
};

static void sata_register_types(void)
{
    type_register_static(&sata_type);
}
type_init(sata_register_types)
