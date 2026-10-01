"""Host checks for the dashboard backend and the Chinese UI strings."""

from __future__ import annotations

import json
import os
import re
import tempfile
import time
import unittest
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timezone
from pathlib import Path

import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "backend"))

from xhs_fetch import (  # noqa: E402
    net_followers_7d,
    normalize_cookie,
    profile_fields,
    public_stats,
    stats_from_payloads,
    token_matches,
)


PROFILE = {
    "success": True,
    "data": {
        "name": "小红",
        "fans_count": 49,
        "faved_count": 1043,
        "avatar": "https://example.invalid/avatar.jpg",
    },
}

NOTES = {
    "success": True,
    "data": {
        "seven": {"rise_fans_count": 5, "leave_fans_count": 1},
        "thirty": {"rise_fans_count": 9},
    },
}


class BackendTest(unittest.TestCase):
    def test_cookie_and_token(self) -> None:
        self.assertEqual(normalize_cookie("Cookie: a=1; b=2"), "a=1; b=2")
        self.assertEqual(normalize_cookie("  a=1  "), "a=1")
        self.assertTrue(token_matches("token-ok-1", "token-ok-1"))
        self.assertFalse(token_matches("token-ok-1", "token-ok-2"))
        self.assertFalse(token_matches("", "token-ok-1"))

    def test_profile_and_net_growth(self) -> None:
        name, fans, likes, avatar = profile_fields(PROFILE)
        self.assertEqual((name, fans, likes, avatar), ("小红", 49, 1043, "https://example.invalid/avatar.jpg"))
        self.assertEqual(net_followers_7d(NOTES), 4)
        rise_only = {"data": {"seven": {"rise_fans_count": 5}}}
        self.assertEqual(net_followers_7d(rise_only), 5)
        with self.assertRaises(ValueError):
            net_followers_7d({"success": False, "data": {}})

    def test_public_payload_matches_device_fixture(self) -> None:
        fixture = json.loads((ROOT / "tests" / "fixtures" / "xhs_stats.json").read_text(encoding="utf-8"))
        payload = public_stats("小红", 49, 1043, 4, "2026-09-30 17:40", 1780000000)
        self.assertEqual(payload, fixture)
        when = datetime(2026, 9, 30, 17, 40, tzinfo=timezone.utc)
        built = stats_from_payloads(PROFILE, NOTES, when)
        self.assertEqual(built["followers"], 49)
        self.assertEqual(built["net_followers_7d"], 4)
        self.assertEqual(built["fetched_at"], "2026-09-30 17:40")
        self.assertTrue(built["ok"])

    def test_ui_strings_are_present(self) -> None:
        source = (ROOT / "main" / "xhs_ui.c").read_text(encoding="utf-8")
        for text in (
            "配置失败，请重新刷新固件。",
            "粉丝",
            "获赞与收藏",
            "近7日净涨粉",
            "更新于",
            "尚未更新",
            "自动更新",
            "更新频率",
            "每小时",
            "每天",
            "开",
            "关",
            "设置",
            "长按确认键返回",
            "上下键调整，确认键完成",
            "重新配网",
            "配对码",
            "连接热点",
            "正在连接",
        ):
            self.assertIn(text, source)

    def test_ui_strings_are_in_the_generated_font(self) -> None:
        source = (ROOT / "main" / "xhs_ui.c").read_text(encoding="utf-8")
        font = (ROOT / "assets" / "fonts" / "xhs_font_16.c").read_text(encoding="utf-8")
        needed = set("啊呼鲁")
        for literal in re.findall(r'"((?:\\.|[^"\\])*)"', source):
            for char in literal:
                if "\u4e00" <= char <= "\u9fff" or char in "，。":
                    needed.add(char)
        covered = set()
        for start, length in re.findall(
            r"\.range_start = (\d+), \.range_length = (\d+), \.glyph_id_start = \d+,\s*"
            r"\.unicode_list = NULL",
            font,
        ):
            base = int(start)
            covered.update(chr(code) for code in range(base, base + int(length)))
        lists = {
            name: [int(item, 16) for item in re.findall(r"0x[0-9A-Fa-f]+", body)]
            for name, body in re.findall(r"unicode_list_(\d+)\[\] = \{([^}]+)\}", font)
        }
        for start, name in re.findall(
            r"\.range_start = (\d+), \.range_length = \d+, \.glyph_id_start = \d+,\s*"
            r"\.unicode_list = unicode_list_(\d+)",
            font,
        ):
            base = int(start)
            covered.update(chr(base + offset) for offset in lists[name])
        self.assertEqual(sorted(needed - covered), [])


