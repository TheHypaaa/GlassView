// Customisable keyboard shortcuts: an action registry, a keymap stored in localStorage,
// the global key handler and the editor sheet.
const LS = 'gv.keys';
const MODS = ['Control', 'Shift', 'Alt', 'Meta', 'AltGraph', 'CapsLock'];

/** Normalised combo from a keyboard event, e.g. "Ctrl+Shift+Tab", "M", "Delete". */
export function comboOf(e) {
  if (MODS.includes(e.key)) return null;
  let k;
  if (/^Digit\d$/.test(e.code)) k = e.code.slice(5);
  else if (/^Numpad\d$/.test(e.code)) k = 'Num' + e.code.slice(6);
  else if (e.key === ' ') k = 'Space';
  else if (e.key.length === 1) k = e.key.toUpperCase();
  else k = e.key === 'Esc' ? 'Escape' : e.key;
  const parts = [];
  if (e.ctrlKey || e.metaKey) parts.push('Ctrl');
  if (e.altKey) parts.push('Alt');
  if (e.shiftKey) parts.push('Shift');
  parts.push(k);
  return parts.join('+');
}

export const prettyCombo = (c) => (c ? c.replace(/\+/g, ' + ').replace('Delete', 'Del').replace('Escape', 'Esc').replace('ArrowUp', '↑').replace('ArrowDown', '↓').replace('ArrowLeft', '←').replace('ArrowRight', '→') : '—');

export class Keymap extends EventTarget {
  /** actions: [{ id, label, group, key, run(e), fixed? }] */
  constructor(actions) {
    super();
    this.actions = actions;
    this.byId = new Map(actions.map((a) => [a.id, a]));
    this.custom = {};
    try { this.custom = JSON.parse(localStorage.getItem(LS) || '{}'); } catch (e) { this.custom = {}; }
    this.capture = null;
    this._rebuild();
  }

  keyOf(id) { return id in this.custom ? this.custom[id] : this.byId.get(id)?.key || ''; }

  _rebuild() {
    this.map = new Map();
    for (const a of this.actions) { const k = this.keyOf(a.id); if (k) this.map.set(k, a); }
    this.dispatchEvent(new Event('change'));
  }

  _save() { try { localStorage.setItem(LS, JSON.stringify(this.custom)); } catch (e) { /* ignore */ } }

  /** Assign a combo; returns the action that lost it (if any). */
  assign(id, combo) {
    let lost = null;
    if (combo) {
      const other = this.map.get(combo);
      if (other && other.id !== id) { this.custom[other.id] = ''; lost = other; }
    }
    this.custom[id] = combo || '';
    if (this.custom[id] === (this.byId.get(id).key || '')) delete this.custom[id];
    this._save();
    this._rebuild();
    return lost;
  }

  resetAll() { this.custom = {}; this._save(); this._rebuild(); }
  isCustom(id) { return id in this.custom; }

  /** Global handler: returns true if the event was consumed. */
  handle(e) {
    if (this.capture) { this.capture(e); return true; }
    const t = e.target;
    if (t && (t.tagName === 'INPUT' || t.tagName === 'SELECT' || t.tagName === 'TEXTAREA')) {
      if (e.key === 'Escape') t.blur();
      return false;
    }
    const c = comboOf(e);
    if (!c) return false;
    const a = this.map.get(c);
    if (!a) return false;
    e.preventDefault();
    a.run(e);
    return true;
  }
}

/** Overview grid (settings): every bound action plus the fixed mouse controls. */
export function renderKeyGrid(el, km, mouse) {
  el.innerHTML = '';
  const add = (k, label) => {
    const d = document.createElement('div');
    d.innerHTML = '<kbd></kbd><span></span>';
    d.firstChild.textContent = k;
    d.lastChild.textContent = label;
    el.appendChild(d);
  };
  for (const [k, l] of mouse) add(k, l);
  for (const a of km.actions) { const k = km.keyOf(a.id); if (k) add(prettyCombo(k), a.short || a.label); }
}

