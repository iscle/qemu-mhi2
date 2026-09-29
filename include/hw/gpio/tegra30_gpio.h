#ifndef HW_GPIO_TEGRA30_GPIO_H
#define HW_GPIO_TEGRA30_GPIO_H

#include "qom/object.h"
#include "hw/core/sysbus.h"

/*
 * nVidia Tegra GPIO controller.
 *
 * The controller is organised as 8 banks, each driving 4 ports of 8 pins
 * (256 GPIOs total).  Every bank has its own interrupt line into the legacy
 * interrupt controller.
 */
#define TEGRA30_GPIO_BANKS          8
#define TEGRA30_GPIO_PORTS_PER_BANK 4
#define TEGRA30_GPIO_PORTS          (TEGRA30_GPIO_BANKS * \
                                     TEGRA30_GPIO_PORTS_PER_BANK)
#define TEGRA30_GPIO_PINS_PER_PORT  8
#define TEGRA30_GPIO_NR_GPIOS       (TEGRA30_GPIO_PORTS * \
                                     TEGRA30_GPIO_PINS_PER_PORT)
#define TEGRA30_GPIO_BANK_STRIDE    0x100
#define TEGRA30_GPIO_IOSIZE         (TEGRA30_GPIO_BANKS * \
                                     TEGRA30_GPIO_BANK_STRIDE)

/* Compose a flat GPIO number from a Tegra port index (0..31) and pin (0..7). */
#define TEGRA30_GPIO_NR(port, pin)  ((port) * TEGRA30_GPIO_PINS_PER_PORT + (pin))

#define TYPE_TEGRA30_GPIO "tegra30-gpio"
OBJECT_DECLARE_SIMPLE_TYPE(Tegra30GpioState, TEGRA30_GPIO)

struct Tegra30GpioState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    MemoryRegion iomem;
    qemu_irq irq[TEGRA30_GPIO_BANKS];        /* per-bank IRQ outputs */
    qemu_irq output[TEGRA30_GPIO_NR_GPIOS];  /* driven output-pin lines */

    /* Per-port register state; one bit per pin in bits [7:0] (INT_LVL uses
     * the upper bytes for the EDGE/DELTA trigger configuration). */
    uint32_t cnf[TEGRA30_GPIO_PORTS];
    uint32_t oe[TEGRA30_GPIO_PORTS];
    uint32_t out[TEGRA30_GPIO_PORTS];
    uint32_t in[TEGRA30_GPIO_PORTS];
    uint32_t int_sta[TEGRA30_GPIO_PORTS];
    uint32_t int_enb[TEGRA30_GPIO_PORTS];
    uint32_t int_lvl[TEGRA30_GPIO_PORTS];
};

#endif /* HW_GPIO_TEGRA30_GPIO_H */
