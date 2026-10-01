/*
 * Local Tegra30 PCIe / MHI2 RCC endpoint bring-up model.
 * Register layout: Tegra30 TRM and Linux pci-tegra.c. Endpoint ABI recovered
 * from the firmware's devnp-mib-{mmx,rcc}.so and mmx-pcie-init.
 * Models enumeration, RCC shared descriptor rings, DMA and MSI transport.
 * The optional external Ethernet peer implements companion CPU services.
 */
#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "qemu/module.h"
#include "qemu/log.h"
#include "qemu/bswap.h"
#include "qemu/timer.h"
#include "net/net.h"
#include "system/address-spaces.h"
#include "chardev/char-fe.h"

#define TYPE_TEGRA30_PCIE "tegra30-pcie"
OBJECT_DECLARE_SIMPLE_TYPE(Tegra30PCIEState, TEGRA30_PCIE)

struct Tegra30PCIEState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq irq, msi_irq;
    uint32_t regs[0x5000 / 4];
    uint32_t config[256 / 4];
    uint32_t app[0x1000 / 4];
    uint32_t shared[0x4000 / 4];
    bool bar_probe[4];
    uint32_t most[0x1000 / 4];
    CharFrontend most_video;
    NICConf conf;
    NICState *nic;
    QEMUTimer *poll;
    unsigned tx_head, rx_head;
    bool link_notified;
    CharFrontend peer;
    uint8_t pending[65536];
    unsigned pending_len;
};

static int root_offset(hwaddr addr)
{
    if (addr < 0x2000) {
        return addr & 0xfff;
    }
    if (addr >= 0x4000 && addr < 0x5000) {
        return addr - 0x4000;
    }
    return -1;
}

static void update_irq(Tegra30PCIEState *s)
{
    bool pending = false;
    for (unsigned i = 0; i < 8; i++) {
        pending |= (s->regs[(0x386c + i * 4) / 4] &
                    s->regs[(0x388c + i * 4) / 4]) != 0;
    }
    qemu_set_irq(s->msi_irq, pending && (s->regs[0x38b4 / 4] & 0x100));
}

static void rcc_msi(Tegra30PCIEState *s)
{
    if (s->config[0x50 / 4] & (1U << 16)) {
        unsigned vector = s->config[0x58 / 4] & 0xff;
        s->regs[(0x386c + (vector / 32) * 4) / 4] |= 1U << (vector % 32);
        update_irq(s);
    }
}

static bool rcc_dma(Tegra30PCIEState *s, uint32_t addr, void *data,
                    size_t length, bool write)
{
    unsigned index = (addr & 0xfffffff) >> 23;
    uint32_t window = s->app[(0x200 + index * 8) / 4];
    uint32_t offset = addr & 0x7fffff;
    if (!(window & 1) || length > 0x800000 - offset) return false;
    hwaddr physical = (window & 0xff800000) + offset;
    return address_space_rw(&address_space_memory, physical,
                            MEMTXATTRS_UNSPECIFIED, data, length, write) == MEMTX_OK;
}

/* K3342 devp-iso-mmx-mib2: BAR3 channel doorbells, DMA buffer and length.
 * Unencrypted ISO transmit blocks are exported to the simulated MOST sink.
 */
static void rcc_most_command(Tegra30PCIEState *s, unsigned channel)
{
    if (channel >= 8) return;
    uint32_t command=s->most[channel];
    uint32_t addr=s->most[channel+8], length=s->most[channel+16];
    if (command != 0x22222222 || length < 4 || length > 1024*1024) return;
    g_autofree uint8_t *block=g_malloc(length);
    if (!rcc_dma(s,addr,block,length,false)) return;
    if (channel==3 && qemu_chr_fe_backend_connected(&s->most_video)) {
        uint8_t header[4];
        stl_le_p(header,length);
        qemu_chr_fe_write_all(&s->most_video,header,sizeof(header));
        qemu_chr_fe_write_all(&s->most_video,block,length);
    }
    uint32_t done=0;
    if (!rcc_dma(s,addr,&done,sizeof(done),true)) return;
    s->most[channel]=0;
    unsigned vector=(s->config[0x58/4]&0xff)+channel+2;
    if (vector<256) {
        s->regs[(0x386c+(vector/32)*4)/4] |= 1U<<(vector%32);
        update_irq(s);
    }
}

