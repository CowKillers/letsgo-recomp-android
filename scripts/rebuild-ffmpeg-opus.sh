#!/bin/bash
# Rebuild FFmpeg for Android arm64 with Opus decoder
set -e
cd ~/workspace/letsgo-port/deps/ffmpeg

NDK=~/Android/Sdk/ndk/28.2.13676358
TOOLCHAIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64
API=26
PREFIX=~/workspace/letsgo-port/deps/ffmpeg-android

export PATH=$TOOLCHAIN/bin:$PATH

./configure \
  --prefix=$PREFIX \
  --target-os=android \
  --arch=aarch64 \
  --cpu=armv8-a \
  --cc=$TOOLCHAIN/bin/aarch64-linux-android$API-clang \
  --cxx=$TOOLCHAIN/bin/aarch64-linux-android$API-clang++ \
  --enable-cross-compile \
  --disable-static \
  --enable-shared \
  --disable-doc \
  --disable-programs \
  --disable-avdevice \
  --disable-avfilter \
  --disable-postproc \
  --enable-decoder=opus \
  --enable-decoder=aac \
  --enable-decoder=mp3 \
  --enable-parser=opus \
  --enable-demuxer=ogg \
  --enable-muxer=ogg \
  --disable-everything \
  --enable-decoder=opus \
  --enable-decoder=aac

make -j2
make install
echo "FFMPEG_OPUS_OK"
