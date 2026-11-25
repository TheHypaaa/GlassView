// PMI from STEP AP242: dimensions, geometric tolerances and datums, read by gvkernel.
// Annotation lines are drawn in 3D (on top), labels as HTML tags; hovering highlights the faces.
import * as THREE from 'three';
import { kernelCall } from './kernel.js';

const KCOL = ['#0a84ff', '#e66e14', '#28a05a'];
const KNAME = ['Dimensions', 'Tolerances', 'Datums'];

export class PmiTool {
  constructor(viewer, labelsEl) {
    this.viewer = viewer;
    this.labelsEl = labelsEl;
    this.active = false;
    this.items = [];          // { kind, title, detail, pt, segs, refs, comp }
    this.filter = [true, true, true];
    this.group = new THREE.Group();
    this.group.userData.isOverlay = true;
    this.group.renderOrder = 999;
    this.hi = [];
    this.pinned = -1;
    this.hover = -1;
    this.tags = [];
    this.onChange = null;
    viewer.renderHooks.push((cam) => this._place(cam));
  }

  /** comps: [{ comp, path, meshes: Map(index → mesh) }] of the active document. */
  async load(comps) {
    this.clear();
    const all = [];
    for (const c of comps) {
      const res = await kernelCall({ cmd: 'pmi', path: c.path });
      for (const it of res.items || []) all.push({ ...it, comp: c });
    }
    this.items = all;
    this._build();
    return all.length;
  }

  _build() {
    this.group.clear();
    this.viewer.scene.add(this.group);
    for (const t of this.tags) t.remove();
    this.tags = [];
    this.items.forEach((it, i) => {
      if (!this.filter[it.kind]) return;
      const M = it.comp.comp.matrixWorld;
      if (it.segs && it.segs.length) {
        const g = new THREE.BufferGeometry();
        g.setAttribute('position', new THREE.Float32BufferAttribute(it.segs, 3));
        const l = new THREE.LineSegments(g, new THREE.LineBasicMaterial({ color: KCOL[it.kind], depthTest: false, transparent: true, opacity: 0.95 }));
        l.matrixAutoUpdate = false;
        l.matrix.copy(M);
        l.renderOrder = 999;
        l.raycast = () => {};
        this.group.add(l);
      }
      // label anchor: text point, else the middle of the annotation, else of the faces
      let at = null;
      if (it.pt) at = new THREE.Vector3(...it.pt).applyMatrix4(M);
      else if (it.segs && it.segs.length) {
        const b = new THREE.Box3();
        for (let k = 0; k < it.segs.length; k += 3) b.expandByPoint(new THREE.Vector3(it.segs[k], it.segs[k + 1], it.segs[k + 2]));
        at = b.getCenter(new THREE.Vector3()).applyMatrix4(M);
      } else {
        const b = new THREE.Box3();
        for (const m of this._faceMeshes(it)) b.union(new THREE.Box3().setFromObject(m));
        if (!b.isEmpty()) at = b.getCenter(new THREE.Vector3());
        this._clearHi();
      }
      if (!at) return;
      const tag = document.createElement('div');
      tag.className = `pmi-tag k${it.kind}`;
      tag.style.setProperty('--k', KCOL[it.kind]);
      tag.textContent = it.title;
      tag.title = `${it.title}\n${it.detail}`;
      tag._at = at;
      tag._i = i;
      tag.addEventListener('mouseenter', () => this.setHover(i));
      tag.addEventListener('mouseleave', () => this.setHover(-1));
      tag.addEventListener('click', () => this.pin(this.pinned === i ? -1 : i));
      this.labelsEl.appendChild(tag);
      this.tags.push(tag);
    });
    this.viewer.requestRender();
  }

  _place(cam) {
    if (!this.active || !this.tags.length) return;
    const r = this.viewer.canvas.getBoundingClientRect();
    const v = new THREE.Vector3();
    for (const t of this.tags) {
      v.copy(t._at).project(cam);
      const vis = v.z > -1 && v.z < 1;
      t.style.display = vis ? '' : 'none';
      t.style.left = `${r.left + (v.x + 1) / 2 * r.width}px`;
      t.style.top = `${r.top + (1 - v.y) / 2 * r.height}px`;
    }
  }

