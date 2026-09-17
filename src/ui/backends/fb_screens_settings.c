#define _POSIX_C_SOURCE 200809L

/*
 * The Settings tab: the section list, or one section's label/value rows.
 *
 * The longest renderer in the client, and it is one screen rather than several because the model
 * is: src/ui/settings/ says what the rows are, which of them are editable, what a pending edit
 * is worth and which control says a value best. What is left here is the reading of that model
 * onto rows - a switch for a boolean, a slider for a scale, a dot for an edit that is not saved
 * yet - and the measuring pass that has to agree with it step for step.
 */

#include "fb_widgets.h"

#include "fb_screens_internal.h"

#include "mesh/i18n/strings.h"
#include "mesh/ui/nav.h"
#include "mesh/ui/settings.h"
#include "mesh/ui/trust.h"

#include <stdio.h>
#include <string.h>

/*
 * Whether a settings row says its value with a slider, and where the handle goes if it does.
 *
 * One function, asked twice - once to measure the row's height and once to draw it - for the
 * reason fb_trailing_cols() is one function: a control whose presence was decided by one piece
 * of code and whose room was reserved by another is a control drawn over the row beneath it.
 * Here the two answers are a step apart rather than a cell, which is the more visible half of
 * the same bug.
 *
 * The decision itself is not this screen's. Whether a field's numbers measure something or name
 * something is a fact about the field, stated in its own table entry, and the arithmetic that
 * places a value among the presets is unit-tested in the settings model - so this is a call, not
 * a rule. What the backend decides is only that a scale is worth a length, which is the same
 * choice it makes when a boolean gets a switch instead of the word "On".
 */
static bool settings_row_slider(const struct mesh_ui_settings_item *item,
                                struct mesh_ui_settings_track *out) {
    if (item->kind != MESH_UI_SETTING_NUMBER || item->field == MESH_UI_FIELD_NONE) {
        return false;
    }
    return mesh_ui_settings_number_track(item->field, item->number, out);
}

/*
 * Whether a section's groups are worth drawing as cards at all.
 *
 * A card says "these rows belong together", so it needs something to be together apart from. One
 * card wrapping a whole section says nothing: it is a border drawn round the page, and a border
 * is not a grouping. Modules is fourteen ungrouped rows and was drawn inside one, because
 * `any_cards` was set for every open section while only the *assignment* below looked at
 * headings - the rule that assignment is written against, and did not have.
 *
 * Asked of mesh_ui_settings_section_groups() rather than answered here, because the navigation and
 * the help screen ask the same question and a second implementation of it is a second opinion:
 * a section that drew no cards while L2/R2 claimed to cross them is exactly the disagreement
 * that header's note is about. Groups and cards are the same number - see there.
 */

/* Settings: the section list, or one section's label/value rows. Editable rows show a
   pending edit in place of the radio's value, marked with a dot until Y saves it. */
/* Takes the state mutably, unlike its neighbours: the switches on the toggle rows step an
   animation kept on it. Nothing else here writes to the state. */
