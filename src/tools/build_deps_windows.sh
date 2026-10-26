#!/bin/bash
# make deps-windows (MSYS2 UCRT64 shell): what the shipped Windows build links against instead of MSYS2's packages,
# built from pinned sources into build/deps-win. MSYS2's FFmpeg is a GPLv3 build (x264, x265, xvid, ...); the
# release uses an LGPL-only FFmpeg with just what the game's movies need - WebM: VP9 video + Vorbis audio, plus
# VP8 / Opus / H.264 / AAC in WebM or MP4 - and swscale. swresample stays: FFmpeg's Opus decoder depends on it (without it
# configure drops Opus silently, and Let's Go's music and sound effects - hwopus - need it). Checked below.
# SDL3 (zlib licence) and the compiler runtime DLLs still come from MSYS2.
# Needs: the packages from tools/setup_windows.ps1 (clang, make, nasm). Skipped when its output exists
# (delete build/deps-win to redo).
set -euo pipefail
cd "$(dirname "$0")/.."
P=$PWD/build/deps-win
S=$P/src
mkdir -p "$S"
JOBS=$(nproc)

fetch() {  # url sha256 file
    [ -f "$S/$3" ] || curl -fsSL -o "$S/$3.part" "$1" && { [ -f "$S/$3" ] || mv "$S/$3.part" "$S/$3"; }
    echo "$2  $S/$3" | sha256sum -c --quiet
}
unpack() { rm -rf "$S/$2" && mkdir -p "$S/$2" && tar -xf "$S/$1" -C "$S/$2" --strip-components 1; }

fetch https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz \
      8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e ffmpeg-9.0.2.tar.xz

if [ ! -f "$P/lib/libavcodec.dll.a" ]; then
    unpack ffmpeg-9.0.2.tar.xz ffmpeg
    (cd "$S/ffmpeg" && ./configure --prefix="$P" --cc=clang --cxx=clang++ --arch=x86_64 --target-os=mingw32 \
        --enable-shared --disable-static \
        --disable-autodetect --disable-programs --disable-doc --disable-network --disable-avdevice --disable-avfilter \
        --disable-everything \
        --enable-protocol=file --enable-demuxer=matroska,mov \
        --enable-decoder=vp9,vp8,vorbis,opus,h264,aac --enable-parser=vp9,vp8,vorbis,opus,h264,aac \
        && make -j"$JOBS" && make install)
fi
grep -q 'GPL version 3\|--enable-gpl' "$P/lib/pkgconfig/libavcodec.pc" && { echo "deps-win: FFmpeg is not LGPL"; exit 1; }
for c in opus vorbis vp9 vp8 h264 aac; do  # (every decoder the game needs, really built: configure drops missing deps quietly)
    C=$(echo "$c" | tr a-z A-Z)
    grep -q "^#define CONFIG_${C}_DECODER 1" "$S/ffmpeg/config_components.h" || { echo "deps-win: FFmpeg built without the $c decoder"; exit 1; }
done
echo "deps-win: $(ls "$P/bin"/*.dll | xargs -n1 basename | tr '\n' ' ')"
