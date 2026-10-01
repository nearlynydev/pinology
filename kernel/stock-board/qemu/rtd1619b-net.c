/* SPDX-License-Identifier: GPL-2.0-or-later */
/* R8169 SoC management plane and bounded TX_NO_CLOSE/RX DMA engines. */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "system/dma.h"
#include "hw/net/net_tx_pkt.h"
#include "net/net.h"
#include "net/checksum.h"
#include "migration/vmstate.h"
#include "hw/pci/pci.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/msi.h"
#include "hw/pci/pcie.h"

#define TYPE_RTD_NET "rtd1619b-net"
OBJECT_DECLARE_SIMPLE_TYPE(RtdNetState, RTD_NET)
struct RtdNetState {
    SysBusDevice parent_obj;
    MemoryRegion mmio, iso, ready, por;
    uint8_t mac[256];
    uint16_t ocp[32768], phy[2 * 4096 * 32], page[2];
    uint32_t eri[1024], iso_regs[2];
    uint32_t por_regs[3];
    NICConf conf;
    NICState *nic;
    qemu_irq irq;
    uint64_t tx_base;
    uint16_t tx_tail, tx_close, tx_pos, rx_pos;
    uint32_t tx_len, tx_opts1, tx_opts2;
    bool tx_active;
    PCIDevice *pci; /* Optional experimental RTL8168B front-end; never migrated. */
    QEMUTimer *intr_timer;
    uint8_t tx_buf[65536];
};

