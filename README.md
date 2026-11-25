# GlassView

A fast viewer for STEP, IGES, BREP, STL, 3MF, OBJ, PLY and glTF files on Windows, with a liquid-glass interface,
measuring, sections, exploded views, mass properties, PMI, model comparison, an AM print check and a 3D preview
in the Windows Explorer preview pane.

## What is in this repository

| Folder | What it is |
|---|---|
| `app/` | The app: a [Neutralino](https://neutralino.js.org) window (Edge WebView2) with a three.js viewer. `app/README.txt` is the user guide shipped with every build. |
| `kernel/` | `gvkernel.exe`, the native geometry engine. It uses [OpenCASCADE](https://github.com/Open-Cascade-SAS/occt) to read STEP / IGES / BREP, mesh them, and compute exact measurements, mass properties and PMI. The app talks to it as a child process (JSON lines on stdin / stdout). |
| `native/` | GlassView Native, the pure C++ version (GLFW + Dear ImGui + OpenCASCADE). Also contains the Explorer preview handler (`native/src/preview`, a COM DLL). |
| `launcher/` | `GlassViewOpen.exe`, the double-click handler: it passes files to an open GlassView window instead of starting a second one. |
| `installer/` | The setup wizard (NSIS): per-user or all-users install, file types, default app, Explorer preview, uninstaller. `installer/portable/` has the scripts for the portable zip. |
| `scripts/` | `build.sh` builds everything; `deps.lock` pins the dependency versions; `mingw.cmake` is the cross-compile toolchain. |

## Building

Everything is cross-compiled for Windows x64 on Linux. On Ubuntu 24.04 (or WSL on Windows):

```bash
sudo apt install git cmake ninja-build mingw-w64 g++-mingw-w64-x86-64-posix nsis zip nodejs npm
./scripts/build.sh
```

Results land in `out/`:

- `GlassView-Setup-<version>.exe`: the setup wizard
- `GlassView-<version>-win64-portable.zip`: the same files to unzip anywhere (`Install.cmd` registers them for the current user)

The first build fetches the pinned dependencies into `.deps/` and compiles OpenCASCADE, which takes 30 to 90 minutes
depending on the machine. Later builds take a few minutes. Single steps can be run on their own:

```bash
./scripts/build.sh app            # only the app (GlassView.exe)
./scripts/build.sh kernel package # rebuild the engine and the setup
```

Steps: `deps occt kernel native launcher app package`.

### Working on the app

```bash
cd app
npm ci
npx neu update          # downloads the Neutralino binaries into app/bin
npm run dev             # vite dev server: UI work in a browser, without the native engine
npx neu run             # the real window on Linux / Windows
```

The app finds `gvkernel.exe` next to `GlassView.exe`; without it, STEP files are read with a slower WebAssembly fallback.

## Version and releases

The version lives in `VERSION`. The build writes it into the app, the launcher, the setup and the Windows file
properties.

GitHub Actions (`.github/workflows/build.yml`) builds every push and pull request; the setup and the portable zip
are attached to the run as artifacts. Pushing a tag `v<version>` (for example `v10.5.0`) also creates a GitHub
release with both files. The app's update check (Settings > Updates) looks at this repository's releases.
For a private repository the check cannot see the releases; enter a public repository there or leave it empty.

## Code signing

Unsigned builds trigger the Windows SmartScreen warning. With a code-signing certificate:

```bash
GV_SIGN_PFX=cert.pfx GV_SIGN_PASS=... ./scripts/build.sh package
```

(needs `osslsigncode`; see `native/SIGNING.md`). In GitHub Actions, store the certificate as a secret and pass it the same way.

## Third-party software

GlassView is built on OpenCASCADE Technology (LGPL 2.1 with exception), three.js and three-mesh-bvh (MIT),
Neutralino (MIT), occt-import-js (LGPL 2.1), Dear ImGui (MIT), GLFW (zlib), FreeType (FTL) and stb (public domain).
The Windows builds link OpenCASCADE statically; the LGPL requires that users can relink with a modified
OpenCASCADE, which this repository's sources and build script allow.
