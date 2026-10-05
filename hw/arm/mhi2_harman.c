#include "qemu/osdep.h"
#include "qemu/units.h"
#include "chardev/char-fe.h"
#include "system/address-spaces.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/core/boards.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/arm/tegra30.h"
#include "hw/core/loader.h"
#include "system/reset.h"
#include "hw/misc/mmx_ioc.h"
#include "hw/i2c/i2c.h"
#include "hw/sd/sd.h"
#include "hw/block/flash.h"
#include "system/blockdev.h"
#include "system/block-backend.h"

#define GL_BULK_BYTES (1 * MiB)
/* Existing bridge clients limit one locked record to 16 MiB plus its header. */
#define GL_MAX_RECORD (16 * MiB + 20)
#define GL_TX_BYTES (2 * GL_MAX_RECORD)

typedef struct MHI2MachineState {
    MachineState parent_obj;
    char *iram_filename;
    char *pmic_model;
    Tegra30State *soc;
    MemoryRegion gl_mmio;
    MemoryRegion gl_bulk;
    CharFrontend gl_chr;
    uint8_t gl_tx[4096];
    uint8_t *gl_bulk_tx, *gl_rx;
    unsigned gl_rx_count;
    bool gl_locked;
    uint8_t gl_pending[GL_TX_BYTES];
    unsigned gl_tx_head, gl_tx_count;
    guint gl_watch;
} MHI2MachineState;

/* Optional experimental GLES transport, enabled by -chardev ...,id=glbridge.
 * This aperture is a host service, not a Tegra hardware device.
 */
static gboolean gl_writable(void *unused, GIOCondition condition, void *opaque);

static void gl_drain(MHI2MachineState *s)
{
    unsigned budget = 256 * KiB;

    while (s->gl_tx_count && budget && qemu_chr_fe_backend_open(&s->gl_chr)) {
        unsigned n = MIN(budget, MIN(s->gl_tx_count,
                                    GL_TX_BYTES - s->gl_tx_head));
        int written = qemu_chr_fe_write(&s->gl_chr,
                                       s->gl_pending + s->gl_tx_head, n);
        if (written <= 0) {
            break;
        }
        s->gl_tx_head = (s->gl_tx_head + written) % GL_TX_BYTES;
        s->gl_tx_count -= written;
        budget -= written;
    }
    if (s->gl_tx_count && !s->gl_watch &&
        qemu_chr_fe_backend_open(&s->gl_chr)) {
        s->gl_watch = qemu_chr_fe_add_watch(&s->gl_chr, G_IO_OUT | G_IO_HUP,
                                           gl_writable, s);
    }
}

static gboolean gl_writable(void *unused, GIOCondition condition, void *opaque)
{
    MHI2MachineState *s = opaque;
    s->gl_watch = 0;
    if (!(condition & G_IO_HUP)) {
        gl_drain(s);
    }
    return G_SOURCE_REMOVE;
}

static void gl_queue(MHI2MachineState *s, const uint8_t *data, unsigned length)
{
    if (length > GL_TX_BYTES - s->gl_tx_count) {
        error_report("glbridge: TX exceeds reserved record capacity");
        return;
    }
    unsigned tail = (s->gl_tx_head + s->gl_tx_count) % GL_TX_BYTES;
    unsigned first = MIN(length, GL_TX_BYTES - tail);
    memcpy(s->gl_pending + tail, data, first);
    memcpy(s->gl_pending, data + first, length - first);
    s->gl_tx_count += length;
    gl_drain(s);
}

static void gl_clear_tx(MHI2MachineState *s)
{
    if (s->gl_watch) {
        g_source_remove(s->gl_watch);
        s->gl_watch = 0;
    }
    s->gl_tx_head = s->gl_tx_count = 0;
}

static void gl_event(void *opaque, QEMUChrEvent event)
{
    MHI2MachineState *s = opaque;
    if (event == CHR_EVENT_OPENED) {
        gl_drain(s);
    } else if (event == CHR_EVENT_CLOSED) {
        gl_clear_tx(s);
        s->gl_rx_count = 0;
        s->gl_locked = false;
    }
}

