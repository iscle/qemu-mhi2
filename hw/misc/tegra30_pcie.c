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
#include "hw/core/qdev-properties-system.h"
#include "qemu/module.h"
#include "qemu/log.h"
#include "qemu/bswap.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "net/net.h"
#include "system/address-spaces.h"
#include "system/block-backend.h"
#include "chardev/char-fe.h"
#include "hw/ide/ide-internal.h"
#include "qapi/error.h"

#define TYPE_TEGRA30_PCIE "tegra30-pcie"
OBJECT_DECLARE_SIMPLE_TYPE(Tegra30PCIEState, TEGRA30_PCIE)

#define RCC_TX_BYTES (2 * MiB + 8)
typedef struct RCCOutput {
    CharFrontend chr;
    uint8_t bytes[RCC_TX_BYTES];
    unsigned head, count;
} RCCOutput;

struct Tegra30PCIEState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq irq, msi_irq;
    uint32_t regs[0x5000 / 4];
    uint32_t config[256 / 4];
    uint32_t app[0x1000 / 4];
    uint32_t shared[0x4000 / 4];
    uint8_t ata_mailbox[0x4000];
    IDEBus ata_bus;
    char *ata_drive;
    IDEDMA ata_dma;
    IDEState *ata_active;
    BlockCompletionFunc *ata_dma_cb;
    uint32_t ata_timing[0x80 / 4];
    struct { uint32_t addr, size; } ata_prd[64];
    unsigned ata_prd_count, ata_prd_index, ata_prd_offset;
    uint8_t ata_bm_cmd, ata_bm_status;
    bool ata_dma_read;
    bool bar_probe[4];
    uint32_t most[0x1000 / 4];
    uint8_t most_pending;
    RCCOutput most_video;
    NICConf conf;
    NICState *nic;
    QEMUTimer *poll;
    unsigned tx_head, rx_head;
    bool link_notified;
    RCCOutput peer;
    uint8_t pending[65536];
    unsigned pending_len;
};

/* A companion is allowed to be slow. Never sleep on its pipe under the BQL:
 * keep ownership of unaccepted descriptors and copy accepted data before
 * returning them to the guest. The existing 1 ms poll drains bounded work. */
static void output_drain(RCCOutput *out)
{
    unsigned budget = 64 * KiB;
    while (out->count && budget && qemu_chr_fe_backend_open(&out->chr)) {
        unsigned n = MIN(budget, MIN(out->count, RCC_TX_BYTES - out->head));
        int written = qemu_chr_fe_write(&out->chr, out->bytes + out->head, n);
        if (written <= 0) { break; }
        out->head = (out->head + written) % RCC_TX_BYTES;
        out->count -= written;
        budget -= written;
    }
}

static void output_append(RCCOutput *out, const uint8_t *data, unsigned size)
{
    unsigned tail = (out->head + out->count) % RCC_TX_BYTES;
    unsigned first = MIN(size, RCC_TX_BYTES - tail);
    memcpy(out->bytes + tail, data, first);
    memcpy(out->bytes, data + first, size - first);
    out->count += size;
}

static bool output_packet(RCCOutput *out, const uint8_t *data, unsigned size,
                           bool big_endian)
{
    uint8_t header[4];
    if (!qemu_chr_fe_backend_connected(&out->chr)) { return true; }
    if (size + sizeof(header) > RCC_TX_BYTES - out->count) { return false; }
    if (big_endian) { stl_be_p(header, size); }
    else { stl_le_p(header, size); }
    output_append(out, header, sizeof(header));
    output_append(out, data, size);
    return true;
}

static void peer_event(void *opaque, QEMUChrEvent event)
{
    Tegra30PCIEState *s = opaque;
    if (event == CHR_EVENT_CLOSED) {
        s->peer.head = s->peer.count = s->pending_len = 0;
        s->link_notified = false;
    }
}

static void most_event(void *opaque, QEMUChrEvent event)
{
    Tegra30PCIEState *s = opaque;
    if (event == CHR_EVENT_CLOSED) {
        s->most_video.head = s->most_video.count = 0;
    }
}

