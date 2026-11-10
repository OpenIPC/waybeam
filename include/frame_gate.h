#ifndef FRAME_GATE_H
#define FRAME_GATE_H

#include <stddef.h>
#include <stdint.h>

/* Adaptive frame gate — an overload reflex for the stream VENC channel.
 *
 * When the frame-shm egress ring stops draining, the backend stops draining
 * the encoder's output FIFO, and resumes when the consumer catches up.  No
 * SDK call is made in either direction, and that is the whole design: the
 * intake pause this started as (MI_VENC_StopRecvPic and its per-backend
 * equivalents) emits an IRAP on every resume — measured at one per cycle on
 * Star6E, three on Maruko, 0.6 on CV610 and ~4.6 per second against a real
 * capacity-limited RF link — which is the keyframe the feature exists to
 * avoid.  Letting the FIFO backpressure changes no encoder state at all.
 *
 * There is no enable switch: the gate arms whenever the output is a frame
 * ring, because the ring is the occupancy signal it runs on and no other
 * transport has one.
 *
 * Why a gate and not a rate write.  video0.bitrate is the only proportional
 * rate actuator on the SigmaStar backends and MI_VENC_SetChnAttr implicitly
 * emits an IDR (measured; see the note above apply_bitrate() in
 * src/star6e_controls.c).  Responding to a congested link with a bitrate
 * write therefore injects the largest frame in the stream into an already
 * overflowing link.  No IDR-free *proportional* substitute exists: qpDelta
 * writes s32IPQPDelta and only redistributes bits between I and P, and
 * min_qp is a cliff that abandons CBR rather than a dial (README).  The gate
 * writes neither RC state nor ROI config, so the CBR contract and the ROI
 * QP gradient are both untouched.
 *
 * This module is pure: no SDK types, no I/O, no clock of its own.  The
 * caller supplies ring occupancy and a monotonic timestamp and applies the
 * returned action.  That keeps the policy host-testable and identical
 * across all three backends.
 *
 * Relationship to waybeam-link.  The gate does NOT move rate policy back
 * into the daemon (0.69.0 deliberately moved it out).  It is a local
 * overload reflex acting on a signal and an actuator that both live inside
 * venc and cannot be reached from outside at frame cadence; waybeam-link
 * still owns the rate model and the gate absorbs the transients between its
 * decisions.  See documentation/ADAPTIVE_FRAME_GATE_PLAN.md.
 */

/* Reopen threshold, in slots.  Pinned, not configurable: the ring's healthy
 * idle occupancy is one frame and not zero — the producer samples just after
 * writing — so "<= 1" is the documented healthy band and ">= 2" is standing
 * backlog (include/venc_frame_ring.h).  A different value would not mean
 * "more/less aggressive", it would mean "wrong". */
#define FRAME_GATE_OPEN_SLOTS 1u

/* Defaults for the two knobs that DO want bench tuning. */
#define FRAME_GATE_DEFAULT_CLOSE_SLOTS   3u
#define FRAME_GATE_DEFAULT_MAX_CLOSED_MS 500u

/* Debounce floor while closed.  One frame at 120 fps is ~8.3 ms; 20 ms is a
 * shade over two, enough that a close is never undone before the consumer
 * has had a realistic chance to drain a slot. */
#define FRAME_GATE_MIN_CLOSED_US 20000u

/* Backstop on how long the gate may stay open after a SAFETY ESCAPE.
 *
 * The escape exists so a dead consumer cannot stop the stream for good: it
 * reopens, lets ONE frame through, and the ring reports full_drops — a
 * diagnosable failure rather than a silent one.  Without something holding it
 * open it cannot even do that: occupancy is still above close_slots at the
 * instant it reopens, so the very next observation closes again, and the gated
 * idle path polls every 1-2 ms while a frame takes 10-33 ms.  Measured
 * 2026-09-12, a dead consumer froze production with every drop counter at
 * zero, under both actuators.
 *
 * The pulse ends on the FIRST frame that actually lands, not after a fixed
 * window — that is what keeps it to about one frame.  Occupancy rising above
 * its level at the escape can only mean a producer write, so the pulse never
 * ends early; what it can miss is a write that a concurrent drain cancels out,
 * which costs one extra frame per drain observed inside the pulse and is
 * capped by the backstop below.  In the regime where escapes actually happen
 * the consumer is by definition slow, so drains inside a sub-millisecond-scale
 * pulse are rare: measured ~1.01 frames per pulse on Maruko at a 5 fps drain.
 *
 * A time-based dwell instead let the drain
 * loop flush the whole buffered backlog into the ring while it was open:
 * measured 2026-09-13 on Maruko at 30 fps production against a 5 fps drain,
 * ~2.9 frames per pulse, which exactly replaced what the consumer took.  The
 * gate then never reached its reopen threshold again — close and escape
 * counters advanced in lockstep — and the ring equilibrated near full, making
 * ring residency the dominant latency term on that board.
 *
 * This constant is only the backstop for the case where the admitted frame
 * never arrives at all (encoder stopped, or the ring is full so the write is
 * dropped).  20 ms is two frame periods at 100 fps.  Normal closes are not
 * debounced by any of this — only the pulse that follows an escape. */
