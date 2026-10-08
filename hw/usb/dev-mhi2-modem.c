/*
 * MHI2 Cinterion ALS6 USB modem experiment.
 * The original QNX usblauncher owns enumeration, serial ports and ECM setup.
 * Data is carried by a normal QEMU network backend, not a mobile RF network.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/cutils.h"
#include "qemu/bswap.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/core/qdev-properties.h"
#include "hw/usb/usb.h"
#include "migration/vmstate.h"
#include "net/net.h"
#include "desc.h"
#include "trace.h"

#define TYPE_MHI2_MODEM "usb-mhi2-modem"
OBJECT_DECLARE_SIMPLE_TYPE(MHI2Modem, MHI2_MODEM)
struct MHI2Modem {
    USBDevice parent_obj;
    NICConf conf;
    NICState *nic;
    bool lte, radio, attached, active, echo[4], overflow[4];
    unsigned creg, cgreg, cereg, notify;
    char command[4][1024];
    unsigned command_len[4];
    GByteArray *serial[4];
    GQueue ethernet;
    size_t rx_offset;
    uint8_t tx[2048];
    size_t tx_len;
    char apn[101];
    uint32_t indicators;
    uint8_t gpio;
    char phonebook[3];
};
#define MODEM_EP(n, direction) { \
    .bEndpointAddress = (direction) | (n), \
    .bmAttributes = USB_ENDPOINT_XFER_BULK, .wMaxPacketSize = 512 \
}
#define MODEM_SERIAL(i) { \
    .bInterfaceNumber = (i), .bNumEndpoints = 2, \
    .bInterfaceClass = 0xff, .bInterfaceSubClass = 0xff, \
    .bInterfaceProtocol = 0xff, \
    .eps = (USBDescEndpoint[]) { \
        MODEM_EP((i) + 1, 0x80), MODEM_EP((i) + 1, 0) \
    } \
}

static const USBDescIface modem_interfaces[] = {
    MODEM_SERIAL(0), MODEM_SERIAL(1), MODEM_SERIAL(2), MODEM_SERIAL(3),
    {
        .bInterfaceNumber = 4,
        .bNumEndpoints = 1,
        .bInterfaceClass = 0xff,
        .bInterfaceSubClass = 0xff,
        .bInterfaceProtocol = 0xff,
        .ndesc = 3,
        .descs = (USBDescOther[]) {
            { .data = (uint8_t[]) { 5, 0x24, 0, 0x10, 1 } },
            { .data = (uint8_t[]) { 5, 0x24, 6, 4, 5 } },
            { .data = (uint8_t[]) { 13, 0x24, 15, 4, 0, 0, 0, 0,
                                   0xea, 5, 0, 0, 0 } },
        },
        .eps = (USBDescEndpoint[]) {
            {
                .bEndpointAddress = 0x87,
                .bmAttributes = USB_ENDPOINT_XFER_INT,
                .wMaxPacketSize = 16,
                .bInterval = 8,
            },
        },
    },
    { .bInterfaceNumber = 5, .bInterfaceClass = 0x0a },
    {
        .bInterfaceNumber = 5,
        .bAlternateSetting = 1,
        .bNumEndpoints = 2,
        .bInterfaceClass = 0x0a,
        .eps = (USBDescEndpoint[]) { MODEM_EP(6, 0x80), MODEM_EP(6, 0) },
    },
};
static const USBDescStrings modem_strings = {
    [1] = "Cinterion",
    [2] = "ALS6 virtual modem",
    [3] = "QEMU-MHI2-MODEM",
    [4] = "525400123457",
};
static const USBDescDevice modem_device = {
    .bcdUSB = 0x0200,
    .bMaxPacketSize0 = 64,
    .bNumConfigurations = 1,
    .confs = (USBDescConfig[]) {
        {
            .bNumInterfaces = 6,
            .bConfigurationValue = 1,
            .bmAttributes = USB_CFG_ATT_ONE | USB_CFG_ATT_SELFPOWER,
            .nif = ARRAY_SIZE(modem_interfaces),
            .ifs = modem_interfaces,
        },
    },
};
static const USBDesc modem_desc = {
    .id = {
        .idVendor = 0x1e2d, .idProduct = 0x0060, .bcdDevice = 0x0100,
        .iManufacturer = 1, .iProduct = 2, .iSerialNumber = 3,
    },
    .high = &modem_device,
    .str = modem_strings,
};

static void modem_reply(MHI2Modem *s, unsigned port, const char *text)
{
    GByteArray *q = s->serial[port];
    size_t len = strlen(text);
    if (len + q->len <= 65536) {
        g_byte_array_append(q, (const uint8_t *)text, len);
        usb_wakeup(usb_ep_get(&s->parent_obj, USB_TOKEN_IN, port + 1), 0);
    }
}

static void modem_link(MHI2Modem *s, bool active)
{
    s->active =
        active && s->radio && s->attached && !qemu_get_queue(s->nic)->link_down;
    s->notify = 3;
    usb_wakeup(usb_ep_get(&s->parent_obj, USB_TOKEN_IN, 7), 0);
    if (s->active) {
        qemu_flush_queued_packets(qemu_get_queue(s->nic));
    } else {
        g_queue_clear_full(&s->ethernet, (GDestroyNotify)g_bytes_unref);
        s->rx_offset = 0;
        s->tx_len = 0;
        qemu_purge_queued_packets(qemu_get_queue(s->nic));
    }
}

static const char *const indicator_names[] = {
    "simlocal", "simstatus", "iccid", "imsi",      "service", "roam",
    "psinfo",   "smsfull",   "ceer",  "steerroam", "simdata",
};

static void modem_indicator(MHI2Modem *s, unsigned port, unsigned i)
{
    const char *values[] = {
        "1,0",
        "5",
        "8988211000000000000",
        "001010123456789",
        s->radio ? "1" : "0",
        "0",
        s->radio ? (s->lte ? "16" : "4") : "0",
        "0",
        "0",
        NULL,
        NULL,
    };
    g_autofree char *reply =
        g_strdup_printf("^SIND: %s,%u%s%s\r\n", indicator_names[i],
                        !!(s->indicators & (1U << i)), values[i] ? "," : "",
                        values[i] ? values[i] : "");
    modem_reply(s, port, reply);
}

static bool modem_at(MHI2Modem *s, unsigned port, const char *c)
{
    g_autofree char *answer = NULL;
    unsigned value, cid;
    int end = 0;
    if (!strcmp(c, "")) {
        return true;
    } else if (!strcmp(c, "E0")) {
        s->echo[port] = false;
    } else if (!strcmp(c, "E1")) {
        s->echo[port] = true;
    } else if (!strcmp(c, "I") || !strcmp(c, "I1")) {
        answer = g_strdup("Cinterion\r\nALS6A\r\nREVISION 04.000\r\nA-REVISION "
                          "00.000.01\r\n");
    } else if (!strcmp(c, "I51")) {
        answer = g_strdup("SBL1 01\r\nSBL2 01\r\n");
    } else if (!strcmp(c, "I255")) {
        answer = g_strdup("HW 01\r\n");
    } else if (!strcmp(c, "+CGMI") || !strcmp(c, "+GMI")) {
        answer = g_strdup("Cinterion\r\n");
    } else if (!strcmp(c, "+CGMM") || !strcmp(c, "+GMM")) {
        answer = g_strdup("ALS6A\r\n");
    } else if (!strcmp(c, "+CGMR") || !strcmp(c, "+GMR")) {
        answer = g_strdup("REVISION 04.000\r\n");
    } else if (!strcmp(c, "+CGSN")) {
        answer = g_strdup("000000000000000\r\n");
    } else if (!strcmp(c, "+CIMI")) {
        answer = g_strdup("001010123456789\r\n");
    } else if (!strcmp(c, "^SCID")) {
        answer = g_strdup("^SCID: 8988211000000000000\r\n");
    } else if (!strcmp(c, "+CPIN?")) {
        answer = g_strdup("+CPIN: READY\r\n");
    } else if (!strcmp(c, "+CFUN?")) {
        answer = g_strdup_printf("+CFUN: %u\r\n", s->radio);
    } else if (sscanf(c, "+CFUN=%u%n", &value, &end) == 1 && !c[end] &&
               value <= 1) {
        s->radio = value;
        if (!value) {
            modem_link(s, false);
        }
    } else if (!strcmp(c, "+CGATT?")) {
        answer = g_strdup_printf("+CGATT: %u\r\n", s->attached && s->radio);
    } else if (sscanf(c, "+CGATT=%u%n", &value, &end) == 1 && !c[end] &&
               value <= 1) {
        s->attached = value;
        if (!value) {
            modem_link(s, false);
        }
    } else if (!strcmp(c, "+CREG?") || !strcmp(c, "+CGREG?") ||
               !strcmp(c, "+CEREG?")) {
        unsigned mode = c[2] == 'E'   ? s->cereg
                        : c[2] == 'G' ? s->cgreg
                                      : s->creg;
        answer = g_strdup_printf("%.*s: %u,%u,\"0001\",\"00000001\",%u\r\n",
                                 (int)strlen(c) - 1, c, mode, s->radio ? 1 : 0,
                                 s->lte ? 7 : 2);
    } else if (sscanf(c, "+CREG=%u%n", &value, &end) == 1 && !c[end] &&
               value <= 2) {
        s->creg = value;
    } else if (sscanf(c, "+CGREG=%u%n", &value, &end) == 1 && !c[end] &&
               value <= 2) {
        s->cgreg = value;
    } else if (sscanf(c, "+CEREG=%u%n", &value, &end) == 1 && !c[end] &&
               value <= 2) {
        s->cereg = value;
    } else if (!strcmp(c, "+COPS?")) {
        answer = g_strdup_printf("+COPS: 0,2,\"00101\",%u\r\n", s->lte ? 7 : 2);
    } else if (!strcmp(c, "+COPS=?")) {
        answer = g_strdup_printf(
            "+COPS: (2,\"QEMU\",\"QEMU\",\"00101\",%u)\r\n", s->lte ? 7 : 2);
    } else if (!strcmp(c, "+CSQ")) {
        answer = g_strdup(s->radio ? "+CSQ: 25,99\r\n" : "+CSQ: 99,99\r\n");
    } else if (!strcmp(c, "^SMONI")) {
        if (!s->radio) {
            answer = g_strdup("^SMONI: \r\n");
        } else if (s->lte) {
            answer = g_strdup_printf(
                "^SMONI: "
                "4G,6300,20,10,10,FDD,001,01,0001,0000001,1,30,-80,-7,%s\r\n",
                s->active ? "CONN" : "NOCONN");
        } else {
            answer = g_strdup_printf(
                "^SMONI: 3G,10564,1,-7,-75,001,01,0001,0000001,30,30,--,%s\r\n",
                s->active ? "CONN" : "NOCONN");
        }
    } else if (!strcmp(c, "^SWWAN?")) {
        answer = g_strdup_printf("^SWWAN: 1,%u,1\r\n", s->active);
    } else if (sscanf(c, "^SWWAN=%u,%u%n", &value, &cid, &end) == 2 &&
               (!c[end] || !strcmp(c + end, ",1")) && value <= 1 && cid == 1) {
        if (value &&
            (!s->radio || !s->attached || qemu_get_queue(s->nic)->link_down)) {
            return false;
        }
        modem_link(s, value);
    } else if (!strcmp(c, "+CGACT?")) {
        answer = g_strdup_printf("+CGACT: 1,%u\r\n", s->active);
    } else if (sscanf(c, "+CGACT=%u,%u%n", &value, &cid, &end) == 2 &&
               !c[end] && value <= 1 && cid == 1) {
        if (value &&
            (!s->radio || !s->attached || qemu_get_queue(s->nic)->link_down)) {
            return false;
        }
        modem_link(s, value);
    } else if (!strcmp(c, "+CGDCONT?")) {
        answer = g_strdup_printf(
            "+CGDCONT: 1,\"IP\",\"%s\",\"0.0.0.0\",0,0\r\n", s->apn);
    } else if (g_str_has_prefix(c, "+CGDCONT=")) {
        const char *apn;
        size_t len;
        if (!g_str_has_prefix(c, "+CGDCONT=1,\"IP\"")) {
            return false;
        }
        apn = c + strlen("+CGDCONT=1,\"IP\"");
        if (!*apn) {
            s->apn[0] = 0;
        } else {
            if (strncmp(apn, ",\"", 2)) {
                return false;
            }
            apn += 2;
            len = strlen(apn);
            if (!len || len > sizeof(s->apn) || apn[len - 1] != '"' ||
                memchr(apn, '"', len - 1)) {
                return false;
            }
            memcpy(s->apn, apn, len - 1);
            s->apn[len - 1] = 0;
        }
    } else if (!strcmp(c, "^SIND?")) {
        for (unsigned i = 0; i < ARRAY_SIZE(indicator_names); i++) {
            modem_indicator(s, port, i);
        }
    } else if (g_str_has_prefix(c, "^SIND=")) {
        char name[32];
        if (sscanf(c, "^SIND=\"%31[^\"]\",%u%n", name, &value, &end) != 2 ||
            value > 2 || (c[end] && strcmp(c + end, ",99"))) {
            return false;
        }
        unsigned i;
        for (i = 0; i < ARRAY_SIZE(indicator_names); i++) {
            if (!strcmp(name, indicator_names[i])) {
                break;
            }
        }
        if (i == ARRAY_SIZE(indicator_names) ||
            (c[end] && strcmp(name, "ceer"))) {
            return false;
        }
        if (value < 2) {
            s->indicators = (s->indicators & ~(1U << i)) | (value << i);
        }
        modem_indicator(s, port, i);
    } else if (!strcmp(c, "^SCKS?")) {
        answer = g_strdup("^SCKS: 1,1\r\n");
    } else if (!strcmp(c, "^SPIC")) {
        answer = g_strdup("^SPIC: 3\r\n");
    } else if (!strcmp(c, "^SCTM?")) {
        answer = g_strdup("^SCTM: 0,0,35\r\n");
    } else if (!strcmp(c, "^SBV")) {
        answer = g_strdup("^SBV: 3800\r\n");
    } else if (!strcmp(c, "^SDPORT?")) {
        answer = g_strdup("^SDPORT: 13\r\n");
    } else if (!strcmp(c, "^SCFG?")) {
        answer = g_strdup("^SCFG: \"MEopMode/Prov/Cfg\",\"QEMU\"\r\n^SCFG: "
                          "\"Radio/Band\",\"127\"\r\n");
    } else if (!strcmp(c, "+CLCC") || !strcmp(c, "^SLCC") ||
               !strcmp(c, "+CNUM")) {
        /* No voice calls or assigned subscriber phone number. */
    } else if (sscanf(c, "^SRADC=%u%n", &value, &end) == 1 && !c[end] &&
               value < 2) {
        answer = g_strdup_printf("^SRADC: %u,1,1200\r\n", value);
    } else if (!strcmp(c, "^SRSA=11,0,,,0,1") ||
               !strcmp(c, "^SRSA=11,2,,1,0")) {
        /* Disable/query remote SIM access: the simulated SIM is local. */
        answer = g_strdup("^SRSA: 11,0\r\n");
    } else if (!strcmp(c, "^SGIO=?")) {
        answer = g_strdup("^SGIO: (0-7)\r\n");
    } else if (sscanf(c, "^SGIO=%u%n", &value, &end) == 1 && !c[end] &&
               value < 8) {
        answer = g_strdup_printf("^SGIO: %u\r\n", !!(s->gpio & (1U << value)));
    } else if (!strcmp(c, "+CPBS=\"EN\"") || !strcmp(c, "+CPBS=\"SM\"")) {
        memcpy(s->phonebook, c + 7, 2);
    } else if (!strcmp(c, "+CPBS?")) {
        answer = g_strdup_printf("+CPBS: \"%s\",0,0\r\n", s->phonebook);
    } else if (!strcmp(c, "+CPBR=?")) {
        answer = g_strdup("+CPBR: (0),40,16\r\n");
    } else if (!strcmp(c, "+CRSM=176,28486,0,0,17")) {
        /* EF_SPN: display condition followed by the synthetic provider name. */
        answer =
            g_strdup("+CRSM: 144,0,\"0051454d55ffffffffffffffffffffffff\"\r\n");
    } else if (!strcmp(c, "+CRSM=176,28621,0,0,0")) {
        /* EF_SPDI: PLMN 001/01, not a provisioned commercial subscriber. */
        answer = g_strdup("+CRSM: 144,0,\"a305800300f110\"\r\n");
    } else if (g_str_has_prefix(c, "+CRSM=")) {
        /* File not present in this minimal test SIM. */
        answer = g_strdup("+CRSM: 106,130\r\n");
    } else if (!strcmp(c, "+CSMS=1")) {
        answer = g_strdup("+CSMS: 1,1,1\r\n");
    } else if (g_str_has_prefix(c, "+CLCK=\"SC\",2")) {
        answer = g_strdup("+CLCK: 0\r\n");
    } else {
        static const char *const settings[] = {
            "+CMEE=", "+CSCS=",   "+COPS=0", "+COPS=3,", "+CNMI=", "+CMGF=",
            "+CRC=",  "+CLIP=",   "+COLP=",  "+CSSN=",   "+CMER=", "+CUSD=",
            "^SCKS=", "^SCTM=",   "^SCFG=",  "^SCPIN=",  "^SPIO=", "^SGAUTH=",
            "^SNFS=", "^SAIC=",   "^SNFI=",  "^SNFO=",   "^SRTC=", "^SSDA=",
            "^SLCC=", "^SDPORT=", "&D",      "&C",       "V1",
        };
        bool known = false;
        for (unsigned i = 0; i < ARRAY_SIZE(settings); i++) {
            known |= g_str_has_prefix(c, settings[i]);
        }
        if (!known) {
            return false;
        }
    }
    if (answer) {
        modem_reply(s, port, answer);
    }
    return true;
}

