/* -*- indent-tabs-mode: nil; tab-width: 4; c-basic-offset: 4; -*-

   prompt.c for the Openbox window manager
   Copyright (c) 2008        Dana Jansens

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   See the COPYING file for a copy of the GNU General Public License.
*/

#include "prompt.h"
#include "openbox.h"
#include "screen.h"
#include "client.h"
#include "group.h"
#include "event.h"
#include "paint.h"
#include "gettext.h"
#include "backend.h"
#include <xkbcommon/xkbcommon-keysyms.h>

static GList *prompt_list = NULL;

/* we construct these */
static RrAppearance *prompt_a_bg;
static RrAppearance *prompt_a_button;
static RrAppearance *prompt_a_focus;
static RrAppearance *prompt_a_press;
/* we change the max width which would screw with others */
static RrAppearance *prompt_a_msg;

/* sizing stuff */
#define OUTSIDE_MARGIN 4
#define MSG_BUTTON_SEPARATION 4
#define BUTTON_SEPARATION 4
#define BUTTON_VMARGIN 4
#define BUTTON_HMARGIN 12
#define MAX_WIDTH 400

static void prompt_layout(ObPrompt *self);
static void render_all(ObPrompt *self);
static void render_button(ObPrompt *self, ObPromptElement *e);
static void prompt_resize(ObPrompt *self, gint w, gint h);
static void prompt_run_callback(ObPrompt *self, gint result);

void prompt_startup(gboolean reconfig)
{
    /* note: this is not a copy, don't free it */
    prompt_a_bg = ob_rr_theme->osd_bg;

    prompt_a_button = RrAppearanceCopy(ob_rr_theme->osd_unpressed_button);
    prompt_a_focus = RrAppearanceCopy(ob_rr_theme->osd_focused_button);
    prompt_a_press = RrAppearanceCopy(ob_rr_theme->osd_pressed_button);    

    prompt_a_msg = RrAppearanceCopy(ob_rr_theme->osd_hilite_label);
    prompt_a_msg->texture[0].data.text.flow = TRUE;

    if (reconfig) {
        GList *it;
        for (it = prompt_list; it; it = g_list_next(it)) {
            ObPrompt *p = it->data;
            prompt_layout(p);
            render_all(p);
        }
    }
}

void prompt_shutdown(gboolean reconfig)
{
    GList *it, *next;

    if (!reconfig) {
        for (it = prompt_list; it; it = next) {
            ObPrompt *p = it->data;
            next = it->next;
            if (p->cleanup) p->cleanup(p, p->data);
        }

        g_assert(prompt_list == NULL);
    }

    RrAppearanceFree(prompt_a_button);
    RrAppearanceFree(prompt_a_focus);
    RrAppearanceFree(prompt_a_press);
    RrAppearanceFree(prompt_a_msg);
}

ObPrompt* prompt_new(const gchar *msg, const gchar *title,
                     const ObPromptAnswer *answers, gint n_answers,
                     gint default_result, gint cancel_result,
                     ObPromptCallback func, ObPromptCleanup cleanup,
                     gpointer data)
{
    ObPrompt *self;
    gint i;

    self = g_slice_new0(ObPrompt);
    self->ref = 1;
    self->func = func;
    self->cleanup = cleanup;
    self->data = data;
    self->default_result = default_result;
    self->cancel_result = cancel_result;
    self->super.type = OB_WINDOW_CLASS_PROMPT;
    self->super.window = (ObWindowId)ob_backend->canvas_new(OB_CANVAS_DIALOG,
                                                       OB_SURFACE_NONE, 0);

    /* set the window's title */
    if (title)
        ob_backend->dialog_set_title(self->super.window, title);

    /* set up the text message widow */
    self->msg.text = g_strdup(msg);
    self->msg.window = (ObWindowId)ob_backend->canvas_new_child(
        self->super.window, 0);
    ob_backend->canvas_show(self->msg.window, TRUE);

    /* set up the buttons from the answers */

    self->n_buttons = n_answers;
    if (!self->n_buttons)
        self->n_buttons = 1;

    self->button = g_new0(ObPromptElement, self->n_buttons);

    if (n_answers == 0) {
        g_assert(self->n_buttons == 1); /* should be set to this above.. */
        self->button[0].text = g_strdup(_("OK"));
    }
    else {
        g_assert(self->n_buttons > 0);
        for (i = 0; i < self->n_buttons; ++i) {
            self->button[i].text = g_strdup(answers[i].text);
            self->button[i].result = answers[i].result;
        }
    }

    for (i = 0; i < self->n_buttons; ++i) {
        /* listen for button presses on the buttons */
        self->button[i].window = (ObWindowId)ob_backend->canvas_new_child(
            self->super.window,
            OB_CANVAS_INPUT_BUTTONS | OB_CANVAS_INPUT_DRAG);
        ob_backend->canvas_show(self->button[i].window, TRUE);
        window_add(&self->button[i].window, PROMPT_AS_WINDOW(self));
    }

    prompt_list = g_list_prepend(prompt_list, self);

    return self;
}

