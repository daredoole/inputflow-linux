// Single source of truth for the InputFlow donkey art.
const W = 1024;
const wrap = (inner, extra='', vb=`0 0 ${W} ${W}`, w=W, h=W) =>
  `<svg width="${w}" height="${h}" viewBox="${vb}" xmlns="http://www.w3.org/2000/svg">${extra}${inner}</svg>\n`;

const earsColor = `
  <g transform="translate(450,432) rotate(-20)">
    <rect x="-42" y="-232" width="84" height="252" rx="42" fill="#C7D0E4"/>
    <rect x="-20" y="-206" width="40" height="166" rx="20" fill="#EEF2FA"/>
  </g>
  <g transform="translate(574,432) rotate(20)">
    <rect x="-42" y="-232" width="84" height="252" rx="42" fill="#C7D0E4"/>
    <rect x="-20" y="-206" width="40" height="166" rx="20" fill="#EEF2FA"/>
  </g>`;

const donkeyColor = `${earsColor}
  <path d="M474 446 q14 -64 38 -64 q24 0 38 64 q-38 -22 -76 0 Z" fill="#A6B1CB"/>
  <ellipse cx="512" cy="556" rx="156" ry="170" fill="#C7D0E4"/>
  <ellipse cx="512" cy="676" rx="110" ry="96" fill="#EEF2FA"/>
  <ellipse cx="454" cy="538" rx="22" ry="28" fill="#2E3440"/>
  <ellipse cx="570" cy="538" rx="22" ry="28" fill="#2E3440"/>
  <circle cx="461" cy="528" r="6.5" fill="#FFFFFF"/>
  <circle cx="577" cy="528" r="6.5" fill="#FFFFFF"/>
  <rect x="479" y="668" width="12" height="30" rx="6" fill="#2E3440" transform="rotate(-12 485 683)"/>
  <rect x="533" y="668" width="12" height="30" rx="6" fill="#2E3440" transform="rotate(12 539 683)"/>`;

// flat single-colour silhouette (body shapes only, painter's model -> no holes).
// Safe to convert to an Android VectorDrawable / themed-icon monochrome layer.
const donkeySilhouette = (ink) => `
  <g fill="${ink}">
    <g transform="translate(450,432) rotate(-20)"><rect x="-42" y="-232" width="84" height="252" rx="42"/></g>
    <g transform="translate(574,432) rotate(20)"><rect x="-42" y="-232" width="84" height="252" rx="42"/></g>
    <path d="M474 446 q14 -64 38 -64 q24 0 38 64 q-38 -22 -76 0 Z"/>
    <ellipse cx="512" cy="556" rx="156" ry="170"/>
    <ellipse cx="512" cy="676" rx="110" ry="96"/>
  </g>`;

const donkeyInkShapes = `
      <g transform="translate(450,432) rotate(-20)"><rect x="-42" y="-232" width="84" height="252" rx="42"/></g>
      <g transform="translate(574,432) rotate(20)"><rect x="-42" y="-232" width="84" height="252" rx="42"/></g>
      <path d="M474 446 q14 -64 38 -64 q24 0 38 64 q-38 -22 -76 0 Z"/>
      <ellipse cx="512" cy="556" rx="156" ry="170"/>
      <ellipse cx="512" cy="676" rx="110" ry="96"/>`;

const donkeyKnockoutShapes = `
      <g transform="translate(450,432) rotate(-20)"><rect x="-20" y="-206" width="40" height="150" rx="20"/></g>
      <g transform="translate(574,432) rotate(20)"><rect x="-20" y="-206" width="40" height="150" rx="20"/></g>
      <ellipse cx="454" cy="540" rx="23" ry="29"/>
      <ellipse cx="570" cy="540" rx="23" ry="29"/>
      <rect x="479" y="668" width="12" height="30" rx="6" transform="rotate(-12 485 683)"/>
      <rect x="533" y="668" width="12" height="30" rx="6" transform="rotate(12 539 683)"/>`;

const donkeyMask = (id, extraKnockouts='') => `
  <defs><mask id="${id}">
    <rect width="1024" height="1024" fill="black"/>
    <g fill="white">${donkeyInkShapes}
    </g>
    <g fill="black">${donkeyKnockoutShapes}${extraKnockouts}
    </g>
  </mask></defs>
`;

// masked monochrome: solid face with knocked-out eyes / inner-ears / nostrils.
// Best for raster tray + notification PNGs (survives theme tinting).
const donkeyMono = (id, ink, extraKnockouts='') => `${donkeyMask(id, extraKnockouts)}
  <rect width="1024" height="1024" fill="${ink}" mask="url(#${id})"/>`;

const cursor = `<g transform="translate(656,654) rotate(8)">
    <path d="M0 0 L0 86 L22 64 L36 96 L52 88 L38 58 L66 58 Z" fill="#FFFFFF" stroke="#2E3440" stroke-width="6" stroke-linejoin="round"/>
  </g>`;

const gradientDefs = `<defs><linearGradient id="bg" x1="0" y1="0" x2="1024" y2="1024" gradientUnits="userSpaceOnUse">
    <stop offset="0" stop-color="#22D3EE"/><stop offset="1" stop-color="#6366F1"/></linearGradient></defs>`;

module.exports = { W, wrap, donkeyColor, donkeySilhouette, donkeyMask, donkeyMono, cursor, gradientDefs };
