#ifndef HW_INTC_TEGRA30_ICTLR_H
#define HW_INTC_TEGRA30_ICTLR_H

#include "qom/object.h"
#include "hw/core/sysbus.h"

/* The legacy interrupt controller has up to 5 banks of 32 interrupts. */
#define TEGRA30_ICTLR_BANKS        5
#define TEGRA30_ICTLR_IRQS_PER_BANK 32
#define TEGRA30_ICTLR_NUM_IRQS     (TEGRA30_ICTLR_BANKS * \
                                    TEGRA30_ICTLR_IRQS_PER_BANK)
#define TEGRA30_ICTLR_BANK_SIZE    0x100
#define TEGRA30_ICTLR_IOSIZE       (TEGRA30_ICTLR_BANKS * \
                                    TEGRA30_ICTLR_BANK_SIZE)

/* Per-bank Tegra IRQ numbers used by this board. */
#define TEGRA30_IRQ_SDMMC1         14
#define TEGRA30_IRQ_SDMMC2         15
#define TEGRA30_IRQ_SDMMC3         19
#define TEGRA30_IRQ_SDMMC4         31

#define TYPE_TEGRA30_ICTLR    "tegra30-ictlr"
OBJECT_DECLARE_SIMPLE_TYPE(Tegra30IctlrState, TEGRA30_ICTLR)

struct Tegra30IctlrState {
    /*< private >*/
    SysBusDevice parent_obj;
    /*< public >*/

    MemoryRegion iomem;

    /* CPU IRQ/FIQ outputs (the COP path is modelled but left unconnected). */
    qemu_irq cpu_irq;
    qemu_irq cpu_fiq;

    uint32_t virq_cpu[TEGRA30_ICTLR_BANKS];
    uint32_t vfiq_cpu[TEGRA30_ICTLR_BANKS];
    uint32_t isr[TEGRA30_ICTLR_BANKS];
    uint32_t fir[TEGRA30_ICTLR_BANKS];
    uint32_t cpu_ier[TEGRA30_ICTLR_BANKS];
    uint32_t cpu_iep_class[TEGRA30_ICTLR_BANKS];
    uint32_t cop_ier[TEGRA30_ICTLR_BANKS];
    uint32_t cop_iep_class[TEGRA30_ICTLR_BANKS];
};

#endif /* HW_INTC_TEGRA30_ICTLR_H */
