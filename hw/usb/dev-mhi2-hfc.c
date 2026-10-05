/* Local MHI2 media connector Hub Feature Controller fixture.
 * Microchip AN1941: HFC is a separate 0424:2530 device on an internal hub port.
 * K5126 mediaconnector uses vendor requests 3/4 for XDATA and iProduct for
 * capabilities. No Apple authentication hardware or attached I2C slaves.
 */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/log.h"
#include "hw/usb/usb.h"
#include "migration/vmstate.h"
#include "desc.h"

#define TYPE_MHI2_HFC "usb-mhi2-hfc"
OBJECT_DECLARE_SIMPLE_TYPE(MHI2HFC, MHI2_HFC)
struct MHI2HFC {
    USBDevice parent_obj;
    uint8_t xdata[65536];
    bool i2c_enabled;
};
static const USBDescStrings hfc_strings = {
    [1] = "Microchip",
    [3] = "MHI2-HUB-001",
    /* K5126 CUSBDeviceI2CCapabilities version 1, two host USB ports.
     * No role switch, SIM, SD, temperature sensor or authentication chip. */
    [2] = "Ve10Di55P1u1abP2u2abP3n",
};
static const USBDescIface hfc_iface = {
    .bInterfaceClass = 0xff, .bInterfaceSubClass = 0xff,
};
static const USBDescDevice hfc_device = {
    .bcdUSB = 0x0200, .bMaxPacketSize0 = 64, .bNumConfigurations = 1,
    .confs = (USBDescConfig[]) {{
        .bNumInterfaces = 1, .bConfigurationValue = 1,
        .bmAttributes = USB_CFG_ATT_ONE | USB_CFG_ATT_SELFPOWER,
        .nif = 1, .ifs = &hfc_iface,
    }},
};
static const USBDesc hfc_desc = {
    .id = {.idVendor = 0x0424, .idProduct = 0x2530, .bcdDevice = 0x0100,
           .iManufacturer = 1, .iProduct = 2, .iSerialNumber = 3},
    .full = &hfc_device, .high = &hfc_device, .str = hfc_strings,
};
static void hfc_control(USBDevice *dev, USBPacket *p, int request, int value,
                        int index, int length, uint8_t *data)
{
    MHI2HFC *s = MHI2_HFC(dev);
    if (usb_desc_handle_control(dev, p, request, value, index, length, data) >= 0) {
        return;
    }
    switch (request) {
    case 0x4103: /* Write XDATA. */
    case 0xc104: /* Read XDATA. */
        if (index || length < 0 || length > sizeof(s->xdata) - value) {
            break;
        }
        if (request == 0x4103) {
            memcpy(s->xdata + value, data, length);
        } else {
            memcpy(data, s->xdata + value, length);
            p->actual_length = length;
        }
        return;
    case 0x4105: /* Execute the uploaded I2C frequency setup routine. */
        if (!index && !length && value == 0xa000) {
            return;
        }
        break;
    case 0x4170: /* AN1941 CMD_I2C_ENTER_PASSTHRU. */
        if (!index && !length && !value) {
            s->i2c_enabled = true;
            return;
        }
        break;
    }
    /* An unpopulated I2C bus must NACK, including authentication addresses. */
    qemu_log_mask(LOG_UNIMP, "mhi2-hfc: unsupported request=%04x value=%04x index=%04x length=%d\n",
                  request, value, index, length);
    p->status = USB_RET_STALL;
}
static void hfc_reset(USBDevice *dev)
{
    MHI2HFC *s = MHI2_HFC(dev);
    memset(s->xdata, 0, sizeof(s->xdata));
    s->i2c_enabled = false;
}
static void hfc_realize(USBDevice *dev, Error **errp)
{
    usb_desc_init(dev);
}
static const VMStateDescription hfc_vmstate = {
    .name = TYPE_MHI2_HFC, .version_id = 1, .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_USB_DEVICE(parent_obj, MHI2HFC),
        VMSTATE_UINT8_ARRAY(xdata, MHI2HFC, 65536),
        VMSTATE_BOOL(i2c_enabled, MHI2HFC),
        VMSTATE_END_OF_LIST()
    },
};
static void hfc_class_init(ObjectClass *klass, const void *data)
{
    USBDeviceClass *uc = USB_DEVICE_CLASS(klass);
    uc->realize = hfc_realize;
    uc->usb_desc = &hfc_desc;
    uc->product_desc = "MHI2 media connector HFC";
    uc->handle_attach = usb_desc_attach;
    uc->handle_reset = hfc_reset;
    uc->handle_control = hfc_control;
    DEVICE_CLASS(klass)->vmsd = &hfc_vmstate;
}
static const TypeInfo hfc_info = {
    .name = TYPE_MHI2_HFC, .parent = TYPE_USB_DEVICE,
    .instance_size = sizeof(MHI2HFC), .class_init = hfc_class_init,
};
static void hfc_register(void) { type_register_static(&hfc_info); }
type_init(hfc_register)
