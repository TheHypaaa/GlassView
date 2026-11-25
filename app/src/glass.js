// Liquid-glass refraction: for every .glass element we generate an SVG displacement map that bends
// the backdrop near the rounded edges (a lens-like bezel), then combine it with blur + saturation in
// backdrop-filter. Chromium (and therefore WebView2) supports SVG filters in backdrop-filter.

const SVG_NS = 'http://www.w3.org/2000/svg';
let defs = null;
let seq = 0;
export let glassMode = 'liquid'; // 'liquid' | 'frosted' | 'solid'

function ensureDefs() {
  if (defs) return defs;
  const svg = document.createElementNS(SVG_NS, 'svg');
  svg.setAttribute('width', '0');
  svg.setAttribute('height', '0');
  svg.style.position = 'absolute';
  svg.setAttribute('aria-hidden', 'true');
  defs = document.createElementNS(SVG_NS, 'defs');
  svg.appendChild(defs);
  document.body.appendChild(svg);
  return defs;
}

function displacementMap(w, h, radius, bezel) {
  const scale = 1; // map resolution in CSS px
  const W = Math.max(2, Math.round(w * scale)), H = Math.max(2, Math.round(h * scale));
  const cv = document.createElement('canvas');
  cv.width = W; cv.height = H;
  const g = cv.getContext('2d');
  const img = g.createImageData(W, H);
  const d = img.data;
  const hw = w / 2, hh = h / 2;
  const r = Math.min(radius, hw, hh);
  for (let y = 0; y < H; y++) {
    for (let x = 0; x < W; x++) {
      const px = (x + 0.5) / scale - hw, py = (y + 0.5) / scale - hh;
      const qx = Math.abs(px) - (hw - r), qy = Math.abs(py) - (hh - r);
      const ox = Math.max(qx, 0), oy = Math.max(qy, 0);
      const outside = Math.hypot(ox, oy);
      const dist = r - (outside + Math.min(Math.max(qx, qy), 0)); // distance to edge, inside > 0
      let nx = 0, ny = 0;
      if (qx > 0 && qy > 0) { nx = (ox / (outside || 1)) * Math.sign(px); ny = (oy / (outside || 1)) * Math.sign(py); }
      else if (qx > qy) nx = Math.sign(px);
      else ny = Math.sign(py);
      let s = 0;
      if (dist < bezel && dist > 0) {
        const t = 1 - dist / bezel;
        s = t * t * (3 - 2 * t); // smoothstep: strongest right at the rim, like a convex lens edge
      }
      const i = (y * W + x) * 4;
      // sample inward (negative normal) so the rim magnifies what lies under it
      d[i] = 128 - nx * s * 127;
      d[i + 1] = 128 - ny * s * 127;
      d[i + 2] = 128;
      d[i + 3] = 255;
    }
  }
  g.putImageData(img, 0, 0);
  return cv.toDataURL();
}

const registry = new Map();

function update(el) {
  const st = registry.get(el);
  if (!st) return;
  const rect = el.getBoundingClientRect();
  const w = Math.round(rect.width), h = Math.round(rect.height);
  const blur = el.dataset.blur || '7';
  const sat = el.dataset.sat || '1.7';
  if (glassMode !== 'liquid' || w < 4 || h < 4) {
    el.style.backdropFilter = glassMode === 'solid' ? 'none' : `blur(${+blur + 8}px) saturate(${sat})`;
    return;
  }
  if (st.w !== w || st.h !== h) {
    st.w = w; st.h = h;
    const radius = parseFloat(getComputedStyle(el).borderTopLeftRadius) || 16;
    const bezel = Math.min(+(el.dataset.bezel || 14), w / 2, h / 2);
    st.img.setAttribute('href', displacementMap(w, h, radius, bezel));
    st.img.setAttribute('width', w);
    st.img.setAttribute('height', h);
  }
  el.style.backdropFilter = `url(#${st.id}) blur(${blur}px) saturate(${sat}) brightness(1.04)`;
}

export function makeGlass(el) {
  const d = ensureDefs();
  const id = `lg-${++seq}`;
  const f = document.createElementNS(SVG_NS, 'filter');
  f.setAttribute('id', id);
  f.setAttribute('x', '0'); f.setAttribute('y', '0');
  f.setAttribute('width', '100%'); f.setAttribute('height', '100%');
  f.setAttribute('color-interpolation-filters', 'sRGB');
  const img = document.createElementNS(SVG_NS, 'feImage');
  img.setAttribute('x', '0'); img.setAttribute('y', '0');
  img.setAttribute('preserveAspectRatio', 'none');
  img.setAttribute('result', 'map');
  const disp = document.createElementNS(SVG_NS, 'feDisplacementMap');
  disp.setAttribute('in', 'SourceGraphic');
  disp.setAttribute('in2', 'map');
  disp.setAttribute('scale', el.dataset.refract || '34');
  disp.setAttribute('xChannelSelector', 'R');
  disp.setAttribute('yChannelSelector', 'G');
  f.append(img, disp);
  d.appendChild(f);
  registry.set(el, { id, img, w: 0, h: 0 });
  const ro = new ResizeObserver(() => update(el));
  ro.observe(el);
  update(el);
}

export function setGlassMode(mode) {
  glassMode = mode;
  document.documentElement.dataset.glass = mode;
  for (const el of registry.keys()) { const st = registry.get(el); st.w = 0; update(el); }
}

export function refreshGlass(el) { const st = registry.get(el); if (st) { st.w = 0; update(el); } }
