#include "qemu/osdep.h"
#include "qemu/units.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "system/address-spaces.h"
#include "target/arm/arm-powerctl.h"
#include "hw/misc/tegra30_clk.h"

/* Register offsets */
enum {
    REG_CLK_RST_CONTROLLER_PLLP_BASE_0      = 0x0A0,
    REG_CLK_RST_CONTROLLER_PLLU_BASE_0      = 0x0C0,
    REG_CLK_RST_CONTROLLER_PLLX_BASE_0      = 0x0E0,
    REG_CLK_RST_CONTROLLER_CLK_ENB_U_SET_0  = 0x330,
    REG_CLK_RST_CONTROLLER_RST_CPUG_CMPLX_CLR_0 = 0x344,
};

/* Number of Cortex-A9 cores in the "G" complex driven by RST_CPUG_CMPLX. */
#define TEGRA30_CLK_NUM_G_CPUS  4

/* Where the firmware stores the CPU entry point (EVP block). */
#define EVP_CPU_RESET_VECTOR    0x6000F100

#define REG_INDEX(offset)    (offset / sizeof(uint32_t))

/* CLK register flags */
enum {
    REG_PLLP_LOCK   = (1 << 27),
};

enum {
    REG_PLLU_LOCK   = (1 << 27),
};

enum {
    REG_PLLX_LOCK   = (1 << 27),
};

/* Register reset values */
enum {
    REG_CLK_RST_CONTROLLER_PLLP_BASE_0_RST      = 0b00000000000000000000000100001100,
    REG_CLK_RST_CONTROLLER_PLLU_BASE_0_RST      = 0b00000000000000000000000100001100,
    REG_CLK_RST_CONTROLLER_PLLX_BASE_0_RST      = 0b00000000000000000000000100001100,
    REG_CLK_RST_CONTROLLER_CLK_ENB_U_SET_0_RST  = 0b00101111100000000101000000000,
};

static uint64_t tegra30_clk_read(void *opaque, hwaddr offset,
                                      unsigned size)
{
    const Tegra30ClkState *s = TEGRA30_CLK(opaque);
    /* TRM 6.5: legacy CCLK addresses alias the active cluster. This machine
     * currently runs the G cluster only (the FLOW cluster state is zero). */
    if (offset == 0x20) {
        offset = 0x368;
    } else if (offset == 0x24) {
        offset = 0x36c;
    }
    const uint32_t idx = REG_INDEX(offset);

    switch (offset) {
    case 0x80:  /* PLLC */
    case 0x90:  /* PLLM */
    case 0xb0:  /* PLLA */
    case 0xd0:  /* PLLD */
    case 0x4b8: /* PLLD2 */
        /* Model a locked PLL immediately after software enables it. */
        return (s->regs[idx] & ~(1u << 27)) |
               ((s->regs[idx] & (1u << 30)) ? (1u << 27) : 0);
    case 0xec: /* PLLE_MISC: calibration ready, lock follows PLLE enable. */
        return s->regs[idx] | (1u << 15) |
               ((s->regs[0xe8 / 4] & (1u << 30)) ? (1u << 11) : 0);
    case REG_CLK_RST_CONTROLLER_PLLP_BASE_0:
        return s->regs[idx] | REG_PLLP_LOCK;
    case REG_CLK_RST_CONTROLLER_PLLU_BASE_0:
        return s->regs[idx] | REG_PLLU_LOCK;
    case REG_CLK_RST_CONTROLLER_PLLX_BASE_0:
        return s->regs[idx] | REG_PLLX_LOCK;
    case REG_CLK_RST_CONTROLLER_CLK_ENB_U_SET_0:
        // empty
        break;
    case 0x4c0 ... TEGRA30_CLK_IOSIZE:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: out-of-bounds offset 0x%04x\n",
                      __func__, (uint32_t)offset);
        return 0;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented read offset 0x%04x\n",
                      __func__, (uint32_t)offset);
    }

    return s->regs[idx];
}

