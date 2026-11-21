#ifndef MARUKO_OUTPUT_H
#define MARUKO_OUTPUT_H

#include "output_socket.h"
#include "venc_config.h"
#include "venc_fec.h"
#include "venc_frame_ring.h"
#include "venc_ring.h"

#include <stdint.h>
#include <sys/socket.h>
#include <sys/uio.h>

/* Per-frame sendmmsg() batch capacity. Typical frame at 25 Mbps/120 fps is
 * 19 RTP packets; IDR can exceed that but rarely >50. If it does, the send
 * path mid-flushes (one extra sendmmsg call) rather than dropping. */
#define MARUKO_OUTPUT_BATCH_MAX 64
/* Size of owned per-slot scratch that holds a copy of the RTP header
 * concatenated with payload1. payload1 is either a 3-byte FU-A header
 * (tiny) or an AP packet (up to one MTU). Worst case at the
 * VENC_OUTPUT_PAYLOAD_CEILING_BYTES (4000) live-validated max is
 * 12 (RTP) + 4000 = 4012 bytes; rounded up to 4096 for slack and
 * alignment. Sized for jumbo-frame links such as the Realtek 3993 MTU. */
#define MARUKO_OUTPUT_BATCH_SLOT_SCRATCH 4096

/* Wall-clock ceiling on flushing one frame's batch.  Mirrors
 * STAR6E_OUTPUT_FLUSH_BUDGET_US; see the rationale there. */
#define MARUKO_OUTPUT_FLUSH_BUDGET_US 4000

/* Per-frame sendmmsg batch. We own `scratch[slot]` containing
 * [RTP header || payload1] concatenated — both the header (built on the
 * caller's stack by rtp_packetizer_send_packet) and payload1 (an FU-A
 * header or an AP builder buffer, both reused across packets) must be
 * copied because the caller reuses that memory before we flush.
 *
 * payload2, when present, is a slice of the encoder NAL buffer — that
 * memory is stable until the encoder stream is released after end_frame(),
 * so we keep it as a zero-copy iovec pointer.
 *
 * Transport snapshot (socket_handle/dst/dst_len/connected_udp) is
 * captured at begin_frame() under the transport_gen seqlock so that a
 * concurrent apply_server() on the HTTP thread cannot retarget queued
 * packets mid-frame. Enqueue and flush dereference only batch fields,
 * never the live MarukoOutput transport state. */
typedef struct {
	uint8_t scratch[MARUKO_OUTPUT_BATCH_MAX][MARUKO_OUTPUT_BATCH_SLOT_SCRATCH];
	struct iovec iov[MARUKO_OUTPUT_BATCH_MAX * 2];
	struct mmsghdr msgs[MARUKO_OUTPUT_BATCH_MAX];
	size_t count;
	int active;
	/* Transport snapshot taken at begin_frame() under transport_gen */
	int socket_handle;
	struct sockaddr_storage dst;
	socklen_t dst_len;
	int connected_udp;
	int allow_unix_encoder_stall;
	uint32_t flush_budget_us;
	int discard_remaining;
	int discard_as_error;
} MarukoOutputBatch;

/** Maruko output module — manages socket/SHM lifecycle and destination. */
typedef struct {
	int socket_handle;
	struct sockaddr_storage dst;
	socklen_t dst_len;
	VencOutputUriType transport;
	venc_ring_t *ring;
	venc_frame_ring_t *frame_ring;
	/* Window state for the reading above.  PER OUTPUT, not a file-static
	 * singleton: a second frame-shm output (dual-stream ch1) is a second
	 * ring with its own occupancy, and a shared tracker cannot represent
	 * it -- the ring would publish VHLT with a low-water that nothing ever
	 * writes, which reads as the healthiest value in the range rather than
	 * as an absent gauge. */
	VencRingLowWater low_water;
	int low_water_ready;
	int requested_connected_udp; /* user preference, persisted for apply_server */
	int connected_udp;           /* actual kernel state — set by configure() */
	int allow_unix_encoder_stall; /* unix:// blocking compatibility mode */
	uint32_t send_errors;
	uint32_t transport_gen; /* seqlock: odd = write in progress, even = stable */
	OutputSocketQueue send_queue; /* SO_SNDBUF + learned unix:// capacity */
	/* Lifetime socket-transport counters, producer-thread only.  Mirror
	 * of Star6eOutput; see the docstring there. */
	/* Producer/status shared: access both with __atomic_* relaxed ops. */
	uint32_t socket_drops;
	uint32_t socket_writes;
	MarukoOutputBatch batch;
	/* RS parity (protocols/venc-fec.md), ch0 only.  fec_lost0 is
	 * socket_drops + send_errors at begin_frame; slice_avg sizes the
	 * payload with parity on; payload_used is what the last RTP frame
	 * used (atomic relaxed, read by the status API). */
	VencFec fec;
	uint32_t fec_lost0;
	uint32_t slice_avg;
	uint16_t payload_used;
	/* Transport-pressure observation cache.  Mirrors Star6eOutput; see
	 * the docstring there.  Populated once per frame by
	 * maruko_output_observe_pressure, read by the sidecar emit and by
	 * /api/v1/transport/status on the HTTP thread. */
	int in_pressure;
	uint32_t pressure_drops;
	uint8_t last_fill_pct;
	uint32_t last_full_drops;
	uint32_t last_writes;
	uint32_t last_oversize_drops;
	int gdr_active;
	int svct_active;
	uint8_t gdr_cycle_len;
	uint8_t gdr_counter;
	/* Access units discarded before they could be shipped because the
	 * SDK's packet table was incomplete or invalid.  Transport-independent
	 * (it happens on RTP too), which is why it lives here rather than in
	 * the ring's stats; on frame-shm it is ALSO mirrored into the ring
	 * header's other_drops so the consumer can see the frame vanish.
	 * Written on the pipeline thread, read on the httpd thread: relaxed
	 * atomics, like socket_drops/socket_writes beside it in the same
	 * response. */
	uint64_t bad_au_drops;
	/* One WARN per init when packet metadata is incomplete or invalid (frame
	 * aborted, never shipped truncated). */
	uint8_t trunc_warned;
	/* One WARN per init when an access unit does not fit a ring slot.
	 * Separate one-shot from trunc_warned — different faults. */
	uint8_t oversize_warned;
	/* Recovery for a MALFORMED access unit — byte-symmetric with
	 * Star6eOutput::request_idr, see the rationale there.  An encoder
	 * fault, not egress pressure, so it fires on every transport
	 * including udp:// and unix://.  NULL disables recovery. */
	void (*request_idr)(void *ctx);
	void *idr_ctx;
} MarukoOutput;

