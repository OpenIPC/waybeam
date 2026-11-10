#include "frame_gate.h"

#include <stdio.h>
#include <string.h>

static void frame_gate_resolve(uint32_t close_slots,
	uint32_t max_closed_ms, FrameGateConfig *out)
{
	out->open_slots = FRAME_GATE_OPEN_SLOTS;
	out->min_closed_us = FRAME_GATE_MIN_CLOSED_US;

	out->close_slots = close_slots ? close_slots
					: FRAME_GATE_DEFAULT_CLOSE_SLOTS;
	/* Equal thresholds chatter: one slot arriving and leaving would toggle
	 * the encoder's intake on every frame.  Keep at least one slot of
	 * separation so a close has somewhere to recover to. */
	if (out->close_slots <= out->open_slots)
		out->close_slots = out->open_slots + 1u;

	{
		/* Compute in 64 bits.  max_closed_ms is uint32 and the config
		 * loader and the API both bound it, but a value above 4294967
		 * would wrap the *1000 in uint32 and hand the gate an escape
		 * shorter than its own debounce — silently, from a config file
		 * edited behind the daemon's back.  Saturate instead. */
		uint64_t max_us = (uint64_t)(max_closed_ms ? max_closed_ms
					    : FRAME_GATE_DEFAULT_MAX_CLOSED_MS)
				* 1000u;

		if (max_us > UINT32_MAX)
			max_us = UINT32_MAX;
		out->max_closed_us = (uint32_t)max_us;
	}
	/* An escape shorter than the debounce would fire before the gate was
	 * allowed to reopen normally, turning every close into an escape. */
	if (out->max_closed_us < out->min_closed_us)
		out->max_closed_us = out->min_closed_us;
}

FrameGateSetupStatus frame_gate_setup(FrameGate *g,
	uint32_t close_slots, uint32_t max_closed_ms, uint32_t slot_count)
{
	if (!g)
		return FRAME_GATE_SETUP_NO_RING;

	memset(g, 0, sizeof(*g));
	frame_gate_resolve(close_slots, max_closed_ms, &g->cfg);
	/* Start open.  A gate that began closed could never reopen on a
	 * backend whose idle path only runs while frames flow. */
	g->open = 1;

	/* Arm only against a real frame ring: the ring is the occupancy signal
	 * the policy runs on, and no other transport has one. */
	if (slot_count == 0)
		return FRAME_GATE_SETUP_NO_RING;
	if (g->cfg.close_slots > slot_count)
		return FRAME_GATE_SETUP_CLOSE_TOO_HIGH;
	g->enabled = 1;
	return FRAME_GATE_SETUP_READY;
}

void frame_gate_force_open(FrameGate *g, uint64_t now_us)
{
	if (!g || g->open)
		return;
	if (now_us > g->closed_since_us)
		g->closed_total_us += now_us - g->closed_since_us;
	g->open = 1;
	g->closed_since_us = 0;
}

void frame_gate_restore_closed(FrameGate *g, uint64_t now_us)
{
	if (!g || !g->open)
		return;
	g->open = 0;
	g->closed_since_us = now_us;
	/* The gate is closed again, so any escape pulse is over.  Leaving it
	 * armed would let a stale pulse suppress the next legitimate close
	 * after a NORMAL reopen. */
	g->escape_open_us = 0;
}