static bool net_link(RtdNetState *s)
{
    NetClientState *nc = s->nic ? qemu_get_queue(s->nic) : NULL;
    return nc && nc->peer && !nc->link_down;
}
static void net_irq_update(RtdNetState *s)
{
    bool level = !!(lduw_le_p(s->mac + 0x3c) & lduw_le_p(s->mac + 0x3e));
    if (s->pci) {
        if (msi_enabled(s->pci)) {
            if (level) { msi_notify(s->pci, 0); }
            pci_set_irq(s->pci, 0);
        } else {
            pci_set_irq(s->pci, level);
        }
    } else {
        qemu_set_irq(s->irq, level);
    }
}
static void net_event(RtdNetState *s, uint16_t bits)
{
    stw_le_p(s->mac + 0x3e, lduw_le_p(s->mac + 0x3e) | bits);
    net_irq_update(s);
}
/* Only guest RAM, never MMIO. Reject overflow/reentrant DMA into devices. */
static bool net_ram(uint64_t addr, size_t len)
{
    return addr >= 0x40000 && addr < 0x80000000 &&
           len <= 0x80000000 - addr;
}
static bool net_dma_read(RtdNetState *s, uint64_t addr, void *buf, size_t len)
{
    return net_ram(addr, len) &&
           dma_memory_read(s->pci ? pci_get_address_space(s->pci) :
                           &address_space_memory, addr, buf, len,
                           MEMTXATTRS_UNSPECIFIED) == MEMTX_OK;
}
static bool net_dma_write(RtdNetState *s, uint64_t addr, const void *buf, size_t len)
{
    return net_ram(addr, len) &&
           dma_memory_write(s->pci ? pci_get_address_space(s->pci) :
                            &address_space_memory, addr, buf, len,
                            MEMTXATTRS_UNSPECIFIED) == MEMTX_OK;
}
static void net_keep_fragment(void *context, void *base, size_t len)
{
    /* The packet borrows the device-owned assembly buffer. */
}
static bool net_send_frame(RtdNetState *s)
{
    struct NetTxPkt *pkt;
    bool tso = s->tx_opts1 & BIT(27);
    bool csum = tso || (s->pci ? (s->tx_opts1 & (BIT(16) | BIT(17) | BIT(18))) :
                                (s->tx_opts2 & (BIT(29) | BIT(30) | BIT(31))));
    unsigned mss = s->pci ? (s->tx_opts1 >> 16) & 0x7ff :
                           (s->tx_opts2 >> 18) & 0x7ff;
    bool ok;
    if (!net_link(s) || s->tx_len < 14 ||
        (tso && !mss)) {
        return false;
    }
    if (csum && !tso) {
        unsigned flags = (s->tx_opts2 & BIT(29) ? CSUM_IP : 0) |
                         (s->tx_opts2 & BIT(30) ? CSUM_TCP : 0) |
                         (s->tx_opts2 & BIT(31) ? CSUM_UDP : 0);
        if (s->pci) {
            flags = (s->tx_opts1 & BIT(18) ? CSUM_IP : 0) |
                    (s->tx_opts1 & BIT(16) ? CSUM_TCP : 0) |
                    (s->tx_opts1 & BIT(17) ? CSUM_UDP : 0);
        }
        net_checksum_calculate(s->tx_buf, s->tx_len, flags);
    }
    net_tx_pkt_init(&pkt, 1);
    ok = net_tx_pkt_add_raw_fragment(pkt, s->tx_buf, s->tx_len) &&
         net_tx_pkt_parse(pkt);
    if (ok && (s->tx_opts2 & BIT(17))) {
        net_tx_pkt_setup_vlan_header(pkt, bswap16(s->tx_opts2));
    }
    if (ok) {
        ok = net_tx_pkt_build_vheader(pkt, tso, tso,
                                      mss);
    }
    if (ok) {
        if (tso) {
            net_tx_pkt_update_ip_checksums(pkt);
        }
        ok = net_tx_pkt_send(pkt, qemu_get_queue(s->nic));
    }
    net_tx_pkt_reset(pkt, net_keep_fragment, NULL);
    net_tx_pkt_uninit(pkt);
    return ok;
}
static void net_transmit(RtdNetState *s)
{
    if (!(s->mac[0x37] & 4)) {
        return;
    }
    for (unsigned count = 0;
         (s->pci || s->tx_close != s->tx_tail) && count < 16384; count++) {
        uint8_t desc[16];
        uint64_t at = s->tx_base + s->tx_pos * 16ULL;
        if (!net_dma_read(s, at, desc, sizeof(desc))) {
            net_event(s, 0x8008);
            break;
        }
        uint32_t opts = ldl_le_p(desc);
        if (s->pci && !(opts & BIT(31))) { break; }
        /* TX length is 16 bits; the RX-only 14-bit mask turns a legitimate
         * 16 KiB SG fragment into zero and wedges the entire TX queue.
         * Observed with stock 90080 encrypted SMB reads (opts=0x98004000).
         * The assembly-buffer and RAM bounds checks remain below.
         */
        unsigned len = opts & 0xffff;
        if (!(opts & BIT(31))) {
            break;
        }
        if (opts & BIT(29)) {
            s->tx_len = 0;
            s->tx_active = true;
            s->tx_opts1 = opts;
            s->tx_opts2 = ldl_le_p(desc + 4);
        }
        bool ok = s->tx_active && len &&
                  len <= sizeof(s->tx_buf) - s->tx_len &&
                  net_dma_read(s, ldq_le_p(desc + 8), s->tx_buf + s->tx_len, len);
        if (!ok) {
            net_event(s, 0x8008);
            s->tx_len = 0;
            s->tx_active = false;
            break;
        }
        s->tx_len += len;
        if (opts & BIT(28)) {
            ok = net_send_frame(s);
            s->tx_len = 0;
            s->tx_active = false;
        }
        stl_le_p(desc, opts & ~BIT(31));
        if (!net_dma_write(s, at, desc, 4)) {
            net_event(s, 0x8008);
            break;
        }
        s->tx_pos = opts & BIT(30) ? 0 : (s->tx_pos + 1) & 0x3fff;
        s->tx_close = (s->tx_close + 1) & 0x3fff;
        net_event(s, ok ? 4 : 8);
    }
}
static ssize_t net_receive(NetClientState *nc, const uint8_t *buf, size_t len)
{
    RtdNetState *s = qemu_get_nic_opaque(nc);
    uint8_t desc[16], crc[4] = {0};
    uint64_t at = ldq_le_p(s->mac + 0xe4) + s->rx_pos * 16ULL;
    if (!(s->mac[0x37] & 8) || !net_link(s)) {
        return -1;
    }
    if (!net_dma_read(s, at, desc, sizeof(desc))) {
        net_event(s, 0x8002);
        return len;
    }
    uint32_t opts = ldl_le_p(desc);
    if (!(opts & BIT(31))) {
        net_event(s, 0x10);
        return len;
    }
    if (len + 4 > (opts & 0x3fff)) {
        /* Drop oversized frames without a sticky RX_OVERFLOW/NAPI storm. */
        return len;
    }
    uint64_t addr = ldq_le_p(desc + 8);
    if (!net_dma_write(s, addr, buf, len) || !net_dma_write(s, addr + len, crc, 4)) {
        net_event(s, 0x8002);
        return len;
    }
    stl_le_p(desc, (opts & BIT(30)) | BIT(29) | BIT(28) | (len + 4));
    stl_le_p(desc + 4, 0); /* Leave checksum verification to the guest. */
    if (!net_dma_write(s, at, desc, 8)) {
        net_event(s, 0x8002);
        return len;
    }
    /*
     * Ring size belongs to the guest: the stock binary can differ from GPL
     * defconfig NUM_RX_DESC. Only the descriptor EOR bit wraps the ring.
     */
    if (s->rx_pos == 1023 && !(opts & BIT(30))) {
        qemu_log_mask(LOG_UNIMP,
                      "rtd-net: RX ring extends past 1024 entries\n");
    }
    s->rx_pos = opts & BIT(30) ? 0 : (s->rx_pos + 1) & 0x3fff;
    net_event(s, 1);
    return len;
}
static void net_link_changed(NetClientState *nc)
{
    net_event(qemu_get_nic_opaque(nc), 0x20);
}
static void net_timer_expired(void *opaque)
{
    RtdNetState *s = opaque;
    if (s->pci && ldl_le_p(s->mac + 0x58)) {
        net_event(s, 0x4000);
    }
}
static NetClientInfo net_info = {
    .type = NET_CLIENT_DRIVER_NIC, .size = sizeof(NICState),
    .receive = net_receive, .link_status_changed = net_link_changed,
};

