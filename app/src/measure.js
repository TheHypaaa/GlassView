import * as THREE from 'three';
import { fLen } from './units.js';
import { edgeSnaps, weld } from './edgeinfo.js';
import { isModelMesh } from './viewer.js';

const fA = (deg) => deg.toFixed(2) + '°';
const DEG = 180 / Math.PI;
const PAR = Math.cos(0.01 / DEG); // 0.01° counts as parallel

/** Per-mesh lookup: triangle -> B-rep face id. */
function faceOf(mesh) {
  const f = mesh.userData.faces;
  if (!f) return null;
  if (!f.faceOfTri) {
    const g = mesh.geometry;
    const nTri = g.index ? g.index.count / 3 : g.attributes.position.count / 3;
    const arr = new Int32Array(nTri).fill(-1);
    for (let i = 0; i < f.ranges.length / 2; i++) arr.fill(i, f.ranges[i * 2], f.ranges[i * 2 + 1] + 1);
    f.faceOfTri = arr;
  }
  return f.faceOfTri;
}

/* ------------------------------------------------------------------ entity descriptions
 * Entities keep raw geometry; text is produced on demand so a unit switch relabels everything.
 *   point: { k:'point', p, kind:'vertex'|'centre'|'mid'|'sphere'|'fcentre'|'surface'|'edge', r?, on? }
 *   plane: { k:'plane', p, n, click }
 *   axis:  { k:'axis', o, d, len, src:'cylinder'|'cone'|'torus', r?, r0?, r1?, ang?, R?, rr?, click, cond? }
 * cond (cylinders): 'centre' | 'near' | 'far' – which side of the cylinder a distance goes to.
 */
export function describe(e) {
  if (e.k === 'plane') return { type: 'Plane face', detail: '', cls: 'face' };
  if (e.k === 'axis') {
    const d = e.src === 'cylinder' ? `Cylinder Ø${fLen(e.r * 2)}`
      : e.src === 'cone' ? `Cone ${fA(e.ang * 2)} · Ø${fLen(Math.max(e.r0, e.r1) * 2)}`
        : `Torus R${fLen(e.R)} · r${fLen(e.rr)}`;
    return { type: e.fillet ? 'Fillet axis' : 'Axis', detail: d, cls: 'axis' };
  }
  switch (e.kind) {
    case 'vertex': return { type: 'Vertex', detail: '', cls: 'vertex' };
    case 'centre': return { type: e.fillet ? 'Arc centre' : 'Centre', detail: `Ø${fLen(e.r * 2)}`, cls: 'centre' };
    case 'sphere': return { type: 'Sphere centre', detail: `Ø${fLen(e.r * 2)}`, cls: 'centre' };
    case 'mid': return { type: 'Midpoint', detail: 'of edge', cls: 'mid' };
    case 'fcentre': return { type: 'Face centre', detail: e.on || '', cls: 'fcentre' };
    case 'edge': return { type: 'Point on edge', detail: '', cls: 'point' };
    default: return { type: 'Point', detail: e.on ? `on ${e.on}` : '', cls: 'point' };
  }
}
export const labelOf = (e) => { const d = describe(e); return d.detail ? `${d.type} · ${d.detail}` : d.type; };
export const hasCond = (e) => e.k === 'axis' && e.src === 'cylinder' && e.r > 0;

/** Axis entity (world space) from local endpoints. */
function axisEntity(rec, mesh) {
  const m = mesh.matrixWorld;
  const A = rec.A.clone().applyMatrix4(m), B = rec.B.clone().applyMatrix4(m);
  const len = A.distanceTo(B);
  const d = len > 1e-9 ? B.clone().sub(A).divideScalar(len) : rec.a.clone().transformDirection(m);
  const e = { k: 'axis', o: A, d, len, src: rec.src, r: rec.r, r0: rec.r0, r1: rec.r1, ang: rec.ang, R: rec.R, rr: rec.rr, fillet: !!(rec.group && rec.group.fillet), cond: 'centre' };
  e.click = A.clone().addScaledVector(d, len / 2);
  return e;
}

function axisRec(p, fid) {
  const a = new THREE.Vector3(...p.a), o = new THREE.Vector3(...p.o);
  return {
    fid, a, src: p.t, A: o.clone().addScaledVector(a, p.t0), B: o.clone().addScaledVector(a, p.t1),
    r: p.r, r0: p.r0, r1: p.r1, ang: p.ang, R: p.R, rr: p.t === 'torus' ? p.r : undefined
  };
}

/** Turn a fitted primitive (mesh-local) into a world-space measuring entity. */
function entityFromPrim(prim, mesh, click, fid) {
  const m = mesh.matrixWorld;
  if (prim.t === 'plane') {
    return { k: 'plane', p: new THREE.Vector3(...prim.p).applyMatrix4(m), n: new THREE.Vector3(...prim.n).transformDirection(m), click };
  }
  if (prim.t === 'cylinder' || prim.t === 'cone' || prim.t === 'torus') {
    const s = snapsOf(mesh);
    const rec = s.faceRec[fid] || axisRec(prim, fid);
    const e = axisEntity(rec, mesh);
    e.click = click;
    return e;
  }
  if (prim.t === 'sphere') return { k: 'point', kind: 'sphere', p: new THREE.Vector3(...prim.c).applyMatrix4(m), r: prim.r, click };
  return null;
}

/** Project a clicked point onto the exact surface (removes the tessellation chord error). */
function exactPoint(prim, mesh, p) {
  if (!prim) return p.clone();
  const inv = mesh.matrixWorld.clone().invert();
  const l = p.clone().applyMatrix4(inv);
  if (prim.t === 'plane') {
    const n = new THREE.Vector3(...prim.n);
    l.addScaledVector(n, -n.dot(l.clone().sub(new THREE.Vector3(...prim.p))));
  } else if (prim.t === 'cylinder') {
    const a = new THREE.Vector3(...prim.a), o = new THREE.Vector3(...prim.o);
    const foot = o.clone().addScaledVector(a, a.dot(l.clone().sub(o)));
    const rad = l.clone().sub(foot);
    if (rad.lengthSq() > 0) l.copy(foot).addScaledVector(rad.normalize(), prim.r);
  } else if (prim.t === 'sphere') {
    const c = new THREE.Vector3(...prim.c);
    const dir = l.clone().sub(c);
    if (dir.lengthSq() > 0) l.copy(c).addScaledVector(dir.normalize(), prim.r);
  }
  return l.applyMatrix4(mesh.matrixWorld);
}

/* ------------------------------------------------------------------ per-mesh snap data */
function faceTris(mesh, fid) {
  const f = mesh.userData.faces, g = mesh.geometry;
  const P = g.attributes.position.array, I = g.index ? g.index.array : null;
  return { t0: f.ranges[fid * 2], t1: f.ranges[fid * 2 + 1], P, I, vi: (t, k) => (I ? I[t * 3 + k] : t * 3 + k) };
}

/** Angular coverage (degrees) of a rotational face around its axis. */
function faceSpan(mesh, fid, rec) {
  const { t0, t1, P, vi } = faceTris(mesh, fid);
  const a = rec.a.clone().normalize();
  const u = new THREE.Vector3(1, 0, 0);
  if (Math.abs(a.dot(u)) > 0.9) u.set(0, 1, 0);
  u.sub(a.clone().multiplyScalar(a.dot(u))).normalize();
  const v = a.clone().cross(u);
  const angs = [];
  const q = new THREE.Vector3();
  const step = Math.max(1, Math.floor((t1 - t0 + 1) / 400));
  for (let t = t0; t <= t1; t += step) {
    for (let k = 0; k < 3; k++) {
      const i = vi(t, k);
      q.set(P[i * 3] - rec.A.x, P[i * 3 + 1] - rec.A.y, P[i * 3 + 2] - rec.A.z);
      angs.push(Math.atan2(q.dot(v), q.dot(u)));
    }
  }
  if (angs.length < 2) return 0;
  angs.sort((x, y) => x - y);
  let gap = angs[0] + 2 * Math.PI - angs[angs.length - 1];
  for (let i = 1; i < angs.length; i++) gap = Math.max(gap, angs[i] - angs[i - 1]);
  return (2 * Math.PI - gap) * DEG;
}

/** Area centroid of a face (mesh-local). */
function faceCentroid(mesh, fid) {
  const { t0, t1, P, vi } = faceTris(mesh, fid);
  const c = new THREE.Vector3(), a = new THREE.Vector3(), b = new THREE.Vector3(), d = new THREE.Vector3();
  let A = 0;
  for (let t = t0; t <= t1; t++) {
    const i0 = vi(t, 0), i1 = vi(t, 1), i2 = vi(t, 2);
    a.set(P[i0 * 3], P[i0 * 3 + 1], P[i0 * 3 + 2]);
    b.set(P[i1 * 3], P[i1 * 3 + 1], P[i1 * 3 + 2]);
    d.set(P[i2 * 3], P[i2 * 3 + 1], P[i2 * 3 + 2]);
    const ar = b.clone().sub(a).cross(d.clone().sub(a)).length() / 2;
    c.addScaledVector(a.add(b).add(d), ar / 3);
    A += ar;
  }
  return A > 0 ? c.divideScalar(A) : null;
}

/**
 * Everything snappable on one mesh, in its local frame (cached):
 *   corners, mids (Float32Array xyz), centres [{p, r, kind, fillet}], fcentres [{p, fid, on, fillet}],
 *   axes [{A, B, a, src, r…, fid, fillet}], faceRec[fid] → merged axis record.
 * Rotational faces covering less than 180° around their axis (and all tori) count as fillets.
 */
