# Heliotrope roadmap

Decided with the user on 2026-09-28. Name stays "Heliotrope" for now (revisit
later; shortlist: Heliotrope, Tyrian, Amaranth, Aperture).

## Doing

1. **Content blocking on by default.** WebKit content filters (JSON rule
   lists, WebKitUserContentFilterStore) loaded from a standard ad/tracker
   list, compiled once and cached on disk.
2. **JavaScript on by default, off per site.** A toolbar toggle; the choice
   is remembered per site.
3. **A load address for every library** (SGI's so_locations idea): our lld
   links every shared object at 0x400000, so IRIX rld moves all but the
   first one at every start (506,000 relocations for libWPEWebKit alone) and
   the relocated data pages stay private (7.6 MB per process for
   libWPEWebKit). Assign distinct bases in the pkgsrc build.
   - Follow-on: **Quickstart** (see below), possibly everywhere.
4. **MIPS IV everywhere** (decided 2026-09-28): pkgsrc packages and WebKit
   build with `-march=mips4 -mtune=r10000`, so R5000, R8000, R10000 and later;
   R4000/R4400/R4600 are out. The toolchain runtimes stay MIPS III. Measure
   the gain against the MIPS III baseline, then rebuild every package (which
   also picks up LLVM f5d5847).
5. **Shrink ICU data** with ICU's build-time data filter.
6. **Tune JSC's heap for small machines** (GC earlier; its sizing reads RAM).
7. **Malloc**: try IRIX `mallopt()` tuning and SGI's libmalloc, then a modern
   allocator.
8. **Frames**: convert only damaged rectangles and cap at 30 fps (easy), then
   our own WPE platform rendering straight into an X MIT-SHM image in the
   visual's format (removes a copy and a full-frame buffer).
9. **Trim the build**: section GC, identical code folding, unused features.
10. **Network process (~50 MB)**: profile (OpenSSL certificate store,
    libsoup) for easy wins.

## Measured (2026-09-28, atom, two runs each)

- MIPS IV + the aligned-block allocator vs MIPS III: no difference beyond run
  noise (Wikipedia ~57 s / 97 MB, GitHub ~220 s / 165 MB, Google ~80 s /
  113 MB for the Web process). MIPS IV stays the target (user: MIPS III not
  a concern). The memalign "2x" was untouched padding, never resident.
- JSC heap tuning (8 MB threshold, slower growth): about -4 MB on GitHub.
- **Lean build** (lean.sh: -Os, -fomit-frame-pointer, --icf=safe; geolocation,
  notifications, pdf.js, smooth scrolling, touch, MHTML, offscreen canvas,
  image-diff off): the default now. Web process 92 / 141 / 105 MB (Wikipedia /
  GitHub / Google), 6-25 MB less than -O2; GitHub loads in ~175 s instead of
  ~215 s, the others unchanged; libWPEWebKit 82 MB stripped instead of 104,
  package 25.5 MB instead of 32.5. tools/phase-*.txt hold every run.
- Blocking: these three pages load nothing on Peter Lowe's list (0 blocked;
  the blocker itself verified on a test page). Add ad-heavy pages (a news
  site, a recipe blog) to the benchmark to measure it.

## Compensation mode (later)

A setting: in compensation mode the page's JavaScript is off and
compensation rendering comes in, so JS-dependent sites still look right.
Sketch (not designed yet): WPE's `enable-javascript-markup=FALSE` blocks the
page's own scripts while browser-injected user scripts still run in an
isolated world, so heliotrope can add one small fix-up script plus a
stylesheet: lazy images (`data-src`/`data-srcset` -> `src`/`srcset`),
content hidden for fade-in effects made visible, full-screen overlays
(cookie walls, "loading") hidden and scrolling restored; `<noscript>` content
shown (check WebKit still does this with markup off). Related: a "calm"
stylesheet that turns off CSS animations and transitions (every frame is a
full repaint here), per-site fixes, and an on-demand reader mode.

## JavaScript speed: replacing the CLoop

JavaScriptCore's interpreter (LLInt) is written in offlineasm, with a backend
per CPU; without one it is emitted as C++ (the CLoop), several times slower.
JSC had MIPS support (LLInt backend, macro-assembler, baseline JIT, DFG) until
WebKit 2.44 removed it as unmaintained; it targeted o32, MIPS32r2 and in
practice little-endian, so it never fit IRIX (n32, MIPS IV: no ins/ext,
seb/seh, 3-operand mul, rotr; big-endian) as it was.

1. **MIPS n32 LLInt backend** (next big JavaScript-speed item, after the lean
   build and the memory work): revive offlineasm's MIPS backend from WebKit
   2.42 and port it to n32, big-endian, MIPS IV. Replaces the CLoop; no JIT,
   no executable-memory pool, so still small-machine friendly. Measure.
2. **Baseline JIT** on top: a MIPS n32 macro-assembler, instruction-cache
   flushing (IRIX `cacheflush`), executable memory. Faster again, but every
   Web process pays for a code pool. On the emulator it also exercises iris's
   self-modifying-code handling.
3. DFG/FTL: not realistic (FTL's B3 backend exists only for x86-64/ARM64).

## Decisions pending

- **Replace softpipe** (compositing now runs on Mesa's software rasterizer,
  which keeps textures in main memory too). Options: llvmpipe (needs LLVM's
  JIT on IRIX); host GL through our libGL shim on the emulator (EGL on the
  shim); on real SGI hardware native GL has no shaders (IMPACT, VPro are
  fixed function), so WebKit's TextureMapper cannot use it directly. Decide
  together with item 8.

## Later

- **The emulator (iris)**: its JIT speeds everything (e.g. loops crossing a
  4 KB page run ~2.4x slower), but does not help real machines.
- A pkgsrc `www/wpewebkit` recipe from `wpe/patches/`, then heliotrope as
  its own package.
- MIPS tail calls in LLVM (with the Rust/Zig ports).

## Known problems

- **Fixed 2026-09-28 (patch 0005, heliotrope 84c9f22 and later):** GitHub
  stalled because IRIX starts processes with 200 descriptors (heliotrope now
  raises the soft limit to the hard one); descriptors leaked into children
  (no SOCK_CLOEXEC/MSG_CMSG_CLOEXEC; children are now spawned without
  "inherit all fds"); datagram IPC dropped messages on ENOBUFS with 16 KB
  buffers (256 KB now, ENOBUFS retried); children outlived heliotrope (now
  PR_TERMCHILD, and heliotrope reloads a page whose Web process died). Still
  open: log EMFILE instead of hanging; consider throttling loads on very
  small machines.
- Packages built before LLVM f5d5847 may carry the MIPS64 truncation bug;
  rebuild pkgsrc with the fixed compiler.

## Quickstart, in short

IRIX's rld can skip relocation entirely. At link time each library gets its
registered address and every GOT entry is resolved against the exact
libraries it was linked with; the object records their names, timestamps and
checksums (liblist, conflict list, msym). At run time, if every library loads
at its address and matches, rld does nothing; otherwise it falls back to
normal relocation. SGI quickstarts all of IRIX this way (`rqs` redoes it
after installs). It needs distinct load addresses first, plus those sections
from lld.