static uint16_t phy_read(RtdNetState *s, unsigned ext, unsigned page,
                         unsigned reg)
{
    if (reg == 31) {
        return s->page[ext];
    }
    if (page >= 4096) {
        return 0xffff;
    }
    if (!page && reg == 1) {
        return 0x7809 | (net_link(s) ? 0x24 : 0);
    }
    return s->phy[(ext * 4096 + page) * 32 + reg];
}

static void phy_write(RtdNetState *s, unsigned ext, unsigned page,
                       unsigned reg, uint16_t value)
{
    if (reg == 31) {
        s->page[ext] = value;
    } else if (page < 4096 && (page || reg != 1)) {
        /* BMCR software reset self-clears; no physical link negotiation. */
        s->phy[(ext * 4096 + page) * 32 + reg] =
            !page && !reg ? value & ~0x8000 : value;
    }
}

static MemTxResult net_read(void *opaque, hwaddr a, uint64_t *v,
                            unsigned size, MemTxAttrs attrs)
{
    RtdNetState *s = opaque;
    if (a + size > sizeof(s->mac)) {
        return MEMTX_ERROR;
    }
    if (!s->pci && a == 0x20 && size == 2) {
        *v = s->tx_tail;
        return MEMTX_OK;
    }
    if (!s->pci && a == 0x22 && size == 2) {
        *v = s->tx_close;
        return MEMTX_OK;
    }
    s->mac[0x6c] = net_link(s) ? 0x13 : 0;
    *v = 0;
    for (unsigned i = 0; i < size; i++) {
        *v |= (uint64_t)s->mac[a + i] << (8 * i);
    }
    return MEMTX_OK;
}

