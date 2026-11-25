#!/bin/sh
# configure-native.sh: configure WPE WebKit 2.52.6 natively on IRIX (bsdtezro's
# IRIX userland: pkgsrc clang-irix, CMake, Ninja), with the "lean" feature set of
# lean.sh / configure-wpe.sh, after patches 0001-0014.  Needs pkgsrc cmake
# 4.4.3nb4 or later (its Platform/IRIX64.cmake: uname -s is IRIX64 on 64-bit
# kernels).  Run from the directory
# holding wpewebkit-2.52.6 (on bsdtezro: /build/heliotrope):
#   sh heliotrope/wpe/configure-native.sh   (BUILD=wpe-build-native by default)
set -e
BUILD=${BUILD:-wpe-build-native}
P=/usr/pkg
export PATH=$P/bin:/usr/bin:/bin:/usr/bsd
export CC=clang CXX=clang++
export PKG_CONFIG_PATH=$P/lib/pkgconfig:$P/share/pkgconfig
# EGL_NO_X11: pkgsrc's eglplatform.h pulls in Xlib on any __unix__ system without it, and X.h's
# "#define Success 0" then breaks WTF's ConversionResultCode::Success.  No X11 platform is built.
# -O2 since 2026-11-25 (user): -Os saved ~20% of libWPEWebKit's size but costs speed in the hot loops (CSS
# style resolution, image decoding, the JS interpreter). OPT=-Os for comparison builds.
OPT=${OPT:--O2}
FLAGS="$OPT -fomit-frame-pointer -march=mips4 -DNDEBUG -DEGL_NO_X11 -isystem $P/include/gettext"
LINK="-static-libstdc++ -Wl,-rpath,$P/lib -L$P/lib -lintl -Wl,--icf=safe"
cmake -G Ninja -S wpewebkit-2.52.6 -B $BUILD \
  -DCMAKE_PREFIX_PATH=$P -DCMAKE_INSTALL_PREFIX=$P -DCMAKE_BUILD_TYPE=Release \
  -DPORT=WPE -DUSE_SKIA=OFF -DENABLE_JIT=OFF -DENABLE_C_LOOP=ON -DUSE_SYSTEM_MALLOC=ON \
  -DENABLE_WEBASSEMBLY=OFF -DENABLE_SAMPLING_PROFILER=OFF -DDEVELOPER_MODE=OFF \
  -DENABLE_WPE_PLATFORM=ON -DENABLE_WPE_PLATFORM_HEADLESS=ON -DENABLE_WPE_PLATFORM_DRM=OFF \
  -DENABLE_WPE_PLATFORM_WAYLAND=OFF -DENABLE_WPE_LEGACY_API=OFF -DENABLE_WPE_QT_API=OFF \
  -DENABLE_DOCUMENTATION=OFF -DENABLE_INTROSPECTION=OFF -DENABLE_JOURNALD_LOG=OFF \
  -DENABLE_BUBBLEWRAP_SANDBOX=OFF -DENABLE_GAMEPAD=OFF -DENABLE_GPU_PROCESS=OFF \
  -DENABLE_VIDEO=OFF -DENABLE_WEB_AUDIO=OFF -DENABLE_MEDIA_STREAM=OFF -DENABLE_WEB_RTC=OFF \
  -DENABLE_WEB_CODECS=OFF -DENABLE_MEDIA_RECORDER=OFF -DENABLE_ENCRYPTED_MEDIA=OFF \
  -DENABLE_SPEECH_SYNTHESIS=OFF -DENABLE_XSLT=OFF -DENABLE_WEBDRIVER=OFF -DENABLE_MINIBROWSER=OFF \
  -DENABLE_API_TESTS=OFF -DENABLE_LAYOUT_TESTS=OFF -DENABLE_REMOTE_INSPECTOR=OFF \
  -DUSE_ATK=OFF -DUSE_GBM=OFF -DUSE_LIBDRM=OFF -DUSE_LIBBACKTRACE=OFF -DUSE_LIBHYPHEN=OFF \
  -DUSE_AVIF=OFF -DUSE_JPEGXL=OFF -DUSE_LCMS=OFF -DUSE_WOFF2=OFF -DUSE_FLITE=OFF \
  -DUSE_SYSPROF_CAPTURE=OFF -DUSE_GSTREAMER=OFF -DENABLE_WEBGL=OFF \
  -DENABLE_GEOLOCATION=OFF -DENABLE_NOTIFICATIONS=OFF \
  -DENABLE_PDFJS=OFF -DENABLE_SMOOTH_SCROLLING=OFF -DENABLE_TOUCH_EVENTS=OFF -DENABLE_MHTML=OFF \
  -DENABLE_OFFSCREEN_CANVAS=OFF -DENABLE_OFFSCREEN_CANVAS_IN_WORKERS=OFF -DENABLE_IMAGE_DIFF=OFF \
  "-DCMAKE_C_FLAGS_RELEASE=$FLAGS" "-DCMAKE_CXX_FLAGS_RELEASE=$FLAGS" \
  "-DCMAKE_EXE_LINKER_FLAGS=$LINK" "-DCMAKE_SHARED_LINKER_FLAGS=$LINK" \
  "$@"
