#ifndef HW_MISC_MMX_IOC_H
#define HW_MISC_MMX_IOC_H

#include "qom/object.h"
#include "hw/sysbus.h"
#include "hw/i2c/tegra30_i2c.h"

#define TYPE_MMX_IOC "mmx-ioc"
OBJECT_DECLARE_SIMPLE_TYPE(MmxIocState, MMX_IOC)

/*
 * e.solutions/Harman MIB2 "IOC" companion controller.
 *
 * On the real board the IOC is a Renesas V850 MCU.  The OS talks to it over
 * SPI (BAP) and, for the low-level attention channel modelled here, over I2C:
 * the IOC drives a GPIO ("mmx_ioc_irq") to signal that data is ready and then
 * transfers a frame to the SoC's I2C slave interface.
 */
struct MmxIocState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    /* Link to the I2C controller whose slave interface the IOC drives. */
    Tegra30I2CState *i2c;
    /* Attention line, wired to the SoC GPIO input the OS watches. */
    qemu_irq irq;

    QEMUTimer *timer;
    int32_t irq_level;
};

#endif /* HW_MISC_MMX_IOC_H */
