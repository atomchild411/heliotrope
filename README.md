# Heliotrope

A small browser for IRIX 6.5 (n32, big-endian MIPS) built on WPE WebKit 2.52.6:
Cairo rendering, the CLoop JavaScript interpreter, WPEPlatform headless on Mesa
softpipe EGL, shown in an X window.

- `src/` -- the browser, in C, one module per part (`make` builds it; the
  Makefile's top says how to point it elsewhere):
  - `heliotrope.h` -- the shared types: `App` (the browser's whole state,
    every function's first argument), `Tab`, `MenuItem`, layout constants.
  - `main.c` -- start-up: options, WebKit's network session and settings,
    the environment for WebKit's processes, the main loop.
  - `window.c` -- the X window (visual, colours, fonts, cursors) and the
    geometry of its parts.
  - `draw.c` -- the toolbar, address field and status line.
  - `frames.c` -- WebKit's frames into the window (shared memory or
    present mode), `-o` screenshots.
  - `page.c` -- loads, titles, status messages, `-T` tours, find in page.
  - `field.c` -- the address field and its prompts (address, Save as, Find).
  - `events.c` -- X events and keyboard shortcuts.
  - `tabs.c` -- WebKit views as tabs, the tab strip, new windows.
  - `menus.c` -- the menu bar, popups, the context menu, and `do_action()`
    for every menu choice.
  - `sites.c`, `clipboard.c`, `downloads.c`, `bytecode.c` -- per-site
    choices, the X clipboard, downloads, compiled JavaScript on disk.

  Each `.c` has a header of the same name declaring what other modules may
  call; everything else in it is `static`.
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
