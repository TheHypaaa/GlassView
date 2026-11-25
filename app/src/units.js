// Display units. Geometry is always millimetres / grams internally; only formatting changes.
const LS = 'gv.units';
let state = { length: 'mm', density: 'g/cm3' };
try { Object.assign(state, JSON.parse(localStorage.getItem(LS) || '{}')); } catch (e) { /* ignore */ }

export const units = new EventTarget();
export const getUnits = () => ({ ...state });
export function setUnits(patch) {
  Object.assign(state, patch);
  try { localStorage.setItem(LS, JSON.stringify(state)); } catch (e) { /* ignore */ }
  units.dispatchEvent(new Event('change'));
}

export const LENGTH_UNITS = { mm: { label: 'mm', f: 1, dec: 3 }, in: { label: 'in', f: 1 / 25.4, dec: 4 } };
/** factor from g/cm³ */
export const DENSITY_UNITS = {
  'g/cm3': { label: 'g/cm³', f: 1, dec: 3 },
  'kg/m3': { label: 'kg/m³', f: 1000, dec: 0 },
  'lb/in3': { label: 'lb/in³', f: 0.0361272920, dec: 5 },
  'lb/ft3': { label: 'lb/ft³', f: 62.4279606, dec: 2 }
};

const fix = (v, d) => (Math.abs(v) < 0.5 * 10 ** -d ? (0).toFixed(d) : v.toFixed(d));
const imperial = () => state.length === 'in';

/** Length in mm → "12.000 mm" / "0.4724 in". */
export function fLen(mm, withUnit = true, dec) {
  const u = LENGTH_UNITS[state.length];
  const s = fix(mm * u.f, dec ?? u.dec);
  return withUnit ? `${s} ${u.label}` : s;
}
export const lenUnit = () => LENGTH_UNITS[state.length].label;
export function fArea(mm2, dec) { return imperial() ? `${fix(mm2 / 645.16, dec ?? 3)} in²` : `${fix(mm2 / 100, dec ?? 2)} cm²`; }
export function fVol(mm3, dec) { return imperial() ? `${fix(mm3 / 16387.064, dec ?? 4)} in³` : `${fix(mm3 / 1000, dec ?? 3)} cm³`; }
export function fMass(g) {
  if (imperial()) { const lb = g / 453.59237; return lb < 1 ? `${fix(lb * 16, 3)} oz` : `${fix(lb, 4)} lb`; }
  return g >= 1000 ? `${fix(g / 1000, 4)} kg` : `${fix(g, 2)} g`;
}
/** Inertia from g·mm² → kg·mm² or lb·in². */
export function inertiaFactor() { return imperial() ? 1 / (453.59237 * 645.16) : 1 / 1000; }
export const inertiaUnit = () => (imperial() ? 'lb·in²' : 'kg·mm²');

export const densityUnit = () => DENSITY_UNITS[state.density];
export const toDensityUnit = (gcm3) => gcm3 * densityUnit().f;
export const fromDensityUnit = (v) => v / densityUnit().f;
export function fDensity(gcm3) { const u = densityUnit(); return `${fix(gcm3 * u.f, u.dec)} ${u.label}`; }
