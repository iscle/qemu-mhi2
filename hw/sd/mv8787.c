/*
 * Local Marvell 8787 SDIO companion experiment.
 * Register protocol: Marvell's Linux mwifiex/btmrvl drivers and the supplied
 * QNX io-sdiorm driver. No firmware executable or startup policy is changed.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/sd/sd.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "migration/vmstate.h"
#include "net/net.h"
#include "net/eth.h"
#include "chardev/char-fe.h"
#include "qapi/error.h"

#define TYPE_MV8787 "mv8787-sdio"
OBJECT_DECLARE_SIMPLE_TYPE(MV8787State, MV8787)
#define MV_SPACE 0x20000
#define MV_PORT  0x10000
#define MV_MAX_TRANSFER (512 * 2048)
#define MV_RX_DEPTH 64
#define MV_BT_TX_SIZE (128 * 1024)

struct MV8787State {
    DeviceState parent_obj;
    NICConf conf;
    NICState *nic;
    char *ssid;
    MACAddr bssid;
    uint8_t channel;
    bool associated;
    uint8_t station_bss, wlan_rx_port, wlan_next_rx_port;
    uint16_t rx_sequence;
    GQueue pending[4];
    CharFrontend bt_chr;
    uint8_t bt_rx[4096], bt_tx[MV_BT_TX_SIZE];
    unsigned bt_rx_used, bt_rx_needed, bt_rx_discard, bt_tx_used, bt_watch;
    unsigned bt_function;
    bool bt_open;

    uint8_t regs[4][MV_SPACE];
    uint16_t rca;
    bool selected;
    uint32_t address, remaining, transferred;
    unsigned function;
    bool write, increment;
    uint8_t packet[MV_MAX_TRANSFER];
    uint8_t fw_header[16];
    unsigned fw_header_count;
    uint32_t fw_remaining, fw_bytes, fw_records;
    bool fw_ready, fw_invalid;
    unsigned trace_count;
    uint8_t rx[4][4096];
    uint16_t rx_len[4], rx_pos[4];
    bool wlan_initialized;
    uint16_t tx_buffer_size, power_save_bitmap;
    uint16_t ibss_coalescing, amsdu_enabled;
    uint32_t mac_filter;
    uint8_t multicast[32][6];
    uint16_t multicast_count;
    int16_t tx_power;
    uint8_t mac_address[4][6];
    uint16_t mib[64];
    struct { uint16_t tag, len; uint8_t data[256]; } uap[32];
    uint16_t max_stations;
    uint8_t coex[3][28];
    uint8_t local_name[248];
    uint8_t scan_enable;
    uint8_t hci_config[32][8];
    uint8_t inquiry_response[241];
    int8_t inquiry_tx_power;
};

static SDBus *mv_bus(MV8787State *s)
{
    return SD_BUS(qdev_get_parent_bus(DEVICE(s)));
}

static void mv_irq(MV8787State *s)
{
    unsigned pending = 0;
    for (unsigned f = 1; f <= 3; f++) {
        if (s->regs[f][2] & s->regs[f][3]) {
            pending |= 1 << f;
        }
    }
    s->regs[0][5] = pending;
    sdbus_set_irq(mv_bus(s), (s->regs[0][4] & 1) &&
                  (s->regs[0][4] & pending));
}

static void mv_bt_flush(MV8787State *s);
static void mv_queue_next(MV8787State *s, unsigned f);

static void mv_reset(DeviceState *dev)
{
    MV8787State *s = MV8787(dev);
    memset(s->regs, 0, sizeof(s->regs));
    s->rca = 1;
    s->selected = false;
    s->remaining = s->transferred = 0;
    s->fw_header_count = s->fw_remaining = s->fw_bytes = s->fw_records = 0;
    s->fw_ready = s->fw_invalid = false;
    s->trace_count = 0;
    memset(s->rx_len, 0, sizeof(s->rx_len));
    memset(s->rx_pos, 0, sizeof(s->rx_pos));
    s->associated = false;
    s->station_bss = s->wlan_rx_port = 0;
    s->wlan_next_rx_port = 1;
    s->rx_sequence = 0;
    for (unsigned f = 0; f < 4; f++) {
        g_queue_clear_full(&s->pending[f], (GDestroyNotify)g_bytes_unref);
    }
    s->bt_rx_used = s->bt_rx_needed = s->bt_rx_discard = s->bt_tx_used = 0;
    s->bt_function = 2;
    if (s->bt_watch) {
        g_source_remove(s->bt_watch);
        s->bt_watch = 0;
    }
    if (s->nic) {
        qemu_purge_queued_packets(qemu_get_queue(s->nic));
    }
    s->wlan_initialized = false;
    s->tx_buffer_size = 2048;
    s->power_save_bitmap = 0;
    s->ibss_coalescing = s->amsdu_enabled = 0;
    s->mac_filter = 0;
    s->multicast_count = 0;
    s->tx_power = 10;
    memset(s->mib, 0, sizeof(s->mib));
    memset(s->uap, 0, sizeof(s->uap));
    s->max_stations = 16;
    memset(s->coex, 0, sizeof(s->coex));
    for (unsigned i = 0; i < 4; i++) {
        memcpy(s->mac_address[i], s->conf.macaddr.a, 6);
        s->mac_address[i][5] += i;
    }
    memset(s->local_name, 0, sizeof(s->local_name));
    memcpy(s->local_name, "MHI2 virtual Bluetooth", 22);
    s->scan_enable = 0;
    memset(s->hci_config, 0, sizeof(s->hci_config));
    memset(s->inquiry_response, 0, sizeof(s->inquiry_response));
    s->inquiry_tx_power = 0;
    s->regs[0][0] = 0x32; /* SDIO 3.0, CCCR 2.0 */
    s->regs[0][1] = 2;
    s->regs[0][8] = 2;    /* Multi-block transfers */
    s->regs[0][0x13] = 1; /* High-speed capable */
    s->regs[0][0x5c] = 3;
    for (unsigned f = 0; f <= 3; f++) {
        unsigned cis = 0x1000 + f * 0x100;
        unsigned base = f * 0x100;
        uint8_t *p = &s->regs[0][cis];
        s->regs[0][base + 9] = cis;
        s->regs[0][base + 10] = cis >> 8;
        s->regs[0][base + 0x11] = 1; /* 256-byte default block */
        /* Common ID 9118, per-function IDs 9119 / 911a / 911b. */
        *p++ = 0x20; *p++ = 4;
        stw_le_p(p, 0x02df); p += 2;
        stw_le_p(p, 0x9118 + f); p += 2;
        *p++ = 0x21; *p++ = 2; *p++ = 0x0c; *p++ = 0;
        *p++ = 0x22;
        if (!f) {
            *p++ = 4; *p++ = 0; *p++ = 0; *p++ = 2; *p++ = 0x32;
        } else {
            *p++ = 42;
            p[0] = 1;
            stw_le_p(p + 12, 2048);
            stw_le_p(p + 28, 100);
            p += 42;
            s->regs[0][base] = f == 1 ? 7 : 2;
            /* 32-byte RX units fit full ACL packets in the length register. */
            s->regs[f][0x63] = 5;
            s->regs[f][0x30] = 9; /* Boot ROM can accept download data */
            s->regs[f][0x40] = 16;
            s->regs[f][0x5c] = 3;
            s->regs[f][0x7a] = MV_PORT >> 16;
            s->regs[f][0x801c] = 0x32;
            s->regs[f][0x801d] = 0x30;
        }
        *p = 0xff;
    }
    mv_irq(s);
}

