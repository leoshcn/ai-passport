# Research: LAN discovery into Docker Desktop on Windows (WSL2)

- **Query**: Can a Python process in a container on Docker Desktop for Windows (WSL2 backend) receive a UDP broadcast from an ESP32 on the physical LAN, and can that device reach TCP 8787 at the Windows host's LAN IP? What compose-friendly binding preserves "device appears in the console, user clicks confirm once" without typing the PC IP?
- **Scope**: mixed (Docker and Microsoft docs, plus community reports where the docs are silent)
- **Date**: 2026-10-01

## Findings

### Recommendation

Receive the pairing UDP broadcast in a small process on the Windows host, not in the container. Publish TCP `8787` from Compose so the device can call the API at the Windows LAN IPv4 after it learns that address.

`docker compose up` alone has no documented way to put a container on the physical LAN broadcast domain. The host listener is what makes the device show up for a single confirm click. A one-line Windows launcher can start that listener and then run Compose; Compose itself cannot start the listener.

Fallback, if there is no Windows listener: during SoftAP setup the user supplies the PC's LAN IPv4. Published TCP `8787` is the documented way for the device to reach the API after that. That fallback drops the "no typed IP" happy path.

### 1. Published UDP ports do not document broadcast delivery

Docker Desktop runs the engine in a Linux VM. On the WSL2 backend, `com.docker.backend.exe` is the network proxy between Windows and that VM.

Publishing a port does this (Docker Docs, "Networking on Docker Desktop", section "How exposed ports work"):

- The backend process listens on the host port.
- When an application connects to that port, the backend forwards the connection into the VM over a shared-memory channel, then to the container IP and port.
- Default bind is all interfaces (`0.0.0.0`). It can be restricted to `127.0.0.1` or one adapter. Docker Desktop can also be set to bind published ports to localhost by default.
- The same page says a published port is reachable from the host or the local network, and that host firewalls can allow or deny inbound traffic by filtering `com.docker.backend.exe`.

"Explore networking how-tos on Docker Desktop" repeats that inbound connections pass through `com.docker.backend.exe`.

That is unicast delivery to the address and port the proxy bound. The docs do not say that a datagram addressed to `255.255.255.255` or to a subnet broadcast is accepted or copied into the container.

Compose can publish UDP (`protocol: udp`, default `tcp`). That only selects the transport for the published mapping. It does not add broadcast membership. Source: Docker Docs, "Define services in Docker Compose", `ports` long syntax.

**Uncertain:** whether today's `com.docker.backend.exe` UDP listener happens to receive a limited broadcast because Windows delivered it to a `0.0.0.0` socket, and whether it then forwards that datagram. Current Docker docs do not say. This research did not run a packet test. Do not build the happy path on it.

Historical vpnkit logs (`com.docker.slirp.exe: Socket.Datagram.input ... ignoring broadcast packet`) are from the old Hyper-V/slirp path and from the VM's own DHCP broadcasts to `255.255.255.255:67` (Docker for Windows issue [docker/for-win#125](https://github.com/docker/for-win/issues/125), 2016). That is the opposite direction and an old component. It is not evidence about inbound LAN broadcasts on current WSL2 Desktop.

Per-container IPs are not a substitute. "Explore networking how-tos" lists "Per-container IP addressing is not possible" for Linux containers because the bridge is inside the VM and is not reachable from the host. "I cannot ping my containers" / "Docker Desktop can't route traffic to Linux containers." The ESP32 must use the Windows LAN IPv4, not a `172.x` container address.

`host.docker.internal` is also the wrong address to hand the device. Docker Docs define it as the name a container uses to reach a service on the host ("resolves to the internal IP address of your host"). It is not a name the ESP32 can resolve, and it is not documented as the LAN address.

### 2. `network_mode: host` is not Linux host networking here

Two official descriptions disagree in wording. The Desktop-specific limits are the ones that apply.

Generic host driver (Docker Docs, "Host network driver"):

