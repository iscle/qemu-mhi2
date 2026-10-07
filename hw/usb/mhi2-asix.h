// SPDX-License-Identifier: GPL-3.0-or-later
/* From iscle/mhi2-firmware-analysis, commit 788ad8a,
 * porsche-workspace/pico-companion/src/asix_protocol.h.
 * Kept here so the optional USB fixture needs no external Pico checkout. */
#ifndef ASIX_PROTOCOL_H
#define ASIX_PROTOCOL_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define ASIX_FRAME_MAX 1518
struct asix_state {
    uint8_t mac[6], filter[8], ipg[3], monitor, gpio, software_mii, phy_select;
    uint16_t phy[32], eeprom[64], rx_control, medium;
    uint32_t controls, rejected_controls, rx_frames, malformed_frames;
    uint8_t last_rejected;
    uint8_t header[4], frame[ASIX_FRAME_MAX];
    unsigned header_used, frame_used, frame_size, skip_pad;
};
void asix_init(struct asix_state *, const uint8_t mac[6]);
/* Validates direction and length; returns transferred bytes or -1 (STALL). */
int asix_control(struct asix_state *, bool in, uint8_t request, uint16_t value,
                 uint16_t index, uint8_t *data, unsigned length);
typedef void (*asix_frame_fn)(void *, const uint8_t *, unsigned);
/* OUT transfers can split headers/frames; end marks a short packet/ZLP. */
void asix_receive(struct asix_state *, const uint8_t *, size_t, bool end,
                  asix_frame_fn, void *);
size_t asix_encode(uint8_t *out, size_t capacity, const uint8_t *frame, size_t n);
#endif
