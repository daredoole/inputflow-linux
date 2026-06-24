# Security Policy

InputFlow speaks the legacy Mouse Without Borders protocol used by Microsoft PowerToys. That protocol is encrypted, but it was not designed with modern authenticated transport guarantees.

## Supported branch

Security fixes should target the current `main` branch.

## Reporting a vulnerability

If you discover a vulnerability that could expose input, clipboard data, or pairing secrets:

1. Do not open a public issue with exploit details.
2. Send a private report to the maintainers with:
   - affected commit or release
   - reproduction steps
   - impact assessment
   - logs or packet traces with keys and hostnames removed

If no private contact channel is published yet, open a minimal public issue that only asks for a secure disclosure path and avoid technical detail.

## Security guidance for users

- Prefer `key_file=` or `key_secret_id=` over storing keys inline in shell history.
- Treat the shared MWB security key as sensitive. Anyone with the key and network reachability may be able to impersonate a peer.
- Keep the Linux listener bound behind a local firewall and only allow the intended Windows peer.
- Review exported pairing helpers before moving them to another machine. They may contain the shared key and the Linux host IP.
- Avoid posting raw `mwb-windows-report-*`, `mwb-lock-report-*`, or `mwb-socket-trace-*` files publicly without redacting hostnames, IPs, and keys.

### Android relay and native input injection

- The Android relay's `android_relay_secret` gates input delivery to the phone.
  When a **Shizuku** or **root** injection backend is enabled, an authenticated
  peer can inject input at the system level (including secure fields), so a weak
  secret is a privilege-escalation risk.
- The relay refuses to start, and `android-pair` refuses to emit a pairing URI,
  with a weak secret (`< 16` chars / low variety). Generate one with
  `android-pair --generate`.
- Keep the relay on a trusted LAN/VPN and treat the pairing URI/QR as a secret.

## Current protocol limitations

- The upstream PowerToys protocol uses AES-256-CBC framing but does not provide a modern end-to-end authenticated channel.
- Full AEAD or MAC-based integrity would require a protocol change on both the Linux client and the PowerToys side.
- Public beta users should treat InputFlow as a trusted-LAN tool, not an internet-exposed remote-control service.
