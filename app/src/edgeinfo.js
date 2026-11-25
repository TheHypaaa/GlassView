import * as THREE from 'three';

/**
 * Weld coincident points. pts: Float32Array xyz…, returns { id: Int32Array (per point), n }.
 * Open-addressing hash on quantised coordinates — fast enough for millions of points.
 */
export function weld(pts, tol) {
  const n = pts.length / 3;
  const inv = 1 / tol;
  let cap = 1;
  while (cap < n * 2) cap <<= 1;
  const mask = cap - 1;
  const table = new Int32Array(cap).fill(-1);
  const qx = new Int32Array(n), qy = new Int32Array(n), qz = new Int32Array(n);
  const id = new Int32Array(n);
  let u = 0;
  for (let i = 0; i < n; i++) {
    const x = Math.round(pts[i * 3] * inv), y = Math.round(pts[i * 3 + 1] * inv), z = Math.round(pts[i * 3 + 2] * inv);
    let h = (Math.imul(x, 73856093) ^ Math.imul(y, 19349663) ^ Math.imul(z, 83492791)) & mask;
    for (;;) {
      const t = table[h];
      if (t < 0) { table[h] = u; qx[u] = x; qy[u] = y; qz[u] = z; id[i] = u++; break; }
      if (qx[t] === x && qy[t] === y && qz[t] === z) { id[i] = t; break; }
      h = (h + 1) & mask;
    }
  }
  return { id, n: u, lookup: (x, y, z) => {
    x = Math.round(x * inv); y = Math.round(y * inv); z = Math.round(z * inv);
    let h = (Math.imul(x, 73856093) ^ Math.imul(y, 19349663) ^ Math.imul(z, 83492791)) & mask;
    for (;;) {
      const t = table[h];
      if (t < 0) return -1;
      if (qx[t] === x && qy[t] === y && qz[t] === z) return t;
      h = (h + 1) & mask;
    }
  } };
}

const tolOf = (geom) => {
  if (!geom.boundingSphere) geom.computeBoundingSphere();
  return Math.max(1e-6, geom.boundingSphere.radius * 2e-7);
};

/**
 * Snap points of a mesh in its local frame:
 *   corners – B-rep vertices (edge ends / sharp turns), mids – midpoints of straight edges.
 */
export function edgeSnaps(mesh) {
  const E = mesh.userData.edges;
  if (!E) return { corners: new Float32Array(0), mids: new Float32Array(0) };
  const arr = E.geometry.attributes.position.array;
  const nPt = arr.length / 3;
  if (!nPt) return { corners: new Float32Array(0), mids: new Float32Array(0) };
  const w = weld(arr, tolOf(mesh.geometry));
  // unique segments (edges are written once per adjacent face)
  const seen = new Set();
  const deg = new Int32Array(w.n), nb = new Int32Array(w.n * 2).fill(-1);
  const segs = [];
  for (let s = 0; s < nPt / 2; s++) {
    let a = w.id[s * 2], b = w.id[s * 2 + 1];
    if (a === b) continue;
    if (a > b) [a, b] = [b, a];
    const key = a * 4294967296 + b;
    if (seen.has(key)) continue;
    seen.add(key);
    segs.push(s);
    for (const [p, q] of [[a, b], [b, a]]) { if (deg[p] < 2) nb[p * 2 + deg[p]] = q; deg[p]++; }
  }
  const pos = new Float32Array(w.n * 3);
  for (let i = 0; i < nPt; i++) { const t = w.id[i]; pos[t * 3] = arr[i * 3]; pos[t * 3 + 1] = arr[i * 3 + 1]; pos[t * 3 + 2] = arr[i * 3 + 2]; }
  const isCorner = new Uint8Array(w.n);
  const v1 = new THREE.Vector3(), v2 = new THREE.Vector3();
  const COS = Math.cos(THREE.MathUtils.degToRad(40));
  const corners = [];
  for (let p = 0; p < w.n; p++) {
    if (!deg[p]) continue;
    let c = deg[p] !== 2;
    if (!c) {
      const a = nb[p * 2], b = nb[p * 2 + 1];
      v1.set(pos[p * 3] - pos[a * 3], pos[p * 3 + 1] - pos[a * 3 + 1], pos[p * 3 + 2] - pos[a * 3 + 2]).normalize();
      v2.set(pos[b * 3] - pos[p * 3], pos[b * 3 + 1] - pos[p * 3 + 1], pos[b * 3 + 2] - pos[p * 3 + 2]).normalize();
      c = v1.dot(v2) < COS;
    }
    if (c) { isCorner[p] = 1; corners.push(pos[p * 3], pos[p * 3 + 1], pos[p * 3 + 2]); }
  }
  const mids = [];
  for (const s of segs) {
    const a = w.id[s * 2], b = w.id[s * 2 + 1];
    if (!isCorner[a] || !isCorner[b]) continue;
    mids.push((pos[a * 3] + pos[b * 3]) / 2, (pos[a * 3 + 1] + pos[b * 3 + 1]) / 2, (pos[a * 3 + 2] + pos[b * 3 + 2]) / 2);
  }
  return { corners: new Float32Array(corners), mids: new Float32Array(mids) };
}