static void mv_fw_byte(MV8787State *s, uint8_t value)
{
    if (s->fw_invalid || s->fw_ready) {
        return;
    }
    s->fw_bytes++;
    if (s->fw_remaining) {
        s->fw_remaining--;
        return;
    }
    s->fw_header[s->fw_header_count++] = value;
    if (s->fw_header_count == 16) {
        uint32_t cmd = ldl_le_p(s->fw_header);
        uint32_t len = ldl_le_p(s->fw_header + 8);
        s->fw_header_count = 0;
        if ((cmd != 1 && cmd != 4) || len > MV_MAX_TRANSFER ||
            (cmd == 4 && len)) {
            s->fw_invalid = true;
            error_report("mv8787: invalid firmware record cmd=%u len=%u", cmd, len);
            return;
        }
        s->fw_records++;
        s->fw_remaining = len;
        if (cmd == 4) {
            s->fw_ready = true;
            for (unsigned f = 1; f <= 3; f++) {
                stw_le_p(&s->regs[f][0x60], 0xfedc);
                stw_le_p(&s->regs[f][6], 0xffff);
                s->regs[f][0x30] = 0x09;
            }
            qemu_chr_fe_accept_input(&s->bt_chr);
            fprintf(stderr, "mv8787: firmware received: %u bytes, %u records\n",
                    s->fw_bytes, s->fw_records);
        }
    }
}

static void mv_enqueue(MV8787State *s, unsigned f, const uint8_t *p, unsigned len)
{
    if (len > sizeof(s->rx[f]) ||
        g_queue_get_length(&s->pending[f]) >= MV_RX_DEPTH) {
        error_report("mv8787: RX queue overflow fn=%u len=%u", f, len);
        return;
    }
    if (s->rx_len[f]) {
        g_queue_push_tail(&s->pending[f], g_bytes_new(p, len));
        return;
    }
    memcpy(s->rx[f], p, len);
    s->rx_len[f] = len;
    s->rx_pos[f] = 0;
    s->regs[f][3] |= 3; /* Packet available and transmit slot free */
    unsigned port = 0;
    if (f == 1 && len >= 4 && lduw_le_p(p + 2) == 0) {
        port = s->wlan_next_rx_port;
        s->wlan_next_rx_port = port == 15 ? 1 : port + 1;
    }
    if (f == 1) {
        s->wlan_rx_port = port;
    }
    stw_le_p(&s->regs[f][4], 1u << port);
    stw_le_p(&s->regs[f][8 + 2 * port], len);
    s->regs[f][0x62] = (len + 31) / 32;
    s->regs[f][0x63] = 5;
    s->regs[f][0x30] |= 2;
    mv_irq(s);
}

/* Standard Ethernet backend; the chip terminates the virtual 802.11 link. */
static bool mv_wifi_link(MV8787State *s)
{
    NetClientState *nc = qemu_get_queue(s->nic);
    return s->wlan_initialized && nc->peer && !nc->link_down;
}

static void mv_wifi_event(MV8787State *s, uint32_t event)
{
    uint8_t out[14] = {0};
    stw_le_p(out, sizeof(out));
    stw_le_p(out + 2, 3);
    stl_le_p(out + 4, event | (s->station_bss << 16));
    memcpy(out + 8, s->bssid.a, 6);
    mv_enqueue(s, 1, out, sizeof(out));
}

static bool mv_wifi_can_receive(NetClientState *nc)
{
    MV8787State *s = qemu_get_nic_opaque(nc);
    /* Reserve command/event slots even under a saturated Ethernet backend. */
    return g_queue_get_length(&s->pending[1]) < MV_RX_DEPTH - 8;
}

static ssize_t mv_wifi_receive(NetClientState *nc, const uint8_t *buf,
                               size_t len)
{
    MV8787State *s = qemu_get_nic_opaque(nc);
    uint8_t out[4096] = {0};
    if (!s->associated || !mv_wifi_link(s) || len < 14 || len > 1518) {
        return len;
    }
    if (!mv_wifi_can_receive(nc)) {
        return 0;
    }
    stw_le_p(out, len + 24);
    out[5] = s->station_bss;
    stw_le_p(out + 6, len);
    stw_le_p(out + 8, 20); /* RxPD-relative Ethernet frame offset */
    stw_le_p(out + 12, s->rx_sequence++);
    out[15] = 7; /* Legacy OFDM 18 Mbps, no aggregation/reordering */
    out[16] = 50; out[17] = (uint8_t)-90;
    memcpy(out + 24, buf, len);
    mv_enqueue(s, 1, out, len + 24);
    return len;
}

static void mv_wifi_tx(MV8787State *s, const uint8_t *pd, unsigned len)
{
    unsigned offset, size;
    if (len < 16 || !s->associated || !mv_wifi_link(s)) {
        return;
    }
    size = lduw_le_p(pd + 2);
    offset = lduw_le_p(pd + 4);
    if (offset < 16 || offset > len || size > len - offset ||
        size < 14 || size > 1518 || lduw_le_p(pd + 6) ||
        pd[0] != 0 || pd[1] != s->station_bss) {
        qemu_log_mask(LOG_GUEST_ERROR, "mv8787: invalid Wi-Fi TxPD\n");
        return;
    }
    qemu_send_packet(qemu_get_queue(s->nic), pd + offset, size);
    s->regs[1][3] |= 2;
    mv_irq(s);
}

static void mv_wifi_link_changed(NetClientState *nc)
{
    MV8787State *s = qemu_get_nic_opaque(nc);
    if (nc->link_down && s->associated) {
        s->associated = false;
        qemu_purge_queued_packets(nc);
        mv_wifi_event(s, 8); /* EVENT_DEAUTHENTICATED */
    }
}

static NetClientInfo mv_net_info = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .can_receive = mv_wifi_can_receive,
    .receive = mv_wifi_receive,
    .link_status_changed = mv_wifi_link_changed,
};

/* H4 is the standard HCI UART byte stream, also used by BlueZ btproxy. */
static void mv_bt_packet(MV8787State *s, unsigned f,
                         const uint8_t *h4, unsigned len)
{
    uint8_t out[4096];
    if (len < 1 || len + 3 > sizeof(out)) {
        return;
    }
    stl_le_p(out, len + 3);
    memcpy(out + 3, h4, len);
    mv_enqueue(s, f, out, len + 3);
}

