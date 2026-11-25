GlassView - fast STEP / mesh viewer for Windows (version 10.5)
===============================================================

New in version 10.5 (big STEP assemblies)
  - Faster STEP reading: GlassView now skips OpenCASCADE's orientation and self-intersection checks,
    which clean exports from CAD systems don't need. Face colours are looked up once per part
    instead of searching the whole file for every face. On an 82 MB 3DEXPERIENCE assembly
    (2,800 bodies, 68,000 faces) reading went from 39 s to 26 s on two cores.
    For a broken file (missing or inside-out faces) turn on Settings > "Thorough geometry repair".
  - Repeated parts (bolts, nuts, the same corner on both sides) are meshed and stored once and
    placed many times: about half the cache size, memory and graphics load on that assembly.
  - Body colours from CATIA / 3DEXPERIENCE (colour on a body rather than on its faces) now show.
  - While a file opens you see what the engine is doing and for how long (Reading, Building the
    geometry, Meshing 45 %, ...). If the engine fails on a big file you now get its error message
    instead of a long silent fallback that showed nothing.
  - Files already in the cache are read once more on first open, because the cache format changed.

New in version 10.4
  - Double-clicking a 3D file while GlassView is open now opens it as a new tab in that window and
    brings it to the front. Before, a second GlassView started, often exactly on top of the first,
    so the open model seemed to vanish. Double-clicks go through the small GlassViewOpen.exe
    (installed by the setup / Install.cmd). Settings > Windows > "Open files in this window" turns this off.
  - Explorer preview: uses Explorer's own background, text colour and font (light and dark), no
    grid, no card; just the size in the corner (and the number of bodies for assemblies).
    How-to hint only while the mouse is over it.
  - Explorer preview navigation now feels like the app: same zoom per wheel notch (it was 3x
    faster), zoom towards the mouse, rotation like the app, and it follows your Zoom speed,
    Rotate speed and Reverse scroll-wheel settings (saved to %LOCALAPPDATA%\GlassView\view.ini).

New in version 10.3
  - Space hides the selected part (or the one under the mouse). Shift+Space shows all hidden parts;
    press it again to hide them again. The eye button in the model tree header does the same.
    "Show all parts" is in the right-click menu and can get its own key (Settings > Keyboard).
  - The model tree is resizable: drag its right edge; double-click the edge for the default width.
    The info bar below follows.
  - Part colours (File / Auto / Uniform) now belong to each tab; new tabs start with File.
  - Open panel: format filter chips (Folders, STEP, IGES, STL, 3MF, ...; Ctrl+click to combine),
    the selected card is highlighted properly, and a file without a preview says why when you hover
    it (also logged to %LOCALAPPDATA%\GlassView\cache\thumbs\errors.log).
  - Fixed: the exploded-view slider (and others) showed a half-filled track at 0 %.

New in version 10.2
  - Setup wizard: GlassView-Setup-10.5.0.exe. Install only for you (no admin) or for everyone on
    the PC, desktop shortcut, "Open with" for all 3D file types, make GlassView the default app,
    3D preview in Explorer. Uninstall from Settings > Apps. Run the setup again to update or to
    change the options; it remembers your choices. Silent: GlassView-Setup-10.5.0.exe /S /AllUsers
    The zip with GlassView.exe stays available as the portable version.
  - Default app: Windows lets no program make itself the default. For everyone, the setup uses
    the Windows "default associations" policy (STEP, IGES, BREP, STL, .gva). It is applied at
    every sign-in, so it also switches back if someone picks another app. Just for you, the setup
    opens Settings > Default apps at the end: click "Set default".
  - New Open panel (Ctrl+O, Open, Insert part, Compare): recent files and folders with 3D
    thumbnails and sizes, Desktop / Documents / Downloads, pinned project folders, search through
    a folder and its subfolders, Name / Date sorting, keyboard navigation (arrows, Enter,
    Backspace = up, Ctrl+A). "Browse…" opens the Windows dialog for network drives, USB sticks etc.
    Making a thumbnail also tessellates the file, so opening it afterwards is instant. Thumbnails
    are made for STEP / IGES up to 25 MB and meshes up to 80 MB.
    Settings > "Open files with the GlassView browser" switches back to the Windows dialog.
  - The start screen shows thumbnails next to the recent files.

New in version 10.1
  - Measure: the "Faces & axes / Points" tabs are gone. Faces and axes are picked as before;
    hold Ctrl for a free point on the exact surface.
  - Angles are drawn where they are: an orange arc and wedge at the edge where the two faces (or
    axes) meet, with a ray running into each face. A right angle gets the square corner mark. The
    value is the angle the arc shows; the other side is given underneath.
  - Tooltips no longer cover the control or run off the window.
  - Settings: Zoom speed and Rotate speed sliders (Reset goes back to 1x). Default zoom, rotation
    and inertia are back to the earlier versions' feel.
  - Section: dragging the arrow no longer jitters; the slider range no longer shifts while you work.
  - Section snapping: hold Shift while dragging the slider or the arrow to snap to planar faces,
    cylinder / cone axes and sphere centres. Shift+click a face to put the cut on it (on a
    cylinder: through its axis, facing you).

