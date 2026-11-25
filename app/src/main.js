import './style.css';
import * as THREE from 'three';
import * as NL from '@neutralinojs/lib';
import { Viewer, isModelMesh } from './viewer.js';
import { ViewCube } from './viewcube.js';
import { isNative, ALL_EXT, formatOf, isAssemblyFile, loadSource, sourceFromFile, sourceFromPath, prewarmKernel, clearCache, cachePath, setHealMode } from './loaders.js';
import { makeGlass, setGlassMode, refreshGlass } from './glass.js';
import { BACKGROUNDS } from './backgrounds.js';
import { SectionTool } from './section.js';
import { MeasureTool, labelOf, hasCond } from './measure.js';
import { Keymap, renderKeyGrid, openKeySheet, prettyCombo } from './keys.js';
import { units, getUnits, setUnits, DENSITY_UNITS, fLen, fArea, fVol, fMass, fDensity, inertiaFactor, inertiaUnit, densityUnit, toDensityUnit, fromDensityUnit } from './units.js';
import { applyColors, setOverride, colorOf, SWATCHES } from './colors.js';
import { MoveTool, tagPaths, logicalChildren, logicalParent, pivotOf, serializeTransforms, applyTransforms, findByPath } from './assembly.js';
import { QUALITY } from './loaders.js';
import { startKernel, kernelCall, kernelAvailable } from './kernel.js';
import { meshMassProps, AMAnalysis } from './analysis.js';
import { refineCandidates, refineFaces } from './refine.js';
import { build3mf, buildMatrix } from './export3mf.js';
import { Explode } from './explode.js';
import { Compare } from './compare.js';
import { PmiTool } from './pmi.js';
import { Thumbs, OpenPanel } from './openpanel.js';
import { APP_VERSION, previewDllPresent, previewRegistered, previewMachineWide, registerPreview, checkUpdate, openUrl, renderPng } from './shell.js';

const $ = (s) => document.querySelector(s);
const store = {
  get(k, d) { try { const v = localStorage.getItem('gv.' + k); return v === null ? d : JSON.parse(v); } catch (e) { return d; } },
  set(k, v) { try { localStorage.setItem('gv.' + k, JSON.stringify(v)); } catch (e) { /* ignore */ } }
};
if (isNative) {
  NL.init();
  // the window starts hidden (so a second launch can hand its files over without flashing); never stay invisible
  setTimeout(() => NL.window.show().catch(() => {}), 4000);
}

// ------------------------------------------------------------------ core objects
const viewer = new Viewer($('#view'));
const cube = new ViewCube($('#cube'), viewer);
viewer.renderHooks.push((cam) => cube.sync(cam));
const section = new SectionTool(viewer);
const measure = new MeasureTool(viewer, $('#labels'), $('#hoverTag'));
const move = new MoveTool(viewer, $('#dragReadout'));
move.learned = store.get('learnedAngles', []);
move.addEventListener('learned', () => store.set('learnedAngles', move.learned));
window.gv = { viewer, cube, section, measure, move, docs: () => docs };

// Range sliders paint their filled part from --p. Keep it in step with the value however it changes
// (user input or code), so no slider shows a stale half-filled track.
{
  const paint = (el) => {
    const lo = +el.min || 0, hi = el.max === '' ? 100 : +el.max, v = +el.value;
    el.style.setProperty('--p', `${hi > lo ? ((v - lo) / (hi - lo)) * 100 : 0}%`);
  };
  const d = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value');
  Object.defineProperty(HTMLInputElement.prototype, 'value', {
    configurable: true, enumerable: d.enumerable,
    get() { return d.get.call(this); },
    set(v) { d.set.call(this, v); if (this.type === 'range') paint(this); }
  });
  document.addEventListener('input', (e) => { if (e.target.type === 'range') paint(e.target); }, true);
  document.querySelectorAll('input[type="range"]').forEach(paint);
}
window.gvNL = NL;

document.querySelectorAll('.glass').forEach((el) => makeGlass(el));

// ------------------------------------------------------------------ segmented controls
function segSet(seg, btn) {
  const ind = seg.querySelector('.seg-ind');
  seg.querySelectorAll(':scope > .btn').forEach((b) => b.classList.toggle('active', b === btn));
  if (!btn || !btn.offsetWidth) { ind.style.opacity = 0; return; }
  const wasVisible = ind.style.opacity === '1';
  ind.style.left = btn.offsetLeft + 'px';
  ind.style.width = btn.offsetWidth + 'px';
  ind.style.opacity = 1;
  if (wasVisible) {
    ind.classList.add('moving');
    clearTimeout(ind._t);
    ind._t = setTimeout(() => ind.classList.remove('moving'), 180);
  }
}
const segRefresh = (root) => requestAnimationFrame(() => root.querySelectorAll('.seg').forEach((s) => segSet(s, s.querySelector(':scope > .btn.active'))));

// ------------------------------------------------------------------ toast / info
let toastTimer = 0;
function toast(text, err = false, hideAfter = 0) {
  const t = $('#toast');
  $('#toastText').textContent = text;
  t.classList.toggle('err', err);
  t.classList.add('show');
  refreshGlass(t);
  clearTimeout(toastTimer);
  if (hideAfter) toastTimer = setTimeout(() => t.classList.remove('show'), hideAfter);
}
function hideToast() { $('#toast').classList.remove('show'); }

const fmtN = (n) => (n >= 1e6 ? (n / 1e6).toFixed(2) + ' M' : n >= 1e3 ? (n / 1e3).toFixed(1) + ' k' : String(n));
const fmtL = (v) => (Math.abs(v) >= 100 ? v.toFixed(0) : Math.abs(v) >= 10 ? v.toFixed(1) : v.toFixed(2));
const dims = (box) => {
  const s = box.getSize(new THREE.Vector3());
  if (getUnits().length === 'in') { const k = 1 / 25.4; return `${fmtL(s.x * k)} × ${fmtL(s.y * k)} × ${fmtL(s.z * k)} in`; }
  return `${fmtL(s.x)} × ${fmtL(s.y)} × ${fmtL(s.z)} mm`;
};

function stats(root) {
  let bodies = 0, tris = 0;
  root.traverse((o) => {
    if (!isModelMesh(o)) return;
    bodies++;
    const g = o.geometry;
    tris += g.index ? g.index.count / 3 : g.attributes.position.count / 3;
  });
  return { bodies, tris };
}

function renderInfo() {
  const el = $('#info');
  const d = active;
  if (!d || !d.comps.length) { el.innerHTML = ''; return; }
  const st = stats(d.root);
  const timing = d.info.ms ? ` · ${(d.info.ms / 1000).toFixed(2)} s${d.info.fromCache ? ' (cached)' : ''}` : '';
  el.innerHTML = `<div class="l1"></div><div class="l2">${dims(viewer.modelBox)}</div><div class="l2">${d.comps.length > 1 ? d.comps.length + ' parts · ' : ''}${st.bodies} bod${st.bodies === 1 ? 'y' : 'ies'} · ${fmtN(st.tris)} triangles${timing}</div>${viewer.selected ? '<div class="l3"></div>' : ''}`;
  el.querySelector('.l1').textContent = d.name;
  if (viewer.selected) el.querySelector('.l3').textContent = `${displayName(viewer.selected)} · ${dims(viewer.boxOf(viewer.selected))}`;
}

// ------------------------------------------------------------------ part colours
// Colour mode (file / auto / uniform) belongs to each tab; new tabs start with the file colours.
// colorMode / uniformColor mirror the active tab.
let colorMode = 'file';
let uniformColor = store.get('uniformColor', '#b8c2cc');
const cmOf = (doc) => (doc && doc.colorMode) || 'file';
const ucOf = (doc) => (doc && doc.uniformColor) || store.get('uniformColor', '#b8c2cc');
function recolor(doc = active) {
  if (!doc) return;
  applyColors(doc.root, cmOf(doc), ucOf(doc));
  if (doc === active) { section.refresh(); refreshDots(); syncColorPop(); viewer.requestRender(); }
}

// ------------------------------------------------------------------ documents / tabs
let docs = [];
let active = null;
let docSeq = 0;
let quality = store.get('quality', 'normal');
if (!['coarse', 'normal', 'fine', 'ultra'].includes(quality)) quality = 'normal';

function newDoc(name) {
  const root = new THREE.Group();
  root.name = name;
  const doc = {
    id: ++docSeq, name, root, comps: [], cam: null, selected: null, loading: 0,
    measurements: [], section: SectionTool.defaultState(), info: { ms: 0, fromCache: false }, asmPath: null,
    colorMode: 'file', uniformColor: store.get('uniformColor', '#b8c2cc'), revealed: null
  };
  docs.push(doc);
  return doc;
}

function activate(doc) {
  if (active === doc) return;
  if (active) active.cam = viewer.saveCamera();
  move.attach(null);
  measure.cancel();
  if (compareOn) setCompare(false);
  if (pmi.active) setPmi(false);
  if (explode.applied.size) { explode.reset(); $('#explodeAmt').value = 0; $('#explodeVal').textContent = '0 %'; }
  active = doc;
  viewer.showDoc(doc);
  if (doc) {
    if (doc.cam) viewer.restoreCamera(doc.cam);
    else viewer.fit(false, new THREE.Vector3(...ISO[0]));
  }
  measure.setDoc(doc);
  if (doc) { colorMode = cmOf(doc); uniformColor = ucOf(doc); applyColors(doc.root, colorMode, uniformColor); syncColorModeUI(); }
  if (typeof onAnalysisTargetChange === 'function') setTimeout(onAnalysisTargetChange, 0);
  section.apply(doc ? doc.section : null);
  syncToolPanels();
  buildTree();
  renderInfo();
  renderTabs();
  syncViewSeg();
  syncShowHidden();
  document.body.classList.toggle('nomodel', !doc || !doc.comps.length);
  $('#empty').classList.toggle('hidden', !!doc);
  if (isNative) NL.window.setTitle(doc ? `${doc.name} — GlassView` : 'GlassView').catch(() => {});
  if (doc && !doc.loading) retessellate(doc);
  if (move.active && doc && doc.selected) move.attach(doc.selected);
}

function closeDoc(doc) {
  const i = docs.indexOf(doc);
  if (i < 0) return;
  docs.splice(i, 1);
  measure.dispose(doc.measurements);
  if (active === doc) {
    active = null;
    activate(docs[Math.min(i, docs.length - 1)] || null);
  } else renderTabs();
  doc.root.traverse((o) => {
    if (o.geometry) { o.geometry.boundsTree && o.geometry.disposeBoundsTree(); o.geometry.dispose(); }
    if (o.material && !o.userData.isOverlay) (Array.isArray(o.material) ? o.material : [o.material]).forEach((m) => m.dispose());
  });
}

const tabsEl = $('#tabs');
const tabSeg = $('#tabSeg');
function renderTabs() {
  tabSeg.querySelectorAll('.tab').forEach((t) => t.remove());
  tabsEl.classList.toggle('hidden', docs.length === 0);
  for (const d of docs) {
    const b = document.createElement('button');
    b.className = 'btn tab' + (d.loading ? ' loading' : '');
    b.innerHTML = '<span class="tname"></span><i class="tclose" title="Close (Ctrl+W)">×</i>';
    b.querySelector('.tname').textContent = d.name;
    b.title = d.name;
    b.addEventListener('click', (e) => { if (e.target.classList.contains('tclose')) closeDoc(d); else activate(d); });
    b.addEventListener('auxclick', (e) => { if (e.button === 1) closeDoc(d); });
    tabSeg.appendChild(b);
    if (d === active) b.classList.add('active');
  }
  refreshGlass(tabsEl);
  requestAnimationFrame(() => segSet(tabSeg, tabSeg.querySelector('.tab.active')));
}

/** Load a source into a document as a new component. */
async function loadInto(doc, src, { transforms = null, fit = false } = {}) {
  doc.loading++;
  renderTabs();
  if (doc === active) toast(`Opening ${src.name}…`);
  try {
    let compRef = null;
    const model = await loadSource(src, {
      quality,
      onProgress: (t) => { if (doc === active) toast(`${src.name}: ${t}`); },
      onEdges: (meshes, edges, faces) => {
        meshes.forEach((m, i) => { viewer.attachEdges(m, edges[i]); m.userData.faces = faces ? faces[i] : null; if (faces && faces[i] && faces[i].eranges) m.userData.native = { index: i }; });
        if (compRef) viewer.prepareBVH(compRef);
      }
    });
    const comp = model.root;
    compRef = comp;
    comp.name = src.name.replace(/\.[^.]+$/, '');
    comp.userData.component = { src, quality: model.isCAD ? quality : null, isCAD: model.isCAD, absDefl: model.isCAD ? presetDeflection(comp) : null, native: !!model.native, kernelPath: model.kernelPath || null };
    tagPaths(comp);
    doc.root.add(comp);
    doc.comps.push(comp);
    if (transforms) applyTransforms(comp, transforms, viewer);
    applyColors(doc.root, cmOf(doc), ucOf(doc));
    doc.info = { ms: model.ms, fromCache: model.fromCache };
    if (!model.fromCache) comp.userData.component.tessMs = model.ms;
    if (src.path) addRecent(src.path);
    if (doc === active) {
      viewer.showDoc(doc);
      if (fit) viewer.fit(false, new THREE.Vector3(...ISO[0]));
      section.refresh();
      buildTree();
      renderInfo();
      hideToast();
      document.body.classList.remove('nomodel');
      syncViewSeg();
    } else if (fit) {
      doc.cam = null;
    }
    return comp;
  } catch (e) {
    console.error(e);
    toast(`Could not open ${src.name}: ${e.message || e}`, true, 6000);
    return null;
  } finally {
    doc.loading--;
    renderTabs();
  }
}

async function openInNewTab(src) {
  if (isAssemblyFile(src.name)) return openAssembly(src);
  if (!formatOf(src.name)) { toast(`Unsupported file: ${src.name}`, true, 3000); return; }
  let doc = active && !active.comps.length && !active.loading ? active : null;
  if (doc) { doc.name = src.name; } else doc = newDoc(src.name);
  activate(doc);
  renderTabs();
  await loadInto(doc, src, { fit: true });
  if (!doc.comps.length && !doc.loading) closeDoc(doc);
}

async function insertPart(src) {
  if (!active) return openInNewTab(src);
  if (!formatOf(src.name)) { toast(`Unsupported file: ${src.name}`, true, 3000); return; }
  const doc = active;
  const first = !doc.comps.length;
  const comp = await loadInto(doc, src, { fit: first });
  if (comp && doc === active) {
    if (!first) viewer.fit(true);
    viewer.select(comp);
    toast(`Inserted ${src.name} — press G to move it`, false, 2600);
  }
}

/** Absolute chord tolerance (mm) a preset gives for this part (OCCT: ratio × average bbox size). */
function presetDeflection(comp) {
  const sz = viewer.boxOf(comp).getSize(new THREE.Vector3());
  return QUALITY[quality].linearDeflection * ((sz.x + sz.y + sz.z) / 3);
}

/** State that must survive a re-tessellation: colour overrides and hidden nodes (by stable path). */
function snapshotComp(comp) {
  const st = [];
  comp.traverse((o) => {
    if (o.userData.gvPath === undefined) return;
    if (o.userData.colorOverride != null || !o.visible) st.push({ path: o.userData.gvPath, c: o.userData.colorOverride, v: o.visible });
  });
  return st;
}

/**
 * Replace a CAD component by a new tessellation of the same file, keeping its placement, the moves
 * made inside it, colours, visibility and the selection. The swap happens only once the new mesh,
 * its edges and its exact surfaces are ready, so nothing flickers.
 */
