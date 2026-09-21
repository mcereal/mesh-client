#define _POSIX_C_SOURCE 200809L

/*
 * The forked resolver: a literal answered without a child, a name answered through one, and the
 * two ways a lookup ends badly told apart.
 *
 * **No case here asserts that a particular name resolves.** Whether this machine has DNS is not
 * a fact about this code - CI runs in a container, the Brick runs on somebody's WiFi - and a
 * test that demanded an answer would be reporting on the network. What is asserted instead is
 * the part that is always true: a started lookup reports exactly once, it reports through the
 * loop and never from start(), an OK answer carries a usable address, and an answer that is not
 * OK carries none.
 *
 * Where a case does need a working resolver - telling "no such name" apart from "the lookup did
 * not work" is meaningless without one - it establishes that first, in the same case, and holds
 * the resolver to the distinction only once a nameserver has demonstrably answered. That gate is
 * a precondition the case checks rather than an assumption about CI, and getting the *name* in
 * it wrong is how the case used to report on the network anyway: see
 * resolve_reports_an_unknown_name().
 */

#include "framework/mesh_test.h"

#include "mesh/core/event_loop.h"
#include "mesh/core/resolve.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

struct resolve_probe {
    unsigned calls;
    enum mesh_resolve_outcome outcome;
    struct sockaddr_storage address;
    socklen_t address_len;
};

static void probe_record(void *userdata, const struct mesh_resolve_result *result) {
    struct resolve_probe *probe = (struct resolve_probe *)userdata;
    probe->calls++;
    probe->outcome = result->outcome;
    probe->address = result->address;
    probe->address_len = result->address_len;
}

/*
 * Turns the loop until the lookup has reported, or the budget runs out.
 *
 * Both halves of the contract are driven here on purpose: the loop delivers the child's answer
 * and the tick reaps it, and a caller that ran only one of them would hang. That is the same
 * pairing the TCP transport does every turn.
 */
static bool pump_until_done(struct mesh_event_loop *loop, struct mesh_resolve *resolve,
                            struct resolve_probe *probe) {
    for (unsigned turn = 0U; turn < 200U && probe->calls == 0U; ++turn) {
        (void)mesh_event_loop_run(loop, 50);
        mesh_resolve_tick(resolve, (uint64_t)turn * 50U);
    }
    return probe->calls > 0U;
}

/* ---- literals ------------------------------------------------------------------------- */

MESH_TEST_CASE(resolve_literal_needs_no_child, unit) {
    struct sockaddr_storage address;
    socklen_t address_len = 0;

    if (!mesh_resolve_literal("192.168.1.50", 4403U, &address, &address_len)) {
        record_failure(test_name, "a v4 literal should be recognised");
        return;
    }
    const struct sockaddr_in *v4 = (const struct sockaddr_in *)&address;
    if (v4->sin_family != AF_INET || ntohs(v4->sin_port) != 4403U ||
        address_len != (socklen_t)sizeof *v4) {
        record_failure(test_name, "a v4 literal should carry the port it was given");
        return;
    }

    if (!mesh_resolve_literal("fd00::1", 4404U, &address, &address_len)) {
        record_failure(test_name, "a v6 literal should be recognised");
        return;
    }
    const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)&address;
    if (v6->sin6_family != AF_INET6 || ntohs(v6->sin6_port) != 4404U ||
        address_len != (socklen_t)sizeof *v6) {
        record_failure(test_name, "a v6 literal should carry the port it was given");
        return;
    }

    /* A name is not an error here - it is the caller's cue to start a lookup. */
    if (mesh_resolve_literal("meshtastic.local", 4403U, &address, &address_len)) {
        record_failure(test_name, "a name should not pass for a literal");
        return;
    }
    /* And neither is a near-miss. Trailing rubbish after an address is not an address. */
    if (mesh_resolve_literal("192.168.1.50x", 4403U, &address, &address_len)) {
        record_failure(test_name, "a malformed address should not pass for a literal");
        return;
    }
    record_success(test_name);
}

/* ---- lookups -------------------------------------------------------------------------- */

