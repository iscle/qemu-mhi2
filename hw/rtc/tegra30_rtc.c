/*
 * Tegra30 RTC seconds counter and alarm interrupts.
 *
 * Register semantics follow the Tegra30 TRM and Linux rtc-tegra.c.
 * Millisecond/countdown alarms are not implemented.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "qemu/cutils.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "system/rtc.h"
#include "system/system.h"

#define TYPE_TEGRA30_RTC "tegra30-rtc"
OBJECT_DECLARE_SIMPLE_TYPE(Tegra30RTCState, TEGRA30_RTC)

#define RTC_SECONDS         0x08
#define RTC_SHADOW_SECONDS  0x0c
#define RTC_MILLISECONDS    0x10
#define RTC_SECONDS_ALARM0  0x14
#define RTC_SECONDS_ALARM1  0x18
#define RTC_INTR_MASK       0x28
#define RTC_INTR_STATUS     0x2c

struct Tegra30RTCState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq irq;
    QEMUTimer *timer;
    int64_t offset_ms;
    int64_t next_alarm_ms;
    uint32_t shadow_seconds;
    uint32_t alarm[2];
    uint32_t mask;
    uint32_t status;
};

static int64_t rtc_time_ms(Tegra30RTCState *s)
{
    return qemu_clock_get_ms(rtc_clock) + s->offset_ms;
}

static void rtc_update(Tegra30RTCState *s)
{
    int64_t now = rtc_time_ms(s);
    uint32_t seconds = now / 1000;
    int64_t next = INT64_MAX;
    unsigned i;

    qemu_set_irq(s->irq, !!(s->status & s->mask));
    for (i = 0; i < ARRAY_SIZE(s->alarm); i++) {
        if ((s->mask & BIT(i)) && !(s->status & BIT(i))) {
            uint32_t delta = s->alarm[i] - seconds;

            /* Unsigned subtraction retains the hardware's 32-bit wrap. */
            next = MIN(next, now - now % 1000 + (int64_t)delta * 1000);
        }
    }
    s->next_alarm_ms = next;
    if (next == INT64_MAX) {
        timer_del(s->timer);
    } else {
        timer_mod(s->timer, next - s->offset_ms);
    }
}

static void rtc_alarm(void *opaque)
{
    Tegra30RTCState *s = opaque;
    uint32_t seconds = rtc_time_ms(s) / 1000;
    uint32_t first = s->next_alarm_ms / 1000;
    unsigned i;

    for (i = 0; i < ARRAY_SIZE(s->alarm); i++) {
        /* Timer callbacks can run after the matching second has passed. */
        if ((s->mask & BIT(i)) &&
            (uint32_t)(s->alarm[i] - first) <= (uint32_t)(seconds - first)) {
            s->status |= BIT(i);
        }
    }
    rtc_update(s);
}

static uint64_t rtc_read(void *opaque, hwaddr addr, unsigned size)
{
    Tegra30RTCState *s = opaque;
    int64_t now = rtc_time_ms(s);

    switch (addr) {
    case RTC_SECONDS:
        return (uint32_t)(now / 1000);
    case RTC_MILLISECONDS:
        s->shadow_seconds = now / 1000;
        return now % 1000;
    case RTC_SHADOW_SECONDS:
        return s->shadow_seconds;
    case RTC_SECONDS_ALARM0:
    case RTC_SECONDS_ALARM1:
        return s->alarm[(addr - RTC_SECONDS_ALARM0) / 4];
    case RTC_INTR_MASK:
        return s->mask;
    case RTC_INTR_STATUS:
        return s->status;
    default:
        return 0;
    }
}

static void rtc_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    Tegra30RTCState *s = opaque;

    switch (addr) {
    case RTC_SECONDS:
        s->offset_ms = (int64_t)(uint32_t)value * 1000 -
                       qemu_clock_get_ms(rtc_clock);
        break;
    case RTC_SECONDS_ALARM0:
    case RTC_SECONDS_ALARM1:
        s->alarm[(addr - RTC_SECONDS_ALARM0) / 4] = value;
        break;
    case RTC_INTR_MASK:
        s->mask = value & 3;
        break;
    case RTC_INTR_STATUS:
        s->status &= ~value;
        break;
    default:
        return;
    }
    rtc_update(s);
}

static const MemoryRegionOps rtc_ops = {
    .read = rtc_read,
    .write = rtc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static int rtc_post_load(void *opaque, int version_id)
{
    Tegra30RTCState *s = opaque;

    qemu_set_irq(s->irq, !!(s->status & s->mask));
    if (s->next_alarm_ms == INT64_MAX) {
        timer_del(s->timer);
    } else {
        timer_mod(s->timer, s->next_alarm_ms - s->offset_ms);
    }
    return 0;
}

static const VMStateDescription vmstate_tegra30_rtc = {
    .name = TYPE_TEGRA30_RTC,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = rtc_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_INT64(offset_ms, Tegra30RTCState),
        VMSTATE_INT64(next_alarm_ms, Tegra30RTCState),
        VMSTATE_UINT32(shadow_seconds, Tegra30RTCState),
        VMSTATE_UINT32_ARRAY(alarm, Tegra30RTCState, 2),
        VMSTATE_UINT32(mask, Tegra30RTCState),
        VMSTATE_UINT32(status, Tegra30RTCState),
        VMSTATE_END_OF_LIST()
    },
};

static void rtc_reset(DeviceState *dev)
{
    Tegra30RTCState *s = TEGRA30_RTC(dev);

    /* A warm reset does not reset the battery-backed time counter. */
    s->mask = s->status = 0;
    memset(s->alarm, 0, sizeof(s->alarm));
    rtc_update(s);
}

static void rtc_init(Object *obj)
{
    Tegra30RTCState *s = TEGRA30_RTC(obj);
    struct tm tm;

    qemu_get_timedate(&tm, 0);
    s->offset_ms = (int64_t)mktimegm(&tm) * 1000 - qemu_clock_get_ms(rtc_clock);
    s->timer = timer_new_ms(rtc_clock, rtc_alarm, s);
    memory_region_init_io(&s->mmio, obj, &rtc_ops, s, TYPE_TEGRA30_RTC, 0x100);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
}

static void rtc_finalize(Object *obj)
{
    timer_free(TEGRA30_RTC(obj)->timer);
}

static void rtc_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_tegra30_rtc;
    device_class_set_legacy_reset(dc, rtc_reset);
}

static const TypeInfo rtc_info = {
    .name = TYPE_TEGRA30_RTC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Tegra30RTCState),
    .instance_init = rtc_init,
    .instance_finalize = rtc_finalize,
    .class_init = rtc_class_init,
};

static void rtc_register_types(void)
{
    type_register_static(&rtc_info);
}

type_init(rtc_register_types)
