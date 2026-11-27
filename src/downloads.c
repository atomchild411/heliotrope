/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 atomchild411
 *
 * The licence's full text is in LICENSE.
 */

/*
 * Downloads. Files the browser cannot show are saved: a destination is asked
 * for in the address field ("Save as:", in ~/Downloads, never overwriting),
 * progress and the result show in the status line, and the policy decision
 * here turns such responses into downloads.
 */

#include "heliotrope.h"
#include "downloads.h"
#include "field.h"
#include "page.h"
#include "tabs.h"

static char *downloads_dir(void)
{
    const char *d = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
    char *dir;
    if (d && g_file_test(d, G_FILE_TEST_IS_DIR))
        return g_strdup(d);
    dir = g_build_filename(g_get_home_dir(), "Downloads", NULL);
    if (g_file_test(dir, G_FILE_TEST_IS_DIR))
        return dir;
    g_free(dir);
    return g_strdup(g_get_home_dir());
}

/* dir/name, or dir/name-2.ext etc. if that is taken */
static char *free_path(const char *dir, const char *name)
{
    char *path = g_build_filename(dir, name && *name ? name : "download", NULL);
    const char *dot = strrchr(name && *name ? name : "download", '.');
    int i;
    for (i = 2; g_file_test(path, G_FILE_TEST_EXISTS) && i < 1000; i++) {
        char *base = g_strndup(name, dot && dot != name ? (gsize)(dot - name) : strlen(name));
        char *n = g_strdup_printf("%s-%d%s", base, i, dot && dot != name ? dot : "");
        g_free(path);
        path = g_build_filename(dir, n, NULL);
        g_free(n);
        g_free(base);
    }
    return path;
}

static void ask_destination(App *app, WebKitDownload *download)
{
    const char *name = g_object_get_data(G_OBJECT(download), "heliotrope-name");
    char *dir = downloads_dir(), *path = free_path(dir, name);
    app->prompt_download = download;
    start_prompt(app, PROMPT_SAVE, path);
    g_free(path);
    g_free(dir);
}

static gboolean decide_destination(WebKitDownload *download, const char *suggested, App *app)
{
    g_printerr("heliotrope: download %s: asking where (suggested %s)\n",
        webkit_uri_request_get_uri(webkit_download_get_request(download)), suggested ? suggested : "none");
    char *name = NULL;
    if (suggested && *suggested)
        name = g_path_get_basename(suggested);
    else {
        /* none suggested (a page): the address's last part, else its host, as .html */
        const char *uri = webkit_uri_request_get_uri(webkit_download_get_request(download));
        GUri *u = uri ? g_uri_parse(uri, G_URI_FLAGS_NONE, NULL) : NULL;
        const char *path = u ? g_uri_get_path(u) : NULL;
        char *base = path && *path && strcmp(path, "/") ? g_path_get_basename(path) : NULL;
        if (base && *base && strcmp(base, "/") && strcmp(base, "."))
            name = strchr(base, '.') ? g_strdup(base) : g_strconcat(base, ".html", NULL);
        else if (u && g_uri_get_host(u))
            name = g_strconcat(g_uri_get_host(u), ".html", NULL);
        g_free(base);
        if (u)
            g_uri_unref(u);
        if (!name)
            name = g_strdup("download");
    }
    g_object_set_data_full(G_OBJECT(download), "heliotrope-name", name, g_free);
    g_object_ref(download);
    if (app->prompt_download)
        g_ptr_array_add(app->waiting_downloads, download);
    else
        ask_destination(app, download);
    return TRUE; /* the destination is set when the prompt is answered */
}

static void download_progress(WebKitDownload *download, guint64 length, App *app)
{
    const char *dest = webkit_download_get_destination(download);
    char *name = dest ? g_path_get_basename(dest) : g_strdup("download");
    double p = webkit_download_get_estimated_progress(download);
    gint64 now = g_get_monotonic_time();
    gint64 *last = g_object_get_data(G_OBJECT(download), "heliotrope-shown");
    char *text;
    (void)length;
    if (last && now - *last < G_USEC_PER_SEC / 2)
        return;
    if (!last) {
        last = g_new(gint64, 1);
        g_object_set_data_full(G_OBJECT(download), "heliotrope-shown", last, g_free);
    }
    *last = now;
    text = p > 0 ? g_strdup_printf("Saving %s: %d%%", name, (int)(p * 100))
        : g_strdup_printf("Saving %s: %lu KB", name, (unsigned long)(webkit_download_get_received_data_length(download) / 1024));
    show_message(app, text, 0);
    g_free(text);
    g_free(name);
}

