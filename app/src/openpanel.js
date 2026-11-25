import * as THREE from 'three';
import * as NL from '@neutralinojs/lib';
import { RoomEnvironment } from 'three/examples/jsm/environments/RoomEnvironment.js';
import { loadSource, sourceFromPath, ALL_EXT, cachePath, hashKey } from './loaders.js';
import { makeGlass, refreshGlass } from './glass.js';

/* =====================================================================================
 * GlassView "Open" panel: recent files and folders with 3D thumbnails, pinned folders,
 * search, and "Browse…" for the Windows file dialog.
 * Thumbnails are rendered in the background with a small second WebGL renderer from the
 * same tessellation the app uses (so generating one also pre-caches the file: opening
 * it afterwards is instant). They live in the cache folder: thumbs/<hash>.png + .json.
 * ===================================================================================== */

const EXT = new Set([...ALL_EXT, 'gva'].map((e) => e.toLowerCase()));
const CAD = new Set(['step', 'stp', 'iges', 'igs', 'brep', 'brp']);
const CAD_MAX = 25 * 1024 * 1024; // larger CAD files keep the engine busy too long for a preview
const MESH_MAX = 80 * 1024 * 1024;
const SKIP_DIR = /^(\.|\$|node_modules$|__pycache__$|AppData$|Windows$|Program Files)/i;

const KIND = { step: 'step', stp: 'step', iges: 'iges', igs: 'iges', brep: 'brep', brp: 'brep', stl: 'stl', '3mf': '3mf', obj: 'obj', ply: 'ply', glb: 'gltf', gltf: 'gltf', gva: 'gva' };
const KINDS = [
  { id: 'folder', label: 'Folders' }, { id: 'step', label: 'STEP' }, { id: 'iges', label: 'IGES' }, { id: 'brep', label: 'BREP' },
  { id: 'stl', label: 'STL' }, { id: '3mf', label: '3MF' }, { id: 'obj', label: 'OBJ' }, { id: 'ply', label: 'PLY' },
  { id: 'gltf', label: 'glTF' }, { id: 'gva', label: 'Assemblies' }
];
const extOf = (p) => { const m = /\.([^.\\/]+)$/.exec(p); return m ? m[1].toLowerCase() : ''; };
const sepOf = (p) => (p.includes('\\') ? '\\' : '/');
const baseOf = (p) => p.replace(/[\\/]+$/, '').split(/[\\/]/).pop() || p;
const join = (d, n) => (/[\\/]$/.test(d) ? d + n : d + sepOf(d) + n);
function parentOf(p) {
  const t = p.replace(/[\\/]+$/, '');
  const i = Math.max(t.lastIndexOf('\\'), t.lastIndexOf('/'));
  if (i < 0) return null;
  const par = t.slice(0, i);
  if (/^[A-Za-z]:$/.test(par)) return par + '\\';
  return par || (t.startsWith('/') ? '/' : null);
}
const isRoot = (p) => /^[A-Za-z]:[\\/]?$/.test(p) || p === '/';
const esc = (s) => s.replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));

function fmtSize(b) {
  if (b == null) return '';
  if (b < 1024) return `${b} B`;
  if (b < 1024 * 1024) return `${(b / 1024).toFixed(0)} KB`;
  if (b < 1024 ** 3) return `${(b / 1024 / 1024).toFixed(b < 10 * 1024 * 1024 ? 1 : 0)} MB`;
  return `${(b / 1024 ** 3).toFixed(1)} GB`;
}
function fmtDate(ms) {
  if (!ms) return '';
  const t = ms < 1e12 ? ms * 1000 : ms; // seconds or milliseconds
  const d = (Date.now() - t) / 1000;
  if (d < 60) return 'just now';
  if (d < 3600) return `${Math.floor(d / 60)} min ago`;
  if (d < 86400) return `${Math.floor(d / 3600)} h ago`;
  if (d < 86400 * 7) return `${Math.floor(d / 86400)} d ago`;
  return new Date(t).toLocaleDateString();
}
const fmtDim = (v) => (v >= 100 ? v.toFixed(0) : v >= 10 ? v.toFixed(1) : v.toFixed(2));