/* NVIDIA's FPCI layout differs from ECAM; Linux slides a 4 KiB window. */
static bool config_offset(Tegra30PCIEState *s, hwaddr addr, uint32_t *offset)
{
    uint64_t base = s->regs[0x3818 / 4];
    uint64_t size = (uint64_t)s->regs[0x3800 / 4] << 12;
    uint64_t fpci;

    if (size && addr >= base && addr - base < size) {
        fpci = ((uint64_t)s->regs[0x3830 / 4] << 8) + addr - base;
        if (fpci >= 0xfe00000000ULL && fpci < 0xfe20000000ULL) {
            *offset = fpci & 0x0fffffff;
            return true;
        }
        if (fpci >= 0xfdfe000000ULL && fpci < 0xfe00000000ULL) {
            *offset = fpci & 0x00ffffff;
            return true;
        }
    }
    /* Preserve the original QNX model's aperture before BAR programming. */
    if (!size && addr >= 0x01000000 && addr < 0x02000000) {
        *offset = addr & 0x00ffffff;
        return true;
    }
    return false;
}

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

/* devb-eide-mmx forwards ATA accesses through BAR2 and rings doorbell 1.
 * The first two words publish readiness and the controller timing clock.
 * Commands 1/2 are register vectors / repeated data-port I/O; command 3
 * copies up to 64 DMA descriptors and command 4 stops the bus master.
 * Descriptor addresses use the same RCC outbound windows as Ethernet. */
static void ata_irq(void *opaque, int n, int level)
{
    Tegra30PCIEState *s = opaque;
    if (!level) { return; }
    s->ata_bm_status |= 4;
    if (s->config[0x50 / 4] & BIT(16)) {
        unsigned vector = ((s->config[0x58 / 4] & 0xff) + 1) & 0xff;
        s->regs[(0x386c + (vector / 32) * 4) / 4] |= BIT(vector % 32);
        update_irq(s);
    }
}

static Tegra30PCIEState *ata_dma_owner(const IDEDMA *dma)
{
    return container_of(dma, Tegra30PCIEState, ata_dma);
}

static void ata_dma_start(const IDEDMA *dma, IDEState *ide,
                          BlockCompletionFunc *cb)
{
    Tegra30PCIEState *s = ata_dma_owner(dma);
    s->ata_active = ide;
    s->ata_dma_cb = cb;
    if (s->ata_bm_cmd & 1) { cb(ide, 0); }
}

static void ata_dma_inactive(const IDEDMA *dma, bool more)
{
    Tegra30PCIEState *s = ata_dma_owner(dma);
    s->ata_dma_cb = NULL;
    s->ata_active = NULL;
    s->ata_bm_status = (s->ata_bm_status & ~1) | more;
}

static void ata_dma_restart(const IDEDMA *dma)
{
    Tegra30PCIEState *s = ata_dma_owner(dma);
    s->ata_prd_index = s->ata_prd_offset = 0;
}

static void ata_dma_reset(const IDEDMA *dma)
{
    Tegra30PCIEState *s = ata_dma_owner(dma);
    ata_dma_inactive(dma, false);
    ata_dma_restart(dma);
    s->ata_prd_count = s->ata_bm_cmd = s->ata_bm_status = 0;
}

static int ata_dma_rw(const IDEDMA *dma, bool is_write)
{
    Tegra30PCIEState *s = ata_dma_owner(dma);
    IDEState *ide = s->ata_active;
    while (ide->io_buffer_index < ide->io_buffer_size) {
        unsigned i = s->ata_prd_index;
        if (i >= s->ata_prd_count || is_write != s->ata_dma_read ||
            is_write != !!(s->ata_bm_cmd & 8)) {
            goto error;
        }
        unsigned length = MIN(ide->io_buffer_size - ide->io_buffer_index,
                              s->ata_prd[i].size - s->ata_prd_offset);
        if (!rcc_dma(s, s->ata_prd[i].addr + s->ata_prd_offset,
                     ide->io_buffer + ide->io_buffer_index, length, is_write)) {
            goto error;
        }
        ide->io_buffer_index += length;
        s->ata_prd_offset += length;
        if (s->ata_prd_offset == s->ata_prd[i].size) {
            s->ata_prd_index++;
            s->ata_prd_offset = 0;
        }
    }
    return 1;
error:
    s->ata_bm_status |= 2;
    ide_abort_command(ide);
    ide_bus_set_irq(&s->ata_bus);
    return 0;
}

