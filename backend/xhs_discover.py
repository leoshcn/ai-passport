"""Windows host listener for device pairing broadcasts.

Docker Desktop does not deliver LAN UDP broadcasts into a container. This
process binds on the host, tells the loopback console, and unicasts the reply
to the device. The address in that reply is the LAN IPv4 of the interface that
matches the sender, never a 172.x, WSL, or host.docker.internal address.
"""

from __future__ import annotations

import json
import select
import socket
import time
import urllib.error
import urllib.request
from typing import Iterable

DISCOVER_PORT = 8788
CONSOLE_DISCOVER_URL = "http://127.0.0.1:8790/api/discover"
API_PORT = 8787

_VIRTUAL_NAME_MARKS = ("wsl", "docker", "vethernet", "hyper-v", "loopback", "virtual")


def reply_ip_allowed(ip: str, adapter_name: str = "") -> bool:
    name = adapter_name.lower()
    if any(mark in name for mark in _VIRTUAL_NAME_MARKS):
        return False
    if ip == "host.docker.internal":
        return False
    parts = ip.split(".")
    if len(parts) != 4:
        return False
    try:
        nums = [int(part) for part in parts]
    except ValueError:
        return False
    if any(num < 0 or num > 255 for num in nums):
        return False
    first, second = nums[0], nums[1]
    if first in (0, 127):
        return False
    if first == 169 and second == 254:
        return False
    if first == 172:
        return False
    return True


def prefix_bits(left: str, right: str) -> int:
    try:
        a = [int(part) for part in left.split(".")]
        b = [int(part) for part in right.split(".")]
    except ValueError:
        return 0
    if len(a) != 4 or len(b) != 4:
        return 0
    bits = 0
    for x, y in zip(a, b):
        if x == y:
            bits += 8
            continue
        for shift in range(7, -1, -1):
            if ((x >> shift) & 1) != ((y >> shift) & 1):
                return bits
            bits += 1
        break
    return bits


def choose_reply_ip(received_ip: str | None, route_ip: str | None) -> str | None:
    """Address to send the device: the interface that received the broadcast.

    ``received_ip`` is the bind address of that socket. None means the socket
    was bound to every interface, so ``route_ip`` (the live route toward the
    sender) is the fallback. A received address that must not be published is
    not replaced with a different adapter.
    """
    if received_ip and received_ip != "0.0.0.0":
        return received_ip if reply_ip_allowed(received_ip) else None
    if route_ip and reply_ip_allowed(route_ip):
        return route_ip
    return None


def route_ip_toward(sender: str) -> str | None:
    """Local IPv4 the OS would use to answer this sender. UDP connect sends nothing."""
    if not sender or sender.count(".") != 3 or sender == "255.255.255.255":
        return None
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.connect((sender, 9))
        ip = sock.getsockname()[0]
    except OSError:
        return None
    finally:
        sock.close()
    if not isinstance(ip, str):
        return None
    return ip


def reply_ip_for_sender(sender: str, adapters: Iterable[tuple[str, str]]) -> str | None:
    """Longest-prefix guess. The listener uses the receiving socket instead."""
    best_ip = None
    best_bits = 15
    for name, ip in adapters:
        if not reply_ip_allowed(ip, name):
            continue
        bits = prefix_bits(sender, ip)
        if bits > best_bits:
            best_bits = bits
            best_ip = ip
    return best_ip


def parse_hello(payload: bytes) -> str | None:
    if len(payload) > 256:
        return None
    try:
        data = json.loads(payload.decode("utf-8"))
    except (UnicodeError, json.JSONDecodeError):
        return None
    if not isinstance(data, dict):
        return None
    code = data.get("code")
    if not isinstance(code, str):
        return None
    from xhs_state import pair_code_ok

    if not pair_code_ok(code):
        return None
    return code


def device_reply(console_body: dict, lan_ip: str) -> bytes | None:
    """Build the unicast payload. Returns None when lan_ip must not be sent."""
    if not reply_ip_allowed(lan_ip):
        return None
    status = console_body.get("status")
    if status not in ("pending", "confirmed"):
        return None
    body: dict[str, object] = {"ok": True, "status": status}
    if status == "confirmed":
        token = console_body.get("token")
        if isinstance(token, str) and token:
            body["token"] = token
        body["base"] = "http://%s:%d" % (lan_ip, API_PORT)
    return json.dumps(body, ensure_ascii=False).encode("utf-8")


