import * as THREE from 'three';
import * as NL from '@neutralinojs/lib';
import { startKernel, kernelCall } from './kernel.js';

export const isNative = typeof window.NL_PORT !== 'undefined';

export const FORMATS = {
  step: ['stp', 'step'],
  iges: ['igs', 'iges'],
  brep: ['brep', 'brp'],
  stl: ['stl'],
  obj: ['obj'],
  ply: ['ply'],
  '3mf': ['3mf'],
  gltf: ['glb', 'gltf']
};
export const ALL_EXT = Object.values(FORMATS).flat();
export function isAssemblyFile(name) { return /\.gva$/i.test(name); }
export function formatOf(name) {
  const ext = name.split('.').pop().toLowerCase();
  for (const [k, v] of Object.entries(FORMATS)) if (v.includes(ext)) return k;
  return null;
}

// Presets are deliberately far apart so the difference is visible (angular deflection matters most
// for round features: 0.6 rad ≈ 34° per segment, 0.04 rad ≈ 2.3°).
export const QUALITY = {
  coarse: { linearUnit: 'millimeter', linearDeflectionType: 'bounding_box_ratio', linearDeflection: 0.003, angularDeflection: 0.6 },
  normal: { linearUnit: 'millimeter', linearDeflectionType: 'bounding_box_ratio', linearDeflection: 0.0008, angularDeflection: 0.25 },
  fine: { linearUnit: 'millimeter', linearDeflectionType: 'bounding_box_ratio', linearDeflection: 0.0002, angularDeflection: 0.1 },
  ultra: { linearUnit: 'millimeter', linearDeflectionType: 'bounding_box_ratio', linearDeflection: 0.00005, angularDeflection: 0.04 }
};

// ------------------------------------------------------------------ workers
let jobId = 0;
class WorkerHost {
  constructor(url) { this.url = url; this.w = null; this.jobs = new Map(); }
  get worker() {
    if (!this.w) {
      this.w = new Worker(this.url);
      this.w.onmessage = (ev) => {
        const job = this.jobs.get(ev.data.id);
        if (job) job(ev.data);
      };
    }
    return this.w;
  }
  run(msg, transfer, onMsg) {
    const id = ++jobId;
    this.jobs.set(id, onMsg);
    this.worker.postMessage({ ...msg, id }, transfer || []);
    return () => this.jobs.delete(id);
  }
  reset() { if (this.w) { this.w.terminate(); this.w = null; this.jobs.clear(); } }
}
const base = new URL('.', document.baseURI);
export const occtHost = new WorkerHost(new URL('workers/occt-worker.js', base));
export const meshHost = new WorkerHost(new URL('workers/mesh-worker.js', base));

/** Compile the OCCT wasm module in the background so the first STEP opens faster. */
export function prewarmKernel() {
  occtHost.run({ type: 'warm' }, [], () => {});
}

// ------------------------------------------------------------------ native file access
let mountSeq = 0;
const mounts = new Map(); // folder -> route (kept while the app runs; several files may load at once)
function splitPath(p) {
  const i = Math.max(p.lastIndexOf('\\'), p.lastIndexOf('/'));
  return { dir: p.slice(0, i), base: p.slice(i + 1) };
}

/** Fast native read: mount the folder on Neutralino's HTTP server and fetch() it (binary, no base64). */
async function readNative(path) {
  const { dir, base } = splitPath(path);
  try {
    let route = mounts.get(dir);
    if (!route) {
      route = `/gvf${++mountSeq}`;
      mounts.set(dir, route);
      await NL.server.mount(route, dir);
    }
    const res = await fetch(`${route}/${encodeURIComponent(base)}`);
    if (res.ok) return await res.arrayBuffer();
  } catch (e) { /* fall through */ }
  return await NL.filesystem.readBinaryFile(path);
}

export async function sourceFromPath(path) {
  const st = await NL.filesystem.getStats(path);
  const { base } = splitPath(path);
  return {
    name: base, path, size: st.size,
    key: `${path}|${st.size}|${st.modifiedAt}`,
    read: () => readNative(path)
  };
}

