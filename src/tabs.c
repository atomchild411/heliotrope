/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 atomchild411
 *
 * The licence's full text is in LICENSE.
 */

/*
 * Tabs. Every tab is a WebKit view sharing one Web process; only the tab
 * shown is mapped, so the others do not render. Here: creating views (with
 * WebKit's features and every signal connected), selecting and closing tabs,
 * the tab strip under the toolbar, a page's requests for new views, and New
 * Window (another heliotrope process).
 */

#include "heliotrope.h"
#include "downloads.h"
#include "draw.h"
#include "frames.h"
#include "menus.h"
#include "page.h"
#include "tabs.h"
#include "window.h"

static WebKitWebView *create_web_view(WebKitWebView *web_view, WebKitNavigationAction *action, Tab *tab);
static void close_requested(WebKitWebView *web_view, Tab *tab);

/* WebKit features each view gets: redraw only what changed (damage tracking,
 * off by default in WPE; a full frame costs seconds in software GL, a
 * keystroke's worth a fraction of one; HELIOTROPE_NO_DAMAGE=1 turns it off),
 * plus any named in HELIOTROPE_FEATURES=Name,Name for experiments. */
static void apply_features(App *app, WebKitWebView *web_view)
{
    WebKitSettings *settings = webkit_web_view_get_settings(web_view);
    WebKitFeatureList *all = webkit_settings_get_all_features();
    char *list = g_strjoin(",", env_on("HELIOTROPE_NO_DAMAGE") ? "" : "PropagateDamagingInformation,UseDamagingInformationForCompositing",
        getenv("HELIOTROPE_FEATURES") ? getenv("HELIOTROPE_FEATURES") : "", NULL);
    char **names = g_strsplit(list, ",", -1);
    gsize n = webkit_feature_list_get_length(all), k;
    int j;
    for (j = 0; names[j]; j++) {
        gboolean found = FALSE;
        if (!*names[j])
            continue;
        for (k = 0; k < n; k++) {
            WebKitFeature *feature = webkit_feature_list_get(all, k);
            if (!g_strcmp0(webkit_feature_get_identifier(feature), names[j])) {
                webkit_settings_set_feature_enabled(settings, feature, TRUE);
                found = TRUE;
            }
        }
        if ((!found || app->timing) && app->tabs->len == 0)
            g_printerr("heliotrope: feature %s %s\n", names[j], found ? "on" : "not found");
    }
    g_strfreev(names);
    g_free(list);
    webkit_feature_list_unref(all);
    /* HELIOTROPE_CONSOLE=1: the page's console messages go to stdout, for
     * testing. */
    if (env_on("HELIOTROPE_CONSOLE"))
        webkit_settings_set_enable_write_console_messages_to_stdout(settings, TRUE);
}

/* The page area moved (the tab strip came or went) or changed size. */
void layout_page(App *app)
{
    guint i;
    if (app->present)
        XMoveResizeWindow(app->dpy, app->page_win, 0, page_top(app), page_width(app), page_height(app));
    for (i = 0; i < app->tabs->len; i++) {
        Tab *t = g_ptr_array_index(app->tabs, i);
        wpe_toplevel_resize(wpe_view_get_toplevel(t->view), page_width(app), page_height(app));
    }
}

/* A tab for web_view (made by the caller, or by WebKit for a page that opens
 * a window), not shown yet. */
Tab *add_tab(App *app, WebKitWebView *web_view)
{
    Tab *tab = g_new0(Tab, 1);
    gboolean strip = app->tabs->len == 1;
    tab->app = app;
    tab->web_view = web_view;
    apply_features(app, web_view);
    tab->view = webkit_web_view_get_wpe_view(web_view);
    wpe_toplevel_resize(wpe_view_get_toplevel(tab->view), page_width(app), page_height(app));
    g_signal_connect(tab->view, "buffer-rendered", G_CALLBACK(buffer_rendered), tab);
    g_signal_connect(web_view, "load-changed", G_CALLBACK(load_changed), tab);
    g_signal_connect(web_view, "load-failed", G_CALLBACK(load_failed), tab);
    g_signal_connect(web_view, "notify::title", G_CALLBACK(title_changed), tab);
    g_signal_connect(web_view, "notify::estimated-load-progress", G_CALLBACK(state_changed), tab);
    g_signal_connect(web_view, "notify::is-loading", G_CALLBACK(state_changed), tab);
    g_signal_connect(web_view, "notify::uri", G_CALLBACK(state_changed), tab);
    g_signal_connect(web_view, "mouse-target-changed", G_CALLBACK(mouse_target_changed), tab);
    g_signal_connect(web_view, "web-process-terminated", G_CALLBACK(web_process_terminated), tab);
    g_signal_connect(web_view, "context-menu", G_CALLBACK(context_menu), tab);
    g_signal_connect(web_view, "decide-policy", G_CALLBACK(decide_policy), tab);
    g_signal_connect(web_view, "create", G_CALLBACK(create_web_view), tab);
    g_signal_connect(web_view, "close", G_CALLBACK(close_requested), tab);
    g_ptr_array_add(app->tabs, tab);
    if (app->tab)
        wpe_view_unmap(tab->view);
    if (strip)
        layout_page(app);
    return tab;
}