static const IDEDMAOps ata_dma_ops = {
    .start_dma = ata_dma_start, .rw_buf = ata_dma_rw,
    .restart_dma = ata_dma_restart, .set_inactive = ata_dma_inactive,
    .reset = ata_dma_reset,
};

static bool ata_port_valid(unsigned port, unsigned bits)
{
    if (port == 0x1f0) { return bits == 16 || bits == 32; }
    if ((port > 0x1f0 && port <= 0x1f7) || port == 0x3f6) {
        return bits == 8;
    }
    if (port == 0 || port == 2 || port == 0x48) {
        return bits == 8 || bits == 16;
    }
    return port >= 0x50 && port <= 0x74 && !(port & 3) && bits == 32;
}

static uint32_t ata_port_read(Tegra30PCIEState *s, unsigned port, unsigned bits)
{
    if (port == 0x1f0) {
        return bits == 16 ? ide_data_readw(&s->ata_bus, 0) :
                            ide_data_readl(&s->ata_bus, 0);
    }
    if (port >= 0x1f1 && port <= 0x1f7) {
        return ide_ioport_read(&s->ata_bus, port & 7);
    }
    if (port == 0x3f6) { return ide_status_read(&s->ata_bus, 0); }
    if (port == 0) { return s->ata_bm_cmd; }
    if (port == 2) { return s->ata_bm_status; }
    return s->ata_timing[port / 4];
}

static void ata_port_write(Tegra30PCIEState *s, unsigned port,
                           unsigned bits, uint32_t value)
{
    if (port == 0x1f0) {
        if (bits == 16) { ide_data_writew(&s->ata_bus, 0, value); }
        else { ide_data_writel(&s->ata_bus, 0, value); }
    } else if (port >= 0x1f1 && port <= 0x1f7) {
        ide_ioport_write(&s->ata_bus, port & 7, value);
    } else if (port == 0x3f6) {
        ide_ctrl_write(&s->ata_bus, 0, value);
    } else if (port == 0) {
        bool start = !(s->ata_bm_cmd & 1) && (value & 1);
        if (!(value & 1)) {
            ide_cancel_dma_sync(&s->ata_bus.ifs[s->ata_bus.unit]);
            ata_dma_inactive(&s->ata_dma, false);
        }
        s->ata_bm_cmd = value & 9;
        if (start) {
            s->ata_bm_status |= 1;
            if (s->ata_dma_cb) { s->ata_dma_cb(s->ata_active, 0); }
        }
    } else if (port == 2) {
        s->ata_bm_status &= ~(value & 6); /* error / interrupt W1C */
    } else {
        /* Timing and UDMA-enable registers are retained. Transfers use the
         * virtual controller clock; no host port I/O is performed. */
        s->ata_timing[port / 4] = value;
    }
}

