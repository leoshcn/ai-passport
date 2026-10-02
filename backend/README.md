<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Xiaohongshu dashboard backend

This service runs on a computer in the same LAN as the AI Passport. The device polls it over plain HTTP. The service reads the creator platform with a cookie from the operator's own signed-in browser session.

It does not submit a password, solve a challenge, or forge platform signatures. Use only a creator account you are allowed to use. Do not commit the cookie, the device token, or a Wi-Fi password.

## Run on Windows

From the repository root:

```text
powershell -File backend/start-xhs.ps1
```

That command starts the host discovery process and `docker compose up`.

- Device API: `0.0.0.0:8787`. The device uses this port after pairing.
- Operator console: `0.0.0.0:8790`. Another computer on the same LAN opens `http://<server-lan-ip>:8790`. Do not publish port 8790 on the public internet. Allow LAN access to TCP `8790` in the server firewall. Discovery still calls `127.0.0.1:8790` on the server.
- Discovery listens for the device UDP broadcast on the physical LAN and unicasts the reply. The reply address is the IPv4 of the interface that matches the device, never a `172.x` address, a WSL adapter, or `host.docker.internal`.

Allow inbound TCP `8787` for `com.docker.backend.exe`, and inbound UDP `8788` for the discovery process, through Windows Firewall. If Docker Desktop is set to publish ports on localhost only, the device cannot reach `8787`; the Compose file pins the device API to `0.0.0.0`.

Open `http://<server-lan-ip>:8790`. On the server itself, `http://127.0.0.1:8790` still works. The page shows one status: logged out, logged in, or expired (`logged_out`, `logged_in`, `expired`). Login opens the official creator QR page in a program-controlled browser. The cookie is stored only after the existing stats fetch succeeds. A pasted cookie uses that same check. The page, logs, and device stats responses do not include the cookie or the device token.

The page button labeled 退出登录 clears the saved creator session and the stats cache so another account can sign in. The device pairing and its token stay. About once a minute the device asks `GET /api/v1/session` for a short revision of the signed-in account, never the cookie. A different revision fetches stats immediately. While the account stays the same, the hourly or daily period still applies. The device keeps the last numbers on screen until that fetch succeeds.

Cookie and pairing state are in the `xhs-data` volume and survive a container restart.

`GET /api/v1/stats` and `GET /api/v1/avatar` are unchanged. Both require header `X-Device-Token`. Stats are cached for 30 seconds. The `fetched_at` text is China Standard Time (UTC+8), because the device shows that string as written and the container clock is UTC. A missing cookie or a rejected session returns HTTP 503 and `{"ok": false}` without logging the cookie.

The 7-day value subtracts an unfollow count when the creator payload includes one. When the payload only includes `rise_fans_count`, that new-follower number is used.

## Device setup

`main/xhs_config.h` is an empty template. Do not put a Wi-Fi password, backend URL, or device token in it. With no saved provisioning record, the device opens an open hotspot named `Passport-Setup` and serves a setup page at `http://192.168.4.1`. The page lists up to 12 nearby 2.4 GHz networks, strongest first. Pick one and enter the password, or expand manual entry and type a name. A filled-in manual name is used instead of the picked one. Open networks can omit the password. Hidden networks and 5 GHz-only names do not appear. The page also asks for an optional backend base URL such as `http://<this-computer-lan-ip>:8787`.

The device writes Wi-Fi to NVS only after the station gets an IP, then it stops the hotspot, DNS, and setup HTTP server. A wrong password is not stored. If an older good record exists, a failed attempt keeps it.

Leave the backend URL empty to broadcast a 4-character pairing code and show it on the device. Fill it in to poll that backend instead of broadcasting. Either way, confirm the code once in the console. The backend then creates one device token and delivers it only to that device. A later confirm replaces the previous token.

In device settings, Re-provision clears Wi-Fi, the backend URL, and the token, keeps the last stats in the `xhsdash` NVS namespace, and opens the hotspot again. An NVS init failure does not erase the partition; the exact on-screen sentence is in the Chinese copy of this document.

## Device buttons

- Short press OK on the dashboard: fetch once now.
- Long press OK: open settings.
- In settings, Up and Down move between auto-update, update period, and re-provision. OK starts editing. Up and Down change the value, and OK writes it back.
- On re-provision, OK asks for a second confirmation. That confirmation clears Wi-Fi, the backend URL, and the token, keeps the last stats, and returns to the hotspot.
- Long press OK in settings: save and return to the current main screen.

Auto-update defaults to on, and the period defaults to once a day. The backlight dims to 8% after about 30 seconds idle. The battery percentage is at the top right; it stays blank when the gauge has no reading.
