import * as THREE from 'three';
import { TransformControls } from 'three/examples/jsm/controls/TransformControls.js';

/**
 * Simple assembly editing.
 * Any tree node (component, sub-assembly or body) can be moved/rotated. The first time a node is
 * edited it is wrapped in a "pivot" group placed at the node's bounding-box centre, so rotations
 * happen about the part itself. Translation is reported relative to that original centre
 * (i.e. 0,0,0 = where the part was imported), rotation as XYZ Euler angles in degrees.
 */

/** Stable path of every node inside a component, assigned once at load (before any pivots). */
export function tagPaths(comp) {
  const walk = (o, path) => {
    o.userData.gvPath = path;
    logicalChildren(o).forEach((c, i) => walk(c, path ? `${path}/${i}` : String(i)));
  };
  walk(comp, '');
}

export function logicalChildren(o) {
  return o.children
    .map((c) => (c.userData.isPivot ? c.children.find((x) => !x.userData.isOverlay) : c))
    .filter((c) => c && !c.userData.isOverlay && (c.isGroup || c.isMesh));
}

export function logicalParent(o) {
  let p = o.parent;
  if (p && p.userData.isPivot) p = p.parent;
  return p;
}

export function pivotOf(obj) { return obj.parent && obj.parent.userData.isPivot ? obj.parent : null; }

/** Wrap obj in a pivot at its bbox centre (in parent space). Idempotent. */
export function ensurePivot(obj, viewer) {
  const existing = pivotOf(obj);
  if (existing) return existing;
  const parent = obj.parent;
  parent.updateMatrixWorld(true);
  const worldCenter = viewer.boxOf(obj).getCenter(new THREE.Vector3());
  const c = parent.worldToLocal(worldCenter.clone());
  const pivot = new THREE.Group();
  pivot.userData.isPivot = true;
  pivot.userData.origin = c.clone();
  pivot.position.copy(c);
  const idx = parent.children.indexOf(obj);
  parent.remove(obj);
  parent.children.splice(idx, 0, pivot);
  pivot.parent = parent;
  obj.position.sub(c);
  pivot.add(obj);
  pivot.updateMatrixWorld(true);
  return pivot;
}

/** Serialise the edited transforms of a component. */
export function serializeTransforms(comp) {
  const out = [];
  comp.traverse((o) => {
    if (!o.userData.isPivot) return;
    const inner = o.children.find((x) => !x.userData.isOverlay);
    if (!inner) return;
    const off = o.position.clone().sub(o.userData.origin);
    out.push({ path: inner.userData.gvPath, t: off.toArray(), r: [o.rotation.x, o.rotation.y, o.rotation.z] });
  });
  // the component itself may be wrapped (its pivot sits outside comp)
  const p = pivotOf(comp);
  if (p) out.push({ path: '', t: p.position.clone().sub(p.userData.origin).toArray(), r: [p.rotation.x, p.rotation.y, p.rotation.z] });
  return out;
}

export function findByPath(comp, path) {
  if (!path) return comp;
  let o = comp;
  for (const i of path.split('/')) {
    o = logicalChildren(o)[+i];
    if (!o) return null;
  }
  return o;
}

/** Re-apply serialised transforms (outermost first so centres are computed consistently). */
export function applyTransforms(comp, list, viewer) {
  const sorted = [...list].sort((a, b) => a.path.length - b.path.length);
  for (const e of sorted) {
    const obj = findByPath(comp, e.path);
    if (!obj) continue;
    const p = ensurePivot(obj, viewer);
    p.position.copy(p.userData.origin).add(new THREE.Vector3(...e.t));
    p.rotation.set(e.r[0], e.r[1], e.r[2]);
    p.updateMatrixWorld(true);
  }
}

const DEG = Math.PI / 180;

