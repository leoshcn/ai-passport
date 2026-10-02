# Research: APSTA scan for the SoftAP setup page

- **Query**: On this ESP32-C3 firmware (ESP-IDF 5.5, no PSRAM), how should the SoftAP provisioning page in `main/xhs_net.c` scan nearby 2.4 GHz SSIDs without breaking the captive portal?
- **Scope**: mixed
- **Date**: 2026-10-02

## Decision

Scan once from `net_task`, cache at most 12 SSIDs, and let every HTTP GET render that cache. Do not call `esp_wifi_scan_start` from `portal_get` or `setup_post`.

## Confirmed constraints

| Fact | Anchor |
|---|---|
| Page is one flash string, no external assets. Form posts `ssid`, `password`, `base` to `/setup`. | `main/xhs_net.c` `PORTAL_PAGE` lines 58–68 |
| Wildcard `GET /*` serves that page. Only a `/status` prefix is special. | `portal_get` 362–366; `httpd_uri_match_wildcard` 429; handler 435–438 |
| POST reads `char body[XHS_SETUP_BODY_MAX + 1]` on the httpd task, then a `xhs_setup_form_t`. | `setup_post` 368–414, buffer at 374 |
| httpd: `max_open_sockets = 3`, `lru_purge_enable`, 5 s recv/send timeouts, `stack_size = 8192`. | `http_start` 419–426 |
| AP is already `WIFI_MODE_APSTA`, channel 1, `max_connection = 2`, beacon 100 TU, `WIFI_PS_NONE`. | `provision_start` 484–497 |
| DNS replies to every name with the AP address, so phone probes become HTTP GETs to this server. | `dns_reply` / `dns_task` 234–313 |
| After submit, `net_task` calls `esp_wifi_connect()` and watches the join for 20 s. | `handle_form` 763–787; loop 878–916 waits 200 ms while not `XHS_PROV_READY` |
| `net_task` stack is 12288. | `xhs_net_start` 1024 |
| Form caps: SSID 32, password 64, base URL 12..120 or empty. Body cap 480. Key buffer is 16 bytes. | `main/xhs_logic.h` 86–96; `xhs_http_base_ok` `main/xhs_logic.c` 65–74; parser 552–604 |
| Existing scan is STA-only, non-blocking, cap 5, no SSID dedupe. | `main/demo_wifi.c` 49–60 and 119–128 |
| No PSRAM. Keep HTTP small; do not fetch several assets per page. | `docs/hardware-design/specifications.md` (ESP32-C3, 2.4 GHz 802.11 b/g/n); `docs/reference/phoenixzhc/softap-provisioning-and-resource-budget.md` lines 95–111 |

ESP-IDF 5.5.3 (`esp_wifi_scan_start` / Wi-Fi guide):

- `esp_wifi_scan_start` works in STA and APSTA. It is not the AP-only API. `NULL` config means active scan, `show_hidden=false`, 120 ms max dwell per channel, `home_chan_dwell_time` 30 ms.
- `block=true` blocks the **caller task** until the scan finishes. It does not freeze the chip, but this radio cannot stay on the SoftAP channel while it dwells elsewhere. In APSTA the driver uses a background scan: after each channel it returns to the home channel (channel 1 here) for 30 ms. Default dwell is 120 ms, so for a 13-channel country the radio is off-channel most of about 2 s. Beacons and HTTP only move during those 30 ms gaps and after `WIFI_EVENT_SCAN_DONE`.
- A second `esp_wifi_scan_start` before scan-done aborts the first. Concurrent scans are not supported. If STA is connecting, `esp_wifi_scan_start` fails immediately with `ESP_ERR_WIFI_STATE`.
- `esp_wifi_scan_get_ap_records` returns records **sorted by RSSI descending** and then frees the driver list. Call it once. Call `esp_wifi_scan_get_ap_num` first if the count is needed (`main/demo_wifi.c` already does this order).
- `wifi_ap_record_t` carries `ssid[33]`, `rssi` (`int8_t`, rarely slightly positive), `primary`, `authmode`, `bssid[6]`. The list is per BSSID, not per SSID.
- Hidden SSIDs are omitted when `show_hidden` is false. A 5 GHz-only SSID never appears: this chip is 2.4 GHz only. A dual-band router shows up only as its 2.4 GHz BSSID.

## Scan owner

`net_task` starts the scan and copies the result into the cache. `portal_get` only reads the cache and sends HTML.

Phones hit the wildcard route many times (captive probes, plus `/status` every 2 s). One scan per GET restarts the driver scan, spends ~2 s off-channel per attempt, and holds one of three sockets inside the handler. Probes then time out or get LRU-purged, and the portal never paints.

Concrete rules:

1. After `provision_start` succeeds, `net_task` calls `esp_wifi_scan_start(NULL, false)` once. Same shape as `demo_wifi.c` line 53, not `block=true`.
2. On `WIFI_EVENT_SCAN_DONE`, set a flag and notify `net_task`. `net_task` calls `esp_wifi_scan_get_ap_num`, then `esp_wifi_scan_get_ap_records` once, merges, and drops the raw array.
3. Do not scan while `s_prov.phase == XHS_PROV_JOINING` or `s_join_watch` is set. Connect already scans internally, and `esp_wifi_scan_start` returns `ESP_ERR_WIFI_STATE` for the whole attempt.
4. A rescan control sets one flag. `net_task` coalesces it: ignore it while a scan is in flight. The GET that sets the flag still returns the current cache immediately. Captive paths (`/generate_204` and the rest) must not set that flag.
5. If the cache is empty because the scan has not finished, serve a short “正在扫描” page with a refresh. Do not hold the HTTP handler until scan-done.

