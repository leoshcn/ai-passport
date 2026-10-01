"""Device API and the loopback operator console.

The device API listens on 0.0.0.0:8787. The console listens on 127.0.0.1:8790
unless Docker publishes that port only on the host loopback and sets
XHS_CONSOLE_HOST=0.0.0.0 inside the container network namespace.

Responses other than the device pairing delivery do not contain the cookie or
the device token. Logs record status names and HTTP results, not secrets.
"""

from __future__ import annotations

import hashlib
import json
import os
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Callable

from xhs_browser import LoginBrowser, PlaywrightLoginBrowser
from xhs_fetch import AVATAR_EDGE, fetch_stats, pack_rgb565, token_matches
from xhs_state import XhsState, pair_code_ok, session_revision

ROOT = Path(__file__).resolve().parent
API_HOST = "0.0.0.0"
API_PORT = 8787
CONSOLE_HOST = "127.0.0.1"
CONSOLE_PORT = 8790
CACHE_SEC = 30
PASTE_LIMIT = 8192
DISCOVER_LIMIT = 256

_lock = threading.Lock()
_cache: dict[str, object] = {"at": 0.0, "digest": "", "stats": None, "avatar": None}

LOGIN_LABELS = {
    "logged_out": "未登录",
    "logged_in": "已登录",
    "expired": "已失效",
}