async function swapComp(doc, old, { params = null, cacheTag = null, quiet = false } = {}) {
  const info = old.userData.component;
  const transforms = serializeTransforms(old).filter((e) => e.path !== '');
  const state = snapshotComp(old);
  const selPath = doc.selected && isInside(doc.selected, old) ? doc.selected.userData.gvPath : null;
  let edgesDone;
  const edgesReady = new Promise((r) => { edgesDone = r; });
  const model = await loadSource(info.src, {
    quality, params, cacheTag,
    onProgress: quiet ? null : (t) => { if (doc === active) toast(`${old.name}: ${t}`); },
    onEdges: (meshes, edges, faces) => {
      meshes.forEach((m, i) => { viewer.attachEdges(m, edges[i]); m.userData.faces = faces ? faces[i] : null; if (faces && faces[i] && faces[i].eranges) m.userData.native = { index: i }; });
      edgesDone();
    }
  });
  await edgesReady;
  if (!old.parent) return null; // closed meanwhile
  const comp = model.root;
  comp.name = old.name;
  comp.userData.component = { ...info, native: !!model.native, kernelPath: model.kernelPath || info.kernelPath };
  tagPaths(comp);
  comp.position.copy(old.position); comp.quaternion.copy(old.quaternion); comp.scale.copy(old.scale);
  if (doc === active) { measure.onLeave(); if (move.target && isInside(move.target, old)) move.attach(null); }
  const parent = old.parent;
  const idx = parent.children.indexOf(old);
  parent.remove(old);
  parent.children.splice(idx, 0, comp);
  comp.parent = parent;
  comp.updateMatrixWorld(true);
  applyTransforms(comp, transforms, viewer);
  for (const e of state) { const o = findByPath(comp, e.path); if (o) { o.userData.colorOverride = e.c ?? null; o.visible = e.v; } }
  doc.comps[doc.comps.indexOf(old)] = comp;
  applyColors(doc.root, cmOf(doc), ucOf(doc));
  if (selPath !== null) {
    const o = findByPath(comp, selPath);
    if (doc === active) { viewer.select(o); if (move.active && o) move.attach(o); } else doc.selected = o;
  }
  old.traverse((o) => { if (o.geometry && !o.userData.isOverlay) { o.geometry.boundsTree && o.geometry.disposeBoundsTree(); o.geometry.dispose(); } });
  viewer.prepareBVH(comp);
  if (doc === active) { viewer.showDoc(doc); section.refresh(); buildTree(); renderInfo(); }
  return { comp, model };
}

/** Re-tessellate CAD components whose quality differs from the current setting. */
async function retessellate(doc) {
  const stale = doc.comps.filter((c) => c.userData.component.isCAD && c.userData.component.quality !== quality);
  if (!stale.length) return;
  doc.loading++;
  renderTabs();
  try {
    for (const old of stale) {
      if (doc === active) toast(`Re-tessellating ${old.name} (${quality})…`);
      const r = await swapComp(doc, old);
      if (r) {
        r.comp.userData.component.quality = quality;
        r.comp.userData.component.absDefl = presetDeflection(r.comp);
        doc.info = { ms: r.model.ms, fromCache: r.model.fromCache };
      }
    }
  } catch (e) {
    toast(`Re-tessellation failed: ${e.message || e}`, true, 5000);
  } finally {
    doc.loading--;
    renderTabs();
  }
  if (doc === active) toast(`Tessellation: ${quality} · ${fmtN(stats(doc.root).tris)} triangles`, false, 2200);
}

// ------------------------------------------------------------------ view-dependent refinement
// Like CAD: as you zoom in, parts in view are re-tessellated so the chord error stays below
// about half a pixel. Bounded by a triangle budget; never coarsens on its own.
let refineOn = store.get('refine', true);
let refining = false, refineTimer = 0;
const REFINE_BUDGET = 2.5e6;
function scheduleRefine() { clearTimeout(refineTimer); refineTimer = setTimeout(maybeRefine, 700); }
viewer.controls.addEventListener('change', scheduleRefine);
viewer.addEventListener('viewchange', scheduleRefine);

async function maybeRefine() {
  const doc = active;
  if (!refineOn || !doc || doc.loading || refining || move.dragging || viewer.anim || am.active) return;
  const cam = viewer.camera;
  cam.updateMatrixWorld();
  const frustum = new THREE.Frustum().setFromProjectionMatrix(new THREE.Matrix4().multiplyMatrices(cam.projectionMatrix, cam.matrixWorldInverse));
  const H = viewer.canvas.clientHeight || 800;
  const total = Math.max(1, stats(doc.root).tris);

  // ---- native engine: refine only the faces in view (per-face, like CAD)
  const nat = [];
  for (const comp of doc.comps) {
    const info = comp.userData.component;
    if (!info.native || !comp.visible || !info.absDefl) continue;
    comp.traverseVisible((m) => {
      if (!isModelMesh(m) || !m.userData.native || !m.userData.faces || !m.userData.faces.eranges) return;
      m.userData.native.path = info.kernelPath;
      for (const c of refineCandidates(viewer, m, frustum, H, info.absDefl)) nat.push({ m, ...c });
    });
  }
  if (nat.length) {
    nat.sort((a, b) => b.gain - a.gain);
    const cap = Math.min(1.5e6, Math.max(0, 6e6 - total));
    const groups = new Map();
    let extra = 0, nFaces = 0;
    for (const c of nat) {
      if (extra + c.extra > cap || nFaces > 4000) continue;
      extra += c.extra; nFaces++;
      const key = c.m.uuid + '|' + c.target;
      if (!groups.has(key)) groups.set(key, { m: c.m, target: c.target, faces: [] });
      groups.get(key).faces.push(c.f);
    }
    if (groups.size) {
      refining = true;
      toast(`Refining ${nFaces} face${nFaces === 1 ? '' : 's'}…`);
      const ang = Math.min(QUALITY[quality].angularDeflection, 0.35);
      try {
        for (const g of groups.values()) await refineFaces(viewer, g.m, g.faces, g.target, ang);
        if (doc === active) {
          section.refresh();
          renderInfo();
          toast(`Refined ${nFaces} face${nFaces === 1 ? '' : 's'} · ${fmtN(stats(doc.root).tris)} triangles`, false, 1400);
        }
      } catch (e) {
        console.warn('native refine failed', e);
        hideToast();
      } finally {
        refining = false;
      }
      scheduleRefine();
      return;
    }
  }

  let best = null;
  for (const comp of doc.comps) {
    const info = comp.userData.component;
    if (!info.isCAD || info.native || !comp.visible || !info.absDefl) continue;
    const box = viewer.boxOf(comp);
    if (box.isEmpty() || !frustum.intersectsBox(box)) continue;
    // distance to what you are looking at (orbit target follows zoom-to-cursor), never closer than the part
    const dist = Math.max(box.distanceToPoint(cam.position), cam.position.distanceTo(viewer.controls.target) * 0.6);
    const wpp = cam.isPerspectiveCamera ? (2 * dist * Math.tan(THREE.MathUtils.degToRad(cam.fov) / 2)) / H : (cam.top - cam.bottom) / cam.zoom / H;
    const want = wpp * 0.5;
    const cur = info.absDefl;
    if (want > cur / 2.5) continue;
    const tris = stats(comp).tris;
    // huge or slow-to-tessellate parts are never refined automatically (it would take minutes)
    if (tris > 1.5e6 || (info.tessMs || 0) > 15000) continue;
    const budget = Math.max(3e5, REFINE_BUDGET * (tris / total));
    const floor = cur * Math.pow(tris / budget, 1 / 0.8); // predicted triangles ≈ tris·(cur/target)^0.8
    const sz = box.getSize(new THREE.Vector3());
    const sane = ((sz.x + sz.y + sz.z) / 3) * 2e-5; // finer than this is never visible in practice
    let target = Math.max(Math.pow(2, Math.floor(Math.log2(want))), floor, sane);
    if (target > cur / 1.8) continue;
    const gain = cur / target;
    if (!best || gain > best.gain) best = { comp, target, gain };
  }
  if (!best) return;
  refining = true;
  const { comp, target } = best;
  const ang = Math.min(QUALITY[quality].angularDeflection, 0.35);
  const params = { linearUnit: 'millimeter', linearDeflectionType: 'absolute_value', linearDeflection: target, angularDeflection: ang };
  toast(`Refining ${comp.name}…`);
  try {
    const r = await swapComp(doc, comp, { params, cacheTag: `abs${target.toPrecision(3)}a${ang}`, quiet: true });
    if (r) {
      r.comp.userData.component.absDefl = target;
      if (doc === active) toast(`Refined ${comp.name} · ${fmtN(stats(doc.root).tris)} triangles`, false, 1600);
    }
  } catch (e) {
    console.warn('refine failed', e);
    hideToast();
  } finally {
    refining = false;
  }
  scheduleRefine(); // the view may have changed meanwhile
}
$('#chkRefine').checked = refineOn;
$('#chkRefine').addEventListener('change', (e) => { refineOn = e.target.checked; store.set('refine', refineOn); if (refineOn) scheduleRefine(); });

const isInside = (o, root) => { for (let p = o; p; p = p.parent) if (p === root) return true; return false; };

// ------------------------------------------------------------------ assembly files (.gva)
async function openAssembly(src) {
  if (!isNative || !src.path) { toast('Assembly files can only be opened in the desktop app', true, 4000); return; }
  let data;
  try { data = JSON.parse(await NL.filesystem.readFile(src.path)); } catch (e) { toast('Not a valid assembly file', true, 4000); return; }
  const doc = newDoc(src.name);
  doc.asmPath = src.path;
  activate(doc);
  for (const c of data.components || []) {
    try {
      const s = await sourceFromPath(c.path);
      await loadInto(doc, s, { transforms: c.transforms, fit: doc.comps.length === 0 });
    } catch (e) {
      toast(`Missing part: ${c.path}`, true, 5000);
    }
  }
  if (doc === active) viewer.fit(false, new THREE.Vector3(...ISO[0]));
}

async function saveAssembly() {
  const doc = active;
  if (!doc || !doc.comps.length) return;
  if (!isNative) { toast('Saving assemblies needs the desktop app — use Export GLB here', true, 4000); return; }
  const comps = doc.comps.filter((c) => c.userData.component.src.path);
  const skipped = doc.comps.length - comps.length;
  let path;
  try {
    path = await NL.os.showSaveDialog('Save assembly', { filters: [{ name: 'GlassView assembly', extensions: ['gva'] }], defaultPath: doc.asmPath || doc.name.replace(/\.[^.]+$/, '') + '.gva' });
  } catch (e) { return; }
  if (!path) return;
  if (!/\.gva$/i.test(path)) path += '.gva';
  const data = {
    format: 'glassview-assembly', version: 1,
    components: comps.map((c) => ({ name: c.name, path: c.userData.component.src.path, transforms: serializeTransforms(c) }))
  };
  await NL.filesystem.writeFile(path, JSON.stringify(data, null, 2));
  doc.asmPath = path;
  toast(skipped ? `Saved — ${skipped} part(s) added by drag & drop have no file path and were skipped` : 'Assembly saved', !!skipped, 4000);
}

async function exportGLB() {
  const doc = active;
  if (!doc || !doc.comps.length) return;
  toast('Exporting GLB…');
  const { GLTFExporter } = await import('three/examples/jsm/exporters/GLTFExporter.js');
  const sel = viewer.selected;
  viewer.select(null);
  const hidden = [];
  doc.root.traverse((o) => { if (o.userData.isOverlay && o.visible) { o.visible = false; hidden.push(o); } });
  // glTF is Y-up: export through a temporary rotated wrapper
  const wrap = new THREE.Group();
  wrap.rotation.x = -Math.PI / 2;
  const parent = doc.root.parent;
  wrap.add(doc.root);
  try {
    const glb = await new GLTFExporter().parseAsync(wrap, { binary: true, onlyVisible: true });
    const name = doc.name.replace(/\.[^.]+$/, '') + '.glb';
    if (isNative) {
      const path = await NL.os.showSaveDialog('Export GLB', { filters: [{ name: 'glTF binary', extensions: ['glb'] }], defaultPath: name });
      if (path) { await NL.filesystem.writeBinaryFile(/\.glb$/i.test(path) ? path : path + '.glb', glb); toast('Exported', false, 2000); } else hideToast();
    } else {
      const a = document.createElement('a');
      a.href = URL.createObjectURL(new Blob([glb], { type: 'model/gltf-binary' }));
      a.download = name;
      a.click();
      setTimeout(() => URL.revokeObjectURL(a.href), 5000);
      toast('Exported', false, 2000);
    }
  } catch (e) {
    toast(`Export failed: ${e.message || e}`, true, 4000);
  } finally {
    if (parent) parent.add(doc.root); else wrap.remove(doc.root);
    hidden.forEach((o) => { o.visible = true; });
    viewer._applyDisplay();
    if (sel) viewer.select(sel);
  }
}

// ------------------------------------------------------------------ views
const VIEW_DIRS = { xy: [0, 0, 1], yz: [1, 0, 0], zx: [0, -1, 0] };
const ISO = [[1, -1, 1], [1, 1, 1], [-1, 1, 1], [-1, -1, 1]];
let isoIndex = 0;
const segView = $('#segView');
function setView(name) {
  const cur = viewer.currentViewDir();
  let dir;
  if (name === 'iso') {
    if (cur.dot(new THREE.Vector3(...ISO[isoIndex]).normalize()) > 0.999) isoIndex = (isoIndex + 1) % 4;
    dir = new THREE.Vector3(...ISO[isoIndex]);
  } else {
    dir = new THREE.Vector3(...VIEW_DIRS[name]);
    if (cur.dot(dir) > 0.999) dir.negate();
  }
  viewer.snapTo(dir);
}
segView.querySelectorAll('.btn').forEach((b) => b.addEventListener('click', () => setView(b.dataset.view)));
function syncViewSeg() {
  const cur = viewer.currentViewDir();
  let hit = null;
  for (const b of segView.querySelectorAll('.btn')) {
    const v = b.dataset.view;
    const dirs = v === 'iso' ? ISO : [VIEW_DIRS[v], VIEW_DIRS[v].map((x) => -x)];
    if (dirs.some((d) => cur.dot(new THREE.Vector3(...d).normalize()) > 0.999)) hit = b;
  }
  segSet(segView, hit);
}
viewer.addEventListener('viewchange', syncViewSeg);
viewer.controls.addEventListener('end', () => setTimeout(syncViewSeg, 250));
viewer.controls.addEventListener('start', () => segSet(segView, null));
$('#btnFit').addEventListener('click', () => viewer.fit());
const home = () => { isoIndex = 0; viewer.fit(true, new THREE.Vector3(...ISO[0])); };

const btnProj = $('#btnProj');
function setProjection(p) {
  viewer.setProjection(p);
  btnProj.classList.toggle('ortho', p === 'ortho');
  btnProj.dataset.tip = p === 'ortho' ? 'Orthographic — switch to perspective (P)' : 'Perspective — switch to orthographic (P)';
  store.set('proj2', p);
  window.gvSyncKeyTips?.();
}
btnProj.addEventListener('click', () => setProjection(viewer.camera.isOrthographicCamera ? 'persp' : 'ortho'));

// ------------------------------------------------------------------ display
const segMode = $('#segMode');
function setMode(mode) {
  viewer.setDisplayMode(mode);
  segSet(segMode, segMode.querySelector(`[data-mode="${mode}"]`));
  store.set('mode', mode);
  updateEdgeColor();
}
segMode.querySelectorAll('.btn').forEach((b) => b.addEventListener('click', () => setMode(b.dataset.mode)));
const btnXray = $('#btnXray');
function setXray(on) { viewer.setXray(on); btnXray.classList.toggle('on', on); updateEdgeColor(); }
btnXray.addEventListener('click', () => setXray(!viewer.xray));