const ICON = {
  recent: '<svg viewBox="0 0 20 20"><circle cx="10" cy="10" r="7"/><path d="M10 6v4l2.6 1.8"/></svg>',
  folder: '<svg viewBox="0 0 20 20"><path d="M2.5 6.2c0-1 .8-1.7 1.7-1.7h3.4l1.6 1.8h6.6c.9 0 1.7.8 1.7 1.7v6.6c0 1-.8 1.7-1.7 1.7H4.2c-.9 0-1.7-.8-1.7-1.7z"/></svg>',
  desktop: '<svg viewBox="0 0 20 20"><rect x="2.5" y="3.5" width="15" height="10" rx="1.6"/><path d="M7 17h6M10 13.5V17"/></svg>',
  docs: '<svg viewBox="0 0 20 20"><path d="M5 2.5h6.5L15 6v11.5H5z"/><path d="M11.5 2.5V6H15M7.5 10h5M7.5 13h5"/></svg>',
  down: '<svg viewBox="0 0 20 20"><path d="M10 3v10M6 9.5l4 4 4-4M4 16.5h12"/></svg>',
  pin: '<svg viewBox="0 0 20 20"><path d="M12.5 2.8 17.2 7.5l-2.6.9-3.3 3.3.4 3.1-1.3 1.3-3.2-3.2L3.5 16.5M7.2 12.9 4 9.7l1.3-1.3 3.1.4 3.3-3.3z"/></svg>',
  up: '<svg viewBox="0 0 20 20"><path d="M10 15.5v-11M5.5 9 10 4.5 14.5 9"/></svg>',
  back: '<svg viewBox="0 0 20 20"><path d="M12.5 4.5 7 10l5.5 5.5"/></svg>',
  search: '<svg viewBox="0 0 20 20"><circle cx="8.5" cy="8.5" r="5"/><path d="m12.3 12.3 4.2 4.2"/></svg>',
  plus: '<svg viewBox="0 0 20 20"><path d="M10 4.5v11M4.5 10h11"/></svg>',
  close: '<svg viewBox="0 0 20 20"><path d="M6 6l8 8M14 6l-8 8"/></svg>',
  cube: '<svg viewBox="0 0 48 48"><path d="M24 5 41 14.5v19L24 43 7 33.5v-19z" class="fill soft"/><path d="M24 5 41 14.5v19L24 43 7 33.5v-19zM7 14.5 24 24l17-9.5M24 24v19"/></svg>'
};

/* ------------------------------------------------------------------ thumbnails */
export class Thumbs extends EventTarget {
  constructor(getQuality) {
    super();
    this.getQuality = getQuality;
    this.queue = [];
    this.busy = false;
    this.failed = new Map(); // path -> reason
    this.memo = new Map(); // path|size|mtime -> {url, meta} | null
    this.dirP = null;
  }

  async dir() {
    if (!this.dirP) {
      this.dirP = (async () => {
        const c = await cachePath();
        try { await NL.filesystem.createDirectory(`${c}/thumbs`); } catch (e) { /* exists */ }
        return `${c}/thumbs`;
      })();
    }
    return this.dirP;
  }

  keyOf(path, st) { return hashKey(`thumb1|${path}|${st.size}|${st.modifiedAt}`); }

  /** Existing thumbnail for a file (no generation). */
  async lookup(path, st) {
    const id = `${path}|${st.size}|${st.modifiedAt}`;
    if (this.memo.has(id)) return this.memo.get(id);
    const k = this.keyOf(path, st);
    const d = await this.dir();
    let res = null;
    try {
      await NL.filesystem.getStats(`${d}/${k}.png`);
      let meta = {};
      try { meta = JSON.parse(await NL.filesystem.readFile(`${d}/${k}.json`)); } catch (e) { /* no meta */ }
      res = { url: `/gvcache/thumbs/${k}.png`, meta };
    } catch (e) { res = null; }
    if (res) this.memo.set(id, res);
    return res;
  }

  canMake(path, st) {
    const e = extOf(path);
    if (e === 'gva' || !EXT.has(e)) return false;
    return st.size <= (CAD.has(e) ? CAD_MAX : MESH_MAX);
  }

  /** Ask for a thumbnail; 'thumb' fires with {path, url, meta} when it is ready. */
  request(path, st, { front = false } = {}) {
    if (this.failed.has(path) || !this.canMake(path, st)) return;
    if (this.queue.some((j) => j.path === path)) {
      if (front) { const j = this.queue.find((x) => x.path === path); this.queue = [j, ...this.queue.filter((x) => x !== j)]; }
      return;
    }
    const job = { path, st };
    if (front) this.queue.unshift(job); else this.queue.push(job);
    this._pump();
  }

  cancelAll() { this.queue = []; }

  async _pump() {
    if (this.busy) return;
    const job = this.queue.shift();
    if (!job) return;
    this.busy = true;
    try {
      const have = await this.lookup(job.path, job.st);
      if (have) this.dispatchEvent(Object.assign(new Event('thumb'), { path: job.path, ...have }));
      else {
        let timer;
        const limit = new Promise((_, rej) => { timer = setTimeout(() => rej(new Error('took longer than 60 s')), 60000); });
        const out = await Promise.race([this._make(job.path, job.st), limit]).finally(() => clearTimeout(timer));
        if (out) this.dispatchEvent(Object.assign(new Event('thumb'), { path: job.path, ...out }));
        else this._fail(job.path, 'empty model');
      }
    } catch (e) {
      this._fail(job.path, (e && (e.message || String(e))) || 'unknown error');
    }
    this.busy = false;
    setTimeout(() => this._pump(), 0);
  }

