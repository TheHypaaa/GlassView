#!/bin/bash
# GlassView Native (C++ app) and the Explorer 3D preview DLL, cross-compiled for Windows with MinGW-w64.
# Normally run through scripts/build.sh, which fetches and builds the dependencies first.
#   DEPS=<folder with occt, occt-win, imgui, glfw, build-glfw-win, build-ft-win>  ./native/build.sh
set -e
cd "$(dirname "$0")"
DEPS=${DEPS:-../.deps}
OCCT_SRC=$DEPS/occt
W=$DEPS/occt-win
T=$DEPS
CXX=${CXX:-x86_64-w64-mingw32-g++-posix}
OUT=${OUT:-build}
IMGUI="$T/imgui/imgui.cpp $T/imgui/imgui_draw.cpp $T/imgui/imgui_tables.cpp $T/imgui/imgui_widgets.cpp $T/imgui/backends/imgui_impl_glfw.cpp $T/imgui/backends/imgui_impl_opengl3.cpp"
STL=$OCCT_SRC/src/DataExchange/TKDESTL/RWStl
SRC="src/main.cpp src/model.cpp src/modelcache.cpp src/ui.cpp src/platform.cpp src/measure.cpp src/features.cpp src/am.cpp src/blur.cpp src/shell.cpp src/pmi.cpp src/pmiread.cpp src/threemf.cpp src/compare.cpp $STL/RWStl.cxx $STL/RWStl_Reader.cxx"
INC="-I src -I $T/imgui -I $T/imgui/backends -I $T/glfw/include -I $T/glfw/deps -I $STL -I $W/inc -I $OCCT_SRC/src/Deprecated/NCollectionAliases"
FLAGS="-std=c++17 -O2 -D_USE_MATH_DEFINES -DUNICODE -D_UNICODE -Wno-deprecated-declarations"
JOBS=${JOBS:-$(nproc)}
mkdir -p $OUT/win $OUT/winpv
x86_64-w64-mingw32-windres res/gvnative.rc -O coff -o $OUT/win/res.o
HDR=$(ls -t src/*.hpp | head -1)   # newest header: our sources rebuild when any header changes
n=0
for f in $SRC $IMGUI; do
  o=$OUT/win/$(basename $f .cpp).o
  [ "$o" -nt "$f" ] && { [ "${f#src/}" = "$f" ] || [ "$o" -nt "$HDR" ]; } && continue
  rm -f $o; $CXX $FLAGS $INC -c $f -o $o &
  n=$((n + 1)); [ $((n % JOBS)) -eq 0 ] && wait
done
wait
for f in $SRC; do [ -f $OUT/win/$(basename $f .cpp).o ] || { echo "compile failed: $f"; exit 1; }; done
LIBS="-Wl,--start-group $W/win64/gcc/lib/libTK*.a $T/build-ft-win/libfreetype.a -Wl,--end-group"
SYS="-lopengl32 -lgdi32 -luser32 -lcomdlg32 -lshell32 -lshlwapi -lole32 -loleaut32 -luuid -lwindowscodecs -lwinhttp -lws2_32 -ladvapi32 -lpsapi -lwinmm -ldwmapi -lwsock32"
$CXX $OUT/win/*.o -o $OUT/GlassViewNative.exe -static -static-libgcc -static-libstdc++ -mwindows $LIBS $T/build-glfw-win/src/libglfw3.a $SYS
x86_64-w64-mingw32-strip $OUT/GlassViewNative.exe

# ---- Explorer 3D preview handler (COM DLL for the preview pane)
for f in src/preview/pvscene.cpp src/preview/handler.cpp; do
  $CXX $FLAGS $INC -c $f -o $OUT/winpv/$(basename $f .cpp).o
done
$CXX -shared -o $OUT/GlassViewPreview.dll src/preview/preview.def $OUT/winpv/*.o \
  $OUT/win/model.o $OUT/win/modelcache.o $OUT/win/pmiread.o $OUT/win/RWStl.cxx.o $OUT/win/RWStl_Reader.cxx.o \
  -static -static-libgcc -static-libstdc++ $LIBS $SYS
x86_64-w64-mingw32-strip $OUT/GlassViewPreview.dll
ls -la $OUT/GlassViewNative.exe $OUT/GlassViewPreview.dll
