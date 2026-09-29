#ifndef HW_MISC_TEGRA30_FLOW_H
#define HW_MISC_TEGRA30_FLOW_H

#include "qom/object.h"
#include "hw/core/sysbus.h"

/**
 * @name Constants
 * @{
 */

/** Size of register I/O address space used by the flow controller device */
#define TEGRA30_FLOW_IOSIZE        (0xFFF)

/** @} */

/**
 * @name Object model
 * @{
 */

#define TYPE_TEGRA30_FLOW    "tegra30-flow"
OBJECT_DECLARE_SIMPLE_TYPE(Tegra30FlowState, TEGRA30_FLOW)

/** @} */

/**
 * nVidia Tegra 3 flow controller object instance state.
 */
struct Tegra30FlowState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    /** Maps I/O registers in physical memory */
    MemoryRegion iomem;

    /** Set once the COP->CPU boot hand-off has been performed */
    bool handed_off;
};

#endif /* HW_MISC_TEGRA30_FLOW_H */