_CONSOLE_PAGE = """<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>创作者控制台</title>
<style>
:root {
  --desk: #d5e0ea;
  --card: #f7fafc;
  --ink: #1b2836;
  --muted: #5d6d7e;
  --line: #d3dee8;
  --seal: #d61f3c;
  --seal-ink: #fff7f4;
}
* { box-sizing: border-box; }
body {
  margin: 0;
  min-height: 100vh;
  color: var(--ink);
  background:
    radial-gradient(28rem 16rem at 110% -10%, rgba(214, 31, 60, 0.18), transparent 70%),
    var(--desk);
  font-family: "Avenir Next", "Segoe UI", "PingFang SC", "Hiragino Sans GB", "Noto Sans SC", "Microsoft YaHei", sans-serif;
  font-size: 1rem;
  line-height: 1.5;
}
main {
  width: min(28rem, calc(100% - 1.5rem));
  margin: 2.25rem auto;
  padding: 1.6rem 1.4rem 1.3rem;
  background: var(--card);
  border-radius: 1.2rem;
  box-shadow: 0 16px 40px rgba(27, 40, 54, 0.08);
}
.eyebrow {
  margin: 0;
  color: var(--muted);
  font-size: 0.78rem;
  letter-spacing: 0.14em;
}
.top {
  display: flex;
  align-items: flex-start;
  justify-content: space-between;
  gap: 1rem;
  margin: 0.35rem 0 1.1rem;
}
h1 {
  margin: 0;
  font-family: "Iowan Old Style", Palatino, "Palatino Linotype", "Songti SC", "STSong", "SimSun", serif;
  font-size: 1.85rem;
  font-weight: 650;
  letter-spacing: 0.02em;
  line-height: 1.15;
}
.lede { margin: 0.45rem 0 0; color: var(--muted); font-size: 0.92rem; }
.seal {
  flex: none;
  width: auto;
  margin: 0.15rem 0.2rem 0 0;
  padding: 0.48rem 0.42rem 0.38rem;
  border: 3px solid var(--seal);
  color: var(--seal);
  background: transparent;
  font-family: "Songti SC", "STSong", "SimSun", "Noto Serif SC", serif;
  font-size: 1.2rem;
  font-weight: 700;
  line-height: 1.12;
  letter-spacing: 0;
  text-align: center;
  word-break: break-all;
  transform: rotate(-8deg);
  animation: stamp 420ms ease-out both;
}
body[data-login="logged_in"] .seal {
  background: var(--seal);
  color: var(--seal-ink);
}
body[data-login="expired"] .seal { border-style: dashed; }
@keyframes stamp {
  from { transform: rotate(-16deg) scale(1.06); opacity: 0; }
  to { transform: rotate(-8deg) scale(1); opacity: 1; }
}
#notice:empty { display: none; }
#notice {
  margin: 0 0 0.9rem;
  color: var(--seal);
  font-size: 0.92rem;
}
.actions, .pair { display: flex; flex-wrap: wrap; gap: 0.6rem; }
form { margin: 0; }
button, summary {
  font: inherit;
  cursor: pointer;
}
button {
  border: 0;
  border-radius: 999px;
  padding: 0.62rem 1.05rem;
  background: var(--seal);
  color: var(--seal-ink);
}
button.quiet {
  background: transparent;
  color: var(--ink);
  box-shadow: inset 0 0 0 1px var(--line);
}
button:focus-visible, summary:focus-visible, textarea:focus-visible {
  outline: 2px solid var(--ink);
  outline-offset: 3px;
}
.qr {
  margin: 1rem 0 0;
  padding: 0.75rem;
  background: white;
  border-radius: 0.9rem;
  box-shadow: inset 0 0 0 1px var(--line);
}
.qr img { display: block; width: min(100%, 18rem); height: auto; margin: 0 auto; }
.hint, .empty { margin: 0.7rem 0 0; color: var(--muted); font-size: 0.9rem; }
details { margin-top: 1.15rem; }
summary { color: var(--ink); }
textarea {
  display: block;
  width: 100%;
  margin: 0.7rem 0;
  padding: 0.7rem 0.75rem;
  border: 1px solid var(--line);
  border-radius: 0.7rem;
  resize: vertical;
  font: inherit;
  color: var(--ink);
  background: white;
}
.pairs { margin-top: 1.25rem; padding-top: 1rem; box-shadow: inset 0 1px 0 var(--line); }
.pairs h2 {
  margin: 0 0 0.55rem;
  font-size: 0.78rem;
  font-weight: 650;
  letter-spacing: 0.12em;
  color: var(--muted);
}
.pair button { font-variant-numeric: tabular-nums; letter-spacing: 0.08em; }
@media (prefers-reduced-motion: reduce) {
  .seal { animation: none; }
}
</style>
</head>
<body data-login="{{LOGIN_KEY}}">
<main>
<p class="eyebrow">小红书 · 仅本机</p>
<div class="top">
  <div>
    <h1>创作者控制台</h1>
    <p class="lede">扫码登录创作者账号。退出后可以换号，设备大约一分钟内会跟上。</p>
  </div>
  <p id="state" class="seal" role="status">{{STATE}}</p>
</div>
<p id="notice">{{NOTICE}}</p>
<div class="actions">
  <form method="post" action="/api/login/start"><button type="submit">扫码登录</button></form>
  {{LOGOUT}}
</div>
{{QR}}
<details>
  <summary>Cookie 备用</summary>
  <form method="post" action="/api/login/paste">
    <textarea name="cookie" rows="4" cols="48" placeholder="扫码失败时，把浏览器里的 Cookie 粘贴到这里"></textarea>
    <button class="quiet" type="submit">提交 Cookie</button>
  </form>
</details>
<section class="pairs">
  <h2>设备配对</h2>
  {{PAIRS}}
</section>
</main>
<script>
setInterval(function () {
  fetch("/api/status").then(function (response) { return response.json(); }).then(function (data) {
    if (!data) return;
    var label = data.login === "logged_in" ? "已登录" : (data.login === "expired" ? "已失效" : "未登录");
    document.body.setAttribute("data-login", data.login || "logged_out");
    var title = document.getElementById("state");
    if (title) {
      title.textContent = "";
      for (var i = 0; i < label.length; i++) {
        if (i) title.appendChild(document.createElement("br"));
        title.appendChild(document.createTextNode(label.charAt(i)));
      }
    }
    var note = document.getElementById("notice");
    if (note) note.textContent = data.notice || "";
    var logout = document.getElementById("logout");
    if (logout) logout.style.display = data.login === "logged_out" ? "none" : "";
    if (data.login === "logged_in") {
      var block = document.getElementById("qr-block");
      if (block && block.parentNode) block.parentNode.removeChild(block);
    }
  }).catch(function () {});
}, 3000);
</script>
</body>
</html>
"""


def data_dir_from_env() -> Path:
    raw = os.environ.get("XHS_DATA_DIR", "").strip()
    if raw:
        return Path(raw)
    return ROOT


def api_bind_host() -> str:
    return os.environ.get("XHS_API_HOST", API_HOST).strip() or API_HOST


def console_bind_host() -> str:
    return os.environ.get("XHS_CONSOLE_HOST", CONSOLE_HOST).strip() or CONSOLE_HOST


