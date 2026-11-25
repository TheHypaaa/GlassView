// Recover the exact analytic surface of each B-rep face from its tessellation.
// OpenCASCADE places every mesh vertex exactly on the true surface (sampled in UV), and gives the
// true surface normal there, so fitting planes / cylinders / cones / spheres to those samples
// reproduces the CAD geometry to float precision. Freeform faces stay 'other'.
//
// fitFaces(position, normal, index, ranges) -> array of primitives (mesh-local coordinates):
//   { t:'plane',    n:[3], p:[3] }
//   { t:'cylinder', a:[3], o:[3], r, t0, t1 }          axis o + a*s, s in [t0,t1]
//   { t:'cone',     a:[3], o:[3], t0, t1, r0, r1, ang } radius r0 at t0, r1 at t1, half angle (deg)
//   { t:'sphere',   c:[3], r }
//   { t:'other' }

function gvJacobiEigen(A) {
  // symmetric 3x3 -> {vals:[3], vecs:[[3],[3],[3]]} (vecs[i] is eigenvector i)
  var a = [A[0].slice(), A[1].slice(), A[2].slice()];
  var v = [[1, 0, 0], [0, 1, 0], [0, 0, 1]];
  for (var sweep = 0; sweep < 50; sweep++) {
    var off = Math.abs(a[0][1]) + Math.abs(a[0][2]) + Math.abs(a[1][2]);
    if (off < 1e-15) break;
    for (var p = 0; p < 2; p++) for (var q = p + 1; q < 3; q++) {
      if (Math.abs(a[p][q]) < 1e-18) continue;
      var th = (a[q][q] - a[p][p]) / (2 * a[p][q]);
      var t = (th >= 0 ? 1 : -1) / (Math.abs(th) + Math.sqrt(th * th + 1));
      var c = 1 / Math.sqrt(t * t + 1), s = t * c;
      for (var k = 0; k < 3; k++) {
        var akp = a[k][p], akq = a[k][q];
        a[k][p] = c * akp - s * akq; a[k][q] = s * akp + c * akq;
      }
      for (var k2 = 0; k2 < 3; k2++) {
        var apk = a[p][k2], aqk = a[q][k2];
        a[p][k2] = c * apk - s * aqk; a[q][k2] = s * apk + c * aqk;
      }
      for (var k3 = 0; k3 < 3; k3++) {
        var vkp = v[k3][p], vkq = v[k3][q];
        v[k3][p] = c * vkp - s * vkq; v[k3][q] = s * vkp + c * vkq;
      }
    }
  }
  var vals = [a[0][0], a[1][1], a[2][2]];
  var vecs = [[v[0][0], v[1][0], v[2][0]], [v[0][1], v[1][1], v[2][1]], [v[0][2], v[1][2], v[2][2]]];
  var order = [0, 1, 2].sort(function (i, j) { return vals[i] - vals[j]; });
  return { vals: order.map(function (i) { return vals[i]; }), vecs: order.map(function (i) { return vecs[i]; }) };
}

function gvDot(a, b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
function gvCross(a, b) { return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]; }
function gvNorm(a) { var l = Math.sqrt(gvDot(a, a)) || 1; return [a[0] / l, a[1] / l, a[2] / l]; }

/** Least-squares intersection of lines (p_i + s d_i) in 3D: solve Σ(I - d dᵀ) c = Σ(I - d dᵀ) p. */
function gvLinesPoint(P, D) {
  var M = [[0, 0, 0], [0, 0, 0], [0, 0, 0]], b = [0, 0, 0];
  for (var i = 0; i < P.length; i++) {
    var d = D[i], p = P[i];
    for (var r = 0; r < 3; r++) for (var c = 0; c < 3; c++) {
      var m = (r === c ? 1 : 0) - d[r] * d[c];
      M[r][c] += m; b[r] += m * p[c];
    }
  }
  // solve 3x3 (Cramer); may be singular for cylinders (axis direction free) -> caller handles
  var det = M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) + M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
  if (Math.abs(det) < 1e-12) return null;
  var inv = function (r, c) {
    var m = [[0, 0], [0, 0]], ri = 0;
    for (var i = 0; i < 3; i++) { if (i === c) continue; var ci = 0; for (var j = 0; j < 3; j++) { if (j === r) continue; m[ri][ci++] = M[i][j]; } ri++; }
    return ((r + c) % 2 ? -1 : 1) * (m[0][0] * m[1][1] - m[0][1] * m[1][0]) / det;
  };
  var x = [0, 0, 0];
  for (var r2 = 0; r2 < 3; r2++) for (var c2 = 0; c2 < 3; c2++) x[r2] += inv(r2, c2) * b[c2];
  return x;
}

