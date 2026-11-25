import * as THREE from 'three';

// Z-up convention (same as most CAD / STEP data): Front = -Y, Right = +X, Top = +Z.
const FACES = [
  { dir: [0, -1, 0], label: 'FRONT', sub: 'XZ', up: [0, 0, 1] },
  { dir: [0, 1, 0], label: 'BACK', sub: 'XZ', up: [0, 0, 1] },
  { dir: [1, 0, 0], label: 'RIGHT', sub: 'YZ', up: [0, 0, 1] },
  { dir: [-1, 0, 0], label: 'LEFT', sub: 'YZ', up: [0, 0, 1] },
  { dir: [0, 0, 1], label: 'TOP', sub: 'XY', up: [0, 1, 0] },
  { dir: [0, 0, -1], label: 'BOTTOM', sub: 'XY', up: [0, -1, 0] }
];

// Superellipsoid |x|^n + |y|^n + |z|^n = 1: a cube whose edges and corners blend with
// continuous curvature (no tangent-only fillets) — the "AirPods case" look.
const N = 6;
const C = 0.6; // |coordinate| above this belongs to that side's band → face / edge / corner regions
const CAM_DIST = 9.4;

function roundedCube(seg = 40) {
  const g = new THREE.BoxGeometry(2, 2, 2, seg, seg, seg);
  const p = g.attributes.position;
  const nrm = g.attributes.normal;
  for (let i = 0; i < p.count; i++) {
    const x = p.getX(i), y = p.getY(i), z = p.getZ(i);
    const s = Math.pow(Math.abs(x) ** N + Math.abs(y) ** N + Math.abs(z) ** N, 1 / N);
    const X = x / s, Y = y / s, Z = z / s;
    p.setXYZ(i, X, Y, Z);
    // analytic normal = gradient of the implicit surface
    const nx = Math.sign(X) * Math.abs(X) ** (N - 1), ny = Math.sign(Y) * Math.abs(Y) ** (N - 1), nz = Math.sign(Z) * Math.abs(Z) ** (N - 1);
    const l = Math.hypot(nx, ny, nz) || 1;
    nrm.setXYZ(i, nx / l, ny / l, nz / l);
  }
  g.computeBoundingSphere();
  return g;
}

function regionOf(p) {
  return new THREE.Vector3(...[p.x, p.y, p.z].map((v) => (Math.abs(v) > C ? Math.sign(v) : 0)));
}

function labelTexture(text, sub, dark, hot) {
  const s = 256;
  const cv = document.createElement('canvas');
  cv.width = cv.height = s;
  const g = cv.getContext('2d');
  g.fillStyle = hot ? '#ffffff' : dark ? 'rgba(255,255,255,0.88)' : 'rgba(44,50,62,0.78)';
  g.textAlign = 'center';
  g.textBaseline = 'middle';
  g.font = '650 46px -apple-system, "SF Pro Display", "Segoe UI Variable Display", "Segoe UI", system-ui, sans-serif';
  g.fillText(text, s / 2, s / 2 - 8);
  g.globalAlpha = 0.5;
  g.font = '500 28px -apple-system, "SF Pro Text", "Segoe UI", system-ui, sans-serif';
  g.fillText(sub, s / 2, s / 2 + 34);
  const tex = new THREE.CanvasTexture(cv);
  tex.colorSpace = THREE.SRGBColorSpace;
  tex.anisotropy = 4;
  return tex;
}

function axisSprite(text, color) {
  const s = 64;
  const cv = document.createElement('canvas');
  cv.width = cv.height = s;
  const g = cv.getContext('2d');
  g.fillStyle = color;
  g.beginPath(); g.arc(s / 2, s / 2, s / 2 - 4, 0, Math.PI * 2); g.fill();
  g.fillStyle = '#fff';
  g.font = '700 32px -apple-system, "Segoe UI", system-ui, sans-serif';
  g.textAlign = 'center'; g.textBaseline = 'middle';
  g.fillText(text, s / 2, s / 2 + 2);
  const tex = new THREE.CanvasTexture(cv);
  tex.colorSpace = THREE.SRGBColorSpace;
  const sp = new THREE.Sprite(new THREE.SpriteMaterial({ map: tex, depthTest: false, toneMapped: false }));
  sp.scale.set(0.3, 0.3, 1);
  sp.renderOrder = 10;
  return sp;
}

