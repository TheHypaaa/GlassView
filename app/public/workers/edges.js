// Shared feature-edge extraction (classic worker script, loaded via importScripts).
// computeEdges(position, index|null, faceIdPerTri|null, thresholdDeg) -> Float32Array of line segments.
// - If faceIdPerTri is given (B-rep), an edge is emitted where adjacent triangles belong to different CAD faces.
// - Otherwise an edge is emitted where the dihedral angle exceeds thresholdDeg.
// - Open boundary edges are always emitted.
// Uses open-addressing hash tables on typed arrays (no strings) so it scales to millions of triangles.

var GV_EDGE_MAX_TRIS = 6000000;

function gvNextPow2(n) { var p = 1; while (p < n) p <<= 1; return p; }

function computeEdges(position, index, faceId, thresholdDeg) {
  var nVerts = position.length / 3;
  var nTris = index ? index.length / 3 : nVerts / 3;
  if (nTris === 0 || nTris > GV_EDGE_MAX_TRIS) return new Float32Array(0);

  // --- 1. weld vertices by quantised position --------------------------------
  var minX = Infinity, minY = Infinity, minZ = Infinity, maxX = -Infinity, maxY = -Infinity, maxZ = -Infinity;
  for (var i = 0; i < position.length; i += 3) {
    var x = position[i], y = position[i + 1], z = position[i + 2];
    if (x < minX) minX = x; if (x > maxX) maxX = x;
    if (y < minY) minY = y; if (y > maxY) maxY = y;
    if (z < minZ) minZ = z; if (z > maxZ) maxZ = z;
  }
  var diag = Math.sqrt((maxX - minX) ** 2 + (maxY - minY) ** 2 + (maxZ - minZ) ** 2) || 1;
  var inv = 1 / (diag * 1e-6);

  var vSize = gvNextPow2(nVerts * 2);
  var vMask = vSize - 1;
  var vTable = new Int32Array(vSize).fill(-1);
  var qx = new Int32Array(nVerts), qy = new Int32Array(nVerts), qz = new Int32Array(nVerts);
  var weld = new Int32Array(nVerts);
  var nUnique = 0;
  var uniq = new Int32Array(nVerts); // representative original vertex for each unique id
  for (var v = 0; v < nVerts; v++) {
    var a = Math.round((position[v * 3] - minX) * inv);
    var b = Math.round((position[v * 3 + 1] - minY) * inv);
    var c = Math.round((position[v * 3 + 2] - minZ) * inv);
    var h = (Math.imul(a, 73856093) ^ Math.imul(b, 19349663) ^ Math.imul(c, 83492791)) & vMask;
    for (;;) {
      var e = vTable[h];
      if (e === -1) {
        vTable[h] = nUnique; qx[nUnique] = a; qy[nUnique] = b; qz[nUnique] = c; uniq[nUnique] = v;
        weld[v] = nUnique++; break;
      }
      if (qx[e] === a && qy[e] === b && qz[e] === c) { weld[v] = e; break; }
      h = (h + 1) & vMask;
    }
  }
  vTable = null;

  // --- 2. triangle normals (only needed for angle mode) ----------------------
  var useAngle = !faceId;
  var cosT = Math.cos((thresholdDeg || 30) * Math.PI / 180);
  var tn = useAngle ? new Float32Array(nTris * 3) : null;
  function vid(t, k) { return index ? index[t * 3 + k] : t * 3 + k; }
  if (useAngle) {
    for (var t = 0; t < nTris; t++) {
      var i0 = vid(t, 0) * 3, i1 = vid(t, 1) * 3, i2 = vid(t, 2) * 3;
      var ux = position[i1] - position[i0], uy = position[i1 + 1] - position[i0 + 1], uz = position[i1 + 2] - position[i0 + 2];
      var wx = position[i2] - position[i0], wy = position[i2 + 1] - position[i0 + 1], wz = position[i2 + 2] - position[i0 + 2];
      var nx = uy * wz - uz * wy, ny = uz * wx - ux * wz, nz = ux * wy - uy * wx;
      var l = Math.sqrt(nx * nx + ny * ny + nz * nz) || 1;
      tn[t * 3] = nx / l; tn[t * 3 + 1] = ny / l; tn[t * 3 + 2] = nz / l;
    }
  }

  // --- 3. edge table ---------------------------------------------------------
  var eSize = gvNextPow2(Math.ceil(nTris * 3 * 0.75) + 16);
  var eMask = eSize - 1;
  var eA = new Int32Array(eSize).fill(-1);
  var eB = new Int32Array(eSize);
  var eTri = new Int32Array(eSize);
  var eState = new Uint8Array(eSize); // 1 = seen once, 2 = paired
  var out = [];
  var buf = new Float32Array(65536 * 6);
  var bufN = 0;
  function emit(ua, ub) {
    if (bufN + 6 > buf.length) { out.push(buf.slice(0, bufN)); bufN = 0; }
    var pa = uniq[ua] * 3, pb = uniq[ub] * 3;
    buf[bufN++] = position[pa]; buf[bufN++] = position[pa + 1]; buf[bufN++] = position[pa + 2];
    buf[bufN++] = position[pb]; buf[bufN++] = position[pb + 1]; buf[bufN++] = position[pb + 2];
  }

  for (var t2 = 0; t2 < nTris; t2++) {
    for (var k = 0; k < 3; k++) {
      var p = weld[vid(t2, k)], q = weld[vid(t2, (k + 1) % 3)];
      if (p === q) continue;
      var lo = p < q ? p : q, hi = p < q ? q : p;
      var hh = (Math.imul(lo, 2654435761) ^ Math.imul(hi, 40503)) & eMask;
      for (;;) {
        var ea = eA[hh];
        if (ea === -1) { eA[hh] = lo; eB[hh] = hi; eTri[hh] = t2; eState[hh] = 1; break; }
        if (ea === lo && eB[hh] === hi) {
          if (eState[hh] === 1) {
            var o = eTri[hh];
            var sharp;
            if (useAngle) {
              sharp = (tn[o * 3] * tn[t2 * 3] + tn[o * 3 + 1] * tn[t2 * 3 + 1] + tn[o * 3 + 2] * tn[t2 * 3 + 2]) < cosT;
            } else {
              sharp = faceId[o] !== faceId[t2];
            }
            if (sharp) emit(lo, hi);
            eState[hh] = 2;
          }
          break;
        }
        hh = (hh + 1) & eMask;
      }
    }
  }
  // boundary edges
  for (var s = 0; s < eSize; s++) if (eState[s] === 1) emit(eA[s], eB[s]);

  out.push(buf.slice(0, bufN));
  var total = 0; for (var j = 0; j < out.length; j++) total += out[j].length;
  var res = new Float32Array(total), off = 0;
  for (var j2 = 0; j2 < out.length; j2++) { res.set(out[j2], off); off += out[j2].length; }
  return res;
}

