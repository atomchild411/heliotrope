/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 atomchild411
 *
 * The licence's full text is in LICENSE.
 */

/*
 * The X clipboard for the page. What WebKit copies is offered on CLIPBOARD
 * and PRIMARY; a paste into the page asks the selection's owner for UTF-8
 * text first and hands it to WebKit when it arrives. (Pasting into the
 * address field is in field.c.)
 */

#include "heliotrope.h"
#include "clipboard.h"

/* Hold text as CLIPBOARD and PRIMARY, for other X programs. */
void own_selection(App *app, const char *text)
{
    g_free(app->clip_text);
    app->clip_text = g_strdup(text);
    XSetSelectionOwner(app->dpy, app->clipboard, app->win, CurrentTime);
    XSetSelectionOwner(app->dpy, XA_PRIMARY, app->win, CurrentTime);
}

void selection_request(App *app, XSelectionRequestEvent *r)
{
    XEvent reply;
    memset(&reply, 0, sizeof reply);
    reply.xselection.type = SelectionNotify;
    reply.xselection.display = r->display;
    reply.xselection.requestor = r->requestor;
    reply.xselection.selection = r->selection;
    reply.xselection.target = r->target;
    reply.xselection.time = r->time;
    reply.xselection.property = None;
    if (app->clip_text) {
        Atom property = r->property != None ? r->property : r->target;
        if (r->target == app->targets) {
            Atom list[3] = { app->targets, app->utf8, XA_STRING };
            XChangeProperty(app->dpy, r->requestor, property, XA_ATOM, 32, PropModeReplace, (unsigned char *)list, 3);
            reply.xselection.property = property;
        } else if (r->target == app->utf8) {
            XChangeProperty(app->dpy, r->requestor, property, app->utf8, 8, PropModeReplace,
                (unsigned char *)app->clip_text, (int)strlen(app->clip_text));
            reply.xselection.property = property;
        } else if (r->target == XA_STRING) {
            char *latin1 = g_convert_with_fallback(app->clip_text, -1, "ISO-8859-1", "UTF-8", "?", NULL, NULL, NULL);
            if (latin1) {
                XChangeProperty(app->dpy, r->requestor, property, XA_STRING, 8, PropModeReplace,
                    (unsigned char *)latin1, (int)strlen(latin1));
                reply.xselection.property = property;
                g_free(latin1);
            }
        }
    }
    XSendEvent(app->dpy, r->requestor, False, 0, &reply);
}

/* After the page copied (Ctrl+C, the Copy menu): what WebKit put on its
 * clipboard, offered to X. */
gboolean export_clipboard(gpointer data)
{
    App *app = data;
    WPEClipboard *clipboard = wpe_display_get_clipboard(app->display);
    gint64 count = clipboard ? wpe_clipboard_get_change_count(clipboard) : 0;
    if (clipboard && count != app->wpe_clip_count) {
        gsize size = 0;
        char *text = wpe_clipboard_read_text(clipboard, "text/plain", &size);
        app->wpe_clip_count = count;
        if (text && *text)
            own_selection(app, text);
        g_free(text);
    }
    return G_SOURCE_REMOVE;
}

/* A paste into the page: another client's CLIPBOARD put on WebKit's
 * clipboard first (ours is there already). */
void paste_into_page(App *app)
{
    if (XGetSelectionOwner(app->dpy, app->clipboard) == app->win) {
        webkit_web_view_execute_editing_command(app->web_view, WEBKIT_EDITING_COMMAND_PASTE);
        return;
    }
    app->paste_pending = TRUE;
    XConvertSelection(app->dpy, app->clipboard, app->utf8, app->paste_property, app->win, CurrentTime);
}

void page_paste_arrived(App *app, XSelectionEvent *e)
{
    Atom type;
    int format;
    unsigned long n, after;
    unsigned char *data = NULL;
    app->paste_pending = FALSE;
    if (e->property != None && XGetWindowProperty(app->dpy, app->win, e->property, 0, 1 << 20, True, AnyPropertyType,
            &type, &format, &n, &after, &data) == Success && data && format == 8) {
        WPEClipboard *clipboard = wpe_display_get_clipboard(app->display);
        WPEClipboardContent *content = wpe_clipboard_content_new();
        char *text = type == XA_STRING ? g_convert((char *)data, (gssize)n, "UTF-8", "ISO-8859-1", NULL, NULL, NULL)
            : g_strndup((char *)data, n);
        wpe_clipboard_content_set_text(content, text ? text : "");
        wpe_clipboard_set_content(clipboard, content);
        app->wpe_clip_count = wpe_clipboard_get_change_count(clipboard);
        wpe_clipboard_content_unref(content);
        g_free(text);
    } else if (e->property == None && e->target == app->utf8) {
        /* no UTF8_STRING: ask for STRING */
        app->paste_pending = TRUE;
        XConvertSelection(app->dpy, app->clipboard, XA_STRING, app->paste_property, app->win, CurrentTime);
        return;
    }
    if (data)
        XFree(data);
    webkit_web_view_execute_editing_command(app->web_view, WEBKIT_EDITING_COMMAND_PASTE);
}
