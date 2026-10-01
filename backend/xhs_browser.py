"""Program-controlled browser session for the creator QR login page.

The real session opens the official page and reads cookies the site sets after
the operator scans the QR code. It does not type a password, solve a challenge,
or forge a signature. Tests supply FakeLoginBrowser and never start Chromium.
"""

from __future__ import annotations

import os
import threading
import time
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


def corner_switch_tip(
    icons: list[tuple[float, float, float, float]],
    title: tuple[float, float, float, float],
) -> tuple[float, float] | None:
    """Tip of the pink corner control above the SMS heading.

    The painted triangle sits in the top-right of its box. The center of that
    box is empty card, so a click there does not switch the form.
    """
    title_x, title_y, _title_w, _title_h = title
    best: tuple[float, float, float, float] | None = None
    best_right = -1.0
    for left, top, width, height in icons:
        if width < 24 or height < 24 or width > 220 or height > 220:
            continue
        if left < title_x:
            continue
        if top + height > title_y + 80:
            continue
        right = left + width
        if right >= best_right:
            best_right = right
            best = (left, top, width, height)
    if best is None:
        return None
    left, top, width, _height = best
    return (left + width - 8, top + 8)


def login_card_switch_point(x: float, y: float, width: float, height: float) -> tuple[float, float]:
    """Click point for the corner control on the SMS login card.

    That control is the top-right of the card. The page has no "扫码" label
    until it is clicked.
    """
    return (x + width - 36, y + 36)


def choose_qr_index(boxes: list[tuple[float, float, bool]]) -> int | None:
    """Index of the login QR, or None when only decorations are present.

    Each box is width, height, and whether its class already names a QR.
    The corner graphic on the creator login card is small. A banner is wide.
    The QR is the largest near-square image of at least 120px, and a named
    QR image wins over any other square.
    """
    named_index = None
    named_area = 0.0
    best_index = None
    best_area = 0.0
    for index, (width, height, named) in enumerate(boxes):
        if width < 120 or height < 120:
            continue
        if height == 0 or not 0.75 <= (width / height) <= 1.35:
            continue
        area = width * height
        if named and area >= named_area:
            named_index = index
            named_area = area
        if area >= best_area:
            best_index = index
            best_area = area
    if named_index is not None:
        return named_index
    return best_index


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
            if not qr:
                raise RuntimeError("qr missing")
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
    """Open the QR side of the login card and return that image.

    While the SMS heading is still visible, the card screenshot is the form
    the operator already saw, so it is not returned.
    """
    clicks = 0
    deadline = time.monotonic() + 12
    while True:
        if not _sms_heading_visible(page):
            shot = _screenshot_qr(page) or _screenshot_login_card(page)
            if shot is not None:
                print("login qr image")
                return shot
        elif clicks < 3:
            clicks += 1
            clicked = _click_corner_icon(page)
            print("login switch %s" % ("clicked" if clicked else "missed"))
        if time.monotonic() >= deadline:
            break
        try:
            page.wait_for_timeout(500)  # type: ignore[attr-defined]
        except Exception:
            break
    print("login qr missing")
    return None


def _screenshot_qr(page: object) -> bytes | None:
    try:
        locator = page.locator("img, canvas")  # type: ignore[attr-defined]
        count = min(int(locator.count()), 40)
    except Exception:
        return None
    boxes: list[tuple[float, float, bool]] = []
    items = []
    for index in range(count):
        item = locator.nth(index)
        try:
            box = item.bounding_box()
            class_name = (item.get_attribute("class") or "").lower()
        except Exception:
            continue
        if not box:
            continue
        named = "qrcode" in class_name or "qr-code" in class_name
        boxes.append((float(box["width"]), float(box["height"]), named))
        items.append(item)
    chosen = choose_qr_index(boxes)
    if chosen is None:
        return None
    try:
        return items[chosen].screenshot(type="png")
    except Exception:
        return None


_LAYOUT_SCRIPT = """() => {
    const titleEl = [...document.querySelectorAll("body *")].find((el) => {
        const own = [...el.childNodes]
            .filter((node) => node.nodeType === 3)
            .map((node) => node.textContent || "")
            .join("")
            .trim();
        return own === "短信登录";
    });
    const title = titleEl ? titleEl.getBoundingClientRect() : null;
    const icons = [...document.querySelectorAll("img, svg")].map((el) => {
        const rect = el.getBoundingClientRect();
        return {x: rect.x, y: rect.y, width: rect.width, height: rect.height};
    });
    return {
        title: title ? {x: title.x, y: title.y, width: title.width, height: title.height} : null,
        icons,
    };
}"""


