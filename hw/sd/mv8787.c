/*
 * Local Marvell 8787 SDIO companion experiment.
 * Register protocol: Marvell's Linux mwifiex/btmrvl drivers and the supplied
 * QNX io-sdiorm driver. No firmware executable or startup policy is changed.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "hw/sd/sd.h"
#include "hw/core/qdev-properties.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "qemu/module.h"
#include "migration/vmstate.h"

#define TYPE_MV8787 "mv8787-sdio"
OBJECT_DECLARE_SIMPLE_TYPE(MV8787State, MV8787)
#define MV_SPACE 0x20000
#define MV_PORT  0x10000
#define MV_MAX_TRANSFER (512 * 2048)

struct MV8787State {
    DeviceState parent_obj;
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
        memcpy(s->mac_address[i], "\x02\x00\x02\x87\x87\x01", 6);
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
            fprintf(stderr, "mv8787: firmware received: %u bytes, %u records\n",
                    s->fw_bytes, s->fw_records);
        }
    }
}

static void mv_enqueue(MV8787State *s, unsigned f, const uint8_t *p, unsigned len)
{
    if (len > sizeof(s->rx[f]) || s->rx_len[f]) {
        error_report("mv8787: RX queue overflow fn=%u len=%u", f, len);
        return;
    }
    memcpy(s->rx[f], p, len);
    s->rx_len[f] = len;
    s->rx_pos[f] = 0;
    s->regs[f][3] |= 3; /* Packet available and transmit slot free */
    stw_le_p(&s->regs[f][4], 1);
    stw_le_p(&s->regs[f][8], len);
    s->regs[f][0x62] = (len + 3) / 4;
    s->regs[f][0x63] = 2;
    s->regs[f][0x30] |= 2;
    mv_irq(s);
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
        /* Reset, event mask, name, scan, version, buffer/address queries. */
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
        mv_hci(s, f, p, plen + 4);
        return;
    }
    if (plen < 4 || plen > n) {
        error_report("mv8787: malformed runtime packet fn=%u len=%u", f, plen);
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
    case 0x00a9: s->wlan_initialized = true; break;
    case 0x00aa: s->wlan_initialized = false; break;
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
        memcpy(h + 8, "\x02\x00\x02\x87\x87\x01", 6);
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
            if (s->rx_pos[f] < s->rx_len[f]) {
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
            stw_le_p(&s->regs[f][8], 0);
            s->regs[f][0x62] = 0;
            s->regs[f][0x30] &= ~2;
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
    mv_reset(dev);
    sdbus_set_inserted(mv_bus(MV8787(dev)), true);
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
};
static void mv_register(void) { type_register_static(&mv_type); }
type_init(mv_register)
