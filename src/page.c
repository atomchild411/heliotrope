/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 atomchild411
 *
 * The licence's full text is in LICENSE.
 */

/*
 * A page's life: what the address field's text loads (an address or a
 * DuckDuckGo Lite search), WebKit's load, title, state and crash signals for
 * each tab, titles made Latin-1 for X, the link under the pointer and timed
 * messages in the status line, -T tours, and find in page.
 */

#include "heliotrope.h"
#include "draw.h"
#include "frames.h"
#include "page.h"
#include "sites.h"
#include "tabs.h"
#include "window.h"

#include <unicode/utrans.h>
#include <unicode/ustring.h>

double wall_now(void) { return g_get_real_time() / 1e6; }

/* The tour's next page, or the end of it. */
gboolean tour_next(gpointer data)
{
    App *app = data;
    app->tour_timer = 0;
    g_printerr("heliotrope: tour %d/%d leave %.3f %s\n", app->tour_i + 1, app->tour_n, wall_now(),
        webkit_web_view_get_uri(app->web_view));
    if (++app->tour_i >= app->tour_n) {
        g_printerr("heliotrope: tour end %.3f\n", wall_now());
        g_main_loop_quit(app->loop);
        return G_SOURCE_REMOVE;
    }
    g_printerr("heliotrope: tour %d/%d go %.3f %s\n", app->tour_i + 1, app->tour_n, wall_now(), app->tour[app->tour_i]);
    app->tour_loaded = FALSE;
    app->tour_timer = g_timeout_add_seconds(90, tour_next, app);
    load_address(app, app->tour[app->tour_i]);
    return G_SOURCE_REMOVE;
}

void load_changed(WebKitWebView *web_view, WebKitLoadEvent event, Tab *tab)
{
    App *app = tab->app;
    if (event == WEBKIT_LOAD_STARTED || event == WEBKIT_LOAD_REDIRECTED)
        apply_site_choices(app, web_view);
    if (tab != app->tab) {
        draw_tabs(app);
        XFlush(app->dpy);
        return;
    }
    if (event == WEBKIT_LOAD_STARTED) {
        app->load_start = g_get_monotonic_time();
        app->first_frame_seen = FALSE;
        app->frames = 0;
        timing_mark(app, "load started");
    } else if (event == WEBKIT_LOAD_COMMITTED) {
        app->first_frame_seen = FALSE; /* frames of the old page don't count */
        timing_mark(app, "committed (first bytes of the page)");
    } else if (event == WEBKIT_LOAD_FINISHED)
        timing_mark(app, "load finished");
    if (event == WEBKIT_LOAD_FINISHED) {
        g_printerr("heliotrope: loaded %s\n", webkit_web_view_get_uri(app->web_view));
        if (app->shot_path && !app->shot_done)
            g_timeout_add_seconds(app->shot_delay, take_shot, app);
        /* On a tour: the page stays -s seconds from its first finish. */
        if (app->tour && !app->tour_loaded) {
            app->tour_loaded = TRUE;
            if (app->tour_timer)
                g_source_remove(app->tour_timer);
            g_printerr("heliotrope: tour %d/%d loaded %.3f in %.1f s\n", app->tour_i + 1, app->tour_n, wall_now(),
                (g_get_monotonic_time() - app->load_start) / 1e6);
            app->tour_timer = g_timeout_add_seconds(app->shot_delay, tour_next, app);
        }
    }
    draw_bar(app);
    draw_tabs(app);
    XFlush(app->dpy);
}

gboolean load_failed(WebKitWebView *web_view, WebKitLoadEvent event, char *uri, GError *error, Tab *tab)
{
    (void)web_view;
    (void)event;
    (void)tab;
    g_printerr("heliotrope: failed to load %s: %s\n", uri, error->message);
    return FALSE; /* WebKit shows its error page */
}

/* A UTF-8 title as Latin-1, which is what 4Dwm can show in WM_NAME (in its C
 * locale a COMPOUND_TEXT title comes out garbled and cut short): other
 * scripts transliterated (Cyrillic as BGN's: Википедия -> Vikipediya; others
 * ICU's Any-Latin: Ελληνικά -> Ellēniká, かな -> kana), letters Latin-1 lacks
 * folded (ē -> e, Ł -> L), and
 * Han left as "?" (its Latin is Mandarin's, wrong on a Japanese page). */
