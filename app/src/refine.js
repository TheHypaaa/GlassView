import * as THREE from 'three';
import { kernelCall } from './kernel.js';
import { readGvc, cachePath } from './loaders.js';
import * as NL from '@neutralinojs/lib';

// CAD-style, per-face refinement with the native engine: only faces that are in view and whose
// chord error exceeds ~half a pixel are re-meshed; the new triangles are spliced into the part.

/** Vertex range [v0, v1) of a face's triangle range (faces own contiguous vertex blocks). */
function vrange(index, t0, t1) {
  let a = Infinity, b = -1;
  for (let i = t0 * 3; i < (t1 + 1) * 3; i++) { const v = index[i]; if (v < a) a = v; if (v > b) b = v; }
  return b < 0 ? [0, 0] : [a, b + 1];
}

/** Bounding sphere (local) per face, cached on the mesh. */
function faceSpheres(mesh) {
  if (mesh.userData.faceSph) return mesh.userData.faceSph;
  const F = mesh.userData.faces, P = mesh.geometry.attributes.position.array, I = mesh.geometry.index.array;
  const n = F.ranges.length / 2, out = new Float32Array(n * 4);
  for (let f = 0; f < n; f++) {
    const t0 = F.ranges[f * 2], t1 = F.ranges[f * 2 + 1];
    if (t1 < t0) { out[f * 4 + 3] = -1; continue; }
    const [v0, v1] = vrange(I, t0, t1);
    let x0 = Infinity, y0 = Infinity, z0 = Infinity, x1 = -Infinity, y1 = -Infinity, z1 = -Infinity;
    for (let v = v0; v < v1; v++) {
      const x = P[v * 3], y = P[v * 3 + 1], z = P[v * 3 + 2];
      if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; if (z < z0) z0 = z; if (z > z1) z1 = z;
    }
    out[f * 4] = (x0 + x1) / 2; out[f * 4 + 1] = (y0 + y1) / 2; out[f * 4 + 2] = (z0 + z1) / 2;
    out[f * 4 + 3] = Math.hypot(x1 - x0, y1 - y0, z1 - z0) / 2;
  }
  mesh.userData.faceSph = out;
  return out;
}

/**
 * Faces of a native mesh that need refining for the current view.
 * Returns [{ f, target, tris, gain }] sorted by benefit.
 */
export function refineCandidates(viewer, mesh, frustum, H, baseDefl) {
  const F = mesh.userData.faces;
  if (!F || !F.ranges) return [];
  const n = F.ranges.length / 2;
  if (!mesh.userData.faceDefl) mesh.userData.faceDefl = new Float32Array(n).fill(baseDefl);
  const defl = mesh.userData.faceDefl;
  const sph = faceSpheres(mesh);
  const cam = viewer.camera;
  const toTarget = cam.position.distanceTo(viewer.controls.target);
  const c = new THREE.Vector3(), s = new THREE.Sphere();
  const tanH = Math.tan(THREE.MathUtils.degToRad(cam.fov || 30) / 2);
  const out = [];
  for (let f = 0; f < n; f++) {
    const r = sph[f * 4 + 3];
    if (r < 0) continue;
    c.set(sph[f * 4], sph[f * 4 + 1], sph[f * 4 + 2]).applyMatrix4(mesh.matrixWorld);
    s.set(c, r);
    if (!frustum.intersectsSphere(s)) continue;
    const dist = Math.max(cam.position.distanceTo(c) - r, toTarget * 0.6, 1e-3);
    const wpp = cam.isPerspectiveCamera ? (2 * dist * tanH) / H : (cam.top - cam.bottom) / cam.zoom / H;
    const want = wpp * 0.5;
    const cur = defl[f];
    if (want > cur / 2.5) continue;
    const target = Math.max(Math.pow(2, Math.floor(Math.log2(want))), r * 2e-5, 1e-4);
    if (target > cur / 1.8) continue;
    const tris = F.ranges[f * 2 + 1] - F.ranges[f * 2] + 1;
    out.push({ f, target, tris, gain: cur / target, extra: tris * (Math.pow(cur / target, 0.8) - 1) });
  }
  out.sort((a, b) => b.gain - a.gain);
  return out;
}

