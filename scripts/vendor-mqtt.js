// Copies the browser build of mqtt.js into app/vendor so the web app can
// load it locally (works offline and inside the Capacitor WebView).
// Run: npm run vendor
const fs = require('fs');
const path = require('path');

const src = path.join(__dirname, '..', 'node_modules', 'mqtt', 'dist', 'mqtt.min.js');
const destDir = path.join(__dirname, '..', 'app', 'vendor');
const dest = path.join(destDir, 'mqtt.min.js');

fs.mkdirSync(destDir, { recursive: true });
fs.copyFileSync(src, dest);
console.log('vendored', path.relative(process.cwd(), dest));