- On Docker Engine on Linux, the container shares the host network namespace. `-p` is ignored: `WARNING: Published ports are discarded when using host network mode`.
- Docker Desktop 4.34 and later can enable host networking, but only after the user signs in to a Docker account, opens Settings → Resources → Network, checks "Enable host networking", and applies a restart.
- Desktop's feature "works in both directions" for TCP and UDP. The examples are `nc` to `localhost`, not LAN broadcast.
- Limits written on that page:
  - Processes in the container cannot bind the host's IP addresses, "because the container has no direct access to the interfaces of the host."
  - "The host network feature of Docker Desktop works on layer 4." Protocols below TCP or UDP are not supported.
  - It does not work with Enhanced Container Isolation.
  - Linux containers only.

Compose docs ("Networking in Compose") describe `network_mode: host` as sharing the host stack so the container "can observe all network traffic on the host." "Define services in Docker Compose" says port mapping must not be used with `network_mode: host`. That text matches Linux Engine. It does not override the Desktop limits above. On Desktop, host mode still has no direct access to host interfaces, so it is not a socket on the physical NIC.

A May 2025 Docker Community Forums thread ("Host networking not working on Docker Desktop in WSL2 with mirrored mode") reports that with WSL mirrored mode and Desktop "Enable host networking" turned on, the container still does not get the Windows LAN IP. The reply says Desktop does not use the mirrored interface for the engine namespace, and that the feature mirrors requests between host and container via vpnkit. The reporter's fix was to leave Docker Desktop and run docker-ce inside a WSL distro. Treat this as a community report, not a Docker guarantee. It matches the official "no direct access to the interfaces of the host" limit.

A March 2025 forums reply on UDP broadcast scanning from a container (thread started 2021, "Is it possible to send UDP broadcast inside docker container...") says Desktop host networking "mirrors packages on layer 4" and the author is "not sure if it works with broadcast." If it does not, "there is no way to make it work with Docker Desktop." That uncertainty is unresolved by Docker's own docs.

**Conclusion:** `network_mode: host` in Compose on Docker Desktop Windows is an opt-in L4 proxy. It requires a signed-in settings change, so it is not part of a plain `docker compose up` happy path. It is not documented to receive `255.255.255.255` or subnet broadcasts from the home Wi-Fi. Do not use it for pairing.

### 3. What Compose can honestly publish

Use a normal bridge network and publish TCP:

```yaml
services:
  api:
    ports:
      - "8787:8787"   # TCP is the default; binds 0.0.0.0 unless host_ip is set
```

That matches the documented path: `com.docker.backend.exe` listens on the Windows host, including the LAN address, and forwards connections to the container. The ESP32 then uses `http://<windows-lan-ipv4>:8787`.

Caveats that are documented, not guesses:

- Windows Firewall can block `com.docker.backend.exe`. First use may need an allow rule. Docker Docs, "Networking on Docker Desktop", "Firewalls and endpoint visibility".
- If Docker Desktop is configured to publish on localhost only, LAN clients will not connect. The mapping above must stay on `0.0.0.0` (omit `host_ip`, and do not rely on the Desktop "localhost by default" setting).
- **Undocumented, so do not depend on it:** the TCP peer address seen inside the container. The published path is a userland forward into the VM. Docker does not promise that the container observes the ESP32's LAN address. Put the device id in the application payload.

Product tension with this task's PRD: the console is supposed to be reachable only on the machine, while this question requires TCP 8787 at the LAN IP. Those can be different ports. Keep the human console on `127.0.0.1`. Publish `8787` for the device API. Putting both on one LAN-wide port exposes the console on the home network.

There is no Compose key that joins the physical LAN:

| Approach | On Docker Desktop Windows | Source |
|---|---|---|
| `ports` with `protocol: udp` | Publishes a UDP port on the host proxy. Broadcast delivery is not documented. | Compose services `ports`; Desktop "How exposed ports work" |
| `network_mode: host` | Opt-in L4 proxy, no host-interface access, not a silent compose up | Host network driver, Docker Desktop section |
| `macvlan` | "Not supported on Docker Desktop for Mac or Windows, or Docker Engine on Windows." | Docker Docs, "Macvlan network driver", platform support |
| `ipvlan` with a LAN parent | No Desktop support statement found on the macvlan page's ipvlan note. The parent would be an interface of the Docker VM, which is not the physical NIC. Community replies say ipvlan/macvlan on Desktop stay on that NAT interface. Not a supported pairing design. | Macvlan doc mentions ipvlan only as a Linux alternative; forums threads "Ipvlan DockerDesktop for Windows with WSL2" (2023) |