export function snapsOf(mesh) {
  if (mesh.userData.snaps) return mesh.userData.snaps;
  const { corners, mids } = edgeSnaps(mesh);
  const f = mesh.userData.faces;
  const R = (mesh.geometry.boundingSphere || (mesh.geometry.computeBoundingSphere(), mesh.geometry.boundingSphere)).radius || 1;
  const tol = R * 1e-4;
  const key = (v, t) => `${Math.round(v.x / t)},${Math.round(v.y / t)},${Math.round(v.z / t)}`;
  const axes = [], aMap = new Map(), faceRec = [], perFace = [], perFaceById = new Map();
  const fcentres = [], centres = [], cKeys = new Map();
  if (f && f.prims) {
    f.prims.forEach((p, fid) => {
      if (p.t !== 'cylinder' && p.t !== 'cone' && p.t !== 'torus') return;
      const rec = axisRec(p, fid);
      rec.span = p.t === 'torus' ? 0 : faceSpan(mesh, fid, rec);
      perFace.push(rec);
      perFaceById.set(fid, rec);
      const d = rec.a.clone();
      if (d.x < -1e-9 || (Math.abs(d.x) < 1e-9 && (d.y < -1e-9 || (Math.abs(d.y) < 1e-9 && d.z < 0)))) d.negate();
      const foot = rec.A.clone().addScaledVector(d, -d.dot(rec.A));
      const k = `${p.t}|${(p.r ?? p.R ?? 0).toFixed(4)}|${key(d, 1e-4)}|${key(foot, tol * 10)}`;
      let grp = aMap.get(k);
      if (!grp) {
        grp = { ...rec, A: rec.A.clone(), B: rec.B.clone(), span: 0, faces: [] };
        aMap.set(k, grp);
        axes.push(grp);
      } else {
        const dir = grp.B.clone().sub(grp.A);
        const L = dir.length() || 1;
        dir.divideScalar(L);
        let t0 = 0, t1 = L;
        for (const q of [rec.A, rec.B]) { const t = q.clone().sub(grp.A).dot(dir); t0 = Math.min(t0, t); t1 = Math.max(t1, t); }
        const A0 = grp.A.clone();
        grp.A = A0.clone().addScaledVector(dir, t0);
        grp.B = A0.clone().addScaledVector(dir, t1);
      }
      grp.span += rec.span;
      grp.faces.push(fid);
      rec.group = grp;
      faceRec[fid] = grp;
    });
    for (const g of axes) { g.fillet = g.src === 'torus' || g.span < 180; g.group = g; }
    const addC = (p, r, kind, fillet) => {
      const k = key(p, tol);
      const ex = cKeys.get(k);
      if (ex) { ex.fillet = ex.fillet && fillet; return; }
      const c = { p, r, kind, fillet };
      cKeys.set(k, c);
      centres.push(c);
    };
    for (const rec of perFace) {
      if (rec.src === 'torus') continue;
      const fil = rec.group.fillet;
      addC(rec.A.clone(), rec.src === 'cylinder' ? rec.r : rec.r0, 'centre', fil);
      addC(rec.B.clone(), rec.src === 'cylinder' ? rec.r : rec.r1, 'centre', fil);
    }
    f.prims.forEach((p, fid) => {
      if (p.t === 'sphere') { addC(new THREE.Vector3(...p.c), p.r, 'sphere', false); return; }
      if (p.t === 'cylinder' || p.t === 'cone') {
        const rec = faceRec[fid];
        const pr = perFaceById.get(fid);
        fcentres.push({ p: pr.A.clone().lerp(pr.B, 0.5), fid, on: `${p.t} (on axis)`, fillet: rec.fillet });
        return;
      }
      if (p.t === 'torus') return;
      const c = faceCentroid(mesh, fid);
      if (c) fcentres.push({ p: c, fid, on: p.t === 'plane' ? 'plane' : 'surface', fillet: false });
    });
  }
  const cornerKeys = new Set();
  for (let i = 0; i < corners.length; i += 3) cornerKeys.add(key({ x: corners[i], y: corners[i + 1], z: corners[i + 2] }, tol));
  return (mesh.userData.snaps = { corners, mids, centres, fcentres, axes, faceRec, cornerKeys, tol, key });
}

/** Boundary and snap points of one face (mesh-local), for the Shift/Alt face lock. */
function faceData(mesh, fid) {
  const cache = (mesh.userData.faceData ||= new Map());
  if (cache.has(fid)) return cache.get(fid);
  const s = snapsOf(mesh);
  const f = mesh.userData.faces, E = mesh.userData.edges;
  let segs;
  if (f.eranges && E) {
    const arr = E.geometry.attributes.position.array;
    const first = f.eranges[fid * 2], n = f.eranges[fid * 2 + 1];
    segs = arr.slice(first * 6, (first + n) * 6);
  } else {
    // boundary = triangle edges used once within the face
    const { t0, t1, P, vi } = faceTris(mesh, fid);
    const pts = new Float32Array((t1 - t0 + 1) * 9);
    for (let t = t0, o = 0; t <= t1; t++) for (let k = 0; k < 3; k++, o += 3) { const i = vi(t, k); pts[o] = P[i * 3]; pts[o + 1] = P[i * 3 + 1]; pts[o + 2] = P[i * 3 + 2]; }
    const w = weld(pts, s.tol * 0.1);
    const cnt = new Map(); // edge key -> [uses, offset of end a, offset of end b]
    const nT = t1 - t0 + 1;
    for (let t = 0; t < nT; t++) {
      for (let k = 0; k < 3; k++) {
        const ia = t * 3 + k, ib = t * 3 + ((k + 1) % 3);
        const a = w.id[ia], b = w.id[ib];
        if (a === b) continue;
        const kk = a < b ? a * 4294967296 + b : b * 4294967296 + a;
        const c = cnt.get(kk);
        if (c) c[0]++;
        else cnt.set(kk, [1, ia * 3, ib * 3]);
      }
    }
    const out = [];
    for (const v of cnt.values()) if (v[0] === 1) out.push(pts[v[1]], pts[v[1] + 1], pts[v[1] + 2], pts[v[2]], pts[v[2] + 1], pts[v[2] + 2]);
    segs = new Float32Array(out);
  }
  // corners: boundary points that are B-rep vertices (or sharp turns of the loop)
  const pts = [];
  const seen = new Set();
  const isC = (x, y, z) => s.cornerKeys.has(s.key({ x, y, z }, s.tol));
  const ck = new Set();
  for (let i = 0; i < segs.length; i += 3) {
    const k = s.key({ x: segs[i], y: segs[i + 1], z: segs[i + 2] }, s.tol);
    if (seen.has(k)) continue;
    seen.add(k);
    if (isC(segs[i], segs[i + 1], segs[i + 2])) { ck.add(k); pts.push({ p: new THREE.Vector3(segs[i], segs[i + 1], segs[i + 2]), kind: 'vertex' }); }
  }
  for (let i = 0; i < segs.length; i += 6) {
    const ka = s.key({ x: segs[i], y: segs[i + 1], z: segs[i + 2] }, s.tol), kb = s.key({ x: segs[i + 3], y: segs[i + 4], z: segs[i + 5] }, s.tol);
    if (ck.has(ka) && ck.has(kb) && ka !== kb) pts.push({ p: new THREE.Vector3((segs[i] + segs[i + 3]) / 2, (segs[i + 1] + segs[i + 4]) / 2, (segs[i + 2] + segs[i + 5]) / 2), kind: 'mid' });
  }
  // centres of circular boundaries: centres lying on this face's plane inside its box, or the face's own
  const prim = f.prims[fid];
  const box = new THREE.Box3();
  for (let i = 0; i < segs.length; i += 3) box.expandByPoint(new THREE.Vector3(segs[i], segs[i + 1], segs[i + 2]));
  box.expandByScalar(s.tol * 10);
  if (prim && prim.t === 'plane') {
    const n = new THREE.Vector3(...prim.n), q = new THREE.Vector3(...prim.p);
    for (const c of s.centres) if (Math.abs(n.dot(c.p.clone().sub(q))) < s.tol * 20 && box.containsPoint(c.p)) pts.push({ p: c.p, kind: c.kind, r: c.r, fillet: c.fillet });
  } else if (s.faceRec[fid]) {
    const r = s.faceRec[fid];
    for (const c of s.centres) {
      const v = c.p.clone().sub(r.A);
      const ax = r.B.clone().sub(r.A).normalize();
      if (v.clone().sub(ax.clone().multiplyScalar(v.dot(ax))).length() < s.tol * 20 && box.clone().expandByScalar((r.r || r.r0 || 0) * 2).containsPoint(c.p)) pts.push({ p: c.p, kind: c.kind, r: c.r, fillet: c.fillet });
    }
  }
  const fc = s.fcentres.find((x) => x.fid === fid);
  if (fc) pts.push({ p: fc.p, kind: 'fcentre', on: fc.on });
  const d = { segs, pts };
  cache.set(fid, d);
  return d;
}

/* ------------------------------------------------------------------ measuring */
const footOnLine = (p, o, d) => o.clone().addScaledVector(d, d.dot(p.clone().sub(o)));
const footOnPlane = (p, q, n) => p.clone().addScaledVector(n, -n.dot(p.clone().sub(q)));
const mid = (e) => (e.k === 'axis' ? e.o.clone().addScaledVector(e.d, e.len / 2) : e.k === 'plane' ? e.click.clone() : e.p.clone());
const anchorOf = (e) => (e.k === 'point' ? e.p : e.k === 'axis' ? (e.click || mid(e)) : e.click);
const sgn = (e) => (hasCond(e) && e.cond && e.cond !== 'centre' ? (e.cond === 'near' ? -1 : 1) : 0);
const perp = (d) => { const u = Math.abs(d.x) < 0.9 ? new THREE.Vector3(1, 0, 0) : new THREE.Vector3(0, 1, 0); return u.sub(d.clone().multiplyScalar(d.dot(u))).normalize(); };
const condName = (e) => (e.k === 'axis' ? (hasCond(e) ? e.cond || 'centre' : 'axis') : e.k);

