#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/misc/tegra30_arb_sema.h"

/*
 * nVidia Tegra Arbitration Semaphore (single-processor model).
 *
 * The arbitration semaphore arbitrates ownership of shared resources between
 * the main CPU and the AVP/COP.  Software requests a semaphore bit by writing
 * it to SMP_GET, then polls SMP_GNT_ST until the bit appears, and releases it
 * by writing the bit to SMP_PUT.
 *
 * This SoC model only ever runs one processor at a time, so there is no
 * contention: every requested bit is granted immediately.
 */

/* Register offsets */
enum {
    REG_SMP_GNT_ST = 0x0,   /* grant status (RO)            */
    REG_SMP_GET    = 0x4,   /* request bits (write to set)  */
    REG_SMP_PUT    = 0x8,   /* release bits (write to clear)*/
    REG_SMP_REQ_ST = 0xC,   /* pending request status (RO)  */
};

static uint64_t tegra30_arb_sema_read(void *opaque, hwaddr offset,
                                      unsigned size)
{
    Tegra30ArbSemaState *s = TEGRA30_ARB_SEMA(opaque);

    switch (offset) {
    case REG_SMP_GNT_ST:
        return s->gnt_st;
    case REG_SMP_GET:
        return s->get;
    case REG_SMP_REQ_ST:
        return s->get & ~s->gnt_st;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented read offset 0x%04x\n",
                      __func__, (uint32_t)offset);
        return 0;
    }
}

static void tegra30_arb_sema_write(void *opaque, hwaddr offset,
                                   uint64_t val, unsigned size)
{
    Tegra30ArbSemaState *s = TEGRA30_ARB_SEMA(opaque);

    switch (offset) {
    case REG_SMP_GET:
        /* Request bits; uncontended, so grant them right away. */
        s->get |= (uint32_t)val;
        s->gnt_st = s->get;
        break;
    case REG_SMP_PUT:
        /* Release bits. */
        s->get &= ~(uint32_t)val;
        s->gnt_st &= ~(uint32_t)val;
        break;
    default:
        qemu_log_mask(LOG_UNIMP, "%s: unimplemented write offset 0x%04x\n",
                      __func__, (uint32_t)offset);
        break;
    }
}

static const MemoryRegionOps tegra30_arb_sema_ops = {
    .read = tegra30_arb_sema_read,
    .write = tegra30_arb_sema_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .impl.min_access_size = 4,
};

static void tegra30_arb_sema_reset(DeviceState *dev)
{
    Tegra30ArbSemaState *s = TEGRA30_ARB_SEMA(dev);

    s->get = 0;
    s->gnt_st = 0;
}

static void tegra30_arb_sema_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    Tegra30ArbSemaState *s = TEGRA30_ARB_SEMA(obj);

    memory_region_init_io(&s->iomem, OBJECT(s), &tegra30_arb_sema_ops, s,
                          TYPE_TEGRA30_ARB_SEMA, TEGRA30_ARB_SEMA_IOSIZE);
    sysbus_init_mmio(sbd, &s->iomem);
}

static const VMStateDescription tegra30_arb_sema_vmstate = {
    .name = "tegra30-arb-sema",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(get, Tegra30ArbSemaState),
        VMSTATE_UINT32(gnt_st, Tegra30ArbSemaState),
        VMSTATE_END_OF_LIST()
    }
};

static void tegra30_arb_sema_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, tegra30_arb_sema_reset);
    dc->vmsd = &tegra30_arb_sema_vmstate;
}

static const TypeInfo tegra30_arb_sema_info = {
    .name          = TYPE_TEGRA30_ARB_SEMA,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_init = tegra30_arb_sema_init,
    .instance_size = sizeof(Tegra30ArbSemaState),
    .class_init    = tegra30_arb_sema_class_init,
};

static void tegra30_arb_sema_register(void)
{
    type_register_static(&tegra30_arb_sema_info);
}

type_init(tegra30_arb_sema_register)
