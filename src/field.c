/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 atomchild411
 *
 * The licence's full text is in LICENSE.
 */

/*
 * The address field: editing (cursor, selection, typing, pasting from the X
 * selections) and its prompts. The same field asks for an address, a file
 * name to save to ("Save as:") or text to find ("Find:"), and Enter hands the
 * text to whoever asked.
 */

#include "heliotrope.h"
#include "downloads.h"
#include "draw.h"
#include "field.h"
#include "page.h"
#include "window.h"

/* The field asks for something: an address (as typed or the page's), a
 * file name for a download, or text to find in the page. */
void start_prompt(App *app, int prompt, const char *text)
{
    app->editing = TRUE;
    app->prompt = prompt;
    g_string_assign(app->edit, text ? text : "");
    app->cursor = app->edit->len;
    app->all_selected = FALSE;
    draw_bar(app);
}

void start_editing(App *app)
{
    start_prompt(app, PROMPT_ADDRESS, webkit_web_view_get_uri(app->web_view));
    app->all_selected = TRUE;
    draw_bar(app);
}

void stop_editing(App *app)
{
    int prompt = app->prompt;
    app->editing = FALSE;
    app->all_selected = FALSE;
    app->prompt = PROMPT_ADDRESS;
    /* a download's prompt left unanswered: not saved */
    if (prompt == PROMPT_SAVE && app->prompt_download)
        destination_chosen(app, NULL);
    draw_bar(app);
}

static void insert_text(App *app, const char *text, gsize len)
{
    gsize i;
    if (app->all_selected) {
        g_string_truncate(app->edit, 0);
        app->cursor = 0;
        app->all_selected = FALSE;
    }
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c < 32 || c == 127)
            continue; /* no newlines or controls from a paste */
        g_string_insert_c(app->edit, (gssize)app->cursor, (char)c);
        app->cursor++;
    }
}

void field_paste(App *app, Atom selection)
{
    XConvertSelection(app->dpy, selection, XA_STRING, app->selection_property, app->win, CurrentTime);
}

void selection_arrived(App *app, XSelectionEvent *e)
{
    Atom type;
    int format;
    unsigned long n, after;
    unsigned char *data = NULL;
    if (e->property == None || !app->editing)
        return;
    if (XGetWindowProperty(app->dpy, app->win, e->property, 0, 4096, True, AnyPropertyType,
            &type, &format, &n, &after, &data) == Success && data && format == 8) {
        insert_text(app, (const char *)data, n);
        draw_bar(app);
    }
    if (data)
        XFree(data);
}

/* the byte offset in the field text nearest to window x */
gsize offset_at(App *app, int x)
{
    XRectangle r;
    gsize i;
    int left;
    field_rect(app, &r);
    if (!app->font)
        return app->edit->len;
    left = r.x + 6;
    if (app->prompt != PROMPT_ADDRESS) {
        const char *label = app->prompt == PROMPT_SAVE ? "Save as:" : "Find:";
        left += XTextWidth(app->font, label, (int)strlen(label)) + 8;
    }
    for (i = 0; i < app->edit->len; i++) {
        int w = XTextWidth(app->font, app->edit->str, (int)i);
        int cw = XTextWidth(app->font, app->edit->str + i, 1);
        if (left + w + cw / 2 > x)
            return i;
    }
    return app->edit->len;
}

gboolean handle_field_key(App *app, KeySym sym, const char *text, unsigned int state)
{
    gboolean control = (state & ControlMask) != 0;
    switch (sym) {
    case XK_Return:
    case XK_KP_Enter: {
        char *text = g_strdup(app->edit->str);
        int prompt = app->prompt;
        app->editing = FALSE;
        app->all_selected = FALSE;
        app->prompt = PROMPT_ADDRESS;
        draw_bar(app);
        if (prompt == PROMPT_SAVE)
            destination_chosen(app, text);
        else if (prompt == PROMPT_FIND) {
            if (*text)
                find_text(app, text);
        } else
            load_address(app, text);
        g_free(text);
        return TRUE;
    }
    case XK_Escape:
        stop_editing(app);
        return TRUE;
    case XK_Left:
        if (app->all_selected)
            app->cursor = 0;
        else if (app->cursor)
            app->cursor--;
        app->all_selected = FALSE;
        break;
    case XK_Right:
        if (!app->all_selected && app->cursor < app->edit->len)
            app->cursor++;
        else if (app->all_selected)
            app->cursor = app->edit->len;
        app->all_selected = FALSE;
        break;
    case XK_Home:
        app->cursor = 0;
        app->all_selected = FALSE;
        break;
    case XK_End:
        app->cursor = app->edit->len;
        app->all_selected = FALSE;
        break;
    case XK_BackSpace:
        if (app->all_selected) {
            g_string_truncate(app->edit, 0);
            app->cursor = 0;
            app->all_selected = FALSE;
        } else if (app->cursor) {
            g_string_erase(app->edit, (gssize)app->cursor - 1, 1);
            app->cursor--;
        }
        break;
    case XK_Delete:
        if (app->all_selected) {
            g_string_truncate(app->edit, 0);
            app->cursor = 0;
            app->all_selected = FALSE;
        } else if (app->cursor < app->edit->len)
            g_string_erase(app->edit, (gssize)app->cursor, 1);
        break;
    default:
        if (control) {
            if (sym == XK_a || sym == XK_A)
                app->all_selected = TRUE;
            else if (sym == XK_u || sym == XK_U) {
                g_string_truncate(app->edit, 0);
                app->cursor = 0;
                app->all_selected = FALSE;
            } else if (sym == XK_k || sym == XK_K)
                g_string_truncate(app->edit, app->cursor);
            else if (sym == XK_v || sym == XK_V)
                field_paste(app, app->clipboard);
            else if (sym == XK_l || sym == XK_L)
                app->all_selected = TRUE;
        } else if (text[0])
            insert_text(app, text, strlen(text));
        break;
    }
    draw_bar(app);
    return TRUE;
}
