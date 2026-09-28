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
4. **Two CPU builds.** Keep a MIPS III build (R4000/R4400/R4600), add a
   MIPS IV one (`-mcpu=r10000`: R5000, R8000, R10000 and later). Measure the
   gain on the interpreter first.
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

- GitHub (github.com/techomancer/iris) never finishes loading: the Web
  process grows to ~200 MB in 4 minutes, then stays flat.
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
