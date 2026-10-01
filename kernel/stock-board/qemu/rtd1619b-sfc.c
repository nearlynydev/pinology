/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Realtek SFC command/window and bounded flash DMA, backed by upstream NOR. */
#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/ssi/ssi.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include "qemu/log.h"

#define TYPE_RTD_SFC "rtd1619b-sfc"
#define NOR_BASE 0x88100000U
#define NOR_SIZE 0x1000000U
OBJECT_DECLARE_SIMPLE_TYPE(RtdSfcState, RTD_SFC)

struct RtdSfcState {
    SysBusDevice parent_obj;
    MemoryRegion regs, window, dma;
    SSIBus *spi;
    qemu_irq cs;
    uint32_t reg[11], md[12];
};

static void spi_begin(RtdSfcState *s, uint8_t op)
{
    qemu_set_irq(s->cs, 1);
    qemu_set_irq(s->cs, 0);
    ssi_transfer(s->spi, op);
}

static void spi_end(RtdSfcState *s)
{
    qemu_set_irq(s->cs, 1);
}

static void spi_address(RtdSfcState *s, uint32_t a)
{
    ssi_transfer(s->spi, (a >> 16) & 0xff);
    ssi_transfer(s->spi, (a >> 8) & 0xff);
    ssi_transfer(s->spi, a & 0xff);
}

static uint8_t spi_status(RtdSfcState *s)
{
    spi_begin(s, 0x05);
    uint8_t value = ssi_transfer(s->spi, 0);
    spi_end(s);
    return value;
}

static bool flash_range(hwaddr a, unsigned len)
{
    return len && a < NOR_SIZE && len <= NOR_SIZE - a;
}

static bool flash_program(RtdSfcState *s, hwaddr a,
                          const uint8_t *buf, unsigned len)
{
    if (!flash_range(a, len) || len > 256 - (a & 255)) {
        return false;
    }
    /* The controller issues WREN automatically when EN_WR is enabled. */
    if ((s->reg[7] & 0x1ff) == 0x106) {
        spi_begin(s, 0x06);
        spi_end(s);
    }
    if (!(spi_status(s) & 2)) {
        return true; /* NOR ignores program commands while write-disabled. */
    }
    spi_begin(s, 0x02);
    spi_address(s, a);
    for (unsigned i = 0; i < len; i++) {
        ssi_transfer(s->spi, buf[i]);
    }
    spi_end(s);
    spi_begin(s, 0x04); /* Page program clears the flash write-enable latch. */
    spi_end(s);
    return true;
}

static void flash_read(RtdSfcState *s, hwaddr a, uint8_t *buf, unsigned len)
{
    spi_begin(s, 0x03);
    spi_address(s, a);
    for (unsigned i = 0; i < len; i++) {
        buf[i] = ssi_transfer(s->spi, 0);
    }
    spi_end(s);
}

static MemTxResult window_read(void *opaque, hwaddr a, uint64_t *v,
                               unsigned size, MemTxAttrs attrs)
{
    RtdSfcState *s = opaque;
    uint8_t op = s->reg[0], data[4] = {0};
    *v = 0;
    if (!flash_range(a, size)) {
        return MEMTX_ERROR;
    }
    switch (op) {
    case 0x03: /* READ */
        flash_read(s, a, data, size);
        break;
    case 0x9f: /* JEDEC ID; each window transaction starts at byte a. */
    case 0x05: /* RDSR */
    case 0x70: /* Micron flag status */
        if (a + size > 6) {
            return MEMTX_ERROR;
        }
        spi_begin(s, op);
        for (unsigned i = 0; i < a; i++) {
            ssi_transfer(s->spi, 0);
        }
        for (unsigned i = 0; i < size; i++) {
            data[i] = ssi_transfer(s->spi, 0);
        }
        spi_end(s);
        break;
    case 0x06: /* WREN is clocked by a window read, not register write. */
    case 0x04: /* WRDI */
        spi_begin(s, op);
        spi_end(s);
        break;
    case 0x20: /* 4 KiB erase */
    case 0xd8: /* 64 KiB erase */
    case 0xc7: /* chip erase */
        if (!(spi_status(s) & 2)) {
            return MEMTX_OK; /* A write-disabled NOR ignores erase commands. */
        }
        spi_begin(s, op);
        if (op != 0xc7) {
            /* Flash ignores low sector bits; also bound upstream memset. */
            spi_address(s, a & ~(op == 0x20 ? 0xfffU : 0xffffU));
        }
        spi_end(s);
        spi_begin(s, 0x04); /* Erase consumes WEL. */
        spi_end(s);
        break;
    default:
        qemu_log_mask(LOG_GUEST_ERROR, "rtd-sfc: unsupported read op=%x\n", op);
        return MEMTX_ERROR;
    }
    for (unsigned i = 0; i < size; i++) {
        *v |= (uint64_t)data[i] << (i * 8);
    }
    return MEMTX_OK;
}

