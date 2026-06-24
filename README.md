# InputFlow

![Status: Public Beta](https://img.shields.io/badge/status-public%20beta-e0a100)
![License: GPLv3](https://img.shields.io/badge/license-GPLv3-blue)

InputFlow is a native C++17 Linux companion for [Microsoft PowerToys "Mouse Without Borders"](https://learn.microsoft.com/en-us/windows/powertoys/mouse-without-borders), enabling seamless cursor and keyboard sharing between Linux and Windows.

## 🚀 Quick Start (Tray & UI Setup)

Recommended first-run flow for most users:

1. **Install Prerequisites:**
   - **Fedora:** `sudo dnf install python3-gobject gtk3 libayatana-appindicator3`
   - **Ubuntu/Debian:** `sudo apt install python3-gi gir1.2-gtk-3.0 libayatana-appindicator3-0.1`
2. **Launch Setup UI:** Run `./mwb-desktop-ui.sh menu`
3. **Configure:**
   - Go to **Settings** -> Enter your Windows Host IP and Security Key.
4. **Use PowerToys layout for normal setups:**
   - If this Linux/Fedora machine has one monitor, do not configure topology. Let Windows PowerToys Mouse Without Borders own the Linux/Windows machine placement.
   - If topology was enabled while testing, choose **Use PowerToys Layout Only** to set `topology_enabled=false`.
5. **Pair with Windows:**
   - In the same UI, use the **Export Helper** option.
   - Run the exported `.ps1` script on your Windows machine to register the Linux peer.
6. **Start:** Choose **Start Service** or launch the tray with `./build/mwb_tray`.
7. **Advanced layouts only:** Open **Advanced Topology/Layout** if you have multiple Linux monitors, stacked/asymmetric edges, wrap behavior, or wrong-edge handoff problems.

For the full beta setup, health-check, diagnostics, connection-quality, and packaging-verification workflow, see [docs/beta-workflow.md](docs/beta-workflow.md).

---

## 🛠️ Build & Installation

### 1. Prerequisites

**Ubuntu / Debian:**
```bash
sudo apt-get install -y build-essential cmake pkg-config libssl-dev zlib1g-dev \
    python3-gi gir1.2-gtk-3.0 libayatana-appindicator3-dev
```

**Fedora:**
```bash
sudo dnf install -y gcc-c++ cmake make pkgconf-pkg-config openssl-devel zlib-devel \
    python3-gobject gtk3 libayatana-appindicator3-devel
```

### 2. Compile

```bash
cmake -S . -B build -DMWB_BUILD_TRAY=ON
cmake --build build -j$(nproc)
```

### 3. Setup `/dev/uinput` (Crucial for Mouse/Keyboard)

```bash
sudo modprobe uinput
sudo groupadd -r inputflow
sudo usermod -aG inputflow $USER
echo 'KERNEL=="uinput", GROUP="inputflow", MODE="0660", OPTIONS+="static_node=uinput"' | sudo tee /etc/udev/rules.d/99-inputflow-uinput.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
```
*Logout and back in for group changes to take effect.*

---

## ✨ Features

- **Absolute Cursor Movement:** Precise pointer control across screens.
- **Keyboard Sync:** Full keyboard sharing with media key support.
- **Rich Clipboard:** Text, HTML, and **Image** synchronization (plain text stays plain).
- **Systemd Integration:** Runs as a lightweight user service.
- **Tray + Dashboard:** Connection status, live peer list, LAN peer discovery, and settings that apply on save.
- **Self-Healing Reconnect:** Follows a peer across IP changes (DHCP/VPN/resume) without a restart — configure peers by **name**.
- **Lock on Disconnect:** Optionally lock the Linux session when the controlling peer drops.
- **Android Peer:** Control an Android device as a screen, with no-root (Accessibility/Shizuku) or root native-grade input injection. See [docs/android.md](docs/android.md).

---

## ⚠️ Public Beta Status

InputFlow is usable today but is still stabilizing. Expect rough edges around reconnection and desktop-environment edge cases.

**What is working well:**
- Windows-to-Linux keyboard/mouse input.
- Full Clipboard sync (Text/HTML/Images).
- `systemd` service management.
- Windows pairing-helper for easy setup.
- Self-healing reconnect across peer IP changes.
- Android peer relay (no-root and, optionally, Shizuku/root native injection).

See the [CHANGELOG](CHANGELOG.md) for what's new and [docs/](docs/) for the full
guides (compatibility, topology, Android, beta workflow, MWB-parity roadmap).

---

## 📖 Advanced Usage & CLI

For power users who prefer manual control:

```bash
./build/mwb_client run --config ~/.config/mwb-client/config.ini
./build/mwb_client discover
./build/mwb_client doctor --config ~/.config/mwb-client/config.ini
```

See the full [documentation section](#detailed-documentation) for environment variables and protocol details.

User-facing beta operations:

- [Guided Windows pairing and export helper](docs/beta-workflow.md#guided-pairing-and-export-helper)
- [Topology/layout wizard](docs/beta-workflow.md#topologylayout-wizard)
- [Health checks and diagnostics bundle](docs/beta-workflow.md#health-check)
- [Connection quality and latency reporting](docs/beta-workflow.md#connection-quality)
- [Packaging verification](docs/beta-workflow.md#packaging-verification)
- [Topology config contract and layout wizard expectations](docs/topology.md)
- [Android peer MVP](docs/android.md)
- [Migration from other keyboard/mouse sharing tools](docs/migration.md)
- [Compatibility matrix and platform caveats](docs/compatibility.md)

<a name="detailed-documentation"></a>
## Detailed Documentation

### Attribution
This repository started as a fork of [chrischip/mwb-client-linux](https://github.com/chrischip/mwb-client-linux) and has been substantially expanded with service management, rich clipboard support, and recovery tooling.

### Configuration (`config.ini`)
Supports `connection_mode`, `key_file`, `key_secret_id` (keyring), `screen_width/height` overrides, `topology_enabled`, `topology_file`, experimental `android_peers_enabled`, and more. Default path: `~/.config/mwb-client/config.ini`.

`connection_mode=powertoys` is the default Windows PowerToys/MWB compatibility path. `connection_mode=inputflow` runs native InputFlow peer services without requiring a Windows host/key. `connection_mode=hybrid` enables both paths at once.

Display-level topology is a separate opt-in contract. The default runtime remains MWB-compatible machine placement unless topology is explicitly enabled; see [docs/topology.md](docs/topology.md) for examples, wrap policies, validation, and cross-machine handoff behavior.

Windows PowerToys still owns the Windows-side machine layout. InputFlow topology does not edit PowerToys per-display geometry; it only tells Linux which local display edge should hand off back to Windows. Keep the PowerToys machine position and the InputFlow topology links consistent.

### Screen Sizing
The client detects screen size in this order:
1. Config/CLI overrides
2. KDE logical geometry (`kscreen-doctor`)
3. DRM connector modes (`/sys/class/drm`)
4. 1920x1080 fallback

### Network & Protocol
- **Port:** 15101 (Input), 15100 (Clipboard).
- **Encryption:** AES-256-CBC (PowerToys compatible).

---

## License
GNU GPL v3.0 — see [LICENSE](LICENSE).

InputFlow is independent and not affiliated with Microsoft. Interoperability is based on the open-source [microsoft/PowerToys](https://github.com/microsoft/PowerToys) implementation.