// ------------------------------------------------------------------ backgrounds & day/night
const mq = window.matchMedia('(prefers-color-scheme: dark)');
let appearance = store.get('appearance', 'auto');
let currentBg = null;
const isNight = () => appearance === 'night' || (appearance === 'auto' && mq.matches);

/** Light / dark: one calm background each (no colour themes). */
function applyBg(bg) {
  currentBg = bg;
  $('#bg').style.background = bg.css;
  document.documentElement.dataset.theme = bg.dark ? 'dark' : 'light';
  cube.setDark(bg.dark);
  viewer.setDark(bg.dark);
  updateEdgeColor();
  viewer.requestRender();
}
function applyAppearance() {
  applyBg(BACKGROUNDS.find((b) => b.id === (isNight() ? 'night' : 'studio')));
  segSet(segAppearance, segAppearance.querySelector(`[data-ap="${appearance}"]`));
}
const segAppearance = $('#segAppearance');
segAppearance.querySelectorAll('.btn').forEach((b) => b.addEventListener('click', () => {
  appearance = b.dataset.ap; store.set('appearance', appearance); applyAppearance();
}));
mq.addEventListener('change', () => { if (appearance === 'auto') applyAppearance(); });

function updateEdgeColor() {
  const noFaces = viewer.displayMode === 'wire' || viewer.xray;
  viewer.setEdgeColor(!(noFaces && currentBg && currentBg.dark));
}
function cycleBg() {
  appearance = isNight() ? 'day' : 'night';
  store.set('appearance', appearance);
  applyAppearance();
}

// ------------------------------------------------------------------ popovers & settings
const pops = { bg: $('#bgPop'), set: $('#setPop'), color: $('#colorPop') };
function togglePop(name) {
  for (const [k, el] of Object.entries(pops)) {
    const open = k === name && !el.classList.contains('open');
    el.classList.toggle('open', open);
    if (open) { refreshGlass(el); segRefresh(el); }
  }
}
$('#btnBg').addEventListener('click', (e) => { e.stopPropagation(); togglePop('bg'); });
$('#btnSettings').addEventListener('click', (e) => { e.stopPropagation(); togglePop('set'); });
$('#btnColors').addEventListener('click', (e) => { e.stopPropagation(); togglePop('color'); syncColorPop(); });
document.addEventListener('pointerdown', (e) => {
  if (!e.target.closest('.popover') && !e.target.closest('#btnBg, #btnSettings, #btnColors, .row .dot')) togglePop(null);
});

// --- colour popover
const segColorMode = $('#segColorMode');
function syncColorModeUI() {
  const seg = $('#segColorMode');
  segSet(seg, seg.querySelector(`[data-cm="${colorMode}"]`));
  $('#uniformRow').classList.toggle('hidden', colorMode !== 'uniform');
  $('#uniformColor').value = uniformColor;
}
function setColorMode(m) {
  colorMode = m;
  if (active) { active.colorMode = m; recolor(active); }
  syncColorModeUI();
}
segColorMode.querySelectorAll('.btn').forEach((b) => b.addEventListener('click', () => setColorMode(b.dataset.cm)));
$('#uniformColor').addEventListener('input', (e) => {
  uniformColor = e.target.value; store.set('uniformColor', uniformColor);
  if (active) { active.uniformColor = uniformColor; if (colorMode === 'uniform') recolor(active); }
});
const paintSelected = (hex) => { if (viewer.selected) { setOverride(viewer.selected, hex); recolor(); } };
for (const hex of SWATCHES) {
  const b = document.createElement('button');
  b.className = 'sw';
  b.style.background = hex;
  b.title = hex;
  b.addEventListener('click', () => paintSelected(hex));
  $('#partSwatches').appendChild(b);
}
$('#partColor').addEventListener('input', (e) => paintSelected(e.target.value));
$('#btnResetColor').addEventListener('click', () => paintSelected(null));
$('#btnResetAllColors').addEventListener('click', () => {
  if (!active) return;
  active.root.traverse((o) => { o.userData.colorOverride = null; });
  recolor();
});
function syncColorPop() {
  const sel = viewer.selected;
  $('#selColorBox').classList.toggle('none', !sel);
  $('#selColorName').textContent = sel ? displayName(sel) : 'Select a part to recolor it';
  if (sel) $('#partColor').value = colorOf(sel);
}

const segQuality = $('#segQuality');
function setQuality(q, apply = true) {
  quality = q;
  store.set('quality', q);
  segSet(segQuality, segQuality.querySelector(`[data-q="${q}"]`));
  if (apply && active && !active.loading) retessellate(active);
}
segQuality.querySelectorAll('.btn').forEach((b) => b.addEventListener('click', () => setQuality(b.dataset.q)));
const segGlass = $('#segGlass');
function applyGlass(g) { setGlassMode(g); store.set('glass', g); segSet(segGlass, segGlass.querySelector(`[data-g="${g}"]`)); }
segGlass.querySelectorAll('.btn').forEach((b) => b.addEventListener('click', () => applyGlass(b.dataset.g)));
$('#chkGrid').addEventListener('change', (e) => {
  viewer.setGrid(e.target.checked); store.set('grid', e.target.checked);
  $('#gridOpts').classList.toggle('hidden', !e.target.checked);
  segRefresh($('#gridOpts'));
});
const segGridSpacing = $('#segGridSpacing');
let gridSpacing = store.get('gridSpacing', 'auto');
let gridExt = store.get('gridExt', 3.2);
function applyGridOpts() {
  viewer.setGridOptions(gridSpacing, gridExt);
  segSet(segGridSpacing, segGridSpacing.querySelector(`[data-gs="${gridSpacing}"]`));
  const gs = $('#gridSize');
  gs.value = Math.round(gridExt * 10);
  gs.style.setProperty('--p', ((gs.value - gs.min) / (gs.max - gs.min)) * 100 + '%');
}
segGridSpacing.querySelectorAll('.btn').forEach((b) => b.addEventListener('click', () => { gridSpacing = b.dataset.gs; store.set('gridSpacing', gridSpacing); applyGridOpts(); }));
$('#gridSize').addEventListener('input', (e) => { gridExt = e.target.value / 10; store.set('gridExt', gridExt); applyGridOpts(); });
$('#btnClearCache').addEventListener('click', async () => {
  const n = await clearCache();
  toast(isNative ? `Cleared ${n} cached model${n === 1 ? '' : 's'}` : 'The cache is only used in the desktop app', false, 1800);
});

// ------------------------------------------------------------------ tools
let massOn = false;
const am = new AMAnalysis(viewer);
const tools = { measure: $('#btnMeasure'), cut: $('#btnCut'), move: $('#btnMove'), mass: $('#btnMass'), am: $('#btnAM'), explode: $('#btnExplode'), compare: $('#btnCompare'), pmi: $('#btnPmi') };
const explode = new Explode(viewer);
const compare = new Compare(viewer);
const pmi = new PmiTool(viewer, $('#labels'));
let explodeOn = false, compareOn = false;
function setMeasure(on) {
  if (on && move.active) setMove(false);
  measure.setActive(on);
  if (on && active) viewer.prepareBVH(active.root);
  syncToolPanels();
}
function setMove(on) {
  if (on && measure.active) setMeasure(false);
  move.setActive(on);
  syncToolPanels();
  if (on && !viewer.selected) toast('Select a part to move — click it or pick it in the tree', false, 2400);
}
function setCut(on) {
  if (!active) return;
  active.section.on = on;
  if (on && active.section.axis === 'view') active.section.normal = viewer.currentViewDir();
  section.apply(active.section);
  syncToolPanels();
}
tools.measure.addEventListener('click', () => setMeasure(!measure.active));
tools.move.addEventListener('click', () => setMove(!move.active));
tools.cut.addEventListener('click', () => setCut(!(active && active.section.on)));

function syncToolPanels() {
  const cutOn = !!(active && active.section.on);
  tools.measure.classList.toggle('on', measure.active);
  tools.move.classList.toggle('on', move.active);
  tools.cut.classList.toggle('on', cutOn);
  tools.mass.classList.toggle('on', massOn);
  tools.am.classList.toggle('on', am.active);
  tools.explode.classList.toggle('on', explodeOn);
  tools.compare.classList.toggle('on', compareOn);
  tools.pmi.classList.toggle('on', pmi.active);
  for (const [id, on] of [['#cutPanel', cutOn], ['#measurePanel', measure.active], ['#movePanel', move.active], ['#massPanel', massOn], ['#amPanel', am.active], ['#explodePanel', explodeOn], ['#comparePanel', compareOn], ['#pmiPanel', pmi.active]]) {
    const el = $(id);
    if (on !== el.classList.contains('open')) {
      el.classList.toggle('open', on);
      if (on) { refreshGlass(el); segRefresh(el); }
    }
  }
  if (cutOn) syncCutPanel();
  renderMeasureList();
  syncMovePanel();
}

// --- cut panel
const segCut = $('#segCut');
const cutSlider = $('#cutSlider');
function syncCutPanel() {
  const st = active.section;
  segSet(segCut, segCut.querySelector(`[data-axis="${st.axis}"]`));
  cutSlider.value = Math.round(st.t * 1000);
  cutSlider.style.setProperty('--p', st.t * 100 + '%');
  if (document.activeElement !== $('#cutValue')) $('#cutValue').value = section.offsetValue().toFixed(2);
  $('#chkCaps').checked = st.caps;
}
segCut.querySelectorAll('.btn').forEach((b) => b.addEventListener('click', () => {
  const st = active.section;
  st.axis = b.dataset.axis;
  st.t = 0.5;
  if (st.axis === 'view') st.normal = viewer.currentViewDir();
  section.apply(st);
  syncCutPanel();
}));
let shiftDown = false;
window.addEventListener('keydown', (e) => { if (e.key === 'Shift') shiftDown = true; });
window.addEventListener('keyup', (e) => { if (e.key === 'Shift') shiftDown = false; });
window.addEventListener('blur', () => { shiftDown = false; });
function cutSnapNote(sn) {
  const el = $('#cutSnap');
  if (!el) return;
  el.textContent = sn ? `⇧ snapped to ${sn.kind}` : 'Hold ⇧ Shift while dragging to snap to faces and axes · ⇧+click a face to cut on it';
  el.classList.toggle('on', !!sn);
}
cutSlider.addEventListener('input', () => {
  const st = active.section;
  let t = cutSlider.value / 1000;
  let sn = null;
  if (shiftDown) {
    const { lo, hi } = section.range();
    sn = section.snapOffset(lo + (hi - lo) * t, (hi - lo) * 0.025);
    if (sn && hi > lo) t = (sn.d - lo) / (hi - lo);
  }
  st.t = t;
  section.update();
  syncCutPanel();
  cutSnapNote(sn);
});
cutSlider.addEventListener('change', () => cutSnapNote(null));
$('#cutValue').addEventListener('change', (e) => { section.setOffsetValue(+e.target.value); syncCutPanel(); });
$('#btnFlip').addEventListener('click', () => { active.section.flip = !active.section.flip; section.update(); });
$('#chkCaps').addEventListener('change', (e) => { active.section.caps = e.target.checked; section.apply(active.section); });
viewer.addEventListener('bounds', () => { if (active && active.section.on) { section.rebase(); syncCutPanel(); } });
section.addEventListener('change', () => { syncCutPanel(); cutSnapNote(section.snapped); });
{
  const cv = $('#view');
  let cutDrag = false;
  cv.addEventListener('pointerdown', (e) => {
    if (e.button !== 0 || !active || !active.section.on || !section.hitHandle(e)) return;
    e.stopImmediatePropagation(); e.preventDefault();
    cv.setPointerCapture(e.pointerId);
    cutDrag = true;
    section.beginDrag(e);
  }, { capture: true });
  cv.addEventListener('pointermove', (e) => {
    if (cutDrag) { e.stopImmediatePropagation(); section.dragTo(e); return; }
    if (active && active.section.on && !e.buttons) section.setHot(section.hitHandle(e));
  }, { capture: true });
  cv.addEventListener('pointerup', (e) => {
    if (!cutDrag) return;
    e.stopImmediatePropagation();
    cutDrag = false;
    section.endDrag();
    cutSnapNote(null);
  }, { capture: true });
}

// --- measure panel
function renderMeasureList() {
  const el = $('#mlist');
  el.innerHTML = '';
  if (!active) return;
  active.measurements.forEach((m, i) => {
    const r = document.createElement('div');
    r.className = 'mrow';
    r.innerHTML = `<div class="mtop"><span class="idx">${i + 1}</span><b></b><span class="kind"></span><i title="Remove">×</i></div>`;
    r.querySelector('b').textContent = m.res.main;
    r.querySelector('.kind').textContent = m.res.kind;
    r.title = m.from || '';
    r.querySelector('i').addEventListener('click', () => measure.remove(m));
    if (m.delta) {
      const d = document.createElement('div');
      d.className = 'mxyz';
      d.innerHTML = ['x', 'y', 'z'].map((a) => `<span class="ax-${a}">Δ${a.toUpperCase()} <b>${fLen(Math.abs(m.delta[a]), false)}</b></span>`).join('');
      r.appendChild(d);
    }
    if (m.res.cond) {
      for (const [w, e] of [['A', m.A], ['B', m.B]]) {
        if (!hasCond(e)) continue;
        const c = document.createElement('div');
        c.className = 'mcond';
        c.innerHTML = `<span>Ø${fLen(e.r * 2, false, 2)}</span><div class="seg tiny"></div>`;
        const sg = c.querySelector('.seg');
        for (const [v, lbl, tip] of [['centre', 'Centre', 'Centre line / axis'], ['near', 'Near side', 'Nearest surface (min)'], ['far', 'Far side', 'Far surface (max)']]) {
          const b = document.createElement('button');
          b.className = 'btn txt' + ((e.cond || 'centre') === v ? ' on' : '');
          b.textContent = lbl;
          b.title = tip;
          b.addEventListener('click', () => measure.setCond(m, w, v));
          sg.appendChild(b);
        }
        r.appendChild(c);
      }
    }
    el.appendChild(r);
  });
}
measure.addEventListener('change', renderMeasureList);
$('#btnClearMeasure').addEventListener('click', () => measure.clear());
$('#chkSnap').addEventListener('change', (e) => { measure.snap = e.target.checked; store.set('mSnap', measure.snap); measure.refreshOverview(); });
measure.snap = store.get('mSnap', true); $('#chkSnap').checked = measure.snap;
measure.showSnaps = store.get('mShowSnaps', true); $('#chkSnapPts').checked = measure.showSnaps;
$('#chkXyz').checked = measure.xyz = store.get('mXyz', false);
$('#chkXyz').addEventListener('change', (e) => { store.set('mXyz', e.target.checked); measure.setXyz(e.target.checked, docs.map((d) => d.measurements)); renderMeasureList(); });
measure.setShow(store.get('mShow', measure.show));
const showChips = [...document.querySelectorAll('#showChips .chip-t')];
const syncChips = () => showChips.forEach((b) => b.classList.toggle('on', !!measure.show[b.dataset.show]));
showChips.forEach((b) => b.addEventListener('click', () => {
  measure.setShow({ [b.dataset.show]: !measure.show[b.dataset.show] });
  store.set('mShow', measure.show);
  syncChips();
}));
syncChips();
$('#chkSnapPts').addEventListener('change', (e) => { measure.showSnaps = e.target.checked; store.set('mShowSnaps', measure.showSnaps); measure.refreshOverview(); });
const segMUnit = $('#segMUnit');
segMUnit.querySelectorAll('.btn').forEach((b) => b.addEventListener('click', () => setUnits({ length: b.dataset.lu })));
const segLenUnit = $('#segLenUnit');
segLenUnit.querySelectorAll('.btn').forEach((b) => b.addEventListener('click', () => setUnits({ length: b.dataset.lu })));
const segDenUnit = $('#segDenUnit');
segDenUnit.querySelectorAll('.btn').forEach((b) => b.addEventListener('click', () => setUnits({ density: b.dataset.du })));
function syncUnits() {
  const u = getUnits();
  for (const sg of [segMUnit, segLenUnit]) segSet(sg, sg.querySelector(`[data-lu="${u.length}"]`));
  segSet(segDenUnit, segDenUnit.querySelector(`[data-du="${u.density}"]`));
}
units.addEventListener('change', () => {
  syncUnits();
  measure.relabel(docs.map((d) => d.measurements));
  if (typeof syncDensityField === 'function') syncDensityField();
  if (massOn) computeMass();
  syncAmPanel();
  if (am.active && am.stats) am.dispatchEvent(new Event('stats'));
  renderInfo();
});
measure.mode = 'smart';
measure.addEventListener('pending', () => {
  $('#mHint').textContent = measure.pending
    ? `1st: ${labelOf(measure.pending)} — now pick the second entity. Esc cancels.`
    : 'Hover to preview, click two entities. Hold Shift or Alt to lock onto one face. Drag a result label to move it. Esc cancels.';
});