void fb_render_settings(struct mesh_ui_backend_fb_state *state,
                        const struct mesh_ui_snapshot *snapshot, struct fb_layout *layout) {
    const struct mesh_ui_nav *nav = &snapshot->nav;
    const struct mesh_ui_settings *settings = &snapshot->settings;
    const struct mesh_ui_handshake_state *handshake =
        snapshot->handshake_valid ? &snapshot->handshake : NULL;
    const bool section_open = (nav->settings_section != MESH_UI_SETTINGS_NO_SECTION);
    const enum mesh_ui_settings_section section =
        (enum mesh_ui_settings_section)nav->settings_section;

    /*
     * The breadcrumb, as slots rather than as a sentence.
     *
     * It used to be one catalog entry - "Settings > %s%s%s" - with "Modules > " for the third
     * level and " (unsaved)" arriving as the trailing %s. Three things were wrong with that and
     * the app bar answers all three: a translator was handed the trail's grammar along with its
     * words, a count glued in with %s cannot be a badge, and at the title's glyph scale
     * "Settings > Modules > Telemetry" is thirty of the thirty-four cells on the line, so the
     * leaf - the only part naming *this* screen - was the half that got elided.
     *
     * What is left here is a level per slot. The separators are the component's, and it draws
     * them as chevrons.
     */
    struct fb_app_bar bar = {.title = mesh_str(MESH_STR_SETTINGS_TITLE)};
    char title[96];
    char unsaved[32];
    if (section_open) {
        /*
         * The levels *between* the tab and this screen, which for most sections is none: the
         * navigation bar is already saying "Settings", selected, three rows above, and a trail
         * that repeated it would spend a body row on a word the frame already carries. What is
         * left is the one level nothing else says - "Modules" over a module's own section, and
         * "Channels" over one channel - which is exactly where the breadcrumb was earning its
         * keep and nowhere else.
         */
        if (nav->settings_channel != MESH_UI_SETTINGS_NO_CHANNEL) {
            /* One channel out of the Channels list: the list is the level above it, and unlike
               Modules it is not in settings_parent - a channel is identified by its number
               rather than by a section of its own. */
            bar.trail[bar.trail_count++] = mesh_ui_settings_section_name(MESH_UI_SETTINGS_CHANNELS);
            mesh_str_format(title, sizeof title, MESH_STR_SETTINGS_TITLE_CHANNEL,
                            (unsigned)nav->settings_channel);
            bar.title = title;
        } else {
            if (nav->settings_parent != MESH_UI_SETTINGS_NO_SECTION) {
                bar.trail[bar.trail_count++] = mesh_ui_settings_section_name(
                    (enum mesh_ui_settings_section)nav->settings_parent);
            }
            bar.title = mesh_ui_settings_section_name(section);
        }
        /*
         * How many rows are edited and not yet written, in the slot that is about the screen
         * rather than about a row. The warning family because that is what it is: the radio
         * does not know about these yet, and leaving the section is what loses them.
         */
        if (nav->settings_edit_count > 0U) {
            mesh_str_format(unsaved, sizeof unsaved, MESH_STR_SETTINGS_UNSAVED,
                            (unsigned)nav->settings_edit_count);
            bar.badge = unsaved;
            bar.badge_family = MESH_UI_FAMILY_WARNING;
        }
    }
    fb_draw_app_bar(state, layout, &bar);

    /* Every other section describes the radio, but About describes this client, so the tab
       stays usable with nothing connected: the section list still draws (About is the only
       row not greyed out) and opening About still works. Modules is let through for the
       reason the section list itself is - it is a list of what exists, not a read of the
       radio, and each of its rows says "not loaded" on its own. */
    if (!settings->loaded && (handshake == NULL || !handshake->has_my_info) && section_open &&
        section != MESH_UI_SETTINGS_ABOUT && section != MESH_UI_SETTINGS_MODULES) {
        fb_draw_empty(state, layout, MESH_UI_ICON_SETTINGS,
                      mesh_str(MESH_STR_SETTINGS_EMPTY_DISCONNECT));
        return;
    }

    /*
     * The section's rows, all of them, before anything is placed.
     *
     * Built once rather than asked for row by row, which is what step 9's list model requires of
     * any screen whose rows are not all one height: the window, the highlight and the scroll
     * thumb are three sums of the heights, and the model has to be handed them before it decides
     * which rows are on screen. The node detail has had this shape since that step; the settings
     * screen only needed it once a row could be two steps tall.
     *
     * It is also strictly cheaper than what it replaced. mesh_ui_settings_item() rebuilds the
     * whole section from the radio's config for every row it answers, so the loop below used to
     * build it once per visible row.
     */
    struct mesh_ui_settings_item items[MESH_UI_SETTINGS_ITEMS_MAX];
    const uint32_t count =
        section_open
            ? mesh_ui_settings_items(settings, handshake, nav->settings_edits,
                                     nav->settings_edit_count, section, nav->settings_channel,
                                     items, MESH_UI_SETTINGS_ITEMS_MAX)
            : mesh_ui_settings_root_count();
    if (count == 0U) {
        /* Which of the two empty sections this is: one a refresh may fill in, and one it never
           will. Asked of the same predicate the section list asks, so the row and the screen
           behind it cannot disagree about why it is blank. */
        fb_draw_empty(state, layout, MESH_UI_ICON_SETTINGS,
                      mesh_str(mesh_ui_settings_availability_reason(
                          mesh_ui_settings_section_availability(settings, handshake, section))));
        return;
    }

    /*
     * The leading slot is declared for the whole list or not at all - a list that indents only
     * the rows with something in it is a list the eye cannot run down - so it is decided here,
     * once, rather than per row. Two lists here are lists of *subjects*: the section list, and
     * Modules, which is one wearing a section's clothes. Everything else is settings, and a
     * setting's row already says what it is in its label column.
     */
    /* Label column: a fixed width so values line up, capped for narrow scales. */
    const size_t label_cols = fb_field_label_cols(state, layout, 0U);
    /*
     * Which card each row stands on, and whether that card is a card of verbs - measured here,
     * in one pass, before anything is placed.
     *
     * This is the node detail's arrangement arriving on the screen it was always going to be
     * wanted on. A heading opens a card and everything under it belongs to that card until the
     * next one; the heading itself stands on no card, in the break, where it becomes the card's
     * label and pays for both cards' insets without costing a row. Derived rather than declared
     * because the groups are already in the rows - a `group` field on the item would be a second
     * way of saying what MESH_UI_SETTING_HEADING says.
     *
     * A section with no headings gets no cards at all and draws exactly as it always has. That
     * is the right answer rather than a gap: a card is what says "these rows belong together",
     * and a list with one group has nothing to say it about.
     *
     * The leading slot is decided per card, which is the same rule the node detail follows and
     * for the same reason. A slot is declared for a whole list or for none of it, because a list
     * that indents only the rows carrying a symbol starts its text in two columns - and on a
     * column of cards the run of rows that rule is about is the card, not the screen. Two shapes
     * and nothing between them: a card of verbs, where every row leads with a disc, and a card
     * of settings, whose rows start at the card's own padding.
     */
    uint8_t cards[MESH_UI_SETTINGS_ITEMS_MAX];
    bool any_cards = false;
    memset(cards, FB_LIST_NO_CARD, sizeof cards);
    if (section_open && mesh_ui_settings_section_groups(items, count) >= 2U) {
        /*
         * Every row of an open section stands on a card, and the headings are what break the
         * column into them. A section with no headings at all is one card, which is the same
         * statement with one group in it - and the rows ahead of a section's first heading are
         * a group too, an unnamed one, exactly as the block at the top of a phone's settings
         * page is a card before any label appears.
         *
         * One kind of row stands on the panel instead, and the leading slot is what forces it.
         * A card of verbs indents every row past a disc and a card of settings starts at the
         * card's own padding, so a card holding both would begin its words in two columns -
         * the exact failure the slot's all-or-nothing rule exists to prevent, and one that is
         * visible the moment a section puts a press under a group of fields: LoRa's "Ham mode"
         * is three values and then the switch that applies them.
         *
         * **So a group that is not all verbs puts its verbs on the panel, under its card.**
         *
         * The alternative was to give that run a card of its own, and it cannot be done here:
         * fb_list_cards() spends a card's bottom padding *into the step the next group's
         * heading stands in*, which is where the break between two cards comes from. Two card
         * runs with no step between them have nowhere to take that break from - the second is
         * painted over the first's padding, its bottom edge and its corners - and no arithmetic
         * fixes it, because the gap has to come out of a step and a step is a row. Standing the
         * verbs on the panel spends the card's padding against a non-card step, which is exactly
         * what a heading already is.
         *
         * It reads as the better answer rather than merely the available one: a verb under a
         * group of fields is the thing that *applies* them, which is a button under a form
         * rather than one more row in it. A group that is nothing but verbs is a different
         * shape - there the card is the verbs - and it keeps its card.
         */
        uint8_t card = 0U;
        uint32_t group_start = 0U;
        bool group_all_verbs = true;
        bool group_has_rows = false;
        any_cards = true;
        for (uint32_t r = 0; r <= count; ++r) {
            const bool boundary = (r == count) || items[r].kind == MESH_UI_SETTING_HEADING;
            if (boundary) {
                /*
                 * The group that just ended, placed now that what is in it is known. A group of
                 * verbs is one card; any other group cards its settings and floats its verbs,
                 * with a fresh ordinal per run so that two runs separated by a float are two
                 * cards rather than one the painter believes the window cut in half.
                 */
                if (group_has_rows && !group_all_verbs) {
                    bool run_verbs = false;
                    bool run_open = false;
                    for (uint32_t g = group_start; g < r; ++g) {
                        if (items[g].kind == MESH_UI_SETTING_HEADING) {
                            continue;
                        }
                        const bool verb = mesh_ui_settings_item_is_verb(&items[g]);
                        if (verb) {
                            /* On the panel, and a full row rather than a break: the card above
                               closes at the step boundary instead of spending its padding into
                               this row's disc. See FB_LIST_PANEL_ROW. */
                            cards[g] = FB_LIST_PANEL_ROW;
                            run_open = false;
                            continue;
                        }
                        if (!run_open || run_verbs) {
                            card = (uint8_t)(card + 1U);
                        }
                        cards[g] = card;
                        run_verbs = verb;
                        run_open = true;
                    }
                }
                if (r == count) {
                    break;
                }
                /* The heading stands on no card, in the break between the one that ended and the
                   one it opens - which is where the column gets the only air it has, and why the
                   grouping costs no rows. See the card-list note in fb_widgets.h. */
                cards[r] = FB_LIST_NO_CARD;
                card = (uint8_t)(card + 1U);
                group_start = r + 1U;
                group_all_verbs = true;
                group_has_rows = false;
                continue;
            }
            group_has_rows = true;
            if (!mesh_ui_settings_item_is_verb(&items[r])) {
                group_all_verbs = false;
            }
            /* Provisional: right for a group that turns out to be all verbs, and rewritten
               above for one that does not. */
            cards[r] = card;
        }
    }
    /*
     * Which rows lead with a symbol, for everything that is not a verb.
     *
     * The two lists here that are lists of *subjects* rather than of settings: the section list,
     * and Modules, which is one wearing a section's clothes. Neither is grouped, so both answer
     * for the whole screen the way they always have.
     *
     * A verb does not consult this and does not need to. The split above leaves every card
     * either all verbs or no verbs, so "this row leads with a disc" and "this card leads with
     * discs" are the same statement - which is what makes the leading slot's all-or-nothing rule
     * hold per card without anything having to check it twice.
     */
    const bool rows_lead_with_icon = !section_open || mesh_ui_settings_section_icons_rows(section);
    /*
     * Whether the section has a value column at all, which is what decides where a *verb's* own
     * value is written.
     *
     * A section is settings with presses among them or it is a list of presses, and the two want
     * the value in different places. Radio actions is the second: eleven verbs, no field, nothing
     * to line a column up with, and what a row has to say goes against the trailing edge where a
     * trailing age goes - "21 nodes", the size of what the press costs, beside the eye.
     *
     * About and About radio are the first, and there the trailing edge is the wrong column. Both
     * screens are mostly read: a stack of facts with their values at the column every other
     * settings row puts one in. A verb standing among them with something to state - and the
     * rows that do are the ones whose value is a *consequence* rather than a setting, since a
     * press that merely steps its own value is not a verb at all - would otherwise come out
     * alone against the right-hand edge, leaving the screen reading its values in two columns
     * with nothing to say which row belonged to which. A row joins the column its neighbours
     * are in.
     *
     * A withdrawn verb's reason is not a value and keeps the trailing slot either way - see the
     * verb branch below.
     */
    bool section_has_field = false;
    if (section_open) {
        for (uint32_t r = 0; r < count; ++r) {
            if (items[r].kind == MESH_UI_SETTING_HEADING) {
                continue;
            }
            if (!mesh_ui_settings_item_is_verb(&items[r])) {
                section_has_field = true;
            }
        }
    }
    /*
     * What a row that draws no symbol of its own puts at its leading edge - and the rule behind
     * it: **every open section reserves the leading slot, whether or not anything fills it.**
     *
     * The slot is declared for a whole list or for none of it - the rule stated at the top of
     * this function - and this is that rule taken one level out, from the list to the tab. It
     * used to be asked per section, off whether the section held a verb, and the answer was
     * right for every screen and wrong for the set of them: Position reserves the gutter because
     * of one press below the fold and Radio UI, which is the same list of fields with no press
     * in it, does not - so walking between the two moves every word about two cells sideways for
     * a reason nothing on either screen shows. Device and User did it a third way again, by
     * having no cards at all to indent inside.
     *
     * So the width is spent unconditionally. It costs the label column the disc's gutter on the
     * sections that have no verb, which is the price of the Settings tab having one text column -
     * and the reader who would notice those cells is the reader who was noticing the jump.
     * ui_capture_every_section_starts_in_the_same_column holds it.
     *
     * Not the two lists of *subjects*, which fill their own narrower icon slot on every row and
     * are answered by `rows_lead_with_icon` above: the section list and Modules are lists of
     * things to open rather than of settings, and neither has ever mixed the two.
     */
    const struct fb_leading leading_slot = section_open
                                               ? (struct fb_leading){.kind = FB_LEADING_TONAL_SLOT}
                                               : (struct fb_leading){.kind = FB_LEADING_NONE};
    /*
     * Which rows carry a slider, measured here and handed to the model before the first row is
     * placed - the node detail's arrangement, and the rule step 9 left behind: the screen
     * measures, because the screen is the only thing that knows whether a row carries a bar, and
     * from there the model is the authority on every height.
     *
     * A field's answer does not depend on its current value, deliberately. The slider is dropped
     * for a field whose numbers name something rather than measure it, and that is a fact about
     * the field; if it depended on the value, stepping a row would change the height of the row
     * the cursor is sitting on.
     */
    uint8_t heights[MESH_UI_SETTINGS_ITEMS_MAX];
    for (uint32_t r = 0; r < count; ++r) {
        heights[r] = (section_open && settings_row_slider(&items[r], NULL)) ? 2U : 1U;
    }
    struct fb_list list = fb_list_begin_cards(layout, count, nav->cursor[MESH_UI_SCREEN_SETTINGS],
                                              heights, any_cards ? cards : NULL);
    uint32_t i;
    while (fb_list_next(&list, &i)) {
        if (section_open) {
            const struct mesh_ui_settings_item item = items[i];
            /* A heading names the group below it: dimmed, no marker, and no value column -
               the same row the node detail draws, so the two screens stay identical. */
            if (item.kind == MESH_UI_SETTING_HEADING) {
                /*
                 * The card's own symbol, from the group rather than from here, in the disc that
                 * makes this line a card *header* rather than a label floating over a panel.
                 *
                 * Optional per heading, unlike the node detail's, because a settings group's
                 * subject is not always a thing this client has a rune for - "Sent with a
                 * position" is a sentence about ten bits.
                 *
                 * It still owes the gutter, though, on exactly the terms its rows do: the
                 * leading slot is declared for a whole list or for none of it, and a heading is
                 * a row of the list. One left at the panel's own margin over rows that begin a
                 * disc further in is the two-column seam the slot exists to close, with the
                 * label of a card sitting outside the column it names - LoRa's "Advanced" and
                 * Position's "Sent with a position" are the two that showed it. So a heading
                 * with no symbol takes the empty slot, on the same terms its rows do.
                 */
                fb_list_subheader_icon(
                    state, &list, i, item.label,
                    item.icon != MESH_UI_ICON_NONE
                        ? (struct fb_leading){.kind = FB_LEADING_TONAL, .icon = item.icon}
                        : leading_slot);
                continue;
            }
            /*
             * What the row offers, in the marker gutter: the pencil on one Left and Right
             * change, and the dot on one already changed and not yet written. An action row
             * offers something else - it opens - and says so with the chevron every row that
             * opens something ends in, on the trailing edge rather than in the gutter.
             *
             * The triangle takes the gutter and the tone ahead of both, and that ordering is the
             * point rather than an accident of the chain: it is the one mark here that is about
             * the *value* - the radio will not honour this - where the other two are about what
             * the row offers and what is waiting to be written. Neither of those is worth saying
             * over it, and a gutter holds one mark.
             */
            const enum mesh_ui_icon marker = item.conflict ? MESH_UI_ICON_WARNING
                                             : item.dirty  ? MESH_UI_ICON_UNSAVED
                                             /* A is what steps this one, where a field steps on
                                                Left and Right. The gutter is where a row says
                                                how it is changed, so the two runes go in it
                                                together - and it is what keeps a row that
                                                cycles from reading as a fact now that the
                                                leading disc belongs to verbs alone. */
                                             : item.cycle                       ? MESH_UI_ICON_SWAP
                                             : item.field != MESH_UI_FIELD_NONE ? MESH_UI_ICON_EDIT
                                                                                : MESH_UI_ICON_NONE;
            /* The rows of this kind that open a list - a channel slot, a module - as against
               the ones that step a value where they stand. A chevron promises a screen. */
            const bool opens = (item.kind == MESH_UI_SETTING_ACTION) && !item.cycle;
            const enum mesh_ui_tone tone = item.conflict ? MESH_UI_TONE_WARNING
                                           : item.dirty  ? MESH_UI_TONE_STRONG
                                                         : MESH_UI_TONE_NORMAL;
            /* Empty on every list but Modules, and reserved on all of that one's rows - which
               is what the kind means, and why it is set from the list's answer rather than
               from whether this particular row filled it. */
            const struct fb_leading leading =
                rows_lead_with_icon
                    ? (struct fb_leading){.kind = FB_LEADING_ICON, .icon = item.icon}
                    : leading_slot;
            /*
             * Which of this row's two tiers recedes, asked of the model rather than of the
             * kind - see mesh_ui_settings_item_is_fact(). A reading puts the question in the
             * quiet tier and keeps the row's ink for the answer; a control does the opposite,
             * because there the label is what the reader is choosing. It is the node detail's
             * rule, and asking it here is what stopped the two screens drawing the same row two
             * ways: About radio's fourteen readings were at full strength beside a node's.
             */
            const bool fact = mesh_ui_settings_item_is_fact(&item);
            /*
             * A verb, drawn as one: the symbol in a tonal disc at the leading edge, the label
             * across the row, and nothing in a value column - which is where "press A" used to
             * be, eight times down one screen.
             *
             * That value is still on the item and still what the CLI backend draws. Here it is
             * noise twice over: the action bar three rows below already says what A does on this
             * screen, and a column of identical instructions is a column the eye has to skip to
             * reach the labels, which are the only part that differs. The chevron says the row
             * opens something - every one of these opens a confirm sheet or a screen - and the
             * disc says what it is about, which is what the words were carrying alone.
             *
             * The colour is the row's own tone, stated once. FB_LEADING_TONAL reads the family
             * back out of it for the disc, `accent_edge` takes the same answer for the bar down
             * a row that cannot be walked back, and the label keeps the ordinary ink either way -
             * because a card where every verb shouts is a card where none of them does. This is
             * word for word the node detail's action card, which is the point: there is one way
             * this client draws a verb.
             *
             * What rides the trailing edge instead of the chevron, when there is something to
             * say: the count a forget would remove, as a badge in the row's own family, because
             * "seven nodes" is the size of what the press costs and a figure the reader is meant
             * to weigh belongs where the eye already is. A withdrawn verb puts the reason there
             * as quiet words and drops the chevron altogether - a row that does nothing must not
             * claim to open anything.
             */
            if (mesh_ui_settings_item_is_verb(&item)) {
                const bool off = (item.kind == MESH_UI_SETTING_ACTION_OFF);
                const enum mesh_ui_tone verb_tone = item.conflict ? MESH_UI_TONE_WARNING
                                                    : item.dirty  ? MESH_UI_TONE_STRONG
                                                                  : item.tone;
                /*
                 * A verb with a value, in a section that has a column for one.
                 *
                 * The same row as the one below in every other respect - the disc, the ordinary
                 * ink, the chevron, the edge on a press with no way back - and only the value
                 * moves, into the column the fields around it are already writing in. `label`
                 * rather than `text` is the whole of the difference: it is what asks
                 * fb_list_item() for a label column, and the column is measured once for the
                 * screen, so the verb's value starts in the same cell a setting's does.
                 *
                 * MESH_UI_SETTING_ACTION only, and not the withdrawn one beside it: "not
                 * supported" is a reason rather than a value, it belongs against the trailing
                 * edge where the chevron it replaces was, and it is drawn quietly there - which
                 * is what says the offer is withdrawn rather than the answer being blank.
                 */
                /*
                 * Whether the press raises anything, which is what the chevron is for. It used
                 * to be spent on any verb with an empty value column, and that is a reading of
                 * the row rather than of the press: it is true of every verb that opens
                 * something and of several that do not. Asked of the model, which is where the
                 * nav's own answer lives.
                 */
                const bool verb_opens = !off && mesh_ui_settings_action_opens(
                                                    (enum mesh_ui_settings_action)item.number);
                if (section_has_field && item.kind == MESH_UI_SETTING_ACTION &&
                    item.value[0] != '\0') {
                    const struct fb_list_item value_row = {
                        .leading = {.kind = FB_LEADING_TONAL, .icon = item.icon},
                        .label = item.label,
                        .label_cols = label_cols,
                        .value = item.value,
                        .tone = verb_tone,
                        .label_plain = true,
                        .trailing = verb_opens ? (struct fb_trailing){.kind = FB_TRAILING_ICON,
                                                                      .icon = MESH_UI_ICON_CHEVRON}
                                               : (struct fb_trailing){.kind = FB_TRAILING_NONE},
                        .accent_edge = verb_tone == MESH_UI_TONE_ERROR,
                    };
                    fb_list_item(state, &list, i, &value_row);
                    continue;
                }
                /*
                 * The trailing slot says the one thing the row has left to say, and for most
                 * verbs that is "this opens something" - which every one of these does, into a
                 * confirm sheet or a screen.
                 *
                 * A verb with a value states the value instead, quietly, in the slot a trailing
                 * age takes: the count a forget would remove, the language a press would cycle
                 * to, the reason a withdrawn verb cannot be pressed. One shape for all three
                 * rather than a badge for the counts, and that is deliberate - a filled capsule
                 * is a count that *shouts*, which is right for unread messages and wrong for
                 * "English". A card where every row ends in a bubble is a column of colour
                 * reporting nothing, which is the bar fb_draw_badge() is already held to.
                 *
                 * The chevron goes when a value takes the slot. That is the honest order of the
                 * two: the reader needs the figure before the press more than they need to be
                 * told there is a question after it, and the action bar names A either way.
                 */
                const struct fb_list_item row = {
                    .leading = {.kind = FB_LEADING_TONAL, .icon = item.icon},
                    .text = item.label,
                    .tone = verb_tone,
                    .trailing =
                        item.value[0] != '\0'
                            ? (struct fb_trailing){.kind = FB_TRAILING_TEXT, .text = item.value}
                        : verb_opens ? (struct fb_trailing){.kind = FB_TRAILING_ICON,
                                                            .icon = MESH_UI_ICON_CHEVRON}
                                     : (struct fb_trailing){.kind = FB_TRAILING_NONE},
                    /* The tone goes to the disc and the edge, never to the words - which is
                       what the paragraph above claims and what this flag is what makes true.
                       Without it `tone` also inks the label, and a section where nine rows in
                       ten carry a weight draws nine tinted labels: the wall of orange this was
                       supposed to have replaced. */
                    .label_plain = true,
                    /* Only on a row whose press cannot be walked back, and only while it is one:
                       a withdrawn verb is an absent offer rather than a dangerous one, so it
                       keeps neither the red nor the bar. */
                    .accent_edge = !off && verb_tone == MESH_UI_TONE_ERROR,
                };
                fb_list_item(state, &list, i, &row);
                continue;
            }
            /*
             * A boolean gets a switch rather than the words. The words are still what the CLI
             * backend draws and still what item.value holds - this is the fb backend deciding
             * how to say the same thing on a screen, which is exactly the choice a backend is
             * for.
             *
             * The field id is the switch's identity, and it has to be one no other row in the
             * frame shares: the channel rows repeat the same fields per channel, so the
             * channel is mixed in. A read-only toggle has no field at all and is keyed on its
             * row instead, above everything the field enum can reach.
             */
            /*
             * A level gets a bar next to the words, on the same terms as a boolean getting a
             * switch instead of them: the fb backend deciding how to say what the item already
             * says. The row is keyed on its index, above everything the field enum can reach,
             * because a meter row has no field of its own - it is a fact, not a control.
             */
            if (item.kind == MESH_UI_SETTING_METER) {
                const bool unknown = item.number == MESH_UI_METER_UNKNOWN;
                struct fb_meter meter = {
                    .id = 0x03000000U | i,
                    .kind = unknown ? FB_METER_INDETERMINATE : FB_METER_DETERMINATE,
                    .value = unknown ? 0 : (int32_t)item.number,
                    .tone = MESH_UI_TONE_PRIMARY,
                };
                const struct fb_list_item row = {
                    .leading = leading,
                    .label = item.label,
                    .label_cols = label_cols,
                    .label_quiet = fact,
                    .marker_icon = marker,
                    .value = item.value,
                    .tone = tone,
                    .trailing = {.kind = FB_TRAILING_METER, .meter = &meter},
                };
                fb_list_item(state, &list, i, &row);
                continue;
            }
            /*
             * A flag gets a checkbox, and that is the one place this screen says something a
             * switch could not. The ten rows under "Sent with a position" are not ten settings
             * that each act on their own: they are the members of one word, and a square is
             * how the control set says "any of these" where the switch says "this thing is
             * on". Same edit, same value in the column, different sentence.
             *
             * Keyed like the switch, on the field with the channel mixed in, so a control
             * animating in one frame is the same control in the next; 0x06 keeps it clear of
             * the switch, the meters and the slider.
             */
            if (item.kind == MESH_UI_SETTING_FLAG) {
                struct fb_selection sel = {
                    .id = 0x06000000U | ((uint32_t)nav->settings_channel << 16) |
                          (uint32_t)item.field,
                    .on = item.number != 0U,
                    .dim = item.field == MESH_UI_FIELD_NONE,
                };
                const struct fb_list_item row = {
                    .leading = leading,
                    .label = item.label,
                    .label_cols = label_cols,
                    /* A switch or a box the radio decides and this client only reports is a
                       reading like any other, and its words recede with them - which is what
                       keeps "Screen lock" in the same tier as the "Language" above it on Radio
                       UI's second card, where all three are things set somewhere else. */
                    .label_quiet = fact,
                    .marker_icon = marker,
                    .tone = tone,
                    .trailing = {.kind = FB_TRAILING_CHECKBOX, .sel = &sel},
                };
                fb_list_item(state, &list, i, &row);
                continue;
            }
            if (item.kind == MESH_UI_SETTING_TOGGLE) {
                struct fb_switch sw = {
                    .id = item.field != MESH_UI_FIELD_NONE
                              ? 0x01000000U | ((uint32_t)nav->settings_channel << 16) |
                                    (uint32_t)item.field
                              : 0x02000000U | i,
                    .on = item.number != 0U,
                    .dim = item.field == MESH_UI_FIELD_NONE,
                };
                const struct fb_list_item row = {
                    .leading = leading,
                    .label = item.label,
                    .label_cols = label_cols,
                    .label_quiet = fact,
                    .marker_icon = marker,
                    .tone = tone,
                    .trailing = {.kind = FB_TRAILING_SWITCH, .sw = &sw},
                };
                fb_list_item(state, &list, i, &row);
                continue;
            }
            /*
             * A number on a scale gets the scale drawn under it.
             *
             * The same choice again, one kind further along: the item already says what the
             * value is - "5m", and the CLI backend draws exactly that and nothing else - and
             * this is the fb backend adding what the word cannot carry, which is where 5m falls
             * among the durations this field will accept. A row of intervals used to be a
             * column of figures that could only be compared against each other by reading all
             * of them.
             *
             * It costs the row its second step, and that is the deal §1.4 struck: a bar with the
             * row to itself is the one that can be aimed at, and the trailing slot's eight cells
             * cannot carry a dozen stops. A control the reader is about to change is exactly the
             * kind of thing that earns a step, where a figure the eye passes does not.
             *
             * Keyed on the field, with the channel mixed in for the rows the Channels section
             * repeats per slot - the switch's identity, because this is a control on a field in
             * the same way, and 0x05 keeps it clear of everything the switch and the meters use.
             */
            struct mesh_ui_settings_track track;
            if (settings_row_slider(&item, &track)) {
                struct fb_slider slider = {
                    .id = 0x05000000U | ((uint32_t)nav->settings_channel << 16) |
                          (uint32_t)item.field,
                    .position = track.position,
                    .stops = track.stops,
                    .unplaced = track.unplaced,
                    .tone = MESH_UI_TONE_PRIMARY,
                };
                const struct fb_list_item row = {
                    .leading = leading,
                    .label = item.label,
                    .label_cols = label_cols,
                    .label_quiet = fact,
                    .marker_icon = marker,
                    /* The figure stays. The track says how far along, the word says how long,
                       and neither is the other's caption - a slider with no reading is a
                       control that cannot be set to a value anybody could name. */
                    .value = item.value,
                    .tone = tone,
                    .slider = &slider,
                };
                fb_list_item(state, &list, i, &row);
                continue;
            }
            /*
             * A small set of alternatives gets the whole set rather than the one word.
             *
             * On the same terms as a boolean getting a switch: the item already says what it is
             * and what it is set to, and this is the fb backend choosing how to say it on a
             * screen. The CLI backend still draws the word, and so does this one when the value
             * column is too narrow for the segments - which is what `value` on the struct is
             * for, and why the choice between the two is the component's rather than a test
             * written out here.
             *
             * Two to four, because five equal shares of a value column are five clipped words.
             * The larger enums - thirty-eight regions, seventeen presets - are stepped exactly
             * as they were; a set nobody can take in at a glance is better read one at a time.
             */
            const uint32_t choices =
                item.kind == MESH_UI_SETTING_ENUM ? mesh_ui_settings_enum_count(item.field) : 0U;
            /* A segment nobody may pick is a segment that must not be drawn: the control says
               "one of these", so a constrained row falls back to the stepped word rather than
               offering a set it would then refuse. No row does this today - the two constrained
               ones are a region and a preset, both far wider than a segmented button - and the
               test is here so the first one that does cannot draw a lie. */
            if (item.choices == 0U && choices >= 2U && choices <= FB_SEGMENTED_MAX) {
                struct fb_segmented segmented = {
                    .count = choices,
                    .active = item.number,
                    .value = item.value,
                };
                for (uint32_t c = 0U; c < choices; ++c) {
                    segmented.labels[c] = mesh_ui_settings_enum_name(item.field, c);
                }
                const struct fb_list_item row = {
                    .leading = leading,
                    .label = item.label,
                    .label_cols = label_cols,
                    .marker_icon = marker,
                    /* No value column: the set is the value, and the word for the chosen one is
                       inside the control that decides which of the two forms to draw. */
                    .tone = tone,
                    .trailing = {.kind = FB_TRAILING_SEGMENTED, .segmented = &segmented},
                };
                fb_list_item(state, &list, i, &row);
                continue;
            }
            const struct fb_list_item row = {
                .leading = leading,
                .label = item.label,
                .label_cols = label_cols,
                .label_quiet = fact,
                .marker_icon = marker,
                .value = item.value,
                .tone = tone,
                .trailing = {.kind = FB_TRAILING_ICON,
                             .icon = opens ? MESH_UI_ICON_CHEVRON : MESH_UI_ICON_NONE},
            };
            fb_list_item(state, &list, i, &row);
        } else {
            const enum mesh_ui_settings_section section_row = mesh_ui_settings_root_at(i);
            const enum mesh_ui_settings_availability available =
                mesh_ui_settings_section_availability(settings, handshake, section_row);
            const bool loaded = available == MESH_UI_SETTINGS_SECTION_READY;
            const struct fb_list_item row = {
                /* What the section is, in the slot the eye reaches first. The one list on this
                   screen that was a column of words with nothing to aim at. */
                .leading = {.kind = FB_LEADING_ICON,
                            .icon = mesh_ui_settings_section_icon(section_row)},
                .label = mesh_ui_settings_section_name(section_row),
                .label_cols = label_cols,
                .value = loaded ? "" : mesh_str(mesh_ui_settings_availability_label(available)),
                .tone = loaded ? MESH_UI_TONE_NORMAL : MESH_UI_TONE_DIM,
                /* Every row here opens a section, which is what the section list *is*. */
                .trailing = {.kind = FB_TRAILING_ICON, .icon = MESH_UI_ICON_CHEVRON},
            };
            fb_list_item(state, &list, i, &row);
        }
    }
}
