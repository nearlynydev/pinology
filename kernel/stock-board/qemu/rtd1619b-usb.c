/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Realtek wrapper/PHY protocol around upstream DWC3 and real xHCI DMA.
 */
#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/usb/hcd-dwc3.h"
#include "migration/vmstate.h"

#define TYPE_RTD_USB "rtd1619b-usb"
OBJECT_DECLARE_SIMPLE_TYPE(RtdUsbState, RTD_USB)
struct RtdUsbState {
    SysBusDevice parent_obj;
    USBDWC3 dwc;
    MemoryRegion wrapper, core, host, globals, phyacc, device_regs;
    uint32_t regs[0x200 / 4];
    uint16_t phy3[256];
    uint8_t phy2[3 * 256];
    uint32_t acc, dctl, imod;
    uint8_t nibble, phase, page;
};
static bool usb_known(hwaddr a)
{
    return a == 0 || a == 0xc || a == 0x10 || a == 0x14 ||
           a == 0x60 || a == 0x70 ||
           a == 0x128 || a == 0x160 || a == 0x164;
}
static MemTxResult usb_read(void *opaque, hwaddr a, uint64_t *v,
                            unsigned size, MemTxAttrs attrs)
{
    /* Vendor debugfs snapshots all wrapper words, including reserved ones. */
    *v = RTD_USB(opaque)->regs[a / 4];
    return MEMTX_OK;
}
static MemTxResult usb_write(void *opaque, hwaddr a, uint64_t v,
                             unsigned size, MemTxAttrs attrs)
{
    RtdUsbState *s = RTD_USB(opaque);
    if (!usb_known(a)) {
        return MEMTX_ERROR;
    }
    if (a == 0x10) {
        unsigned reg = (v >> 8) & 0xff;
        if (v & 1) {
            s->phy3[reg] = v >> 16;
        }
        v = (v & 0xff7f) | ((uint32_t)s->phy3[reg] << 16);
    }
    s->regs[a / 4] = v;
    return MEMTX_OK;
}
static uint64_t usb_phy_read(void *opaque, hwaddr a, unsigned size)
{
    return RTD_USB(opaque)->acc;
}
static void usb_phy_write(void *opaque, hwaddr a, uint64_t v, unsigned size)
{
    RtdUsbState *s = RTD_USB(opaque);
    unsigned nibble = (v >> 8) & 0xf;
    s->acc = v & ~BIT(23); /* Synchronous transaction, not analog timing. */
    if (!(v & BIT(25))) {
        return;
    }
    if (!s->phase) {
        s->nibble = nibble;
        s->phase = 1;
    } else {
        unsigned addr = (nibble << 4) | s->nibble;
        s->phase = 0;
        /* Realtek reads use address minus 0x20; writes use e0..ff. */
        if (addr >= 0xe0) {
            uint8_t data = s->regs[0x14 / 4];
            if (addr == 0xf4) {
                unsigned page = (data >> 5) & 3;
                s->page = page < 3 ? page : 0;
            }
            s->phy2[s->page * 256 + addr] = data;
        } else {
            s->acc |= s->phy2[s->page * 256 + ((addr + 0x20) & 0xff)];
        }
    }
}
static const MemoryRegionOps usb_ops = {
    .read_with_attrs = usb_read, .write_with_attrs = usb_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};
static const MemoryRegionOps usb_phy_ops = {
    .read = usb_phy_read, .write = usb_phy_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};
/*
 * Host-only synthesis: no gadget endpoints. The vendor probes DCTL reset
 * before selecting host mode and debugfs snapshots the remaining registers.
 */
