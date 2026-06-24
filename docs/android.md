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

The Android project lives in `android/`. Targets Android 15+ (`targetSdk` /
`compileSdk` 36, `minSdk` 26). Build with the bundled wrapper and JDK 21:

```bash
cd android
JAVA_HOME=/path/to/jdk-21 ./gradlew :app:assembleDebug
```

The relay foreground service uses the `connectedDevice` type so Android 15's
`dataSync` runtime cap does not kill long sessions.

### Input injection backends (Settings → Input method)

Choose how input is delivered to the phone:

- **Accessibility** (default, no root): overlay cursor + gesture/focused-node
  edits. Works everywhere but is gesture-based and cannot type into secure
  fields.
- **Shizuku** (no root): shell-UID `injectInputEvent` — a real system cursor and
  key events, including secure fields. Requires the [Shizuku](https://shizuku.rikka.app/)
  app running; tap **Grant Shizuku** in Settings.
- **Root** (libsu): same native injection as root. Tap **Grant Root**.
- **Auto**: prefers Root > Shizuku > Accessibility by availability.

Native backends inject real `MotionEvent`/`KeyEvent` at system level, so they
behave like a hardware mouse/keyboard rather than synthesized accessibility
gestures.

### Pairing

Generate (and mint a strong secret if needed) with:

```bash
./build/mwb_client android-pair --generate --config ~/.config/mwb-client/config.ini
```

Use the printed `inputflow://android-peer?...` URI as QR content, open it on
Android, or enter the host, port, and secret manually. The relay refuses to
start with a weak `android_relay_secret` (see Security below).

**Connect by host name, not IP.** `android-pair` emits the Linux box's **host
name** by default; the app re-resolves it on every reconnect, so it follows the
Linux machine across IP changes (DHCP/VPN) — the same self-healing model as the
desktop client. If the phone can't resolve the bare name on your network, use
`<hostname>.local` (mDNS/Avahi), a name your DNS resolves, or
`android-pair --ip` to bake a fixed address.

## Security

The relay can drive **system-level input injection** on the phone when a Shizuku
or root backend is selected — that includes secure fields. The shared
`android_relay_secret` is therefore the gate to full device control over the LAN.

- The relay **refuses to start** and `android-pair` **refuses to emit a URI**
  when the secret is weak (`< 16` characters or too little variety).
- Generate a strong (256-bit) secret with `android-pair --generate`.
- Keep the relay on a trusted LAN/VPN; never expose its port to the internet.
- Treat the pairing URI/QR like a password — it contains the secret.

## Current limitations

- Accessibility backend is gesture-based (less precise, no secure-field input).
  Use Shizuku or root for native-grade injection.
- Linux physical keyboard/mouse capture needs the KDE/Wayland `libei`/EIS path for monitor-like behavior. The evdev fallback is intentionally opt-in and diagnostic only.
- Android can request control release from the app; richer edge-based return behavior is future work.
- The relay forwards button-up events, so native clicks are synthesized as
  press+release (taps/clicks work; true drag needs a relay-protocol addition).

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