void prompt_ref(ObPrompt *self)
{
    ++self->ref;
}

void prompt_unref(ObPrompt *self)
{
    if (self && --self->ref == 0) {
        gint i;

        if (self->mapped)
            prompt_hide(self);

        prompt_list = g_list_remove(prompt_list, self);

        for (i = 0; i < self->n_buttons; ++i) {
            window_remove(self->button[i].window);
            ob_backend->canvas_free(self->button[i].window);
        }

        ob_backend->canvas_free(self->msg.window);
        ob_backend->canvas_free(self->super.window);
        g_slice_free(ObPrompt, self);
    }
}

static void prompt_layout(ObPrompt *self)
{
    gint l, r, t, b;
    gint i;
    gint allbuttonsw, allbuttonsh, buttonx;
    gint w, h;
    gint maxw;

    RrMargins(prompt_a_bg, &l, &t, &r, &b);
    l += OUTSIDE_MARGIN;
    t += OUTSIDE_MARGIN;
    r += OUTSIDE_MARGIN;
    b += OUTSIDE_MARGIN;

    {
        const Rect *area = screen_physical_area_all_monitors();
        maxw = MIN(MAX_WIDTH, area->width*4/5);
    }

    /* find the button sizes and how much space we need for them */
    allbuttonsw = allbuttonsh = 0;
    for (i = 0; i < self->n_buttons; ++i) {
        gint bw, bh;

        prompt_a_button->texture[0].data.text.string = self->button[i].text;
        prompt_a_focus->texture[0].data.text.string = self->button[i].text;
        prompt_a_press->texture[0].data.text.string = self->button[i].text;
        RrMinSize(prompt_a_button, &bw, &bh);
        self->button[i].width = bw;
        self->button[i].height = bh;
        RrMinSize(prompt_a_focus, &bw, &bh);
        self->button[i].width = MAX(self->button[i].width, bw);
        self->button[i].height = MAX(self->button[i].height, bh);
        RrMinSize(prompt_a_press, &bw, &bh);
        self->button[i].width = MAX(self->button[i].width, bw);
        self->button[i].height = MAX(self->button[i].height, bh);

        self->button[i].width += BUTTON_HMARGIN * 2;
        self->button[i].height += BUTTON_VMARGIN * 2;

        allbuttonsw += self->button[i].width + (i > 0 ? BUTTON_SEPARATION : 0);
        allbuttonsh = MAX(allbuttonsh, self->button[i].height);
    }

    self->msg_wbound = MAX(allbuttonsw, maxw);

    /* measure the text message area */
    prompt_a_msg->texture[0].data.text.string = self->msg.text;
    prompt_a_msg->texture[0].data.text.maxwidth = self->msg_wbound;
    RrMinSize(prompt_a_msg, &self->msg.width, &self->msg.height);

    /* width and height inside the outer margins */
    w = MAX(self->msg.width, allbuttonsw);
    h = self->msg.height + MSG_BUTTON_SEPARATION + allbuttonsh;

    /* position the text message */
    self->msg.x = l + (w - self->msg.width) / 2;
    self->msg.y = t;

    /* position the button buttons on the right of the dialog */
    buttonx = l + w;
    for (i = self->n_buttons - 1; i >= 0; --i) {
        self->button[i].x = buttonx - self->button[i].width;
        buttonx -= self->button[i].width + BUTTON_SEPARATION;
        self->button[i].y = t + h - allbuttonsh;
        self->button[i].y += (allbuttonsh - self->button[i].height) / 2;
    }

    /* size and position the toplevel window */
    prompt_resize(self, w + l + r, h + t + b);

    /* move and resize the internal windows */
    paint_place(self->msg.window,
                self->msg.x, self->msg.y,
                self->msg.width, self->msg.height);
    for (i = 0; i < self->n_buttons; ++i)
        paint_place(self->button[i].window,
                    self->button[i].x, self->button[i].y,
                    self->button[i].width, self->button[i].height);
}