FrameGateAction frame_gate_observe(FrameGate *g, uint32_t used_slots,
	uint64_t now_us)
{
	uint64_t elapsed_us;

	if (!g)
		return FRAME_GATE_ACTION_NONE;

	if (g->open) {
		if (used_slots < g->cfg.close_slots) {
			g->escape_open_us = 0;
			return FRAME_GATE_ACTION_NONE;
		}
		/* An escape pulse is still open.  End it on the FIRST frame
		 * that lands — occupancy rising above what it was at the escape
		 * — so about one frame is admitted.  Occupancy can only rise on
		 * a producer write, so this never ends the pulse early; a drain
		 * that cancels out a write costs one extra frame, bounded by
		 * the backstop.  Holding for a fixed window instead let the
		 * drain loop flush its whole backlog through, which is what
		 * pinned Maruko's ring near full.
		 *
		 * The backstop covers the frame that never arrives: a stopped
		 * encoder, or a full ring where the write is dropped rather
		 * than landing.  Both are the diagnosable outcome the escape
		 * exists to produce, so closing again is correct. */
		if (g->escape_open_us) {
			int landed = used_slots > g->escape_used_slots;
			/* A reinit can hand us a fresh epoch, the same case the
			 * closed branch below defends against.  Count it as
			 * expired: ending the pulse re-closes a gate that is
			 * doing its job, while holding it open would leave the
			 * stream ungated for a whole epoch's worth of clock. */
			int expired = now_us < g->escape_open_us ||
				(now_us - g->escape_open_us) >=
					FRAME_GATE_ESCAPE_BACKSTOP_US;

			if (!landed && !expired)
				return FRAME_GATE_ACTION_NONE;
		}
		g->escape_open_us = 0;
		g->open = 0;
		g->closed_since_us = now_us;
		g->close_events++;
		return FRAME_GATE_ACTION_CLOSE;
	}

	/* Closed.  A monotonic clock should never run backwards, but a reinit
	 * can hand us a fresh epoch — treat that as "no time has passed"
	 * rather than as a gigantic elapsed that escapes immediately. */
	elapsed_us = (now_us > g->closed_since_us)
		? now_us - g->closed_since_us : 0u;

	if (elapsed_us < g->cfg.min_closed_us)
		return FRAME_GATE_ACTION_NONE;

	/* Normal reopen: the consumer drained back to the healthy band.  This
	 * is checked BEFORE the escape so that a gate which drained on the
	 * very poll its backstop expires is not miscounted as an escape.
	 * With max_closed_ms at or below the debounce the two deadlines
	 * coincide (max_closed_us is floored to min_closed_us), and the
	 * escape would otherwise always win, turning every drained reopen
	 * into a spurious pulse in the counters. */
	if (used_slots <= g->cfg.open_slots) {
		frame_gate_force_open(g, now_us);
		return FRAME_GATE_ACTION_OPEN;
	}

	/* Safety escape: if the consumer died outright, used_slots never falls
	 * and the stream would stop for good.  Reopening lets the ring
	 * overflow and report full_drops instead, which is a diagnosable
	 * failure rather than a silent one. */
	if (elapsed_us >= g->cfg.max_closed_us) {
		g->escape_events++;
		frame_gate_force_open(g, now_us);
		/* Hold open until one frame lands, or the escape re-closes
		 * before the ring can report anything.  Remember the occupancy
		 * so "a frame landed" is observable.  See
		 * FRAME_GATE_ESCAPE_BACKSTOP_US. */
		g->escape_open_us = now_us ? now_us : 1u;
		g->escape_used_slots = used_slots;
		return FRAME_GATE_ACTION_OPEN;
	}

	return FRAME_GATE_ACTION_NONE;
}

int frame_gate_status_json(const FrameGate *g, uint64_t now_us,
	char *out, size_t cap)
{
	uint64_t closed_us;

	if (!out || cap == 0)
		return 0;
	out[0] = '\0';
	if (!g || !g->enabled)
		return 0;

	/* closed_total_us only accrues on reopen, so a gate closed right now
	 * would under-report by the whole current interval — and a caller polls
	 * this precisely while congested, which is exactly when it is closed.
	 * Fold the in-progress interval in. */
	closed_us = g->closed_total_us;
	if (!g->open && now_us > g->closed_since_us)
		closed_us += now_us - g->closed_since_us;

	/* Return snprintf's would-be length rather than discarding it: that is
	 * the only value that tracks the format string automatically, so a test
	 * can assert the fragment fits without naming any field.  A test that
	 * looks for the LAST field by name cannot see a field APPENDED after
	 * it — the old name is still present, merely no longer at the end. */
	return snprintf(out, cap,
		",\"gateClosed\":%s,\"gateCloseEvents\":%u,"
		"\"gateEscapeEvents\":%u,\"gateClosedMs\":%llu",
		g->open ? "false" : "true",
		(unsigned)g->close_events, (unsigned)g->escape_events,
		(unsigned long long)(closed_us / 1000u));
}
