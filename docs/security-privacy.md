# Security and privacy model

InputFlow is a remote-input utility for trusted LANs and private VPNs. The
PowerToys Mouse Without Borders compatibility protocol is encrypted with
AES-256-CBC but does not provide modern authenticated integrity. Do not expose
its ports directly to the public internet.

The native Linux-to-Android relay uses a separate protocol. A random challenge
and the pairing secret derive directional session keys; all post-authentication
frames use AES-256-GCM with ordered sequence numbers. This protects keyboard,
pointer, notification, and topology data from passive capture, tampering, and
replay on the local network.

## Data handled

| Data | Purpose | Storage and sharing |
| --- | --- | --- |
| Pairing secrets | Authenticate Linux, Windows, and Android peers | Linux Secret Service or owner-only files; Android Keystore encryption; never diagnostic output |
| Hostnames and IP addresses | Connect to and display peers | Local config/state only; redacted from support bundles |
| Keyboard, pointer, and clipboard data | Deliver the requested control stream | Processed in memory; never telemetry or diagnostics |
| Android notification content | Optional notification synchronization | In memory during the active relay; excluded from diagnostics |
| Monitor geometry and topology | Route pointer movement | Local config/state; diagnostics report only non-identifying capability summaries |
| OS/session/package details | Troubleshooting | Local, user-reviewed diagnostic bundle only |

InputFlow has no telemetry or automatic crash/diagnostic upload. Diagnostic
bundles remain on the device, omit journal and network details by default, and
include a machine-readable consent manifest. Users must review and attach a
bundle manually.

## Privileged boundaries

- `/dev/uinput`, Android Accessibility, Shizuku, root injection, notification
  access, and IME access are separate user/admin grants.
- Native Android injection requires an additional in-app consent toggle.
- Revoking a platform permission must disable the corresponding feature without
  weakening another boundary.
- Config and state writes are atomic, owner-only, and reject symlink targets.

Security-sensitive logs must describe state transitions without pairing
secrets, clipboard/notification contents, device models, usernames, hostnames,
or network addresses.

Input-event metadata is logged only when an explicit debug-input option is
enabled. Do not enable that option during normal use, and review journal output
before sharing it.
