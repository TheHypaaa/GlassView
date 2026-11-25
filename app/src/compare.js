// Compare two versions: the active model is coloured by its signed distance to another file
// (red = material added / outside the other version, blue = removed / inside, green = within ±tolerance).
import * as THREE from 'three';
import { MeshBVH } from 'three-mesh-bvh';
import { isModelMesh } from './viewer.js';

const GREY = new THREE.Color('#8c919e');
const lerpC = (a, b, t) => new THREE.Color(a).lerp(new THREE.Color(b), t);

export class Compare {
  constructor(viewer) {
    this.viewer = viewer;
    this.active = false;
    this.tol = 0.05;
    this.range = 0;
    this.ghost = true;
    this.otherName = '';
    this.stats = null;
    this.clones = [];      // coloured copies of the model meshes
    this.hidden = [];      // original meshes hidden while comparing
    this.ghostObj = null;
    this.otherRoot = null;
    this.tip = document.createElement('div');
    this.tip.className = 'cmp-tip';
    document.body.appendChild(this.tip);
  }

  colorFor(d, out = new THREE.Color()) {
    if (!Number.isFinite(d)) return out.copy(GREY);
    const R = Math.max(this.range, this.tol * 1.0001);
    if (Math.abs(d) <= this.tol) return out.set('#3dbf6b');
    const t = Math.min(1, (Math.abs(d) - this.tol) / Math.max(1e-12, R - this.tol));
    return out.copy(d > 0 ? lerpC('#ffcc33', '#eb2826', t) : lerpC('#4dccf2', '#2452eb', t));
  }

  /** CSS gradient for the legend bar. */
  legendCss() {
    const stops = [];
    for (let i = 0; i <= 24; i++) { const d = -this.range + (2 * this.range * i) / 24; stops.push(`#${this.colorFor(d).getHexString()} ${(i / 24 * 100).toFixed(1)}%`); }
    return `linear-gradient(90deg, ${stops.join(', ')})`;
  }

  /**
   * root: the active model (THREE.Object3D); other: the loaded other version (its root group).
   * Builds coloured copies of the model; returns statistics.
   */
  run(root, other, otherName) {
    this.clear();
    this.active = true;
    this.otherName = otherName;
    this.otherRoot = other;
    root.updateMatrixWorld(true);
    other.updateMatrixWorld(true);
    // ---- the other version as one world-space geometry with a BVH
    const pos = [], idx = [];
    let base = 0;
    const v = new THREE.Vector3();
    other.traverse((m) => {
      if (!m.isMesh || !m.geometry?.attributes?.position) return;
      const P = m.geometry.attributes.position, I = m.geometry.index;
      for (let i = 0; i < P.count; i++) { v.fromBufferAttribute(P, i).applyMatrix4(m.matrixWorld); pos.push(v.x, v.y, v.z); }
      if (I) for (let i = 0; i < I.count; i++) idx.push(base + I.getX(i));
      else for (let i = 0; i < P.count; i++) idx.push(base + i);
      base += P.count;
    });
    if (!idx.length) throw new Error('the other file has no geometry');
    const og = new THREE.BufferGeometry();
    og.setAttribute('position', new THREE.Float32BufferAttribute(pos, 3));
    og.setIndex(idx);
    const bvh = new MeshBVH(og);
    const OP = og.attributes.position, OI = og.index;
    const a = new THREE.Vector3(), b = new THREE.Vector3(), c = new THREE.Vector3(), n = new THREE.Vector3();

    const box = this.viewer.boxOf(root);
    const diag = box.isEmpty() ? 100 : box.getSize(new THREE.Vector3()).length();
    const maxD = diag * 0.1, maxEdge = diag / 120;
    const target = {};
    let sum = 0, cnt = 0, within = 0, total = 0;
    this.maxPos = 0; this.maxNeg = 0;
    const meshes = [];
    root.traverse((m) => { if (isModelMesh(m) && m.visible) meshes.push(m); });
    for (const m of meshes) {
      // non-indexed copy, long triangles split so the colours show detail inside big faces
      const src = m.geometry.index ? m.geometry.toNonIndexed() : m.geometry.clone();
      const g = refine(src, m.matrixWorld, maxEdge);
      const P = g.attributes.position;
      const dev = new Float32Array(P.count);
      const cols = new Float32Array(P.count * 3);
      const wp = new THREE.Vector3(), col = new THREE.Color();
      for (let i = 0; i < P.count; i++) {
        wp.fromBufferAttribute(P, i).applyMatrix4(m.matrixWorld);
        const hit = bvh.closestPointToPoint(wp, target, 0, maxD);
        let d = NaN;
        if (hit) {
          const f = hit.faceIndex;
          a.fromBufferAttribute(OP, OI.getX(f * 3)); b.fromBufferAttribute(OP, OI.getX(f * 3 + 1)); c.fromBufferAttribute(OP, OI.getX(f * 3 + 2));
          n.subVectors(b, a).cross(c.clone().sub(a));
          d = hit.distance * (wp.clone().sub(hit.point).dot(n) >= 0 ? 1 : -1);
          sum += Math.abs(d); cnt++;
          if (Math.abs(d) <= this.tol) within++;
          this.maxPos = Math.max(this.maxPos, d); this.maxNeg = Math.min(this.maxNeg, d);
        }
        total++;
        dev[i] = d;
      }
      if (this.range <= 0) this.range = Math.max(this.tol * 4, Math.abs(this.maxPos), Math.abs(this.maxNeg));
      g.setAttribute('color', new THREE.BufferAttribute(cols, 3));
      g.userData.dev = dev;
      g.computeVertexNormals();
      g.computeBoundsTree?.();
      const mat = new THREE.MeshStandardMaterial({ vertexColors: true, roughness: 0.62, metalness: 0.0, side: THREE.DoubleSide, polygonOffset: true, polygonOffsetFactor: 1, polygonOffsetUnits: 1 });
      const clone = new THREE.Mesh(g, mat);
      clone.matrixAutoUpdate = false;
      clone.matrix.copy(m.matrixWorld);
      clone.userData.isOverlay = true;
      clone.userData.isCompare = true;
      this.viewer.scene.add(clone);
      this.clones.push(clone);
      // hide the original faces (edges stay – they draw the CAD outline over the colour map)
      this.hidden.push([m, m.layers.mask]);
      m.layers.set(30);
    }
    this.stats = { mean: cnt ? sum / cnt : 0, within: total ? within / total : 0 };
    this.recolour();
    // ---- ghost of the other version
    this.ghostObj = other;
    other.traverse((m) => {
      if (!m.isMesh) return;
      m.material = new THREE.MeshBasicMaterial({ color: 0x5a6070, transparent: true, opacity: 0.16, depthWrite: false, side: THREE.DoubleSide });
      m.userData.isOverlay = true;
      m.raycast = () => {};
    });
    other.userData.isOverlay = true;
    other.visible = this.ghost;
    this.viewer.scene.add(other);
    this.viewer.requestRender();
    return this.stats;
  }