static void mv_queue_next(MV8787State *s, unsigned f)
{
    GBytes *next = g_queue_pop_head(&s->pending[f]);
    if (next) {
        gsize len;
        const void *data = g_bytes_get_data(next, &len);
        mv_enqueue(s, f, data, len);
        g_bytes_unref(next);
    }
    if (f == 1 && s->nic) {
        qemu_flush_queued_packets(qemu_get_queue(s->nic));
    }
    if (f == s->bt_function) {
        qemu_chr_fe_accept_input(&s->bt_chr);
    }
}

static gboolean mv_bt_writable(void *unused, GIOCondition condition,
                               void *opaque)
{
    MV8787State *s = opaque;
    s->bt_watch = 0;
    if (!(condition & G_IO_HUP)) {
        mv_bt_flush(s);
    }
    return G_SOURCE_REMOVE;
}

static void mv_bt_flush(MV8787State *s)
{
    while (s->bt_open && s->bt_tx_used) {
        int n = qemu_chr_fe_write(&s->bt_chr, s->bt_tx, s->bt_tx_used);
        if (n <= 0) {
            break;
        }
        s->bt_tx_used -= n;
        memmove(s->bt_tx, s->bt_tx + n, s->bt_tx_used);
    }
    if (s->bt_open && s->bt_tx_used && !s->bt_watch) {
        s->bt_watch = qemu_chr_fe_add_watch(&s->bt_chr, G_IO_OUT | G_IO_HUP,
                                           mv_bt_writable, s);
    }
    if (s->bt_tx_used <= MV_BT_TX_SIZE - 4096) {
        s->regs[s->bt_function][3] |= 2;
        mv_irq(s);
    }
}

static void mv_bt_send(MV8787State *s, unsigned f, const uint8_t *p, unsigned n)
{
    unsigned expected;
    if (n < 7) {
        return;
    }
    switch (p[3]) {
    case 1:
    case 3:
        expected = 7 + p[6];
        break;
    case 2:
        if (n < 8) {
            return;
        }
        expected = 8 + lduw_le_p(p + 6);
        break;
    default:
        return;
    }
    if (expected != n || n - 3 > 4093) {
        qemu_log_mask(LOG_GUEST_ERROR, "mv8787: malformed Bluetooth TX\n");
        return;
    }
    s->bt_function = f;
    /* This is a chip-local health query, not a command for the host radio. */
    if (p[3] == 1 && lduw_le_p(p + 4) == 0xfc0f && p[6] == 0) {
        const uint8_t revision[] = {
            4, 0x0e, 8, 1, 0x0f, 0xfc, 0, 0, 0, 0x0e, 1
        };
        mv_bt_packet(s, f, revision, sizeof(revision));
        return;
    }
    if (!s->bt_open || n - 3 > sizeof(s->bt_tx) - s->bt_tx_used) {
        const uint8_t failure[] = {4, 0x10, 1, 1}; /* Hardware Error */
        mv_bt_packet(s, f, failure, sizeof(failure));
        return;
    }
    memcpy(s->bt_tx + s->bt_tx_used, p + 3, n - 3);
    s->bt_tx_used += n - 3;
    s->regs[f][3] &= ~2;
    mv_bt_flush(s);
}

static int mv_bt_can_read(void *opaque)
{
    MV8787State *s = opaque;
    if (!s->fw_ready ||
        g_queue_get_length(&s->pending[s->bt_function]) >= MV_RX_DEPTH - 1) {
        return 0;
    }
    if (s->bt_rx_discard) {
        return MIN(s->bt_rx_discard, 4096);
    }
    return s->bt_rx_needed ? s->bt_rx_needed - s->bt_rx_used : 1;
}

static void mv_bt_read(void *opaque, const uint8_t *buf, int size)
{
    MV8787State *s = opaque;
    while (size--) {
        unsigned header;
        if (s->bt_rx_discard) {
            s->bt_rx_discard--;
            buf++;
            continue;
        }
        s->bt_rx[s->bt_rx_used++] = *buf++;
        switch (s->bt_rx[0]) {
        case 4:
            header = 3;
            break;
        case 2:
            header = 5;
            break;
        case 3:
            header = 4;
            break;
        default:
            s->bt_rx_used = s->bt_rx_needed = 0;
            qemu_log_mask(LOG_GUEST_ERROR, "mv8787: invalid H4 RX type\n");
            continue;
        }
        if (s->bt_rx_used < header) {
            s->bt_rx_needed = header;
            continue;
        }
        if (s->bt_rx_used == header) {
            unsigned payload = s->bt_rx[0] == 2 ? lduw_le_p(s->bt_rx + 3) :
                                                s->bt_rx[header - 1];
            s->bt_rx_needed = header + payload;
            if (s->bt_rx_needed + 3 > sizeof(s->rx[0])) {
                s->bt_rx_discard = payload;
                s->bt_rx_used = s->bt_rx_needed = 0;
                continue;
            }
        }
        if (s->bt_rx_used == s->bt_rx_needed) {
            mv_bt_packet(s, s->bt_function, s->bt_rx, s->bt_rx_used);
            s->bt_rx_used = s->bt_rx_needed = 0;
        }
    }
}

static void mv_bt_event(void *opaque, QEMUChrEvent event)
{
    MV8787State *s = opaque;
    if (event == CHR_EVENT_OPENED || event == CHR_EVENT_CLOSED) {
        s->bt_open = event == CHR_EVENT_OPENED;
        s->bt_rx_used = s->bt_rx_needed = s->bt_rx_discard = s->bt_tx_used = 0;
        if (s->bt_watch) {
            g_source_remove(s->bt_watch);
            s->bt_watch = 0;
        }
        if (!s->bt_open && s->fw_ready) {
            const uint8_t failure[] = {4, 0x10, 1, 1};
            mv_bt_packet(s, s->bt_function, failure, sizeof(failure));
        }
    }
}

/* Validate Marvell scan TLVs and report the single open infrastructure BSS. */
static int mv_scan_match(MV8787State *s, const uint8_t *p, unsigned len)
{
    bool match = true;
    if (len < 7) {
        return -1;
    }
    if (p[0] != 1 && p[0] != 3) {
        match = false;
    }
    if (memcmp(p + 1, "\0\0\0\0\0\0", 6) && memcmp(p + 1, s->bssid.a, 6)) {
        match = false;
    }
    for (unsigned off = 7; off < len;) {
        unsigned tag, n;
        if (len - off < 4) {
            return -1;
        }
        tag = lduw_le_p(p + off); n = lduw_le_p(p + off + 2); off += 4;
        if (n > len - off) {
            return -1;
        }
        if (tag == 0 && n &&
            (n != strlen(s->ssid) || memcmp(p + off, s->ssid, n))) {
            match = false;
        } else if (tag == 0x101) {
            bool channel = false;
            if (n % 7) {
                return -1;
            }
            for (unsigned i = 0; i < n; i += 7) {
                channel |= p[off + i + 1] == s->channel;
            }
            match &= channel;
        }
        off += n;
    }
    return match && mv_wifi_link(s);
}

