# Research Prompt: Cross-Platform KVM Feedback And Compatibility

Use this prompt for a focused research pass before implementing feedback-inspired features.

## Objective

Identify the user-visible failures and winning features across Linux/Windows keyboard-mouse sharing tools, then convert them into InputFlow requirements that preserve Microsoft PowerToys Mouse Without Borders compatibility.

## Sources To Review

- Official documentation and release notes for PowerToys Mouse Without Borders.
- Official repositories, docs, and issue trackers for Barrier, Input Leap, Deskflow, Synergy, and Cursr.
- Recent user feedback threads from Reddit, GitHub issues, forums, and distro communities.
- InputFlow local docs and source, especially `README.md`, `SECURITY.md`, `src/InputManager.cpp`, `src/ScreenGeometry.h`, `src/main.cpp`, and `mwb-desktop-ui.sh`.

## Questions To Answer

- Which tools currently support Windows plus Linux, and what protocol or architecture do they use?
- Which projects are maintained, deprecated, forked, or commercially supported as of the research date?
- What setup steps users praise or complain about?
- What multi-monitor layouts fail most often?
- How do tools model displays: per-machine rectangle, per-monitor topology, or explicit edge graph?
- Which tools support edge wrap, bidirectional traversal, and layouts like `B A B`?
- What clipboard formats are supported, and where do users report reliability problems?
- What installer or helper behaviors are considered unsafe or invasive?
- What security model is used: shared key, TLS/certificates, accounts/cloud, or unauthenticated LAN?

## InputFlow Compatibility Requirements

- Must remain compatible with PowerToys Mouse Without Borders on Windows.
- Must keep the existing AES-256-CBC MWB-compatible protocol unless a separate opt-in mode is designed.
- Must use ports `15101` for input and `15100` for clipboard unless PowerToys compatibility changes.
- Must support existing config keys and auth sources.
- Must preserve Linux input injection through `/dev/uinput`.
- Must work with the existing user systemd service and tray/controller workflow.
- Must not require installing a Windows service or stopping unrelated Windows applications.
- Must keep diagnostics redacted by default.

## Deliverables

- A dated evidence table with source links, project status, supported platforms, topology behavior, security model, and install friction.
- A ranked list of InputFlow feature candidates with implementation risk.
- A topology requirements document covering `AAB`, `BAA`, `ABA`, `BAB`, stacked displays, asymmetric resolutions, and wrap modes.
- A compatibility risk assessment for any feature that touches protocol, encryption, clipboard, input mapping, or Windows helper behavior.

## Output Format

- Keep direct quotes short and cite each source.
- Separate verified facts from inferred product recommendations.
- Flag unstable facts that need re-checking before release notes or marketing copy.

