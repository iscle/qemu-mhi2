#ifndef HW_MISC_TEGRA30_APB_MISC_H
#define HW_MISC_TEGRA30_APB_MISC_H

#include "qom/object.h"
#include "hw/core/sysbus.h"

/**
 * @name Constants
 * @{
 */

/** Size of register I/O address space used by APB misc device */
#define TEGRA30_APB_MISC_IOSIZE        (0x3FFF)

/** Total number of known registers */
#define TEGRA30_APB_MISC_REGS_NUM      (TEGRA30_APB_MISC_IOSIZE / sizeof(uint32_t))

/** @} */

/**
 * @name Object model
 * @{
 */

#define TYPE_TEGRA30_APB_MISC    "tegra30-apb-misc"
OBJECT_DECLARE_SIMPLE_TYPE(Tegra30ApbMiscState, TEGRA30_APB_MISC)

/** @} */

/**
 * nVidia Tegra 3 APB misc object instance state.
 */
struct Tegra30ApbMiscState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    /** Maps I/O registers in physical memory */
    MemoryRegion iomem;

    /** Array of hardware registers */
    uint32_t regs[TEGRA30_APB_MISC_REGS_NUM];

};

#endif /* HW_MISC_TEGRA30_APB_MISC_H */