WSL mirrored networking is a different feature and does not fix Docker Desktop. Microsoft Learn, "Accessing network applications with WSL", "Mirrored mode networking":

- Windows 11 22H2 or later, `networkingMode=mirrored` in `.wslconfig`.
- Documented benefits include multicast and connecting to WSL from the LAN.
- The benefits list does not say UDP broadcast.
- Inbound traffic still needs a Hyper-V firewall change. Microsoft's sample is `Set-NetFirewallHyperVVMSetting ... -DefaultInboundAction Allow` or `New-NetFirewallHyperVRule` for a TCP port. The VM creator id in that doc is `{40E0AC32-46A5-438A-A0B2-2B479E8F2E90}`.
- Default WSL mode is NAT. In NAT, a WSL distro is not on the LAN; Microsoft's LAN workaround is `netsh interface portproxy`, which is TCP port proxy, not UDP broadcast.

The same community thread as above says the engine namespace inside `docker-desktop` does not see the mirrored NIC. Running docker-ce inside a user WSL distro, with mirrored mode and a firewall change, is a different install. It is not "Docker Desktop, one `docker compose up`".

### 4. Smallest reliable design that keeps one confirm click

**Binding:** UDP socket in a Windows process on the same PC, bound on the physical LAN stack. The container only sees a localhost HTTP post.

Flow:

1. ESP32 joins home Wi-Fi and sends a short UDP hello (limited broadcast or subnet broadcast). Payload includes a device id. The Windows socket's receive path yields the sender address; do not ask the container to learn it from Docker.
2. The Windows listener POSTs that hello to the container at `http://127.0.0.1:<published-port>/...`. The console lists the device.
3. The listener unicasts a reply to the hello's source address. The reply carries this PC's IPv4 on the interface that received the broadcast, plus whatever pairing state the console has stored.
4. The user clicks confirm once. The listener's reply (immediate, or the next hello) tells the device the confirm result and `http://<that-lan-ipv4>:8787`.
5. The device uses TCP to that URL. Published port `8787` is the documented reachability path.

Why this address and not another:

- The arrival interface can be Wi-Fi or Ethernet. The PC also has vEthernet (WSL), Docker, and other virtual adapters. The reply IP must be the LAN address of the interface that got the broadcast. `host.docker.internal`, a `172.16`–`172.31` Docker/WSL address, or "the first address Windows lists" can be unreachable from the ESP32.
- The listener is a normal Windows UDP bind, so it is in the broadcast domain the ESP32 is using. The container is not.

**Compose shape:** the API service stays on the default bridge and publishes TCP `8787:8787` on `0.0.0.0`. Do not set `network_mode: host`. Do not expect a published UDP port to be the discovery socket.

**One user command:** ship a Windows launcher that starts the UDP listener and runs `docker compose up`. Compose has no service type that runs a Windows socket beside the Linux VM. Calling that launcher "compose-friendly" is as far as the platform goes. Plain `docker compose up` with only a Linux service will not hear the hello.

**Fallback:** no host listener. While the phone or PC is on the device SoftAP, the setup form includes the PC's LAN IPv4 (and port 8787). After the device joins the home Wi-Fi it opens TCP to that address. No broadcast, no Docker UDP, no host mode. The user types an address once. This is the smallest path that uses only documented Desktop port publishing.

mDNS/SSDP are not a better container-side substitute. They are multicast. Desktop host networking does not document multicast, and Microsoft's multicast note applies to WSL mirrored mode, not to the Docker Desktop engine.

### Code and task context

