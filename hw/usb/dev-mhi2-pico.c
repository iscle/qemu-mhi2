/* Local development fixture for testing the Pico ASIX protocol against QNX.
 * The ASIX protocol is self-contained; no Pico firmware or SDK is needed.
 * This fixture is not an RP2040 CPU or TinyUSB controller emulator.
 * High-speed mode exists because the local hub does not yet implement splits.
 */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/core/qdev-properties.h"
#include "hw/usb/usb.h"
#include "migration/vmstate.h"
#include "net/net.h"
#include "desc.h"
#include "mhi2-asix.h"

#define TYPE_MHI2_PICO "usb-mhi2-pico"
OBJECT_DECLARE_SIMPLE_TYPE(MHI2Pico, MHI2_PICO)
struct MHI2Pico {
    USBDevice parent_obj;
    NICConf conf;
    NICState *nic;
    struct asix_state adapter;
    struct {uint8_t bytes[ASIX_FRAME_MAX+16];size_t size,offset;} rx[4];
    unsigned head,count;
    bool zlp;
};
static const USBDescStrings strings={
    [1]="MHI2 local development",[2]="Pico protocol test fixture",[3]="QEMU-PICO-001",
};
#define PICO_IFACE(packet) { .bNumEndpoints=3,.bInterfaceClass=0xff,.bInterfaceSubClass=0xff, \
 .eps=(USBDescEndpoint[]){ \
 {.bEndpointAddress=0x81,.bmAttributes=USB_ENDPOINT_XFER_BULK,.wMaxPacketSize=packet}, \
 {.bEndpointAddress=0x02,.bmAttributes=USB_ENDPOINT_XFER_BULK,.wMaxPacketSize=packet}, \
 {.bEndpointAddress=0x83,.bmAttributes=USB_ENDPOINT_XFER_INT,.wMaxPacketSize=8,.bInterval=10}} }
static const USBDescIface fs_if=PICO_IFACE(64),hs_if=PICO_IFACE(512);
#define PICO_DEVICE(iface) { .bcdUSB=0x0200,.bMaxPacketSize0=64,.bNumConfigurations=1, \
 .confs=(USBDescConfig[]){{.bNumInterfaces=1,.bConfigurationValue=1, \
 .bmAttributes=USB_CFG_ATT_ONE,.bMaxPower=125,.nif=1,.ifs=&iface}} }
static const USBDescDevice fs=PICO_DEVICE(fs_if),hs=PICO_DEVICE(hs_if);
static const USBDesc desc={.id={.idVendor=0x2001,.idProduct=0x3c05,.bcdDevice=1,
 .iManufacturer=1,.iProduct=2,.iSerialNumber=3},.full=&fs,.high=&hs,.str=strings};