/**
 * Angle construction for drawing: a vertex O and two unit rays u, v whose opening is the reported angle.
 * pA / pB are the picked points the rays should reach (so it is obvious which entity each ray belongs to).
 * If the true vertex (intersection) is far away — nearly parallel faces — a virtual vertex between the
 * picks is used and `virtual` is set.
 */
function angleArc(O, u, v, pA, pB, virtualAt) {
  const span = pA.distanceTo(pB);
  let virtual = false;
  if (!O || O.distanceTo(pA) > Math.max(span * 2.5, 1e-6) || O.distanceTo(pB) > Math.max(span * 2.5, 1e-6)) {
    O = virtualAt.clone();
    virtual = true;
  }
  const ang = Math.acos(THREE.MathUtils.clamp(u.dot(v), -1, 1));
  const la = Math.max(pA.clone().sub(O).dot(u), 0), lb = Math.max(pB.clone().sub(O).dot(v), 0);
  let r = Math.min(la || Infinity, lb || Infinity);
  if (!isFinite(r) || r < span * 0.15) r = Math.max(span * 0.35, 1e-3);
  r *= virtual ? 0.9 : 0.7;
  const rayA = Math.max(la, r * 1.25), rayB = Math.max(lb, r * 1.25);
  const pts = [];
  const w = v.clone().addScaledVector(u, -u.dot(v));
  const wl = w.length();
  if (wl > 1e-9) w.divideScalar(wl);
  if (Math.abs(ang * DEG - 90) < 0.01) {
    // right angle: the usual square corner mark
    const q = r * 0.45;
    pts.push(O.clone().addScaledVector(u, q), O.clone().addScaledVector(u, q).addScaledVector(w, q), O.clone().addScaledVector(w, q));
  } else {
    const N = Math.max(8, Math.ceil(ang * DEG / 3));
    for (let i = 0; i <= N; i++) {
      const a = (ang * i) / N;
      pts.push(O.clone().addScaledVector(u, Math.cos(a) * r).addScaledVector(w, Math.sin(a) * r));
    }
  }
  const midDir = wl > 1e-9 ? u.clone().multiplyScalar(Math.cos(ang / 2)).addScaledVector(w, Math.sin(ang / 2)) : u.clone();
  return {
    O, r, ang: ang * DEG, virtual, pts,
    rays: [[O.clone(), O.clone().addScaledVector(u, rayA)], [O.clone(), O.clone().addScaledVector(v, rayB)]],
    label: O.clone().addScaledVector(midDir, r * 1.0),
    links: virtual ? [[O.clone().addScaledVector(u, rayA), pA.clone()], [O.clone().addScaledVector(v, rayB), pB.clone()]] : []
  };
}

/** Measure between two entities. Returns { main, sub, kind, line:[a,b]|null, anchor, cond }. */
export function measureEntities(A, B) {
  const order = { point: 0, axis: 1, plane: 2 };
  if (order[A.k] > order[B.k]) [A, B] = [B, A];
  const key = A.k + '-' + B.k;
  const condTxt = () => (sgn(A) || sgn(B) ? ` · ${condName(A)} ↔ ${condName(B)}` : '');
  if (key === 'point-point') {
    const d = B.p.clone().sub(A.p);
    return { kind: 'Distance', main: fLen(d.length()), sub: 'point to point', line: [A.p, B.p] };
  }
  if (key === 'point-plane') {
    const f = footOnPlane(A.p, B.p, B.n);
    return { kind: 'Point ↔ plane', main: fLen(f.distanceTo(A.p)), sub: 'perpendicular distance', line: [A.p, f] };
  }
  if (key === 'point-axis') {
    const f = footOnLine(A.p, B.o, B.d);
    const d = f.distanceTo(A.p);
    const u = d > 1e-9 ? f.clone().sub(A.p).divideScalar(d) : perp(B.d);
    const end = f.clone().addScaledVector(u, sgn(B) * (B.r || 0));
    return { kind: hasCond(B) && sgn(B) ? 'Point ↔ cylinder' : 'Point ↔ axis', main: fLen(end.distanceTo(A.p)), sub: `to ${describe(B).detail}${condTxt()}`, line: [A.p, end], cond: hasCond(B) };
  }
  if (key === 'plane-plane') {
    const c = Math.abs(A.n.dot(B.n));
    if (c > PAR) {
      const f = footOnPlane(A.click, B.p, B.n);
      return { kind: 'Plane ↔ plane', main: fLen(f.distanceTo(A.click)), sub: 'parallel planes', line: [A.click, f] };
    }
    // vertex on the planes' intersection line, nearest to the two picks; rays run inside each face
    const L = A.n.clone().cross(B.n).normalize();
    const M = A.click.clone().add(B.click).multiplyScalar(0.5);
    // point on both planes: solve with the in-plane directions perpendicular to L
    const dA = A.n.dot(A.p), dB = B.n.dot(B.p), nn = A.n.dot(B.n);
    const det = 1 - nn * nn;
    const c1 = (dA - dB * nn) / det, c2 = (dB - dA * nn) / det;
    let O = A.n.clone().multiplyScalar(c1).addScaledVector(B.n, c2);
    O.addScaledVector(L, L.dot(M.clone().sub(O)));
    const inPlane = (e) => { const q = e.click.clone().sub(O); q.addScaledVector(L, -L.dot(q)); return q.lengthSq() > 1e-12 ? q.normalize() : e.n.clone().cross(L).normalize(); };
    const u = inPlane(A), v = inPlane(B);
    const arc = angleArc(O, u, v, A.click, B.click, M);
    const ang = arc.ang;
    if (arc.virtual) {
      // virtual vertex: draw the rays parallel to the faces through the midpoint
      arc.rays = [[M.clone(), M.clone().addScaledVector(u, arc.r * 1.6)], [M.clone(), M.clone().addScaledVector(v, arc.r * 1.6)]];
    }
    arc.axis = arc.virtual ? null : [O.clone().addScaledVector(L, -arc.r * 1.2), O.clone().addScaledVector(L, arc.r * 1.2)];
    return { kind: 'Angle', main: fA(ang), sub: `between faces · other side ${fA(180 - ang)}`, line: null, anchor: arc.label, arc, angle: true };
  }
  if (key === 'axis-axis') {
    const cond = hasCond(A) || hasCond(B);
    const cr = A.d.clone().cross(B.d);
    const s = cr.length();
    let pA, pB, sub, kind = 'Axis ↔ axis';
    if (s < Math.sin(0.01 / DEG)) {
      pA = mid(A); pB = footOnLine(pA, B.o, B.d);
      sub = 'parallel axes';
    } else {
      const w = A.o.clone().sub(B.o);
      const b = A.d.dot(B.d), d = A.d.dot(w), e = B.d.dot(w), den = 1 - b * b;
      pA = A.o.clone().addScaledVector(A.d, (b * e - d) / den);
      pB = B.o.clone().addScaledVector(B.d, (e - b * d) / den);
      const ang = Math.acos(Math.min(1, Math.abs(b))) * DEG;
      if (pA.distanceTo(pB) < 1e-4 && !sgn(A) && !sgn(B)) {
        const ca = anchorOf(A), cb = anchorOf(B);
        const u = A.d.clone().multiplyScalar(Math.sign(A.d.dot(ca.clone().sub(pA))) || 1);
        const v = B.d.clone().multiplyScalar(Math.sign(B.d.dot(cb.clone().sub(pA))) || 1);
        const arc = angleArc(pA, u, v, ca, cb, pA);
        return { kind: 'Axis ∠ axis', main: fA(arc.ang), sub: `intersecting axes · other side ${fA(180 - arc.ang)}`, line: null, anchor: arc.label, arc, cond, angle: true };
      }
      sub = `skew · angle ${fA(ang)}`;
    }
    const dd = pA.distanceTo(pB);
    const u = dd > 1e-9 ? pB.clone().sub(pA).divideScalar(dd) : perp(A.d);
    const a2 = pA.clone().addScaledVector(u, -sgn(A) * (A.r || 0));
    const b2 = pB.clone().addScaledVector(u, sgn(B) * (B.r || 0));
    if (sgn(A) || sgn(B)) kind = 'Cylinder ↔ cylinder';
    return { kind, main: fLen(a2.distanceTo(b2)), sub: sub + condTxt(), line: [a2, b2], cond };
  }
  if (key === 'axis-plane') {
    const sn = Math.abs(A.d.dot(B.n));
    if (sn < Math.sin(0.01 / DEG)) {
      const p1 = mid(A), f = footOnPlane(p1, B.p, B.n);
      const d = f.distanceTo(p1);
      const u = d > 1e-9 ? f.clone().sub(p1).divideScalar(d) : B.n.clone();
      const a2 = p1.clone().addScaledVector(u, -sgn(A) * (A.r || 0));
      return { kind: sgn(A) ? 'Cylinder ↔ plane' : 'Axis ↔ plane', main: fLen(a2.distanceTo(f)), sub: 'axis parallel to plane' + condTxt(), line: [a2, f], cond: hasCond(A) };
    }
    const ang = Math.asin(Math.min(1, sn)) * DEG;
    // vertex where the axis pierces the plane; one ray along the axis, one along its shadow in the plane
    const t = B.n.dot(B.p.clone().sub(A.o)) / A.d.dot(B.n);
    const O = A.o.clone().addScaledVector(A.d, t);
    const ca = mid(A);
    const u = A.d.clone().multiplyScalar(Math.sign(A.d.dot(ca.clone().sub(O))) || 1);
    let v = u.clone().addScaledVector(B.n, -u.dot(B.n));
    if (v.lengthSq() < 1e-12) v = perp(B.n); else v.normalize();
    const pb = O.clone().add(v.clone().multiplyScalar(Math.max(ca.distanceTo(O), 1e-3)));
    const arc = angleArc(O, u, v, ca, ang > 89.99 ? pb : footOnPlane(ca, B.p, B.n), ca.clone().addScaledVector(A.d, 0));
    if (arc.virtual) {
      const M = ca;
      arc.rays = [[M.clone(), M.clone().addScaledVector(u, arc.r * 1.6)], [M.clone(), M.clone().addScaledVector(v, arc.r * 1.6)]];
      arc.links = [[M.clone(), footOnPlane(M, B.p, B.n)]];
    }
    return { kind: 'Axis ∠ plane', main: fA(ang), sub: ang > 89.99 ? 'perpendicular' : 'angle between axis and plane', line: null, anchor: arc.label, arc, angle: true };
  }
  return { kind: '?', main: '–', sub: '', line: null };
}

