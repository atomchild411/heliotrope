/*
 * glcaps: what an EGL/GLES-on-libGL layer (irix-egl-gles) can count on from
 * this machine's IRIX OpenGL. Uses only libGL.so, GLX and X11.
 *
 *   DISPLAY=:0 glcaps
 *
 * Reports limits (texture size, units, stencil/alpha bits, pbuffer limits),
 * texture memory (residency of 512x512 RGBA textures) and upload speed, then
 * renders into a pbuffer and reads back to check what compositing needs:
 * a textured quad (GL_REPLACE), opacity (GL_MODULATE + blending), stencil
 * clipping, Cairo's big-endian ARGB fixed at upload by SGI_color_matrix, and
 * the fill rate of a 512x512 textured quad.
 */

#include <X11/Xlib.h>
#include <GL/gl.h>
#include <GL/glx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#ifndef GL_COLOR
#define GL_COLOR 0x1800
#endif
#ifndef GL_COLOR_MATRIX_SGI
#define GL_COLOR_MATRIX_SGI 0x80B1
#endif
#ifndef GL_MAX_TEXTURE_UNITS_ARB
#define GL_MAX_TEXTURE_UNITS_ARB 0x84E2
#endif

static int failures;

static double now(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

static int has_ext(const char *list, const char *name)
{
    const char *p = list;
    size_t n = strlen(name);
    while (p && (p = strstr(p, name))) {
        if ((p == list || p[-1] == ' ') && (p[n] == ' ' || p[n] == 0))
            return 1;
        p += n;
    }
    return 0;
}

static void check(const char *what, int ok, const char *detail)
{
    printf("  %-58s %s%s%s\n", what, ok ? "ok" : "FAIL", detail ? "  " : "", detail ? detail : "");
    if (!ok)
        failures++;
}

/* read one RGBA pixel of the current drawable */
static void pixel(int x, int y, unsigned char rgba[4])
{
    glReadPixels(x, y, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
}

static int near(const unsigned char *p, int r, int g, int b, int a)
{
    return abs(p[0] - r) <= 3 && abs(p[1] - g) <= 3 && abs(p[2] - b) <= 3 && (a < 0 || abs(p[3] - a) <= 3);
}

static void texquad(float x0, float y0, float x1, float y1)
{
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(x0, y0);
    glTexCoord2f(1, 0); glVertex2f(x1, y0);
    glTexCoord2f(1, 1); glVertex2f(x1, y1);
    glTexCoord2f(0, 1); glVertex2f(x0, y1);
    glEnd();
}

int main(void)
{
    Display *dpy = XOpenDisplay(NULL);
    int screen, major = 0, minor = 0, nconfigs = 0, i;
    const char *glx_ext, *gl_ext;
    GLXFBConfigSGIX *configs;
    GLXPbufferSGIX pbuffer = 0;
    GLXContext ctx;
    XVisualInfo *vi;
    GLint v[2];
    int pw = 512, ph = 512;
    unsigned char px[4];

    if (!dpy) {
        fprintf(stderr, "glcaps: cannot open display\n");
        return 2;
    }
    screen = DefaultScreen(dpy);
    glXQueryVersion(dpy, &major, &minor);
    glx_ext = glXQueryExtensionsString(dpy, screen);
    printf("GLX %d.%d\n", major, minor);
    if (!has_ext(glx_ext, "GLX_SGIX_fbconfig") || !has_ext(glx_ext, "GLX_SGIX_pbuffer")) {
        printf("no GLX_SGIX_fbconfig/GLX_SGIX_pbuffer: offscreen rendering needs a window here\n");
        return 1;
    }

    /* --- pbuffer-capable configs --- */
    {
        int attrs[] = { GLX_RENDER_TYPE_SGIX, GLX_RGBA_BIT_SGIX, GLX_DRAWABLE_TYPE_SGIX, GLX_PBUFFER_BIT_SGIX,
            GLX_RED_SIZE, 8, GLX_GREEN_SIZE, 8, GLX_BLUE_SIZE, 8, GLX_ALPHA_SIZE, 8, GLX_STENCIL_SIZE, 1, None };
        configs = glXChooseFBConfigSGIX(dpy, screen, attrs, &nconfigs);
    }
    printf("pbuffer configs with RGBA8 + stencil: %d\n", nconfigs);
    if (!nconfigs)
        return 1;
    for (i = 0; i < nconfigs && i < 4; i++) {
        int w = 0, h = 0, pix = 0, a = 0, s = 0, d = 0, db = 0;
        glXGetFBConfigAttribSGIX(dpy, configs[i], GLX_MAX_PBUFFER_WIDTH_SGIX, &w);
        glXGetFBConfigAttribSGIX(dpy, configs[i], GLX_MAX_PBUFFER_HEIGHT_SGIX, &h);
        glXGetFBConfigAttribSGIX(dpy, configs[i], GLX_MAX_PBUFFER_PIXELS_SGIX, &pix);
        glXGetFBConfigAttribSGIX(dpy, configs[i], GLX_ALPHA_SIZE, &a);
        glXGetFBConfigAttribSGIX(dpy, configs[i], GLX_STENCIL_SIZE, &s);
        glXGetFBConfigAttribSGIX(dpy, configs[i], GLX_DEPTH_SIZE, &d);
        glXGetFBConfigAttribSGIX(dpy, configs[i], GLX_DOUBLEBUFFER, &db);
        printf("  config %d: max pbuffer %dx%d (%d pixels), alpha %d, stencil %d, depth %d, double %d\n", i, w, h, pix, a, s, d, db);
    }
    {
        int pattrs[] = { GLX_PRESERVED_CONTENTS_SGIX, True, GLX_LARGEST_PBUFFER_SGIX, False, None };
        pbuffer = glXCreateGLXPbufferSGIX(dpy, configs[0], pw, ph, pattrs);
    }
    vi = glXGetVisualFromFBConfigSGIX(dpy, configs[0]);
    ctx = glXCreateContextWithConfigSGIX(dpy, configs[0], GLX_RGBA_TYPE_SGIX, NULL, True);
    if (!pbuffer && ctx && vi) {
        /* GLX allows a pbuffer with no context current; the iris host GL
         * shim does not (it makes the pbuffer in the current context's share
         * group). Make the context current on an unmapped window and retry. */
        int pattrs[] = { GLX_PRESERVED_CONTENTS_SGIX, True, None };
        XSetWindowAttributes swa;
        Window w;
        swa.colormap = XCreateColormap(dpy, RootWindow(dpy, screen), vi->visual, AllocNone);
        swa.border_pixel = 0;
        w = XCreateWindow(dpy, RootWindow(dpy, screen), 0, 0, 16, 16, 0, vi->depth, InputOutput, vi->visual,
            CWColormap | CWBorderPixel, &swa);
        printf("NOTE: pbuffer creation with no current context failed; retrying with a context current\n");
        if (glXMakeCurrent(dpy, w, ctx))
            pbuffer = glXCreateGLXPbufferSGIX(dpy, configs[0], pw, ph, pattrs);
        printf("      with a context current: %s\n", pbuffer ? "ok" : "still fails");
    }
    if (!pbuffer || !ctx || !glXMakeCurrent(dpy, pbuffer, ctx)) {
        printf("cannot make a %dx%d pbuffer current (pbuffer %lu, context %p)\n", pw, ph, (unsigned long)pbuffer, (void *)ctx);
        return 1;
    }
    (void)vi;

    /* --- limits --- */
    gl_ext = (const char *)glGetString(GL_EXTENSIONS);
    printf("\nGL_VENDOR   %s\nGL_RENDERER %s\nGL_VERSION  %s\n", glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION));
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, v);
    printf("GL_MAX_TEXTURE_SIZE %d", v[0]);
    {
        int size, largest = 0;
        for (size = 256; size <= 8192; size *= 2) {
            GLint w = 0;
            glTexImage2D(GL_PROXY_TEXTURE_2D, 0, GL_RGBA8_EXT, size, size, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            glGetTexLevelParameteriv(GL_PROXY_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
            if (w == size)
                largest = size;
        }
        printf(" (largest RGBA8 accepted by proxy: %d)\n", largest);
    }
    if (has_ext(gl_ext, "GL_ARB_multitexture")) {
        glGetIntegerv(GL_MAX_TEXTURE_UNITS_ARB, v);
        printf("GL_MAX_TEXTURE_UNITS_ARB %d\n", v[0]);
    }
    glGetIntegerv(GL_MAX_VIEWPORT_DIMS, v);
    printf("GL_MAX_VIEWPORT_DIMS %dx%d\n", v[0], v[1]);
    glGetIntegerv(GL_ALPHA_BITS, v);
    printf("GL_ALPHA_BITS %d", v[0]);
    glGetIntegerv(GL_STENCIL_BITS, v);
    printf("  GL_STENCIL_BITS %d\n", v[0]);
    printf("extensions this layer would use: packed_pixels %s, abgr %s, bgra %s, color_matrix %s, edge_clamp %s, texture_object %s, subtexture %s\n",
        has_ext(gl_ext, "GL_EXT_packed_pixels") ? "yes" : "no", has_ext(gl_ext, "GL_EXT_abgr") ? "yes" : "no",
        has_ext(gl_ext, "GL_EXT_bgra") ? "yes" : "no", has_ext(gl_ext, "GL_SGI_color_matrix") ? "yes" : "no",
        has_ext(gl_ext, "GL_SGIS_texture_edge_clamp") ? "yes" : "no", has_ext(gl_ext, "GL_EXT_texture_object") ? "yes" : "no",
        has_ext(gl_ext, "GL_EXT_subtexture") ? "yes" : "no");

    /* --- texture memory and upload speed --- */
    {
        enum { N = 64, S = 512 };
        GLuint tex[N];
        GLboolean resident[N];
        unsigned char *data = malloc(S * S * 4);
        int n, fits = 0;
        double t0, t1;
        memset(data, 0x80, S * S * 4);
        glGenTextures(N, tex);
        t0 = now();
        for (n = 0; n < N; n++) {
            glBindTexture(GL_TEXTURE_2D, tex[n]);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8_EXT, S, S, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
            if (glAreTexturesResident(n + 1, tex, resident))
                fits = n + 1;
        }
        glFinish();
        t1 = now();
        printf("\ntexture memory: %d of %d 512x512 RGBA textures resident at once (%d MB)\n", fits, N, fits);
        printf("upload: %d x 1 MB in %.2f s = %.1f MB/s\n", N, t1 - t0, N / (t1 - t0));
        t0 = now();
        for (n = 0; n < 16; n++) {
            glBindTexture(GL_TEXTURE_2D, tex[0]);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, S, S, GL_RGBA, GL_UNSIGNED_BYTE, data);
        }
        glFinish();
        t1 = now();
        printf("glTexSubImage2D 512x512: %.1f ms each\n", (t1 - t0) * 1000 / 16);
        glDeleteTextures(N, tex);
        free(data);
    }

    /* --- functional checks, rendered into the pbuffer --- */
    printf("\ncompositing checks (render into the pbuffer, read back):\n");
    glViewport(0, 0, pw, ph);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glOrtho(0, pw, 0, ph, -1, 1);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    {
        /* a 2x2 texture: red, green / blue, white (RGBA bytes) */
        static const unsigned char rgba[16] = { 255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255 };
        /* the same colours as Cairo ARGB32 on a big-endian CPU: bytes A, R, G, B */
        static const unsigned char argb[16] = { 255, 255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255 };
        GLuint t;
        char detail[128];
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8_EXT, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
        glEnable(GL_TEXTURE_2D);

        /* 1. plain textured quad */
        glClearColor(0, 0, 0, 0);
        glClear(GL_COLOR_BUFFER_BIT);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
        texquad(0, 0, 200, 200);
        glFinish();
        pixel(50, 50, px);
        sprintf(detail, "red corner = %d,%d,%d,%d", px[0], px[1], px[2], px[3]);
        check("textured quad, GL_REPLACE", near(px, 255, 0, 0, 255), detail);
        pixel(150, 150, px);
        check("  other corner (white)", near(px, 255, 255, 255, 255), NULL);

        /* 2. opacity 0.5 over a black background, premultiplied as WebKit does */
        glClear(GL_COLOR_BUFFER_BIT);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
        glColor4f(0.5f, 0.5f, 0.5f, 0.5f);
        texquad(0, 0, 200, 200);
        glFinish();
        glDisable(GL_BLEND);
        pixel(50, 50, px);
        sprintf(detail, "= %d,%d,%d,%d", px[0], px[1], px[2], px[3]);
        check("opacity 0.5 (GL_MODULATE, premultiplied blend)", near(px, 128, 0, 0, 128), detail);
        glColor4f(1, 1, 1, 1);

        /* 3. stencil clip: only the left half may be drawn */
        glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        glEnable(GL_STENCIL_TEST);
        glStencilFunc(GL_ALWAYS, 1, 1);
        glStencilOp(GL_REPLACE, GL_REPLACE, GL_REPLACE);
        glColorMask(0, 0, 0, 0);
        glDisable(GL_TEXTURE_2D);
        texquad(0, 0, 100, 200);
        glEnable(GL_TEXTURE_2D);
        glColorMask(1, 1, 1, 1);
        glStencilFunc(GL_EQUAL, 1, 1);
        glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
        glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
        texquad(0, 0, 200, 200);
        glFinish();
        glDisable(GL_STENCIL_TEST);
        {
            unsigned char in[4], out[4];
            pixel(50, 50, in);
            pixel(150, 50, out);
            sprintf(detail, "inside %d,%d,%d  outside %d,%d,%d", in[0], in[1], in[2], out[0], out[1], out[2]);
            check("stencil clip", near(in, 255, 0, 0, 255) && near(out, 0, 0, 0, 0), detail);
        }

        /* 4. Cairo ARGB bytes, reordered by SGI_color_matrix during upload.
         * Uploaded as RGBA the texel reads r=A g=R b=G a=B; the matrix maps
         * that back to (R, G, B, A). */
        if (has_ext(gl_ext, "GL_SGI_color_matrix")) {
            static const GLfloat m[16] = { /* column-major: out = M * in */
                0, 0, 0, 1,   1, 0, 0, 0,   0, 1, 0, 0,   0, 0, 1, 0 };
            glMatrixMode(GL_COLOR);
            glLoadMatrixf(m);
            glMatrixMode(GL_MODELVIEW);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8_EXT, 2, 2, 0, GL_RGBA, GL_UNSIGNED_BYTE, argb);
            glMatrixMode(GL_COLOR);
            glLoadIdentity();
            glMatrixMode(GL_MODELVIEW);
            glClear(GL_COLOR_BUFFER_BIT);
            texquad(0, 0, 200, 200);
            glFinish();
            {
                unsigned char a[4], b[4];
                pixel(50, 50, a);
                pixel(150, 50, b);
                sprintf(detail, "red %d,%d,%d,%d green %d,%d,%d,%d", a[0], a[1], a[2], a[3], b[0], b[1], b[2], b[3]);
                check("ARGB upload fixed by SGI_color_matrix", near(a, 255, 0, 0, 255) && near(b, 0, 255, 0, 255), detail);
            }
        } else
            check("ARGB upload fixed by SGI_color_matrix", 0, "no GL_SGI_color_matrix");

        /* 5. fill rate: a 512x512 textured quad */
        {
            enum { S = 512 };
            unsigned char *data = malloc(S * S * 4);
            double t0, t1;
            int n;
            memset(data, 0xa0, S * S * 4);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8_EXT, S, S, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
            glFinish();
            t0 = now();
            for (n = 0; n < 20; n++)
                texquad(0, 0, S, S);
            glFinish();
            t1 = now();
            printf("  fill rate: 512x512 textured quad in %.2f ms = %.1f Mpixel/s\n", (t1 - t0) * 1000 / 20, 20.0 * S * S / (t1 - t0) / 1e6);
            t0 = now();
            for (n = 0; n < 5; n++) {
                glReadPixels(0, 0, S, S, GL_RGBA, GL_UNSIGNED_BYTE, data);
            }
            t1 = now();
            printf("  readback: 512x512 glReadPixels in %.1f ms\n", (t1 - t0) * 1000 / 5);
            free(data);
        }
        glDeleteTextures(1, &t);
    }
    printf("\n%s\n", failures ? "SOME CHECKS FAILED" : "all compositing checks passed");
    glXMakeCurrent(dpy, None, NULL);
    glXDestroyGLXPbufferSGIX(dpy, pbuffer);
    XCloseDisplay(dpy);
    return failures ? 1 : 0;
}
