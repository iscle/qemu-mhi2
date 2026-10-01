/*
 * nVidia Tegra GPIO controller.
 *
 * Eight banks each control four 8-pin ports (256 GPIOs).  Each port exposes
 * configuration (CNF), direction (OE), output (OUT), input (IN) and interrupt
 * (INT_STA/INT_ENB/INT_LVL/INT_CLR) registers, plus a "masked" alias of each
 * writable register that updates only the bits selected by the mask carried in
 * bits [15:8] of the written value.  A per-bank interrupt is asserted while any
 * enabled, latched status bit is set in that bank.
 *
 * The register semantics follow the Tegra3 TRM (arhw/argpio.h).
 */

#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "hw/gpio/tegra30_gpio.h"

/* Register groups within a bank; each group holds one 32-bit word per port. */
enum {
    REG_CNF         = 0x0,
    REG_OE          = 0x1,
    REG_OUT         = 0x2,
    REG_IN          = 0x3,
    REG_INT_STA     = 0x4,
    REG_INT_ENB     = 0x5,
    REG_INT_LVL     = 0x6,
    REG_INT_CLR     = 0x7,
    REG_MSK_CNF     = 0x8,
    REG_MSK_OE      = 0x9,
    REG_MSK_OUT     = 0xa,
    REG_MSK_INT_STA = 0xc,
    REG_MSK_INT_ENB = 0xd,
    REG_MSK_INT_LVL = 0xe,
};

/* INT_LVL field layout (per pin p): polarity bit p, EDGE bit p+8, DELTA p+16. */
#define INT_LVL_EDGE_SHIFT  8
#define INT_LVL_DELTA_SHIFT 16

static void tegra30_gpio_update_bank(Tegra30GpioState *s, int bank)
{
    uint32_t pending = 0;

    for (int p = 0; p < TEGRA30_GPIO_PORTS_PER_BANK; p++) {
        int port = bank * TEGRA30_GPIO_PORTS_PER_BANK + p;
        pending |= s->int_sta[port] & s->int_enb[port];
    }
    qemu_set_irq(s->irq[bank], !!pending);
}

/* Latch the interrupt status for one pin after its input level changed. */
static void tegra30_gpio_eval_irq(Tegra30GpioState *s, int port, int pin,
                                  bool old_level, bool new_level)
{
    uint32_t mask = 1u << pin;
    bool lvl   = s->int_lvl[port] & mask;
    bool edge  = s->int_lvl[port] & (mask << INT_LVL_EDGE_SHIFT);
    bool delta = s->int_lvl[port] & (mask << INT_LVL_DELTA_SHIFT);
    bool fire;

    if (edge) {
        fire = delta ? (old_level != new_level)
                     : (lvl ? (!old_level && new_level)
                            : (old_level && !new_level));
    } else {
        fire = (new_level == lvl);
    }

    if (fire) {
        s->int_sta[port] |= mask;
        tegra30_gpio_update_bank(s, port / TEGRA30_GPIO_PORTS_PER_BANK);
    }
}

/* External input line into the controller (driven by a companion device). */
static void tegra30_gpio_set_in(void *opaque, int gpio, int level)
{
    Tegra30GpioState *s = opaque;
    int port = gpio / TEGRA30_GPIO_PINS_PER_PORT;
    int pin = gpio % TEGRA30_GPIO_PINS_PER_PORT;
    uint32_t mask = 1u << pin;
    bool old_level = s->in[port] & mask;

    if (level) {
        s->in[port] |= mask;
    } else {
        s->in[port] &= ~mask;
    }
    if (!!level != old_level) {
        tegra30_gpio_eval_irq(s, port, pin, old_level, !!level);
    }
}

/* Reflect OUT/OE changes onto the driven output-pin lines. */
static void tegra30_gpio_refresh_outputs(Tegra30GpioState *s, int port,
                                         uint32_t prev_driven_out)
{
    uint32_t driven_out = s->out[port] & s->oe[port];
    uint32_t changed = driven_out ^ prev_driven_out;

    for (int pin = 0; pin < TEGRA30_GPIO_PINS_PER_PORT; pin++) {
        if (changed & (1u << pin)) {
            int gpio = port * TEGRA30_GPIO_PINS_PER_PORT + pin;
            qemu_set_irq(s->output[gpio], !!(driven_out & (1u << pin)));
        }
    }
}