// CAD edges from B-rep face patches: an edge belongs to the drawing if it is used by exactly one
// triangle of its face (i.e. lies on the face boundary). Each face is welded on its own so
// periodic seams (cylinders) disappear. Works face by face with small tables, so it scales to
// any model size (memory ~ largest face, time ~ linear).
function computeFaceBoundaryEdges(position, index, ranges) {
  var nFaces = ranges.length / 2;
  var minX = Infinity, minY = Infinity, minZ = Infinity, maxX = -Infinity, maxY = -Infinity, maxZ = -Infinity;
  for (var i = 0; i < position.length; i += 3) {
    var x = position[i], y = position[i + 1], z = position[i + 2];
    if (x < minX) minX = x; if (x > maxX) maxX = x;
    if (y < minY) minY = y; if (y > maxY) maxY = y;
    if (z < minZ) minZ = z; if (z > maxZ) maxZ = z;
  }
  var diag = Math.sqrt((maxX - minX) ** 2 + (maxY - minY) ** 2 + (maxZ - minZ) ** 2) || 1;
  var inv = 1 / (diag * 1e-6);
  var chunks = [], buf = new Float32Array(1 << 18), bufN = 0;
  function emit(a, b) {
    if (bufN + 6 > buf.length) { chunks.push(buf.slice(0, bufN)); bufN = 0; }
    buf[bufN++] = position[a * 3]; buf[bufN++] = position[a * 3 + 1]; buf[bufN++] = position[a * 3 + 2];
    buf[bufN++] = position[b * 3]; buf[bufN++] = position[b * 3 + 1]; buf[bufN++] = position[b * 3 + 2];
  }
  // scratch tables, grown on demand and reused across faces
  var vT = new Int32Array(0), qx = new Int32Array(0), qy = new Int32Array(0), qz = new Int32Array(0), weld = new Int32Array(0), rep = new Int32Array(0);
  var eA = new Int32Array(0), eB = new Int32Array(0), eC = new Uint8Array(0);
  for (var f = 0; f < nFaces; f++) {
    var first = ranges[f * 2], last = ranges[f * 2 + 1];
    var nT = last - first + 1;
    if (nT <= 0) continue;
    var vmin = Infinity, vmax = -1;
    for (var t = first; t <= last; t++) for (var k = 0; k < 3; k++) { var vi = index[t * 3 + k]; if (vi < vmin) vmin = vi; if (vi > vmax) vmax = vi; }
    var nV = vmax - vmin + 1;
    var vSize = gvNextPow2(nV * 2), vMask = vSize - 1;
    if (vT.length < vSize) vT = new Int32Array(vSize);
    vT.fill(-1, 0, vSize);
    if (qx.length < nV) { qx = new Int32Array(nV); qy = new Int32Array(nV); qz = new Int32Array(nV); weld = new Int32Array(nV); rep = new Int32Array(nV); }
    var nU = 0;
    for (var v = 0; v < nV; v++) {
      var g = (vmin + v) * 3;
      var a = Math.round((position[g] - minX) * inv), b = Math.round((position[g + 1] - minY) * inv), c = Math.round((position[g + 2] - minZ) * inv);
      var h = (Math.imul(a, 73856093) ^ Math.imul(b, 19349663) ^ Math.imul(c, 83492791)) & vMask;
      for (;;) {
        var e = vT[h];
        if (e === -1) { vT[h] = nU; qx[nU] = a; qy[nU] = b; qz[nU] = c; rep[nU] = vmin + v; weld[v] = nU++; break; }
        if (qx[e] === a && qy[e] === b && qz[e] === c) { weld[v] = e; break; }
        h = (h + 1) & vMask;
      }
    }
    var eSize = gvNextPow2(nT * 3 * 2), eMask = eSize - 1;
    if (eA.length < eSize) { eA = new Int32Array(eSize); eB = new Int32Array(eSize); eC = new Uint8Array(eSize); }
    eA.fill(-1, 0, eSize);
    for (var t2 = first; t2 <= last; t2++) {
      for (var k2 = 0; k2 < 3; k2++) {
        var p = weld[index[t2 * 3 + k2] - vmin], q = weld[index[t2 * 3 + (k2 + 1) % 3] - vmin];
        if (p === q) continue;
        var lo = p < q ? p : q, hi = p < q ? q : p;
        var hh = (Math.imul(lo, 2654435761) ^ Math.imul(hi, 40503)) & eMask;
        for (;;) {
          if (eA[hh] === -1) { eA[hh] = lo; eB[hh] = hi; eC[hh] = 1; break; }
          if (eA[hh] === lo && eB[hh] === hi) { if (eC[hh] < 2) eC[hh]++; break; }
          hh = (hh + 1) & eMask;
        }
      }
    }
    for (var s = 0; s < eSize; s++) if (eA[s] !== -1 && eC[s] === 1) emit(rep[eA[s]], rep[eB[s]]);
  }
  chunks.push(buf.slice(0, bufN));
  var total = 0; for (var j = 0; j < chunks.length; j++) total += chunks[j].length;
  var res = new Float32Array(total), off = 0;
  for (var j2 = 0; j2 < chunks.length; j2++) { res.set(chunks[j2], off); off += chunks[j2].length; }
  return res;
}
