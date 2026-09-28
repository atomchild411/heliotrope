#!/bin/sh
# The "lean" configuration (2026-09-28): MIPS IV, -Os, no frame pointers, identical
# code folding, and features a small IRIX browser does not need switched off.
BUILD=wpe-build-lean CPU=mips4 OPT=-Os CFLAGS_EXTRA=-fomit-frame-pointer LDFLAGS_EXTRA=-Wl,--icf=safe \
exec sh ./configure-wpe.sh \
  -DENABLE_GEOLOCATION=OFF -DENABLE_NOTIFICATIONS=OFF \
  -DENABLE_PDFJS=OFF -DENABLE_SMOOTH_SCROLLING=OFF -DENABLE_TOUCH_EVENTS=OFF -DENABLE_MHTML=OFF \
  -DENABLE_OFFSCREEN_CANVAS=OFF -DENABLE_OFFSCREEN_CANVAS_IN_WORKERS=OFF -DENABLE_IMAGE_DIFF=OFF
