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

## Rendering: three kinds of machine

Where the time goes (2026-09-28, SpeedShop on the Web process): 74% in Mesa
softpipe interpreting WebKit's compositing shader per pixel, ~4 s per full
620x640 frame on atom; Cairo/pixman painting only ~1.5%.

| Machine | Compositing path | Notes |
|---|---|---|
| **In iris (emulator)** | host GL passthrough (Mac GPU) or the CPU compositor (3b) | the software-GL items below may not matter here |
| **Real SGI, accelerated** (IMPACT, VPro, Octane, InfiniteReality) | 3b; later maybe a fixed-function OpenGL 1.x compositor | no shaders, so TextureMapper can't use the hardware as is; small texture memory (IMPACT 1-4 MB) |
| **Real SGI, unaccelerated** (Indy XL/XZ, Indigo2 XL, some O2) | 3b | many are 8-bit: heliotrope needs a dithering path (today it requires 24-bit TrueColor) |

**Software-GL path items** (done 2026-09-28; they matter while the Web process
composites with softpipe; revisit per machine class):
1. No gratuitous repaints: no caret blink or animations (heliotrope), tile
   pre-painting in one pass over the visible area (WebKit patch 0008). Idle
   pages stop repainting (before: forever, ~4 s a frame).
2. Damage tracking on (heliotrope): small updates ~0.3 s a frame instead of
   ~3.8 s.
3. **Next, under discussion:** 3a, a copy fast path inside the GL compositor
   (tiles byte-swapped to RGBA at upload, plain copies instead of the shader);
   or 3b, a CPU compositor for the shared-memory path (tiles composited from
   their Cairo images with pixman, no GL, no Mesa in the Web process).
   Recommendation: 3b.
4. llvmpipe (see Decisions pending): needs LLVM's JIT on IRIX and loads
   libLLVM into every Web process.