static uint64_t tegra30_gpio_read(void *opaque, hwaddr offset, unsigned size)
{
    Tegra30GpioState *s = TEGRA30_GPIO(opaque);
    int bank = offset / TEGRA30_GPIO_BANK_STRIDE;
    int local = offset % TEGRA30_GPIO_BANK_STRIDE;
    int group = local / 0x10;
    int port = bank * TEGRA30_GPIO_PORTS_PER_BANK + (local % 0x10) / 4;

    if (bank >= TEGRA30_GPIO_BANKS) {
        return 0;
    }

    switch (group) {
    case REG_CNF:
    case REG_MSK_CNF:
        return s->cnf[port];
    case REG_OE:
    case REG_MSK_OE:
        return s->oe[port];
    case REG_OUT:
    case REG_MSK_OUT:
        return s->out[port];
    case REG_IN:
        /* Output pins read back their driven value; inputs the pin level. */
        return (s->in[port] & ~s->oe[port]) | (s->out[port] & s->oe[port]);
    case REG_INT_STA:
    case REG_MSK_INT_STA:
        return s->int_sta[port];
    case REG_INT_ENB:
    case REG_MSK_INT_ENB:
        return s->int_enb[port];
    case REG_INT_LVL:
    case REG_MSK_INT_LVL:
        return s->int_lvl[port];
    default:
        return 0;
    }
}

/* Apply a register write, honouring masked-alias semantics (mask in [15:8]). */
static uint32_t tegra30_gpio_apply(uint32_t old, uint64_t value, bool masked,
                                   uint32_t width_mask)
{
    if (masked) {
        uint32_t mask = (value >> 8) & 0xff;
        return (old & ~mask) | ((uint32_t)value & mask);
    }
    return (uint32_t)value & width_mask;
}

