#!/bin/bash
# Assemble the Let's Go Recomp Android APK manually (no Gradle):
# javac -> d8 -> aapt2 -> zipalign -> apksigner (CaseWest debug keystore).
set -u
cd "$(dirname "$0")"
ROOT="$PWD"
SDK=~/Android/Sdk
BT=$SDK/build-tools/35.0.0
PLATFORM=$SDK/platforms/android-34/android.jar
KS=~/workspace/mvc2-xbox/apk-test/debug.keystore

PKG=com.letsgorecomp
OUT=$ROOT/build/apk
rm -rf $OUT
mkdir -p $OUT/classes $OUT/dex $OUT/lib/arm64-v8a

# 1. java -> classes (our activity + SDL3's java)
find apk/java deps/SDL3/android-project/app/src/main/java -name "*.java" > $OUT/sources.txt
javac -encoding UTF-8 -source 8 -target 8 -bootclasspath $PLATFORM -d $OUT/classes \
    @$OUT/sources.txt 2>$OUT/javac.log || { echo JAVAC_FAIL; tail -5 $OUT/javac.log; exit 1; }

# 2. classes -> dex
$BT/d8 --min-api 28 --lib $PLATFORM --output $OUT/dex \
    $(find $OUT/classes -name "*.class") 2>$OUT/d8.log || { echo D8_FAIL; tail -5 $OUT/d8.log; exit 1; }

# 3. native libs (unversioned copies: the APK lib dir wants plain lib*.so names)
cp $ROOT/build/apk-lib/*.so $OUT/lib/arm64-v8a/ 2>/dev/null
cp $ROOT/build/liblgimport.so $OUT/lib/arm64-v8a/ 2>/dev/null
cp $ROOT/deps/SDL3/build-android/libSDL3.so $OUT/lib/arm64-v8a/
cp $ROOT/build/game/liblasgame.so $OUT/lib/arm64-v8a/ 2>/dev/null
# FFmpeg shared libs: copy the .so files the runtime's DT_NEEDED references
# (libav* AND libsw*: libswscale/libswresample don't match libav*)
for f in $ROOT/deps/ffmpeg-android/lib/libav*.so $ROOT/deps/ffmpeg-android/lib/libsw*.so; do
    [ -f "$f" ] && [ ! -L "$f" ] && cp $f $OUT/lib/arm64-v8a/ 2>/dev/null
done
# C++ runtime (DT_NEEDED by the C++ parts: imgui, glslang remnants, game code)
cp ~/Android/Sdk/ndk/28.2.13676358/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so $OUT/lib/arm64-v8a/ 2>/dev/null
ls $OUT/lib/arm64-v8a/

# 4. aapt2 link (manifest + res; dex + libs are added with zip below)
$BT/aapt2 compile --dir apk/res -o $OUT/res.zip 2>>$OUT/aapt.log \
    || { echo AAPT_COMPILE_FAIL; tail -10 $OUT/aapt.log; exit 1; }
$BT/aapt2 link -o $OUT/base.apk \
    --manifest apk/AndroidManifest.xml \
    --java $OUT/gen \
    -I $PLATFORM \
    -A apk/assets \
    --min-sdk-version 28 --target-sdk-version 34 \
    $OUT/res.zip \
    2>$OUT/aapt.log || { echo AAPT_FAIL; tail -10 $OUT/aapt.log; exit 1; }
cp $OUT/base.apk $OUT/unsigned.apk
# classes.dex at the apk root; .so files stored uncompressed under lib/arm64-v8a
cp $OUT/dex/classes.dex $OUT/classes.dex
cd $OUT && zip -q unsigned.apk classes.dex && zip -q -0 unsigned.apk lib/arm64-v8a/*.so && cd $ROOT

# 5. align (16KB page alignment for native libs, required on new Android) + sign
$BT/zipalign -f -P 16 4 $OUT/unsigned.apk $OUT/aligned.apk 2>>$OUT/aapt.log || { echo ALIGN_FAIL; exit 1; }
$BT/apksigner sign --ks $KS --ks-pass pass:android --key-pass pass:android \
    --out $OUT/LetsGoRecomp-android.apk $OUT/aligned.apk 2>>$OUT/aapt.log || { echo SIGN_FAIL; exit 1; }
$BT/apksigner verify --print-certs $OUT/LetsGoRecomp-android.apk | grep -m1 SHA-256

cp $OUT/LetsGoRecomp-android.apk $ROOT/
ls -la $ROOT/LetsGoRecomp-android.apk
echo APK_OK
