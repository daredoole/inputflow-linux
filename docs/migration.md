# Migration Guide

InputFlow is a native Linux peer for PowerToys Mouse Without Borders (MWB). It is built to interoperate with the Windows PowerToys MWB implementation; it is not a generic Synergy-family protocol clone and should not be expected to join Barrier, Synergy, Input Leap, Deskflow, or Cursr groups directly.

Use this guide when moving from another keyboard/mouse sharing setup, from Wine-based MWB experiments, or from an older InputFlow configuration.

## Mental Model

PowerToys MWB and InputFlow use a peer model. Each machine has a name, address, shared security key, and layout position. There is no permanent "server" machine that owns every other client in the way Synergy-family tools commonly describe the topology.

In practice:

- The Windows machine runs PowerToys Mouse Without Borders.
- The Linux machine runs InputFlow.
- Both sides must agree on the shared key, peer names, peer addresses, and layout.
- InputFlow can export a Windows helper script that seeds the Linux peer into PowerToys MWB settings.

For the guided pairing flow, see [Public Beta Workflow](beta-workflow.md#guided-pairing-and-export-helper).

## Legacy Term Map

| Legacy term | InputFlow / PowerToys MWB concept |
| --- | --- |
| Server | A peer that currently owns the local pointer and sends input to another peer. This role is situational, not a fixed machine type. |
| Client | A peer receiving remote input. This role is also situational. |
| Screen | A machine entry in the current MWB layout. Display-level topology is a separate opt-in preview contract. |
| Screen name | `machine_name` / MWB peer name. Names must match what the other peer expects. |
| Configuration file | `~/.config/mwb-client/config.ini` for InputFlow; PowerToys MWB settings on Windows. |
| Shared secret / password | MWB security key. InputFlow can read it from an inline `key`, `key_file`, or Secret Service `key_secret_id`. |
| Edge transition | MWB layout adjacency between peers. |
| Clipboard sharing | InputFlow clipboard sync using local helper tools and MWB clipboard transport. |
| Barrier / Synergy protocol | Not applicable. InputFlow targets PowerToys MWB compatibility instead. |

This map is only a vocabulary bridge. It does not claim that the named projects are maintained, unmaintained, compatible, or incompatible beyond the protocol distinction above.

## Migrating From Barrier, Synergy, Input Leap, Deskflow, Or Cursr

Do not reuse old server/client topology files as-is. Convert the intent instead:

1. Pick the Windows PowerToys MWB machine and Linux InputFlow machine names.
2. Choose or copy the MWB security key into one InputFlow authentication source.
3. Set the Windows host IP and InputFlow local machine name in `config.ini` or the desktop UI.
4. Export the Windows pairing helper from InputFlow and run it on Windows.
5. Verify the peer layout in PowerToys MWB before starting regular use.

Common differences to expect:

- InputFlow joins a PowerToys MWB peer group, not a Synergy-family server process.
- Layout is expressed through MWB peer placement, not a separate Synergy-style screen graph.
- Input injection on Linux depends on `/dev/uinput`; Wayland may also require compositor policy or user approval.
- Clipboard support depends on installed local helpers. Install `wl-clipboard` for Wayland or `xclip`/`xsel` for X11 when needed.

## Migrating From Wine Or Windows MWB Attempts

Running Windows PowerToys MWB through Wine is not the intended Linux path. InputFlow replaces that approach with a native Linux peer that talks to PowerToys MWB on Windows.

Before migrating:

- Stop any Wine-hosted MWB process so it does not compete for ports or peer names.
- Keep the PowerToys MWB security key if you want the same trusted group.
- Recreate the Linux peer through InputFlow export rather than copying Wine registry or settings files.
- Expect Linux input delivery to use `/dev/uinput`, not Windows input APIs.

If a previous Wine setup used the same Linux machine name, remove stale duplicate peer entries from PowerToys MWB or overwrite them with the exported helper.

## Authentication Sources

Configure exactly one practical key source for normal use:

- `key=` stores the MWB security key inline in `config.ini`; simple but easiest to expose.
- `key_file=` reads the key from a local file; preferable for scripts and backups.
- `key_secret_id=` reads from Secret Service through the desktop session keyring; preferable when a session bus and keyring are available.

Avoid publishing configs, helper scripts, logs, or screenshots that expose keys, peer IPs, or Secret Service identifiers.

## Topology Roadmap

InputFlow currently focuses on MWB-compatible machine placement. The topology preview adds a cleaner machine/display split, explicit wrap policies, AAB/BAA/ABA layouts, stacked layouts, asymmetric layouts, and dry-run path previews so users can inspect pointer transitions before applying them.

Until the runtime topology feature gate is enabled and validated for your setup, treat topology as machine-level MWB placement and verify changes in PowerToys MWB after exporting. If you are testing the layout wizard or runtime topology branch, use the [Topology Config Contract](topology.md) and keep dry-run enabled until validation and preview output match the intended handoff behavior.