/* A new view sharing the first one's Web process and settings. */
static WebKitWebView *related_view(App *app, WebKitWebView *related)
{
    (void)app; /* the display, context and content manager are related's */
    return WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "related-view", related, NULL));
}

/* A tab shown again draws nothing by itself (its page has not changed):
 * a size one row short, then the right one again, gets a whole frame. */
static gboolean restore_size(gpointer data)
{
    App *app = data;
    app->repaint_timer = 0;
    wpe_toplevel_resize(wpe_view_get_toplevel(app->view), page_width(app), page_height(app));
    return G_SOURCE_REMOVE;
}

static void force_frame(App *app)
{
    int h = page_height(app);
    wpe_toplevel_resize(wpe_view_get_toplevel(app->view), page_width(app), h > 1 ? h - 1 : h + 1);
    if (app->repaint_timer)
        g_source_remove(app->repaint_timer);
    app->repaint_timer = g_timeout_add(40, restore_size, app);
}

void select_tab(App *app, Tab *tab)
{
    Tab *old = app->tab;
    if (old == tab)
        return;
    if (old) {
        wpe_view_focus_out(old->view);
        wpe_view_unmap(old->view);
    }
    app->tab = tab;
    app->web_view = tab->web_view;
    app->view = tab->view;
    wpe_view_map(tab->view);
    wpe_view_focus_in(tab->view);
    if (old)
        force_frame(app);
    else
        wpe_toplevel_resize(wpe_view_get_toplevel(tab->view), page_width(app), page_height(app));
    app->progress_value = webkit_web_view_get_estimated_load_progress(tab->web_view);
    g_clear_pointer(&app->status, g_free);
    if (app->editing && app->prompt == PROMPT_ADDRESS) {
        app->editing = FALSE;
        app->all_selected = FALSE;
    }
    show_title(app);
    redraw(app);
    draw_tabs(app);
}

/* A new tab loading uri (an address or a search), shown or not. */
Tab *open_tab(App *app, const char *uri, gboolean show)
{
    Tab *tab = add_tab(app, related_view(app, app->web_view));
    if (show)
        select_tab(app, tab);
    if (uri && *uri) {
        WebKitWebView *shown = app->web_view;
        app->web_view = tab->web_view;
        load_address(app, uri);
        app->web_view = shown;
    }
    draw_tabs(app);
    XFlush(app->dpy);
    return tab;
}

void close_tab(App *app, Tab *tab)
{
    guint i;
    if (app->tabs->len <= 1) {
        g_main_loop_quit(app->loop);
        return;
    }
    for (i = 0; i < app->tabs->len && g_ptr_array_index(app->tabs, i) != tab; i++)
        ;
    if (i == app->tabs->len)
        return;
    if (tab == app->tab)
        select_tab(app, g_ptr_array_index(app->tabs, i + 1 < app->tabs->len ? i + 1 : i - 1));
    g_ptr_array_remove_index(app->tabs, i);
    g_signal_handlers_disconnect_by_data(tab->view, tab);
    g_signal_handlers_disconnect_by_data(tab->web_view, tab);
    g_object_unref(tab->web_view);
    g_free(tab->title);
    g_free(tab);
    if (app->tabs->len == 1)
        layout_page(app);
    redraw(app);
    draw_tabs(app);
}

void step_tab(App *app, int d)
{
    guint i, n = app->tabs->len;
    for (i = 0; i < n && g_ptr_array_index(app->tabs, i) != app->tab; i++)
        ;
    if (n > 1)
        select_tab(app, g_ptr_array_index(app->tabs, (i + n + d) % n));
}

static void tab_rect(App *app, guint i, XRectangle *r)
{
    int n = (int)app->tabs->len, w = (app->width - 8) / (n ? n : 1);
    if (w > 220)
        w = 220;
    r->x = 4 + (int)i * w;
    r->y = MENU_HEIGHT + BAR_HEIGHT + 2;
    r->width = w - 2;
    r->height = TAB_HEIGHT - 3;
}