static void tegra30_clk_write(void *opaque, hwaddr offset,
                                   uint64_t val, unsigned size)
{
    Tegra30ClkState *s = TEGRA30_CLK(opaque);
    /* TRM 6.5: legacy CCLK addresses alias the active cluster. This machine
     * currently runs the G cluster only (the FLOW cluster state is zero). */
    if (offset == 0x20) {
        offset = 0x368;
    } else if (offset == 0x24) {
        offset = 0x36c;
    }
    const uint32_t idx = REG_INDEX(offset);

    switch (offset) {
    case REG_CLK_RST_CONTROLLER_RST_CPUG_CMPLX_CLR_0: {
        /*
         * Writing a CPUn core-reset bit (bits [3:0]) to RST_CPUG_CMPLX_CLR
         * deasserts that Cortex-A9's reset, i.e. brings it out of reset.  The
         * QNX startup uses this to release the secondary G-cluster cores after
         * programming their shared entry point into EVP_CPU_RESET_VECTOR.
         * Fetch that vector and power the affected cores on there.  CPU0 is
         * already running, so arm_set_cpu_on() is a no-op for it.
         */
        uint32_t vec = address_space_ldl_le(&address_space_memory,
                                            EVP_CPU_RESET_VECTOR,
                                            MEMTXATTRS_UNSPECIFIED, NULL);
        for (int cpu = 0; cpu < TEGRA30_CLK_NUM_G_CPUS; cpu++) {
            if ((val & (1u << cpu)) && vec != 0) {
                arm_set_cpu_on(cpu, vec, 0, 1 /* EL1 */, false /* AArch32 */);
            }
        }
        break;
    }
    case 0x4c0 ... TEGRA30_CLK_IOSIZE:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: out-of-bounds offset 0x%04x\n",
                      __func__, (uint32_t)offset);
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented write offset 0x%04x\n",
                      __func__, (uint32_t)offset);
        break;
    }

    s->regs[idx] = (uint32_t) val;
}

static const MemoryRegionOps tegra30_clk_ops = {
    .read = tegra30_clk_read,
    .write = tegra30_clk_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .impl.min_access_size = 4,
};

static void tegra30_clk_reset(DeviceState *dev)
{
    Tegra30ClkState *s = TEGRA30_CLK(dev);

    memset(s->regs, 0, sizeof(s->regs));
    /* NVIDIA T30 arclk_rst.h reset values. */
    s->regs[0x20 / 4] = 0x10000000; /* CCLK_BURST_POLICY: IDLE, CLK_M */
    s->regs[0x368 / 4] = 0x10000000; /* CCLKG_BURST_POLICY: IDLE, CLK_M */
    s->regs[0x370 / 4] = 0x10000000; /* CCLKLP_BURST_POLICY: IDLE, CLK_M */
    /* TRM 6.5.22, .26, .32, .37, .173: DIVN=1, DIVM=12 at reset.
     * Unspecified reset bits are represented as zero. */
    s->regs[0x80 / 4] = 0x10c;
    s->regs[0x90 / 4] = 0x10c;
    s->regs[0xb0 / 4] = 0x10c;
    s->regs[0xd0 / 4] = 0x10c;
    s->regs[0x4b8 / 4] = 0x10c;
    s->regs[0xe8 / 4] = 0x0d18c801; /* PLLE_BASE, TRM 6.5.41 */
    s->regs[0x50 / 4] = 0x3f1; /* OSC_CTRL: default 13 MHz crystal. */
    if (s->oscillator_12mhz) {
        s->regs[0x50 / 4] |= 8U << 28;
    }

    /* Set default values for registers */
    s->regs[REG_INDEX(REG_CLK_RST_CONTROLLER_PLLP_BASE_0)] = REG_CLK_RST_CONTROLLER_PLLP_BASE_0_RST;
    s->regs[REG_INDEX(REG_CLK_RST_CONTROLLER_PLLU_BASE_0)] = REG_CLK_RST_CONTROLLER_PLLU_BASE_0_RST;
    s->regs[REG_INDEX(REG_CLK_RST_CONTROLLER_PLLX_BASE_0)] = REG_CLK_RST_CONTROLLER_PLLX_BASE_0_RST;
    s->regs[REG_INDEX(REG_CLK_RST_CONTROLLER_CLK_ENB_U_SET_0)] = REG_CLK_RST_CONTROLLER_CLK_ENB_U_SET_0_RST;
}

static void tegra30_clk_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    Tegra30ClkState *s = TEGRA30_CLK(obj);

    /* Memory mapping */
    memory_region_init_io(&s->iomem, OBJECT(s), &tegra30_clk_ops, s,
                          TYPE_TEGRA30_CLK, TEGRA30_CLK_IOSIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

static const VMStateDescription tegra30_clk_vmstate = {
    .name = "tegra30-clk",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, Tegra30ClkState, TEGRA30_CLK_REGS_NUM),
        VMSTATE_END_OF_LIST()
    }
};

static const Property tegra30_clk_properties[] = {
    DEFINE_PROP_BOOL("oscillator-12mhz", Tegra30ClkState, oscillator_12mhz,
                     false),
};

static void tegra30_clk_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, tegra30_clk_reset);
    dc->vmsd = &tegra30_clk_vmstate;
    device_class_set_props(dc, tegra30_clk_properties);
}

static const TypeInfo tegra30_clk_info = {
    .name          = TYPE_TEGRA30_CLK,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_init = tegra30_clk_init,
    .instance_size = sizeof(Tegra30ClkState),
    .class_init    = tegra30_clk_class_init,
};

static void tegra30_clk_register(void)
{
    type_register_static(&tegra30_clk_info);
}

type_init(tegra30_clk_register)
