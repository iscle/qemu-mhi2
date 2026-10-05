/* Local MHI2 experiment: USB AOA bulk transport to an Android Auto head-unit
 * server. The Android endpoint performs the real projection/TLS protocol.
 * This device only adapts its byte stream to a high-speed USB accessory. */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/usb/usb.h"
#include "migration/vmstate.h"
#include "chardev/char-fe.h"
#include "desc.h"

#define TYPE_USB_ANDROID_AUTO "usb-android-auto"
#define AA_BUFFER (1024 * 1024)
OBJECT_DECLARE_SIMPLE_TYPE(USBAndroidAuto, USB_ANDROID_AUTO)
struct USBAndroidAuto {
    USBDevice parent_obj;
    CharFrontend chr;
    uint8_t rx[AA_BUFFER], tx[AA_BUFFER];
    unsigned rx_head, rx_count, tx_head, tx_count;
    uint64_t usb_in_bytes, usb_out_bytes;
    guint watch;
    char accessory_strings[6][256];
};
static const USBDescStrings aa_strings = {
    [1] = "Google", [2] = "Android Auto USB transport", [3] = "MHI2-AVD-001",
};
#define AA_IFACE(packet) { .bNumEndpoints=2, .bInterfaceClass=0xff, \
 .bInterfaceSubClass=0xff, .eps=(USBDescEndpoint[]){ \
 {.bEndpointAddress=USB_DIR_IN|1, .bmAttributes=USB_ENDPOINT_XFER_BULK, .wMaxPacketSize=packet}, \
 {.bEndpointAddress=USB_DIR_OUT|2, .bmAttributes=USB_ENDPOINT_XFER_BULK, .wMaxPacketSize=packet} } }
static const USBDescIface aa_fs_iface = AA_IFACE(64);
static const USBDescIface aa_hs_iface = AA_IFACE(512);
#define AA_DEVICE(iface) { .bcdUSB=0x0200, .bMaxPacketSize0=64, .bNumConfigurations=1, \
 .confs=(USBDescConfig[]){{.bNumInterfaces=1, .bConfigurationValue=1, \
 .bmAttributes=USB_CFG_ATT_ONE, .bMaxPower=250, .nif=1, .ifs=&iface}} }
