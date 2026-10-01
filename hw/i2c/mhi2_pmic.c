/*
 * MHI2 PMIC register interfaces for Linux bring-up experiments.
 *
 * The supported subset is the power-slave register bank: voltage selectors,
 * enable/FPS configuration and interrupt status/masks. OTP defaults are a
 * bring-up fixture, not a dump of any production PMIC. RTC, power sequencing
 * delays, analogue faults and external converter feedback are not modeled.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/module.h"

#define TYPE_MHI2_PMIC "mhi2-pmic"
OBJECT_DECLARE_SIMPLE_TYPE(MHI2PMICState, MHI2_PMIC)

struct MHI2PMICState {
    I2CSlave parent_obj;
    qemu_irq irq;
    uint8_t regs[256];
    uint8_t pointer;
    uint16_t profile;
    bool addressed;
    bool maxim;
};

static void pmic_irq(MHI2PMICState *s)
{
    unsigned pending = 0;

    if (s->maxim) {
        /* INTLBT sources feed IRQTOP.GLBL. GLBLM masks the entire pin. */
        s->regs[5] = (s->regs[5] & 0x7f) |
                     ((s->regs[6] & ~s->regs[14] & 0x0e) ? 0x80 : 0);
        if (!(s->regs[14] & 1)) {
            pending = s->regs[5] & ~s->regs[13];
        }
    } else {
        for (unsigned i = 0; i < 3; i++) {
            pending |= s->regs[0x50 + i * 2] & ~s->regs[0x51 + i * 2];
        }
    }
    qemu_set_irq(s->irq, !!pending);
}

static int pmic_event(I2CSlave *i2c, enum i2c_event event)
{
    MHI2PMICState *s = MHI2_PMIC(i2c);

    if (event == I2C_START_SEND) {
        s->addressed = false;
    }
    return 0;
}

static uint8_t pmic_recv(I2CSlave *i2c)
{
    MHI2PMICState *s = MHI2_PMIC(i2c);
    uint8_t reg = s->pointer++;
    uint8_t value = s->regs[reg];

    /* Maxim interrupt latches clear on read; TI uses write-one-to-clear. */
    if (s->maxim && reg >= 5 && reg <= 12) {
        s->regs[reg] = 0;
        pmic_irq(s);
    }
    return value;
}

static int pmic_send(I2CSlave *i2c, uint8_t value)
{
    MHI2PMICState *s = MHI2_PMIC(i2c);
    uint8_t reg;

    if (!s->addressed) {
        s->pointer = value;
        s->addressed = true;
        return 0;
    }
    reg = s->pointer++;
    if (s->maxim) {
        if ((reg >= 5 && reg <= 12) || (reg >= 0x13 && reg <= 0x15) ||
            (reg >= 0x58 && reg <= 0x5d)) {
            return 0;
        }
    } else if (reg == 0x50 || reg == 0x52 || reg == 0x54) {
        s->regs[reg] &= ~value;
        pmic_irq(s);
        return 0;
    }
    s->regs[reg] = value;
    pmic_irq(s);
    return 0;
}

static void pmic_reset(DeviceState *dev)
{
    MHI2PMICState *s = MHI2_PMIC(dev);

    memset(s->regs, 0, sizeof(s->regs));
    s->pointer = 0;
    s->addressed = false;
    if (s->maxim) {
        s->regs[0x0d] = 0xff;
        s->regs[0x0e] = 0x0f;
        s->regs[0x5c] = s->profile <= 0xff ? s->profile : 0x44;
        s->regs[0x5d] = 0x10;
        /* Enabled supplies, 1.25 V SD0 and 1.35 V SD1 for test fixtures. */
        s->regs[0x16] = 36;
        s->regs[0x17] = 60;
        for (unsigned i = 0x1d; i <= 0x21; i++) {
            s->regs[i] = 0x30;
        }
        for (unsigned i = 0x23; i <= 0x33; i += 2) {
            s->regs[i] = 0xc0;
        }
        for (unsigned i = 0x46; i <= 0x53; i++) {
            s->regs[i] = 0xc0; /* Software-controlled, no FPS source. */
        }
    } else {
        s->regs[0x1e] = 1;
        s->regs[0x20] = 5;
        s->regs[0x21] = s->regs[0x24] = 0x0d;
        s->regs[0x22] = s->regs[0x23] = 55;
        s->regs[0x25] = s->regs[0x26] = 63;
        s->regs[0x27] = 1;
        s->regs[0x28] = s->regs[0x29] = 55;
        for (unsigned i = 0x30; i <= 0x37; i++) {
            s->regs[i] = 0x15;
        }
        s->regs[0x3f] = 4;
        s->regs[0x51] = s->regs[0x53] = s->regs[0x55] = 0xff;
        /* Quickboot uses this EEPROM-programmed value as a profile marker. */
        s->regs[0x6a] = s->profile <= 0xff ? s->profile : 0x20;
    }
    pmic_irq(s);
}

static int pmic_post_load(void *opaque, int version_id)
{
    pmic_irq(opaque);
    return 0;
}

static const VMStateDescription vmstate_pmic = {
    .name = TYPE_MHI2_PMIC,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = pmic_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, MHI2PMICState),
        VMSTATE_UINT8_ARRAY(regs, MHI2PMICState, 256),
        VMSTATE_UINT8(pointer, MHI2PMICState),
        VMSTATE_BOOL(addressed, MHI2PMICState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property pmic_properties[] = {
    DEFINE_PROP_BOOL("maxim", MHI2PMICState, maxim, false),
    DEFINE_PROP_UINT16("profile", MHI2PMICState, profile, 0x100),
};

static void pmic_init(Object *obj)
{
    qdev_init_gpio_out(DEVICE(obj), &MHI2_PMIC(obj)->irq, 1);
}

static void pmic_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *ic = I2C_SLAVE_CLASS(klass);

    ic->event = pmic_event;
    ic->recv = pmic_recv;
    ic->send = pmic_send;
    dc->vmsd = &vmstate_pmic;
    device_class_set_legacy_reset(dc, pmic_reset);
    device_class_set_props(dc, pmic_properties);
}

static const TypeInfo pmic_info = {
    .name = TYPE_MHI2_PMIC,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(MHI2PMICState),
    .instance_init = pmic_init,
    .class_init = pmic_class_init,
};

static void pmic_register_types(void)
{
    type_register_static(&pmic_info);
}
type_init(pmic_register_types)
