/* Tegra30 HOST1X syncpoints, display methods and limited linear GR2D copies.
 * Register layout: NVIDIA T30 arhost1x_sync.h and Linux host1x01_sync.h.
 */
#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "qemu/module.h"
#include "qemu/log.h"
#include "qemu/host-utils.h"
#include "system/address-spaces.h"
#include "migration/vmstate.h"
#include "hw/misc/tegra30_host1x.h"

static void host1x_update(Tegra30Host1xState *s)
{
    uint32_t pending = 0;
    for (unsigned id = 0; id < 32; id++) {
        if ((int32_t)(s->regs[(0x3400 + id * 4) / 4] -
                      s->regs[(0x3500 + id * 4) / 4]) >= 0) {
            pending |= 1u << id;
        }
    }
    s->regs[0x3040 / 4] |= pending & s->enabled;
    qemu_set_irq(s->irq, !!(s->regs[0x3040 / 4] & s->enabled));
}

void tegra30_host1x_increment(Tegra30Host1xState *s, unsigned id)
{
    if (s && id < 32) {
        s->regs[(0x3400 + id * 4) / 4]++;
        host1x_update(s);
    }
}

static bool host1x_fetch(Tegra30Host1xState *s, uint32_t iova, uint32_t *word)
{
    hwaddr pa;
    MemTxResult result;
    if (!tegra30_mc_translate(s->mc, 0x250, iova, &pa)) {
        return false;
    }
    *word = address_space_ldl_le(&address_space_memory, pa,
                                 MEMTXATTRS_UNSPECIFIED, &result);
    return result == MEMTX_OK;
}

/* GR2D register layout follows the T20/T30 G2SB register interface.
 * Only linear, identity stretch blits are implemented at present.
 */
static bool host1x_gr2d_copy(Tegra30Host1xState *s, uint32_t *r)
{
    unsigned format = r[0x1c] & 31;
    unsigned bpp = format == 8 || format == 12 ? 2 : 4;
    unsigned width = r[0x38] & 0x7fff;
    unsigned height = ((r[0x38] >> 16) & 0x7fff) + 1;
    unsigned src_stride = r[0x33] & 0xffff;
    unsigned dst_stride = r[0x2e] & 0xffff;
    uint32_t src = r[0x31], dst = r[0x2b];
    uint8_t pixel[4];

    if ((format != 8 && format != 12 && format != 14 && format != 15) ||
        ((r[0x1c] >> 8) & 31) != format || r[0x46] ||
        r[0x11] != 0x1000 || r[0x13] != 0x1000 ||
        r[0x12] || r[0x14] || r[0x39] || r[0x3a] ||
        r[0x37] != r[0x38] || (r[0x1d] & 0x18082038) ||
        width > 4096 || height > 4096 ||
        width * bpp > src_stride || width * bpp > dst_stride) {
        qemu_log_mask(LOG_UNIMP, "host1x: unsupported GR2D stretch blit\n");
        return false;
    }
    for (unsigned y = 0; y < height; y++) {
        for (unsigned x = 0; x < width * bpp; x++) {
            hwaddr sp, dp;
            /* Translate per byte, including non-contiguous SMMU pages. */
            if (!tegra30_mc_translate(s->mc, 0x24c, src + y * src_stride + x, &sp) ||
                !tegra30_mc_translate(s->mc, 0x24c, dst + y * dst_stride + x, &dp) ||
                address_space_read(&address_space_memory, sp,
                    MEMTXATTRS_UNSPECIFIED, pixel, 1) != MEMTX_OK ||
                address_space_write(&address_space_memory, dp,
                    MEMTXATTRS_UNSPECIFIED, pixel, 1) != MEMTX_OK) {
                return false;
            }
        }
    }
    return true;
}

static bool host1x_gr2d_method(Tegra30Host1xState *s, unsigned cls,
                               unsigned method, uint32_t value)
{
    unsigned context = cls >= 0x58 ? 2 : cls >= 0x54 ? 1 : 0;
    uint32_t *r = s->gr2d[context];
    if (method >= 0x80) {
        return false;
    }
    if (!method) {
        if ((value & 0xff) >= 32 || ((value >> 8) & 0xff) > 1) {
            return false;
        }
        /* Work is executed synchronously before OP_DONE is reached. */
        tegra30_host1x_increment(s, value & 0xff);
        return true;
    }
    r[method] = value;
    if (method > 0xb && ((r[9] & 0x7f) == method ||
                        (r[10] & 0x7f) == method ||
                        (r[11] & 0x7f) == method)) {
        if (cls == 0x52 || cls == 0x56 || cls == 0x5a) {
            return host1x_gr2d_copy(s, r);
        }
        qemu_log_mask(LOG_UNIMP, "host1x: unsupported GR2D operation\n");
        return false;
    }
    return true;
}