static const USBDescDevice aa_fs = AA_DEVICE(aa_fs_iface);
static const USBDescDevice aa_hs = AA_DEVICE(aa_hs_iface);
static const USBDesc aa_desc = {
    .id={.idVendor=0x18d1,.idProduct=0x2d00,.bcdDevice=0x0200,
         .iManufacturer=1,.iProduct=2,.iSerialNumber=3},
    .full=&aa_fs,.high=&aa_hs,.str=aa_strings,
};
static gboolean aa_writable(void *unused, GIOCondition condition, void *opaque);
static void aa_drain(USBAndroidAuto *s)
{
    while (s->tx_count && qemu_chr_fe_backend_open(&s->chr)) {
        unsigned size=MIN(s->tx_count,AA_BUFFER-s->tx_head);
        int n=qemu_chr_fe_write(&s->chr,s->tx+s->tx_head,size);
        if(n<=0)break;
        s->tx_head=(s->tx_head+n)%AA_BUFFER;s->tx_count-=n;
    }
    if(s->tx_count && !s->watch && qemu_chr_fe_backend_open(&s->chr)) {
        s->watch=qemu_chr_fe_add_watch(&s->chr,G_IO_OUT|G_IO_HUP,aa_writable,s);
    }
}
static gboolean aa_writable(void *unused, GIOCondition condition, void *opaque)
{
    USBAndroidAuto *s=opaque;s->watch=0;
    if(!(condition&G_IO_HUP))aa_drain(s);
    return G_SOURCE_REMOVE;
}
static void aa_reset(USBDevice *dev)
{
    USBAndroidAuto *s=USB_ANDROID_AUTO(dev);
    s->rx_head=s->rx_count=s->tx_head=s->tx_count=0;
    if(s->watch){g_source_remove(s->watch);s->watch=0;}
}
static void aa_control(USBDevice *dev,USBPacket *p,int request,int value,
                       int index,int length,uint8_t *data)
{
    USBAndroidAuto *s=USB_ANDROID_AUTO(dev);
    if(usb_desc_handle_control(dev,p,request,value,index,length,data)>=0)return;
    switch(request){
    case VendorDeviceRequest|51: /* ACCESSORY_GET_PROTOCOL */
        if(length<2)break;data[0]=2;data[1]=0;p->actual_length=2;return;
    case VendorDeviceOutRequest|52: /* ACCESSORY_SEND_STRING */
        if(index<0||index>=6||length<1||length>256||data[length-1]!=0)break;
        memcpy(s->accessory_strings[index],data,length);return;
    case VendorDeviceOutRequest|53: /* Already enumerated in accessory mode. */
        if(length==0)return;
        break;
    default:break;
    }
    p->status=USB_RET_STALL;
}
static void aa_data(USBDevice *dev,USBPacket *p)
{
    USBAndroidAuto *s=USB_ANDROID_AUTO(dev);
    if(!qemu_chr_fe_backend_open(&s->chr)){p->status=USB_RET_NAK;return;}
    if(p->pid==USB_TOKEN_IN && p->ep->nr==1){
        if(!s->rx_count){p->status=USB_RET_NAK;return;}
        unsigned n=MIN(p->iov.size,s->rx_count),first=MIN(n,AA_BUFFER-s->rx_head);
        usb_packet_copy(p,s->rx+s->rx_head,first);
        if(n>first)usb_packet_copy(p,s->rx,n-first);
        s->rx_head=(s->rx_head+n)%AA_BUFFER;s->rx_count-=n;
        s->usb_in_bytes += n;
        qemu_chr_fe_accept_input(&s->chr);return;
    }
    if(p->pid==USB_TOKEN_OUT && p->ep->nr==2){
        if(p->iov.size>AA_BUFFER-s->tx_count){p->status=USB_RET_NAK;return;}
        unsigned start=(s->tx_head+s->tx_count)%AA_BUFFER;
        unsigned n=p->iov.size,first=MIN(n,AA_BUFFER-start);
        usb_packet_copy(p,s->tx+start,first);
        if(n>first)usb_packet_copy(p,s->tx,n-first);
        s->usb_out_bytes += n;
        s->tx_count+=n;aa_drain(s);return;
    }
    p->status=USB_RET_STALL;
}
static int aa_can_read(void *opaque)
{ USBAndroidAuto *s=opaque;return s->parent_obj.attached?AA_BUFFER-s->rx_count:0; }
static void aa_read(void *opaque,const uint8_t *data,int len)
{
    USBAndroidAuto *s=opaque;
    assert(len>=0 && len<=AA_BUFFER-s->rx_count);
    unsigned start=(s->rx_head+s->rx_count)%AA_BUFFER,first=MIN(len,AA_BUFFER-start);
    memcpy(s->rx+start,data,first);if(len>first)memcpy(s->rx,data+first,len-first);
    s->rx_count+=len;usb_wakeup(usb_ep_get(&s->parent_obj,USB_TOKEN_IN,1),0);
}
static void aa_event(void *opaque,QEMUChrEvent event)
{
    USBAndroidAuto *s=opaque;USBDevice *dev=&s->parent_obj;
    if(event==CHR_EVENT_OPENED && !dev->attached)usb_device_attach(dev,&error_abort);
    if(event==CHR_EVENT_CLOSED){if(dev->attached)usb_device_detach(dev);aa_reset(dev);}
}
static void aa_realize(USBDevice *dev,Error **errp)
{
    USBAndroidAuto *s=USB_ANDROID_AUTO(dev);
    if(!qemu_chr_fe_backend_connected(&s->chr)){error_setg(errp,"chardev is required");return;}
    usb_desc_create_serial(dev);usb_desc_init(dev);dev->auto_attach=false;
    Error *local_err = NULL;
    usb_check_attach(dev, &local_err);
    if (local_err) { error_propagate(errp, local_err); return; }
    qemu_chr_fe_set_handlers(&s->chr,aa_can_read,aa_read,aa_event,NULL,s,NULL,true);
    if(qemu_chr_fe_backend_open(&s->chr)&&!dev->attached)usb_device_attach(dev,errp);
}
static void aa_unrealize(USBDevice *dev)
{ USBAndroidAuto *s=USB_ANDROID_AUTO(dev);aa_reset(dev);qemu_chr_fe_deinit(&s->chr,false); }
static const Property aa_props[]={DEFINE_PROP_CHR("chardev",USBAndroidAuto,chr)};
static void aa_init(Object *obj)
{
    USBAndroidAuto *s = USB_ANDROID_AUTO(obj);
    /* Lifetime counters distinguish USB detection from projection traffic.
     * Keep them across bus resets and backend reconnects for diagnostics. */
    object_property_add_uint64_ptr(obj, "usb-in-bytes", &s->usb_in_bytes,
                                   OBJ_PROP_FLAG_READ);
    object_property_add_uint64_ptr(obj, "usb-out-bytes", &s->usb_out_bytes,
                                   OBJ_PROP_FLAG_READ);
}
static const VMStateDescription aa_vmstate={.name=TYPE_USB_ANDROID_AUTO,.unmigratable=1};
static void aa_class(ObjectClass *klass,const void *data)
{
    DeviceClass *dc=DEVICE_CLASS(klass);USBDeviceClass *uc=USB_DEVICE_CLASS(klass);
    uc->product_desc="Android Auto USB stream bridge";uc->usb_desc=&aa_desc;
    uc->realize=aa_realize;uc->unrealize=aa_unrealize;uc->handle_reset=aa_reset;
    uc->handle_attach=usb_desc_attach;
    uc->handle_control=aa_control;uc->handle_data=aa_data;dc->vmsd=&aa_vmstate;
    device_class_set_props(dc,aa_props);set_bit(DEVICE_CATEGORY_USB,dc->categories);
}
static const TypeInfo aa_info={.name=TYPE_USB_ANDROID_AUTO,.parent=TYPE_USB_DEVICE,
    .instance_size=sizeof(USBAndroidAuto),.instance_init=aa_init,.class_init=aa_class};
static void aa_register(void){type_register_static(&aa_info);}
type_init(aa_register)
