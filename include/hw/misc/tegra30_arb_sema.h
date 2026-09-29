#ifndef HW_MISC_TEGRA30_ARB_SEMA_H
#define HW_MISC_TEGRA30_ARB_SEMA_H

#include "qom/object.h"
#include "hw/core/sysbus.h"

/**
 * @name Constants
 * @{
 */

/** Size of register I/O address space used by the arbitration semaphore */
#define TEGRA30_ARB_SEMA_IOSIZE        (0xFFF)

/** @} */

/**
 * @name Object model
 * @{
 */

#define TYPE_TEGRA30_ARB_SEMA    "tegra30-arb-sema"
OBJECT_DECLARE_SIMPLE_TYPE(Tegra30ArbSemaState, TEGRA30_ARB_SEMA)

/** @} */

/**
 * nVidia Tegra 3 arbitration semaphore object instance state.
 */
struct Tegra30ArbSemaState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    /** Maps I/O registers in physical memory */
    MemoryRegion iomem;

    /** Bits currently requested (SMP_GET) */
    uint32_t get;
    /** Bits currently granted (SMP_GNT_ST) */
    uint32_t gnt_st;
};

#endif /* HW_MISC_TEGRA30_ARB_SEMA_H */