def _sms_heading_visible(page: object) -> bool:
    try:
        title = page.get_by_text("短信登录", exact=True)  # type: ignore[attr-defined]
        return title.count() > 0 and bool(title.first.is_visible())
    except Exception:
        return False


def _click_corner_icon(page: object) -> bool:
    try:
        layout = page.evaluate(_LAYOUT_SCRIPT)  # type: ignore[attr-defined]
    except Exception:
        layout = None
    point = None
    if isinstance(layout, dict) and isinstance(layout.get("title"), dict):
        title = layout["title"]
        icons = []
        for item in layout.get("icons") or []:
            if not isinstance(item, dict):
                continue
            icons.append((
                float(item.get("x", 0)), float(item.get("y", 0)),
                float(item.get("width", 0)), float(item.get("height", 0)),
            ))
        point = corner_switch_tip(
            icons,
            (float(title["x"]), float(title["y"]), float(title["width"]), float(title["height"])),
        )
    if point is None:
        try:
            image = page.locator("img").first  # type: ignore[attr-defined]
            box = image.bounding_box()
        except Exception:
            box = None
        if box:
            point = (float(box["x"]) + float(box["width"]) - 8, float(box["y"]) + 8)
    if point is None:
        return _click_scan_tab(page)
    try:
        page.mouse.click(point[0], point[1])  # type: ignore[attr-defined]
        return True
    except Exception:
        return False


def _click_scan_tab(page: object) -> bool:
    if _click_text(page, "扫码"):
        return True
    return _click_login_card_corner(page)


def _click_text(page: object, text: str) -> bool:
    try:
        tab = page.get_by_text(text, exact=False)  # type: ignore[attr-defined]
        if tab.count() < 1:
            return False
        tab.first.click(timeout=1000)
        return True
    except Exception:
        return False


_CARD_SCRIPT = """(el) => {
    const limit = window.innerWidth * 0.85;
    let node = el;
    let card = null;
    while (node && node !== document.body) {
        const rect = node.getBoundingClientRect();
        if (rect.width >= 220 && rect.height >= 220 && rect.width < limit) {
            card = node;
            break;
        }
        node = node.parentElement;
    }
    if (!card) {
        card = document.querySelector(".login-box-container");
    }
    if (!card) return null;
    const rect = card.getBoundingClientRect();
    let icon = null;
    let iconX = -1;
    for (const item of card.querySelectorAll("img, svg")) {
        const box = item.getBoundingClientRect();
        if (box.width < 20 || box.width > 120 || box.height < 20 || box.height > 120) continue;
        if (box.x >= iconX) {
            iconX = box.x;
            icon = item;
        }
    }
    if (icon) {
        const box = icon.getBoundingClientRect();
        return {x: box.x + box.width / 2, y: box.y + box.height / 2, clicked: "icon"};
    }
    return {x: rect.x, y: rect.y, width: rect.width, height: rect.height, clicked: "point"};
}"""


def _click_login_card_corner(page: object) -> bool:
    try:
        title = page.get_by_text("短信登录", exact=False)  # type: ignore[attr-defined]
        if title.count() < 1:
            box = page.locator(".login-box-container")  # type: ignore[attr-defined]
            if box.count() < 1:
                return False
            point = box.first.evaluate(_CARD_SCRIPT)
        else:
            point = title.first.evaluate(_CARD_SCRIPT)
        if not isinstance(point, dict):
            return False
        if point.get("clicked") == "icon":
            page.mouse.click(float(point["x"]), float(point["y"]))  # type: ignore[attr-defined]
            return True
        x, y = login_card_switch_point(
            float(point["x"]), float(point["y"]),
            float(point["width"]), float(point["height"]),
        )
        page.mouse.click(x, y)  # type: ignore[attr-defined]
        return True
    except Exception:
        return False


def _screenshot_login_card(page: object) -> bytes | None:
    try:
        box = page.locator(".login-box-container")  # type: ignore[attr-defined]
        if box.count() > 0:
            return box.first.screenshot(type="png")
        title = page.get_by_text("短信登录", exact=False)  # type: ignore[attr-defined]
        if title.count() < 1:
            return None
        card = title.first.locator("xpath=ancestor::*[self::div][position()<=6]").last
        return card.screenshot(type="png")
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
