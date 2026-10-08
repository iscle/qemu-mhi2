"""QEMU backend options for the soldered Marvell SDIO radio."""
import os


def arguments(wifi='user', ssid='QEMU Wi-Fi', bluetooth='', cellular='off'):
    if not 1 <= len(ssid.encode('utf-8')) <= 32 or '\0' in ssid:
        raise ValueError('Wi-Fi SSID must contain 1–32 UTF-8 bytes and no NUL')
    if not wifi:
        raise ValueError('Wi-Fi backend must be a QEMU -nic option or "none"')
    # The driver.property=value form of -global preserves the literal value.
    options = ['-global', 'mv8787-sdio.ssid=' + ssid]
    if wifi == 'none':
        options += ['-nic', 'none']
    else:
        options += ['-nic', wifi + ',model=mv8787-sdio']
    if bluetooth:
        options += ['-chardev', bluetooth + ',id=mhi2-bt',
                    '-global', 'mv8787-sdio.bluetooth-chardev=mhi2-bt']
    if cellular not in ('off', '3g', '4g'):
        raise ValueError('Cellular mode must be off, 3g, or 4g')
    if cellular != 'off':
        options += ['-netdev', 'user,id=cellular,net=10.0.3.0/24',
                    '-device', 'usb-mhi2-modem,id=cellmodem,bus=usb-bus.2,port=1,'
                    'netdev=cellular,lte=' + ('on' if cellular == '4g' else 'off')]
    return options


def from_environment():
    return arguments(os.environ.get('MHI2_WIFI', 'user'),
                     os.environ.get('MHI2_WIFI_SSID', 'QEMU Wi-Fi'),
                     os.environ.get('MHI2_BLUETOOTH', ''),
                     os.environ.get('MHI2_CELLULAR', 'off'))
