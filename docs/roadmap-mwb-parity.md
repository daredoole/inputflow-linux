# MWB Feature-Parity Roadmap

Status of Mouse Without Borders features on the Linux client, and what each
remaining item requires. Updated 2026-06-23.

## Shipped

- Absolute cursor movement, keyboard sync, media keys.
- Rich clipboard (text / HTML / image) with size guards.
- Auto-reconnect with exponential backoff.
- **Self-healing peer rediscovery** — follows a peer that changes IP (DHCP/VPN)
  without a restart. Works by peer name; also recovers a stale IP-literal host
  when exactly one approved peer is known. Triggered lazily on backoff
  saturation *and* immediately on netlink link/address changes (resume,
  new lease, VPN up). See `PeerRecovery.cpp`, `NetworkManager` watcher.
- **Session lock integration** — `lock_on_disconnect=true` locks the local
  session when the controlling peer drops; manual "Lock Screen" tray action.
  Uses logind (`loginctl lock-session`) with screen-locker fallbacks.
- Dashboard: live peer list (connected/last-seen), discovered-peer picker that
  prefers peer **names**, live-apply (offers service restart after save).

## Not yet implemented — and why

### File transfer / drag-and-drop
PowerToys MWB moves files over an undocumented, separate channel that is **not**
part of the reconstructed packet protocol in `Protocol.h` (no file packet
types). Wire-compatible support requires reverse-engineering the PowerToys
file-drop path and is a significant, uncertain effort. Recommended interim:
clipboard already carries text/HTML/images; a `text/uri-list` clipboard bridge
could cover "copy file path" without protocol changes — a smaller, self-driven
feature that does not depend on PowerToys internals.

### Lock-all-machines (networked)
Real MWB sends a lock signal to every machine in the group. No corresponding
packet type is present in the reconstructed protocol, so we cannot emit one that
PowerToys would honor. The local half is shipped (`lock_on_disconnect`, manual
lock). Networked lock-all needs the same protocol RE as file transfer.

### Global switch hotkey
A global hotkey on Wayland requires the compositor's GlobalShortcuts portal
(`org.freedesktop.portal.GlobalShortcuts`) or routing through the already-active
libei capture session. This is a self-contained but non-trivial piece of portal
plumbing; deferred rather than shipped half-working. On X11 a direct XGrabKey
path is possible. MWB's own switching is edge-driven (Matrix packet), which the
client already participates in.

## Suggested next steps (smallest → largest)
1. `text/uri-list` clipboard bridge for file-path copy (no protocol RE).
2. GlobalShortcuts-portal hotkey to toggle auto-connect / release control.
3. PowerToys file-drop protocol RE (prereq for file transfer + networked lock).
