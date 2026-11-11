// Development server for the style preview (see README.md). Serves vector tiles built
// on the fly from omt.json plus the app's styles and glyphs.
// Usage: node server.cjs <node_modules> <this folder> <App/Resources> <port>
// Test server: vector tiles from omt.json, plus static files. Only for rendering tests.
const http = require('http'), fs = require('fs'), path = require('path');
const NM = process.argv[2], ROOT = process.argv[3], APP = process.argv[4], PORT = +process.argv[5];
const vtpbf = require(path.join(NM, 'vt-pbf'));
(async () => {
const geojsonvt = (await import(path.join(NM, 'geojson-vt/src/index.js'))).default;
const layers = JSON.parse(fs.readFileSync(path.join(ROOT, 'omt.json')));
const idx = {};
for (const [k, feats] of Object.entries(layers))
  idx[k] = geojsonvt({ type: 'FeatureCollection', features: feats }, { maxZoom: 16, indexMaxZoom: 10, tolerance: 2, extent: 4096, buffer: 64 });
const types = { '.html': 'text/html', '.js': 'text/javascript', '.json': 'application/json', '.css': 'text/css', '.geojson': 'application/json' };
http.createServer((req, res) => {
  const u = decodeURIComponent(req.url.split('?')[0]);
  let m = u.match(/^\/tiles\/(\d+)\/(\d+)\/(\d+)\.pbf$/);
  if (m) {
    const [z, x, y] = m.slice(1).map(Number), out = {};
    for (const [k, i] of Object.entries(idx)) { const t = i.getTile(z, x, y); if (t) out[k] = t; }
    const buf = Buffer.from(vtpbf.fromGeojsonVt(out, { version: 2 }));
    res.writeHead(200, { 'Content-Type': 'application/x-protobuf' }); return res.end(buf);
  }
  let file;
  if (u.startsWith('/app/')) file = path.join(APP, u.slice(5));
  else if (u.startsWith('/nm/')) file = path.join(NM, u.slice(4));
  else file = path.join(ROOT, u.slice(1));
  fs.readFile(file, (e, d) => {
    if (e) { res.writeHead(404); return res.end(); }
    res.writeHead(200, { 'Content-Type': types[path.extname(file)] || 'application/octet-stream' }); res.end(d);
  });
}).listen(PORT, () => console.log('listening', PORT));
})();