/**
 * Restyle three.js' transform gizmo to the app's design language: Apple system colours, thicker
 * rounded shafts with pill-shaped tips, smooth rings, rounded-square plane handles and a soft
 * white centre. Only the visible handles change; the (invisible) pick volumes stay as they are.
 */
function roundedSquare(size, radius) {
  const h = size / 2, r = radius, s = new THREE.Shape();
  s.moveTo(-h + r, -h);
  s.lineTo(h - r, -h); s.quadraticCurveTo(h, -h, h, -h + r);
  s.lineTo(h, h - r); s.quadraticCurveTo(h, h, h - r, h);
  s.lineTo(-h + r, h); s.quadraticCurveTo(-h, h, -h, h - r);
  s.lineTo(-h, -h + r); s.quadraticCurveTo(-h, -h, -h + r, -h);
  return new THREE.ShapeGeometry(s, 8);
}
/**
 * TransformControls bakes every handle's placement into its geometry. To swap in a new shape we
 * regenerate the original (canonical) geometry, solve the affine transform that maps it onto the
 * baked one (4 well-spread corresponding vertices), and apply that transform to the new shape.
 */
function bakedMatrix(baked, canon) {
  const A = canon.attributes.position, B = baked.attributes.position;
  if (A.count !== B.count) return null;
  const va = (i) => new THREE.Vector3().fromBufferAttribute(A, i);
  const vb = (i) => new THREE.Vector3().fromBufferAttribute(B, i);
  const i0 = 0;
  let i1 = 0, i2 = 0, i3 = 0, best = 0;
  for (let i = 0; i < A.count; i++) { const d = va(i).distanceToSquared(va(i0)); if (d > best) { best = d; i1 = i; } }
  const e1 = va(i1).sub(va(i0)).normalize();
  best = 0;
  for (let i = 0; i < A.count; i++) { const v = va(i).sub(va(i0)); const d = v.sub(e1.clone().multiplyScalar(v.dot(e1))).lengthSq(); if (d > best) { best = d; i2 = i; } }
  const n = new THREE.Vector3().crossVectors(e1, va(i2).sub(va(i0))).normalize();
  best = 0;
  for (let i = 0; i < A.count; i++) { const d = Math.abs(va(i).sub(va(i0)).dot(n)); if (d > best) { best = d; i3 = i; } }
  const thin = best < 1e-9; // planar canonical shape: use the normal as the third direction
  const a0 = va(i0), b0 = vb(i0);
  const Ma = new THREE.Matrix3(), Mb = new THREE.Matrix3();
  const da1 = va(i1).sub(a0), da2 = va(i2).sub(a0), db1 = vb(i1).sub(b0), db2 = vb(i2).sub(b0);
  const da3 = thin ? new THREE.Vector3().crossVectors(da1, da2) : va(i3).sub(a0);
  const db3 = thin ? new THREE.Vector3().crossVectors(db1, db2) : vb(i3).sub(b0);
  Ma.set(da1.x, da2.x, da3.x, da1.y, da2.y, da3.y, da1.z, da2.z, da3.z);
  Mb.set(db1.x, db2.x, db3.x, db1.y, db2.y, db3.y, db1.z, db2.z, db3.z);
  if (Math.abs(Ma.determinant()) < 1e-12) return null;
  const L = Mb.multiply(Ma.invert());
  const e = L.elements;
  const t = b0.clone().sub(a0.clone().applyMatrix3(L));
  return new THREE.Matrix4().set(e[0], e[3], e[6], t.x, e[1], e[4], e[7], t.y, e[2], e[5], e[8], t.z, 0, 0, 0, 1);
}