static ssize_t rcc_deliver(Tegra30PCIEState *s, const uint8_t *buf, size_t size)
{
    if (!(s->shared[1] & 2) || size > 2048) return size;
    uint32_t *desc = &s->shared[(0x48 + s->rx_head * 28) / 4];
    if (!(desc[4] & 0x80000000)) return 0;
    if (!rcc_dma(s, desc[0], (void *)buf, size, true)) return size;
    desc[1] = size;
    desc[2] = size;
    desc[4] = (desc[4] & 0x3fffffff) | 0x40000000;
    s->rx_head = (s->rx_head + 1) % 32;
    s->shared[0x20 / 4] = s->rx_head;
    rcc_msi(s);
    return size;
}

static ssize_t rcc_receive(NetClientState *nc, const uint8_t *buf, size_t size)
{
    return rcc_deliver(qemu_get_nic_opaque(nc), buf, size);
}

static int peer_can_receive(void *opaque)
{
    Tegra30PCIEState *s = opaque;
    return sizeof(s->pending) - s->pending_len;
}

static void peer_receive(void *opaque, const uint8_t *buf, int size)
{
    Tegra30PCIEState *s = opaque;
    memcpy(s->pending + s->pending_len, buf, size);
    s->pending_len += size;
}

static void rcc_poll(void *opaque)
{
    Tegra30PCIEState *s = opaque;
    if (s->shared[1] & 2) {
        if (!s->link_notified) {
            s->link_notified = true;
            rcc_msi(s);
        }
        for (unsigned packets = 0; packets < 32; packets++) {
            uint8_t packet[2048];
            unsigned total = 0, count, head = s->tx_head;
            uint32_t *first = &s->shared[(0x3c8 + head * 28) / 4];
            if (!(first[4] & 0x80000000)) break;
            count = first[4] & 0xffff;
            if (!count || count > 32) break;
            bool valid = true;
            for (unsigned i = 0; i < count; i++) {
                uint32_t *desc = &s->shared[(0x3c8 + ((head + i) % 32) * 28) / 4];
                unsigned length = desc[1];
                if (!(desc[4] & 0x80000000) || length > sizeof(packet) - total ||
                    !rcc_dma(s, desc[0], packet + total, length, false)) {
                    valid = false;
                    break;
                }
                total += length;
            }
            if (!valid || total != first[2]) break;
            qemu_send_packet(qemu_get_queue(s->nic), packet, total);
            if (qemu_chr_fe_backend_connected(&s->peer)) {
                uint8_t header[4];
                stl_be_p(header, total);
                qemu_chr_fe_write_all(&s->peer, header, sizeof(header));
                qemu_chr_fe_write_all(&s->peer, packet, total);
            }
            for (unsigned i = 0; i < count; i++) {
                uint32_t *desc = &s->shared[(0x3c8 + ((head + i) % 32) * 28) / 4];
                desc[4] = (desc[4] & 0x3fffffff) | 0x40000000;
            }
            s->tx_head = (head + count) % 32;
            s->shared[0x44 / 4] = (s->tx_head + 31) % 32;
            rcc_msi(s);
        }
        qemu_flush_queued_packets(qemu_get_queue(s->nic));
        while (s->pending_len >= 4) {
            unsigned length = ldl_be_p(s->pending);
            if (length > 2048) {
                s->pending_len = 0;
                break;
            }
            if (s->pending_len < length + 4) break;
            if (!rcc_deliver(s, s->pending + 4, length)) break;
            s->pending_len -= length + 4;
            memmove(s->pending, s->pending + length + 4, s->pending_len);
        }
        qemu_chr_fe_accept_input(&s->peer);
    }
    timer_mod(s->poll, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000);
}