/** Editor sheet. */
export function openKeySheet(km, { toast, refreshGlass }) {
  let sheet = document.getElementById('keySheet');
  if (!sheet) {
    sheet = document.createElement('div');
    sheet.id = 'keySheet';
    sheet.className = 'sheet-wrap';
    sheet.innerHTML = `<div class="glass sheet" data-bezel="20">
      <div class="sheet-head"><span class="tp-title">Keyboard shortcuts</span>
        <input class="ksearch" placeholder="Search…" />
        <button class="btn txt pill" data-act="reset">Reset all</button>
        <button class="btn txt pill primary" data-act="done">Done</button></div>
      <p class="hint">Click a shortcut and press the new key combination. Esc cancels, × removes it.
        Mouse: left drag rotate · right/middle drag pan · wheel zoom · double-click sets the pivot ·
        in Measure hold <kbd>Shift</kbd> or <kbd>Alt</kbd> to pick faces only.</p>
      <div class="klist"></div></div>`;
    document.body.appendChild(sheet);
    sheet.addEventListener('pointerdown', (e) => { if (e.target === sheet) close(); });
    sheet.querySelector('[data-act="done"]').addEventListener('click', () => close());
    sheet.querySelector('[data-act="reset"]').addEventListener('click', () => { km.resetAll(); render(); toast('Shortcuts reset to defaults', false, 1600); });
    sheet.querySelector('.ksearch').addEventListener('input', () => render());
  }
  const list = sheet.querySelector('.klist');
  function close() { stopCapture(); sheet.classList.remove('open'); }
  let capBtn = null;
  function stopCapture() { km.capture = null; if (capBtn) capBtn.classList.remove('rec'); capBtn = null; }
  function render() {
    const q = sheet.querySelector('.ksearch').value.trim().toLowerCase();
    list.innerHTML = '';
    let group = null;
    for (const a of km.actions) {
      if (q && !a.label.toLowerCase().includes(q) && !(a.group || '').toLowerCase().includes(q)) continue;
      if (a.group !== group) {
        group = a.group;
        const h = document.createElement('div');
        h.className = 'kgroup';
        h.textContent = group;
        list.appendChild(h);
      }
      const row = document.createElement('div');
      row.className = 'krow';
      row.innerHTML = '<span class="kl"></span><button class="kchip"></button><button class="kx" title="Remove shortcut">×</button>';
      row.querySelector('.kl').textContent = a.label;
      const chip = row.querySelector('.kchip');
      chip.textContent = prettyCombo(km.keyOf(a.id));
      chip.classList.toggle('custom', km.isCustom(a.id));
      chip.classList.toggle('none', !km.keyOf(a.id));
      if (a.fixed) { chip.disabled = true; row.querySelector('.kx').remove(); row.classList.add('fixed'); }
      chip.addEventListener('click', () => {
        stopCapture();
        capBtn = chip;
        chip.classList.add('rec');
        chip.textContent = 'Press keys…';
        km.capture = (e) => {
          if (MODS.includes(e.key)) return;
          e.preventDefault();
          e.stopPropagation();
          if (e.key === 'Escape' && !e.ctrlKey && !e.shiftKey && !e.altKey) { stopCapture(); render(); return; }
          const c = comboOf(e);
          stopCapture();
          const fixed = km.actions.find((x) => x.fixed && km.keyOf(x.id) === c);
          if (fixed) { toast(`${prettyCombo(c)} is reserved for “${fixed.label}”`, true, 2600); render(); return; }
          const lost = km.assign(a.id, c);
          if (lost) toast(`${prettyCombo(c)} moved from “${lost.label}” to “${a.label}”`, false, 3000);
          render();
        };
      });
      row.querySelector('.kx')?.addEventListener('click', () => { stopCapture(); km.assign(a.id, ''); render(); });
      list.appendChild(row);
    }
  }
  render();
  sheet.classList.add('open');
  refreshGlass(sheet.querySelector('.sheet'));
  return { close };
}
