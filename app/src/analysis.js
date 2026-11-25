import * as THREE from 'three';
import { isModelMesh } from './viewer.js';

// ------------------------------------------------------------------ mass properties from meshes
/** Symmetric 3x3 eigenvalues (Jacobi). */
function eig3(A) {
  const a = A.map((r) => r.slice());
  for (let s = 0; s < 50; s++) {
    const off = Math.abs(a[0][1]) + Math.abs(a[0][2]) + Math.abs(a[1][2]);
    if (off < 1e-14 * (Math.abs(a[0][0]) + Math.abs(a[1][1]) + Math.abs(a[2][2]) + 1e-30)) break;
    for (let p = 0; p < 2; p++) for (let q = p + 1; q < 3; q++) {
      if (Math.abs(a[p][q]) < 1e-30) continue;
      const th = (a[q][q] - a[p][p]) / (2 * a[p][q]);
      const t = (th >= 0 ? 1 : -1) / (Math.abs(th) + Math.sqrt(th * th + 1));
      const c = 1 / Math.sqrt(t * t + 1), sn = t * c;
      for (let k = 0; k < 3; k++) { const x = a[k][p], y = a[k][q]; a[k][p] = c * x - sn * y; a[k][q] = sn * x + c * y; }
      for (let k = 0; k < 3; k++) { const x = a[p][k], y = a[q][k]; a[p][k] = c * x - sn * y; a[q][k] = sn * x + c * y; }
    }
  }
  return [a[0][0], a[1][1], a[2][2]].sort((x, y) => y - x);
}

/**
 * Volume, area, centre of gravity and inertia of closed triangle meshes (divergence theorem;
 * signed tetrahedra to the origin). density in g/mm³ per mesh. Inertia about the CoG in g·mm².
 */
export function meshMassProps(meshes, densityOf) {
  let V = 0, M = 0, area = 0;
  const S1 = [0, 0, 0];                 // ∫ρ x dV
  const S2 = [[0, 0, 0], [0, 0, 0], [0, 0, 0]]; // ∫ρ x xᵀ dV
  const a = new THREE.Vector3(), b = new THREE.Vector3(), c = new THREE.Vector3();
  for (const m of meshes) {
    const rho = densityOf(m);
    const g = m.geometry, P = g.attributes.position, I = g.index ? g.index.array : null;
    const nT = I ? I.length / 3 : P.count / 3;
    m.updateMatrixWorld(true);
    const mw = m.matrixWorld;
    for (let t = 0; t < nT; t++) {
      a.fromBufferAttribute(P, I ? I[t * 3] : t * 3).applyMatrix4(mw);
      b.fromBufferAttribute(P, I ? I[t * 3 + 1] : t * 3 + 1).applyMatrix4(mw);
      c.fromBufferAttribute(P, I ? I[t * 3 + 2] : t * 3 + 2).applyMatrix4(mw);
      const cx = b.y * c.z - b.z * c.y, cy = b.z * c.x - b.x * c.z, cz = b.x * c.y - b.y * c.x;
      const v = (a.x * cx + a.y * cy + a.z * cz) / 6;
      const ux = b.x - a.x, uy = b.y - a.y, uz = b.z - a.z, wx = c.x - a.x, wy = c.y - a.y, wz = c.z - a.z;
      area += 0.5 * Math.hypot(uy * wz - uz * wy, uz * wx - ux * wz, ux * wy - uy * wx);
      V += v; M += rho * v;
      const s = [a.x + b.x + c.x, a.y + b.y + c.y, a.z + b.z + c.z];
      for (let i = 0; i < 3; i++) S1[i] += (rho * v * s[i]) / 4;
      const pts = [[a.x, a.y, a.z], [b.x, b.y, b.z], [c.x, c.y, c.z]];
      for (let i = 0; i < 3; i++) for (let j = 0; j < 3; j++) {
        let q = s[i] * s[j];
        for (const p of pts) q += p[i] * p[j];
        S2[i][j] += (rho * v * q) / 20;
      }
    }
  }
  const sign = M < 0 ? -1 : 1; // inside-out meshes
  M *= sign; V *= sign;
  for (let i = 0; i < 3; i++) { S1[i] *= sign; for (let j = 0; j < 3; j++) S2[i][j] *= sign; }
  const cog = M > 0 ? S1.map((x) => x / M) : [0, 0, 0];
  // covariance about CoG, then inertia tensor I = tr(C)·E − C
  const C = [0, 1, 2].map((i) => [0, 1, 2].map((j) => S2[i][j] - M * cog[i] * cog[j]));
  const tr = C[0][0] + C[1][1] + C[2][2];
  const In = [0, 1, 2].map((i) => [0, 1, 2].map((j) => (i === j ? tr : 0) - C[i][j]));
  return { exact: false, volume: V, area, mass: M, cog, inertia: In.flat(), principal: eig3(In) };
}

