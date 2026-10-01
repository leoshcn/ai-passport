"""Program-controlled browser session for the creator QR login page.

The real session opens the official page and reads cookies the site sets after
the operator scans the QR code. It does not type a password, solve a challenge,
or forge a signature. Tests supply FakeLoginBrowser and never start Chromium.
"""

from __future__ import annotations

import os
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


class PlaywrightLoginBrowser:
    """Headless Chromium via Playwright. Imported only when a session starts."""

    def __init__(self) -> None:
        self._playwright = None
        self._browser = None
        self._context = None
        self._page = None
        self._closed = False

    def open_login(self) -> None:
        from playwright.sync_api import sync_playwright

        self._playwright = sync_playwright().start()
        self._browser = self._playwright.chromium.launch(
            headless=True,
            args=chromium_launch_args(),
        )
        self._context = self._browser.new_context()
        self._page = self._context.new_page()
        self._page.goto(CREATOR_LOGIN_URL, wait_until="domcontentloaded", timeout=30000)
        self._closed = False

    def qr_png(self) -> bytes | None:
        if self._page is None or self._closed:
            return None
        try:
            image = self._page.locator("img")
            if image.count() > 0:
                return image.first.screenshot(type="png")
            return self._page.screenshot(type="png")
        except Exception:
            return None

    def read_cookie(self) -> str | None:
        if self._context is None or self._closed:
            return None
        try:
            cookies = self._context.cookies()
        except Exception:
            return None
        if not cookies:
            return None
        parts = []
        for item in cookies:
            name = item.get("name") or ""
            value = item.get("value") or ""
            if not name:
                continue
            parts.append("%s=%s" % (name, value))
        if not parts:
            return None
        return "; ".join(parts)

    def close(self) -> None:
        self._closed = True
        for closer in (self._context, self._browser):
            if closer is None:
                continue
            try:
                closer.close()
            except Exception:
                pass
        self._context = None
        self._browser = None
        self._page = None
        if self._playwright is not None:
            try:
                self._playwright.stop()
            except Exception:
                pass
            self._playwright = None
