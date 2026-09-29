/*
 * nVidia Tegra legacy interrupt controller (ICTLR).
 *
 * Models the per-bank "main" interrupt controller used by the bootloader:
 * device interrupts feed the ISR, the CPU IER masks them, and CPU_IEP_CLASS
 * selects IRQ vs FIQ.  The gated result drives the CPU IRQ/FIQ lines.  The
 * COP register set is kept for register compatibility but its outputs are
 * unused (the AVP is halted after the boot hand-off).
 *
 * Logic adapted from the Tegra2 reference implementation by Dmitry Osipenko.
 */

#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "hw/intc/tegra30_ictlr.h"

enum {
    REG_VIRQ_CPU      = 0x00,
    REG_VIRQ_COP      = 0x04,
    REG_VFIQ_CPU      = 0x08,
    REG_VFIQ_COP      = 0x0C,
    REG_ISR           = 0x10,
    REG_FIR           = 0x14,
    REG_FIR_SET       = 0x18,
    REG_FIR_CLR       = 0x1C,
    REG_CPU_IER       = 0x20,
    REG_CPU_IER_SET   = 0x24,
    REG_CPU_IER_CLR   = 0x28,
    REG_CPU_IEP_CLASS = 0x2C,
    REG_COP_IER       = 0x30,
    REG_COP_IER_SET   = 0x34,
    REG_COP_IER_CLR   = 0x38,
    REG_COP_IEP_CLASS = 0x3C,
};

static void tegra30_ictlr_update(Tegra30IctlrState *s)
{
    int irq_level = 0, fiq_level = 0;

    for (int b = 0; b < TEGRA30_ICTLR_BANKS; b++) {
        uint32_t pending = (s->fir[b] | s->isr[b]) & s->cpu_ier[b];

        /* IEP_CLASS bit set => FIQ, clear => IRQ. */
        s->vfiq_cpu[b] = pending & s->cpu_iep_class[b];
        s->virq_cpu[b] = pending & ~s->cpu_iep_class[b];

        irq_level |= !!s->virq_cpu[b];
        fiq_level |= !!s->vfiq_cpu[b];
    }

    qemu_set_irq(s->cpu_irq, irq_level);
    qemu_set_irq(s->cpu_fiq, fiq_level);
}

static void tegra30_ictlr_set_irq(void *opaque, int irq, int level)
{
    Tegra30IctlrState *s = opaque;
    int bank = irq / TEGRA30_ICTLR_IRQS_PER_BANK;
    uint32_t mask = 1u << (irq % TEGRA30_ICTLR_IRQS_PER_BANK);

    if (level) {
        s->isr[bank] |= mask;
    } else {
        s->isr[bank] &= ~mask;
    }

    tegra30_ictlr_update(s);
}

static uint64_t tegra30_ictlr_read(void *opaque, hwaddr offset, unsigned size)
{
    Tegra30IctlrState *s = TEGRA30_ICTLR(opaque);
    int bank = offset / TEGRA30_ICTLR_BANK_SIZE;
    int reg = offset % TEGRA30_ICTLR_BANK_SIZE;

    if (bank >= TEGRA30_ICTLR_BANKS) {
        return 0;
    }

    switch (reg) {
    case REG_VIRQ_CPU:      return s->virq_cpu[bank];
    case REG_VFIQ_CPU:      return s->vfiq_cpu[bank];
    case REG_ISR:           return s->isr[bank];
    case REG_FIR:           return s->fir[bank];
    case REG_CPU_IER:       return s->cpu_ier[bank];
    case REG_CPU_IEP_CLASS: return s->cpu_iep_class[bank];
    case REG_COP_IER:       return s->cop_ier[bank];
    case REG_COP_IEP_CLASS: return s->cop_iep_class[bank];
    default:                return 0;
    }
}