#define FRAME_GATE_ESCAPE_BACKSTOP_US 20000u

/* Depth of the encoder's bitstream buffer, in frames.
 *
 * This is a LATENCY control, not a gate threshold, and it belongs with the
 * gate because the gate is what makes the depth visible.  Measured 2026-09-12
 * on Star6E: while gated, a frame's age at the ring is the buffer depth times
 * the CONSUMER's frame period, so at a 10 fps drain each slot costs ~97 ms —
 * 534 ms at the SDK default of 3, 436 at 2, 341 at 1.  Ungated it is 4.1 ms
 * at every depth, because nothing queues.
 *
 * Each slot also buys one PRODUCER frame period (10 ms at 100 fps) of
 * tolerance to a consumer that stalls, which is why this is 2 and not 1: the
 * recorder shares ch0 in mirror mode and SD flash GC can stall a write, so
 * one slot of headroom is kept.  Dropping to 1 buys ~95 ms more and leaves
 * none.
 *
 * The SDK drops the pending image BEFORE encoding when this buffer is full
 * (MI VENC API v2.12 §1.3.16), so a shallow buffer can cost frames but can
 * never break the reference chain.  It must be set after channel creation and
 * before encoding starts; the SDK advises against changing it live. */
#define FRAME_GATE_STREAM_BUF_FRAMES 2u

typedef struct {
	uint32_t close_slots;    /* close when used_slots >= this */
	uint32_t open_slots;     /* reopen when used_slots <= this */
	uint32_t min_closed_us;  /* debounce floor while closed */
	uint32_t max_closed_us;  /* safety escape */
} FrameGateConfig;

typedef enum {
	FRAME_GATE_ACTION_NONE = 0,
	FRAME_GATE_ACTION_CLOSE,
	FRAME_GATE_ACTION_OPEN,
} FrameGateAction;

typedef struct {
	FrameGateConfig cfg;
	int      enabled;            /* armed: the output is a frame ring */
	int      open;               /* 1 = intake enabled (the initial state) */
	uint64_t closed_since_us;    /* valid only while !open */
	uint64_t closed_total_us;    /* cumulative time gated, for duty cycle */
	uint32_t close_events;       /* closes since setup */
	uint64_t escape_open_us;     /* when an escape reopened; 0 = not an escape */
	uint32_t escape_used_slots;  /* occupancy at that instant; +1 ends the pulse */
	uint32_t escape_events;      /* reopens forced by max_closed_us */
} FrameGate;

/* What frame_gate_setup() made of the request.  The caller reports it; the
 * checks live here so both backends cannot drift on what counts as a
 * misconfiguration. */
typedef enum {
	FRAME_GATE_SETUP_READY = 0,      /* armed */
	FRAME_GATE_SETUP_NO_RING,        /* transport has no occupancy signal */
	FRAME_GATE_SETUP_CLOSE_TOO_HIGH, /* threshold above the ring's capacity */
} FrameGateSetupStatus;

/* Resolve the config, initialise the gate open, and sanity-check the
 * threshold against the ring.
 *
 * There is no enable switch.  The gate arms itself whenever the output is a
 * frame ring, because the ring IS the signal it needs and every other
 * transport lacks one — so "enabled" and "has a frame ring" were always the
 * same question asked twice.  `slot_count` is the egress ring's capacity,
 * or 0 when this transport has no frame ring at all.
 *
 * `close_slots` 0 and `max_closed_ms` 0 select the defaults above.
 * close_slots is clamped to at least open_slots+1: equal thresholds make the
 * gate chatter open/closed on a single slot, which is worse than not gating.
 *
 * A non-READY status still leaves a valid, inert gate — the caller only has
 * to log it. */
FrameGateSetupStatus frame_gate_setup(FrameGate *g,
	uint32_t close_slots, uint32_t max_closed_ms, uint32_t slot_count);

