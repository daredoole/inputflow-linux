// Single source of truth for the InputFlow device-to-device flow mark.
const W = 1024;
const wrap = (inner, extra = '', vb = `0 0 ${W} ${W}`, w = W, h = W) =>
  `<svg width="${w}" height="${h}" viewBox="${vb}" xmlns="http://www.w3.org/2000/svg">${extra}${inner}</svg>\n`
    .replace(/[ \t]+$/gm, '');

const gradientDefs = `<defs><linearGradient id="inputflow-bg" x1="80" y1="80" x2="944" y2="944" gradientUnits="userSpaceOnUse">
    <stop offset="0" stop-color="#06B6D4"/><stop offset="1" stop-color="#4F46E5"/></linearGradient></defs>`;

// Two device outlines joined by one directional path. The geometry is deliberately
// sparse so it remains recognizable as a monochrome 16 px tray/notification icon.
const flowGlyph = (ink = '#FFFFFF') => `
  <g fill="none" stroke="${ink}" stroke-width="64" stroke-linecap="round" stroke-linejoin="round">
    <rect x="176" y="272" width="264" height="480" rx="64"/>
    <rect x="584" y="272" width="264" height="480" rx="64"/>
    <path d="M336 512 H688"/>
    <path d="M624 448 L688 512 L624 576"/>
  </g>
  <g fill="${ink}">
    <circle cx="308" cy="656" r="18"/>
    <circle cx="716" cy="656" r="18"/>
  </g>`;

module.exports = { W, wrap, gradientDefs, flowGlyph };