static void tegra30_ictlr_write(void *opaque, hwaddr offset,
                                uint64_t value, unsigned size)
{
    Tegra30IctlrState *s = TEGRA30_ICTLR(opaque);
    int bank = offset / TEGRA30_ICTLR_BANK_SIZE;
    int reg = offset % TEGRA30_ICTLR_BANK_SIZE;

    if (bank >= TEGRA30_ICTLR_BANKS) {
        return;
    }

    switch (reg) {
    case REG_FIR_SET:       s->fir[bank] |= value; break;
    case REG_FIR_CLR:       s->fir[bank] &= ~value; break;
    case REG_CPU_IER_SET:   s->cpu_ier[bank] |= value; break;
    case REG_CPU_IER_CLR:   s->cpu_ier[bank] &= ~value; break;
    case REG_CPU_IEP_CLASS: s->cpu_iep_class[bank] = value; break;
    case REG_COP_IER_SET:   s->cop_ier[bank] |= value; break;
    case REG_COP_IER_CLR:   s->cop_ier[bank] &= ~value; break;
    case REG_COP_IEP_CLASS: s->cop_iep_class[bank] = value; break;
    default:                return;
    }

    tegra30_ictlr_update(s);
}

static const MemoryRegionOps tegra30_ictlr_ops = {
    .read = tegra30_ictlr_read,
    .write = tegra30_ictlr_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
    .impl.min_access_size = 4,
};

static void tegra30_ictlr_reset(DeviceState *dev)
{
    Tegra30IctlrState *s = TEGRA30_ICTLR(dev);

    memset(s->virq_cpu, 0, sizeof(s->virq_cpu));
    memset(s->vfiq_cpu, 0, sizeof(s->vfiq_cpu));
    memset(s->isr, 0, sizeof(s->isr));
    memset(s->fir, 0, sizeof(s->fir));
    memset(s->cpu_ier, 0, sizeof(s->cpu_ier));
    memset(s->cpu_iep_class, 0, sizeof(s->cpu_iep_class));
    memset(s->cop_ier, 0, sizeof(s->cop_ier));
    memset(s->cop_iep_class, 0, sizeof(s->cop_iep_class));
}

static void tegra30_ictlr_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    Tegra30IctlrState *s = TEGRA30_ICTLR(obj);

    memory_region_init_io(&s->iomem, obj, &tegra30_ictlr_ops, s,
                          TYPE_TEGRA30_ICTLR, TEGRA30_ICTLR_IOSIZE);
    sysbus_init_mmio(sbd, &s->iomem);

    qdev_init_gpio_in(DEVICE(obj), tegra30_ictlr_set_irq,
                      TEGRA30_ICTLR_NUM_IRQS);
    sysbus_init_irq(sbd, &s->cpu_irq);
    sysbus_init_irq(sbd, &s->cpu_fiq);
}

static const VMStateDescription tegra30_ictlr_vmstate = {
    .name = "tegra30-ictlr",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(virq_cpu, Tegra30IctlrState, TEGRA30_ICTLR_BANKS),
        VMSTATE_UINT32_ARRAY(vfiq_cpu, Tegra30IctlrState, TEGRA30_ICTLR_BANKS),
        VMSTATE_UINT32_ARRAY(isr, Tegra30IctlrState, TEGRA30_ICTLR_BANKS),
        VMSTATE_UINT32_ARRAY(fir, Tegra30IctlrState, TEGRA30_ICTLR_BANKS),
        VMSTATE_UINT32_ARRAY(cpu_ier, Tegra30IctlrState, TEGRA30_ICTLR_BANKS),
        VMSTATE_UINT32_ARRAY(cpu_iep_class, Tegra30IctlrState,
                             TEGRA30_ICTLR_BANKS),
        VMSTATE_UINT32_ARRAY(cop_ier, Tegra30IctlrState, TEGRA30_ICTLR_BANKS),
        VMSTATE_UINT32_ARRAY(cop_iep_class, Tegra30IctlrState,
                             TEGRA30_ICTLR_BANKS),
        VMSTATE_END_OF_LIST()
    }
};

static void tegra30_ictlr_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, tegra30_ictlr_reset);
    dc->vmsd = &tegra30_ictlr_vmstate;
}

static const TypeInfo tegra30_ictlr_info = {
    .name          = TYPE_TEGRA30_ICTLR,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_init = tegra30_ictlr_init,
    .instance_size = sizeof(Tegra30IctlrState),
    .class_init    = tegra30_ictlr_class_init,
};

static void tegra30_ictlr_register(void)
{
    type_register_static(&tegra30_ictlr_info);
}

type_init(tegra30_ictlr_register)
