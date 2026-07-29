// Builds every InputFlow master SVG from one device-to-device flow definition.
const fs = require('fs');
const path = require('path');
const { wrap, flowGlyph, gradientDefs } = require('./flow-mark');

const OUT = path.join(__dirname, 'masters');
fs.mkdirSync(OUT, { recursive: true });

function badge() {
  return { cx: 780, cy: 768, r: 146 };
}

// Brand logo: a restrained gradient tile and a single white flow glyph.
const logo = wrap(`${gradientDefs}
  <rect x="0" y="0" width="1024" height="1024" rx="224" fill="url(#inputflow-bg)"/>
  ${flowGlyph('#FFFFFF')}`);
fs.writeFileSync(path.join(OUT, 'inputflow-logo.svg'), logo);

// Transparent foreground for Android adaptive and in-app use.
fs.writeFileSync(
  path.join(OUT, 'inputflow-foreground.svg'),
  wrap(flowGlyph('#FFFFFF')),
);

function trayBase(ink) {
  return `
  <rect x="72" y="72" width="880" height="880" rx="248" fill="${ink}"/>
  <g transform="translate(128,128) scale(0.75)">${flowGlyph('#FFFFFF')}</g>`;
}

fs.writeFileSync(
  path.join(OUT, 'inputflow-tray.svg'),
  wrap(trayBase('#0F766E')),
);

function trayVariant(name, symbolInner, ink) {
  const b = badge();
  const inner = `
  ${trayBase(ink)}
  <circle cx="${b.cx}" cy="${b.cy}" r="${b.r}" fill="#FFFFFF"/>
  <g fill="${ink}">${symbolInner(b)}</g>`;
  fs.writeFileSync(
    path.join(OUT, `inputflow-tray-${name}.svg`),
    wrap(inner),
  );
}

trayVariant('attention', b => `
  <path d="M ${b.cx - 20} ${b.cy - 64} h 40 l -8 84 h -24 Z"/>
  <circle cx="${b.cx}" cy="${b.cy + 48}" r="22"/>`, '#DC2626');

trayVariant('busy', b => `
  <rect x="${b.cx - 46}" y="${b.cy - 52}" width="32" height="104" rx="12"/>
  <rect x="${b.cx + 14}" y="${b.cy - 52}" width="32" height="104" rx="12"/>`, '#D97706');

trayVariant('offline', b => `
  <path d="M ${b.cx - 66} ${b.cy - 44} l 26 -26 l 110 110 l -26 26 Z"/>`, '#64748B');

// Android tints notification artwork, so keep it a single-color transparent mark.
fs.writeFileSync(
  path.join(OUT, 'inputflow-notification.svg'),
  wrap(flowGlyph('#FFFFFF')),
);

console.log('masters written:', fs.readdirSync(OUT).join(', '));
