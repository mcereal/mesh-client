#define _POSIX_C_SOURCE 200809L

/*
 * This client's name on inkwell's crash report.
 *
 * The reporter is not this suite's subject any more - inkwell's runtime_crash suite holds the
 * handler, the forked faults, the log ring, the discard and the re-aiming install. What is left
 * here is the seam: that mesh_crash_install() hands inkwell an identity that is actually this
 * client's, and that the three note slots this client declares line up with the labels it gave
 * them.
 *
 * The warning is the assertion worth keeping. It is what a user reads before deciding whether
 * to attach the file to a public issue, and the first version of it claimed the file held "no
 * message text, no node names, no coordinates" while the log tail below quoted sent messages
 * and named places. So this checks that the file *warns* and deliberately not that it
 * reassures - a test pinning reassuring wording would have locked the wrong half in.
 */

#include "framework/mesh_test.h"
#include "support/fs_fixture.h"

#include "inkwell/runtime/crash.h"
#include "mesh/utils/crash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

MESH_TEST_CASE(crash_report_carries_this_clients_identity, unit) {
    char dir[] = "/tmp/mesh_crash_identityXXXXXX";
    MESH_TEST_FAIL_IF(mkdtemp(dir) == NULL, "mkdtemp failed");
    MESH_TEST_FAIL_IF_CLEANUP(mesh_crash_install(dir) != 0, mesh_test_remove_tree(dir),
                              "the client's own config was refused");

    inkwell_crash_note(MESH_CRASH_NOTE_VERSION, "9.9.9-test");
    inkwell_crash_note(MESH_CRASH_NOTE_ROUTE, "nodes/map");
    inkwell_crash_note(MESH_CRASH_NOTE_TRANSPORT, "connected: Test Radio");

    char path[256];
    snprintf(path, sizeof path, "%s/report.txt", dir);
    FILE *file = fopen(path, "we");
    MESH_TEST_FAIL_IF_CLEANUP(file == NULL, mesh_test_remove_tree(dir), "could not open a report");
    inkwell_crash_write_report(fileno(file), 11);
    (void)fclose(file);

    static char body[16384];
    FILE *back = fopen(path, "re");
    MESH_TEST_FAIL_IF_CLEANUP(back == NULL, mesh_test_remove_tree(dir), "could not read it back");
    const size_t read = fread(body, 1U, sizeof body - 1U, back);
    body[read] = '\0';
    (void)fclose(back);
    mesh_test_remove_tree(dir);

    MESH_TEST_FAIL_IF(strstr(body, "MeshClient crash report") == NULL,
                      "the report does not name this client");
    MESH_TEST_FAIL_IF(strstr(body, "addr2line -fpe meshclient") == NULL,
                      "the report does not name the binary an address resolves against");
    MESH_TEST_FAIL_IF(strstr(body, "github.com/mcereal/mesh-client/issues") == NULL,
                      "the report does not say where to send it");

    /* Each note under the label this client gave its slot, which is the only thing the
       enum above and the table in src/utils/crash.c have to agree about. */
    MESH_TEST_FAIL_IF(strstr(body, "version      9.9.9-test") == NULL, "the version note");
    MESH_TEST_FAIL_IF(strstr(body, "route        nodes/map") == NULL, "the route note");
    MESH_TEST_FAIL_IF(strstr(body, "transport    connected: Test Radio") == NULL,
                      "the transport note");

    /* And the warning that is this client's to write, because inkwell has never seen its log. */
    MESH_TEST_FAIL_IF(strstr(body, "Please read it before attaching it") == NULL,
                      "the report no longer asks to be read before it is shared");
    MESH_TEST_FAIL_IF(strstr(body, "quote a message you sent") == NULL,
                      "the report no longer warns what its log tail can contain");
    record_success(test_name);
}
