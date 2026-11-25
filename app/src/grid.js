import * as THREE from 'three';

/**
 * Ground grid as a single shaded quad: anti-aliased minor/major lines computed per pixel,
 * minor lines fade out when they get too dense, and the whole grid fades radially so it never
 * shows a hard cut-off edge. X / Y axes are drawn in red / green through the origin.
 */
export function makeGrid() {
  const mat = new THREE.ShaderMaterial({
    transparent: true,
    depthWrite: false,
    side: THREE.DoubleSide,
    uniforms: {
      uMinor: { value: 1 },
      uMajor: { value: 10 },
      uCenter: { value: new THREE.Vector2() },
      uFade: { value: 100 },
      uColor: { value: new THREE.Color(0x5b6472) },
      uOpacity: { value: 1 }
    },
    vertexShader: /* glsl */ `
      varying vec3 vWorld;
      void main() {
        vec4 w = modelMatrix * vec4(position, 1.0);
        vWorld = w.xyz;
        gl_Position = projectionMatrix * viewMatrix * w;
      }`,
    fragmentShader: /* glsl */ `
      uniform float uMinor, uMajor, uFade, uOpacity;
      uniform vec2 uCenter;
      uniform vec3 uColor;
      varying vec3 vWorld;
      float lines(vec2 p, float s, out float density) {
        vec2 c = p / s;
        vec2 fw = fwidth(c);
        density = max(fw.x, fw.y);
        vec2 g = abs(fract(c - 0.5) - 0.5) / max(fw, 1e-6);
        return 1.0 - min(min(g.x, g.y), 1.0);
      }
      float axis(float v) {
        float fw = fwidth(v);
        return 1.0 - min(abs(v) / max(fw * 1.2, 1e-6), 1.0);
      }
      void main() {
        vec2 p = vWorld.xy;
        float dMin, dMaj;
        float minor = lines(p, uMinor, dMin);
        float major = lines(p, uMajor, dMaj);
        minor *= 1.0 - smoothstep(0.08, 0.25, dMin);
        major *= 1.0 - smoothstep(0.25, 0.6, dMaj);
        float fade = 1.0 - smoothstep(uFade * 0.25, uFade, length(p - uCenter));
        float ax = axis(p.y), ay = axis(p.x);
        vec3 col = uColor;
        float a = max(minor * 0.16, major * 0.38);
        if (ax > 0.01) { col = mix(col, vec3(0.95, 0.27, 0.23), ax); a = max(a, ax * 0.85); }
        if (ay > 0.01) { col = mix(col, vec3(0.19, 0.78, 0.35), ay); a = max(a, ay * 0.85); }
        a *= fade * uOpacity;
        if (a < 0.004) discard;
        gl_FragColor = vec4(col, a);
      }`
  });
  const mesh = new THREE.Mesh(new THREE.PlaneGeometry(2, 2), mat);
  mesh.raycast = () => {};
  mesh.renderOrder = -1;
  mesh.frustumCulled = false;
  return mesh;
}

/**
 * spacing: 'auto' or a major spacing in mm; extent: multiplier of the model radius.
 */
export function fitGrid(grid, box, dark, spacing = 'auto', extent = 3.2) {
  const size = box.getSize(new THREE.Vector3());
  const center = box.getCenter(new THREE.Vector3());
  const R = Math.max(size.length() / 2, 1e-3);
  const major = spacing === 'auto' ? Math.pow(10, Math.floor(Math.log10(R))) : +spacing;
  const fade = Math.max(R * extent, major * 4);
  const u = grid.material.uniforms;
  u.uMajor.value = major;
  u.uMinor.value = major / 10;
  u.uCenter.value.set(center.x, center.y);
  u.uFade.value = fade;
  u.uColor.value.set(dark ? 0xaab3c2 : 0x5b6472);
  grid.scale.set(fade, fade, 1);
  grid.position.set(center.x, center.y, box.min.z);
  grid.updateMatrixWorld(true);
  return fade;
}
