import * as THREE from 'three';
import { isModelMesh } from './viewer.js';

const ACCENT = new THREE.Color(0x2f8cff);

// Vivid, clearly distinct hues (ordered so neighbouring parts contrast strongly).
export const AUTO_PALETTE = [
  '#0a84ff', '#ff9f0a', '#30d158', '#ff375f', '#bf5af2', '#64d2ff', '#ffd60a', '#ff453a',
  '#5e5ce6', '#00c7be', '#ff6b1a', '#a2e635', '#e040fb', '#2563eb', '#f43f5e', '#14b8a6',
  '#facc15', '#8b5cf6', '#22c55e', '#fb7185'
];
export const SWATCHES = [
  // vivid
  '#ff3b30', '#ff6b1a', '#ff9f0a', '#ffd60a', '#a2e635', '#30d158', '#00c7be', '#64d2ff',
  '#0a84ff', '#2563eb', '#5e5ce6', '#bf5af2', '#e040fb', '#ff2d92', '#ff375f', '#a2845e',
  // metals & neutrals
  '#ffffff', '#e5e8ec', '#c9ced6', '#a7b0bc', '#7d8693', '#565e6a', '#2f343c', '#111317'
];
/** Remember the file's own colours once, before anything is recoloured. */
export function captureOriginal(root) {
  root.traverse((m) => {
    if (!isModelMesh(m) || m.userData.orig) return;
    const mats = Array.isArray(m.material) ? m.material : [m.material];
    m.userData.orig = mats.map((x) => ({ c: x.color.getHex(), v: x.vertexColors }));
  });
}

/** Set a material colour without fighting the selection highlight (which stores the "real" colour). */
function setMatColor(mat, hex, vertexColors) {
  const vcChanged = mat.vertexColors !== vertexColors;
  mat.vertexColors = vertexColors;
  if (mat.userData.prev) { mat.userData.prev.c = hex; mat.color.setHex(hex).lerp(ACCENT, mat.userData.prev.l ?? 0.7); }
  else mat.color.setHex(hex);
  if (vcChanged) mat.needsUpdate = true;
}

/**
 * Apply colour mode to a document root.
 * mode: 'file' | 'auto' | 'uniform'. Per-object overrides (obj.userData.colorOverride) win.
 */
export function applyColors(root, mode, uniformHex) {
  captureOriginal(root);
  let i = 0;
  const walk = (o, override) => {
    const ov = o.userData.colorOverride ?? override;
    if (isModelMesh(o)) {
      const mats = Array.isArray(o.material) ? o.material : [o.material];
      const orig = o.userData.orig;
      mats.forEach((mat, k) => {
        if (ov != null) setMatColor(mat, ov, false);
        else if (mode === 'auto') setMatColor(mat, new THREE.Color(AUTO_PALETTE[i % AUTO_PALETTE.length]).getHex(), false);
        else if (mode === 'uniform') setMatColor(mat, new THREE.Color(uniformHex).getHex(), false);
        else setMatColor(mat, orig[k].c, orig[k].v);
      });
      i++;
    }
    for (const c of o.children) if (!c.userData.isOverlay) walk(c, ov);
  };
  walk(root, null);
}

export function setOverride(obj, hex) {
  obj.userData.colorOverride = hex == null ? null : new THREE.Color(hex).getHex();
  // overrides on descendants are replaced by the new parent colour
  obj.traverse((o) => { if (o !== obj) o.userData.colorOverride = null; });
}

/** Representative colour of an object (first body), as CSS hex. */
export function colorOf(obj) {
  let hex = null;
  obj.traverse((m) => {
    if (hex !== null || !isModelMesh(m)) return;
    const mat = Array.isArray(m.material) ? m.material[0] : m.material;
    const c = mat.userData.prev ? new THREE.Color(mat.userData.prev.c) : mat.color;
    hex = mat.vertexColors && !mat.userData.prev ? '#c7cdd6' : '#' + c.getHexString();
  });
  return hex || '#c7cdd6';
}