export function sourceFromFile(file) {
  return {
    name: file.name, path: null, size: file.size,
    key: `${file.name}|${file.size}|${file.lastModified}`,
    read: () => file.arrayBuffer()
  };
}

// ------------------------------------------------------------------ tessellation cache (native only)
const CACHE_VERSION = 'gvc4';
let cacheDirP = null;
function cacheDir() {
  if (!cacheDirP) {
    cacheDirP = (async () => {
      // Prefer the local (non-roaming) AppData on Windows so a big cache never syncs with a roaming profile.
      let dataDir = '';
      try { dataDir = await NL.os.getEnv('LOCALAPPDATA'); } catch (e) { /* not Windows */ }
      if (!dataDir) dataDir = await NL.os.getPath('data');
      try { await NL.filesystem.createDirectory(`${dataDir}/GlassView`); } catch (e) { /* exists */ }
      const dir = `${dataDir}/GlassView/cache`;
      try { await NL.filesystem.createDirectory(dir); } catch (e) { /* exists */ }
      try { await NL.server.mount('/gvcache', dir); } catch (e) { /* already mounted */ }
      return dir;
    })();
  }
  return cacheDirP;
}
export function hashKey(s) {
  let h1 = 0x811c9dc5, h2 = 0x01000193;
  for (let i = 0; i < s.length; i++) {
    const c = s.charCodeAt(i);
    h1 = Math.imul(h1 ^ c, 16777619) >>> 0;
    h2 = Math.imul(h2 ^ c, 2246822519) >>> 0;
  }
  return h1.toString(16).padStart(8, '0') + h2.toString(16).padStart(8, '0');
}

const ARRAY_TYPES = { f32: Float32Array, u32: Uint32Array, u8: Uint8Array, i32: Int32Array };
function typeTag(a) { return a instanceof Float32Array ? 'f32' : a instanceof Uint32Array ? 'u32' : a instanceof Int32Array ? 'i32' : 'u8'; }

/** Serialise mesh data + edges into one binary blob: [magic][jsonLen][json][pad][arrays...]. */
function packCache(data, edges, faces) {
  const arrays = [];
  const add = (a) => { if (!a) return -1; arrays.push(a); return arrays.length - 1; };
  const meta = {
    tree: data.tree, units: data.units,
    meshes: data.meshes.map((m, i) => ({
      name: m.name, color: m.color,
      position: add(m.position), normal: add(m.normal), index: add(m.index), colors: add(m.colors),
      edges: add(edges && edges[i]),
      ranges: add(faces && faces[i] && faces[i].ranges),
      prims: faces && faces[i] ? faces[i].prims : null
    }))
  };
  meta.arrays = arrays.map((a) => ({ t: typeTag(a), n: a.length }));
  const json = new TextEncoder().encode(JSON.stringify(meta));
  let off = 8 + json.length;
  off = (off + 3) & ~3;
  const offsets = [];
  for (const a of arrays) { offsets.push(off); off += a.byteLength; off = (off + 3) & ~3; }
  const buf = new ArrayBuffer(off);
  const u8 = new Uint8Array(buf);
  const dv = new DataView(buf);
  dv.setUint32(0, 0x31435647, true); // 'GVC1'
  dv.setUint32(4, json.length, true);
  u8.set(json, 8);
  arrays.forEach((a, i) => u8.set(new Uint8Array(a.buffer, a.byteOffset, a.byteLength), offsets[i]));
  return buf;
}