/* ------------------------------------------------------------------ snap-point dots */
// kinds: 0 corner, 1 centre, 2 midpoint, 3 face centre, 4 fillet arc centre, 5 fillet face centre
function makeSnapMaterial() {
  return new THREE.ShaderMaterial({
    uniforms: { uScale: { value: window.devicePixelRatio || 1 }, uBias: { value: 0.1 }, uShow: { value: [1, 1, 1, 1, 0, 0] } },
    clipping: true,
    depthWrite: false,
    transparent: true,
    vertexShader: `
      attribute float kind;
      varying float vKind;
      uniform float uScale;
      uniform float uBias;
      uniform float uShow[6];
      #include <clipping_planes_pars_vertex>
      void main() {
        vKind = kind;
        vec4 mvPosition = modelViewMatrix * vec4(position, 1.0);
        vec4 biased = mvPosition;
        biased.xyz += (isOrthographic ? vec3(0.0, 0.0, 1.0) : normalize(-mvPosition.xyz)) * uBias;
        gl_Position = projectionMatrix * biased;
        int k = int(kind + 0.5);
        if (uShow[k] < 0.5) gl_Position = vec4(2.0, 2.0, 2.0, 1.0);
        gl_PointSize = (k == 0 ? 7.0 : k == 1 ? 9.5 : k == 2 ? 6.0 : k == 3 ? 7.5 : k == 4 ? 7.0 : 6.0) * uScale;
        #include <clipping_planes_vertex>
      }`,
    fragmentShader: `
      varying float vKind;
      #include <clipping_planes_pars_fragment>
      void main() {
        #include <clipping_planes_fragment>
        vec2 c = gl_PointCoord * 2.0 - 1.0;
        float d = dot(c, c);
        if (d > 1.0) discard;
        int k = int(vKind + 0.5);
        vec3 fill = k == 0 ? vec3(1.0) : k == 1 ? vec3(1.0, 0.62, 0.04) : k == 2 ? vec3(0.19, 0.78, 0.86)
          : k == 3 ? vec3(1.0, 0.18, 0.57) : k == 4 ? vec3(1.0, 0.8, 0.5) : vec3(1.0, 0.6, 0.78);
        vec3 ring = k == 0 ? vec3(0.08, 0.09, 0.11) : vec3(1.0);
        gl_FragColor = vec4(d > 0.42 ? ring : fill, 0.95);
      }`
  });
}

const KIND_OF = (c) => (c.kind === 'vertex' ? 0 : c.kind === 'mid' ? 2 : c.kind === 'fcentre' ? (c.fillet ? 5 : 3) : c.fillet ? 4 : 1);

/**
 * Measuring tool.
 *  Faces & axes mode: planes pick as planes, cylinders/cones as their axis, spheres as centre.
 *  Corners, midpoints, centres and face centres snap; hovering near an axis line picks the axis.
 *  Shift or Alt held: locked to the face under the cursor – only that face and the points on
 *  its boundary (corners, midpoints, any point along its edges, centres, face centre) are picked.
 *  Points mode: always a point (projected onto the exact surface).
 */
export class MeasureTool extends EventTarget {
  constructor(viewer, layer, tagEl) {
    super();
    this.viewer = viewer;
    this.layer = layer;
    this.tag = tagEl;
    this.active = false;
    this.snap = true;
    this.showSnaps = true;
    this.xyz = false;
    this.show = { corners: true, mids: true, centres: true, fcentres: true, axes: true, fillets: false };
    this.mode = 'smart';
    this.list = null;
    this.pending = null;
    this.hover = null;
    this.axisMesh = null;
    this.lock = null;
    this.lastEv = null;
    this.group = new THREE.Group();
    viewer.overlay.add(this.group);
    this.lineMat = new THREE.LineBasicMaterial({ color: 0x0a84ff, depthTest: false, transparent: true });
    this.arcMat = new THREE.LineBasicMaterial({ color: 0xff9f0a, depthTest: false, transparent: true, toneMapped: false });
    this.arcFillMat = new THREE.MeshBasicMaterial({ color: 0xff9f0a, transparent: true, opacity: 0.22, depthTest: false, depthWrite: false, side: THREE.DoubleSide, toneMapped: false });
    this.leaderMat = new THREE.LineBasicMaterial({ color: 0x8e8e93, depthTest: false, transparent: true, opacity: 0.8 });
    this.legMats = [0xff453a, 0x30d158, 0x0a84ff].map((c) => new THREE.LineDashedMaterial({ color: c, depthTest: false, transparent: true, dashSize: 1, gapSize: 0.6, toneMapped: false }));
    this.axisMat = new THREE.LineDashedMaterial({ color: 0x0a84ff, depthTest: false, transparent: true, dashSize: 1, gapSize: 1 });
    this.faintAxisMat = new THREE.LineDashedMaterial({ color: 0x5e5ce6, depthTest: false, transparent: true, opacity: 0.5, dashSize: 1, gapSize: 1, toneMapped: false });
    this.faceMat = new THREE.MeshBasicMaterial({ color: 0x2f8cff, transparent: true, opacity: 0.32, depthWrite: false, polygonOffset: true, polygonOffsetFactor: -2, polygonOffsetUnits: -2, side: THREE.DoubleSide, toneMapped: false });
    this.lockMat = this.faceMat.clone();
    this.lockMat.opacity = 0.45;
    this.pickMat = this.faceMat.clone();
    this.pickMat.color.set(0xff9f0a);
    this.pickMat.opacity = 0.42;
    this.faceEdgeMat = new THREE.LineBasicMaterial({ color: 0x0a84ff, transparent: true, depthWrite: false, toneMapped: false });
    this.snapMat = makeSnapMaterial();
    this.lockPtsMat = makeSnapMaterial();
    this.lockPtsMat.uniforms.uShow.value = [1, 1, 1, 1, 1, 1];
    this.rubber = this._line([new THREE.Vector3(), new THREE.Vector3()], true);
    this.rubber.visible = false;
    viewer.overlay.add(this.rubber);
    this.hoverFx = [];
    this.pendingFx = [];
    this.cursor = document.createElement('div');
    this.cursor.className = 'm-cursor';
    layer.appendChild(this.cursor);
    viewer.renderHooks.push(() => {
      this.layout();
      for (const o of [...this.hoverFx, ...this.pendingFx]) if (o.userData && o.userData.tube) this._sizeTube(o);
    });
    this._snapQueue = [];
    const mod = (e) => {
      if (!this.active || (e.key !== 'Shift' && e.key !== 'Alt' && e.key !== 'Control')) return;
      if (e.key === 'Control') { if (this.lastEv) this.onMove({ clientX: this.lastEv.clientX, clientY: this.lastEv.clientY, shiftKey: e.shiftKey, altKey: e.altKey, ctrlKey: e.ctrlKey }); return; }
      if (e.key === 'Alt') e.preventDefault();
      const held = e.shiftKey || e.altKey;
      if (!held) this._unlock();
      else if (!this.lock && this.hover && this.hover.fid >= 0 && this.hover.mesh) this._lockTo(this.hover.mesh, this.hover.fid);
      if (this.lastEv) this.onMove({ clientX: this.lastEv.clientX, clientY: this.lastEv.clientY, shiftKey: e.shiftKey, altKey: e.altKey, ctrlKey: e.ctrlKey });
    };
    window.addEventListener('keydown', mod);
    window.addEventListener('keyup', mod);
    window.addEventListener('blur', () => this._unlock());
    this._syncMask();
  }

  // ---------------------------------------------------------------- helpers
  _line(pts, dashed = false, mat = null) {
    const l = new THREE.Line(new THREE.BufferGeometry().setFromPoints(pts), mat || (dashed ? this.axisMat.clone() : this.lineMat));
    l.renderOrder = 1000;
    l.raycast = () => {};
    if (dashed) this._dash(l);
    return l;
  }

