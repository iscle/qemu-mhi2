import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))

import firmware_profile
import skoda_profile
from rcc_persistence import Persistence
from rcc_services import Services


class ProfileTests(unittest.TestCase):
    def test_existing_brand_selection_is_unchanged(self):
        self.assertEqual(firmware_profile.select_profile({})['brand'], 'volkswagen')
        self.assertEqual(firmware_profile.select_profile(
            {'release': 'MHI2_ER_AU37x_P5089'})['brand'], 'audi')
        self.assertEqual(firmware_profile.select_profile(
            {'release': 'MHI2_ER_POG11_K5126'})['brand'], 'porsche')

    def test_exact_skoda_release_and_mu_select_experimental_profile(self):
        profile = firmware_profile.select_profile(
            {'release': skoda_profile.RELEASE, 'MUVersion': skoda_profile.MU})
        self.assertEqual(profile['brand'], 'skoda')
        self.assertEqual(profile['main_viewport'], (1280, 640))
        self.assertEqual(profile['cockpit_viewport'], (1280, 480))
        self.assertEqual(profile['touch_wire_divisor'], (2, 2))
        self.assertIsNone(profile['oscillator_12mhz'])

    def test_metadata_reader_ignores_other_common_fields_and_rejects_duplicates(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / 'metainfo2.txt'
            path.write_text('[common]\nrelease="MHI2_ER_SKG13_P4526"\n'
                            'MUVersion="1440"\nvariant0="-SKG-EXPERIMENT"\n')
            self.assertEqual(skoda_profile.read_metadata(path)['MUVersion'], '1440')
            path.write_text('[common]\nrelease="MHI2_ER_SKG13_P4526"\n'
                            'release="MHI2_ER_SKG13_P4526"\nMUVersion="1440"\n')
            with self.assertRaisesRegex(ValueError, 'Duplicate'):
                skoda_profile.read_metadata(path)
    def test_mismatched_mu_and_conflicting_brand_are_rejected(self):
        with self.assertRaises(ValueError):
            firmware_profile.select_profile(
                {'release': skoda_profile.RELEASE, 'MUVersion': '1430'})
        with self.assertRaises(ValueError):
            firmware_profile.select_profile(
                {'release': skoda_profile.RELEASE, 'MUVersion': skoda_profile.MU},
                brand='audi')

    def test_profile_metadata_has_no_private_fixture_pins_or_vehicle_data(self):
        data = skoda_profile.profile_data()
        serialized = json.dumps(data).lower()
        for forbidden in ('sha256', 'archive', 'required_members',
                          'extraction_manifest', 'vin', 'adaptation', 'coding'):
            self.assertNotIn(forbidden, serialized)

    def test_skoda_persistence_is_read_only_without_vehicle_identity(self):
        persistence = Persistence(metadata={
            'release': skoda_profile.RELEASE, 'MUVersion': skoda_profile.MU})
        self.assertTrue(persistence.read_only)
        self.assertIsNone(persistence.identity)
        self.assertEqual(set(persistence.values), {
            '30:1966083', '30:1966084', '46924065:401'})

    def test_service_vin_reply_uses_empty_sk_default_and_preserves_legacy_identity(self):
        definition_name = 'DSICarVehicleStates'

        def vin_reply(identity):
            with patch('rcc_services.Persistence',
                       return_value=SimpleNamespace(identity=identity)):
                services = Services()
            spec = next(spec for spec in
                        services.definitions[definition_name]['replies'].values()
                        if spec['name'] == 'updateVINData')
            return services.initial(definition_name, spec)

        self.assertEqual(vin_reply(None), ['', 129])
        self.assertEqual(vin_reply({'vin': 'LEGACY-TEST-VIN'}),
                         ['LEGACY-TEST-VIN', 129])

    def test_configure_requires_explicit_media_and_experiment_settings(self):
        with self.assertRaises(ValueError):
            firmware_profile.configure(skoda_profile.ID)
        with tempfile.TemporaryDirectory() as temporary:
            media = Path(temporary)
            (media / 'nor.bin').write_bytes(b'nor')
            (media / 'iram.bin').write_bytes(b'iram')
            (media / 'emmc.raw').write_bytes(b'emmc')
            (media / 'metainfo2.txt').write_text(
                '[common]\nrelease="MHI2_ER_SKG13_P4526"\nMUVersion="1440"\n')
            (media / 'ui-manifest.json').write_text(json.dumps({
                'firmware_profile': skoda_profile.ID,
                'firmware_train': skoda_profile.RELEASE,
                'firmware_mu': skoda_profile.MU,
                'emulator_only': True,
                'metadata': 'metainfo2.txt',
            }))
            old = {key: os.environ.get(key) for key in (
                'MHI2_SKODA_OSCILLATOR_12MHZ', 'MHI2_SKODA_INPUT_EXPERIMENT',
                'MHI2_FIRMWARE', 'MHI2_FIRMWARE_META', 'MHI2_METADATA',
                'MHI2_OSCILLATOR_12MHZ', 'MHI2_DEBUG_NOR', 'MHI2_EMMC',
                'MHI2_MEDIA_ROOT', 'MHI2_SHADER_CACHE')}
            try:
                os.environ['MHI2_SKODA_OSCILLATOR_12MHZ'] = '0'
                os.environ['MHI2_SKODA_INPUT_EXPERIMENT'] = '1'
                result = firmware_profile.configure(skoda_profile.ID, media)
                self.assertEqual(result['brand'], 'skoda')
                self.assertEqual(result['main_viewport'], (1280, 640))
            finally:
                for key, value in old.items():
                    if value is None:
                        os.environ.pop(key, None)
                    else:
                        os.environ[key] = value


if __name__ == '__main__':
    unittest.main()