static void modem_command(MHI2Modem *s, unsigned port)
{
    char *line = s->command[port];
    bool ok = !s->overflow[port] && g_ascii_strncasecmp(line, "AT", 2) == 0;
    if (s->echo[port]) {
        modem_reply(s, port, line);
    }
    modem_reply(s, port, "\r\n");
    if (ok) {
        /* V.250 ignores whitespace outside strings. Keep APN/auth values. */
        bool quoted = false;
        char *out = line;
        for (char *in = line; *in; in++) {
            if (*in == '"') {
                quoted = !quoted;
            }
            if (quoted || !g_ascii_isspace(*in)) {
                *out++ = quoted ? *in : g_ascii_toupper(*in);
            }
        }
        *out = 0;
        ok = !quoted;
        char *c = line + 2;
        while (ok) {
            char *next = c;
            quoted = false;
            while (*next) {
                if (*next == '"') {
                    quoted = !quoted;
                } else if (*next == ';' && !quoted) {
                    break;
                }
                next++;
            }
            bool last = !*next;
            *next = 0;
            /* Never trace APNs, passwords, phone numbers or command values. */
            g_autofree char *name = g_strndup(c, strcspn(c, "=? "));
            ok = modem_at(s, port, c);
            trace_mhi2_modem_at(port, name, ok);
            if (last) {
                break;
            }
            c = next + 1;
        }
    }
    modem_reply(s, port, ok ? "OK\r\n" : "ERROR\r\n");
    s->command_len[port] = 0;
    s->overflow[port] = false;
}