static void pico_reset(USBDevice *dev)
{
    MHI2Pico *s=MHI2_PICO(dev);
    asix_init(&s->adapter,s->conf.macaddr.a);
    s->head=s->count=0;s->zlp=false;
}
static void pico_control(USBDevice *dev,USBPacket *p,int request,int value,int index,int length,uint8_t *data)
{
    MHI2Pico *s=MHI2_PICO(dev);
    if(usb_desc_handle_control(dev,p,request,value,index,length,data)>=0)return;
    if((request>>8&0x7f)==0x40 && length>=0 && length<=64){
        bool in=(request>>8&0x80)!=0;
        int n=asix_control(&s->adapter,in,request&255,value,index,data,length);
        if(n>=0){if(in)p->actual_length=n;qemu_flush_queued_packets(qemu_get_queue(s->nic));return;}
    }
    p->status=USB_RET_STALL;
}
static void frame_out(void *opaque,const uint8_t *frame,unsigned size)
{
    MHI2Pico *s=opaque;
    qemu_send_packet(qemu_get_queue(s->nic),frame,size);
}
static void pico_data(USBDevice *dev,USBPacket *p)
{
    MHI2Pico *s=MHI2_PICO(dev);
    unsigned packet=dev->speed==USB_SPEED_HIGH?512:64;
    if(p->pid==USB_TOKEN_OUT && p->ep->nr==2){
        uint8_t data[16384];size_t size=p->iov.size;
        if(size>sizeof(data)){p->status=USB_RET_STALL;return;}
        usb_packet_copy(p,data,size);
        asix_receive(&s->adapter,data,size,size%packet!=0,frame_out,s);return;
    }
    if(p->pid==USB_TOKEN_IN && p->ep->nr==3){
        /* EHCI submits the entire qTD to this model, not individual USB
         * transactions. QNX keeps a 256-byte interrupt URB pending. Each
         * eight-byte status record fills wMaxPacketSize, so it must not end
         * that URB as a short transfer. Coalesce the records here; the real
         * Pico sends them separately on its full-speed interrupt endpoint.
         * This fixture deliberately does not validate endpoint timing.
         */
        uint8_t status[8]={0,0,1,0,0,0,0,0};
        size_t left=p->iov.size;
        while(left){
            size_t n=MIN(left,sizeof(status));
            usb_packet_copy(p,status,n);left-=n;
        }
        return;
    }
    if(p->pid==USB_TOKEN_IN && p->ep->nr==1){
        if(s->zlp){s->zlp=false;return;}
        if(!s->count || !(s->adapter.rx_control&0x80)){p->status=USB_RET_NAK;return;}
        unsigned slot=s->head;
        size_t n=MIN(p->iov.size,s->rx[slot].size-s->rx[slot].offset);
        usb_packet_copy(p,s->rx[slot].bytes+s->rx[slot].offset,n);s->rx[slot].offset+=n;
        if(s->rx[slot].offset==s->rx[slot].size){
            s->zlp=n==p->iov.size && n%packet==0;
            s->head=(s->head+1)%4;s->count--;
            qemu_flush_queued_packets(qemu_get_queue(s->nic));
        }
        return;
    }
    p->status=USB_RET_STALL;
}
static ssize_t pico_receive(NetClientState *nc,const uint8_t *bytes,size_t size)
{
    MHI2Pico *s=qemu_get_nic_opaque(nc);
    if(size>ASIX_FRAME_MAX)return size;
    if(s->count==4 || !(s->adapter.rx_control&0x80))return 0;
    unsigned slot=(s->head+s->count)%4;
    size_t n=asix_encode(s->rx[slot].bytes,sizeof(s->rx[slot].bytes),bytes,size);
    if(!n)return size;
    s->rx[slot].size=n;s->rx[slot].offset=0;s->count++;
    usb_wakeup(usb_ep_get(&s->parent_obj,USB_TOKEN_IN,1),0);
    return size;
}
static NetClientInfo net_info={.type=NET_CLIENT_DRIVER_NIC,.size=sizeof(NICState),.receive=pico_receive};
static void pico_realize(USBDevice *dev,Error **errp)
{
    MHI2Pico *s=MHI2_PICO(dev);
    qemu_macaddr_default_if_unset(&s->conf.macaddr);
    usb_desc_init(dev);
    s->nic=qemu_new_nic(&net_info,&s->conf,TYPE_MHI2_PICO,dev->qdev.id,&dev->qdev.mem_reentrancy_guard,s);
    pico_reset(dev);
}
static void pico_unrealize(USBDevice *dev){qemu_del_nic(MHI2_PICO(dev)->nic);}
static const VMStateDescription vmstate={.name=TYPE_MHI2_PICO,.unmigratable=1};
static const Property props[]={DEFINE_NIC_PROPERTIES(MHI2Pico,conf)};
static void pico_class(ObjectClass *klass,const void *unused)
{
    USBDeviceClass *uc=USB_DEVICE_CLASS(klass);DeviceClass *dc=DEVICE_CLASS(klass);
    uc->usb_desc=&desc;uc->product_desc="Pico ASIX protocol fixture";
    uc->realize=pico_realize;uc->unrealize=pico_unrealize;uc->handle_reset=pico_reset;
    uc->handle_attach=usb_desc_attach;uc->handle_control=pico_control;uc->handle_data=pico_data;
    dc->vmsd=&vmstate;device_class_set_props(dc,props);
}
static const TypeInfo info={.name=TYPE_MHI2_PICO,.parent=TYPE_USB_DEVICE,.instance_size=sizeof(MHI2Pico),.class_init=pico_class};
static void register_pico(void){type_register_static(&info);}
type_init(register_pico)
