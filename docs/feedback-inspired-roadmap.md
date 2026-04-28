# Feedback-Inspired Roadmap

This plan turns user feedback from Linux/Windows keyboard-mouse sharing discussions into concrete InputFlow work. The recurring signal is that users do not just want another Synergy-style KVM. They want Mouse Without Borders behavior on Linux: reliable Windows interoperability, safe setup, shared-key pairing, sane multi-monitor traversal, and recoverable diagnostics.

## Product Goals

- Preserve PowerToys Mouse Without Borders protocol compatibility.
- Make Windows plus Linux setup safer and easier than Synergy-family tools.
- Treat multi-monitor geometry as a first-class feature, not a side effect.
- Make beta failures diagnosable without asking users to hand-copy logs.
- Avoid invasive installers or helpers that stop unrelated Windows software.

## Compatibility Targets

- Windows side: Microsoft PowerToys Mouse Without Borders settings and protocol behavior.
- Linux side: X11 and Wayland sessions where input injection is available through `/dev/uinput`.
- Network: trusted LAN operation over the existing MWB-compatible ports `15101` for input and `15100` for clipboard.
- Auth: existing shared security key model, including inline config, key file, and Secret Service-backed key references.
- Packaging: user-scoped systemd service gated by config presence, distro package install must not auto-enable remote-control behavior.
- Existing names: keep `mwb_client`, `mwb_tray`, `mwb-client.service`, and `~/.config/mwb-client/config.ini` until an alias migration is explicitly designed.

## Phase 1: Stabilize Current Beta Flow

- Keep the health check, diagnostics bundle, connection quality panel, and guided pairing flow.
- Add beta issue templates that request diagnostics bundle output, distro, desktop session, Windows version, PowerToys version, monitor layout, and whether clipboard is enabled.
- Add release notes that explain the trust boundary: InputFlow is for trusted LANs, not internet exposure.

## Phase 2: Screen Topology And Wrap

- Add a topology model that represents machines and individual displays separately.
- Add explicit wrap policies: `none`, `horizontal`, `vertical`, and `both`.
- Support edge-transition layouts that users call out as broken elsewhere: `AAB`, `BAA`, `ABA`, `BAB`, stacked displays, and asymmetric resolutions.
- Validate impossible layouts before saving them.
- Add tests for edge traversal, release/press preservation across transitions, and monitor-boundary ambiguity.

## Phase 3: Layout Wizard

- Add a visual layout wizard in the controller for Linux and Windows displays.
- Provide presets for common two-machine and three-screen layouts.
- Surface warnings for known protocol limitations before users start the service.
- Export the selected layout into the Windows helper flow when PowerToys settings need to be seeded.

## Phase 4: Safe Windows Helper

- Add dry-run output that shows exact PowerToys settings changes.
- Back up the PowerToys Mouse Without Borders settings file before writing.
- Provide a restore command or restore instructions in the generated helper.
- Do not stop browsers, VPNs, endpoint security, UPS utilities, or unrelated Windows processes.
- Keep the helper idempotent so users can rerun it after changing display topology.

## Phase 5: Migration And Positioning

- Add docs for users coming from Barrier, Input Leap, Deskflow, Synergy, Cursr, and Wine/MWB attempts.
- Explain that InputFlow is MWB-compatible, not Synergy-protocol compatible.
- Call out the design difference: InputFlow should prioritize MWB-style peer behavior, shared key setup, and wrap/topology handling over generic client-server KVM behavior.

## Decision Gates

- Do not claim competitor status or maintenance state without fresh primary-source verification.
- Do not add a new protocol mode unless MWB compatibility remains the default path.
- Do not ship topology changes without regression tests for the existing absolute cursor and reconnect behavior.
- Do not package auto-start behavior that activates remote input before a user creates config and opts in.

