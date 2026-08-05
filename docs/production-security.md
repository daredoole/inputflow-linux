# Production security and release standard

InputFlow is production-eligible only within its declared deployment boundary:
a trusted LAN or a private VPN with host firewall rules limiting reachability to
approved peers. The PowerToys compatibility ports must never be exposed directly
to the public internet.

This standard follows the risk-management structure of NIST SP 800-218 (SSDF),
uses OpenSSF Scorecard for repository supply-chain signals, and treats automated
checks as necessary evidence rather than proof that no vulnerability exists.

## Threat model

| Asset or boundary | Primary threats | Required controls |
| --- | --- | --- |
| Keyboard, pointer, and clipboard stream | Passive capture, tampering, replay, peer impersonation | Trusted LAN/VPN, host firewall, strong shared key, authenticated machine-ID pinning after first approved session |
| Pairing secrets | Shell history, permissive files, diagnostics, process memory, stolen pairing export | Secret Service or owner-only file, atomic symlink-safe writes, diagnostics redaction, short-lived derived copies, manual review of exports |
| Network parsers | Malformed frames, oversized payloads, decompression bombs, memory-safety defects | Fixed packet sizes, 16 MiB payload and inflation caps, connection limits/timeouts, negative tests, ASan/UBSan, coverage-guided fuzzing |
| Linux input privilege | Compromised network peer reaching `/dev/uinput` or desktop input portals | Explicit OS permission, no privilege escalation, hardened user service, revocable feature grants |
| Android input privilege | Abuse of Accessibility, Shizuku, root, notification, or IME access | Per-feature user consent, strong relay secret, Android Keystore, AEAD session protocol, non-exported or permission-protected components |
| Build and release | Compromised Actions, dependency drift, unsigned artifacts, secret leakage | SHA-pinned Actions, Gradle distribution checksum, Dependabot, CodeQL, Scorecard, SBOM, provenance, checksums, signed publication artifacts |

## Non-negotiable protocol boundary

The legacy Mouse Without Borders protocol uses compatibility-mandated
AES-256-CBC framing without modern authenticated integrity. InputFlow adds
challenge validation, session source/destination checks, and stable remote
machine-ID pinning, but it cannot add AEAD or a MAC unilaterally without breaking
PowerToys interoperability.

Therefore:

- direct internet exposure is unsupported and is a release-blocking deployment error;
- first contact remains shared-secret authenticated trust-on-first-use;
- after approval, a peer address change is accepted only when the encrypted handshake reports the pinned machine ID;
- users needing hostile-network operation must place the connection inside a mutually authenticated VPN.

## Mandatory automated gates

Every production commit must pass:

1. Ubuntu Release build, complete CTest suite, archive smoke test, and hardening checks.
2. Fedora package build and tests using the pinned container digest.
3. ASan and UBSan with failures treated as fatal.
4. Thirty-second libFuzzer smoke coverage of clipboard decompression, text/HTML parsing, socket headers, and protocol type dispatch.
5. Android release assembly, unit tests, lint, Gradle wrapper validation, and the pinned wrapper checksum.
6. CodeQL `security-extended` analysis for C/C++ and Java/Kotlin.
7. OpenSSF Scorecard analysis and GitHub secret scanning with push protection.
8. Release repository hygiene, privacy tests, SBOM, provenance, and SHA-256 checksums.

No failed or skipped security gate may be waived silently. A waiver must name
the failed control, owner, compensating control, expiration date, and follow-up
issue in the release notes.

## Manual production gates

- Use a protected `main` branch with required status checks and resolved review conversations.
- Review changes to protocol, cryptography, secret handling, CI workflows, packaging, and privileged Android components separately from feature approval.
- Sign Linux release metadata and the Android APK with protected release keys; verify signatures before publication. Unsigned Android artifacts are test artifacts only.
- Run `INPUTFLOW_ANDROID_APK=/path/to/signed.apk scripts/production-release-gate.sh`; the production wrapper fails unless `apksigner` verifies the supplied APK.
- Run an outage/recovery soak and a real two-host interoperability test for every networking release.
- Resolve all critical/high CodeQL, dependency, and security-advisory findings before release.
- Obtain independent security review before claiming safety outside the trusted-LAN/private-VPN boundary.

## Incident response and maintenance

Private reports use GitHub Security Advisories as described in `SECURITY.md`.
Critical fixes receive a regression test whenever reproducible. Dependency and
GitHub Actions updates are proposed weekly. Threat-model and release-gate changes
are reviewed whenever a new network listener, privileged backend, data type, or
external build dependency is introduced.

## Production claim

Passing this standard supports the claim “production-ready for trusted LANs and
private VPNs.” It does not support “safe for arbitrary hostile networks” or
“formally verified.” Those claims require a mutually authenticated modern
transport, external penetration testing, and release-key operational controls
beyond this repository.
