// Copies the WebAssembly STEP reader (fallback when the native engine is missing) into public/occt.
const fs = require('fs');
const path = require('path');
const src = path.join(__dirname, '..', 'node_modules', 'occt-import-js', 'dist');
const dst = path.join(__dirname, '..', 'public', 'occt');
fs.mkdirSync(dst, { recursive: true });
for (const f of ['occt-import-js.js', 'occt-import-js.wasm', 'license.occt-import-js.txt', 'license.occt.txt']) {
  fs.copyFileSync(path.join(src, f), path.join(dst, f));
}
console.log('occt-import-js copied to public/occt');