New in version 10
  - Appearance is just Light / Dark / Auto (Auto follows Windows); the colour themes are gone.
  - The info bar has the same width as the model tree above it.
  - Files opened from Explorer go into the GlassView window that is already open, as a new tab
    (Settings > Windows > Open files in this window).
  - Explorer 3D preview: Settings > Windows > "3D preview in Explorer". Press Alt+P in Explorer and
    select a STEP / IGES / STL file: rotate, pan, zoom right in the preview pane. Keep
    GlassViewPreview.dll next to GlassView.exe (Install.cmd copies it).
  - PMI (D, "PMI" button): dimensions, GD&T and datums from STEP AP242 files with their 3D annotation
    lines; hover an entry or a label to highlight its faces (needs gvkernel.exe).
  - Exploded view (V): slider, sub-assemblies move apart from the centre.
  - Compare versions (Ctrl+Shift+C): colours this model by its distance to another file (red added,
    blue removed, green within tolerance), largest / mean deviation, hover read-out.
  - Export as 3MF (right-click): watertight meshes with colours and the current positions (moves,
    exploded view). AM check: click a face to put it on the build plate; "Export 3MF in this
    orientation" turns the build direction up and sits the part on z = 0.
  - Export view as PNG (Ctrl+Shift+E, right-click) at 1x / 2x / 4x (Settings > Windows).
  - Updates: Settings > Updates checks a GitHub releases page (owner/repo) weekly.
  - Fixed: Export STEP / STL / GLB, Save assembly and Copy values were blocked by the app's
    permission list in version 9.

Quick start
  - Run GlassView.exe (portable, single file), or
  - Run Install.cmd to install for your user (no admin needed): Start Menu entry and
    "Open with > GlassView" for .stp .step .igs .iges .brep .stl .obj .ply .3mf .glb .gltf .gva
  - Needs the Microsoft Edge WebView2 runtime (preinstalled on Windows 11 / current Windows 10).

Tabs (bottom of the window)
  Every opened file gets its own tab (+ button, Ctrl+O). Ctrl+Tab switches, Ctrl+W closes,
  middle-click on a tab closes it.

Drag & drop
  Drag files onto the window and drop them on
    "Open in new tab"   - one tab per file, or
    "Add to assembly"   - inserts the parts into the current tab.

Part colors (K, palette button)
  Color by File (colors from the STEP), Auto (a distinct color per part) or Uniform.
  Select a part (or click the color dot in the tree) to give it its own color; Reset undoes it.

Parts and selection
  Hovering a part tints it lightly, shows its name next to the cursor and marks its row in the
  tree (hovering a tree row tints the part). Clicking selects it (stronger tint).
  The view opens in orthographic projection; P switches to perspective.
  Colour swatches and Auto colours use a vivid palette.

Light / dark (B, half-moon button)
  Light, Dark or Auto (follows the Windows light/dark setting); lighting, edges, grid and the UI adapt.

Grid
  Settings > Ground grid: spacing (Auto / 1 / 10 / 100 mm) and size.

Assembly (Move tool, G)
  Insert parts with the Insert button (Ctrl+I) or by dropping them on "Add to assembly".
  Select a part (click it or pick it in the tree): arrows move it, rings rotate it - both at
  once, or type translation (mm) and rotation (deg), about the part's own centre.
  Snapping: Magnetic (default) pulls angles to 90/45/15/5 deg and to angles you used before,
  more strongly when you drag fast and barely when you drag slowly; moves snap to round
  values that match the zoom level. Step = fixed increments. Off = free.
  Hold Shift for fixed steps, Ctrl for free movement. A live readout follows the cursor.
  World/Local axes,
  "Select parent" (arrow) to move a whole sub-assembly, "Reset part" to undo its moves.
  Save assembly... writes a small .gva file (file paths + positions); open it again later.
  Parts added by drag & drop have no file path and are skipped in .gva - use Insert for those,
  or use Export GLB, which stores the complete geometry in one file.

Measure (M) - on the exact CAD geometry
  Faces & axes mode: planes pick as planes, cylinders and cones as their axis, spheres as their
  centre. Points mode always picks a point on the exact surface.
  Snap points (chips in the panel switch each kind on/off, for display AND picking):
    Corners (white), Midpoints of straight edges (teal), Centres of holes/bosses/arcs (orange),
    Face centres (pink - area centre of planar faces, axis middle of cylinders), Axes (dashed
    purple, of the part under the cursor - move near one to pick it), Fillets (axes and arc
    centres of fillets/rounds: faces covering less than 180 deg; off by default).
  Hold Shift or Alt over a face to LOCK onto it: the face turns blue and only its own points
  can be picked - its corners, edge midpoints, any point along its edges, hole/arc centres on
  it and its face centre - or the face itself, wherever the cursor is. Release to unlock.
  What you are about to pick is always previewed (filled + outlined face, thick axis, reticle
  per type, a coloured chip naming it). Your first pick turns orange.
  Cylinders: each measurement involving a cylinder has Centre / Near side / Far side buttons
  per cylinder in the list (centre-to-centre, outside-to-outside, outside-to-inside ...).
  X / Y / Z components: every distance shows dX, dY, dZ in the list; switch on "X / Y / Z
  components" to draw the red/green/blue legs with their values in the view.
  Result labels can be dragged anywhere (a leader line connects them); double-click a label
  to put it back.
  Units: mm or inch in the Measure panel (same switch as Settings > Units, shortcut I).