static void rcc_ata_command(Tegra30PCIEState *s)
{
    uint8_t *m = s->ata_mailbox;
    unsigned command = ldl_le_p(m + 8), count = ldl_le_p(m + 12);
    if (!(command & 0x80)) { return; }
    if (command == 0x81) {
        if (count > (sizeof(s->ata_mailbox) - 20) / 8) { goto invalid; }
        for (unsigned i = 0; i < count; i++) {
            uint8_t *r = m + 20 + i * 8;
            if (!ata_port_valid(lduw_le_p(r), r[2]) || r[3] > 1) {
                goto invalid;
            }
        }
        for (unsigned i = 0; i < count; i++) {
            uint8_t *r = m + 20 + i * 8;
            if (r[3]) { stl_le_p(r + 4, ata_port_read(s, lduw_le_p(r), r[2])); }
            else { ata_port_write(s, lduw_le_p(r), r[2], ldl_le_p(r + 4)); }
        }
    } else if (command == 0x82) {
        unsigned port = lduw_le_p(m + 20), bits = m[22], bytes = bits / 8;
        if (port != 0x1f0 || !ata_port_valid(port, bits) || m[23] > 1 ||
            count > 8192 / bytes) { goto invalid; }
        for (unsigned i = 0; i < count; i++) {
            uint8_t *p = m + 24 + i * bytes;
            if (m[23]) {
                uint32_t value = ata_port_read(s, port, bits);
                if (bytes == 2) { stw_le_p(p, value); }
                else { stl_le_p(p, value); }
            } else {
                ata_port_write(s, port, bits,
                               bytes == 2 ? lduw_le_p(p) : ldl_le_p(p));
            }
        }
    } else if (command == 0x83) {
        unsigned n, total = 0;
        if (s->ata_bm_cmd & 1 || ldl_le_p(m + 16) > 1) { goto invalid; }
        for (n = 0; n < ARRAY_SIZE(s->ata_prd); n++) {
            uint32_t addr = ldl_le_p(m + 20 + n * 8);
            uint32_t length = ldl_le_p(m + 24 + n * 8);
            unsigned size = length & 0x7fffffff;
            if (!size || (size & 1) || (addr & 3) || size > 0x40000 - total ||
                size > 0x800000 - (addr & 0x7fffff)) { goto invalid; }
            total += size;
            if (length & BIT(31)) { break; }
        }
        if (n == ARRAY_SIZE(s->ata_prd)) { goto invalid; }
        s->ata_prd_count = n + 1;
        for (unsigned i = 0; i <= n; i++) {
            s->ata_prd[i].addr = ldl_le_p(m + 20 + i * 8);
            s->ata_prd[i].size = ldl_le_p(m + 24 + i * 8) & 0x7fffffff;
        }
        s->ata_dma_read = ldl_le_p(m + 16);
        ata_dma_restart(&s->ata_dma);
    } else if (command == 0x84) {
        ata_port_write(s, 0, 16, 0);
        ata_port_write(s, 2, 16, 6);
    } else {
        goto invalid;
    }
    stl_le_p(m + 8, command & ~0x80);
    return;
invalid:
    /* Unknown requests remain busy: never acknowledge an operation we did
     * not perform. The native driver has a bounded timeout for this case. */
    qemu_log_mask(LOG_GUEST_ERROR, "RCC ATA: invalid mailbox command %#x count %u\n",
                  command, count);
}

/* K3342 devp-iso-mmx-mib2: BAR3 channel doorbells, DMA buffer and length.
 * Unencrypted ISO transmit blocks are exported to the simulated MOST sink.
 */
static void rcc_most_command(Tegra30PCIEState *s, unsigned channel)
{
    if (channel >= 8 || !(s->most_pending & BIT(channel))) return;
    uint32_t command=s->most[channel];
    uint32_t addr=s->most[channel+8], length=s->most[channel+16];
    if (command != 0x22222222 || length < 4 || length > 1024*1024) {
        s->most_pending &= ~BIT(channel);
        return;
    }
    if (channel == 3 && qemu_chr_fe_backend_connected(&s->most_video.chr) &&
        length + 4 > RCC_TX_BYTES - s->most_video.count) {
        return;
    }
    g_autofree uint8_t *block=g_malloc(length);
    if (!rcc_dma(s,addr,block,length,false)) return;
    if (channel == 3 && !output_packet(&s->most_video, block, length, false)) {
        return;
    }
    uint32_t done=0;
    if (!rcc_dma(s,addr,&done,sizeof(done),true)) return;
    s->most[channel]=0;
    s->most_pending &= ~BIT(channel);
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
    output_drain(&s->peer);
    output_drain(&s->most_video);
    for (unsigned i = 0; i < 8; i++) {
        rcc_most_command(s, i);
    }
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
            if (!output_packet(&s->peer, packet, total, true)) break;
            qemu_send_packet(qemu_get_queue(s->nic), packet, total);
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
        qemu_chr_fe_accept_input(&s->peer.chr);
    }
    timer_mod(s->poll, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + 1000000);
}