  /** Translucent wedge between the two rays of an angle. */
  _arcFill(a) {
    if (a.pts.length < 2) return null;
    const P = [];
    for (let i = 0; i < a.pts.length - 1; i++) P.push(a.O, a.pts[i], a.pts[i + 1]);
    const g = new THREE.BufferGeometry().setFromPoints(P);
    const mesh = new THREE.Mesh(g, this.arcFillMat);
    mesh.renderOrder = 999;
    mesh.raycast = () => {};
    return mesh;
  }

  _dash(l) {
    l.computeLineDistances();
    const pos = l.geometry.attributes.position;
    const L = new THREE.Vector3().fromBufferAttribute(pos, 0).distanceTo(new THREE.Vector3().fromBufferAttribute(pos, 1)) || 1;
    l.material.dashSize = l.material.gapSize = L / 30;
  }

  /** Thick axis (a thin cylinder kept at ~3 px on screen) for hover / picked feedback. */
  _axisTube(e, color) {
    const ext = Math.max(e.len * 0.15, this.viewer.modelRadius * 0.04);
    const len = e.len + 2 * ext;
    const g = new THREE.CylinderGeometry(1, 1, 1, 10, 1, false);
    const m = new THREE.Mesh(g, new THREE.MeshBasicMaterial({ color, depthTest: false, transparent: true, opacity: 0.95, toneMapped: false }));
    m.position.copy(e.o).addScaledVector(e.d, e.len / 2);
    m.quaternion.setFromUnitVectors(new THREE.Vector3(0, 1, 0), e.d);
    m.userData.tube = len;
    m.renderOrder = 1001;
    m.raycast = () => {};
    const ends = [-0.5, 0.5].map((y) => {
      const c = new THREE.Mesh(new THREE.SphereGeometry(1, 12, 8), m.material);
      c.position.y = y; c.raycast = () => {}; c.renderOrder = 1001;
      return c;
    });
    m.add(...ends);
    m.userData.ends = ends;
    this._sizeTube(m);
    return m;
  }

  _px(at) {
    const v = this.viewer, cam = v.camera;
    return cam.isPerspectiveCamera
      ? cam.position.distanceTo(at) * Math.tan(THREE.MathUtils.degToRad(cam.fov) / 2) * 2 / v.canvas.clientHeight
      : (cam.top - cam.bottom) / cam.zoom / v.canvas.clientHeight;
  }

  _sizeTube(m) {
    const r = this._px(m.position) * 1.6;
    m.scale.set(r, m.userData.tube, r);
    for (const c of m.userData.ends) c.scale.set(2.2, 2.2 * r / m.userData.tube, 2.2);
  }

  _axisLine(e, mat = null) {
    const ext = Math.max(e.len * 0.15, this.viewer.modelRadius * 0.04);
    return this._line([e.o.clone().addScaledVector(e.d, -ext), e.o.clone().addScaledVector(e.d, e.len + ext)], !mat, mat);
  }

  /** Highlight one B-rep face (shares the mesh buffers; only the draw range changes). */
  _faceOverlay(mesh, fid, mat) {
    const slot = mat === this.pickMat ? 'pickOverlay' : mat === this.lockMat ? 'lockOverlay' : 'hoverOverlay';
    let ov = mesh.userData[slot];
    if (!ov || ov.geometry.attributes.position !== mesh.geometry.attributes.position) {
      if (ov) mesh.remove(ov);
      const g0 = mesh.geometry;
      const g = new THREE.BufferGeometry();
      g.setAttribute('position', g0.attributes.position);
      g.setIndex(g0.index);
      g.boundingSphere = g0.boundingSphere;
      ov = new THREE.Mesh(g, mat);
      ov.userData.isOverlay = true;
      ov.raycast = () => {};
      ov.renderOrder = 999;
      mesh.add(ov);
      mesh.userData[slot] = ov;
    }
    const f = mesh.userData.faces;
    ov.geometry.setDrawRange(f.ranges[fid * 2] * 3, (f.ranges[fid * 2 + 1] - f.ranges[fid * 2] + 1) * 3);
    mat.clippingPlanes = this.viewer.clipPlanes.length ? this.viewer.clipPlanes : null;
    ov.visible = true;
    return ov;
  }

  /** Outline of a face: its own boundary edges in the accent colour. */
  _faceEdges(mesh, fid) {
    const f = mesh.userData.faces, E = mesh.userData.edges;
    if (!f || !f.eranges || !E) {
      if (!f) return null;
      // no per-face edges (fallback engine): draw the computed boundary
      const d = faceData(mesh, fid);
      const g = new THREE.BufferGeometry();
      g.setAttribute('position', new THREE.BufferAttribute(d.segs, 3));
      const l = new THREE.LineSegments(g, this.faceEdgeMat);
      l.raycast = () => {};
      l.renderOrder = 998;
      mesh.add(l);
      return l;
    }
    let ov = mesh.userData.faceEdgeOverlay;
    if (!ov || ov.geometry.attributes.position !== E.geometry.attributes.position) {
      if (ov) mesh.remove(ov);
      const g = new THREE.BufferGeometry();
      g.setAttribute('position', E.geometry.attributes.position);
      g.boundingSphere = E.geometry.boundingSphere;
      ov = new THREE.LineSegments(g, this.faceEdgeMat);
      ov.userData.isOverlay = true;
      ov.raycast = () => {};
      ov.renderOrder = 998;
      mesh.add(ov);
      mesh.userData.faceEdgeOverlay = ov;
    }
    const first = f.eranges[fid * 2], n = f.eranges[fid * 2 + 1];
    if (!n) return null;
    ov.geometry.setDrawRange(first * 2, n * 2);
    this.faceEdgeMat.clippingPlanes = this.viewer.clipPlanes.length ? this.viewer.clipPlanes : null;
    ov.visible = true;
    return ov;
  }

  _clearFx(list) {
    for (const o of list) {
      if (o.userData && o.userData.isOverlay) o.visible = false; // persistent overlays
      else {
        o.parent?.remove(o);
        o.geometry.dispose();
        if (o.userData && o.userData.tube) { o.material.dispose(); o.userData.ends.forEach((c) => c.geometry.dispose()); }
      }
    }
    list.length = 0;
  }

  setDoc(doc) {
    const show = (m, on) => { m.group.visible = on; m.el.style.display = on ? '' : 'none'; m.dots.forEach((d) => { d.hidden = !on; }); m.legEls.forEach((d) => { d.hidden = !on; }); };
    if (this.list) for (const m of this.list) show(m, false);
    this.list = doc ? doc.measurements : null;
    if (this.list) for (const m of this.list) show(m, true);
    this.axisMesh = null;
    this._unlock();
    this.cancel();
    this.refreshOverview();
    this.viewer.requestRender();
  }

  setActive(on) {
    this.active = on;
    this._unlock();
    this.cancel();
    this._setHover(null);
    this._showAxes(null);
    this.cursor.style.display = 'none';
    this._tag(null);
    this.viewer.canvas.classList.toggle('measuring', on);
    this.refreshOverview();
  }

  cancel() {
    this.pending = null;
    this.rubber.visible = false;
    this._clearFx(this.pendingFx);
    this.dispatchEvent(new Event('pending'));
    this.viewer.requestRender();
  }

  // ---------------------------------------------------------------- what is shown / snapped
  /** Update category visibility (corners, mids, centres, fcentres, axes, fillets). */
  setShow(patch) {
    Object.assign(this.show, patch);
    this._syncMask();
    // axis lines are filtered when built – rebuild them
    this.viewer.forEachMesh((m) => { if (m.userData.axisLines) { m.remove(m.userData.axisLines); m.userData.axisLines.geometry.dispose(); delete m.userData.axisLines; } });
    this._axesOn = null;
    this.refreshOverview();
  }

  _syncMask() {
    const s = this.show;
    this.snapMat.uniforms.uShow.value = [s.corners, s.centres, s.mids, s.fcentres, s.centres && s.fillets, s.fcentres && s.fillets].map((x) => (x ? 1 : 0));
  }

  _okPoint(c) {
    const s = this.show;
    if (c.kind === 'vertex') return s.corners;
    if (c.kind === 'mid') return s.mids;
    if (c.kind === 'fcentre') return s.fcentres && (!c.fillet || s.fillets);
    return s.centres && (!c.fillet || s.fillets);
  }

  _okAxis(a) { return this.show.axes && (!a.fillet || this.show.fillets); }

  /** Show (or hide) the dots for every snappable point of the visible model. Built lazily. */
  refreshOverview() {
    const v = this.viewer;
    const on = this.active && this.snap && this.showSnaps && !this.lock;
    this.snapMat.clippingPlanes = v.clipPlanes.length ? v.clipPlanes : null;
    for (const m of [this.snapMat, this.lockPtsMat]) {
      m.uniforms.uScale.value = window.devicePixelRatio || 1;
      m.uniforms.uBias.value = v.modelRadius * 0.006;
    }
    const todo = [];
    v.forEachMesh((m) => {
      if (m.userData.snapPts) m.userData.snapPts.visible = on;
      else if (on) todo.push(m);
    });
    this._snapQueue = todo;
    if (on && todo.length && !this._building) {
      this._building = true;
      const step = (dl) => {
        while (this._snapQueue.length && (!dl || !dl.timeRemaining || dl.timeRemaining() > 3)) {
          const m = this._snapQueue.pop();
          if (!m.parent || m.userData.snapPts) continue;
          this._buildPts(m);
        }
        v.requestRender();
        if (this._snapQueue.length) (window.requestIdleCallback || setTimeout)(step);
        else this._building = false;
      };
      (window.requestIdleCallback || setTimeout)(step);
    }
    v.requestRender();
  }

