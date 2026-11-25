import * as THREE from 'three';
import { isModelMesh } from './viewer.js';

const AXES = { x: new THREE.Vector3(1, 0, 0), y: new THREE.Vector3(0, 1, 0), z: new THREE.Vector3(0, 0, 1) };
const CAP_LIMIT = 2500; // above this many bodies, skip solid caps (3 draw calls per body)

/**
 * Section plane. Model, edges and tessellation overlay are clipped with a THREE.Plane;
 * solid "caps" are drawn per body using the stencil buffer (back faces +1, front faces -1,
 * then a plane quad where stencil != 0), coloured like a darker version of the body.
 */
export class SectionTool extends EventTarget {
  constructor(viewer) {
    super();
    this.viewer = viewer;
    this.plane = new THREE.Plane(new THREE.Vector3(-1, 0, 0), 0);
    this.state = null; // per document: { on, axis, flip, offset, normal, caps }
    this.capGeo = new THREE.PlaneGeometry(1, 1);
    this.capObjects = [];
    this.outline = new THREE.LineLoop(
      new THREE.BufferGeometry().setFromPoints([new THREE.Vector3(-0.5, -0.5, 0), new THREE.Vector3(0.5, -0.5, 0), new THREE.Vector3(0.5, 0.5, 0), new THREE.Vector3(-0.5, 0.5, 0)]),
      new THREE.LineBasicMaterial({ color: 0x0a84ff, transparent: true, opacity: 0.9, depthTest: false })
    );
    this.outline.renderOrder = 999;
    this.fill = new THREE.Mesh(this.capGeo, new THREE.MeshBasicMaterial({ color: 0x0a84ff, transparent: true, opacity: 0.06, depthWrite: false, side: THREE.DoubleSide }));
    this.helper = new THREE.Group();
    this.helper.add(this.outline, this.fill);
    this.helper.visible = false;
    this.helper.traverse((o) => { o.raycast = () => {}; });
    viewer.overlay.add(this.helper);
    this._buildHandle();
    viewer.renderHooks.push((cam) => this._sizeHandle(cam));
  }

  // ---------------------------------------------------------------- 3D drag handle
  _buildHandle() {
    const mat = new THREE.MeshBasicMaterial({ color: 0x0a84ff, depthTest: false, transparent: true, toneMapped: false });
    const white = new THREE.MeshBasicMaterial({ color: 0xffffff, depthTest: false, transparent: true, toneMapped: false });
    const h = new THREE.Group();
    const shaft = new THREE.Mesh(new THREE.CylinderGeometry(0.11, 0.11, 3.2, 12), mat);
    shaft.rotation.x = Math.PI / 2;
    const tipA = new THREE.Mesh(new THREE.ConeGeometry(0.42, 0.9, 20), mat);
    tipA.rotation.x = Math.PI / 2; tipA.position.z = 2.0;
    const tipB = tipA.clone(); tipB.rotation.x = -Math.PI / 2; tipB.position.z = -2.0;
    const knob = new THREE.Mesh(new THREE.SphereGeometry(0.55, 24, 16), white);
    const ring = new THREE.Mesh(new THREE.TorusGeometry(0.62, 0.13, 10, 32), mat);
    // generous invisible hit volume
    const hit = new THREE.Mesh(new THREE.CylinderGeometry(0.8, 0.8, 5.4, 8), new THREE.MeshBasicMaterial({ visible: false }));
    hit.rotation.x = Math.PI / 2;
    h.add(shaft, tipA, tipB, knob, ring, hit);
    h.traverse((o) => { o.renderOrder = 1002; });
    h.visible = false;
    this.handle = h;
    this.handleMat = mat;
    this.hitMesh = hit;
    this.viewer.overlay.add(h);
  }

  _worldPerPixel(cam, at) {
    const el = this.viewer.canvas;
    if (cam.isPerspectiveCamera) {
      const d = cam.position.distanceTo(at);
      return (2 * d * Math.tan(THREE.MathUtils.degToRad(cam.fov) / 2)) / el.clientHeight;
    }
    return (cam.top - cam.bottom) / cam.zoom / el.clientHeight;
  }

