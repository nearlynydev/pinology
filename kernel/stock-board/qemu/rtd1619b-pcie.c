/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Experimental Realtek indirect PCI configuration host, stock DS423 contract.
 * No ECAM/root-port substitution. One endpoint at bus0:00.0 per host.
 * Electrical timing and analog PHY values are not a silicon simulation.
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/units.h"
#include "hw/core/boards.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_bus.h"
#include "hw/pci/pci_host.h"
#include "hw/pci/msi.h"
#include "migration/blocker.h"
#include "system/address-spaces.h"

#define TYPE_RTD_PCIE "rtd1619b-pcie"
OBJECT_DECLARE_SIMPLE_TYPE(RtdPcie, RTD_PCIE)
struct RtdPcie {
    PCIHostState parent_obj;
    MemoryRegion ctrl, wrapper, pci_mem, pci_io, mem_window, io_window;
    MemoryRegion dma_root, dma_ram, msi;
    AddressSpace dma_as, io_as;
    qemu_irq msi_irq, intx_irq;
    uint32_t reg[1024], wrap[4], mem_base, io_base;
    uint16_t mdio[256];
    bool perst, msi_mapped;
    int intx;
    Error *migration_blocker;
};

static bool link_up(RtdPcie *s)
{
    PCIHostState *h = PCI_HOST_BRIDGE(s);
    return s->perst && (s->reg[0xc00 / 4] & 2) &&
           (s->reg[0x710 / 4] & 0x20) &&
           (s->mdio[0x1f] & s->mdio[0x5f] & 0x8000) &&
           pci_find_device(h->bus, 0, 0);
}

static void update_irq(RtdPcie *s)
{
    bool link = link_up(s);
    memory_region_set_enabled(&s->dma_ram, link);
    memory_region_set_enabled(&s->mem_window, link && s->wrap[3]);
    memory_region_set_enabled(&s->io_window, link && s->wrap[3]);
    qemu_set_irq(s->msi_irq, link_up(s) &&
                 (s->reg[0x830 / 4] & s->reg[0x828 / 4] &
                  ~s->reg[0x82c / 4] & 0xffff));
    qemu_set_irq(s->intx_irq, link_up(s) && s->intx);
}

static void set_intx(void *opaque, int pin, int level)
{
    RtdPcie *s = opaque;
    s->intx = level;
    update_irq(s);
}
static int map_intx(PCIDevice *dev, int pin) { return 0; }

