const fs = require('fs');
const path = require('path');
const cp = require('child_process');
const { Resvg } = require('@resvg/resvg-js');
const s2v = require('svg2vectordrawable');
const Mark = require('./flow-mark');

const ROOT = path.join(__dirname, 'dist', 'inputflow-icons');
const M = path.join(__dirname, 'masters');
const mk = p => (fs.mkdirSync(p, { recursive: true }), p);
const write = (p, c) => (mk(path.dirname(p)), fs.writeFileSync(p, c));
const png = (svg, file, w) => {
  const r = new Resvg(svg, { fitTo: { mode: 'width', value: w }, background: 'rgba(0,0,0,0)' });
  write(file, r.render().asPng());
};
const readM = n => fs.readFileSync(path.join(M, n), 'utf8');

mk(ROOT);

/* ---------------------------------------------------------------- SOURCES */
// canonical source SVGs (committed). copy masters + derive square/silhouette/fg.
const src = path.join(ROOT, 'assets', 'icons');
for (const f of fs.readdirSync(M)) fs.copyFileSync(path.join(M, f), path.join(mk(src), f));

// square (full-bleed) logo -> Play Store + adaptive raster fallback + bg
const squareLogo = Mark.wrap(`${Mark.gradientDefs}
  <rect width="1024" height="1024" fill="url(#inputflow-bg)"/>${Mark.flowGlyph('#FFFFFF')}`);
write(path.join(src, 'inputflow-logo-square.svg'), squareLogo);
// solid monochrome source for themed icons.
write(path.join(src, 'inputflow-glyph.svg'), Mark.wrap(Mark.flowGlyph('#172033')));

/* ----------------------------------------------------------------- LINUX  */
const hicolor = path.join(ROOT, 'linux', 'hicolor');
// scalable app icon
fs.copyFileSync(path.join(M, 'inputflow-logo.svg'),
  mk(path.join(hicolor, 'scalable', 'apps')) + '/inputflow.svg');
// rastered app icons
for (const sz of [16, 24, 32, 48, 64, 128, 256, 512])
  png(readM('inputflow-logo.svg'), path.join(hicolor, `${sz}x${sz}`, 'apps', 'inputflow.png'), sz);

// tray: scalable + raster, in the status context
const trays = ['', '-attention', '-busy', '-offline'];
for (const v of trays) {
  const name = `inputflow-tray${v}`;
  fs.copyFileSync(path.join(M, `${name}.svg`),
    mk(path.join(hicolor, 'scalable', 'status')) + `/${name}.svg`);
  for (const sz of [16, 22, 24, 32, 48])
    png(readM(`${name}.svg`), path.join(hicolor, `${sz}x${sz}`, 'status', `${name}.png`), sz);
}

/* --------------------------------------------------------------- ANDROID  */
const res = path.join(ROOT, 'android', 'app', 'src', 'main', 'res');
const dpi = { mdpi: 1, hdpi: 1.5, xhdpi: 2, xxhdpi: 3, xxxhdpi: 4 };

// --- adaptive icon: vector background + foreground + monochrome ---
async function toVD(svg, opts = {}) { return (await s2v(svg, opts)).replace(/\n\s*\n/g, '\n'); }

(async () => {
  // background: full-bleed gradient, 108 viewport
  const bgSvg = `<svg width="108" height="108" viewBox="0 0 108 108" xmlns="http://www.w3.org/2000/svg">
    <defs><linearGradient id="g" x1="0" y1="0" x2="108" y2="108" gradientUnits="userSpaceOnUse">
      <stop offset="0" stop-color="#22D3EE"/><stop offset="1" stop-color="#6366F1"/></linearGradient></defs>
    <rect width="108" height="108" fill="url(#g)"/></svg>`;
  write(path.join(res, 'drawable', 'ic_launcher_background.xml'), await toVD(bgSvg, { floatPrecision: 2 }));

  // Foreground: compact flow mark centered in the adaptive-icon safe zone.
  const fgSvg = `<svg width="108" height="108" viewBox="0 0 1024 1024" xmlns="http://www.w3.org/2000/svg">
    ${Mark.flowGlyph('#FFFFFF')}</svg>`;
  write(path.join(res, 'drawable', 'ic_launcher_foreground.xml'), await toVD(fgSvg, { floatPrecision: 2 }));

  // Monochrome layer for Android 13+ themed icons.
  const monoSvg = `<svg width="108" height="108" viewBox="0 0 1024 1024" xmlns="http://www.w3.org/2000/svg">
    ${Mark.flowGlyph('#000000')}</svg>`;
  write(path.join(res, 'drawable', 'ic_launcher_monochrome.xml'), await toVD(monoSvg, { floatPrecision: 2 }));

  // In-app vector (brand-colored mark, no background).
  write(path.join(res, 'drawable', 'ic_inputflow.xml'),
    await toVD(`<svg width="1024" height="1024" viewBox="0 0 1024 1024" xmlns="http://www.w3.org/2000/svg">${Mark.flowGlyph('#4F46E5')}</svg>`,
      { floatPrecision: 2 }));

  // adaptive icon descriptors
  const adaptive = `<?xml version="1.0" encoding="utf-8"?>
<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">
    <background android:drawable="@drawable/ic_launcher_background"/>
    <foreground android:drawable="@drawable/ic_launcher_foreground"/>
    <monochrome android:drawable="@drawable/ic_launcher_monochrome"/>
</adaptive-icon>
`;
  write(path.join(res, 'mipmap-anydpi-v26', 'ic_launcher.xml'), adaptive);
  write(path.join(res, 'mipmap-anydpi-v26', 'ic_launcher_round.xml'), adaptive);

  /* --- raster fallbacks (pre-API26 launchers) --- */
  for (const [d, scale] of Object.entries(dpi)) {
    const s = Math.round(48 * scale); // 48,72,96,144,192
    const sq = path.join(res, `mipmap-${d}`, 'ic_launcher.png');
    png(squareLogo, sq, s);
    // round = circular crop of the square logo
    const rd = path.join(res, `mipmap-${d}`, 'ic_launcher_round.png');
    png(squareLogo, '/tmp/_sq.png', s);
    cp.execSync(`convert /tmp/_sq.png \\( +clone -alpha extract -fill black -colorize 100 ` +
      `-fill white -draw "circle ${ (s-1)/2 },${ (s-1)/2 } ${ (s-1)/2 },0" \\) ` +
      `-alpha off -compose CopyOpacity -composite "${rd}"`);
  }

  // notification: white silhouette PNGs (24/36/48/72/96), Android tints them
  const notif = readM('inputflow-notification.svg');
  for (const [d, scale] of Object.entries(dpi)) {
    const s = Math.round(24 * scale); // 24,36,48,72,96
    png(notif, path.join(res, `drawable-${d}`, 'ic_notification.png'), s);
  }

  // Play Store icon (full-bleed 512)
  png(squareLogo, path.join(ROOT, 'android', 'play-store-icon-512.png'), 512);

  /* --- manifest snippet --- */
  write(path.join(ROOT, 'android', 'AndroidManifest.snippet.xml'),
`<!-- merge into <application ...> in AndroidManifest.xml -->
<application
    android:icon="@mipmap/ic_launcher"
    android:roundIcon="@mipmap/ic_launcher_round"
    ... >

    <!-- default notification icon (white silhouette, system-tinted) -->
    <meta-data
        android:name="com.google.firebase.messaging.default_notification_icon"
        android:resource="@drawable/ic_notification" />
</application>
`);

  console.log('android vector + raster set written');
  buildReadmeAndZip();
})();

