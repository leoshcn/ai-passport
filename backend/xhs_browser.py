"""Program-controlled browser session for the creator QR login page.

The real session opens the official page and reads cookies the site sets after
the operator scans the QR code. It does not type a password, solve a challenge,
or forge a signature. Tests supply FakeLoginBrowser and never start Chromium.
"""

from __future__ import annotations

import os
import threading
from typing import Protocol

CREATOR_LOGIN_URL = "https://creator.xiaohongshu.com/login"


class LoginBrowser(Protocol):
    def open_login(self) -> None:
        """Open the official creator login page."""

    def qr_png(self) -> bytes | None:
        """PNG of the login QR, or None when it is not available yet."""

    def read_cookie(self) -> str | None:
        """Cookie header if the page has one, otherwise None. Do not log it."""

    def close(self) -> None:
        """Release the browser. Safe to call more than once."""


class FakeLoginBrowser:
    """In-memory stand-in. Tests decide the cookie; no browser process starts."""

    def __init__(self, cookie: str | None = None, qr: bytes | None = None) -> None:
        self._cookie = cookie
        self._qr = qr if qr is not None else b"\x89PNG\r\n\x1a\nfake-qr"
        self.opened = False
        self.closed = False

    def open_login(self) -> None:
        self.opened = True
        self.closed = False

    def qr_png(self) -> bytes | None:
        if not self.opened or self.closed:
            return None
        return self._qr

    def read_cookie(self) -> str | None:
        if not self.opened or self.closed:
            return None
        return self._cookie

    def close(self) -> None:
        self.closed = True


def chromium_launch_args() -> list[str]:
    """Chromium refuses to start as root inside Docker unless the sandbox is off."""
    euid = getattr(os, "geteuid", lambda: -1)()
    if euid == 0:
        return ["--no-sandbox", "--disable-dev-shm-usage"]
    return []


def chromium_launch_kwargs() -> dict[str, object]:
    """Use the full Chromium build already in the image.

    Playwright 1.49 runs headless through a separate shell binary. This image
    only contains ``chrome-linux/chrome``. ``channel="chromium"`` selects it.
    """
    return {
        "headless": True,
        "channel": "chromium",
        "args": chromium_launch_args(),
    }


def format_cookie_header(cookies: list) -> str | None:
    parts = []
    for item in cookies:
        if not isinstance(item, dict):
            continue
        name = item.get("name") or ""
        value = item.get("value") or ""
        if not name:
            continue
        parts.append("%s=%s" % (name, value))
    if not parts:
        return None
    return "; ".join(parts)


class PlaywrightLoginBrowser:
    """Headless Chromium via Playwright. Imported only when a session starts.

    The sync API is tied to the thread that starts it. This object keeps that
    thread for the whole session and only shares the QR bytes and cookie text.
    """

    def __init__(self) -> None:
        self._ready = threading.Event()
        self._stop = threading.Event()
        self._lock = threading.Lock()
        self._thread: threading.Thread | None = None
        self._qr: bytes | None = None
        self._cookie: str | None = None
        self._error: BaseException | None = None

    def open_login(self) -> None:
        self._thread = threading.Thread(target=self._run, name="xhs-browser", daemon=True)
        self._thread.start()
        if not self._ready.wait(45):
            self.close()
            raise TimeoutError("login browser did not start")
        if self._error is not None:
            error = self._error
            self.close()
            raise error

    def qr_png(self) -> bytes | None:
        with self._lock:
            return self._qr

    def read_cookie(self) -> str | None:
        with self._lock:
            return self._cookie

    def close(self) -> None:
        self._stop.set()
        thread = self._thread
        if thread is not None and thread is not threading.current_thread():
            thread.join(timeout=5)

    def _run(self) -> None:
        playwright = None
        browser = None
        context = None
        try:
            from playwright.sync_api import sync_playwright

            playwright = sync_playwright().start()
            browser = playwright.chromium.launch(**chromium_launch_kwargs())
            context = browser.new_context()
            page = context.new_page()
            page.goto(CREATOR_LOGIN_URL, wait_until="domcontentloaded", timeout=30000)
            qr = _capture_qr(page)
            with self._lock:
                self._qr = qr
        except Exception as exc:
            self._error = exc
            self._ready.set()
            _close_playwright(context, browser, playwright)
            return
        self._ready.set()
        while not self._stop.wait(1):
            header = format_cookie_header(_safe_cookies(context))
            with self._lock:
                self._cookie = header
        _close_playwright(context, browser, playwright)


def _safe_cookies(context: object) -> list:
    try:
        cookies = context.cookies()  # type: ignore[attr-defined]
    except Exception:
        return []
    return cookies if isinstance(cookies, list) else []


def _capture_qr(page: object) -> bytes | None:
    try:
        image = page.locator("img")  # type: ignore[attr-defined]
        if image.count() > 0:
            return image.first.screenshot(type="png")
        return page.screenshot(type="png")  # type: ignore[attr-defined]
    except Exception:
        return None


def _close_playwright(context: object, browser: object, playwright: object) -> None:
    for closer in (context, browser):
        if closer is None:
            continue
        try:
            closer.close()  # type: ignore[attr-defined]
        except Exception:
            pass
    if playwright is not None:
        try:
            playwright.stop()  # type: ignore[attr-defined]
        except Exception:
            pass
