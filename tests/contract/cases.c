/* Lumen contract tests: what the window manager must do.

   Each case starts from a screen with no windows of its own and destroys what
   it made. The cases rely on the default configuration (data/rc.xml): new
   windows take the focus and there are four desktops. */

#include "contract.h"

#include <stddef.h>

static const ct_rect SMALL = { 100, 100, 300, 200 };

static ct_win *mapped(ct_win_type type, const char *title, ct_rect want)
{
    ct_win *w = ct_window_new(type, title, want);
    ct_window_map(w);
    CT_EVENTUALLY(ct_window_managed(w) && ct_window_viewable(w));
    return w;
}

/* A mapped window is managed and shown, and given its size. */
static void map_manages(void)
{
    ct_win *w = mapped(CT_WIN_NORMAL, "map", SMALL);
    ct_rect c = ct_window_client(w);
    CT_CHECK(c.w == SMALL.w && c.h == SMALL.h);
    CT_CHECK(ct_rect_contains(ct_window_frame(w), c));
    CT_CHECK(ct_rect_contains(ct_screen(), ct_window_frame(w)));
    ct_window_destroy(w);
}

/* Taskbars see the title a window gives itself, and see it change. */
static void title_shown(void)
{
    ct_win *w = mapped(CT_WIN_NORMAL, "contract title", SMALL);
    CT_EVENTUALLY(ct_window_title_is(w, "contract title"));
    ct_window_set_title(w, "renamed");
    CT_EVENTUALLY(ct_window_title_is(w, "renamed"));
    ct_window_destroy(w);
}

/* A window without a title is shown with its application's name. */
static void untitled_shows_app_id(void)
{
    ct_win *w = ct_window_new(CT_WIN_NORMAL, NULL, SMALL);
    ct_window_set_app_id(w, "ContractApp");
    ct_window_map(w);
    CT_EVENTUALLY(ct_window_managed(w) && ct_window_viewable(w));
    CT_EVENTUALLY(ct_window_title_is(w, "ContractApp"));
    ct_window_destroy(w);
}

/* A window that draws its own decorations gets none from the window
   manager. */
static void undecorated_request_honoured(void)
{
    ct_win *w = ct_window_new(CT_WIN_NORMAL, "undecorated", SMALL);
    ct_window_set_undecorated(w);
    ct_window_map(w);
    CT_EVENTUALLY(ct_window_managed(w) && ct_window_viewable(w));
    CT_EVENTUALLY(ct_rect_eq(ct_window_frame(w), ct_window_client(w)));
    ct_window_destroy(w);
}

/* A destroyed window stops being managed. */
static void destroy_unmanages(void)
{
    ct_win *w = mapped(CT_WIN_NORMAL, "destroy", SMALL);
    ct_window_destroy(w);
    CT_EVENTUALLY(!ct_window_managed(w));
    CT_CHECK(ct_window_stack_index(w) == -1);
}

/* A window the client withdraws is let go, and managed again when the
   client maps it again. */
static void withdraw_unmanages(void)
{
    ct_win *w = mapped(CT_WIN_NORMAL, "withdraw", SMALL);
    ct_window_withdraw(w);
    CT_EVENTUALLY(!ct_window_managed(w));
    CT_CHECK(!ct_window_viewable(w));

    ct_window_map(w);
    CT_EVENTUALLY(ct_window_managed(w));
    CT_EVENTUALLY(ct_window_viewable(w));
    ct_window_destroy(w);
}

/* A new window takes the focus. */
static void new_window_focused(void)
{
    ct_win *a = mapped(CT_WIN_NORMAL, "first", SMALL);
    CT_EVENTUALLY(ct_window_focused(a));
    ct_win *b = mapped(CT_WIN_NORMAL, "second", SMALL);
    CT_EVENTUALLY(ct_window_focused(b));
    CT_CHECK(!ct_window_focused(a));
    ct_window_destroy(b);
    ct_window_destroy(a);
}

/* A client that gives itself the focus becomes the active window. */
static void client_focus_followed(void)
{
    CT_REQUIRE(CT_CAP_CLIENT_FOCUS);
    ct_win *a = mapped(CT_WIN_NORMAL, "first", SMALL);
    ct_win *b = mapped(CT_WIN_NORMAL, "second", SMALL);
    CT_EVENTUALLY(ct_window_focused(b));

    ct_window_take_focus(a);
    CT_EVENTUALLY(ct_window_focused(a));
    ct_window_destroy(b);
    ct_window_destroy(a);
}

/* When a popup that took the focus goes away, the focus comes back to a
   window. */
