#include "frame_gate.h"
#include "test_helpers.h"

#include <string.h>

#define MS 1000u   /* microseconds per millisecond */

/* Ring big enough that the threshold is never the thing under test. */
#define RING 64u

static void gate_on(FrameGate *g, uint32_t close_slots, uint32_t max_closed_ms)
{
	(void)frame_gate_setup(g, close_slots, max_closed_ms, RING);
}

int test_frame_gate(void)
{
	int failures = 0;
	FrameGate g;

	/* ── setup: resolution + sanity checks ──────────────────────── */
	CHECK("setup_default_ready",
		frame_gate_setup(&g, 0, 0, RING)
			== FRAME_GATE_SETUP_READY);
	CHECK("setup_default_close",
		g.cfg.close_slots == FRAME_GATE_DEFAULT_CLOSE_SLOTS);
	CHECK("setup_default_open", g.cfg.open_slots == FRAME_GATE_OPEN_SLOTS);
	CHECK("setup_default_escape",
		g.cfg.max_closed_us == FRAME_GATE_DEFAULT_MAX_CLOSED_MS * MS);
	CHECK("setup_debounce", g.cfg.min_closed_us == FRAME_GATE_MIN_CLOSED_US);
	CHECK("setup_starts_open", frame_gate_is_open(&g));

	/* close_slots must stay strictly above open_slots or the gate
	 * chatters on a single slot arriving and leaving. */
	(void)frame_gate_setup(&g, 1, 0, RING);
	CHECK("setup_clamps_equal", g.cfg.close_slots == g.cfg.open_slots + 1u);
	(void)frame_gate_setup(&g, 8, 0, RING);
	CHECK("setup_honours_explicit", g.cfg.close_slots == 8);

	/* An escape below the debounce would make every close an escape. */
	(void)frame_gate_setup(&g, 0, 1, RING);
	CHECK("setup_escape_floor",
		g.cfg.max_closed_us == FRAME_GATE_MIN_CLOSED_US);

	/* A max_closed_ms whose *1000 overflows uint32 must saturate, not
	 * wrap to an escape shorter than the debounce.  4294968 * 1000 wraps
	 * to 704 in uint32 — the value a config file edited behind the
	 * daemon's back could carry, with only the negative floor guarding
	 * it before this. */
	(void)frame_gate_setup(&g, 0, 4294968u, RING);
	CHECK("setup_escape_no_wrap", g.cfg.max_closed_us == UINT32_MAX);

	/* Drain on the exact poll the backstop expires: with max_closed_ms at
	 * the debounce floor the two deadlines coincide, and the drain reopen
	 * must win.  Otherwise every drained reopen is counted as an escape
	 * and arms a spurious pulse. */
	(void)frame_gate_setup(&g, 3, 20, RING);
	CHECK("boundary_close",
		frame_gate_observe(&g, 5, 0) == FRAME_GATE_ACTION_CLOSE);
	CHECK("boundary_drain_reopen_not_escape",
		frame_gate_observe(&g, 0, g.cfg.max_closed_us)
			== FRAME_GATE_ACTION_OPEN);
	CHECK("boundary_no_escape_counted", g.escape_events == 0);
	CHECK("boundary_no_pulse_armed", g.escape_open_us == 0);

	/* Status reporting: no ring and an unreachable threshold are both
	 * flagged, and each still leaves a valid, inert gate. */
	CHECK("setup_status_no_ring",
		frame_gate_setup(&g, 0, 0, 0)
			== FRAME_GATE_SETUP_NO_RING);
	CHECK("setup_no_ring_open", frame_gate_is_open(&g));
	/* No enable switch: a frame ring arms the gate and nothing else does. */
	CHECK("setup_no_ring_not_enabled", !frame_gate_enabled(&g));
	CHECK("setup_ring_arms",
		(frame_gate_setup(&g, 0, 0, RING) == FRAME_GATE_SETUP_READY &&
		 frame_gate_enabled(&g)));
	CHECK("setup_too_high_not_enabled",
		(frame_gate_setup(&g, 9, 0, 8) == FRAME_GATE_SETUP_CLOSE_TOO_HIGH
		 && !frame_gate_enabled(&g)));
	CHECK("setup_status_close_too_high",
		frame_gate_setup(&g, 9, 0, 8)
			== FRAME_GATE_SETUP_CLOSE_TOO_HIGH);
	CHECK("setup_too_high_open", frame_gate_is_open(&g));
	/* Exactly at capacity is legitimate, not an error. */
	CHECK("setup_close_at_capacity",
		frame_gate_setup(&g, 8, 0, 8)
			== FRAME_GATE_SETUP_READY);

	/* Counters must not survive a re-setup. */
	gate_on(&g, 3, 500);
	(void)frame_gate_observe(&g, 5, 0);
	CHECK("setup_resets_counters",
		(frame_gate_setup(&g, 3, 500, RING),
		 g.close_events == 0 && g.closed_total_us == 0 &&
		 frame_gate_is_open(&g)));

	/* ── basic close / reopen ────────────────────────────────────── */
	gate_on(&g, 3, 500);
	CHECK("on_starts_open", frame_gate_is_open(&g));
	CHECK("on_enabled", frame_gate_enabled(&g));
	CHECK("below_threshold_none",
		frame_gate_observe(&g, 2, 1000) == FRAME_GATE_ACTION_NONE);
	CHECK("still_open_at_2", frame_gate_is_open(&g));
	CHECK("at_threshold_closes",
		frame_gate_observe(&g, 3, 2000) == FRAME_GATE_ACTION_CLOSE);
	CHECK("now_closed", !frame_gate_is_open(&g));
	CHECK("close_counted", g.close_events == 1);

	/* Idempotent while the backlog persists — no repeated CLOSE. */
	CHECK("no_repeat_close",
		frame_gate_observe(&g, 5, 3000) == FRAME_GATE_ACTION_NONE);

	/* Debounce: drained, but too soon. */
	CHECK("debounce_holds",
		frame_gate_observe(&g, 0, 2000 + FRAME_GATE_MIN_CLOSED_US - 1)
			== FRAME_GATE_ACTION_NONE);
	CHECK("debounce_still_closed", !frame_gate_is_open(&g));

	/* Debounce elapsed and drained → reopen. */
	CHECK("reopens",
		frame_gate_observe(&g, 1, 2000 + FRAME_GATE_MIN_CLOSED_US)
			== FRAME_GATE_ACTION_OPEN);
	CHECK("open_again", frame_gate_is_open(&g));
	CHECK("no_escape_counted", g.escape_events == 0);
	CHECK("closed_total_accrued",
		g.closed_total_us == FRAME_GATE_MIN_CLOSED_US);

	/* ── reopen requires <= open_slots, not merely < close_slots ─── */
	gate_on(&g, 4, 500);
	CHECK("hyst_close",
		frame_gate_observe(&g, 4, 0) == FRAME_GATE_ACTION_CLOSE);
	/* 2 and 3 are below close_slots but above open_slots: the dead band.
	 * Reopening here is what would cause oscillation. */
	CHECK("hyst_deadband_2",
		frame_gate_observe(&g, 2, 100 * MS) == FRAME_GATE_ACTION_NONE);
	CHECK("hyst_deadband_3",
		frame_gate_observe(&g, 3, 200 * MS) == FRAME_GATE_ACTION_NONE);
	CHECK("hyst_still_closed", !frame_gate_is_open(&g));
	CHECK("hyst_reopen_at_1",
		frame_gate_observe(&g, 1, 300 * MS) == FRAME_GATE_ACTION_OPEN);

	/* ── safety escape: consumer died, ring never drains ─────────── */
	gate_on(&g, 3, 500);
	CHECK("escape_close",
		frame_gate_observe(&g, 8, 0) == FRAME_GATE_ACTION_CLOSE);
	CHECK("escape_not_yet",
		frame_gate_observe(&g, 8, 499 * MS) == FRAME_GATE_ACTION_NONE);
	CHECK("escape_fires",
		frame_gate_observe(&g, 8, 500 * MS) == FRAME_GATE_ACTION_OPEN);
	CHECK("escape_open", frame_gate_is_open(&g));
	CHECK("escape_counted", g.escape_events == 1);
	/* Still congested, so it closes again — the escape is a pressure valve,
	 * not a latch.  But NOT within the dwell: this assertion used to demand
	 * a re-close at 501 ms, one millisecond after the escape, which is the
	 * behaviour that made the escape useless on device.  Occupancy is still
	 * above close_slots at that instant, so an immediate re-close beat the
	 * encoder to the punch and no frame ever reached the ring — a dead
	 * consumer froze the stream with every drop counter at zero.  The valve
	 * has to stay open until a frame passes.  Occupancy is held constant
	 * at 8 here, so no frame lands and the pulse runs to its backstop —
	 * which is the dead-consumer case the escape exists for. */
	CHECK("escape_holds_when_no_frame_lands",
		frame_gate_observe(&g, 8, 501 * MS) == FRAME_GATE_ACTION_NONE);
	CHECK("escape_recloses_at_backstop",
		frame_gate_observe(&g, 8, 500 * MS + FRAME_GATE_ESCAPE_BACKSTOP_US)
			== FRAME_GATE_ACTION_CLOSE);

	/* ── clock regression (reinit hands over a fresh epoch) ──────── */
	gate_on(&g, 3, 500);
	CHECK("clockback_close",
		frame_gate_observe(&g, 5, 10ull * 1000ull * MS)
			== FRAME_GATE_ACTION_CLOSE);
	/* now_us < closed_since_us: must read as "no time passed", not as a
	 * huge elapsed that escapes on the spot. */
	CHECK("clockback_no_escape",
		frame_gate_observe(&g, 0, 5) == FRAME_GATE_ACTION_NONE);
	CHECK("clockback_still_closed", !frame_gate_is_open(&g));
	CHECK("clockback_no_escape_counted", g.escape_events == 0);

	/* ── force_open ──────────────────────────────────────────────── */
	gate_on(&g, 3, 500);
	(void)frame_gate_observe(&g, 5, 0);
	frame_gate_force_open(&g, 50 * MS);
	CHECK("force_open_opens", frame_gate_is_open(&g));
	CHECK("force_open_accrues", g.closed_total_us == 50 * MS);
	/* Idempotent. */
	frame_gate_force_open(&g, 60 * MS);
	CHECK("force_open_idempotent", g.closed_total_us == 50 * MS);

	/* ── failed OPEN rolls policy back so the actuator is retried ─── */
	frame_gate_restore_closed(&g, 70 * MS);
	CHECK("restore_closed_closes", !frame_gate_is_open(&g));
	CHECK("restore_closed_debounces",
		frame_gate_observe(&g, 0, 70 * MS + FRAME_GATE_MIN_CLOSED_US - 1)
			== FRAME_GATE_ACTION_NONE);
	CHECK("restore_closed_retries_open",
		frame_gate_observe(&g, 0, 70 * MS + FRAME_GATE_MIN_CLOSED_US)
			== FRAME_GATE_ACTION_OPEN);
	frame_gate_restore_closed(NULL, 0);       /* must not crash */

	/* ── escape pulse must survive long enough to land a frame ────── */
	{
		FrameGate e;
		uint64_t t = 1000 * MS;

		frame_gate_setup(&e, 0, 0, RING);
		/* Close on a backed-up ring, then let the escape fire. */
		CHECK("pulse_closes",
			frame_gate_observe(&e, 5, t) == FRAME_GATE_ACTION_CLOSE);
		t += e.cfg.max_closed_us;
		CHECK("pulse_escapes",
			frame_gate_observe(&e, 5, t) == FRAME_GATE_ACTION_OPEN);

		/* The consumer is dead, so occupancy is still above close_slots.
		 * Without a dwell the very next observation re-closes and the
		 * admitted frame never reaches the ring — the silent stop
		 * measured on device.  It must refuse to close here. */
		CHECK("pulse_holds_open_at_1ms",
			frame_gate_observe(&e, 5, t + 1 * MS)
				== FRAME_GATE_ACTION_NONE);
		CHECK("pulse_still_open_just_under_dwell",
			frame_gate_observe(&e, 5,
				t + FRAME_GATE_ESCAPE_BACKSTOP_US - 1)
				== FRAME_GATE_ACTION_NONE);
		CHECK("pulse_open_during_dwell", frame_gate_is_open(&e));

		/* Once the dwell is served the gate closes again normally. */
		CHECK("pulse_closes_after_dwell",
			frame_gate_observe(&e, 5, t + FRAME_GATE_ESCAPE_BACKSTOP_US)
				== FRAME_GATE_ACTION_CLOSE);

		/* A normal close is NOT debounced by the open dwell: burst
		 * response must be unchanged. */
		frame_gate_setup(&e, 0, 0, RING);
		CHECK("normal_close_not_delayed",
			frame_gate_observe(&e, 5, 1 * MS)
				== FRAME_GATE_ACTION_CLOSE);

		/* A drain during the dwell clears it, so the next backlog
		 * closes immediately rather than inheriting a stale pulse. */
		frame_gate_setup(&e, 0, 0, RING);
		t = 2000 * MS;
		(void)frame_gate_observe(&e, 5, t);
		t += e.cfg.max_closed_us;
		(void)frame_gate_observe(&e, 5, t);
		CHECK("drain_clears_pulse",
			frame_gate_observe(&e, 0, t + 1 * MS)
				== FRAME_GATE_ACTION_NONE);
		CHECK("close_immediate_after_drain",
			frame_gate_observe(&e, 5, t + 2 * MS)
				== FRAME_GATE_ACTION_CLOSE);
	}

	/* ── the escape pulse ends on the FIRST frame that lands ─────── */
	{
		FrameGate f;
		uint64_t t = 7000 * MS;

		gate_on(&f, 3, 500);
		CHECK("landed_closes", frame_gate_observe(&f, 5, t)
			== FRAME_GATE_ACTION_CLOSE);
		t += f.cfg.max_closed_us;
		CHECK("landed_escapes", frame_gate_observe(&f, 5, t)
			== FRAME_GATE_ACTION_OPEN);

		/* Same occupancy: the admitted frame has not arrived, so the
		 * pulse must stay open even though it could close on level. */
		CHECK("landed_holds_before_frame",
			frame_gate_observe(&f, 5, t + 1 * MS)
				== FRAME_GATE_ACTION_NONE);

		/* Occupancy rises by one: the frame landed.  Close NOW, well
		 * inside the backstop — holding longer is what let the drain
		 * loop flush a whole backlog through the pulse. */
		CHECK("landed_closes_immediately",
			frame_gate_observe(&f, 6, t + 2 * MS)
				== FRAME_GATE_ACTION_CLOSE);
		CHECK("landed_closed", !frame_gate_is_open(&f));

		/* And the next escape re-arms against the NEW occupancy, so a
		 * ring that stays at 6 does not read as "a frame landed".
		 * The close happened at t+2ms, so the escape is due from
		 * there, not from t. */
		t += 2 * MS + f.cfg.max_closed_us;
		CHECK("rearm_escapes", frame_gate_observe(&f, 6, t)
			== FRAME_GATE_ACTION_OPEN);
		CHECK("rearm_holds_at_same_level",
			frame_gate_observe(&f, 6, t + 1 * MS)
				== FRAME_GATE_ACTION_NONE);
		CHECK("rearm_closes_on_next_landing",
			frame_gate_observe(&f, 7, t + 2 * MS)
				== FRAME_GATE_ACTION_CLOSE);
	}

	/* ── a drain inside the pulse costs one extra frame, and no more ─ */
	{
		FrameGate f;
		uint64_t t = 9000 * MS;

		gate_on(&f, 3, 500);
		CHECK("drain_closes", frame_gate_observe(&f, 5, t)
			== FRAME_GATE_ACTION_CLOSE);
		t += f.cfg.max_closed_us;
		CHECK("drain_escapes", frame_gate_observe(&f, 5, t)
			== FRAME_GATE_ACTION_OPEN);

		/* The consumer takes one while the pulse is open.  Occupancy
		 * FALLS, which is not a landing, and the arm level is not
		 * lowered — so the pulse holds.  This is the documented
		 * imprecision: pinned so a future tightening is a deliberate
		 * change rather than a silent one. */
		CHECK("drain_below_arm_holds",
			frame_gate_observe(&f, 4, t + 1 * MS)
				== FRAME_GATE_ACTION_NONE);

		/* A write brings it back to the arm level: net zero, still not
		 * a landing by this rule, so the pulse still holds. */
		CHECK("drain_back_to_arm_holds",
			frame_gate_observe(&f, 5, t + 2 * MS)
				== FRAME_GATE_ACTION_NONE);

		/* The next write finally exceeds it and ends the pulse — two
		 * frames admitted for one drain, bounded and well inside the
		 * backstop. */
		CHECK("drain_second_write_closes",
			frame_gate_observe(&f, 6, t + 3 * MS)
				== FRAME_GATE_ACTION_CLOSE);
	}

	/* ── a regressed clock ENDS the pulse, it does not wedge it open ─ */
	{
		FrameGate f;
		uint64_t t = 11000 * MS;

		gate_on(&f, 3, 500);
		CHECK("clockback_pulse_closes", frame_gate_observe(&f, 8, t)
			== FRAME_GATE_ACTION_CLOSE);
		t += f.cfg.max_closed_us;
		CHECK("clockback_pulse_escapes", frame_gate_observe(&f, 8, t)
			== FRAME_GATE_ACTION_OPEN);

		/* A reinit hands over a fresh epoch mid-pulse.  The ring is
		 * full, so no frame can ever land and only the time term can
		 * end this pulse — if a regressed clock reads as "not expired"
		 * the gate stays open for a whole epoch's worth of clock.  The
		 * closed branch already defends against this case. */
		CHECK("clockback_pulse_does_not_wedge_open",
			frame_gate_observe(&f, 8, 5u)
				== FRAME_GATE_ACTION_CLOSE);
		CHECK("clockback_pulse_closed", !frame_gate_is_open(&f));
	}

	/* ── restore_closed disarms the pulse ────────────────────────── */
	{
		FrameGate f;
		uint64_t t = 13000 * MS;

		gate_on(&f, 3, 500);
		CHECK("restore_closes", frame_gate_observe(&f, 5, t)
			== FRAME_GATE_ACTION_CLOSE);
		t += f.cfg.max_closed_us;
		CHECK("restore_escapes", frame_gate_observe(&f, 5, t)
			== FRAME_GATE_ACTION_OPEN);

		/* The caller refuses the OPEN and puts the gate back.  That
		 * must disarm the pulse: otherwise it survives into the next
		 * NORMAL reopen and suppresses the first legitimate close
		 * there. */
		frame_gate_restore_closed(&f, t);
		CHECK("restore_closed_gate", !frame_gate_is_open(&f));

		/* Shorten the debounce so a normal reopen can land while a
		 * stale pulse would still be live.  With the shipped constants
		 * FRAME_GATE_MIN_CLOSED_US equals FRAME_GATE_ESCAPE_BACKSTOP_US
		 * exactly, so the earliest normal reopen coincides with the
		 * stale pulse's expiry and the bug is invisible — the disarm is
		 * correct on its own terms, not because those two constants
		 * happen to be equal today. */
		f.cfg.min_closed_us = 5000u;

		/* Drain to the reopen threshold so the gate reopens the normal
		 * way, well before another escape is due. */
		t += f.cfg.min_closed_us;
		CHECK("restore_normal_reopen",
			frame_gate_observe(&f, 0, t) == FRAME_GATE_ACTION_OPEN);

		/* A burst refills the ring.  This close is NOT an escape pulse
		 * and must not be debounced by one. */
		CHECK("restore_close_not_suppressed",
			frame_gate_observe(&f, 5, t + 1 * MS)
				== FRAME_GATE_ACTION_CLOSE);
	}

	/* ── an unarmed gate must never read CLOSED ──────────────────── */
	{
		FrameGate z;

		/* A zeroed struct is the shape a future caller could reach a
		 * drain loop with.  is_open() is the drain predicate, so if a
		 * zeroed gate read closed the backend would stop draining with
		 * no armed policy able to reopen it — a permanently hung
		 * stream from nothing worse than a missed setup. */
		memset(&z, 0, sizeof(z));
		CHECK("zeroed_gate_reads_open", frame_gate_is_open(&z));
		CHECK("zeroed_gate_not_enabled", !frame_gate_enabled(&z));

		/* Same for the two non-armed setup outcomes. */
		(void)frame_gate_setup(&z, 0, 0, 0);
		CHECK("no_ring_reads_open", frame_gate_is_open(&z));
		(void)frame_gate_setup(&z, 9, 0, 8);
		CHECK("close_too_high_reads_open", frame_gate_is_open(&z));
	}

	/* ── escape dwell boundary: equality must still hold it open ─── */
	{
		FrameGate b;
		uint64_t t = 5000 * MS;

		gate_on(&b, 3, 500);
		CHECK("dwell_eq_closes", frame_gate_observe(&b, 5, t)
			== FRAME_GATE_ACTION_CLOSE);
		t += b.cfg.max_closed_us;
		CHECK("dwell_eq_escapes", frame_gate_observe(&b, 5, t)
			== FRAME_GATE_ACTION_OPEN);
		/* Two observations at the same timestamp: the dwell has not
		 * elapsed, so the pulse must survive.  A strict > comparison
		 * would skip the dwell here and re-close instantly, which is
		 * the exact defect the dwell was added to fix. */
		CHECK("dwell_holds_at_equal_now",
			frame_gate_observe(&b, 5, t) == FRAME_GATE_ACTION_NONE);
		CHECK("dwell_open_at_equal_now", frame_gate_is_open(&b));
	}

	/* ── the status fragment never truncates ─────────────────────── */
	{
		FrameGate f;
		char js[FRAME_GATE_STATUS_JSON_CAP];
		int need;

		gate_on(&f, 3, 500);
		/* Saturate every counter the fragment prints.  A field added to
		 * the format string without growing the cap truncates here, and
		 * snprintf reports that to nobody — the three backends declare
		 * this buffer from FRAME_GATE_STATUS_JSON_CAP precisely so one
		 * assertion covers all of them. */
		f.close_events    = 0xFFFFFFFFu;
		f.escape_events   = 0xFFFFFFFFu;
		f.closed_total_us = 0xFFFFFFFFFFFFFFFFull;
		f.open = 1;   /* no in-progress interval to fold in */

		memset(js, 0x7F, sizeof(js));
		need = frame_gate_status_json(&f, 0, js, sizeof(js));

		/* The would-be length is the assertion that matters, because it
		 * tracks the format string with no field names in it: ANY field
		 * added without growing the cap fails here, including one
		 * APPENDED after the current last.  strlen() cannot do this job
		 * — snprintf always terminates within cap, so a length check on
		 * the OUTPUT is true even when the output was truncated. */
		CHECK("cap_fragment_fits", need > 0 && need < (int)sizeof(js));
		/* Corroborate against the real bound: 111 chars, 112 with the
		 * NUL, every counter saturated.  If this ever trips, the format
		 * string changed and FRAME_GATE_STATUS_JSON_CAP must be
		 * re-derived rather than the number here simply updated. */
		CHECK("cap_worst_case_is_112", need == 111);
		CHECK("cap_counters_intact",
			strstr(js, "\"gateCloseEvents\":4294967295") != NULL &&
			strstr(js, "\"gateEscapeEvents\":4294967295") != NULL &&
			strstr(js, "\"gateClosedMs\":18446744073709551") != NULL);
	}

	/* ── the fragment splices, and an unarmed gate contributes none ─ */
	{
		FrameGate f;
		char js[FRAME_GATE_STATUS_JSON_CAP];

		gate_on(&f, 3, 500);
		(void)frame_gate_observe(&f, 5, 1000 * MS);
		frame_gate_status_json(&f, 1000 * MS, js, sizeof(js));
		/* Leading comma: the caller splices this before a closing brace,
		 * so a missing one would produce invalid JSON on device. */
		CHECK("json_leading_comma", js[0] == ',');
		CHECK("json_reports_closed",
			strstr(js, "\"gateClosed\":true") != NULL);

		/* slot_count 0 means "not a frame ring": the gate does not arm,
		 * and the caller must get an empty string rather than fields
		 * describing a policy that is not running. */
		(void)frame_gate_setup(&f, 3, 500, 0);
		memset(js, 0x7F, sizeof(js));
		frame_gate_status_json(&f, 0, js, sizeof(js));
		CHECK("unarmed_json_empty", js[0] == '\0');
	}

	/* ── NULL safety ─────────────────────────────────────────────── */
	CHECK("null_observe",
		frame_gate_observe(NULL, 5, 0) == FRAME_GATE_ACTION_NONE);
	CHECK("null_is_open", frame_gate_is_open(NULL));
	CHECK("null_enabled_false", !frame_gate_enabled(NULL));
	frame_gate_force_open(NULL, 0);          /* must not crash */
	CHECK("null_setup",
		frame_gate_setup(NULL, 0, 0, RING)
			== FRAME_GATE_SETUP_NO_RING);

	return failures;
}