static uint32_t read_word(Tegra30PCIEState *s, hwaddr addr)
{
    uint32_t cfg;

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
    if (config_offset(s, addr, &cfg)) {
        unsigned bus = (cfg >> 16) & 0xff;
        unsigned slot = (cfg >> 8) & 0xff;
        unsigned reg = ((cfg >> 16) & 0xf00) | (cfg & 0xff);
        unsigned secondary = (s->regs[0x1018 / 4] >> 8) & 0xff;
        if (!secondary || bus != secondary || slot != 0) return UINT32_MAX;
        if (reg >= sizeof(s->config)) {
            return 0; /* No extended capabilities on the RCC endpoint. */
        }
        if (reg == 0x10 && s->bar_probe[0]) return 0xfffff000;
        if (reg == 0x14 && s->bar_probe[1]) return 0xffffc000;
        if (reg == 0x18 && s->bar_probe[2]) return 0xffffc000;
        if (reg == 0x1c && s->bar_probe[3]) return 0xfffff000;
        return s->config[reg / 4];
    }
    uint32_t bar0 = s->config[0x10 / 4] & ~0xfffU;
    uint32_t bar1 = s->config[0x14 / 4] & ~0x3fffU;
    uint32_t bar2 = s->config[0x18 / 4] & ~0x3fffU;
    uint32_t bar3 = s->config[0x1c / 4] & ~0xfffU;
    if (bar0 && addr >= bar0 && addr - bar0 < sizeof(s->app)) {
        return s->app[(addr - bar0) / 4];
    }
    if (bar1 && addr >= bar1 && addr - bar1 < sizeof(s->shared)) {
        return s->shared[(addr - bar1) / 4];
    }
    if (bar2 && addr >= bar2 && addr - bar2 < sizeof(s->ata_mailbox)) {
        return ldl_le_p(s->ata_mailbox + addr - bar2);
    }
    if (bar3 && addr >= bar3 && addr-bar3 < sizeof(s->most)) {
        return s->most[(addr-bar3)/4];
    }
    return UINT32_MAX;
}

