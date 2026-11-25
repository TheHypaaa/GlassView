// 3MF export (3D printing): one welded, watertight mesh object per body, with colours and the
// positions the parts have right now (moves, exploded view). An optional extra matrix rotates
// the build direction onto +Z; `drop` sits the result on z = 0 and centres it on the plate.
import * as THREE from 'three';

let crcTable = null;
function crc32(u8) {
  if (!crcTable) {
    crcTable = new Uint32Array(256);
    for (let i = 0; i < 256; i++) { let c = i; for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1; crcTable[i] = c >>> 0; }
  }
  let c = 0xffffffff;
  for (let i = 0; i < u8.length; i++) c = crcTable[(c ^ u8[i]) & 255] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}

/** Minimal ZIP writer (stored entries) → Uint8Array. */
function zip(files) {
  const enc = new TextEncoder();
  const parts = [], central = [];
  let offset = 0;
  for (const f of files) {
    const name = enc.encode(f.name);
    const data = typeof f.data === 'string' ? enc.encode(f.data) : f.data;
    const crc = crc32(data);
    const h = new DataView(new ArrayBuffer(30));
    h.setUint32(0, 0x04034b50, true); h.setUint16(4, 20, true); h.setUint16(10, 0, true); h.setUint16(12, 0x21, true);
    h.setUint32(14, crc, true); h.setUint32(18, data.length, true); h.setUint32(22, data.length, true); h.setUint16(26, name.length, true);
    parts.push(new Uint8Array(h.buffer), name, data);
    const c = new DataView(new ArrayBuffer(46));
    c.setUint32(0, 0x02014b50, true); c.setUint16(4, 20, true); c.setUint16(6, 20, true); c.setUint16(14, 0x21, true);
    c.setUint32(16, crc, true); c.setUint32(20, data.length, true); c.setUint32(24, data.length, true); c.setUint16(28, name.length, true);
    c.setUint32(42, offset, true);
    central.push(new Uint8Array(c.buffer), name);
    offset += 30 + name.length + data.length;
  }
  const cSize = central.reduce((s, a) => s + a.length, 0);
  const e = new DataView(new ArrayBuffer(22));
  e.setUint32(0, 0x06054b50, true); e.setUint16(8, files.length, true); e.setUint16(10, files.length, true);
  e.setUint32(12, cSize, true); e.setUint32(16, offset, true);
  const all = [...parts, ...central, new Uint8Array(e.buffer)];
  const out = new Uint8Array(all.reduce((s, a) => s + a.length, 0));
  let o = 0;
  for (const a of all) { out.set(a, o); o += a.length; }
  return out;
}

