// Builds the InputFlow master SVGs from a single shared donkey definition,
// so the colored logo and the monochrome tray/notification glyphs stay identical.
const fs = require('fs');
const path = require('path');
const { wrap, donkeyColor, donkeyMono, cursor, gradientDefs } = require('./donkey');

const OUT = path.join(__dirname, 'masters');
fs.mkdirSync(OUT, { recursive: true });

// corner status badge: a solid disc with a symbol knocked out (evenodd), plus a
// transparent gap ring so it separates from the donkey under any tint.
function badge(symbolPath) {
  const cx = 768, cy = 760, r = 150; // bottom-right
  return { cx, cy, r, symbolPath };
}

// 1) BRAND LOGO (color, gradient squircle bg + donkey + cursor)
const logo = wrap(`${gradientDefs}
  <rect x="0" y="0" width="1024" height="1024" rx="224" fill="url(#bg)"/>
  ${donkeyColor}
  ${cursor}`);
fs.writeFileSync(path.join(OUT,'inputflow-logo.svg'), logo);

// foreground-only donkey (transparent) for Android adaptive + in-app, re-centred
const fg = wrap(`${donkeyColor}`);
fs.writeFileSync(path.join(OUT,'inputflow-foreground.svg'), fg);

// 2) TRAY default (mono)
fs.writeFileSync(path.join(OUT,'inputflow-tray.svg'), wrap(donkeyMono('m0','#2E3440')));

// tray variants: donkey + corner badge. badge drawn as disc with knocked-out symbol,
// and we punch a transparent ring out of the donkey so the badge reads cleanly.
function trayVariant(name, symbolInner, ink='#2E3440') {
  const b = badge();
  const gapRing = `
      <circle cx="${b.cx}" cy="${b.cy}" r="${b.r+34}"/>`;
  const inner = `
  ${donkeyMono('dk', ink, gapRing)}
  <g fill="${ink}" fill-rule="evenodd">${symbolInner(b)}</g>`;
  fs.writeFileSync(path.join(OUT,`inputflow-tray-${name}.svg`), wrap(inner));
}

// attention: disc with "!" knocked out
trayVariant('attention', b => `
  <path fill-rule="evenodd" d="
    M ${b.cx} ${b.cy-b.r} a ${b.r} ${b.r} 0 1 0 0.01 0 Z
    M ${b.cx-20} ${b.cy-64} h 40 l -8 84 h -24 Z
    M ${b.cx-22} ${b.cy+48} a 22 22 0 1 0 44 0 a 22 22 0 1 0 -44 0 Z"/>`);

// busy: disc with two pause-bars knocked out
trayVariant('busy', b => `
  <path fill-rule="evenodd" d="
    M ${b.cx} ${b.cy-b.r} a ${b.r} ${b.r} 0 1 0 0.01 0 Z
    M ${b.cx-46} ${b.cy-52} h 32 v 104 h -32 Z
    M ${b.cx+14} ${b.cy-52} h 32 v 104 h -32 Z"/>`);

// offline: disc with a diagonal slash knocked out
trayVariant('offline', b => `
  <path fill-rule="evenodd" d="
    M ${b.cx} ${b.cy-b.r} a ${b.r} ${b.r} 0 1 0 0.01 0 Z
    M ${b.cx-66} ${b.cy-44} l 26 -26 l 110 110 l -26 26 Z"/>`);

// 3) NOTIFICATION (white silhouette on transparent, Android tints it)
fs.writeFileSync(path.join(OUT,'inputflow-notification.svg'), wrap(donkeyMono('mn','#FFFFFF')));

console.log('masters written:', fs.readdirSync(OUT).join(', '));