static void prompt_resize(ObPrompt *self, gint w, gint h)
{
    self->width = w;
    self->height = h;

    /* the user can't resize the prompt */
    ob_backend->dialog_set_size(self->super.window, w, h, self->mapped);
}

static void setup_button_focus_tex(ObPromptElement *e, RrAppearance *a,
                                   gboolean on)
{
    gint i, l, r, t, b;

    for (i = 1; i < 5; ++i)
        a->texture[i].type = on ? RR_TEXTURE_LINE_ART : RR_TEXTURE_NONE;

    if (!on) return;

    RrMargins(a, &l, &t, &r, &b);
    l += MIN(BUTTON_HMARGIN, BUTTON_VMARGIN) / 2;
    r += MIN(BUTTON_HMARGIN, BUTTON_VMARGIN) / 2;
    t += MIN(BUTTON_HMARGIN, BUTTON_VMARGIN) / 2;
    b += MIN(BUTTON_HMARGIN, BUTTON_VMARGIN) / 2;

    /* top line */
    a->texture[1].data.lineart.x1 = l;
    a->texture[1].data.lineart.x2 = e->width - r - 1;
    a->texture[1].data.lineart.y1 = t;
    a->texture[1].data.lineart.y2 = t;

    /* bottom line */
    a->texture[2].data.lineart.x1 = l;
    a->texture[2].data.lineart.x2 = e->width - r - 1;
    a->texture[2].data.lineart.y1 = e->height - b - 1;
    a->texture[2].data.lineart.y2 = e->height - b - 1;

    /* left line */
    a->texture[3].data.lineart.x1 = l;
    a->texture[3].data.lineart.x2 = l;
    a->texture[3].data.lineart.y1 = t;
    a->texture[3].data.lineart.y2 = e->height - b - 1;

    /* right line */
    a->texture[4].data.lineart.x1 = e->width - r - 1;
    a->texture[4].data.lineart.x2 = e->width - r - 1;
    a->texture[4].data.lineart.y1 = t;
    a->texture[4].data.lineart.y2 = e->height - b - 1;
}

static void render_button(ObPrompt *self, ObPromptElement *e)
{
    RrAppearance *a;

    if (e->hover && e->pressed)       a = prompt_a_press;
    else if (self->focus == e)        a = prompt_a_focus;
    else                              a = prompt_a_button;

    a->surface.parent = prompt_a_bg;
    a->surface.parentx = e->x;
    a->surface.parenty = e->y;

    /* draw the keyfocus line */
    if (self->focus == e)
        setup_button_focus_tex(e, a, TRUE);

    a->texture[0].data.text.string = e->text;
    paint_canvas(e->window, a, e->width, e->height);

    /* turn off the keyfocus line so that it doesn't affect size calculations
     */
    if (self->focus == e)
        setup_button_focus_tex(e, a, FALSE);
}

static void render_all(ObPrompt *self)
{
    gint i;

    paint_canvas(self->super.window, prompt_a_bg, self->width, self->height);

    prompt_a_msg->surface.parent = prompt_a_bg;
    prompt_a_msg->surface.parentx = self->msg.x;
    prompt_a_msg->surface.parenty = self->msg.y;

    prompt_a_msg->texture[0].data.text.string = self->msg.text;
    prompt_a_msg->texture[0].data.text.maxwidth = self->msg_wbound;
    paint_canvas(self->msg.window, prompt_a_msg, self->msg.width, self->msg.height);

    for (i = 0; i < self->n_buttons; ++i)
        render_button(self, &self->button[i]);
}

void prompt_show(ObPrompt *self, ObClient *parent, gboolean modal)
{
    gint i;

    if (self->mapped) {
        /* activate the prompt, as its application would */
        ob_backend_events.request_activate(self->super.window,
                                           OB_REQUEST_FROM_APPLICATION,
                                           event_time());
        return;
    }

    /* set the focused button (if not found then the first button is used) */
    self->focus = &self->button[0];
    for (i = 0; i < self->n_buttons; ++i)
        if (self->button[i].result == self->default_result) {
            self->focus = &self->button[i];
            break;
        }

    if (parent) {
        /* transient for the window's group, or the window directly */
        if (parent->group)
            ob_backend->dialog_set_parent(self->super.window, OB_SURFACE_NONE,
                                          parent->group->leader, modal);
        else
            ob_backend->dialog_set_parent(self->super.window, parent->window,
                                          OB_SURFACE_NONE, modal);
    }
    else
        ob_backend->dialog_set_parent(self->super.window, OB_SURFACE_NONE,
                                      OB_SURFACE_NONE, FALSE);

    /* set up the dialog and render it */
    prompt_layout(self);
    render_all(self);

    client_manage(self->super.window, self);

    self->mapped = TRUE;
}