static MemTxResult window_write(void *opaque, hwaddr a, uint64_t v,
                                unsigned size, MemTxAttrs attrs)
{
    RtdSfcState *s = opaque;
    uint8_t data[4];
    if (!flash_range(a, size)) {
        return MEMTX_ERROR;
    }
    for (unsigned i = 0; i < size; i++) {
        data[i] = v >> (8 * i);
    }
    if ((s->reg[0] & 0xff) == 0x02) {
        return flash_program(s, a, data, size) ? MEMTX_OK : MEMTX_ERROR;
    }
    if ((s->reg[0] & 0xff) == 0x01 && !a && size == 1 &&
        (spi_status(s) & 2)) {
        spi_begin(s, 0x01);
        ssi_transfer(s->spi, v & 0xff);
        spi_end(s);
        return MEMTX_OK;
    }
    return MEMTX_ERROR;
}

static uint64_t regs_read(void *opaque, hwaddr a, unsigned size)
{
    return RTD_SFC(opaque)->reg[a / 4];
}

static void regs_write(void *opaque, hwaddr a, uint64_t v, unsigned size)
{
    RtdSfcState *s = opaque;
    s->reg[a / 4] = v;
    if (a == 4 && !v && (s->reg[0] & 0xff) == 4) {
        spi_begin(s, 4);
        spi_end(s);
    }
}

static uint64_t dma_read(void *opaque, hwaddr a, unsigned size)
{
    return RTD_SFC(opaque)->md[a / 4];
}

static MemTxResult dma_write(void *opaque, hwaddr a, uint64_t v,
                             unsigned size, MemTxAttrs attrs)
{
    RtdSfcState *s = opaque;
    uint8_t data[256];
    uint64_t ram = s->md[3], flash = s->md[4];
    uint32_t mode = s->md[5] & 0xff000000;
    unsigned len = s->md[5] & 0x00ffffff;
    s->md[a / 4] = v;
    if (a != 0x18 || v != 3) {
        return MEMTX_OK;
    }
    /* DMA must never recurse into MMIO, wrap around, or exceed its buffer. */
    if (!len || len > sizeof(data) || ram < 0x40000 ||
        ram >= 0x80000000 || len > 0x80000000 - ram ||
        flash < NOR_BASE || !flash_range(flash - NOR_BASE, len) ||
        (mode != 0x0c000000 && mode != 0x06000000)) {
        qemu_log_mask(LOG_GUEST_ERROR, "rtd-sfc: rejected DMA descriptor\n");
        return MEMTX_ERROR;
    }
    flash -= NOR_BASE;
    if (mode == 0x0c000000) {
        flash_read(s, flash, data, len);
        if (address_space_write(&address_space_memory, ram,
                                MEMTXATTRS_UNSPECIFIED,
                                data, len) != MEMTX_OK) {
            return MEMTX_ERROR;
        }
    } else {
        if (address_space_read(&address_space_memory, ram,
                               MEMTXATTRS_UNSPECIFIED,
                               data, len) != MEMTX_OK ||
            !flash_program(s, flash, data, len)) {
            return MEMTX_ERROR;
        }
    }
    s->md[6] &= ~1U; /* synchronous transfer complete, not timed silicon */
    return MEMTX_OK;
}

static const MemoryRegionOps regs_ops = {
    .read = regs_read, .write = regs_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};
static const MemoryRegionOps window_ops = {
    .read_with_attrs = window_read, .write_with_attrs = window_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};
static const MemoryRegionOps dma_ops = {
    .read = dma_read, .write_with_attrs = dma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void sfc_reset(DeviceState *dev)
{
    RtdSfcState *s = RTD_SFC(dev);
    memset(s->reg, 0, sizeof(s->reg));
    memset(s->md, 0, sizeof(s->md));
    s->reg[0] = 3;
    spi_end(s);
}

static const VMStateDescription sfc_vmstate = {
    .name = TYPE_RTD_SFC, .version_id = 1, .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(reg, RtdSfcState, 11),
        VMSTATE_UINT32_ARRAY(md, RtdSfcState, 12),
        VMSTATE_END_OF_LIST()
    },
};

static void sfc_init(Object *obj)
{
    RtdSfcState *s = RTD_SFC(obj);
    SysBusDevice *bus = SYS_BUS_DEVICE(obj);
    s->spi = ssi_create_bus(DEVICE(obj), "spi");
    sysbus_init_irq(bus, &s->cs);
    memory_region_init_io(&s->regs, obj, &regs_ops, s, "rtd-sfc-regs", 0x2c);
    memory_region_init_io(&s->window, obj, &window_ops, s,
                          "rtd-sfc-window", 0x2000000);
    memory_region_init_io(&s->dma, obj, &dma_ops, s, "rtd-sfc-dma", 0x30);
    sysbus_init_mmio(bus, &s->regs);
    sysbus_init_mmio(bus, &s->window);
    sysbus_init_mmio(bus, &s->dma);
}

static void sfc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    device_class_set_legacy_reset(dc, sfc_reset);
    dc->vmsd = &sfc_vmstate;
}

static const TypeInfo sfc_type = {
    .name = TYPE_RTD_SFC, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RtdSfcState), .instance_init = sfc_init,
    .class_init = sfc_class_init,
};

static void sfc_register(void)
{
    type_register_static(&sfc_type);
}
type_init(sfc_register)
