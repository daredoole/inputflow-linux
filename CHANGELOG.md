# Changelog

All notable changes to InputFlow are documented here. Format loosely follows
[Keep a Changelog](https://keepachangelog.com/); the project uses date-based
beta releases until a stable 1.0.

## [0.2.0] - 2026-06-24

First consolidated public-beta release.

### Added
- **Self-healing reconnect.** The client now follows a peer that changes its IP
  (DHCP lease, VPN, resume-from-suspend) without a restart. Recovery runs lazily
  when reconnect backoff saturates and immediately on netlink link/address
  changes. Works by peer **name**; also recovers a stale IP-literal host when
  exactly one approved peer is known. Configure the peer by name (not a hardcoded
  IP) to get the best behavior.
- **Lock on disconnect.** New `lock_on_disconnect` config option (and Settings
  toggle) locks the local session when the controlling peer disconnects. A manual
  **Lock Screen** action was added to the tray.
- **Dashboard upgrades.** Live peer list (connected / last-seen) on the Status
  tab, a **Discover…** button that scans the LAN and fills the peer *name*, and
  immediate apply: saving settings now offers to restart the service.
- **Android: native-grade input injection.** New pluggable backends behind a
  Settings toggle — **Accessibility** (no-root, default), **Shizuku** (no-root,
  shell-UID `injectInputEvent`), and **Root** (libsu). Native backends deliver a
  real system cursor and key events, including into secure fields. Auto mode
  prefers Root > Shizuku > Accessibility by availability.
- `android-pair --generate` mints a strong random `android_relay_secret` and
  saves it to the config.

### Changed
- **Android build modernized for 2026 / Android 15+:** `targetSdk`/`compileSdk`
  36 (Android 16), AGP 8.13, Kotlin 2.1.x, Java 21 toolchain. Build with the
  bundled `./gradlew` (Gradle 9.4.1) and JDK 21.
- Clipboard: plain text is now authoritative on write — incoming rich copies no
  longer paste as HTML markup or create duplicate clipboard entries.

### Security
- **Android relay foreground service** moved from `dataSync` to `connectedDevice`
  so Android 15's ~6h `dataSync` runtime cap no longer kills the relay.
- **Relay secret strength enforced.** Because the relay can now drive
  native/root input injection on the phone, the relay refuses to start and
  `android-pair` refuses to emit a pairing URI when `android_relay_secret` is
  weak (`< 16` chars or low variety). Use `android-pair --generate`.

### Fixed
- Reconnect loop no longer hammers a stale address indefinitely after a peer's
  IP changes.