## Result cache

Cap **12** names on the page.

Pull a wider window than 12 from the driver before dedupe. The driver list is per BSSID and already strongest-first, so the first 12 records can be twelve copies of one SSID. Read up to **32** records (the strongest 32), then merge.

Merge belongs in `xhs_logic` (that file has no IDF dependency; `tests/test_xhs_logic.c` can call it):

- Input: SSID string + `int8_t rssi`. Output: the same, max 12.
- Skip empty SSID.
- Dedupe with `strcmp`. Keep the higher RSSI. On a tie, keep the one already stored.
- Sort the output by RSSI descending inside the merge, so the test does not depend on IDF order.
- Store only this small cache for the page. Free the temporary `wifi_ap_record_t` array before returning to the net loop. Prefer a short-lived heap block: the full IDF struct is much larger than the three-field stub in `tests/demo_stubs/demo_test_stubs.h`, and 32 of them do not belong on the 8192-byte httpd stack.

## Page delivery

Do not put a multi-kilobyte HTML buffer on the httpd task. `stack_size` is 8192 and `setup_post` already places the body array and `xhs_setup_form_t` there (about 0.7 KB at the current 480 cap).

Use either:

- Flash strings plus `httpd_resp_send_chunk`: static head and `<style>`, then one stack line per SSID (HTML-escape `"`, `&`, `<`), then a static tail. One line is a few hundred bytes, not a whole document.
- Or one heap buffer freed immediately after `httpd_resp_send`.

CSS stays in that same response. A separate `/style.css` spends another of the three sockets while probes are open. No external font or image. The phone renders the UTF-8; this is not an LVGL glyph problem.

## Form contract

Keep `ssid`, `password`, and `base`. Add optional `ssid_manual`.

After decode, if `ssid_manual` is non-empty, it replaces `ssid` before `xhs_setup_form_ok`. If it is empty, the selected `ssid` stands. If both are empty, parsing still fails. The value stored on `xhs_setup_form_t` stays a single `ssid`; `xhs_prov_submit` does not grow a fourth field. `"ssid_manual"` is 11 bytes and fits the existing `key[16]`.

URL-encoded worst case (`application/x-www-form-urlencoded`: anything outside `A-Za-z0-9*-._` becomes `%HH`; space would be `+`, but a valid base cannot contain a space):

| Piece | Decoded max | Encoded max |
|---|---:|---:|
| `ssid=` | 32 | 5 + 96 |
| `&password=` | 64 | 10 + 192 |
| `&base=` | 120 | 6 + 352 |
| `&ssid_manual=` | 32 | 13 + 96 |
| **Total** | | **770** |

Base is 352 rather than 360 because `http://` encodes to `http%3A%2F%2F` (13 bytes, not 21) and `xhs_http_base_ok` rejects anything longer than 120. A Chinese SSID really does hit the 3× case (UTF-8 bytes are percent-encoded).

**480 is not enough.** 770 − 480 = 290 bytes short. Raise `XHS_SETUP_BODY_MAX` to **800**. The POST buffer grows from 481 to 801 bytes on the httpd stack; that still fits. The existing host test rejects `XHS_SETUP_BODY_MAX + 1` and should keep doing so. Do not raise it far enough to also hold the HTML page.

## What not to do

- Do not call `esp_wifi_scan_start` from the wildcard GET, including `/status` refreshes and captive probes.
- Do not block the HTTP handler on `esp_wifi_scan_start(..., true)` for the whole scan.
- Do not enable `show_hidden` expecting a usable name. Hidden networks stay on the manual field. Typing a 5 GHz-only name still cannot associate.
- Do not load external fonts, images, or a second stylesheet request.
- Do not log the POST body, password, or echo the password back in HTML. `setup_post` already zeroes `body` and the stack form after the critical-section copy; keep that.

## External references

- [ESP-IDF 5.5.3 `esp_wifi_scan_start`](https://docs.espressif.com/projects/esp-idf/en/v5.5.3/esp32/api-reference/network/esp_wifi.html) — NULL-config defaults, `home_chan_dwell_time` 30 ms, `ESP_ERR_WIFI_STATE` while connecting, `esp_wifi_scan_get_ap_records` frees the list and returns RSSI-descending order.
- [ESP-IDF 5.5 Wi-Fi scan guide](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32/api-guides/wifi.html) — scan only in STA or APSTA; APSTA scan is a background scan that returns to the home channel between channels; `show_hidden=0` drops hidden SSIDs; overlapping scans are not supported.

## Caveats

- The ~2 s figure is 13 × (120 + 30) ms from the documented defaults, not a trace from this board. Country channel count changes it.
- `sizeof(wifi_ap_record_t)` was not measured in this tree. The demo stub struct is smaller than the IDF 5.5.3 struct.
- IDF does not dedupe SSIDs. Truncating to 12 before the merge drops unique names.
