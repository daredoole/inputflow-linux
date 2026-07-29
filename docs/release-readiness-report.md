# InputFlow 0.2.0 Release Readiness Report

Assessment date: 2026-07-28

## Outcome

The current source is a release candidate. The automated release gate passes,
release artifacts are reproducible from the working tree, and generated
checksums verify. Publication still requires the external checks listed below.

## Implemented controls

- Android relay protocol v2 authenticates sessions with HMAC and protects
  post-authentication traffic with directional AES-256-GCM keys, ordered
  sequence numbers, replay rejection, frame limits, and authentication
  timeouts.
- Android pairing secrets use Android Keystore-backed AES-GCM storage with
  migration away from legacy plaintext preferences.
- Configuration and state use atomic, owner-only file writes and reject
  symbolic-link targets.
- Diagnostics are local-only by default, redact secrets and device/input
  metadata, and require explicit options for journal or network data.
- GUI-launched helpers use fixed argument vectors rather than shell command
  strings.
- Android backups exclude protected application data and lock-screen
  notifications avoid endpoint details.
- Linux and Android interfaces share an accessible InputFlow visual system,
  masked secret fields, descriptive controls, day/night palettes, and
  responsive settings surfaces.

## Verification evidence

- Nine-stage `scripts/release-gate.sh`: passed.
- Linux CTest suite: 18/18 passed.
- Diagnostics privacy regression: passed.
- Portable archive checksum and extracted-binary smoke test: passed.
- Android release assembly and JVM unit tests: passed.
- Android release lint: 0 errors, 23 non-blocking maintenance warnings.
- Release archive, unsigned APK, SBOM, and provenance checksums: passed.
- Source whitespace check: passed.
- Unsafe shell/process API scan: no matches.
- GTK accessibility inspection: four tabs exposed, status named, secrets
  masked, and settings actions reachable.

## Publication requirements

- Sign the Android APK with the protected production signing key, run
  `apksigner verify`, and archive signing evidence. The generated APK is
  intentionally unsigned.
- Run the checklist on representative GNOME and KDE systems across X11 and
  Wayland, plus Android API 26, a current Android release, and at least one
  OEM device. No Android emulator or physical device was available for this
  assessment.
- Perform a multi-device seven-day soak covering reconnects, sleep/wake,
  clipboard, topology transitions, notifications, and service restarts.
- Run the gate from a reviewed clean commit in CI. Provenance generated from
  the current working tree correctly records that the tree is dirty.
- Review and intentionally accept or fix the remaining Android lint
  maintenance warnings before the final store submission.

No finite test program can prove an application is perfect or vulnerability
free. Release approval should be based on the passing evidence above plus the
external device matrix, soak, signing, and clean-commit checks.
