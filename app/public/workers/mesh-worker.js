// Mesh worker: fast STL parsing (binary + ASCII) and generic feature-edge extraction.
importScripts('edges.js');

function parseBinarySTL(buf) {
  var dv = new DataView(buf);
  var n = dv.getUint32(80, true);
  var pos = new Float32Array(n * 9);
  var nor = new Float32Array(n * 9);
  var off = 84;
  for (var t = 0; t < n; t++, off += 50) {
    var nx = dv.getFloat32(off, true), ny = dv.getFloat32(off + 4, true), nz = dv.getFloat32(off + 8, true);
    var o = t * 9;
    for (var k = 0; k < 9; k++) pos[o + k] = dv.getFloat32(off + 12 + k * 4, true);
    // Recompute the normal from the vertices: many exporters write zero or wrong normals.
    var ux = pos[o + 3] - pos[o], uy = pos[o + 4] - pos[o + 1], uz = pos[o + 5] - pos[o + 2];
    var wx = pos[o + 6] - pos[o], wy = pos[o + 7] - pos[o + 1], wz = pos[o + 8] - pos[o + 2];
    var cx = uy * wz - uz * wy, cy = uz * wx - ux * wz, cz = ux * wy - uy * wx;
    var l = Math.sqrt(cx * cx + cy * cy + cz * cz);
    if (l > 0) { nx = cx / l; ny = cy / l; nz = cz / l; }
    for (var j = 0; j < 3; j++) { nor[o + j * 3] = nx; nor[o + j * 3 + 1] = ny; nor[o + j * 3 + 2] = nz; }
  }
  return { position: pos, normal: nor };
}

function parseAsciiSTL(buf) {
  var text = new TextDecoder().decode(new Uint8Array(buf));
  var re = /vertex\s+([-+0-9.eE]+)\s+([-+0-9.eE]+)\s+([-+0-9.eE]+)/g;
  var arr = [];
  var m;
  while ((m = re.exec(text)) !== null) arr.push(+m[1], +m[2], +m[3]);
  var pos = new Float32Array(arr.length - (arr.length % 9));
  pos.set(arr.slice(0, pos.length));
  var nor = new Float32Array(pos.length);
  for (var o = 0; o < pos.length; o += 9) {
    var ux = pos[o + 3] - pos[o], uy = pos[o + 4] - pos[o + 1], uz = pos[o + 5] - pos[o + 2];
    var wx = pos[o + 6] - pos[o], wy = pos[o + 7] - pos[o + 1], wz = pos[o + 8] - pos[o + 2];
    var cx = uy * wz - uz * wy, cy = uz * wx - ux * wz, cz = ux * wy - uy * wx;
    var l = Math.sqrt(cx * cx + cy * cy + cz * cz) || 1;
    for (var j = 0; j < 3; j++) { nor[o + j * 3] = cx / l; nor[o + j * 3 + 1] = cy / l; nor[o + j * 3 + 2] = cz / l; }
  }
  return { position: pos, normal: nor };
}

function isBinarySTL(buf) {
  if (buf.byteLength < 84) return false;
  var n = new DataView(buf).getUint32(80, true);
  if (84 + n * 50 === buf.byteLength) return true;
  var head = new TextDecoder().decode(new Uint8Array(buf, 0, Math.min(512, buf.byteLength)));
  return !/^\s*solid[\s\S]*facet/.test(head);
}

onmessage = function (ev) {
  var d = ev.data;
  try {
    if (d.type === 'stl') {
      postMessage({ id: d.id, type: 'progress', text: 'Parsing STL…' });
      var g = isBinarySTL(d.buffer) ? parseBinarySTL(d.buffer) : parseAsciiSTL(d.buffer);
      var posCopy = g.position.length / 9 <= GV_EDGE_MAX_TRIS ? g.position.slice() : null;
      postMessage({
        id: d.id, type: 'mesh',
        data: { tree: { name: d.name, meshes: [0], children: [] }, meshes: [{ name: d.name, color: null, position: g.position, normal: g.normal, index: null, colors: null }], units: 'mm' }
      }, [g.position.buffer, g.normal.buffer]);
      var e = posCopy ? computeEdges(posCopy, null, null, d.angle || 30) : new Float32Array(0);
      postMessage({ id: d.id, type: 'edges', edges: [e] }, [e.buffer]);
    } else if (d.type === 'edges') {
      var out = [], tr = [];
      for (var i = 0; i < d.items.length; i++) {
        var it = d.items[i];
        var eg = computeEdges(it.position, it.index, null, d.angle || 30);
        out.push(eg); tr.push(eg.buffer);
      }
      postMessage({ id: d.id, type: 'edges', edges: out }, tr);
    }
  } catch (err) {
    postMessage({ id: d.id, type: 'error', message: (err && err.message) || String(err) });
  }
};