  _sizeHandle(cam) {
    if (!this.handle.visible) return;
    const s = this._worldPerPixel(cam, this.handle.position) * (this.hot ? 18 : 15);
    if (Math.abs(this.handle.scale.x - s) / s > 0.01) {
      this.handle.scale.setScalar(s);
      this.viewer.requestRender();
    }
  }

  _ray(ev) {
    const rc = new THREE.Raycaster();
    rc.setFromCamera(this.viewer.pointerNDC(ev), this.viewer.camera);
    return rc;
  }

  hitHandle(ev) {
    if (!this.handle.visible) return false;
    this.handle.updateMatrixWorld(true);
    return this._ray(ev).intersectObject(this.hitMesh, false).length > 0;
  }

  setHot(on) {
    if (this.hot === on) return;
    this.hot = on;
    this.handleMat.color.set(on ? 0x46a3ff : 0x0a84ff);
    this.viewer.canvas.style.cursor = on ? 'grab' : '';
    this.viewer.requestRender();
  }

  /** Parameter along the line (p0, n) closest to the mouse ray. */
  _paramAt(ev, p0 = this.handle.position, n = this.normal()) {
    const ray = this._ray(ev).ray;
    const b = n.dot(ray.direction);
    const w0 = p0.clone().sub(ray.origin);
    const d = n.dot(w0), e = ray.direction.dot(w0);
    const den = 1 - b * b;
    if (den < 1e-4) return null; // looking straight along the normal
    return (b * e - d) / den;
  }

  beginDrag(ev) {
    // the reference line is frozen for the whole drag: measuring against the moving handle fed the
    // motion back into itself and made the cut jitter
    const p0 = this.handle.position.clone(), n = this.normal();
    const ray = this._ray(ev).ray;
    const steep = 1 - n.dot(ray.direction) ** 2 < 0.02; // nearly looking along the normal → use vertical mouse motion
    this.drag = { d0: this.offsetValue(), p0, n, steep, s0: steep ? null : this._paramAt(ev, p0, n), y0: ev.clientY, wpp: this._worldPerPixel(this.viewer.camera, p0) };
    this.viewer.canvas.style.cursor = 'grabbing';
  }

  dragTo(ev) {
    if (!this.drag) return;
    const g = this.drag;
    const s = g.steep ? null : this._paramAt(ev, g.p0, g.n);
    const delta = s !== null && g.s0 !== null ? s - g.s0 : (g.y0 - ev.clientY) * g.wpp;
    const { lo, hi } = this.range();
    let d = THREE.MathUtils.clamp(g.d0 + delta, lo, hi);
    this.snapped = null;
    if (ev.shiftKey) {
      const sn = this.snapOffset(d, g.wpp * 12);
      if (sn) { d = sn.d; this.snapped = sn; }
    }
    this.state.t = hi > lo ? (d - lo) / (hi - lo) : 0.5;
    this.update();
    this.dispatchEvent(new Event('change'));
  }

  endDrag() {
    this.drag = null;
    this.snapped = null;
    this.viewer.canvas.style.cursor = this.hot ? 'grab' : '';
  }

