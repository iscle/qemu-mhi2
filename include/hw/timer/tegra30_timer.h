#ifndef HW_TIMER_TEGRA30_TIMER_H
#define HW_TIMER_TEGRA30_TIMER_H

#include "qom/object.h"
#include "hw/core/sysbus.h"
#include "hw/core/ptimer.h"

/**
 * @name Constants
 * @{
 */

/** Size of register I/O address space used by clock device */
#define TEGRA30_TIMER_IOSIZE        (0x400)

/** Total number of known registers */
#define TEGRA30_TIMER_REGS_NUM      (TEGRA30_TIMER_IOSIZE / sizeof(uint32_t))

/** Number of timers */
#define TEGRA30_TIMER_TIMER_NR    6

/** @} */

/**
 * @name Object model
 * @{
 */

#define TYPE_TEGRA30_TIMER    "tegra30-timer"
OBJECT_DECLARE_SIMPLE_TYPE(Tegra30TimerState, TEGRA30_TIMER)

/** @} */

/**
 * nVidia Tegra 3 clock object instance state.
 */
typedef struct Tegra30TimerChannel {
    ptimer_state *counter;
    qemu_irq irq;
    bool pending;
} Tegra30TimerChannel;

struct Tegra30TimerState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    /** Maps I/O registers in physical memory */
    MemoryRegion iomem;

    /** Array of hardware registers */
    uint32_t regs[TEGRA30_TIMER_REGS_NUM];
    Tegra30TimerChannel channel[6];

};

#endif /* HW_TIMER_TEGRA30_TIMER_H */
