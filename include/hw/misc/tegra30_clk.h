#ifndef HW_MISC_TEGRA30_CLK_H
#define HW_MISC_TEGRA30_CLK_H

#include "qom/object.h"
#include "hw/core/sysbus.h"

/**
 * @name Constants
 * @{
 */

/** Size of register I/O address space used by clock device */
#define TEGRA30_CLK_IOSIZE        (0xFFF)

/** Total number of known registers */
#define TEGRA30_CLK_REGS_NUM      (TEGRA30_CLK_IOSIZE / sizeof(uint32_t))

/** @} */

/**
 * @name Object model
 * @{
 */

#define TYPE_TEGRA30_CLK    "tegra30-clk"
OBJECT_DECLARE_SIMPLE_TYPE(Tegra30ClkState, TEGRA30_CLK)

/** @} */

/**
 * nVidia Tegra 3 clock object instance state.
 */
struct Tegra30ClkState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    /** Maps I/O registers in physical memory */
    MemoryRegion iomem;

    /** Array of hardware registers */
    uint32_t regs[TEGRA30_CLK_REGS_NUM];
    bool oscillator_12mhz;
    qemu_irq i2c_reset[5];

};

#endif /* HW_MISC_TEGRA30_CLK_H */
