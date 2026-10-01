/* Tegra30 APB DMA: register layout from Linux tegra20-apb-dma.c and the
 * original MHI2 nvtdm driver. Local hardware-emulation experiment. */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/log.h"
#include "qemu/bswap.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/tegra30_mc.h"
#include "system/address-spaces.h"
#include "migration/vmstate.h"

#define TYPE_TEGRA30_APBDMA "tegra30-apbdma"
OBJECT_DECLARE_SIMPLE_TYPE(Tegra30APBDMAState, TEGRA30_APBDMA)
#define EN (1u << 31)
#define EOC (1u << 30)
#define ONCE (1u << 27)
#define PONG (1u << 28)
#define DIR (1u << 28)

typedef struct DMAChannel {
    Tegra30APBDMAState *owner;
    QEMUTimer *timer;
    qemu_irq irq;
    uint32_t r[8];
    uint32_t active_ptr;
    unsigned index;
} DMAChannel;

struct Tegra30APBDMAState {
    SysBusDevice parent_obj;
    Tegra30MCState *mc;
    MemoryRegion iomem, ahub;
    uint32_t ahub_regs[1024];
    uint16_t mic[8192], playback[8192];
    uint32_t mic_read, mic_count, play_read, play_count;
    uint32_t global[16];
    DMAChannel channel[32];
};

static void dma_irq(DMAChannel *c)
{
    qemu_set_irq(c->irq, !!((c->r[1] & EOC) && (c->r[0] & EOC) &&
                 (c->owner->global[7] & (1u << c->index))));
}

static bool dma_running(DMAChannel *c)
{
    return (c->owner->global[0] & EN) && (c->r[0] & EN) && !(c->r[3] & EN);
}

static void dma_schedule(DMAChannel *c)
{
    if (!dma_running(c)) {
        timer_del(c->timer);
        c->r[1] &= ~EN;
        return;
    }
    c->r[1] |= EN;
    if (!timer_pending(c->timer)) {
        /* The attached MHI2 TDM link is 48 kHz, eight 32-bit slots/frame.
         * A continuous transfer interrupts at each half-buffer boundary. */
        unsigned bytes = (c->r[0] & 0xfffc) + 4;
        timer_mod(c->timer, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                  MAX(1000ULL, (uint64_t)bytes * 1000000000 / (48000 * 32)));
    }
}

static bool dma_memory(DMAChannel *c, uint8_t *buffer, unsigned bytes, bool write)
{
    for (unsigned done = 0; done < bytes;) {
        uint32_t va = c->active_ptr + done;
        unsigned n = MIN(bytes - done, 4096 - (va & 4095));
        hwaddr pa;
        if (!c->owner->mc || !tegra30_mc_translate(c->owner->mc, 0x270, va, &pa)) {
            return false;
        }
        MemTxResult result = write ?
            address_space_write(&address_space_memory, pa, MEMTXATTRS_UNSPECIFIED,
                                buffer + done, n) :
            address_space_read(&address_space_memory, pa, MEMTXATTRS_UNSPECIFIED,
                               buffer + done, n);
        if (result != MEMTX_OK) { return false; }
        done += n;
    }
    return true;
}

