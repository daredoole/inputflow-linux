# Android peer MVP

InputFlow can expose an experimental Android controlled-peer relay. The Linux client remains the hub: Windows sends input to InputFlow through the existing MWB session, and InputFlow forwards input to a paired Android app when topology hands off to the configured Android machine.

## Linux configuration

Add these keys to `~/.config/mwb-client/config.ini`:

```ini
connection_mode=hybrid
android_peers_enabled=true
android_relay_port=15102
android_relay_secret=replace-with-a-long-random-secret
android_peer_name=pixel-8
android_capture_backend=none
```

Then enable topology and add a machine/display whose machine id matches `android_peer_name`. When a cross-machine topology edge targets that machine, InputFlow forwards mouse events to Android. Keyboard events follow while the Android relay is active.

The relay is disabled by default. If `android_relay_secret` is empty, the relay does not start. Use `connection_mode=inputflow` for Android-only testing without a Windows PowerToys host, or `connection_mode=hybrid` when Windows and Android should both be active.

`android_capture_backend` controls Linux-local physical mouse capture:

- `none`: default. Android relay stays available for already-forwarded topology input without trying to capture the Fedora pointer.
- `evdev`: prototype fallback only. It can mirror/stick the KDE Wayland cursor and should not be used as a production monitor-like path.
- `libei`: planned KDE/Wayland backend. This is the target for real compositor-mediated capture/release behavior.

## Android app

The Android project lives in `android/`.

The app is intentionally no-root:

- `RelayForegroundService` connects to the Linux relay over LAN.
- `InputFlowAccessibilityService` shows a cursor overlay, dispatches click/scroll gestures, and performs basic focused-node text actions.
- `InputFlowImeService` is included as an optional keyboard surface for later richer text handling.

Generate a pairing payload with:

```bash
./build/mwb_client android-pair --config ~/.config/mwb-client/config.ini
```

Use the printed `inputflow://android-peer?...` URI as QR content, open it on Android, or enter the same host, port, and secret manually in the app.

## Current limitations

- Android input injection uses a no-root overlay plus Accessibility gestures/focused-node text edits, so it is less complete than a real HID or privileged input path.
- Linux physical keyboard/mouse capture needs the KDE/Wayland `libei`/EIS path for monitor-like behavior. The evdev fallback is intentionally opt-in and diagnostic only.
- Android can request control release from the app; richer edge-based return behavior is future work.

## Controls

With `android_capture_backend=libei` on KDE/Wayland:

- Enter Android: push through the configured Fedora edge.
- Return to Fedora: move left until the Android cursor reaches the left edge and keep moving left.
- Left click: tap/click at the Android cursor.
- Right click or `Esc`: Android Back.
- Middle click or `Meta`: Android Home.
- `Alt+Tab`: Android Recents.
- `Ctrl+Alt+N`: notification shade.
- `Ctrl+Alt+Q`: quick settings.
- Two-finger scroll / wheel: scroll focused Android content.
- Basic text input: letters, numbers, punctuation, space, enter, and backspace edit the focused Android text field.
