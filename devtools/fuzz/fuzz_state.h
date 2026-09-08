#ifndef MESH_FUZZ_STATE_H
#define MESH_FUZZ_STATE_H

/*
 * The ids the session harness pins and the seed generator quotes back.
 *
 * Two of the session's decoders only run inside a conversation the client started, and check an
 * id to prove it: a `config_complete_id` is acted on only while a want_config is in flight and
 * only when it echoes that request, and a TRACEROUTE_APP payload is decoded only while a trace
 * is pending and only when `Data.request_id` echoes the request we sent. A harness that starts
 * from a freshly initialised session therefore cannot reach either - both return early, and the
 * nested RouteDiscovery decode behind the second one is never entered at all.
 *
 * So the harness puts the session into exactly the state those replies would arrive in, and the
 * ids are pinned rather than generated because the corpus has to be able to name them: an id
 * drawn from the session's own counter is one a fixed seed can never match.
 */

#define MESH_FUZZ_CONFIG_REQUEST_ID 0x4E4F4E43U
#define MESH_FUZZ_TRACEROUTE_REQUEST_ID 0x7ACE0001U
#define MESH_FUZZ_TRACEROUTE_TARGET 0x336699AAU

#endif /* MESH_FUZZ_STATE_H */
