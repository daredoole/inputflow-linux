// Builds the InputFlow master SVGs from a single shared donkey definition,
// so the colored logo and the monochrome tray/notification glyphs stay identical.
const fs = require('fs');
const path = require('path');
const OUT = path.join(__dirname, 'masters');
fs.mkdirSync(OUT, { recursive: true });

// ---- shared donkey geometry (authored in a 1024 box, centred ~ (512,500)) ----
const D = {
  earL:  't(450,432) r(-20)',
  earR:  't(574,432) r(20)',
};

// colored, multi-part donkey (for the brand logo)
const donkeyColor = `
  <!-- ears -->
  <g transform="translate(450,432) rotate(-20)">
    <rect x="-42" y="-232" width="84" height="252" rx="42" fill="#C7D0E4"/>
    <rect x="-20" y="-206" width="40" height="166" rx="20" fill="#EEF2FA"/>
  </g>
  <g transform="translate(574,432) rotate(20)">
    <rect x="-42" y="-232" width="84" height="252" rx="42" fill="#C7D0E4"/>
    <rect x="-20" y="-206" width="40" height="166" rx="20" fill="#EEF2FA"/>
  </g>
  <!-- forelock -->
  <path d="M474 446 q14 -64 38 -64 q24 0 38 64 q-38 -22 -76 0 Z" fill="#A6B1CB"/>
  <!-- head + muzzle -->
  <ellipse cx="512" cy="556" rx="156" ry="170" fill="#C7D0E4"/>
  <ellipse cx="512" cy="676" rx="110" ry="96" fill="#EEF2FA"/>
  <!-- eyes -->
  <ellipse cx="454" cy="538" rx="22" ry="28" fill="#2E3440"/>
  <ellipse cx="570" cy="538" rx="22" ry="28" fill="#2E3440"/>
  <circle cx="461" cy="528" r="6.5" fill="#FFFFFF"/>
  <circle cx="577" cy="528" r="6.5" fill="#FFFFFF"/>
  <!-- nostrils -->
  <rect x="479" y="668" width="12" height="30" rx="6" fill="#2E3440" transform="rotate(-12 485 683)"/>
  <rect x="533" y="668" width="12" height="30" rx="6" fill="#2E3440" transform="rotate(12 539 683)"/>
`;

// monochrome donkey via a mask (solid ink face with knocked-out eyes / inner-ears /
// nostrils) -> survives single-colour theme tinting on Linux tray + Android notif.
function donkeyMono(id, ink) {
  return `
  <defs>
    <mask id="${id}">
      <rect x="0" y="0" width="1024" height="1024" fill="black"/>
      <!-- white = ink -->
      <g fill="white">
        <g transform="translate(450,432) rotate(-20)"><rect x="-42" y="-232" width="84" height="252" rx="42"/></g>
        <g transform="translate(574,432) rotate(20)"><rect x="-42" y="-232" width="84" height="252" rx="42"/></g>
        <path d="M474 446 q14 -64 38 -64 q24 0 38 64 q-38 -22 -76 0 Z"/>
        <ellipse cx="512" cy="556" rx="156" ry="170"/>
        <ellipse cx="512" cy="676" rx="110" ry="96"/>
      </g>
      <!-- black = holes -->
      <g fill="black">
        <g transform="translate(450,432) rotate(-20)"><rect x="-20" y="-206" width="40" height="150" rx="20"/></g>
        <g transform="translate(574,432) rotate(20)"><rect x="-20" y="-206" width="40" height="150" rx="20"/></g>
        <ellipse cx="454" cy="540" rx="23" ry="29"/>
        <ellipse cx="570" cy="540" rx="23" ry="29"/>
        <rect x="479" y="668" width="12" height="30" rx="6" transform="rotate(-12 485 683)"/>
        <rect x="533" y="668" width="12" height="30" rx="6" transform="rotate(12 539 683)"/>
      </g>
    </mask>
  </defs>
  <rect x="0" y="0" width="1024" height="1024" fill="${ink}" mask="url(#${id})"/>`;
}

// corner status badge: a solid disc with a symbol knocked out (evenodd), plus a
// transparent gap ring so it separates from the donkey under any tint.
function badge(symbolPath) {
  const cx = 768, cy = 760, r = 150; // bottom-right
  return { cx, cy, r, symbolPath };
}

function wrap(inner, extra='') {
  return `<svg width="1024" height="1024" viewBox="0 0 1024 1024" xmlns="http://www.w3.org/2000/svg">${extra}${inner}</svg>\n`;
}

// 1) BRAND LOGO (color, gradient squircle bg + donkey + cursor)
const logo = wrap(`
  <defs>
    <linearGradient id="bg" x1="0" y1="0" x2="1024" y2="1024" gradientUnits="userSpaceOnUse">
      <stop offset="0" stop-color="#22D3EE"/>
      <stop offset="1" stop-color="#6366F1"/>
    </linearGradient>
  </defs>
  <rect x="0" y="0" width="1024" height="1024" rx="224" fill="url(#bg)"/>
  ${donkeyColor}
  <g transform="translate(656,654) rotate(8)">
    <path d="M0 0 L0 86 L22 64 L36 96 L52 88 L38 58 L66 58 Z" fill="#FFFFFF" stroke="#2E3440" stroke-width="6" stroke-linejoin="round"/>
  </g>`);
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
  const inner = `
  <defs>
    <mask id="dk">
      <rect width="1024" height="1024" fill="black"/>
      <g fill="white">
        <g transform="translate(450,432) rotate(-20)"><rect x="-42" y="-232" width="84" height="252" rx="42"/></g>
        <g transform="translate(574,432) rotate(20)"><rect x="-42" y="-232" width="84" height="252" rx="42"/></g>
        <path d="M474 446 q14 -64 38 -64 q24 0 38 64 q-38 -22 -76 0 Z"/>
        <ellipse cx="512" cy="556" rx="156" ry="170"/>
        <ellipse cx="512" cy="676" rx="110" ry="96"/>
      </g>
      <g fill="black">
        <g transform="translate(450,432) rotate(-20)"><rect x="-20" y="-206" width="40" height="150" rx="20"/></g>
        <g transform="translate(574,432) rotate(20)"><rect x="-20" y="-206" width="40" height="150" rx="20"/></g>
        <ellipse cx="454" cy="540" rx="23" ry="29"/>
        <ellipse cx="570" cy="540" rx="23" ry="29"/>
        <rect x="479" y="668" width="12" height="30" rx="6" transform="rotate(-12 485 683)"/>
        <rect x="533" y="668" width="12" height="30" rx="6" transform="rotate(12 539 683)"/>
        <!-- gap ring so the badge separates -->
        <circle cx="${b.cx}" cy="${b.cy}" r="${b.r+34}"/>
      </g>
    </mask>
  </defs>
  <rect width="1024" height="1024" fill="${ink}" mask="url(#dk)"/>
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
