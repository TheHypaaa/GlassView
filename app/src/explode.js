// Exploded view: sub-assemblies (or single parts) move away from the assembly centre.
// The offset is applied to the objects' positions (so exports, measurements and .gva files
// see the positions on screen) and removed exactly when the slider goes back to 0.
import * as THREE from 'three';
import { logicalChildren } from './assembly.js';

export class Explode {
  constructor(viewer) { this.viewer = viewer; this.f = 0; this.applied = new Map(); }

  /** The level that is exploded: below single-child wrappers, the first level with several items. */
  groupsOf(root) {
    let top = root;
    for (let i = 0; i < 20; i++) {
      const kids = logicalChildren(top);
      if (kids.length === 1 && !kids[0].isMesh) top = kids[0]; else break;
    }
    let groups = logicalChildren(top).filter((o) => o.visible);
    if (groups.length === 1 && !groups[0].isMesh) groups = logicalChildren(groups[0]).filter((o) => o.visible);
    return groups;
  }

  /** f in 0…1. */
  apply(root, f) {
    const groups = this.groupsOf(root);
    // remove current offsets first (base positions)
    this.reset(false);
    if (f <= 0 || groups.length < 2 && !(groups.length === 1)) { this.f = 0; this.viewer.requestRender(); return; }
    root.updateMatrixWorld(true);
    const all = new THREE.Box3();
    const centres = groups.map((g) => { const b = this.viewer.boxOf(g); all.union(b); return b.isEmpty() ? null : b.getCenter(new THREE.Vector3()); });
    if (all.isEmpty()) return;
    const ac = all.getCenter(new THREE.Vector3()), diag = all.getSize(new THREE.Vector3()).length();
    groups.forEach((g, i) => {
      if (!centres[i]) return;
      const v = centres[i].clone().sub(ac);
      if (v.length() < diag * 0.02) v.set(0, 0, diag * 0.25); // centred part: lift it
      const world = v.multiplyScalar(1.4 * f);
      // world offset → parent space
      const parent = g.parent;
      const inv = new THREE.Matrix4().copy(parent.matrixWorld).invert();
      const local = world.clone().transformDirection(inv).multiplyScalar(world.length() / Math.max(1e-12, world.clone().transformDirection(inv).length()));
      const s = new THREE.Vector3();
      parent.matrixWorld.decompose(new THREE.Vector3(), new THREE.Quaternion(), s);
      local.divideScalar(Math.max(1e-12, (s.x + s.y + s.z) / 3));
      g.position.add(local);
      this.applied.set(g, local);
    });
    root.updateMatrixWorld(true);
    this.f = f;
    this.viewer.requestRender();
  }

  /** Puts everything back where it was. */
  reset(render = true) {
    for (const [g, local] of this.applied) g.position.sub(local);
    this.applied.clear();
    this.f = 0;
    if (render) this.viewer.requestRender();
  }
}
