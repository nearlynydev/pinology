/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Read-only RTD1619B OTP boot subset, derived from Synology rtk-efuse.c.
 * Reference-source copyright (C) 2016-2020 Realtek Semiconductor Corporation;
 * author Cheng-Yu Lee <cylee12@realtek.com>. See NOTICE.md.
 * Default contents are synthetic zeroes, not a physical DS223 fuse dump.
 * Only the known power-saving command is implemented; programming is rejected.
 * See OTP.md for the limits of this register-level model.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"

#define TYPE_RTD1619B_OTP "rtd1619b-otp"
OBJECT_DECLARE_SIMPLE_TYPE(RtdOtpState, RTD1619B_OTP)

#define OTP_SIZE 0x1000
#define OTP_CTRL 0x00
#define OTP_CTRL_ST 0x04
#define OTP_TM_ST 0x14
#define OTP_POWERSAVING 0x0c00c000

struct RtdOtpState {
    SysBusDevice parent_obj;
    MemoryRegion data, control;
    uint8_t contents[OTP_SIZE];
    uint32_t command;
    char *otp_file;
};

static MemTxResult otp_read(void *opaque, hwaddr a, uint64_t *value,
                            unsigned size, MemTxAttrs attrs)
{
    RtdOtpState *s = opaque;

    *value = 0;
    if (a >= OTP_SIZE || size > OTP_SIZE - a) {
        return MEMTX_ERROR;
    }
    for (unsigned i = 0; i < size; i++) {
        *value |= (uint64_t)s->contents[a + i] << (i * 8);
    }
    /* Addresses only: an operator-supplied profile may contain private data. */
    qemu_log_mask(LOG_UNIMP, "rtd1619b-otp: data read +0x%" HWADDR_PRIx
                  " size=%u\n", a, size);
    return MEMTX_OK;
}

static MemTxResult otp_write(void *opaque, hwaddr a, uint64_t value,
                             unsigned size, MemTxAttrs attrs)
{
    qemu_log_mask(LOG_UNIMP, "rtd1619b-otp: rejected read-only data write "
                  "+0x%" HWADDR_PRIx " size=%u\n", a, size);
    return MEMTX_ERROR;
}

static MemTxResult otp_control_read(void *opaque, hwaddr a, uint64_t *value,
                                    unsigned size, MemTxAttrs attrs)
{
    RtdOtpState *s = opaque;

    switch (a) {
    case OTP_CTRL:
        /* Modeled command readback, not a measured silicon reset value. */
        *value = s->command;
        return MEMTX_OK;
    case OTP_CTRL_ST:
    case OTP_TM_ST:
        /* Idle; never advertise programming success (TM_ST[9:8] == 1). */
        *value = 0;
        return MEMTX_OK;
    default:
        *value = 0;
        qemu_log_mask(LOG_UNIMP, "rtd1619b-otp: unsupported control read "
                      "+0x%" HWADDR_PRIx "\n", a);
        return MEMTX_ERROR;
    }
}

static MemTxResult otp_control_write(void *opaque, hwaddr a, uint64_t value,
                                     unsigned size, MemTxAttrs attrs)
{
    RtdOtpState *s = opaque;

    if (a == OTP_CTRL && value == OTP_POWERSAVING) {
        s->command = value;
        qemu_log_mask(LOG_UNIMP,
                      "rtd1619b-otp: power-saving command accepted\n");
        return MEMTX_OK;
    }
    qemu_log_mask(LOG_UNIMP, "rtd1619b-otp: rejected control write "
                  "+0x%" HWADDR_PRIx " value=0x%08x\n", a, (uint32_t)value);
    return MEMTX_ERROR;
}

static const MemoryRegionOps otp_ops = {
    .read_with_attrs = otp_read,
    .write_with_attrs = otp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 4, .unaligned = true },
};

static const MemoryRegionOps otp_control_ops = {
    .read_with_attrs = otp_control_read,
    .write_with_attrs = otp_control_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void otp_reset(DeviceState *dev)
{
    RtdOtpState *s = RTD1619B_OTP(dev);

    s->command = 0;
    /* Fuse contents are immutable and survive controller reset. */
}

static void otp_realize(DeviceState *dev, Error **errp)
{
    RtdOtpState *s = RTD1619B_OTP(dev);
    g_autofree char *contents = NULL;
    g_autoptr(GError) err = NULL;
    gsize length;

    if (!s->otp_file) {
        qemu_log_mask(LOG_UNIMP,
                      "rtd1619b-otp: synthetic zero-filled profile\n");
        return;
    }
    if (!g_file_get_contents(s->otp_file, &contents, &length, &err)) {
        error_setg(errp, "rtd1619b-otp: cannot read otp-file: %s",
                   err->message);
        return;
    }
    if (length != OTP_SIZE) {
        error_setg(errp,
                   "rtd1619b-otp: otp-file must contain exactly 4096 bytes");
        return;
    }
    memcpy(s->contents, contents, OTP_SIZE);
}

static void otp_init(Object *obj)
{
    RtdOtpState *s = RTD1619B_OTP(obj);

    memory_region_init_io(&s->data, obj, &otp_ops, s, "rtd1619b-otp-data",
                          OTP_SIZE);
    memory_region_init_io(&s->control, obj, &otp_control_ops, s,
                          "rtd1619b-otp-control", 0x400);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->data);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->control);
}

static const VMStateDescription otp_vmstate = {
    .name = TYPE_RTD1619B_OTP,
    .version_id = 1, .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(contents, RtdOtpState, OTP_SIZE),
        VMSTATE_UINT32(command, RtdOtpState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property otp_properties[] = {
    DEFINE_PROP_STRING("otp-file", RtdOtpState, otp_file),
};

static void otp_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = otp_realize;
    dc->vmsd = &otp_vmstate;
    device_class_set_props(dc, otp_properties);
    device_class_set_legacy_reset(dc, otp_reset);
}

static const TypeInfo otp_type = {
    .name = TYPE_RTD1619B_OTP, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RtdOtpState), .instance_init = otp_init,
    .class_init = otp_class_init,
};

static void otp_register_types(void)
{
    type_register_static(&otp_type);
}
type_init(otp_register_types)
