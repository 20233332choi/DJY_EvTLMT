import io
import json
import math
import sys
import threading
import time
import unittest
import urllib.error
import urllib.request
from http.server import ThreadingHTTPServer
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "gateway"))

from ev_gateway import (  # noqa: E402
    DisplayTestState,
    EVGateway,
    TelemetryStore,
    is_local_http_request,
    make_handler,
    normalize_packet,
    parse_front_status,
    wrapped_sas_delta,
)


class EVGatewayTests(unittest.TestCase):
    def test_parses_front_direct_sensor_status(self):
        packet = parse_front_status(
            b"FRONT sas_raw=8192 sas_abs=180.00 sas_ok=1 "
            b"tps_raw=868 tps_pct=0.00 tps_ok=1 "
            b"tqv_pct=40 regen_pct=0 mode=1 flags=0x01\r\n"
        )
        self.assertIsNotNone(packet)
        assert packet is not None
        self.assertEqual(packet["sas_raw"], 8192)
        self.assertAlmostEqual(packet["sas_relative_deg"], 0.0)
        self.assertAlmostEqual(packet["sas_deg"], 0.0)
        self.assertTrue(packet["sas_ok"])
        self.assertTrue(packet["tps_ok"])
        self.assertEqual(packet["tv_requested_pct"], 40.0)

    def test_sas_wrap_uses_shortest_14_bit_delta(self):
        self.assertEqual(wrapped_sas_delta(4, 16380), 8)
        self.assertEqual(wrapped_sas_delta(16380, 4), -8)

    def test_normalizes_current_esp_packet(self):
        packet = normalize_packet(
            {
                "seq": 3, "rpm_left": 1604, "rpm_right": 2609,
                "tps_pct": 50, "stm_online": True,
                "wifi_connected": True, "wifi_rssi_dbm": -61,
                "internet_relay_enabled": True, "internet_relay_tx": 12,
            }
        )
        self.assertEqual(packet["rpm_left"], 1604)
        self.assertTrue(packet["stm_online"])
        self.assertTrue(packet["wifi_connected"])
        self.assertEqual(packet["wifi_rssi_dbm"], -61)
        self.assertEqual(packet["internet_relay_tx"], 12)
        expected = ((1604 + 2609) / 2 / 3.8) * (2 * math.pi * 0.2286) / 60 * 3.6
        self.assertAlmostEqual(packet["speed_kmh"], expected, places=3)

    def test_normalizes_future_esp_wireless_bms_packet(self):
        packet = normalize_packet({
            "battery_pack_voltage_v": 53.0,
            "battery_current_a": -12.5,
            "battery_power_kw": -0.6625,
            "battery_soc_pct": 51.2,
            "bms_online": True,
            "bms_cell_count": 14,
            "bms_cell_delta_mv": 4,
            "bms_cell_voltages_v": [3.790, 3.794],
        })
        self.assertTrue(packet["bms_online"])
        self.assertEqual(packet["bms_source"], "ESP / WIRELESS")
        self.assertEqual(packet["bms_cell_count"], 14)
        self.assertEqual(packet["bms_cell_voltages_v"], [3.790, 3.794])

    def test_normalizes_future_gnss_packet(self):
        packet = normalize_packet({
            "gnss_online": True,
            "gnss_fix_type": 3,
            "gnss_satellites": 12,
            "gnss_latitude_deg": 37.1234567,
            "gnss_longitude_deg": 127.1234567,
            "gnss_altitude_m": 42.5,
            "gnss_heading_deg": 361.5,
            "gnss_hdop": 0.8,
            "gnss_age_ms": 120,
        })
        self.assertTrue(packet["gnss_online"])
        self.assertEqual(packet["gnss_fix_type"], 3)
        self.assertEqual(packet["gnss_satellites"], 12)
        self.assertAlmostEqual(packet["gnss_latitude_deg"], 37.1234567)
        self.assertAlmostEqual(packet["gnss_longitude_deg"], 127.1234567)
        self.assertEqual(packet["gnss_heading_deg"], 1.5)

    def test_aliases_and_limits(self):
        packet = normalize_packet(
            {
                "rpm_l": 10,
                "rpm_r": 20,
                "throttle_pct": 120,
                "fault": 4,
                "soc_pct": 120,
                "differential_pct": -140,
                "tv_gain_pct": 125,
                "mode": "QULIFYING",
            }
        )
        self.assertEqual(packet["rpm_right"], 20)
        self.assertEqual(packet["tps_pct"], 100.0)
        self.assertEqual(packet["fault_code"], 4)
        self.assertEqual(packet["battery_soc_pct"], 100.0)
        self.assertEqual(packet["tv_percent"], -100.0)
        self.assertEqual(packet["tqv_setting_pct"], 100.0)
        self.assertEqual(packet["drive_mode"], "QUALIFYING")

    def test_control_fields_keep_requested_and_applied_separate(self):
        packet = normalize_packet(
            {
                "tv_requested_pct": 80,
                "tv_applied_pct": 45,
                "regen_requested_pct": 30,
                "regen_applied_pct": 0,
                "drive_mode": 3,
                "pit_adjust_allowed": True,
            }
        )
        self.assertEqual(packet["tv_requested_pct"], 80.0)
        self.assertEqual(packet["tv_applied_pct"], 45.0)
        self.assertTrue(packet["tv_limited"])
        self.assertTrue(packet["regen_limited"])
        self.assertEqual(packet["drive_mode"], "ATTACK")
        self.assertEqual(packet["drive_mode_id"], 3)

    def test_store_reports_loss_and_stale(self):
        store = TelemetryStore()
        store.update({"seq": 10}, now=1.0)
        store.update({"seq": 13}, now=1.1)
        live = store.snapshot(now=1.2)
        stale = store.snapshot(now=3.0)
        self.assertEqual(live["lost_packets"], 2)
        self.assertTrue(live["online"])
        self.assertFalse(stale["online"])

    def test_display_test_overrides_are_isolated(self):
        state = DisplayTestState()
        state.apply({"action": "set_rpm", "value": 6000})
        state.apply({"action": "set_battery", "value": 25})
        state.apply({"action": "set_flag", "value": "YELLOW"})
        state.apply({"action": "set_mode", "value": "ATTACK"})
        state.apply({"action": "set_tv", "value": -35})
        state.apply({"action": "set_tqv", "value": 60})
        state.apply({"action": "set_sensor", "sensor": "bms_ok", "value": False})
        snapshot = state.snapshot()
        self.assertEqual(snapshot["rpm_left"], 6000)
        self.assertEqual(snapshot["battery_soc_pct"], 25.0)
        self.assertEqual(snapshot["flag"], "YELLOW")
        self.assertEqual(snapshot["drive_mode"], "ATTACK")
        self.assertEqual(snapshot["tv_percent"], -35.0)
        self.assertEqual(snapshot["tqv_setting_pct"], 60.0)
        self.assertFalse(snapshot["bms_ok"])
        self.assertTrue(snapshot["test_override"])

        state.apply({"action": "clear_overrides"})
        cleared = state.snapshot()
        self.assertNotIn("rpm_left", cleared)
        self.assertNotIn("flag", cleared)
        self.assertFalse(cleared["test_override"])

    def test_display_rpm_can_follow_live_throttle_only_as_override(self):
        state = DisplayTestState()
        state.apply({"action": "set_rpm_follow_throttle", "value": True, "max_rpm": 12000})
        quarter = state.snapshot(25.0)
        full = state.snapshot(100.0)
        self.assertEqual(quarter["rpm_left"], 3000)
        self.assertEqual(quarter["rpm_right"], 3000)
        self.assertEqual(full["rpm_left"], 12000)
        self.assertEqual(full["rpm_test_mode"], "THROTTLE")
        self.assertTrue(full["test_override"])

        state.apply({"action": "set_rpm_follow_throttle", "value": False})
        stopped = state.snapshot(50.0)
        self.assertNotIn("rpm_left", stopped)
        self.assertFalse(stopped["test_override"])

    def test_display_override_never_reaches_pit_snapshot(self):
        gateway = EVGateway("127.0.0.1", 9003, "127.0.0.1", 9004)
        try:
            gateway.store.update({"seq": 1, "rpm_left": 1234, "battery_soc_pct": 61})
            gateway.display_test.apply({"action": "set_rpm", "value": 9999})
            gateway.display_test.apply({"action": "set_battery", "value": 5})

            pit = gateway.snapshot()
            driver_test = gateway.snapshot(include_display_test=True)

            self.assertEqual(pit["rpm_left"], 1234)
            self.assertEqual(pit["battery_soc_pct"], 61)
            self.assertNotIn("test_override", pit)
            self.assertEqual(driver_test["rpm_left"], 9999)
            self.assertEqual(driver_test["battery_soc_pct"], 5)
            self.assertTrue(driver_test["test_override"])
        finally:
            gateway.forward_socket.close()

    def test_pit_control_is_read_only_by_default(self):
        gateway = EVGateway("127.0.0.1", 9003, "127.0.0.1", 9004)
        try:
            gateway.store.update({"seq": 1, "pit_adjust_allowed": True, "speed_kmh": 0, "tps_pct": 0})
            result = gateway.submit_config({"request_id": 7, "tv_limit_pct": 60})
            self.assertFalse(result["ok"])
            self.assertEqual(result["command_status"], "REJECTED")
        finally:
            gateway.forward_socket.close()

    def test_pit_control_requires_vehicle_permission(self):
        gateway = EVGateway("127.0.0.1", 9003, "127.0.0.1", 9004, enable_control=True)
        try:
            gateway.latest_udp_sender = ("127.0.0.1", 9999)
            gateway.store.update({"seq": 1, "pit_adjust_allowed": False, "speed_kmh": 0, "tps_pct": 0})
            denied = gateway.submit_config({"request_id": 8, "tv_limit_pct": 60})
            self.assertFalse(denied["ok"])
            gateway.store.update({"seq": 2, "pit_adjust_allowed": True, "speed_kmh": 0, "tps_pct": 0})
            accepted = gateway.submit_config({"request_id": 9, "tv_limit_pct": 60})
            self.assertTrue(accepted["ok"])
            self.assertEqual(accepted["command_status"], "SENT")
        finally:
            gateway.forward_socket.close()

    def test_live_control_is_five_percent_and_allowed_while_moving(self):
        gateway = EVGateway("127.0.0.1", 9003, "127.0.0.1", 9004, enable_control=True)
        stream = io.BytesIO()
        try:
            gateway.serial_stream = stream
            gateway.store.update({
                "seq": 1,
                "stm_online": True,
                "speed_kmh": 42.0,
                "tps_pct": 55.0,
                "fault_code": 0,
            })
            result = gateway.submit_live_control({
                "type": "live_tv",
                "request_id": 17,
                "strength_pct": 53,
                "limit_pct": 88,
                "tv_enable": True,
            })
            self.assertTrue(result["ok"])
            self.assertEqual(result["live_strength_pct"], 55)
            self.assertEqual(result["live_limit_pct"], 90)
            command = json.loads(stream.getvalue().decode("utf-8"))
            self.assertEqual(command["type"], "live_tv")
            self.assertEqual(command["strength_pct"], 55)
            self.assertEqual(command["limit_pct"], 90)
        finally:
            gateway.forward_socket.close()

    def test_live_control_fault_allows_only_neutral(self):
        gateway = EVGateway("127.0.0.1", 9003, "127.0.0.1", 9004, enable_control=True)
        stream = io.BytesIO()
        try:
            gateway.serial_stream = stream
            gateway.store.update({"seq": 1, "stm_online": True, "fault_code": 3})
            denied = gateway.submit_live_control({
                "type": "live_tv", "request_id": 1,
                "strength_pct": 50, "limit_pct": 100, "tv_enable": True,
            })
            self.assertFalse(denied["ok"])
            neutral = gateway.submit_live_control({
                "type": "live_tv", "request_id": 2,
                "strength_pct": 0, "limit_pct": 100, "tv_enable": False,
            })
            self.assertTrue(neutral["ok"])
        finally:
            gateway.forward_socket.close()

    def test_live_control_ack_stays_latched_for_expected_sequence(self):
        gateway = EVGateway("127.0.0.1", 9003, "127.0.0.1", 9004, enable_control=True)
        try:
            gateway.command_state.update({"command_status": "ACKED", "command_expected_seq": 23})
            gateway.store.update({"seq": 1, "config_seq": 23, "config_ack": False})
            self.assertTrue(gateway.snapshot()["config_ack"])
        finally:
            gateway.forward_socket.close()

    def test_live_control_deadman_requests_neutral_without_cmake_refresh(self):
        gateway = EVGateway("127.0.0.1", 9003, "127.0.0.1", 9004, enable_control=True)
        stream = io.BytesIO()
        gateway.serial_stream = stream
        gateway.store.update({"seq": 1, "stm_online": True, "fault_code": 0})
        heartbeat = threading.Thread(target=gateway.control_heartbeat_loop, daemon=True)
        heartbeat.start()
        try:
            result = gateway.submit_live_control({
                "type": "live_tv", "request_id": 24,
                "strength_pct": 50, "limit_pct": 80, "tv_enable": True,
            })
            self.assertTrue(result["ok"])
            deadline = time.monotonic() + 1.2
            while gateway.snapshot()["command_status"] != "FAILSAFE":
                self.assertLess(time.monotonic(), deadline)
                time.sleep(0.02)
            commands = [json.loads(line) for line in stream.getvalue().decode("utf-8").splitlines()]
            self.assertGreaterEqual(len(commands), 2)
            self.assertEqual(commands[-1]["strength_pct"], 0)
            self.assertFalse(commands[-1]["tv_enable"])
            self.assertFalse(gateway.snapshot()["live_control_active"])
        finally:
            gateway.stop_event.set()
            heartbeat.join(1.0)
            gateway.forward_socket.close()

    def test_relay_requires_exact_bearer_token(self):
        gateway = EVGateway("127.0.0.1", 9003, "127.0.0.1", 9004, relay_token="secret")
        try:
            self.assertTrue(gateway.relay_authorized("Bearer secret"))
            self.assertFalse(gateway.relay_authorized("Bearer wrong"))
            self.assertFalse(gateway.relay_authorized(""))
        finally:
            gateway.forward_socket.close()

    def test_relay_returns_queued_command(self):
        gateway = EVGateway(
            "127.0.0.1", 9003, "127.0.0.1", 9004,
            enable_control=True, relay_token="secret",
        )
        try:
            first = gateway.relay_exchange({"seq": 1, "stm_online": True}, "CAR-A")
            self.assertIsNone(first["command"])
            result = gateway.submit_live_control({
                "type": "live_tv", "request_id": 44,
                "strength_pct": 35, "limit_pct": 80, "tv_enable": True,
            })
            self.assertTrue(result["ok"])
            self.assertIn("internet relay", result["command_message"])
            second = gateway.relay_exchange({"seq": 2, "stm_online": True}, "CAR-A")
            self.assertEqual(second["command"]["request_id"], 44)
            self.assertEqual(gateway.snapshot()["telemetry_transport"], "INTERNET_RELAY")
        finally:
            gateway.forward_socket.close()

    def test_harmless_relay_ping_does_not_require_stm_online(self):
        gateway = EVGateway(
            "127.0.0.1", 9003, "127.0.0.1", 9004,
            enable_control=True, relay_token="secret",
        )
        try:
            gateway.relay_exchange({"seq": 1, "stm_online": False}, "BENCH")
            result = gateway.submit_control({"type": "relay_ping", "request_id": 91})
            self.assertTrue(result["ok"])
            response = gateway.relay_exchange({"seq": 2, "stm_online": False}, "BENCH")
            self.assertEqual(response["command"], {"type": "relay_ping", "request_id": 91})
            self.assertEqual(gateway.snapshot()["command_status"], "SENT")
            gateway.relay_exchange({
                "seq": 3, "stm_online": False, "internet_relay_commands": 1,
            }, "BENCH")
            snapshot = gateway.snapshot()
            self.assertEqual(snapshot["command_status"], "ACKED")
            self.assertIn("ESP received internet ping seq 91", snapshot["command_message"])
        finally:
            gateway.forward_socket.close()

    def test_wired_transport_wins_while_fresh(self):
        gateway = EVGateway("127.0.0.1", 9003, "127.0.0.1", 9004, relay_token="secret")
        try:
            gateway.accept({"seq": 10}, transport="USB")
            accepted = gateway.accept({"seq": 99}, transport="INTERNET_RELAY")
            self.assertFalse(accepted)
            snapshot = gateway.snapshot()
            self.assertEqual(snapshot["seq"], 10)
            self.assertEqual(snapshot["telemetry_transport"], "USB")
            self.assertTrue(snapshot["relay_link_online"])
        finally:
            gateway.forward_socket.close()

    def test_forwarded_ngrok_request_is_not_local_control(self):
        self.assertTrue(is_local_http_request("127.0.0.1", {}))
        self.assertFalse(is_local_http_request(
            "127.0.0.1", {"X-Forwarded-For": "203.0.113.9"}
        ))
        self.assertFalse(is_local_http_request("192.0.2.10", {}))

    def test_http_vehicle_exchange_is_authenticated(self):
        gateway = EVGateway("127.0.0.1", 9003, "127.0.0.1", 9004, relay_token="relay-secret-1234")
        server = ThreadingHTTPServer(("127.0.0.1", 0), make_handler(gateway))
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        url = f"http://127.0.0.1:{server.server_port}/api/vehicle/exchange"
        body = json.dumps({"seq": 77, "stm_online": True}).encode("utf-8")
        try:
            unauthorized = urllib.request.Request(url, data=body, method="POST")
            with self.assertRaises(urllib.error.HTTPError) as denied:
                urllib.request.urlopen(unauthorized, timeout=2)
            self.assertEqual(denied.exception.code, 401)
            denied.exception.read()
            denied.exception.close()

            authorized = urllib.request.Request(
                url, data=body, method="POST",
                headers={
                    "Authorization": "Bearer relay-secret-1234",
                    "Content-Type": "application/json",
                    "X-Vehicle-ID": "CAR-A",
                },
            )
            with urllib.request.urlopen(authorized, timeout=2) as response:
                result = json.loads(response.read().decode("utf-8"))
            self.assertTrue(result["ok"])
            self.assertEqual(result["vehicle_id"], "CAR-A")
            self.assertEqual(gateway.snapshot()["seq"], 77)
        finally:
            server.shutdown()
            server.server_close()
            gateway.forward_socket.close()

    def test_http_pit_dashboard_was_removed_in_favor_of_cmake(self):
        gateway = EVGateway("127.0.0.1", 9003, "127.0.0.1", 9004)
        server = ThreadingHTTPServer(("127.0.0.1", 0), make_handler(gateway))
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()
        base = f"http://127.0.0.1:{server.server_port}"
        try:
            with urllib.request.urlopen(f"{base}/driver", timeout=2) as response:
                self.assertEqual(response.status, 200)
            with self.assertRaises(urllib.error.HTTPError) as missing:
                urllib.request.urlopen(f"{base}/pit", timeout=2)
            self.assertEqual(missing.exception.code, 404)
            missing.exception.read()
            missing.exception.close()
        finally:
            server.shutdown()
            server.server_close()
            gateway.forward_socket.close()


if __name__ == "__main__":
    unittest.main()
