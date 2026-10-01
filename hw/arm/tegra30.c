#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "hw/core/qdev.h"
#include "hw/core/sysbus.h"
#include "hw/char/serial-mm.h"
#include "hw/misc/unimp.h"
#include "hw/usb/hcd-ehci.h"
#include "hw/core/loader.h"
#include "hw/sd/sdhci.h"
#include "system/system.h"
#include "net/net.h"
#include "hw/arm/tegra30.h"

static void tegra30_init(Object *obj)
{
    Tegra30State *s = TEGRA30(obj);

    info_report("Tegra 3 initialization: s=%p", s);

    /* "G" cluster: four high-performance Cortex-A9 cores. */
    for (int i = 0; i < TEGRA30_NUM_CPUS; i++) {
        object_initialize_child(obj, "cpu[*]", &s->cpus[i],
                                ARM_CPU_TYPE_NAME("cortex-a9"));
    }

    /* Low-power "LP" companion Cortex-A9 core. */
    object_initialize_child(obj, "lp-cpu", &s->lp_cpu,
                            ARM_CPU_TYPE_NAME("cortex-a9"));

    /*
     * AVP (COP) boot processor.  It is an ARM7-class core on real hardware;
     * model it as a Cortex-A9 for now, which is enough to run the boot ROM
     * and first-stage bootloader.
     */
    object_initialize_child(obj, "avp", &s->avp,
                            ARM_CPU_TYPE_NAME("cortex-a9"));

    object_initialize_child(obj, "apb-misc", &s->apb_misc,
                            TYPE_TEGRA30_APB_MISC);

    object_initialize_child(obj, "clk", &s->clk, TYPE_TEGRA30_CLK);

    object_initialize_child(obj, "pmc", &s->pmc, TYPE_TEGRA30_PMC);

    object_initialize_child(obj, "timer", &s->timer, TYPE_TEGRA30_TIMER);

    object_initialize_child(obj, "flow", &s->flow, TYPE_TEGRA30_FLOW);

    object_initialize_child(obj, "arb-sema", &s->arb_sema,
                            TYPE_TEGRA30_ARB_SEMA);

    object_initialize_child(obj, "snor", &s->snor, TYPE_TEGRA30_SNOR);

    object_initialize_child(obj, "ictlr", &s->ictlr, TYPE_TEGRA30_ICTLR);

    object_initialize_child(obj, "gpio", &s->gpio, TYPE_TEGRA30_GPIO);

    object_initialize_child(obj, "mc", &s->mc, TYPE_TEGRA30_MC);
    object_initialize_child(obj, "host1x", &s->host1x, TYPE_TEGRA30_HOST1X);
    object_initialize_child(obj, "dc-a", &s->dc_a, TYPE_TEGRA30_DC);
    object_initialize_child(obj, "dc-b", &s->dc_b, TYPE_TEGRA30_DC);

    object_initialize_child(obj, "a9mpcore", &s->a9mpcore,
                            TYPE_A9MPCORE_PRIV);

    for (int i = 0; i < ARRAY_SIZE(s->i2c); i++) {
        object_initialize_child(obj, "i2c[*]", &s->i2c[i], TYPE_TEGRA30_I2C);
    }

    info_report("Tegra 3 CPUs initialized");
}

