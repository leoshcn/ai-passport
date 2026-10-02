<p align="right">
  <a href="xhs-dashboard.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Xiaohongshu creator dashboard

This guide starts from a powered-off device. It covers the computer that serves the numbers, the first Wi-Fi setup, pairing, everyday reading, and the settings screen.

The device shows follower counts for a Xiaohongshu creator account you are allowed to use. The computer keeps that login. The device keeps the home Wi-Fi password and a private token used only to ask the computer for numbers. The web page and the logs do not show the login cookie, the Wi-Fi password, or the token.

## What you need

- The device, with this firmware already installed.
- A phone that can join a Wi-Fi network.
- A computer on the same home network as the device will use. Debian and Windows are both supported.
- Docker, and Python 3 on that computer.
- A Xiaohongshu creator account you can sign in to.

Keep the console on the home network. Do not publish it on the public internet.

The computer has two jobs, and both must stay running:

- A discovery program on the computer itself. It hears the device and reports the pairing code. Docker alone does not hear that broadcast.
- The console and the device API, started with Docker. The console is TCP port `8790`. The device API is TCP port `8787`. Discovery listens on UDP port `8788`.

Allow those three ports from the home network in the computer firewall.

## Turn the device on

The power button is separate from the three function buttons.

- Hold the power button for about half a second to turn the device on.
- Hold it for about two seconds to turn the device off.

The function buttons are up, down, and confirm. A short press is a click. Holding confirm for about half a second is the long press used below.

After about 30 seconds with no button press, the backlight dims. Press any function button to brighten the screen. The percentage in the top-right corner is the battery level. It stays blank when the gauge has no reading.

## Start the computer

On the computer that will serve the device, open the repository root.

Debian:

```bash
python3 backend/xhs_discover.py &
discover_pid=$!
trap 'kill $discover_pid' EXIT
docker compose up --build
```

Windows:

```text
powershell -File backend/start-xhs.ps1
```

Leave that window open. On another computer on the same network, open `http://<server-lan-ip>:8790`. On the server itself, `http://127.0.0.1:8790` also works.

The page shows one status: logged out, logged in, or expired.

## Sign in

1. Click the scan-to-login button.
2. A QR code from the official creator site appears on this page. Confirm it on your phone.
3. The status becomes logged in only after the computer can read the account numbers. The cookie box is not filled in for you.

If the QR code fails, open the spare cookie section, paste the cookie from a browser where you are already signed in, and submit it. The same check applies.

Log out when you want to switch accounts. Pairing and the device token stay. The device notices the new account within about a minute. Until the new numbers arrive, the screen keeps the previous ones.

An expired status means the saved login no longer works. Scan again, or paste a working cookie. You do not need to set up Wi-Fi again.

## Connect the device to home Wi-Fi

On first use, and again after you set up Wi-Fi from the settings screen, the device opens a hotspot named `Passport-Setup`. The screen shows that name and the address `192.168.4.1`. The hotspot has no password.

On the phone:

1. Join `Passport-Setup`. The phone leaves your home Wi-Fi while it is on this hotspot.
2. Open `http://192.168.4.1`. If a setup page does not appear by itself, type that address.
3. Pick the home network from the list and enter its password. An open network can leave the password empty. If the name is not listed, expand Manual entry and type it. A name filled in there is the one the device uses.
4. Leave the backend address empty.
5. Tap Connect.

The device shows that it is connecting. The phone page refreshes on its own.

- When the phone says the home Wi-Fi is connected, leave `Passport-Setup` and rejoin the home Wi-Fi. Read the pairing code on the device. The hotspot closes about 15 seconds later.
- When the phone says it did not connect, follow the link back to the list, check the name and password, and try again. A wrong password is not saved. The device returns to the hotspot screen.

Fill in the backend address only when this computer cannot hear the broadcast. Use `http://<server-lan-ip>:8787`. The device then asks that address directly and does not broadcast. You still confirm the pairing code once, on that computer.

## Confirm the pairing code

The device shows a four-character code.

On the console, under device pairing, a button with the same code appears within a few seconds. The open page updates on its own. Click that button once.

The computer then gives this device its token. Confirming another code later replaces that token, so keep one device on this console.

If the button never appears:

- The discovery program is not running on the console computer. Start it with the commands above, on that computer, outside Docker.
- The phone is still joined to `Passport-Setup`, so the device is not on the home network yet.
- A backend address was filled in, and it does not point at this console. Leave it empty unless you intend the direct-address path.

## Read the dashboard

After pairing, the screen shows:

- The account name and picture. Before the first successful update, the name says it has not been updated yet.
- Followers.
- Likes and collects.
- Net follower change over the last seven days. A gain and a loss use different colors. Before the first success, the three numbers are dashes.
- The time of the last update, in China Standard Time (UTC+8).

On the dashboard, a short press of confirm fetches new numbers immediately.

Automatic update is on by default and runs once a day. The device also checks about once a minute for an account change on the computer. When the account is unchanged, it still follows the hourly or daily schedule.

If a later fetch fails, the previous numbers stay and the bottom line says the update failed.

## Change settings

Hold confirm for about half a second. The settings screen has four rows. Up and down move the highlight. Confirm enters the highlighted row.

| Row | What confirm does |
| --- | --- |
| Auto update | Up and down switch it on or off. Confirm saves the value. The default is on. |
| Update interval | Up and down switch between every hour and every day. Confirm saves the value. The default is every day. |
| Refresh now | One confirm returns to the dashboard and fetches immediately when setup is complete. |
| Set up Wi-Fi again | Confirm once. The row then asks you to confirm. Confirm again. |

Hold confirm while settings are open to save and return to the current main screen.

Setting up Wi-Fi again clears the saved home Wi-Fi, the backend address, and the device token. The last numbers stay on the device. The hotspot `Passport-Setup` opens, and you repeat the phone setup and the pairing confirmation.

## When the failure screen appears

One screen says that configuration failed and the firmware should be refreshed. It has two causes.

- It appears as soon as you power on, before the hotspot screen. The device could not open its storage. Setup will not repair it. Refresh the firmware.
- It appears after pairing, before any numbers have been shown. The first fetch failed. Sign in on the console, or wait if the creator site is briefly unreachable. The device retries about once a minute. A successful fetch returns to the dashboard. Hold confirm if you need the settings screen while this message is up.

## Stop the computer

On Debian, closing the Compose window also stops the discovery program started by the command above. On Windows, closing the start script does the same. The device keeps the last numbers and tries again after you start the computer again.