  _pointsObject(list, mat) {
    const n = list.length;
    const pos = new Float32Array(n * 3), kind = new Float32Array(n);
    list.forEach((c, k) => { pos[k * 3] = c.p.x; pos[k * 3 + 1] = c.p.y; pos[k * 3 + 2] = c.p.z; kind[k] = KIND_OF(c); });
    const g = new THREE.BufferGeometry();
    g.setAttribute('position', new THREE.BufferAttribute(pos, 3));
    g.setAttribute('kind', new THREE.BufferAttribute(kind, 1));
    g.computeBoundingSphere();
    const pts = new THREE.Points(g, mat);
    pts.raycast = () => {};
    pts.renderOrder = 997;
    return pts;
  }

  _buildPts(mesh) {
    const s = snapsOf(mesh);
    const list = [];
    for (let i = 0; i < s.mids.length; i += 3) list.push({ p: { x: s.mids[i], y: s.mids[i + 1], z: s.mids[i + 2] }, kind: 'mid' });
    for (let i = 0; i < s.corners.length; i += 3) list.push({ p: { x: s.corners[i], y: s.corners[i + 1], z: s.corners[i + 2] }, kind: 'vertex' });
    for (const c of s.fcentres) list.push({ p: c.p, kind: 'fcentre', fillet: c.fillet });
    for (const c of s.centres) list.push(c);
    if (!list.length) return;
    const pts = this._pointsObject(list, this.snapMat);
    pts.userData.isOverlay = true;
    pts.visible = this.active && this.snap && this.showSnaps && !this.lock;
    mesh.add(pts);
    mesh.userData.snapPts = pts;
  }

  /** Dashed axis lines of the part under the cursor, so axes can be found and picked. */
  _showAxes(mesh) {
    if (this._axesOn === mesh) return;
    if (this._axesOn && this._axesOn.userData.axisLines) this._axesOn.userData.axisLines.visible = false;
    this._axesOn = mesh;
    if (!mesh || this.mode !== 'smart') return;
    let L = mesh.userData.axisLines;
    if (!L) {
      const ax = snapsOf(mesh).axes.filter((a) => this._okAxis(a));
      if (!ax.length) return;
      const R = this.viewer.modelRadius;
      const pts = [];
      for (const a of ax) {
        const d = a.B.clone().sub(a.A);
        const len = d.length() || 1;
        d.divideScalar(len);
        const ext = Math.max(len * 0.15, R * 0.03);
        pts.push(a.A.clone().addScaledVector(d, -ext), a.B.clone().addScaledVector(d, ext));
      }
      const g = new THREE.BufferGeometry().setFromPoints(pts);
      L = new THREE.LineSegments(g, this.faintAxisMat);
      L.computeLineDistances();
      this.faintAxisMat.dashSize = this.faintAxisMat.gapSize = R / 120;
      L.userData.isOverlay = true;
      L.raycast = () => {};
      L.renderOrder = 996;
      mesh.add(L);
      mesh.userData.axisLines = L;
    }
    L.visible = true;
  }

  // ---------------------------------------------------------------- face lock (Shift / Alt)
  _lockTo(mesh, fid) {
    if (!mesh.userData.faces || fid < 0) return;
    this._unlock();
    const d = faceData(mesh, fid);
    const fx = [this._faceOverlay(mesh, fid, this.lockMat)];
    const fe = this._faceEdges(mesh, fid);
    if (fe) fx.push(fe);
    const list = d.pts.filter((c) => c.kind === 'vertex' || c.kind === 'mid' || this._okPoint(c) || c.kind === 'fcentre');
    if (list.length) {
      this.lockPtsMat.clippingPlanes = this.viewer.clipPlanes.length ? this.viewer.clipPlanes : null;
      const pts = this._pointsObject(list, this.lockPtsMat);
      mesh.add(pts);
      fx.push(pts);
    }
    this.lock = { mesh, fid, data: d, list, fx };
    this.refreshOverview();
    this.viewer.requestRender();
  }

  _unlock() {
    if (!this.lock) return;
    this._clearFx(this.lock.fx);
    this.lock = null;
    this.refreshOverview();
    this.viewer.requestRender();
  }

  // ---------------------------------------------------------------- picking
  _alive(m) {
    if (!m || !m.parent || !isModelMesh(m)) return false;
    for (let p = m; p; p = p.parent) if (!p.visible) return false;
    return true;
  }

  /** Is a world point visible (not hidden behind geometry, not cut away)? */
  _visible(p) {
    const v = this.viewer;
    for (const pl of v.clipPlanes) if (pl.distanceToPoint(p) < 0) return false;
    const ndc = p.clone().project(v.camera);
    if (ndc.z > 1 || ndc.z < -1) return false;
    const h = v.pickAt(new THREE.Vector2(ndc.x, ndc.y));
    if (!h) return true;
    const ray = v._raycaster.ray;
    const t = p.clone().sub(ray.origin).dot(ray.direction);
    return h.distance >= t - v.modelRadius * 0.004;
  }

  _screen(ev) {
    const v = this.viewer, r = v.canvas.getBoundingClientRect();
    const cam = v.camera;
    return { r, mx: ev.clientX - r.left, my: ev.clientY - r.top, pv: new THREE.Matrix4().multiplyMatrices(cam.projectionMatrix, cam.matrixWorldInverse) };
  }

  /** Nearest point on line segments (local, flat xyz pairs) to the cursor, within maxPx. */
  _onSegs(segs, mesh, sc, maxPx, filter) {
    const vp = sc.pv.clone().multiply(mesh.matrixWorld);
    const a = new THREE.Vector3(), b = new THREE.Vector3();
    let best = null, bestD = maxPx * maxPx;
    for (let i = 0; i < segs.length; i += 6) {
      if (filter && !filter(i)) continue;
      a.set(segs[i], segs[i + 1], segs[i + 2]).applyMatrix4(vp);
      b.set(segs[i + 3], segs[i + 4], segs[i + 5]).applyMatrix4(vp);
      if (a.z > 1 || b.z > 1) continue;
      const ax = ((a.x + 1) / 2) * sc.r.width, ay = ((1 - a.y) / 2) * sc.r.height;
      const bx = ((b.x + 1) / 2) * sc.r.width, by = ((1 - b.y) / 2) * sc.r.height;
      const dx = bx - ax, dy = by - ay, L2 = dx * dx + dy * dy;
      const t = L2 > 1e-9 ? Math.max(0, Math.min(1, ((sc.mx - ax) * dx + (sc.my - ay) * dy) / L2)) : 0;
      const d2 = (ax + t * dx - sc.mx) ** 2 + (ay + t * dy - sc.my) ** 2;
      if (d2 < bestD) { bestD = d2; best = { i, t }; }
    }
    if (!best) return null;
    const i = best.i, t = best.t;
    return new THREE.Vector3(segs[i] + (segs[i + 3] - segs[i]) * t, segs[i + 1] + (segs[i + 4] - segs[i + 1]) * t, segs[i + 2] + (segs[i + 5] - segs[i + 2]) * t).applyMatrix4(mesh.matrixWorld);
  }

  /** Nearest snap candidate from a list [{p(local), kind…}] (radius per kind). */
  _bestOf(list, mesh, sc, checkVis) {
    const vp = sc.pv.clone().multiply(mesh.matrixWorld);
    const tmp = new THREE.Vector3();
    const RAD = { vertex: 11, mid: 9, centre: 15, sphere: 15, fcentre: 12 };
    const cands = [];
    for (const c of list) {
      tmp.set(c.p.x, c.p.y, c.p.z).applyMatrix4(vp);
      if (tmp.z > 1) continue;
      const sx = ((tmp.x + 1) / 2) * sc.r.width, sy = ((1 - tmp.y) / 2) * sc.r.height;
      const rad = RAD[c.kind] || 10;
      const d2 = (sx - sc.mx) ** 2 + (sy - sc.my) ** 2;
      if (d2 < rad * rad) cands.push({ s: d2 / (rad * rad), c });
    }
    cands.sort((a, b) => a.s - b.s);
    for (const { c } of cands.slice(0, 5)) {
      const p = new THREE.Vector3(c.p.x, c.p.y, c.p.z).applyMatrix4(mesh.matrixWorld);
      if (checkVis && !this._visible(p)) continue;
      return { mesh, fid: -1, snapped: true, ent: { k: 'point', p, kind: c.kind, r: c.r, on: c.on, fillet: c.fillet } };
    }
    return null;
  }

  _snapPoint(ev, hit) {
    const sc = this._screen(ev);
    const meshes = [...new Set([hit && hit.object, this.axisMesh])].filter((m) => this._alive(m));
    let best = null;
    for (const mesh of meshes) {
      const s = snapsOf(mesh);
      const list = [];
      if (this.show.corners) for (let i = 0; i < s.corners.length; i += 3) list.push({ p: { x: s.corners[i], y: s.corners[i + 1], z: s.corners[i + 2] }, kind: 'vertex' });
      if (this.show.mids) for (let i = 0; i < s.mids.length; i += 3) list.push({ p: { x: s.mids[i], y: s.mids[i + 1], z: s.mids[i + 2] }, kind: 'mid' });
      for (const c of s.centres) if (this._okPoint(c)) list.push(c);
      for (const c of s.fcentres) if (this._okPoint({ ...c, kind: 'fcentre' })) list.push({ ...c, kind: 'fcentre' });
      best = this._bestOf(list, mesh, sc, true);
      if (best) return best;
    }
    // any point along the edges near the cursor (hovered part only)
    if (!hit) return null;
    const mesh = hit.object, E = mesh.userData.edges;
    if (!E) return null;
    const arr = E.geometry.attributes.position.array;
    const local = hit.point.clone().applyMatrix4(mesh.matrixWorld.clone().invert());
    const rad2 = Math.pow(this.viewer.modelRadius * 0.15, 2);
    const near = (i) => {
      const x = (arr[i] + arr[i + 3]) / 2 - local.x, y = (arr[i + 1] + arr[i + 4]) / 2 - local.y, z = (arr[i + 2] + arr[i + 5]) / 2 - local.z;
      return x * x + y * y + z * z < rad2;
    };
    const p = this._onSegs(arr, mesh, sc, 7, near);
    if (!p) return null;
    return { mesh, fid: -1, snapped: true, ent: { k: 'point', kind: 'edge', p } };
  }

