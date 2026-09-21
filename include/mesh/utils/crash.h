#ifndef MESH_UTILS_CRASH_H
#define MESH_UTILS_CRASH_H

/*
 * What this client is called when it faults, and which facts its report carries.
 *
 * The reporter itself is inkwell's - inkwell/runtime/crash.h has the handler, the report
 * writer, the log ring and the discipline all of it follows. None of that was ever about
 * Meshtastic. What is here is the identity inkwell cannot know: the product's name, where its
 * issues go, the three notes this client keeps, and the sentence describing what *this
 * client's* log can contain.
 *
 * That last one is the reason this file exists rather than the app passing a config inline. The
 * report's warning is read by somebody deciding whether to attach the file to a public issue,
 * and it has been wrong before: it used to claim the file held "no message text, no node names,
 * no coordinates", while the log tail below it quoted sent messages, named channels and
 * waypoints, and printed a hand-entered position as the two numbers that were typed. So the
 * warning lives beside this client's log rather than inside a layer that has never seen it, and
 * it is specific enough to act on. Channel keys really are never logged: the one line that
 * mentions a passkey prints "held" or "absent" rather than the value.
 *
 * Everything else a caller wants is inkwell's directly - inkwell_crash_note(),
 * inkwell_crash_report_waiting(), inkwell_crash_discard(), inkwell_crash_log_line().
 */

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The facts the report carries about where the client was standing, as slot indices into
 * inkwell's notes.
 *
 * Each is set from ordinary context whenever the client's own state changes, and the handler
 * only ever writes out what it finds.
 */
enum mesh_crash_note_slot {
    /* Which build this is, so a report names the binary it came from. */
    MESH_CRASH_NOTE_VERSION = 0,
    /* Where the UI was: the route, as `mesh_ui_route_describe()` spells it. The single most
       useful line in the file, because it turns "it crashed" into "it crashed on the map". */
    MESH_CRASH_NOTE_ROUTE,
    /* What the link was doing - the transport's own status string. The other half of the same
       question, since most of what this client does at all is driven by a radio. */
    MESH_CRASH_NOTE_TRANSPORT,
    MESH_CRASH_NOTE_SLOT_COUNT,
};

/*
 * Install inkwell's crash handler under this client's name, writing into `dir`.
 *
 * Returns whatever inkwell_crash_install() returns; see it for what a second call does and for
 * why the report a previous run left is read here rather than on demand.
 */
int mesh_crash_install(const char *dir);

#ifdef __cplusplus
}
#endif

#endif /* MESH_UTILS_CRASH_H */
