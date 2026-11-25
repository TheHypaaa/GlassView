import * as THREE from 'three';
import { classifyEdges } from './edgeinfo.js';
import { OrbitControls } from 'three/examples/jsm/controls/OrbitControls.js';
import { RoomEnvironment } from 'three/examples/jsm/environments/RoomEnvironment.js';
import { computeBoundsTree, disposeBoundsTree, acceleratedRaycast } from 'three-mesh-bvh';
import { makeGrid, fitGrid } from './grid.js';

THREE.BufferGeometry.prototype.computeBoundsTree = computeBoundsTree;
THREE.BufferGeometry.prototype.disposeBoundsTree = disposeBoundsTree;
THREE.Mesh.prototype.raycast = acceleratedRaycast;

const Z_UP = new THREE.Vector3(0, 0, 1);
const ACCENT = new THREE.Color(0x2f8cff);
const easeOut = (t) => 1 - Math.pow(1 - t, 3);

export function isModelMesh(o) { return o.isMesh && !o.userData.isOverlay; }

/**
 * The 3D viewport. One renderer, one scene; each tab ("document") owns a root group that is
 * swapped in. Renders only when something changed.
 */
export class Viewer extends EventTarget {
  constructor(canvas) {
    super();
    this.canvas = canvas;
    const r = (this.renderer = new THREE.WebGLRenderer({
      canvas, antialias: true, alpha: true, stencil: true, powerPreference: 'high-performance'
    }));
    r.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));
    r.setClearColor(0x000000, 0);
    r.toneMapping = THREE.NeutralToneMapping;
    r.localClippingEnabled = true;
    r.autoClear = false; // we clear manually: grid pre-pass, then the scene

    this.scene = new THREE.Scene();
    const pmrem = new THREE.PMREMGenerator(r);
    this.scene.environment = pmrem.fromScene(new RoomEnvironment(), 0.04).texture;
    this.scene.environmentIntensity = 0.85;
    pmrem.dispose();

    this.persp = new THREE.PerspectiveCamera(32, 1, 0.1, 1e6);
    this.ortho = new THREE.OrthographicCamera(-1, 1, 1, -1, -1e5, 1e5);
    for (const c of [this.persp, this.ortho]) {
      c.up.copy(Z_UP);
      c.position.set(300, -300, 300);
      const head = new THREE.DirectionalLight(0xffffff, 1.1);
      head.position.set(0.3, 0.6, 1);
      c.add(head);
      c.add(head.target);
      this.scene.add(c);
    }
    this.scene.add(new THREE.HemisphereLight(0xffffff, 0x8890a0, 0.35));

    this.camera = this.persp;
    this.controls = new OrbitControls(this.camera, canvas);
    const ctl = this.controls;
    ctl.enableDamping = true;
    ctl.dampingFactor = 0.15;
    ctl.screenSpacePanning = true;
    ctl.zoomToCursor = true;
    ctl.rotateSpeed = 1.0;
    ctl.zoomSpeed = 1.0;
    ctl.minPolarAngle = 0;
    ctl.maxPolarAngle = Math.PI;
    ctl.mouseButtons = { LEFT: THREE.MOUSE.ROTATE, MIDDLE: THREE.MOUSE.PAN, RIGHT: THREE.MOUSE.PAN };
    ctl.addEventListener('change', () => this.requestRender());

    this.overlay = new THREE.Group(); // tool helpers (measure lines, cut plane outline, gizmo)
    this.scene.add(this.overlay);

    this.doc = null;
    this.model = null;
    this.modelBox = new THREE.Box3(new THREE.Vector3(-50, -50, -50), new THREE.Vector3(50, 50, 50));
    this.modelRadius = 100;
    this.modelCenter = new THREE.Vector3();
    this.displayMode = 'edges';
    this.xray = false;
    this.selected = null;
    // The grid lives in its own scene and is drawn in a pre-pass with its own near/far range,
    // so it can never be clipped by the model-fitted camera range.
    this.gridScene = new THREE.Scene();
    this.grid = makeGrid();
    this.grid.visible = false;
    this.gridExtent = 0;
    this.gridSpacing = 'auto';
    this.gridExtentMul = 3.2;
    this.gridScene.add(this.grid);
    this._gridPersp = new THREE.PerspectiveCamera();
    this._gridOrtho = new THREE.OrthographicCamera();
    this.showGrid = false;
    this.darkBg = false;
    this.clipPlanes = []; // set by the cut tool
    this.anim = null;
    this._dirty = true;
    this._raycaster = new THREE.Raycaster();
    this.renderHooks = [];

    this._resizeObs = new ResizeObserver(() => this.resize());
    this._resizeObs.observe(canvas.parentElement);
    this.resize();
    this._loop = this._loop.bind(this);
    requestAnimationFrame(this._loop);
  }

  requestRender() { this._dirty = true; }

  resize() {
    const el = this.canvas.parentElement;
    const w = el.clientWidth, h = el.clientHeight;
    if (!w || !h) return;
    this.renderer.setSize(w, h, false);
    this.persp.aspect = w / h;
    this.persp.updateProjectionMatrix();
    this._updateOrthoFrustum();
    this.requestRender();
  }

  _updateOrthoFrustum(height) {
    const el = this.canvas.parentElement;
    const aspect = el.clientWidth / Math.max(1, el.clientHeight);
    const h = height ?? (this.ortho.top - this.ortho.bottom);
    this.ortho.top = h / 2; this.ortho.bottom = -h / 2;
    this.ortho.left = (-h * aspect) / 2; this.ortho.right = (h * aspect) / 2;
    this.ortho.updateProjectionMatrix();
  }

  _updateClipping() {
    const R = this.modelRadius;
    const d = this.camera.position.distanceTo(this.modelCenter);
    const farR = R * 1.5;
    if (this.camera.isPerspectiveCamera) {
      this.persp.near = Math.max(d - R * 1.5, R * 0.002, d * 0.0005, farR * 1e-5);
      this.persp.far = d + farR;
      this.persp.updateProjectionMatrix();
    } else {
      this.ortho.near = d - farR;
      this.ortho.far = d + farR;
      this.ortho.updateProjectionMatrix();
    }
  }

  _gridCamera() {
    const cam = this.camera;
    const g = this.grid;
    const h = Math.abs(cam.position.z - g.position.z);
    const span = cam.position.distanceTo(g.position) + this.gridExtent * 2;
    if (cam.isPerspectiveCamera) {
      const c = this._gridPersp;
      c.fov = cam.fov; c.aspect = cam.aspect;
      c.near = Math.max(h * 0.25, this.gridExtent * 1e-5);
      c.far = span;
      c.position.copy(cam.position); c.quaternion.copy(cam.quaternion);
      c.updateProjectionMatrix(); c.updateMatrixWorld(true);
      return c;
    }
    const c = this._gridOrtho;
    c.left = cam.left; c.right = cam.right; c.top = cam.top; c.bottom = cam.bottom; c.zoom = cam.zoom;
    c.near = -span; c.far = span;
    c.position.copy(cam.position); c.quaternion.copy(cam.quaternion);
    c.updateProjectionMatrix(); c.updateMatrixWorld(true);
    return c;
  }

  _loop(t) {
    requestAnimationFrame(this._loop);
    if (this.anim) {
      this._stepAnim(t);
      this._dirty = true;
    } else if (this.controls.enabled && this.controls.update()) {
      this._dirty = true;
    }
    if (!this._dirty) return;
    this._dirty = false;
    const r = this.renderer;
    r.clear();
    if (this.grid.visible) {
      r.render(this.gridScene, this._gridCamera());
      r.clearDepth();
    }
    this._updateClipping();
    r.render(this.scene, this.camera);
    for (const h of this.renderHooks) h(this.camera);
  }

  // ---------------------------------------------------------------- documents
  showDoc(doc) {
    if (this.model) this.scene.remove(this.model);
    this.doc = doc;
    this.model = doc ? doc.root : null;
    this.selected = doc ? doc.selected : null;
    if (this.model) {
      this.scene.add(this.model);
      this.updateBounds();
      this._applyDisplay();
    }
    this._refreshGrid();
    this.requestRender();
  }

  saveCamera() {
    return {
      pos: this.camera.position.clone(), quat: this.camera.quaternion.clone(),
      target: this.controls.target.clone(), ortho: this.camera.isOrthographicCamera,
      orthoH: (this.ortho.top - this.ortho.bottom) / this.ortho.zoom
    };
  }

  restoreCamera(s) {
    this.anim = null;
    this.controls.enabled = true;
    this.setProjection(s.ortho ? 'ortho' : 'persp');
    if (s.ortho) { this.ortho.zoom = 1; this._updateOrthoFrustum(s.orthoH); }
    this.camera.position.copy(s.pos);
    this.camera.quaternion.copy(s.quat);
    this.controls.target.copy(s.target);
    this.controls.update();
    this.requestRender();
  }

  /** Recompute model bounds (after load, insert, or moving parts). */
  updateBounds() {
    const box = new THREE.Box3();
    if (this.model) {
      this.model.updateMatrixWorld(true);
      this.model.traverseVisible((o) => {
        if (!isModelMesh(o)) return;
        const g = o.geometry;
        if (!g.boundingBox) g.computeBoundingBox();
        box.union(g.boundingBox.clone().applyMatrix4(o.matrixWorld));
      });
    }
    if (box.isEmpty()) box.set(new THREE.Vector3(-50, -50, -50), new THREE.Vector3(50, 50, 50));
    this.modelBox = box;
    const s = box.getBoundingSphere(new THREE.Sphere());
    this.modelCenter.copy(s.center);
    this.modelRadius = s.radius || 1;
    this._refreshGrid();
    this.dispatchEvent(new Event('bounds'));
    return box;
  }

  forEachMesh(fn, root = this.model) {
    if (!root) return;
    root.traverse((o) => { if (isModelMesh(o)) fn(o); });
  }

  /** Build BVHs in idle slices so picking/measuring is instant later. */
  prepareBVH(root, onDone) {
    const list = [];
    root.traverse((o) => { if (isModelMesh(o) && !o.geometry.boundsTree) list.push(o.geometry); });
    const uniq = [...new Set(list)];
    const step = (deadline) => {
      while (uniq.length && (!deadline || deadline.timeRemaining() > 4)) {
        const g = uniq.pop();
        if (!g.boundsTree) g.computeBoundsTree({ indirect: true });
      }
      if (uniq.length) (window.requestIdleCallback || setTimeout)(step);
      else onDone?.();
    };
    (window.requestIdleCallback || setTimeout)(step);
  }

  /** Attach feature edges (line segments in the mesh's local frame) to a mesh. */
  attachEdges(mesh, segs) {
    if (!segs || !segs.length) return;
    if (mesh.userData.edges) { mesh.remove(mesh.userData.edges); if (!mesh.userData.edges.geometry.userData.shared) mesh.userData.edges.geometry.dispose(); }
    // repeated parts hand in the same segment array: share one edge geometry (and its classification)
    this._edgeGeoms ||= new WeakMap();
    let g = this._edgeGeoms.get(segs);
    if (g) g.userData.shared = true;
    else {
      g = new THREE.BufferGeometry();
      g.setAttribute('position', new THREE.BufferAttribute(segs, 3));
      this._edgeGeoms.set(segs, g);
    }
    const lines = new THREE.LineSegments(g, this.edgeMaterial());
    lines.userData.isOverlay = true;
    lines.raycast = () => {};
    mesh.add(lines);
    mesh.userData.edges = lines;
    mesh.userData.snaps = null;
    this._applyDisplayTo(mesh);
    if (this.edgeStyle === 'catia') this.queueEdgeClassify(mesh);
    this.requestRender();
  }

  /** Edge material. A per-vertex "soft" attribute (tangent-continuous edges) blends towards a
   *  lighter colour and lower opacity when the CATIA-style edge display is on. */
  edgeMaterial() {
    if (!this._edgeMat) {
      const m = new THREE.LineBasicMaterial({ color: 0x1b1f27, transparent: true, opacity: 0.55, depthWrite: false });
      m.userData.u = { softColor: { value: new THREE.Color(0x8d949f) }, softOn: { value: 0 }, softAlpha: { value: 0.75 } };
      m.onBeforeCompile = (sh) => {
        Object.assign(sh.uniforms, m.userData.u);
        sh.vertexShader = sh.vertexShader
          .replace('void main() {', 'attribute float soft;\nvarying float vSoft;\nvoid main() {\n  vSoft = soft;');
        sh.fragmentShader = sh.fragmentShader
          .replace('void main() {', 'uniform vec3 softColor;\nuniform float softOn;\nuniform float softAlpha;\nvarying float vSoft;\nvoid main() {')
          .replace('#include <color_fragment>', '#include <color_fragment>\n  float sk = vSoft * softOn;\n  diffuseColor.rgb = mix(diffuseColor.rgb, softColor, sk);\n  diffuseColor.a *= mix(1.0, softAlpha, sk);');
      };
      m.customProgramCacheKey = () => 'gv-edge';
      this._edgeMat = m;
    }
    return this._edgeMat;
  }

  setEdgeColor(dark) {
    this._edgeDark = dark;
    this._styleEdges();
  }

  /** 'uniform' – every CAD edge the same; 'catia' – sharp edges dark, tangent edges soft grey. */
  setEdgeStyle(style) {
    this.edgeStyle = style;
    this._styleEdges();
    if (style === 'catia') this.forEachMesh((m) => this.queueEdgeClassify(m));
  }

  _styleEdges() {
    const m = this.edgeMaterial(), u = m.userData.u, dark = this._edgeDark !== false;
    const catia = this.edgeStyle === 'catia';
    if (catia) {
      m.color.set(dark ? 0x05070a : 0xf4f6fa);
      m.opacity = dark ? 0.82 : 0.9;
      u.softColor.value.set(dark ? 0x7d8590 : 0x6b7482);
      u.softAlpha.value = dark ? 0.6 : 0.7;
    } else {
      m.color.set(dark ? 0x1b1f27 : 0xe8edf5);
      m.opacity = 0.55;
    }
    u.softOn.value = catia ? 1 : 0;
    this.requestRender();
  }

  queueEdgeClassify(mesh) {
    const E = mesh.userData.edges;
    if (!E || E.geometry.userData.softFor === E.geometry.attributes.position) return;
    (this._clsQ ||= new Set()).add(mesh);
    if (this._clsBusy) return;
    this._clsBusy = true;
    const step = (dl) => {
      for (const m of this._clsQ) {
        if (dl && dl.timeRemaining && dl.timeRemaining() < 3) break;
        this._clsQ.delete(m);
        const eg = m.userData.edges && m.userData.edges.geometry;
        if (!eg || eg.userData.softFor === eg.attributes.position) continue;
        const soft = classifyEdges(m);
        if (soft) eg.setAttribute('soft', new THREE.BufferAttribute(soft, 1));
        eg.userData.softFor = eg.attributes.position;
      }
      this.requestRender();
      if (this._clsQ.size) (window.requestIdleCallback || setTimeout)(step);
      else this._clsBusy = false;
    };
    (window.requestIdleCallback || setTimeout)(step);
  }

  setDisplayMode(mode) { this.displayMode = mode; this._applyDisplay(); }
  setXray(on) { this.xray = on; this._applyDisplay(); }

  _applyDisplay() { this.forEachMesh((m) => this._applyDisplayTo(m)); this.requestRender(); }

  _applyDisplayTo(mesh) {
    const mode = this.displayMode;
    const mats = Array.isArray(mesh.material) ? mesh.material : [mesh.material];
    for (const m of mats) {
      m.visible = mode !== 'wire';
      m.transparent = this.xray;
      m.opacity = this.xray ? 0.28 : 1;
      m.depthWrite = !this.xray;
      m.clippingPlanes = this.clipPlanes.length ? this.clipPlanes : null;
      m.needsUpdate = true;
    }
    if (mesh.userData.edges) mesh.userData.edges.visible = mode === 'edges' || mode === 'wire' || this.xray;
    if (mode === 'mesh') {
      if (!mesh.userData.tri) {
        const tri = new THREE.Mesh(mesh.geometry, this._triMaterial());
        tri.userData.isOverlay = true;
        tri.raycast = () => {};
        mesh.add(tri);
        mesh.userData.tri = tri;
      }
      mesh.userData.tri.visible = true;
    } else if (mesh.userData.tri) {
      mesh.userData.tri.visible = false;
    }
  }

  _triMaterial() {
    if (!this._triMat) this._triMat = new THREE.MeshBasicMaterial({ color: 0x0a0d12, wireframe: true, transparent: true, opacity: 0.35, depthWrite: false });
    return this._triMat;
  }

  /** Called by the cut tool: planes apply to model, edges and tessellation overlay. */
  setClipPlanes(planes) {
    this.clipPlanes = planes;
    const cp = planes.length ? planes : null;
    this.edgeMaterial().clippingPlanes = cp;
    this._triMaterial().clippingPlanes = cp;
    this.edgeMaterial().needsUpdate = true;
    this._triMaterial().needsUpdate = true;
    this._applyDisplay();
  }

  // ---------------------------------------------------------------- grid
  setGrid(on) { this.showGrid = on; this._refreshGrid(); }
  setGridOptions(spacing, extentMul) { this.gridSpacing = spacing; this.gridExtentMul = extentMul; this._refreshGrid(); }
  setDark(dark) {
    this.darkBg = dark;
    // night tuning: slightly lower exposure and softer reflections so parts don't glow on dark backgrounds
    this.renderer.toneMappingExposure = dark ? 0.94 : 1.0;
    this.scene.environmentIntensity = dark ? 0.72 : 0.85;
    this._refreshGrid();
  }
  _refreshGrid() {
    this.grid.visible = this.showGrid && !!this.model;
    if (this.grid.visible) this.gridExtent = fitGrid(this.grid, this.modelBox, this.darkBg, this.gridSpacing, this.gridExtentMul);
    this.requestRender();
  }

  // ---------------------------------------------------------------- camera
  fit(animate = true, dir = null, box = null) {
    const sphere = (box || this.modelBox).getBoundingSphere(new THREE.Sphere());
    const d = dir ? dir.clone().normalize() : this.currentViewDir();
    const fov = THREE.MathUtils.degToRad(this.persp.fov);
    const aspect = this.persp.aspect;
    const fitFov = aspect < 1 ? 2 * Math.atan(Math.tan(fov / 2) * aspect) : fov;
    const dist = (sphere.radius / Math.sin(fitFov / 2)) * 1.05;
    const orthoH = (sphere.radius * 2.2) / Math.min(1, aspect);
    this._animateTo(sphere.center, d, dist, orthoH, animate);
  }

  snapTo(dir, animate = true) {
    const dist = this.camera.position.distanceTo(this.controls.target);
    this._animateTo(this.controls.target.clone(), dir.clone().normalize(), dist, null, animate);
  }

  currentViewDir() {
    return this.camera.position.clone().sub(this.controls.target).normalize();
  }

  _targetQuat(target, dir, dist) {
    let d = dir.clone();
    if (Math.abs(d.z) > 0.9999) d = new THREE.Vector3(0, -1e-4, Math.sign(d.z)).normalize();
    const eye = target.clone().addScaledVector(d, dist);
    const m = new THREE.Matrix4().lookAt(eye, target, Z_UP);
    return { quat: new THREE.Quaternion().setFromRotationMatrix(m), eye };
  }

  _animateTo(target, dir, dist, orthoH, animate) {
    const cam = this.camera;
    const from = {
      target: this.controls.target.clone(),
      quat: cam.quaternion.clone(),
      dist: cam.position.distanceTo(this.controls.target),
      orthoH: (this.ortho.top - this.ortho.bottom) / this.ortho.zoom
    };
    const { quat } = this._targetQuat(target, dir, dist);
    const to = { target: target.clone(), quat, dist, orthoH: orthoH ?? from.orthoH };
    this.controls.enabled = false;
    this.anim = { from, to, start: performance.now(), dur: animate ? 420 : 0 };
    if (!animate) this._stepAnim(performance.now());
    this.requestRender();
  }

  _stepAnim(now) {
    const a = this.anim;
    const t = a.dur ? Math.min(1, (now - a.start) / a.dur) : 1;
    const k = easeOut(t);
    const cam = this.camera;
    const q = a.from.quat.clone().slerp(a.to.quat, k);
    const target = a.from.target.clone().lerp(a.to.target, k);
    const dist = a.from.dist + (a.to.dist - a.from.dist) * k;
    const back = new THREE.Vector3(0, 0, 1).applyQuaternion(q);
    cam.quaternion.copy(q);
    cam.position.copy(target).addScaledVector(back, dist);
    this.controls.target.copy(target);
    if (a.from.orthoH !== a.to.orthoH || this.ortho.zoom !== 1) {
      this.ortho.zoom = 1;
      this._updateOrthoFrustum(a.from.orthoH + (a.to.orthoH - a.from.orthoH) * k);
    }
    if (t >= 1) {
      this.anim = null;
      const { eye } = this._targetQuat(a.to.target, back, a.to.dist);
      cam.position.copy(eye);
      cam.lookAt(a.to.target);
      this.controls.enabled = true;
      this.controls.update();
      this.dispatchEvent(new Event('viewchange'));
    }
  }

  orbitBy(dx, dy) {
    const target = this.controls.target;
    const off = this.camera.position.clone().sub(target);
    const sph = new THREE.Spherical().setFromVector3(new THREE.Vector3(off.x, off.z, -off.y));
    sph.theta -= dx * 0.01;
    sph.phi = THREE.MathUtils.clamp(sph.phi - dy * 0.01, 1e-4, Math.PI - 1e-4);
    const v = new THREE.Vector3().setFromSpherical(sph);
    this.camera.position.copy(target).add(new THREE.Vector3(v.x, -v.z, v.y));
    this.camera.lookAt(target);
    this.requestRender();
  }

  setProjection(kind) {
    const isOrtho = kind === 'ortho';
    if (isOrtho === this.camera.isOrthographicCamera) return;
    const from = this.camera, to = isOrtho ? this.ortho : this.persp;
    const target = this.controls.target;
    const dist = from.position.distanceTo(target);
    const fov = THREE.MathUtils.degToRad(this.persp.fov);
    if (isOrtho) {
      this.ortho.zoom = 1;
      this._updateOrthoFrustum(2 * dist * Math.tan(fov / 2));
      to.position.copy(from.position);
    } else {
      const h = (this.ortho.top - this.ortho.bottom) / this.ortho.zoom;
      const newDist = h / 2 / Math.tan(fov / 2);
      const dir = from.position.clone().sub(target).normalize();
      to.position.copy(target).addScaledVector(dir, newDist);
    }
    to.quaternion.copy(from.quaternion);
    this.camera = to;
    this.controls.object = to;
    this.controls.update();
    this.dispatchEvent(new Event('camera'));
    this.requestRender();
  }

  // ---------------------------------------------------------------- picking
  pointerNDC(ev) {
    const r = this.canvas.getBoundingClientRect();
    return new THREE.Vector2(((ev.clientX - r.left) / r.width) * 2 - 1, -((ev.clientY - r.top) / r.height) * 2 + 1);
  }

  toScreen(p) {
    const v = p.clone().project(this.camera);
    const r = this.canvas.getBoundingClientRect();
    return { x: r.left + ((v.x + 1) / 2) * r.width, y: r.top + ((1 - v.y) / 2) * r.height, behind: v.z > 1 };
  }

  pick(ev) { return this.pickAt(this.pointerNDC(ev)); }

  /** Pick at normalised device coordinates (also leaves the ray in this._raycaster). */
  pickAt(ndc) {
    if (!this.model) return null;
    const rc = this._raycaster;
    rc.setFromCamera(ndc, this.camera);
    rc.firstHitOnly = this.clipPlanes.length === 0;
    const hits = rc.intersectObject(this.model, true);
    outer: for (const h of hits) {
      if (!isModelMesh(h.object) || !isVisibleChain(h.object)) continue;
      for (const p of this.clipPlanes) if (p.distanceToPoint(h.point) < 0) continue outer;
      return h;
    }
    return null;
  }

  select(obj) {
    if (this.selected === obj) return;
    const each = (o, on) => o.traverse((m) => { if (isModelMesh(m)) setHighlight(m, on ? 2 : 0); });
    if (this.selected) each(this.selected, false);
    this.selected = obj;
    if (this.doc) this.doc.selected = obj;
    if (obj) each(obj, true);
    if (this.hovered) this.setHover(this.hovered, true);
    this.requestRender();
    this.dispatchEvent(new CustomEvent('select', { detail: obj }));
  }

  /** Light pre-highlight of what a click would select (never overrides the selection). */
  setHover(obj, force = false) {
    if (this.hovered === obj && !force) return;
    const sel = this.selected;
    const inSel = (m) => { for (let p = m; p; p = p.parent) if (p === sel) return true; return false; };
    if (this.hovered && this.hovered !== obj) this.hovered.traverse((m) => { if (isModelMesh(m) && !inSel(m)) setHighlight(m, 0); });
    this.hovered = obj;
    if (obj) obj.traverse((m) => { if (isModelMesh(m) && !inSel(m)) setHighlight(m, 1); });
    this.requestRender();
  }

  boxOf(obj) {
    const box = new THREE.Box3();
    obj.updateMatrixWorld(true);
    obj.traverse((m) => {
      if (!isModelMesh(m)) return;
      if (!m.geometry.boundingBox) m.geometry.computeBoundingBox();
      box.union(m.geometry.boundingBox.clone().applyMatrix4(m.matrixWorld));
    });
    return box;
  }

  setPivot(point) {
    const dir = this.currentViewDir();
    const dist = this.camera.position.distanceTo(point);
    this._animateTo(point, dir, dist, null, true);
  }
}

function isVisibleChain(o) {
  for (let p = o; p; p = p.parent) if (!p.visible) return false;
  return true;
}

export const HL = [0, 0.3, 0.7];
export function setHighlight(mesh, level) {
  level = level === true ? 2 : level === false ? 0 : level;
  const mats = Array.isArray(mesh.material) ? mesh.material : [mesh.material];
  for (const m of mats) {
    if (!m.emissive || m.userData.noHighlight) continue;
    if (level) {
      if (!m.userData.prev) m.userData.prev = { c: m.color.getHex(), e: m.emissive.getHex() };
      m.userData.prev.l = HL[level];
      m.color.setHex(m.userData.prev.c).lerp(ACCENT, HL[level]);
      m.emissive.copy(ACCENT).multiplyScalar(level === 2 ? 0.18 : 0.08);
    } else if (m.userData.prev) {
      m.color.setHex(m.userData.prev.c); m.emissive.setHex(m.userData.prev.e);
      m.userData.prev = null;
    }
  }
}
