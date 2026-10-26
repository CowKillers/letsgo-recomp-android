#!/bin/bash
# Build liblgimport.so (on-device NSP importer) for Android arm64. (Portado do build do Zelda.)
set -u
cd "$(dirname "$0")"
ROOT="$PWD"
IMP="$ROOT/importer"
NDK=~/Android/Sdk/ndk/28.2.13676358
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
CXX=$TC/aarch64-linux-android26-clang++
CC=$TC/aarch64-linux-android26-clang

# JNI class is com.letsgorecomp.NativeImporter -> Java_com_letsgorecomp_NativeImporter_*
CXXFLAGS="-O2 -fPIC -fno-strict-aliasing -std=c++17 -D__ANDROID__ -DNXI_JNI_PKG=com_letsgorecomp"
CXXFLAGS="$CXXFLAGS -I$IMP"

OBJDIR=build/obj-importer-android
mkdir -p $OBJDIR build
FAIL=0
for f in nximport.cpp nximport_jni.cpp nxcrypto.cpp; do
    obj=$OBJDIR/${f%.*}.o
    echo "compiling $f"
    $CXX $CXXFLAGS -c "$IMP/$f" -o "$obj" 2>build/importer-errors.log || { echo "FAIL: $f"; FAIL=1; }
done
# romfs_build.c is C (not C++): void* doesn't implicitly convert in C++
CFLAGS="-O2 -fPIC -D__ANDROID__ -I$IMP"
echo "compiling romfs_build.c"
$CC $CFLAGS -c "$IMP/romfs_build.c" -o "$OBJDIR/romfs_build.o" 2>>build/importer-errors.log || { echo "FAIL: romfs_build.c"; FAIL=1; }
[ $FAIL -ne 0 ] && { echo "importer build failed"; tail -30 build/importer-errors.log; exit 1; }

$CXX -shared -o build/liblgimport.so $OBJDIR/*.o -llog -landroid 2>>build/importer-errors.log \
    || { echo "link failed"; tail -30 build/importer-errors.log; exit 1; }
ls -la build/liblgimport.so
echo "IMPORTER_OK"