/** Replace some faces of a mesh with new triangulations (sub = one-mesh .gvc in `faces` order). */
export function spliceFaces(viewer, mesh, faceIds, sub) {
  // a repeated part shares geometry, face data and edges with its copies: give this one its own first
  const shared = !!mesh.userData.instanced;
  if (shared) {
    mesh.userData.faces = { ...mesh.userData.faces };
    if (mesh.userData.edges) mesh.userData.edges.geometry = mesh.userData.edges.geometry.clone();
    mesh.userData.instanced = false;
  }
  const g = mesh.geometry;
  const F = mesh.userData.faces;
  const P = g.attributes.position.array, N = g.attributes.normal.array, I = g.index.array;
  const C = g.attributes.color ? g.attributes.color.array : null;
  const E = mesh.userData.edges ? mesh.userData.edges.geometry.attributes.position.array : new Float32Array(0);
  const sd = sub.data.meshes[0], sf = sub.faces[0], se = sub.edges[0] || new Float32Array(0);
  const n = F.ranges.length / 2;
  const map = new Map();
  faceIds.forEach((f, k) => map.set(f, k));
  // pass 1: sizes
  const plan = new Array(n);
  let nv = 0, nt = 0, ns = 0;
  for (let f = 0; f < n; f++) {
    const k = map.get(f);
    const src = k === undefined
      ? { P, N, C, I, E, t0: F.ranges[f * 2], t1: F.ranges[f * 2 + 1], s0: F.eranges ? F.eranges[f * 2] : 0, sc: F.eranges ? F.eranges[f * 2 + 1] : 0 }
      : { P: sd.position, N: sd.normal, C: sd.colors, I: sd.index, E: se, t0: sf.ranges[k * 2], t1: sf.ranges[k * 2 + 1], s0: sf.eranges[k * 2], sc: sf.eranges[k * 2 + 1] };
    const [v0, v1] = src.t1 >= src.t0 ? vrange(src.I, src.t0, src.t1) : [0, 0];
    src.v0 = v0; src.v1 = v1;
    plan[f] = src;
    nv += v1 - v0; nt += Math.max(0, src.t1 - src.t0 + 1); ns += src.sc;
  }
  const NP = new Float32Array(nv * 3), NN = new Float32Array(nv * 3), NI = new Uint32Array(nt * 3);
  const NC = C ? new Uint8Array(nv * 3) : null;
  const NE = new Float32Array(ns * 6);
  const ranges = new Int32Array(n * 2), eranges = new Int32Array(n * 2);
  let vo = 0, to = 0, so = 0;
  for (let f = 0; f < n; f++) {
    const s = plan[f];
    const cnt = s.v1 - s.v0;
    NP.set(s.P.subarray(s.v0 * 3, s.v1 * 3), vo * 3);
    NN.set(s.N.subarray(s.v0 * 3, s.v1 * 3), vo * 3);
    if (NC) {
      if (s.C) NC.set(s.C.subarray(s.v0 * 3, s.v1 * 3), vo * 3);
      else NC.fill(200, vo * 3, (vo + cnt) * 3);
    }
    const tc = Math.max(0, s.t1 - s.t0 + 1);
    ranges[f * 2] = to; ranges[f * 2 + 1] = to + tc - 1;
    const shift = vo - s.v0;
    for (let i = s.t0 * 3, j = to * 3; i < (s.t0 + tc) * 3; i++, j++) NI[j] = s.I[i] + shift;
    NE.set(s.E.subarray(s.s0 * 6, (s.s0 + s.sc) * 6), so * 6);
    eranges[f * 2] = so; eranges[f * 2 + 1] = s.sc;
    vo += cnt; to += tc; so += s.sc;
  }
  const ng = new THREE.BufferGeometry();
  ng.setAttribute('position', new THREE.BufferAttribute(NP, 3));
  ng.setAttribute('normal', new THREE.BufferAttribute(NN, 3));
  if (NC) ng.setAttribute('color', new THREE.BufferAttribute(NC, 3, true));
  ng.setIndex(new THREE.BufferAttribute(NI, 1));
  ng.computeBoundingSphere();
  ng.computeBoundingBox();
  // swap geometry and every overlay that shared the old one
  const old = mesh.geometry;
  mesh.geometry = ng;
  if (mesh.userData.tri) mesh.userData.tri.geometry = ng;
  for (const slot of ['hoverOverlay', 'pickOverlay']) {
    if (mesh.userData[slot]) { mesh.remove(mesh.userData[slot]); delete mesh.userData[slot]; }
  }
  if (mesh.userData.edges) {
    const eg = mesh.userData.edges.geometry;
    eg.setAttribute('position', new THREE.BufferAttribute(NE, 3));
    eg.deleteAttribute('soft');
    eg.computeBoundingSphere();
    if (viewer.edgeStyle === 'catia') viewer.queueEdgeClassify(mesh);
  } else if (NE.length) viewer.attachEdges(mesh, NE);
  if (!shared) {
    if (old.boundsTree) old.disposeBoundsTree();
    old.dispose();
  }
  F.ranges = ranges;
  F.eranges = eranges;
  F.faceOfTri = null;
  F.centers = null;
  mesh.userData.snaps = null;
  mesh.userData.faceData = null;
  if (mesh.userData.lockOverlay) { mesh.remove(mesh.userData.lockOverlay); delete mesh.userData.lockOverlay; }
  for (const slot of ['snapPts', 'axisLines', 'faceEdgeOverlay']) {
    const o = mesh.userData[slot];
    if (!o) continue;
    mesh.remove(o);
    if (slot !== 'faceEdgeOverlay') o.geometry.dispose(); // the face-edge overlay shares the edge buffer
    delete mesh.userData[slot];
  }
  mesh.userData.faceSph = null;
  viewer.prepareBVH(mesh);
}

let reqSeq = 0;
/** Ask the engine for new triangulations of some faces and splice them in. */
export async function refineFaces(viewer, mesh, faceIds, lin, ang) {
  const info = mesh.userData.native;
  const dir = await cachePath();
  const file = `refine-${Date.now().toString(36)}-${++reqSeq}.gvc`;
  await kernelCall({ cmd: 'refine', path: info.path, mesh: info.index, faces: faceIds, lin, ang, out: `${dir}/${file}` });
  const sub = await readGvc(file);
  NL.filesystem.remove(`${dir}/${file}`).catch(() => {});
  if (!mesh.parent) return false;
  spliceFaces(viewer, mesh, faceIds, sub);
  for (const f of faceIds) mesh.userData.faceDefl[f] = lin;
  return true;
}