/** Solve a small dense linear system (Gaussian elimination with partial pivoting). */
function gvSolve(A, b) {
  var n = b.length, M = A.map(function (r, i) { return r.concat([b[i]]); });
  for (var c = 0; c < n; c++) {
    var piv = c;
    for (var r = c + 1; r < n; r++) if (Math.abs(M[r][c]) > Math.abs(M[piv][c])) piv = r;
    if (Math.abs(M[piv][c]) < 1e-14) return null;
    var tmp = M[c]; M[c] = M[piv]; M[piv] = tmp;
    for (var r2 = c + 1; r2 < n; r2++) {
      var f = M[r2][c] / M[c][c];
      for (var k = c; k <= n; k++) M[r2][k] -= f * M[c][k];
    }
  }
  var x = new Array(n);
  for (var r3 = n - 1; r3 >= 0; r3--) {
    var sum = M[r3][n];
    for (var k2 = r3 + 1; k2 < n; k2++) sum -= M[r3][k2] * x[k2];
    x[r3] = sum / M[r3][r3];
  }
  return x;
}

/** Least squares for rows [features..] = target. */
function gvLstsq(rows, target) {
  var m = rows[0].length;
  var A = [], b = [];
  for (var i = 0; i < m; i++) { A.push(new Array(m).fill(0)); b.push(0); }
  for (var r = 0; r < rows.length; r++) {
    var row = rows[r];
    for (var i2 = 0; i2 < m; i2++) {
      b[i2] += row[i2] * target[r];
      for (var j = 0; j < m; j++) A[i2][j] += row[i2] * row[j];
    }
  }
  return gvSolve(A, b);
}

/** Algebraic circle fit (Kåsa) on 2D points (already roughly centred). */
function gvCircle(pts) {
  if (pts.length < 3) return null;
  var rows = [], tgt = [];
  for (var i = 0; i < pts.length; i++) { rows.push([pts[i][0], pts[i][1], 1]); tgt.push(-(pts[i][0] * pts[i][0] + pts[i][1] * pts[i][1])); }
  var x = gvLstsq(rows, tgt);
  if (!x) return null;
  var cx = -x[0] / 2, cy = -x[1] / 2, r2 = cx * cx + cy * cy - x[2];
  if (!(r2 > 0)) return null;
  return { cx: cx, cy: cy, r: Math.sqrt(r2) };
}