  /** Temporary face highlight meshes for an item (children of the model meshes, so they follow moves). */
  _faceMeshes(it) {
    const out = [];
    const byMesh = new Map();
    for (const [mi, fi] of it.refs || []) {
      if (!byMesh.has(mi)) byMesh.set(mi, []);
      byMesh.get(mi).push(fi);
    }
    for (const [mi, faces] of byMesh) {
      const mesh = it.comp.meshes.get(mi);
      if (!mesh || !mesh.userData.faces) continue;
      const R = mesh.userData.faces.ranges, I = mesh.geometry.index;
      const idx = [];
      for (const f of faces) {
        const t0 = R[f * 2], t1 = R[f * 2 + 1];
        for (let t = t0; t <= t1; t++) idx.push(I.getX(t * 3), I.getX(t * 3 + 1), I.getX(t * 3 + 2));
      }
      if (!idx.length) continue;
      const g = new THREE.BufferGeometry();
      g.setAttribute('position', mesh.geometry.attributes.position);
      if (mesh.geometry.attributes.normal) g.setAttribute('normal', mesh.geometry.attributes.normal);
      g.setIndex(idx);
      const h = new THREE.Mesh(g, new THREE.MeshBasicMaterial({ color: KCOL[it.kind], transparent: true, opacity: 0.45, depthWrite: false,
        polygonOffset: true, polygonOffsetFactor: -2, polygonOffsetUnits: -2, side: THREE.DoubleSide }));
      h.userData.isOverlay = true;
      h.raycast = () => {};
      mesh.add(h);
      out.push(h);
      this.hi.push(h);
    }
    return out;
  }

  _clearHi() {
    for (const h of this.hi) { h.parent?.remove(h); h.geometry.setIndex(null); h.material.dispose(); }
    this.hi = [];
  }

  _showHi() {
    this._clearHi();
    const i = this.hover >= 0 ? this.hover : this.pinned;
    if (i >= 0 && this.items[i]) this._faceMeshes(this.items[i]);
    for (const t of this.tags) t.classList.toggle('hot', t._i === this.hover || t._i === this.pinned);
    this.viewer.requestRender();
    this.onChange?.();
  }

  setHover(i) { if (i !== this.hover) { this.hover = i; this._showHi(); } }
  pin(i) { this.pinned = i; this._showHi(); }
  setFilter(k, on) { this.filter[k] = on; this._build(); this._showHi(); }

  setActive(on) {
    this.active = on;
    if (!on) this.clear();
    this.group.visible = on;
    this.viewer.requestRender();
  }

  clear() {
    this._clearHi();
    this.group.clear();
    this.viewer.scene.remove(this.group);
    for (const t of this.tags) t.remove();
    this.tags = [];
    this.items = [];
    this.pinned = this.hover = -1;
  }

  /** Panel contents. */
  renderList(listEl, chipsEl) {
    chipsEl.innerHTML = '';
    listEl.innerHTML = '';
    const cnt = [0, 0, 0];
    for (const it of this.items) cnt[it.kind]++;
    for (let k = 0; k < 3; k++) {
      if (!cnt[k]) continue;
      const c = document.createElement('button');
      c.className = 'chip-t' + (this.filter[k] ? ' on' : '');
      c.innerHTML = `<i style="background:${KCOL[k]}"></i>${KNAME[k]} ${cnt[k]}`;
      c.addEventListener('click', () => { this.setFilter(k, !this.filter[k]); this.renderList(listEl, chipsEl); });
      chipsEl.appendChild(c);
    }
    for (let k = 0; k < 3; k++) {
      if (!cnt[k] || !this.filter[k]) continue;
      const g = document.createElement('div');
      g.className = 'grp';
      g.textContent = KNAME[k];
      listEl.appendChild(g);
      this.items.forEach((it, i) => {
        if (it.kind !== k) return;
        const row = document.createElement('div');
        row.className = 'pmi-row' + (this.pinned === i ? ' pinned' : '');
        row.style.setProperty('--k', KCOL[k]);
        const n = (it.refs || []).length;
        row.innerHTML = `<b></b><span></span>`;
        row.querySelector('b').textContent = it.title;
        row.querySelector('span').textContent = it.detail + (n ? ` · ${n} face${n === 1 ? '' : 's'}` : '');
        row.addEventListener('mouseenter', () => this.setHover(i));
        row.addEventListener('mouseleave', () => this.setHover(-1));
        row.addEventListener('click', () => { this.pin(this.pinned === i ? -1 : i); this.renderList(listEl, chipsEl); });
        listEl.appendChild(row);
      });
    }
  }
}
