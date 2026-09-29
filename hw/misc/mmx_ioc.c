/*
 * e.solutions/Harman MIB2 "IOC" companion controller (minimal model).
 *
 * The IOC (a Renesas V850 MCU on the real board) signals the SoC that it has
 * data by asserting a GPIO ("mmx_ioc_irq") and then delivers a frame over the
 * SoC's I2C slave interface.  This model drives that attention line and the
 * slave-receive path periodically so the OS I2C-IOC resource manager comes up
 * instead of timing out waiting for the companion.
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/qdev-properties.h"
#include "hw/irq.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "hw/i2c/tegra30_i2c.h"
#include "hw/misc/mmx_ioc.h"

#define MMX_IOC_INITIAL_DELAY_MS  250
#define MMX_IOC_POLL_MS           500

/*
 * IocI2c frame CRC-8 (reverse-engineered from libooc unpack_message,
 * ioclib vaddr 0x11894c).  MSB-first CRC-8, polynomial 0xa6, init 0.  The CRC
 * covers frame bytes [0..6] then [0xb..0xe] and the result must equal frame
 * byte [0xf]; bytes [7..0xa] and [0x10..0x17] are not covered.  A mismatch is
 * the "[OOC.IocI2c] CRC failed for ..." rejection.
 */
#define MMX_IOC_CRC_POLY  0xa6

static uint8_t mmx_ioc_crc8_table[256];

static void mmx_ioc_crc8_init(void)
{
    for (int i = 0; i < 256; i++) {
        uint8_t c = i;
        for (int b = 0; b < 8; b++) {
            c = (c & 0x80) ? (uint8_t)((c << 1) ^ MMX_IOC_CRC_POLY)
                           : (uint8_t)(c << 1);
        }
        mmx_ioc_crc8_table[i] = c;
    }
}

static uint8_t mmx_ioc_frame_crc(const uint8_t *f)
{
    static const int idx[] = { 0, 1, 2, 3, 4, 5, 6, 0xb, 0xc, 0xd, 0xe };
    uint8_t c = 0;
    for (size_t i = 0; i < ARRAY_SIZE(idx); i++) {
        c = mmx_ioc_crc8_table[c ^ f[idx[i]]];
    }
    return c;
}

static void mmx_ioc_send_frame(MmxIocState *s)
{
    /*
     * unpack_message reads 24 (0x18) bytes.  We deliver a benign idle frame
     * (all-zero content) with a valid CRC-8 at byte [0xf] so it passes the
     * IocI2c CRC gate and is decoded as a no-data IOC status frame.
     */
    uint8_t frame[24] = { 0 };

    frame[0xf] = mmx_ioc_frame_crc(frame);

    if (s->i2c) {
        tegra30_i2c_slave_deliver(s->i2c, frame, sizeof(frame));
    }
}

static void mmx_ioc_tick(void *opaque)
{
    MmxIocState *s = opaque;

    /*
     * The IOC periodically masters the bus to push a frame into the Tegra I2C
     * slave FIFO (the OS's "I2C slave thread" waits for this activity), and
     * holds the data-ready line (DD0) asserted.
     */
    mmx_ioc_send_frame(s);
    s->irq_level = 1;
    qemu_set_irq(s->irq, 1);
    timer_mod(s->timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + MMX_IOC_POLL_MS);
}

/*
 * The MMX drives its request line (GPIO DD3) to ask the IOC for data.  The OS
 * enables the I2C slave interrupt only around that request, so the frame must
 * be delivered in response to the request edge -- delivering proactively would
 * raise the interrupt while it is masked and be lost.
 */
static void mmx_ioc_request(void *opaque, int n, int level)
{
    MmxIocState *s = opaque;

    if (level) {
        mmx_ioc_send_frame(s);
    }
}

static void mmx_ioc_realize(DeviceState *dev, Error **errp)
{
    MmxIocState *s = MMX_IOC(dev);

    mmx_ioc_crc8_init();
    s->timer = timer_new_ms(QEMU_CLOCK_VIRTUAL, mmx_ioc_tick, s);
    timer_mod(s->timer,
              qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) + MMX_IOC_INITIAL_DELAY_MS);
}

static void mmx_ioc_init(Object *obj)
{
    MmxIocState *s = MMX_IOC(obj);

    /* Output 0 = data-ready line (DD0); input 0 = the MMX request line (DD3). */
    qdev_init_gpio_out(DEVICE(obj), &s->irq, 1);
    qdev_init_gpio_in(DEVICE(obj), mmx_ioc_request, 1);
}

static const Property mmx_ioc_properties[] = {
    DEFINE_PROP_LINK("i2c", MmxIocState, i2c, TYPE_TEGRA30_I2C,
                     Tegra30I2CState *),
    DEFINE_PROP_END_OF_LIST(),
};

static const VMStateDescription mmx_ioc_vmstate = {
    .name = "mmx-ioc",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_INT32(irq_level, MmxIocState),
        VMSTATE_TIMER_PTR(timer, MmxIocState),
        VMSTATE_END_OF_LIST()
    }
};

static void mmx_ioc_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mmx_ioc_realize;
    dc->vmsd = &mmx_ioc_vmstate;
    device_class_set_props(dc, mmx_ioc_properties);
}

static const TypeInfo mmx_ioc_info = {
    .name          = TYPE_MMX_IOC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_init = mmx_ioc_init,
    .instance_size = sizeof(MmxIocState),
    .class_init    = mmx_ioc_class_init,
};

static void mmx_ioc_register(void)
{
    type_register_static(&mmx_ioc_info);
}

type_init(mmx_ioc_register)
