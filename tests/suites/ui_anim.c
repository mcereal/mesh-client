#define _POSIX_C_SOURCE 200809L

/*
 * The animation core: the curves, the value in flight, and the table that keys one per control.
 *
 * All of it is arithmetic over a clock the test supplies, which is the whole reason it is a
 * module of its own rather than a few lines inside the switch widget - a slide is testable
 * here, frame by frame and to the permille, with no framebuffer anywhere near it.
 */

#include "framework/mesh_test.h"

#include "mesh/ui/anim.h"

MESH_TEST_CASE(anim_easings_pin_both_ends, unit) {
    for (int ease = 0; ease < MESH_UI_EASE_COUNT; ++ease) {
        MESH_TEST_FAIL_IF(mesh_ui_ease((enum mesh_ui_ease)ease, 0) != 0,
                          "every curve should start at 0");
        MESH_TEST_FAIL_IF(mesh_ui_ease((enum mesh_ui_ease)ease, MESH_UI_ANIM_ONE) !=
                              MESH_UI_ANIM_ONE,
                          "every curve should land exactly on its target");
        /* Out of range is clamped rather than extrapolated: a clock that jumped must not put a
           knob outside its track. */
        MESH_TEST_FAIL_IF(mesh_ui_ease((enum mesh_ui_ease)ease, -500) != 0,
                          "a negative progress should clamp to 0");
        MESH_TEST_FAIL_IF(mesh_ui_ease((enum mesh_ui_ease)ease, 5 * MESH_UI_ANIM_ONE) !=
                              MESH_UI_ANIM_ONE,
                          "an overshooting progress should clamp to the end");
    }

    /* Ease-out covers most of the distance early - that is what "out" means, and it is what
       makes a control feel like it answered the press rather than thought about it. */
    const int32_t linear = mesh_ui_ease(MESH_UI_EASE_LINEAR, MESH_UI_ANIM_ONE / 4);
    const int32_t out = mesh_ui_ease(MESH_UI_EASE_OUT, MESH_UI_ANIM_ONE / 4);
    MESH_TEST_FAIL_IF(out <= linear, "ease-out should be ahead of linear a quarter of the way in");

    /* Ease-in-out is symmetric about the middle, and passes through it. */
    MESH_TEST_FAIL_IF(mesh_ui_ease(MESH_UI_EASE_IN_OUT, MESH_UI_ANIM_ONE / 2) !=
                          MESH_UI_ANIM_ONE / 2,
                      "ease-in-out should cross the halfway point halfway through");
    const int32_t early = mesh_ui_ease(MESH_UI_EASE_IN_OUT, MESH_UI_ANIM_ONE / 5);
    const int32_t late = mesh_ui_ease(MESH_UI_EASE_IN_OUT, 4 * MESH_UI_ANIM_ONE / 5);
    MESH_TEST_FAIL_IF(early + late != MESH_UI_ANIM_ONE, "ease-in-out should be symmetric");
    record_success(test_name);
}

MESH_TEST_CASE(anim_value_walks_from_start_to_target, unit) {
    struct mesh_ui_anim anim;
    mesh_ui_anim_set(&anim, 0);
    MESH_TEST_FAIL_IF(mesh_ui_anim_value(&anim, 1000U) != 0, "a set value should sit where it is");
    MESH_TEST_FAIL_IF(mesh_ui_anim_active(&anim, 1000U), "nothing set should be in flight");

    mesh_ui_anim_to(&anim, 1000U, MESH_UI_ANIM_ONE, 200U, MESH_UI_EASE_LINEAR);
    MESH_TEST_FAIL_IF(!mesh_ui_anim_active(&anim, 1000U), "it should be in flight at the start");
    MESH_TEST_FAIL_IF(mesh_ui_anim_value(&anim, 1000U) != 0, "it should start where it was");
    MESH_TEST_FAIL_IF(mesh_ui_anim_value(&anim, 1100U) != MESH_UI_ANIM_ONE / 2,
                      "linear should be halfway at half the duration");
    MESH_TEST_FAIL_IF(mesh_ui_anim_value(&anim, 1200U) != MESH_UI_ANIM_ONE,
                      "it should land exactly on its target");
    MESH_TEST_FAIL_IF(mesh_ui_anim_active(&anim, 1200U),
                      "it should be settled the moment the window closes");

    /* A clock that jumped a long way - a device that slept, a capture stepping time - lands on
       the target rather than running off past it. */
    MESH_TEST_FAIL_IF(mesh_ui_anim_value(&anim, 900000U) != MESH_UI_ANIM_ONE,
                      "a far-future clock should still read the target");
    record_success(test_name);
}