  /** Axis whose screen projection passes within a few pixels of the cursor. */
  _nearAxis(ev) {
    const mesh = this.axisMesh;
    if (!this._alive(mesh)) return null;
    const ax = snapsOf(mesh).axes.filter((a) => this._okAxis(a));
    if (!ax.length) return null;
    const v = this.viewer;
    const sc = this._screen(ev);
    const vp = sc.pv.clone().multiply(mesh.matrixWorld);
    const a = new THREE.Vector3(), b = new THREE.Vector3();
    let best = null, bestD = 6 * 6;
    for (const rec of ax) {
      const d = rec.B.clone().sub(rec.A), len = d.length() || 1;
      d.divideScalar(len);
      const ext = Math.max(len * 0.15, v.modelRadius * 0.03);
      a.copy(rec.A).addScaledVector(d, -ext).applyMatrix4(vp);
      b.copy(rec.B).addScaledVector(d, ext).applyMatrix4(vp);
      if (a.z > 1 || b.z > 1) continue;
      const ax0 = ((a.x + 1) / 2) * sc.r.width, ay0 = ((1 - a.y) / 2) * sc.r.height;
      const ax1 = ((b.x + 1) / 2) * sc.r.width, ay1 = ((1 - b.y) / 2) * sc.r.height;
      const dx = ax1 - ax0, dy = ay1 - ay0, L2 = dx * dx + dy * dy;
      if (L2 < 4) continue; // axis seen end-on – use the centre snap instead
      const t = Math.max(0, Math.min(1, ((sc.mx - ax0) * dx + (sc.my - ay0) * dy) / L2));
      const d2 = (ax0 + t * dx - sc.mx) ** 2 + (ay0 + t * dy - sc.my) ** 2;
      if (d2 < bestD) { bestD = d2; best = rec; }
    }
    if (!best) return null;
    return { mesh, fid: best.faces ? best.faces[0] : best.fid, ent: axisEntity(best, mesh), onAxis: true };
  }

  /** Face entity for (mesh, fid) at a world point p (or the face centre). */
  _faceEntity(mesh, fid, p, point) {
    const prim = mesh.userData.faces.prims[fid];
    if (!point && prim) {
      const e = entityFromPrim(prim, mesh, p, fid);
      if (e) return e;
    }
    return { k: 'point', kind: 'surface', p, on: prim && prim.t !== 'other' ? prim.t : null };
  }

  _probeLocked(ev) {
    const L = this.lock;
    if (!this._alive(L.mesh)) { this._unlock(); return null; }
    const sc = this._screen(ev);
    const pts = L.list.filter((c) => c.kind !== 'fcentre' || true);
    const s = this._bestOf(pts, L.mesh, sc, false);
    if (s) return { ...s, locked: true };
    const p = this._onSegs(L.data.segs, L.mesh, sc, 9);
    if (p) return { mesh: L.mesh, fid: -1, snapped: true, locked: true, ent: { k: 'point', kind: 'edge', p } };
    // the face itself – at the cursor if over it, otherwise at its centre
    const hit = this.viewer.pick(ev);
    const fot = hit && hit.object === L.mesh ? faceOf(L.mesh) : null;
    const prim = L.mesh.userData.faces.prims[L.fid];
    let at;
    if (fot && fot[hit.faceIndex] === L.fid) at = exactPoint(prim, L.mesh, hit.point);
    else {
      const fc = snapsOf(L.mesh).fcentres.find((x) => x.fid === L.fid);
      at = fc ? fc.p.clone().applyMatrix4(L.mesh.matrixWorld) : (hit ? hit.point : new THREE.Vector3());
    }
    return { mesh: L.mesh, fid: L.fid, locked: true, faceOnly: true, ent: this._faceEntity(L.mesh, L.fid, at, this.mode === 'point') };
  }

  /** What would be picked under the cursor. */
  probe(ev) {
    this.mode = ev.ctrlKey || ev.metaKey ? 'point' : 'smart'; // Ctrl = free point on the exact surface
    const held = !!(ev.shiftKey || ev.altKey);
    if (!held && this.lock) this._unlock();
    if (this.lock) return this._probeLocked(ev);
    const hit = this.viewer.pick(ev);
    if (hit) this.axisMesh = hit.object;
    if (held && hit) {
      const fot = faceOf(hit.object);
      const fid = fot && hit.faceIndex != null ? fot[hit.faceIndex] : -1;
      if (fid >= 0) { this._lockTo(hit.object, fid); return this._probeLocked(ev); }
    }
    if (!held) {
      if (this.snap) { const s = this._snapPoint(ev, hit); if (s) return s; }
      if (this.mode === 'smart') { const a = this._nearAxis(ev); if (a) return a; }
    }
    if (!hit) return null;
    const mesh = hit.object;
    const fot = faceOf(mesh);
    const fid = fot && hit.faceIndex != null ? fot[hit.faceIndex] : -1;
    const prim = fid >= 0 ? mesh.userData.faces.prims[fid] : null;
    const p = exactPoint(prim, mesh, hit.point);
    if (fid >= 0) {
      const e = this._faceEntity(mesh, fid, p, this.mode !== 'smart');
      return { mesh, fid, ent: e, surf: e.k === 'point' };
    }
    return { mesh, fid, ent: { k: 'point', kind: 'surface', p }, surf: true };
  }

  _hoverKey(h) { return h ? `${h.mesh.id}|${h.fid}|${h.ent.k}|${h.ent.kind || ''}|${h.onAxis ? 1 : 0}|${h.locked ? 1 : 0}` : ''; }

  _setHover(h) {
    const key = this._hoverKey(h);
    const same = h && h.fid >= 0 && !h.snapped && key === this._hoverK;
    if (!same) {
      this._clearFx(this.hoverFx);
      if (h && h.fid >= 0 && !h.snapped && !h.locked) {
        if (h.ent.k !== 'point' || (this.mode === 'smart' && !h.surf)) {
          this.hoverFx.push(this._faceOverlay(h.mesh, h.fid, this.faceMat));
          const fe = this._faceEdges(h.mesh, h.fid);
          if (fe) this.hoverFx.push(fe);
        }
      }
      if (h && h.ent.k === 'axis' && !h.locked) { const l = this._axisTube(h.ent, 0x5e5ce6); this.viewer.overlay.add(l); this.hoverFx.push(l); }
    }
    this._hoverK = key;
    this.hover = h;
    this._showAxes(this.active && this._alive(this.axisMesh) && !this.lock ? this.axisMesh : null);
  }

  onMove(ev) {
    if (!this.active) return;
    this.lastEv = { clientX: ev.clientX, clientY: ev.clientY };
    const h = this.probe(ev);
    this._setHover(h);
    const r = this.layer.getBoundingClientRect();
    const c = this.cursor;
    if (h) {
      const d = describe(h.ent);
      const anchor = h.ent.k === 'point' ? h.ent.p : h.onAxis ? null : h.ent.click;
      if (anchor) {
        const s = this.viewer.toScreen(anchor);
        c.style.display = 'block';
        c.style.transform = `translate(${s.x - r.left}px, ${s.y - r.top}px)`;
      } else c.style.display = 'none';
      c.className = `m-cursor k-${h.snapped ? d.cls : h.ent.k === 'point' ? 'point' : 'face'}${h.snapped ? ' snapped' : ''}`;
      let extra = '';
      if (this.pending) extra = `<b class="tres">→ ${measureEntities(this.pending, h.ent).main}</b>`;
      const mods = h.locked ? '<em>🔒 this face</em>' : '';
      this._tag(`<span class="tk tk-${d.cls}">${d.type}</span>${d.detail ? `<span>${d.detail}</span>` : ''}${mods}${extra}`, ev);
    } else {
      c.style.display = 'none';
      this._tag(null);
    }
    if (this.pending && h) {
      const a = anchorOf(this.pending);
      const b = h.ent.k === 'point' ? h.ent.p : h.onAxis ? mid(h.ent) : h.ent.click;
      const pos = this.rubber.geometry.attributes.position;
      pos.setXYZ(0, a.x, a.y, a.z); pos.setXYZ(1, b.x, b.y, b.z);
      pos.needsUpdate = true;
      this.rubber.geometry.computeBoundingSphere();
      this._dash(this.rubber);
      this.rubber.visible = true;
    } else this.rubber.visible = false;
    this.viewer.requestRender();
  }

  _tag(html, ev) {
    if (!this.tag) return;
    if (!html) { this.tag.classList.remove('show'); return; }
    this.tag.innerHTML = html;
    this.tag.classList.add('show');
    this.tag.style.transform = `translate(${ev.clientX + 16}px, ${ev.clientY + 18}px)`;
  }

  onLeave() { this.cursor.style.display = 'none'; this._tag(null); this._setHover(null); this.viewer.requestRender(); }

