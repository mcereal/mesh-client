#ifndef MESH_TEST_SUPPORT_UI_FIXTURE_H
#define MESH_TEST_SUPPORT_UI_FIXTURE_H

/* Pre-populated UI store state, so nav tests start from a realistic screen. */

#include "mesh/ui/store.h"

void mesh_test_nav_populate(struct mesh_ui_store *store);

#endif /* MESH_TEST_SUPPORT_UI_FIXTURE_H */
