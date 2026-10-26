#!/bin/bash
# Link the runtime objects into liblasrecomp.so for Android arm64.
set -u
cd "$(dirname "$0")"
ROOT="$PWD"
DEPS="$ROOT/deps"
NDK=~/Android/Sdk/ndk/28.2.13676358
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
CXX=$TC/aarch64-linux-android26-clang++

SC=$DEPS/shaderc/build-android
[ -f "$SC/libshaderc/libshaderc.a" ] || { echo "shaderc not built yet"; exit 1; }

OUTDIR=$ROOT/build/apk-lib
mkdir -p $OUTDIR

# shaderc static libs (order matters)
SC_LIBS="$SC/libshaderc/libshaderc.a $SC/libshaderc_util/libshaderc_util.a"
SC_LIBS="$SC_LIBS $(find $SC/third_party/spirv-tools -name 'libSPIRV-Tools*.a' | sort)"
SC_LIBS="$SC_LIBS $(find $SC/third_party/glslang -name '*.a' | sort)"

FF=$DEPS/ffmpeg-android/lib

$CXX -shared -o $OUTDIR/liblasrecomp.so \
    $ROOT/build/obj-android/*.o \
    -L$DEPS/SDL3/build-android -lSDL3 \
    -L$FF -lavformat -lavcodec -lswscale -lavutil \
    $SC_LIBS \
    -L$ROOT/build/apk-lib -ladrenotools \
    -lvulkan -llog -landroid -ldl -lm \
    2>link-runtime-errors.log || { echo LINK_FAIL; tail -5 link-runtime-errors.log; exit 1; }

# the game library id, for the launcher's setup check (see gamelib.c id_matches)
[ -f "$ROOT/GAMELIB_ID.txt" ] && cp "$ROOT/GAMELIB_ID.txt" $OUTDIR/liblasgame.so.id
ls -la $OUTDIR/
echo RUNTIME_LINK_OK