static bool host1x_method(Tegra30Host1xState *s, unsigned ch,
                          unsigned method, uint32_t value)
{
    unsigned cls = s->channel_class[ch];
    if (getenv("TEGRA_DC_DEBUG")) {
        fprintf(stderr, "host1x method class=%03x reg=%03x value=%08x\n", cls, method, value);
    }
    if (cls == 0x51 || cls == 0x52 || cls == 0x55 ||
        cls == 0x56 || cls == 0x59 || cls == 0x5a) {
        return host1x_gr2d_method(s, cls, method, value);
    }
    if (cls == 0x70 || cls == 0x71) {
        hwaddr base = cls == 0x70 ? 0x54200000 : 0x54240000;
        address_space_stl_le(&address_space_memory, base + method * 4,
                             value, MEMTXATTRS_UNSPECIFIED, NULL);
        return true;
    }
    if (cls == 1 && method == 0) {
        tegra30_host1x_increment(s, value & 0xff);
        return true;
    }
    if (cls == 1 && method == 8) {
        unsigned id = value >> 24;
        return id < 32 && (int32_t)(s->regs[(0x3400 + id * 4) / 4] -
                                    (value & 0xffffff)) >= 0;
    }
    if (cls == 1 && (method == 0xb || method == 0xc)) {
        unsigned id = value >> 24;
        if (id >= 8) {
            /* QNX emits base 0xff for jobs without an assigned wait base. */
            return true;
        }
        if (method == 0xb) {
            s->regs[(0x3600 + id * 4) / 4] = value & 0xffffff;
        } else {
            s->regs[(0x3600 + id * 4) / 4] += value & 0xffffff;
        }
        return true;
    }
    qemu_log_mask(LOG_UNIMP, "host1x: unsupported class %x method %x\n", cls, method);
    return false;
}

static bool host1x_stream(Tegra30Host1xState *s, unsigned ch,
                          uint32_t *get, uint32_t end, unsigned depth,
                          unsigned *budget)
{
    while (*get != end && *budget) {
        uint32_t op, value, next;
        unsigned method, count, mask = 0, code;
        (*budget)--;
        if (!host1x_fetch(s, *get, &op)) {
            return false;
        }
        next = *get + 4;
        method = (op >> 16) & 0xfff;
        count = op & 0xffff;
        code = op >> 28;
        if (getenv("TEGRA_DC_DEBUG")) {
            fprintf(stderr, "host1x ch%u get=%08x op=%08x\n", ch, *get, op);
        }
        switch (code) {
        case 0:
            s->channel_class[ch] = (op >> 6) & 0x3ff;
            mask = op & 0x3f;
            count = ctpop32(mask);
            break;
        case 1:
        case 2:
            break;
        case 3:
            mask = count;
            count = ctpop32(mask);
            break;
        case 4:
            if (!host1x_method(s, ch, method, op & 0xffff)) {
                return false;
            }
            count = 0;
            break;
        case 5:
            *get = (op & 0x0fffffff) << 4;
            continue;
        case 6: {
            uint32_t gather;
            if (depth >= 4 || (op & 0xffffc000) != 0x60000000 ||
                !host1x_fetch(s, next, &gather)) {
                return false;
            }
            if (getenv("TEGRA_DC_DEBUG")) {
                for (unsigned i = 0; i < MIN(count, 256); i++) {
                    uint32_t word;
                    if (!host1x_fetch(s, gather + i * 4, &word)) {
                        break;
                    }
                    fprintf(stderr, "gather ch%u %08x: %08x\n", ch,
                            gather + i * 4, word);
                }
            }
            if (!host1x_stream(s, ch, &gather, gather + count * 4, depth + 1, budget)) {
                return false;
            }
            next += 4;
            count = 0;
            break;
        }
        case 14: {
            unsigned lock = op & 0xff;
            unsigned subop = (op >> 24) & 15;
            if (lock >= 16 || subop > 1) {
                return false;
            }
            uint32_t *owner = &s->regs[(0x3340 + lock * 4) / 4];
            if (!subop) {
                if (*owner && *owner != (1u | (ch << 8))) {
                    return false;
                }
                *owner = 1u | (ch << 8);
            } else {
                *owner = 0;
            }
            count = 0;
            break;
        }
        default:
            qemu_log_mask(LOG_UNIMP, "host1x: unsupported opcode %08x\n", op);
            return false;
        }
        while (count--) {
            if (!*budget) {
                return false;
            }
            (*budget)--;
            if (next == end || !host1x_fetch(s, next, &value)) {
                return false;
            }
            unsigned reg = method;
            if (mask) {
                reg += ctz32(mask);
                mask &= mask - 1;
            }
            if (!host1x_method(s, ch, reg, value)) {
                return false;
            }
            if (code == 1) {
                method++;
            }
            next += 4;
        }
        *get = next;
    }
    return *get == end;
}

