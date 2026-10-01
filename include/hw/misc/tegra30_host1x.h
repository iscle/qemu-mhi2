#ifndef HW_MISC_TEGRA30_HOST1X_H
#define HW_MISC_TEGRA30_HOST1X_H
#include "hw/core/sysbus.h"
#include "hw/misc/tegra30_mc.h"
#define TYPE_TEGRA30_HOST1X "tegra30-host1x"
OBJECT_DECLARE_SIMPLE_TYPE(Tegra30Host1xState, TEGRA30_HOST1X)
struct Tegra30Host1xState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[0x24000 / 4];
    uint32_t enabled;
    Tegra30MCState *mc;
    uint32_t channel_class[8];
    uint32_t gr2d[3][0x80];
};
void tegra30_host1x_increment(Tegra30Host1xState *s, unsigned id);
#endif
