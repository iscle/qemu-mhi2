/* Tegra30 MC/SMMU page tables, as documented by NVIDIA armc.h and Linux
 * tegra-smmu.c. TLB/cache operations are synchronous; translations walk RAM.
 */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include "hw/misc/tegra30_mc.h"

bool tegra30_mc_translate(Tegra30MCState *s, unsigned group_reg,
                          uint32_t iova, hwaddr *physical)
{
    uint32_t group = s->regs[group_reg / 4];
    uint32_t pde, pte, ptb;
    MemTxResult result;
    *physical = iova;
    if (!(s->regs[0x10 / 4] & 1) || !(group & (1u << 31))) {
        return true;
    }
    ptb = s->ptb[group & 0x7f];
    if (!(ptb & (1u << 31))) {
        return false;
    }
    pde = address_space_ldl_le(&address_space_memory,
            ((hwaddr)(ptb & 0xfffff) << 12) + ((iova >> 22) * 4),
            MEMTXATTRS_UNSPECIFIED, &result);
    if (result != MEMTX_OK || !(pde & (1u << 31))) {
        return false;
    }
    if (!(pde & (1u << 28))) {
        *physical = ((hwaddr)(pde & 0xffc00) << 12) | (iova & 0x3fffff);
        return true;
    }
    pte = address_space_ldl_le(&address_space_memory,
            ((hwaddr)(pde & 0xfffff) << 12) + (((iova >> 12) & 1023) * 4),
            MEMTXATTRS_UNSPECIFIED, &result);
    if (result != MEMTX_OK || !(pte & (1u << 31))) {
        return false;
    }
    *physical = ((hwaddr)(pte & 0xfffff) << 12) | (iova & 0xfff);
    return true;
}

static uint64_t mc_read(void *opaque, hwaddr offset, unsigned size)
{
    Tegra30MCState *s = opaque;
    if (offset == 0x20) {
        return s->ptb[s->regs[0x1c / 4] & 0x7f];
    }
    if (offset == 0x204) {
        /* CLIENT_HOTRESET_STATUS: all modeled memory transactions complete
         * synchronously, so a requested client flush is already drained.
         * Tegra30 control/status offsets match Linux drivers/memory/tegra/
         * tegra30.c's tegra30_mc_resets table. */
        return s->regs[0x200 / 4];
    }
    return s->regs[offset / 4];
}
static void mc_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    Tegra30MCState *s = opaque;
    if (offset == 0x204) {
        return; /* Read-only completion state. */
    }
    if (offset == 0x20) {
        s->ptb[s->regs[0x1c / 4] & 0x7f] = value;
    }
    s->regs[offset / 4] = value;
}
static const MemoryRegionOps mc_ops = {
    .read = mc_read, .write = mc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};
static void mc_reset(DeviceState *dev)
{
    Tegra30MCState *s = TEGRA30_MC(dev);
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->ptb, 0, sizeof(s->ptb));
}
static void mc_init(Object *obj)
{
    Tegra30MCState *s = TEGRA30_MC(obj);
    memory_region_init_io(&s->iomem, obj, &mc_ops, s, TYPE_TEGRA30_MC, 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
}
static const VMStateDescription mc_vmstate = {
    .name = TYPE_TEGRA30_MC, .version_id = 1, .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, Tegra30MCState, 1024),
        VMSTATE_UINT32_ARRAY(ptb, Tegra30MCState, 128),
        VMSTATE_END_OF_LIST()
    }
};
static void mc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->vmsd = &mc_vmstate;
    device_class_set_legacy_reset(dc, mc_reset);
}
static const TypeInfo mc_info = {
    .name = TYPE_TEGRA30_MC, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Tegra30MCState), .instance_init = mc_init,
    .class_init = mc_class_init,
};
static void mc_register_types(void) { type_register_static(&mc_info); }
type_init(mc_register_types)