def post_discover(code: str, url: str = CONSOLE_DISCOVER_URL) -> dict:
    payload = json.dumps({"code": code}).encode("utf-8")
    request = urllib.request.Request(
        url,
        data=payload,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(request, timeout=3) as response:
        data = json.loads(response.read().decode("utf-8"))
    if not isinstance(data, dict):
        raise ValueError("discover response is not an object")
    return data


def list_adapters() -> list[tuple[str, str]]:
    found: list[tuple[str, str]] = []
    if os_name_is_windows():
        found.extend(_windows_adapters())
    if not found:
        ip = _default_route_ipv4()
        if ip:
            found.append(("default", ip))
    return found


def os_name_is_windows() -> bool:
    import os

    return os.name == "nt"


def _default_route_ipv4() -> str | None:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.connect(("8.8.8.8", 80))
        ip = sock.getsockname()[0]
    except OSError:
        return None
    finally:
        sock.close()
    return ip


def _windows_adapters() -> list[tuple[str, str]]:
    import subprocess

    script = (
        "Get-NetIPAddress -AddressFamily IPv4 | "
        "ForEach-Object { $_.InterfaceAlias + '|' + $_.IPAddress }"
    )
    try:
        completed = subprocess.run(
            ["powershell", "-NoProfile", "-Command", script],
            check=False,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
            timeout=15,
        )
    except (OSError, subprocess.TimeoutExpired):
        return []
    rows: list[tuple[str, str]] = []
    for line in completed.stdout.splitlines():
        if "|" not in line:
            continue
        name, ip = line.split("|", 1)
        name = name.strip()
        ip = ip.strip()
        if name and ip:
            rows.append((name, ip))
    return rows


def _bind_udp(ip: str) -> socket.socket | None:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        sock.bind((ip, DISCOVER_PORT))
        sock.setblocking(False)
    except OSError:
        sock.close()
        return None
    return sock


def discovery_uses_wildcard(windows: bool) -> bool:
    """Linux delivers broadcasts only to a socket bound to every address.

    Windows delivers limited and subnet broadcasts to a socket bound to the
    interface address, so those listeners stay per adapter.
    """
    return not windows


def bind_discovery_sockets() -> list[tuple[socket.socket, str | None]]:
    """Bind listeners that can actually receive the device broadcast.

    On Linux the socket is ``0.0.0.0``. The reply address is chosen later from
    the route toward the sender. On Windows, one socket per allowed adapter
    keeps that adapter's address as the reply. A wildcard socket is the
    fallback when no adapter socket could bind.
    """
    if discovery_uses_wildcard(os_name_is_windows()):
        wild = _bind_udp("0.0.0.0")
        return [(wild, None)] if wild is not None else []
    bound: list[tuple[socket.socket, str | None]] = []
    for name, ip in list_adapters():
        if not reply_ip_allowed(ip, name):
            continue
        sock = _bind_udp(ip)
        if sock is not None:
            bound.append((sock, ip))
    if bound:
        return bound
    wild = _bind_udp("0.0.0.0")
    if wild is not None:
        bound.append((wild, None))
    return bound


def _close_listeners(listeners: list[tuple[socket.socket, str | None]]) -> None:
    for sock, _ip in listeners:
        try:
            sock.close()
        except OSError:
            pass


def _answer_hello(sock: socket.socket, payload: bytes, addr: tuple, bound_ip: str | None,
                  console_url: str) -> None:
    code = parse_hello(payload)
    if not code:
        return
    sender = addr[0] if addr else ""
    route_ip = None if bound_ip else route_ip_toward(sender)
    lan_ip = choose_reply_ip(bound_ip, route_ip)
    if not lan_ip:
        print("discovery ignored a sender with no lan interface")
        return
    try:
        console_body = post_discover(code, console_url)
    except (OSError, urllib.error.URLError, ValueError, json.JSONDecodeError):
        print("discovery console request failed")
        return
    reply = device_reply(console_body, lan_ip)
    if reply is None:
        print("discovery skipped a non-lan reply address")
        return
    try:
        sock.sendto(reply, addr)
    except OSError:
        print("discovery reply was not sent")
        return
    status = console_body.get("status")
    print("discovery %s" % (status if status in ("pending", "confirmed") else "pending"))


def _bound_ips(listeners: list[tuple[socket.socket, str | None]]) -> list[str]:
    return sorted(ip for _sock, ip in listeners if ip)


def _allowed_adapter_ips() -> list[str]:
    return sorted(ip for name, ip in list_adapters() if reply_ip_allowed(ip, name))


def serve_forever(console_url: str = CONSOLE_DISCOVER_URL) -> None:
    listeners = bind_discovery_sockets()
    refreshed = time.monotonic()
    print("discovery listening on %d" % DISCOVER_PORT)
    while True:
        now = time.monotonic()
        if not listeners or now - refreshed >= 15:
            refreshed = now
            wanted = _allowed_adapter_ips()
            if wanted != _bound_ips(listeners):
                _close_listeners(listeners)
                listeners = bind_discovery_sockets()
            if not listeners:
                time.sleep(1)
                continue
        socks = [sock for sock, _ip in listeners]
        try:
            readable, _w, _e = select.select(socks, [], [], 2.0)
        except OSError:
            _close_listeners(listeners)
            listeners = []
            continue
        bound = {sock: ip for sock, ip in listeners}
        for sock in readable:
            try:
                payload, addr = sock.recvfrom(512)
            except (TimeoutError, BlockingIOError, OSError):
                continue
            _answer_hello(sock, payload, addr, bound.get(sock), console_url)


def main() -> None:
    serve_forever()


if __name__ == "__main__":
    main()