// --- move panel
const segSpace = $('#segSpace');
const segSnap = $('#segSnap');
const moveInputs = [...document.querySelectorAll('#moveInputs input')];
function syncMovePanel() {
  if (!move.active) return;
  const t = move.target;
  const nameEl = $('#moveName');
  nameEl.textContent = t ? displayName(t) : 'Select a part';
  nameEl.classList.toggle('none', !t);
  $('#btnParent').disabled = !t || !active || logicalParent(t) === active.root;
  $('#moveInputs').classList.toggle('disabled', !t);
  const v = move.values();
  moveInputs.forEach((inp) => {
    if (document.activeElement === inp) return;
    const k = inp.dataset.k;
    const val = v ? (k[0] === 't' ? v.t : v.r)[+k[1]] : 0;
    inp.value = Math.abs(val) < 1e-9 ? '0' : +val.toFixed(2);
  });
  segSet(segSpace, segSpace.querySelector(`[data-space="${move.tcT.space}"]`));
  segSet(segSnap, segSnap.querySelector(`[data-snap="${move.snapMode}"]`));
  $('#stepRow').classList.toggle('hidden', move.snapMode !== 'step');
  $('#btnSaveAsm').style.display = isNative ? '' : 'none';
}
move.addEventListener('change', syncMovePanel);
move.addEventListener('committed', () => { section.refresh(); renderInfo(); syncMovePanel(); onAnalysisTargetChange(); });
moveInputs.forEach((inp) => inp.addEventListener('change', () => {
  const v = move.values();
  if (!v) return;
  const k = inp.dataset.k;
  (k[0] === 't' ? v.t : v.r)[+k[1]] = +inp.value || 0;
  move.setValues(v);
}));
segSpace.querySelectorAll('.btn').forEach((b) => b.addEventListener('click', () => { move.setSpace(b.dataset.space); syncMovePanel(); }));
const applySnap = (mode = move.snapMode) => {
  move.setSnap(mode, +$('#snapT').value || 1, +$('#snapR').value || 15);
  store.set('snap', { mode, t: move.tStep, r: move.rStep });
  syncMovePanel();
};
segSnap.querySelectorAll('.btn').forEach((b) => b.addEventListener('click', () => applySnap(b.dataset.snap)));
['#snapT', '#snapR'].forEach((sel) => $(sel).addEventListener('change', () => applySnap()));
{ const sn = store.get('snap', { mode: 'magnetic', t: 1, r: 15 }); $('#snapT').value = sn.t; $('#snapR').value = sn.r; move.setSnap(sn.mode, sn.t, sn.r); }
$('#btnResetMove').addEventListener('click', () => move.reset());
$('#btnParent').addEventListener('click', () => {
  const t = move.target;
  if (!t) return;
  const p = logicalParent(t);
  if (p && p !== active.root) viewer.select(p);
});
$('#btnSaveAsm').addEventListener('click', saveAssembly);
$('#btnExportGlb').addEventListener('click', exportGLB);

// ------------------------------------------------------------------ tree panel
const treePanel = $('#treePanel');
const treeEl = $('#tree');
let rowOf = new Map();
function setTreeVisible(on) {
  treePanel.classList.toggle('hidden', !on);
  $('#btnTree').classList.toggle('on', on);
  store.set('tree', on);
}
$('#btnTree').addEventListener('click', () => setTreeVisible(treePanel.classList.contains('hidden')));

const ICON_CHEV = '<svg viewBox="0 0 12 12"><path d="M4 2.5 7.5 6 4 9.5"/></svg>';
const ICON_EYE = '<svg viewBox="0 0 12 12"><path d="M1 6s1.8-3.5 5-3.5S11 6 11 6 9.2 9.5 6 9.5 1 6 1 6z"/><circle cx="6" cy="6" r="1.4"/></svg>';
const ICON_EYE_OFF = '<svg viewBox="0 0 12 12"><path d="M1 6s1.8-3.5 5-3.5S11 6 11 6 9.2 9.5 6 9.5 1 6 1 6zM2 10 10 2"/></svg>';

function displayName(obj) { return obj.name || (obj.isMesh ? 'Body' : 'Assembly'); }
function collapse(obj) {
  let o = obj;
  for (;;) {
    const ch = logicalChildren(o);
    if (o.isGroup && ch.length === 1 && !o.userData.component && (!o.name || !ch[0].name || o.name === ch[0].name)) o = ch[0];
    else return o;
  }
}

function makeRow(orig, depth) {
  const obj = collapse(orig);
  const wrap = document.createElement('div');
  const row = document.createElement('div');
  row.className = 'row';
  row.style.paddingLeft = 4 + depth * 14 + 'px';
  const kids = () => logicalChildren(obj);
  const nKids = kids().length;
  row._obj = obj;
  row.innerHTML = `<span class="chev${nKids ? '' : ' leaf'}">${ICON_CHEV}</span><span class="dot" title="Color"></span><span class="name"></span><span class="muted">${nKids || ''}</span><span class="eye">${obj.visible ? ICON_EYE : ICON_EYE_OFF}</span>`;
  row.querySelector('.name').textContent = displayName(orig.userData.component ? orig : obj);
  if (!obj.visible) row.classList.add('off');
  const chev = row.querySelector('.chev');
  const eye = row.querySelector('.eye');
  const dot = row.querySelector('.dot');
  dot.style.background = colorOf(obj);
  dot.addEventListener('click', (e) => {
    e.stopPropagation();
    viewer.select(obj);
    if (!pops.color.classList.contains('open')) togglePop('color');
    syncColorPop();
  });
  row.addEventListener('mouseenter', () => { if (!measure.active && showHover) viewer.setHover(obj); });
  row.addEventListener('mouseleave', () => viewer.setHover(null));
  wrap.appendChild(row);
  const childBox = document.createElement('div');
  wrap.appendChild(childBox);
  for (let o = obj; o && o !== orig.parent; o = o.parent) rowOf.set(o, row);
  let built = false;
  const expand = (open) => {
    if (!nKids) return;
    if (open && !built) { for (const k of kids()) childBox.appendChild(makeRow(k, depth + 1)); built = true; }
    childBox.style.display = open ? '' : 'none';
    chev.classList.toggle('open', open);
  };
  row._expand = expand;
  chev.addEventListener('click', (e) => { e.stopPropagation(); expand(!chev.classList.contains('open')); });
  eye.addEventListener('click', (e) => {
    e.stopPropagation();
    obj.visible = !obj.visible;
    eye.innerHTML = obj.visible ? ICON_EYE : ICON_EYE_OFF;
    row.classList.toggle('off', !obj.visible);
    if (active && active.revealed) active.revealed = active.revealed.filter((o) => o !== obj); // your choice wins
    viewer.requestRender();
  });
  row.addEventListener('click', () => viewer.select(viewer.selected === obj ? null : obj));
  row.addEventListener('dblclick', () => { const b = viewer.boxOf(obj); if (!b.isEmpty()) viewer.fit(true, null, b); });
  if (depth === 0 || (depth === 1 && nKids <= 50)) expand(true);
  return wrap;
}

function buildTree() {
  treeEl.innerHTML = '';
  rowOf = new Map();
  if (!active) { $('#treeCount').textContent = ''; return; }
  for (const c of active.comps) treeEl.appendChild(makeRow(c, 0));
  const { bodies } = stats(active.root);
  $('#treeCount').textContent = `${bodies} bod${bodies === 1 ? 'y' : 'ies'}`;
  if (viewer.selected) markSelected(viewer.selected);
}

function refreshDots() {
  for (const [o, row] of rowOf) if (row._obj === o) row.querySelector('.dot').style.background = colorOf(o);
}

function revealRow(obj) {
  const chain = [];
  for (let o = obj; o; o = o.parent) chain.unshift(o);
  let row = null;
  for (const o of chain) { const r = rowOf.get(o); if (r) { row = r; r._expand(true); } }
  return rowOf.get(obj) || row;
}
function markSelected(obj) {
  treeEl.querySelectorAll('.row.sel').forEach((r) => r.classList.remove('sel'));
  if (!obj) return;
  const row = revealRow(obj);
  if (row) { row.classList.add('sel'); row.scrollIntoView({ block: 'nearest', behavior: 'smooth' }); }
}
viewer.addEventListener('select', (e) => {
  markSelected(e.detail);
  renderInfo();
  syncColorPop();
  onAnalysisTargetChange();
  if (move.active) move.attach(e.detail);
});

// ------------------------------------------------------------------ opening files
async function nativePick(title, multiple) {
  try {
    return (await NL.os.showOpenDialog(title, {
      multiSelections: multiple,
      filters: [{ name: '3D models & assemblies', extensions: [...ALL_EXT, 'gva'] }, { name: 'All files', extensions: ['*'] }]
    })) || [];
  } catch (e) { return []; }
}
// GlassView's own open panel (recent files with 3D thumbnails, pinned folders, search); "Browse…" in it
// still opens the Windows dialog. Settings > "Open files with the GlassView browser" switches back.
const thumbs = new Thumbs(() => quality);
const openPanel = isNative ? new OpenPanel({ store, thumbs, nativePick }) : null;
async function pickFiles(multiple, title = 'Open', action = 'Open') {
  if (isNative) {
    const paths = openPanel && store.get('openPanel', true)
      ? await openPanel.show({ title, action, multiple })
      : await nativePick(title, multiple);
    const out = [];
    for (const p of paths || []) { try { out.push(await sourceFromPath(p)); } catch (e) { toast(`Cannot read ${p}`, true, 4000); } }
    return out;
  }
  return new Promise((resolve) => {
    const inp = $('#fileInput');
    inp.multiple = multiple;
    inp.onchange = () => { resolve([...inp.files].map(sourceFromFile)); inp.value = ''; };
    inp.click();
  });
}
async function openDialog() { for (const s of await pickFiles(true, 'Open', 'Open')) openInNewTab(s); }
async function insertDialog() { for (const s of await pickFiles(true, 'Insert part', 'Insert')) await insertPart(s); }
$('#btnOpen').addEventListener('click', openDialog);
$('#btnOpen2').addEventListener('click', openDialog);
$('#btnNewTab').addEventListener('click', openDialog);
$('#btnInsert').addEventListener('click', insertDialog);

async function openPath(path) {
  try { await openInNewTab(await sourceFromPath(path)); }
  catch (e) { toast(`Could not open ${path}`, true, 5000); removeRecent(path); }
}

function getRecent() { return store.get('recent', []); }
function addRecent(p) {
  store.set('recent', [p, ...getRecent().filter((x) => x !== p)].slice(0, 40));
  renderRecent();
  // make (or refresh) its thumbnail a little later, from the tessellation that is now cached
  setTimeout(async () => { try { thumbs.request(p, await NL.filesystem.getStats(p)); } catch (e) { /* gone */ } }, 2500);
}
function removeRecent(p) { store.set('recent', getRecent().filter((x) => x !== p)); renderRecent(); }
function renderRecent() {
  const el = $('#recent');
  el.innerHTML = '';
  const list = isNative ? getRecent().slice(0, 6) : [];
  if (!list.length) return;
  el.innerHTML = '<div class="rtitle">Recent</div>';
  for (const p of list) {
    const it = document.createElement('div');
    it.className = 'ritem';
    it.innerHTML = '<b></b><span></span>';
    it.querySelector('b').textContent = p.split(/[\\/]/).pop();
    it.querySelector('span').textContent = p;
    it.addEventListener('click', () => openPath(p));
    el.appendChild(it);
    NL.filesystem.getStats(p).then((st) => thumbs.lookup(p, st)).then((t) => {
      if (!t) return;
      const img = document.createElement('img');
      img.className = 'rthumb'; img.src = t.url; img.alt = '';
      it.classList.add('has-thumb');
      it.prepend(img);
      refreshGlass($('#empty'));
    }).catch(() => {});
  }
  refreshGlass($('#empty'));
}

// --- drag & drop import: two targets (new tab / add to assembly)
const dropEl = $('#drop');
let dragDepth = 0;
const dropCards = [...document.querySelectorAll('.drop-card')];
function hotCard(e) {
  const el = document.elementFromPoint(e.clientX, e.clientY);
  const card = el && el.closest('.drop-card:not(.disabled)');
  dropCards.forEach((c) => c.classList.toggle('hot', c === card));
  return card;
}
window.addEventListener('dragenter', (e) => {
  if (![...(e.dataTransfer?.types || [])].includes('Files')) return;
  e.preventDefault();
  if (dragDepth++ === 0) {
    const canInsert = !!(active && active.comps.length);
    dropCards[1].classList.toggle('disabled', !canInsert);
    if (canInsert) $('#dropInsertTo').textContent = `Insert into “${active.name}”`;
    dropEl.classList.add('on');
    dropCards.forEach((c) => refreshGlass(c));
  }
});
window.addEventListener('dragleave', () => { if (--dragDepth <= 0) { dragDepth = 0; dropEl.classList.remove('on'); } });
window.addEventListener('dragover', (e) => { e.preventDefault(); hotCard(e); });
window.addEventListener('drop', async (e) => {
  e.preventDefault();
  const card = hotCard(e);
  dragDepth = 0;
  dropEl.classList.remove('on');
  dropCards.forEach((c) => c.classList.remove('hot'));
  const files = [...e.dataTransfer.files];
  const insert = card && card.dataset.target === 'insert';
  for (const f of files) {
    const s = sourceFromFile(f);
    if (insert) await insertPart(s); else openInNewTab(s);
  }
});