static int gl_can_receive(void *opaque)
{
    MHI2MachineState *s = opaque;
    return GL_BULK_BYTES - s->gl_rx_count;
}
static void gl_receive(void *opaque, const uint8_t *buf, int size)
{
    MHI2MachineState *s = opaque;
    memcpy(s->gl_rx + s->gl_rx_count, buf, size);
    s->gl_rx_count += size;
}
static uint64_t gl_read(void *opaque, hwaddr addr, unsigned size)
{
    MHI2MachineState *s = opaque;
    if (addr == 0) {
        return 0x474c4252;
    }
    if (addr == 16) {
        /* Apply backpressure before accepting a record, without sleeping
         * under the BQL. The existing guest lock retry yields its CPU. */
        bool held = s->gl_locked || s->gl_tx_count > GL_TX_BYTES - GL_MAX_RECORD;
        if (!held) {
            s->gl_locked = true;
        }
        return held;
    }
    if (addr == 8) {
        return MIN(s->gl_rx_count, 4096);
    }
    if (addr == 24) {
        return GL_BULK_BYTES;
    }
    if (addr == 32) {
        return s->gl_rx_count;
    }
    if (addr >= 0x2000 && addr - 0x2000 + size <= s->gl_rx_count) {
        uint64_t value = 0;
        for (unsigned i = 0; i < size; i++) {
            value |= (uint64_t)s->gl_rx[addr - 0x2000 + i] << (8 * i);
        }
        return value;
    }
    if (addr == 12 && s->gl_rx_count) {
        uint8_t value = s->gl_rx[0];
        memmove(s->gl_rx, s->gl_rx + 1, --s->gl_rx_count);
        qemu_chr_fe_accept_input(&s->gl_chr);
        return value;
    }
    return 0;
}
static void gl_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    MHI2MachineState *s = opaque;
    if (addr == 16) {
        s->gl_locked = false;
    } else if (((addr == 20 && value <= 4096) || addr == 36) &&
               value <= s->gl_rx_count) {
        s->gl_rx_count -= value;
        memmove(s->gl_rx, s->gl_rx + value, s->gl_rx_count);
        qemu_chr_fe_accept_input(&s->gl_chr);
    } else if (addr >= 0x1000 && addr + size <= 0x2000) {
        for (unsigned i = 0; i < size; i++) {
            s->gl_tx[addr - 0x1000 + i] = value >> (i * 8);
        }
    } else if (addr == 4 && value <= sizeof(s->gl_tx)) {
        gl_queue(s, s->gl_tx, value);
    } else if (addr == 28 && value <= GL_BULK_BYTES) {
        gl_queue(s, s->gl_bulk_tx, value);
    }
}
static const MemoryRegionOps gl_ops = {
    .read = gl_read, .write = gl_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static char *mhi2_get_pmic(Object *obj, Error **errp)
{
    MHI2MachineState *s = (MHI2MachineState *)obj;

    return g_strdup(s->pmic_model ? s->pmic_model : "legacy");
}

static void mhi2_set_pmic(Object *obj, const char *value, Error **errp)
{
    MHI2MachineState *s = (MHI2MachineState *)obj;

    if (strcmp(value, "legacy") && strcmp(value, "tps65911") &&
        strcmp(value, "max20024")) {
        error_setg(errp, "pmic must be legacy, tps65911 or max20024");
        return;
    }
    g_free(s->pmic_model);
    s->pmic_model = g_strdup(value);
}

static char *mhi2_get_iram(Object *obj, Error **errp)
{
    return g_strdup(((MHI2MachineState *)obj)->iram_filename);
}

static void mhi2_set_iram(Object *obj, const char *value, Error **errp)
{
    MHI2MachineState *s = (MHI2MachineState *)obj;
    g_free(s->iram_filename);
    s->iram_filename = g_strdup(value);
}

static void mhi2_reset(void *opaque)
{
    MHI2MachineState *s = opaque;
    CPUState *boot_cpu = CPU(&s->soc->avp);

    s->gl_locked = false;
    s->gl_rx_count = 0;
    gl_clear_tx(s);

    for (int i = 0; i < TEGRA30_NUM_CPUS; i++) {
        cpu_reset(CPU(&s->soc->cpus[i]));
    }
    cpu_reset(CPU(&s->soc->lp_cpu));
    cpu_reset(boot_cpu);
    /* IOC normal-power indication: Quickboot samples GPIO CC5 for lp=. */
    qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->soc->gpio),
                                 TEGRA30_GPIO_NR(28, 5)), 1);
    /* devg-nvgpio maps the active-low USB overcurrent input to C6.
     * A powered virtual connector with no electrical fault leaves it high. */
    qemu_set_irq(qdev_get_gpio_in(DEVICE(&s->soc->gpio),
                                 TEGRA30_GPIO_NR(2, 6)), 1);
    cpu_set_pc(boot_cpu, 0x480e0000);
}

