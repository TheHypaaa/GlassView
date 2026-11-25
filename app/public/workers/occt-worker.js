// STEP / IGES / BREP import worker. OpenCASCADE compiled to WebAssembly (occt-import-js).
// The module is compiled once and kept warm; the app pre-warms it in idle time after startup.
importScripts('../occt/occt-import-js.js', 'edges.js', 'faces.js');

var occtPromise = null;
function getOcct() {
  if (!occtPromise) {
    occtPromise = fetch('../occt/occt-import-js.wasm')
      .then(function (r) { return r.arrayBuffer(); })
      .then(function (bin) {
        return occtimportjs({ wasmBinary: bin, locateFile: function (p) { return '../occt/' + p; } });
      });
  }
  return occtPromise;
}

function convertTree(node) {
  return {
    name: node.name || '',
    meshes: node.meshes ? node.meshes.slice() : [],
    children: (node.children || []).map(convertTree)
  };
}

onmessage = async function (ev) {
  var d = ev.data;
  if (d.type === 'warm') {
    try { await getOcct(); postMessage({ id: d.id, type: 'warm' }); }
    catch (e) { postMessage({ id: d.id, type: 'error', message: String(e) }); }
    return;
  }
  try {
    var t0 = performance.now();
    postMessage({ id: d.id, type: 'progress', text: 'Starting geometry kernel…' });
    var occt = await getOcct();
    postMessage({ id: d.id, type: 'progress', text: 'Reading & tessellating B-rep…' });
    var res = occt.ReadFile(d.format, new Uint8Array(d.buffer), d.params);
    if (!res || !res.success) throw new Error('OpenCASCADE could not read this file.');
    var t1 = performance.now();

    var meshes = [];
    var transfer = [];
    var faceIds = [];
    for (var m = 0; m < res.meshes.length; m++) {
      var src = res.meshes[m];
      var pos = new Float32Array(src.attributes.position.array);
      var nor = src.attributes.normal ? new Float32Array(src.attributes.normal.array) : null;
      var idx = new Uint32Array(src.index.array);
      var nTris = idx.length / 3;
      // per-triangle B-rep face ids + optional per-face colours
      var fid = new Int32Array(nTris);
      var col = null;
      var faces = src.brep_faces || [];
      var anyFaceColor = false;
      for (var f = 0; f < faces.length; f++) if (faces[f].color) { anyFaceColor = true; break; }
      if (anyFaceColor) {
        col = new Uint8Array(pos.length);
        var base = src.color || [0.75, 0.78, 0.82];
        for (var v = 0; v < pos.length; v += 3) { col[v] = base[0] * 255; col[v + 1] = base[1] * 255; col[v + 2] = base[2] * 255; }
      }
      for (var f2 = 0; f2 < faces.length; f2++) {
        var bf = faces[f2];
        for (var t = bf.first; t <= bf.last; t++) {
          fid[t] = f2;
          if (col && bf.color) {
            for (var k = 0; k < 3; k++) {
              var vi = idx[t * 3 + k] * 3;
              col[vi] = bf.color[0] * 255; col[vi + 1] = bf.color[1] * 255; col[vi + 2] = bf.color[2] * 255;
            }
          }
        }
      }
      faceIds.push(fid);
      meshes.push({ name: src.name || '', color: src.color || null, position: pos, normal: nor, index: idx, colors: col });
      transfer.push(pos.buffer, idx.buffer);
      if (nor) transfer.push(nor.buffer);
      if (col) transfer.push(col.buffer);
    }
    // Keep copies needed for edge extraction before transferring the originals away.
    var posCopies = meshes.map(function (mm) { return mm.position.slice(); });
    var idxCopies = meshes.map(function (mm) { return mm.index.slice(); });
    var norCopies = meshes.map(function (mm) { return mm.normal ? mm.normal.slice() : null; });
    var rangeList = res.meshes.map(function (src) {
      var bf = src.brep_faces || [], r = new Int32Array(bf.length * 2);
      for (var q = 0; q < bf.length; q++) { r[q * 2] = bf[q].first; r[q * 2 + 1] = bf[q].last; }
      return r;
    });

    postMessage({
      id: d.id, type: 'mesh',
      data: { tree: convertTree(res.root), meshes: meshes, units: 'mm' },
      timing: { kernel: t1 - t0 }
    }, transfer);

    // Second pass: CAD face-boundary edges, streamed after the shaded model is already on screen.
    // ... and the exact analytic surfaces (planes, cylinders, cones, spheres) of every B-rep face.
    var edges = [], faces = [];
    var et = [];
    for (var e = 0; e < posCopies.length; e++) {
      var eg = computeFaceBoundaryEdges(posCopies[e], idxCopies[e], rangeList[e]);
      edges.push(eg); et.push(eg.buffer);
      faces.push({ ranges: rangeList[e], prims: fitFaces(posCopies[e], norCopies[e], idxCopies[e], rangeList[e]) });
      et.push(rangeList[e].buffer);
    }
    postMessage({ id: d.id, type: 'edges', edges: edges, faces: faces }, et);
  } catch (err) {
    postMessage({ id: d.id, type: 'error', message: (err && err.message) || String(err) });
  }
};