// ------------------------------------------------------------------ viewport interaction
const canvas = $('#view');
let downAt = null;
canvas.addEventListener('pointerdown', (e) => {
  downAt = { x: e.clientX, y: e.clientY, b: e.button, gizmo: move.hovering };
  togglePop(null);
});
// pre-highlight what a click would select, with its name next to the cursor
const partTag = document.createElement('div');
partTag.id = 'partTag';
document.body.appendChild(partTag);
let hoverRaf = 0, hoverEv = null, linkedRow = null;
function linkRow(obj) {
  if (linkedRow) linkedRow.classList.remove('hover-link');
  linkedRow = null;
  for (let o = obj; o; o = o.parent) { const r = rowOf.get(o); if (r) { linkedRow = r; r.classList.add('hover-link'); break; } }
}
function partHover() {
  hoverRaf = 0;
  const e = hoverEv;
  if (!e || !showHover || measure.active || move.dragging || downAt || am.active) { viewer.setHover(null); partTag.classList.remove('show'); linkRow(null); return; }
  const hit = viewer.pick(e);
  const obj = hit ? hit.object : null;
  viewer.setHover(obj);
  linkRow(obj);
  if (obj) {
    const comp = compOf(obj);
    const nm = displayName(obj);
    const sub = comp && comp !== obj && displayName(comp) !== nm ? displayName(comp) : '';
    partTag.innerHTML = '<span></span><small></small>';
    partTag.firstChild.textContent = nm;
    partTag.lastChild.textContent = viewer.selected === obj ? 'selected' : sub;
    partTag.style.transform = `translate(${e.clientX + 14}px, ${e.clientY + 16}px)`;
    partTag.classList.add('show');
  } else partTag.classList.remove('show');
}
canvas.addEventListener('pointermove', (e) => {
  if (!downAt) measure.onMove(e);
  if (compareOn && compare.clones.length && !downAt) compareTip(e);
  hoverEv = { clientX: e.clientX, clientY: e.clientY };
  if (!hoverRaf) hoverRaf = requestAnimationFrame(partHover);
});
canvas.addEventListener('pointerleave', () => { measure.onLeave(); hoverEv = null; viewer.setHover(null); partTag.classList.remove('show'); linkRow(null); });
canvas.addEventListener('wheel', () => partTag.classList.remove('show'), { passive: true });
canvas.addEventListener('pointerup', (e) => {
  const d = downAt;
  downAt = null;
  if (!d || d.b !== 0 || d.gizmo || move.dragging) return;
  if (Math.hypot(e.clientX - d.x, e.clientY - d.y) > 4) return;
  if (measure.active) { measure.onClick(e); return; }
  if (e.shiftKey && active && active.section.on) {
    // Shift+click with the cut on: put the cut on that face / axis
    const h = viewer.pick(e);
    const what = h && section.alignToHit(h);
    if (what) { syncCutPanel(); toast(`Cut placed on the ${what}`, false, 1400); }
    return;
  }
  if (am.active && $('#chkAmPlace').checked) {
    const h = viewer.pick(e);
    if (h && h.face) {
      const n = h.face.normal.clone().transformDirection(h.object.matrixWorld);
      am.dir.copy(n.negate().normalize());
      saveAm(); syncAmPanel(); am.apply();
      toast('Face placed on the build plate', false, 1500);
    }
    return;
  }
  const hit = viewer.pick(e);
  viewer.select(hit ? hit.object : null);
});
canvas.addEventListener('dblclick', (e) => {
  if (measure.active) return;
  const hit = viewer.pick(e);
  if (hit) viewer.setPivot(hit.point);
});

// ------------------------------------------------------------------ what is shown on screen
let showHover = true;
for (const [id, key, apply] of [
  ['#chkShowCube', 'showCube', (on) => document.body.classList.toggle('hide-cube', !on)],
  ['#chkShowInfo', 'showInfo', (on) => document.body.classList.toggle('hide-info', !on)],
  ['#chkShowHover', 'showHover', (on) => { showHover = on; if (!on) { viewer.setHover(null); } }],
  ['#chkShowMeas', 'showMeas', (on) => { document.body.classList.toggle('hide-meas', !on); measure.group.visible = on; viewer.requestRender(); }],
  ['#chkShowTabs', 'showTabs', (on) => document.body.classList.toggle('hide-tabs', !on)]
]) {
  const el = $(id);
  el.checked = store.get(key, true);
  apply(el.checked);
  el.addEventListener('change', () => { store.set(key, el.checked); apply(el.checked); });
}

// ------------------------------------------------------------------ edge style
let edgeStyle = store.get('edgeStyle', 'catia');
const segEdgeStyle = $('#segEdgeStyle');
function setEdgeStyle(st) {
  edgeStyle = st;
  store.set('edgeStyle', st);
  viewer.setEdgeStyle(st);
  segSet(segEdgeStyle, segEdgeStyle.querySelector(`[data-es="${st}"]`));
}
segEdgeStyle.querySelectorAll('.btn').forEach((b) => b.addEventListener('click', () => setEdgeStyle(b.dataset.es)));
setEdgeStyle(edgeStyle);
syncUnits();

// ------------------------------------------------------------------ keyboard (customisable)
const nextTab = (dir) => { if (docs.length > 1) { const i = docs.indexOf(active); activate(docs[(i + dir + docs.length) % docs.length]); } };
const keymap = new Keymap([
  { id: 'open', group: 'Files & tabs', label: 'Open file…', key: 'Ctrl+O', run: () => openDialog() },
  { id: 'insert', group: 'Files & tabs', label: 'Insert part into assembly…', key: 'Ctrl+I', run: () => insertDialog() },
  { id: 'closeTab', group: 'Files & tabs', label: 'Close tab', key: 'Ctrl+W', run: () => { if (active) closeDoc(active); } },
  { id: 'nextTab', group: 'Files & tabs', label: 'Next tab', key: 'Ctrl+Tab', run: () => nextTab(1) },
  { id: 'prevTab', group: 'Files & tabs', label: 'Previous tab', key: 'Ctrl+Shift+Tab', run: () => nextTab(-1) },
  { id: 'saveAsm', group: 'Files & tabs', label: 'Save assembly layout…', key: 'Ctrl+S', run: () => saveAssembly() },
  { id: 'fit', group: 'View', label: 'Fit all', short: 'fit', key: 'F', run: () => viewer.fit() },
  { id: 'home', group: 'View', label: 'Home view', short: 'home', key: 'H', run: () => home() },
  { id: 'iso', group: 'View', label: 'Isometric view', short: 'iso', key: '0', run: () => setView('iso') },
  { id: 'viewXY', group: 'View', label: 'Look at XY plane', short: 'XY', key: '7', run: () => setView('xy') },
  { id: 'viewYZ', group: 'View', label: 'Look at YZ plane', short: 'YZ', key: '8', run: () => setView('yz') },
  { id: 'viewZX', group: 'View', label: 'Look at ZX plane', short: 'ZX', key: '9', run: () => setView('zx') },
  { id: 'proj', group: 'View', label: 'Toggle orthographic / perspective', short: 'ortho / persp', key: 'P', run: () => btnProj.click() },
  { id: 'shaded', group: 'Display', label: 'Shaded', short: 'shaded', key: '1', run: () => setMode('shaded') },
  { id: 'edges', group: 'Display', label: 'Shaded with edges', short: 'edges', key: '2', run: () => setMode('edges') },
  { id: 'wire', group: 'Display', label: 'Wireframe', short: 'wire', key: '3', run: () => setMode('wire') },
  { id: 'mesh', group: 'Display', label: 'Show tessellation', short: 'mesh', key: '4', run: () => setMode('mesh') },
  { id: 'xray', group: 'Display', label: 'X-ray', short: 'x-ray', key: 'X', run: () => setXray(!viewer.xray) },
  { id: 'edgeStyle', group: 'Display', label: 'Edges: uniform ↔ sharp / tangent', short: 'edge style', key: 'E', run: () => setEdgeStyle(edgeStyle === 'catia' ? 'uniform' : 'catia') },
  { id: 'bg', group: 'Display', label: 'Light / dark', short: 'light/dark', key: 'B', run: () => cycleBg() },
  { id: 'hidePart', group: 'Parts', label: 'Hide selected / pointed-at part', short: 'hide', key: 'Space', run: () => hidePart() },
  { id: 'showHidden', group: 'Parts', label: 'Show / hide again the hidden parts', short: 'show hidden', key: 'Shift+Space', run: () => toggleHidden() },
  { id: 'showAll', group: 'Parts', label: 'Show all parts', key: '', run: () => showAll() },
  { id: 'panels', group: 'Display', label: 'Hide / show panels', short: 'panels', key: 'U', run: () => document.body.classList.toggle('ui-hidden') },
  { id: 'tree', group: 'Display', label: 'Model tree', short: 'tree', key: 'T', run: () => $('#btnTree').click() },
  { id: 'measure', group: 'Tools', label: 'Measure', short: 'measure', key: 'M', run: () => setMeasure(!measure.active) },
  { id: 'cut', group: 'Tools', label: 'Section cut', short: 'cut', key: 'C', run: () => setCut(!(active && active.section.on)) },
  { id: 'move', group: 'Tools', label: 'Move / rotate parts', short: 'move', key: 'G', run: () => setMove(!move.active) },
  { id: 'colors', group: 'Tools', label: 'Part colours', short: 'colours', key: 'K', run: () => { togglePop('color'); syncColorPop(); } },
  { id: 'mass', group: 'Tools', label: 'Mass properties', short: 'mass', key: 'W', run: () => setMass(!massOn) },
  { id: 'am', group: 'Tools', label: 'AM check', short: 'AM check', key: 'A', run: () => setAM(!am.active) },
  { id: 'explode', group: 'Tools', label: 'Exploded view', short: 'explode', key: 'V', run: () => setExplode(!explodeOn) },
  { id: 'compare', group: 'Tools', label: 'Compare with another version…', key: 'Ctrl+Shift+C', run: () => (compareOn ? setCompare(false) : setCompare(true)) },
  { id: 'pmi', group: 'Tools', label: 'PMI – dimensions & tolerances', short: 'PMI', key: 'D', run: () => setPmi(!pmi.active) },
  { id: 'png', group: 'Files & tabs', label: 'Export view as PNG…', key: 'Ctrl+Shift+E', run: () => exportPng() },
  { id: 'units', group: 'Tools', label: 'Switch mm ↔ inch', short: 'mm / inch', key: 'I', run: () => setUnits({ length: getUnits().length === 'mm' ? 'in' : 'mm' }) },
  { id: 'settings', group: 'Tools', label: 'Settings', short: 'settings', key: ',', run: () => togglePop('set') },
  { id: 'resetPart', group: 'Tools', label: 'Reset moved part', key: 'Delete', run: () => { if (move.active && move.target) move.reset(); } },
  { id: 'escape', group: 'Tools', label: 'Cancel / deselect', key: 'Escape', fixed: true, run: () => {
    if (measure.active && measure.pending) measure.cancel();
    else if (viewer.selected) viewer.select(null);
    else if (measure.active) setMeasure(false);
    else if (move.active) setMove(false);
    togglePop(null);
    closeCtx();
  } }
]);
window.addEventListener('keydown', (e) => keymap.handle(e));
const MOUSE_KEYS = [['Left drag', 'rotate'], ['Right/Middle', 'pan'], ['Wheel', 'zoom to cursor'], ['Dbl-click', 'set pivot'], ['Shift / Alt', 'measure: lock face'], ['Ctrl', 'measure: free point'], ['Shift+click', 'cut on face'], ['Right-click', 'part menu']];
// tooltips mention the current key
const TIP_KEYS = { btnMeasure: 'measure', btnCut: 'cut', btnMove: 'move', btnMass: 'mass', btnAM: 'am', btnFit: 'fit', btnProj: 'proj', btnXray: 'xray', btnTree: 'tree' };
function syncKeyUi() {
  renderKeyGrid($('#keysGrid'), keymap, MOUSE_KEYS);
  for (const [id, act] of Object.entries(TIP_KEYS)) {
    const el = document.getElementById(id);
    if (!el || !el.dataset.tip) continue;
    const k = keymap.keyOf(act);
    el.dataset.tip = el.dataset.tip.replace(/\s*\([^()]*\)$/, '') + (k ? ` (${prettyCombo(k)})` : '');
  }
}
keymap.addEventListener('change', syncKeyUi);
syncKeyUi();
window.gvSyncKeyTips = syncKeyUi;
window.gv.keymap = keymap;
$('#btnKeys').addEventListener('click', () => { togglePop(null); openKeySheet(keymap, { toast, refreshGlass }); });

// ------------------------------------------------------------------ mass properties
const MATERIALS = [
  ['Aluminium 6061', 2.70], ['Aluminium 7075', 2.81], ['AlSi10Mg (AM)', 2.67], ['Steel', 7.85], ['Stainless 316L', 8.00],
  ['Titanium Ti-6Al-4V', 4.43], ['Inconel 718', 8.19], ['Copper', 8.96], ['CFRP (quasi-iso)', 1.55], ['GFRP', 1.90],
  ['PA12', 1.01], ['PA12-CF', 1.15], ['PLA', 1.24], ['PETG', 1.27], ['ABS', 1.04], ['PEEK', 1.30], ['Custom', null]
];
let massJob = 0;
let lastMass = null;
const cogMarker = new THREE.Group();
{
  const m = new THREE.Mesh(new THREE.SphereGeometry(1, 20, 14), new THREE.MeshBasicMaterial({ color: 0xff9f0a, depthTest: false, transparent: true, toneMapped: false }));
  const ring = new THREE.Mesh(new THREE.TorusGeometry(1.8, 0.18, 8, 40), new THREE.MeshBasicMaterial({ color: 0xffffff, depthTest: false, transparent: true, opacity: 0.9, toneMapped: false }));
  m.renderOrder = ring.renderOrder = 1001;
  cogMarker.add(ring, m);
  cogMarker.traverse((o) => { o.raycast = () => {}; });
  cogMarker.visible = false;
  viewer.overlay.add(cogMarker);
  viewer.renderHooks.push((cam) => {
    if (!cogMarker.visible) return;
    const d = cam.isPerspectiveCamera ? cam.position.distanceTo(cogMarker.position) * Math.tan(THREE.MathUtils.degToRad(cam.fov) / 2) * 2 / viewer.canvas.clientHeight : (cam.top - cam.bottom) / cam.zoom / viewer.canvas.clientHeight;
    const s = d * 5;
    if (Math.abs(cogMarker.scale.x - s) / s > 0.02) { cogMarker.scale.setScalar(s); viewer.requestRender(); }
    cogMarker.quaternion.copy(cam.quaternion);
  });
}
let densityGcm3 = 2.70; // always stored in g/cm³; the field shows the chosen unit
function syncDensityField() {
  const u = densityUnit();
  const sel = $('#matSelect');
  sel.innerHTML = '';
  for (const [name, rho] of MATERIALS) {
    const o = document.createElement('option');
    o.value = name; o.textContent = rho ? `${name} · ${fDensity(rho)}` : name;
    sel.appendChild(o);
  }
  sel.value = curMaterial;
  const f = $('#density');
  f.step = u.dec ? (10 ** -Math.min(u.dec, 4)).toString() : '1';
  f.value = +toDensityUnit(densityGcm3).toFixed(u.dec);
  $('#densityUnit').textContent = u.label;
}
let curMaterial = 'Aluminium 6061';
{
  const saved = store.get('material', ['Aluminium 6061', 2.70]);
  curMaterial = saved[0]; densityGcm3 = saved[1];
  syncDensityField();
}
$('#matSelect').addEventListener('change', () => {
  curMaterial = $('#matSelect').value;
  const m = MATERIALS.find((x) => x[0] === curMaterial);
  if (m && m[1]) densityGcm3 = m[1];
  syncDensityField();
  store.set('material', [curMaterial, densityGcm3]);
  computeMass();
});
$('#density').addEventListener('change', () => {
  const v = fromDensityUnit(+$('#density').value);
  if (!(v > 0)) { syncDensityField(); return; }
  densityGcm3 = v;
  const m = MATERIALS.find((x) => x[1] && Math.abs(x[1] - v) < 1e-6);
  curMaterial = m ? m[0] : 'Custom';
  $('#matSelect').value = curMaterial;
  store.set('material', [curMaterial, v]);
  computeMass();
});

function analysisTarget() { return viewer.selected || (active ? active.root : null); }
function targetLabel(t) { return !t ? '—' : t === active?.root ? `Whole model · ${active.name}` : displayName(t); }
function meshesOf(obj) { const l = []; if (obj) obj.traverseVisible((o) => { if (isModelMesh(o)) l.push(o); }); return l; }
function compOf(o) { for (let p = o; p; p = p.parent) if (p.userData.component) return p; return null; }

