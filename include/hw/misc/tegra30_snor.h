#ifndef HW_MISC_TEGRA30_SNOR_H
#define HW_MISC_TEGRA30_SNOR_H

#include "qom/object.h"
#include "hw/core/sysbus.h"

/**
 * @name Constants
 * @{
 */

/** Size of register I/O address space used by the SNOR controller */
#define TEGRA30_SNOR_IOSIZE        (0x1000)

/** Number of 32-bit registers modelled */
#define TEGRA30_SNOR_NUM_REGS      (0x40)

/** @} */

/**
 * @name Object model
 * @{
 */

#define TYPE_TEGRA30_SNOR    "tegra30-snor"
OBJECT_DECLARE_SIMPLE_TYPE(Tegra30SnorState, TEGRA30_SNOR)

/** @} */

/**
 * nVidia Tegra 3 SNOR (sync NOR) controller object instance state.
 *
 * The controller is mostly a DMA engine that copies between the NOR flash
 * window (NOR_ADDR_PTR) and system memory (AHB_ADDR_PTR).  Boot software uses
 * it to pull the partition table and boot stages out of the SPI/sync NOR.
 */
struct Tegra30SnorState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    /** Maps I/O registers in physical memory */
    MemoryRegion iomem;

    /** Register file (indexed by offset / 4) */
    uint32_t regs[TEGRA30_SNOR_NUM_REGS];

    /** DMA-completion interrupt line */
    qemu_irq irq;
};

#endif /* HW_MISC_TEGRA30_SNOR_H */
