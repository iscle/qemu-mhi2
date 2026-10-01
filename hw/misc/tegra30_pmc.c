#include "qemu/osdep.h"
#include "qemu/units.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/misc/tegra30_pmc.h"

/* Register offsets */
enum {
    REG_PWRGATE_TOGGLE              = 0x030,
    REG_REMOVE_CLAMPING             = 0x034,
    REG_APBDEV_PMC_PWRGATE_STATUS_0 = 0x038,
    REG_APBDEV_PMC_SCRATCH1_0       = 0x054,
};

#define REG_INDEX(offset)    (offset / sizeof(uint32_t))

// /* CCU register flags */
// enum {
//     REG_DRAM_CFG_UPDATE      = (1 << 16),
// };

// enum {
//     REG_PLL_ENABLE           = (1 << 31),
//     REG_PLL_LOCK             = (1 << 28),
// };


/* Register reset values */
enum {
    REG_APBDEV_PMC_PWRGATE_STATUS_0_RST = 0b11111111111111,
    REG_APBDEV_PMC_SCRATCH1_0_RST       = 0x0,
};

static uint64_t tegra30_pmc_read(void *opaque, hwaddr offset,
                                      unsigned size)
{
    const Tegra30PmcState *s = TEGRA30_PMC(opaque);
    const uint32_t idx = REG_INDEX(offset);

    switch (offset) {
    case REG_PWRGATE_TOGGLE:
    case REG_REMOVE_CLAMPING:
        break;
    case REG_APBDEV_PMC_PWRGATE_STATUS_0:
        // empty
        break;
    case REG_APBDEV_PMC_SCRATCH1_0:
        // empty
        break;
    case 0x314 ... TEGRA30_PMC_IOSIZE - 1:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: out-of-bounds offset 0x%04x\n",
                      __func__, (uint32_t)offset);
        return 0;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented read offset 0x%04x\n",
                      __func__, (uint32_t)offset);
    }

    return s->regs[idx];
}

static void tegra30_pmc_write(void *opaque, hwaddr offset,
                                   uint64_t val, unsigned size)
{
    Tegra30PmcState *s = TEGRA30_PMC(opaque);
    const uint32_t idx = REG_INDEX(offset);

    switch (offset) {
    case REG_PWRGATE_TOGGLE:
        /* START toggles the selected partition; completion clears START. */
        if ((val & BIT(8)) && (val & 0x1f) < 14) {
            s->regs[REG_INDEX(REG_APBDEV_PMC_PWRGATE_STATUS_0)] ^=
                BIT(val & 0x1f);
        }
        s->regs[idx] = val & ~BIT(8);
        return;
    case REG_REMOVE_CLAMPING:
        /* Command register: clamp removal completes synchronously. */
        s->regs[idx] = 0;
        return;
    case REG_APBDEV_PMC_PWRGATE_STATUS_0:
        return;
    case 0x314 ... TEGRA30_PMC_IOSIZE - 1:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: out-of-bounds offset 0x%04x\n",
                      __func__, (uint32_t)offset);
        return;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented write offset 0x%04x\n",
                      __func__, (uint32_t)offset);
        break;
    }

    s->regs[idx] = (uint32_t) val;
}

static const MemoryRegionOps tegra30_pmc_ops = {
    .read = tegra30_pmc_read,
    .write = tegra30_pmc_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .impl.min_access_size = 4,
};

static void tegra30_pmc_reset(DeviceState *dev)
{
    Tegra30PmcState *s = TEGRA30_PMC(dev);

    memset(s->regs, 0, sizeof(s->regs));
    /* Set default values for registers */
    s->regs[REG_INDEX(REG_APBDEV_PMC_PWRGATE_STATUS_0)] = REG_APBDEV_PMC_PWRGATE_STATUS_0_RST;
    s->regs[REG_INDEX(REG_APBDEV_PMC_SCRATCH1_0)] = REG_APBDEV_PMC_SCRATCH1_0_RST;
}

static void tegra30_pmc_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    Tegra30PmcState *s = TEGRA30_PMC(obj);

    /* Memory mapping */
    memory_region_init_io(&s->iomem, OBJECT(s), &tegra30_pmc_ops, s,
                          TYPE_TEGRA30_PMC, TEGRA30_PMC_IOSIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

static const VMStateDescription tegra30_pmc_vmstate = {
    .name = "tegra30-pmc",
    .version_id = 2,
    .minimum_version_id = 2,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, Tegra30PmcState, TEGRA30_PMC_REGS_NUM),
        VMSTATE_END_OF_LIST()
    }
};

static void tegra30_pmc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, tegra30_pmc_reset);
    dc->vmsd = &tegra30_pmc_vmstate;
}

static const TypeInfo tegra30_pmc_info = {
    .name          = TYPE_TEGRA30_PMC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_init = tegra30_pmc_init,
    .instance_size = sizeof(Tegra30PmcState),
    .class_init    = tegra30_pmc_class_init,
};

static void tegra30_pmc_register(void)
{
    type_register_static(&tegra30_pmc_info);
}

type_init(tegra30_pmc_register)