/** Engine request items for meshes that come from the native engine (exact B-rep), or null. */
function kernelItems(meshes) {
  if (!kernelAvailable() || !meshes.length) return null;
  const paths = [], pathIdx = [], mesh = [], mat = [];
  for (const m of meshes) {
    const comp = compOf(m);
    if (!m.userData.native || !comp || !comp.userData.component.native || !comp.userData.component.kernelPath) return null;
    let pi = paths.indexOf(comp.userData.component.kernelPath);
    if (pi < 0) { paths.push(comp.userData.component.kernelPath); pi = paths.length - 1; }
    m.updateMatrixWorld(true);
    pathIdx.push(pi); mesh.push(m.userData.native.index); mat.push(...m.matrixWorld.elements);
  }
  return { paths, pathIdx, mesh, mat };
}

const f1 = (v, d = 3) => (Math.abs(v) < 10 ** -d / 2 ? (0).toFixed(d) : v.toFixed(d));
const fmtMass = (g) => (g >= 1000 ? `${f1(g / 1000, 4)} kg` : `${f1(g, 2)} g`);
async function computeMass() {
  if (!massOn || !active) return;
  const job = ++massJob;
  const target = analysisTarget();
  $('#massTarget').textContent = targetLabel(target);
  const meshes = meshesOf(target);
  const rho = densityGcm3 / 1000; // g/mm³
  let r = null;
  const items = kernelItems(meshes);
  try {
    if (items) r = await kernelCall({ cmd: 'mass', ...items, density: items.mesh.map(() => rho) });
  } catch (e) { console.warn('engine mass failed', e); r = null; }
  if (!r) r = meshMassProps(meshes, () => rho);
  if (job !== massJob) return;
  lastMass = { ...r, target: targetLabel(target), rho: rho * 1000, material: $('#matSelect').value };
  const badge = $('#massBadge');
  badge.textContent = r.exact ? 'Exact · B-rep' : 'From mesh';
  badge.classList.toggle('exact', !!r.exact);
  const kI = inertiaFactor(), L = (x) => fLen(x, false, getUnits().length === 'in' ? 4 : 2);
  const I = r.inertia.map((x) => x * kI);
  const warn = r.exact && r.openShells && !r.solids ? '<span class="full" style="color:#ff9f0a">Surface model – no closed solids, volume/mass are 0.</span>' : '';
  $('#massResults').innerHTML = `
    <span>Density</span><b>${fDensity(densityGcm3)}</b>
    <span>Volume</span><b>${fVol(r.volume)}</b>
    <span>Surface area</span><b>${fArea(r.area)}</b>
    <span>Mass</span><b>${fMass(r.mass)}</b>
    <span>Centre of gravity</span><b>${L(r.cog[0])}, ${L(r.cog[1])}, ${L(r.cog[2])} ${getUnits().length}</b>
    <span class="full">Inertia about CoG (${inertiaUnit()})</span>
    <div class="full tensor">${I.map((x) => `<span>${f1(x, 3)}</span>`).join('')}</div>
    <span>Principal moments</span><b>${r.principal.map((x) => f1(x * kI, 2)).join(' · ')}</b>${warn}`;
  cogMarker.position.set(...r.cog);
  cogMarker.visible = r.mass > 0;
  viewer.requestRender();
}
function setMass(on) {
  massOn = on;
  cogMarker.visible = false;
  syncToolPanels();
  if (on) computeMass(); else viewer.requestRender();
}
tools.mass.addEventListener('click', () => setMass(!massOn));
$('#btnCopyMass').addEventListener('click', async () => {
  const r = lastMass;
  if (!r) return;
  const kI = inertiaFactor(), iu = inertiaUnit(), imp = getUnits().length === 'in';
  const I = r.inertia.map((x) => (x * kI).toFixed(4));
  const txt = [`${r.target} — ${r.material} (${fDensity(r.rho)})${r.exact ? ' — exact B-rep' : ' — from mesh'}`,
    `Volume\t${fVol(r.volume)}`, `Surface area\t${fArea(r.area)}`, `Mass\t${fMass(r.mass)}`,
    `CoG\t${r.cog.map((x) => fLen(x, false)).join('\t')}\t${imp ? 'in' : 'mm'}`, `Ixx Ixy Ixz\t${I.slice(0, 3).join('\t')}\t${iu}`, `Iyx Iyy Iyz\t${I.slice(3, 6).join('\t')}`, `Izx Izy Izz\t${I.slice(6, 9).join('\t')}`,
    `Principal\t${r.principal.map((x) => (x * kI).toFixed(4)).join('\t')}\t${iu}`].join('\n');
  try { await navigator.clipboard.writeText(txt); toast('Copied to clipboard', false, 1400); }
  catch (e) { if (isNative) { await NL.clipboard.writeText(txt); toast('Copied to clipboard', false, 1400); } }
});

// ------------------------------------------------------------------ AM check
{
  const st = store.get('am', { dir: '0,0,1', map: 'overhang', limit: 45, wall: 1.0 });
  am.dir.set(...st.dir.split(',').map(Number));
  am.map = st.map; am.limit = st.limit; am.minWall = st.wall;
  $('#amLimit').value = st.limit; $('#amWall').value = st.wall;
}
const saveAm = () => store.set('am', { dir: am.dir.toArray().map((x) => Math.round(x * 1e4) / 1e4).join(','), map: am.map, limit: am.limit, wall: am.minWall });
function syncAmPanel() {
  const key = am.dir.toArray().map((x) => Math.round(x)).join(',');
  const exact = am.dir.toArray().every((x) => Math.abs(x - Math.round(x)) < 1e-6);
  segSet($('#segBuild'), exact ? $('#segBuild').querySelector(`[data-bd="${key}"]`) : null);
  segSet($('#segAmMap'), $('#segAmMap').querySelector(`[data-am="${am.map}"]`));
  $('#amLimitVal').textContent = am.limit + '°';
  const ls = $('#amLimit'); ls.style.setProperty('--p', ((am.limit - 20) / 50) * 100 + '%');
  $('#amLimitRow').classList.toggle('hidden', am.map !== 'overhang');
  $('#amWallRow').classList.toggle('hidden', am.map !== 'thickness');
  $('#amLegend').innerHTML = am.map === 'overhang'
    ? `<span><i style="background:#ed3833"></i>needs support (&lt;${am.limit}°)</span><span><i style="background:#ffbf26"></i>borderline</span><span><i style="background:#4089f2"></i>on build plate</span>`
    : `<span><i style="background:#ed3833"></i>&lt; ${fLen(am.minWall, true, 2)}</span><span><i style="background:#ffbf26"></i>&lt; ${fLen(2 * am.minWall, true, 2)}</span><span><i style="background:#40c773"></i>≥ ${fLen(3 * am.minWall, true, 2)}</span>`;
  const wf = $('#amWall');
  if (document.activeElement !== wf) wf.value = +fLen(am.minWall, false, getUnits().length === 'in' ? 3 : 2);
  $('#amWallUnit').textContent = getUnits().length === 'in' ? 'in wall' : 'mm wall';
}
am.addEventListener('stats', () => {
  const s = am.stats;
  if (!s) return;
  $('#amStats').innerHTML = `
    <span>Build height</span><b>${fLen(s.height, true, 2)}</b>
    <span>Needs support</span><b>${fArea(s.overhangArea)}</b>
    <span>Support footprint</span><b>${fArea(s.support)} (projected)</b>
    <span>On build plate</span><b>${fArea(s.plateArea)}</b>
    ${s.thickness ? `<span>Thinnest wall</span><b>${isFinite(s.thickness.min) ? fLen(s.thickness.min, true, 2) : '—'}</b><span>Below minimum</span><b>${(s.thickness.thinShare * 100).toFixed(1)} % of surface points</b>` : ''}`;
});
am.addEventListener('progress', (e) => {
  const p = $('#amProgress');
  p.classList.toggle('hidden', e.detail >= 1);
  p.firstElementChild.style.width = (e.detail * 100).toFixed(0) + '%';
});
function setAM(on) {
  if (on && active) { am.setTargets(analysisTarget()); $('#amTarget').textContent = targetLabel(analysisTarget()); }
  am.setActive(on && !!active);
  $('#orientList').innerHTML = '';
  syncToolPanels();
  syncAmPanel();
}
tools.am.addEventListener('click', () => setAM(!am.active));
$('#segBuild').querySelectorAll('.btn').forEach((b) => b.addEventListener('click', () => { am.dir.set(...b.dataset.bd.split(',').map(Number)); saveAm(); syncAmPanel(); am.apply(); }));
$('#segAmMap').querySelectorAll('.btn').forEach((b) => b.addEventListener('click', () => { am.map = b.dataset.am; saveAm(); syncAmPanel(); am.apply(); }));
$('#amLimit').addEventListener('input', (e) => { am.limit = +e.target.value; syncAmPanel(); clearTimeout(am._t); am._t = setTimeout(() => { saveAm(); am.apply(); }, 120); });
$('#amWall').addEventListener('change', (e) => { am.minWall = Math.max(0.01, (+e.target.value || 1) * (getUnits().length === 'in' ? 25.4 : 1)); saveAm(); syncAmPanel(); am.apply(); });
$('#btnAutoOrient').addEventListener('click', () => {
  toast('Evaluating build orientations…');
  setTimeout(() => {
    const res = am.autoOrient();
    hideToast();
    const el = $('#orientList');
    el.innerHTML = '';
    const name = (d) => {
      const a = d.toArray(), ax = ['X', 'Y', 'Z'];
      const i = a.map(Math.abs).indexOf(Math.max(...a.map(Math.abs)));
      const lbl = (a[i] > 0 ? '+' : '−') + ax[i];
      const tilt = THREE.MathUtils.radToDeg(Math.acos(Math.min(1, Math.abs(a[i]))));
      return tilt < 0.5 ? lbl : `${lbl} ∠${tilt.toFixed(0)}°`;
    };
    res.forEach((r, k) => {
      const row = document.createElement('div');
      row.className = 'orow';
      row.title = r.dir.toArray().map((x) => x.toFixed(3)).join(', ');
      row.innerHTML = `<b>${k === 0 ? '★ ' : ''}${name(r.dir)}</b><span>${fArea(r.support, 1)} support · ${fLen(r.height, true, 0)} high</span>`;
      row.addEventListener('click', () => { am.dir.copy(r.dir); saveAm(); syncAmPanel(); am.apply(); });
      el.appendChild(row);
    });
    if (res[0]) { am.dir.copy(res[0].dir); saveAm(); syncAmPanel(); am.apply(); }
  }, 30);
});

// ------------------------------------------------------------------ exploded view
function setExplode(on) {
  explodeOn = on && !!active;
  syncToolPanels();
}
tools.explode.addEventListener('click', () => setExplode(!explodeOn));
$('#explodeAmt').addEventListener('input', (e) => {
  if (!active) return;
  const f = +e.target.value / 100;
  $('#explodeVal').textContent = `${e.target.value} %`;
  explode.apply(active.root, f);
  section.refresh(); renderInfo();
  if (move.active && move.target) move.attach(move.target);
});
$('#btnCollapse').addEventListener('click', () => {
  explode.reset(); $('#explodeAmt').value = 0; $('#explodeVal').textContent = '0 %';
  section.refresh(); renderInfo();
});

// ------------------------------------------------------------------ compare versions
const cmpUnit = () => (getUnits().length === 'in' ? 25.4 : 1);
function syncComparePanel() {
  const u = getUnits().length === 'in' ? 'in' : 'mm';
  document.querySelectorAll('.cmpUnit').forEach((e) => { e.textContent = u; });
  $('#cmpTol').value = +(compare.tol / cmpUnit()).toPrecision(4);
  $('#cmpRange').value = +((compare.range || 1) / cmpUnit()).toPrecision(4);
  $('#cmpBar').style.background = compare.legendCss();
  $('#cmpLo').textContent = `−${fLen(compare.range || 1, true, 2)} removed`;
  $('#cmpHi').textContent = `added +${fLen(compare.range || 1, true, 2)}`;
  $('#cmpWhat').textContent = compare.otherName ? `This model vs. ${compare.otherName}` : 'Pick the other version of this model';
  const st = compare.stats;
  $('#cmpStats').innerHTML = st ? `<span>Largest outward</span><b>+${fLen(compare.maxPos, true, 3)}</b><span>Largest inward</span><b>−${fLen(Math.abs(compare.maxNeg), true, 3)}</b>` +
    `<span>Mean |deviation|</span><b>${fLen(st.mean, true, 3)}</b><span>Within tolerance</span><b>${(st.within * 100).toFixed(1)} %</b>` : '';
}
async function pickCompare(given = null) {
  if (!active) return;
  const src = given || (await pickFiles(false, 'Compare with', 'Compare'))[0];
  if (!src) return;
  $('#cmpProgress').classList.remove('hidden');
  toast(`Loading ${src.name} for comparison…`);
  try {
    const model = await loadSource(src, { quality: quality === 'coarse' ? 'normal' : quality });
    if (!compareOn) return;
    compare.range = 0;
    compare.run(active.root, model.root, src.name);
    hideToast();
  } catch (e) { toast(`Could not compare: ${e.message || e}`, true, 5000); }
  $('#cmpProgress').classList.add('hidden');
  syncComparePanel();
}
function setCompare(on) {
  compareOn = on && !!active;
  if (!compareOn) compare.clear();
  syncToolPanels();
  syncComparePanel();
  if (compareOn && !compare.otherName) pickCompare();
}
tools.compare.addEventListener('click', () => setCompare(!compareOn));
$('#btnCmpPick').addEventListener('click', () => { compare.clear(); compareOn = true; pickCompare(); });
$('#cmpTol').addEventListener('change', (e) => { const v = +e.target.value; if (v > 0) { compare.tol = v * cmpUnit(); compare.recolour(); syncComparePanel(); } });
$('#cmpRange').addEventListener('change', (e) => { const v = +e.target.value; if (v > 0) { compare.range = v * cmpUnit(); compare.recolour(); syncComparePanel(); } });
$('#chkCmpGhost').addEventListener('change', (e) => compare.setGhost(e.target.checked));
function compareTip(e) {
  const d = compare.probe(viewer.pointerNDC(e));
  const tip = compare.tip;
  if (d === null) { tip.style.display = 'none'; return; }
  tip.textContent = Number.isFinite(d) ? `${d >= 0 ? '+' : '−'}${fLen(Math.abs(d), true, getUnits().length === 'in' ? 4 : 3)}` : 'no counterpart';
  tip.style.setProperty('--k', '#' + compare.colorFor(d).getHexString());
  tip.style.left = `${e.clientX + 16}px`;
  tip.style.top = `${e.clientY - 34}px`;
  tip.style.display = 'block';
}
canvas.addEventListener('pointerleave', () => { compare.tip.style.display = 'none'; });