static MemTxResult msi_write(void *opaque, hwaddr addr, uint64_t data,
                             unsigned size, MemTxAttrs attrs)
{
    RtdPcie *s = opaque;
    if (addr || data >= 16 || !link_up(s)) {
        return MEMTX_ERROR;
    }
    s->reg[0x830 / 4] |= 1U << data;
    update_irq(s);
    return MEMTX_OK;
}
static MemTxResult msi_read(void *opaque, hwaddr addr, uint64_t *value,
                            unsigned size, MemTxAttrs attrs)
{
    *value = 0;
    return MEMTX_ERROR;
}
static const MemoryRegionOps msi_ops = {
    .read_with_attrs = msi_read, .write_with_attrs = msi_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void update_msi(RtdPcie *s)
{
    uint64_t addr = (uint64_t)s->reg[0x824 / 4] << 32 | s->reg[0x820 / 4];
    memory_region_transaction_begin();
    if (s->msi_mapped) {
        memory_region_del_subregion(&s->dma_root, &s->msi);
        s->msi_mapped = false;
    }
    if (addr && !(addr & 3) && addr <= UINT64_MAX - 4) {
        memory_region_add_subregion_overlap(&s->dma_root, addr, &s->msi, 1);
        s->msi_mapped = true;
    }
    memory_region_transaction_commit();
}

static void transact(RtdPcie *s)
{
    PCIHostState *h = PCI_HOST_BRIDGE(s);
    uint32_t addr = s->reg[0xc44 / 4], en = s->reg[0xc3c / 4];
    bool write = en & 1;
    unsigned lanes = en & 2 ? (en >> 4) & 15 : 15;
    PCIDevice *dev = pci_find_device(h->bus, 0, 0);
    s->reg[0xc38 / 4] = 0;
    s->reg[0xc40 / 4] = 1;
    s->reg[0xc4c / 4] = UINT32_MAX;
    if (!link_up(s) || !dev || (addr & ~0xffcU) || !lanes ||
        s->reg[0xc14 / 4] != (write ? 0x12 : 0x10)) {
        s->reg[0xc40 / 4] |= 2;
        s->reg[0xc7c / 4] |= 0x10;
        return;
    }
    /* Preserve byte-lane placement. Byte writes do not read-modify-write W1C
     * config registers. Full dword accesses remain one PCI transaction. */
    if (lanes == 15) {
        if (write) {
            pci_host_config_write_common(dev, addr, pci_config_size(dev),
                                         s->reg[0xc48 / 4], 4);
        } else {
            s->reg[0xc4c / 4] = pci_host_config_read_common(dev, addr,
                                                          pci_config_size(dev), 4);
        }
    } else {
        uint32_t result = 0;
        for (unsigned i = 0; i < 4; i++) {
            if (!(lanes & (1U << i))) { continue; }
            if (write) {
                pci_host_config_write_common(dev, addr + i, pci_config_size(dev),
                                             s->reg[0xc48 / 4] >> (8 * i), 1);
            } else {
                result |= pci_host_config_read_common(dev, addr + i,
                              pci_config_size(dev), 1) << (8 * i);
            }
        }
        if (!write) { s->reg[0xc4c / 4] = result; }
    }
}

static uint64_t ctrl_read(void *opaque, hwaddr addr, unsigned size)
{
    RtdPcie *s = opaque;
    if (addr == 0xcb4) { return link_up(s) ? 0x800 : 0; }
    return s->reg[addr / 4];
}
static void ctrl_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    RtdPcie *s = opaque;
    uint32_t v = value;
    switch (addr) {
    case 0xcb4: return;
    case 0xc40: s->reg[addr / 4] &= ~(v & 3); return;
    case 0xc7c: s->reg[addr / 4] &= ~(v & 0x1f); return;
    case 0x830: s->reg[addr / 4] &= ~v; update_irq(s); return;
    case 0xc1c: {
        unsigned index = (v >> 8) & 255;
        if (v & 1) {
            s->mdio[index] = v >> 16;
            unsigned base = index & 0x40;
            if ((index & ~0x40) == 0x10 && (v >> 16) == 0x3c4 &&
                (s->mdio[base + 0x19] & 4) && (s->mdio[base + 0x0d] & 0x40)) {
                s->mdio[base + 0x1f] &= ~0x40;
            }
            if ((index & ~0x40) == 9 && (v >> 16) == 0x721c &&
                !(s->mdio[base + 0x1f] & 0x40)) {
                s->mdio[base + 0x1f] |= 0x8000;
            }
        }
        s->reg[addr / 4] = (v & 0xff7e) | (uint32_t)s->mdio[index] << 16;
        update_irq(s);
        return;
    }
    default: s->reg[addr / 4] = v; break;
    }
    if (addr == 0xc38 && (v & 1)) { transact(s); }
    if (addr == 0x820 || addr == 0x824) { update_msi(s); }
    update_irq(s);
}
static const MemoryRegionOps ctrl_ops = {
    .read = ctrl_read, .write = ctrl_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static uint64_t wrap_read(void *opaque, hwaddr addr, unsigned size)
{
    RtdPcie *s = opaque;
    return s->wrap[addr / 4];
}
static void wrap_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    RtdPcie *s = opaque;
    if (addr == 12) { return; }
    s->wrap[addr / 4] = value;
    bool valid = s->wrap[0] == s->mem_base &&
                 s->wrap[1] == s->mem_base + MiB - 0x1000 && s->wrap[2] == 1;
    s->wrap[3] = valid;
    update_irq(s);
}
static const MemoryRegionOps wrap_ops = {
    .read = wrap_read, .write = wrap_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static AddressSpace *dma_space(PCIBus *bus, void *opaque, int devfn)
{
    return &((RtdPcie *)opaque)->dma_as;
}
static const PCIIOMMUOps dma_ops = { .get_address_space = dma_space };

/* Keep the CPU IO aperture opaque and page-aligned. An alias exposes the
 * tiny BAR subregions over backing RAM (the translated DT IO aperture is low),
 * creating sub-host-page RAM holes that HVF cannot map. Unassigned IO must
 * not fall through to RAM in any accelerator. */
static MemTxResult io_read(void *opaque, hwaddr addr, uint64_t *value,
                           unsigned size, MemTxAttrs attrs)
{
    RtdPcie *s = opaque;
    MemTxResult result;
    addr += s->io_base;
    switch (size) {
    case 1: *value = address_space_ldub(&s->io_as, addr, attrs, &result); break;
    case 2: *value = address_space_lduw_le(&s->io_as, addr, attrs, &result); break;
    default: *value = address_space_ldl_le(&s->io_as, addr, attrs, &result); break;
    }
    return result;
}
static MemTxResult io_write(void *opaque, hwaddr addr, uint64_t value,
                            unsigned size, MemTxAttrs attrs)
{
    RtdPcie *s = opaque;
    MemTxResult result;
    addr += s->io_base;
    switch (size) {
    case 1: address_space_stb(&s->io_as, addr, value, attrs, &result); break;
    case 2: address_space_stw_le(&s->io_as, addr, value, attrs, &result); break;
    default: address_space_stl_le(&s->io_as, addr, value, attrs, &result); break;
    }
    return result;
}
static const MemoryRegionOps io_ops = {
    .read_with_attrs = io_read, .write_with_attrs = io_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void perst_input(void *opaque, int n, int level)
{
    RtdPcie *s = opaque;
    if (s->perst && !level) {
        bus_cold_reset(BUS(PCI_HOST_BRIDGE(s)->bus));
        s->intx = 0;
        s->reg[0x830 / 4] = 0;
    }
    s->perst = level;
    update_irq(s);
}
static void host_reset(DeviceState *dev)
{
    RtdPcie *s = RTD_PCIE(dev);
    memset(s->reg, 0, sizeof(s->reg));
    memset(s->wrap, 0, sizeof(s->wrap));
    memset(s->mdio, 0, sizeof(s->mdio));
    s->mdio[0x1f] = s->mdio[0x5f] = 0x40;
    s->perst = false;
    s->intx = 0;
    memory_region_set_enabled(&s->mem_window, false);
    memory_region_set_enabled(&s->io_window, false);
    update_msi(s);
    update_irq(s);
}
static void host_realize(DeviceState *dev, Error **errp)
{
    RtdPcie *s = RTD_PCIE(dev);
    PCIHostState *h = PCI_HOST_BRIDGE(dev);
    error_setg(&s->migration_blocker, "Experimental RTD PCIe state is not migratable; use a cold checkpoint");
    if (migrate_add_blocker(&s->migration_blocker, errp) < 0) {
        return;
    }
    memory_region_init(&s->pci_mem, OBJECT(s), "rtd-pcie-memory", UINT64_MAX);
    memory_region_init(&s->pci_io, OBJECT(s), "rtd-pcie-io", 1ULL << 32);
    memory_region_init_alias(&s->mem_window, OBJECT(s), "rtd-pcie-window",
                             &s->pci_mem, s->mem_base, MiB);
    address_space_init(&s->io_as, &s->pci_io, "rtd-pcie-io");
    memory_region_init_io(&s->io_window, OBJECT(s), &io_ops, s,
                          "rtd-pcie-io-window", 0x10000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mem_window);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->io_window);
    memory_region_init(&s->dma_root, OBJECT(s), "rtd-pcie-dma", UINT64_MAX);
    memory_region_init_alias(&s->dma_ram, OBJECT(s), "rtd-pcie-dma-ram",
                             MACHINE(qdev_get_machine())->ram, 0, 2 * GiB);
    memory_region_add_subregion(&s->dma_root, 0, &s->dma_ram);
    address_space_init(&s->dma_as, &s->dma_root, "rtd-pcie-dma");
    h->bus = pci_register_root_bus(dev, "pci", set_intx, map_intx, s,
                                   &s->pci_mem, &s->pci_io, 0, 1, TYPE_PCI_BUS);
    pci_setup_iommu(h->bus, &dma_ops, s);
    msi_nonbroken = true;
}
static void host_init(Object *obj)
{
    RtdPcie *s = RTD_PCIE(obj);
    memory_region_init_io(&s->ctrl, obj, &ctrl_ops, s, "rtd-pcie-ctrl", 0x1000);
    memory_region_init_io(&s->wrapper, obj, &wrap_ops, s, "rtd-pcie-wrapper", 0x10);
    memory_region_init_io(&s->msi, obj, &msi_ops, s, "rtd-pcie-msi", 4);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->ctrl);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->wrapper);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->msi_irq);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->intx_irq);
    qdev_init_gpio_in_named(DEVICE(s), perst_input, "perst", 1);
}
static const Property host_properties[] = {
    DEFINE_PROP_UINT32("mem-base", RtdPcie, mem_base, 0xa0100000),
    DEFINE_PROP_UINT32("io-base", RtdPcie, io_base, 0x50000),
};
static void host_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->realize = host_realize;
    device_class_set_props(dc, host_properties);
    device_class_set_legacy_reset(dc, host_reset);
    /* No migration contract yet: do not silently drop controller state. */
    dc->user_creatable = false;
}
static const TypeInfo host_type = {
    .name = TYPE_RTD_PCIE, .parent = TYPE_PCI_HOST_BRIDGE,
    .instance_size = sizeof(RtdPcie), .instance_init = host_init,
    .class_init = host_class_init,
};
static void register_types(void) { type_register_static(&host_type); }
type_init(register_types)