static void mhi2_harman_init(MachineState *machine)
{
    Tegra30State *soc;
    MHI2MachineState *ms = (MHI2MachineState *)machine;

    if (!machine->firmware) {
        error_report("MHI2 requires -bios <64 MiB NOR image>");
        exit(1);
    }
    info_report("Initializing MHI2 Harman");

    soc = TEGRA30(object_new(TYPE_TEGRA30));
    ms->soc = soc;
    object_property_add_child(OBJECT(machine), "soc", OBJECT(soc));
    
    // TODO: SoC setup

    // TODO: Move emem init to tegra30.c
    memory_region_init_ram(&soc->emem, OBJECT(soc), "emem", machine->ram_size, &error_fatal);
    memory_region_add_subregion(get_system_memory(), 0x80000000, &soc->emem);

    sysbus_realize_and_unref(SYS_BUS_DEVICE(soc), &error_fatal);
    Chardev *glchr = qemu_chr_find("glbridge");
    if (glchr) {
        /* RAM staging avoids trapping each pixel word through MMIO. Control
         * registers still serialize complete records and bound both lengths.
         * This is part of the optional host bridge, not Tegra hardware. */
        memory_region_init_ram(&ms->gl_bulk, OBJECT(soc),
                               "experimental-glbridge-bulk", 2 * GL_BULK_BYTES,
                               &error_fatal);
        memory_region_add_subregion(get_system_memory(), 0x5e000000,
                                    &ms->gl_bulk);
        ms->gl_bulk_tx = memory_region_get_ram_ptr(&ms->gl_bulk);
        ms->gl_rx = ms->gl_bulk_tx + GL_BULK_BYTES;
        qemu_chr_fe_init(&ms->gl_chr, glchr, &error_fatal);
        qemu_chr_fe_set_handlers(&ms->gl_chr, gl_can_receive, gl_receive,
                                gl_event, NULL, ms, NULL, true);
        memory_region_init_io(&ms->gl_mmio, OBJECT(machine), &gl_ops, ms,
                              "experimental-glbridge", 0x3000);
        memory_region_add_subregion(get_system_memory(), 0x5f000000,
                                    &ms->gl_mmio);
    }


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
    I2CSlave *pmic;
    if (!ms->pmic_model || !strcmp(ms->pmic_model, "legacy")) {
        pmic = i2c_slave_new("tegra30-i2c-dbg", 0x2d);
        qdev_prop_set_uint8(DEVICE(pmic), "fill", 0x20);
        i2c_slave_realize_and_unref(pmic, soc->i2c[4].bus, &error_fatal);
    } else {
        bool maxim = !strcmp(ms->pmic_model, "max20024");

        pmic = i2c_slave_new("mhi2-pmic", maxim ? 0x3c : 0x2d);
        qdev_prop_set_bit(DEVICE(pmic), "maxim", maxim);
        i2c_slave_realize_and_unref(pmic, soc->i2c[4].bus, &error_fatal);
        /* Emulator fixture wiring; production PMIC IRQ routing is unproven. */
        qdev_connect_gpio_out(DEVICE(pmic), 0,
                              qdev_get_gpio_in(DEVICE(&soc->a9mpcore), 86));
    }

    /*
     * IOC control interface: QNX's IocI2c resmgr masters to the IOC as an I2C
     * slave at 0x4c on the DVC bus (i2c[4]) to push configuration register
     * writes.  Modelled here as a register-file stub so those writes ACK (a
     * NACK makes the resmgr never start its slave thread); the IOC's data
     * channel back to the Tegra is the i2c[1] slave handled by mmx_ioc.
     */
    I2CSlave *ioc_cfg = i2c_slave_new("tegra30-i2c-dbg", 0x4c);
    qdev_prop_set_uint8(DEVICE(ioc_cfg), "fill", 0x00);
    /* devg-nvtmon accesses the local/remote diode sensor at DVC 0x4c. */
    qdev_prop_set_bit(DEVICE(ioc_cfg), "temperature-sensor", true);
    i2c_slave_realize_and_unref(ioc_cfg, soc->i2c[4].bus, &error_fatal);

    /*
     * eMMC on SDMMC4 (QNX mounts the app filesystem from it). Backed
     * by the IF_SD drive if the user supplies one, e.g.
     * -drive if=sd,format=raw,file=emmc.img
     */
    DriveInfo *di = drive_get(IF_SD, 0, 0);
    BlockBackend *blk = di ? blk_by_legacy_dinfo(di) : NULL;
    DeviceState *emmc = qdev_new(TYPE_EMMC);
    qdev_prop_set_uint32(emmc, "ocr-power-delay-ns", 100000000);
    if (blk) {
        qdev_prop_set_drive(emmc, "drive", blk);
    }
    qdev_realize_and_unref(emmc, qdev_get_child_bus(soc->sdmmc[3], "sd-bus"),
                           &error_fatal);

    /* Removable readers: stock QNX identifies SDMMC1/3 with CD D3/D4 and
     * WP V2/V3. IF_SD index 0 remains the eMMC for existing launchers. */
    for (int slot = 0; slot < 2; slot++) {
        DeviceState *host = soc->sdmmc[slot ? 2 : 0];
        DriveInfo *card_di = drive_get(IF_SD, slot + 1, 0);

        qdev_connect_gpio_out_named(host, "card-inserted", 0,
            qemu_irq_invert(qdev_get_gpio_in(DEVICE(&soc->gpio),
                                            TEGRA30_GPIO_NR(3, 3 + slot))));
        qdev_connect_gpio_out_named(host, "card-readonly", 0,
            qdev_get_gpio_in(DEVICE(&soc->gpio), TEGRA30_GPIO_NR(21, 2 + slot)));
        if (card_di && blk_bs(blk_by_legacy_dinfo(card_di))) {
            DeviceState *card = qdev_new(TYPE_SD_CARD);

            qdev_prop_set_drive(card, "drive", blk_by_legacy_dinfo(card_di));
            qdev_realize_and_unref(card, qdev_get_child_bus(host, "sd-bus"),
                                  &error_fatal);
        }
    }

    uint8_t *nor;
    size_t nor_size;
    if (!g_file_get_contents(machine->firmware, (gchar **) &nor, (gsize *) &nor_size, NULL)) {
        error_report("Failed to load nor");
        exit(1);
    }

    if (nor_size != 64 * MiB) {
        error_report("MHI2 NOR image must be exactly 64 MiB");
        exit(1);
    }

    /* QuickBoot is shadowed to and executed from DRAM at 0x83F28000. */
    rom_add_blob_fixed("mhi2-quickboot-shadow", nor, nor_size, 0x83F28000);
    g_free(nor);

    /*
     * NOR boot flash window at 0x48000000.  The firmware probes the NOR as an
     * AMD/Spansion CFI flash (CFI query at word 0x55 returning "QRY", JEDEC
     * autoselect with unlock cycles at words 0x555/0x2aa) and then DMAs it
     * through the SNOR controller, so model it as a real CFI flash rather than
     * a plain RAM window.  64 MiB, x16, 512 x 128 KiB sectors.
     */
    /* Writable per-run overlay: QNX persists files without modifying -bios. */
    BlockBackend *nor_blk = blk_new_open(
        machine->firmware,
        NULL, NULL, BDRV_O_RDWR | BDRV_O_SNAPSHOT, &error_fatal);
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

    if (ms->iram_filename) {
        if (load_image_targphys(ms->iram_filename, 0x40000000, 0x40000, &error_fatal) < 0) {
            error_report("Cannot load IRAM image %s", ms->iram_filename);
            exit(1);
        }
    } else {
        error_report("Quickboot needs -machine iram=<captured IRAM/BIT image>");
        exit(1);
    }

    qemu_register_reset(mhi2_reset, ms);

}

static void mhi2_harman_machine_init(MachineClass *mc)
{
    static const char * const valid_cpu_types[] = {
        ARM_CPU_TYPE_NAME("cortex-a9"),
        NULL
    };

    object_class_property_add_str(OBJECT_CLASS(mc), "iram",
                                  mhi2_get_iram, mhi2_set_iram);
    object_class_property_add_str(OBJECT_CLASS(mc), "pmic",
                                  mhi2_get_pmic, mhi2_set_pmic);
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

DEFINE_MACHINE_EXTENDED("mhi2-harman", MACHINE, MHI2MachineState,
                        mhi2_harman_machine_init, false, false, NULL)