  // ---------------------------------------------------------------- snapping (Shift)
  /** Offsets along the current normal where a planar face, an axis or a sphere centre lies. Cached per normal. */
  snapTargets() {
    const n = this.normal();
    const key = n.toArray().map((x) => x.toFixed(5)).join(',');
    if (this._snaps && this._snaps.key === key) return this._snaps.list;
    const list = [];
    const v = new THREE.Vector3(), w = new THREE.Vector3();
    const PAR = Math.cos(THREE.MathUtils.degToRad(0.5)), PERP = Math.sin(THREE.MathUtils.degToRad(0.5));
    this.viewer.forEachMesh((m) => {
      const f = m.userData.faces;
      if (!f || !f.prims || (m.visible === false)) return;
      const M = m.matrixWorld;
      for (const p of f.prims) {
        if (!p) continue;
        if (p.t === 'plane') {
          w.set(...p.n).transformDirection(M);
          if (Math.abs(w.dot(n)) < PAR) continue;
          list.push({ d: v.set(...p.p).applyMatrix4(M).dot(n), kind: 'face', out: Math.sign(w.dot(n)) });
        } else if (p.t === 'cylinder' || p.t === 'cone' || p.t === 'torus') {
          w.set(...p.a).transformDirection(M);
          if (Math.abs(w.dot(n)) > PERP) continue; // the axis must lie in the cut plane
          list.push({ d: v.set(...p.o).applyMatrix4(M).dot(n), kind: 'axis' });
        } else if (p.t === 'sphere') {
          list.push({ d: v.set(...p.c).applyMatrix4(M).dot(n), kind: 'centre' });
        }
      }
    });
    const c = this.viewer.modelCenter.dot(n);
    list.push({ d: c, kind: 'centre of the model' });
    list.sort((a, b) => a.d - b.d);
    // merge duplicates (prefer axis > face labels when coincident)
    const out = [];
    for (const x of list) {
      const last = out[out.length - 1];
      if (last && Math.abs(last.d - x.d) < 1e-4) {
        if (x.kind === 'axis') { last.kind = 'axis'; last.out = 0; }
        else if (x.out && last.out && x.out !== last.out) last.out = 0; // faces on both sides: material both ways
        continue;
      }
      out.push(x);
    }
    this._snaps = { key, list: out };
    return out;
  }

  /** Tiny shift into the material for cuts that land exactly on a face (avoids a cap z-fighting with the face). */
  _faceEps() { const { lo, hi } = this.range(); return Math.max(hi - lo, 1e-6) * 2e-4; }

  snapOffset(d, tol) {
    let best = null;
    for (const x of this.snapTargets()) {
      const e = Math.abs(x.d - d);
      if (e <= tol && (!best || e < Math.abs(best.d - d))) best = x;
    }
    if (!best) return null;
    return { kind: best.kind, d: best.d - (best.out || 0) * this._faceEps(), at: best.d };
  }

  /**
   * Shift+click: put the cut on a picked face. Planar face → cut lies in that face;
   * cylinder / cone → cut contains its axis (turned towards the viewer); sphere → through its centre.
   * Returns a short description, or null if the face gives no direction.
   */
  alignToHit(hit) {
    const f = hit.object.userData.faces;
    const fot = f && f.faceOfTri;
    let prim = null;
    if (f && f.prims && f.ranges && hit.faceIndex != null) {
      const fid = fot ? fot[hit.faceIndex] : this._faceIdOf(f.ranges, hit.faceIndex);
      prim = fid >= 0 ? f.prims[fid] : null;
    }
    const M = hit.object.matrixWorld;
    const cam = this.viewer.camera;
    const toCam = cam.position.clone().sub(hit.point).normalize();
    let n, at, what, out = null;
    if (prim && prim.t === 'plane') {
      n = new THREE.Vector3(...prim.n).transformDirection(M);
      out = n.clone();
      at = new THREE.Vector3(...prim.p).applyMatrix4(M);
      what = 'face';
    } else if (prim && (prim.t === 'cylinder' || prim.t === 'cone' || prim.t === 'torus')) {
      const a = new THREE.Vector3(...prim.a).transformDirection(M);
      at = new THREE.Vector3(...prim.o).applyMatrix4(M);
      if (prim.t === 'torus') { n = a.clone(); what = 'torus mid-plane'; }
      else {
        n = toCam.clone().addScaledVector(a, -toCam.dot(a));
        if (n.lengthSq() < 1e-8) n = new THREE.Vector3(1, 0, 0).cross(a);
        if (n.lengthSq() < 1e-8) n = new THREE.Vector3(0, 1, 0).cross(a);
        n.normalize();
        what = 'axis';
      }
    } else if (prim && prim.t === 'sphere') {
      at = new THREE.Vector3(...prim.c).applyMatrix4(M);
      n = toCam.clone();
      what = 'sphere centre';
    } else if (hit.face) {
      n = hit.face.normal.clone().transformDirection(M);
      out = n.clone();
      at = hit.point.clone();
      what = 'face';
    } else return null;
    // remove the side that faces the camera, so the section is what you look at
    if (n.dot(toCam) < 0) n.negate();
    const st = this.state;
    const axis = Object.entries(AXES).find(([, v]) => Math.abs(v.dot(n)) > 0.99999);
    if (axis) { st.axis = axis[0]; st.normal = null; st.flip = AXES[axis[0]].dot(n) < 0; }
    else { st.axis = 'view'; st.normal = n.clone(); st.flip = false; }
    delete st._r;
    const nn = this.normal(st);
    const { lo, hi } = this.range(st);
    let d = at.dot(nn);
    if (out) d -= Math.sign(out.dot(nn)) * (hi - lo) * 2e-4; // just inside the material so the section shows
    st.t = hi > lo ? THREE.MathUtils.clamp((d - lo) / (hi - lo), 0, 1) : 0.5;
    this.apply(st);
    return what;
  }