static void modem_control(USBDevice *dev, USBPacket *p, int request, int value,
                          int index, int length, uint8_t *data)
{
    if (usb_desc_handle_control(dev, p, request, value, index, length, data) >=
        0) {
        return;
    }
    if (index < 4 && ((request == 0x2120 && length == 7) ||
                      ((request == 0x2122 || request == 0x2123) && !length))) {
        return;
    }
    if (request == 0x2143 && index == 4 && !length) {
        return;
    }
    if (request == 0xa121 && index < 4 && length >= 7) {
        stl_le_p(data, 115200);
        data[4] = 0;
        data[5] = 0;
        data[6] = 8;
        p->actual_length = 7;
        return;
    }
    qemu_log_mask(LOG_UNIMP, "mhi2-modem: USB control %04x %04x %04x %d\n",
                  request, value, index, length);
    p->status = USB_RET_STALL;
}

static void modem_data(USBDevice *dev, USBPacket *p)
{
    MHI2Modem *s = MHI2_MODEM(dev);
    unsigned ep = p->ep->nr;
    size_t n = p->iov.size;
    if (ep >= 1 && ep <= 4) {
        unsigned port = ep - 1;
        if (p->pid == USB_TOKEN_IN) {
            GByteArray *q = s->serial[port];
            if (!q->len) {
                p->status = USB_RET_NAK;
                return;
            }
            n = MIN(n, q->len);
            usb_packet_copy(p, q->data, n);
            g_byte_array_remove_range(q, 0, n);
        } else {
            uint8_t bytes[512];
            if (n > sizeof(bytes)) {
                p->status = USB_RET_STALL;
                return;
            }
            usb_packet_copy(p, bytes, n);
            for (size_t i = 0; i < n; i++) {
                if (bytes[i] == '\r' || bytes[i] == '\n') {
                    if (s->command_len[port]) {
                        s->command[port][s->command_len[port]] = 0;
                        modem_command(s, port);
                    }
                } else if (s->command_len[port] <
                           sizeof(s->command[port]) - 1) {
                    s->command[port][s->command_len[port]++] = bytes[i];
                } else {
                    /* Never execute a truncated command or its suffix. */
                    s->overflow[port] = true;
                }
            }
        }
        return;
    }
    if (ep == 7 && p->pid == USB_TOKEN_IN) {
        uint8_t event[16] = {0xa1, 0, 0, 0, 4, 0, 0, 0};
        if (!s->notify) {
            p->status = USB_RET_NAK;
            return;
        }
        size_t size = 8;
        if (s->notify & 1) {
            event[2] = s->active;
            s->notify &= ~1;
        } else {
            event[1] = 0x2a;
            event[6] = 8;
            stl_le_p(event + 8, s->lte ? 100000000 : 21000000);
            stl_le_p(event + 12, s->lte ? 50000000 : 5000000);
            size = 16;
            s->notify &= ~2;
        }
        usb_packet_copy(p, event, MIN(size, n));
        return;
    }
    if (ep == 6 && p->pid == USB_TOKEN_IN) {
        GBytes *frame = g_queue_peek_head(&s->ethernet);
        if (!frame) {
            p->status = USB_RET_NAK;
            return;
        }
        gsize size;
        const uint8_t *data = g_bytes_get_data(frame, &size);
        size_t take = MIN(n, size - s->rx_offset);
        usb_packet_copy(p, (void *)(data + s->rx_offset), take);
        s->rx_offset += take;
        if (s->rx_offset == size && (take < n || size % 512)) {
            g_bytes_unref(g_queue_pop_head(&s->ethernet));
            s->rx_offset = 0;
            qemu_flush_queued_packets(qemu_get_queue(s->nic));
        }
        return;
    }
    if (ep == 6 && p->pid == USB_TOKEN_OUT) {
        if (n > sizeof(s->tx) - s->tx_len) {
            s->tx_len = 0;
            p->status = USB_RET_STALL;
            return;
        }
        usb_packet_copy(p, s->tx + s->tx_len, n);
        s->tx_len += n;
        if (n % 512 || !n) {
            if (s->active && s->tx_len >= 14 && s->tx_len <= 1518) {
                qemu_send_packet(qemu_get_queue(s->nic), s->tx, s->tx_len);
            }
            s->tx_len = 0;
        }
        return;
    }
    p->status = USB_RET_STALL;
}