// ------------------------------------------------------------------ PMI
function pmiComps() {
  const out = [];
  if (!active) return out;
  for (const comp of active.comps) {
    const info = comp.userData.component;
    if (!info || !info.native || !info.kernelPath) continue;
    const meshes = new Map();
    comp.traverse((m) => { if (isModelMesh(m) && m.userData.native) meshes.set(m.userData.native.index, m); });
    out.push({ comp, path: info.kernelPath, meshes });
  }
  return out;
}
async function setPmi(on) {
  if (!on || !active) { pmi.setActive(false); syncToolPanels(); return; }
  const comps = pmiComps();
  pmi.setActive(true);
  syncToolPanels();
  $('#pmiList').innerHTML = '';
  $('#pmiChips').innerHTML = '';
  if (!comps.length) { $('#pmiHint').textContent = isNative ? 'PMI needs a STEP file opened with the native engine (gvkernel.exe).' : 'PMI needs the Windows app with the native engine.'; return; }
  $('#pmiHint').textContent = 'Reading PMI…';
  try {
    const n = await pmi.load(comps);
    if (!pmi.active) { pmi.clear(); return; }
    $('#pmiHint').textContent = n ? 'Hover an entry to highlight its faces, click to keep it highlighted.' : 'No PMI in this file. GlassView shows dimensions, GD&T and datums from STEP AP242 files (CATIA, NX, Creo … export with PMI / 3D annotations).';
    pmi.renderList($('#pmiList'), $('#pmiChips'));
  } catch (e) { $('#pmiHint').textContent = `Could not read PMI: ${e.message || e}`; }
}
tools.pmi.addEventListener('click', () => setPmi(!pmi.active));

// ------------------------------------------------------------------ AM: 3MF in the build orientation
$('#btnAm3mf').addEventListener('click', () => {
  if (!active) return;
  export3mf(analysisTarget(), { extra: buildMatrix(am.dir.clone()), drop: true, suffix: '_print' });
});

// ------------------------------------------------------------------ Windows integration settings
{
  const chkSingle = $('#chkSingle');
  chkSingle.checked = store.get('singleInstance', true);
  chkSingle.addEventListener('change', () => { store.set('singleInstance', chkSingle.checked); writeViewIni(); });
  const segPng = $('#segPng');
  const pngSel = () => segSet(segPng, segPng.querySelector(`[data-png="${store.get('pngScale', 2)}"]`));
  segPng.querySelectorAll('.btn').forEach((b) => b.addEventListener('click', () => { store.set('pngScale', +b.dataset.png); pngSel(); }));
  requestAnimationFrame(pngSel);
  const chkPv = $('#chkPreview');
  (async () => {
    const present = await previewDllPresent();
    chkPv.disabled = !present;
    $('#previewNote').textContent = present ? 'Select a STEP / STL file in Explorer with the preview pane open (Alt+P).' : (isNative ? 'GlassViewPreview.dll is missing next to GlassView.exe.' : 'Windows app only.');
    chkPv.checked = present && await previewRegistered();
    if (present && await previewMachineWide()) {
      chkPv.disabled = true;
      $('#previewNote').textContent = 'Set up for all users of this PC. To change it, run the GlassView setup again (Settings > Apps > GlassView > Modify).';
    }
  })();
  chkPv.addEventListener('change', async () => {
    const ok = await registerPreview(chkPv.checked).catch(() => false);
    if (!ok) { toast('Could not change the Explorer preview', true, 4000); chkPv.checked = !chkPv.checked; return; }
    toast(chkPv.checked ? 'Explorer preview on – select a STEP / STL file with the preview pane open (Alt+P)' : 'Explorer preview off', false, 4000);
  });
  // updates
  $('#verInfo').textContent = `· GlassView ${APP_VERSION}`;
  const repo = $('#updRepo');
  const DEFAULT_REPO = typeof __UPDATE_REPO__ !== 'undefined' ? __UPDATE_REPO__ : '';
  repo.value = store.get('updateRepo', DEFAULT_REPO);
  repo.addEventListener('change', () => store.set('updateRepo', repo.value.trim()));
  const chkAuto = $('#chkAutoUpd');
  chkAuto.checked = store.get('autoUpdate', true);
  chkAuto.addEventListener('change', () => store.set('autoUpdate', chkAuto.checked));
  const btn = $('#btnCheckUpd');
  async function check(silent) {
    const r = store.get('updateRepo', DEFAULT_REPO);
    if (!r) { if (!silent) toast('Enter the GitHub repository (owner/repo) first', true, 3000); return; }
    btn.textContent = 'Checking…';
    try {
      const u = await checkUpdate(r);
      store.set('updateLast', Date.now());
      if (u.newer) {
        btn.textContent = `Download ${u.latest}…`;
        btn.onclick = () => openUrl(u.url);
        toast(`Update available: ${u.latest} – see Settings`, false, 8000);
        return;
      }
      if (!silent) toast(`GlassView ${APP_VERSION} is up to date`, false, 3000);
    } catch (e) { if (!silent) toast(`Update check failed: ${e.message || e}`, true, 4000); }
    btn.textContent = 'Check now';
  }
  btn.addEventListener('click', () => { if (!btn.onclick) check(false); });
  if (isNative && store.get('autoUpdate', true) && store.get('updateRepo', DEFAULT_REPO) && Date.now() - store.get('updateLast', 0) > 7 * 864e5) setTimeout(() => check(true), 5000);
}

function onAnalysisTargetChange() {
  if (massOn) computeMass();
  if (am.active && active) { am.setTargets(analysisTarget()); $('#amTarget').textContent = targetLabel(analysisTarget()); }
}

// ------------------------------------------------------------------ tree context menu & export
const ctx = $('#ctxMenu');
function closeCtx() { ctx.classList.remove('open'); }
document.addEventListener('pointerdown', (e) => { if (!e.target.closest('#ctxMenu')) closeCtx(); });
window.addEventListener('blur', closeCtx);

function stlFromMeshes(meshes) {
  let n = 0;
  for (const m of meshes) n += m.geometry.index ? m.geometry.index.count / 3 : m.geometry.attributes.position.count / 3;
  const buf = new ArrayBuffer(84 + n * 50), dv = new DataView(buf);
  const head = 'GlassView STL export';
  for (let i = 0; i < head.length; i++) dv.setUint8(i, head.charCodeAt(i));
  dv.setUint32(80, n, true);
  let o = 84;
  const a = new THREE.Vector3(), b = new THREE.Vector3(), c = new THREE.Vector3();
  for (const m of meshes) {
    const g = m.geometry, P = g.attributes.position, I = g.index ? g.index.array : null;
    const nT = I ? I.length / 3 : P.count / 3;
    for (let t = 0; t < nT; t++) {
      a.fromBufferAttribute(P, I ? I[t * 3] : t * 3).applyMatrix4(m.matrixWorld);
      b.fromBufferAttribute(P, I ? I[t * 3 + 1] : t * 3 + 1).applyMatrix4(m.matrixWorld);
      c.fromBufferAttribute(P, I ? I[t * 3 + 2] : t * 3 + 2).applyMatrix4(m.matrixWorld);
      const nn = b.clone().sub(a).cross(c.clone().sub(a)).normalize();
      for (const v of [nn, a, b, c]) { dv.setFloat32(o, v.x, true); dv.setFloat32(o + 4, v.y, true); dv.setFloat32(o + 8, v.z, true); o += 12; }
      dv.setUint16(o, 0, true); o += 2;
    }
  }
  return buf;
}

async function writeFileChunked(path, buf) {
  const CH = 8 * 1024 * 1024;
  for (let o = 0; o < buf.byteLength || o === 0; o += CH) {
    const part = buf.slice(o, Math.min(buf.byteLength, o + CH));
    if (o === 0) await NL.filesystem.writeBinaryFile(path, part); else await NL.filesystem.appendBinaryFile(path, part);
    if (buf.byteLength === 0) break;
  }
}

/** 3MF: current positions (moves, explode) and colours; `extra`/`drop` for the AM build orientation. */
async function export3mf(obj, { extra = null, drop = false, suffix = '' } = {}) {
  const meshes = meshesOf(obj);
  if (!meshes.length) return;
  const base = displayName(obj).replace(/[\\/:*?"<>|]+/g, '_') + suffix;
  let bytes;
  try { bytes = build3mf(meshes, { colorOf: (m) => new THREE.Color(colorOf(m)), extra, drop, title: base }); }
  catch (e) { toast(`3MF export failed: ${e.message || e}`, true, 4000); return; }
  if (!isNative) {
    const a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob([bytes], { type: 'model/3mf' }));
    a.download = base + '.3mf';
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 4000);
    return;
  }
  let path;
  try { path = await NL.os.showSaveDialog('Export 3MF', { defaultPath: `${base}.3mf`, filters: [{ name: '3MF (3D printing)', extensions: ['3mf'] }] }); } catch (e) { return; }
  if (!path) return;
  if (!/\.3mf$/i.test(path)) path += '.3mf';
  try { await writeFileChunked(path, bytes.buffer); toast(`Exported ${path.split(/[\\/]/).pop()}`, false, 2500); }
  catch (e) { toast(`Export failed: ${e.message || e}`, true, 5000); }
}
window.gv.export3mf = export3mf;
Object.assign(window.gv, { compareWith: async (p) => { compareOn = true; syncToolPanels(); await pickCompare(await sourceFromPath(p)); }, explode, compare, pmi, build3mf, buildMatrix, renderPng, setExplode, setCompare, setPmi, am: () => am, meshesOf, colorOf });

async function exportPng() {
  if (!active) return;
  const factor = store.get('pngScale', 2);
  let bytes;
  try { bytes = await renderPng(viewer, factor); } catch (e) { toast(`Could not render the image: ${e.message || e}`, true, 4000); return; }
  const base = active.name.replace(/\.[^.]+$/, '');
  if (!isNative) {
    const a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob([bytes], { type: 'image/png' }));
    a.download = base + '.png';
    a.click();
    return;
  }
  let path;
  try { path = await NL.os.showSaveDialog('Export view as PNG', { defaultPath: `${base}.png`, filters: [{ name: 'PNG image', extensions: ['png'] }] }); } catch (e) { return; }
  if (!path) return;
  if (!/\.png$/i.test(path)) path += '.png';
  try { await writeFileChunked(path, bytes.buffer); toast(`Saved ${path.split(/[\\/]/).pop()}`, false, 2500); }
  catch (e) { toast(`Could not save: ${e.message || e}`, true, 4000); }
}

async function exportNode(obj, fmt, forcePath = null) {
  if (fmt === '3mf') return export3mf(obj);
  const meshes = meshesOf(obj);
  if (!meshes.length) return;
  const base = displayName(obj).replace(/[\\/:*?"<>|]+/g, '_');
  const items = kernelItems(meshes);
  if (fmt === 'step' && !items) { toast('STEP export needs CAD parts opened with the native engine', true, 4000); return; }
  if (!isNative) {
    if (fmt !== 'stl') return;
    const a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob([stlFromMeshes(meshes)], { type: 'model/stl' }));
    a.download = base + '.stl';
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 4000);
    return;
  }
  let path = forcePath;
  if (!path) try {
    path = await NL.os.showSaveDialog(`Export ${fmt.toUpperCase()}`, {
      defaultPath: `${base}.${fmt === 'step' ? 'step' : 'stl'}`,
      filters: fmt === 'step' ? [{ name: 'STEP', extensions: ['step', 'stp'] }] : [{ name: 'STL (binary)', extensions: ['stl'] }]
    });
  } catch (e) { return; }
  if (!path) return;
  if (!/\.(step|stp|stl)$/i.test(path)) path += fmt === 'step' ? '.step' : '.stl';
  toast(`Exporting ${base}…`);
  try {
    if (items) {
      const names = meshes.map((m) => (m.name && m.name !== 'Body' ? m.name : compOf(m)?.name || base));
      const colors = [];
      for (const m of meshes) {
        const changed = colorMode !== 'file' || [m, ...(() => { const a = []; for (let p = m.parent; p; p = p.parent) a.push(p); return a; })()].some((o) => o.userData.colorOverride != null);
        if (changed) { const c = new THREE.Color(colorOf(m)); colors.push(c.r, c.g, c.b); } else colors.push(-1, -1, -1);
      }
      const box = viewer.boxOf(obj).getSize(new THREE.Vector3());
      const lin = ((box.x + box.y + box.z) / 3) * 0.0002;
      await kernelCall({ cmd: 'export', format: fmt, out: path, ...items, names, colors, lin, ang: 0.1 });
    } else {
      await writeFileChunked(path, stlFromMeshes(meshes));
    }
    toast(`Exported ${path.split(/[\\/]/).pop()}`, false, 2500);
  } catch (e) {
    toast(`Export failed: ${e.message || e}`, true, 5000);
  }
}

window.gv.exportNode = exportNode;
function isolate(obj) {
  if (!active) return;
  const anc = new Set();
  for (let p = obj; p; p = p.parent) anc.add(p);
  active.root.traverse((o) => {
    if (o === active.root || o.userData.isOverlay || !(o.isGroup || o.isMesh)) return;
    o.visible = anc.has(o) || isInside(o, obj);
  });
  active.revealed = null;
  syncShowHidden();
  buildTree(); viewer.updateBounds(); viewer.requestRender();
}
function showAll() {
  if (!active) return;
  active.root.traverse((o) => { if (!o.userData.isOverlay && (o.isGroup || o.isMesh)) o.visible = true; });
  active.revealed = null;
  syncShowHidden();
  buildTree(); viewer.updateBounds(); viewer.requestRender();
}

// --- hide (Space) / show hidden (Shift+Space)
function setRowVisible(obj) {
  const r = rowOf.get(obj);
  if (!r) return;
  r.classList.toggle('off', !obj.visible);
  const eye = r.querySelector('.eye');
  if (eye) eye.innerHTML = obj.visible ? ICON_EYE : ICON_EYE_OFF;
}
let hideHint = 0;
function hidePart() {
  if (!active) return;
  let obj = viewer.selected || viewer.hovered;
  if (!obj) { toast('Select a part, or point at it, then press Space to hide it', false, 2200); return; }
  const r = rowOf.get(obj);
  if (r && r._obj) obj = r._obj; // the node the tree row shows (single-child chains are collapsed)
  obj.visible = false;
  if (active.revealed) active.revealed = active.revealed.filter((o) => o !== obj);
  if (viewer.selected && (viewer.selected === obj || isInside(viewer.selected, obj))) viewer.select(null);
  viewer.setHover(null);
  partTag.classList.remove('show');
  setRowVisible(obj);
  viewer.updateBounds();
  viewer.requestRender();
  syncShowHidden();
  if (hideHint++ < 2) toast(`${displayName(obj)} hidden · Shift+Space shows hidden parts`, false, 2200);
}
function hiddenNodes() {
  const out = [];
  active.root.traverse((o) => { if (o !== active.root && !o.userData.isOverlay && (o.isGroup || o.isMesh) && !o.visible) out.push(o); });
  return out;
}
function toggleHidden() {
  if (!active) return;
  if (active.revealed) {
    for (const o of active.revealed) o.visible = false;
    toast(`${active.revealed.length} part${active.revealed.length === 1 ? '' : 's'} hidden again`, false, 1500);
    active.revealed = null;
  } else {
    const list = hiddenNodes();
    if (!list.length) { toast('Nothing is hidden', false, 1500); return; }
    list.forEach((o) => { o.visible = true; });
    active.revealed = list;
    toast(`${list.length} hidden part${list.length === 1 ? '' : 's'} shown · Shift+Space hides them again`, false, 2400);
  }
  syncShowHidden();
  buildTree(); viewer.updateBounds(); viewer.requestRender();
}
function syncShowHidden() {
  const b = $('#btnShowHidden');
  const on = !!(active && active.revealed);
  b.classList.toggle('on', on);
  b.dataset.tip = on ? 'Hide them again (Shift+Space)' : 'Show hidden parts (Shift+Space)';
}
$('#btnShowHidden').addEventListener('click', toggleHidden);

