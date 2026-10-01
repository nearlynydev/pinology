/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * RTD1619B GPIO and GPIO-only ISO IRQ mux subset. See GPIO.md for limits.
 * Layout: Synology GPL gpio-rtd.c and irq-realtek-mux.c, DS223 model.dtb.
 * The ISO +4 enable alias is an experimental driver-compatibility assumption,
 * not a verified silicon specification. Debounce configuration is stored only.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"

#define TYPE_RTD1619B_GPIO "rtd1619b-gpio"
OBJECT_DECLARE_SIMPLE_TYPE(RtdGpioState, RTD1619B_GPIO)

#define NPINS 82
#define GPIO_IRQS ((1U << 19) | (1U << 20))
#define ISO_SUPPORTED_IRQS (GPIO_IRQS | (1U << 4) | (1U << 8) | (1U << 11))

typedef struct RtdGpioRegion {
    MemoryRegion mr;
    RtdGpioState *state;
    hwaddr offset;
} RtdGpioRegion;

struct RtdGpioState {
    SysBusDevice parent_obj;
    RtdGpioRegion regions[4];
    qemu_irq irq;
    qemu_irq outputs[NPINS];
    uint32_t dir[3], dato[3], dati[3], ie[3], dp[3];
    uint32_t gpa[3], gpda[3], debounce[11];
    uint32_t iso_enable;
    MemoryRegion pinctrl;
    uint32_t pinconf[0xf18 / 4];
    MemoryRegion sda;
    uint32_t sda_delay[2], iso_inputs;
};

/*
 * Vendor pinctrl-rtd1619b.h mux and pad configuration storage. Electrical
 * effects and routing to GPIO/peripheral outputs are not modeled yet.
 */