  onClick(ev) {
    if (!this.active || !this.list) return false;
    const h = this.probe(ev);
    if (!h) return true;
    if (!this.pending) {
      this.pending = h.ent;
      if (h.fid >= 0 && !h.snapped && (h.ent.k !== 'point' || h.faceOnly)) this.pendingFx.push(this._faceOverlay(h.mesh, h.fid, this.pickMat));
      if (h.ent.k === 'axis') { const l = this._axisTube(h.ent, 0xff9f0a); this.viewer.overlay.add(l); this.pendingFx.push(l); }
      if (h.ent.k === 'point') {
        const d = document.createElement('div');
        d.className = 'm-dot pending';
        this.layer.appendChild(d);
        this.pendingFx.push({ userData: {}, parent: { remove: () => d.remove() }, geometry: { dispose() {} }, _dom: d, _p: h.ent.p.clone() });
      }
      this.dispatchEvent(new Event('pending'));
      this.layout();
      return true;
    }
    this.add(this.pending, h.ent);
    this.cancel();
    return true;
  }

  // ---------------------------------------------------------------- measurements
  add(A, B) {
    const group = new THREE.Group();
    this.group.add(group);
    const el = document.createElement('div');
    el.className = 'm-label';
    el.innerHTML = '<b></b><span></span><i title="Remove">×</i>';
    this.layer.appendChild(el);
    const m = { A, B, group, el, dots: [], legEls: [], labelPos: null };
    this._drag(m);
    el.querySelector('i').addEventListener('click', (e) => { e.stopPropagation(); this.remove(m); });
    this.list.push(m);
    this.render(m);
    this.dispatchEvent(new Event('change'));
  }

  /** Drag the result label anywhere (it stays put in 3D); double-click puts it back. */
  _drag(m) {
    const el = m.el;
    el.addEventListener('pointerdown', (e) => {
      if (e.button !== 0 || e.target.tagName === 'I') return;
      e.preventDefault();
      e.stopPropagation();
      el.setPointerCapture(e.pointerId);
      const v = this.viewer;
      const start = (m.labelPos || m.anchor).clone();
      const n = v.currentViewDir ? v.currentViewDir().clone() : new THREE.Vector3(0, 0, 1);
      const plane = new THREE.Plane().setFromNormalAndCoplanarPoint(n, start);
      const rc = new THREE.Raycaster();
      const at = (ev) => { rc.setFromCamera(v.pointerNDC(ev), v.camera); return rc.ray.intersectPlane(plane, new THREE.Vector3()); };
      const p0 = at(e);
      if (!p0) return;
      el.classList.add('dragging');
      const move = (ev) => {
        const p = at(ev);
        if (!p) return;
        m.labelPos = start.clone().add(p.sub(p0));
        this._leader(m);
        v.requestRender();
      };
      const up = () => { el.classList.remove('dragging'); el.removeEventListener('pointermove', move); el.removeEventListener('pointerup', up); };
      el.addEventListener('pointermove', move);
      el.addEventListener('pointerup', up);
    });
    el.addEventListener('dblclick', (e) => { e.stopPropagation(); m.labelPos = null; this._leader(m); this.viewer.requestRender(); });
  }

  _leader(m) {
    if (m.leader) { m.group.remove(m.leader); m.leader.geometry.dispose(); m.leader = null; }
    if (!m.labelPos) return;
    m.leader = this._line([m.anchor, m.labelPos], false, this.leaderMat);
    m.group.add(m.leader);
  }

  /** (Re)build a measurement's 3D graphics and text from its entities. */
  render(m) {
    for (const o of [...m.group.children]) { m.group.remove(o); o.geometry?.dispose(); }
    m.leader = null;
    m.dots.forEach((d) => d.remove());
    m.legEls.forEach((d) => d.remove());
    m.dots = []; m.legEls = [];
    const res = (m.res = measureEntities(m.A, m.B));
    if (res.line && !res.angle) m.group.add(this._line(res.line));
    for (const e of [m.A, m.B]) if (e.k === 'axis') m.group.add(this._axisLine(e));
    if (res.arc) {
      const a = res.arc;
      const fill = this._arcFill(a);
      if (fill) m.group.add(fill);
      m.group.add(this._line(a.pts, false, this.arcMat));
      for (const r of a.rays) m.group.add(this._line(r, false, this.arcMat));
      for (const l of a.links || []) m.group.add(this._line(l, true));
      if (a.axis) m.group.add(this._line(a.axis, true));
    }
    const pts = res.line ? res.line : [anchorOf(m.A), anchorOf(m.B)];
    m.pts = pts;
    m.dots = pts.map(() => { const d = document.createElement('div'); d.className = 'm-dot'; this.layer.appendChild(d); return d; });
    m.anchor = res.line ? res.line[0].clone().add(res.line[1]).multiplyScalar(0.5) : (res.anchor || pts[0]).clone();
    // X / Y / Z legs of the measured vector
    m.legs = [];
    if (res.line && !res.angle) {
      const a = res.line[0], b = res.line[1], d = b.clone().sub(a);
      m.delta = d;
      if (this.xyz) {
        let p = a.clone();
        ['x', 'y', 'z'].forEach((ax, k) => {
          if (Math.abs(d[ax]) < 1e-6) return;
          const q = p.clone();
          q[ax] += d[ax];
          const l = this._line([p, q], false, this.legMats[k].clone());
          l.computeLineDistances();
          l.material.dashSize = l.material.gapSize = Math.max(Math.abs(d[ax]) / 24, 1e-3);
          m.group.add(l);
          const el = document.createElement('div');
          el.className = `m-leg m-leg-${ax}`;
          el.textContent = `${ax.toUpperCase()} ${fLen(Math.abs(d[ax]))}`;
          this.layer.appendChild(el);
          m.legEls.push(el);
          m.legs.push(p.clone().add(q).multiplyScalar(0.5));
          p = q;
        });
      }
    } else m.delta = null;
    m.el.querySelector('b').textContent = res.main;
    m.el.querySelector('span').textContent = `${res.kind} · ${res.sub}`;
    m.text = `${res.kind}: ${res.main}`;
    m.from = `${labelOf(m.A)} → ${labelOf(m.B)}`;
    this._leader(m);
    if (this.list && !this.list.includes(m)) this._hideM(m);
    this.viewer.requestRender();
  }

  _hideM(m) { m.group.visible = false; m.el.style.display = 'none'; m.dots.forEach((d) => { d.hidden = true; }); m.legEls.forEach((d) => { d.hidden = true; }); }

  /** Change near / centre / far of one cylinder of a measurement. */
  setCond(m, which, cond) {
    const e = which === 'A' ? m.A : m.B;
    e.cond = cond;
    this.render(m);
    this.dispatchEvent(new Event('change'));
  }

  setXyz(on, lists) {
    this.xyz = on;
    for (const l of lists) for (const m of l) this.render(m);
    for (const l of lists) if (l !== this.list) for (const m of l) this._hideM(m);
  }

  /** Re-format every measurement (after a unit change). */
  relabel(lists) {
    for (const l of lists) for (const m of l) this.render(m);
    for (const l of lists) if (l !== this.list) for (const m of l) this._hideM(m);
    this.dispatchEvent(new Event('change'));
  }

  remove(m) {
    const i = this.list.indexOf(m);
    if (i >= 0) this.list.splice(i, 1);
    this.group.remove(m.group);
    m.el.remove();
    m.dots.forEach((d) => d.remove());
    m.legEls.forEach((d) => d.remove());
    this.dispatchEvent(new Event('change'));
    this.viewer.requestRender();
  }

  clear() { for (const m of [...(this.list || [])]) this.remove(m); }

  dispose(list) { for (const m of list) { this.group.remove(m.group); m.el.remove(); m.dots.forEach((d) => d.remove()); m.legEls.forEach((d) => d.remove()); } }

  layout() {
    const r = this.layer.getBoundingClientRect();
    for (const o of this.pendingFx) {
      if (!o._dom) continue;
      const q = this.viewer.toScreen(o._p);
      o._dom.style.transform = `translate(${q.x - r.left}px, ${q.y - r.top}px)`;
    }
    if (!this.list) return;
    for (const m of this.list) {
      const s = this.viewer.toScreen(m.labelPos || m.anchor);
      if (!m.labelPos && m.res && m.res.arc) {
        // angle: sit just outside the arc, along the bisector, so the arc stays visible
        const o = this.viewer.toScreen(m.res.arc.O);
        let dx = s.x - o.x, dy = s.y - o.y;
        const L = Math.hypot(dx, dy) || 1;
        dx /= L; dy /= L;
        const w = m.el.offsetWidth || 120, h = m.el.offsetHeight || 40;
        const push = Math.abs(dx) * w / 2 + Math.abs(dy) * h / 2 + 10;
        m.el.style.transform = `translate(${s.x + dx * push - r.left}px, ${s.y + dy * push - r.top}px) translate(-50%, -50%)`;
      } else m.el.style.transform = m.labelPos
        ? `translate(${s.x - r.left}px, ${s.y - r.top}px) translate(-50%, -50%)`
        : `translate(${s.x - r.left}px, ${s.y - r.top}px) translate(-50%, -130%)`;
      m.el.style.visibility = s.behind ? 'hidden' : '';
      m.pts.forEach((p, i) => {
        const q = this.viewer.toScreen(p);
        m.dots[i].style.transform = `translate(${q.x - r.left}px, ${q.y - r.top}px)`;
        m.dots[i].style.display = q.behind ? 'none' : '';
      });
      m.legs.forEach((p, i) => {
        const q = this.viewer.toScreen(p);
        m.legEls[i].style.transform = `translate(${q.x - r.left}px, ${q.y - r.top}px) translate(-50%, -50%)`;
      });
    }
  }
}