def _download_avatar(url: str, cookie: str) -> bytes | None:
    if not url.startswith("http://") and not url.startswith("https://"):
        return None
    try:
        from PIL import Image
        import io
    except ImportError:
        return None
    request = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0", "Cookie": cookie})
    try:
        with urllib.request.urlopen(request, timeout=15) as response:
            blob = response.read(2_000_000)
    except (OSError, urllib.error.URLError):
        return None
    try:
        return pack_rgb565(Image.open(io.BytesIO(blob)))
    except (OSError, ValueError):
        return None


def load_snapshot(state: XhsState, force: bool = False) -> tuple[dict | None, bytes | None, str | None]:
    cookie = state.read_cookie()
    if not cookie:
        return None, None, "missing creator cookie"
    digest = hashlib.sha256(cookie.encode("utf-8")).hexdigest()
    now = time.monotonic()
    with _lock:
        cached = _cache["stats"]
        if (not force and cached is not None and _cache["digest"] == digest
                and now - float(_cache["at"]) < CACHE_SEC):
            return cached, _cache["avatar"], None  # type: ignore[return-value]
    try:
        fetched = state.fetch(cookie)
    except (OSError, ValueError, json.JSONDecodeError, urllib.error.URLError, TimeoutError):
        return None, None, "creator platform request failed"
    if not isinstance(fetched, tuple) or len(fetched) != 2 or not isinstance(fetched[0], dict):
        return None, None, "creator platform request failed"
    stats, avatar_url = fetched
    avatar = _download_avatar(avatar_url, cookie) if avatar_url else None
    with _lock:
        _cache["at"] = time.monotonic()
        _cache["digest"] = digest
        _cache["stats"] = stats
        _cache["avatar"] = avatar
    return stats, avatar, None


def clear_snapshot_cache() -> None:
    with _lock:
        _cache["at"] = 0.0
        _cache["digest"] = ""
        _cache["stats"] = None
        _cache["avatar"] = None


def _html_escape(text: str) -> str:
    return (text.replace("&", "&amp;").replace("<", "&lt;")
            .replace(">", "&gt;").replace('"', "&quot;"))


def _parse_form(body: bytes) -> dict[str, str]:
    text = body.decode("utf-8", errors="replace")
    parsed = urllib.parse.parse_qs(text, keep_blank_values=True, strict_parsing=False)
    return {key: values[-1] if values else "" for key, values in parsed.items()}


class Service:
    def __init__(self, state: XhsState,
                 browser_factory: Callable[[], LoginBrowser] | None = None,
                 poll_seconds: float = 1.0) -> None:
        self.state = state
        self._browser_factory = browser_factory or PlaywrightLoginBrowser
        self._poll_seconds = poll_seconds
        self._browser: LoginBrowser | None = None
        self._qr: bytes | None = None
        self._login_error = ""
        self._watch: threading.Thread | None = None
        self._watch_stop = threading.Event()
        self._browser_lock = threading.Lock()

    def start_login(self) -> None:
        self.stop_login()
        browser = None
        try:
            browser = self._browser_factory()
            browser.open_login()
        except Exception as exc:
            print("login browser failed: %s" % type(exc).__name__)
            if browser is not None:
                try:
                    browser.close()
                except Exception:
                    pass
            self._login_error = "登录页没有打开（%s）" % type(exc).__name__
            return
        with self._browser_lock:
            self._browser = browser
            self._qr = browser.qr_png()
            self._login_error = ""
        self._watch_stop = threading.Event()
        self._watch = threading.Thread(target=self._watch_login, name="xhs-login", daemon=True)
        self._watch.start()

    def stop_login(self) -> None:
        self._watch_stop.set()
        watch = self._watch
        if watch is not None and watch is not threading.current_thread():
            watch.join(timeout=25)
        with self._browser_lock:
            browser = self._browser
            self._browser = None
            self._qr = None
        if browser is not None:
            browser.close()

    def logout(self) -> None:
        """Drop the creator session so another account can sign in."""
        self._watch_stop.set()
        self.state.clear_login()
        clear_snapshot_cache()
        self.stop_login()
        with self._browser_lock:
            self._login_error = ""
        print("login logged_out")

    def qr_png(self) -> bytes | None:
        with self._browser_lock:
            return self._qr

    def _watch_login(self) -> None:
        while not self._watch_stop.wait(self._poll_seconds):
            with self._browser_lock:
                browser = self._browser
            if browser is None:
                return
            try:
                raw = browser.read_cookie()
            except Exception:
                raw = None
            if not raw:
                continue
            status = self.state.accept_cookie(raw)
            if self._watch_stop.is_set():
                return
            print("login %s" % status)
            if status != "logged_in":
                self._login_error = "手机已确认，账号数据还没读到"
                continue
            self._login_error = ""
            browser.close()
            with self._browser_lock:
                if self._browser is browser:
                    self._browser = None
            return

    def console_page(self) -> bytes:
        payload = self.state.status_payload()
        login = str(payload["login"])
        if login not in LOGIN_LABELS:
            login = "logged_out"
        pending = payload["pending"]
        rows = []
        if isinstance(pending, list):
            for code in pending:
                safe = _html_escape(str(code))
                rows.append(
                    "<form class=\"pair\" method=\"post\" action=\"/api/pair/confirm\">"
                    "<input type=\"hidden\" name=\"code\" value=\"%s\">"
                    "<button type=\"submit\">确认 %s</button></form>" % (safe, safe)
                )
        if self.qr_png():
            qr = (
                "<div id=\"qr-block\" class=\"qr\">"
                "<img alt=\"qr\" src=\"/api/login/qr\">"
                "<p class=\"hint\">在手机上点同意后，红印会变成已登录。Cookie 框不会自动填写。</p>"
                "</div>"
            )
        else:
            qr = ""
        hidden = "" if login != "logged_out" else " style=\"display:none\""
        logout = (
            "<form id=\"logout\" method=\"post\" action=\"/api/login/logout\"%s>"
            "<button class=\"quiet\" type=\"submit\">退出登录</button></form>" % hidden
        )
        pairs = "".join(rows) if rows else "<p class=\"empty\">设备亮出配对码之后，确认按钮会出现在这里。</p>"
        page = (_CONSOLE_PAGE
                .replace("{{LOGIN_KEY}}", login)
                .replace("{{STATE}}", "<br>".join(LOGIN_LABELS[login]))
                .replace("{{NOTICE}}", _html_escape(self._login_error))
                .replace("{{LOGOUT}}", logout)
                .replace("{{QR}}", qr)
                .replace("{{PAIRS}}", pairs))
        return page.encode("utf-8")