static void download_finished(WebKitDownload *download, App *app)
{
    const char *dest = webkit_download_get_destination(download);
    g_printerr("heliotrope: download to %s finished\n", dest ? dest : "?");
    if (!g_object_get_data(G_OBJECT(download), "heliotrope-failed") && dest) {
        char *text = g_strdup_printf("Saved %s", dest);
        show_message(app, text, 8);
        g_free(text);
    }
}

static void download_failed(WebKitDownload *download, GError *error, App *app)
{
    g_object_set_data(G_OBJECT(download), "heliotrope-failed", GINT_TO_POINTER(1));
    g_printerr("heliotrope: download failed: %s\n", error->message);
    if (!g_error_matches(error, WEBKIT_DOWNLOAD_ERROR, WEBKIT_DOWNLOAD_ERROR_CANCELLED_BY_USER)) {
        char *text = g_strdup_printf("Not saved: %s", error->message);
        show_message(app, text, 8);
        g_free(text);
    } else
        show_message(app, "Not saved", 3);
}

/* every download of the session: links saved, pages saved, files the
 * browser cannot show */
void download_started(WebKitNetworkSession *session, WebKitDownload *download, App *app)
{
    (void)session;
    g_printerr("heliotrope: download %s started\n", webkit_uri_request_get_uri(webkit_download_get_request(download)));
    g_signal_connect(download, "decide-destination", G_CALLBACK(decide_destination), app);
    g_signal_connect(download, "received-data", G_CALLBACK(download_progress), app);
    g_signal_connect(download, "finished", G_CALLBACK(download_finished), app);
    g_signal_connect(download, "failed", G_CALLBACK(download_failed), app);
}

/* The prompt for a download answered: path, or NULL for no. */
void destination_chosen(App *app, const char *path)
{
    WebKitDownload *download = app->prompt_download;
    app->prompt_download = NULL;
    if (download) {
        if (path && *path) {
            char *full = g_str_has_prefix(path, "~/") ? g_build_filename(g_get_home_dir(), path + 2, NULL) : g_strdup(path);
            webkit_download_set_allow_overwrite(download, TRUE);
            webkit_download_set_destination(download, full);
            g_free(full);
        } else {
            /* the destination callback must be answered before the download goes */
            webkit_download_set_destination(download, "");
            webkit_download_cancel(download);
        }
        g_object_unref(download);
    }
    if (app->waiting_downloads->len) {
        WebKitDownload *next = g_ptr_array_steal_index(app->waiting_downloads, 0);
        ask_destination(app, next);
    }
}

/* Files WebKit cannot show are saved; a middle click on a link opens it in a new tab. */
gboolean decide_policy(WebKitWebView *web_view, WebKitPolicyDecision *decision, WebKitPolicyDecisionType type, Tab *tab)
{
    (void)web_view;
    if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE) {
        WebKitResponsePolicyDecision *r = WEBKIT_RESPONSE_POLICY_DECISION(decision);
        if (!webkit_response_policy_decision_is_mime_type_supported(r)
            && webkit_response_policy_decision_is_main_frame_main_resource(r)) {
            webkit_policy_decision_download(decision);
            return TRUE;
        }
    } else if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
        WebKitNavigationAction *a = webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(decision));
        if (webkit_navigation_action_get_navigation_type(a) == WEBKIT_NAVIGATION_TYPE_LINK_CLICKED
            && webkit_navigation_action_get_mouse_button(a) == 2) {
            const char *uri = webkit_uri_request_get_uri(webkit_navigation_action_get_request(a));
            open_tab(tab->app, uri, FALSE);
            show_message(tab->app, "Opened in a new tab", 3);
            webkit_policy_decision_ignore(decision);
            return TRUE;
        }
    }
    return FALSE;
}
