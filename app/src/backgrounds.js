// Background presets. The WebGL canvas is transparent, so the background is plain CSS behind it:
// zero render cost, and the glass panels refract both the model and the background.
export const BACKGROUNDS = [
  { id: 'studio', name: 'Studio', dark: false,
    css: 'radial-gradient(120% 90% at 50% 30%, #ffffff 0%, #eceef2 42%, #c8cdd5 100%)' },
  { id: 'cad', name: 'Classic CAD', dark: true,
    css: 'linear-gradient(180deg, #3e5f8a 0%, #7f9cc0 55%, #dfe7f1 100%)' },
  { id: 'aurora', name: 'Aurora', dark: true,
    css: 'radial-gradient(60% 70% at 15% 20%, #2bd2c5 0%, transparent 60%), radial-gradient(55% 65% at 85% 25%, #8a5cff 0%, transparent 62%), radial-gradient(70% 70% at 60% 100%, #ff5fa2 0%, transparent 60%), #10142a' },
  { id: 'night', name: 'Night', dark: true,
    css: 'radial-gradient(110% 90% at 50% 25%, #3b3f48 0%, #25282e 55%, #16181c 100%)' },
  { id: 'midnight', name: 'Midnight', dark: true,
    css: 'radial-gradient(110% 90% at 50% 25%, #2c3554 0%, #151a2b 55%, #07090f 100%)' },
  { id: 'sunset', name: 'Sunset', dark: false,
    css: 'linear-gradient(160deg, #ffd6b0 0%, #ffb3c7 45%, #c9b8ff 100%)' },
  { id: 'mint', name: 'Mint', dark: false,
    css: 'radial-gradient(100% 80% at 30% 20%, #f3fffb 0%, #c8efe2 50%, #9fd6c9 100%)' },
  { id: 'paper', name: 'Paper', dark: false, css: '#f5f5f7' },
  { id: 'graphite', name: 'Graphite', dark: true, css: '#1d1e22' },
  { id: 'black', name: 'Black', dark: true, css: '#000000' }
];

function luminance(hex) {
  const n = parseInt(hex.slice(1), 16);
  const c = [(n >> 16) & 255, (n >> 8) & 255, n & 255].map((v) => {
    v /= 255;
    return v <= 0.03928 ? v / 12.92 : Math.pow((v + 0.055) / 1.055, 2.4);
  });
  return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2];
}

export function customBackground(a, b, style) {
  const css = style === 'solid' ? a
    : style === 'radial' ? `radial-gradient(110% 90% at 50% 30%, ${a} 0%, ${b} 100%)`
    : `linear-gradient(180deg, ${a} 0%, ${b} 100%)`;
  // theme follows the top of the screen, where the toolbars sit
  const dark = luminance(a) < 0.35;
  return { id: 'custom', name: 'Custom', css, dark, a, b, style };
}
