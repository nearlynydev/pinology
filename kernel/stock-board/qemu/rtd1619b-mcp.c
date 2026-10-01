/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Minimal MCP control plane. No crypto DMA: GO fails with MCP_ERROR. */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"

#define TYPE_RTD_MCP "rtd1619b-mcp"
OBJECT_DECLARE_SIMPLE_TYPE(RtdMcpState, RTD_MCP)
struct RtdMcpState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    uint32_t regs[0x200 / 4];
};

static bool mcp_known(hwaddr a)
{
    return (a >= 0x100 && a <= 0x138) || a == 0x198 ||
           a == 0x19c || a == 0x1e0;
}

static MemTxResult mcp_read(void *opaque, hwaddr a, uint64_t *v,
                            unsigned size, MemTxAttrs attrs)
{
    RtdMcpState *s = opaque;
    if (!mcp_known(a)) {
        return MEMTX_ERROR;
    }
    *v = s->regs[a / 4];
    return MEMTX_OK;
}

static MemTxResult mcp_write(void *opaque, hwaddr a, uint64_t v,
                             unsigned size, MemTxAttrs attrs)
{
    RtdMcpState *s = opaque;
    uint32_t *r;
    if (!mcp_known(a)) {
        return MEMTX_ERROR;
    }
    r = &s->regs[a / 4];
    if (a == 0x100 || a == 0x104 || a == 0x108) {
        /* Bit zero selects set/clear, it is not a retained register bit. */
        *r = v & 1 ? *r | (v & ~1U) : *r & ~(v & ~1U);
        if (a == 0x100 && (v & 0x11) == 0x11) {
            *r &= ~0x12U; /* CLEAR stops the engine and self-clears. */
            s->regs[0x104 / 4] = 0;
        }
        if (a == 0x100 && (v & 3) == 3) {
            *r &= ~2U;
            s->regs[0x104 / 4] |= 4; /* MCP_ERROR, never success. */
            qemu_log_mask(LOG_UNIMP, "rtd-mcp: crypto DMA unsupported\n");
        }
    } else if (a == 0x19c) {
        qemu_log_mask(LOG_UNIMP, "rtd-mcp: OTP key loading unsupported\n");
        return MEMTX_ERROR;
    } else {
        *r = v;
    }
    return MEMTX_OK;
}

static const MemoryRegionOps mcp_ops = {
    .read_with_attrs = mcp_read, .write_with_attrs = mcp_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void mcp_reset(DeviceState *dev)
{
    memset(RTD_MCP(dev)->regs, 0, sizeof(RTD_MCP(dev)->regs));
}

static void mcp_init(Object *obj)
{
    RtdMcpState *s = RTD_MCP(obj);
    memory_region_init_io(&s->mmio, obj, &mcp_ops, s, "rtd-mcp", 0x200);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
}

static const VMStateDescription mcp_vmstate = {
    .name = TYPE_RTD_MCP, .version_id = 1, .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, RtdMcpState, 0x200 / 4),
        VMSTATE_END_OF_LIST()
    }
};

static void mcp_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->vmsd = &mcp_vmstate;
    device_class_set_legacy_reset(dc, mcp_reset);
}

static const TypeInfo mcp_type = {
    .name = TYPE_RTD_MCP, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RtdMcpState), .instance_init = mcp_init,
    .class_init = mcp_class_init,
};

static void mcp_register_types(void)
{
    type_register_static(&mcp_type);
}
type_init(mcp_register_types)

/* HSE command queue controls, from hse/{hse.h,engine.c}. DMA unsupported. */
#define TYPE_RTD_HSE "rtd1619b-hse"
OBJECT_DECLARE_SIMPLE_TYPE(RtdHseState, RTD_HSE)
struct RtdHseState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq irq;
    uint32_t regs[0x420 / 4];
};

static void hse_update(RtdHseState *s)
{
    uint32_t active = 0;
    for (unsigned base = 0x200; base <= 0x300; base += 0x100) {
        active |= s->regs[(base + 0x14) / 4] &
                  s->regs[(base + 0x24) / 4] & 6;
    }
    qemu_set_irq(s->irq, !!active);
}

static bool hse_known(hwaddr a)
{
    return a == 0x41c || (a >= 0x200 && a <= 0x224) ||
           (a >= 0x300 && a <= 0x324);
}

static MemTxResult hse_read(void *opaque, hwaddr a, uint64_t *v,
                            unsigned size, MemTxAttrs attrs)
{
    RtdHseState *s = opaque;
    if (!hse_known(a)) {
        return MEMTX_ERROR;
    }
    *v = s->regs[a / 4];
    return MEMTX_OK;
}

static MemTxResult hse_write(void *opaque, hwaddr a, uint64_t v,
                             unsigned size, MemTxAttrs attrs)
{
    RtdHseState *s = opaque;
    if (!hse_known(a)) {
        return MEMTX_ERROR;
    }
    if ((a & 0xff) == 0x14) {
        s->regs[a / 4] &= ~(v & 6);
    } else {
        s->regs[a / 4] = v;
        if ((a & 0xff) == 0x10 && (v & 1)) {
            s->regs[(a + 4) / 4] |= 4; /* Command error, not IRQ_OK. */
            qemu_log_mask(LOG_UNIMP, "rtd-hse: command DMA unsupported\n");
        }
    }
    hse_update(s);
    return MEMTX_OK;
}

static const MemoryRegionOps hse_ops = {
    .read_with_attrs = hse_read, .write_with_attrs = hse_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void hse_reset(DeviceState *dev)
{
    RtdHseState *s = RTD_HSE(dev);
    memset(s->regs, 0, sizeof(s->regs));
    hse_update(s);
}

static void hse_init(Object *obj)
{
    RtdHseState *s = RTD_HSE(obj);
    memory_region_init_io(&s->mmio, obj, &hse_ops, s, "rtd-hse", 0x420);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static int hse_post_load(void *opaque, int version_id)
{
    hse_update(opaque);
    return 0;
}

static const VMStateDescription hse_vmstate = {
    .name = TYPE_RTD_HSE, .version_id = 1, .minimum_version_id = 1,
    .post_load = hse_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, RtdHseState, 0x420 / 4),
        VMSTATE_END_OF_LIST()
    }
};

static void hse_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->vmsd = &hse_vmstate;
    device_class_set_legacy_reset(dc, hse_reset);
}

static const TypeInfo hse_type = {
    .name = TYPE_RTD_HSE, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RtdHseState), .instance_init = hse_init,
    .class_init = hse_class_init,
};

static void hse_register_types(void)
{
    type_register_static(&hse_type);
}
type_init(hse_register_types)