GOOD_COOKIE = "cookie-good-value"
STALE_COOKIE = "cookie-stale-value"
OFFLINE_COOKIE = "cookie-offline-value"


def fake_fetch(cookie: str, when: datetime | None = None):
    del when
    if cookie == GOOD_COOKIE:
        return (dict(json.loads((ROOT / "tests" / "fixtures" / "xhs_stats.json").read_text(encoding="utf-8"))), "")
    if cookie == OFFLINE_COOKIE:
        raise urllib.error.URLError("offline")
    raise ValueError("rejected")


class PairingAndConsoleTest(unittest.TestCase):
    def setUp(self) -> None:
        from xhs_browser import FakeLoginBrowser
        from xhs_server import Service, clear_snapshot_cache, serve
        from xhs_state import XhsState

        self._tmpdir = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmpdir.cleanup)
        clear_snapshot_cache()
        self.state = XhsState(Path(self._tmpdir.name), fake_fetch)
        self.browser = FakeLoginBrowser(GOOD_COOKIE)
        self.service = Service(self.state, lambda: self.browser, poll_seconds=0.05)
        self.api, self.console = serve(self.service, "127.0.0.1", 0, "127.0.0.1", 0)
        self.addCleanup(self._stop_servers)
        self.api_url = "http://127.0.0.1:%d" % self.api.server_address[1]
        self.console_url = "http://127.0.0.1:%d" % self.console.server_address[1]

    def _stop_servers(self) -> None:
        self.service.stop_login()
        self.api.shutdown()
        self.console.shutdown()
        self.api.server_close()
        self.console.server_close()

    def _request(self, url: str, data: bytes | None = None, headers: dict | None = None) -> tuple[int, bytes]:
        request = urllib.request.Request(url, data=data, headers=headers or {}, method="POST" if data is not None else "GET")
        try:
            with urllib.request.urlopen(request, timeout=5) as response:
                return response.status, response.read()
        except urllib.error.HTTPError as error:
            return error.code, error.read()

    def test_login_states_and_paste_share_one_path(self) -> None:
        from xhs_browser import FakeLoginBrowser

        status, body = self._request(self.console_url + "/api/status")
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(body)["login"], "logged_out")
        self.assertNotIn(GOOD_COOKIE.encode("utf-8"), body)

        self.assertEqual(self.state.accept_cookie(STALE_COOKIE), "logged_out")
        self.assertFalse(self.state.cookie_path.is_file())

        self.state.cookie_path.write_text(STALE_COOKIE + "\n", encoding="utf-8")
        self.assertEqual(self.state.login_status(), "expired")

        self.state.cookie_path.write_text(OFFLINE_COOKIE + "\n", encoding="utf-8")
        self.assertEqual(self.state.login_status(), "logged_in")

        pasted = urllib.parse.urlencode({"cookie": GOOD_COOKIE}).encode("utf-8")
        status, body = self._request(self.console_url + "/", data=None)
        status, page = self._request(
            self.console_url + "/api/login/paste",
            data=pasted,
            headers={"Content-Type": "application/x-www-form-urlencoded"},
        )
        self.assertEqual(status, 200)
        self.assertNotIn(GOOD_COOKIE.encode("utf-8"), page)
        self.assertIn("已登录".encode("utf-8"), page)
        self.assertNotIn(b"<h1 id=\"state\">logged_out</h1>", page)

        reloaded_browser = FakeLoginBrowser(GOOD_COOKIE)
        self.assertEqual(self.state.adopt_browser_cookie(reloaded_browser), "logged_in")
        self.assertTrue(reloaded_browser.closed)
        again = type(self.state)(self.state.data_dir, fake_fetch)
        self.assertEqual(again.login_status(), "logged_in")

    def test_logout_clears_the_creator_session_and_keeps_pairing(self) -> None:
        code = "AB3K"
        self._request(self.api_url + "/api/v1/pair?code=" + code)
        status, _body = self._request(
            self.console_url + "/api/pair/confirm",
            data=json.dumps({"code": code}).encode("utf-8"),
            headers={"Content-Type": "application/json"},
        )
        self.assertEqual(status, 200)
        token = self.state.read_token()
        self.state.accept_cookie(GOOD_COOKIE)
        status, page = self._request(self.console_url + "/")
        self.assertIn("退出登录".encode("utf-8"), page)
        self.assertNotIn(b"display:none", page)

        def fetch_during_logout(cookie: str, when: datetime | None = None):
            del cookie, when
            self.state.clear_login()
            return fake_fetch(GOOD_COOKIE)

        self.state._fetch = fetch_during_logout
        self.assertEqual(self.state.accept_cookie(GOOD_COOKIE), "logged_out")
        self.assertFalse(self.state.cookie_path.is_file())
        self.state._fetch = fake_fetch
        self.state.accept_cookie(GOOD_COOKIE)

        status, page = self._request(self.console_url + "/api/login/logout", data=b"")
        self.assertEqual(status, 200)
        self.assertIn("未登录".encode("utf-8"), page)
        self.assertIn(b"display:none", page)
        self.assertNotIn(GOOD_COOKIE.encode("utf-8"), page)
        self.assertNotIn(token.encode("utf-8"), page)
        self.assertFalse(self.state.cookie_path.is_file())
        self.assertEqual(self.state.read_token(), token)
        self.assertEqual(self.state.login_status(), "logged_out")
        denied, body = self._request(
            self.api_url + "/api/v1/stats",
            headers={"X-Device-Token": token},
        )
        self.assertEqual(denied, 503)
        self.assertNotIn(GOOD_COOKIE.encode("utf-8"), body)

    def test_session_revision_changes_when_the_account_changes(self) -> None:
        from xhs_state import session_revision

        code = "AB3K"
        self._request(self.api_url + "/api/v1/pair?code=" + code)
        self._request(
            self.console_url + "/api/pair/confirm",
            data=json.dumps({"code": code}).encode("utf-8"),
            headers={"Content-Type": "application/json"},
        )
        token = self.state.read_token()
        status, body = self._request(
            self.api_url + "/api/v1/session",
            headers={"X-Device-Token": token},
        )
        self.assertEqual(status, 200)
        self.assertEqual(json.loads(body)["revision"], "")
        self.assertNotIn(GOOD_COOKIE.encode("utf-8"), body)
        self.state.accept_cookie(GOOD_COOKIE)
        status, body = self._request(
            self.api_url + "/api/v1/session",
            headers={"X-Device-Token": token},
        )
        first = json.loads(body)["revision"]
        self.assertEqual(first, session_revision(GOOD_COOKIE))
        self.assertNotIn(GOOD_COOKIE.encode("utf-8"), body)
        other = "cookie-other-value"

        def fetch_other(cookie: str, when: datetime | None = None):
            if cookie == other:
                return fake_fetch(GOOD_COOKIE)
            return fake_fetch(cookie, when)

        self.state._fetch = fetch_other
        self.state.clear_login()
        self.assertEqual(self.state.accept_cookie(other), "logged_in")
        status, body = self._request(
            self.api_url + "/api/v1/session",
            headers={"X-Device-Token": token},
        )
        second = json.loads(body)["revision"]
        self.assertNotEqual(second, first)
        self.assertEqual(second, session_revision(other))
        self.assertNotIn(other.encode("utf-8"), body)
        denied, _body = self._request(self.api_url + "/api/v1/session")
        self.assertEqual(denied, 401)

    def test_qr_login_uses_the_fake_browser(self) -> None:
        self.assertNotIn("playwright", sys.modules)
        status, _body = self._request(self.console_url + "/api/login/start", data=b"")
        self.assertEqual(status, 200)
        self.assertTrue(self.browser.opened)
        deadline = time.time() + 2
        while time.time() < deadline and self.state.login_status() != "logged_in":
            time.sleep(0.05)
        self.assertEqual(self.state.login_status(), "logged_in")
        status, qr = self._request(self.console_url + "/api/login/qr")
        self.assertIn(status, (200, 404))
        status, page = self._request(self.console_url + "/")
        self.assertNotIn(GOOD_COOKIE.encode("utf-8"), page)
        self.assertNotIn(b"playwright", page)

    def test_login_failure_is_shown_on_the_page(self) -> None:
        class Broken:
            def open_login(self) -> None:
                raise RuntimeError("sandbox")

            def close(self) -> None:
                return None

        self.service._browser_factory = lambda: Broken()
        status, page = self._request(self.console_url + "/api/login/start", data=b"")
        self.assertEqual(status, 200)
        self.assertIn("登录页没有打开".encode("utf-8"), page)
        self.assertNotIn(b"http-equiv", page)

    def test_root_chromium_disables_the_sandbox(self) -> None:
        import xhs_browser

        original = getattr(os, "geteuid", None)
        try:
            os.geteuid = lambda: 0  # type: ignore[attr-defined]
            self.assertIn("--no-sandbox", xhs_browser.chromium_launch_args())
            self.assertEqual(xhs_browser.chromium_launch_kwargs()["channel"], "chromium")
            os.geteuid = lambda: 1000  # type: ignore[attr-defined]
            self.assertEqual(xhs_browser.chromium_launch_args(), [])
            self.assertEqual(xhs_browser.chromium_launch_kwargs()["args"], [])
            boxes = [(48.0, 48.0, False), (420.0, 70.0, False), (180.0, 180.0, False)]
            self.assertEqual(xhs_browser.choose_qr_index(boxes), 2)
            self.assertEqual(
                xhs_browser.choose_qr_index([(200.0, 200.0, False), (160.0, 160.0, True)]),
                1,
            )
            self.assertIsNone(xhs_browser.choose_qr_index([(40.0, 40.0, False)]))
            self.assertEqual(xhs_browser.login_card_switch_point(800, 120, 360, 420), (1124, 156))
            title = (120.0, 90.0, 80.0, 28.0)
            icons = [
                (16.0, 16.0, 48.0, 48.0),
                (340.0, 24.0, 72.0, 72.0),
                (140.0, 420.0, 18.0, 18.0),
            ]
            self.assertEqual(xhs_browser.corner_switch_tip(icons, title), (404.0, 32.0))
            before = {"web_session": "guest"}
            self.assertFalse(xhs_browser.login_confirmed(
                "https://creator.xiaohongshu.com/login", before, dict(before),
            ))
            self.assertTrue(xhs_browser.login_confirmed(
                "https://creator.xiaohongshu.com/login",
                before,
                {"web_session": "signed-in"},
            ))
            self.assertTrue(xhs_browser.login_confirmed(
                "https://creator.xiaohongshu.com/new/home", before, dict(before),
            ))
            named = xhs_browser.session_values([
                {"name": "webId", "value": "guest"},
                {"name": "galaxy.creator.beaker.session.id", "value": "abc"},
            ])
            self.assertEqual(list(named), ["galaxy.creator.beaker.session.id"])
        finally:
            if original is None:
                delattr(os, "geteuid")
            else:
                os.geteuid = original  # type: ignore[attr-defined]

    def test_pair_confirm_delivers_token_only_on_the_device_channel(self) -> None:
        code = "AB3K"
        status, body = self._request(self.api_url + "/api/v1/pair?code=" + code)
        self.assertEqual(status, 200)
        pending = json.loads(body)
        self.assertEqual(pending["status"], "pending")
        self.assertNotIn("token", pending)

        status, body = self._request(
            self.console_url + "/api/pair/confirm",
            data=json.dumps({"code": code}).encode("utf-8"),
            headers={"Content-Type": "application/json"},
        )
        self.assertEqual(status, 200)
        self.assertNotIn(b"token", body)
        token = self.state.read_token()
        self.assertGreaterEqual(len(token), 8)
        self.assertNotIn(token.encode("utf-8"), body)

        status, body = self._request(self.api_url + "/api/v1/pair?code=" + code)
        delivered = json.loads(body)
        self.assertEqual(delivered.get("token"), token)

        status, page = self._request(self.console_url + "/")
        status_code, status_body = self._request(self.console_url + "/api/status")
        self.assertNotIn(token.encode("utf-8"), page)
        self.assertNotIn(token.encode("utf-8"), status_body)
        self.assertEqual(json.loads(status_body)["login"], "logged_out")

        self.state.accept_cookie(GOOD_COOKIE)
        status, stats = self._request(
            self.api_url + "/api/v1/stats",
            headers={"X-Device-Token": token},
        )
        self.assertEqual(status, 200)
        self.assertNotIn(token.encode("utf-8"), stats)
        self.assertNotIn(GOOD_COOKIE.encode("utf-8"), stats)
        self.assertEqual(json.loads(stats)["followers"], 49)

        status, body = self._request(self.api_url + "/api/v1/pair?code=" + code)
        self.assertNotIn("token", json.loads(body))

        other = "K7NP"
        self._request(self.api_url + "/api/v1/pair?code=" + other)
        status, _body = self._request(
            self.console_url + "/api/pair/confirm",
            data=json.dumps({"code": other}).encode("utf-8"),
            headers={"Content-Type": "application/json"},
        )
        self.assertEqual(status, 200)
        self.assertNotEqual(self.state.read_token(), token)
        denied, _body = self._request(
            self.api_url + "/api/v1/stats",
            headers={"X-Device-Token": token},
        )
        self.assertEqual(denied, 401)

    def test_discover_reply_uses_the_receiving_lan_address(self) -> None:
        from xhs_discover import (
            choose_reply_ip, device_reply, discovery_uses_wildcard, reply_ip_allowed,
            reply_ip_for_sender, route_ip_toward,
        )

        self.assertTrue(discovery_uses_wildcard(False))
        self.assertFalse(discovery_uses_wildcard(True))

        adapters = [
            ("Wi-Fi", "192.168.1.20"),
            ("vEthernet (WSL)", "172.22.32.1"),
            ("DockerNAT", "10.0.75.1"),
        ]
        self.assertEqual(reply_ip_for_sender("192.168.1.50", adapters), "192.168.1.20")
        self.assertIsNone(reply_ip_for_sender("172.17.0.8", adapters))
        self.assertEqual(choose_reply_ip("192.168.1.20", "10.0.75.1"), "192.168.1.20")
        self.assertEqual(choose_reply_ip("192.168.1.20", "172.22.32.1"), "192.168.1.20")
        self.assertIsNone(choose_reply_ip("172.17.0.2", "192.168.1.20"))
        self.assertEqual(choose_reply_ip(None, "192.168.1.20"), "192.168.1.20")
        self.assertIsNone(choose_reply_ip(None, "172.17.0.2"))
        self.assertIsNone(choose_reply_ip("0.0.0.0", "host.docker.internal"))
        self.assertEqual(route_ip_toward("127.0.0.1"), "127.0.0.1")
        self.assertIsNone(choose_reply_ip(None, route_ip_toward("127.0.0.1")))
        self.assertFalse(reply_ip_allowed("172.17.0.2", "Ethernet"))
        self.assertFalse(reply_ip_allowed("192.168.1.20", "vEthernet (WSL)"))
        self.assertFalse(reply_ip_allowed("host.docker.internal"))
        self.assertIsNone(device_reply({"status": "confirmed", "token": "tok-12345678"}, "172.17.0.2"))
        self.assertIsNone(device_reply({"status": "confirmed", "token": "tok-12345678"}, "host.docker.internal"))

        pending = device_reply({"status": "pending"}, "192.168.1.20")
        assert pending is not None
        self.assertNotIn(b"token", pending)
        self.assertNotIn(b"172.", pending)

        code = "H3NP"
        self._request(self.console_url + "/api/discover", data=json.dumps({"code": code}).encode("utf-8"),
                      headers={"Content-Type": "application/json"})
        self._request(
            self.console_url + "/api/pair/confirm",
            data=json.dumps({"code": code}).encode("utf-8"),
            headers={"Content-Type": "application/json"},
        )
        status, body = self._request(
            self.console_url + "/api/discover",
            data=json.dumps({"code": code}).encode("utf-8"),
            headers={"Content-Type": "application/json"},
        )
        console_body = json.loads(body)
        self.assertEqual(status, 200)
        reply = device_reply(console_body, "192.168.1.20")
        assert reply is not None
        parsed = json.loads(reply)
        self.assertEqual(parsed["base"], "http://192.168.1.20:8787")
        self.assertNotIn("172.", parsed["base"])
        self.assertNotIn("host.docker.internal", reply.decode("utf-8"))
        self.assertEqual(parsed["token"], self.state.read_token())

    def test_compose_launcher_and_empty_firmware_template(self) -> None:
        compose = (ROOT / "docker-compose.yml").read_text(encoding="utf-8")
        self.assertEqual(compose.count("host_ip: 0.0.0.0"), 2)
        self.assertIn("published: 8787", compose)
        self.assertNotIn("host_ip: 127.0.0.1", compose)
        self.assertIn("published: 8790", compose)
        self.assertNotIn("network_mode: host", compose)
        self.assertIn("xhs-data:/data", compose)
        launcher = (ROOT / "backend" / "start-xhs.ps1").read_text(encoding="utf-8")
        self.assertIn("xhs_discover.py", launcher)
        self.assertIn("docker compose up", launcher)
        header = (ROOT / "main" / "xhs_config.h").read_text(encoding="utf-8")
        self.assertIn('#define XHS_WIFI_SSID ""', header)
        self.assertIn('#define XHS_WIFI_PASSWORD ""', header)
        self.assertIn('#define XHS_BACKEND_BASE ""', header)
        self.assertIn('#define XHS_DEVICE_TOKEN ""', header)
        self.assertNotRegex(header, r"http://\d+")

    def test_unauthorized_creator_session_is_expired_and_unsaved(self) -> None:
        from xhs_state import XhsState

        def fetch(cookie: str, when: datetime | None = None):
            del when
            code = 401 if cookie == "denied" else 503
            raise urllib.error.HTTPError(
                "https://creator.xiaohongshu.com/", code, "no", None, None)

        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        state = XhsState(Path(tmp.name), fetch)
        self.assertEqual(state.accept_cookie("denied"), "logged_out")
        self.assertFalse(state.cookie_path.is_file())
        state.cookie_path.write_text("denied\n", encoding="utf-8")
        self.assertEqual(state.login_status(), "expired")
        state.cookie_path.write_text("flaky\n", encoding="utf-8")
        self.assertEqual(state.login_status(), "logged_in")
        status, body = self._request(self.console_url + "/api/status")
        self.assertEqual(status, 200)
        self.assertNotIn(b"denied", body)
        self.assertNotIn(b"flaky", body)

    def test_default_console_bind_is_loopback(self) -> None:
        from xhs_server import API_HOST, CONSOLE_HOST, api_bind_host, console_bind_host

        self.assertEqual(API_HOST, "0.0.0.0")
        self.assertEqual(CONSOLE_HOST, "127.0.0.1")
        previous_api = os.environ.pop("XHS_API_HOST", None)
        previous_console = os.environ.pop("XHS_CONSOLE_HOST", None)
        try:
            self.assertEqual(api_bind_host(), "0.0.0.0")
            self.assertEqual(console_bind_host(), "127.0.0.1")
        finally:
            if previous_api is not None:
                os.environ["XHS_API_HOST"] = previous_api
            if previous_console is not None:
                os.environ["XHS_CONSOLE_HOST"] = previous_console


if __name__ == "__main__":
    unittest.main()