static void focus_returns_after_popup(void)
{
    CT_REQUIRE(CT_CAP_CLIENT_FOCUS);
    ct_win *a = mapped(CT_WIN_NORMAL, "witness", SMALL);
    CT_EVENTUALLY(ct_window_focused(a));

    ct_win *p = ct_window_new(CT_WIN_POPUP, "popup", SMALL);
    ct_window_map(p);
    CT_EVENTUALLY(ct_window_viewable(p));
    CT_CHECK(!ct_window_managed(p));
    ct_window_take_focus(p);
    CT_EVENTUALLY(!ct_window_focused(a));

    ct_window_destroy(p);
    CT_EVENTUALLY(ct_window_focused(a));
    ct_window_destroy(a);
}

/* Activating a window focuses it and raises it above the others. */
static void activate_focuses_and_raises(void)
{
    CT_REQUIRE(CT_CAP_STACKING);
    ct_win *a = mapped(CT_WIN_NORMAL, "below", SMALL);
    ct_win *b = mapped(CT_WIN_NORMAL, "above", SMALL);
    CT_EVENTUALLY(ct_window_focused(b));
    CT_CHECK(ct_window_stack_index(b) > ct_window_stack_index(a));

    ct_request_activate(a);
    CT_EVENTUALLY(ct_window_focused(a));
    CT_EVENTUALLY(ct_window_stack_index(a) > ct_window_stack_index(b));
    ct_window_destroy(b);
    ct_window_destroy(a);
}

/* Closing asks the window to close; the focus falls back to another one. */
static void close_falls_back(void)
{
    ct_win *a = mapped(CT_WIN_NORMAL, "stays", SMALL);
    ct_win *b = mapped(CT_WIN_NORMAL, "closes", SMALL);
    CT_EVENTUALLY(ct_window_focused(b));

    ct_request_close(b);
    CT_EVENTUALLY(ct_window_closed(b));
    CT_EVENTUALLY(ct_window_focused(a));
    ct_window_destroy(b);
    ct_window_destroy(a);
}

/* A maximized window fills the work area; restoring gives its size back. */
static void maximize_fills_workarea(void)
{
    ct_win *w = mapped(CT_WIN_NORMAL, "maximize", SMALL);
    ct_rect before = ct_window_frame(w);

    ct_request_state(w, CT_STATE_MAXIMIZED, true);
    CT_EVENTUALLY(ct_window_has_state(w, CT_STATE_MAXIMIZED));
    CT_EVENTUALLY(ct_rect_eq(ct_window_frame(w), ct_workarea()));

    ct_request_state(w, CT_STATE_MAXIMIZED, false);
    CT_EVENTUALLY(!ct_window_has_state(w, CT_STATE_MAXIMIZED));
    CT_EVENTUALLY(ct_rect_eq(ct_window_frame(w), before));
    ct_window_destroy(w);
}

/* Toggling a state flips it, both ways. */
static void maximize_toggles(void)
{
    ct_win *w = mapped(CT_WIN_NORMAL, "toggle", SMALL);
    ct_rect before = ct_window_frame(w);

    ct_request_toggle_state(w, CT_STATE_MAXIMIZED);
    CT_EVENTUALLY(ct_window_has_state(w, CT_STATE_MAXIMIZED));
    CT_EVENTUALLY(ct_rect_eq(ct_window_frame(w), ct_workarea()));

    ct_request_toggle_state(w, CT_STATE_MAXIMIZED);
    CT_EVENTUALLY(!ct_window_has_state(w, CT_STATE_MAXIMIZED));
    CT_EVENTUALLY(ct_rect_eq(ct_window_frame(w), before));
    ct_window_destroy(w);
}

/* A fullscreen window covers the screen with no decorations. */
static void fullscreen_covers_screen(void)
{
    ct_win *w = mapped(CT_WIN_NORMAL, "fullscreen", SMALL);
    ct_rect before = ct_window_client(w);

    ct_request_state(w, CT_STATE_FULLSCREEN, true);
    CT_EVENTUALLY(ct_window_has_state(w, CT_STATE_FULLSCREEN));
    CT_EVENTUALLY(ct_rect_eq(ct_window_client(w), ct_screen()));
    CT_CHECK(ct_rect_eq(ct_window_frame(w), ct_screen()));

    ct_request_state(w, CT_STATE_FULLSCREEN, false);
    CT_EVENTUALLY(!ct_window_has_state(w, CT_STATE_FULLSCREEN));
    CT_EVENTUALLY(ct_window_client(w).w == before.w &&
                  ct_window_client(w).h == before.h);
    ct_window_destroy(w);
}

