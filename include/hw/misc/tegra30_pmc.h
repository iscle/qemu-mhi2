#ifndef HW_MISC_TEGRA30_PMC_H
#define HW_MISC_TEGRA30_PMC_H

#include "qom/object.h"
#include "hw/core/sysbus.h"

/**
 * @name Constants
 * @{
 */

/** Size of register I/O address space used by power management controller device */
#define TEGRA30_PMC_IOSIZE        0x400

/** Total number of known registers */
#define TEGRA30_PMC_REGS_NUM      (TEGRA30_PMC_IOSIZE / sizeof(uint32_t))

/** @} */

/**
 * @name Object model
 * @{
 */

#define TYPE_TEGRA30_PMC    "tegra30-pmc"
OBJECT_DECLARE_SIMPLE_TYPE(Tegra30PmcState, TEGRA30_PMC)

/** @} */

/**
 * nVidia Tegra 3 power management controller object instance state.
 */
struct Tegra30PmcState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    /** Maps I/O registers in physical memory */
    MemoryRegion iomem;

    /** Array of hardware registers */
    uint32_t regs[TEGRA30_PMC_REGS_NUM];

};

#endif /* HW_MISC_TEGRA30_PMC_H */