**What WebKit asks of GL** (2026-09-28, tools/shaders-2026-09-28-nir.txt):
102 GL/EGL entry points (GLES 2 subset); on ordinary pages one fragment
shader draws every tile: `gl_FragColor = u_textureColorSpaceMatrix *
texture2D(s_sampler, uv)`, where the matrix only reorders Cairo's
big-endian ARGB bytes (plus Mesa's solid-colour clear shader). softpipe time:
~81% glDrawArrays (64% the interpreted shader, 15% bilinear sampling), ~4%
uploads and readback, ~9-15% shader and state setup.
- Fixed-function mapping (real SGI): tiles uploaded as GL_BGRA /
  GL_UNSIGNED_INT_8_8_8_8_REV (Cairo's ARGB32 on any byte order), drawn as
  textured quads with GL_REPLACE, or GL_MODULATE + glColor4f for opacity;
  stencil for rounded clips; matrix stack for transforms; CPU fallback for
  blur/filters. A WebKit-shaped "GLES 2 on IRIX libGL.so" layer is realistic.
- Cheap softpipe tweaks: GL_NEAREST for 1:1 tile draws (bilinear is 7%+ of
  softpipe); swap bytes at tile upload so the colour matrix drops out.

Page loads are unchanged by 1-2 (Google ~72 s, Wikipedia ~47 s): each still
needs 2-3 full frames, plus JavaScript (Google ~45 s) and 8-13 s of startup.

## Plan: irix-egl-gles (decided with the user, 2026-09-28)

Everything accelerated or not goes through IRIX's own libGL.so: real
accelerated boards (hardware OpenGL), unaccelerated ones (SGI's software
OpenGL, e.g. REX3/Newport), and iris (the guest-tools libGL.so shim onto the
Mac's OpenGL). So:
1. **irix-egl-gles**: an IRIX-only pkgsrc package depending only on libGL.so
   and GLX, installing Khronos-named libEGL.so.1 and libGLESv2.so.2. It
   implements the ~102 GLES/EGL calls WebKit uses, maps WebKit's compositing
   shaders to fixed-function state (textured quads; GL_MODULATE + colour for
   opacity; stencil for rounded clips), emulates render targets with GLX
   pbuffers (no FBOs in GL 1.1), pads textures to powers of two, and handles
   Cairo's ARGB byte order at upload (no GL_BGRA: SGI_color_matrix during the
   upload, or a CPU swap). Blur and filter effects need a WebKit-side answer.
2. **libepoxy** stays (WebKit calls GL through it) and loads irix-egl-gles's
   libEGL.so.1 / libGLESv2.so.2; there is no second implementation to pick.
3. **heliotrope's own X11 WPE platform**, so WebKit composites straight into
   the window (no readback, no shared-memory copy).
4. **Mesa** is then not needed for heliotrope, and is not a fallback
   (decided 2026-09-28): on a machine irix-egl-gles cannot drive, heliotrope
   stops and says why.

atom's libGL (the shim, 2026-09-28 glxinfo): OpenGL 1.1 "IRIS host OpenGL",
GLX 1.2 with SGIX_fbconfig + SGIX_pbuffer, IMPACT-like extensions
(EXT_packed_pixels, EXT_abgr, SGI_color_matrix, ARB_multitexture,
EXT_texture_object, EXT_subtexture, SGIS_texture_edge_clamp, blend
extensions), 24-bit TrueColor visuals with 8-bit alpha and 8-bit stencil;
no EXT_bgra.

`tools/glcaps.c` (libGL + GLX + X11 only; run `DISPLAY=:0 glcaps` on any
IRIX machine) measures what glxinfo does not and renders the compositing
cases into a pbuffer. atom, 2026-09-28 (`tools/glcaps-atom-2026-09-28.txt`):
- limits: GL_MAX_TEXTURE_SIZE 16384 (proxy accepts RGBA8 up to 8192),
  8 texture units, pbuffers up to 4096x4096 in every RGBA8+stencil config,
  8-bit alpha and stencil in the pbuffer;
- texture memory: all 64 MB probed stayed resident (the Mac GPU; real
  IMPACT has 1-4 MB of TRAM, so this is the number to rerun on hardware);
- speed: upload ~930 MB/s, 512x512 glTexSubImage2D 1.0 ms, 512x512
  glReadPixels 6.5 ms;
- all compositing checks pass: textured quad (GL_REPLACE), opacity via
  GL_MODULATE + premultiplied blend (128,0,0,128 at 0.5), stencil clip, and
  **Cairo's big-endian ARGB made right during the upload by SGI_color_matrix**
  (so no CPU swap is needed where the extension exists).
- one shim difference: iris's libGL cannot create a pbuffer with no context
  current (GLX allows it; `notes/gl-discrepancies.md` #13). irix-egl-gles
  should make a context current before its first pbuffer anyway, so it runs
  on both.
**Decided 2026-09-28: irix-egl-gles adapts at run time.** We have no real
SGI hardware; testers ("friends") will run it on theirs. So nothing is
tuned per board by hand. At eglInitialize, once per display, it probes
(a few milliseconds) and picks a path:
- **max texture size** from proxy textures, not GL_MAX_TEXTURE_SIZE (the
  shim says 16384, the proxy stops at 8192);
- **ARGB upload**: a 2x2 upload through SGI_color_matrix, read back and
  timed; wrong or slow (the matrix done in software) means the CPU swap. The
  extension string alone is not trusted;
- **pbuffers**: always make a context current first (works on GLX and on
  the iris shim); if no RGBA8 + alpha + stencil pbuffer config exists (e.g.
  8-bit Indy XL), eglInitialize fails with a reason;
- **texture memory** is managed rather than measured: textures are kept in
  an LRU with a budget, glAreTexturesResident after uploads, and the budget
  shrinks when residency drops (1-4 MB TRAM on IMPACT, unlimited in iris);
- **power-of-two padding** always (GL 1.1), unless
  ARB_texture_non_power_of_two appears.

**No Mesa fallback; fail clearly** (user, 2026-09-28): falling back to Mesa
on SGI hardware would mean the browser is unusable anyway. Heliotrope
initialises EGL in its own process at startup, before starting WebKit, and
if that fails it exits with the reason irix-egl-gles gives (for example "no
24-bit RGBA pbuffer with alpha and stencil on this display"). The message
goes to stderr and into an X window, since it may be started from the
toolchest with no terminal. The Web process never falls back silently.

For testers: `IRIX_EGL_DEBUG=1` prints what was probed and which path was
chosen, and the package ships glcaps (as `egl-gles-info`) so a tester runs
one command and pastes the output back.

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