static void tegra30_realize(DeviceState *dev, Error **errp)
{
    Tegra30State *s = TEGRA30(dev);

    info_report("Realizing Tegra 3");

    /*
     * The AVP boot processor is the only core running out of reset; it runs
     * the boot ROM / first-stage bootloader and brings the Cortex-A9 complex
     * out of reset once it has programmed the CPU reset vector.  All A9 cores
     * (G cluster and the LP companion) therefore start powered off.
     */
    for (int i = 0; i < TEGRA30_NUM_CPUS; i++) {
        qdev_prop_set_bit(DEVICE(&s->cpus[i]), "has_el3", false);
        qdev_prop_set_bit(DEVICE(&s->cpus[i]), "start-powered-off", true);
        /*
         * The Cortex-A9 CBAR (Configuration Base Address Register, read via
         * "mrc p15, 4, rX, c15, c0, 0") must return the PERIPHBASE of the A9
         * MPCore private peripheral region.  The QNX startup reads it to locate
         * the SCU/GIC/timers; if it reads back zero it never discovers them.
         */
        qdev_prop_set_uint64(DEVICE(&s->cpus[i]), "reset-cbar", 0x50040000);
        qdev_realize(DEVICE(&s->cpus[i]), NULL, &error_fatal);
    }

    qdev_prop_set_bit(DEVICE(&s->lp_cpu), "has_el3", false);
    qdev_prop_set_bit(DEVICE(&s->lp_cpu), "start-powered-off", true);
    qdev_prop_set_uint64(DEVICE(&s->lp_cpu), "reset-cbar", 0x50040000);
    qdev_realize(DEVICE(&s->lp_cpu), NULL, &error_fatal);

    qdev_prop_set_bit(DEVICE(&s->avp), "has_el3", false);
    qdev_realize(DEVICE(&s->avp), NULL, &error_fatal);

    /* APB misc */
    sysbus_realize(SYS_BUS_DEVICE(&s->apb_misc), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->apb_misc), 0, 0x70000000);

    /* Clock */
    sysbus_realize(SYS_BUS_DEVICE(&s->clk), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->clk), 0, 0x60006000);

    /* PMC */
    sysbus_realize(SYS_BUS_DEVICE(&s->pmc), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->pmc), 0, 0x7000E400);

    /* Timer */
    sysbus_realize(SYS_BUS_DEVICE(&s->timer), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->timer), 0, 0x60005000);

    /* Flow controller */
    sysbus_realize(SYS_BUS_DEVICE(&s->flow), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->flow), 0, 0x60007000);

    /* Arbitration semaphore */
    sysbus_realize(SYS_BUS_DEVICE(&s->arb_sema), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->arb_sema), 0, 0x60002000);

    /* SNOR (sync NOR) controller; QuickBoot DMAs the NOR off this. */
    sysbus_realize(SYS_BUS_DEVICE(&s->snor), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->snor), 0, 0x70009000);

    /*
     * Legacy interrupt controller (LIC).  Per the Tegra30 device tree the LIC
     * (interrupt-controller@60004000) has interrupt-parent = the A9 GIC: it is
     * a secondary masking/wakeup wrapper that chains INTO the GIC, it does not
     * drive the CPUs directly.  The bootloader only polls its status registers.
     * So map it but leave its CPU outputs unconnected -- the GIC is the sole
     * controller that drives the A9 cores (wiring the LIC to cpus[0] as well
     * would clobber the GIC's interrupt line to that core).
     */
    sysbus_realize(SYS_BUS_DEVICE(&s->ictlr), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->ictlr), 0, 0x60004000);

    /*
     * Cortex-A9 MPCore private peripherals (SCU, GIC distributor + CPU
     * interface, global and per-CPU timers) at PERIPHBASE 0x50040000.  This is
     * the interrupt controller and timer block the OS kernel (QNX) drives.  Its
     * GIC IRQ/FIQ outputs are wired to the four G-cluster A9 cores (taking over
     * CPU0 from the legacy ICTLR, which the bootloader only polls).
     */
    object_property_set_int(OBJECT(&s->a9mpcore), "num-cpu", TEGRA30_NUM_CPUS,
                            &error_fatal);
    object_property_set_int(OBJECT(&s->a9mpcore), "num-irq", 192, &error_fatal);
    /* Quickboot configures PLLX/CPU to 1.2 GHz; PERIPHCLK is CPU/2. */
    qdev_prop_set_uint32(DEVICE(&s->a9mpcore.gtimer), "frequency", 600000000);
    sysbus_realize(SYS_BUS_DEVICE(&s->a9mpcore), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->a9mpcore), 0, 0x50040000);
    static const unsigned timer_irq[6] = { 0, 1, 41, 42, 121, 122 };
    for (int i = 0; i < 6; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->timer), i,
                           qdev_get_gpio_in(DEVICE(&s->a9mpcore), timer_irq[i]));
    }
    for (int i = 0; i < TEGRA30_NUM_CPUS; i++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->a9mpcore), i,
                           qdev_get_gpio_in(DEVICE(&s->cpus[i]), ARM_CPU_IRQ));
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->a9mpcore), TEGRA30_NUM_CPUS + i,
                           qdev_get_gpio_in(DEVICE(&s->cpus[i]), ARM_CPU_FIQ));
    }

    /*
     * PL310 L2 cache controller (cache-controller@50043000).  QNX startup runs
     * L2 maintenance (clean/invalidate-by-PA and cache-sync) very early; without
     * a model these fall through to the unimplemented background region, which
     * both faults and -- via its per-access logging -- throttles the kernel's
     * cache-flush loop to a crawl.  The standard l2x0 model accepts the
     * maintenance ops as completed no-ops.
     */
    sysbus_create_simple("l2x0", 0x50043000, NULL);
    DeviceState *pcie = qdev_new("tegra30-pcie");
    qemu_configure_nic_device(pcie, false, NULL);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(pcie), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(pcie), 0, 0);
    sysbus_connect_irq(SYS_BUS_DEVICE(pcie), 0,
                       qdev_get_gpio_in(DEVICE(&s->a9mpcore), 98));
    sysbus_connect_irq(SYS_BUS_DEVICE(pcie), 1,
                       qdev_get_gpio_in(DEVICE(&s->a9mpcore), 99));

    sysbus_create_simple("tegra30-rtc", 0x7000e000,
                         qdev_get_gpio_in(DEVICE(&s->a9mpcore), 2));

    /* I2C controllers (I2C1-4 and the DVC controller, I2C5) + GIC SPIs. */
    static const hwaddr i2c_base[5] = {
        0x7000C000, 0x7000C400, 0x7000C500, 0x7000C700, 0x7000D000,
    };
    static const int i2c_irq[5] = { 38, 84, 92, 120, 53 };
    for (int i = 0; i < ARRAY_SIZE(s->i2c); i++) {
        sysbus_realize(SYS_BUS_DEVICE(&s->i2c[i]), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->i2c[i]), 0, i2c_base[i]);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->i2c[i]), 0,
                           qdev_get_gpio_in(DEVICE(&s->a9mpcore), i2c_irq[i]));
    }

    /*
     * SNOR (sync NOR) DMA-completion interrupt -> GIC SPI 96 (INT_SNOR =
     * INT_QUAD_BASE + 0).  The QNX/nVidia SNOR driver sets DMA_CFG.IE_DMA_DONE
     * and blocks on a semaphore signalled by this interrupt; without it the
     * IFSLOADER's main_stage2 block DMAs time out and retry even though the
     * data is transferred correctly.
     */
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->snor), 0,
                       qdev_get_gpio_in(DEVICE(&s->a9mpcore), 96));

    /*
     * GPIO controller.  Each of the 8 banks raises one GIC SPI; the OS uses
     * these via the A9 GIC.  (The bootloader does not use GPIO interrupts, so
     * the legacy-ICTLR view of the same lines is left unconnected.)
     */
    static const int gpio_bank_irq[TEGRA30_GPIO_BANKS] = {
        32, 33, 34, 35, 55, 87, 89, 125,
    };
    sysbus_realize(SYS_BUS_DEVICE(&s->gpio), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->gpio), 0, 0x6000d000);
    for (int b = 0; b < TEGRA30_GPIO_BANKS; b++) {
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->gpio), b,
                           qdev_get_gpio_in(DEVICE(&s->a9mpcore),
                                            gpio_bank_irq[b]));
    }

    /*
     * Display controllers DC-A (DISPLAY) and DC-B (DISPLAYB).  Each presents a
     * 0x40000-byte register window and drives its own QemuConsole; the OS
     * programs an overlay window's framebuffer base/format/size and the device
     * blits it from guest RAM each refresh.
     */
    sysbus_realize(SYS_BUS_DEVICE(&s->mc), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->mc), 0, 0x7000f000);
    s->host1x.mc = &s->mc;
    sysbus_realize(SYS_BUS_DEVICE(&s->host1x), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->host1x), 0, 0x50000000);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->host1x), 0,
                       qdev_get_gpio_in(DEVICE(&s->a9mpcore), 65));
    s->dc_a.host1x = &s->host1x;
    s->dc_b.host1x = &s->host1x;
    sysbus_realize(SYS_BUS_DEVICE(&s->dc_a), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->dc_a), 0, 0x54200000);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->dc_a), 0,
                       qdev_get_gpio_in(DEVICE(&s->a9mpcore), 73));
    sysbus_realize(SYS_BUS_DEVICE(&s->dc_b), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(&s->dc_b), 0, 0x54240000);
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->dc_b), 0,
                       qdev_get_gpio_in(DEVICE(&s->a9mpcore), 74));

    /* Tegra30 USB controllers: native EHCI transfers plus integrated PHY. */
    static const unsigned usb_irqs[] = { 20, 21, 97 };
    for (unsigned i = 0; i < ARRAY_SIZE(usb_irqs); i++) {
        DeviceState *usb = qdev_new(TYPE_TEGRA30_EHCI);
        object_property_add_child(OBJECT(s), "usb[*]", OBJECT(usb));
        sysbus_realize_and_unref(SYS_BUS_DEVICE(usb), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(usb), 0, 0x7d000000 + i * 0x4000);
        sysbus_connect_irq(SYS_BUS_DEVICE(usb), 0,
                           qdev_get_gpio_in(DEVICE(&s->a9mpcore), usb_irqs[i]));
    }

    /* SD/MMC (SDHCI) controllers; SDMMC4 carries the eMMC (QNX "mnand"). */
    static const hwaddr sdmmc_base[4] = {
        0x78000000, 0x78000200, 0x78000400, 0x78000600,
    };
    static const int sdmmc_irq[4] = {
        TEGRA30_IRQ_SDMMC1, TEGRA30_IRQ_SDMMC2,
        TEGRA30_IRQ_SDMMC3, TEGRA30_IRQ_SDMMC4,
    };
    for (int i = 0; i < ARRAY_SIZE(s->sdmmc); i++) {
        s->sdmmc[i] = qdev_new(TYPE_SYSBUS_SDHCI);
        object_property_add_child(OBJECT(s), "sdmmc[*]", OBJECT(s->sdmmc[i]));
        qdev_prop_set_uint32(s->sdmmc[i], "capareg", 0x05780A8A);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(s->sdmmc[i]), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(s->sdmmc[i]), 0, sdmmc_base[i]);
        /*
         * The OS (QNX devb-mmcsd) takes the SDMMC completion interrupt via the
         * A9 GIC (e.g. mnand/SDMMC4 = irq 63 = GIC SPI 31), not the legacy
         * ICTLR, so wire the controller IRQ straight to the GIC like the I2C
         * controllers.  Without this the driver issues the init command and
         * blocks forever waiting on a completion interrupt that never arrives.
         */
        sysbus_connect_irq(SYS_BUS_DEVICE(s->sdmmc[i]), 0,
                           qdev_get_gpio_in(DEVICE(&s->a9mpcore), sdmmc_irq[i]));
    }

    /* Native audio DMA and the attached MHI2 TDM endpoint. */
    {
        DeviceState *dma = qdev_new("tegra30-apbdma");
        object_property_add_child(OBJECT(s), "apbdma", OBJECT(dma));
        object_property_set_link(OBJECT(dma), "mc", OBJECT(&s->mc), &error_fatal);
        sysbus_realize_and_unref(SYS_BUS_DEVICE(dma), &error_fatal);
        sysbus_mmio_map(SYS_BUS_DEVICE(dma), 0, 0x6000a000);
        sysbus_mmio_map(SYS_BUS_DEVICE(dma), 1, 0x70080000);
        for (unsigned i = 0; i < 32; i++) {
            unsigned irq = i < 16 ? 104 + i : 128 + i - 16;
            sysbus_connect_irq(SYS_BUS_DEVICE(dma), i,
                              qdev_get_gpio_in(DEVICE(&s->a9mpcore), irq));
        }
    }

    /*
     * Exception Vector Pointers block.  The AVP firmware programs the main
     * CPU entry point into EVP_CPU_RESET_VECTOR (0x6000F100); back it with
     * RAM so the value is retained and can be picked up by the flow
     * controller when the COP is halted.
     */
    memory_region_init_ram(&s->evp, OBJECT(s), "evp", 0x1000, &error_fatal);
    memory_region_add_subregion(get_system_memory(), 0x6000F000, &s->evp);

    serial_mm_init(get_system_memory(), 0x70006000, 2,
                   qdev_get_gpio_in(DEVICE(&s->a9mpcore), 36),
                   115200, serial_hd(0), DEVICE_NATIVE_ENDIAN);
    serial_mm_init(get_system_memory(), 0x70006040, 2,
                   qdev_get_gpio_in(DEVICE(&s->a9mpcore), 37),
                   115200, serial_hd(1), DEVICE_NATIVE_ENDIAN);
    serial_mm_init(get_system_memory(), 0x70006200, 2,
                   qdev_get_gpio_in(DEVICE(&s->a9mpcore), 46),
                   115200, serial_hd(2), DEVICE_NATIVE_ENDIAN);
    serial_mm_init(get_system_memory(), 0x70006300, 2,
                   qdev_get_gpio_in(DEVICE(&s->a9mpcore), 90),
                   115200, serial_hd(3), DEVICE_NATIVE_ENDIAN);

    create_unimplemented_device("unimplemented-memory", 0, 0xFFFFFFFF);
    create_unimplemented_device("System Registers", 0x6000C000, 0x2FF);
    create_unimplemented_device("SE", 0x70012000, 0x3FF);

    /*
     * Minimal register-file models for the peripherals the OS (QNX) startup
     * pokes during early bring-up: GPIO, the 0x60001000 misc block, the memory
     * controller / fuse aperture, and the 0x70050000 AHB/arbitration block.
     * RAM-backed so configuration writes read back; enough to get past startup
     * before the real device semantics are needed.
     */
    static const struct { const char *name; hwaddr base; uint64_t size; } stubs[] = {
        { "tegra30-misc1",  0x60001000, 0x1000 },
        { "tegra30-ahbgz",  0x70050000, 0x8000 },
    };
    for (int i = 0; i < ARRAY_SIZE(stubs); i++) {
        MemoryRegion *mr = g_new(MemoryRegion, 1);
        memory_region_init_ram(mr, OBJECT(s), stubs[i].name, stubs[i].size,
                               &error_fatal);
        memory_region_add_subregion(get_system_memory(), stubs[i].base, mr);
    }

    info_report("Tegra30 realized");
}

static void tegra30_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = tegra30_realize;
}

static const TypeInfo tegra30_type_info = {
    .name = TYPE_TEGRA30,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Tegra30State),
    .instance_init = tegra30_init,
    .class_init = tegra30_class_init,
};

static void tegra30_register_types(void)
{
    type_register_static(&tegra30_type_info);
}

type_init(tegra30_register_types)