/** Frosted-glass shading for the cube, with region highlighting done per pixel (crisp borders). */
function glassMaterial() {
  return new THREE.ShaderMaterial({
    transparent: true,
    depthWrite: true,
    uniforms: {
      uHover: { value: new THREE.Vector3() },
      uC: { value: C },
      uBase: { value: new THREE.Color(0xffffff) },
      uAccent: { value: new THREE.Color(0x0a84ff) },
      uDark: { value: 0 }
    },
    vertexShader: /* glsl */ `
      varying vec3 vPos; varying vec3 vN; varying vec3 vV;
      void main() {
        vPos = position;
        vec4 mv = modelViewMatrix * vec4(position, 1.0);
        vN = normalize(normalMatrix * normal);
        vV = normalize(-mv.xyz);
        gl_Position = projectionMatrix * mv;
      }`,
    fragmentShader: /* glsl */ `
      uniform vec3 uHover; uniform float uC; uniform vec3 uBase; uniform vec3 uAccent; uniform float uDark;
      varying vec3 vPos; varying vec3 vN; varying vec3 vV;
      void main() {
        vec3 r = vec3(abs(vPos.x) > uC ? sign(vPos.x) : 0.0,
                      abs(vPos.y) > uC ? sign(vPos.y) : 0.0,
                      abs(vPos.z) > uC ? sign(vPos.z) : 0.0);
        bool hot = dot(uHover, uHover) > 0.5 && all(equal(r, uHover));
        vec3 N = normalize(vN), V = normalize(vV);
        vec3 L = normalize(vec3(-0.35, 0.7, 0.9));
        float ndl = max(dot(N, L), 0.0);
        float fres = pow(1.0 - max(dot(N, V), 0.0), 2.5);
        float spec = pow(max(dot(reflect(-L, N), V), 0.0), 40.0);
        vec3 base = uDark > 0.5 ? vec3(0.36, 0.40, 0.47) : uBase * 0.97;
        vec3 col = base * (0.7 + 0.3 * ndl);
        float a = uDark > 0.5 ? 0.46 : 0.66;
        if (hot) { col = mix(col, uAccent, 0.88); a = 0.93; }
        col += vec3(1.0) * (fres * 0.45 + spec * 0.55);
        a = clamp(a + fres * 0.35 + spec * 0.4, 0.0, 1.0);
        gl_FragColor = vec4(col, a);
      }`
  });
}

export class ViewCube {
  constructor(canvas, viewer) {
    this.canvas = canvas;
    this.viewer = viewer;
    this.dark = false;
    this.renderer = new THREE.WebGLRenderer({ canvas, antialias: true, alpha: true });
    this.renderer.setPixelRatio(Math.min(window.devicePixelRatio || 1, 2));
    this.renderer.setClearColor(0, 0);
    this.scene = new THREE.Scene();
    this.camera = new THREE.PerspectiveCamera(24, 1, 0.1, 50);
    this.scene.add(this.camera);

    this.material = glassMaterial();
    this.body = new THREE.Mesh(roundedCube(), this.material);
    this.body.renderOrder = 1;
    this.scene.add(this.body);

    // labels sit on the flat middle of each face
    this.labels = [];
    for (const f of FACES) {
      const n = new THREE.Vector3(...f.dir), up = new THREE.Vector3(...f.up);
      const right = new THREE.Vector3().crossVectors(up, n);
      const plane = new THREE.Mesh(
        new THREE.PlaneGeometry(1.25, 1.25),
        new THREE.MeshBasicMaterial({ map: labelTexture(f.label, f.sub, false), transparent: true, depthWrite: false, toneMapped: false })
      );
      plane.matrixAutoUpdate = false;
      plane.matrix.makeBasis(right, up, n).setPosition(n.clone().multiplyScalar(1.004));
      plane.raycast = () => {};
      plane.userData.face = f;
      plane.userData.dir = n.clone();
      plane.renderOrder = 2;
      this.scene.add(plane);
      this.labels.push(plane);
    }

    // axis triad from the front-left-bottom corner
    const o = new THREE.Vector3(-1.02, -1.02, -1.02);
    for (const a of [{ v: [1, 0, 0], c: '#ff453a', t: 'X' }, { v: [0, 1, 0], c: '#30d158', t: 'Y' }, { v: [0, 0, 1], c: '#0a84ff', t: 'Z' }]) {
      const end = o.clone().addScaledVector(new THREE.Vector3(...a.v), 1.95);
      const line = new THREE.Line(new THREE.BufferGeometry().setFromPoints([o, end]), new THREE.LineBasicMaterial({ color: a.c, depthTest: false, transparent: true, opacity: 0.9 }));
      line.renderOrder = 9;
      this.scene.add(line);
      const sp = axisSprite(a.t, a.c);
      sp.position.copy(end);
      this.scene.add(sp);
    }

    this.hover = null;
    this.raycaster = new THREE.Raycaster();
    this._bind();
    this.resize();
  }

