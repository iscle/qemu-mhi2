#include "qemu/osdep.h"
#include "qemu/units.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/misc/tegra30_apb_misc.h"

/* Register offsets */
enum {
    REG_PINMUX_AUX_GMI_A16_0    = 0x230,
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
    REG_PINMUX_AUX_GMI_A16_0_RST         = 0b00110010,
};

static uint64_t tegra30_apb_misc_read(void *opaque, hwaddr offset,
                                      unsigned size)
{
    const Tegra30ApbMiscState *s = TEGRA30_APB_MISC(opaque);
    const uint32_t idx = REG_INDEX(offset);

    switch (offset) {
    case REG_PINMUX_AUX_GMI_A16_0:
        // empty
        break;
    case 0x3e4 ... TEGRA30_APB_MISC_IOSIZE:
        qemu_log_mask(LOG_GUEST_ERROR, "%s: out-of-bounds offset 0x%04x\n",
                      __func__, (uint32_t)offset);
        return 0;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented read offset 0x%04x\n",
                      __func__, (uint32_t)offset);
    }

    return s->regs[idx];
}

static void tegra30_apb_misc_write(void *opaque, hwaddr offset,
                                   uint64_t val, unsigned size)
{
    Tegra30ApbMiscState *s = TEGRA30_APB_MISC(opaque);
    const uint32_t idx = REG_INDEX(offset);

    switch (offset) {
    case REG_PINMUX_AUX_GMI_A16_0:
        // empty
        break;
    case 0x3e4 ... TEGRA30_APB_MISC_IOSIZE:
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

static const MemoryRegionOps tegra30_apb_misc_ops = {
    .read = tegra30_apb_misc_read,
    .write = tegra30_apb_misc_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .impl.min_access_size = 4,
};

static void tegra30_apb_misc_reset(DeviceState *dev)
{
    Tegra30ApbMiscState *s = TEGRA30_APB_MISC(dev);

    /* Set default values for registers */
    s->regs[REG_INDEX(REG_PINMUX_AUX_GMI_A16_0)] = REG_PINMUX_AUX_GMI_A16_0_RST;
}

static void tegra30_apb_misc_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    Tegra30ApbMiscState *s = TEGRA30_APB_MISC(obj);

    /* Memory mapping */
    memory_region_init_io(&s->iomem, OBJECT(s), &tegra30_apb_misc_ops, s,
                          TYPE_TEGRA30_APB_MISC, TEGRA30_APB_MISC_IOSIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

static const VMStateDescription tegra30_apb_misc_vmstate = {
    .name = "tegra30-apb-misc",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, Tegra30ApbMiscState, TEGRA30_APB_MISC_REGS_NUM),
        VMSTATE_END_OF_LIST()
    }
};

static void tegra30_apb_misc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, tegra30_apb_misc_reset);
    dc->vmsd = &tegra30_apb_misc_vmstate;
}

static const TypeInfo tegra30_apb_misc_info = {
    .name          = TYPE_TEGRA30_APB_MISC,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_init = tegra30_apb_misc_init,
    .instance_size = sizeof(Tegra30ApbMiscState),
    .class_init    = tegra30_apb_misc_class_init,
};

static void tegra30_apb_misc_register(void)
{
    type_register_static(&tegra30_apb_misc_info);
}

type_init(tegra30_apb_misc_register)