/* Feed one observation.  `used_slots` is the instantaneous egress ring
 * occupancy and `now_us` a monotonic microsecond clock.
 *
 * Instantaneous, not the ring header's low_water_slots: that is published on
 * a 200 ms window (VENC_RING_LOW_WATER_WINDOW_US), which is the right
 * cadence for an external rate controller and far too slow to catch a burst.
 *
 * Returns the action taken.  Backends need not apply it: the policy flip IS
 * the actuator, since the drain loop consults frame_gate_is_open().  The
 * return value and the force_open()/restore_closed() rollbacks remain for a
 * backend that ever does drive hardware here. */
FrameGateAction frame_gate_observe(FrameGate *g, uint32_t used_slots,
	uint64_t now_us);

/* Drop back to the open state without emitting an action.  For teardown, for
 * a reinit, and for a caller whose actuator refused the last CLOSE. */
void frame_gate_force_open(FrameGate *g, uint64_t now_us);

/* Render the gate's observability fields for a transport-status payload, as a
 * leading-comma JSON fragment ready to splice before the closing braces.
 *
 * Without these the gate is invisible on device: "cycling normally under load",
 * "escape firing against a dead consumer" and "stream stopped" all look alike
 * from the outside.  Shared so the three backends cannot drift on names.
 *
 * `now_us` folds the in-progress closed interval into gateClosedMs, which
 * closed_total_us alone omits.  Writes at most `cap` bytes including the NUL
 * and always NUL-terminates; an unarmed gate yields an empty string. */
/* Returns what snprintf would have written, excluding the NUL — so a value
 * >= cap means the fragment was truncated — or 0 when nothing was written.
 * Callers may ignore it; the point of returning it is that a test can assert
 * the fragment fits without hardcoding any field name, and so keeps tracking
 * the format string as fields are added. */
int frame_gate_status_json(const FrameGate *g, uint64_t now_us,
	char *out, size_t cap);

/* Buffer size that always holds the fragment above with every counter
 * saturated: 111 chars, 112 including the NUL.  gateClosedMs is the uint64
 * closed_us divided by 1000, so it tops out at 17 digits rather than 20 — the
 * two event counters are uint32 and contribute 10 each.
 *
 * It lives here, not at the call sites, because the length is a property of
 * the format string in frame_gate.c.  Three backends declare this buffer, and
 * a field added to that format string would otherwise overrun a stale local
 * constant in each of them — silently, because snprintf truncates and nothing
 * downstream inspects the fragment.
 *
 * To be clear about what this did and did not fix: the 128-byte locals it
 * replaces were NOT too small — 112 fits — so no truncation was ever
 * reachable.  What was wrong is that the number lived three files away from
 * the format string that determines it, with 16 bytes of headroom, so the
 * next field added would have overrun all three at once.  Adding one uint32
 * counter takes the worst case to 145.  This is a locality fix, not a bug
 * fix; tests/test_frame_gate.c asserts the real bound so the two cannot
 * drift. */
#define FRAME_GATE_STATUS_JSON_CAP 192u

/* Reading the gate on a live device.
 *
 * There is no counter for "the escape pulse ended without a frame landing",
 * because two fields already on /api/v1/transport/status say it between them:
 * the ring counts a frame in framesSent only when the write actually lands, so
 * a full ring increments transportDrops and leaves framesSent alone
 * (include/venc_frame_ring.h).  Therefore, over any interval:
 *
 *   framesSent rising, gateEscapeEvents flat     -> healthy; the gate is
 *                                                   reopening through the
 *                                                   normal drain path
 *   framesSent rising, gateEscapeEvents rising   -> starved but alive; pulses
 *                                                   are landing their frame
 *   framesSent FLAT,   gateEscapeEvents rising   -> dead consumer; pulses are
 *                                                   firing and nothing gets
 *                                                   through.  transportDrops
 *                                                   rising narrows it to a
 *                                                   full ring, flat to a
 *                                                   stopped encoder
 *
 * gateCloseEvents and gateEscapeEvents advancing in LOCKSTEP, one apart, is
 * the separate pathology fixed in 0.85.1: every close follows an escape, so
 * the gate never reopens through the drain path at all. */

/* Restore the closed policy state after the actuator refused an OPEN.  The
 * next observation retries after min_closed_us instead of leaving policy
 * open while encoder intake is still stopped. */
void frame_gate_restore_closed(FrameGate *g, uint64_t now_us);

/* The drain predicate.  An unarmed gate is OPEN, never closed: a zeroed or
 * never-set-up FrameGate would otherwise read closed, and the backends would
 * stop draining with no armed policy left to reopen them — a permanently hung
 * stream from a struct that was merely uninitialised. */
static inline int frame_gate_is_open(const FrameGate *g)
{
	return (g && g->enabled) ? g->open : 1;
}

static inline int frame_gate_enabled(const FrameGate *g)
{
	return g && g->enabled;
}


#endif /* FRAME_GATE_H */
