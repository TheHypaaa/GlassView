#!/bin/bash
# GlassView: builds everything for Windows x64 on Linux (Ubuntu 24.04 or newer), from a clean clone.
#
#   ./scripts/build.sh            everything: setup .exe + portable zip in out/
#   ./scripts/build.sh <step>...  only some steps: deps occt kernel native launcher app package
#
# Needs: git cmake ninja-build mingw-w64 (posix threads) nsis zip nodejs/npm  (see README.md)
# Dependencies are fetched and built once into .deps/ (OpenCASCADE takes 30-90 min the first time).
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
DEPS=${DEPS:-$ROOT/.deps}
OUT=$ROOT/out
STAGE=$ROOT/build/stage
JOBS=${JOBS:-$(nproc)}
VERSION=$(tr -d ' \n\r' < "$ROOT/VERSION")
IFS=. read -r V1 V2 V3 <<< "$VERSION"
CXX=x86_64-w64-mingw32-g++-posix

# Pinned dependency versions (the ones GlassView is tested with) - scripts/deps.lock
source "$ROOT/scripts/deps.lock"

log() { printf '\n\033[1;34m== %s\033[0m\n' "$*"; }

fetch() { # fetch <dir> <repo> <rev>: shallow checkout of one commit or tag
  local dir=$1 repo=$2 rev=$3
  if [ -f "$dir/.gv-rev" ] && [ "$(cat "$dir/.gv-rev")" = "$rev" ]; then return; fi
  rm -rf "$dir"; mkdir -p "$dir"
  git -C "$dir" init -q
  git -C "$dir" remote add origin "$repo"
  git -C "$dir" fetch -q --depth 1 origin "$rev"
  git -C "$dir" checkout -q FETCH_HEAD
  echo "$rev" > "$dir/.gv-rev"
}

step_deps() {
  log "Dependencies -> $DEPS"
  mkdir -p "$DEPS"
  fetch "$DEPS/occt" "$OCCT_REPO" "$OCCT_REV"
  fetch "$DEPS/freetype" "$FREETYPE_REPO" "$FREETYPE_REV"
  fetch "$DEPS/glfw" "$GLFW_REPO" "$GLFW_REV"
  fetch "$DEPS/imgui" "$IMGUI_REPO" "$IMGUI_REV"
  if [ ! -f "$DEPS/build-ft-win/libfreetype.a" ]; then
    cmake -S "$DEPS/freetype" -B "$DEPS/build-ft-win" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$ROOT/scripts/mingw.cmake" \
      -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DFT_DISABLE_BROTLI=ON -DFT_DISABLE_BZIP2=ON \
      -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_PNG=ON -DFT_DISABLE_ZLIB=ON
    ninja -C "$DEPS/build-ft-win" -j "$JOBS"
  fi
  if [ ! -f "$DEPS/build-glfw-win/src/libglfw3.a" ]; then
    cmake -S "$DEPS/glfw" -B "$DEPS/build-glfw-win" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$ROOT/scripts/mingw.cmake" \
      -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF -DGLFW_BUILD_DOCS=OFF -DGLFW_BUILD_EXAMPLES=OFF -DGLFW_BUILD_TESTS=OFF
    ninja -C "$DEPS/build-glfw-win" -j "$JOBS"
  fi
}

step_occt() {
  log "OpenCASCADE (static, MinGW) -> $DEPS/occt-win"
  if ls "$DEPS"/occt-win/win64/gcc/lib/libTKDESTEP.a >/dev/null 2>&1 && [ -f "$DEPS/occt-win/.rev" ] && [ "$(cat "$DEPS/occt-win/.rev")" = "$OCCT_REV" ]; then
    echo "up to date"; return
  fi
  cmake -S "$DEPS/occt" -B "$DEPS/occt-win" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$ROOT/scripts/mingw.cmake" \
    -DCMAKE_BUILD_TYPE=Release -DBUILD_LIBRARY_TYPE=Static -DBUILD_CPP_STANDARD=C++17 \
    -DBUILD_MODULE_ApplicationFramework=OFF -DBUILD_MODULE_DataExchange=OFF -DBUILD_MODULE_Draw=OFF \
    -DBUILD_MODULE_FoundationClasses=OFF -DBUILD_MODULE_ModelingAlgorithms=OFF -DBUILD_MODULE_ModelingData=OFF \
    -DBUILD_MODULE_Visualization=OFF -DBUILD_ADDITIONAL_TOOLKITS="TKDESTEP;TKDEIGES;TKXCAF;TKMesh;TKOpenGl;TKV3d;TKService;TKDESTL" \
    -DBUILD_DOC_Overview=OFF -DBUILD_GTEST=OFF -DBUILD_RESOURCES=OFF -DBUILD_SAMPLES_QT=OFF -DBUILD_USE_PCH=OFF \
    -DBUILD_RELEASE_DISABLE_EXCEPTIONS=ON -DBUILD_ENABLE_FPE_SIGNAL_HANDLER=OFF -DBUILD_Inspector=OFF -DBUILD_YACCLEX=OFF \
    -DUSE_FREETYPE=OFF -DUSE_OPENGL=ON -DUSE_D3D=OFF -DUSE_FFMPEG=OFF -DUSE_FREEIMAGE=OFF -DUSE_OPENVR=OFF -DUSE_TBB=OFF \
    -DUSE_TCL=OFF -DUSE_XLIB=OFF -DUSE_GTEST=OFF -DUSE_RAPIDJSON=OFF -DUSE_DRACO=OFF -DUSE_VTK=OFF -DUSE_EIGEN=OFF \
    -DCMAKE_CXX_FLAGS="-O2 -w" -DCMAKE_INSTALL_PREFIX="$DEPS/occt-win-install" $OCCT_CMAKE_EXTRA
  ninja -C "$DEPS/occt-win" -j "$JOBS"
  echo "$OCCT_REV" > "$DEPS/occt-win/.rev"
}

