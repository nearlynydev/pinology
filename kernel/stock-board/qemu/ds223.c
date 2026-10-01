/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Experimental DS223/RTD1619B direct-kernel boot board.
 * Addresses and CPU affinities are from the DS223 7.4.1 model.dtb.
 * This is not a complete RTD1619B SoC or a boot-ROM/U-Boot implementation.
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/units.h"
#include "qobject/qlist.h"
#include "hw/core/boards.h"
#include "hw/core/irq.h"
#include "hw/core/or-irq.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/ssi/ssi.h"
#include "hw/i2c/i2c.h"
#include "system/blockdev.h"
#include "hw/arm/boot.h"
#include "hw/arm/bsa.h"
#include "hw/arm/machines-qom.h"
#include "hw/char/serial-mm.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/ide/ide-dev.h"
#include "hw/intc/arm_gicv3_common.h"
#include "hw/intc/arm_gic.h"
#include "system/address-spaces.h"
#include "system/device_tree.h"
#include "system/system.h"
#include "net/net.h"
#include "target/arm/cpu.h"
#include "system/kvm.h"
#include "target/arm/kvm_arm.h"

#define TYPE_DS223_MACHINE MACHINE_TYPE_NAME("ds223")
OBJECT_DECLARE_SIMPLE_TYPE(DS223MachineState, DS223_MACHINE)

#define DS223_CPUS 4
#define DS223_SPIS 256
#define DS223_GICD 0xff100000ULL
#define DS223_GICR 0xff140000ULL
#define DS223_UART0 0x98007800ULL

struct DS223MachineState {
    MachineState parent_obj;
    struct arm_boot_info boot;
    MemoryRegion i2c_window[5];
    MemoryRegion uart_ext[3];
    MemoryRegion npu_unavailable;
    DeviceState *sata, *gpio;
    unsigned disk_bays;
    bool experimental_gicv2;
    Notifier disk_presence_ready;
};

/* DT internal_slot detect_pin_gpio is active-low, GPIO73..76. CLI -device
 * drives are realized AFTER board init, so sample them at machine-init-done.
 * This is cold-attachment presence, not hotplug/power/link emulation. GPIO
 * external input levels survive controller resets. Never claim empty bays.
 */
static void ds223_disk_presence(Notifier *notifier, void *opaque)
{
    DS223MachineState *s = container_of(notifier, DS223MachineState,
                                       disk_presence_ready);
    for (unsigned i = 0; i < s->disk_bays; i++) {
        g_autofree char *name = g_strdup_printf("ide.%u", i);
        BusState *bus = qdev_get_child_bus(s->sata, name);
        BusChild *child;
        bool present = false;
        assert(bus);
        QTAILQ_FOREACH(child, &bus->children, sibling) {
            if (object_dynamic_cast(OBJECT(child->child), TYPE_IDE_DEVICE) &&
                IDE_DEVICE(child->child)->conf.blk) {
                present = true;
            }
        }
        qemu_set_irq(qdev_get_gpio_in_named(s->gpio, "pin", 73 + i), !present);
    }
}

/* Experimental unavailable-NPU profile, NOT accelerator emulation.
 * Stock galcore rtk_npu_wrapper_init reads wrapper+0x1800: bit 4 is
 * BISR done, bit 11 reset done, bits 10:8 idle. Returning not-ready
 * makes its own probe return an error rather than taking a bus abort.
 * No success bits, job completion, DMA or interrupt are synthesized.
 * The rest of the NPU aperture remains unmapped. Silicon reset values
 * have not been established; this is an explicit incomplete-device state.
 */