function fitFace(P, N, scale) {
  var n = P.length;
  if (n < 3) return { t: 'other' };
  var tol = Math.max(scale * 2e-5, 2e-5) * 5;
  var c = [0, 0, 0], mn = [0, 0, 0];
  for (var i = 0; i < n; i++) for (var k = 0; k < 3; k++) { c[k] += P[i][k] / n; mn[k] += N[i][k] / n; }

  // ---- plane: points are coplanar (positions are exact; normals only orient it)
  var Cp = [[0, 0, 0], [0, 0, 0], [0, 0, 0]];
  for (var i1 = 0; i1 < n; i1++) {
    var d1 = [P[i1][0] - c[0], P[i1][1] - c[1], P[i1][2] - c[2]];
    for (var r = 0; r < 3; r++) for (var q = 0; q < 3; q++) Cp[r][q] += d1[r] * d1[q];
  }
  var ep = gvJacobiEigen(Cp);
  var pn = gvNorm(ep.vecs[0]);
  var maxRes = 0;
  for (var i2 = 0; i2 < n; i2++) maxRes = Math.max(maxRes, Math.abs(gvDot(pn, [P[i2][0] - c[0], P[i2][1] - c[1], P[i2][2] - c[2]])));
  if (maxRes < tol && ep.vals[1] > 0) {
    if (gvDot(pn, mn) < 0) pn = [-pn[0], -pn[1], -pn[2]];
    return { t: 'plane', n: pn, p: c };
  }

  // ---- cylinder / cone: normal vectors' tips lie on a plane whose normal is the axis
  var C = [[0, 0, 0], [0, 0, 0], [0, 0, 0]];
  for (var i5 = 0; i5 < n; i5++) {
    var dn = [N[i5][0] - mn[0], N[i5][1] - mn[1], N[i5][2] - mn[2]];
    for (var r5 = 0; r5 < 3; r5++) for (var q5 = 0; q5 < 3; q5++) C[r5][q5] += dn[r5] * dn[q5];
  }
  var eg = gvJacobiEigen(C);
  var spread = eg.vals[1] + eg.vals[2];
  if (spread > 1e-9 && eg.vals[0] < spread * 2e-3) {
    var a = gvNorm(eg.vecs[0]);
    var u = gvNorm(Math.abs(a[0]) < 0.9 ? gvCross(a, [1, 0, 0]) : gvCross(a, [0, 1, 0]));
    var v = gvCross(a, u);
    var cu = gvDot(c, u), cv = gvDot(c, v);
    var Q = new Array(n), T = new Array(n);
    for (var i6 = 0; i6 < n; i6++) { Q[i6] = [gvDot(P[i6], u) - cu, gvDot(P[i6], v) - cv]; T[i6] = gvDot(P[i6], a); }
    // group points by height along the axis; fit one circle per level
    var order = T.map(function (_, ii) { return ii; }).sort(function (x, y) { return T[x] - T[y]; });
    var levels = [], cur = [order[0]];
    for (var o = 1; o < n; o++) {
      if (T[order[o]] - T[cur[0]] < tol) cur.push(order[o]);
      else { levels.push(cur); cur = [order[o]]; }
    }
    levels.push(cur);
    var circles = [], covered = 0;
    for (var L = 0; L < levels.length; L++) {
      if (levels[L].length < 3) continue;
      var circ = gvCircle(levels[L].map(function (ii) { return Q[ii]; }));
      if (!circ) continue;
      var tl = 0; levels[L].forEach(function (ii) { tl += T[ii] / levels[L].length; });
      circ.t = tl; circ.n = levels[L].length;
      circles.push(circ); covered += levels[L].length;
    }
    var cx, cy, rOf;
    if (circles.length && covered >= n * 0.8) {
      cx = 0; cy = 0; var w = 0;
      circles.forEach(function (ci) { cx += ci.cx * ci.n; cy += ci.cy * ci.n; w += ci.n; });
      cx /= w; cy /= w;
      if (circles.length === 1) { var r1 = circles[0].r; rOf = function () { return r1; }; }
      else {
        var st = 0, sr = 0, stt = 0, str = 0, m = circles.length;
        circles.forEach(function (ci) { st += ci.t; sr += ci.r; stt += ci.t * ci.t; str += ci.t * ci.r; });
        var den = m * stt - st * st;
        var k1 = Math.abs(den) > 1e-12 ? (m * str - st * sr) / den : 0, k0 = (sr - k1 * st) / m;
        rOf = function (t) { return k0 + k1 * t; };
      }
    } else {
      // scattered samples: x²+y² + D x + E y + F + G t + H t² = 0  (exact for cylinders and cones)
      var rows = [], tgt = [];
      for (var i7 = 0; i7 < n; i7++) { rows.push([Q[i7][0], Q[i7][1], 1, T[i7], T[i7] * T[i7]]); tgt.push(-(Q[i7][0] * Q[i7][0] + Q[i7][1] * Q[i7][1])); }
      var x = gvLstsq(rows, tgt);
      if (!x) return { t: 'other' };
      cx = -x[0] / 2; cy = -x[1] / 2;
      var F = x[2], G = x[3], H = x[4];
      rOf = function (t) { return Math.sqrt(Math.max(0, cx * cx + cy * cy - F - G * t - H * t * t)); };
    }
    // validate against every sample
    var dev = 0, t0 = Infinity, t1 = -Infinity;
    for (var i8 = 0; i8 < n; i8++) {
      var rr = Math.sqrt(Math.pow(Q[i8][0] - cx, 2) + Math.pow(Q[i8][1] - cy, 2));
      dev = Math.max(dev, Math.abs(rr - rOf(T[i8])));
      t0 = Math.min(t0, T[i8]); t1 = Math.max(t1, T[i8]);
    }
    if (dev < tol) {
      var ox = cu + cx, oy = cv + cy;
      var origin = [u[0] * ox + v[0] * oy, u[1] * ox + v[1] * oy, u[2] * ox + v[2] * oy];
      var ra = rOf(t0), rb = rOf(t1);
      if (Math.abs(ra - rb) < tol) return { t: 'cylinder', a: a, o: origin, r: (ra + rb) / 2, t0: t0, t1: t1 };
      if (t1 - t0 > tol) {
        return { t: 'cone', a: a, o: origin, t0: t0, t1: t1, r0: ra, r1: rb, ang: Math.atan(Math.abs(rb - ra) / (t1 - t0)) * 180 / Math.PI };
      }
    }
  }

  // ---- sphere: algebraic fit on exact points, normals must point (roughly) radially
  var rowsS = [], tgtS = [];
  for (var i9 = 0; i9 < n; i9++) {
    var px = P[i9][0] - c[0], py = P[i9][1] - c[1], pz = P[i9][2] - c[2];
    rowsS.push([px, py, pz, 1]); tgtS.push(-(px * px + py * py + pz * pz));
  }
  var xs = gvLstsq(rowsS, tgtS);
  if (xs) {
    var sc = [-xs[0] / 2, -xs[1] / 2, -xs[2] / 2];
    var rs2 = sc[0] * sc[0] + sc[1] * sc[1] + sc[2] * sc[2] - xs[3];
    if (rs2 > 0) {
      var rs = Math.sqrt(rs2), devS = 0, radial = 0;
      for (var j = 0; j < n; j++) {
        var dvec = [P[j][0] - c[0] - sc[0], P[j][1] - c[1] - sc[1], P[j][2] - c[2] - sc[2]];
        var dl = Math.sqrt(gvDot(dvec, dvec));
        devS = Math.max(devS, Math.abs(dl - rs));
        radial += Math.abs(gvDot(N[j], [dvec[0] / dl, dvec[1] / dl, dvec[2] / dl])) / n;
      }
      if (devS < tol && radial > 0.97 && rs < scale * 20) return { t: 'sphere', c: [sc[0] + c[0], sc[1] + c[1], sc[2] + c[2]], r: rs };
    }
  }
  return { t: 'other' };
}

