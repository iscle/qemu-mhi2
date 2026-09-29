#include "qemu/osdep.h"
#include "qemu/units.h"
#include "system/address-spaces.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/core/boards.h"
#include "hw/core/qdev-properties.h"
#include "hw/arm/tegra30.h"
#include "hw/arm/boot.h"
#include "hw/misc/mmx_ioc.h"
#include "hw/i2c/i2c.h"
#include "hw/sd/sd.h"
#include "hw/block/flash.h"
#include "system/blockdev.h"
#include "system/block-backend.h"

static struct arm_boot_info mhi2_harman_binfo;

static void mhi2_harman_init(MachineState *machine)
{
    Tegra30State *soc;

    info_report("Initializing MHI2 Harman");

    soc = TEGRA30(object_new(TYPE_TEGRA30));
    object_property_add_child(OBJECT(machine), "soc", OBJECT(soc));
    
    // TODO: SoC setup

    // TODO: Move emem init to tegra30.c
    memory_region_init_ram(&soc->emem, OBJECT(soc), "emem", machine->ram_size, &error_fatal);
    memory_region_add_subregion(get_system_memory(), 0x80000000, &soc->emem);

    /*
     * The Tegra 3 DRAM is also visible at low addresses.  The OS boot image
     * (an Android boot image) is built for low load addresses - the recovery
     * kernel decompresses to kernel_addr 0xa00800 and the second-stage loader
     * disables the MMU and jumps there expecting it to be the same DRAM that
     * lives at 0x80a00800.  Mirror DRAM at 0x00000000 up to the IRAM so those
     * low addresses resolve to the same memory.
     */
    MemoryRegion *dram_low = g_new(MemoryRegion, 1);
    uint64_t low_size = MIN(machine->ram_size, 0x40000000);
    memory_region_init_alias(dram_low, OBJECT(soc), "dram-low-alias",
                             &soc->emem, 0, low_size);
    memory_region_add_subregion(get_system_memory(), 0, dram_low);

    sysbus_realize_and_unref(SYS_BUS_DEVICE(soc), &error_fatal);

    /*
     * IOC companion controller (the V850 MCU).  It signals data-ready on GPIO
     * PDD0 (the OS's "mmx_ioc_irq", port DD pin 0 -- the only bank-7 pin the OS
     * configures for edge detection; it polls the GPIO INT_STA rather than
     * taking a CPU interrupt) and delivers frames to the I2C2 slave interface
     * (soc->i2c[1], the bus the OOC's IocI2c resmgr uses).
     */
    DeviceState *ioc = qdev_new(TYPE_MMX_IOC);
    /*
     * The IOC link uses two Tegra I2C controllers: QNX masters configuration
     * to the IOC (a slave at 0x4c) on i2c[4] (DVC), and the IOC masters data
     * frames back into the Tegra slave on i2c[1] (I2C2) -- which QNX's "I2C
     * slave thread" polls.  This link is the data (IOC->Tegra) channel.
     */
    object_property_set_link(OBJECT(ioc), "i2c", OBJECT(&soc->i2c[1]),
                             &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(ioc), &error_fatal);
    /* IOC data-ready output -> GPIO PDD0 input (mmx_ioc_irq). */
    qdev_connect_gpio_out(ioc, 0,
                          qdev_get_gpio_in(DEVICE(&soc->gpio),
                                           TEGRA30_GPIO_NR(29, 0)));
    /* MMX request output GPIO PDD3 -> IOC request input. */
    qdev_connect_gpio_out(DEVICE(&soc->gpio), TEGRA30_GPIO_NR(29, 3),
                          qdev_get_gpio_in(ioc, 0));

    /*
     * PMIC / board-ID device on the DVC I2C bus (slave 0x2d).  The bootloader
     * reads the 6-byte board ID from registers 0x6a.. and matches the first
     * byte against a board-variant table; 0x20 selects a known board.
     */
    I2CSlave *pmic = i2c_slave_new("tegra30-i2c-dbg", 0x2d);
    qdev_prop_set_uint8(DEVICE(pmic), "fill", 0x20);
    i2c_slave_realize_and_unref(pmic, soc->i2c[4].bus, &error_fatal);

    /*
     * IOC control interface: QNX's IocI2c resmgr masters to the IOC as an I2C
     * slave at 0x4c on the DVC bus (i2c[4]) to push configuration register
     * writes.  Modelled here as a register-file stub so those writes ACK (a
     * NACK makes the resmgr never start its slave thread); the IOC's data
     * channel back to the Tegra is the i2c[1] slave handled by mmx_ioc.
     */
    I2CSlave *ioc_cfg = i2c_slave_new("tegra30-i2c-dbg", 0x4c);
    qdev_prop_set_uint8(DEVICE(ioc_cfg), "fill", 0x00);
    i2c_slave_realize_and_unref(ioc_cfg, soc->i2c[4].bus, &error_fatal);

    /*
     * eMMC on SDMMC4 (the device the second stage loads the OS from).  Backed
     * by the IF_SD drive if the user supplies one, e.g.
     * -drive if=sd,format=raw,file=emmc.img
     */
    DriveInfo *di = drive_get(IF_SD, 0, 0);
    BlockBackend *blk = di ? blk_by_legacy_dinfo(di) : NULL;
    DeviceState *emmc = qdev_new(TYPE_EMMC);
    if (blk) {
        qdev_prop_set_drive(emmc, "drive", blk);
    }
    qdev_realize_and_unref(emmc, qdev_get_child_bus(soc->sdmmc[3], "sd-bus"),
                           &error_fatal);

    uint8_t *nor;
    size_t nor_size;
    if (!g_file_get_contents("/mnt/f51890a6-f42e-44c1-a4b0-92e1089fd7de/home/iscle/Documents/mib/mmx_fs0_k2589", (gchar **) &nor, (gsize *) &nor_size, NULL)) {
        error_report("Failed to load nor");
        exit(1);
    }

    AddressSpace *nsas = cpu_get_address_space(CPU(&soc->avp), ARMASIdx_NS);
    /* QuickBoot is shadowed to and executed from DRAM at 0x83F28000. */
    address_space_write(nsas, 0x83F28000, MEMTXATTRS_UNSPECIFIED, nor, nor_size);

    /*
     * NOR boot flash window at 0x48000000.  The firmware probes the NOR as an
     * AMD/Spansion CFI flash (CFI query at word 0x55 returning "QRY", JEDEC
     * autoselect with unlock cycles at words 0x555/0x2aa) and then DMAs it
     * through the SNOR controller, so model it as a real CFI flash rather than
     * a plain RAM window.  64 MiB, x16, 512 x 128 KiB sectors.
     */
    BlockBackend *nor_blk = blk_new_open(
        "/mnt/f51890a6-f42e-44c1-a4b0-92e1089fd7de/home/iscle/Documents/mib/mmx_fs0_k2589",
        NULL, NULL, 0, &error_fatal);
    DeviceState *flash = qdev_new(TYPE_PFLASH_CFI02);
    qdev_prop_set_drive(flash, "drive", nor_blk);
    qdev_prop_set_uint32(flash, "num-blocks", 512);
    qdev_prop_set_uint32(flash, "sector-length", 0x20000);
    qdev_prop_set_uint8(flash, "width", 2);
    qdev_prop_set_uint16(flash, "unlock-addr0", 0x555);
    qdev_prop_set_uint16(flash, "unlock-addr1", 0x2aa);
    qdev_prop_set_uint16(flash, "id0", 0x0001);
    qdev_prop_set_string(flash, "name", "tegra3-nor");
    sysbus_realize_and_unref(SYS_BUS_DEVICE(flash), &error_fatal);
    sysbus_mmio_map(SYS_BUS_DEVICE(flash), 0, 0x48000000);

    /*
     * Tegra internal RAM (IRAM, 256 KiB at 0x40000000).  On real hardware the
     * boot ROM leaves a Boot Information Table here; the QuickBoot loader
     * validates it before reading the next stage.  Seed it from the captured
     * tegra3_iram.bin dump.
     */
    memory_region_init_ram(&soc->iram, OBJECT(soc), "iram", 0x40000,
                           &error_fatal);
    memory_region_add_subregion(get_system_memory(), 0x40000000, &soc->iram);

    uint8_t *iram;
    size_t iram_size;
    if (g_file_get_contents("/mnt/f51890a6-f42e-44c1-a4b0-92e1089fd7de/home/iscle/Documents/mib/tegra3_iram.bin",
                            (gchar **)&iram, (gsize *)&iram_size, NULL)) {
        address_space_write(nsas, 0x40000000, MEMTXATTRS_UNSPECIFIED,
                            iram, iram_size);
        g_free(iram);
    } else {
        error_report("Failed to load tegra3_iram.bin");
    }


    /*
     * Boot starts on the AVP (COP) processor, which runs the first-stage
     * bootloader out of NOR flash.  It will bring the Cortex-A9 complex out
     * of reset via the flow controller once it has set up the system.
     */
    cpu_set_pc(CPU(&soc->avp), 0x48000000 + 0xe0000);
}

static void mhi2_harman_machine_init(MachineClass *mc)
{
    static const char * const valid_cpu_types[] = {
        ARM_CPU_TYPE_NAME("cortex-a9"),
        NULL
    };

    mc->desc = "MHI2 Harman";
    mc->init = mhi2_harman_init;
    mc->block_default_type = IF_SD;
    mc->units_per_default_bus = 1;
    mc->min_cpus = TEGRA30_NUM_PROCS;
    mc->max_cpus = TEGRA30_NUM_PROCS;
    mc->default_cpus = TEGRA30_NUM_PROCS;
    mc->default_cpu_type = ARM_CPU_TYPE_NAME("cortex-a9");
    mc->valid_cpu_types = valid_cpu_types;
    mc->default_ram_size = 2 * GiB;
    mc->default_ram_id = "mhi2-harman.ram";
}

DEFINE_MACHINE("mhi2-harman", mhi2_harman_machine_init)