static MemTxResult net_write(void *opaque, hwaddr a, uint64_t v,
                             unsigned size, MemTxAttrs attrs)
{
    RtdNetState *s = opaque;
    if (a + size > sizeof(s->mac)) {
        return MEMTX_ERROR;
    }
    if (a == 0x20 && size == 4) {
        s->tx_base = (s->tx_base & 0xffffffff00000000ULL) | v;
        s->tx_tail = s->tx_close = s->tx_pos = s->tx_len = 0;
        s->tx_active = false;
    } else if (a == 0x24 && size == 4) {
        s->tx_base = (s->tx_base & 0xffffffff) | (v << 32);
    } else if (!s->pci && a == 0x20 && size == 2) {
        s->tx_tail = v & 0x3fff;
        return MEMTX_OK;
    } else if (!s->pci && a == 0x22 && size == 2) {
        return MEMTX_OK;
    } else if (a == 0xe4 && size == 4) {
        s->rx_pos = 0;
    } else if (a == 0x3e) {
        v = lduw_le_p(s->mac + 0x3e) & ~v; /* Interrupt status W1C. */
    } else if (a == 0x37) {
        if (v & 0x10) {
            timer_del(s->intr_timer);
            stl_le_p(s->mac + 0x58, 0);
            s->tx_tail = s->tx_close = s->tx_pos = s->rx_pos = s->tx_len = 0;
            s->tx_active = false;
            stw_le_p(s->mac + 0x3e, 0);
        }
        v &= ~0x10U;
    } else if (a == 0x38) {
        if (v & 0x40) {
            net_transmit(s);
        }
        return MEMTX_OK;
    } else if (a == 0x6c) {
        return MEMTX_OK; /* Read-only actual backend link state. */
    } else if (s->pci && a == 0x40 && size == 4) {
        v = (v & ~0x7cf00000U) | 0x30000000; /* RTL8168B CFG_METHOD_1. */
    } else if (a == 0x68 && size == 4) {
        /* CSI config reads/writes complete; no PCI function behind this MAC. */
        v ^= BIT(31);
    } else if ((a == 0x60 || a == 0x80) && size == 4) {
        unsigned ext = a == 0x80;
        unsigned reg = (v >> 16) & 31;
        if (v & 0x80000000) {
            phy_write(s, ext, s->page[ext], reg, v);
            v &= ~0x80000000U;
        } else {
            v = 0x80000000U | phy_read(s, ext, s->page[ext], reg);
        }
    } else if (a == 0xb0 && size == 4) {
        unsigned index = (v >> 16) & 0x7fff;
        if (v & 0x80000000) {
            s->ocp[index] = v;
            if (index == 0xde0a / 2 || index == 0xde2a / 2) {
                unsigned ext = index == 0xde2a / 2;
                unsigned page = s->ocp[index + 1];
                unsigned reg = v & 31;
                if (v & 0x8000) {
                    phy_write(s, ext, page, reg, s->ocp[index - 1]);
                } else {
                    s->ocp[index - 1] = phy_read(s, ext, page, reg);
                }
                s->ocp[index] &= ~0x4000; /* MDIO command completed. */
            }
        } else {
            v = s->ocp[index];
        }
    } else if (a == 0x74 && size == 4) {
        unsigned index = (v & 0xffc) / 4;
        if (v & 0x80000000) {
            uint32_t data = ldl_le_p(s->mac + 0x70);
            for (unsigned i = 0; i < 4; i++) {
                if (v & (1U << (12 + i))) {
                    uint32_t mask = 0xffU << (8 * i);
                    s->eri[index] = (s->eri[index] & ~mask) | (data & mask);
                }
            }
            v &= ~0x80000000U;
        } else {
            stl_le_p(s->mac + 0x70, s->eri[index]);
            v |= 0x80000000U;
        }
    }
    for (unsigned i = 0; i < size; i++) {
        s->mac[a + i] = v >> (8 * i);
    }
    /* RTL8168B driver switches to TimerInt after NAPI. Without a real delayed
     * event its 0x4040 mask stalls after the first packet. Use a one-shot PCI
     * 33 MHz timer approximation; no claim of exact physical moderation. */
    if (s->pci && (a == 0x48 || a == 0x58) && size == 4) {
        uint32_t ticks = ldl_le_p(s->mac + 0x58);
        timer_del(s->intr_timer);
        if (ticks) {
            timer_mod(s->intr_timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                      MAX(1ULL, (uint64_t)ticks * 1000000000ULL / 33000000));
        }
    }
    net_irq_update(s);
    if (a == 0x37 && (v & 8)) {
        qemu_flush_queued_packets(qemu_get_queue(s->nic));
    }
    return MEMTX_OK;
}