const esc = (s) => String(s).replace(/[&<>"']/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&apos;' }[c]));
const hex = (c) => '#' + c.getHexString().toUpperCase() + 'FF';

/**
 * meshes: THREE.Mesh list; colorOf(mesh) → THREE.Color (part colour as shown).
 * Returns the .3mf file as Uint8Array.
 */
export function build3mf(meshes, { colorOf, extra = null, drop = false, title = 'GlassView export' } = {}) {
  const mats = [], matIdx = new Map();
  const mat = (c) => { const h = hex(c); if (!matIdx.has(h)) { matIdx.set(h, mats.length); mats.push(h); } return matIdx.get(h); };
  const objs = [];
  const box = new THREE.Box3();
  const v = new THREE.Vector3(), col = new THREE.Color();
  for (const m of meshes) {
    const g = m.geometry, P = g.attributes.position, I = g.index ? g.index.array : null, C = g.attributes.color;
    const M = extra ? extra.clone().multiply(m.matrixWorld) : m.matrixWorld;
    const flip = M.determinant() < 0;
    // weld coincident vertices (faces of a B-rep are meshed separately)
    const tmpBox = new THREE.Box3();
    for (let i = 0; i < P.count; i++) tmpBox.expandByPoint(v.fromBufferAttribute(P, i).applyMatrix4(M));
    const ext = tmpBox.getSize(new THREE.Vector3());
    const tol = Math.max(1e-7, 1e-7 * Math.max(ext.x, ext.y, ext.z));
    const weld = new Map(), map = new Int32Array(P.count), verts = [];
    for (let i = 0; i < P.count; i++) {
      v.fromBufferAttribute(P, i).applyMatrix4(M);
      const key = `${Math.round(v.x / tol)},${Math.round(v.y / tol)},${Math.round(v.z / tol)}`;
      let id = weld.get(key);
      if (id === undefined) { id = verts.length / 3; weld.set(key, id); verts.push(v.x, v.y, v.z); box.expandByPoint(v); }
      map[i] = id;
    }
    const base = mat(colorOf(m));
    const useVc = C && m.material && m.material.vertexColors;
    const tris = [];
    const nT = I ? I.length / 3 : P.count / 3;
    for (let t = 0; t < nT; t++) {
      const i0 = I ? I[t * 3] : t * 3, i1 = I ? I[t * 3 + 1] : t * 3 + 1, i2 = I ? I[t * 3 + 2] : t * 3 + 2;
      let a = map[i0], b = map[i1], c = map[i2];
      if (a === b || b === c || a === c) continue;
      if (flip) [b, c] = [c, b];
      let mi = base;
      if (useVc) { col.fromBufferAttribute(C, i0); mi = mat(col); }
      tris.push(a, b, c, mi);
    }
    if (tris.length) objs.push({ name: m.name || m.parent?.name || 'Body', verts, tris, base });
  }
  if (!objs.length) throw new Error('nothing to export');
  let sx = 0, sy = 0, sz = 0;
  if (drop) { const c = box.getCenter(new THREE.Vector3()); sx = -c.x; sy = -c.y; sz = -box.min.z; }
  const out = [];
  out.push('<?xml version="1.0" encoding="UTF-8"?>\n<model unit="millimeter" xml:lang="en-US" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02">\n');
  out.push(` <metadata name="Application">GlassView</metadata>\n <metadata name="Title">${esc(title)}</metadata>\n <resources>\n  <basematerials id="1">\n`);
  mats.forEach((h, i) => out.push(`   <base name="Colour ${i + 1}" displaycolor="${h}"/>\n`));
  out.push('  </basematerials>\n');
  const f = (x) => +x.toPrecision(7);
  objs.forEach((o, k) => {
    out.push(`  <object id="${k + 2}" type="model" name="${esc(o.name)}" pid="1" pindex="${o.base}">\n   <mesh>\n    <vertices>\n`);
    const vs = [];
    for (let i = 0; i < o.verts.length; i += 3) vs.push(`     <vertex x="${f(o.verts[i] + sx)}" y="${f(o.verts[i + 1] + sy)}" z="${f(o.verts[i + 2] + sz)}"/>\n`);
    out.push(vs.join(''), '    </vertices>\n    <triangles>\n');
    const ts = [];
    for (let i = 0; i < o.tris.length; i += 4) {
      const m = o.tris[i + 3];
      ts.push(m === o.base ? `     <triangle v1="${o.tris[i]}" v2="${o.tris[i + 1]}" v3="${o.tris[i + 2]}"/>\n`
                           : `     <triangle v1="${o.tris[i]}" v2="${o.tris[i + 1]}" v3="${o.tris[i + 2]}" pid="1" p1="${m}"/>\n`);
    }
    out.push(ts.join(''), '    </triangles>\n   </mesh>\n  </object>\n');
  });
  out.push(' </resources>\n <build>\n');
  objs.forEach((o, k) => out.push(`  <item objectid="${k + 2}"/>\n`));
  out.push(' </build>\n</model>\n');
  const types = '<?xml version="1.0" encoding="UTF-8"?>\n<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">\n <Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>\n <Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/>\n</Types>\n';
  const rels = '<?xml version="1.0" encoding="UTF-8"?>\n<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">\n <Relationship Target="/3D/3dmodel.model" Id="rel0" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/>\n</Relationships>\n';
  return zip([{ name: '[Content_Types].xml', data: types }, { name: '_rels/.rels', data: rels }, { name: '3D/3dmodel.model', data: out.join('') }]);
}

/** Matrix that turns `dir` (the build direction, unit vector) onto +Z. */
export function buildMatrix(dir) {
  const q = new THREE.Quaternion().setFromUnitVectors(dir.clone().normalize(), new THREE.Vector3(0, 0, 1));
  return new THREE.Matrix4().makeRotationFromQuaternion(q);
}
