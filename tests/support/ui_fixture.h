#ifndef MESH_TEST_SUPPORT_UI_FIXTURE_H
#define MESH_TEST_SUPPORT_UI_FIXTURE_H

/* Pre-populated UI store state, so nav tests start from a realistic screen. */

#include "mesh/ui/settings.h"
#include "mesh/ui/store.h"

#include <stdbool.h>

void mesh_test_nav_populate(struct mesh_ui_store *store);

/*
 * Walks the tab strip to `screen` with the shoulder presses a device would make.
 *
 * A test must not press Right a fixed number of times to reach a tab. Fourteen of them did, and
 * every one of those counts became wrong the day a sixth tab was added between Nodes and
 * Devices - failing with a message about Settings rather than about the tab strip. This walks
 * until it arrives, so the count lives in one place and a test says which tab it wants.
 *
 * Returns false when the tab could not be reached, which means an overlay is holding the keys.
 */
bool mesh_test_open_tab(struct mesh_ui_store *store, enum mesh_ui_screen screen);

/*
 * Walks the Settings tab to `section` and opens it, from wherever the cursor is.
 *
 * A test must not press Down a fixed number of times to reach a section: the row order is
 * mesh_ui_settings_root_at()'s, not the enum's, and a module is one level further down. This
 * finds the row, presses Down that many times, and presses A - descending through Modules
 * first when the section lives there. Returns false if the section was not reached.
 */
bool mesh_test_settings_open(struct mesh_ui_store *store, enum mesh_ui_settings_section section);

#endif /* MESH_TEST_SUPPORT_UI_FIXTURE_H */