/* A window can ask to show up fullscreen. */
static void starts_fullscreen(void)
{
    ct_win *w = ct_window_new(CT_WIN_NORMAL, "starts fullscreen", SMALL);
    ct_window_set_initial_state(w, CT_STATE_FULLSCREEN);
    ct_window_map(w);
    CT_EVENTUALLY(ct_window_managed(w) && ct_window_viewable(w));
    CT_EVENTUALLY(ct_window_has_state(w, CT_STATE_FULLSCREEN));
    CT_EVENTUALLY(ct_rect_eq(ct_window_client(w), ct_screen()));
    ct_window_destroy(w);
}

/* An iconified window is hidden and gives up the focus; activating it
   shows it again. */
static void iconify_hides(void)
{
    ct_win *a = mapped(CT_WIN_NORMAL, "witness", SMALL);
    ct_win *w = mapped(CT_WIN_NORMAL, "iconify", SMALL);
    CT_EVENTUALLY(ct_window_focused(w));

    ct_request_state(w, CT_STATE_ICONIC, true);
    CT_EVENTUALLY(ct_window_has_state(w, CT_STATE_ICONIC));
    CT_EVENTUALLY(!ct_window_viewable(w));
    CT_CHECK(ct_window_managed(w));
    CT_EVENTUALLY(ct_window_focused(a));

    ct_request_activate(w);
    CT_EVENTUALLY(ct_window_viewable(w));
    CT_CHECK(!ct_window_has_state(w, CT_STATE_ICONIC));
    CT_EVENTUALLY(ct_window_focused(w));
    ct_window_destroy(w);
    ct_window_destroy(a);
}

/* Mapping an iconified window again shows it. */
static void map_restores_iconic(void)
{
    ct_win *w = mapped(CT_WIN_NORMAL, "remap", SMALL);
    ct_request_state(w, CT_STATE_ICONIC, true);
    CT_EVENTUALLY(!ct_window_viewable(w));

    ct_window_map(w);
    CT_EVENTUALLY(ct_window_viewable(w));
    CT_CHECK(!ct_window_has_state(w, CT_STATE_ICONIC));
    CT_CHECK(ct_window_managed(w));
    ct_window_destroy(w);
}

/* A window that is kept above stays above the windows that get raised. */
static void above_stays_above(void)
{
    CT_REQUIRE(CT_CAP_STACKING);
    ct_win *top = mapped(CT_WIN_NORMAL, "kept above", SMALL);
    ct_request_state(top, CT_STATE_ABOVE, true);
    CT_EVENTUALLY(ct_window_has_state(top, CT_STATE_ABOVE));

    ct_win *w = mapped(CT_WIN_NORMAL, "raised", SMALL);
    ct_request_activate(w);
    CT_EVENTUALLY(ct_window_focused(w));
    CT_CHECK(ct_window_stack_index(top) > ct_window_stack_index(w));
    ct_window_destroy(w);
    ct_window_destroy(top);
}

/* A window can lower itself, and a pager can raise it again. */
static void restack_requests_honoured(void)
{
    CT_REQUIRE(CT_CAP_STACKING);
    ct_win *a = mapped(CT_WIN_NORMAL, "stays", SMALL);
    ct_win *b = mapped(CT_WIN_NORMAL, "lowers itself", SMALL);
    CT_EVENTUALLY(ct_window_stack_index(b) > ct_window_stack_index(a));

    ct_request_lower(b);
    CT_EVENTUALLY(ct_window_stack_index(b) < ct_window_stack_index(a));

    ct_pager_raise(b);
    CT_EVENTUALLY(ct_window_stack_index(b) > ct_window_stack_index(a));
    ct_window_destroy(b);
    ct_window_destroy(a);
}

/* A dialog stays above the window it belongs to. */
static void dialog_above_parent(void)
{
    CT_REQUIRE(CT_CAP_STACKING);
    ct_win *parent = mapped(CT_WIN_NORMAL, "parent", SMALL);
    ct_win *d = ct_window_new(CT_WIN_DIALOG, "dialog", SMALL);
    ct_window_set_parent(d, parent);
    ct_window_map(d);
    CT_EVENTUALLY(ct_window_viewable(d) && ct_window_focused(d));

    ct_request_activate(parent);
    ct_settle();
    CT_CHECK(ct_window_stack_index(d) > ct_window_stack_index(parent));
    ct_window_destroy(d);
    ct_window_destroy(parent);
}

