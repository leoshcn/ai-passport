"""Read the signed-in creator's own Xiaohongshu stats.

The creator platform accepts the same cookie the user's browser already has.
This module does not type a password, solve a challenge, or forge request signatures.
"""

from __future__ import annotations

import hmac
import json
import urllib.request
from datetime import datetime
from typing import Any

CREATOR_PROFILE = "https://creator.xiaohongshu.com/api/galaxy/creator/home/personal_info"
CREATOR_NOTES = "https://creator.xiaohongshu.com/api/galaxy/creator/data/note_detail_new"
LOSS_KEYS = ("leave_fans_count", "loss_fans_count", "unfollow_count", "fans_lose_count")
AVATAR_EDGE = 48


def normalize_cookie(raw: str) -> str:
    text = raw.strip()
    if text.lower().startswith("cookie:"):
        text = text.split(":", 1)[1].strip()
    return text


def token_matches(presented: str | None, expected: str) -> bool:
    if not presented or not expected:
        return False
    return hmac.compare_digest(presented, expected)


def _as_int(value: Any) -> int:
    if isinstance(value, bool) or value is None:
        raise ValueError("missing number")
    if isinstance(value, int):
        return value
    if isinstance(value, str) and value.strip().lstrip("-").isdigit():
        return int(value.strip())
    raise ValueError("not a number")


def unwrap_data(payload: Any) -> dict[str, Any]:
    if not isinstance(payload, dict):
        raise ValueError("payload is not an object")
    if payload.get("success") is False:
        raise ValueError("creator platform rejected the session")
    data = payload.get("data", payload)
    if isinstance(data, dict) and "fans_count" not in data and isinstance(data.get("data"), dict):
        data = data["data"]
    if not isinstance(data, dict):
        raise ValueError("missing data object")
    return data


def profile_fields(payload: Any) -> tuple[str, int, int, str]:
    data = unwrap_data(payload)
    name = data.get("name") or data.get("nickname") or ""
    avatar = data.get("avatar") or data.get("image") or data.get("userAvatar") or data.get("avatar_url") or ""
    if not isinstance(name, str) or not isinstance(avatar, str):
        raise ValueError("profile fields have the wrong type")
    return name, _as_int(data.get("fans_count")), _as_int(data.get("faved_count")), avatar


def net_followers_7d(payload: Any) -> int:
    data = unwrap_data(payload)
    seven = data.get("seven") if isinstance(data.get("seven"), dict) else data
    if not isinstance(seven, dict) or "rise_fans_count" not in seven:
        raise ValueError("7-day follower count is missing")
    rise = _as_int(seven["rise_fans_count"])
    for key in LOSS_KEYS:
        if seven.get(key) is not None:
            return rise - _as_int(seven[key])
    return rise


def public_stats(name: str, followers: int, likes_collects: int, net_followers: int,
                 fetched_at: str, fetched_unix: int) -> dict[str, Any]:
    return {
        "ok": True,
        "username": name,
        "followers": followers,
        "likes_collects": likes_collects,
        "net_followers_7d": net_followers,
        "fetched_at": fetched_at,
        "fetched_unix": fetched_unix,
    }


def stats_from_payloads(profile: Any, notes: Any, when: datetime) -> dict[str, Any]:
    name, followers, likes, _avatar = profile_fields(profile)
    return public_stats(
        name, followers, likes, net_followers_7d(notes),
        when.strftime("%Y-%m-%d %H:%M"), int(when.timestamp()),
    )


def _creator_get(url: str, cookie: str) -> Any:
    request = urllib.request.Request(
        url,
        headers={
            "User-Agent": (
                "Mozilla/5.0 (Windows NT 10.0; Win64; x64) "
                "AppleWebKit/537.36 (KHTML, like Gecko) "
                "Chrome/131.0.0.0 Safari/537.36"
            ),
            "Accept": "application/json, text/plain, */*",
            "Origin": "https://creator.xiaohongshu.com",
            "Referer": "https://creator.xiaohongshu.com/creator/home",
            "Cookie": cookie,
        },
    )
    with urllib.request.urlopen(request, timeout=20) as response:
        return json.loads(response.read().decode("utf-8"))


def fetch_stats(cookie: str, when: datetime | None = None) -> tuple[dict[str, Any], str]:
    profile = _creator_get(CREATOR_PROFILE, cookie)
    notes = _creator_get(CREATOR_NOTES, cookie)
    moment = when or datetime.now().astimezone()
    _name, _fans, _likes, avatar = profile_fields(profile)
    return stats_from_payloads(profile, notes, moment), avatar


def pack_rgb565(image: Any) -> bytes:
    converted = image.convert("RGB").resize((AVATAR_EDGE, AVATAR_EDGE))
    packed = bytearray()
    for red, green, blue in converted.getdata():
        value = ((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3)
        packed.append(value & 0xFF)
        packed.append((value >> 8) & 0xFF)
    if len(packed) != AVATAR_EDGE * AVATAR_EDGE * 2:
        raise ValueError("avatar size mismatch")
    return bytes(packed)