MESH_TEST_CASE(resolve_finds_a_name, unit) {
    struct mesh_event_loop loop;
    if (mesh_event_loop_init(&loop) != 0) {
        record_failure(test_name, "the loop did not start");
        return;
    }
    struct mesh_resolve resolve;
    (void)mesh_resolve_init(&resolve, &loop);

    struct resolve_probe probe;
    memset(&probe, 0, sizeof probe);

    if (mesh_resolve_start(&resolve, "localhost", 4403U, probe_record, &probe, 0U) != 0) {
        record_failure(test_name, "the lookup did not start");
        goto cleanup;
    }
    if (!mesh_resolve_busy(&resolve)) {
        record_failure(test_name, "a started lookup should be busy");
        goto cleanup;
    }
    /* The callback must not have run yet: start() forks, and forking is not answering. */
    if (probe.calls != 0U) {
        record_failure(test_name, "start() reported before it returned");
        goto cleanup;
    }

    if (!pump_until_done(&loop, &resolve, &probe)) {
        record_failure(test_name, "the lookup never reported");
        goto cleanup;
    }
    if (probe.calls != 1U) {
        record_failure(test_name, "a lookup should report exactly once");
        goto cleanup;
    }
    if (mesh_resolve_busy(&resolve)) {
        record_failure(test_name, "a finished lookup should be idle");
        goto cleanup;
    }

    if (probe.outcome != MESH_RESOLVE_OK) {
        /*
         * No resolver on this machine, which is not this code's failure - but it still may not
         * hand back a half-answer for the caller to connect to.
         */
        if (probe.address_len != 0U) {
            record_failure(test_name, "a failed lookup must carry no address");
            goto cleanup;
        }
        record_success(test_name);
        goto cleanup;
    }

    if (probe.address_len == 0U) {
        record_failure(test_name, "a resolved name should carry an address");
        goto cleanup;
    }
    /* The port goes in via getaddrinfo's service argument, so this is what proves the answer is
       usable as-is rather than needing the caller to poke at sin_port. */
    uint16_t port = 0U;
    if (probe.address.ss_family == AF_INET) {
        port = ntohs(((const struct sockaddr_in *)&probe.address)->sin_port);
    } else if (probe.address.ss_family == AF_INET6) {
        port = ntohs(((const struct sockaddr_in6 *)&probe.address)->sin6_port);
    } else {
        record_failure(test_name, "a resolved name should be v4 or v6");
        goto cleanup;
    }
    if (port != 4403U) {
        record_failure(test_name, "the port should already be in the address");
        goto cleanup;
    }
    record_success(test_name);

cleanup:
    mesh_resolve_shutdown(&resolve);
    mesh_event_loop_shutdown(&loop);
}

/*
 * A name that cannot exist is NOT_FOUND, not FAILED.
 *
 * The distinction is the whole reason the outcomes are not one error: the TCP link says "no such
 * host" for this and "could not look up the name" for the other, and those ask the person
 * holding the device to do different things.
 *
 * Asking for NOT_FOUND only means anything where something is able to say no, so this gates the
 * strong assertion on a lookup that proves a nameserver answered. On a machine with no resolver
 * the case checks the weaker thing that is still true - a name that does not resolve yields no
 * address.
 *
 * **The gate has to prove a *no*, not a *yes*.** `localhost` was the first attempt and was
 * wrong twice over: nsswitch answers it out of /etc/hosts (`hosts: files dns`) before it ever
 * reaches a nameserver, so it succeeded on a machine with no DNS at all - and the case then
 * demanded NOT_FOUND from a lookup that can only answer FAILED, because an unreachable resolver
 * is EAI_AGAIN rather than EAI_NONAME. Run the suite under `unshare -rn` and that is exactly
 * what happened, which is the whole class of failure the docstring at the top of this file says
 * it is avoiding.
 *
 * A name that *does* resolve is a better gate but still the wrong shape, because it only proves
 * a yes. Pin one in /etc/hosts, or leave it warm in a cache whose upstream has gone, and the
 * gate opens again on a machine whose resolver cannot answer anything.
 *
 * So the gate is a name that has to cross the network and must not be there when it arrives.
 * Only a nameserver that is genuinely reachable can say NXDOMAIN about it: /etc/hosts has no
 * wildcards, and nothing caches a name it has never been asked. The label carries the pid so
 * that no pinned entry can anticipate it, and it sits under a real delegated TLD rather than
 * one of the RFC 2606 names - `example.com` and friends answer NODATA rather than NXDOMAIN, and
 * a reserved TLD like `.invalid` is the one a stub resolver is allowed to answer by itself,
 * which is the `localhost` mistake over again. Saying no to this lookup is the exact capability
 * the assertion below depends on:
 *
 *     a reachable nameserver    -> NXDOMAIN   -> NOT_FOUND -> the gate opens
 *     no resolver at all        -> EAI_AGAIN  -> FAILED    -> the gate stays shut
 *     one that hijacks NXDOMAIN -> an address -> OK        -> the gate stays shut
 *
 * It is a *gate*, never an assertion: every way it can go wrong leaves it shut, and a shut gate
 * fails nothing. No case in this file reports on the network.
 */
