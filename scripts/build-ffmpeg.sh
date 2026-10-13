#!/usr/bin/env bash
# Run inside the MSVC environment, with MSYS2 make and clang-cl on PATH.
set -euo pipefail
source_dir=$(cygpath -u "$1")
build_dir=$(cygpath -u "$2")
prefix=$(cygpath -m "$3")
mkdir -p "$build_dir"
cd "$build_dir"
"$source_dir/configure" --prefix="$prefix" --toolchain=msvc --cc=clang-cl --ld=lld-link \
  --arch=x86_64 --target-os=win32 --disable-autodetect --disable-everything \
  --disable-programs --disable-doc --disable-debug --disable-x86asm --disable-pthreads --enable-w32threads \
  --disable-avdevice --disable-avformat --disable-avfilter \
  --enable-avcodec --enable-avutil --enable-swscale --enable-swresample \
  --enable-decoder=mpeg2video,atrac3p --enable-parser=mpegvideo \
  --enable-shared --disable-static
grep -q '^#define CONFIG_ATRAC3P_DECODER 1$' config_components.h
grep -q '^#define CONFIG_MPEG2VIDEO_DECODER 1$' config_components.h
grep -q '^#define CONFIG_MPEGVIDEO_PARSER 1$' config_components.h
grep -q '^#define CONFIG_GPL 0$' config.h
grep -q '^#define CONFIG_NONFREE 0$' config.h
make -j4
make install