  recolour() {
    const col = new THREE.Color();
    let within = 0, total = 0;
    for (const c of this.clones) {
      const g = c.geometry, dev = g.userData.dev, C = g.attributes.color;
      for (let i = 0; i < dev.length; i++) {
        this.colorFor(dev[i], col);
        C.setXYZ(i, col.r, col.g, col.b);
        total++;
        if (Number.isFinite(dev[i]) && Math.abs(dev[i]) <= this.tol) within++;
      }
      C.needsUpdate = true;
    }
    if (this.stats) this.stats.within = total ? within / total : 0;
    this.viewer.requestRender();
  }

  setGhost(on) { this.ghost = on; if (this.ghostObj) this.ghostObj.visible = on; this.viewer.requestRender(); }

  /** Deviation under the cursor (NDC) → number | NaN | null (nothing hit). */
  probe(ndc) {
    if (!this.clones.length) return null;
    const rc = new THREE.Raycaster();
    rc.setFromCamera(ndc, this.viewer.camera);
    rc.firstHitOnly = true;
    const hits = rc.intersectObjects(this.clones, false);
    if (!hits.length) return null;
    const h = hits[0], g = h.object.geometry, dev = g.userData.dev, f = h.faceIndex;
    const ia = g.index ? g.index.getX(f * 3) : f * 3, ib = g.index ? g.index.getX(f * 3 + 1) : f * 3 + 1, ic = g.index ? g.index.getX(f * 3 + 2) : f * 3 + 2;
    const bc = h.barycoord || new THREE.Vector3(1 / 3, 1 / 3, 1 / 3);
    return bc.x * dev[ia] + bc.y * dev[ib] + bc.z * dev[ic];
  }

  clear() {
    for (const c of this.clones) { this.viewer.scene.remove(c); c.geometry.disposeBoundsTree?.(); c.geometry.dispose(); c.material.dispose(); }
    this.clones = [];
    for (const [m, mask] of this.hidden) m.layers.mask = mask;
    this.hidden = [];
    if (this.ghostObj) { this.viewer.scene.remove(this.ghostObj); this.ghostObj.traverse((m) => { if (m.isMesh) { m.geometry.dispose(); m.material.dispose(); } }); }
    this.ghostObj = null;
    this.otherRoot = null;
    this.stats = null;
    this.active = false;
    this.tip.style.display = 'none';
    this.viewer.requestRender();
  }
}

/** Splits triangles longer than maxEdge (in world units) into a barycentric grid (non-indexed geometry). */
function refine(g, M, maxEdge) {
  const P = g.attributes.position;
  if (P.count > 1.2e6) return g;
  const s = new THREE.Vector3();
  M.decompose(new THREE.Vector3(), new THREE.Quaternion(), s);
  const k = (Math.abs(s.x) + Math.abs(s.y) + Math.abs(s.z)) / 3 || 1;
  const out = [];
  const A = new THREE.Vector3(), B = new THREE.Vector3(), C = new THREE.Vector3();
  for (let t = 0; t < P.count; t += 3) {
    A.fromBufferAttribute(P, t); B.fromBufferAttribute(P, t + 1); C.fromBufferAttribute(P, t + 2);
    const L = Math.max(A.distanceTo(B), B.distanceTo(C), C.distanceTo(A)) * k;
    const n = Math.min(16, Math.max(1, Math.ceil(L / maxEdge)));
    if (n === 1) { out.push(A.x, A.y, A.z, B.x, B.y, B.z, C.x, C.y, C.z); continue; }
    const pt = (i, j) => { const u = i / n, v = j / n, w = 1 - u - v; return [A.x * w + B.x * u + C.x * v, A.y * w + B.y * u + C.y * v, A.z * w + B.z * u + C.z * v]; };
    for (let i = 0; i < n; i++)
      for (let j = 0; j < n - i; j++) {
        out.push(...pt(i, j), ...pt(i + 1, j), ...pt(i, j + 1));
        if (j < n - i - 1) out.push(...pt(i + 1, j), ...pt(i + 1, j + 1), ...pt(i, j + 1));
      }
  }
  const r = new THREE.BufferGeometry();
  r.setAttribute('position', new THREE.Float32BufferAttribute(out, 3));
  return r;
}