static void dma_complete(void *opaque)
{
    DMAChannel *c = opaque;
    uint32_t csr = c->r[0], apb = c->r[6];
    unsigned bytes = (csr & 0xfffc) + 4;
    uint8_t buffer[65536];
    if (!dma_running(c)) {
        return;
    }
    /* Until an APB peripheral is attached, requests must remain pending.
     * The only attached stream here is the MHI2 AHUB TDM FIFO aperture. */
    if (apb < 0x70080000 || apb >= 0x70080080 ||
        (apb & 31) != ((csr & DIR) ? 0x0c : 0x10)) {
        return;
    }
    if (csr & DIR) { /* AHB -> APB */
        if (!dma_memory(c, buffer, bytes, false)) {
            return;
        }
        /* TDM1 announcement slots: native NvAudioMuxStreamsTDM1 places
         * ANN1/ANN2 in the final 32-bit word of each eight-slot frame.
         * The host companion reads this downstream tap, never guest queues. */
        if (apb == 0x7008000c) {
            for (unsigned i = 0; i + 32 <= bytes; i += 32) {
                int sample = (int16_t)lduw_le_p(buffer + i + 28) +
                             (int16_t)lduw_le_p(buffer + i + 30);
                Tegra30APBDMAState *s = c->owner;
                if (s->play_count < 8192) {
                    s->playback[(s->play_read + s->play_count++) % 8192] =
                        MIN(32767, MAX(-32768, sample));
                }
            }
        }
    } else {
        memset(buffer, 0, bytes);
        /* Digital microphone input to the RCC TDM return stream. Layout from
         * native NvAudioDemuxStreamsTDM1: mono at byte 18, six-channel slot 0
         * at byte 22. Empty host input is silence. */
        for (unsigned i = 0; i + 32 <= bytes; i += 32) {
            Tegra30APBDMAState *s = c->owner;
            uint16_t sample = 0;
            if (s->mic_count) {
                sample = s->mic[s->mic_read++ % 8192];
                s->mic_count--;
            }
            stw_le_p(buffer + i + 18, sample);
            stw_le_p(buffer + i + 22, sample);
        }
        if (!dma_memory(c, buffer, bytes, true)) {
            return;
        }
    }
    c->r[1] |= EOC;
    if (csr & ONCE) {
        c->r[0] &= ~EN;
        c->r[1] &= ~EN;
    } else {
        c->r[1] ^= PONG;
        c->active_ptr = c->r[4] + ((c->r[1] & PONG) ? bytes : 0);
    }
    dma_irq(c);
    dma_schedule(c);
}

static uint64_t dma_read(void *opaque, hwaddr off, unsigned size)
{
    Tegra30APBDMAState *s = opaque;
    if (off < sizeof(s->global)) {
        if (off == 0x14 || off == 0x18) {
            uint32_t status = 0;
            for (unsigned i = 0; i < 32; i++) {
                if (s->channel[i].r[1] & EOC) {
                    status |= 1u << i;
                }
            }
            return off == 0x18 ? status & s->global[7] : status;
        }
        return s->global[off / 4];
    }
    if (off >= 0x1000 && off < 0x1400) {
        return s->channel[(off - 0x1000) / 32].r[(off & 31) / 4];
    }
    return 0;
}

static void dma_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    Tegra30APBDMAState *s = opaque;
    if (off < sizeof(s->global)) {
        if (off == 0x20) {
            s->global[7] |= value;
        } else if (off == 0x24) {
            s->global[7] &= ~value;
        } else if (off != 0x14 && off != 0x18) {
            s->global[off / 4] = value;
        }
        for (unsigned i = 0; i < 32; i++) {
            dma_irq(&s->channel[i]);
            dma_schedule(&s->channel[i]);
        }
    } else if (off >= 0x1000 && off < 0x1400) {
        DMAChannel *c = &s->channel[(off - 0x1000) / 32];
        unsigned reg = (off & 31) / 4;
        if (reg == 1) {
            c->r[1] &= ~(value & EOC);
        } else {
            if (reg == 0 && !(c->r[0] & EN) && (value & EN)) {
                c->active_ptr = c->r[4];
                c->r[1] &= ~PONG;
            }
            c->r[reg] = value;
        }
        dma_irq(c);
        dma_schedule(c);
    }
}