step_kernel() {
  log "Geometry engine: gvkernel.exe"
  mkdir -p "$STAGE"
  local W=$DEPS/occt-win
  $CXX -std=c++17 -O2 -D_USE_MATH_DEFINES -Wno-deprecated-declarations \
    -I "$W/inc" -I "$DEPS/occt/src/Deprecated/NCollectionAliases" "$ROOT/kernel/gvkernel.cpp" -o "$STAGE/gvkernel.exe" \
    -static -static-libgcc -static-libstdc++ -mwindows -Wl,--stack,33554432 \
    -Wl,--start-group "$W"/win64/gcc/lib/libTK*.a "$DEPS/build-ft-win/libfreetype.a" -Wl,--end-group \
    -lwindowscodecs -lws2_32 -luser32 -lgdi32 -ladvapi32 -lpsapi -lshell32 -lole32 -loleaut32 -lwinmm -lcomdlg32 -lwsock32
  x86_64-w64-mingw32-strip "$STAGE/gvkernel.exe"
}

step_native() {
  log "GlassView Native + Explorer preview DLL"
  DEPS=$DEPS OUT="$ROOT/build/native" JOBS=$JOBS "$ROOT/native/build.sh"
  mkdir -p "$STAGE"
  cp "$ROOT/build/native/GlassViewPreview.dll" "$STAGE/"
}

step_launcher() {
  log "GlassViewOpen.exe (double-click handler)"
  mkdir -p "$ROOT/build/launcher" "$STAGE"
  (cd "$ROOT/launcher" && x86_64-w64-mingw32-windres -DGV_V1=$V1 -DGV_V2=$V2 -DGV_V3=$V3 -DGV_VSTR="\\\"$VERSION\\\"" \
    open.rc -O coff -o "$ROOT/build/launcher/open_res.o")
  $CXX -std=c++17 -O2 -municode -mwindows -DUNICODE -D_UNICODE "$ROOT/launcher/open.cpp" "$ROOT/build/launcher/open_res.o" \
    -o "$STAGE/GlassViewOpen.exe" -static -static-libgcc -static-libstdc++ -lshell32 -luser32
  x86_64-w64-mingw32-strip "$STAGE/GlassViewOpen.exe"
}

step_app() {
  log "App (Neutralino + three.js): GlassView.exe"
  cd "$ROOT/app"
  node -e "const f='neutralino.config.json',c=JSON.parse(require('fs').readFileSync(f));c.version='$VERSION';require('fs').writeFileSync(f,JSON.stringify(c,null,2)+'\n')"
  [ -d node_modules ] || npm ci
  [ -f bin/neutralino-win_x64.exe ] || npx neu update
  node scripts/patch-exe.cjs bin/neutralino-win_x64.exe bin/neutralino-win_x64.exe icon.ico "$VERSION"
  npm run build
  mkdir -p "$STAGE"
  cp dist/GlassView/GlassView-win_x64.exe "$STAGE/GlassView.exe"
  cp README.txt "$STAGE/README.txt"
  cd "$ROOT"
}

step_package() {
  log "Setup wizard + portable zip -> out/"
  mkdir -p "$OUT"
  for f in GlassView.exe GlassViewOpen.exe gvkernel.exe GlassViewPreview.dll README.txt; do
    [ -f "$STAGE/$f" ] || { echo "missing $STAGE/$f - run the other steps first"; exit 1; }
  done
  # optional Authenticode signing: GV_SIGN_PFX=cert.pfx GV_SIGN_PASS=... (see native/SIGNING.md)
  sign() {
    [ -n "${GV_SIGN_PFX:-}" ] && command -v osslsigncode >/dev/null || return 0
    osslsigncode sign -pkcs12 "$GV_SIGN_PFX" -pass "${GV_SIGN_PASS:-}" -n GlassView -t http://timestamp.digicert.com -h sha256 \
      -in "$1" -out "$1.signed" && mv "$1.signed" "$1" && echo "signed $1"
  }
  for f in GlassView.exe GlassViewOpen.exe gvkernel.exe GlassViewPreview.dll; do sign "$STAGE/$f"; done
  makensis -V2 -DVERSION="$VERSION" -DSRC="$STAGE" -DOUTDIR="$OUT" "$ROOT/installer/GlassView.nsi"
  sign "$OUT/GlassView-Setup-$VERSION.exe"
  # portable: the same files + Install.cmd (per-user registration without the setup)
  rm -rf "$ROOT/build/portable"; mkdir -p "$ROOT/build/portable/GlassView"
  cp "$STAGE"/{GlassView.exe,GlassViewOpen.exe,gvkernel.exe,GlassViewPreview.dll,README.txt} "$ROOT/build/portable/GlassView/"
  cp "$ROOT"/installer/portable/* "$ROOT/native/SIGNING.md" "$ROOT/build/portable/GlassView/"
  rm -f "$OUT/GlassView-$VERSION-win64-portable.zip"
  (cd "$ROOT/build/portable" && zip -qr "$OUT/GlassView-$VERSION-win64-portable.zip" GlassView)
  ls -la "$OUT"
}

STEPS=("$@")
[ ${#STEPS[@]} -eq 0 ] && STEPS=(deps occt kernel native launcher app package)
for s in "${STEPS[@]}"; do "step_$s"; done
