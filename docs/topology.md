# Topology Files

InputFlow topology files describe machines, their individual displays, explicit border links, and optional wrap fallback. The runtime consumes these files only when `topology_enabled=true` and `topology_file=...` are set in `config.ini`.

This is additive to the beta flow. If topology is disabled, missing, or invalid, InputFlow keeps the existing single-screen runtime behavior.

## Simple Setup

Use the tray/controller unless you have a weird layout:

1. Open **InputFlow Controller**.
2. Click **Topology/Layout Wizard**.
3. Pick the layout that matches your desk:
   - **Linux left, Windows right**: one Linux display beside Windows.
   - **Linux above Windows**: one display stacked above the other.
   - **Two Linux displays, then Windows**: Linux | Linux | Windows.
   - **Windows, then two Linux displays**: Windows | Linux | Linux.
   - **Linux split around Windows**: Linux | Windows | Linux.
   - **Advanced/manual topology**: asymmetric or unusual layouts.
4. Confirm the preview.
5. Click **Explain Current Topology** and confirm the English explanation matches your desk.
6. Restart the InputFlow service when prompted.

CLI equivalent:

```bash
mwb_client topology explain --config ~/.config/mwb-client/config.ini
```

## PowerToys Layout Interaction

Windows PowerToys Mouse Without Borders still owns the Windows-side machine layout. InputFlow topology does not rewrite PowerToys `settings.json` or per-display geometry on Windows.

Keep both sides consistent:

- Use **export-windows-pair** or the Windows helper to place the Linux machine next to Windows at the MWB machine level.
- Use InputFlow topology to describe the Linux-side displays and the exact Linux edge that returns control to Windows.
- If PowerToys says Linux is left of Windows, do not make the Linux topology return to Windows from an unrelated edge.
- If these disagree, cursor movement will feel wrong because Windows and Linux will be making different assumptions.

Mental model: PowerToys decides **which machines are neighbors**. InputFlow topology decides **which Linux display edge performs the handoff**.

## Format

Topology files are line-based `key=value` files:

| Key | Format |
| --- | --- |
| `wrap` | `none`, `horizontal`, `vertical`, or `both` |
| `machine` | `MACHINE_ID` |
| `display` | `DISPLAY_ID,MACHINE_ID,X,Y,WIDTH,HEIGHT` |
| `link` | `SOURCE_DISPLAY,EXIT_EDGE,TARGET_DISPLAY,ENTRY_EDGE` |

Edges are `left`, `right`, `up`, or `down`. Explicit links win over wrap fallback. Display coordinates are per-machine logical geometry, not physical millimeters.

The layout wizard writes this format after a dry-run preview. Manual files should stay in `~/.config/mwb-client/*.topology`.

## Examples

### AAB

```ini
# topology-example: aab
wrap=none
machine=A
machine=B
display=A1,A,0,0,1920,1080
display=A2,A,1920,0,1920,1080
display=B1,B,3840,0,1920,1080
link=A1,right,A2,left
link=A2,left,A1,right
link=A2,right,B1,left
link=B1,left,A2,right
```

### BAA

```ini
# topology-example: baa
wrap=none
machine=A
machine=B
display=B1,B,0,0,1920,1080
display=A1,A,1920,0,1920,1080
display=A2,A,3840,0,1920,1080
link=B1,right,A1,left
link=A1,left,B1,right
link=A1,right,A2,left
link=A2,left,A1,right
```

### ABA

```ini
# topology-example: aba
wrap=none
machine=A
machine=B
display=A1,A,0,0,1920,1080
display=B1,B,1920,0,1920,1080
display=A2,A,3840,0,1920,1080
link=A1,right,B1,left
link=B1,left,A1,right
link=B1,right,A2,left
link=A2,left,B1,right
```

### Stacked

```ini
# topology-example: stacked
wrap=none
machine=A
machine=B
display=A1,A,0,0,1920,1080
display=B1,B,0,1080,1920,1080
link=A1,down,B1,up
link=B1,up,A1,down
```

### Asymmetric

```ini
# topology-example: asymmetric
wrap=none
machine=A
machine=B
display=A1,A,0,0,3840,2160
display=B1,B,3840,540,1920,1080
link=A1,right,B1,left
link=B1,left,A1,right
```

### Horizontal Wrap

```ini
# topology-example: wrap-horizontal
wrap=horizontal
machine=A
machine=B
display=A1,A,0,0,1920,1080
display=A2,A,1920,0,1920,1080
display=B1,B,3840,0,1920,1080
link=A2,right,B1,left
link=B1,left,A2,right
```

## Runtime Contract

`topology_enabled=false` is the default. Enabling topology loads and validates the topology file during startup. Invalid topology logs a warning and falls back to the existing behavior instead of blocking startup.

The current runtime uses topology to resolve edge transitions before local mouse injection. Same-machine transitions stay local. Cross-machine transitions send a mapped MWB mouse move back to the active peer and suppress the local edge move, so the pointer can return to the Windows side on configured borders.

## Troubleshooting

Use the diagnostics bundle when reporting topology bugs. It records `topology_enabled`, `topology_file`, load/validation status, display geometry, session type, and recent runtime logs.

If movement is unexpected, set `wrap=none` and add explicit `link=` lines for each intended transition. If the wizard output is wrong, edit the `.topology` file directly and restart the user service.
