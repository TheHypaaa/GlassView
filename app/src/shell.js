// Windows integration for the WebView build: Explorer 3D preview (GlassViewPreview.dll),
// update check against GitHub releases, PNG export of the 3D view.
import * as THREE from 'three';
import * as NL from '@neutralinojs/lib';

// set at build time from neutralino.config.json (see vite.config.js)
export const APP_VERSION = typeof __APP_VERSION__ !== 'undefined' ? __APP_VERSION__ : 'dev';
const native = typeof window.NL_PORT !== 'undefined';
const appDir = () => (window.NL_PATH || '.').replace(/[\\/]+$/, '');
const win = (p) => p.replace(/\//g, '\\');

// ------------------------------------------------------------------ Explorer preview pane
const CLSID = '{7A3F1C2E-5B8D-4E61-9C07-2D4B6A8E91F3}';
export const previewDll = () => `${appDir()}/GlassViewPreview.dll`;

export async function previewDllPresent() {
  if (!native) return false;
  try { await NL.filesystem.getStats(previewDll()); return true; } catch (e) { return false; }
}

/** True when Explorer uses this copy of the DLL (per-user or all-users registration). */
export async function previewRegistered() {
  if (!native || window.NL_OS !== 'Windows') return false;
  try {
    const r = await NL.os.execCommand(`reg query "HKCR\\CLSID\\${CLSID}\\InprocServer32" /ve`);
    return (r.stdOut || '').toLowerCase().includes(win(previewDll()).toLowerCase());
  } catch (e) { return false; }
}

/** True when the setup registered the preview for all users (then only the setup can change it). */
export async function previewMachineWide() {
  if (!native || window.NL_OS !== 'Windows') return false;
  try {
    const r = await NL.os.execCommand(`reg query "HKLM\\Software\\Classes\\CLSID\\${CLSID}\\InprocServer32" /ve`);
    return r.exitCode === 0 && (r.stdOut || '').toLowerCase().includes(win(previewDll()).toLowerCase());
  } catch (e) { return false; }
}

/** The DLL registers itself for the current user only (HKCU): no admin rights needed. */
export async function registerPreview(on) {
  const r = await NL.os.execCommand(`regsvr32 /s ${on ? '' : '/u '}"${win(previewDll())}"`);
  return r.exitCode === 0;
}

// ------------------------------------------------------------------ updates (GitHub releases)
function newer(tag, cur) {
  const p = (s) => (String(s).match(/\d+/g) || []).map(Number);
  const a = p(tag), b = p(cur);
  for (let i = 0; i < Math.max(a.length, b.length); i++) {
    const x = a[i] || 0, y = b[i] || 0;
    if (x !== y) return x > y;
  }
  return false;
}

/** → { latest, url, newer } or throws with a readable message. */
export async function checkUpdate(repo) {
  repo = String(repo || '').trim().replace(/^https?:\/\/github\.com\//, '').replace(/\/+$/, '');
  if (!/^[\w.-]+\/[\w.-]+$/.test(repo)) throw new Error('enter the repository as owner/repo');
  const res = await fetch(`https://api.github.com/repos/${repo}/releases/latest`, { headers: { Accept: 'application/vnd.github+json' } });
  if (res.status === 404) throw new Error('no release found');
  if (!res.ok) throw new Error(`GitHub answered ${res.status}`);
  const j = await res.json();
  return { latest: j.tag_name, url: j.html_url, newer: newer(j.tag_name, APP_VERSION) };
}

export async function openUrl(url) {
  if (native) { try { await NL.os.open(url); return; } catch (e) { /* fall through */ } }
  window.open(url, '_blank');
}

// ------------------------------------------------------------------ PNG export of the view
/**
 * Renders the scene off-screen at `factor` × the canvas size (no interface, no view cube)
 * and returns PNG bytes.
 */
export async function renderPng(viewer, factor = 2) {
  // render to the real canvas at a higher pixel ratio (same tone mapping and colours as on screen),
  // copy it in the same task, then restore the normal size
  const r = viewer.renderer;
  const pr = r.getPixelRatio();
  const max = r.capabilities.maxTextureSize || 8192;
  const size = r.getSize(new THREE.Vector2());
  const f = Math.max(1, Math.min(factor * pr, max / Math.max(size.x, size.y)));
  r.setPixelRatio(f);
  r.setSize(size.x, size.y, false);
  r.setClearColor(0x000000, 0);
  r.clear();
  r.render(viewer.scene, viewer.camera);
  const W = r.domElement.width, H = r.domElement.height;
  const cv = new OffscreenCanvas(W, H);
  const g = cv.getContext('2d');
  const grad = g.createLinearGradient(0, 0, 0, H);
  const dark = document.documentElement.dataset.theme === 'dark';
  grad.addColorStop(0, dark ? '#3b3f48' : '#ffffff');
  grad.addColorStop(1, dark ? '#16181c' : '#c8cdd5');
  g.fillStyle = grad;
  g.fillRect(0, 0, W, H);
  g.drawImage(r.domElement, 0, 0);
  r.setPixelRatio(pr);
  r.setSize(size.x, size.y, false);
  viewer.requestRender();
  const blob = await cv.convertToBlob({ type: 'image/png' });
  return new Uint8Array(await blob.arrayBuffer());
}
