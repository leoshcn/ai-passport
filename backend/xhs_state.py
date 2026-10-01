"""Cookie file, login status, and the single device-pairing record.

Secrets stay in the data directory. Status responses and logs use only
logged_out, logged_in, and expired. The device token is written for the
confirmed device and is not placed in console status payloads.
"""

from __future__ import annotations

import hashlib
import json
import os
import secrets
import threading
import urllib.error
from pathlib import Path
from typing import Callable

from xhs_fetch import normalize_cookie

PAIR_ALPHABET = "ABCDEFGHJKMNPQRSTUVWXYZ23456789"
LOGIN_STATES = ("logged_out", "logged_in", "expired")


def session_revision(cookie: str) -> str:
    """Short account stamp. Empty when logged out. Not the cookie."""
    text = normalize_cookie(cookie or "")
    if not text:
        return ""
    return hashlib.sha256(text.encode("utf-8")).hexdigest()[:16]


def pair_code_ok(code: str | None) -> bool:
    if not code or len(code) != 4:
        return False
    return all(ch in PAIR_ALPHABET for ch in code)


class XhsState:
    def __init__(self, data_dir: Path, fetch_stats: Callable[..., object]) -> None:
        self.data_dir = Path(data_dir)
        self.data_dir.mkdir(parents=True, exist_ok=True)
        self._fetch = fetch_stats
        self._lock = threading.Lock()
        self._pending: list[str] = []
        self._confirmed: str = ""
        self._token_acked = False
        self._login_generation = 0
        self._load_pairing()

    @property
    def cookie_path(self) -> Path:
        return self.data_dir / "xhs.cookie"

    @property
    def token_path(self) -> Path:
        return self.data_dir / "device.token"

    @property
    def pairing_path(self) -> Path:
        return self.data_dir / "pairing.json"

    def _read_text(self, path: Path) -> str:
        if not path.is_file():
            return ""
        return path.read_text(encoding="utf-8").strip()

    def _write_secret(self, path: Path, text: str) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        tmp = path.with_suffix(path.suffix + ".tmp")
        tmp.write_text(text.strip() + "\n", encoding="utf-8")
        try:
            os.chmod(tmp, 0o600)
        except OSError:
            pass
        os.replace(tmp, path)

    def read_cookie(self) -> str:
        with self._lock:
            return normalize_cookie(self._read_text(self.cookie_path))

    def fetch(self, cookie: str) -> object:
        return self._fetch(cookie)

    def read_token(self) -> str:
        with self._lock:
            return self._read_text(self.token_path)

    def _load_pairing(self) -> None:
        try:
            raw = json.loads(self.pairing_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            return
        if not isinstance(raw, dict):
            return
        pending = raw.get("pending")
        if isinstance(pending, list):
            self._pending = [item for item in pending if pair_code_ok(str(item))]
        confirmed = raw.get("confirmed")
        if pair_code_ok(str(confirmed) if confirmed is not None else ""):
            self._confirmed = str(confirmed)
        self._token_acked = bool(raw.get("token_acked"))

    def _save_pairing(self) -> None:
        payload = {
            "pending": list(self._pending),
            "confirmed": self._confirmed,
            "token_acked": self._token_acked,
        }
        tmp = self.pairing_path.with_suffix(".json.tmp")
        tmp.write_text(json.dumps(payload, ensure_ascii=False), encoding="utf-8")
        os.replace(tmp, self.pairing_path)

    def login_status(self) -> str:
        cookie = self.read_cookie()
        if not cookie:
            return "logged_out"
        try:
            self._fetch(cookie)
        except urllib.error.HTTPError as error:
            if error.fp is not None:
                error.close()
            if error.code in (401, 403):
                return "expired"
            return "logged_in"
        except (ValueError, json.JSONDecodeError):
            return "expired"
        except (OSError, urllib.error.URLError, TimeoutError):
            return "logged_in"
        return "logged_in"

    def clear_login(self) -> None:
        """Remove the creator cookie. Pairing and the device token stay."""
        with self._lock:
            self._login_generation += 1
            if self.cookie_path.is_file():
                self.cookie_path.unlink()

    def accept_cookie(self, raw: str) -> str:
        """Persist only when fetch_stats succeeds. Never echo the cookie."""
        cookie = normalize_cookie(raw or "")
        if not cookie:
            return self.login_status()
        with self._lock:
            generation = self._login_generation
        try:
            self._fetch(cookie)
        except urllib.error.HTTPError as error:
            if error.fp is not None:
                error.close()
            return self.login_status()
        except (OSError, urllib.error.URLError, TimeoutError, ValueError, json.JSONDecodeError):
            return self.login_status()
        with self._lock:
            if generation != self._login_generation:
                return "logged_out"
            self._write_secret(self.cookie_path, cookie)
        return "logged_in"

    def adopt_browser_cookie(self, browser: object) -> str:
        browser.open_login()  # type: ignore[attr-defined]
        try:
            raw = browser.read_cookie()  # type: ignore[attr-defined]
            if not raw:
                return self.login_status()
            return self.accept_cookie(raw)
        finally:
            browser.close()  # type: ignore[attr-defined]

    def status_payload(self) -> dict[str, object]:
        with self._lock:
            pending = list(self._pending)
        return {"login": self.login_status(), "pending": pending}

    def remember_code(self, code: str) -> None:
        if not pair_code_ok(code):
            return
        with self._lock:
            if code == self._confirmed or code in self._pending:
                return
            self._pending.append(code)
            self._save_pairing()

    def confirm(self, code: str) -> bool:
        """Replace the single device token. The token is not returned."""
        if not pair_code_ok(code):
            return False
        with self._lock:
            if code not in self._pending and code != self._confirmed:
                return False
            token = secrets.token_urlsafe(18)
            self._write_secret(self.token_path, token)
            self._confirmed = code
            self._token_acked = False
            self._pending = [item for item in self._pending if item != code]
            self._save_pairing()
        return True

    def pair_delivery(self, code: str) -> dict[str, object]:
        """Device channel. Includes the token only after confirm, until stats uses it."""
        if not pair_code_ok(code):
            return {"ok": False, "status": "pending"}
        with self._lock:
            if code != self._confirmed:
                if code not in self._pending:
                    self._pending.append(code)
                    self._save_pairing()
                return {"ok": True, "status": "pending"}
            body: dict[str, object] = {"ok": True, "status": "confirmed"}
            if not self._token_acked:
                token = self._read_text(self.token_path)
                if token:
                    body["token"] = token
            return body

    def note_token_used(self) -> None:
        with self._lock:
            if not self._token_acked:
                self._token_acked = True
                self._save_pairing()
