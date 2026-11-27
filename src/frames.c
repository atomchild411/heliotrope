/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 atomchild411
 *
 * The licence's full text is in LICENSE.
 */

/*
 * WebKit's frames. In present mode irix-egl-gles copies them on the GPU and
 * nothing reaches us; otherwise each WPE buffer arrives here and is converted
 * into an XImage (MIT-SHM when the server allows it) in the visual's format.
 * Also -o's PPM screenshot and HELIOTROPE_TIMING's load timeline.
 */

#include "heliotrope.h"
#include "draw.h"
#include "frames.h"
#include "window.h"

#include <sys/ipc.h>
#include <sys/shm.h>

static int shm_failed;

static int shm_error_handler(Display *dpy, XErrorEvent *e)
{
    (void)dpy;
    (void)e;
    shm_failed = 1;
    return 0;
}

static void free_image(App *app)
{
    if (!app->image)
        return;
    if (app->use_shm) {
        XShmDetach(app->dpy, &app->shm);
        XSync(app->dpy, False);
        XDestroyImage(app->image);
        shmdt(app->shm.shmaddr);
    } else
        XDestroyImage(app->image);
    app->image = NULL;
}

static void make_image(App *app, int w, int h)
{
    free_image(app);
    app->image_width = w;
    app->image_height = h;
    if (app->use_shm) {
        app->image = XShmCreateImage(app->dpy, app->visual, app->depth, ZPixmap, NULL, &app->shm, w, h);
        if (app->image) {
            app->shm.shmid = shmget(IPC_PRIVATE, (size_t)app->image->bytes_per_line * h, IPC_CREAT | 0600);
            app->shm.shmaddr = app->shm.shmid >= 0 ? shmat(app->shm.shmid, NULL, 0) : (char *)-1;
            if (app->shm.shmaddr != (char *)-1) {
                XErrorHandler old;
                app->image->data = app->shm.shmaddr;
                app->shm.readOnly = False;
                shm_failed = 0;
                old = XSetErrorHandler(shm_error_handler);
                XShmAttach(app->dpy, &app->shm);
                XSync(app->dpy, False);
                XSetErrorHandler(old);
                /* IRIX cannot attach a segment already marked for removal: only now. */
                shmctl(app->shm.shmid, IPC_RMID, NULL);
                if (!shm_failed)
                    return;
                shmdt(app->shm.shmaddr);
            } else if (app->shm.shmid >= 0)
                shmctl(app->shm.shmid, IPC_RMID, NULL);
            app->image->data = NULL;
            XDestroyImage(app->image);
            app->image = NULL;
        }
        g_printerr("heliotrope: X shared memory unavailable, using XPutImage\n");
        app->use_shm = FALSE;
    }
    app->image = XCreateImage(app->dpy, app->visual, app->depth, ZPixmap, 0,
        malloc((size_t)w * h * 4), w, h, 32, 0);
}