function openCtx(obj, x, y) {
  const items = kernelItems(meshesOf(obj));
  const it = (label, fn, opts = {}) => ({ label, fn, ...opts });
  const list = [
    it('Export as STEP…', () => exportNode(obj, 'step'), { disabled: !items || !isNative, hint: items ? '' : 'CAD + native engine' }),
    it('Export as STL…', () => exportNode(obj, 'stl')),
    it('Export as 3MF…', () => exportNode(obj, '3mf')),
    it('Export view as PNG…', () => exportPng()),
    null,
    it('Mass properties', () => { viewer.select(obj); setMass(true); }, { hint: 'W' }),
    it('AM check', () => { viewer.select(obj); setAM(true); }, { hint: 'A' }),
    it('Color…', () => { viewer.select(obj); if (!pops.color.classList.contains('open')) togglePop('color'); syncColorPop(); }, { hint: 'K' }),
    null,
    it('Fit to view', () => { const b = viewer.boxOf(obj); if (!b.isEmpty()) viewer.fit(true, null, b); }),
    it('Isolate', () => isolate(obj)),
    it('Show all', showAll)
  ];
  ctx.innerHTML = '';
  const h = document.createElement('div');
  h.className = 'ch';
  h.textContent = displayName(obj);
  ctx.appendChild(h);
  for (const e of list) {
    const d = document.createElement('div');
    if (!e) { d.className = 'cs'; ctx.appendChild(d); continue; }
    d.className = 'ci' + (e.disabled ? ' disabled' : '');
    d.innerHTML = `<span></span>${e.hint ? `<small>${e.hint}</small>` : ''}`;
    d.firstChild.textContent = e.label;
    d.addEventListener('click', () => { closeCtx(); e.fn(); });
    ctx.appendChild(d);
  }
  ctx.classList.add('open');
  const r = ctx.getBoundingClientRect();
  ctx.style.left = Math.min(x, window.innerWidth - r.width - 8) + 'px';
  ctx.style.top = Math.min(y, window.innerHeight - r.height - 8) + 'px';
  refreshGlass(ctx);
}
treeEl.addEventListener('contextmenu', (e) => {
  const row = e.target.closest('.row');
  if (!row || !row._obj) return;
  e.preventDefault();
  openCtx(row._obj, e.clientX, e.clientY);
});
// also on parts in the viewport (right-click without dragging)
{
  let rdown = null;
  $('#view').addEventListener('pointerdown', (e) => { if (e.button === 2) rdown = { x: e.clientX, y: e.clientY }; });
  $('#view').addEventListener('contextmenu', (e) => {
    e.preventDefault();
    if (!rdown || Math.hypot(e.clientX - rdown.x, e.clientY - rdown.y) > 4) return;
    const hit = viewer.pick(e);
    if (hit) openCtx(hit.object, e.clientX, e.clientY);
  });
}

// ------------------------------------------------------------------ responsive layout & hideable panes
function layoutUI() {
  const body = document.body;
  const W = window.innerWidth, H = window.innerHeight;
  const L = $('#capLeft'), Cc = $('#capCenter'), R = $('#capRight');
  const fits = () => Cc.offsetWidth + 2 * Math.max(L.offsetWidth, R.offsetWidth) + 56 <= W;
  body.classList.remove('compact', 'stacked');
  if (!fits()) body.classList.add('compact');
  if (!fits()) body.classList.add('stacked');
  const capH = L.offsetHeight || 46;
  const top = 12 + capH + (body.classList.contains('stacked') ? capH + 8 : 0) + 12;
  const rs = document.documentElement.style;
  rs.setProperty('--ui-top', top + 'px');
  rs.setProperty('--cube', (H < 760 || W < 900 ? 136 : 172) + 'px');
  // tabs share the bottom row with the info chip
  const chip = $('#info');
  chip.classList.toggle('mini', store.get('chipMini', false) || W < 1000);
  const chipW = chip.offsetWidth || 0;
  rs.setProperty('--tabs-max', Math.max(220, W - 2 * (chipW + 28)) + 'px');
  cube.resize();
  document.querySelectorAll('.seg').forEach((sg) => segSet(sg, sg.querySelector(':scope > .btn.active')));
  document.querySelectorAll('.glass').forEach((el) => refreshGlass(el));
}
window.addEventListener('resize', () => { clearTimeout(layoutUI._t); layoutUI._t = setTimeout(layoutUI, 60); });
if (document.fonts) document.fonts.ready.then(layoutUI);

document.querySelectorAll('[data-collapse]').forEach((b) => b.addEventListener('click', () => {
  const p = $('#' + b.dataset.collapse);
  p.classList.toggle('collapsed');
  if (!p.classList.contains('collapsed')) segRefresh(p);
}));
const closers = { cutPanel: () => setCut(false), measurePanel: () => setMeasure(false), movePanel: () => setMove(false), massPanel: () => setMass(false), amPanel: () => setAM(false), explodePanel: () => setExplode(false), comparePanel: () => setCompare(false), pmiPanel: () => setPmi(false) };
document.querySelectorAll('[data-close]').forEach((b) => b.addEventListener('click', () => closers[b.dataset.close]()));
$('#btnTreeHide').addEventListener('click', () => setTreeVisible(false));
$('#info').addEventListener('click', () => {
  const mini = !$('#info').classList.contains('mini');
  store.set('chipMini', mini);
  $('#info').classList.toggle('mini', mini);
  layoutUI();
});

// reverse scroll-wheel zoom
let reverseWheel = store.get('reverseWheel', false);
{
  const ctl = viewer.controls;
  const orig = ctl._handleMouseWheel.bind(ctl);
  ctl._handleMouseWheel = (e) => orig(reverseWheel ? { clientX: e.clientX, clientY: e.clientY, deltaY: -e.deltaY, deltaMode: e.deltaMode } : e);
}
// zoom / rotate speed: slider 0..100 maps to 0.25×..4× (50 = 1×, the three.js defaults)
{
  const ctl = viewer.controls;
  const toMul = (v) => Math.pow(4, (v - 50) / 50);
  const toVal = (m) => Math.round(50 + 50 * Math.log(m) / Math.log(4));
  const rows = [['zoomSpd', 'zoomSpeed', 1.0], ['rotSpd', 'rotateSpeed', 1.0]];
  const apply = (id, key, mul) => {
    ctl[key] = mul;
    const el = $('#' + id);
    el.value = toVal(mul);
    el.style.setProperty('--p', el.value + '%');
    $('#' + id + 'Val').textContent = `${mul < 1 ? mul.toFixed(2) : mul.toFixed(1)}×`;
  };
  for (const [id, key, def] of rows) {
    apply(id, key, store.get(id, def));
    $('#' + id).addEventListener('input', (e) => {
      let m = toMul(+e.target.value);
      if (Math.abs(+e.target.value - 50) <= 2) m = 1; // detent at 1×
      apply(id, key, m);
      store.set(id, m);
      writeViewIni();
    });
  }
  $('#btnSpdReset').addEventListener('click', () => { for (const [id, key, def] of rows) { apply(id, key, def); store.set(id, def); } writeViewIni(); });
}
$('#chkWheel').checked = reverseWheel;
$('#chkOpenPanel').checked = store.get('openPanel', true);
setHealMode(store.get('fullHeal', false) ? 'full' : 'fast');
$('#chkHeal').checked = store.get('fullHeal', false);
$('#chkHeal').addEventListener('change', (e) => {
  store.set('fullHeal', e.target.checked);
  setHealMode(e.target.checked ? 'full' : 'fast');
  toast(e.target.checked ? 'Thorough repair: applies to STEP files you open from now on (reopen a file to apply it)' : 'Fast repair: applies to STEP files you open from now on', false, 3000);
});
$('#chkOpenPanel').addEventListener('change', (e) => store.set('openPanel', e.target.checked));
$('#chkWheel').addEventListener('change', (e) => { reverseWheel = e.target.checked; store.set('reverseWheel', reverseWheel); writeViewIni(); });

// the model tree ends just above the info chip (same width, stacked)
new ResizeObserver(() => {
  const el = $('#info');
  const h = el.offsetParent && el.innerHTML && !document.body.classList.contains('hide-info') ? el.offsetHeight : 0;
  document.documentElement.style.setProperty('--chip-h', `${h}px`);
}).observe($('#info'));

// --- model tree width: drag its right edge (the info chip below follows), double-click resets
{
  const grip = $('#treeGrip');
  const root = document.documentElement;
  const clampW = (w) => Math.round(Math.max(200, Math.min(w, Math.min(720, innerWidth * 0.55))));
  const setW = (w) => {
    if (w) root.style.setProperty('--tree-w', `${clampW(w)}px`); else root.style.removeProperty('--tree-w');
    refreshGlass($('#treePanel')); refreshGlass($('#info'));
  };
  const saved = store.get('treeW', 0);
  if (saved) setW(saved);
  let drag = null;
  grip.addEventListener('pointerdown', (e) => {
    if (e.button !== 0) return;
    e.preventDefault();
    grip.setPointerCapture(e.pointerId);
    drag = { x: e.clientX, w: $('#treePanel').getBoundingClientRect().width };
    grip.classList.add('drag');
    document.body.classList.add('tree-resizing');
  });
  grip.addEventListener('pointermove', (e) => { if (drag) setW(drag.w + e.clientX - drag.x); });
  const end = () => {
    if (!drag) return;
    drag = null;
    grip.classList.remove('drag');
    document.body.classList.remove('tree-resizing');
    store.set('treeW', $('#treePanel').getBoundingClientRect().width);
    if (typeof layoutUI === 'function') layoutUI();
  };
  grip.addEventListener('pointerup', end);
  grip.addEventListener('pointercancel', end);
  grip.addEventListener('dblclick', () => { store.set('treeW', 0); setW(0); });
  window.addEventListener('resize', () => { const w = store.get('treeW', 0); if (w) setW(w); });
}

// ------------------------------------------------------------------ restore state & start
document.body.classList.add('nomodel');
(function restore() {
  applyAppearance();
  setQuality(quality, false);
  applyGlass(store.get('glass', 'liquid'));
  const grid = store.get('grid', false);
  $('#chkGrid').checked = grid;
  viewer.setGrid(grid);
  $('#gridOpts').classList.toggle('hidden', !grid);
  applyGridOpts();
  syncColorModeUI();
  setTreeVisible(store.get('tree', true));
  setProjection(store.get('proj2', 'ortho')); // orthographic is the default, like CAD
  requestAnimationFrame(() => setMode(store.get('mode', 'edges')));
  renderRecent();
  renderTabs();
  layoutUI();
})();

// ------------------------------------------------------------------ one window: files opened from Explorer go to the running GlassView
// Settings that GlassView's helpers outside the app read: the Explorer preview (navigation speeds) and
// GlassViewOpen.exe (single window). %LOCALAPPDATA%\GlassView\view.ini
function writeViewIni() {
  clearTimeout(writeViewIni._t);
  writeViewIni._t = setTimeout(async () => {
    try {
      const dir = (await cachePath()).replace(/[\\/]cache$/, '');
      const ini = ['[view]',
        `zoomSpeed=${(+store.get('zoomSpd', 1)).toFixed(3)}`,
        `rotateSpeed=${(+store.get('rotSpd', 1)).toFixed(3)}`,
        `reverseWheel=${store.get('reverseWheel', false) ? 1 : 0}`,
        `singleInstance=${store.get('singleInstance', true) ? 1 : 0}`, ''].join('\r\n');
      await NL.filesystem.writeFile(`${dir}/view.ini`, ini);
    } catch (e) { /* not native */ }
  }, 300);
}

async function startInstance() {
  const args = (window.NL_ARGS || []).slice(1).filter((a) => !a.startsWith('--') && (formatOf(a) || isAssemblyFile(a)));
  let dir = null;
  try { dir = await cachePath(); } catch (e) { /* no cache dir */ }
  const single = store.get('singleInstance', true) && dir;
  if (single && args.length) {
    try {
      const beat = Number(await NL.filesystem.readFile(`${dir}/instance.beat`));
      if (Date.now() - beat < 2500) {
        // another GlassView is running: hand the files over and quit
        await NL.filesystem.writeFile(`${dir}/inbox-${Date.now()}-${Math.random().toString(36).slice(2)}.txt`, args.join('\n'));
        await NL.app.exit();
        return false;
      }
    } catch (e) { /* none running */ }
  }
  try { await NL.window.show(); } catch (e) { /* already visible */ }
  for (const a of args) openPath(a);
  if (!dir) return;
  const beat = () => NL.filesystem.writeFile(`${dir}/instance.beat`, String(Date.now())).catch(() => {});
  beat();
  setInterval(beat, 1000);
  // files handed over by GlassViewOpen.exe or a second GlassView.exe
  let polling = false;
  const poll = async () => {
    if (polling) return;
    polling = true;
    let entries = [];
    try { entries = await NL.filesystem.readDirectory(dir); } catch (e) { polling = false; return; }
    for (const e of entries) {
      const name = e.entry || e.path?.split(/[\\/]/).pop() || '';
      if (!name.startsWith('inbox-') || !name.endsWith('.txt')) continue;
      const file = `${dir}/${name}`;
      let txt = '';
      try { txt = await NL.filesystem.readFile(file); await NL.filesystem.remove(file); } catch (err) { continue; }
      for (const p of txt.split(/\r?\n/).filter(Boolean)) openPath(p.trim());
      try { await NL.window.show(); await NL.window.focus(); } catch (err) { /* ignore */ }
    }
    polling = false;
  };
  setInterval(poll, 500);
  // a hidden / covered window gets its timers slowed down: check as soon as it is shown again
  document.addEventListener('visibilitychange', () => { if (!document.hidden) { beat(); poll(); } });
  window.addEventListener('focus', () => { beat(); poll(); });
  return true;
}
// Hand-over check first: a second copy that only passes its files on must not start the engine.
(isNative ? startInstance() : Promise.resolve(true)).then((keep) => {
  if (keep === false) return;
  if (isNative) writeViewIni();
  // Start the native engine if it is installed; otherwise pre-compile the WebAssembly kernel.
  startKernel().then((k) => {
    $('#engineInfo').textContent = k ? `Native OpenCASCADE · ${k.threads} threads` : 'Built-in (WebAssembly)';
    if (!k) setTimeout(() => (window.requestIdleCallback || setTimeout)(() => prewarmKernel()), 1200);
  });
});

// --- tooltips (v10.1): a single floating tip, placed below the control (above if there is no room),
// clamped to the window and never covering the control itself.
{
  const tip = document.createElement('div');
  tip.id = 'tipFloat';
  document.body.appendChild(tip);
  let cur = null, timer = 0;
  const hide = () => { clearTimeout(timer); cur = null; tip.classList.remove('show'); };
  const place = (el) => {
    const r = el.getBoundingClientRect();
    if (!r.width && !r.height) return hide();
    tip.textContent = el.dataset.tip;
    const w = tip.offsetWidth, h = tip.offsetHeight, gap = 8, W = innerWidth, H = innerHeight;
    let below = r.bottom + gap + h <= H - 4;
    if (el.closest('.tabs')) below = r.top - gap - h < 4; // tab strip sits at the bottom
    const y = below ? r.bottom + gap : r.top - gap - h;
    const x = Math.max(6, Math.min(W - w - 6, r.left + r.width / 2 - w / 2));
    tip.style.left = `${Math.round(x)}px`;
    tip.style.top = `${Math.round(y)}px`;
    tip.style.setProperty('--dy', below ? '-4px' : '4px');
    tip.classList.add('show');
  };
  document.addEventListener('pointerover', (e) => {
    const el = e.target.closest?.('[data-tip]');
    if (el === cur) return;
    hide();
    if (!el || !el.dataset.tip || e.pointerType === 'touch') return;
    cur = el;
    timer = setTimeout(() => { if (cur === el && el.isConnected) place(el); }, 550);
  });
  document.addEventListener('pointerdown', hide, true);
  document.addEventListener('wheel', hide, { passive: true, capture: true });
  window.addEventListener('blur', hide);
  document.addEventListener('pointerout', (e) => { if (cur && !cur.contains(e.relatedTarget)) hide(); });
}