static unsigned mv_scan_response(MV8787State *s, uint8_t *out)
{
    uint8_t *beacon = out + 17, *ie = beacon + 19;
    unsigned n = strlen(s->ssid), len;
    memcpy(beacon, s->bssid.a, 6);
    beacon[6] = 40; /* -40 dBm */
    stw_le_p(beacon + 15, 100); /* beacon interval */
    stw_le_p(beacon + 17, 0x21); /* ESS, short preamble; no privacy */
    *ie++ = 0; *ie++ = n; memcpy(ie, s->ssid, n); ie += n;
    *ie++ = 1; *ie++ = 8;
    memcpy(ie, "\x82\x84\x8b\x96\x0c\x12\x18\x24", 8); ie += 8;
    *ie++ = 3; *ie++ = 1; *ie++ = s->channel;
    len = ie - beacon;
    stw_le_p(out + 12, len + 2); out[14] = 1;
    stw_le_p(out + 15, len);
    return len + 13;
}

static void mv_hci(MV8787State *s, unsigned f, const uint8_t *p, unsigned n)
{
    uint8_t out[280] = {0};
    uint8_t *r = out + 9;
    unsigned len = 1;
    if (n < 7 || p[3] != 1 || n < 7u + p[6]) {
        return;
    }
    unsigned op = lduw_le_p(p + 4);
    fprintf(stderr, "mv8787: HCI %04x plen=%u\n", op, p[6]);
    static const struct { uint16_t read, write; uint8_t len; } config[] = {
        {0x080e, 0x080f, 2}, {0x0c15, 0x0c16, 2}, {0x0c17, 0x0c18, 2},
        {0x0c1b, 0x0c1c, 4}, {0x0c1d, 0x0c1e, 4}, {0x0c1f, 0x0c20, 1},
        {0x0c21, 0x0c22, 1}, {0x0c23, 0x0c24, 3}, {0x0c25, 0x0c26, 2},
        {0x0c29, 0x0c2a, 1}, {0x0c2b, 0x0c2c, 1}, {0x0c2e, 0x0c2f, 1},
        {0x0c3b, 0x0c3c, 1}, {0x0c3d, 0x0c3e, 1}, {0x0c42, 0x0c43, 1},
        {0x0c44, 0x0c45, 1}, {0x0c46, 0x0c47, 1}, {0x0c48, 0x0c49, 1},
        {0x0c5a, 0x0c5b, 1}, {0, 0x0c31, 1}, {0, 0x0c33, 7},
        {0, 0x0c6d, 2},
    };
    for (unsigned i = 0; i < ARRAY_SIZE(config); i++) {
        if ((config[i].read && op == config[i].read) || op == config[i].write) {
            unsigned expected = op == config[i].write ? config[i].len : 0;
            if (p[6] != expected) {
                r[0] = 0x12;
            } else if (expected) {
                memcpy(s->hci_config[i], p + 7, expected);
            } else {
                memcpy(r + 1, s->hci_config[i], config[i].len);
                len += config[i].len;
            }
            goto complete;
        }
    }
    switch (op) {
    case 0x0401: /* Inquiry: a powered empty radio has no remote devices. */
        if (p[6] != 5 || !p[10] || p[10] > 0x30) {
            r[0] = 0x12; break;
        }
        /* Command Status precedes the asynchronous Inquiry Complete event. */
        {
            const uint8_t status[] = {4, 0x0f, 4, 0, 1, 1, 4};
            const uint8_t done[] = {4, 1, 1, 0};
            mv_bt_packet(s, f, status, sizeof(status));
            mv_bt_packet(s, f, done, sizeof(done));
        }
        return;
    case 0x0402: /* Inquiry Cancel (the empty scan has already completed). */
        r[0] = 0x0c; break;
    case 0x0c03: /* Reset */
        if (p[6]) { r[0] = 0x12; break; }
        s->scan_enable = 0;
        memset(s->hci_config, 0, sizeof(s->hci_config));
        memset(s->inquiry_response, 0, sizeof(s->inquiry_response));
        s->inquiry_tx_power = 0;
        break;
    case 0x0c51: /* Read Extended Inquiry Response */
        if (p[6]) { r[0] = 0x12; break; }
        memcpy(r + 1, s->inquiry_response, sizeof(s->inquiry_response));
        len = 1 + sizeof(s->inquiry_response);
        break;
    case 0x0c52: /* Write Extended Inquiry Response (FEC flag + 240 bytes) */
        if (p[6] != sizeof(s->inquiry_response) || p[7] > 1) {
            r[0] = 0x12; break;
        }
        memcpy(s->inquiry_response, p + 7, sizeof(s->inquiry_response));
        break;
    case 0x0c58: /* Read Inquiry Response Transmit Power Level, dBm */
        if (p[6]) { r[0] = 0x12; break; }
        r[1] = s->inquiry_tx_power; len = 2;
        break;
    case 0x0c59: /* Write Inquiry Transmit Power Level */
        if (p[6] != 1 || (int8_t)p[7] < -70 || (int8_t)p[7] > 20) {
            r[0] = 0x12; break;
        }
        s->inquiry_tx_power = (int8_t)p[7];
        break;
    case 0xfc0f: /* Marvell firmware revision, consumed by QNX health query */
        if (p[6]) { r[0] = 0x12; break; }
        stl_le_p(r + 1, 0x010e0000); len = 5;
        break;
    case 0x1001: /* Read Local Version Information */
        r[1] = 5; stw_le_p(r + 2, 1); r[4] = 5;
        stw_le_p(r + 5, 72); stw_le_p(r + 7, 0x8787); len = 9;
        break;
    case 0x1002: /* Read Supported Commands */
        len = 65;
        /* Inquiry/cancel, reset, event mask, name, scan and local queries. */
        r[1] = 0x03;
        r[1 + 5] = 0xc0; r[1 + 6] = 0x03;
        r[1 + 7] = 0x0c; r[1 + 14] = 0xf8; r[1 + 15] = 0x02;
        break;
    case 0x1003: /* Read Local Supported Features: basic BR/EDR only */
        len = 9; break;
    case 0x1004: /* Extended features */
        r[1] = p[6] ? p[7] : 0; len = 11; break;
    case 0x1005: /* ACL/SCO buffers */
        stw_le_p(r + 1, 1021); r[3] = 64;
        stw_le_p(r + 4, 8); stw_le_p(r + 6, 4); len = 8; break;
    case 0x1009: /* Local address, locally administered emulated identity */
        memcpy(r + 1, "\x01\x87\x87\x02\x00\x02", 6); len = 7; break;
    case 0x0c14: /* Read local name */
        memcpy(r + 1, s->local_name, 248); len = 249; break;
    case 0x0c13: /* Write local name */
        if (p[6] != 248) { r[0] = 0x12; break; }
        memcpy(s->local_name, p + 7, 248); break;
    case 0x0c19: r[1] = s->scan_enable; len = 2; break;
    case 0x0c1a:
        if (p[6] != 1 || p[7] > 3) { r[0] = 0x12; break; }
        s->scan_enable = p[7]; break;
    case 0x0c0d: /* Read stored keys: empty emulated key store */
        if (p[6] != 7) { r[0] = 0x12; break; }
        len = 5; break;
    case 0x0c12: /* Delete stored keys */
        if (p[6] != 7) { r[0] = 0x12; break; }
        len = 3; break;
    case 0x0c38: r[1] = 1; len = 2; break;
    case 0x0c39:
        r[1] = 1; r[2] = 0x33; r[3] = 0x8b; r[4] = 0x9e; len = 5; break;
    case 0x0c05: /* Event filters */
        if (p[6] != 1 || p[7] != 0) { r[0] = 0x12; }
        break;
    case 0x0c63:
    case 0x0c01: /* Event mask; no unsolicited radio events yet */
        if (p[6] != 8) { r[0] = 0x12; }
        break;
    default:
        r[0] = 1; /* Unknown HCI Command, never silently claim support */
        break;
    }
complete:
    stl_le_p(out, len + 9); /* RX includes header; QNX TX excludes it. */
    out[3] = 4;
    out[4] = 0x0e; out[5] = len + 3; out[6] = 1;
    stw_le_p(out + 7, op);
    mv_enqueue(s, f, out, len + 9);
}