static bool modem_can_receive(NetClientState *nc)
{
    return g_queue_get_length(&MHI2_MODEM(qemu_get_nic_opaque(nc))->ethernet) <
           64;
}

static ssize_t modem_receive(NetClientState *nc, const uint8_t *buf,
                             size_t size)
{
    MHI2Modem *s = qemu_get_nic_opaque(nc);
    if (!s->active || nc->link_down || size < 14 || size > 1518) {
        return size;
    }
    if (!modem_can_receive(nc)) {
        return 0;
    }
    g_queue_push_tail(&s->ethernet, g_bytes_new(buf, size));
    usb_wakeup(usb_ep_get(&s->parent_obj, USB_TOKEN_IN, 6), 0);
    return size;
}

static void modem_link_changed(NetClientState *nc)
{
    MHI2Modem *s = qemu_get_nic_opaque(nc);
    if (nc->link_down) {
        modem_link(s, false);
    }
}

static NetClientInfo modem_net = {
    .type = NET_CLIENT_DRIVER_NIC,
    .size = sizeof(NICState),
    .receive = modem_receive,
    .can_receive = modem_can_receive,
    .link_status_changed = modem_link_changed,
};
static void modem_reset(USBDevice *dev)
{
    MHI2Modem *s = MHI2_MODEM(dev);
    s->radio = s->attached = true;
    s->active = false;
    s->notify = 3;
    s->rx_offset = s->tx_len = 0;
    s->creg = s->cgreg = s->cereg = 0;
    s->indicators = 0;
    /* NAD uses an active-low SIM detect input on GPIO 0. */
    s->gpio = 0;
    pstrcpy(s->phonebook, sizeof(s->phonebook), "SM");
    memset(s->command_len, 0, sizeof(s->command_len));
    memset(s->overflow, 0, sizeof(s->overflow));
    for (unsigned i = 0; i < 4; i++) {
        s->echo[i] = true;
        g_byte_array_set_size(s->serial[i], 0);
    }
    g_queue_clear_full(&s->ethernet, (GDestroyNotify)g_bytes_unref);
    qemu_purge_queued_packets(qemu_get_queue(s->nic));
    pstrcpy(s->apn, sizeof(s->apn), "qemu");
}