static uint64_t usb_device_read(void *opaque, hwaddr a, unsigned size)
{
    RtdUsbState *s = RTD_USB(opaque);
    return a == 4 ? s->dctl : a == 0x300 ? s->imod : 0;
}
static MemTxResult usb_device_write(void *opaque, hwaddr a, uint64_t v,
                                    unsigned size, MemTxAttrs attrs)
{
    if (a == 4 && !(v & BIT(31))) {
        RTD_USB(opaque)->dctl = v & ~(BIT(30) | BIT(29));
        return MEMTX_OK;
    }
    if (a == 0x300) {
        /* Device IRQ moderation interval; no gadget events in host mode. */
        RTD_USB(opaque)->imod = v & 0xffff;
        return MEMTX_OK;
    }
    return MEMTX_ERROR; /* Gadget run/stop and endpoint commands unsupported. */
}
static const MemoryRegionOps usb_device_ops = {
    .read = usb_device_read, .write_with_attrs = usb_device_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};
static void usb_reset(DeviceState *dev)
{
    RtdUsbState *s = RTD_USB(dev);
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->phy2, 0, sizeof(s->phy2));
    memset(s->phy3, 0, sizeof(s->phy3));
    s->acc = s->dctl = s->imod = s->nibble = s->phase = s->page = 0;
}
static void usb_realize(DeviceState *dev, Error **errp)
{
    RtdUsbState *s = RTD_USB(dev);
    SysBusDevice *dwc = SYS_BUS_DEVICE(&s->dwc);
    if (!sysbus_realize(dwc, errp)) {
        return;
    }
    MemoryRegion *mr = sysbus_mmio_get_region(dwc, 0);
    memory_region_init_alias(&s->host, OBJECT(s), "rtd-usb-host",
                             mr, 0, 0x8000);
    memory_region_init_alias(&s->globals, OBJECT(s), "rtd-usb-globals",
                             mr, 0xc100, 0x600);
    memory_region_add_subregion(&s->core, 0, &s->host);
    memory_region_add_subregion(&s->core, 0x8100, &s->globals);
    memory_region_add_subregion(&s->core, 0x8700, &s->device_regs);
    memory_region_add_subregion_overlap(&s->core, 0x8280, &s->phyacc, 1);
    sysbus_pass_irq(SYS_BUS_DEVICE(s), SYS_BUS_DEVICE(&s->dwc.sysbus_xhci));
}
static void usb_init(Object *obj)
{
    RtdUsbState *s = RTD_USB(obj);
    object_initialize_child(obj, "dwc3", &s->dwc, TYPE_USB_DWC3);
    qdev_prop_set_uint32(DEVICE(&s->dwc), "intrs", 1);
    qdev_prop_set_uint32(DEVICE(&s->dwc), "slots", 8);
    memory_region_init_io(&s->wrapper, obj, &usb_ops, s, "rtd-usb-wrap", 0x200);
    memory_region_init(&s->core, obj, "rtd-usb-core", 0x9000);
    memory_region_init_io(&s->phyacc, obj, &usb_phy_ops, s, "rtd-usb-phy", 4);
    memory_region_init_io(&s->device_regs, obj, &usb_device_ops, s,
                          "rtd-usb-host-only", 0x900);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->wrapper);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->core);
}
static const VMStateDescription usb_vmstate = {
    .name = TYPE_RTD_USB, .version_id = 1, .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, RtdUsbState, 0x200 / 4),
        VMSTATE_UINT16_ARRAY(phy3, RtdUsbState, 256),
        VMSTATE_UINT8_ARRAY(phy2, RtdUsbState, 3 * 256),
        VMSTATE_UINT32(acc, RtdUsbState),
        VMSTATE_UINT32(dctl, RtdUsbState),
        VMSTATE_UINT32(imod, RtdUsbState),
        VMSTATE_UINT8(nibble, RtdUsbState),
        VMSTATE_UINT8(phase, RtdUsbState),
        VMSTATE_UINT8(page, RtdUsbState),
        VMSTATE_END_OF_LIST()
    }
};
static void usb_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->vmsd = &usb_vmstate;
    dc->realize = usb_realize;
    device_class_set_legacy_reset(dc, usb_reset);
}
static const TypeInfo usb_type = {
    .name = TYPE_RTD_USB, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RtdUsbState), .instance_init = usb_init,
    .class_init = usb_class_init,
};
static void usb_register_types(void)
{
    type_register_static(&usb_type);
}
type_init(usb_register_types)