  _fail(path, reason) {
    console.warn('thumbnail failed', path, reason);
    this.failed.set(path, reason);
    this.dispatchEvent(Object.assign(new Event('thumbfail'), { path, reason }));
    // keep a small log next to the thumbnails, so a problem on another PC can be looked at
    this.dir().then((d) => NL.filesystem.appendFile(`${d}/errors.log`, `${new Date().toISOString()}  ${path}  ${reason}\n`)).catch(() => {});
  }

  _setup() {
    if (this.r) return;
    const cv = document.createElement('canvas');
    cv.width = 360; cv.height = 270;
    const r = new THREE.WebGLRenderer({ canvas: cv, antialias: true, alpha: true, preserveDrawingBuffer: true });
    r.setPixelRatio(1);
    r.setSize(360, 270, false);
    r.setClearColor(0x000000, 0);
    r.toneMapping = THREE.NeutralToneMapping;
    const scene = new THREE.Scene();
    const pm = new THREE.PMREMGenerator(r);
    scene.environment = pm.fromScene(new RoomEnvironment(), 0.04).texture;
    scene.environmentIntensity = 0.85;
    pm.dispose();
    const cam = new THREE.PerspectiveCamera(30, 360 / 270, 0.1, 1e7);
    cam.up.set(0, 0, 1);
    const head = new THREE.DirectionalLight(0xffffff, 1.1);
    head.position.set(0.3, 0.6, 1);
    cam.add(head, head.target);
    scene.add(cam, new THREE.HemisphereLight(0xffffff, 0x8890a0, 0.35));
    Object.assign(this, { r, scene, cam, cv });
  }

  async _make(path, st) {
    const src = await sourceFromPath(path);
    const model = await loadSource(src, { quality: this.getQuality() });
    this._setup();
    const root = model.root;
    root.updateMatrixWorld(true);
    const box = new THREE.Box3().setFromObject(root);
    if (box.isEmpty()) { dispose(root); return null; }
    const size = box.getSize(new THREE.Vector3());
    const c = box.getCenter(new THREE.Vector3());
    const R = Math.max(size.length() / 2, 1e-6);
    const dir = new THREE.Vector3(1, -1, 0.85).normalize();
    // fit the bounding sphere into the smaller field of view, with a margin
    const fov = THREE.MathUtils.degToRad(this.cam.fov);
    const fovMin = Math.min(fov, 2 * Math.atan(Math.tan(fov / 2) * this.cam.aspect));
    const dist = (R / Math.sin(fovMin / 2)) * 0.92;
    this.cam.position.copy(c).addScaledVector(dir, dist);
    this.cam.near = dist / 100; this.cam.far = dist * 100;
    this.cam.lookAt(c);
    this.cam.updateProjectionMatrix();
    this.scene.add(root);
    this.r.render(this.scene, this.cam);
    this.scene.remove(root);
    let tris = 0;
    root.traverse((o) => { if (o.isMesh) tris += (o.geometry.index ? o.geometry.index.count : o.geometry.attributes.position.count) / 3; });
    let bodies = 0;
    root.traverse((o) => { if (o.isMesh) bodies++; });
    dispose(root);
    this.r.renderLists.dispose();
    const blob = await new Promise((res) => this.cv.toBlob(res, 'image/png'));
    const buf = await blob.arrayBuffer();
    const d = await this.dir();
    const k = this.keyOf(path, st);
    const meta = { dims: [size.x, size.y, size.z], tris: Math.round(tris), bodies };
    await NL.filesystem.writeBinaryFile(`${d}/${k}.png`, buf);
    await NL.filesystem.writeFile(`${d}/${k}.json`, JSON.stringify(meta));
    const out = { url: `/gvcache/thumbs/${k}.png`, meta };
    this.memo.set(`${path}|${st.size}|${st.modifiedAt}`, out);
    return out;
  }
}

function dispose(root) {
  root.traverse((o) => {
    if (o.geometry) o.geometry.dispose();
    if (o.material) (Array.isArray(o.material) ? o.material : [o.material]).forEach((m) => m.dispose());
  });
}

/* ------------------------------------------------------------------ the panel */
export class OpenPanel {
  /**
   * @param store       {get,set} persistent settings
   * @param thumbs      Thumbs instance
   * @param nativePick  async (title, multiple) => string[]   (the Windows dialog)
   */
  constructor({ store, thumbs, nativePick }) {
    this.store = store;
    this.thumbs = thumbs;
    this.nativePick = nativePick;
    this.loc = 'recent';
    this.items = [];
    this.sel = new Set();
    this.anchor = -1;
    this.history = [];
    this.searchSeq = 0;
    this._build();
    thumbs.addEventListener('thumb', (e) => this._onThumb(e));
    thumbs.addEventListener('thumbfail', (e) => this._onThumbFail(e));
    this.all = [];
  }

  // ---------------------------------------------------------------- recents & pins (shared with main.js)
  recent() { return this.store.get('recent', []); }
  pins() { return this.store.get('pinnedFolders', []); }
  setPins(p) { this.store.set('pinnedFolders', p); this._renderSide(); this._renderTop(); }