/**
 * Hard / soft edges, CATIA style: a segment is "soft" when the two faces meeting there are
 * tangent-continuous (their surface normals agree along it). Returns Float32Array per edge vertex
 * (0 hard, 1 soft) or null when the mesh has no B-rep faces.
 */
export function classifyEdges(mesh, maxDeg = 4) {
  const E = mesh.userData.edges, F = mesh.userData.faces;
  if (!E || !F || !F.ranges) return null;
  const ea = E.geometry.attributes.position.array;
  const nEp = ea.length / 3;
  const out = new Float32Array(nEp);
  if (!nEp) return out;
  const g = mesh.geometry;
  const P = g.attributes.position.array, N = g.attributes.normal ? g.attributes.normal.array : null;
  if (!N) return out;
  const w = weld(ea, tolOf(g));
  // vertex -> face via triangle ranges
  const nV = P.length / 3;
  const vFace = new Int32Array(nV).fill(-1);
  const I = g.index ? g.index.array : null;
  const R = F.ranges;
  for (let f = 0; f < R.length / 2; f++) {
    for (let t = R[f * 2]; t <= R[f * 2 + 1]; t++) {
      for (let k = 0; k < 3; k++) { const v = I ? I[t * 3 + k] : t * 3 + k; vFace[v] = f; }
    }
  }
  // per welded edge point: linked list of (face, normal)
  const head = new Int32Array(w.n).fill(-1);
  const next = [], fc = [], nx = [], ny = [], nz = [];
  for (let v = 0; v < nV; v++) {
    const t = w.lookup(P[v * 3], P[v * 3 + 1], P[v * 3 + 2]);
    if (t < 0) continue;
    const f = vFace[v];
    let dup = false;
    for (let e = head[t]; e >= 0; e = next[e]) if (fc[e] === f) { dup = true; break; }
    if (dup) continue;
    fc.push(f); nx.push(N[v * 3]); ny.push(N[v * 3 + 1]); nz.push(N[v * 3 + 2]);
    next.push(head[t]); head[t] = fc.length - 1;
  }
  const COS = Math.cos(THREE.MathUtils.degToRad(maxDeg));
  const at = (p, f) => { for (let e = head[p]; e >= 0; e = next[e]) if (fc[e] === f) return e; return -1; };
  for (let s = 0; s < nEp / 2; s++) {
    const a = w.id[s * 2], b = w.id[s * 2 + 1];
    // faces present at both ends of the segment
    const common = [];
    for (let e = head[a]; e >= 0; e = next[e]) if (fc[e] >= 0 && at(b, fc[e]) >= 0) common.push(fc[e]);
    if (common.length < 2) continue;
    let soft = true;
    for (let i = 0; i < common.length && soft; i++) {
      for (let j = i + 1; j < common.length && soft; j++) {
        for (const p of [a, b]) {
          const e1 = at(p, common[i]), e2 = at(p, common[j]);
          if (nx[e1] * nx[e2] + ny[e1] * ny[e2] + nz[e1] * nz[e2] < COS) { soft = false; break; }
        }
      }
    }
    if (soft) out[s * 2] = out[s * 2 + 1] = 1;
  }
  return out;
}