// ------------------------------------------------------------------ AM analysis
const OK = new THREE.Color(0.80, 0.83, 0.87);
const PLATE = new THREE.Color(0.25, 0.55, 0.95);
const WARN = new THREE.Color(1.0, 0.75, 0.15);
const BAD = new THREE.Color(0.93, 0.22, 0.2);
const GOOD = new THREE.Color(0.25, 0.78, 0.45);

/** Lambert material that takes its colour from an 'acolor' vertex attribute (keeps file colours untouched). */
function makeAnalysisMaterial() {
  const m = new THREE.MeshLambertMaterial({ color: 0xffffff, side: THREE.DoubleSide, polygonOffset: true, polygonOffsetFactor: 1, polygonOffsetUnits: 1 });
  m.userData.noHighlight = true;
  m.onBeforeCompile = (sh) => {
    sh.vertexShader = sh.vertexShader
      .replace('#include <common>', '#include <common>\nattribute vec3 acolor;\nvarying vec3 vAc;')
      .replace('#include <begin_vertex>', '#include <begin_vertex>\nvAc = acolor;');
    sh.fragmentShader = sh.fragmentShader
      .replace('#include <common>', '#include <common>\nvarying vec3 vAc;')
      .replace('#include <color_fragment>', '#include <color_fragment>\ndiffuseColor.rgb *= vAc;');
  };
  return m;
}

function fibonacciDirs(n) {
  const out = [];
  const ga = Math.PI * (3 - Math.sqrt(5));
  for (let i = 0; i < n; i++) {
    const y = 1 - (2 * (i + 0.5)) / n, r = Math.sqrt(1 - y * y), th = i * ga;
    out.push(new THREE.Vector3(Math.cos(th) * r, Math.sin(th) * r, y));
  }
  for (const v of [[1, 0, 0], [-1, 0, 0], [0, 1, 0], [0, -1, 0], [0, 0, 1], [0, 0, -1]]) out.push(new THREE.Vector3(...v));
  return out;
}

export class AMAnalysis extends EventTarget {
  constructor(viewer) {
    super();
    this.viewer = viewer;
    this.active = false;
    this.meshes = [];
    this.dir = new THREE.Vector3(0, 0, 1);
    this.limit = 45;
    this.map = 'overhang';
    this.minWall = 1.0;
    this.mat = makeAnalysisMaterial();
    this.plate = new THREE.Mesh(new THREE.PlaneGeometry(1, 1), new THREE.MeshBasicMaterial({ color: 0x0a84ff, transparent: true, opacity: 0.12, side: THREE.DoubleSide, depthWrite: false }));
    this.plate.raycast = () => {};
    this.plate.visible = false;
    viewer.overlay.add(this.plate);
    this.plateEdge = new THREE.LineLoop(new THREE.BufferGeometry().setFromPoints([[-0.5, -0.5], [0.5, -0.5], [0.5, 0.5], [-0.5, 0.5]].map(([x, y]) => new THREE.Vector3(x, y, 0))), new THREE.LineBasicMaterial({ color: 0x0a84ff, transparent: true, opacity: 0.6 }));
    this.plate.add(this.plateEdge);
    this.stats = null;
    this._job = 0;
  }

  setTargets(root) {
    const list = [];
    if (root) root.traverseVisible((o) => { if (isModelMesh(o)) list.push(o); });
    this.restore();
    this.meshes = list;
    if (this.active) this.apply();
  }

  setActive(on) {
    this.active = on;
    if (on) this.apply(); else { this.restore(); this.plate.visible = false; this.viewer.requestRender(); }
  }

  restore() {
    for (const m of this.meshes) if (m.userData.amOrig) { m.material = m.userData.amOrig; delete m.userData.amOrig; }
    this._job++;
  }