  // ---------------------------------------------------------------- DOM
  _build() {
    const wrap = document.createElement('div');
    wrap.className = 'sheet-wrap op-wrap';
    wrap.innerHTML = `
      <div class="glass sheet op-sheet" data-bezel="22">
        <aside class="op-side">
          <div class="op-head"><span class="op-title"></span></div>
          <div class="op-navs"></div>
        </aside>
        <section class="op-main">
          <div class="op-top">
            <button class="btn mini op-back" data-tip="Back (Alt+←)">${ICON.back}</button>
            <button class="btn mini op-up" data-tip="Up one folder (Backspace)">${ICON.up}</button>
            <div class="op-crumbs"></div>
            <button class="btn mini op-pin" data-tip="Pin this folder">${ICON.pin}</button>
            <label class="op-search">${ICON.search}<input type="search" placeholder="Search" spellcheck="false" /></label>
            <div class="seg small op-sort"><span class="seg-ind"></span><button class="btn txt" data-sort="name">Name</button><button class="btn txt" data-sort="date">Date</button></div>
          </div>
          <div class="op-filters"></div>
          <div class="op-grid" tabindex="0"></div>
          <div class="op-foot">
            <span class="op-status"></span>
            <button class="btn txt pill op-browse" data-tip="The Windows file dialog: network drives, USB sticks, anything else">Browse…</button>
            <button class="btn txt pill op-cancel">Cancel</button>
            <button class="btn txt pill primary op-go" disabled>Open</button>
          </div>
        </section>
      </div>`;
    document.body.appendChild(wrap);
    this.wrap = wrap;
    this.sheet = wrap.querySelector('.op-sheet');
    this.grid = wrap.querySelector('.op-grid');
    this.search = wrap.querySelector('.op-search input');
    makeGlass(this.sheet);
    const q = (s) => wrap.querySelector(s);
    q('.op-cancel').addEventListener('click', () => this._finish(null));
    q('.op-go').addEventListener('click', () => this._openSelection());
    q('.op-browse').addEventListener('click', async () => {
      const res = await this.nativePick(this.title, this.multiple);
      if (res && res.length) this._finish(res);
    });
    q('.op-back').addEventListener('click', () => this._back());
    q('.op-up').addEventListener('click', () => this._up());
    q('.op-pin').addEventListener('click', () => {
      if (this.loc === 'recent') return;
      const p = this.pins();
      this.setPins(p.includes(this.loc) ? p.filter((x) => x !== this.loc) : [...p, this.loc]);
    });
    wrap.addEventListener('pointerdown', (e) => { if (e.target === wrap) this._finish(null); });
    this.search.addEventListener('input', () => { clearTimeout(this._st); this._st = setTimeout(() => this._list(), 180); });
    this.search.addEventListener('keydown', (e) => {
      if (e.key === 'ArrowDown' || e.key === 'Enter') { e.preventDefault(); this.grid.focus(); if (this.anchor < 0 && this.items.length) this._select(0); if (e.key === 'Enter') this._openSelection(); }
    });
    wrap.querySelectorAll('.op-sort .btn').forEach((b) => b.addEventListener('click', () => {
      this.store.set('openSort', b.dataset.sort);
      this._syncSort();
      this._sortItems();
      this._renderGrid();
    }));
    // capture on window: works wherever the focus is while the panel is open, and keeps the app's shortcuts out
    window.addEventListener('keydown', (e) => this._key(e), true);
    this.io = new IntersectionObserver((ents) => {
      for (const en of ents) {
        if (!en.isIntersecting) continue;
        const it = en.target._item;
        if (it && !it.dir && !it.thumb && it.st) this.thumbs.request(it.path, it.st, { front: true });
      }
    }, { root: this.grid, rootMargin: '120px' });
  }

  _syncSort() {
    const seg = this.wrap.querySelector('.op-sort');
    const b = seg.querySelector(`[data-sort="${this.store.get('openSort', 'date')}"]`);
    seg.querySelectorAll('.btn').forEach((x) => x.classList.toggle('active', x === b));
    const ind = seg.querySelector('.seg-ind');
    if (b && b.offsetWidth) { ind.style.left = b.offsetLeft + 'px'; ind.style.width = b.offsetWidth + 'px'; ind.style.opacity = 1; }
  }

  // ---------------------------------------------------------------- show / hide
  /** Resolves with the chosen paths, or null when cancelled. */
  show({ title = 'Open', action = 'Open', multiple = true } = {}) {
    if (this._resolve) this._finish(null);
    this.title = title;
    this.multiple = multiple;
    this.wrap.querySelector('.op-title').textContent = title;
    this.wrap.querySelector('.op-go').textContent = action;
    this.search.value = '';
    this.wrap.classList.add('open');
    document.body.classList.add('op-open');
    this._places().then(() => this._renderSide());
    this._renderSide();
    const start = this.store.get('openLoc', 'recent');
    this.history = [];
    this.go(start === 'recent' || !start ? 'recent' : start, false);
    requestAnimationFrame(() => { refreshGlass(this.sheet); this._syncSort(); this.search.focus(); });
    return new Promise((res) => { this._resolve = res; });
  }