Refine when zooming (Settings, on by default)
  Like CAD, parts you zoom into are re-tessellated in the background so curved edges stay
  smooth (chord error below about half a pixel), within a triangle budget.

Mass properties (W, scale button)
  Volume, surface area, mass, centre of gravity (orange marker in the view), inertia tensor
  about the CoG and principal moments for the selected part, or the whole model if nothing
  is selected. Pick a material preset or type a density. With the native engine the values
  are computed on the exact B-rep ("Exact · B-rep"); meshes and fallback-engine parts use
  the triangles ("From mesh"). Copy values puts a tab-separated table on the clipboard.

AM check (A, layer button)
  Build direction +/-X/Y/Z. Overhang map: red faces need support (steeper than the limit,
  20-70 deg), amber is borderline, blue sits on the build plate. Wall thickness map: red is
  below your minimum wall, green is 3x or more. Stats: build height, area needing support,
  projected support footprint, area on the plate, thinnest wall. "Suggest build orientation"
  tests 126 directions (support area vs. build height, axis-aligned set-ups win ties) and
  lists the best four; click one to apply it. These are quick geometric estimates, not a
  slicer or process simulation.

Right-click menu (tree rows and parts in the view)
  Export as STEP... - writes the exact B-rep with names, colours and your assembly moves
    (needs CAD parts opened with the native engine).
  Export as STL... - binary STL in world coordinates (exact parts are re-meshed finely,
    meshes are written as they are).
  Plus Mass properties, AM check, Color, Fit to view, Isolate, Show all.

Settings
  Show on screen: view cube, model info bar, part names & highlight on hover, measurement
  labels & lines, tab bar, ground grid.
  Edges: "Sharp / tangent" (default) draws sharp edges black and tangent-continuous edges
  (fillet boundaries etc.) in soft grey, like CATIA / 3DEXPERIENCE; "Uniform" draws all alike.
  Shortcut E toggles it.
  Units: length mm / inch (measure, mass properties, AM check, info chip) and density
  g/cm3, kg/m3, lb/in3 or lb/ft3 (mass properties).
  Keyboard shortcuts: the list in Settings shows every current shortcut; "Customize..." opens
  an editor - click a shortcut, press the new key combination (Esc cancels, x removes it).
  If the key was in use, it moves to the new command and you are told which one lost it.
  "Reset all" restores the defaults.

Section cut (C)
  Cut along X, Y, Z or parallel to the screen ("View"). Drag the blue arrow in the view,
  the slider, or type the position in mm. Flip the side, Caps fills the cut faces.

View
  View cube: click faces, edges or corners; click the face you are looking at to flip;
  drag it to orbit. XY/YZ/ZX buttons (again = opposite side), ISO cycles, H = home.
  Left drag rotate, right/middle pan, wheel zooms towards the cursor,
  double-click sets the rotation pivot.

Panels & window sizes
  The toolbar compacts and wraps onto two rows in narrow windows; panels scale with the window.
  Tool panels: ^ collapses, x closes. Tree: < hides (or T). Click the info chip to shrink it.
  U hides/shows all panels at once.
  Settings > Reverse scroll-wheel zoom.

Keys
  (all customisable - see Settings) W mass  A AM check  E edge style  I mm/inch  7/8/9 XY/YZ/ZX  F fit  H home  0 iso  1-4 shaded/edges/wire/mesh  X x-ray  P persp/ortho  B background
  T tree  M measure  C cut  G move/rotate  K colors  U hide panels  Esc cancel/deselect

Native geometry engine (gvkernel.exe)
  Keep gvkernel.exe next to GlassView.exe (Install.cmd copies both). It is OpenCASCADE compiled
  natively: STEP/IGES are read and meshed on all CPU cores (about 2.8x faster than the built-in
  engine even on 2 cores, much more on typical 8-16-thread CPUs), surface data (planes, cylinders,
  cones, spheres, tori) comes straight from the B-rep, and zooming in re-meshes only the faces you
  look at. Settings shows which engine is active. Without the file, GlassView uses the built-in
  WebAssembly engine as before.

Performance notes
  - STEP/IGES are tessellated by OpenCASCADE in a background thread. Settings > tessellation
    (Coarse / Normal / Fine / Ultra) re-tessellates the open model immediately.
  - Tessellations are cached in %LOCALAPPDATA%\GlassView\cache; reopening is near-instant.
  - Weak GPU? Settings > Glass effect > Frosted or Solid.

Uninstall: run Uninstall.cmd.
Third-party: three.js (MIT), three-mesh-bvh (MIT), OpenCASCADE (LGPL-2.1 + OCCT exception; native
engine source & build recipe in the source package), occt-import-js (LGPL-2.1),
Neutralinojs (MIT).