/* ---------------------------------------------------------------- README  */
function buildReadmeAndZip() {
  write(path.join(ROOT, 'README.md'), README);
  // contact sheet for quick visual QA
  cp.execSync(`cd "${__dirname}" && node render.js masters/inputflow-logo.svg /tmp/cs_logo.png 256`);
  console.log('done');
}

const README = `# InputFlow icon set

App mark: two device outlines connected by one directional flow path. The sparse
geometry stays recognizable from the full launcher icon down to a 16 px tray slot.
The brand tile uses a cyan-to-indigo gradient; monochrome tray, themed-icon, and
notification variants come from the same \`flow-mark.js\` source.

## What's here

### \`assets/icons/\` — source SVG masters (commit these)
| File | Use |
|---|---|
| \`inputflow-logo.svg\` | 1024² brand icon (gradient tile + flow mark) |
| \`inputflow-logo-square.svg\` | full-bleed variant (Play Store / adaptive bg source) |
| \`inputflow-foreground.svg\` | flow mark only, transparent (adaptive foreground / in-app) |
| \`inputflow-glyph.svg\` | one-color flow mark (themed-icon source) |
| \`inputflow-tray{,-attention,-busy,-offline}.svg\` | Linux tray, 4 states |
| \`inputflow-notification.svg\` | white silhouette for Android notifications |

### \`linux/hicolor/\` — installed icon theme tree
- \`scalable/apps/inputflow.svg\` + raster \`16,24,32,48,64,128,256,512\`px under \`<size>/apps/inputflow.png\`. Already wired in your \`.desktop\` / rpm.
- Tray in the **status** context: \`scalable/status/inputflow-tray*.svg\` + raster \`16,22,24,32,48\`px under \`<size>/status/\`.
- The legacy \`mwb-*\` set is intentionally dropped — code only references \`inputflow-*\`.

Install example:
\`\`\`
cp -r linux/hicolor/* /usr/share/icons/hicolor/
gtk-update-icon-cache /usr/share/icons/hicolor
\`\`\`

### \`android/app/src/main/res/\`
- **Adaptive launcher** (\`mipmap-anydpi-v26/ic_launcher.xml\` + \`_round\`): vector
  \`ic_launcher_background\` + \`ic_launcher_foreground\`, art kept inside the 66dp
  safe zone of the 108dp canvas, plus an \`ic_launcher_monochrome\` layer for
  Android 13+ themed icons.
- **Raster fallback** \`mipmap-{mdpi..xxxhdpi}/ic_launcher.png\` (+\`_round\`) at
  48/72/96/144/192.
- **Notification** \`drawable-{mdpi..xxxhdpi}/ic_notification.png\` — white-only at
  24/36/48/72/96; Android applies its own tint.
- **In-app** \`drawable/ic_inputflow.xml\` (vector).
- **Play Store** \`play-store-icon-512.png\` (512², full-bleed).
- Wire-up lives in \`AndroidManifest.snippet.xml\` (\`android:icon\` + \`android:roundIcon\`).

## Regenerate
\`\`\`
npm ci                         # + ImageMagick for the round crop
node build-masters.js     # rebuild the 6 master SVGs
node build.js             # rebuild every Linux + Android output
\`\`\`

## Tweaking
Colors and geometry live in \`flow-mark.js\`. Change them once there and re-run the
two build steps.
`;
