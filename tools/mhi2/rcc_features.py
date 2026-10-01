"""Feature-state vector supplied by the simulated RCC to the native MMX FEC app.

This is an emulator fixture, not a signed FEC container for a physical unit.
The original MMX FEC service distributes these states to its native clients.
"""
import struct

# Known MHI2 feature IDs; each consumer filters the vector for its own app ID.
# Reference: M.I.B. wiki, MHI2-MHI2Q-FEC-overview; speech IDs also present in
# the K3342 speech/SmartphoneIntegration consumers.
FEATURES = {
    0x00030000: 'AMI / USB media',
    0x00030001: 'Gracenote',
    0x00040100: 'Navigation',
    0x00050000: 'Bluetooth',
    0x00060100: 'Vehicle data interface',
    0x00060200: 'Infotainment control',
    0x00060300: 'MirrorLink',
    0x00060400: 'Sport HMI',
    0x00060500: 'Sport Chrono',
    0x00060700: 'Mobile online services',
    0x00060800: 'Apple CarPlay',
    0x00060900: 'Android Auto',
    0x00060a00: 'Performance data interface',
    0x00060b00: 'Baidu CarLife',
    0x00060f00: 'DAB full',
    0x00070100: 'Speech dialogue',
    0x00070200: 'Navigation speech dialogue',
    0x00070400: 'In-car communication',
    # FSC list in P470_N60S5MIBH3_EU/Mib2 content.pkg (brand variants).
    0x02300040: 'P470 map database',
    0x09300002: 'P470 map database',
    0x08300002: 'P470 map database',
    0x07300040: 'P470 map database',
    0x06300040: 'P470 map database',
    0x03300040: 'P470 map database',
}

# Production map data public modulus, present byte-for-byte at 0x178a900 in
# K3342 RCC/efs-system/21/default/efs-system.efs. RSA exponent 3, SHA-1,
# PKCS#1 v1.5. Also independently matches the supplied map content signatures.
DATA_MODULUS = int(
    'c0f389eec7b66c9dc736508ff88aeb1fb113942ead020814d08d29e868f14b2086bc'
    'd7ddccba7559f999e76d24619660bbe17434da59988087f2a99cd465b1ff423522b7'
    '8cb0de463a669613d356dfa9e86e0e2e0b6dab5de89131c5a0727aeab1767278a'
    'b101dcd9c3cfc1026705c1dab3bf53bf50afafb3f52da2ceb0bee57', 16)


def verify_data_signature(digest, signature):
    """Check the exact RSA encoded message; reject malformed lengths/padding."""
    if len(digest) != 20 or len(signature) != 128:
        return False
    integer = int.from_bytes(signature, 'big')
    if integer >= DATA_MODULUS:
        return False
    tail = bytes.fromhex('3021300906052b0e03021a05000414') + digest
    expected = b'\0\1' + b'\xff'*(128-len(tail)-3) + b'\0' + tail
    return pow(integer, 3, DATA_MODULUS).to_bytes(128, 'big') == expected


def state_vector():
    # asi.fec.SFecState[]: optional array, optional struct, uint32 fsid/index,
    # enum EFecState.ePermissionGranted=1. No signature-verification response.
    return struct.pack('!BI', 0, len(FEATURES)) + b''.join(
        struct.pack('!BIIi', 0, feature, index, 1)
        for index, feature in enumerate(FEATURES))
