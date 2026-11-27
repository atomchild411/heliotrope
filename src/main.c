/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 atomchild411
 *
 * The licence's full text is in LICENSE.
 */

/*
 * Heliotrope -- a small web browser for IRIX, on WPE WebKit.
 *
 *   heliotrope [-g WIDTHxHEIGHT[+X+Y]] [-o shot.ppm] [-s seconds] [-q] [URL]
 *
 * WebKit renders into WPE's headless platform. With irix-egl-gles (EGL and
 * GLES on IRIX's own OpenGL) it copies each frame on the GPU into the page
 * area's child window; otherwise frames arrive in shared memory and are
 * copied into a 24-bit TrueColor X window. X input goes back to WebKit as
 * WPE events.
 *
 * The menu bar has File (New Tab, New Window, Open Location, Save Page As,
 * Close Tab, Exit), Edit (Copy, Paste, Select All, Find), View (Reload,
 * zoom, JavaScript for the site), Go and Help. The toolbar has
 * Back, Forward, Reload (Stop while loading) and Home, and the address
 * field: click it (or Ctrl+L) and type; Enter goes, Escape cancels. Text
 * that is not an address searches DuckDuckGo Lite. The field also asks for
 * a file name when something is saved ("Save as:", in ~/Downloads) and for
 * text to find ("Find:"). The scrollbar is WebKit's own
 * (HELIOTROPE_OVERLAY_SCROLLBARS=1 for the kind that appears only while
 * scrolling).
 *
 * Tabs: a strip under the toolbar once there are two; they share one Web
 * process and only the one shown renders. The right button gives a link's
 * menu (open here, in a new tab or a new window, save, copy its address),
 * an image's, or the page's; the middle button opens a link in a new tab; a
 * page's new windows open as tabs. New Window starts another heliotrope.
 * Files the browser cannot show are saved. Copy and paste go through the X
 * clipboard (CLIPBOARD and PRIMARY).
 *
 * Keys: Ctrl+T/W new and close tab, Ctrl+PgUp/PgDn (Ctrl+Tab) between tabs,
 * Ctrl+N new window, Ctrl+S save the page, Ctrl+F/G find, Ctrl++/-/0 zoom,
 * Alt+Left/Right back and forward, F5 reload, Escape stop, Ctrl+Q quit.
 *
 * -o writes the page as a PPM image once it has loaded and -s seconds
 * (default 5) have passed; -q then quits. HELIOTROPE_HOME sets Home (default:
 * the first page).
 *
 * Per site: the JS button turns JavaScript off for the current site,
 * remembered in ~/.config/heliotrope/sites. Nothing is blocked by heliotrope
 * itself: filtering is the network's business (DNS, a proxy).
 *
 * Caches: WebKit's HTTP cache (it follows Cache-Control, Expires and
 * revalidation) is in ~/.cache/heliotrope, and scripts' compiled bytecode in
 * ~/.cache/heliotrope/bytecode, trimmed to HELIOTROPE_BYTECODE_CACHE_MB
 * (default 64) at start; HELIOTROPE_NO_BYTECODE_CACHE=1 turns that off.
 *
 * Memory: every site shares one Web process (HELIOTROPE_SITE_ISOLATION=1
 * gives each site its own, as WebKit does by default), caches are sized for
 * documents rather than a big browser, and WebKit is asked to shed caches
 * well before HELIOTROPE_MEMORY_LIMIT (MB per process, default 256).
 */

#include "heliotrope.h"
#include "bytecode.h"
#include "downloads.h"
#include "events.h"
#include "page.h"
#include "sites.h"
#include "tabs.h"
#include "window.h"

#include <sys/resource.h>

/* WebKit's processes on real IRIX: IRIX's own Xlib loaded first. WebKit links
 * pkgsrc's libX11.so.6, whose XOpenDisplay then serves irix-egl-gles too, and
 * SGI's GLX cannot render directly on a display pkgsrc's Xlib opened: it quietly
 * makes every context indirect, rendered by a helper process of the X server,
 * which the kernel kills (RRM_ValidFault "not bound to an rn"); the page then
 * stops drawing. With IRIX's libX11.so.1 first, one Xlib serves both. Only the
 * processes WebKit starts get it (they inherit the environment); this one
 * keeps pkgsrc's. HELIOTROPE_NO_IRIX_XLIB=1 leaves it out;
 * HELIOTROPE_CHILD_PRELOAD adds libraries (tests). */
