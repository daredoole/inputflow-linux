# Repository Guidelines

## Project Structure & Module Organization
This repository is a small C++17 client for PowerToys Mouse Without Borders on Linux. Core code lives in `src/`:

- `main.cpp` wires startup, CLI parsing, and the receive loop.
- `NetworkManager.*` handles the TCP protocol and callbacks.
- `InputManager.*` handles `uinput`-based mouse and keyboard injection.
- `CryptoHelper.*` contains protocol encryption helpers.
- `Protocol.h` defines shared packet layouts and protocol enums.

Keep new code in `src/` with matching header and implementation files. Do not commit build output; `build/` is ignored.

## Build, Test, and Development Commands
Install prerequisites on Debian/Ubuntu:

```bash
sudo apt-get install -y build-essential cmake pkg-config libssl-dev libevdev-dev
```

Configure and build locally:

```bash
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

Run the client from `build/`:

```bash
./mwb_client <WINDOWS_IP> <SECURITY_KEY> [PORT]
```

Container build is available for isolated verification:

```bash
docker build -t mwb-linux .
docker run --rm -it --device /dev/uinput:/dev/uinput mwb-linux <WINDOWS_IP> <SECURITY_KEY>
```

## Coding Style & Naming Conventions
Follow the existing C++ style: 4-space indentation, opening braces on the same line, and `#pragma once` in headers. Use `PascalCase` for classes and headers (`NetworkManager`), `camelCase` for functions and local variables, and the existing `m_` prefix for private members. Keep shared protocol types in `namespace mwb`, and preserve packed protocol structures in `Protocol.h`.

## Testing Guidelines
Automated tests are available through CTest. At minimum, every change must build cleanly in a fresh `build/` directory and pass:

```bash
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure
```

For behavior changes, include manual verification notes in the PR: Linux distro, desktop session, whether `/dev/uinput` was exercised, and the Windows PowerToys scenario used.

## Commit & Pull Request Guidelines
Git history currently uses concise, sentence-style subjects with a colon when useful, for example `Initial commit: Linux client for PowerToys Mouse Without Borders`. Follow that pattern with imperative summaries such as `InputManager: fix wheel delta handling`.

Do not use `codex`, `[codex]`, or other agent/tool branding as a commit-message or PR-title prefix. Use real project scopes instead, such as `Clipboard: add structured payload model`, `InputManager: fix wheel delta handling`, or `Tests: cover reconnect cleanup`.

PRs should describe the user-visible change, list build and manual verification steps, and link the relevant issue if one exists. Include logs or terminal snippets when changing protocol, input injection, or connection behavior.