  get isOpen() { return this.wrap.classList.contains('open'); }

  _finish(paths) {
    this.wrap.classList.remove('open');
    document.body.classList.remove('op-open');
    this.thumbs.cancelAll();
    this.searchSeq++;
    const r = this._resolve;
    this._resolve = null;
    if (r) r(paths);
  }

  // ---------------------------------------------------------------- locations
  async _places() {
    if (this.placeList) return;
    const out = [];
    let home = '';
    try { home = await NL.os.getEnv('USERPROFILE'); } catch (e) { /* not Windows */ }
    if (!home) { try { home = await NL.os.getEnv('HOME'); } catch (e) { /* none */ } }
    const add = async (name, icon, p) => {
      if (!p) return;
      try { const st = await NL.filesystem.getStats(p); if (st.isDirectory) out.push({ name, icon, path: p }); } catch (e) { /* missing */ }
    };
    if (home) await add('Desktop', ICON.desktop, join(home, 'Desktop'));
    let docs = '', dl = '';
    try { docs = await NL.os.getPath('documents'); } catch (e) { /* */ }
    try { dl = await NL.os.getPath('downloads'); } catch (e) { /* */ }
    await add('Documents', ICON.docs, docs || (home && join(home, 'Documents')));
    await add('Downloads', ICON.down, dl || (home && join(home, 'Downloads')));
    this.placeList = out;
  }

  _recentFolders() {
    const seen = new Set(), out = [];
    for (const p of this.recent()) {
      const d = parentOf(p);
      if (!d || seen.has(d)) continue;
      seen.add(d);
      out.push(d);
      if (out.length >= 5) break;
    }
    return out;
  }

  _renderSide() {
    const nav = this.wrap.querySelector('.op-navs');
    const row = (icon, name, loc, title, extra = '') =>
      `<button class="op-nav${this.loc === loc ? ' on' : ''}" data-loc="${esc(loc)}" title="${esc(title || loc)}">${icon}<span>${esc(name)}</span>${extra}</button>`;
    let h = row(ICON.recent, 'Recent', 'recent', 'Recently opened files');
    if (this.placeList && this.placeList.length) {
      h += '<div class="op-group">Places</div>';
      for (const p of this.placeList) h += row(p.icon, p.name, p.path, p.path);
    }
    h += `<div class="op-group">Pinned<button class="op-add" data-tip="Pin a folder…">${ICON.plus}</button></div>`;
    const pins = this.pins();
    if (!pins.length) h += '<div class="op-empty-side">Pin project folders with the pin button above the files.</div>';
    for (const p of pins) h += row(ICON.folder, baseOf(p), p, p, `<i class="op-unpin" data-unpin="${esc(p)}" title="Unpin">${ICON.close}</i>`);
    const rf = this._recentFolders().filter((d) => !pins.includes(d));
    if (rf.length) {
      h += '<div class="op-group">Recent folders</div>';
      for (const d of rf) h += row(ICON.folder, baseOf(d), d, d);
    }
    nav.innerHTML = h;
    nav.querySelectorAll('.op-nav').forEach((b) => b.addEventListener('click', (e) => {
      const un = e.target.closest('[data-unpin]');
      if (un) { e.stopPropagation(); this.setPins(this.pins().filter((x) => x !== un.dataset.unpin)); return; }
      this.search.value = '';
      this.go(b.dataset.loc);
    }));
    nav.querySelector('.op-add').addEventListener('click', async () => {
      try {
        const p = await NL.os.showFolderDialog('Pin a folder');
        if (p && !this.pins().includes(p)) { this.setPins([...this.pins(), p]); this.go(p); }
      } catch (e) { /* cancelled */ }
    });
  }

  go(loc, push = true) {
    if (push && this.loc !== loc) this.history.push(this.loc);
    this.loc = loc;
    this.store.set('openLoc', loc);
    this.sel.clear();
    this.anchor = -1;
    this._renderSide();
    this._renderTop();
    this._list();
  }

  _back() { const p = this.history.pop(); if (p) this.go(p, false); }
  _up() { if (this.loc === 'recent') return; const p = parentOf(this.loc); if (p) this.go(p); }