static char *latin1_title(const char *utf8)
{
    static UTransliterator *tr;
    static gboolean tried;
    glong n = 0;
    gunichar2 *u16;
    char *text = NULL, *latin1;
    const char *c;

    /* Most titles are Latin-1 already: no ICU then. Making the
     * transliterator costs a few percent of a page load's CPU time. */
    for (c = utf8; *c; c = g_utf8_next_char(c))
        if (g_utf8_get_char(c) > 0xff)
            break;
    if (!*c)
        return g_convert(utf8, -1, "ISO-8859-1", "UTF-8", NULL, NULL, NULL);

    if (!tried) {
        /* Punctuation first (an em dash), then Cyrillic as BGN's, the other
         * scripts but Han, and what is left outside Latin-1. As one rule set:
         * with BGN's step leading, the steps after it took no effect. */
        static const char rules[] =
            "::[^\\u0000-\\u00ff] Latin-ASCII; ::[:Cyrillic:] Russian-Latin/BGN; "
            "::[:^Han:] Any-Latin; ::[^\\u0000-\\u00ff] Latin-ASCII;";
        UErrorCode err = U_ZERO_ERROR;
        UParseError where;
        UChar id[32], text_rules[sizeof rules];
        u_uastrcpy(id, "Heliotrope-Title");
        u_uastrcpy(text_rules, rules);
        tr = utrans_openU(id, -1, UTRANS_FORWARD, text_rules, -1, &where, &err);
        if (U_FAILURE(err))
            tr = NULL;
        tried = TRUE;
    }
    if (tr && (u16 = g_utf8_to_utf16(utf8, -1, NULL, &n, NULL)) != NULL) {
        /* Room for the Latin, which may be longer (one kana, two letters). */
        int32_t len = (int32_t)n, cap = len * 4 + 16, limit = len;
        UChar *buf = g_new(UChar, cap);
        UErrorCode err = U_ZERO_ERROR;
        memcpy(buf, u16, (size_t)len * sizeof(UChar));
        utrans_transUChars(tr, buf, &len, cap, 0, &limit, &err);
        if (U_SUCCESS(err))
            text = g_utf16_to_utf8((const gunichar2 *)buf, len, NULL, NULL, NULL);
        g_free(buf);
        g_free(u16);
    }
    latin1 = g_convert_with_fallback(text ? text : utf8, -1, "ISO-8859-1", "UTF-8", "?", NULL, NULL, NULL);
    g_free(text);
    return latin1;
}

void show_title(App *app)
{
    const char *title = webkit_web_view_get_title(app->web_view);
    char *name = g_strdup_printf("Heliotrope%s%s", title && *title ? " - " : "", title ? title : "");
    char *latin1 = latin1_title(name);
    /* WM_NAME and WM_ICON_NAME as 4Dwm can show them (Latin-1 STRING), and
     * the title itself as _NET_WM_NAME for window managers that take UTF-8. */
    XStoreName(app->dpy, app->win, latin1 ? latin1 : name);
    XSetIconName(app->dpy, app->win, latin1 ? latin1 : name);
    XChangeProperty(app->dpy, app->win, XInternAtom(app->dpy, "_NET_WM_NAME", False),
                    XInternAtom(app->dpy, "UTF8_STRING", False), 8, PropModeReplace,
                    (const unsigned char *)name, (int)strlen(name));
    g_free(latin1);
    g_free(name);
}

void title_changed(GObject *object, GParamSpec *pspec, Tab *tab)
{
    const char *title = webkit_web_view_get_title(tab->web_view);
    (void)object;
    (void)pspec;
    g_free(tab->title);
    tab->title = title && *title ? latin1_title(title) : NULL;
    if (tab == tab->app->tab)
        show_title(tab->app);
    draw_tabs(tab->app);
    XFlush(tab->app->dpy);
}

void state_changed(GObject *object, GParamSpec *pspec, Tab *tab)
{
    App *app = tab->app;
    (void)object;
    (void)pspec;
    if (tab != app->tab)
        return;
    app->progress_value = webkit_web_view_get_estimated_load_progress(app->web_view);
    draw_bar(app);
    XFlush(app->dpy);
}

