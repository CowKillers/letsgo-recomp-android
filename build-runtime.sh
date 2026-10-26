#!/bin/bash
# Smoke-compile the Let's Go Recomp runtime for Android arm64 (objects only, no link yet).
set -u
cd "$(dirname "$0")"
ROOT="$PWD"
SRC="$ROOT/src"
DEPS="$ROOT/deps"
NDK=~/Android/Sdk/ndk/28.2.13676358
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
CC=$TC/aarch64-linux-android26-clang
CXX=$TC/aarch64-linux-android26-clang++

CFLAGS="-O2 -fPIC -fno-strict-aliasing"
# (no -fvisibility=hidden here: SDL_main and the JNI entry must stay visible,
#  unlike the game code which the Makefile builds hidden)
CFLAGS="$CFLAGS -I$SRC/runtime -I$SRC/third_party/vma"
CFLAGS="$CFLAGS -I$DEPS/SDL3/include -I$DEPS/SDL3/build-android/include"
CFLAGS="$CFLAGS -I$DEPS/ffmpeg-android/include"
CFLAGS="$CFLAGS -I$SRC/third_party/imgui -I$SRC/third_party/imgui/backends"
CFLAGS="$CFLAGS -I$SRC/third_party/smaa/Textures"
# (SMAA.hlsl -> smaa_hlsl.inc codegen step from the Makefile is needed for smaa_data.c; handled at link stage)
CFLAGS="$CFLAGS -I$DEPS/shaderc/libshaderc/include"
# (Vulkan headers come from the NDK sysroot.)
# Adrenotools (Turnip custom GPU drivers): dispatch via g_vkd, loaded from libadrenotools.so.
CFLAGS="$CFLAGS -DLETSGO_ADRENOTOOLS -I$ROOT/deps/adrenotools/include"

OBJDIR=build/obj-android
mkdir -p $OBJDIR build/inc
# Use a file for failure tracking: background jobs run in subshells,
# so a shell variable set in a subshell is invisible to the parent.
FAILFILE=$OBJDIR/.build_failed
rm -f $FAILFILE

# GAMELIB_ID must match the game library's (see build-gamelib.sh)
if [ -f "$ROOT/GAMELIB_ID.txt" ]; then
    GAMELIB_ID=$(cat "$ROOT/GAMELIB_ID.txt")
    CFLAGS="$CFLAGS -DGAMELIB_ID=\"$GAMELIB_ID\""
fi

# SMAA.hlsl -> C string (same recipe as the Makefile)
if [ ! -f build/inc/smaa_hlsl.inc ]; then
    LC_ALL=C tr -d '\r' < $SRC/third_party/smaa/SMAA.hlsl | LC_ALL=C tr '\200-\377' '?' \
        | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$/\\n"/' > build/inc/smaa_hlsl.inc
fi
CFLAGS="$CFLAGS -I$ROOT/build/inc"

compile_c() {
    local src="$1"
    local obj=$OBJDIR/$(echo $src | sed 's|/|_|g').o
    local extra=""
    case "$src" in */main.c) extra="-Dmain=SDL_main";; esac
    if ! $CC $CFLAGS $extra -c "$SRC/$src" -o "$obj" 2>>build-errors.log; then
        echo "FAIL: $src"; touch $FAILFILE
    fi
}
compile_cxx() {
    local src="$1"
    local obj=$OBJDIR/$(echo $src | sed 's|/|_|g').o
    if ! $CXX $CFLAGS -std=c++17 -x c++ -c "$SRC/$src" -o "$obj" 2>>build-errors.log; then
        echo "FAIL: $src"; touch $FAILFILE
    fi
}

rm -f build-errors.log
for f in $SRC/runtime/*.c $SRC/runtime/hle/*.c $SRC/runtime/services/*.c \
         $SRC/runtime/nvn/*.c $SRC/runtime/nvn/maxwell/*.c $SRC/runtime/nvn/vulkan/*.c \
         $SRC/runtime/platform/android.c; do
    compile_c "${f#$SRC/}" &
done
for f in $SRC/runtime/nvn/vulkan/*.cpp; do compile_cxx "${f#$SRC/}" & done
for f in $SRC/runtime/ui/*.mm; do compile_cxx "${f#$SRC/}" & done
for f in imgui.cpp imgui_draw.cpp imgui_tables.cpp imgui_widgets.cpp \
         backends/imgui_impl_sdl3.cpp backends/imgui_impl_vulkan.cpp; do
    extra=""
    # ImGui must resolve Vulkan through the same loader as the renderer.
    if [[ "$CFLAGS" == *-DLETSGO_ADRENOTOOLS* && "$f" == backends/imgui_impl_vulkan.cpp ]]; then
        extra="-DIMGUI_IMPL_VULKAN_NO_PROTOTYPES"
    fi
    $CXX $CFLAGS $extra -std=c++17 -c "$SRC/third_party/imgui/$f" \
        -o "$OBJDIR/imgui_$(echo $f | sed 's|/|_|g').o" 2>>build-errors.log \
        || { echo "FAIL: imgui/$f"; touch $FAILFILE; } &
done
wait
echo "objects: $(ls $OBJDIR/*.o 2>/dev/null | wc -l)"
# Check the failure file (not a shell variable: background jobs can't modify parent vars)
if [ ! -f $FAILFILE ]; then echo SMOKE_OK; else echo SMOKE_FAIL; grep -m5 "error:" build-errors.log; exit 1; fi