class Handler(BaseHTTPRequestHandler):
    server_version = "xhs-passport"
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt: str, *args: object) -> None:
        print("%s %s" % (self.address_string(), fmt % args))

    def _service(self) -> Service:
        return self.server.service  # type: ignore[attr-defined]

    def _role(self) -> str:
        return self.server.role  # type: ignore[attr-defined]

    def _json(self, status: int, payload: dict) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def _bytes(self, status: int, body: bytes, content_type: str) -> None:
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def _redirect(self) -> None:
        self.send_response(303)
        self.send_header("Location", "/")
        self.send_header("Content-Length", "0")
        self.send_header("Connection", "close")
        self.end_headers()

    def _read_limited(self, limit: int) -> bytes | None:
        raw_len = self.headers.get("Content-Length", "0")
        try:
            length = int(raw_len)
        except ValueError:
            self._json(400, {"ok": False})
            return None
        if length < 0 or length > limit:
            self._json(413, {"ok": False})
            return None
        if length == 0:
            return b""
        return self.rfile.read(length)

    def _authorized(self) -> bool:
        return token_matches(self.headers.get("X-Device-Token"), self._service().state.read_token())

    def do_GET(self) -> None:  # noqa: N802
        path = urllib.parse.urlsplit(self.path)
        route = path.path
        if self._role() == "api":
            self._api_get(route, urllib.parse.parse_qs(path.query))
            return
        self._console_get(route)

    def do_POST(self) -> None:  # noqa: N802
        route = urllib.parse.urlsplit(self.path).path
        if self._role() == "api":
            self._json(404, {"ok": False})
            return
        self._console_post(route)

    def _api_get(self, route: str, query: dict[str, list[str]]) -> None:
        service = self._service()
        if route == "/api/v1/pair":
            code = (query.get("code") or [""])[0]
            if not pair_code_ok(code):
                self._json(400, {"ok": False})
                return
            self._json(200, service.state.pair_delivery(code))
            return
        if not self._authorized():
            self._json(401, {"ok": False})
            return
        service.state.note_token_used()
        if route == "/api/v1/session":
            self._json(200, {"revision": session_revision(service.state.read_cookie())})
            return
        if route == "/api/v1/stats":
            stats, _avatar, error = load_snapshot(service.state)
            if error or stats is None:
                self._json(503, {"ok": False})
                return
            self._json(200, stats)
            return
        if route == "/api/v1/avatar":
            _stats, avatar, error = load_snapshot(service.state)
            if error or avatar is None or len(avatar) != AVATAR_EDGE * AVATAR_EDGE * 2:
                self.send_response(404)
                self.send_header("Content-Length", "0")
                self.send_header("Connection", "close")
                self.end_headers()
                return
            self._bytes(200, avatar, "application/octet-stream")
            return
        self._json(404, {"ok": False})

    def _console_get(self, route: str) -> None:
        service = self._service()
        if route == "/":
            page = service.console_page()
            self._bytes(200, page, "text/html; charset=utf-8")
            return
        if route == "/api/status":
            payload = service.state.status_payload()
            with service._browser_lock:
                payload["notice"] = service._login_error
            self._json(200, payload)
            return
        if route == "/api/login/qr":
            png = service.qr_png()
            if not png:
                self._json(404, {"ok": False})
                return
            self._bytes(200, png, "image/png")
            return
        self._json(404, {"ok": False})

    def _console_post(self, route: str) -> None:
        service = self._service()
        if route == "/api/login/logout":
            body = self._read_limited(64)
            if body is None:
                return
            service.logout()
            self._redirect()
            return
        if route == "/api/login/start":
            body = self._read_limited(64)
            if body is None:
                return
            service.start_login()
            self._redirect()
            return
        if route == "/api/login/paste":
            body = self._read_limited(PASTE_LIMIT)
            if body is None:
                return
            form = _parse_form(body)
            status = service.state.accept_cookie(form.get("cookie", ""))
            print("login %s" % status)
            self._redirect()
            return
        if route == "/api/pair/confirm":
            body = self._read_limited(256)
            if body is None:
                return
            code = ""
            content_type = self.headers.get("Content-Type", "")
            if "application/json" in content_type:
                try:
                    payload = json.loads(body.decode("utf-8") or "{}")
                except (UnicodeError, json.JSONDecodeError):
                    self._json(400, {"ok": False})
                    return
                if isinstance(payload, dict) and isinstance(payload.get("code"), str):
                    code = payload["code"]
            else:
                code = _parse_form(body).get("code", "")
            ok = service.state.confirm(code)
            print("pair %s" % ("confirmed" if ok else "rejected"))
            if "application/json" in content_type:
                self._json(200 if ok else 404, {"ok": ok})
                return
            self._redirect()
            return
        if route == "/api/discover":
            body = self._read_limited(DISCOVER_LIMIT)
            if body is None:
                return
            try:
                payload = json.loads(body.decode("utf-8") or "{}")
            except (UnicodeError, json.JSONDecodeError):
                self._json(400, {"ok": False})
                return
            code = payload.get("code") if isinstance(payload, dict) else ""
            if not isinstance(code, str) or not pair_code_ok(code):
                self._json(400, {"ok": False})
                return
            delivery = service.state.pair_delivery(code)
            print("discover %s" % delivery.get("status"))
            self._json(200, delivery)
            return
        self._json(404, {"ok": False})