static void close_box(const XRectangle *r, XRectangle *box)
{
    box->width = box->height = 11;
    box->x = r->x + r->width - 16;
    box->y = r->y + (r->height - 11) / 2;
}

void draw_tabs(App *app)
{
    int top = MENU_HEIGHT + BAR_HEIGHT;
    guint i;
    if (!app->tabs || app->tabs->len < 2)
        return;
    XSetForeground(app->dpy, app->gc, app->bar_bg);
    XFillRectangle(app->dpy, app->win, app->gc, 0, top, app->width, TAB_HEIGHT);
    XSetForeground(app->dpy, app->gc, app->bar_shadow);
    XDrawLine(app->dpy, app->win, app->gc, 0, top + TAB_HEIGHT - 1, app->width, top + TAB_HEIGHT - 1);
    for (i = 0; i < app->tabs->len; i++) {
        Tab *t = g_ptr_array_index(app->tabs, i);
        XRectangle r, box;
        const char *text = t->title ? t->title : webkit_web_view_get_uri(t->web_view);
        int len, avail;
        tab_rect(app, i, &r);
        XSetForeground(app->dpy, app->gc, t == app->tab ? app->field_edit_bg : app->field_bg);
        XFillRectangle(app->dpy, app->win, app->gc, r.x, r.y, r.width, r.height);
        bevel(app, &r, t != app->tab);
        if (webkit_web_view_is_loading(t->web_view)) {
            XSetForeground(app->dpy, app->gc, app->progress);
            XFillRectangle(app->dpy, app->win, app->gc, r.x + 1, r.y + r.height - 3,
                (unsigned)((r.width - 2) * webkit_web_view_get_estimated_load_progress(t->web_view)), 2);
        }
        if (!text)
            text = "New tab";
        len = (int)strlen(text);
        avail = r.width - 28;
        while (len > 0 && app->font && XTextWidth(app->font, text, len) > avail)
            len--;
        XSetForeground(app->dpy, app->gc, app->ink);
        if (app->font)
            XDrawString(app->dpy, app->win, app->gc, r.x + 6, r.y + (r.height + app->font->ascent - app->font->descent) / 2,
                text, len);
        close_box(&r, &box);
        XSetForeground(app->dpy, app->gc, app->ink_disabled);
        XDrawLine(app->dpy, app->win, app->gc, box.x + 2, box.y + 2, box.x + box.width - 3, box.y + box.height - 3);
        XDrawLine(app->dpy, app->win, app->gc, box.x + 2, box.y + box.height - 3, box.x + box.width - 3, box.y + 2);
    }
}

void handle_tab_strip(App *app, XButtonEvent *b)
{
    guint i;
    if (b->type != ButtonPress)
        return;
    for (i = 0; i < app->tabs->len; i++) {
        XRectangle r, box;
        tab_rect(app, i, &r);
        if (!in_rect(&r, b->x, b->y))
            continue;
        close_box(&r, &box);
        if (b->button == Button2 || (b->button == Button1 && in_rect(&box, b->x, b->y)))
            close_tab(app, g_ptr_array_index(app->tabs, i));
        else if (b->button == Button1)
            select_tab(app, g_ptr_array_index(app->tabs, i));
        return;
    }
}

/* A page opens a window (target=_blank, window.open): a tab, shown when ready. */
static void ready_to_show(WebKitWebView *web_view, Tab *tab)
{
    (void)web_view;
    select_tab(tab->app, tab);
}

static WebKitWebView *create_web_view(WebKitWebView *web_view, WebKitNavigationAction *action, Tab *tab)
{
    Tab *t = add_tab(tab->app, related_view(tab->app, web_view));
    (void)action;
    g_signal_connect(t->web_view, "ready-to-show", G_CALLBACK(ready_to_show), t);
    draw_tabs(tab->app);
    return t->web_view;
}

static void close_requested(WebKitWebView *web_view, Tab *tab)
{
    (void)web_view;
    close_tab(tab->app, tab);
}

/* Another browser window: a process of its own (its own Web and network
 * processes as well: only when asked for). */
void spawn_window(App *app, const char *uri)
{
    char *argv[3];
    GError *error = NULL;
    argv[0] = (char *)app->argv0;
    argv[1] = (char *)(uri && *uri ? uri : app->home);
    argv[2] = NULL;
    if (!g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &error)) {
        show_message(app, error->message, 5);
        g_error_free(error);
    }
}