  /** Recompute colours + stats for the current settings. */
  async apply() {
    if (!this.active) return;
    const job = ++this._job;
    const d = this.dir.clone().normalize();
    // build plate height along the build direction
    let hMin = Infinity, hMax = -Infinity;
    const v = new THREE.Vector3();
    for (const m of this.meshes) {
      m.updateMatrixWorld(true);
      const P = m.geometry.attributes.position;
      for (let i = 0; i < P.count; i++) { const h = v.fromBufferAttribute(P, i).applyMatrix4(m.matrixWorld).dot(d); if (h < hMin) hMin = h; if (h > hMax) hMax = h; }
    }
    if (!isFinite(hMin)) return;
    const eps = Math.max((hMax - hMin) * 2e-3, 0.05);
    const cosL = Math.cos(THREE.MathUtils.degToRad(this.limit));
    const cosW = Math.cos(THREE.MathUtils.degToRad(Math.min(89, this.limit + 10)));
    let support = 0, overhangArea = 0, plateArea = 0;
    const n = new THREE.Vector3(), nm = new THREE.Matrix3();
    const a = new THREE.Vector3(), b = new THREE.Vector3(), c = new THREE.Vector3();
    for (const m of this.meshes) {
      const g = m.geometry, P = g.attributes.position, N = g.attributes.normal, I = g.index ? g.index.array : null;
      nm.getNormalMatrix(m.matrixWorld);
      let col = g.attributes.acolor;
      if (!col || col.count !== P.count) { col = new THREE.BufferAttribute(new Float32Array(P.count * 3), 3); g.setAttribute('acolor', col); }
      const arr = col.array;
      if (this.map === 'overhang' || this.map === 'none') {
        for (let i = 0; i < P.count; i++) {
          let cc = OK;
          if (this.map === 'overhang') {
            n.fromBufferAttribute(N, i).applyMatrix3(nm).normalize();
            const h = v.fromBufferAttribute(P, i).applyMatrix4(m.matrixWorld).dot(d);
            const down = -n.dot(d);
            if (h < hMin + eps && down > 0.98) cc = PLATE;
            else if (down > cosL) cc = BAD;
            else if (down > cosW) cc = WARN;
          }
          arr[i * 3] = cc.r; arr[i * 3 + 1] = cc.g; arr[i * 3 + 2] = cc.b;
        }
        col.needsUpdate = true;
      }
      // per-triangle statistics (projected area needing support)
      const nT = I ? I.length / 3 : P.count / 3;
      for (let t = 0; t < nT; t++) {
        a.fromBufferAttribute(P, I ? I[t * 3] : t * 3).applyMatrix4(m.matrixWorld);
        b.fromBufferAttribute(P, I ? I[t * 3 + 1] : t * 3 + 1).applyMatrix4(m.matrixWorld);
        c.fromBufferAttribute(P, I ? I[t * 3 + 2] : t * 3 + 2).applyMatrix4(m.matrixWorld);
        const cr = b.clone().sub(a).cross(c.clone().sub(a));
        const ar = cr.length() / 2;
        if (ar === 0) continue;
        const down = -cr.normalize().dot(d);
        const h = (a.dot(d) + b.dot(d) + c.dot(d)) / 3;
        if (h < hMin + eps && down > 0.98) { plateArea += ar; continue; }
        if (down > cosL) { overhangArea += ar; support += ar * down; }
      }
      if (!m.userData.amOrig) { m.userData.amOrig = m.material; m.material = this.mat; }
    }
    this.mat.clippingPlanes = this.viewer.clipPlanes.length ? this.viewer.clipPlanes : null;
    this.mat.needsUpdate = true;
    this._placePlate(d, hMin);
    this.stats = { height: hMax - hMin, support, overhangArea, plateArea, thickness: null };
    this.dispatchEvent(new Event('stats'));
    this.viewer.requestRender();
    if (this.map === 'thickness') await this._thickness(job);
  }

  _placePlate(d, hMin) {
    const box = new THREE.Box3();
    for (const m of this.meshes) box.union(this.viewer.boxOf(m));
    const c = box.getCenter(new THREE.Vector3());
    const size = box.getSize(new THREE.Vector3()).length() * 1.1;
    const pos = c.clone().addScaledVector(d, hMin - c.dot(d));
    this.plate.position.copy(pos);
    this.plate.quaternion.setFromUnitVectors(new THREE.Vector3(0, 0, 1), d);
    this.plate.scale.set(size, size, 1);
    this.plate.visible = true;
  }