| File | Why it matters |
|---|---|
| `.trellis/tasks/10-01-xhs-setup/prd.md` | Backend is local Python on `0.0.0.0:8787` today. Device calls `GET /api/v1/stats` and `GET /api/v1/avatar` with `X-Device-Token`. Discovery after Wi-Fi join is still an open question; acceptance for it is not written. Console acceptance says the page is local-only. |
| `backend/xhs_server.py` | Current server process. No Docker network config in this task yet. |
| `main/xhs_net.c` | Still station-only. No broadcast hello exists yet. |

No repository code publishes Docker ports or listens for LAN broadcasts. Nothing in-tree contradicts the Docker docs above.

### External references

- [Networking on Docker Desktop](https://docs.docker.com/desktop/features/networking/) — VM plus `com.docker.backend.exe`; how published ports are forwarded; firewall visibility. Fetched 2026-10-01.
- [Explore networking how-tos on Docker Desktop](https://docs.docker.com/desktop/features/networking/networking-how-tos/) — `host.docker.internal`; publish for host or LAN access; per-container IP not reachable; inbound path is the backend process. Fetched 2026-10-01.
- [Host network driver](https://docs.docker.com/engine/network/drivers/host/) — Desktop 4.34 opt-in, sign-in and settings toggle, L4 only, no direct access to host interfaces. Fetched 2026-10-01.
- [Define services in Docker Compose](https://docs.docker.com/compose/compose-file/05-services/) — `ports` protocol `tcp`/`udp`; do not combine `ports` with `network_mode: host`.
- [Networking in Compose](https://docs.docker.com/compose/how-tos/networking/) — generic `network_mode: host` wording (Linux-style). Do not apply the "observe all network traffic" sentence to Docker Desktop.
- [Macvlan network driver](https://docs.docker.com/engine/network/drivers/macvlan/) — unsupported on Docker Desktop for Windows. Fetched 2026-10-01.
- [Accessing network applications with WSL](https://learn.microsoft.com/en-us/windows/wsl/networking) — NAT vs mirrored mode, multicast and LAN access for WSL, Hyper-V firewall. Fetched 2026-10-01.
- Docker Community Forums, "Host networking not working on Docker Desktop in WSL2 with mirrored mode" (2025) — community report that Desktop host networking does not take the Windows LAN IP. https://forums.docker.com/t/host-networking-not-working-on-docker-desktop-in-wsl2-with-mirrored-mode/147994
- Docker Community Forums, "Is it possible to send UDP broadcast inside docker container to scan devices IP connected to my Windows host network?" — 2025 reply: L4 host networking, broadcast uncertain, otherwise not possible on Desktop. https://forums.docker.com/t/is-it-possible-to-send-udp-broadcast-inside-docker-container-to-scan-devices-ip-connected-to-my-windows-host-network/113376
- [docker/for-win#125](https://github.com/docker/for-win/issues/125) — historical vpnkit "ignoring broadcast packet" on VM DHCP. Not the inbound LAN path.

### Related specs

- `.trellis/tasks/10-01-xhs-setup/prd.md` — deployment and pairing requirements. No `.trellis/spec/` document covers Docker Desktop networking.

## Caveats / Not found

- No current Docker document was found that says "published UDP ports receive 255.255.255.255" or "they do not." Absence is treated as unsupported, not as a tested failure.
- No test was run on this machine (no container, no ESP32 packet capture).
- Docker Desktop host-networking broadcast/multicast behavior is explicitly uncertain in a community reply and unspecified in the official L4 limits.
- Source IP preservation on published TCP connections is not stated in the Desktop networking pages.
- Home Wi-Fi client isolation, a guest SSID, or a VLAN between the ESP32 and the PC will drop broadcasts before Windows sees them. That is outside Docker. The TCP fallback (typed LAN IP) still needs the AP to allow station-to-station or station-to-LAN TCP, which isolation also blocks.
- IPvlan-on-Desktop was not found as a supported LAN attachment. Do not invent a Compose network driver for it.
- Winsock pages for `SO_BROADCAST` were not fetched (HTTP 404 on the URLs tried). The host-listener recommendation does not depend on a specific socket option: it depends on the process being on Windows rather than behind the Desktop proxy.