static void mv_runtime(MV8787State *s, unsigned f, const uint8_t *p, unsigned n)
{
    if (f != 1 && getenv("MHI2_SDIO_TRACE")) {
        fprintf(stderr, "mv8787: BT packet fn=%u n=%u data=", f, n);
        for (unsigned i = 0; i < MIN(n, 48); i++) { fprintf(stderr, "%02x", p[i]); }
        fprintf(stderr, "\n");
    }
    unsigned plen = n >= 4 ? lduw_le_p(p) : 0;
    if (f != 1 && n >= 4 && plen <= n - 4) {
        if (qemu_chr_fe_backend_connected(&s->bt_chr)) {
            mv_bt_send(s, f, p, plen + 4);
        } else {
            mv_hci(s, f, p, plen + 4);
        }
        return;
    }
    if (plen < 4 || plen > n) {
        error_report("mv8787: malformed runtime packet fn=%u len=%u", f, plen);
        return;
    }
    if (lduw_le_p(p + 2) == 0) {
        /* Multi-port TX concatenates SDIO frames padded to function blocks. */
        unsigned block = lduw_le_p(&s->regs[0][0x110]);
        unsigned off = 0;
        if (!block) {
            block = 256;
        }
        while (n - off >= 4) {
            unsigned length = lduw_le_p(p + off);
            if (length < 4 || length > n - off || lduw_le_p(p + off + 2)) {
                break;
            }
            mv_wifi_tx(s, p + off + 4, length - 4);
            unsigned padded = DIV_ROUND_UP(length, block) * block;
            if (padded > n - off) {
                break;
            }
            off += padded;
        }
        s->regs[1][3] |= 2;
        mv_irq(s);
        return;
    }
    if (plen < 12 || lduw_le_p(p + 2) != 1) {
        return;
    }
    unsigned cmd = lduw_le_p(p + 4);
    unsigned size = lduw_le_p(p + 6);
    uint8_t out[4096] = {0};
    if (size < 8 || size + 4 > plen || size + 4 > sizeof(out)) {
        return;
    }
    memcpy(out, p, size + 4);
    stw_le_p(out + 4, cmd | 0x8000);
    stw_le_p(out + 10, 0);
    fprintf(stderr, "mv8787: WLAN cmd=%04x size=%u data=", cmd, size);
    for (unsigned i = 12; i < MIN(plen, 80); i++) {
        fprintf(stderr, "%02x", p[i]);
    }
    fprintf(stderr, "\n");
    switch (cmd) {
    case 0x0006: { /* Legacy scan, used by the 8787 firmware API */
        int match = mv_scan_match(s, p + 12, size - 8);
        memset(out + 12, 0, sizeof(out) - 12);
        if (match < 0) {
            stw_le_p(out + 10, 2);
            size = 8;
        } else {
            size = match ? mv_scan_response(s, out) : 11;
        }
        break;
    }
    case 0x0012: { /* Associate with the emulated open AP */
        bool ok = size >= 21 && mv_wifi_link(s) &&
                  !memcmp(p + 12, s->bssid.a, 6) && !(lduw_le_p(p + 18) & 0x10);
        /* Reject malformed or foreign SSID parameters. */
        for (unsigned off = 25; ok && off < size + 4;) {
            if (size + 4 - off < 4) {
                ok = false;
                break;
            }
            unsigned tag = lduw_le_p(p + off), n = lduw_le_p(p + off + 2);
            off += 4;
            if (n > size + 4 - off) {
                ok = false;
                break;
            }
            if (tag == 0 &&
                (n != strlen(s->ssid) || memcmp(p + off, s->ssid, n))) {
                ok = false;
            }
            off += n;
        }
        memset(out + 12, 0, sizeof(out) - 12);
        stw_le_p(out + 12, 0x21);
        stw_le_p(out + 14, ok ? 0 : 1);
        stw_le_p(out + 16, ok ? 0xc001 : 0);
        size = 14;
        s->associated = ok;
        s->station_bss = (lduw_le_p(p + 8) >> 8) & 0xf;
        break;
    }
    case 0x0024: /* Deauthenticate */
        if (size != 16) {
            stw_le_p(out + 10, 2);
            break;
        }
        s->associated = false;
        qemu_purge_queued_packets(qemu_get_queue(s->nic));
        break;
    case 0x001d: /* RF channel */
        if (size < 12 || lduw_le_p(p + 12) > 1 ||
            (lduw_le_p(p + 12) && lduw_le_p(p + 14) != s->channel)) {
            stw_le_p(out + 10, 2); break;
        }
        stw_le_p(out + 14, s->channel);
        break;
    case 0x00a4: /* RSSI/NF statistics */
        if (size < 14) {
            stw_le_p(out + 10, 2);
            break;
        }
        size = 38;
        for (unsigned i = 0; i < 8; i++) {
            stw_le_p(out + 18 + 2 * i, (uint16_t)(i & 1 ? -90 : -40));
        }
        break;

    case 0x00a9: s->wlan_initialized = true; break;
    case 0x00aa:
        s->wlan_initialized = false;
        s->associated = false;
        break;
    case 0x0010: { /* Multicast receive address list */
        if (size < 12 || lduw_le_p(p + 12) != 1) {
            stw_le_p(out + 10, 2); break;
        }
        unsigned count = lduw_le_p(p + 14);
        if (count > 32 || size < 12 + count * 6) {
            stw_le_p(out + 10, 2); break;
        }
        s->multicast_count = count;
        memcpy(s->multicast, p + 16, count * 6);
        break;
    }
    case 0x004d: { /* Interface MAC address */
        unsigned bss = (lduw_le_p(p + 8) >> 8) & 3;
        if (size < 16 || lduw_le_p(p + 12) > 1) {
            stw_le_p(out + 10, 2); break;
        }
        if (lduw_le_p(p + 12)) {
            if (p[14] & 1) { stw_le_p(out + 10, 2); break; }
            memcpy(s->mac_address[bss], p + 14, 6);
        }
        memcpy(out + 14, s->mac_address[bss], 6);
        break;
    }
    case 0x0016: { /* SNMP MIB configuration */
        if (size < 16) { stw_le_p(out + 10, 2); break; }
        unsigned action = lduw_le_p(p + 12), oid = lduw_le_p(p + 14);
        if (action > 1 || oid >= ARRAY_SIZE(s->mib) ||
            lduw_le_p(p + 16) != 2) { stw_le_p(out + 10, 2); break; }
        if (action) { s->mib[oid] = lduw_le_p(p + 18); }
        stw_le_p(out + 18, s->mib[oid]);
        break;
    }
    case 0x00b0: { /* uAP system configuration TLVs */
        if (size < 10 || lduw_le_p(p + 12) > 1) {
            stw_le_p(out + 10, 2); break;
        }
        bool set = lduw_le_p(p + 12) == 1;
        unsigned off = 14;
        /* A query without a TLV asks for current configuration, not an empty
         * success. This radio has no active BSS until a backend can start it. */
        if (!set && size == 10) {
            stw_le_p(out + off, 0x12b); stw_le_p(out + off + 2, 6);
            memcpy(out + off + 4, s->mac_address[1], 6); off += 10;
            stw_le_p(out + off, 0x155); stw_le_p(out + off + 2, 4);
            stl_le_p(out + off + 4, s->max_stations); off += 8;
            stw_le_p(out + off, 0x193); stw_le_p(out + off + 2, 2);
            stw_le_p(out + off + 4, 0); off += 6;
            for (unsigned i = 0; i < ARRAY_SIZE(s->uap); i++) {
                if (!s->uap[i].tag) { continue; }
                unsigned len = s->uap[i].len;
                if (off + 4 + len > sizeof(out)) {
                    stw_le_p(out + 10, 2); break;
                }
                stw_le_p(out + off, s->uap[i].tag);
                stw_le_p(out + off + 2, len);
                memcpy(out + off + 4, s->uap[i].data, len); off += 4 + len;
            }
            size = off - 4;
            break;
        }
        /* Reject malformed lists before changing any configuration. */
        while (off < size + 4) {
            if (off + 4 > size + 4 || lduw_le_p(p + off + 2) > 256 ||
                off + 4 + lduw_le_p(p + off + 2) > size + 4) {
                stw_le_p(out + 10, 2); break;
            }
            off += 4 + lduw_le_p(p + off + 2);
        }
        if (lduw_le_p(out + 10)) { break; }
        off = 14;
        while (off + 4 <= size + 4) {
            unsigned tag = lduw_le_p(p + off), len = lduw_le_p(p + off + 2);
            if (len > 256 || off + 4 + len > size + 4) {
                stw_le_p(out + 10, 2); break;
            }
            if (tag == 0x12b && len == 6) {
                if (set && (p[off + 4] & 1)) {
                    stw_le_p(out + 10, 2); break;
                }
                if (set) { memcpy(s->mac_address[1], p + off + 4, 6); }
                memcpy(out + off + 4, s->mac_address[1], 6);
            } else if (tag == 0x193) { /* BSS status: stopped */
                if (set || len != 2) { stw_le_p(out + 10, 2); break; }
                stw_le_p(out + off + 4, 0);
            } else if (tag == 0x155) { /* QNX uses a padded 32-bit field. */
                if (len != 2 && len != 4) { stw_le_p(out + 10, 2); break; }
                unsigned count = len == 2 ? lduw_le_p(p + off + 4) :
                                             ldl_le_p(p + off + 4);
                if (set) {
                    if (!count || count > 16) { stw_le_p(out + 10, 2); break; }
                    s->max_stations = count;
                }
                memset(out + off + 4, 0, len);
                stw_le_p(out + off + 4, s->max_stations);
            } else {
                unsigned slot;
                for (slot = 0; slot < ARRAY_SIZE(s->uap); slot++) {
                    if (s->uap[slot].tag == tag || !s->uap[slot].tag) { break; }
                }
                if (slot == ARRAY_SIZE(s->uap) || !tag) {
                    stw_le_p(out + 10, 2); break;
                }
                if (set) {
                    s->uap[slot].tag = tag; s->uap[slot].len = len;
                    memcpy(s->uap[slot].data, p + off + 4, len);
                } else if (s->uap[slot].tag == tag && s->uap[slot].len == len) {
                    memcpy(out + off + 4, s->uap[slot].data, len);
                } else {
                    stw_le_p(out + 10, 2); break;
                }
            }
            off += 4 + len;
        }
        break;
    }
    case 0x00e0: { /* Robust WLAN/BT coexistence policy (coex.cfg). */
        static const unsigned lengths[] = { 20, 28, 24 };
        uint8_t next[3][28];
        unsigned off = 16;
        if (size < 12 || lduw_le_p(p + 12) > 1) {
            stw_le_p(out + 10, 2); break;
        }
        bool set = lduw_le_p(p + 12) == 1;
        memcpy(next, s->coex, sizeof(next));
        if (!set && size == 12) {
            for (unsigned i = 0; i < 3; i++) {
                stw_le_p(out + off, 0x16c + i);
                stw_le_p(out + off + 2, lengths[i]);
                memcpy(out + off + 4, s->coex[i], lengths[i]);
                off += 4 + lengths[i];
            }
            size = off - 4;
            break;
        }
        while (off < size + 4) {
            if (off + 4 > size + 4) { stw_le_p(out + 10, 2); break; }
            unsigned tag = lduw_le_p(p + off), len = lduw_le_p(p + off + 2);
            if (tag < 0x16c || tag > 0x16e || len != lengths[tag - 0x16c] ||
                off + 4 + len > size + 4) {
                stw_le_p(out + 10, 2); break;
            }
            if (set) { memcpy(next[tag - 0x16c], p + off + 4, len); }
            else { memcpy(out + off + 4, s->coex[tag - 0x16c], len); }
            off += 4 + len;
        }
        if (set && !lduw_le_p(out + 10)) {
            memcpy(s->coex, next, sizeof(next));
        }
        break;
    }
    case 0x001e: /* Transmit power, dBm */
        if (size < 14 || lduw_le_p(p + 12) > 1) {
            stw_le_p(out + 10, 2); break;
        }
        if (lduw_le_p(p + 12)) {
            int power = (int16_t)lduw_le_p(p + 14);
            if (power < 0 || power > 20) { stw_le_p(out + 10, 2); break; }
            s->tx_power = power;
        }
        stw_le_p(out + 14, s->tx_power); out[16] = 20; out[17] = 0;
        break;
    case 0x0083: /* IBSS coalescing policy; no other stations in the radio model */
        if (size < 12 || lduw_le_p(p + 12) > 1) {
            stw_le_p(out + 10, 2); break;
        }
        if (lduw_le_p(p + 12)) {
            s->ibss_coalescing = lduw_le_p(p + 14) != 0;
        }
        stw_le_p(out + 14, s->ibss_coalescing);
        break;
    case 0x00df: /* A-MSDU aggregation configuration */
        if (size < 14 || lduw_le_p(p + 12) > 1) {
            stw_le_p(out + 10, 2); break;
        }
        if (lduw_le_p(p + 12)) {
            s->amsdu_enabled = lduw_le_p(p + 14) != 0;
        }
        stw_le_p(out + 14, s->amsdu_enabled);
        stw_le_p(out + 16, s->tx_buffer_size);
        break;
    case 0x0028: /* Receive filter */
        if (size != 12) { stw_le_p(out + 10, 2); break; }
        s->mac_filter = ldl_le_p(p + 12);
        break;
    case 0x00d9: { /* Configure transmit buffer size */
        if (size < 16) { stw_le_p(out + 10, 2); break; }
        unsigned action = lduw_le_p(p + 12);
        unsigned requested = lduw_le_p(p + 14);
        if (action == 1 && requested >= 512 && requested <= 4096) {
            s->tx_buffer_size = requested;
        } else if (action != 0) {
            stw_le_p(out + 10, 2); break;
        }
        stw_le_p(out + 14, s->tx_buffer_size);
        stw_le_p(out + 16, 16);
        break;
    }
    case 0x00e4: { /* Enhanced power save state; always wake on host access */
        if (size < 12) { stw_le_p(out + 10, 2); break; }
        unsigned action = lduw_le_p(p + 12);
        unsigned bitmap = lduw_le_p(p + 14);
        if (action == 0xff) {
            s->power_save_bitmap |= bitmap;
        } else if (action == 0xfe) {
            s->power_save_bitmap &= ~bitmap;
        } else if (action != 0) {
            stw_le_p(out + 10, 2); break;
        }
        stw_le_p(out + 14, s->power_save_bitmap);
        break;
    }
    case 0x00d6: { /* Query legacy PHY transmit rate scope */
        if (size < 12 || lduw_le_p(p + 12) != 0) {
            stw_le_p(out + 10, 2); break;
        }
        unsigned off = 16;
        while (off + 4 <= size + 4) {
            unsigned tag = lduw_le_p(out + off);
            unsigned len = lduw_le_p(out + off + 2);
            if (off + 4 + len > size + 4) {
                stw_le_p(out + 10, 2); break;
            }
            if (tag == 0x153 && len >= 4) {
                stw_le_p(out + off + 4, 0x000f); /* DSSS rates */
                stw_le_p(out + off + 6, 0x00ff); /* OFDM rates */
            }
            off += 4 + len;
        }
        break;
    }
    case 0x0003: { /* Get hardware specification */
        size = MAX(size, 72);
        uint8_t *h = out + 12;
        stw_le_p(h, 1); stw_le_p(h + 2, 1); stw_le_p(h + 6, 32);
        memcpy(h + 8, s->conf.macaddr.a, 6);
        stw_le_p(h + 14, 0x30); stw_le_p(h + 16, 1);
        stl_le_p(h + 18, 0x0e000001); /* Emulated firmware revision */
        stw_le_p(h + 43, 16); stw_le_p(h + 45, 4);
        break;
    }
    default:
        stw_le_p(out + 10, 2); /* Explicit unsupported-command response */
        break;
    }
    stw_le_p(out, size + 4); stw_le_p(out + 6, size);
    mv_enqueue(s, f, out, size + 4);
    if (s->associated) {
        qemu_flush_queued_packets(qemu_get_queue(s->nic));
    }
}