function unpackCache(buf) {
  const dv = new DataView(buf);
  if (dv.getUint32(0, true) !== 0x31435647) throw new Error('bad cache');
  const jl = dv.getUint32(4, true);
  const meta = JSON.parse(new TextDecoder().decode(new Uint8Array(buf, 8, jl)));
  let off = (8 + jl + 3) & ~3;
  const arrays = meta.arrays.map((d) => {
    const T = ARRAY_TYPES[d.t];
    const a = new T(buf, off, d.n);
    off += a.byteLength; off = (off + 3) & ~3;
    return a;
  });
  const get = (i) => (i >= 0 ? arrays[i] : null);
  // Engine 1.3+: repeated parts carry {geom: k, matrix} and share meshes[k]'s arrays (and face data),
  // every part has its placement in `matrix` and its geometry in part coordinates.
  const src = (m) => (m.geom != null && m.geom >= 0 ? meta.meshes[m.geom] : m);
  const faces = meta.meshes.map((m) => (m.geom == null && m.ranges >= 0 ? { ranges: get(m.ranges), prims: m.prims, eranges: m.eranges >= 0 ? get(m.eranges) : null } : null));
  return {
    data: {
      tree: meta.tree, units: meta.units,
      meshes: meta.meshes.map((m) => {
        const g = src(m);
        return { name: m.name, color: m.color, position: get(g.position), normal: get(g.normal), index: get(g.index), colors: get(g.colors),
          geom: m.geom != null && m.geom >= 0 ? m.geom : null, matrix: m.matrix || null };
      })
    },
    edges: meta.meshes.map((m) => get(src(m).edges)),
    faces: meta.meshes.map((m, i) => (m.geom != null && m.geom >= 0 ? faces[m.geom] : faces[i])),
    native: !!meta.native
  };
}

export async function readGvc(file) {
  const res = await fetch(`/gvcache/${file}`);
  return unpackCache(res.ok ? await res.arrayBuffer() : await NL.filesystem.readBinaryFile(`${await cacheDir()}/${file}`));
}
export async function cachePath() { return cacheDir(); }

/** Files without a path (drag & drop) are written to the cache folder so the engine can read them. */
async function tempCopy(src, buffer) {
  const dir = await cacheDir();
  const ext = src.name.split('.').pop();
  const path = `${dir}/src-${hashKey(src.key)}.${ext}`;
  try { await NL.filesystem.getStats(path); return path; } catch (e) { /* write it */ }
  const CH = 8 * 1024 * 1024;
  for (let o = 0; o < buffer.byteLength; o += CH) {
    const part = buffer.slice(o, Math.min(buffer.byteLength, o + CH));
    if (o === 0) await NL.filesystem.writeBinaryFile(path, part);
    else await NL.filesystem.appendBinaryFile(path, part);
  }
  return path;
}

async function cacheRead(key) {
  if (!isNative) return null;
  try {
    const dir = await cacheDir();
    const file = `${hashKey(key)}.gvc`;
    await NL.filesystem.getStats(`${dir}/${file}`);
    const res = await fetch(`/gvcache/${file}`);
    const buf = res.ok ? await res.arrayBuffer() : await NL.filesystem.readBinaryFile(`${dir}/${file}`);
    return unpackCache(buf);
  } catch (e) {
    return null;
  }
}

async function cacheWrite(key, data, edges, faces) {
  if (!isNative) return;
  try {
    const dir = await cacheDir();
    const name = hashKey(key);
    const tmp = `${dir}/${name}.tmp`;
    const buf = packCache(data, edges, faces);
    const CH = 8 * 1024 * 1024;
    for (let o = 0; o < buf.byteLength; o += CH) {
      const part = buf.slice(o, Math.min(buf.byteLength, o + CH));
      if (o === 0) await NL.filesystem.writeBinaryFile(tmp, part);
      else await NL.filesystem.appendBinaryFile(tmp, part);
    }
    try { await NL.filesystem.remove(`${dir}/${name}.gvc`); } catch (e) { /* none */ }
    await NL.filesystem.move(tmp, `${dir}/${name}.gvc`);
  } catch (e) {
    console.warn('cache write failed', e);
  }
}

export async function clearCache() {
  if (!isNative) return 0;
  const dir = await cacheDir();
  let n = 0;
  try {
    for (const e of await NL.filesystem.readDirectory(dir)) {
      if (e.type === 'FILE') { await NL.filesystem.remove(`${dir}/${e.entry}`); n++; }
    }
  } catch (e) { /* ignore */ }
  return n;
}