static uint64_t net_iso_read(void *opaque, hwaddr a, unsigned size)
{
    return RTD_NET(opaque)->iso_regs[a / 4];
}
static void net_iso_write(void *opaque, hwaddr a, uint64_t v, unsigned size)
{
    RTD_NET(opaque)->iso_regs[a / 4] = v;
}
static const MemoryRegionOps net_ops = {
    .read_with_attrs = net_read, .write_with_attrs = net_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};
static const MemoryRegionOps net_iso_ops = {
    .read = net_iso_read, .write = net_iso_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};
static uint64_t net_por_read(void *opaque, hwaddr a, unsigned size)
{
    return RTD_NET(opaque)->por_regs[a / 4];
}
static void net_por_write(void *opaque, hwaddr a, uint64_t v, unsigned size)
{
    if (a < 8) {
        RTD_NET(opaque)->por_regs[a / 4] = v;
    }
}
static const MemoryRegionOps net_por_ops = {
    .read = net_por_read, .write = net_por_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};
static uint64_t net_ready_read(void *opaque, hwaddr a, unsigned size)
{
    return 1; /* Management register reset state installed; RBUS accessible. */
}
static void net_ready_write(void *opaque, hwaddr a, uint64_t v, unsigned size)
{
    /* Read-only autoload completion, not link or packet readiness. */
}
static const MemoryRegionOps net_ready_ops = {
    .read = net_ready_read, .write = net_ready_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};
static void net_reset(DeviceState *dev)
{
    RtdNetState *s = RTD_NET(dev);
    timer_del(s->intr_timer);
    memset(s->mac, 0, sizeof(s->mac));
    memset(s->ocp, 0, sizeof(s->ocp));
    memset(s->phy, 0, sizeof(s->phy));
    memset(s->page, 0, sizeof(s->page));
    memset(s->eri, 0, sizeof(s->eri));
    memset(s->iso_regs, 0, sizeof(s->iso_regs));
    memset(s->por_regs, 0, sizeof(s->por_regs));
    s->mac[0] = 2; /* Locally administered synthetic lab address. */
    s->mac[5] = 0x23;
    if (s->pci) {
        memcpy(s->mac, s->conf.macaddr.a, 6);
        stl_le_p(s->mac + 0x40, 0x30000000);
    }
    s->mac[0xd3] = 0x30; /* Synchronous packet engines are idle. */
    s->tx_base = 0;
    s->tx_tail = s->tx_close = s->tx_pos = s->rx_pos = s->tx_len = 0;
    s->tx_opts1 = s->tx_opts2 = 0;
    s->tx_active = false;
    net_irq_update(s);
}
static void net_init(Object *obj)
{
    RtdNetState *s = RTD_NET(obj);
    s->intr_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, net_timer_expired, s);
    memory_region_init_io(&s->mmio, obj, &net_ops, s, "rtd-net", 0x100);
    memory_region_init_io(&s->iso, obj, &net_iso_ops, s, "rtd-net-iso", 8);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iso);
    memory_region_init_io(&s->ready, obj, &net_ready_ops, s,
                          "rtd-net-ready", 4);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->ready);
    memory_region_init_io(&s->por, obj, &net_por_ops, s, "rtd-net-por", 12);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->por);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}