function fitFaces(position, normal, index, ranges) {
  var out = [];
  if (!normal) { for (var f0 = 0; f0 < ranges.length / 2; f0++) out.push({ t: 'other' }); return out; }
  // overall scale for tolerances
  var mn = [Infinity, Infinity, Infinity], mx = [-Infinity, -Infinity, -Infinity];
  for (var i = 0; i < position.length; i += 3) for (var k = 0; k < 3; k++) { mn[k] = Math.min(mn[k], position[i + k]); mx[k] = Math.max(mx[k], position[i + k]); }
  var scale = Math.max(mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2], 1e-6);
  for (var f = 0; f < ranges.length / 2; f++) {
    var first = ranges[f * 2], last = ranges[f * 2 + 1];
    var vmin = Infinity, vmax = -1;
    for (var t = first; t <= last; t++) for (var k2 = 0; k2 < 3; k2++) { var vi = index[t * 3 + k2]; if (vi < vmin) vmin = vi; if (vi > vmax) vmax = vi; }
    var P = [], N = [];
    for (var v = vmin; v <= vmax; v++) {
      P.push([position[v * 3], position[v * 3 + 1], position[v * 3 + 2]]);
      N.push(gvNorm([normal[v * 3], normal[v * 3 + 1], normal[v * 3 + 2]]));
    }
    var prim;
    try { prim = fitFace(P, N, scale); } catch (e) { prim = { t: 'other' }; }
    out.push(prim);
  }
  return out;
}
