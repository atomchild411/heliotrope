# Heliotrope

A small browser for IRIX 6.5 (n32, big-endian MIPS) built on WPE WebKit 2.52.6:
Cairo rendering, the CLoop JavaScript interpreter, WPEPlatform headless on Mesa
softpipe EGL, shown in an X window.

- `src/heliotrope.c` -- the browser: X11 window, toolbar and address bar,
  WPE view and input.
- `wpe/patches/` -- the WebKit changes (apply to wpewebkit-2.52.6 in order):
  the IRIX port, a big-endian TextureMapper colour fix, and a big-endian
  CompactVariant fix.
- `wpe/*.sh` -- the spike build: configure, build (ninja -j5), and package as
  an IRIX binary package (`wpewebkit-heliotrope`). Paths still point at the
  workspace's `scratch/wpe`; a pkgsrc `www/wpewebkit` recipe will replace them.
- `tools/` -- IRIX diagnostics used for the memory work: `pmapsgi` (resident
  and private KB per mapping, PIOCMAP_SGI), `mtrace.c` (an `_RLDN32_LIST`
  preload that logs large allocations with callers), `heapscan`, `pagedump`.

Build dependencies come from our pkgsrc fork (MesaLib nb6 or later: lazy
softpipe texture caches) and the IRIX LLVM toolchain (lld 0c6d5a9 or later:
multi-GOT relocations IRIX rld accepts).