#define IRIX_XLIB "/usr/lib32/libX11.so.1"

static void use_irix_xlib_for_children(void)
{
    const char *old = g_getenv("_RLDN32_LIST");
    const char *extra = g_getenv("HELIOTROPE_CHILD_PRELOAD");
    GString *list;
    if (env_on("HELIOTROPE_NO_IRIX_XLIB") || access(IRIX_XLIB, R_OK) != 0)
        return;
    list = g_string_new(IRIX_XLIB);
    if (extra && *extra)
        g_string_append_printf(list, ":%s", extra);
    /* rld's list ends with DEFAULT (the usual search); keep one that was set */
    g_string_append_printf(list, ":%s", old && *old ? old : "DEFAULT");
    g_setenv("_RLDN32_LIST", list->str, TRUE);
    g_string_free(list, TRUE);
}

/* IRIX starts processes with 200 descriptors. A page like GitHub loads ~140
 * resources at once, and WebKit passes each chunk of their data between its
 * processes as a shared-memory file descriptor: at 200 the Web process ran
 * out and the page never finished loading. Raise the soft limit to the hard
 * one; the Web and network processes inherit it. */
static void raise_descriptor_limit(void)
{
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max) {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
    }
}

/* Can the web process put frames straight into a window? It can when its
 * EGL (the one libepoxy will open) is irix-egl-gles, which offers
 * eglPresentToWindowIRIX. HELIOTROPE_NO_PRESENT=1 keeps the shared-memory
 * copy. */
static gboolean present_available(void)
{
    const char *path = getenv("EPOXY_EGL_LIB");
    void *lib, *(*get_proc)(const char *);
    gboolean yes = FALSE;

    if (env_on("HELIOTROPE_NO_PRESENT"))
        return FALSE;
    /* libepoxy's own choice (EPOXY_EGL_LIB) first, then pkgsrc's prefix,
     * then the soname wherever rld finds it. */
    lib = dlopen(path && *path ? path : "/usr/pkg/lib/libEGL.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!lib)
        lib = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!lib)
        return FALSE;
    get_proc = (void *(*)(const char *))dlsym(lib, "eglGetProcAddress");
    yes = get_proc && get_proc("eglPresentToWindowIRIX");
    dlclose(lib);
    return yes;
}

