// Builds the InputFlow master SVGs from a single shared donkey definition,
// so the colored logo and the monochrome tray/notification glyphs stay identical.
const fs = require('fs');
const path = require('path');
const { wrap, donkeyColor, donkeyMono, cursor, gradientDefs } = require('./donkey');

const OUT = path.join(__dirname, 'masters');
fs.mkdirSync(OUT, { recursive: true });

// corner status badge sized for tiny system-tray slots.
function badge() {
  const cx = 768, cy = 760, r = 150; // bottom-right
  return { cx, cy, r };
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

const trayViewBox = '0 0 1024 1024';

function trayBase(ink) {
  return `
  <rect x="72" y="72" width="880" height="880" rx="224" fill="${ink}"/>
  <g transform="translate(0,28)">${donkeyMono('tray-dk', '#FFFFFF')}</g>`;
}

// 2) TRAY default (active/healthy)
fs.writeFileSync(path.join(OUT,'inputflow-tray.svg'),
  wrap(trayBase('#10B981'), '', trayViewBox));

// tray variants: colored tile + white donkey + readable status badge.
function trayVariant(name, symbolInner, ink) {
  const b = badge();
  const inner = `
  ${trayBase(ink)}
  <circle cx="${b.cx}" cy="${b.cy}" r="${b.r}" fill="#FFFFFF"/>
  <g fill="${ink}">${symbolInner(b)}</g>`;
  fs.writeFileSync(path.join(OUT,`inputflow-tray-${name}.svg`), wrap(inner, '', trayViewBox));
}

// attention: "!" inside the white badge
trayVariant('attention', b => `
  <path d="M ${b.cx-20} ${b.cy-64} h 40 l -8 84 h -24 Z"/>
  <circle cx="${b.cx}" cy="${b.cy+48}" r="22"/>`, '#EF4444');

// busy: two pause bars inside the white badge
trayVariant('busy', b => `
  <rect x="${b.cx-46}" y="${b.cy-52}" width="32" height="104" rx="12"/>
  <rect x="${b.cx+14}" y="${b.cy-52}" width="32" height="104" rx="12"/>`, '#F59E0B');

// offline: diagonal slash inside the white badge
trayVariant('offline', b => `
  <path d="M ${b.cx-66} ${b.cy-44} l 26 -26 l 110 110 l -26 26 Z"/>`, '#6B7280');

// 3) NOTIFICATION (white silhouette on transparent, Android tints it)
fs.writeFileSync(path.join(OUT,'inputflow-notification.svg'), wrap(donkeyMono('mn','#FFFFFF')));

console.log('masters written:', fs.readdirSync(OUT).join(', '));