  _faceIdOf(ranges, tri) {
    for (let i = 0; i < ranges.length; i += 2) if (tri >= ranges[i] && tri <= ranges[i + 1]) return i / 2;
    return -1;
  }

  static defaultState() { return { on: false, axis: 'x', flip: false, t: 0.5, normal: null, caps: true }; }

  /** Range of the plane offset along the current normal, from the model bounds. */
  range(st = this.state) {
    const n = this.normal(st);
    // frozen per normal: re-tessellation / small bound changes must not move the cut under the slider
    const key = n.toArray().map((x) => x.toFixed(6)).join(',');
    if (st._r && st._r.key === key) return st._r;
    const b = this.viewer.modelBox;
    let lo = Infinity, hi = -Infinity;
    for (let i = 0; i < 8; i++) {
      const p = new THREE.Vector3(i & 1 ? b.max.x : b.min.x, i & 2 ? b.max.y : b.min.y, i & 4 ? b.max.z : b.min.z);
      const d = p.dot(n);
      lo = Math.min(lo, d); hi = Math.max(hi, d);
    }
    st._r = { key, lo, hi };
    return st._r;
  }

  normal(st = this.state) {
    const n = st.axis === 'view' && st.normal ? st.normal.clone() : AXES[st.axis === 'view' ? 'x' : st.axis].clone();
    return n;
  }

  /** Activate the given document's state (called on tab switch and on every change). */
  apply(st) {
    this.state = st;
    this._snaps = null;
    this._clearCaps();
    const v = this.viewer;
    if (!st || !st.on || !v.model) {
      v.setClipPlanes([]);
      this.helper.visible = false;
      this.handle.visible = false;
      v.requestRender();
      return;
    }
    this.update();
    v.setClipPlanes([this.plane]);
    if (st.caps) this._buildCaps();
  }

  /** Position the plane from state (cheap; used while dragging the slider). */
  update() {
    const st = this.state;
    if (!st || !st.on) return;
    const n = this.normal(st);
    const { lo, hi } = this.range(st);
    const d = lo + (hi - lo) * st.t;
    // Plane keeps the side where n·p < d (i.e. removes material in +n direction); flip reverses.
    const keepN = st.flip ? n : n.clone().negate();
    this.plane.normal.copy(keepN);
    this.plane.constant = st.flip ? -d : d;
    // helper + caps
    const v = this.viewer;
    const c = v.modelCenter.clone();
    const onPlane = c.addScaledVector(n, d - c.dot(n));
    const size = v.modelRadius * 2.6; // caps must cover the whole section
    const hsize = v.modelRadius * 1.7;
    const q = new THREE.Quaternion().setFromUnitVectors(new THREE.Vector3(0, 0, 1), n);
    this.helper.position.copy(onPlane);
    this.helper.quaternion.copy(q);
    this.helper.scale.set(hsize, hsize, 1);
    this.helper.visible = true;
    this.handle.position.copy(onPlane);
    this.handle.quaternion.copy(q);
    this.handle.visible = true;
    for (const cap of this.capObjects) {
      if (!cap.userData.isCap) continue; // stencil passes stay attached to their bodies untouched
      cap.position.copy(onPlane);
      cap.quaternion.copy(q);
      cap.scale.set(size, size, 1);
    }
    v.requestRender();
    return d;
  }