static uint8_t mv_read_reg(MV8787State *s, unsigned f, unsigned addr)
{
    if (f && (addr == 0x40 || addr == 0x41) && !s->fw_ready) {
        unsigned request = s->fw_remaining ? MIN(s->fw_remaining, 2048) : 16;
        return addr == 0x40 ? request : request >> 8;
    }
    uint8_t value = s->regs[f][addr];
    if (f && addr == 3) {
        s->regs[f][3] &= ~(value & s->regs[f][1]);
        mv_irq(s);
    }
    return value;
}

static void mv_write_reg(MV8787State *s, unsigned f, unsigned addr, uint8_t value)
{
    if (!f) {
        if (addr == 6 && (value & 8)) {
            mv_reset(DEVICE(s));
        } else if (addr == 2) {
            s->regs[0][2] = value & 0x0e;
            s->regs[0][3] = value & 0x0e;
        } else if (addr == 4 || addr == 7 || addr == 0x13 ||
                   (addr >= 0x10 && (addr & 0xff) >= 0x10 &&
                    (addr & 0xff) <= 0x11 && addr < 0x400)) {
            s->regs[0][addr] = value;
        }
    } else if (addr == 3) {
        s->regs[f][addr] &= value; /* Marvell interrupt W0C */
    } else {
        s->regs[f][addr] = value;
    }
    mv_irq(s);
}

