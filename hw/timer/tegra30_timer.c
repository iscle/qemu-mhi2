/* Local MHI2 Linux bring-up model. Tegra3 TRM chapter 7: six 1 MHz
 * countdown timers; PTV uses n+1 periods and PCR[30] is interrupt W1C. */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "hw/timer/tegra30_timer.h"

static const unsigned offsets[6] = { 0x00, 0x08, 0x50, 0x58, 0x60, 0x68 };

static int channel_for(hwaddr offset)
{
    for (int i = 0; i < 6; i++) {
        if (offset == offsets[i] || offset == offsets[i] + 4) {
            return i;
        }
    }
    return -1;
}

static void tegra_countdown_expired(void *opaque)
{
    Tegra30TimerChannel *c = opaque;
    c->pending = true;
    qemu_set_irq(c->irq, 1);
}

static uint64_t tegra30_timer_read(void *opaque, hwaddr offset, unsigned size)
{
    Tegra30TimerState *s = opaque;
    int i = channel_for(offset);
    if (offset == 0x10) {
        return (uint32_t)(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) / 1000);
    }
    if (i >= 0 && offset == offsets[i] + 4) {
        uint64_t count = ptimer_get_count(s->channel[i].counter);
        return count ? (count - 1) & 0x1fffffff : 0;
    }
    return s->regs[offset / 4];
}

static void tegra30_timer_write(void *opaque, hwaddr offset,
                               uint64_t value, unsigned size)
{
    Tegra30TimerState *s = opaque;
    int i = channel_for(offset);
    if (i >= 0) {
        Tegra30TimerChannel *c = &s->channel[i];
        if (offset == offsets[i] + 4) {
            if (value & (1u << 30)) {
                c->pending = false;
                qemu_set_irq(c->irq, 0);
            }
            return;
        }
        ptimer_transaction_begin(c->counter);
        ptimer_stop(c->counter);
        ptimer_set_limit(c->counter, (value & 0x1fffffff) + 1, 1);
        if (value & (1u << 31)) {
            ptimer_run(c->counter, !(value & (1u << 30)));
        }
        ptimer_transaction_commit(c->counter);
    }
    s->regs[offset / 4] = value;
}

static const MemoryRegionOps timer_ops = {
    .read = tegra30_timer_read, .write = tegra30_timer_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void timer_reset(DeviceState *dev)
{
    Tegra30TimerState *s = TEGRA30_TIMER(dev);
    memset(s->regs, 0, sizeof(s->regs));
    s->regs[0x14 / 4] = 12;
    for (int i = 0; i < 6; i++) {
        Tegra30TimerChannel *c = &s->channel[i];
        ptimer_transaction_begin(c->counter);
        ptimer_stop(c->counter);
        ptimer_set_count(c->counter, 0);
        ptimer_transaction_commit(c->counter);
        c->pending = false;
        qemu_set_irq(c->irq, 0);
    }
}

static void tegra_countdown_init(Object *obj)
{
    Tegra30TimerState *s = TEGRA30_TIMER(obj);
    memory_region_init_io(&s->iomem, obj, &timer_ops, s,
                          TYPE_TEGRA30_TIMER, TEGRA30_TIMER_IOSIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    for (int i = 0; i < 6; i++) {
        Tegra30TimerChannel *c = &s->channel[i];
        sysbus_init_irq(SYS_BUS_DEVICE(s), &c->irq);
        c->counter = ptimer_init(tegra_countdown_expired, c, PTIMER_POLICY_NO_COUNTER_ROUND_DOWN);
        ptimer_transaction_begin(c->counter);
        ptimer_set_freq(c->counter, 1000000);
        ptimer_transaction_commit(c->counter);
    }
}

static void timer_finalize(Object *obj)
{
    Tegra30TimerState *s = TEGRA30_TIMER(obj);
    for (int i = 0; i < 6; i++) {
        ptimer_free(s->channel[i].counter);
    }
}

static int timer_post_load(void *opaque, int version)
{
    Tegra30TimerState *s = opaque;
    for (int i = 0; i < 6; i++) {
        qemu_set_irq(s->channel[i].irq, s->channel[i].pending);
    }
    return 0;
}
static const VMStateDescription channel_vmstate = {
    .name = "tegra30-timer/channel", .version_id = 1, .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PTIMER(counter, Tegra30TimerChannel),
        VMSTATE_BOOL(pending, Tegra30TimerChannel),
        VMSTATE_END_OF_LIST()
    },
};
static const VMStateDescription timer_vmstate = {
    .name = "tegra30-timer", .version_id = 2, .minimum_version_id = 2,
    .post_load = timer_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, Tegra30TimerState, TEGRA30_TIMER_REGS_NUM),
        VMSTATE_STRUCT_ARRAY(channel, Tegra30TimerState, 6, 0,
                             channel_vmstate, Tegra30TimerChannel),
        VMSTATE_END_OF_LIST()
    },
};
static void timer_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    device_class_set_legacy_reset(dc, timer_reset);
    dc->vmsd = &timer_vmstate;
}
static const TypeInfo timer_info = {
    .name = TYPE_TEGRA30_TIMER, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Tegra30TimerState), .instance_init = tegra_countdown_init,
    .instance_finalize = timer_finalize, .class_init = timer_class_init,
};
static void timer_register(void) { type_register_static(&timer_info); }
type_init(timer_register)
