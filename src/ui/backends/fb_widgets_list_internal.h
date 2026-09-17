#ifndef MESH_UI_BACKENDS_FB_WIDGETS_LIST_INTERNAL_H
#define MESH_UI_BACKENDS_FB_WIDGETS_LIST_INTERNAL_H

/*
 * The handful of answers the list window and the row it draws both need.
 *
 * fb_widgets_list.c owns the window - where a row lands, whether it stands on a card, what the
 * cursor is on - and fb_widgets_item.c draws into it. Everything here would still be `static`
 * if the two were one file; it is declared only because splitting them is what keeps the
 * window's arithmetic and the row's slots readable apart.
 *
 * Not public API, and not part of fb_widgets.h: nothing outside these two files should include
 * it.
 */

/*
 * The disc and what is in it: a node's initials, or an icon for the rows that are not a person.
 *
 * Shared by two components and therefore neither's: a list row's leading slot draws one, and so
 * does a card heading over rows that carry them. See fb_widgets_list.c for what it measures and
 * why the fit is measured rather than assumed.
 */
void fb_draw_avatar(const struct mesh_ui_backend_fb_state *state, int x, int y, int size,
                    const char *label, enum mesh_ui_icon icon, struct mesh_ui_paint paint);

/* Whether item `index` draws the cursor's highlight. Never on a list whose card is focused
   instead, which is what FB_LIST_FOCUS_CARD means. */
bool fb_list_is_cursor(const struct fb_list *list, uint32_t index);

/* Whether item `index` stands on a card at all. */
bool fb_list_on_card(const struct fb_list *list, uint32_t index);

/* Whether this list draws its groups as cards at all - a question about the list, not the row. */
bool fb_list_has_cards(const struct fb_list *list);

/* The card surfaces and the scroll rail, drawn once before the first row. Every entry point
   that draws a row calls it, and it does its work only on the first. */
void fb_list_chrome(const struct mesh_ui_backend_fb_state *state, struct fb_list *list);

/* The ink a row's text takes: its tone, or the cursor's, or the quiet pairing for the slots
   that are deliberately secondary. */
struct mesh_ui_rgb fb_item_ink(const struct mesh_ui_backend_fb_state *state, enum mesh_ui_tone tone,
                               bool selected, bool quiet);

#endif /* MESH_UI_BACKENDS_FB_WIDGETS_LIST_INTERNAL_H */