function styleGizmo(tc) {
  tc.setColors(0xff453a, 0x30d158, 0x0a84ff, 0xffd60a);
  const ring = (r, arc, tube, rs, ts) => { const g = new THREE.TorusGeometry(r, tube, rs, ts, arc); g.rotateY(Math.PI / 2); g.rotateX(Math.PI / 2); return g; };
  const recipes = [
    { // arrow tip -> pill
      match: (t, p) => t === 'CylinderGeometry' && p.radiusTop === 0 && p.radiusBottom === 0.04,
      canon: () => new THREE.CylinderGeometry(0, 0.04, 0.1, 12).translate(0, 0.05, 0),
      make: () => new THREE.CapsuleGeometry(0.03, 0.045, 6, 16).translate(0, 0.045, 0)
    },
    { // shaft -> thicker round rod
      match: (t, p) => t === 'CylinderGeometry' && p.radiusTop === 0.0075,
      canon: () => new THREE.CylinderGeometry(0.0075, 0.0075, 0.5, 3).translate(0, 0.25, 0),
      make: () => new THREE.CylinderGeometry(0.011, 0.011, 0.5, 14).translate(0, 0.25, 0)
    },
    { // plane handle -> rounded square
      match: (t, p) => t === 'BoxGeometry' && p.width === 0.15 && p.depth === 0.01,
      canon: () => new THREE.BoxGeometry(0.15, 0.15, 0.01),
      make: () => roundedSquare(0.14, 0.035)
    },
    { // centre -> soft sphere
      match: (t, p) => t === 'OctahedronGeometry' && p.radius === 0.1,
      canon: () => new THREE.OctahedronGeometry(0.1, 0),
      make: () => new THREE.SphereGeometry(0.055, 24, 16)
    },
    { // rotation rings -> round, smooth tube
      match: (t, p) => t === 'TorusGeometry' && p.tube === 0.0075,
      canon: (p) => ring(p.radius, p.arc, 0.0075, 3, 64),
      make: (p) => ring(p.radius, p.arc, 0.012, 10, 128)
    }
  ];
  tc.getHelper().traverse((o) => {
    if (!o.isMesh || !o.material || o.material.visible === false) return;
    const g = o.geometry, p = g.parameters || {};
    const r = recipes.find((x) => x.match(g.type, p));
    if (!r) return;
    const M = bakedMatrix(g, r.canon(p));
    if (!M) return;
    const ng = r.make(p).applyMatrix4(M);
    ng.computeBoundingSphere();
    o.geometry = ng;
    if (g.type === 'BoxGeometry') { o.material.side = THREE.DoubleSide; o.material.opacity = 0.42; }
    o.material.toneMapped = false;
  });
}

export class MoveTool extends EventTarget {
  constructor(viewer, readoutEl) {
    super();
    this.viewer = viewer;
    this.readout = readoutEl;
    this.active = false;
    this.target = null;
    this.snapMode = 'magnetic';
    this.tStep = 1;
    this.rStep = 15;
    this.learned = []; // angles (deg) the user has chosen before
    this.mods = { shift: false, ctrl: false };
    this._speed = 0;
    this._last = null;

    const mk = (mode, size) => {
      const tc = new TransformControls(viewer.camera, viewer.canvas);
      tc.setMode(mode);
      tc.setSize(size);
      tc.enabled = false;
      const helper = tc.getHelper();
      helper.visible = false;
      viewer.overlay.add(helper);
      tc.addEventListener('change', () => viewer.requestRender());
      tc.addEventListener('dragging-changed', (e) => this._dragChanged(tc, e.value));
      tc.addEventListener('objectChange', () => { this._post(tc); this.dispatchEvent(new Event('change')); });
      return { tc, helper };
    };
    ({ tc: this.tcT, helper: this.helperT } = mk('translate', 0.9));
    ({ tc: this.tcR, helper: this.helperR } = mk('rotate', 1.18));
    styleGizmo(this.tcT);
    styleGizmo(this.tcR);
    this.tcR.showXYZE = false; // the free-rotate ball would swallow clicks on the arrows
    this.tcR.showE = false;
    this.tc = this.tcT; // compatibility (mode/space queries)

    // decide which gizmo owns the drag before either sees the event
    viewer.canvas.addEventListener('pointerdown', () => {
      if (!this.target) return;
      if (this.tcT.axis !== null) this.tcR.enabled = false;
      else if (this.tcR.axis !== null) this.tcT.enabled = false;
    }, { capture: true });
    window.addEventListener('pointerup', () => {
      if (this.target) { this.tcT.enabled = true; this.tcR.enabled = true; }
    });
    const keys = (e) => { this.mods.shift = e.shiftKey; this.mods.ctrl = e.ctrlKey || e.altKey; };
    window.addEventListener('keydown', keys);
    window.addEventListener('keyup', keys);
    viewer.canvas.addEventListener('pointermove', (e) => { this._px = e.clientX; this._py = e.clientY; keys(e); });
    viewer.addEventListener('camera', () => { this.tcT.camera = viewer.camera; this.tcR.camera = viewer.camera; });
  }