  _renderTop() {
    const cr = this.wrap.querySelector('.op-crumbs');
    const rec = this.loc === 'recent';
    this.wrap.querySelector('.op-up').disabled = rec || isRoot(this.loc);
    this.wrap.querySelector('.op-back').disabled = !this.history.length;
    const pin = this.wrap.querySelector('.op-pin');
    pin.style.display = rec ? 'none' : '';
    const pinned = !rec && this.pins().includes(this.loc);
    pin.classList.toggle('on', pinned);
    pin.dataset.tip = pinned ? 'Unpin this folder' : 'Pin this folder';
    if (rec) { cr.innerHTML = '<span class="op-crumb cur">Recent files</span>'; return; }
    // path → clickable segments
    const sep = sepOf(this.loc);
    const parts = this.loc.split(/[\\/]/).filter(Boolean);
    const abs = this.loc.startsWith('/') ? '/' : '';
    let acc = abs, h = '';
    parts.forEach((p, i) => {
      acc = i === 0 ? abs + p + (/^[A-Za-z]:$/.test(p) ? sep : '') : join(acc, p);
      const last = i === parts.length - 1;
      h += `<button class="op-crumb${last ? ' cur' : ''}" data-p="${esc(acc)}">${esc(p)}</button>${last ? '' : '<i>›</i>'}`;
    });
    cr.innerHTML = h;
    cr.querySelectorAll('.op-crumb[data-p]').forEach((b) => b.addEventListener('click', () => this.go(b.dataset.p)));
    cr.scrollLeft = cr.scrollWidth;
  }

  // ---------------------------------------------------------------- listing
  async _list() {
    const seq = ++this.searchSeq;
    const q = this.search.value.trim().toLowerCase();
    this.thumbs.cancelAll();
    const status = this.wrap.querySelector('.op-status');
    let items = [];
    if (this.loc === 'recent') {
      const paths = this.recent().filter((p) => !q || baseOf(p).toLowerCase().includes(q));
      items = (await Promise.all(paths.map((p) => this._fileItem(p, true)))).filter(Boolean);
      items.forEach((it, i) => { it.order = i; });
      if (seq !== this.searchSeq) return;
      this.all = items;
      this._refresh(q ? 'No recent file matches.' : 'Files you open appear here.');
      status.textContent = items.length ? `${items.length} recent file${items.length === 1 ? '' : 's'}` : '';
      return;
    }
    if (q) return this._search(q, seq);
    let ents = [];
    try { ents = await NL.filesystem.readDirectory(this.loc); }
    catch (e) {
      if (seq !== this.searchSeq) return;
      this.all = [];
      this._refresh('This folder cannot be opened.');
      status.textContent = '';
      return;
    }
    const dirs = [], files = [];
    for (const e of ents) {
      const name = e.entry;
      if (!name || name === '.' || name === '..') continue;
      const p = join(this.loc, name);
      if (e.type === 'DIRECTORY') { if (!SKIP_DIR.test(name)) dirs.push({ dir: true, path: p, name }); }
      else if (EXT.has(extOf(name))) files.push(p);
    }
    const fi = (await Promise.all(files.map((p) => this._fileItem(p, false)))).filter(Boolean);
    if (seq !== this.searchSeq) return;
    this.all = [...dirs, ...fi];
    this._refresh('No 3D files or folders here.');
    status.textContent = `${fi.length} file${fi.length === 1 ? '' : 's'}${dirs.length ? ` · ${dirs.length} folder${dirs.length === 1 ? '' : 's'}` : ''}`;
  }

  /** Recursive name search below the current folder (bounded, streams results). */
  async _search(q, seq) {
    const status = this.wrap.querySelector('.op-status');
    this.all = [];
    this._refresh('');
    status.textContent = 'Searching…';
    const queue = [[this.loc, 0]];
    let visited = 0, found = 0;
    const MAXDIR = 600, MAXDEPTH = 6, MAXHIT = 300;
    while (queue.length && visited < MAXDIR && found < MAXHIT) {
      const [d, depth] = queue.shift();
      visited++;
      let ents = [];
      try { ents = await NL.filesystem.readDirectory(d); } catch (e) { continue; }
      if (seq !== this.searchSeq) return;
      const hits = [];
      for (const e of ents) {
        const name = e.entry;
        if (!name || name === '.' || name === '..') continue;
        const p = join(d, name);
        if (e.type === 'DIRECTORY') { if (depth < MAXDEPTH && !SKIP_DIR.test(name)) queue.push([p, depth + 1]); }
        else if (EXT.has(extOf(name)) && name.toLowerCase().includes(q)) hits.push(p);
      }
      if (hits.length) {
        const its = (await Promise.all(hits.map((p) => this._fileItem(p, true)))).filter(Boolean);
        if (seq !== this.searchSeq) return;
        found += its.length;
        this.all.push(...its);
        this._refresh('');
      }
      status.textContent = `Searching… ${found} found`;
    }
    if (seq !== this.searchSeq) return;
    if (!this.all.length) this._refresh(`Nothing called “${q}” in this folder or below.`);
    status.textContent = `${found} found${visited >= MAXDIR || found >= MAXHIT ? ' (search stopped early, narrow it down)' : ''}`;
  }

  async _fileItem(p, showFolder) {
    try {
      const st = await NL.filesystem.getStats(p);
      if (!st.isFile) return null;
      const it = { path: p, name: baseOf(p), st, showFolder };
      const t = await this.thumbs.lookup(p, st);
      if (t) { it.thumb = t.url; it.meta = t.meta; }
      return it;
    } catch (e) { return null; }
  }