  /** Wall thickness: from every vertex, cast a ray into the material and measure to the opposite wall. */
  async _thickness(job) {
    const ray = new THREE.Ray();
    const n = new THREE.Vector3(), p = new THREE.Vector3();
    let minT = Infinity, thin = 0, total = 0;
    for (const m of this.meshes) {
      const g = m.geometry;
      if (!g.boundsTree) g.computeBoundsTree({ indirect: true });
      const P = g.attributes.position, N = g.attributes.normal;
      const arr = g.attributes.acolor.array;
      const sz = g.boundingSphere ? g.boundingSphere.radius : 1;
      const eps = Math.max(sz * 1e-6, 1e-5);
      let last = performance.now();
      for (let i = 0; i < P.count; i++) {
        if (performance.now() - last > 30) {
          await new Promise((r) => setTimeout(r, 0));
          if (job !== this._job) return;
          last = performance.now();
          this.dispatchEvent(new CustomEvent('progress', { detail: (i / P.count) }));
          g.attributes.acolor.needsUpdate = true;
          this.viewer.requestRender();
        }
        p.fromBufferAttribute(P, i);
        n.fromBufferAttribute(N, i).normalize();
        ray.origin.copy(p).addScaledVector(n, -eps);
        ray.direction.copy(n).negate();
        const hit = g.boundsTree.raycastFirst(ray, THREE.DoubleSide);
        const t = hit ? hit.distance + eps : Infinity;
        let cc = OK;
        if (t < this.minWall) { cc = BAD; thin++; }
        else if (t < this.minWall * 2) cc = new THREE.Color().copy(BAD).lerp(WARN, (t - this.minWall) / this.minWall);
        else if (t < this.minWall * 3) cc = new THREE.Color().copy(WARN).lerp(GOOD, (t - this.minWall * 2) / this.minWall);
        else if (isFinite(t)) cc = GOOD;
        if (t < minT) minT = t;
        total++;
        arr[i * 3] = cc.r; arr[i * 3 + 1] = cc.g; arr[i * 3 + 2] = cc.b;
      }
      g.attributes.acolor.needsUpdate = true;
    }
    if (job !== this._job) return;
    this.stats.thickness = { min: minT, thinShare: total ? thin / total : 0 };
    this.dispatchEvent(new Event('stats'));
    this.dispatchEvent(new CustomEvent('progress', { detail: 1 }));
    this.viewer.requestRender();
  }

  /** Rank build directions by support area and height. Returns the best few. */
  autoOrient() {
    const tris = [];
    const a = new THREE.Vector3(), b = new THREE.Vector3(), c = new THREE.Vector3();
    let total = 0;
    for (const m of this.meshes) total += m.geometry.index ? m.geometry.index.count / 3 : m.geometry.attributes.position.count / 3;
    const stride = Math.max(1, Math.floor(total / 150000));
    for (const m of this.meshes) {
      const g = m.geometry, P = g.attributes.position, I = g.index ? g.index.array : null;
      const nT = I ? I.length / 3 : P.count / 3;
      for (let t = 0; t < nT; t += stride) {
        a.fromBufferAttribute(P, I ? I[t * 3] : t * 3).applyMatrix4(m.matrixWorld);
        b.fromBufferAttribute(P, I ? I[t * 3 + 1] : t * 3 + 1).applyMatrix4(m.matrixWorld);
        c.fromBufferAttribute(P, I ? I[t * 3 + 2] : t * 3 + 2).applyMatrix4(m.matrixWorld);
        const cr = b.clone().sub(a).cross(c.clone().sub(a));
        const ar = (cr.length() / 2) * stride;
        if (ar === 0) continue;
        cr.normalize();
        tris.push(cr.x, cr.y, cr.z, ar, (a.x + b.x + c.x) / 3, (a.y + b.y + c.y) / 3, (a.z + b.z + c.z) / 3);
      }
    }
    const cosL = Math.cos(THREE.MathUtils.degToRad(this.limit));
    const res = [];
    for (const d of fibonacciDirs(120)) {
      let hMin = Infinity, hMax = -Infinity;
      for (let i = 0; i < tris.length; i += 7) { const h = tris[i + 4] * d.x + tris[i + 5] * d.y + tris[i + 6] * d.z; if (h < hMin) hMin = h; if (h > hMax) hMax = h; }
      const eps = Math.max((hMax - hMin) * 2e-3, 0.05);
      let sup = 0;
      for (let i = 0; i < tris.length; i += 7) {
        const down = -(tris[i] * d.x + tris[i + 1] * d.y + tris[i + 2] * d.z);
        if (down <= cosL) continue;
        const h = tris[i + 4] * d.x + tris[i + 5] * d.y + tris[i + 6] * d.z;
        if (h < hMin + eps && down > 0.98) continue; // sits on the plate
        sup += tris[i + 3] * down;
      }
      res.push({ dir: d, support: sup, height: hMax - hMin });
    }
    const maxS = Math.max(...res.map((r) => r.support), 1e-9), maxH = Math.max(...res.map((r) => r.height), 1e-9);
    for (const r of res) {
      r.score = r.support / maxS + 0.35 * (r.height / maxH);
      // printers and slicers favour axis-aligned set-ups: small bonus so near-ties go to them
      const ax = Math.max(Math.abs(r.dir.x), Math.abs(r.dir.y), Math.abs(r.dir.z));
      if (ax > 0.9999) r.score -= 0.04;
    }
    res.sort((x, y) => x.score - y.score);
    // keep distinct directions only
    const out = [];
    for (const r of res) { if (out.every((o) => o.dir.dot(r.dir) < 0.97)) out.push(r); if (out.length === 4) break; }
    return out;
  }
}