/* The Web process died (a crash, or killed for memory): reload once; if the page
 * kills it again within a minute, show why instead of looping. */
void web_process_terminated(WebKitWebView *web_view, WebKitWebProcessTerminationReason reason, Tab *tab)
{
    App *app = tab->app;
    static gint64 last;
    gint64 now = g_get_monotonic_time();
    const char *why = reason == WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT ? "it used too much memory"
        : reason == WEBKIT_WEB_PROCESS_CRASHED ? "it crashed" : "it was terminated";
    (void)app;
    g_printerr("heliotrope: the Web process ended (%s)\n", why);
    if (last && now - last < 60 * G_USEC_PER_SEC) {
        char *html = g_strdup_printf("<body style='font-family:sans-serif;margin:3em'><h2>This page stopped</h2>"
            "<p>The page's process ended because %s, twice within a minute.</p></body>", why);
        webkit_web_view_load_alternate_html(web_view, html, webkit_web_view_get_uri(web_view), NULL);
        g_free(html);
    } else
        webkit_web_view_reload(web_view);
    last = now;
}

void mouse_target_changed(WebKitWebView *web_view, WebKitHitTestResult *hit, guint modifiers, Tab *tab)
{
    App *app = tab->app;
    const char *link = webkit_hit_test_result_context_is_link(hit) ? webkit_hit_test_result_get_link_uri(hit) : NULL;
    (void)web_view;
    (void)modifiers;
    if (tab != app->tab)
        return;
    if (g_strcmp0(link, app->status)) {
        g_free(app->status);
        app->status = g_strdup(link);
        draw_page(app);
    }
    set_cursor(app, link ? app->hand : webkit_hit_test_result_context_is_editable(hit) ? app->text : app->arrow);
    XFlush(app->dpy);
}

/* An address, or else a search: text with a space, or with no dot, colon or slash. */
void load_address(App *app, const char *text)
{
    char *uri;
    while (*text == ' ')
        text++;
    if (!*text)
        return;
    if (strstr(text, "://") || g_str_has_prefix(text, "about:") || g_str_has_prefix(text, "data:"))
        uri = g_strdup(text);
    else if (strchr(text, ' ') || !strpbrk(text, ".:/")) {
        char *q = g_uri_escape_string(text, NULL, TRUE);
        uri = g_strconcat("https://lite.duckduckgo.com/lite/?q=", q, NULL);
        g_free(q);
    } else
        uri = g_strconcat("https://", text, NULL);
    webkit_web_view_load_uri(app->web_view, uri);
    g_free(uri);
}

/* --- the status line's messages ---------------------------------------------- */

static gboolean message_expired(gpointer data)
{
    App *app = data;
    app->message_timer = 0;
    g_clear_pointer(&app->message, g_free);
    draw_page(app);
    XFlush(app->dpy);
    return G_SOURCE_REMOVE;
}

/* seconds 0: until the next message */
void show_message(App *app, const char *text, int seconds)
{
    g_free(app->message);
    app->message = g_strdup(text);
    if (app->message_timer)
        g_source_remove(app->message_timer);
    app->message_timer = seconds > 0 ? g_timeout_add_seconds((guint)seconds, message_expired, app) : 0;
    draw_page(app);
    XFlush(app->dpy);
}

/* --- find in page ------------------------------------------------------------ */

static void find_failed(WebKitFindController *find, App *app)
{
    char *text = g_strdup_printf("Not found: %s", webkit_find_controller_get_search_text(find));
    show_message(app, text, 4);
    g_free(text);
}

void find_text(App *app, const char *text)
{
    WebKitFindController *find = webkit_web_view_get_find_controller(app->web_view);
    if (!g_object_get_data(G_OBJECT(find), "heliotrope-find")) {
        g_object_set_data(G_OBJECT(find), "heliotrope-find", GINT_TO_POINTER(1));
        g_signal_connect(find, "failed-to-find-text", G_CALLBACK(find_failed), app);
    }
    g_free(app->find_text);
    app->find_text = g_strdup(text);
    webkit_find_controller_search(find, text, WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE | WEBKIT_FIND_OPTIONS_WRAP_AROUND, G_MAXUINT);
}
