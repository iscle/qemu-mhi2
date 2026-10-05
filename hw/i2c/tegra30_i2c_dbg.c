/*
 * Debug / register-file I2C slave for bringing up the Tegra3 MIB2 boot.
 *
 * Behaves like a simple 8-bit-addressed register device (PMIC/EEPROM style):
 * the first byte of a write sets the internal register pointer, subsequent
 * written bytes land in the backing store, and reads return the store
 * starting at the current pointer (auto-incrementing).  Every access is
 * logged so the firmware's expectations can be reverse engineered, and the
 * backing store can be pre-seeded per slave address.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/i2c/i2c.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qom/object.h"

#define TYPE_TEGRA30_I2C_DBG "tegra30-i2c-dbg"
OBJECT_DECLARE_SIMPLE_TYPE(Tegra30I2CDbgState, TEGRA30_I2C_DBG)

struct Tegra30I2CDbgState {
    I2CSlave parent_obj;

    uint8_t store[256];
    uint8_t ptr;
    bool addr_received;
    /* Byte returned for registers that were never written. */
    uint8_t fill;
    bool temperature_sensor;
};

static void tegra30_i2c_dbg_reset(DeviceState *dev)
{
    Tegra30I2CDbgState *s = TEGRA30_I2C_DBG(dev);

    memset(s->store, s->fill, sizeof(s->store));
    if (s->temperature_sensor) {
        s->store[3] = 0; /* standard range until configuration write */
        s->store[4] = 8;
        s->store[5] = s->store[7] = 127;
        s->store[0xfe] = 0x41;
        s->store[0xff] = 0x57;
    }
    s->ptr = 0;
    s->addr_received = false;
}

static int tegra30_i2c_dbg_event(I2CSlave *i2c, enum i2c_event event)
{
    Tegra30I2CDbgState *s = TEGRA30_I2C_DBG(i2c);

    switch (event) {
    case I2C_START_SEND:
        s->addr_received = false;
        break;
    case I2C_START_RECV:
    case I2C_FINISH:
    case I2C_NACK:
        break;
    default:
        break;
    }
    return 0;
}

static uint8_t tegra30_i2c_dbg_recv(I2CSlave *i2c)
{
    Tegra30I2CDbgState *s = TEGRA30_I2C_DBG(i2c);
    uint8_t val = s->store[s->ptr];
    /* NCT1008-compatible local/remote diode readings. K5126 adds 8 C to
     * the remote value, so 32 C represents a 40 C emulated SoC. */
    if (s->temperature_sensor && s->ptr <= 1) {
        val = (s->ptr ? 32 : 25) + ((s->store[3] & 4) ? 64 : 0);
    }

    qemu_log_mask(LOG_UNIMP, "%s[0x%02x]: read  reg 0x%02x => 0x%02x\n",
                  TYPE_TEGRA30_I2C_DBG, i2c->address, s->ptr, val);
    s->ptr++;
    return val;
}

static int tegra30_i2c_dbg_send(I2CSlave *i2c, uint8_t data)
{
    Tegra30I2CDbgState *s = TEGRA30_I2C_DBG(i2c);

    if (!s->addr_received) {
        s->ptr = data;
        s->addr_received = true;
    } else {
        qemu_log_mask(LOG_UNIMP, "%s[0x%02x]: write reg 0x%02x <= 0x%02x\n",
                      TYPE_TEGRA30_I2C_DBG, i2c->address, s->ptr, data);
        uint8_t reg = s->ptr++;
        if (s->temperature_sensor && reg >= 9 && reg <= 15) reg -= 6;
        if (!s->temperature_sensor || reg > 2) s->store[reg] = data;
    }
    return 0;
}

static const Property tegra30_i2c_dbg_props[] = {
    DEFINE_PROP_UINT8("fill", Tegra30I2CDbgState, fill, 0),
    DEFINE_PROP_BOOL("temperature-sensor", Tegra30I2CDbgState, temperature_sensor, false),
};

static const VMStateDescription tegra30_i2c_dbg_vmstate = {
    .name = "tegra30-i2c-dbg",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, Tegra30I2CDbgState),
        VMSTATE_UINT8_ARRAY(store, Tegra30I2CDbgState, 256),
        VMSTATE_UINT8(ptr, Tegra30I2CDbgState),
        VMSTATE_BOOL(addr_received, Tegra30I2CDbgState),
        VMSTATE_END_OF_LIST()
    }
};

static void tegra30_i2c_dbg_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->event = tegra30_i2c_dbg_event;
    k->recv = tegra30_i2c_dbg_recv;
    k->send = tegra30_i2c_dbg_send;
    device_class_set_legacy_reset(dc, tegra30_i2c_dbg_reset);
    device_class_set_props(dc, tegra30_i2c_dbg_props);
    dc->vmsd = &tegra30_i2c_dbg_vmstate;
}

static const TypeInfo tegra30_i2c_dbg_info = {
    .name          = TYPE_TEGRA30_I2C_DBG,
    .parent        = TYPE_I2C_SLAVE,
    .instance_size = sizeof(Tegra30I2CDbgState),
    .class_init    = tegra30_i2c_dbg_class_init,
};

static void tegra30_i2c_dbg_register(void)
{
    type_register_static(&tegra30_i2c_dbg_info);
}

type_init(tegra30_i2c_dbg_register)