  get hovering() { return !!this.target && (this.tcT.axis !== null || this.tcR.axis !== null); }
  get dragging() { return this.tcT.dragging || this.tcR.dragging; }

  setActive(on) {
    this.active = on;
    this.attach(on ? this.viewer.selected : null);
  }

  attach(obj) {
    this.target = obj && this.active ? obj : null;
    for (const [tc, helper] of [[this.tcT, this.helperT], [this.tcR, this.helperR]]) {
      if (this.target) { tc.attach(ensurePivot(this.target, this.viewer)); tc.enabled = true; helper.visible = true; }
      else { tc.detach(); tc.enabled = false; helper.visible = false; }
    }
    this.dispatchEvent(new Event('change'));
    this.viewer.requestRender();
  }

  setSpace(space) { this.tcT.setSpace(space); this.tcR.setSpace(space); }
  setSnap(mode, tStep = this.tStep, rStep = this.rStep) { this.snapMode = mode; this.tStep = tStep; this.rStep = rStep; }

  _effectiveMode() {
    if (this.mods.ctrl) return 'off';
    if (this.mods.shift) return 'step';
    return this.snapMode;
  }

  _dragChanged(tc, dragging) {
    this.viewer.controls.enabled = !this.dragging;
    if (dragging) {
      this._last = null; this._speed = 0; this._lastFree = null;
      this._showReadout(true);
    } else {
      this._showReadout(false);
      if (tc === this.tcR && this._lastAngle != null) this._learn(this._lastAngle);
      this.viewer.updateBounds();
      this.dispatchEvent(new Event('committed'));
    }
  }

  _learn(deg) {
    const a = Math.round(Math.abs(deg) * 10) / 10;
    if (a < 1 || a % 5 === 0) return; // multiples of 5° are magnets anyway
    this.learned = [a, ...this.learned.filter((x) => Math.abs(x - a) > 0.5)].slice(0, 6);
    this.dispatchEvent(new Event('learned'));
  }

  _magnetAngle(deg) {
    // capture radius scales with angular speed (deg/ms, smoothed): slow = precise, fast = snappy
    const k = Math.min(2.4, 0.55 + this._speed * 10);
    const targets = [[90, 4.5], [45, 3.2], [15, 2.0], [5, 0.7]];
    for (const [step, r] of targets) {
      const n = Math.round(deg / step) * step;
      if (Math.abs(deg - n) < r * k) return n;
    }
    for (const a of this.learned) {
      for (const n of [a, -a]) if (Math.abs(deg - n) < 2.0 * k) return n;
    }
    return deg;
  }