static size_t mv_command(SDState *card, SDRequest *req,
                         uint8_t *resp, size_t respsz)
{
    MV8787State *s = MV8787(card);
    uint32_t arg = req->arg, result = 0;
    unsigned f = (arg >> 28) & 7, addr = (arg >> 9) & 0x1ffff;
    bool write = arg >> 31;
    if (g_getenv("MHI2_SDIO_TRACE") && s->trace_count++ < 800) {
        fprintf(stderr, "mv8787: CMD%u arg=%08x\n", req->cmd, arg);
    }
    switch (req->cmd) {
    case 0:
        mv_reset(DEVICE(s));
        return 0;
    case 5:
        result = 0xb0ff8000; /* Ready, three I/O functions, no memory */
        break;
    case 3:
        result = s->rca << 16;
        break;
    case 7:
        s->selected = (arg >> 16) == s->rca;
        result = s->selected ? 0x900 : 0x700;
        break;
    case 52:
    case 53:
        result = s->selected ? 0x2000 : 0x1000;
        if (f > 3) {
            result |= 0x200; /* R5 FUNCTION_NUMBER */
            break;
        }
        if (req->cmd == 52) {
            if (write) {
                mv_write_reg(s, f, addr, arg);
            }
            result |= (write && !(arg & (1u << 27))) ? (arg & 255) :
                      mv_read_reg(s, f, addr);
        } else {
            unsigned count = arg & 511;
            unsigned block = lduw_le_p(&s->regs[0][f * 256 + 0x10]);
            unsigned total = arg & (1 << 27) ? count * block : (count ? count : 512);
            if (!s->selected || (f && !(s->regs[0][3] & (1 << f))) ||
                !total || total > MV_MAX_TRANSFER ||
                ((arg & (1 << 26)) && addr + total > MV_SPACE)) {
                result |= 0x100; /* R5 OUT_OF_RANGE */
                break;
            }
            s->function = f;
            s->address = addr;
            s->increment = arg & (1 << 26);
            s->write = write;
            s->remaining = total;
            s->transferred = 0;
        }
        break;
    default:
        return 0;
    }
    if (respsz < 4) {
        return 0;
    }
    stl_be_p(resp, result);
    return 4;
}