static void net_realize(DeviceState *dev, Error **errp)
{
    RtdNetState *s = RTD_NET(dev);
    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&net_info, &s->conf, object_get_typename(OBJECT(dev)),
                         dev->id, &dev->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
}
static int net_post_load(void *opaque, int version_id)
{
    RtdNetState *s = opaque;
    if (s->tx_len > sizeof(s->tx_buf) ||
        s->tx_pos > 0x3fff || s->rx_pos > 0x3fff ||
        s->tx_tail > 0x3fff || s->tx_close > 0x3fff) {
        return -EINVAL;
    }
    net_irq_update(s);
    return 0;
}
static const Property net_properties[] = {
    DEFINE_NIC_PROPERTIES(RtdNetState, conf),
};
static const VMStateDescription net_vmstate = {
    .name = TYPE_RTD_NET, .version_id = 4, .minimum_version_id = 4,
    .post_load = net_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(mac, RtdNetState, 256),
        VMSTATE_UINT16_ARRAY(ocp, RtdNetState, 32768),
        VMSTATE_UINT16_ARRAY(phy, RtdNetState, 2 * 4096 * 32),
        VMSTATE_UINT16_ARRAY(page, RtdNetState, 2),
        VMSTATE_UINT32_ARRAY(eri, RtdNetState, 1024),
        VMSTATE_UINT32_ARRAY(iso_regs, RtdNetState, 2),
        VMSTATE_UINT32_ARRAY(por_regs, RtdNetState, 3),
        VMSTATE_UINT64(tx_base, RtdNetState),
        VMSTATE_UINT16(tx_tail, RtdNetState),
        VMSTATE_UINT16(tx_close, RtdNetState),
        VMSTATE_UINT16(tx_pos, RtdNetState),
        VMSTATE_UINT16(rx_pos, RtdNetState),
        VMSTATE_UINT32(tx_len, RtdNetState),
        VMSTATE_UINT32(tx_opts1, RtdNetState),
        VMSTATE_UINT32(tx_opts2, RtdNetState),
        VMSTATE_BOOL(tx_active, RtdNetState),
        VMSTATE_TIMER_PTR(intr_timer, RtdNetState),
        VMSTATE_UINT8_ARRAY(tx_buf, RtdNetState, 65536),
        VMSTATE_END_OF_LIST()
    }
};
static void net_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->vmsd = &net_vmstate;
    dc->realize = net_realize;
    device_class_set_props(dc, net_properties);
    device_class_set_legacy_reset(dc, net_reset);
}
static const TypeInfo net_type = {
    .name = TYPE_RTD_NET, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RtdNetState), .instance_init = net_init,
    .class_init = net_class_init,
};

/* Synthetic RTL8168B-compatible PCI endpoint, not the physical DS423 NIC BOM.
 * BAR2/TxConfig/legacy OWN descriptor contract follows Realtek's GPL driver.
 * Kept behind a board opt-in until stock driver/network tests pass.
 */
#define TYPE_RTD_PCI_NET "rtd-r8168-pci"
OBJECT_DECLARE_SIMPLE_TYPE(RtdPciNet, RTD_PCI_NET)
struct RtdPciNet {
    PCIDevice parent_obj;
    NICConf conf;
    RtdNetState core;
    MemoryRegion io_alias, mem_alias;
};
static void pci_net_init(Object *obj)
{
    RtdPciNet *s = RTD_PCI_NET(obj);
    object_initialize_child(obj, "core", &s->core, TYPE_RTD_NET);
}
static void pci_net_realize(PCIDevice *dev, Error **errp)
{
    RtdPciNet *s = RTD_PCI_NET(dev);
    s->core.pci = dev;
    s->core.conf = s->conf; /* Do not reuse the SoC's global netdev/MAC. */
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->core), errp)) { return; }
    dev->config[PCI_INTERRUPT_PIN] = 1;
    memory_region_init_alias(&s->io_alias, OBJECT(dev), "r8168-io",
                             &s->core.mmio, 0, 0x100);
    memory_region_init_alias(&s->mem_alias, OBJECT(dev), "r8168-mmio",
                             &s->core.mmio, 0, 0x100);
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_IO, &s->io_alias);
    pci_register_bar(dev, 2, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mem_alias);
    if (msi_init(dev, 0x50, 1, true, false, errp)) { return; }
    pcie_endpoint_cap_init(dev, 0x70);
}
static const Property pci_net_properties[] = {
    DEFINE_NIC_PROPERTIES(RtdPciNet, conf),
};
static void pci_net_class_init(ObjectClass *oc, const void *data)
{
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(oc);
    pc->realize = pci_net_realize;
    pc->vendor_id = 0x10ec;
    pc->device_id = 0x8168;
    pc->revision = 1;
    pc->class_id = PCI_CLASS_NETWORK_ETHERNET;
    device_class_set_props(DEVICE_CLASS(oc), pci_net_properties);
}
static const TypeInfo pci_net_type = {
    .name = TYPE_RTD_PCI_NET, .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(RtdPciNet), .instance_init = pci_net_init,
    .class_init = pci_net_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_PCIE_DEVICE }, { }
    },
};
static void net_register_types(void)
{
    type_register_static(&net_type);
    type_register_static(&pci_net_type);
}
type_init(net_register_types)
