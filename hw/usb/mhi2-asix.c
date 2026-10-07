// SPDX-License-Identifier: GPL-3.0-or-later
/* From iscle/mhi2-firmware-analysis, commit 788ad8a,
 * porsche-workspace/pico-companion/src/asix_protocol.c.
 * Kept here so the optional USB fixture needs no external Pico checkout. */
// Register numbers/framing documented by Linux drivers/net/usb/asix*.c.
// This is a local compatibility implementation, not an ASIX product.
#include "qemu/osdep.h"
#include "mhi2-asix.h"
#include <string.h>
static uint16_t get16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static void put16(uint8_t *p, uint16_t v) { p[0]=v; p[1]=v>>8; }
void asix_init(struct asix_state *s, const uint8_t mac[6]) {
    memset(s,0,sizeof(*s)); memcpy(s->mac,mac,6);
    s->phy[0]=0x3100; s->phy[1]=0x786d; /* 100/full, autoneg complete, link */
    s->phy[2]=0x003b; s->phy[3]=0x1861;
    s->phy[4]=0x01e1; s->phy[5]=0x45e1; s->phy[6]=1;
    s->phy[16]=0x003c; s->phy[17]=0xac00;
    s->ipg[0]=0x15; s->ipg[1]=0x0c; s->ipg[2]=0x12;
    s->medium=0x0306;
    for(unsigned i=0;i<3;i++)s->eeprom[4+i]=get16(mac+i*2);
}
int asix_control(struct asix_state *s, bool in, uint8_t r, uint16_t v,
                 uint16_t i, uint8_t *d, unsigned n) {
    s->controls++;
    if(in) {
        if(r==0x07 && n==2 && i<32) {put16(d,s->phy[i]);return 2;}
        if(r==0x09 && n==1) {d[0]=s->software_mii?1:0;return 1;}
        if(r==0x0b && n==2 && v<64) {put16(d,s->eeprom[v]);return 2;}
        if(r==0x0f && n==2) {put16(d,s->rx_control);return 2;}
        if(r==0x11 && n==3) {memcpy(d,s->ipg,3);return 3;}
        if((r==0x13 || r==0x17) && n==6) {memcpy(d,s->mac,6);return 6;}
        if(r==0x19 && n==2) {d[0]=0;d[1]=0x10;return 2;}
        if(r==0x1a && n==2) {put16(d,s->medium);return 2;}
        if(r==0x1c && n==1) {d[0]=s->monitor;return 1;}
        if(r==0x1e && n==1) {d[0]=s->gpio;return 1;}
        if(r==0x21 && n==1) {d[0]=s->phy_select;return 1;}
    } else {
        if(r==0x06 && !n) {s->software_mii=1;return 0;}
        if(r==0x0a && !n) {s->software_mii=0;return 0;}
        if(r==0x08 && n==2 && i<32) {
            if(i==0) s->phy[i]=get16(d)&~0x8200; /* reset/restart bits self-clear */
            else if(i!=1 && i!=2 && i!=3 && i!=5) s->phy[i]=get16(d);
            return 2;
        }
        if((r==0x0d || r==0x0e) && !n) return 0;
        if(r==0x0c && n==2 && v<64) {s->eeprom[v]=get16(d);return 2;} /* RAM only */
        if(r==0x10 && !n) {s->rx_control=v;return 0;}
        // QNX ax_enable_88772 sends a redundant three-byte data stage;
        // Linux writes the same values using only wValue/wIndex.
        if(r==0x12 && (n==0 || n==3)) {s->ipg[0]=v;s->ipg[1]=v>>8;s->ipg[2]=i;return n;}
        if(r==0x14 && n==6) {memcpy(s->mac,d,6);return 6;}
        if(r==0x16 && n==8) {memcpy(s->filter,d,8);return 8;}
        if(r==0x1b && !n) {s->medium=v;return 0;}
        if(r==0x1d && !n) {s->monitor=v;return 0;}
        if(r==0x1f && !n) {s->gpio=v;return 0;}
        if(r==0x20 && !n) {
            s->header_used=s->frame_used=s->frame_size=s->skip_pad=0;
            return 0;
        }
        if(r==0x22 && !n) {s->phy_select=v;return 0;}
    }
    s->rejected_controls++;s->last_rejected=r;return -1;
}
static void reset_frame(struct asix_state *s) {
    s->header_used=s->frame_used=s->frame_size=s->skip_pad=0;
}
void asix_receive(struct asix_state *s,const uint8_t *data,size_t n,bool end,
                  asix_frame_fn emit,void *ctx) {
    while(n) {
        if(s->skip_pad) {data++;n--;s->skip_pad=0;continue;}
        if(s->header_used<4) {
            s->header[s->header_used++]=*data++;n--;
            if(s->header_used<4)continue;
            unsigned len=get16(s->header), inv=get16(s->header+2);
            if((len ^ inv)!=0xffff || len>ASIX_FRAME_MAX || (len && len<14)) {
                s->malformed_frames++;reset_frame(s);return;
            }
            if(!len) {reset_frame(s);continue;} /* ASIX transfer terminator */
            s->frame_size=len;
        }
        size_t take=s->frame_size-s->frame_used;if(take>n)take=n;
        memcpy(s->frame+s->frame_used,data,take);
        s->frame_used+=take;data+=take;n-=take;
        if(s->frame_used==s->frame_size) {
            emit(ctx,s->frame,s->frame_size);s->rx_frames++;
            s->skip_pad=s->frame_size&1;
            s->header_used=s->frame_used=s->frame_size=0;
        }
    }
    if(end) {
        if(s->header_used || s->frame_used)s->malformed_frames++;
        reset_frame(s);
    }
}
size_t asix_encode(uint8_t *out,size_t capacity,const uint8_t *frame,size_t n) {
    if(n<14 || n>ASIX_FRAME_MAX)return 0;
    size_t size=4+((n+1)&~(size_t)1);
    bool terminator=!(size%64);if(terminator)size+=4;
    if(capacity<size)return 0;
    put16(out,n);put16(out+2,(uint16_t)~n);memcpy(out+4,frame,n);
    if(n&1)out[4+n]=0;
    if(terminator){put16(out+size-4,0);put16(out+size-2,0xffff);}
    return size;
}