static void write_word(Tegra30PCIEState *s, hwaddr addr, uint32_t value)
{
    uint32_t cfg;

    if (addr < sizeof(s->regs)) {
        int offset = root_offset(addr);
        if (offset == 0 || offset == 8 || offset == 0xc) return;
        if (offset == 0x10 || offset == 0x14) return;
        if (addr >= 0x386c && addr < 0x388c) s->regs[addr / 4] &= ~value;
        else s->regs[addr / 4] = value;
        update_irq(s);
        return;
    }
    if (config_offset(s, addr, &cfg)) {
        unsigned bus = (cfg >> 16) & 0xff, slot = (cfg >> 8) & 0xff;
        unsigned reg = ((cfg >> 16) & 0xf00) | (cfg & 0xff);
        unsigned secondary = (s->regs[0x1018 / 4] >> 8) & 0xff;
        if (!secondary || bus != secondary || slot != 0) return;
        if (reg >= sizeof(s->config)) {
            return;
        }
        if (reg == 0x10 || reg == 0x14 || reg == 0x18 || reg == 0x1c) {
            unsigned index = (reg - 0x10) / 4;
            s->bar_probe[index] = value == UINT32_MAX;
            if (!s->bar_probe[index]) {
                s->config[reg / 4] = value &
                    (index == 1 || index == 2 ? 0xffffc000 : 0xfffff000);
            }
        } else if (reg >= 0x18 && reg <= 0x30) {
            /* BAR4, BAR5 and expansion ROM are absent. */
        } else if (reg == 0x50) {
            s->config[reg / 4] = (value & 0x00710000) | 0x00080005;
        } else if (reg != 0 && reg != 8 && reg != 0xc && reg != 0x34) {
            s->config[reg / 4] = value;
        }
        return;
    }
    uint32_t bar0 = s->config[0x10 / 4] & ~0xfffU;
    uint32_t bar1 = s->config[0x14 / 4] & ~0x3fffU;
    uint32_t bar2 = s->config[0x18 / 4] & ~0x3fffU;
    uint32_t bar3 = s->config[0x1c / 4] & ~0xfffU;
    if (bar0 && addr >= bar0 && addr - bar0 < sizeof(s->app)) {
        unsigned offset = addr - bar0;
        s->app[offset / 4] = value;
        if (offset == 0x54 && value == 1) { rcc_ata_command(s); }
        if (offset == 0x54 && value >= 2 && value < 10) {
            s->most_pending |= BIT(value - 2);
            rcc_most_command(s,value-2);
        }
        if (offset == 0x7c && value == 0xdeadbeef) {
            /* RCC publishes its ring layout after MMX outbound DMA setup. */
            s->tx_head = s->rx_head = 0;
            s->link_notified = false;
            memset(s->shared, 0, sizeof(s->shared));
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
    } else if (bar2 && addr >= bar2 && addr - bar2 < sizeof(s->ata_mailbox)) {
        if (addr - bar2 >= 8) { stl_le_p(s->ata_mailbox + addr - bar2, value); }
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
    memset(s->ata_mailbox, 0, sizeof(s->ata_mailbox));
    memset(s->ata_timing, 0, sizeof(s->ata_timing));
    stl_le_p(s->ata_mailbox, 0x80000001); /* ready, DMA stop command */
    stl_le_p(s->ata_mailbox + 4, 100000000); /* virtual ATA clock, Hz */
    ide_bus_reset(&s->ata_bus, IDE_RESET_HARDWARE);
    memset(s->most, 0, sizeof(s->most));
    s->most_pending = 0;
    memset(s->bar_probe, 0, sizeof(s->bar_probe));
    s->tx_head = s->rx_head = 0;
    s->link_notified = false;
    s->pending_len = 0;
    s->peer.head = s->peer.count = 0;
    s->most_video.head = s->most_video.count = 0;
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
    s->config[0x50 / 4] = 0x00080005; /* 32-bit, 16 vectors: net, ATA, MOST */
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
    ide_bus_init(&s->ata_bus, sizeof(s->ata_bus), DEVICE(obj), 0, 2);
}

static NetClientInfo rcc_net_info = {
    .type = NET_CLIENT_DRIVER_NIC, .size = sizeof(NICState), .receive = rcc_receive,
};

static void pcie_realize(DeviceState *dev, Error **errp)
{
    Tegra30PCIEState *s = TEGRA30_PCIE(dev);
    ide_bus_init_output_irq(&s->ata_bus, qemu_allocate_irq(ata_irq, s, 0));
    s->ata_dma.ops = &ata_dma_ops;
    s->ata_bus.dma = &s->ata_dma;
    DeviceState *cd = qdev_new("ide-cd");
    qdev_prop_set_uint32(cd, "unit", 0);
    qdev_prop_set_string(cd, "model", "MHI2 VIRTUAL DVD");
    qdev_prop_set_string(cd, "serial", "MHI2-DVD-0001");
    if (s->ata_drive) {
        BlockBackend *drive = blk_by_name(s->ata_drive);
        if (!drive) {
            error_setg(errp, "RCC DVD backend '%s' does not exist", s->ata_drive);
            object_unref(OBJECT(cd));
            return;
        }
        qdev_prop_set_drive(cd, "drive", drive);
    }
    if (!qdev_realize_and_unref(cd, &s->ata_bus.qbus, errp)) { return; }
    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&rcc_net_info, &s->conf, TYPE_TEGRA30_PCIE, dev->id,
                          &dev->mem_reentrancy_guard, s);
    Chardev *most = qemu_chr_find("mostvideo");
    if (most) {
        if (!qemu_chr_fe_init(&s->most_video.chr, most, errp)) { return; }
        qemu_chr_fe_set_handlers(&s->most_video.chr, NULL, NULL,
                                 most_event, NULL, s, NULL, true);
    }
    Chardev *peer = qemu_chr_find("rcceth");
    if (peer) {
        if (!qemu_chr_fe_init(&s->peer.chr, peer, errp)) return;
        qemu_chr_fe_set_handlers(&s->peer.chr, peer_can_receive, peer_receive,
                                 peer_event, NULL, s, NULL, true);
    }
}

static const Property pcie_properties[] = {
    DEFINE_NIC_PROPERTIES(Tegra30PCIEState, conf),
    DEFINE_PROP_STRING("dvd-drive", Tegra30PCIEState, ata_drive),
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
