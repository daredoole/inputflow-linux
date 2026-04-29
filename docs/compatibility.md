# Compatibility Guide

InputFlow is a native Linux peer for Microsoft PowerToys Mouse Without Borders (MWB). It targets PowerToys MWB interoperability with Linux input, clipboard, service, and configuration integration. It is not a Barrier, Synergy, Input Leap, Deskflow, or Cursr protocol implementation.

## Compatibility Matrix

| Area | Status | Notes |
| --- | --- | --- |
| Linux on X11 | Supported beta path | Input delivery uses `/dev/uinput`. Clipboard sync needs `xclip` or `xsel` when clipboard features are enabled. |
| Linux on Wayland | Supported with caveats | Input delivery still needs writable `/dev/uinput`. Some compositors may apply extra policy, prompts, or restrictions for synthetic input. Clipboard sync needs `wl-clipboard`, and polling may be needed in some sessions. |
| `/dev/uinput` | Required for input injection | Load the `uinput` module and grant the user access, usually through the packaged `inputflow` group and udev rule. |
| PowerToys MWB on Windows | Target peer | Pair InputFlow with the PowerToys Mouse Without Borders feature on Windows. Exported helpers seed `MachinePool`, `MachineMatrixString`, `Name2IP`, peer name, address, layout, and key material. |
| Barrier / Synergy / Input Leap / Deskflow | Not protocol compatible | Use the migration guide to translate concepts, not configuration files. |
| Cursr | Not protocol compatible | InputFlow does not join Cursr groups. Use MWB peer placement instead. |
| Authentication: Secret Service | Supported when available | Use `key_secret_id=` when a session bus and keyring are available. Headless sessions may not provide this. |
| Authentication: `key_file` | Supported | Good default for service usage when file permissions are managed carefully. |
| Authentication: inline `key` | Supported | Useful for quick setup, but avoid sharing configs because the key is stored directly. |
| Clipboard receive/send | Supported beta path | Requires local helpers: `wl-clipboard` on Wayland or `xclip`/`xsel` on X11. Availability is reported by `doctor`. |
| systemd user service | Opt-in | Packaging includes a user unit, but users should enable/start it only after validating config, key source, and `/dev/uinput` access. |
| Network trust model | Trusted LAN/subnet | Use on a trusted local network. Do not expose MWB ports to untrusted networks or the public internet. |
| Display-level topology config | Opt-in preview | The contract is documented in [Topology Config Contract](topology.md), but the default runtime remains MWB-compatible machine placement while handoff behavior matures. |

## Linux Session Details

X11 is the simpler path because clipboard helpers and desktop automation policy are more predictable. Wayland can work, but compositor policy matters: even with `/dev/uinput` access, the compositor or desktop environment may restrict, gate, or prompt around synthetic input behavior.

Run the health check after setup:

```bash
./build/mwb_client doctor --config ~/.config/mwb-client/config.ini
```

Review warnings for session type, `/dev/uinput`, group membership, clipboard helpers, Secret Service availability, and authentication conflicts.

## Windows PowerToys MWB

InputFlow is intended to pair with the Windows PowerToys MWB implementation. Use the exported Windows helper when possible because it writes the MWB settings that are easy to mistype by hand: peer name, address, shared key, `MachinePool`, `MachineMatrixString`, and `Name2IP`.

InputFlow is independent and not affiliated with Microsoft. Compatibility is based on the open-source PowerToys MWB behavior.

## Network Assumptions

InputFlow assumes a trusted local network where peers can reach each other directly by IP or resolvable host name. Keep MWB traffic on a private LAN, VPN, or otherwise trusted subnet.

Avoid:

- Forwarding MWB ports from the internet.
- Pairing across networks you do not control.
- Publishing configs or exported helper scripts that include keys or private addresses.

## Service Expectations

The systemd user service is a convenience, not a required first step. During migration or first setup, run the desktop UI or CLI manually, confirm `doctor` output, and verify Windows pairing. Enable the user service only after those checks pass.

## Topology Expectations

Current compatibility is machine-level MWB placement. Display-level topology is intentionally gated and opt-in so InputFlow can remain compatible with PowerToys MWB while the runtime handoff behavior matures.

The topology contract separates machines from displays and supports configurable wrap policies, AAB/BAA/ABA layouts, stacked layouts, asymmetric layouts, and dry-run path previews. See [Topology Config Contract](topology.md) for the preview file format and validation expectations.
