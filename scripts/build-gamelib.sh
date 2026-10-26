#!/bin/bash
# Build the recompiled game code (generated-aarch64/) as liblasgame.so for Android arm64.
set -u
cd "$(dirname "$0")"
ROOT="$PWD"
SRC="$ROOT/src"
NDK=~/Android/Sdk/ndk/28.2.13676358
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
CC=$TC/aarch64-linux-android26-clang

GEN=$SRC/generated-aarch64
[ -d "$GEN/main" ] || { echo "run make gen first"; exit 1; }

# GAMELIB_ID, same recipe as the Makefile (generator sources + GENFLAGS)
GENFLAGS="-O1 -fno-strict-aliasing -Wno-return-type -Wno-unused-label -fvisibility=hidden -Iruntime"
GAMELIB_SRCS="tools/gen.py tools/lifter.py tools/analyze.py tools/nso.py tools/passes.py tools/patches.py tools/simd_c.py tools/gamelib_entry.c runtime/hle/hle_symbols.txt runtime/cpu.h runtime/simd.h runtime/simd_fp.h runtime/gamelib.h"
GAMELIB_ID=$((cd "$SRC" && cat $GAMELIB_SRCS; echo "$GENFLAGS") | sha256sum | cut -c1-16)
echo "GAMELIB_ID=$GAMELIB_ID"
echo "$GAMELIB_ID" > "$ROOT/GAMELIB_ID.txt"

OUTDIR=$ROOT/build/game
mkdir -p $OUTDIR/obj
fail=0
for f in $(find $GEN -name "*.c") $SRC/tools/gamelib_entry.c; do
    obj=$OUTDIR/obj/$(echo "$f" | tr '/' '_' | sed 's/\.c$/.o/')
    { [ -f "$obj" ] && [ "$obj" -nt "$f" ]; } && continue  # skip up to date
    if ! $CC $GENFLAGS -DGAMELIB_ID='"'$GAMELIB_ID'"' -I$SRC/runtime -c "$f" -o "$obj" 2>>build-gamelib-errors.log; then
        echo "FAIL: $f"; fail=1
    fi &
    # throttle: never more than nproc jobs
    while [ $(jobs -r | wc -l) -ge 2 ]; do sleep 2; done
done
wait
echo "objects: $(ls $OUTDIR/obj/*.o 2>/dev/null | wc -l)"
[ $fail -ne 0 ] && { echo GAMELIB_BUILD_FAIL; grep -m5 "error:" build-gamelib-errors.log; exit 1; }
$CC -shared -o $OUTDIR/liblasgame.so $OUTDIR/obj/*.o 2>>build-gamelib-errors.log || { echo LINK_FAIL; exit 1; }
echo -n "$GAMELIB_ID" > $OUTDIR/liblasgame.so.id
ls -la $OUTDIR/liblasgame.so
echo GAMELIB_OK
