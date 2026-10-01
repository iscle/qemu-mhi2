#ifndef HW_MISC_TEGRA30_MC_H
#define HW_MISC_TEGRA30_MC_H
#include "hw/core/sysbus.h"
#define TYPE_TEGRA30_MC "tegra30-mc"
OBJECT_DECLARE_SIMPLE_TYPE(Tegra30MCState, TEGRA30_MC)
struct Tegra30MCState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t regs[1024];
    uint32_t ptb[128];
};
bool tegra30_mc_translate(Tegra30MCState *s, unsigned group_reg,
                          uint32_t iova, hwaddr *physical);
#endif