  _sortItems() {
    const by = this.store.get('openSort', 'date');
    const mt = (it) => (it.st ? it.st.modifiedAt : 0);
    this.items.sort((a, b) => {
      if (!!a.dir !== !!b.dir) return a.dir ? -1 : 1;
      if (by === 'date' && !a.dir) return mt(b) - mt(a);
      return a.name.localeCompare(b.name, undefined, { numeric: true, sensitivity: 'base' });
    });
  }

  // ---------------------------------------------------------------- grid
  _renderGrid(emptyText) {
    const g = this.grid;
    this.io.disconnect();
    g.innerHTML = '';
    if (!this.items.length) {
      if (emptyText) g.innerHTML = `<div class="op-none">${ICON.cube}<span>${esc(emptyText)}</span></div>`;
      this._syncGo();
      return;
    }
    const frag = document.createDocumentFragment();
    this.items.forEach((it, i) => {
      const c = document.createElement('div');
      c.className = 'op-card' + (it.dir ? ' dir' : '') + (this.sel.has(it.path) ? ' picked' : '');
      c._item = it;
      c.dataset.i = i;
      c.title = it.path;
      if (it.dir) {
        c.innerHTML = `<div class="op-thumb">${ICON.folder}</div><div class="op-name"></div><div class="op-meta">Folder</div>`;
      } else {
        const e = extOf(it.name).toUpperCase();
        c.innerHTML = `<div class="op-thumb">${it.thumb ? `<img src="${it.thumb}" alt="" draggable="false">` : `<span class="op-ph">${ICON.cube}</span>`}<b class="op-ext">${esc(e)}</b></div><div class="op-name"></div><div class="op-meta"></div>`;
        if (!it.thumb && !this.thumbs.canMake(it.path, it.st)) c.classList.add('nothumb');
        const why = this.thumbs.failed.get(it.path);
        if (!it.thumb && why) { c.classList.add('failed'); c.title = `${it.path}\nNo preview: ${why}`; }
      }
      c.querySelector('.op-name').textContent = it.name;
      if (!it.dir) c.querySelector('.op-meta').textContent = this._meta(it);
      c.addEventListener('click', (e) => this._click(i, e));
      c.addEventListener('dblclick', () => this._activate(i));
      frag.appendChild(c);
    });
    g.appendChild(frag);
    g.querySelectorAll('.op-card:not(.dir)').forEach((c) => this.io.observe(c));
    this._syncGo();
  }

  _meta(it) {
    const parts = [];
    if (it.meta && it.meta.dims) parts.push(it.meta.dims.map(fmtDim).join(' × ') + ' mm');
    else parts.push(fmtSize(it.st && it.st.size));
    parts.push(it.showFolder ? baseOf(parentOf(it.path) || '') : fmtDate(it.st && it.st.modifiedAt));
    return parts.filter(Boolean).join(' · ');
  }

  _onThumb(e) {
    const it = this.items.find((x) => x.path === e.path);
    if (!it) return;
    it.thumb = e.url; it.meta = e.meta;
    const c = [...this.grid.children].find((x) => x._item === it);
    if (!c) return;
    const box = c.querySelector('.op-thumb');
    const ph = box.querySelector('.op-ph');
    const img = document.createElement('img');
    img.src = e.url; img.alt = ''; img.draggable = false;
    img.className = 'fresh';
    if (ph) ph.replaceWith(img);
    c.querySelector('.op-meta').textContent = this._meta(it);
  }

  _onThumbFail(e) {
    const c = [...this.grid.children].find((x) => x._item && x._item.path === e.path);
    if (c) { c.classList.add('failed'); c.title = `${e.path}\nNo preview: ${e.reason}`; }
  }

  // ---------------------------------------------------------------- format filter
  _kindOf(it) { return it.dir ? 'folder' : (KIND[extOf(it.name)] || 'other'); }

  /** Rebuild this.items from this.all with the active format chips, then sort and draw. */
  _refresh(emptyText) {
    const on = new Set(this.store.get('openFilter', []));
    this.items = on.size ? this.all.filter((it) => on.has(this._kindOf(it))) : [...this.all];
    for (const p of [...this.sel]) if (!this.items.some((x) => x.path === p)) this.sel.delete(p);
    this._sortItems();
    this._renderFilters();
    this._renderGrid(this.items.length ? '' : (this.all.length && on.size ? 'Nothing of the chosen types here.' : emptyText));
  }