/*
 * The case that is easy to get wrong and obvious once it is wrong: flicking a switch twice in
 * quick succession. The knob has to turn round from where it actually is, not from the end it
 * originally left.
 */
MESH_TEST_CASE(anim_reversal_starts_from_where_it_got_to, unit) {
    struct mesh_ui_anim anim;
    mesh_ui_anim_set(&anim, 0);
    mesh_ui_anim_to(&anim, 1000U, MESH_UI_ANIM_ONE, 200U, MESH_UI_EASE_LINEAR);

    const int32_t midway = mesh_ui_anim_value(&anim, 1100U);
    MESH_TEST_FAIL_IF(midway != MESH_UI_ANIM_ONE / 2, "half a duration in should be half way");

    mesh_ui_anim_to(&anim, 1100U, 0, 200U, MESH_UI_EASE_LINEAR);
    MESH_TEST_FAIL_IF(mesh_ui_anim_value(&anim, 1100U) != midway,
                      "a reversal should begin from where the knob actually is");
    MESH_TEST_FAIL_IF(mesh_ui_anim_value(&anim, 1300U) != 0,
                      "and should still reach the new target");

    /* Re-aiming at the target it is already heading for changes nothing, so a widget may call
       this every frame with the state it can see. */
    mesh_ui_anim_to(&anim, 1000U, MESH_UI_ANIM_ONE, 200U, MESH_UI_EASE_LINEAR);
    const struct mesh_ui_anim before = anim;
    mesh_ui_anim_to(&anim, 1050U, MESH_UI_ANIM_ONE, 200U, MESH_UI_EASE_LINEAR);
    MESH_TEST_FAIL_IF(anim.start_ms != before.start_ms || anim.from != before.from,
                      "re-aiming at the current target should be a no-op");
    record_success(test_name);
}

/*
 * First sight of an id adopts the value; a change after that animates. This is what stops every
 * switch on a Settings section sliding in from off on the frame the screen opens.
 */
MESH_TEST_CASE(anim_table_adopts_then_animates, unit) {
    struct mesh_ui_anim_table table;
    mesh_ui_anim_table_reset(&table);

    const int32_t first =
        mesh_ui_anim_track(&table, 7U, 1000U, MESH_UI_ANIM_ONE, 200U, MESH_UI_EASE_LINEAR);
    MESH_TEST_FAIL_IF(first != MESH_UI_ANIM_ONE, "a control should be drawn at its value on sight");
    MESH_TEST_FAIL_IF(mesh_ui_anim_table_active(&table, 1000U),
                      "a first paint should not leave anything moving");

    const int32_t flipped = mesh_ui_anim_track(&table, 7U, 1000U, 0, 200U, MESH_UI_EASE_LINEAR);
    MESH_TEST_FAIL_IF(flipped != MESH_UI_ANIM_ONE, "a change should start from the old value");
    MESH_TEST_FAIL_IF(!mesh_ui_anim_table_active(&table, 1000U),
                      "a change should ask for more frames");
    MESH_TEST_FAIL_IF(mesh_ui_anim_track(&table, 7U, 1100U, 0, 200U, MESH_UI_EASE_LINEAR) !=
                          MESH_UI_ANIM_ONE / 2,
                      "the table should carry the animation forward across frames");
    MESH_TEST_FAIL_IF(mesh_ui_anim_track(&table, 7U, 1200U, 0, 200U, MESH_UI_EASE_LINEAR) != 0,
                      "and should land on the target");
    MESH_TEST_FAIL_IF(mesh_ui_anim_table_active(&table, 1200U),
                      "a settled table should stop asking for frames");

    /* Two ids are two controls: one moving must not report the other's position. */
    (void)mesh_ui_anim_track(&table, 8U, 1200U, MESH_UI_ANIM_ONE, 200U, MESH_UI_EASE_LINEAR);
    (void)mesh_ui_anim_track(&table, 8U, 1200U, 0, 200U, MESH_UI_EASE_LINEAR);
    MESH_TEST_FAIL_IF(mesh_ui_anim_track(&table, 7U, 1300U, 0, 200U, MESH_UI_EASE_LINEAR) != 0,
                      "one control moving should not disturb another");
    record_success(test_name);
}