int main(int argc, char **argv)
{
    App app = { 0 };
    WPEDisplay *display;
    const char *url = "https://en.wikipedia.org/wiki/IRIX";
    int i;

    app.width = 1024;
    app.height = 768;
    app.shot_delay = 5;
    app.pressed_button = -1;
    app.popup_menu = -1;
    app.popup_hover = -1;
    app.argv0 = argv[0];
    app.tabs = g_ptr_array_new();
    app.waiting_downloads = g_ptr_array_new();
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-g") && i + 1 < argc)
            app.positioned = sscanf(argv[++i], "%dx%d+%d+%d", &app.width, &app.height, &app.x, &app.y) == 4;
        else if (!strcmp(argv[i], "-o") && i + 1 < argc)
            app.shot_path = argv[++i];
        else if (!strcmp(argv[i], "-s") && i + 1 < argc)
            app.shot_delay = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-q"))
            app.quit_after_shot = TRUE;
        else if (!strcmp(argv[i], "-T") && i + 1 < argc) {
            char *text = NULL, **lines;
            GPtrArray *urls = g_ptr_array_new();
            int k;
            if (!g_file_get_contents(argv[++i], &text, NULL, NULL)) {
                g_printerr("heliotrope: cannot read %s\n", argv[i]);
                return 2;
            }
            lines = g_strsplit(text, "\n", -1);
            for (k = 0; lines[k]; k++) {
                char *l = g_strstrip(lines[k]);
                if (*l && *l != '#')
                    g_ptr_array_add(urls, g_strdup(l));
            }
            g_strfreev(lines);
            g_free(text);
            app.tour_n = urls->len;
            g_ptr_array_add(urls, NULL);
            app.tour = (char **)g_ptr_array_free(urls, FALSE);
            if (app.tour_n == 0) {
                g_printerr("heliotrope: %s has no addresses\n", argv[i]);
                return 2;
            }
            url = app.tour[0];
        }
        else if (argv[i][0] == '-') {
            g_printerr("usage: heliotrope [-g WIDTHxHEIGHT[+X+Y]] [-o shot.ppm] [-s seconds] [-q] [-T tour-file] [URL]\n");
            return 2;
        } else
            url = argv[i];
    }
    app.home = g_strdup(getenv("HELIOTROPE_HOME") ? getenv("HELIOTROPE_HOME") : url);
    app.timing = env_on("HELIOTROPE_TIMING");
    raise_descriptor_limit();
    /* WebKit's caches (HTTP, HSTS) then live under ~/.cache/heliotrope */
    g_set_prgname("heliotrope");
    setup_bytecode_cache();
    /* No accessibility bus: asking the D-Bus session bus for it would
     * start a session bus (dbus-launch) when there is none, and wait up
     * to 30 seconds for an answer, before the first page process. Set
     * the variable to a bus address to have one. */
    g_setenv("WEBKIT_A11Y_BUS_ADDRESS", "", FALSE);
    use_irix_xlib_for_children();
    /* Nor a session bus started for anything else (GLib's settings and
     * proxy modules ask for one): with no bus of the user's, an address
     * that fails at once, where GLib would autolaunch a dbus-daemon that
     * outlives the browser. */
    g_setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/heliotrope-no-session-bus", FALSE);

    app.edit = g_string_new(NULL);
    app.present = present_available();
    if (!open_window(&app))
        return 1;
    if (app.present) {
        char id[32];
        /* the web processes inherit it (WebKit patch 0010) */
        g_snprintf(id, sizeof id, "0x%lx", (unsigned long)app.page_win);
        g_setenv("WPE_PRESENT_X11_WINDOW", id, TRUE);
        if (app.timing)
            g_printerr("heliotrope: frames presented into window %s\n", id);
    }

    display = wpe_display_headless_new();
    {
        /* Every repaint costs a full software-composited frame here (seconds on
         * an R10000): no caret blinking (a static page then stops repainting),
         * and ask pages for reduced motion. HELIOTROPE_ANIMATIONS=1 undoes both. */
        WPESettings *wpe_settings = wpe_display_get_settings(display);
        if (!env_on("HELIOTROPE_ANIMATIONS")) {
            wpe_settings_set_uint32(wpe_settings, WPE_SETTING_CURSOR_BLINK_TIME, 0, WPE_SETTINGS_SOURCE_APPLICATION, NULL);
            wpe_settings_set_boolean(wpe_settings, WPE_SETTING_DISABLE_ANIMATIONS, TRUE, WPE_SETTINGS_SOURCE_APPLICATION, NULL);
        }
        /* WebKit's own scrollbar, always shown (the overlay kind appears only
         * while scrolling); HELIOTROPE_OVERLAY_SCROLLBARS=1 for that. */
        if (!env_on("HELIOTROPE_OVERLAY_SCROLLBARS"))
            wpe_settings_set_boolean(wpe_settings, WPE_SETTING_OVERLAY_SCROLLBARS, FALSE, WPE_SETTINGS_SOURCE_APPLICATION, NULL);
    }
    {
        const char *limit = getenv("HELIOTROPE_MEMORY_LIMIT");
        guint mb = limit && atoi(limit) > 0 ? (guint)atoi(limit) : 256;
        WebKitMemoryPressureSettings *pressure = webkit_memory_pressure_settings_new();
        WebKitWebContext *context;
        app.content_manager = webkit_user_content_manager_new();
        if (env_on("HELIOTROPE_FAST_TEXT")) {
            /* WPE shapes all text with HarfBuzz unless text-rendering is
             * optimizeSpeed; this measures plain text with the fast path
             * (no kerning; fonts that ask for features are still shaped). */
            WebKitUserStyleSheet *fast = webkit_user_style_sheet_new(
                "* { text-rendering: optimizeSpeed !important; }",
                WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES, WEBKIT_USER_STYLE_LEVEL_USER, NULL, NULL);
            webkit_user_content_manager_add_style_sheet(app.content_manager, fast);
            webkit_user_style_sheet_unref(fast);
        }
        webkit_memory_pressure_settings_set_memory_limit(pressure, mb);
        webkit_memory_pressure_settings_set_conservative_threshold(pressure, 0.33);
        webkit_memory_pressure_settings_set_strict_threshold(pressure, 0.5);
        webkit_network_session_set_memory_pressure_settings(pressure);
        context = WEBKIT_WEB_CONTEXT(g_object_new(WEBKIT_TYPE_WEB_CONTEXT,
            "memory-pressure-settings", pressure,
            "process-swap-on-cross-site-navigation-enabled", env_on("HELIOTROPE_SITE_ISOLATION"),
            NULL));
        webkit_memory_pressure_settings_free(pressure);
        /* HELIOTROPE_CACHE_MODEL=viewer: no disk cache and the smallest memory caches (for measuring
         * what the network process's cache costs) */
        webkit_web_context_set_cache_model(context, !g_strcmp0(getenv("HELIOTROPE_CACHE_MODEL"), "viewer")
            ? WEBKIT_CACHE_MODEL_DOCUMENT_VIEWER : WEBKIT_CACHE_MODEL_DOCUMENT_BROWSER);
        /* Test mode (HELIOTROPE_PROXY, below): no disk cache, so every run
         * loads everything through the proxy and runs compare. */
        WebKitNetworkSession *test_session = getenv("HELIOTROPE_PROXY") && *getenv("HELIOTROPE_PROXY")
            ? webkit_network_session_new_ephemeral() : NULL;
        app.web_view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "display", display,
            "web-context", context, "user-content-manager", app.content_manager,
            test_session ? "network-session" : NULL, test_session, NULL));
        g_clear_object(&test_session);
        g_object_unref(context);
    }
    app.display = display; /* kept: tabs are made with it */
    /* Test mode, for measuring without the network: every load goes through
     * HELIOTROPE_PROXY (tools/replayproxy.py, which records a site once and
     * plays it back) and TLS certificate errors are ignored so the proxy can
     * read HTTPS. Never for ordinary browsing. */
    if (getenv("HELIOTROPE_PROXY") && *getenv("HELIOTROPE_PROXY")) {
        WebKitNetworkSession *session = webkit_web_view_get_network_session(app.web_view);
        WebKitNetworkProxySettings *proxy = webkit_network_proxy_settings_new(getenv("HELIOTROPE_PROXY"), NULL);
        webkit_network_session_set_proxy_settings(session, WEBKIT_NETWORK_PROXY_MODE_CUSTOM, proxy);
        webkit_network_proxy_settings_free(proxy);
        webkit_network_session_set_tls_errors_policy(session, WEBKIT_TLS_ERRORS_POLICY_IGNORE);
        g_printerr("heliotrope: TEST MODE: all loads through %s, TLS errors ignored, no disk cache\n", getenv("HELIOTROPE_PROXY"));
    }
    select_tab(&app, add_tab(&app, app.web_view));
    g_signal_connect(webkit_web_view_get_network_session(app.web_view), "download-started", G_CALLBACK(download_started), &app);
    g_unix_fd_add(ConnectionNumber(app.dpy), G_IO_IN, x_ready, &app);
    load_sites(&app);
    if (app.tour) {
        g_printerr("heliotrope: tour 1/%d go %.3f %s\n", app.tour_n, wall_now(), url);
        app.tour_timer = g_timeout_add_seconds(120, tour_next, &app);
    }
    load_address(&app, url);

    app.loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(app.loop);
    return 0;
}