// ------------------------------------------------------------------ build three.js objects
// Geometry repair while reading STEP: 'fast' (default) skips orientation / self-intersection checks
// that clean CAD exports do not need; 'full' runs everything OpenCASCADE has (slower).
let healMode = 'fast';
export const setHealMode = (m) => { healMode = m === 'full' ? 'full' : 'fast'; };
const heal = () => healMode;

const DEFAULT_COLOR = new THREE.Color().setRGB(0.62, 0.68, 0.76, THREE.SRGBColorSpace);

function makeMaterial(color, vertexColors) {
  return new THREE.MeshStandardMaterial({
    color: vertexColors ? 0xffffff : color,
    vertexColors,
    metalness: 0.12,
    roughness: 0.48,
    side: THREE.DoubleSide,
    polygonOffset: true, polygonOffsetFactor: 1, polygonOffsetUnits: 1
  });
}

function meshObjs_place(mesh, matrix) {
  if (!matrix) return;
  mesh.matrix.fromArray(matrix);
  mesh.matrix.decompose(mesh.position, mesh.quaternion, mesh.scale);
}

/** Build a Group from worker/caches mesh data. Returns { root, meshes[], tree }. */
export function buildFromData(data) {
  const geoms = [];
  const meshObjs = data.meshes.map((m, i) => {
    if (m.geom != null && geoms[m.geom]) {
      // a repeated part: same geometry (GPU buffers, picking tree), own material and placement
      const color = m.color ? new THREE.Color().setRGB(m.color[0], m.color[1], m.color[2], THREE.SRGBColorSpace) : DEFAULT_COLOR;
      const mesh = new THREE.Mesh(geoms[m.geom], makeMaterial(color, !!m.colors));
      mesh.name = m.name;
      mesh.userData.instanced = true;
      meshObjs_place(mesh, m.matrix);
      geoms[i] = geoms[m.geom];
      return mesh;
    }
    const g = new THREE.BufferGeometry();
    geoms[i] = g;
    g.setAttribute('position', new THREE.BufferAttribute(m.position, 3));
    if (m.normal) g.setAttribute('normal', new THREE.BufferAttribute(m.normal, 3));
    if (m.index) g.setIndex(new THREE.BufferAttribute(m.index, 1));
    if (m.colors) g.setAttribute('color', new THREE.BufferAttribute(m.colors, 3, true));
    if (!m.normal) g.computeVertexNormals();
    g.computeBoundingSphere();
    const color = m.color ? new THREE.Color().setRGB(m.color[0], m.color[1], m.color[2], THREE.SRGBColorSpace) : DEFAULT_COLOR;
    const mesh = new THREE.Mesh(g, makeMaterial(color, !!m.colors));
    mesh.name = m.name;
    meshObjs_place(mesh, m.matrix);
    return mesh;
  });
  // geometry used by more than one part
  meshObjs.forEach((mesh, i) => { if (data.meshes[i].geom != null) meshObjs[data.meshes[i].geom].userData.instanced = true; });
  const used = new Set();
  const makeNode = (n) => {
    const grp = new THREE.Group();
    grp.name = n.name;
    for (const i of n.meshes) {
      let mesh = meshObjs[i];
      if (!mesh) continue;
      if (used.has(i)) {
        const src = mesh;
        mesh = new THREE.Mesh(src.geometry, src.material.clone());
        mesh.name = src.name;
        mesh.position.copy(src.position); mesh.quaternion.copy(src.quaternion); mesh.scale.copy(src.scale);
        mesh.userData.instanced = true; src.userData.instanced = true;
      }
      used.add(i);
      grp.add(mesh);
    }
    for (const c of n.children) grp.add(makeNode(c));
    return grp;
  };
  const root = makeNode(data.tree);
  return { root, meshes: meshObjs };
}

function triCount(root) {
  let n = 0;
  root.traverse((o) => {
    if (o.isMesh && !o.userData.isOverlay) {
      const g = o.geometry;
      n += g.index ? g.index.count / 3 : g.attributes.position.count / 3;
    }
  });
  return n;
}