/* An id of 0 means "nothing to key on": it draws correctly and never animates, rather than
   every anonymous control sharing one slot and dragging each other about. */
MESH_TEST_CASE(anim_table_ignores_the_null_id, unit) {
    struct mesh_ui_anim_table table;
    mesh_ui_anim_table_reset(&table);

    MESH_TEST_FAIL_IF(mesh_ui_anim_track(&table, 0U, 1000U, MESH_UI_ANIM_ONE, 200U,
                                         MESH_UI_EASE_OUT) != MESH_UI_ANIM_ONE,
                      "an unkeyed control should draw at its value");
    MESH_TEST_FAIL_IF(mesh_ui_anim_track(&table, 0U, 1000U, 0, 200U, MESH_UI_EASE_OUT) != 0,
                      "an unkeyed control should follow its value exactly");
    MESH_TEST_FAIL_IF(mesh_ui_anim_table_active(&table, 1000U),
                      "an unkeyed control should never ask for a frame");
    record_success(test_name);
}

/*
 * More controls than slots. The one that has been off screen longest gives up its slot, which
 * costs it the memory of a slide nobody was watching and nothing else - it re-seeds at its
 * current value the next time it is drawn.
 */
MESH_TEST_CASE(anim_table_evicts_the_least_recently_drawn, unit) {
    struct mesh_ui_anim_table table;
    mesh_ui_anim_table_reset(&table);

    uint64_t now = 1000U;
    for (uint32_t id = 1U; id <= MESH_UI_ANIM_SLOTS; ++id, now += 10U) {
        (void)mesh_ui_anim_track(&table, id, now, 0, 200U, MESH_UI_EASE_LINEAR);
    }

    /* Keep every slot but the first one warm, then ask for one more control than fits. */
    for (uint32_t id = 2U; id <= MESH_UI_ANIM_SLOTS; ++id) {
        (void)mesh_ui_anim_track(&table, id, now, 0, 200U, MESH_UI_EASE_LINEAR);
    }
    now += 10U;
    MESH_TEST_FAIL_IF(mesh_ui_anim_track(&table, 999U, now, MESH_UI_ANIM_ONE, 200U,
                                         MESH_UI_EASE_LINEAR) != MESH_UI_ANIM_ONE,
                      "a control taking a reused slot should adopt its value, not slide to it");

    /* The evicted one is still drawable; it has simply forgotten where it was. */
    MESH_TEST_FAIL_IF(mesh_ui_anim_track(&table, 1U, now, MESH_UI_ANIM_ONE, 200U,
                                         MESH_UI_EASE_LINEAR) != MESH_UI_ANIM_ONE,
                      "an evicted control should re-seed rather than misbehave");
    record_success(test_name);
}

/*
 * The loop: a value with no destination, which is what an indeterminate progress bar is made of.
 *
 * The two properties that matter are that it is a pure function of the clock - so a missed
 * frame costs nothing and a capture stepping time lands exactly where the arithmetic says - and
 * that it starts from its own beginning rather than from wherever the monotonic clock happened
 * to be when the widget appeared.
 */