static uint32_t read_word(Tegra30PCIEState *s, hwaddr addr)
{
    if (addr < sizeof(s->regs)) {
        int offset = root_offset(addr);
        uint32_t value = s->regs[addr / 4];
        /* Port 1 is wired to the RCC. The other root ports still enumerate. */
        if (addr >= 0x1000 && addr < 0x2000) {
            if (offset == 0xf00) value |= 1U << 30;
            if (offset == 0x90) value |= 1U << 29;
        }
        if (addr == 0x30b4) value |= 1U << 8; /* PADS PLL lock */
        return value;
    }
    /* AFI BAR0 config aperture: bus:device:function = 16:11:8. */
    if (addr >= 0x1000000 && addr < 0x2000000) {
        unsigned bus = (addr >> 16) & 0xff;
        unsigned slot = (addr >> 8) & 0xff;
        unsigned reg = addr & 0xff;
        unsigned secondary = (s->regs[0x1018 / 4] >> 8) & 0xff;
        if (!secondary || bus != secondary || slot != 0) return UINT32_MAX;
        if (reg == 0x10 && s->bar_probe[0]) return 0xfffff000;
        if (reg == 0x14 && s->bar_probe[1]) return 0xffffc000;
        if (reg == 0x18 && s->bar_probe[2]) return 0xfffff000;
        if (reg == 0x1c && s->bar_probe[3]) return 0xfffff000;
        return s->config[reg / 4];
    }
    uint32_t bar0 = s->config[0x10 / 4] & ~0xfffU;
    uint32_t bar1 = s->config[0x14 / 4] & ~0x3fffU;
    uint32_t bar3 = s->config[0x1c / 4] & ~0xfffU;
    if (bar0 && addr >= bar0 && addr - bar0 < sizeof(s->app)) {
        return s->app[(addr - bar0) / 4];
    }
    if (bar1 && addr >= bar1 && addr - bar1 < sizeof(s->shared)) {
        return s->shared[(addr - bar1) / 4];
    }
    if (bar3 && addr >= bar3 && addr-bar3 < sizeof(s->most)) {
        return s->most[(addr-bar3)/4];
    }
    return UINT32_MAX;
}

static void write_word(Tegra30PCIEState *s, hwaddr addr, uint32_t value)
{
    if (addr < sizeof(s->regs)) {
        int offset = root_offset(addr);
        if (offset == 0 || offset == 8 || offset == 0xc) return;
        if (offset == 0x10 || offset == 0x14) return;
        if (addr >= 0x386c && addr < 0x388c) s->regs[addr / 4] &= ~value;
        else s->regs[addr / 4] = value;
        update_irq(s);
        return;
    }
    if (addr >= 0x1000000 && addr < 0x2000000) {
        unsigned bus = (addr >> 16) & 0xff, slot = (addr >> 8) & 0xff;
        unsigned reg = addr & 0xff;
        unsigned secondary = (s->regs[0x1018 / 4] >> 8) & 0xff;
        if (!secondary || bus != secondary || slot != 0) return;
        if (reg == 0x10 || reg == 0x14 || reg == 0x18 || reg == 0x1c) {
            unsigned index = (reg - 0x10) / 4;
            s->bar_probe[index] = value == UINT32_MAX;
            if (!s->bar_probe[index]) {
                s->config[reg / 4] = value & (index == 1 ? 0xffffc000 : 0xfffff000);
            }
        } else if (reg >= 0x18 && reg <= 0x30) {
            /* BAR4, BAR5 and expansion ROM are absent. BAR2 is reserved
             * so QNX retains the production index of the ISO BAR3. */
        } else if (reg != 0 && reg != 8 && reg != 0xc && reg != 0x34) {
            s->config[reg / 4] = value;
        }
        return;
    }
    uint32_t bar0 = s->config[0x10 / 4] & ~0xfffU;
    uint32_t bar1 = s->config[0x14 / 4] & ~0x3fffU;
    uint32_t bar3 = s->config[0x1c / 4] & ~0xfffU;
    if (bar0 && addr >= bar0 && addr - bar0 < sizeof(s->app)) {
        unsigned offset = addr - bar0;
        s->app[offset / 4] = value;
        if (offset == 0x54 && value >= 2 && value < 10) {
            rcc_most_command(s,value-2);
        }
        if (offset == 0x7c && value == 0xdeadbeef) {
            /* RCC publishes its ring layout after MMX outbound DMA setup. */
            s->shared[0x18 / 4] = 0x48;
            s->shared[0x1c / 4] = 32;
            s->shared[0x30 / 4] = 0x48 + 32 * 28;
            s->shared[0x34 / 4] = 32;
            s->shared[0x44 / 4] = 31;
            s->shared[0] = 1;
            qemu_log_mask(LOG_GUEST_ERROR, "RCC PCIe: DMA window ready, rings published\n");
        }
    } else if (bar1 && addr >= bar1 && addr - bar1 < sizeof(s->shared)) {
        s->shared[(addr - bar1) / 4] = value;
    } else if (bar3 && addr>=bar3 && addr-bar3<sizeof(s->most)) {
        s->most[(addr-bar3)/4]=value;
    }
}