static void write_ppm(App *app)
{
    FILE *f = fopen(app->shot_path, "wb");
    if (!f) {
        perror(app->shot_path);
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", app->last_width, app->last_height);
    fwrite(app->last_rgb, 3, (size_t)app->last_width * app->last_height, f);
    fclose(f);
    g_printerr("heliotrope: wrote %s (%dx%d)\n", app->shot_path, app->last_width, app->last_height);
}

void timing_mark(App *app, const char *what)
{
    if (app->timing && app->load_start)
        g_printerr("heliotrope: +%6.1f s  %s (%u frames)\n", (g_get_monotonic_time() - app->load_start) / 1e6, what, app->frames);
}

void buffer_rendered(WPEView *view, WPEBuffer *buffer, Tab *tab)
{
    App *app = tab->app;
    gint64 t0 = g_get_monotonic_time();
    if (tab != app->tab)
        return;
    app->frames++;
    if (!app->first_frame_seen && app->load_start) {
        app->first_frame_seen = TRUE;
        timing_mark(app, "first frame");
    } else if (app->timing && app->load_start)
        timing_mark(app, "frame");
    if (app->present) {
        /* WebKit has already copied the frame into page_win. */
        if (app->timing)
            g_printerr("heliotrope:          frame presented by WebKit\n");
        return;
    }
    GError *error = NULL;
    /* transfer none: the pixels belong to the buffer (valid until it is released). */
    GBytes *bytes = wpe_buffer_import_to_pixels(buffer, &error);
    int w = wpe_buffer_get_width(buffer), h = wpe_buffer_get_height(buffer);
    const guint8 *src;
    gsize size;
    guint stride;
    int x, y;
    gboolean direct;
    (void)view;

    if (!bytes) {
        g_printerr("heliotrope: frame: %s\n", error ? error->message : "no pixels");
        g_clear_error(&error);
        return;
    }
    src = g_bytes_get_data(bytes, &size);
    stride = WPE_IS_BUFFER_SHM(buffer) ? wpe_buffer_shm_get_stride(WPE_BUFFER_SHM(buffer)) : (guint)w * 4;
    if ((gsize)stride * h > size)
        return;

    if (!app->image || app->image_width != w || app->image_height != h)
        make_image(app, w, h);
    if (app->shot_path && (app->last_width != w || app->last_height != h)) {
        g_free(app->last_rgb);
        app->last_rgb = g_malloc((size_t)w * h * 3);
        app->last_width = w;
        app->last_height = h;
    }

    /* ARGB8888 as DRM defines it: a little-endian 32-bit word, so B, G, R, A
     * in memory whatever the host's byte order. Alpha is premultiplied, and
     * the page is opaque: drop it. A 32-bit image in the host's byte order is
     * written directly; anything else goes through XPutPixel. */
    direct = app->image->bits_per_pixel == 32
        && app->image->byte_order == (G_BYTE_ORDER == G_BIG_ENDIAN ? MSBFirst : LSBFirst);
    for (y = 0; y < h; y++) {
        const guint8 *p = src + (gsize)y * stride;
        guint32 *out = (guint32 *)(app->image->data + (gsize)y * app->image->bytes_per_line);
        guint8 *rgb = app->last_rgb ? app->last_rgb + (gsize)y * w * 3 : NULL;
        for (x = 0; x < w; x++, p += 4) {
            guint32 pixel = ((guint32)p[2] << app->rshift) | ((guint32)p[1] << app->gshift)
                | ((guint32)p[0] << app->bshift);
            if (direct)
                out[x] = pixel;
            else
                XPutPixel(app->image, x, y, pixel);
            if (rgb) {
                *rgb++ = p[2];
                *rgb++ = p[1];
                *rgb++ = p[0];
            }
        }
    }
    draw_page(app);
    XFlush(app->dpy);
    if (app->timing)
        g_printerr("heliotrope:          frame %dx%d copied to X in %.0f ms\n", w, h, (g_get_monotonic_time() - t0) / 1e3);
}

/* The page as shown, read back from the window (present mode, for -o). */
static void grab_page(App *app)
{
    int w = page_width(app), h = page_height(app), x, y;
    XImage *img = XGetImage(app->dpy, app->page_win, 0, 0, w, h, AllPlanes, ZPixmap);
    unsigned long rm = app->visual->red_mask, gm = app->visual->green_mask, bm = app->visual->blue_mask;
    guint8 *rgb;

    if (!img)
        return;
    g_free(app->last_rgb);
    app->last_rgb = rgb = g_malloc((size_t)w * h * 3);
    app->last_width = w;
    app->last_height = h;
    for (y = 0; y < h; y++)
        for (x = 0; x < w; x++) {
            unsigned long p = XGetPixel(img, x, y);
            *rgb++ = (guint8)((p & rm) >> app->rshift);
            *rgb++ = (guint8)((p & gm) >> app->gshift);
            *rgb++ = (guint8)((p & bm) >> app->bshift);
        }
    XDestroyImage(img);
}

gboolean take_shot(gpointer data)
{
    App *app = data;
    if (app->present)
        grab_page(app);
    if (app->last_rgb)
        write_ppm(app);
    app->shot_done = TRUE;
    if (app->quit_after_shot)
        g_main_loop_quit(app->loop);
    return G_SOURCE_REMOVE;
}