static uint64_t ds223_npu_unavailable_read(void *opaque, hwaddr a,
                                          unsigned size)
{
    return 0;
}
static MemTxResult ds223_npu_unavailable_write(void *opaque, hwaddr a,
                                               uint64_t v, unsigned size,
                                               MemTxAttrs attrs)
{
    return MEMTX_ERROR;
}
static const MemoryRegionOps ds223_npu_unavailable_ops = {
    .read = ds223_npu_unavailable_read,
    .write_with_attrs = ds223_npu_unavailable_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

/* DW synthesized without additional features, backed by QEMU's real 16550. */
static MemTxResult ds223_uart_ext_read(void *opaque, hwaddr a, uint64_t *v,
                                       unsigned size, MemTxAttrs attrs)
{
    SerialState *s = opaque;
    switch (a + 0x20) {
    case 0xf8: /* UCV == 0 means ADDITIONAL_FEATURES disabled (8250_dwlib). */
    case 0xf4:
    case 0xc0:
        *v = 0;
        return MEMTX_OK;
    case 0x7c: /* USR: BUSY, TFNF, TFE, RFNE, RFF. */
        *v = (!(s->lsr & 0x40)) | ((s->lsr & 0x20) ? 6 : 0) |
             ((s->lsr & 1) ? 8 : 0) |
             (fifo8_is_full(&s->recv_fifo) ? 16 : 0);
        return MEMTX_OK;
    default:
        return MEMTX_ERROR;
    }
}

static MemTxResult ds223_uart_ext_write(void *opaque, hwaddr a, uint64_t v,
                                        unsigned size, MemTxAttrs attrs)
{
    return a + 0x20 == 0xc0 ? MEMTX_OK : MEMTX_ERROR;
}

static const MemoryRegionOps ds223_uart_ext_ops = {
    .read_with_attrs = ds223_uart_ext_read,
    .write_with_attrs = ds223_uart_ext_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

/* arm_load_dtb replaces RAM nodes. DS223 retains the vendor prefix; DS423
 * also excludes the two translated PCI I/O apertures (0x40000..0x5ffff).
 * Otherwise link-up switches live page-table RAM into MMIO. This is explicit
 * loader-supplied memory metadata, not a patch to the on-disk vendor DTB. */
static void ds223_boot_dtb(const struct arm_boot_info *info, void *fdt)
{
    DS223MachineState *s = DS223_MACHINE(qdev_get_machine());
    bool ds423 = object_dynamic_cast(OBJECT(qdev_get_machine()),
                                     MACHINE_TYPE_NAME("ds423"));
    uint32_t start = ds423 ? 0x60000 : 0x40000;
    qemu_fdt_nop_node(fdt, "/memory@0");
    qemu_fdt_add_subnode(fdt, "/memory@4000");
    qemu_fdt_setprop_string(fdt, "/memory@4000", "device_type", "memory");
    qemu_fdt_setprop_cells(fdt, "/memory@4000", "reg", start, 0x80000000 - start);
    if (s->experimental_gicv2) {
        /* These are coherent QEMU DMA devices, not the physical Realtek bus.
         * Under KVM, guest non-cacheable DMA mappings alias QEMU's cacheable
         * RAM mapping; retaining the vendor's non-coherent DT can lose ring
         * updates across cache lines. Like virt, describe coherent DMA at the
         * root so all virtual DMA masters inherit the actual memory contract.
         * Keep this opt-in while the existing default platform is unchanged.
         */
        qemu_fdt_setprop(fdt, "/", "dma-coherent", NULL, 0);
        /* Explicit virtual-board adaptation, NOT a byte-identical vendor DT.
         * Preserve phandle 1 and the SPI/PPI namespace used by all peripherals.
         * GICC reuses the former redistributor base; no EL2/virtual GIC frames.
         */
        const char *gic = "/soc@0/interrupt-controller@ff100000";
        qemu_fdt_setprop_string(fdt, gic, "compatible", "arm,cortex-a15-gic");
        qemu_fdt_setprop_cells(fdt, gic, "reg",
                              DS223_GICD, 0x1000, DS223_GICR, 0x2000);
        qemu_fdt_setprop_cells(fdt, gic, "interrupts", 1, 9, 0xf04);
        qemu_fdt_setprop_cells(fdt, "/timer", "interrupts",
                              1, 13, 0xf04, 1, 14, 0xf04, 1, 11, 0xf04,
                              1, 10, 0xf04, 1, 9, 0xf04);
        qemu_fdt_setprop_cells(fdt, "/pmu", "interrupts", 1, 7, 0xf04);
        /* KVM assigns MPIDRs itself. Keep DT phandles/topology references but
         * expose the actual affinities, not the vendor's i << 8 numbering. */
        for (unsigned i = 0; i < DS223_CPUS; i++) {
            g_autofree char *node = g_strdup_printf("/cpus/cpu@%x", i << 8);
            ARMCPU *cpu = ARM_CPU(qemu_get_cpu(i));
            if (cpu->mp_affinity > UINT32_MAX) {
                error_report("experimental GICv2 requires 32-bit CPU affinity");
                exit(1);
            }
            qemu_fdt_setprop_cell(fdt, node, "reg", cpu->mp_affinity);
        }
    }
    if (ds423) {
        /* The stock GIC declares one parent address cell, but PCI interrupt-map
         * omits it. Supply the missing zero unit address in loader metadata.
         * Without this, of_irq_parse_pci returns -EFAULT and legacy r8168 gets
         * IRQ 0. Keep both physical routes on their DT-specified SPI62. */
        qemu_fdt_setprop_cells(fdt, "/soc@0/rbus@98000000/pcie@a0000",
                              "interrupt-map", 0, 0, 0, 0, 1, 0, 0, 62, 4);
        qemu_fdt_setprop_cells(fdt, "/soc@0/rbus@98000000/pcie@c0000",
                              "interrupt-map", 0, 0, 0, 0, 1, 0, 0, 62, 4);
    }
}

static void ds223_init(MachineState *machine)
{
    DS223MachineState *s = DS223_MACHINE(machine);
    MemoryRegion *sysmem = get_system_memory();
    bool ds423 = object_dynamic_cast(OBJECT(machine), MACHINE_TYPE_NAME("ds423"));
    DeviceState *pcie[2] = { NULL, NULL };
    memory_region_init_io(&s->npu_unavailable, OBJECT(machine),
                          &ds223_npu_unavailable_ops, s,
                          "rtd1619b-npu-unavailable-status", 4);
    memory_region_add_subregion(sysmem, 0x98086800, &s->npu_unavailable);
    DeviceState *gic;
    SysBusDevice *gicbus;
    QList *redists;
    const int timer_irq[] = {
        [GTIMER_PHYS] = ARCH_TIMER_NS_EL1_IRQ,
        [GTIMER_VIRT] = ARCH_TIMER_VIRT_IRQ,
        [GTIMER_HYP] = ARCH_TIMER_NS_EL2_IRQ,
        [GTIMER_SEC] = ARCH_TIMER_S_EL1_IRQ,
        [GTIMER_HYPVIRT] = ARCH_TIMER_NS_EL2_VIRT_IRQ,
        [GTIMER_S_EL2_PHYS] = ARCH_TIMER_S_EL2_IRQ,
        [GTIMER_S_EL2_VIRT] = ARCH_TIMER_S_EL2_VIRT_IRQ,
    };

    if (machine->ram_size != 2 * GiB || machine->smp.cpus != DS223_CPUS) {
        error_report("ds223 prototype requires -m 2048 -smp 4");
        exit(1);
    }
    if (!machine->dtb || !machine->kernel_filename) {
        error_report("ds223 requires -kernel Image.stock -dtb model.dtb");
        exit(1);
    }
    memory_region_add_subregion(sysmem, 0, machine->ram);

    DeviceState *crg = qdev_new("rtd1619b-crg");
    sysbus_realize_and_unref(SYS_BUS_DEVICE(crg), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(crg), 0, 0x98000000);
    sysbus_mmio_map(SYS_BUS_DEVICE(crg), 1, 0x98007088);
    sysbus_mmio_map(SYS_BUS_DEVICE(crg), 2, 0x9801a000);
    const unsigned power_offsets[] = {
        0xb00, 0xb20, 0xb60, 0x290, 0x238, 0x260, 0x3b0, 0x28c, 0xfd0,
        0x640, 0x678
    };
    for (unsigned i = 0; i < ARRAY_SIZE(power_offsets); i++) {
        sysbus_mmio_map(SYS_BUS_DEVICE(crg), 3 + i,
                       0x98007000 + power_offsets[i]);
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(crg), 14, 0x9801d700);
    sysbus_mmio_map(SYS_BUS_DEVICE(crg), 15, 0x9801d000);
    sysbus_mmio_map(SYS_BUS_DEVICE(crg), 16, 0x9801db00);
    sysbus_mmio_map(SYS_BUS_DEVICE(crg), 17, 0x9801d604);
    sysbus_mmio_map(SYS_BUS_DEVICE(crg), 18, 0x98007a20);
    sysbus_mmio_map(SYS_BUS_DEVICE(crg), 19, 0x98007aa0);
    sysbus_mmio_map(SYS_BUS_DEVICE(crg), 20, 0x98007fb0);
    sysbus_mmio_map(SYS_BUS_DEVICE(crg), 21, 0x9801d800);

    DeviceState *mcp = sysbus_create_simple("rtd1619b-mcp", 0x98015000, NULL);
    (void)mcp;
    DeviceState *net = qdev_new("rtd1619b-net");
    object_property_add_child(OBJECT(machine), "net", OBJECT(net));
    qemu_configure_nic_device(net, true, NULL);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(net), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(net), 0, 0x98016000);
    sysbus_mmio_map(SYS_BUS_DEVICE(net), 1, 0x9800705c);
    sysbus_mmio_map(SYS_BUS_DEVICE(net), 2, 0x98007070);
    sysbus_mmio_map(SYS_BUS_DEVICE(net), 3, 0x98007210);

    DeviceState *otp = qdev_new("rtd1619b-otp");
    object_property_add_child(OBJECT(machine), "otp", OBJECT(otp));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(otp), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(otp), 0, 0x98032000);
    sysbus_mmio_map(SYS_BUS_DEVICE(otp), 1, 0x98017800);

    for (int i = 0; i < DS223_CPUS; i++) {
        Object *cpu = object_new(machine->cpu_type);
        object_property_set_int(cpu, "mp-affinity",
                                s->experimental_gicv2 ? i : i << 8, &error_fatal);
        if (object_property_find(cpu, "has_el3")) {
            object_property_set_bool(cpu, "has_el3", false, &error_fatal);
        }
        if (object_property_find(cpu, "has_el2")) {
            object_property_set_bool(cpu, "has_el2", false, &error_fatal);
        }
        object_property_set_link(cpu, "memory", OBJECT(sysmem), &error_fatal);
        qdev_realize(DEVICE(cpu), NULL, &error_fatal);
        object_unref(cpu);
    }

    gic = qdev_new(s->experimental_gicv2 ? gic_class_name() : gicv3_class_name());
    object_property_add_child(OBJECT(machine), "gic", OBJECT(gic));
    qdev_prop_set_uint32(gic, "revision", s->experimental_gicv2 ? 2 : 3);
    qdev_prop_set_uint32(gic, "num-cpu", DS223_CPUS);
    qdev_prop_set_uint32(gic, "num-irq", DS223_SPIS + 32);
    if (!s->experimental_gicv2) {
        redists = qlist_new();
        qlist_append_int(redists, DS223_CPUS);
        qdev_prop_set_array(gic, "redist-region-count", redists);
    }
    gicbus = SYS_BUS_DEVICE(gic);
    sysbus_realize_and_unref(gicbus, &error_fatal);
    sysbus_mmio_map(gicbus, 0, DS223_GICD);
    sysbus_mmio_map(gicbus, 1, DS223_GICR);
    sysbus_connect_irq(SYS_BUS_DEVICE(net), 0, qdev_get_gpio_in(gic, 22));
    const hwaddr usb_wrapper[] = {0x98013200, 0x98013c00, 0x98013e00};
    const hwaddr usb_core[] = {0x98020000, 0x98029000, 0x98050000};
    const unsigned usb_irq[] = {95, 21, 94};
    for (unsigned i = 0; i < ARRAY_SIZE(usb_wrapper); i++) {
        DeviceState *usb = qdev_new("rtd1619b-usb");
        g_autofree char *name = g_strdup_printf("usb%u", i);
        object_property_add_child(OBJECT(machine), name, OBJECT(usb));
        sysbus_realize_and_unref(SYS_BUS_DEVICE(usb), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(usb), 0, usb_wrapper[i]);
        sysbus_mmio_map(SYS_BUS_DEVICE(usb), 1, usb_core[i]);
        sysbus_connect_irq(SYS_BUS_DEVICE(usb), 0,
                           qdev_get_gpio_in(gic, usb_irq[i]));
    }
    if (ds423) {
        DeviceState *pcie_irq = qdev_new(TYPE_OR_IRQ);
        object_property_add_child(OBJECT(machine), "pcie-shared-irq", OBJECT(pcie_irq));
        qdev_prop_set_uint16(pcie_irq, "num-lines", 3);
        qdev_realize_and_unref(pcie_irq, NULL, &error_fatal);
        qdev_connect_gpio_out(pcie_irq, 0, qdev_get_gpio_in(gic, 62));
        /* Stock DTB domains 1/2 put the endpoint itself at bus0:00.0. */
        for (unsigned i = 0; i < 2; i++) {
            pcie[i] = qdev_new("rtd1619b-pcie");
            g_autofree char *name = g_strdup_printf("pcie%u", i + 1);
            pcie[i]->id = g_strdup(name);
            object_property_add_child(OBJECT(machine), name, OBJECT(pcie[i]));
            qdev_prop_set_uint32(pcie[i], "mem-base", 0xa0000000 + i * MiB);
            qdev_prop_set_uint32(pcie[i], "io-base", 0x40000 + i * 0x10000);
            sysbus_realize_and_unref(SYS_BUS_DEVICE(pcie[i]), &error_fatal);
            sysbus_mmio_map(SYS_BUS_DEVICE(pcie[i]), 0, 0x980a0000 + i * 0x20000);
            sysbus_mmio_map(SYS_BUS_DEVICE(pcie[i]), 1, 0x9801d630 + i * 0x10);
            sysbus_mmio_map(SYS_BUS_DEVICE(pcie[i]), 2, 0xa0000000 + i * MiB);
            /* rbus ranges translate child 0x10040000 to CPU 0x40000. */
            sysbus_mmio_map(SYS_BUS_DEVICE(pcie[i]), 3, 0x40000 + i * 0x10000);
            sysbus_connect_irq(SYS_BUS_DEVICE(pcie[i]), 0,
                               i ? qdev_get_gpio_in(gic, 107) :
                                   qdev_get_gpio_in(pcie_irq, 0));
            /* Both INTx maps share SPI62 with slot1 MSI: wired-OR levels. */
            sysbus_connect_irq(SYS_BUS_DEVICE(pcie[i]), 1,
                               qdev_get_gpio_in(pcie_irq, i + 1));
        }
        DeviceState *sata = qdev_new("ich9-ahci");
        s->sata = sata;
        s->disk_bays = 4;
        object_property_add_child(OBJECT(machine), "sata", OBJECT(sata));
        qdev_prop_set_uint32(sata, "num-ports", 4);
        qdev_prop_set_int32(sata, "addr", PCI_DEVFN(0, 0));
        pci_realize_and_unref(PCI_DEVICE(sata),
                              PCI_BUS(qdev_get_child_bus(pcie[1], "pci")), &error_fatal);
    } else {
    DeviceState *sata = qdev_new("sysbus-ahci");
    s->sata = sata;
    s->disk_bays = 2;
    object_property_add_child(OBJECT(machine), "sata", OBJECT(sata));
    qdev_prop_set_uint32(sata, "num-ports", 2);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(sata), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(sata), 0, 0x9803f000);
    sysbus_connect_irq(SYS_BUS_DEVICE(sata), 0, qdev_get_gpio_in(gic, 49));
    }
    /* Shared main2-misc SATA/PCIe PHY mux exists in both hardware profiles. */
    DeviceState *sata_phy = sysbus_create_simple("rtd1619b-sata-phy",
                                                0x9803ff00, NULL);
    sysbus_mmio_map(SYS_BUS_DEVICE(sata_phy), 1, 0x9804f050);
    sysbus_create_simple("rtd1619b-hse", 0x98005000,
                         qdev_get_gpio_in(gic, 27));

    DeviceState *sb2 = qdev_new("rtd1619b-sb2");
    object_property_add_child(OBJECT(machine), "sb2", OBJECT(sb2));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(sb2), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(sb2), 0, 0x9801a004);
    sysbus_mmio_map(SYS_BUS_DEVICE(sb2), 1, 0x9801a200);
    sysbus_connect_irq(SYS_BUS_DEVICE(sb2), 0, qdev_get_gpio_in(gic, 36));
    sysbus_mmio_map(SYS_BUS_DEVICE(sb2), 2, 0x9801b000);
    DriveInfo *flash_drive = drive_get(IF_PFLASH, 0, 0);
    if (flash_drive) {
        DeviceState *sfc = qdev_new("rtd1619b-sfc");
        object_property_add_child(OBJECT(machine), "sfc", OBJECT(sfc));
        sysbus_realize_and_unref(SYS_BUS_DEVICE(sfc), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(sfc), 0, 0x9801a800);
        sysbus_mmio_map(SYS_BUS_DEVICE(sfc), 1, 0x88100000);
        sysbus_mmio_map(SYS_BUS_DEVICE(sfc), 2, 0x9801b700);
        DeviceState *flash = qdev_new("is25lp128");
        qdev_prop_set_drive_err(flash, "drive",
                               blk_by_legacy_dinfo(flash_drive), &error_fatal);
        ssi_realize_and_unref(flash, (SSIBus *)qdev_get_child_bus(sfc, "spi"),
                             &error_fatal);
        sysbus_connect_irq(SYS_BUS_DEVICE(sfc), 0,
                           qdev_get_gpio_in_named(flash, SSI_GPIO_CS, 0));
    } else {
        sysbus_mmio_map(SYS_BUS_DEVICE(sb2), 3, 0x9801a800);
        sysbus_mmio_map(SYS_BUS_DEVICE(sb2), 4, 0x88100000);
    }
    sysbus_mmio_map(SYS_BUS_DEVICE(sb2), 5, 0x9801a020);
    sysbus_mmio_map(SYS_BUS_DEVICE(sb2), 6, 0x9801ad00);
    sysbus_connect_irq(SYS_BUS_DEVICE(sb2), 1, qdev_get_gpio_in(gic, 40));
    sysbus_connect_irq(SYS_BUS_DEVICE(sb2), 2, qdev_get_gpio_in(gic, 89));
    sysbus_connect_irq(SYS_BUS_DEVICE(sb2), 3, qdev_get_gpio_in(gic, 90));

    DeviceState *gpio = qdev_new("rtd1619b-gpio");
    s->gpio = gpio;
    object_property_add_child(OBJECT(machine), "gpio", OBJECT(gpio));
    sysbus_realize_and_unref(SYS_BUS_DEVICE(gpio), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(gpio), 0, 0x98007000);
    sysbus_mmio_map(SYS_BUS_DEVICE(gpio), 1, 0x98007090);
    sysbus_mmio_map(SYS_BUS_DEVICE(gpio), 2, 0x980070e0);
    sysbus_mmio_map(SYS_BUS_DEVICE(gpio), 3, 0x98007100);
    sysbus_mmio_map(SYS_BUS_DEVICE(gpio), 4, 0x9804e000);
    sysbus_connect_irq(SYS_BUS_DEVICE(gpio), 0, qdev_get_gpio_in(gic, 41));
    sysbus_mmio_map(SYS_BUS_DEVICE(gpio), 5, 0x98007080);
    if (ds423) {
        for (unsigned i = 0; i < 2; i++) {
            qdev_connect_gpio_out_named(gpio, "pin-out", 18 + i,
                                        qdev_get_gpio_in_named(pcie[i], "perst", 0));
        }
    }
    sysbus_connect_irq(SYS_BUS_DEVICE(crg), 0,
                       qdev_get_gpio_in_named(gpio, "iso-source", 4));

    const hwaddr i2c_addresses[] = {
        0x98007d00, 0x98007c00, 0x9801b900, 0x9801ba00, 0x9801bb00
    };
    const unsigned i2c_irqs[] = {8, 11, 23, 15, 14};
    for (unsigned i = 0; i < ARRAY_SIZE(i2c_addresses); i++) {
        DeviceState *i2c = qdev_new("designware-i2c");
        g_autofree char *name = g_strdup_printf("i2c%u", i);
        object_property_add_child(OBJECT(machine), name, OBJECT(i2c));
        sysbus_realize_and_unref(SYS_BUS_DEVICE(i2c), &error_fatal);
        /* Stock DT: pericom,pt7c4337 on i2c1 at 0x68. Synology's
         * rtc-ds1307 maps that chip to ds_1307. DS1338 implements the
         * compatible calendar interface; this is not a complete PT7C4337
         * model (oscillator-stop / battery-backed persistence excluded).
         * Time comes from QEMU's RTC clock, not fixed success values.
         */
        if (i == 1) {
            I2CBus *bus = I2C_BUS(qdev_get_child_bus(i2c, "i2c-bus"));
            i2c_slave_create_simple(bus, "ds1338", 0x68);
        }
        /* DW model has a 4K aperture; Realtek exposes just its first 256B. */
        memory_region_init_alias(&s->i2c_window[i], OBJECT(machine), name,
                                 sysbus_mmio_get_region(SYS_BUS_DEVICE(i2c), 0),
                                 0, 0x100);
        memory_region_add_subregion(sysmem, i2c_addresses[i],
                                    &s->i2c_window[i]);
        sysbus_connect_irq(SYS_BUS_DEVICE(i2c), 0,
                           qdev_get_gpio_in_named(i < 2 ? gpio : sb2,
                               i < 2 ? "iso-source" : "misc-source",
                               i2c_irqs[i]));
    }

    for (int i = 0; i < DS223_CPUS; i++) {
        DeviceState *cpu = DEVICE(qemu_get_cpu(i));
        int ppi = DS223_SPIS + i * 32;
        for (unsigned t = 0; t < ARRAY_SIZE(timer_irq); t++) {
            qdev_connect_gpio_out(cpu, t,
                                  qdev_get_gpio_in(gic, ppi + timer_irq[t]));
        }
        if (!s->experimental_gicv2) {
            qdev_connect_gpio_out_named(cpu, "gicv3-maintenance-interrupt", 0,
                                        qdev_get_gpio_in(gic, ppi + 25));
        }
        qdev_connect_gpio_out_named(cpu, "pmu-interrupt", 0,
                                    qdev_get_gpio_in(gic, ppi + 23));
        sysbus_connect_irq(gicbus, i, qdev_get_gpio_in(cpu, ARM_CPU_IRQ));
        sysbus_connect_irq(gicbus, i + DS223_CPUS,
                           qdev_get_gpio_in(cpu, ARM_CPU_FIQ));
        sysbus_connect_irq(gicbus, i + 2 * DS223_CPUS,
                           qdev_get_gpio_in(cpu, ARM_CPU_VIRQ));
        sysbus_connect_irq(gicbus, i + 3 * DS223_CPUS,
                           qdev_get_gpio_in(cpu, ARM_CPU_VFIQ));
    }

    const hwaddr uart_addresses[] = {DS223_UART0, 0x9801b200, 0x9801b400};
    for (unsigned i = 0; i < ARRAY_SIZE(uart_addresses); i++) {
        SerialMM *uart = serial_mm_init(sysmem, uart_addresses[i], 2,
            i == 0 ? qdev_get_gpio_in(gic, 68) :
            qdev_get_gpio_in_named(sb2, "misc-source", i == 1 ? 3 : 8),
            432000000 / 16, serial_hd(i), DEVICE_LITTLE_ENDIAN);
        memory_region_init_io(&s->uart_ext[i], OBJECT(machine),
                              &ds223_uart_ext_ops, &uart->serial,
                              "ds223-uart-ext", 0xe0);
        memory_region_add_subregion(sysmem, uart_addresses[i] + 0x20,
                                    &s->uart_ext[i]);
    }

    s->boot.ram_size = machine->ram_size;
    s->disk_presence_ready.notify = ds223_disk_presence;
    qemu_add_machine_init_done_notifier(&s->disk_presence_ready);
    if (s->experimental_gicv2 && kvm_enabled()) {
        /* A KVM host CPU may expose a PMU. Enabling its CPU feature without
         * configuring/initializing the vPMU makes the first KVM_RUN fail. */
        for (unsigned i = 0; i < DS223_CPUS; i++) {
            ARMCPU *cpu = ARM_CPU(qemu_get_cpu(i));
            if (arm_feature(&cpu->env, ARM_FEATURE_PMU)) {
                kvm_arm_pmu_set_irq(cpu, 23);
                kvm_arm_pmu_init(cpu);
            }
        }
    }
    s->boot.loader_start = 0;
    s->boot.board_id = -1;
    s->boot.psci_conduit = s->experimental_gicv2 ? QEMU_PSCI_CONDUIT_HVC :
                                                QEMU_PSCI_CONDUIT_SMC;
    s->boot.modify_dtb = ds223_boot_dtb;
    arm_load_kernel(ARM_CPU(qemu_get_cpu(0)), machine, &s->boot);
}