// ------------------------------------------------------------------ main entry
/**
 * Load a source. Callbacks:
 *  onProgress(text), onModel({root, tris, fromCache, ms}), onEdges(meshes[], edges[])
 */
export async function loadSource(src, { quality = 'normal', params = null, cacheTag = null, onProgress, onModel, onEdges }) {
  const fmt = formatOf(src.name);
  if (!fmt) throw new Error('Unsupported file type');
  const t0 = performance.now();
  const isCAD = fmt === 'step' || fmt === 'iges' || fmt === 'brep';
  const cacheKey = `${CACHE_VERSION}|${src.key}|${isCAD ? cacheTag || quality : ''}`;

  // Native OpenCASCADE engine (multi-threaded, exact B-rep data). Used whenever it is installed.
  const kernel = isCAD && isNative && !params ? await startKernel() : null;
  const nKey = `${CACHE_VERSION}|native2|${src.key}|${quality}|${heal()}`;
  if (kernel) {
    onProgress?.('Checking cache…');
    let cached = await cacheRead(nKey);
    const wasCached = !!cached;
    let kernelPath = src.path;
    try {
      if (!cached) {
        if (!kernelPath) { onProgress?.('Preparing file…'); kernelPath = await tempCopy(src, await src.read()); }
        onProgress?.('Reading the file…');
        const q = QUALITY[quality];
        const tk = performance.now();
        const secs = () => Math.round((performance.now() - tk) / 1000);
        let stage = 'Reading the file', pct = null;
        const show = () => onProgress?.(`${stage}${stage === 'Meshing' && pct > 0 && pct < 100 ? ` ${pct} %` : ''}… ${secs() >= 3 ? `${secs()} s` : ''}`);
        const tick = setInterval(show, 1000); // keeps counting while one long step runs
        try {
          await kernelCall({ cmd: 'load', path: kernelPath, out: `${await cacheDir()}/${hashKey(nKey)}.gvc`, lin: q.linearDeflection, linType: 'ratio', ang: q.angularDeflection, heal: heal() },
            (st, p) => { stage = st; pct = p; show(); });
        } finally { clearInterval(tick); }
        onProgress?.('Opening…');
        cached = await cacheRead(nKey);
        if (!cached) throw new Error('the engine finished but its result could not be read');
      }
    } catch (e) {
      // A big CAD file would take minutes (and lots of memory) in the WebAssembly fallback: say what went wrong instead.
      const size = src.size || 0;
      if (size > 15 * 1024 * 1024) throw new Error(`The geometry engine could not open this file: ${e.message || e}`);
      console.warn('native engine failed, falling back to WebAssembly', e);
      cached = null;
    }
    if (cached) {
      if (!kernelPath) kernelPath = `${await cacheDir()}/src-${hashKey(src.key)}.${src.name.split('.').pop()}`;
      const built = buildFromData(cached.data);
      const model = { root: built.root, tris: triCount(built.root), fromCache: wasCached, ms: performance.now() - t0, isCAD, native: true, kernelPath };
      onModel?.(model);
      setTimeout(() => onEdges?.(built.meshes, cached.edges, cached.faces), 0);
      return model;
    }
  }

  if (isCAD) {
    onProgress?.('Checking cache…');
    const cached = await cacheRead(cacheKey);
    if (cached) {
      const built = buildFromData(cached.data);
      const model = { root: built.root, tris: triCount(built.root), fromCache: true, ms: performance.now() - t0, isCAD };
      onModel?.(model);
      setTimeout(() => onEdges?.(built.meshes, cached.edges, cached.faces), 0);
      return model;
    }
  }

  onProgress?.('Reading file…');
  const buffer = await src.read();

  if (isCAD || fmt === 'stl') {
    const host = isCAD ? occtHost : meshHost;
    const msg = isCAD
      ? { type: 'cad', format: fmt, buffer, params: params || QUALITY[quality] }
      : { type: 'stl', buffer, name: src.name.replace(/\.[^.]+$/, '') };
    return await new Promise((resolve, reject) => {
      let built = null, data = null;
      const done = host.run(msg, [buffer], (m) => {
        if (m.type === 'progress') onProgress?.(m.text);
        else if (m.type === 'error') { done(); reject(new Error(m.message)); }
        else if (m.type === 'mesh') {
          data = m.data;
          built = buildFromData(data);
          const model = { root: built.root, tris: triCount(built.root), fromCache: false, ms: performance.now() - t0, isCAD };
          onModel?.(model);
          resolve(model);
        } else if (m.type === 'edges') {
          done();
          onEdges?.(built.meshes, m.edges, m.faces);
          if (isCAD) {
            const write = () => cacheWrite(cacheKey, data, m.edges, m.faces);
            (window.requestIdleCallback || setTimeout)(write, { timeout: 2000 });
            // A huge file leaves the wasm heap large; recycle the worker and re-warm it.
            if (src.size > 40e6) { setTimeout(() => { occtHost.reset(); prewarmKernel(); }, 3000); }
          }
        }
      });
    });
  }

  // Other mesh formats: three.js loaders, loaded on demand to keep startup lean.
  onProgress?.('Parsing…');
  let root;
  if (fmt === 'obj') {
    const { OBJLoader } = await import('three/examples/jsm/loaders/OBJLoader.js');
    root = new OBJLoader().parse(new TextDecoder().decode(buffer));
    restyle(root, true);
  } else if (fmt === 'ply') {
    const { PLYLoader } = await import('three/examples/jsm/loaders/PLYLoader.js');
    const g = new PLYLoader().parse(buffer);
    if (!g.attributes.normal) g.computeVertexNormals();
    root = new THREE.Group();
    root.add(new THREE.Mesh(g, makeMaterial(DEFAULT_COLOR, !!g.attributes.color)));
  } else if (fmt === '3mf') {
    const { ThreeMFLoader } = await import('three/examples/jsm/loaders/3MFLoader.js');
    root = new ThreeMFLoader().parse(buffer);
    restyle(root, false);
  } else if (fmt === 'gltf') {
    const { GLTFLoader } = await import('three/examples/jsm/loaders/GLTFLoader.js');
    const gltf = await new Promise((res, rej) => new GLTFLoader().parse(buffer, '', res, rej));
    root = new THREE.Group();
    gltf.scene.rotation.x = Math.PI / 2; // glTF is Y-up
    root.add(gltf.scene);
    restyle(root, false);
  }
  const model = { root, tris: triCount(root), fromCache: false, ms: performance.now() - t0, isCAD: false };
  onModel?.(model);

  // edges in the background
  const meshes = [];
  root.traverse((o) => { if (o.isMesh) meshes.push(o); });
  const items = meshes.map((m) => ({
    position: m.geometry.attributes.position.array.slice(),
    index: m.geometry.index ? new Uint32Array(m.geometry.index.array) : null
  }));
  const tr = items.flatMap((i) => (i.index ? [i.position.buffer, i.index.buffer] : [i.position.buffer]));
  const done = meshHost.run({ type: 'edges', items, angle: 30 }, tr, (m) => {
    if (m.type === 'edges') { done(); onEdges?.(meshes, m.edges); }
  });
  return model;
}

function restyle(root, replace) {
  root.traverse((o) => {
    if (!o.isMesh) return;
    if (!o.geometry.attributes.normal) o.geometry.computeVertexNormals();
    if (replace) { o.material = makeMaterial(DEFAULT_COLOR, false); return; }
    const mats = Array.isArray(o.material) ? o.material : [o.material];
    o.material = mats.map((m) => {
      const c = m.clone();
      c.side = THREE.DoubleSide;
      c.polygonOffset = true; c.polygonOffsetFactor = 1; c.polygonOffsetUnits = 1;
      return c;
    });
    if (o.material.length === 1) o.material = o.material[0];
  });
}
