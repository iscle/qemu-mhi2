#ifndef HW_ARM_TEGRA30_H
#define HW_ARM_TEGRA30_H

#include "qom/object.h"
#include "hw/intc/arm_gic.h"
#include "hw/cpu/a9mpcore.h"
#include "target/arm/cpu.h"
#include "system/block-backend.h"
#include "hw/misc/tegra30_apb_misc.h"
#include "hw/misc/tegra30_clk.h"
#include "hw/misc/tegra30_pmc.h"
#include "hw/misc/tegra30_flow.h"
#include "hw/misc/tegra30_arb_sema.h"
#include "hw/misc/tegra30_snor.h"
#include "hw/timer/tegra30_timer.h"
#include "hw/i2c/tegra30_i2c.h"
#include "hw/intc/tegra30_ictlr.h"
#include "hw/gpio/tegra30_gpio.h"
#include "hw/display/tegra30_dc.h"

/*
 * Tegra 3 CPU complex.
 *
 * The SoC contains a "G" cluster of four high-performance Cortex-A9 cores, a
 * single low-power "LP" companion Cortex-A9 core, and the AVP (Audio/Video
 * Processor), an ARM7-class core that runs the boot ROM and first-stage
 * bootloader.  The cores are laid out so that the four G-cluster cores keep
 * CPU indices 0..3 (index 0 is the main CPU started by the AVP), followed by
 * the LP core and finally the AVP.
 */

/** Number of Cortex-A9 cores in the main "G" CPU cluster */
#define TEGRA30_NUM_CPUS      (4)

/** Index of the low-power "LP" companion Cortex-A9 core */
#define TEGRA30_LP_CPU        (TEGRA30_NUM_CPUS)

/** Index of the AVP (COP) boot processor */
#define TEGRA30_AVP_CPU       (TEGRA30_NUM_CPUS + 1)

/** Total number of processors instantiated by the SoC */
#define TEGRA30_NUM_PROCS     (TEGRA30_NUM_CPUS + 2)

/**
 * nVidia Tegra 3 object model
 * @{
 */

/** Object type for the nVidia Tegra 3 SoC */
#define TYPE_TEGRA30 "tegra30"

/** Convert input object to nVidia Tegra 3 state object */
OBJECT_DECLARE_SIMPLE_TYPE(Tegra30State, TEGRA30)

/** @} */

/**
 * nVidia Tegra 3 object
 *
 * This struct contains the state of all the devices
 * which are currently emulated by the Tegra 3 SoC code.
 */
struct Tegra30State {
    SysBusDevice parent_obj;

    ARMCPU cpus[TEGRA30_NUM_CPUS];
    ARMCPU lp_cpu;
    ARMCPU avp;
    Tegra30ApbMiscState apb_misc;
    Tegra30ClkState clk;
    Tegra30PmcState pmc;
    Tegra30TimerState timer;
    Tegra30FlowState flow;
    Tegra30ArbSemaState arb_sema;
    Tegra30SnorState snor;
    Tegra30IctlrState ictlr;
    Tegra30GpioState gpio;
    Tegra30DCState dc_a;
    Tegra30DCState dc_b;
    A9MPPrivState a9mpcore;
    Tegra30I2CState i2c[5];
    DeviceState *sdmmc[4];

    MemoryRegion emem;
    MemoryRegion nor_flash;
    MemoryRegion evp;
    MemoryRegion iram;
};

#endif /* HW_ARM_TEGRA30_H */
