/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 atomchild411
 *
 * The licence's full text is in LICENSE.
 */

/*
 * Per-site choices, remembered in ~/.config/heliotrope/sites, one host per
 * line with its flags: for now, JavaScript off for the site (the JS button).
 */

#include "heliotrope.h"
#include "sites.h"

/* the SITE_* flags of an address's site */
static unsigned site_flags_of(App *app, const char *uri)
{
    GUri *parsed = uri ? g_uri_parse(uri, G_URI_FLAGS_NONE, NULL) : NULL;
    unsigned flags = 0;
    if (parsed) {
        const char *host = g_uri_get_host(parsed);
        if (host)
            flags = GPOINTER_TO_UINT(g_hash_table_lookup(app->sites, host));
        g_uri_unref(parsed);
    }
    return flags;
}

/* the SITE_* flags of the page being shown */
unsigned current_site_flags(App *app)
{
    return site_flags_of(app, webkit_web_view_get_uri(app->web_view));
}

void load_sites(App *app)
{
    char *text = NULL, **lines;
    int i;
    app->sites = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    app->sites_path = g_build_filename(g_get_user_config_dir(), "heliotrope", "sites", NULL);
    if (!g_file_get_contents(app->sites_path, &text, NULL, NULL))
        return;
    lines = g_strsplit(text, "\n", -1);
    for (i = 0; lines[i]; i++) {
        char **w = g_strsplit_set(g_strstrip(lines[i]), " \t", -1);
        unsigned flags = 0;
        int j;
        if (!w[0] || !*w[0] || w[0][0] == '#') {
            g_strfreev(w);
            continue;
        }
        for (j = 1; w[j]; j++) {
            if (!strcmp(w[j], "nojs"))
                flags |= SITE_NO_JS;
        }
        if (flags)
            g_hash_table_insert(app->sites, g_strdup(w[0]), GUINT_TO_POINTER(flags));
        g_strfreev(w);
    }
    g_strfreev(lines);
    g_free(text);
}

static void save_sites(App *app)
{
    GString *out = g_string_new("# heliotrope per-site choices: host nojs\n");
    GHashTableIter it;
    gpointer key, value;
    char *dir = g_path_get_dirname(app->sites_path);
    g_hash_table_iter_init(&it, app->sites);
    while (g_hash_table_iter_next(&it, &key, &value)) {
        unsigned flags = GPOINTER_TO_UINT(value);
        g_string_append_printf(out, "%s%s\n", (char *)key, flags & SITE_NO_JS ? " nojs" : "");
    }
    g_mkdir_with_parents(dir, 0755);
    g_file_set_contents(app->sites_path, out->str, (gssize)out->len, NULL);
    g_free(dir);
    g_string_free(out, TRUE);
}

/* Apply a page's site's choices before its document and resources load. */
void apply_site_choices(App *app, WebKitWebView *web_view)
{
    unsigned flags = site_flags_of(app, webkit_web_view_get_uri(web_view));
    webkit_settings_set_enable_javascript(webkit_web_view_get_settings(web_view), !(flags & SITE_NO_JS));
}

void toggle_site(App *app, unsigned flag)
{
    const char *uri = webkit_web_view_get_uri(app->web_view);
    GUri *parsed = uri ? g_uri_parse(uri, G_URI_FLAGS_NONE, NULL) : NULL;
    const char *host = parsed ? g_uri_get_host(parsed) : NULL;
    if (host && *host) {
        unsigned flags = GPOINTER_TO_UINT(g_hash_table_lookup(app->sites, host)) ^ flag;
        if (flags)
            g_hash_table_insert(app->sites, g_strdup(host), GUINT_TO_POINTER(flags));
        else
            g_hash_table_remove(app->sites, host);
        save_sites(app);
        apply_site_choices(app, app->web_view);
        webkit_web_view_reload(app->web_view);
    }
    if (parsed)
        g_uri_unref(parsed);
}