  _renderFilters() {
    const box = this.wrap.querySelector('.op-filters');
    const counts = new Map();
    for (const it of this.all) { const k = this._kindOf(it); counts.set(k, (counts.get(k) || 0) + 1); }
    const on = new Set(this.store.get('openFilter', []));
    const kinds = KINDS.filter((k) => counts.has(k.id) || on.has(k.id));
    if (kinds.length < 2 && !on.size) { box.innerHTML = ''; box.style.display = 'none'; return; }
    box.style.display = '';
    let h = `<button class="op-chip${on.size ? '' : ' on'}" data-k="">All</button>`;
    for (const k of kinds) h += `<button class="op-chip${on.has(k.id) ? ' on' : ''}" data-k="${k.id}">${k.id === 'folder' ? ICON.folder : ''}${k.label}<span style="opacity:.6">${counts.get(k.id) || 0}</span></button>`;
    box.innerHTML = h;
    box.querySelectorAll('.op-chip').forEach((b) => b.addEventListener('click', (e) => {
      const k = b.dataset.k;
      let set = new Set(this.store.get('openFilter', []));
      if (!k) set.clear();
      else if (e.ctrlKey || e.shiftKey || e.metaKey) { if (set.has(k)) set.delete(k); else set.add(k); } // add to the filter
      else if (set.size === 1 && set.has(k)) set.clear();                                               // click again: all
      else set = new Set([k]);
      this.store.set('openFilter', [...set]);
      this._refresh('');
    }));
  }

  // ---------------------------------------------------------------- selection
  _click(i, e) {
    const it = this.items[i];
    if (e.shiftKey && this.anchor >= 0 && this.multiple) {
      const [a, b] = [Math.min(this.anchor, i), Math.max(this.anchor, i)];
      if (!(e.ctrlKey || e.metaKey)) this.sel.clear();
      for (let k = a; k <= b; k++) if (!this.items[k].dir) this.sel.add(this.items[k].path);
    } else if ((e.ctrlKey || e.metaKey) && this.multiple && !it.dir) {
      if (this.sel.has(it.path)) this.sel.delete(it.path); else this.sel.add(it.path);
      this.anchor = i;
    } else this._select(i);
    this._paintSel();
  }

  _select(i) {
    this.sel.clear();
    const it = this.items[i];
    if (it) this.sel.add(it.path);
    this.anchor = i;
    this._paintSel();
    const c = this.grid.children[i];
    if (c) c.scrollIntoView({ block: 'nearest' });
  }

  _paintSel() {
    [...this.grid.children].forEach((c) => { if (c._item) c.classList.toggle('picked', this.sel.has(c._item.path)); });
    this._syncGo();
  }

  _syncGo() {
    const files = this.items.filter((x) => !x.dir && this.sel.has(x.path));
    const go = this.wrap.querySelector('.op-go');
    go.disabled = !files.length && !(this.sel.size === 1 && this.items.some((x) => x.dir && this.sel.has(x.path)));
  }

  _activate(i) {
    const it = this.items[i];
    if (!it) return;
    if (it.dir) { this.search.value = ''; this.go(it.path); }
    else this._finish([it.path]);
  }

  _openSelection() {
    const sel = this.items.filter((x) => this.sel.has(x.path));
    if (sel.length === 1 && sel[0].dir) return this._activate(this.items.indexOf(sel[0]));
    const files = sel.filter((x) => !x.dir).map((x) => x.path);
    if (files.length) this._finish(this.multiple ? files : files.slice(0, 1));
  }

  _cols() {
    const cards = this.grid.children;
    if (cards.length < 2) return 1;
    const top = cards[0].offsetTop;
    let n = 0;
    for (const c of cards) { if (c.offsetTop !== top) break; n++; }
    return Math.max(1, n);
  }

  _key(e) {
    if (!this.isOpen) return;
    const inSearch = document.activeElement === this.search;
    if (e.key === 'Escape') {
      e.preventDefault(); e.stopPropagation();
      if (inSearch && this.search.value) { this.search.value = ''; this._list(); } else this._finish(null);
      return;
    }
    if (inSearch) return;
    if (e.target && e.target.closest && e.target.closest('input, select, textarea')) return;
    e.stopPropagation(); // keep the app's shortcuts out while the panel is open
    const n = this.items.length;
    const cur = this.anchor;
    const move = (k) => { e.preventDefault(); if (n) this._select(Math.max(0, Math.min(n - 1, k))); };
    if (e.key === 'ArrowRight') move(cur + 1);
    else if (e.key === 'ArrowLeft') { if (e.altKey) { e.preventDefault(); this._back(); } else move(cur - 1); }
    else if (e.key === 'ArrowDown') move(cur < 0 ? 0 : cur + this._cols());
    else if (e.key === 'ArrowUp') move(cur - this._cols());
    else if (e.key === 'Home') move(0);
    else if (e.key === 'End') move(n - 1);
    else if (e.key === 'Enter') { e.preventDefault(); this._openSelection(); }
    else if (e.key === 'Backspace') { e.preventDefault(); this._up(); }
    else if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'a' && this.multiple) {
      e.preventDefault();
      this.items.forEach((x) => { if (!x.dir) this.sel.add(x.path); });
      this._paintSel();
    } else if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === 'f') { e.preventDefault(); this.search.focus(); }
    else if (e.key.length === 1 && !e.ctrlKey && !e.metaKey && !e.altKey) { this.search.focus(); }
  }
}
