"""K5126 native DSI listener contracts, recovered from its proxy factories.

These interfaces are distinct from the Java DSI interfaces. Method IDs come
from the RPCStubReply dispatchers; attribute IDs and UUIDs come from GAL's
CDSISpyRetriever and CTimeProvider. Never register these for another release.
"""


def definitions(schema):
    def service(source, identity, key, methods):
        original = schema['services'][source]
        replies = {spec['name']: spec for spec in original['replies'].values()}
        selected = {str(mid): dict(replies['update' + name])
                    for mid, name in methods.items()}
        names = {name.upper() for name in methods.values()}
        return {
            'uuid': identity, 'key': key,
            'calls': {'0': 'clearNotification()',
                      '1': 'clearNotification(int[] attrs)',
                      '2': 'clearNotification(int attr)',
                      '3': 'setNotification()',
                      '4': 'setNotification(int[] attrs)',
                      '5': 'setNotification(int attr)'},
            'replies': selected,
            'attrs': {k: v for k, v in original['attrs'].items() if v in names},
            'initial_source': source,
        }

    general = service(
        'DSIGeneralVehicleStates', 'e0bd4291-037d-5649-9806-6d51438e5ecb',
        '50383fec-3fc6-5515-874e-df26404bdb84', {
            6: 'CarVelocityThreshold', 7: 'DisplayDayNightDesign',
            9: 'TankInfo', 10: 'ServiceKeyData',
            11: 'DestinationInputVelocityThreshold',
            12: 'MessagingVelocityThreshold', 13: 'TVVelocityThreshold',
            14: 'VehicleStandstill', 15: 'ReverseGear',
            16: 'BTBondingVelocityThreshold', 17: 'ParkingBrake',
        })
    clock = service(
        'DSICarTimeUnitsLanguage', 'cc574f3e-189d-5109-b9a6-fe3b6220212d',
        '6c56ec2d-fb0e-5ab3-a403-c91de840d655', {
            6: 'ClockDate', 7: 'ClockDayLightSaving', 8: 'ClockGPSSyncData',
            9: 'ClockTime', 10: 'ClockTimeZoneOffset', 11: 'MenuLanguage',
            12: 'UTCOffset', 13: 'ClockSource',
        })
    # The native GPS structure is flat; the Java version nests date/time.
    # No GPS receiver is connected, so this update is marked unavailable.
    schema['structs']['NativeSpyClockGPSSyncData'] = [
        ('year', 'Int32'), ('month', 'Int8'), ('day', 'Int8'),
        ('hours', 'Int8'), ('minutes', 'Int8'), ('seconds', 'Int8')]
    clock['replies']['8']['types'] = ['OptionalNativeSpyClockGPSSyncData', 'Int32']
    kombi = service(
        'DSICarKombi', '3b6cf3bd-93e1-53fd-b6f5-6322bbb8e482',
        '27508d99-c84d-5510-924e-a41ed766f21f', {
            38: 'BCShortTermGeneral', 41: 'BCViewOptions',
        })
    vehicle = service(
        'DSICarVehicleStates', 'c4d233e7-9b49-5aa1-889f-973d8eb645a3',
        '976e8491-d325-5496-9f7b-4ef05a49923f', {
            21: 'DynamicVehicleInfoMidFrequent',
            22: 'DynamicVehicleInfoMidFrequentViewOptions',
            23: 'DynamicVehicleInfoHighFrequent',
            24: 'DynamicVehicleInfoHighFrequentViewOptions',
        })
    return {'SpyGeneralVehicleStates': general, 'SpyCarTimeUnitsLanguage': clock,
            'SpyCarKombi': kombi, 'SpyCarVehicleStates': vehicle}