static bool ds223_get_gicv2(Object *obj, Error **errp)
{
    return DS223_MACHINE(obj)->experimental_gicv2;
}

static void ds223_set_gicv2(Object *obj, bool value, Error **errp)
{
    DS223_MACHINE(obj)->experimental_gicv2 = value;
}

static void ds223_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    mc->desc = "Experimental Synology DS223 (RTD1619B) boot probe";
    mc->init = ds223_init;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a55");
    mc->default_ram_size = 2 * GiB;
    mc->default_ram_id = "ds223.ram";
    mc->min_cpus = DS223_CPUS;
    mc->max_cpus = DS223_CPUS;
    mc->default_cpus = DS223_CPUS;
    mc->no_cdrom = true;
    object_class_property_add_bool(oc, "experimental-gicv2",
                                   ds223_get_gicv2, ds223_set_gicv2);
    object_class_property_set_description(oc, "experimental-gicv2",
        "Experimental GICv2 with adapted in-memory DT and CPU affinities");
}

static const TypeInfo ds223_type = {
    .name = TYPE_DS223_MACHINE,
    .parent = TYPE_MACHINE,
    .instance_size = sizeof(DS223MachineState),
    .class_init = ds223_class_init,
    .interfaces = aarch64_machine_interfaces,
};

static void ds423_class_init(ObjectClass *oc, const void *data)
{
    MACHINE_CLASS(oc)->desc = "Experimental DS423/RTD1619B PCIe board";
}

static void ds223_register_types(void)
{
    type_register_static(&ds223_type);
    static const TypeInfo ds423_type = {
        .name = MACHINE_TYPE_NAME("ds423"),
        .parent = TYPE_DS223_MACHINE,
        .class_init = ds423_class_init,
    };
    type_register_static(&ds423_type);
}
type_init(ds223_register_types)
