#ifndef MESH_TEST_SUPPORT_BACKUP_FIXTURE_H
#define MESH_TEST_SUPPORT_BACKUP_FIXTURE_H

/*
 * A Meshtastic radio as the want_config handshake leaves it, for the cases that back one up.
 *
 * Every Config section, the owner, the metadata, all eight channel slots, one module and the
 * canned messages, with values a round trip can be checked against: an EU_868 MEDIUM_FAST LoRa
 * config at hop limit 5 and 17 dBm, a ROUTER role, "Ridge relay", a secondary channel "Ops" in
 * slot 2 with a sixteen-byte key of 0x42, telemetry every 1800 s, and a SecurityConfig holding a
 * private key of 0xAB bytes that no backup may carry. The node is 0x0badcafe and counts 57.
 *
 * Shared because two suites need the same radio: the capture's (radio_backup.c) and the app's,
 * which drives the triggers through a real struct mesh_app.
 */

#include "mesh/core/radio_settings.h"
#include "mesh/core/session.h"

void mesh_test_backup_radio(struct mesh_radio_settings *settings,
                            struct mesh_handshake_status *status);

#endif /* MESH_TEST_SUPPORT_BACKUP_FIXTURE_H */