class RoleServer(ThreadingHTTPServer):
    def __init__(self, address: tuple[str, int], service: Service, role: str) -> None:
        self.service = service
        self.role = role
        super().__init__(address, Handler)


def serve(service: Service, api_host: str, api_port: int,
          console_host: str, console_port: int) -> tuple[RoleServer, RoleServer]:
    api = RoleServer((api_host, api_port), service, "api")
    console = RoleServer((console_host, console_port), service, "console")
    threading.Thread(target=api.serve_forever, name="xhs-api", daemon=True).start()
    threading.Thread(target=console.serve_forever, name="xhs-console", daemon=True).start()
    return api, console


def main() -> None:
    state = XhsState(data_dir_from_env(), fetch_stats)
    service = Service(state, PlaywrightLoginBrowser)
    api_host = api_bind_host()
    api_port = int(os.environ.get("XHS_API_PORT", str(API_PORT)))
    console_host = console_bind_host()
    console_port = int(os.environ.get("XHS_CONSOLE_PORT", str(CONSOLE_PORT)))
    serve(service, api_host, api_port, console_host, console_port)
    print("api on %s:%d" % (api_host, api_port))
    print("console on %s:%d" % (console_host, console_port))
    threading.Event().wait()


if __name__ == "__main__":
    main()