static size_t mv_read(SDState *card, void *buf, size_t len)
{
    MV8787State *s = MV8787(card);
    uint8_t *p = buf;
    for (size_t i = 0; i < len; i++) {
        p[i] = 0;
        if (!s->remaining || s->write) {
            continue;
        }
        unsigned f = s->function;
        if (f && s->address >= MV_PORT) {
            if (s->rx_pos[f] < s->rx_len[f] &&
                (f != 1 || (s->address & 15) == s->wlan_rx_port)) {
                p[i] = s->rx[f][s->rx_pos[f]++];
            }
        } else {
            p[i] = mv_read_reg(s, f, s->address);
        }
        s->remaining--;
        if (!s->remaining && f && s->rx_pos[f] >= s->rx_len[f]) {
            s->rx_len[f] = s->rx_pos[f] = 0;
            s->regs[f][3] &= ~1;
            stw_le_p(&s->regs[f][4], 0);
            memset(&s->regs[f][8], 0, 32);
            s->regs[f][0x62] = 0;
            s->regs[f][0x30] &= ~2;
            mv_queue_next(s, f);
            mv_irq(s);
        }
        if (s->increment) {
            s->address = (s->address + 1) & (MV_SPACE - 1);
        }
    }
    return len;
}

static size_t mv_write(SDState *card, const void *buf, size_t len)
{
    MV8787State *s = MV8787(card);
    const uint8_t *p = buf;
    for (size_t i = 0; i < len; i++) {
        if (!s->remaining || !s->write) {
            continue;
        }
        if (s->function && s->address >= MV_PORT) {
            s->packet[s->transferred++] = p[i];
        } else {
            mv_write_reg(s, s->function, s->address, p[i]);
        }
        s->remaining--;
        if (s->increment) {
            s->address = (s->address + 1) & (MV_SPACE - 1);
        }
        if (!s->remaining && s->transferred) {
            if (!s->fw_ready) {
                /* SD block padding is not part of the ROM's requested data. */
                unsigned n = s->fw_remaining ? MIN(s->fw_remaining, 2048) : 16;
                for (unsigned j = 0; j < MIN(n, s->transferred); j++) {
                    mv_fw_byte(s, s->packet[j]);
                }
            } else {
                mv_runtime(s, s->function, s->packet, s->transferred);
            }
        }
    }
    return len;
}

static bool mv_data_ready(SDState *card)
{
    MV8787State *s = MV8787(card);
    return s->remaining && !s->write;
}
static bool mv_receive_ready(SDState *card)
{
    MV8787State *s = MV8787(card);
    return s->remaining && s->write;
}
static bool mv_inserted(SDState *card) { return true; }
static bool mv_readonly(SDState *card) { return false; }
static void mv_voltage(SDState *card, uint16_t voltage) { }
static void mv_realize(DeviceState *dev, Error **errp)
{
    MV8787State *s = MV8787(dev);
    if (!s->ssid[0] || strlen(s->ssid) > 32 || !s->channel || s->channel > 11 ||
        is_multicast_ether_addr(s->bssid.a)) {
        error_setg(errp, "mv8787: SSID must be 1..32 bytes, "
                   "channel 1..11, BSSID unicast");
        return;
    }
    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    if (!memcmp(s->bssid.a, "\0\0\0\0\0\0", 6)) {
        memcpy(s->bssid.a, "\x52\x54\x00\x87\x87\x01", 6);
    }
    s->nic = qemu_new_nic(&mv_net_info, &s->conf, TYPE_MV8787, dev->id,
                          &dev->mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
    mv_reset(dev);
    qemu_chr_fe_set_handlers(&s->bt_chr, mv_bt_can_read, mv_bt_read,
                             mv_bt_event, NULL, s, NULL, true);
    sdbus_set_inserted(mv_bus(s), true);
}
static void mv_unrealize(DeviceState *dev)
{
    MV8787State *s = MV8787(dev);
    mv_reset(dev);
    qemu_chr_fe_deinit(&s->bt_chr, false);
    qemu_del_nic(s->nic);
    s->nic = NULL;
}

static const Property mv_properties[] = {
    DEFINE_NIC_PROPERTIES(MV8787State, conf),
    DEFINE_PROP_STRING("ssid", MV8787State, ssid),
    DEFINE_PROP_MACADDR("bssid", MV8787State, bssid),
    DEFINE_PROP_UINT8("channel", MV8787State, channel, 6),
    DEFINE_PROP_CHR("bluetooth-chardev", MV8787State, bt_chr),
};

static void mv_init(Object *obj)
{
    MV8787State *s = MV8787(obj);
    s->ssid = g_strdup("QEMU Wi-Fi");
}

static const VMStateDescription mv_vmstate = {
    .name = TYPE_MV8787,
    .unmigratable = true,
};
static void mv_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SDCardClass *sc = SD_DEVICE_CLASS(klass);
    dc->realize = mv_realize;
    dc->unrealize = mv_unrealize;
    device_class_set_props(dc, mv_properties);
    dc->vmsd = &mv_vmstate;
    dc->desc = "Marvell 8787 SDIO combo controller (local experiment)";
    device_class_set_legacy_reset(dc, mv_reset);
    sc->do_command = mv_command;
    sc->read_data = mv_read;
    sc->write_data = mv_write;
    sc->data_ready = mv_data_ready;
    sc->receive_ready = mv_receive_ready;
    sc->get_inserted = mv_inserted;
    sc->get_readonly = mv_readonly;
    sc->set_voltage = mv_voltage;
}
static const TypeInfo mv_type = {
    .name = TYPE_MV8787,
    .parent = TYPE_SD_DEVICE,
    .instance_size = sizeof(MV8787State),
    .class_init = mv_class_init,
    .instance_init = mv_init,
};
static void mv_register(void) { type_register_static(&mv_type); }
type_init(mv_register)