  _post(tc) {
    const p = tc.object;
    if (!p || !tc.dragging) return;
    const mode = this._effectiveMode();
    const now = performance.now();
    if (tc === this.tcR) {
      const raw = tc.rotationAngle / DEG;
      if (this._last) {
        const dt = Math.max(1, now - this._last.t);
        this._speed = this._speed * 0.7 + (Math.abs(raw - this._last.v) / dt) * 0.3;
      }
      this._last = { t: now, v: raw };
      let deg = raw;
      if (mode === 'step') deg = Math.round(raw / this.rStep) * this.rStep;
      else if (mode === 'magnetic') deg = this._magnetAngle(raw);
      if (deg !== raw) {
        const q = new THREE.Quaternion().setFromAxisAngle(tc.rotationAxis, deg * DEG);
        if (tc.space === 'local') p.quaternion.copy(tc._quaternionStart).multiply(q).normalize();
        else p.quaternion.copy(q).multiply(tc._quaternionStart).normalize();
        p.updateMatrixWorld(true);
      }
      this._lastAngle = deg;
      const snapped = deg !== raw && mode !== 'off';
      this._setReadout(`${deg > 0 ? '+' : ''}${deg.toFixed(1)}°`, snapped);
    } else {
      const axis = tc.axis || '';
      const off = p.position.clone().sub(p.userData.origin);
      let snapped = false;
      if (tc.space === 'world' && mode !== 'off') {
        const wpp = this._worldPerPixel();
        const nice = mode === 'step' ? this.tStep : Math.pow(10, Math.ceil(Math.log10(wpp * 14)));
        const capture = mode === 'step' ? Infinity : wpp * 7;
        for (const [i, c] of [[0, 'X'], [1, 'Y'], [2, 'Z']]) {
          if (!axis.includes(c)) continue;
          const v = off.getComponent(i);
          const n = Math.round(v / nice) * nice;
          if (Math.abs(v - n) < capture) { off.setComponent(i, n); snapped = true; }
        }
        p.position.copy(p.userData.origin).add(off);
        p.updateMatrixWorld(true);
      }
      const f = (v) => (Math.abs(v) < 1e-9 ? '0' : (+v.toFixed(2)).toString());
      this._setReadout(`Δ ${f(off.x)}, ${f(off.y)}, ${f(off.z)} mm`, snapped);
    }
  }

  _worldPerPixel() {
    const cam = this.viewer.camera;
    const el = this.viewer.canvas;
    if (cam.isPerspectiveCamera) {
      const at = this.tcT.object ? this.tcT.object.getWorldPosition(new THREE.Vector3()) : this.viewer.controls.target;
      return (2 * cam.position.distanceTo(at) * Math.tan((cam.fov * DEG) / 2)) / el.clientHeight;
    }
    return (cam.top - cam.bottom) / cam.zoom / el.clientHeight;
  }

  _showReadout(on) { if (this.readout) this.readout.classList.toggle('show', on); }
  _setReadout(text, snapped) {
    if (!this.readout) return;
    this.readout.textContent = text;
    this.readout.classList.toggle('snapped', snapped);
    this.readout.style.transform = `translate(${(this._px || 0) + 18}px, ${(this._py || 0) + 18}px)`;
  }

  values() {
    const p = this.target && pivotOf(this.target);
    if (!p) return null;
    const t = p.position.clone().sub(p.userData.origin);
    const r = [p.rotation.x, p.rotation.y, p.rotation.z].map((a) => THREE.MathUtils.radToDeg(a));
    return { t: t.toArray(), r };
  }

  setValues({ t, r }) {
    const p = this.target && pivotOf(this.target);
    if (!p) return;
    if (t) p.position.copy(p.userData.origin).add(new THREE.Vector3(...t));
    if (r) {
      const old = [p.rotation.x, p.rotation.y, p.rotation.z].map((a) => a / DEG);
      r.forEach((v, i) => { if (Math.abs(v - old[i]) > 1e-6) this._learn(v); });
      p.rotation.set(...r.map((a) => a * DEG));
    }
    p.updateMatrixWorld(true);
    this.viewer.updateBounds();
    this.viewer.requestRender();
    this.dispatchEvent(new Event('committed'));
  }

  reset() { this.setValues({ t: [0, 0, 0], r: [0, 0, 0] }); }
}