MESH_TEST_CASE(resolve_reports_an_unknown_name, unit) {
    struct mesh_event_loop loop;
    if (mesh_event_loop_init(&loop) != 0) {
        record_failure(test_name, "the loop did not start");
        return;
    }
    struct mesh_resolve resolve;
    (void)mesh_resolve_init(&resolve, &loop);

    /* Per-run, so that neither a cache nor a pinned /etc/hosts line can answer it. */
    char gate[64];
    (void)snprintf(gate, sizeof gate, "meshclient-no-such-name-%ld.com", (long)getpid());

    struct resolve_probe working;
    memset(&working, 0, sizeof working);
    if (mesh_resolve_start(&resolve, gate, 80U, probe_record, &working, 0U) != 0 ||
        !pump_until_done(&loop, &resolve, &working)) {
        record_failure(test_name, "the precondition lookup never reported");
        goto cleanup;
    }
    /* A *no* about a name that had to come from DNS; see the note above for why not a yes. */
    const bool dns_answers = working.outcome == MESH_RESOLVE_NOT_FOUND;

    struct resolve_probe probe;
    memset(&probe, 0, sizeof probe);

    /* `.invalid` is reserved by RFC 6761 precisely so that it never resolves anywhere. */
    if (mesh_resolve_start(&resolve, "meshclient-nothing-here.invalid", 4403U, probe_record, &probe,
                           0U) != 0) {
        record_failure(test_name, "the lookup did not start");
        goto cleanup;
    }
    if (!pump_until_done(&loop, &resolve, &probe)) {
        record_failure(test_name, "the lookup never reported");
        goto cleanup;
    }
    if (probe.outcome == MESH_RESOLVE_OK) {
        record_failure(test_name, "a reserved name should not resolve");
        goto cleanup;
    }
    if (probe.address_len != 0U) {
        record_failure(test_name, "a name that did not resolve must carry no address");
        goto cleanup;
    }
    if (dns_answers && probe.outcome != MESH_RESOLVE_NOT_FOUND) {
        record_failure(test_name, "a resolver that works should call an unknown name NOT_FOUND");
        goto cleanup;
    }
    record_success(test_name);

cleanup:
    mesh_resolve_shutdown(&resolve);
    mesh_event_loop_shutdown(&loop);
}

/* ---- the lifecycle -------------------------------------------------------------------- */

MESH_TEST_CASE(resolve_refuses_a_second_lookup, unit) {
    struct mesh_event_loop loop;
    if (mesh_event_loop_init(&loop) != 0) {
        record_failure(test_name, "the loop did not start");
        return;
    }
    struct mesh_resolve resolve;
    (void)mesh_resolve_init(&resolve, &loop);

    struct resolve_probe probe;
    memset(&probe, 0, sizeof probe);

    if (mesh_resolve_start(&resolve, "localhost", 4403U, probe_record, &probe, 0U) != 0) {
        record_failure(test_name, "the lookup did not start");
        goto cleanup;
    }
    if (mesh_resolve_start(&resolve, "localhost", 4403U, probe_record, &probe, 0U) != -EBUSY) {
        record_failure(test_name, "a second lookup should be refused");
        goto cleanup;
    }

    /*
     * A cancel takes the child with it and says nothing. That is what a link being dropped mid
     * lookup depends on: a completion arriving after the decision would open a socket to a
     * target nobody is connecting to any more.
     */
    mesh_resolve_cancel(&resolve);
    if (mesh_resolve_busy(&resolve)) {
        record_failure(test_name, "a cancelled lookup should be idle");
        goto cleanup;
    }
    for (unsigned turn = 0U; turn < 20U; ++turn) {
        (void)mesh_event_loop_run(&loop, 10);
        mesh_resolve_tick(&resolve, (uint64_t)turn * 10U);
    }
    if (probe.calls != 0U) {
        record_failure(test_name, "a cancelled lookup must not report");
        goto cleanup;
    }
    record_success(test_name);

cleanup:
    mesh_resolve_shutdown(&resolve);
    mesh_event_loop_shutdown(&loop);
}

MESH_TEST_CASE(resolve_without_a_loop_is_unavailable, unit) {
    struct mesh_resolve resolve;
    (void)mesh_resolve_init(&resolve, NULL);

    if (mesh_resolve_available(&resolve)) {
        record_failure(test_name, "a resolver with no loop should be unavailable");
        return;
    }

    struct resolve_probe probe;
    memset(&probe, 0, sizeof probe);
    if (mesh_resolve_start(&resolve, "localhost", 4403U, probe_record, &probe, 0U) != -ENOTSUP) {
        record_failure(test_name, "a lookup with no loop should be refused");
        return;
    }
    /* And the refusals that come before that, none of which may fork anything. */
    if (mesh_resolve_start(&resolve, "", 4403U, probe_record, &probe, 0U) != -EINVAL ||
        mesh_resolve_start(&resolve, "localhost", 4403U, NULL, &probe, 0U) != -EINVAL) {
        record_failure(test_name, "an empty name and a missing callback should be refused");
        return;
    }
    if (probe.calls != 0U) {
        record_failure(test_name, "a refused lookup must not report");
        return;
    }

    /* Shutdown on a resolver that never ran anything is a no-op, not a reap of pid 0. */
    mesh_resolve_shutdown(&resolve);
    record_success(test_name);
}