  setDark(dark) {
    if (this.dark === dark) return;
    this.dark = dark;
    this.material.uniforms.uDark.value = dark ? 1 : 0;
    for (const l of this.labels) this._label(l, false);
    this.render();
  }

  _label(l, hot) {
    l.material.map.dispose();
    l.material.map = labelTexture(l.userData.face.label, l.userData.face.sub, this.dark, hot);
    l.material.needsUpdate = true;
  }

  resize() {
    const r = this.canvas.getBoundingClientRect();
    if (r.width && r.height) this.renderer.setSize(r.width, r.height, false);
    this.render();
  }

  sync(mainCam) {
    this.camera.position.set(0, 0, CAM_DIST).applyQuaternion(mainCam.quaternion);
    this.camera.quaternion.copy(mainCam.quaternion);
    this.render();
  }

  render() { this.renderer.render(this.scene, this.camera); }

  /** Region direction under the pointer, or null. */
  _hit(ev) {
    const r = this.canvas.getBoundingClientRect();
    const ndc = new THREE.Vector2(((ev.clientX - r.left) / r.width) * 2 - 1, -((ev.clientY - r.top) / r.height) * 2 + 1);
    this.raycaster.setFromCamera(ndc, this.camera);
    const h = this.raycaster.intersectObject(this.body, false)[0];
    return h ? regionOf(h.point) : null;
  }

  _setHover(dir) {
    const key = dir ? dir.toArray().join() : null;
    if (key === this.hoverKey) return;
    const faceLabel = (d) => d && Math.abs(d.x) + Math.abs(d.y) + Math.abs(d.z) === 1 && this.labels.find((l) => l.userData.dir.equals(d));
    const old = faceLabel(this.hover);
    if (old) this._label(old, false);
    this.hover = dir;
    this.hoverKey = key;
    this.material.uniforms.uHover.value.copy(dir || new THREE.Vector3());
    const nl = faceLabel(dir);
    if (nl) this._label(nl, true);
    this.canvas.style.cursor = dir ? 'pointer' : 'grab';
    this.render();
  }

  _bind() {
    const c = this.canvas;
    let down = null, dragging = false;
    c.addEventListener('pointermove', (ev) => {
      if (down) {
        const dx = ev.clientX - down.x, dy = ev.clientY - down.y;
        if (!dragging && Math.hypot(dx, dy) > 3) { dragging = true; this._setHover(null); c.style.cursor = 'grabbing'; }
        if (dragging) {
          this.viewer.orbitBy(ev.clientX - down.lx, ev.clientY - down.ly);
          down.lx = ev.clientX; down.ly = ev.clientY;
        }
        return;
      }
      this._setHover(this._hit(ev));
    });
    c.addEventListener('pointerleave', () => { if (!down) this._setHover(null); });
    c.addEventListener('pointerdown', (ev) => {
      c.setPointerCapture(ev.pointerId);
      down = { x: ev.clientX, y: ev.clientY, lx: ev.clientX, ly: ev.clientY };
      dragging = false;
    });
    c.addEventListener('pointerup', (ev) => {
      const wasDrag = dragging;
      down = null; dragging = false;
      c.style.cursor = 'grab';
      if (wasDrag) { this.viewer.controls.update(); return; }
      const d = this._hit(ev);
      if (!d) return;
      const dir = d.clone().normalize();
      const isFace = Math.abs(d.x) + Math.abs(d.y) + Math.abs(d.z) === 1;
      if (isFace && this.viewer.currentViewDir().dot(dir) > 0.999) dir.negate();
      this.viewer.snapTo(dir);
    });
  }
}
