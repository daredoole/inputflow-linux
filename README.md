# InputFlow

![Status: Public Beta](https://img.shields.io/badge/status-public%20beta-e0a100)
![License: GPLv3](https://img.shields.io/badge/license-GPLv3-blue)

> **⚠️ Public Beta**
>
> InputFlow is usable today for Windows-to-Linux keyboard, pointer, and text clipboard sharing, but it is still stabilizing around reconnection behavior, PowerToys peer registration, and desktop-environment edge cases. Expect rough edges, keep logs when something looks wrong, and prefer the pairing-helper onboarding flow described below instead of assuming PowerToys will always learn the Linux peer automatically.

InputFlow is a native C++17 Linux companion for [Microsoft PowerToys "Mouse Without Borders"](https://learn.microsoft.com/en-us/windows/powertoys/mouse-without-borders), enabling seamless cursor and keyboard sharing between Linux and Windows while keeping the Linux-side product identity distinct.

Works on X11 and is being hardened on Wayland via Linux's `uinput` kernel interface. Pointer injection now uses an absolute `EV_ABS` virtual mouse so protocol coordinates map directly into the detected local screen range. Runtime screen sizing prefers KDE's Wayland logical geometry when available, then falls back to `/sys/class/drm`; it does not require `xrandr`.

The command, service, and config paths still use `mwb` / `mwb-client` names for protocol compatibility and upgrade continuity. They will be migrated to `inputflow` aliases in a dedicated compatibility pass.

For contribution and disclosure policy, see [CONTRIBUTING.md](CONTRIBUTING.md) and [SECURITY.md](SECURITY.md).

## Attribution

This repository started as a fork of [chrischip/mwb-client-linux](https://github.com/chrischip/mwb-client-linux).

Since then it has been substantially expanded and reworked with:

- service and config management
- clipboard sync hardening
- controller and tray flows
- packaging and CI
- protocol debugging and recovery tooling
- public beta documentation and support scripts

The upstream project deserves credit for proving out the original Linux-side interoperability work.

## Public Beta Status

What is working well in current testing:

- Windows-to-Linux keyboard input
- Windows-to-Linux pointer movement and clicks
- text clipboard sync
- `systemd --user` service management
- Windows pairing-helper export for first-time setup and recovery

What still needs caution:

- some PowerToys builds do not learn the Linux peer automatically from a blank state
- reconnect behavior is improved but still under active hardening
- Wayland behavior depends on compositor support for `uinput` devices and clipboard helpers

## Quick Start

Recommended first-run flow:

1. Build the project and install a clipboard helper such as `wl-clipboard` on Wayland or `xclip` on X11.
2. Generate a Linux config with the Windows host and Linux machine name:
   `./build/mwb_client init-config --config ~/.config/mwb-client/config.ini --host 192.0.2.10 --name fedora`
3. Store the shared key in the desktop keyring instead of leaving `key=` inline:
   `printf '%s' 'MySecurityKey123' | ./build/mwb_client secret-store --config ~/.config/mwb-client/config.ini --secret-id desktop-default --stdin`
4. Export the Windows helper with `./build/mwb_client export-windows-pair --config ~/.config/mwb-client/config.ini --position top-left`.
5. Run the exported PowerShell helper on Windows to synchronize PowerToys MWB state for the Linux peer:
   `powershell -ExecutionPolicy Bypass -File .\\inputflow-windows-pair-fedora.ps1 -ClosePowerToys`
6. Start the Linux service with `./build/mwb_client install-user-service --config ~/.config/mwb-client/config.ini`.
7. Enable it with `systemctl --user daemon-reload && systemctl --user enable --now mwb-client.service`.
8. Verify the service with `./build/mwb_client doctor --config ~/.config/mwb-client/config.ini`.

Minimal example:

```bash
./build/mwb_client init-config \
  --config ~/.config/mwb-client/config.ini \
  --host 192.0.2.10 \
  --name fedora

printf '%s' 'MySecurityKey123' | ./build/mwb_client secret-store \
  --config ~/.config/mwb-client/config.ini \
  --secret-id desktop-default \
  --stdin

./build/mwb_client export-windows-pair \
  --config ~/.config/mwb-client/config.ini \
  --position top-left
```

## Features

- Absolute cursor movement and click injection (left, right, middle buttons, scroll wheel)
- Keyboard injection via Virtual Key Code translation to Linux `EV_KEY` codes
- Optional MPRIS media-key dispatch through `playerctl` for play/pause, next, previous, and stop
- Text clipboard sync using PowerToys MWB's inline and clipboard-socket flows, with structured payload parsing that preserves CF_HTML metadata while keeping plain-text fallback behavior
- Automatic reconnect with backoff and idle retry when the Windows host is offline
- Bidirectional TCP connection (connects out to Windows and accepts Windows's inbound connection)
- Windows pairing-helper export that seeds PowerToys peer state when current builds do not learn the Linux peer automatically
- Safer key sourcing via `key_file=` / `--key-file` or `key_secret_id=` / `--key-secret-id`
- Lightweight desktop controller and optional tray controller for the `systemd --user` service
- PowerToys-compatible AES-256-CBC transport and packet framing

## Project Structure

```
mwb-client-linux/
├── CMakeLists.txt
├── Dockerfile
├── README.md
├── docs/screenshots/
├── packaging/
├── tests/
├── tools/
└── src/
    ├── AppConfig.* / AppState.*
    ├── ClientRuntime.* / ClipboardManager.*
    ├── CryptoHelper.* / Discovery.*
    ├── InputDispatcher.* / InputManager.*
    ├── MediaKeyBridge.*
    ├── NetworkManager.*
    ├── PeerRecovery.* / SecretStore.*
    ├── Protocol.h / ReconnectPolicy.h / ScreenGeometry.h
    ├── TrayController.cpp
    └── main.cpp
```

## Build

### Prerequisites (Ubuntu / Debian)

```bash
sudo apt-get install -y build-essential cmake pkg-config libssl-dev zlib1g-dev
```

### Prerequisites (Fedora)

```bash
sudo dnf install -y gcc-c++ cmake make pkgconf-pkg-config openssl-devel zlib-devel
```

### Compile

```bash
cmake -S . -B build
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

Optional desktop/runtime helpers:

- `zenity` for the desktop controller
- `wl-clipboard`, `xclip`, or `xsel` for clipboard sync
- `playerctl` for MPRIS media-key dispatch
- GTK 3 plus Ayatana AppIndicator development packages if you want the optional `mwb_tray` binary

### Sanitizer debug build

```bash
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug -DMWB_ENABLE_SANITIZERS=ON
cmake --build build-sanitize -j$(nproc)
ctest --test-dir build-sanitize --output-on-failure
```

## Runtime

### `/dev/uinput` access

Load the kernel module once:

```bash
sudo modprobe uinput
```

Persist it across reboots:

```bash
echo uinput | sudo tee /etc/modules-load.d/uinput.conf
```

Allow non-root access with a dedicated group and udev rule:

```bash
sudo groupadd -r inputflow
sudo usermod -aG inputflow $USER
echo 'KERNEL=="uinput", GROUP="inputflow", MODE="0660", OPTIONS+="static_node=uinput"' | sudo tee /etc/udev/rules.d/99-inputflow-uinput.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
```

Log out and back in for group membership to take effect. Avoid adding desktop users to a broad `input` group unless your distribution explicitly requires it, because that can grant access to physical input event devices too.

If `/dev/uinput` is unavailable, the client still connects for protocol testing, but local input injection stays disabled until the device node is accessible.

Reusable distro packaging snippets for this setup and the user service live under `packaging/`.
The Fedora/RPM skeleton is `packaging/rpm/inputflow.spec`; it packages the
existing `mwb_client` command and `mwb-client.service` compatibility names.

### Screen sizing

The client uses this order:

1. `--screen-width` and `--screen-height`, or `screen_width=` and `screen_height=` in config
2. `MWB_SCREEN_WIDTH` and `MWB_SCREEN_HEIGHT`, if both are set
3. KDE Wayland logical geometry from `kscreen-doctor`, when available
4. Enabled connector modes from `/sys/class/drm`
5. A 1920×1080 fallback

CLI overrides win over environment variables, and environment variables win over values loaded from `config.ini`.
Automatic sizing is usually good enough on KDE Plasma Wayland and straightforward single-desktop setups. Explicit overrides are mainly useful on unusual multi-monitor layouts, mixed-DPI desktops, or inside containers where automatic detection may not reflect the host desktop accurately.

Example:

```bash
./build/mwb_client run --host 192.0.2.10 --key MySecurityKey123 --screen-width 2560 --screen-height 1600
```

### Clipboard helper tools

Text clipboard sync depends on a userspace clipboard command on Linux. Install one of:

- Wayland: `wl-clipboard`
- X11: `xclip` or `xsel`

The current Linux clipboard backend still publishes text to local applications.
The protocol layer now keeps a structured clipboard payload model so explicit
`TXT` entries, CF_HTML raw data, CF_HTML offsets/fragments, normalized plain-text
fallback, and future image payloads can be handled without overloading the
text-only API surface.

Runtime tuning:

- `MWB_CLIPBOARD_POLL_MS=1000` adjusts how often the client polls the local clipboard for changes.
- `MWB_CLIPBOARD_RECEIVE_ONLY=1` keeps incoming clipboard sync enabled while disabling local clipboard watches.
- `MWB_CLIPBOARD_FORCE_POLL=1` re-enables polling on Wayland compositors where `wl-paste --watch` is unsupported.
- `MWB_DISABLE_CLIPBOARD=1` disables clipboard sync entirely for input-latency troubleshooting.
- `MWB_KEY_FILE=/path/to/security-key` loads the security key from a file instead of `key=`.
- `MWB_KEY_SECRET_ID=desktop-default` loads the security key from the desktop keyring via Secret Service.
- `MWB_DEBUG_NETWORK=1` enables verbose packet and heartbeat logging.
- `MWB_KEY_REPEAT_DELAY_MS=250` and `MWB_KEY_REPEAT_PERIOD_MS=33` override the virtual keyboard repeat settings when a desktop session does not apply its own defaults.
- `MWB_MPRIS_PLAYER=spotify` targets a specific MPRIS player for media keys when `playerctl` is installed.
- `MWB_DISABLE_MPRIS_MEDIA_KEYS=1` disables MPRIS media-key dispatch and leaves media keys to the virtual keyboard fallback.
- `MWB_LATENCY_REPORT=1` prints input queue and injection timing when the client shuts down.

Cross-host latency probe:

```bash
# On the Linux client.
python3 tools/latency_probe.py server --bind 0.0.0.0 --port 15111

# On the Windows host, from a checkout/copy of this repository.
py tools\latency_probe.py client <LINUX_IP> --port 15111 --count 1000 --warmup 50 --interval-ms 1 --color always
```

The probe reports round-trip latency, estimated one-way latency, responder ACK processing time, jitter, drops, CPU usage, and resident memory in milliseconds. It does not require synchronized clocks because the client measures round-trip time locally and the responder only reports its own internal ACK processing duration. This cross-host probe is the useful measurement for Windows-to-Linux service/network latency; the CTest input-latency test is only a deterministic local collector check.

Probe methodology:

- Start `server` on the machine being measured as the responder, usually the Linux client.
- Run `client` from the other machine, usually the Windows host.
- The server stays running and accepts repeated client runs. Add `--once` if you want it to exit after one run.
- Each sample sends one numbered TCP probe packet, receives an ACK, and records round-trip time on the client clock.
- `estimated one-way` is `round trip / 2`; use it as an approximation, not a synchronized-clock truth.
- `server ACK process` is measured only on the server clock and shows how long the responder took to receive and ACK the packet.
- `warmup` samples are discarded so connection setup, CPU wakeup, and first-use effects do not skew the table.
- `jitter` is sample standard deviation; high jitter means the input path is inconsistent even when average latency looks acceptable.
- The probe sets `TCP_NODELAY` so results are less affected by TCP batching.

Probe flags:

- `server --bind ADDR` chooses the local address to listen on. Use `0.0.0.0` to accept LAN clients or `127.0.0.1` for local-only testing.
- `server --port PORT` chooses the TCP port. The default is `15111`.
- `server --once` exits after one client run. Without it, the server keeps accepting more tests.
- `client HOST` is the responder IP or host name.
- `client --port PORT` must match the server port.
- `client --count N` is the number of measured samples after warmup.
- `client --warmup N` is the number of initial samples to discard.
- `client --interval-ms MS` waits between probes. Use `1` to approximate a 1000 Hz input cadence, and `0` to stress the network path without pacing.
- `client --timeout-ms MS` controls per-packet timeout.
- `client --color auto|always|never` controls terminal color.

Copyable probe commands:

```bash
# Linux responder, all network interfaces.
python3 tools/latency_probe.py server --bind 0.0.0.0 --port 15111

# Linux responder, local-only smoke test.
python3 tools/latency_probe.py server --bind 127.0.0.1 --port 15111
```

```powershell
# Windows host, 1000 Hz-style paced test. Replace <LINUX_IP>.
python .\latency_probe.py client <LINUX_IP> --port 15111 --count 1000 --warmup 50 --interval-ms 1 --color always

# Windows host, unpaced burst test. Replace <LINUX_IP>.
python .\latency_probe.py client <LINUX_IP> --port 15111 --count 1000 --warmup 50 --interval-ms 0 --color always

# Windows host, longer stability run. Replace <LINUX_IP>.
python .\latency_probe.py client <LINUX_IP> --port 15111 --count 10000 --warmup 100 --interval-ms 1 --timeout-ms 2000 --color always
```

Recommended interval test:

1. Run the paced `--interval-ms 1` command and save the output.
2. Restart the server, then run the unpaced `--interval-ms 0` command and save the output.
3. If unpaced latency is much lower, scheduler/timer pacing or power management is contributing.
4. If both runs have high p95/p99 latency, investigate Wi-Fi quality, VPNs, Windows power mode, CPU load, and LAN path.
5. Prefer p95/p99 and max over average when judging input feel.

If the client prints `socket.timeout`, the responder is not reachable. Confirm the Linux server is still running, the IP address is correct, the port matches, and the firewall allows TCP port `15111`.

On Wayland, the client prefers `wl-paste --watch` for near-immediate clipboard updates when the compositor supports the wlroots data-control protocol. If watch mode is unavailable, local clipboard polling is disabled by default to avoid disrupting launcher shortcuts on GNOME-style sessions. Incoming clipboard writes from Windows still work, and you can opt into an explicit receive-only mode with `MWB_CLIPBOARD_RECEIVE_ONLY=1` or force poll fallback with `MWB_CLIPBOARD_FORCE_POLL=1`.

### Security notes

- The Linux listeners only accept inbound control and clipboard connections from the configured Windows peer IP.
- Remote clipboard payloads are size-limited, and unsupported image clipboard transfers are rejected instead of being buffered indefinitely.
- Clipboard socket trust remains bound to an active authenticated control session before any decoded clipboard text is delivered.
- The client now validates the legacy MWB packet magic/checksum on receive, but the upstream PowerToys protocol itself does not provide modern authenticated encryption. Full MAC/AEAD protection would require a protocol change on both ends.
- Prefer `--key-file` or `--key-secret-id`, or use `secret-store --stdin`, rather than putting the security key directly in shell history.

## Usage

```bash
./build/mwb_client <WINDOWS_IP> <SECURITY_KEY> [PORT]
```

Or use the subcommands:

```bash
./build/mwb_client run --config ~/.config/mwb-client/config.ini
./build/mwb_client run --host 192.0.2.10 --key-file ~/.config/mwb-client/security-key
./build/mwb_client run --host 192.0.2.10 --key-secret-id desktop-default
./build/mwb_client discover
./build/mwb_client doctor --config ~/.config/mwb-client/config.ini
./build/mwb_client init-config --config ~/.config/mwb-client/config.ini
printf '%s' 'MySecurityKey123' | ./build/mwb_client secret-store --config ~/.config/mwb-client/config.ini --secret-id desktop-default --stdin
./build/mwb_client install-user-service
```

`doctor` is read-only. It reports config validity, key source, `/dev/uinput` access, session type, clipboard helpers, XDG portal reachability, and user-service state without starting the network client.

Useful `run` options:

```bash
./build/mwb_client run --host 192.0.2.10 --key MySecurityKey123 --name fedora
./build/mwb_client run --host 192.0.2.10 --key-file ~/.config/mwb-client/security-key --name fedora
./build/mwb_client run --host 192.0.2.10 --key-secret-id desktop-default --name fedora
./build/mwb_client run --config ~/.config/mwb-client/config.ini --screen-width 2560 --screen-height 1600
./build/mwb_client run --config ~/.config/mwb-client/config.ini --clipboard-receive-only
./build/mwb_client run --config ~/.config/mwb-client/config.ini --manual-only
./build/mwb_client run --config ~/.config/mwb-client/config.ini --mpris-player spotify
```

- `WINDOWS_IP` — IP address of the Windows machine running PowerToys MWB
- `SECURITY_KEY` — The security key shown in **PowerToys → Mouse Without Borders → Security key**
- `PORT` — Optional, defaults to `15101` (keyboard/mouse channel). The clipboard socket uses `PORT - 1`, so the PowerToys defaults remain `15101` for input and `15100` for clipboard.

Example:

```bash
./build/mwb_client 192.0.2.10 MySecurityKey123
```

Example with explicit screen sizing:

```bash
./build/mwb_client run --host 192.0.2.10 --key MySecurityKey123 --screen-width 2560 --screen-height 1600
```

### Discovery

`discover` scans directly connected private IPv4 networks for reachable listeners on TCP port `15101`. It does not use the security key, does not trust peers automatically, and does not enable input by itself. Host names are best-effort through Avahi/mDNS and Windows NetBIOS node-status lookup.

```bash
./build/mwb_client discover --port 15101 --timeout-ms 200 --max-hosts 256
```

Use a discovered IP with the existing key-based flow:

```bash
./build/mwb_client run --host 192.0.2.10 --key MySecurityKey123
./build/mwb_client run --host 192.0.2.10 --key-file ~/.config/mwb-client/security-key
./build/mwb_client run --host 192.0.2.10 --key-secret-id desktop-default
```

### Config and user service

Generate a config template:

```bash
./build/mwb_client init-config --config ~/.config/mwb-client/config.ini
```

Export a ready-to-run Windows PowerShell helper from the current Linux config:

```bash
./build/mwb_client export-windows-pair --config ~/.config/mwb-client/config.ini
./build/mwb_client export-windows-pair --config ~/.config/mwb-client/config.ini --position top-left
```

By default this writes `./inputflow-windows-pair-<machine>.ps1` with the Linux machine name,
shared security key, and detected Linux IPv4 baked in. Copy that script to the Windows
machine and run:

```powershell
powershell -ExecutionPolicy Bypass -File .\inputflow-windows-pair-fedora.ps1 -ClosePowerToys
```

The helper synchronizes PowerToys `SecurityKey`, `MachineMatrixString`, `MachinePool`, and
`Name2IP` so the Linux peer appears reliably even when current PowerToys builds do not
persist it after the first TCP handshake. Pass `--linux-ip` if auto-detection picks the
wrong local address, `--position` to pick `top-left`, `top-right`, `bottom-left`, or
`bottom-right`, and `--force` to overwrite an existing exported helper.

Install a `systemd --user` service instead of backgrounding the process manually:

```bash
./build/mwb_client install-user-service
systemctl --user daemon-reload
systemctl --user enable --now mwb-client.service
```

The generated service runs the client in the foreground with:

```bash
./build/mwb_client run --config ~/.config/mwb-client/config.ini
```

`config.ini` now supports:

- `key_file=` to load the security key from a separate file
- `key_secret_id=` to load the security key from the desktop keyring through Secret Service
- `machine_name=` to override the hostname sent to PowerToys
- `clipboard_send_enabled=` for explicit receive-only clipboard mode
- `auto_connect_enabled=` to keep the service connected automatically or leave it idle until re-enabled
- `reconnect_initial_backoff_ms=`, `reconnect_max_backoff_ms=`, and `reconnect_idle_retry_ms=` to tune offline retry behavior; runtime delays are capped at 30000 ms so a recovered peer does not wait many minutes to reconnect
- `screen_width=` and `screen_height=` to override automatic screen-size detection
- `mpris_media_keys_enabled=` and `mpris_player=` to control MPRIS media-key dispatch
- `latency_report=` to print input queue and injection timing when the service stops
- `MWB_MOUSE_TRACE=N` to keep and dump the last N mouse packets on shutdown for input debugging; values above 1024 are capped

Example:

```ini
host=192.0.2.10
key=
key_file=security-key
key_secret_id=
machine_name=fedora
port=15101
clipboard_enabled=true
clipboard_send_enabled=true
clipboard_force_poll=false
clipboard_poll_ms=1000
auto_connect_enabled=true
reconnect_initial_backoff_ms=1000
reconnect_max_backoff_ms=30000
reconnect_idle_retry_ms=30000
screen_width=
screen_height=
mpris_media_keys_enabled=true
mpris_player=
latency_report=false
```

When `key_file` is relative, it is resolved relative to the directory containing `config.ini`.

To migrate an existing inline key or key file into the desktop keyring and rewrite the config in one step:

```bash
printf '%s' 'MySecurityKey123' | ./build/mwb_client secret-store --config ~/.config/mwb-client/config.ini --secret-id desktop-default --stdin
```

To remove a stored desktop-keyring entry:

```bash
./build/mwb_client secret-clear --config ~/.config/mwb-client/config.ini --secret-id desktop-default
```

Runtime state is stored separately under XDG state, for example `~/.local/state/mwb-client/state.ini`. It records a stable local machine ID plus discovered/approved peers without changing the security-key trust model.
The saved peer state also tracks whether a peer is connected right now so the desktop controller can surface live connection status without guessing from historical timestamps.

### Desktop Controller

For Fedora/KDE/GNOME, the repo includes a lightweight Zenity desktop controller:

```bash
./mwb-desktop-ui.sh
```

It edits config, runs discovery, shows known peers, starts/stops/restarts the `systemd --user` service, and can install desktop entries for itself and the tray controller.
The settings flow supports an inline security key, a separate key-file path, or a Secret Service-backed desktop-keyring entry, plus clipboard, screen-size override, MPRIS media-key, and input-latency diagnostic options.
The dedicated Connection Behavior screen lets you switch between auto-connect and manual mode and tune the reconnect timing without editing `config.ini` by hand.
The discovery and known-peer views show whether a peer is already paired, whether it is connected now, and whether it is the configured host.
Known peers are actionable from the controller: you can promote a saved peer to the configured Windows host or forget it from saved peer state without editing files by hand.
The settings flow also distinguishes between editing the current configured host and replacing it with a newly discovered peer.

### Tray Controller

When `gtk+-3.0` and `ayatana-appindicator3` development packages are available at build time, CMake also builds an optional tray controller:

```bash
./build/mwb_tray
```

The InputFlow tray menu can:

- open the Zenity controller
- open the dedicated Connection Behavior editor
- show the InputFlow name and service state in tray hover text when the desktop indicator host supports it
- keep only one tray instance running per user session
- jump directly to settings, peer discovery, known peers, and service details
- start, stop, or restart `mwb-client.service`
- show current service status
- show tray visibility help and install desktop entries

On GNOME/Wayland, tray visibility still depends on AppIndicator support in the shell environment, so the Zenity controller remains the primary fallback.
When the tray is available, the controller and tray share the same peer-management flow, including discovery, known-peer actions, and service control.

### Setup in PowerToys

1. Open **PowerToys → Mouse Without Borders** on Windows.
2. Enable the feature and note the current **Security key** if you are pairing manually.
3. Prefer the exported `inputflow-windows-pair-<machine>.ps1` helper from Linux to seed the shared key, `MachineMatrixString`, `MachinePool`, and `Name2IP`.
4. Reopen PowerToys and confirm the Linux peer appears in the layout.
5. Adjust the layout slot if needed, or regenerate the helper with a different `--position`.
6. Start the Linux service and verify the connection from the tray or desktop controller.

Direct manual pairing in PowerToys can work, but the helper is the more reliable current path when Windows does not persist the Linux peer on its own.

### Troubleshooting and Diagnostics

Start with the built-in doctor:

```bash
./build/mwb_client doctor --config ~/.config/mwb-client/config.ini
```

Useful Linux-side checks:

- `systemctl --user status mwb-client.service`
- `journalctl --user -u mwb-client.service`
- `./build/mwb_client discover --port 15101 --timeout-ms 200 --max-hosts 256`

Windows-side support helpers in `tools/`:

- `windows_mwb_collect.ps1` gathers PowerToys process, listener, event-log, and settings state
- `windows_mwb_lock_inspect.ps1` samples transient lockers for `settings.json`
- `windows_mwb_socket_trace.ps1` captures MWB-specific process, service, event-log, file, and socket traces
- `windows_mwb_seed_peer.ps1` seeds PowerToys peer state directly when recovery is needed

If PowerToys connects transiently but never shows the Linux machine in its layout, rerun the exported pairing helper before assuming the Linux service or security key is wrong.

If you attach a trace publicly, redact:

- shared security keys
- private hostnames
- private IP addresses
- exported helper scripts with baked-in secrets

## Docker

```bash
docker build -t mwb-linux .
docker run --rm -it --network host --device /dev/uinput:/dev/uinput \
    -e MWB_SCREEN_WIDTH=2560 -e MWB_SCREEN_HEIGHT=1600 \
    mwb-linux <WINDOWS_IP> <SECURITY_KEY>
```

## Podman

```bash
podman build -t mwb-linux .
podman run --rm -it --network host --device /dev/uinput:/dev/uinput \
    --security-opt label=disable --group-add keep-groups \
    -e MWB_SCREEN_WIDTH=2560 -e MWB_SCREEN_HEIGHT=1600 \
    localhost/mwb-linux <WINDOWS_IP> <SECURITY_KEY>
```

`--security-opt label=disable` is commonly needed on SELinux-enforcing Fedora hosts for direct `/dev/uinput` access. Rootless Podman may also need `--group-add keep-groups` so the container keeps the host user's supplemental groups.

## Protocol Notes

The PowerToys MWB protocol uses:

- **Transport:** TCP on port 15101 (mouse/keyboard) and 15100 (clipboard)
- **Encryption:** AES-256-CBC, no padding, streaming mode
- **Key derivation:** PBKDF2-HMAC-SHA512, 50 000 iterations, fixed salt derived from `ulong.MaxValue` encoded as UTF-16LE
- **IV:** Fixed string `"1844674407370955"` (ASCII, 16 bytes)
- **Magic number:** 24-bit hash of the security key via 50 000 rounds of SHA-512
- **Packet sizes:** 32 bytes (small: mouse, keyboard, small heartbeat) or 64 bytes (big: handshake, identity, matrix)
- **Handshake:** Both sides exchange type-126 challenge packets and respond with type-127 acknowledgements carrying the bitwise-NOT of the received 16-byte challenge payload
- **Post-handshake control:** `Hello` (3), `Heartbeat` (20), `Awake` (21), and `Heartbeat_ex` / `Heartbeat_ex_l2` / `Heartbeat_ex_l3` (51/52/53) are used for identity, keepalive, and peer-registration flow

The machine name sent by this client must match the name configured in Windows's MWB machine list.

## Known Limitations

- Outside KDE Wayland sessions, automatic screen sizing from `/sys/class/drm` assumes enabled outputs form one horizontal desktop. Use `screen_width`/`screen_height` or `MWB_SCREEN_WIDTH`/`MWB_SCREEN_HEIGHT` for stacked displays, mixed-DPI layouts, or containerized runs. Incorrect geometry means incorrect absolute pointer scaling.
- Some PowerToys builds still do not persist a blank-state Linux peer automatically. In that case, use the exported Windows pairing helper first.
- Clipboard sync currently writes text to the local Linux clipboard. The protocol parser preserves CF_HTML metadata internally for richer future backends, but local multi-MIME HTML ownership, image clipboard data, and drag/drop file transfer are not implemented.
- Wayland compositor handling of synthetic absolute `uinput` pointer devices varies. If cursor reachability breaks, run once with `MWB_MOUSE_TRACE=200`, reproduce the issue, then stop the service and inspect the dumped packet trace.

## Roadmap

Short term:

- Wire the structured clipboard payload model into richer local backends that can publish HTML plus plain-text fallback where the desktop protocol supports it.
- Confirm the PowerToys MWB image wire payload format before enabling image clipboard receive/write support.
- Add explicit file send/receive into a configured folder.

Longer term:

- Add a Wayland-native input backend using libei / XDG desktop portals alongside the current `uinput` backend.
- Add screenshot handoff, command palette actions, and "open on other machine" workflows.
- Add a custom Windows companion only when features require protocol extensions, such as bidirectional lock sync, telemetry, richer file drag/drop, or audio routing.

Future platform track:

- Split shared protocol, crypto, network, and config code into a platform-neutral core.
- Prototype a macOS receiver using CoreGraphics event posting, `NSPasteboard`, a `LaunchAgent`, and an AppKit menu bar controller.
- Consider bidirectional macOS support only after receiver mode works, because global edge capture and keyboard monitoring require macOS Input Monitoring / Accessibility permission flows.

## Test Coverage

The current automated checks cover:

- config, state, and discovery unit tests
- KDE Wayland logical screen-geometry parser tests
- peer-recovery/reconnect candidate selection tests
- reconnect-policy and persisted session-state transition tests
- input-mapping regression tests for adaptive coordinate scaling and edge clamping
- input-latency summary tests
- MPRIS media-key bridge mapping tests
- protocol parsing, structured clipboard payload and CF_HTML preservation tests, session binding, and clipboard socket security regression tests
- `mwb_client --help` smoke validation
- `mwb_client doctor` smoke validation and health-report category assertions

## License

GNU GPL v3.0 — see [LICENSE](LICENSE).

InputFlow is independent and is not affiliated with or endorsed by Microsoft. It interoperates with the PowerToys Mouse Without Borders protocol by studying the published open-source implementation at [microsoft/PowerToys](https://github.com/microsoft/PowerToys).

Microsoft and Mouse Without Borders are trademarks of the Microsoft group of companies.