static uint64_t pcie_read(void *opaque, hwaddr addr, unsigned size)
{
    return read_word(opaque, addr & ~3ULL) >> ((addr & 3) * 8);
}

static void pcie_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    Tegra30PCIEState *s = opaque;
    unsigned shift = (addr & 3) * 8;
    if (size < 4) {
        uint32_t mask = ((1U << (size * 8)) - 1) << shift;
        value = (read_word(s, addr & ~3ULL) & ~mask) | ((value << shift) & mask);
    }
    write_word(s, addr & ~3ULL, value);
}

static const MemoryRegionOps pcie_ops = {
    .read = pcie_read, .write = pcie_write, .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
    .impl = { .min_access_size = 1, .max_access_size = 4 },
};

static void pcie_reset(DeviceState *dev)
{
    Tegra30PCIEState *s = TEGRA30_PCIE(dev);
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->config, 0, sizeof(s->config));
    memset(s->app, 0, sizeof(s->app));
    memset(s->shared, 0, sizeof(s->shared));
    memset(s->most, 0, sizeof(s->most));
    memset(s->bar_probe, 0, sizeof(s->bar_probe));
    s->tx_head = s->rx_head = 0;
    s->link_notified = false;
    s->pending_len = 0;
    timer_mod(s->poll, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000);
    const unsigned ports[] = { 0, 0x1000, 0x4000 };
    for (unsigned i = 0; i < 3; i++) {
        unsigned base = ports[i] / 4;
        s->regs[base] = 0x0e1c10de;
        s->regs[base + 8 / 4] = 0x060400a1;
        s->regs[base + 0xc / 4] = 0x00010000;
        s->regs[base + 0x34 / 4] = 0x80;
        s->regs[base + 0x80 / 4] = 0x00420010;
    }
    s->config[0] = 0xb800104c;
    s->config[4 / 4] = 0x00100000;
    s->config[8 / 4] = 0x02000001;
    s->config[0x34 / 4] = 0x50;
    s->config[0x50 / 4] = 0x00000005; /* 32-bit, one-vector MSI capability */
    s->config[0x3c / 4] = 0x000001ff;
    s->app[0x300 / 4] = 1;
    update_irq(s);
    qemu_set_irq(s->irq, 0);
}

static void pcie_init(Object *obj)
{
    Tegra30PCIEState *s = TEGRA30_PCIE(obj);
    memory_region_init_io(&s->mmio, obj, &pcie_ops, s, TYPE_TEGRA30_PCIE, 0x40000000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->msi_irq);
    s->poll = timer_new_ns(QEMU_CLOCK_VIRTUAL, rcc_poll, s);
}

static NetClientInfo rcc_net_info = {
    .type = NET_CLIENT_DRIVER_NIC, .size = sizeof(NICState), .receive = rcc_receive,
};

static void pcie_realize(DeviceState *dev, Error **errp)
{
    Tegra30PCIEState *s = TEGRA30_PCIE(dev);
    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&rcc_net_info, &s->conf, TYPE_TEGRA30_PCIE, dev->id,
                          &dev->mem_reentrancy_guard, s);
    Chardev *most = qemu_chr_find("mostvideo");
    if (most && !qemu_chr_fe_init(&s->most_video,most,errp)) return;
    Chardev *peer = qemu_chr_find("rcceth");
    if (peer) {
        if (!qemu_chr_fe_init(&s->peer, peer, errp)) return;
        qemu_chr_fe_set_handlers(&s->peer, peer_can_receive, peer_receive,
                                 NULL, NULL, s, NULL, true);
    }
}

static const Property pcie_properties[] = {
    DEFINE_NIC_PROPERTIES(Tegra30PCIEState, conf),
};

static void pcie_class_init(ObjectClass *klass, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(klass), pcie_reset);
    DEVICE_CLASS(klass)->realize = pcie_realize;
    device_class_set_props(DEVICE_CLASS(klass), pcie_properties);
}

static const TypeInfo pcie_info = {
    .name = TYPE_TEGRA30_PCIE, .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(Tegra30PCIEState), .instance_init = pcie_init,
    .class_init = pcie_class_init,
};
static void pcie_register_types(void) { type_register_static(&pcie_info); }
type_init(pcie_register_types)