MESH_TEST_CASE(anim_loop_runs_a_sawtooth_off_the_clock, unit) {
    struct mesh_ui_anim_table table;
    mesh_ui_anim_table_reset(&table);

    /* Whatever the clock reads on first sight, the loop is at its start. */
    MESH_TEST_FAIL_IF(mesh_ui_anim_loop(&table, 1U, 987654U, 1000U) != 0,
                      "a loop should begin at 0 whenever it is first drawn");
    MESH_TEST_FAIL_IF(mesh_ui_anim_loop(&table, 1U, 987654U + 250U, 1000U) != MESH_UI_ANIM_ONE / 4,
                      "a quarter of the period in should be a quarter of the way along");
    MESH_TEST_FAIL_IF(mesh_ui_anim_loop(&table, 1U, 987654U + 750U, 1000U) !=
                          3 * MESH_UI_ANIM_ONE / 4,
                      "three quarters in should be three quarters along");

    /* It wraps rather than stopping, and a clock that jumped a whole period lands where a clock
       that walked there would have. */
    MESH_TEST_FAIL_IF(mesh_ui_anim_loop(&table, 1U, 987654U + 1000U, 1000U) != 0,
                      "a full period should be back at the start");
    MESH_TEST_FAIL_IF(mesh_ui_anim_loop(&table, 1U, 987654U + 7250U, 1000U) != MESH_UI_ANIM_ONE / 4,
                      "seven periods later should be exactly where one period later was");

    /* A loop is never "finished", so what keeps the repaint timer coming is that something is
       still drawing it - and what stops it is that nothing has for a beat. */
    MESH_TEST_FAIL_IF(!mesh_ui_anim_table_active(&table, 987654U + 7250U),
                      "a loop drawn this frame should be asking for the next one");
    MESH_TEST_FAIL_IF(
        !mesh_ui_anim_table_active(&table, 987654U + 7250U + MESH_UI_ANIM_LOOP_STALE_MS),
        "a loop should survive right up to the stale window");
    MESH_TEST_FAIL_IF(
        mesh_ui_anim_table_active(&table, 987654U + 7250U + MESH_UI_ANIM_LOOP_STALE_MS + 1U),
        "a loop nothing has drawn for a beat should stop asking for frames");

    MESH_TEST_FAIL_IF(mesh_ui_anim_loop(NULL, 1U, 1000U, 1000U) != 0,
                      "a NULL table should read 0 rather than crash");
    MESH_TEST_FAIL_IF(mesh_ui_anim_loop(&table, 0U, 1000U, 1000U) != 0,
                      "an id of 0 is not a key, exactly as it is not for a transition");
    MESH_TEST_FAIL_IF(mesh_ui_anim_loop(&table, 1U, 1000U, 0U) != 0,
                      "a period of nothing should read 0 rather than divide by it");
    record_success(test_name);
}

/*
 * A control that stops looping and starts reporting a position - which is exactly what the
 * update meter does the moment the download learns the asset's size.
 *
 * It has to adopt the new value rather than transition to it: the sawtooth position it was
 * carrying was never a reading, so easing from it would slide the bar out of a number that
 * meant nothing into one that does.
 */
MESH_TEST_CASE(anim_loop_and_track_do_not_bleed_into_each_other, unit) {
    struct mesh_ui_anim_table table;
    mesh_ui_anim_table_reset(&table);

    (void)mesh_ui_anim_loop(&table, 7U, 1000U, 1000U);
    const int32_t mid_loop = mesh_ui_anim_loop(&table, 7U, 1600U, 1000U);
    MESH_TEST_FAIL_IF(mid_loop == 0, "the loop should be somewhere in its stride");

    MESH_TEST_FAIL_IF(mesh_ui_anim_track(&table, 7U, 1600U, MESH_UI_ANIM_ONE / 2, 200U,
                                         MESH_UI_EASE_OUT) != MESH_UI_ANIM_ONE / 2,
                      "a loop turning into a reading should adopt it, not slide from a sawtooth");
    MESH_TEST_FAIL_IF(mesh_ui_anim_table_active(&table, 1600U + MESH_UI_ANIM_LOOP_STALE_MS + 1U),
                      "the slot should stop asking for frames once it is no longer looping");

    /* And back the other way: a reading that becomes indeterminate restarts the loop from its
       beginning rather than resuming a stride it never had. */
    MESH_TEST_FAIL_IF(mesh_ui_anim_loop(&table, 7U, 5000U, 1000U) != 0,
                      "a reading turning back into a loop should start the loop at 0");
    record_success(test_name);
}

/* A NULL animation or table answers rather than crashing: the drawing code calls these on every
   frame and a guard at each call site is a guard somebody eventually forgets. */
MESH_TEST_CASE(anim_null_arguments_are_safe, unit) {
    MESH_TEST_FAIL_IF(mesh_ui_anim_value(NULL, 1000U) != 0, "a NULL animation should read 0");
    MESH_TEST_FAIL_IF(mesh_ui_anim_active(NULL, 1000U), "a NULL animation should not be active");
    MESH_TEST_FAIL_IF(mesh_ui_anim_table_active(NULL, 1000U), "a NULL table should not be active");
    MESH_TEST_FAIL_IF(mesh_ui_anim_track(NULL, 1U, 1000U, MESH_UI_ANIM_ONE, 200U,
                                         MESH_UI_EASE_OUT) != MESH_UI_ANIM_ONE,
                      "a NULL table should still report the target");
    mesh_ui_anim_set(NULL, MESH_UI_ANIM_ONE);
    mesh_ui_anim_to(NULL, 1000U, 0, 200U, MESH_UI_EASE_OUT);
    mesh_ui_anim_table_reset(NULL);
    record_success(test_name);
}