/* A dock's strut takes its edge out of the work area, and maximized windows
   keep out of it. */
static void strut_shrinks_workarea(void)
{
    CT_REQUIRE(CT_CAP_STRUTS);
    ct_rect screen = ct_screen();
    ct_rect before = ct_workarea();

    ct_win *dock = ct_window_new(CT_WIN_DOCK, "dock",
                                 (ct_rect){ 0, 0, screen.w, 40 });
    ct_window_set_strut(dock, 40);
    ct_window_map(dock);
    CT_EVENTUALLY(ct_workarea().y >= before.y + 40);
    CT_CHECK(!ct_window_focused(dock));

    ct_win *w = mapped(CT_WIN_NORMAL, "maximized", SMALL);
    ct_request_state(w, CT_STATE_MAXIMIZED, true);
    CT_EVENTUALLY(ct_rect_eq(ct_window_frame(w), ct_workarea()));
    CT_CHECK(ct_window_frame(w).y >= 40);

    ct_window_destroy(dock);
    CT_EVENTUALLY(ct_rect_eq(ct_workarea(), before));
    ct_window_destroy(w);
}

/* A client can resize itself; its minimum size is kept. */
static void resize_request_honoured(void)
{
    ct_win *w = ct_window_new(CT_WIN_NORMAL, "resize", SMALL);
    ct_window_set_min_size(w, 200, 150);
    ct_window_map(w);
    CT_EVENTUALLY(ct_window_viewable(w));
    ct_rect c = ct_window_client(w);

    ct_request_move_resize(w, (ct_rect){ c.x, c.y, 420, 260 });
    CT_EVENTUALLY(ct_window_client(w).w == 420 &&
                  ct_window_client(w).h == 260);

    ct_request_move_resize(w, (ct_rect){ c.x, c.y, 50, 50 });
    ct_settle();
    CT_CHECK(ct_window_client(w).w >= 200 && ct_window_client(w).h >= 150);
    ct_window_destroy(w);
}

/* A larger minimum size set while mapped makes the window grow to it. */
static void min_size_change_applies(void)
{
    ct_win *w = mapped(CT_WIN_NORMAL, "grow", SMALL);
    CT_CHECK(ct_window_client(w).w == SMALL.w);

    ct_window_set_min_size(w, SMALL.w + 100, SMALL.h + 50);
    CT_EVENTUALLY(ct_window_client(w).w >= SMALL.w + 100 &&
                  ct_window_client(w).h >= SMALL.h + 50);
    ct_window_destroy(w);
}

/* A client can place itself. */
static void move_request_honoured(void)
{
    CT_REQUIRE(CT_CAP_CLIENT_POSITION);
    ct_win *w = mapped(CT_WIN_NORMAL, "move", SMALL);
    ct_rect f = ct_window_frame(w);

    ct_request_move_resize(w, (ct_rect){ 250, 150, SMALL.w, SMALL.h });
    CT_EVENTUALLY(ct_window_frame(w).x != f.x ||
                  ct_window_frame(w).y != f.y);
    CT_CHECK(ct_rect_contains(ct_screen(), ct_window_frame(w)));
    ct_window_destroy(w);
}

/* A pager can move and resize a window. */
static void pager_move_resize_honoured(void)
{
    CT_REQUIRE(CT_CAP_CLIENT_POSITION);
    ct_win *w = mapped(CT_WIN_NORMAL, "pager moved", SMALL);

    ct_pager_move_resize(w, (ct_rect){ 300, 200, 350, 250 });
    CT_EVENTUALLY(ct_window_client(w).w == 350 &&
                  ct_window_client(w).h == 250);
    CT_EVENTUALLY(ct_window_frame(w).x == 300 && ct_window_frame(w).y == 200);
    ct_window_destroy(w);
}

/* Switching desktops hides the windows of the old one; a window sent to
   another desktop goes there. */
static void desktops_switch(void)
{
    CT_REQUIRE(CT_CAP_DESKTOPS);
    CT_CHECK(ct_desktop_count() >= 2);
    int home = ct_current_desktop();
    int away = (home + 1) % ct_desktop_count();

    ct_win *w = mapped(CT_WIN_NORMAL, "desktop", SMALL);
    CT_CHECK(ct_window_desktop(w) == home);

    ct_request_desktop(away);
    CT_EVENTUALLY(ct_current_desktop() == away);
    CT_EVENTUALLY(!ct_window_viewable(w));

    ct_request_desktop(home);
    CT_EVENTUALLY(ct_window_viewable(w));

    ct_request_window_desktop(w, away);
    CT_EVENTUALLY(ct_window_desktop(w) == away);
    CT_EVENTUALLY(!ct_window_viewable(w));
    CT_CHECK(ct_current_desktop() == home);
    ct_window_destroy(w);
}