/** Initialize UDP or Unix socket output from a parsed URI. */
int maruko_output_init(MarukoOutput *output, const VencOutputUri *uri,
	int requested_connected_udp, int allow_unix_encoder_stall);

/** Initialize SHM output: create shared memory ring buffer. */
int maruko_output_init_shm(MarukoOutput *output, const char *shm_name);

/** Initialize frame-level SHM output: create frame ring buffer. */
int maruko_output_init_frame_shm(MarukoOutput *output, const char *shm_name);

/** Observe transport pressure for telemetry.  Mirrors
 *  star6e_output_observe_pressure: hysteresis flag + in-pressure counter,
 *  caches fill_pct and SHM lifetime stats for the sidecar emit /
 *  /api/v1/transport/status to read without a second query.  Should
 *  only be called when there is a sidecar subscriber — the data has
 *  no other live consumer on the producer hot path. */
void maruko_output_observe_pressure(MarukoOutput *output);

/* Snapshot the frame-shm ring occupancy for the low-water measurement.
 * Returns -1 when the active transport is not frame-shm://.  Deliberately NOT
 * maruko_output_observe_pressure() -- that one is gated on a live sidecar
 * subscription, and the measurement must run whether or not anyone is
 * watching, because waybeam-link reads the result from the ring header. */
int maruko_output_frame_ring_fill(
	const MarukoOutput *output, venc_frame_ring_fill_t *out);


/** Change output destination URI without stopping streaming (udp:// or unix://). */
int maruko_output_apply_server(MarukoOutput *output, const char *uri);
/* Live outgoing.fec: -1 unless the output is udp:// / unix:// RTP or when
 * the parity arena cannot be allocated. */
int maruko_output_set_fec(MarukoOutput *output, int on);
/* Flush the queued packets, keeping the frame's batch open. */
int maruko_output_flush(MarukoOutput *output);

/** Begin accumulating RTP packets for a frame. When the transport is UDP
 *  and SHM is not in use, subsequent enqueues queue into a sendmmsg()
 *  batch. The batch is flushed by maruko_output_end_frame(). Safe to call
 *  when the transport is SHM — it becomes a no-op in that case. */
void maruko_output_begin_frame(MarukoOutput *output);

/** Flush any batched RTP packets for the current frame via sendmmsg().
 *  Returns number of messages successfully sent, or 0 if batching is
 *  inactive (no packets queued or SHM transport). */
int maruko_output_end_frame(MarukoOutput *output);

/** Queue one RTP packet into the active batch. Returns 0 on success,
 *  -1 if the packet cannot fit the scratch slot (caller falls back to
 *  immediate send). Intended for use by maruko_video.c's RTP write
 *  callback. */
int maruko_output_batch_enqueue(MarukoOutput *output,
	const uint8_t *header, size_t header_len,
	const uint8_t *payload1, size_t payload1_len,
	const uint8_t *payload2, size_t payload2_len);

/** Classify the errno from a failed socket send as congestion or hard error. */
void maruko_output_account_send_failure(MarukoOutput *output, int socket_handle,
	uint32_t packets);

/** Return and reset accumulated send error count. */
uint32_t maruko_output_drain_send_errors(MarukoOutput *output);

/** Close socket/ring and release output resources. */
void maruko_output_teardown(MarukoOutput *output);

#endif /* MARUKO_OUTPUT_H */