static const MemoryRegionOps dma_ops = {
    .read = dma_read, .write = dma_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

/* Explicit emulator/host-bridge mailbox in an unused AHUB aperture. The
 * production driver never accesses these offsets; the added PCM endpoint does.
 * This keeps the original driver's DMA/queue ownership intact. */
static uint64_t ahub_read(void *opaque, hwaddr off, unsigned size)
{
    Tegra30APBDMAState *s = opaque;
    switch (off) {
    case 0xe00: return 0x4d484941;
    case 0xe04: return s->play_count;
    case 0xe08:
        if (!s->play_count) { return 0; }
        s->play_count--;
        return s->playback[s->play_read++ % 8192];
    case 0xe0c: return 8192 - s->mic_count;
    default: return s->ahub_regs[off / 4];
    }
}
static void ahub_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    Tegra30APBDMAState *s = opaque;
    if (off == 0xe10) {
        if (s->mic_count < 8192) {
            s->mic[(s->mic_read + s->mic_count++) % 8192] = value;
        }
    } else if (off < 0xe00) {
        s->ahub_regs[off / 4] = value;
    }
}
static const MemoryRegionOps ahub_ops = {
    .read = ahub_read, .write = ahub_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void dma_reset(DeviceState *dev)
{
    Tegra30APBDMAState *s = TEGRA30_APBDMA(dev);
    memset(s->global, 0, sizeof(s->global));
    memset(s->ahub_regs, 0, sizeof(s->ahub_regs));
    s->mic_read = s->mic_count = s->play_read = s->play_count = 0;
    for (unsigned i = 0; i < 32; i++) {
        DMAChannel *c = &s->channel[i];
        memset(c->r, 0, sizeof(c->r));
        c->active_ptr = 0;
        timer_del(c->timer);
        dma_irq(c);
    }
}

static void dma_init(Object *obj)
{
    Tegra30APBDMAState *s = TEGRA30_APBDMA(obj);
    memory_region_init_io(&s->iomem, obj, &dma_ops, s, TYPE_TEGRA30_APBDMA, 0x2000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->iomem);
    memory_region_init_io(&s->ahub, obj, &ahub_ops, s, "tegra30-ahub", 0x1000);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->ahub);
    for (unsigned i = 0; i < 32; i++) {
        DMAChannel *c = &s->channel[i];
        c->owner = s;
        c->index = i;
        c->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, dma_complete, c);
        sysbus_init_irq(SYS_BUS_DEVICE(s), &c->irq);
    }
}

static void dma_finalize(Object *obj)
{
    Tegra30APBDMAState *s = TEGRA30_APBDMA(obj);
    for (unsigned i = 0; i < 32; i++) {
        timer_free(s->channel[i].timer);
    }
}

static const VMStateDescription vmstate_dma_channel = {
    .name = "tegra30-apbdma/channel",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(r, DMAChannel, 8),
        VMSTATE_UINT32(active_ptr, DMAChannel),
        VMSTATE_TIMER_PTR(timer, DMAChannel),
        VMSTATE_END_OF_LIST()
    },
};
static int dma_post_load(void *opaque, int version_id)
{
    Tegra30APBDMAState *s = opaque;
    if (s->mic_count > 8192 || s->play_count > 8192) {
        return -EINVAL;
    }
    for (unsigned i = 0; i < 32; i++) {
        dma_irq(&s->channel[i]);
    }
    return 0;
}
static const VMStateDescription vmstate_dma = {
    .name = TYPE_TEGRA30_APBDMA,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = dma_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(global, Tegra30APBDMAState, 16),
        VMSTATE_UINT32_ARRAY(ahub_regs, Tegra30APBDMAState, 1024),
        VMSTATE_UINT16_ARRAY(mic, Tegra30APBDMAState, 8192),
        VMSTATE_UINT16_ARRAY(playback, Tegra30APBDMAState, 8192),
        VMSTATE_UINT32(mic_read, Tegra30APBDMAState),
        VMSTATE_UINT32(mic_count, Tegra30APBDMAState),
        VMSTATE_UINT32(play_read, Tegra30APBDMAState),
        VMSTATE_UINT32(play_count, Tegra30APBDMAState),
        VMSTATE_STRUCT_ARRAY(channel, Tegra30APBDMAState, 32, 1,
                             vmstate_dma_channel, DMAChannel),
        VMSTATE_END_OF_LIST()
    },
};

static const Property dma_properties[] = {
    DEFINE_PROP_LINK("mc", Tegra30APBDMAState, mc, TYPE_TEGRA30_MC, Tegra30MCState *),
};

static void dma_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    device_class_set_legacy_reset(dc, dma_reset);
    device_class_set_props(dc, dma_properties);
    dc->vmsd = &vmstate_dma;
    dc->user_creatable = false;
    dc->desc = "Tegra30 APB DMA with MHI2 TDM endpoint";
}

static const TypeInfo dma_type = {
    .name = TYPE_TEGRA30_APBDMA,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Tegra30APBDMAState),
    .instance_init = dma_init,
    .instance_finalize = dma_finalize,
    .class_init = dma_class_init,
};
static void dma_register_types(void) { type_register_static(&dma_type); }
type_init(dma_register_types)
