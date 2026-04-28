# Contributing

InputFlow is in public beta. Small, focused fixes are easier to review and less likely to break protocol compatibility with PowerToys.

## Before opening a PR

1. Build in a clean tree.
2. Run the full CTest suite.
3. Sanity-check the user-facing flow you changed.
4. Make sure no local keys, pairing exports, trace reports, or machine-specific logs are included.

## Build and test

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

For sanitizer coverage:

```bash
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug -DMWB_ENABLE_SANITIZERS=ON
cmake --build build-sanitize --parallel
ctest --test-dir build-sanitize --output-on-failure
```

## Manual verification notes

When a change affects runtime behavior, include:

- Linux distro and desktop session
- Whether `/dev/uinput` input injection was exercised
- Whether clipboard sync was enabled
- Windows PowerToys version
- Whether pairing relied on the exported Windows helper
- Any reconnect, tray, or controller behavior you observed

## Code and docs expectations

- Keep compatibility names such as `mwb_client` and `mwb-client.service` unless the change is explicitly about renaming.
- Prefer `key_file`, keyring-backed secrets, or `stdin`-based examples over inline keys in docs.
- Update `README.md` when CLI flags, setup flow, or pairing expectations change.
- Do not commit generated support artifacts such as `mwb-windows-report-*.txt`, `mwb-lock-report-*.txt`, `mwb-socket-trace-*.txt`, or exported `inputflow-windows-pair-*.ps1` files.

## Good bug reports

Useful reports usually include:

- exact command or service mode used
- `journalctl --user -u mwb-client.service`
- whether the issue happens with a blank or pre-seeded Windows MWB state
- whether PowerToys `Use Service` is enabled
- whether clipboard sync is enabled

If you include logs, scrub security keys and private hostnames first.