static bool pinctrl_known(hwaddr a)
{
    return a <= 0x80 || a == 0x120 || a == 0x124 || a == 0xf14;
}
static MemTxResult pinctrl_read(void *opaque, hwaddr a, uint64_t *v,
                               unsigned size, MemTxAttrs attrs)
{
    RtdGpioState *s = opaque;
    *v = 0;
    if (!pinctrl_known(a)) {
        return MEMTX_ERROR;
    }
    *v = s->pinconf[a / 4];
    return MEMTX_OK;
}
static MemTxResult pinctrl_write(void *opaque, hwaddr a, uint64_t v,
                                unsigned size, MemTxAttrs attrs)
{
    RtdGpioState *s = opaque;
    if (!pinctrl_known(a)) {
        return MEMTX_ERROR;
    }
    s->pinconf[a / 4] = v;
    return MEMTX_OK;
}
static const MemoryRegionOps pinctrl_ops = {
    .read_with_attrs = pinctrl_read, .write_with_attrs = pinctrl_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static uint32_t gpio_pending(RtdGpioState *s)
{
    uint32_t pending = s->iso_inputs & ISO_SUPPORTED_IRQS;

    /* Status banks have 31 pins, unlike the 32-pin configuration banks. */
    for (unsigned pin = 0; pin < NPINS; pin++) {
        uint32_t bit = 1U << (pin % 31 + 1);
        if (!(s->ie[pin / 32] & (1U << (pin % 32)))) {
            continue;
        }
        if (s->gpa[pin / 31] & bit) {
            pending |= 1U << 19;
        }
        if (s->gpda[pin / 31] & bit) {
            pending |= 1U << 20;
        }
    }
    return pending;
}

static void gpio_update(RtdGpioState *s)
{
    qemu_set_irq(s->irq, !!(gpio_pending(s) & s->iso_enable));
    for (unsigned pin = 0; pin < NPINS; pin++) {
        uint32_t bit = 1U << (pin % 32);
        qemu_set_irq(s->outputs[pin],
                     !!(s->dir[pin / 32] & s->dato[pin / 32] & bit));
    }
}

static void gpio_input(void *opaque, int pin, int level)
{
    RtdGpioState *s = opaque;
    unsigned bank = pin / 32;
    uint32_t bit = 1U << (pin % 32);
    bool old = !!(s->dati[bank] & bit);

    level = !!level;
    s->dati[bank] = (s->dati[bank] & ~bit) | (level ? bit : 0);
    if (old != level && !(s->dir[bank] & bit) && (s->ie[bank] & bit)) {
        uint32_t *status = level == !!(s->dp[bank] & bit) ? s->gpa : s->gpda;
        status[pin / 31] |= 1U << (pin % 31 + 1);
    }
    gpio_update(s);
}

static void iso_input(void *opaque, int source, int level)
{
    RtdGpioState *s = opaque;
    s->iso_inputs = (s->iso_inputs & ~(1U << source)) |
                    (level ? 1U << source : 0);
    gpio_update(s);
}

static uint64_t sda_read(void *opaque, hwaddr a, unsigned size)
{
    RtdGpioState *s = opaque;
    return s->sda_delay[a / 4];
}
static void sda_write(void *opaque, hwaddr a, uint64_t v, unsigned size)
{
    RtdGpioState *s = opaque;
    s->sda_delay[a / 4] = v;
}
static const MemoryRegionOps sda_ops = {
    .read = sda_read, .write = sda_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static uint32_t *gpio_config(RtdGpioState *s, hwaddr a,
                              unsigned *bank, unsigned *kind)
{
    static const unsigned bases[] = {0x100, 0x118, 0x12c};
    uint32_t *regs[] = {s->dir, s->dato, s->dati, s->ie, s->dp};

    for (unsigned b = 0; b < 3; b++) {
        if (a >= bases[b] && a <= bases[b] + 0x10) {
            *bank = b;
            *kind = (a - bases[b]) / 4;
            return &regs[*kind][b];
        }
    }
    return NULL;
}

static uint32_t *gpio_status(RtdGpioState *s, hwaddr a)
{
    switch (a) {
    case 0x08: return &s->gpa[0];
    case 0x0c: return &s->gpda[0];
    case 0xe0: return &s->gpa[1];
    case 0xe4: return &s->gpda[1];
    case 0x90: return &s->gpa[2];
    case 0x94: return &s->gpda[2];
    default: return NULL;
    }
}

static MemTxResult gpio_read(void *opaque, hwaddr offset, uint64_t *value,
                             unsigned size, MemTxAttrs attrs)
{
    RtdGpioRegion *r = opaque;
    RtdGpioState *s = r->state;
    hwaddr a = r->offset + offset;
    unsigned bank, kind;
    uint32_t *reg = gpio_config(s, a, &bank, &kind);

    if (!reg) {
        reg = gpio_status(s, a);
    }
    if (reg) {
        *value = *reg;
    } else if (a == 0) {
        *value = gpio_pending(s);
    } else if (a == 4 || a == 0x40) {
        *value = s->iso_enable;
    } else if (a >= 0x144 && a <= 0x16c) {
        *value = s->debounce[(a - 0x144) / 4];
    } else {
        qemu_log_mask(LOG_UNIMP, "rtd1619b-gpio: unsupported read ISO+0x%"
                      HWADDR_PRIx "\n", a);
        *value = 0;
        return MEMTX_ERROR;
    }
    return MEMTX_OK;
}

static MemTxResult gpio_write(void *opaque, hwaddr offset, uint64_t value,
                              unsigned size, MemTxAttrs attrs)
{
    RtdGpioRegion *r = opaque;
    RtdGpioState *s = r->state;
    hwaddr a = r->offset + offset;
    unsigned bank, kind;
    uint32_t *reg = gpio_config(s, a, &bank, &kind);
    uint32_t *status = gpio_status(s, a);

    if (reg) {
        if (kind != 2) { /* DATI is read-only, independent of output latch. */
            *reg = value & (bank == 2 ? 0x3ffff : UINT32_MAX);
        }
    } else if (status) {
        *status &= ~value; /* W1C; reserved bit zero never becomes pending. */
    } else if (a == 0) {
        /* Level-derived parent: child status must be cleared first. */
    } else if (a == 4 || a == 0x40) {
        /* +4 is also used for per-source unmask/ACK, including Ethernet
         * POR sources not modeled here. A full replacement here loses
         * sibling enables and makes the initial RTC probe time out.
         * Compatibility subset: additive unmask at +4, explicit RW enable
         * at +0x40. Independent latched mask/ACK semantics remain unproven;
         * see GPIO.md. No pending interrupt is synthesized by these writes.
         */
        if (a == 4) {
            s->iso_enable |= value & ISO_SUPPORTED_IRQS;
        } else {
            s->iso_enable = value & ISO_SUPPORTED_IRQS;
        }
        if (value & ~ISO_SUPPORTED_IRQS) {
            qemu_log_mask(LOG_UNIMP, "rtd1619b-gpio: unmodeled ISO IRQ "
                          "enable bits 0x%08x\n",
                          (uint32_t)value & ~ISO_SUPPORTED_IRQS);
        }
    } else if (a >= 0x144 && a <= 0x16c) {
        unsigned index = (a - 0x144) / 4;
        for (unsigned field = 0; field < 8 && index * 8 + field < NPINS;
             field++) {
            unsigned shift = field * 4;
            if (value & (8U << shift)) {
                s->debounce[index] = (s->debounce[index] & ~(7U << shift)) |
                                     (value & (7U << shift));
            }
        }
    } else {
        qemu_log_mask(LOG_UNIMP, "rtd1619b-gpio: unsupported write ISO+0x%"
                      HWADDR_PRIx "\n", a);
        return MEMTX_ERROR;
    }
    gpio_update(s);
    return MEMTX_OK;
}

static const MemoryRegionOps gpio_ops = {
    .read_with_attrs = gpio_read,
    .write_with_attrs = gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static void gpio_reset(DeviceState *dev)
{
    RtdGpioState *s = RTD1619B_GPIO(dev);

    memset(s->dir, 0, sizeof(s->dir));
    memset(s->dato, 0, sizeof(s->dato));
    memset(s->ie, 0, sizeof(s->ie));
    memset(s->dp, 0, sizeof(s->dp));
    memset(s->gpa, 0, sizeof(s->gpa));
    memset(s->gpda, 0, sizeof(s->gpda));
    memset(s->debounce, 0, sizeof(s->debounce));
    s->iso_enable = 0;
    memset(s->pinconf, 0, sizeof(s->pinconf));
    memset(s->sda_delay, 0, sizeof(s->sda_delay));
    /* External input levels survive controller reset. */
    gpio_update(s);
}

static int gpio_post_load(void *opaque, int version_id)
{
    gpio_update(opaque);
    return 0;
}

static const VMStateDescription gpio_vmstate = {
    .name = TYPE_RTD1619B_GPIO,
    .version_id = 4, .minimum_version_id = 4,
    .post_load = gpio_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(dir, RtdGpioState, 3),
        VMSTATE_UINT32_ARRAY(dato, RtdGpioState, 3),
        VMSTATE_UINT32_ARRAY(dati, RtdGpioState, 3),
        VMSTATE_UINT32_ARRAY(ie, RtdGpioState, 3),
        VMSTATE_UINT32_ARRAY(dp, RtdGpioState, 3),
        VMSTATE_UINT32_ARRAY(gpa, RtdGpioState, 3),
        VMSTATE_UINT32_ARRAY(gpda, RtdGpioState, 3),
        VMSTATE_UINT32_ARRAY(debounce, RtdGpioState, 11),
        VMSTATE_UINT32(iso_enable, RtdGpioState),
        VMSTATE_UINT32_ARRAY(pinconf, RtdGpioState, 0xf18 / 4),
        VMSTATE_UINT32_ARRAY(sda_delay, RtdGpioState, 2),
        VMSTATE_UINT32(iso_inputs, RtdGpioState),
        VMSTATE_END_OF_LIST()
    }
};

static void gpio_init(Object *obj)
{
    RtdGpioState *s = RTD1619B_GPIO(obj);
    /* Sparse windows leave ISO clock/reset +0x88..0x8f with the CRG device. */
    static const hwaddr offsets[] = {0, 0x90, 0xe0, 0x100};
    static const hwaddr sizes[] = {0x44, 8, 8, 0x100};

    for (unsigned i = 0; i < ARRAY_SIZE(s->regions); i++) {
        RtdGpioRegion *r = &s->regions[i];
        r->state = s;
        r->offset = offsets[i];
        memory_region_init_io(&r->mr, obj, &gpio_ops, r,
                              "rtd1619b-gpio", sizes[i]);
        sysbus_init_mmio(SYS_BUS_DEVICE(obj), &r->mr);
    }
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    qdev_init_gpio_in_named(DEVICE(obj), gpio_input, "pin", NPINS);
    qdev_init_gpio_out_named(DEVICE(obj), s->outputs, "pin-out", NPINS);
    memory_region_init_io(&s->pinctrl, obj, &pinctrl_ops, s,
                          "rtd1619b-pinctrl", 0xf18);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->pinctrl);
    memory_region_init_io(&s->sda, obj, &sda_ops, s, "rtd-i2c-sda-delay", 8);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->sda);
    qdev_init_gpio_in_named(DEVICE(obj), iso_input, "iso-source", 32);
}

static void gpio_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    dc->vmsd = &gpio_vmstate;
    device_class_set_legacy_reset(dc, gpio_reset);
}

static const TypeInfo gpio_type = {
    .name = TYPE_RTD1619B_GPIO, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RtdGpioState), .instance_init = gpio_init,
    .class_init = gpio_class_init,
};

static void gpio_register_types(void)
{
    type_register_static(&gpio_type);
}
type_init(gpio_register_types)