static void modem_realize(USBDevice *dev, Error **errp)
{
    MHI2Modem *s = MHI2_MODEM(dev);
    char mac[13];
    usb_desc_init(dev);
    for (unsigned i = 0; i < 4; i++) {
        s->serial[i] = g_byte_array_new();
    }
    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    s->nic = qemu_new_nic(&modem_net, &s->conf, TYPE_MHI2_MODEM, dev->qdev.id,
                          &dev->qdev.mem_reentrancy_guard, s);
    qemu_format_nic_info_str(qemu_get_queue(s->nic), s->conf.macaddr.a);
    snprintf(mac, sizeof(mac), "%02x%02x%02x%02x%02x%02x", s->conf.macaddr.a[0],
             s->conf.macaddr.a[1], s->conf.macaddr.a[2], s->conf.macaddr.a[3],
             s->conf.macaddr.a[4], s->conf.macaddr.a[5]);
    usb_desc_set_string(dev, 4, mac);
    modem_reset(dev);
}

static void modem_unrealize(USBDevice *dev)
{
    MHI2Modem *s = MHI2_MODEM(dev);
    for (unsigned i = 0; i < 4; i++) {
        g_byte_array_unref(s->serial[i]);
    }
    g_queue_clear_full(&s->ethernet, (GDestroyNotify)g_bytes_unref);
    qemu_del_nic(s->nic);
}

static const Property modem_properties[] = {
    DEFINE_NIC_PROPERTIES(MHI2Modem, conf),
    DEFINE_PROP_BOOL("lte", MHI2Modem, lte, true),
};
static const VMStateDescription modem_vmstate = {
    .name = TYPE_MHI2_MODEM,
    .unmigratable = true,
};
static void modem_class_init(ObjectClass *klass, const void *data)
{
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);
    uc->realize = modem_realize;
    uc->unrealize = modem_unrealize;
    uc->usb_desc = &modem_desc;
    uc->product_desc = "MHI2 Cinterion modem";
    uc->handle_attach = usb_desc_attach;
    uc->handle_reset = modem_reset;
    uc->handle_control = modem_control;
    uc->handle_data = modem_data;
    device_class_set_props(DEVICE_CLASS(klass), modem_properties);
    DEVICE_CLASS(klass)->vmsd = &modem_vmstate;
}

static const TypeInfo modem_type = {
    .name = TYPE_MHI2_MODEM,
    .parent = TYPE_USB_DEVICE,
    .instance_size = sizeof(MHI2Modem),
    .class_init = modem_class_init,
};
static void modem_register(void)
{
    type_register_static(&modem_type);
}
type_init(modem_register)
