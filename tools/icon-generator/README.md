# InputFlow icon set

App mark: two device outlines connected by one directional flow path. The sparse
geometry stays recognizable from the full launcher icon down to a 16 px tray slot.
The brand tile uses a cyan-to-indigo gradient; monochrome tray, themed-icon, and
notification variants come from the same `flow-mark.js` source.

## What's here

### `assets/icons/` — source SVG masters (commit these)
| File | Use |
|---|---|
| `inputflow-logo.svg` | 1024² brand icon (gradient tile + flow mark) |
| `inputflow-logo-square.svg` | full-bleed variant (Play Store / adaptive bg source) |
| `inputflow-foreground.svg` | flow mark only, transparent (adaptive foreground / in-app) |
| `inputflow-glyph.svg` | one-color flow mark (themed-icon source) |
| `inputflow-tray{,-attention,-busy,-offline}.svg` | Linux tray, 4 states |
| `inputflow-notification.svg` | white silhouette for Android notifications |

### `linux/hicolor/` — installed icon theme tree
- `scalable/apps/inputflow.svg` + raster `16,24,32,48,64,128,256,512`px under `<size>/apps/inputflow.png`. Already wired in your `.desktop` / rpm.
- Tray in the **status** context: `scalable/status/inputflow-tray*.svg` + raster `16,22,24,32,48`px under `<size>/status/`.
- The legacy `mwb-*` set is intentionally dropped — code only references `inputflow-*`.

Install example:
```
cp -r linux/hicolor/* /usr/share/icons/hicolor/
gtk-update-icon-cache /usr/share/icons/hicolor
```

### `android/app/src/main/res/`
- **Adaptive launcher** (`mipmap-anydpi-v26/ic_launcher.xml` + `_round`): vector
  `ic_launcher_background` + `ic_launcher_foreground`, art kept inside the 66dp
  safe zone of the 108dp canvas, plus an `ic_launcher_monochrome` layer for
  Android 13+ themed icons.
- **Raster fallback** `mipmap-{mdpi..xxxhdpi}/ic_launcher.png` (+`_round`) at
  48/72/96/144/192.
- **Notification** `drawable-{mdpi..xxxhdpi}/ic_notification.png` — white-only at
  24/36/48/72/96; Android applies its own tint.
- **In-app** `drawable/ic_inputflow.xml` (vector).
- **Play Store** `play-store-icon-512.png` (512², full-bleed).
- Wire-up lives in `AndroidManifest.snippet.xml` (`android:icon` + `android:roundIcon`).

## Regenerate
```
npm ci                         # + ImageMagick for the round crop
node build-masters.js     # rebuild the 6 master SVGs
node build.js             # rebuild every Linux + Android output
```

## Tweaking
Colors and geometry live in `flow-mark.js`. Change them once there and re-run the
two build steps.