/* A window can ask to show up on another desktop; it stays there, hidden,
   and the current desktop doesn't change. */
static void starts_on_desktop(void)
{
    CT_REQUIRE(CT_CAP_DESKTOPS);
    int home = ct_current_desktop();
    int away = (home + 1) % ct_desktop_count();

    ct_win *w = ct_window_new(CT_WIN_NORMAL, "starts away", SMALL);
    ct_window_set_initial_desktop(w, away);
    ct_window_map(w);
    CT_EVENTUALLY(ct_window_managed(w));
    CT_EVENTUALLY(ct_window_desktop(w) == away);
    CT_CHECK(!ct_window_viewable(w));
    CT_CHECK(ct_current_desktop() == home);
    ct_window_destroy(w);
}

/* A key binding runs its action (GoToDesktop on C-A-Right in rc.xml). */
static void key_binding_runs(void)
{
    CT_REQUIRE(CT_CAP_INPUT | CT_CAP_DESKTOPS);
    CT_EVENTUALLY(ct_current_desktop() == 0);
    ct_input_keys("C-A-Right");
    CT_EVENTUALLY(ct_current_desktop() == 1);
    ct_input_keys("C-A-Left");
    CT_EVENTUALLY(ct_current_desktop() == 0);
}

/* Alt-Tab goes to the window used before, and holds the keyboard until Alt
   is let go. */
static void alt_tab_switches(void)
{
    CT_REQUIRE(CT_CAP_INPUT);
    ct_win *a = mapped(CT_WIN_NORMAL, "first", SMALL);
    ct_win *b = mapped(CT_WIN_NORMAL, "second", SMALL);
    CT_EVENTUALLY(ct_window_focused(b));

    ct_input_keys("A-Tab");
    CT_EVENTUALLY(ct_window_focused(a));
    ct_settle();
    ct_input_keys("A-Tab");
    CT_EVENTUALLY(ct_window_focused(b));
    ct_window_destroy(b);
    ct_window_destroy(a);
}

/* Dragging a window with Alt held moves it by as much. */
static void alt_drag_moves(void)
{
    CT_REQUIRE(CT_CAP_INPUT);
    ct_win *w = mapped(CT_WIN_NORMAL, "drag", SMALL);
    ct_settle();
    ct_rect before = ct_window_frame(w);

    ct_input_drag(w, "A", 120, 80);
    CT_EVENTUALLY(ct_window_frame(w).x == before.x + 120 &&
                  ct_window_frame(w).y == before.y + 80);
    CT_CHECK(ct_window_frame(w).w == before.w);
    ct_window_destroy(w);
}

const ct_case ct_cases[] = {
    { "map_manages", map_manages },
    { "title_shown", title_shown },
    { "untitled_shows_app_id", untitled_shows_app_id },
    { "undecorated_request_honoured", undecorated_request_honoured },
    { "destroy_unmanages", destroy_unmanages },
    { "withdraw_unmanages", withdraw_unmanages },
    { "new_window_focused", new_window_focused },
    { "client_focus_followed", client_focus_followed },
    { "focus_returns_after_popup", focus_returns_after_popup },
    { "activate_focuses_and_raises", activate_focuses_and_raises },
    { "close_falls_back", close_falls_back },
    { "maximize_fills_workarea", maximize_fills_workarea },
    { "maximize_toggles", maximize_toggles },
    { "fullscreen_covers_screen", fullscreen_covers_screen },
    { "starts_fullscreen", starts_fullscreen },
    { "iconify_hides", iconify_hides },
    { "map_restores_iconic", map_restores_iconic },
    { "above_stays_above", above_stays_above },
    { "restack_requests_honoured", restack_requests_honoured },
    { "dialog_above_parent", dialog_above_parent },
    { "strut_shrinks_workarea", strut_shrinks_workarea },
    { "resize_request_honoured", resize_request_honoured },
    { "min_size_change_applies", min_size_change_applies },
    { "move_request_honoured", move_request_honoured },
    { "pager_move_resize_honoured", pager_move_resize_honoured },
    { "desktops_switch", desktops_switch },
    { "starts_on_desktop", starts_on_desktop },
    { "key_binding_runs", key_binding_runs },
    { "alt_tab_switches", alt_tab_switches },
    { "alt_drag_moves", alt_drag_moves },
    { NULL, NULL }
};