  offsetValue() {
    const { lo, hi } = this.range();
    return lo + (hi - lo) * this.state.t;
  }

  setOffsetValue(d) {
    const { lo, hi } = this.range();
    this.state.t = hi > lo ? THREE.MathUtils.clamp((d - lo) / (hi - lo), 0, 1) : 0.5;
    this.update();
  }

  _buildCaps() {
    const v = this.viewer;
    const meshes = [];
    v.forEachMesh((m) => meshes.push(m));
    if (meshes.length > CAP_LIMIT) return;
    const base = new THREE.MeshBasicMaterial({ depthWrite: false, depthTest: false, colorWrite: false, stencilWrite: true, stencilFunc: THREE.AlwaysStencilFunc });
    const back = base.clone();
    back.side = THREE.BackSide;
    back.clippingPlanes = [this.plane];
    back.stencilFail = back.stencilZFail = back.stencilZPass = THREE.IncrementWrapStencilOp;
    const front = base.clone();
    front.side = THREE.FrontSide;
    front.clippingPlanes = [this.plane];
    front.stencilFail = front.stencilZFail = front.stencilZPass = THREE.DecrementWrapStencilOp;
    const capMats = new Map();
    meshes.forEach((m, i) => {
      const order = 10 + i * 2;
      for (const mat of [back, front]) {
        const s = new THREE.Mesh(m.geometry, mat);
        s.userData.isOverlay = true;
        s.raycast = () => {};
        s.renderOrder = order;
        m.add(s);
        this.capObjects.push(s);
      }
      const src = Array.isArray(m.material) ? m.material[0] : m.material;
      const key = src.color.getHexString() + (src.vertexColors ? 'v' : '');
      let cm = capMats.get(key);
      if (!cm) {
        cm = new THREE.MeshStandardMaterial({
          color: src.vertexColors ? 0x9aa3ae : src.color.clone().multiplyScalar(0.78),
          metalness: 0.0, roughness: 0.8, side: THREE.DoubleSide,
          stencilWrite: true, stencilRef: 0, stencilFunc: THREE.NotEqualStencilFunc,
          stencilFail: THREE.ReplaceStencilOp, stencilZFail: THREE.ReplaceStencilOp, stencilZPass: THREE.ReplaceStencilOp
        });
        capMats.set(key, cm);
      }
      const cap = new THREE.Mesh(this.capGeo, cm);
      cap.renderOrder = order + 1;
      cap.raycast = () => {};
      cap.userData.isOverlay = true;
      cap.userData.isCap = true;
      cap.onAfterRender = (r) => r.clearStencil();
      v.overlay.add(cap);
      this.capObjects.push(cap);
    });
    this.update();
  }

  _clearCaps() {
    for (const o of this.capObjects) {
      o.parent?.remove(o);
      if (o.userData.isCap && !o.material.userData.disposed) { o.material.dispose(); o.material.userData.disposed = true; }
    }
    this.capObjects = [];
  }

  /** Model bounds changed: keep the cut where it is in space and re-derive the slider range. */
  rebase() {
    const st = this.state;
    if (!st) return;
    this._snaps = null;
    if (!st.on || !st._r) { delete st._r; return; }
    const d = this.offsetValue();
    delete st._r;
    const { lo, hi } = this.range(st);
    st.t = hi > lo ? THREE.MathUtils.clamp((d - lo) / (hi - lo), 0, 1) : 0.5;
    this.update();
  }

  /** Re-apply after the model changed (parts inserted / moved / re-tessellated). */
  refresh() { if (this.state?.on) this.apply(this.state); }
}
