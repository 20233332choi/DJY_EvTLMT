import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "gateway"))

from daly_bms import DalyTelemetryDecoder, build_read_request, extract_frames  # noqa: E402


SAMPLE_FRAMES = [
    "A5 01 90 08 02 12 00 00 75 30 02 00 F9",
    "A5 01 91 08 0E D2 02 0E CE 01 00 00 FE",
    "A5 01 92 08 38 01 38 01 00 00 00 00 B2",
    "A5 01 93 08 00 01 01 ED 00 00 A0 00 D0",
    "A5 01 94 08 0E 02 00 00 00 00 00 3A 8C",
    "A5 01 95 08 01 0E CE 0E D2 0E D1 00 DF",
    "A5 01 95 08 02 0E CF 0E D1 0E D1 00 E0",
    "A5 01 95 08 03 0E D1 0E D1 0E D2 00 E4",
    "A5 01 95 08 04 0E D2 0E D2 0E CF 00 E4",
    "A5 01 95 08 05 0E D1 0E CF 00 00 00 04",
    "A5 01 96 08 01 38 38 00 00 FF FF FF B2",
    "A5 01 97 08 00 00 00 00 00 00 00 00 45",
    "A5 01 98 08 00 00 00 00 00 00 00 00 46",
]


class DalyBmsTests(unittest.TestCase):
    def test_builds_only_public_read_requests(self):
        self.assertEqual(build_read_request(0x90).hex(" ").upper(),
                         "A5 40 90 08 00 00 00 00 00 00 00 00 7D")
        with self.assertRaises(ValueError):
            build_read_request(0xD9)

    def test_extracts_checksum_valid_frames(self):
        raw = bytearray(b"noise" + bytes.fromhex(SAMPLE_FRAMES[0]) + b"tail")
        frames = extract_frames(raw)
        self.assertEqual(len(frames), 1)
        self.assertEqual(frames[0][2], 0x90)
        self.assertEqual(raw, bytearray(b"tail"))

    def test_decodes_live_r24ts_capture(self):
        decoder = DalyTelemetryDecoder()
        for text in SAMPLE_FRAMES:
            decoder.consume(bytes.fromhex(text))
        data = decoder.snapshot()
        self.assertEqual(data["battery_pack_voltage_v"], 53.0)
        self.assertEqual(data["battery_current_a"], 0.0)
        self.assertEqual(data["battery_soc_pct"], 51.2)
        self.assertEqual(data["bms_cell_count"], 14)
        self.assertEqual(data["bms_cycle_count"], 58)
        self.assertEqual(data["bms_remaining_capacity_ah"], 40.96)
        self.assertEqual(data["bms_max_cell_number"], 2)
        self.assertAlmostEqual(data["bms_cell_delta_mv"], 4.0)
        self.assertEqual(len(data["bms_cell_voltages_v"]), 14)
        self.assertEqual(data["bms_temperatures_c"], [16.0, 16.0])
        self.assertFalse(data["bms_fault"])
        self.assertFalse(data["bms_balancing"])


if __name__ == "__main__":
    unittest.main()