void prompt_hide(ObPrompt *self)
{
    ob_backend->canvas_show(self->super.window, FALSE);
    self->mapped = FALSE;
}

gboolean prompt_key_event(ObPrompt *self, const ObInputEvent *e)
{
    gboolean shift;
    guint shift_mask, mods;
    guint32 sym;

    if (e->type != OB_INPUT_KEY_PRESS) return FALSE;

    shift_mask = ob_backend->modkey_mask(OB_MODKEY_SHIFT);
    mods = ob_backend->only_modmasks(e->mods);
    shift = !!(mods & shift_mask);

    /* only accept shift */
    if (mods != 0 && mods != shift_mask)
        return FALSE;

    sym = e->keysym;

    if (sym == XKB_KEY_Escape)
        prompt_cancel(self);
    else if (sym == XKB_KEY_Return || sym == XKB_KEY_KP_Enter || sym == XKB_KEY_space)
        prompt_run_callback(self, self->focus->result);
    else if (sym == XKB_KEY_Tab || sym == XKB_KEY_Left || sym == XKB_KEY_Right) {
        gint i;
        gboolean left;
        ObPromptElement *oldfocus;

        left = (sym == XKB_KEY_Left) || ((sym == XKB_KEY_Tab) && shift);
        oldfocus = self->focus;

        for (i = 0; i < self->n_buttons; ++i)
            if (self->focus == &self->button[i]) break;
        i += (left ? -1 : 1);
        if (i < 0) i = self->n_buttons - 1;
        else if (i >= self->n_buttons) i = 0;
        self->focus = &self->button[i];

        if (oldfocus != self->focus) render_button(self, oldfocus);
        render_button(self, self->focus);
    }
    return TRUE;
}

gboolean prompt_mouse_event(ObPrompt *self, const ObInputEvent *e)
{
    gint i;
    ObPromptElement *but;

    if (e->type != OB_INPUT_BUTTON_PRESS &&
        e->type != OB_INPUT_BUTTON_RELEASE &&
        e->type != OB_INPUT_MOTION) return FALSE;

    /* find the button */
    but = NULL;
    for (i = 0; i < self->n_buttons; ++i)
        if (self->button[i].window == (ObWindowId)e->over)
        {
            but = &self->button[i];
            break;
        }
    if (!but) return FALSE;

    if (e->type == OB_INPUT_BUTTON_PRESS) {
        ObPromptElement *oldfocus;

        oldfocus = self->focus;

        but->pressed = but->hover = TRUE;
        self->focus = but;

        if (oldfocus != but) render_button(self, oldfocus);
        render_button(self, but);
    }
    else if (e->type == OB_INPUT_BUTTON_RELEASE) {
        if (but->hover)
            prompt_run_callback(self, but->result);
        but->pressed = FALSE;
    }
    else if (e->type == OB_INPUT_MOTION) {
        if (but->pressed) {
            gboolean hover;

            hover = (e->x >= 0 && e->y >= 0 &&
                     e->x < but->width && e->y < but->height);

            if (hover != but->hover) {
                but->hover = hover;
                render_button(self, but);
            }
        }
    }
    return TRUE;
}

void prompt_cancel(ObPrompt *self)
{
    prompt_run_callback(self, self->cancel_result);
}

static gboolean prompt_show_message_cb(ObPrompt *p, int res, gpointer data)
{
    return TRUE; /* call the cleanup func */
}

static void prompt_show_message_cleanup(ObPrompt *p, gpointer data)
{
    prompt_unref(p);
}

ObPrompt* prompt_show_message(const gchar *msg, const gchar *title,
                              const gchar *answer)
{
    ObPrompt *p;
    ObPromptAnswer ans[] = {
        { answer, 0 }
    };

    p = prompt_new(msg, title, ans, 1, 0, 0,
                   prompt_show_message_cb, prompt_show_message_cleanup, NULL);
    prompt_show(p, NULL, FALSE);
    return p;
}

static void prompt_run_callback(ObPrompt *self, gint result)
{
    prompt_ref(self);
    if (self->func) {
        gboolean clean = self->func(self, result, self->data);
        if (clean && self->cleanup)
            self->cleanup(self, self->data);
    }
    prompt_hide(self);
    prompt_unref(self);
}
