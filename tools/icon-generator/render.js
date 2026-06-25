const { Resvg } = require('@resvg/resvg-js');
const fs = require('fs');
const [,, src, out, w] = process.argv;
const svg = fs.readFileSync(src, 'utf8');
const r = new Resvg(svg, { fitTo: { mode: 'width', value: parseInt(w,10) } });
fs.writeFileSync(out, r.render().asPng());