static void tegra30_gpio_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size)
{
    Tegra30GpioState *s = TEGRA30_GPIO(opaque);
    int bank = offset / TEGRA30_GPIO_BANK_STRIDE;
    int local = offset % TEGRA30_GPIO_BANK_STRIDE;
    int group = local / 0x10;
    int port = bank * TEGRA30_GPIO_PORTS_PER_BANK + (local % 0x10) / 4;
    uint32_t prev_driven_out;

    if (bank >= TEGRA30_GPIO_BANKS) {
        return;
    }

    switch (group) {
    case REG_CNF:
    case REG_MSK_CNF:
        s->cnf[port] = tegra30_gpio_apply(s->cnf[port], value,
                                          group == REG_MSK_CNF, 0xff);
        break;
    case REG_OE:
    case REG_MSK_OE:
        prev_driven_out = s->out[port] & s->oe[port];
        s->oe[port] = tegra30_gpio_apply(s->oe[port], value,
                                         group == REG_MSK_OE, 0xff);
        tegra30_gpio_refresh_outputs(s, port, prev_driven_out);
        break;
    case REG_OUT:
    case REG_MSK_OUT:
        prev_driven_out = s->out[port] & s->oe[port];
        s->out[port] = tegra30_gpio_apply(s->out[port], value,
                                          group == REG_MSK_OUT, 0xff);
        tegra30_gpio_refresh_outputs(s, port, prev_driven_out);
        break;
    case REG_INT_ENB:
    case REG_MSK_INT_ENB:
        s->int_enb[port] = tegra30_gpio_apply(s->int_enb[port], value,
                                              group == REG_MSK_INT_ENB, 0xff);
        tegra30_gpio_update_bank(s, bank);
        break;
    case REG_INT_LVL:
        s->int_lvl[port] = value & 0xffffff;
        break;
    case REG_MSK_INT_LVL:
        s->int_lvl[port] = tegra30_gpio_apply(s->int_lvl[port], value,
                                              true, 0xffffff);
        break;
    case REG_INT_STA:
    case REG_INT_CLR:
        /* Write-1-to-clear the latched status bits. */
        s->int_sta[port] &= ~((uint32_t)value & 0xff);
        tegra30_gpio_update_bank(s, bank);
        break;
    case REG_MSK_INT_STA:
        /* Masked status write clears the selected bits whose data is 0. */
        s->int_sta[port] = tegra30_gpio_apply(s->int_sta[port], value,
                                              true, 0xff);
        tegra30_gpio_update_bank(s, bank);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps tegra30_gpio_ops = {
    .read = tegra30_gpio_read,
    .write = tegra30_gpio_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    /*
     * Software accesses the per-port registers as 8/16/32-bit; in particular
     * the masked-write aliases are written as 16-bit (mask in [15:8], data in
     * [7:0]).  Each register only occupies one port slot, so pass sub-word
     * accesses straight through to the handlers.
     */
    .valid = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
    .impl = {
        .min_access_size = 1,
        .max_access_size = 4,
    },
};

static void tegra30_gpio_reset(DeviceState *dev)
{
    Tegra30GpioState *s = TEGRA30_GPIO(dev);

    memset(s->cnf, 0, sizeof(s->cnf));
    memset(s->oe, 0, sizeof(s->oe));
    memset(s->out, 0, sizeof(s->out));
    /* External pin levels are board inputs and survive controller reset. */
    memset(s->int_sta, 0, sizeof(s->int_sta));
    memset(s->int_enb, 0, sizeof(s->int_enb));
    memset(s->int_lvl, 0, sizeof(s->int_lvl));
}

static void tegra30_gpio_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    Tegra30GpioState *s = TEGRA30_GPIO(obj);

    memory_region_init_io(&s->iomem, obj, &tegra30_gpio_ops, s,
                          TYPE_TEGRA30_GPIO, TEGRA30_GPIO_IOSIZE);
    sysbus_init_mmio(sbd, &s->iomem);

    qdev_init_gpio_in(DEVICE(obj), tegra30_gpio_set_in, TEGRA30_GPIO_NR_GPIOS);
    qdev_init_gpio_out(DEVICE(obj), s->output, TEGRA30_GPIO_NR_GPIOS);

    for (int b = 0; b < TEGRA30_GPIO_BANKS; b++) {
        sysbus_init_irq(sbd, &s->irq[b]);
    }
}

static const VMStateDescription tegra30_gpio_vmstate = {
    .name = "tegra30-gpio",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(cnf, Tegra30GpioState, TEGRA30_GPIO_PORTS),
        VMSTATE_UINT32_ARRAY(oe, Tegra30GpioState, TEGRA30_GPIO_PORTS),
        VMSTATE_UINT32_ARRAY(out, Tegra30GpioState, TEGRA30_GPIO_PORTS),
        VMSTATE_UINT32_ARRAY(in, Tegra30GpioState, TEGRA30_GPIO_PORTS),
        VMSTATE_UINT32_ARRAY(int_sta, Tegra30GpioState, TEGRA30_GPIO_PORTS),
        VMSTATE_UINT32_ARRAY(int_enb, Tegra30GpioState, TEGRA30_GPIO_PORTS),
        VMSTATE_UINT32_ARRAY(int_lvl, Tegra30GpioState, TEGRA30_GPIO_PORTS),
        VMSTATE_END_OF_LIST()
    }
};

static void tegra30_gpio_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, tegra30_gpio_reset);
    dc->vmsd = &tegra30_gpio_vmstate;
}

static const TypeInfo tegra30_gpio_info = {
    .name          = TYPE_TEGRA30_GPIO,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_init = tegra30_gpio_init,
    .instance_size = sizeof(Tegra30GpioState),
    .class_init    = tegra30_gpio_class_init,
};

static void tegra30_gpio_register(void)
{
    type_register_static(&tegra30_gpio_info);
}

type_init(tegra30_gpio_register)