static void host1x_channel_run(Tegra30Host1xState *s, unsigned ch)
{
    unsigned base = ch * 0x4000;
    unsigned budget = 4096;
    if (!(s->regs[(base + 0x24) / 4] & 1)) {
        host1x_stream(s, ch, &s->regs[(base + 0x1c) / 4],
                     s->regs[(base + 0x18) / 4], 0, &budget);
    }
}

static uint64_t host1x_read(void *opaque, hwaddr offset, unsigned size)
{
    Tegra30Host1xState *s = opaque;
    return s->regs[offset / 4];
}

static void host1x_write(void *opaque, hwaddr offset, uint64_t value,
                         unsigned size)
{
    Tegra30Host1xState *s = opaque;
    if (getenv("TEGRA_DC_DEBUG")) {
        fprintf(stderr, "host1x %04x <- %08x\n", (unsigned)offset, (unsigned)value);
    }
    if (offset / 0x4000 < 8 && offset % 0x4000 < 0x1000) {
        unsigned ch = offset / 0x4000;
        unsigned reg = offset % 0x4000;
        s->regs[offset / 4] = value;
        if (reg == 0x24 && (value & 4)) {
            s->regs[(ch * 0x4000 + 0x1c) / 4] = s->regs[(ch * 0x4000 + 0x18) / 4];
        }
        if (reg == 0x24 || reg == 0x18) {
            host1x_channel_run(s, ch);
        }
        return;
    }
    switch (offset) {
    case 0x3040:
        s->regs[offset / 4] &= ~value;
        break;
    case 0x3060:
        s->enabled &= ~value;
        break;
    case 0x3068:
        s->enabled |= value;
        break;
    case 0x3700:
        for (unsigned id = 0; id < 32; id++) {
            if (value & (1u << id)) {
                tegra30_host1x_increment(s, id);
            }
        }
        break;
    default:
        s->regs[offset / 4] = value;
        break;
    }
    host1x_update(s);
}

static const MemoryRegionOps host1x_ops = {
    .read = host1x_read,
    .write = host1x_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void host1x_reset(DeviceState *dev)
{
    Tegra30Host1xState *s = TEGRA30_HOST1X(dev);
    memset(s->regs, 0, sizeof(s->regs));
    s->enabled = 0;
    memset(s->gr2d, 0, sizeof(s->gr2d));
    memset(s->channel_class, 0, sizeof(s->channel_class));
    qemu_set_irq(s->irq, 0);
}

static void host1x_init(Object *obj)
{
    Tegra30Host1xState *s = TEGRA30_HOST1X(obj);
    memory_region_init_io(&s->iomem, obj, &host1x_ops, s,
                          TYPE_TEGRA30_HOST1X, 0x24000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static const VMStateDescription host1x_vmstate = {
    .name = TYPE_TEGRA30_HOST1X,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, Tegra30Host1xState, 0x24000 / 4),
        VMSTATE_UINT32(enabled, Tegra30Host1xState),
        VMSTATE_UINT32_2DARRAY(gr2d, Tegra30Host1xState, 3, 0x80),
        VMSTATE_UINT32_ARRAY(channel_class, Tegra30Host1xState, 8),
        VMSTATE_END_OF_LIST()
    }
};

static void host1x_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->vmsd = &host1x_vmstate;
    device_class_set_legacy_reset(dc, host1x_reset);
}

static const TypeInfo host1x_info = {
    .name = TYPE_TEGRA30_HOST1X,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Tegra30Host1xState),
    .instance_init = host1x_init,
    .class_init = host1x_class_init,
};
static void host1x_register_types(void)
{
    type_register_static(&host1x_info);
}
type_init(host1x_register_types)
