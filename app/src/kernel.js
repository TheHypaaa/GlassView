// Client for gvkernel.exe, the native OpenCASCADE engine that ships next to GlassView.exe.
// The process is started on demand and kept alive; requests/replies are JSON lines over
// stdin/stdout, bulky results are files in GlassView's cache format.
import * as NL from '@neutralinojs/lib';

const nativeApp = typeof window.NL_PORT !== 'undefined';
let proc = null;
let starting = null;
let failed = false;
let info = null;
let buf = '';
let seq = 0;
let readyResolve = null;
let listening = false;
const pending = new Map();
const progressCbs = new Map(); // id -> (stage, pct) while a request runs

function onEvent(e) {
  const d = e.detail;
  if (!proc || d.id !== proc.id) return;
  if (d.action === 'stdOut') {
    buf += d.data;
    let i;
    while ((i = buf.indexOf('\n')) >= 0) {
      const line = buf.slice(0, i).trim();
      buf = buf.slice(i + 1);
      if (!line) continue;
      let msg;
      try { msg = JSON.parse(line); } catch (err) { continue; }
      if (msg.ready) { info = msg; readyResolve?.(msg); }
      else if (msg.progress) { const cb = progressCbs.get(msg.id); if (cb) cb(msg.progress, msg.pct); }
      else if (pending.has(msg.id)) { pending.get(msg.id)(msg); pending.delete(msg.id); progressCbs.delete(msg.id); }
    }
  } else if (d.action === 'exit') {
    proc = null;
    starting = null;
    for (const r of pending.values()) r({ ok: false, error: 'the geometry engine stopped unexpectedly (out of memory or a crash on this file)' });
    pending.clear();
    progressCbs.clear();
  }
}

/** Start the engine if it exists. Resolves to its hello message, or null (use the WebAssembly path). */
export function startKernel() {
  if (!nativeApp || failed) return Promise.resolve(null);
  if (starting) return starting;
  starting = (async () => {
    const dir = (window.NL_PATH || '.').replace(/[\\/]+$/, '');
    const exe = `${dir}/gvkernel.exe`;
    try { await NL.filesystem.getStats(exe); } catch (e) { failed = true; return null; }
    if (!listening) { NL.events.on('spawnedProcess', onEvent); listening = true; }
    const ready = new Promise((r) => { readyResolve = r; });
    try {
      const cmdPath = window.NL_OS === 'Windows' ? exe.replace(/\//g, '\\') : exe;
      proc = await NL.os.spawnProcess(`"${cmdPath}"`, { cwd: dir });
    } catch (e) { failed = true; return null; }
    const msg = await Promise.race([ready, new Promise((r) => setTimeout(() => r(null), 10000))]);
    if (!msg) {
      failed = true;
      try { await NL.os.updateSpawnedProcess(proc.id, 'exit'); } catch (e) { /* ignore */ }
      proc = null;
      return null;
    }
    return msg;
  })();
  return starting;
}

export async function kernelCall(obj, onProgress) {
  const ok = await startKernel();
  if (!ok || !proc) throw new Error('native engine unavailable');
  const id = ++seq;
  const p = new Promise((r) => pending.set(id, r));
  if (onProgress) progressCbs.set(id, onProgress);
  await NL.os.updateSpawnedProcess(proc.id, 'stdIn', JSON.stringify({ ...obj, id }) + '\n');
  const res = await p;
  if (!res.ok) throw new Error(res.error || 'engine error');
  return res;
}

export const kernelInfo = () => info;
export const kernelAvailable = () => !!info && !failed;
