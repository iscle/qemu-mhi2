#include "qemu/osdep.h"
#include "qemu/units.h"
#include "hw/sysbus.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/timer/tegra30_timer.h"

/* Register offsets */
enum {
    REG_TIMERUS_CNTR_1US_0    = 0x010,
    REG_TIMERUS_USEC_CFG_0    = 0x014,
    REG_TIMERUS_CNTR_FREEZE_0 = 0x04c,
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
    REG_TIMERUS_CNTR_1US_0_RST         = 0x0,
    REG_TIMERUS_USEC_CFG_0_RST         = 0b0000000000001100,
    REG_TIMERUS_CNTR_FREEZE_0_RST      = 0b00000,
};

static uint64_t tegra30_timer_read(void *opaque, hwaddr offset,
                                      unsigned size)
{
    const Tegra30TimerState *s = TEGRA30_TIMER(opaque);
    const uint32_t idx = REG_INDEX(offset);

    switch (offset) {
    case REG_TIMERUS_CNTR_1US_0:
        return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000;
        break;
    // case 0x4c0 ... TEGRA30_TIMER_IOSIZE:
    //     qemu_log_mask(LOG_GUEST_ERROR, "%s: out-of-bounds offset 0x%04x\n",
    //                   __func__, (uint32_t)offset);
    //     return 0;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented read offset 0x%04x\n",
                      __func__, (uint32_t)offset);
    }

    return s->regs[idx];
}

static void tegra30_timer_write(void *opaque, hwaddr offset,
                                   uint64_t val, unsigned size)
{
    Tegra30TimerState *s = TEGRA30_TIMER(opaque);
    const uint32_t idx = REG_INDEX(offset);

    switch (offset) {
    // case 0x4c0 ... TEGRA30_TIMER_IOSIZE:
    //     qemu_log_mask(LOG_GUEST_ERROR, "%s: out-of-bounds offset 0x%04x\n",
    //                   __func__, (uint32_t)offset);
    //     break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented write offset 0x%04x\n",
                      __func__, (uint32_t)offset);
        break;
    }

    s->regs[idx] = (uint32_t) val;
}

static const MemoryRegionOps tegra30_timer_ops = {
    .read = tegra30_timer_read,
    .write = tegra30_timer_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .impl.min_access_size = 4,
};

static void tegra30_timer_reset(DeviceState *dev)
{
    Tegra30TimerState *s = TEGRA30_TIMER(dev);

    /* Set default values for registers */
    s->regs[REG_INDEX(REG_TIMERUS_CNTR_1US_0)] = REG_TIMERUS_CNTR_1US_0_RST;
    s->regs[REG_INDEX(REG_TIMERUS_USEC_CFG_0)] = REG_TIMERUS_USEC_CFG_0_RST;
    s->regs[REG_INDEX(REG_TIMERUS_CNTR_FREEZE_0)] = REG_TIMERUS_CNTR_FREEZE_0_RST;
}

static void tegra30_timer_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    Tegra30TimerState *s = TEGRA30_TIMER(obj);

    /* Memory mapping */
    memory_region_init_io(&s->iomem, OBJECT(s), &tegra30_timer_ops, s,
                          TYPE_TEGRA30_TIMER, TEGRA30_TIMER_IOSIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

static const VMStateDescription tegra30_timer_vmstate = {
    .name = "tegra30-timer",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, Tegra30TimerState, TEGRA30_TIMER_REGS_NUM),
        VMSTATE_END_OF_LIST()
    }
};

static void tegra30_timer_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, tegra30_timer_reset);
    dc->vmsd = &tegra30_timer_vmstate;
}

static const TypeInfo tegra30_timer_info = {
    .name          = TYPE_TEGRA30_TIMER,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_init = tegra30_timer_init,
    .instance_size = sizeof(Tegra30TimerState),
    .class_init    = tegra30_timer_class_init,
};

static void tegra30_timer_register(void)
{
    type_register_static(&tegra30_timer_info);
}

type_init(tegra30_timer_register)
