#ifndef STAR6E_OUTPUT_H
#define STAR6E_OUTPUT_H

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include "output_socket.h"
#include "rtp_packetizer.h"
#include "star6e.h"
#include "venc_config.h"
#include "venc_frame_ring.h"
#include "venc_ring.h"

/* Per-frame sendmmsg() batch capacity. Typical frame at 25 Mbps/120 fps is
 * 19 RTP packets; IDR can exceed that but rarely >50. If it does, the send
 * path mid-flushes (one extra sendmmsg call) rather than dropping. */
#define STAR6E_OUTPUT_BATCH_MAX 64
/* Size of owned per-slot scratch that holds a copy of the RTP header
 * concatenated with payload1. payload1 is either a 3-byte FU header
 * (tiny) or an AP packet (up to one MTU). Worst case at the
 * VENC_OUTPUT_PAYLOAD_CEILING_BYTES (4000) live-validated max is
 * 12 (RTP) + 4000 = 4012 bytes; rounded up to 4096 for slack and
 * alignment. Sized for jumbo-frame links such as the Realtek 3993 MTU. */
#define STAR6E_OUTPUT_BATCH_SLOT_SCRATCH 4096

/* Wall-clock ceiling on flushing one frame's batch.
 *
 * SO_SNDTIMEO bounds a single unix:// sendmsg, but sendmmsg() applies that
 * timeout per message, so a 64-packet batch against a stalled consumer can
 * still accumulate far past a frame period.  This deadline bounds the whole
 * frame: once it passes, the remaining packets are dropped and counted as
 * transport drops rather than stalling the encode thread further.
 *
 * 4 ms sits under a 120 fps frame period (8.3 ms).  With an adequately
 * sized max_dgram_qlen it is never reached — a healthy 15 Mbps frame
 * flushes in ~150 us. */
#define STAR6E_OUTPUT_FLUSH_BUDGET_US 4000

typedef enum {
	STAR6E_STREAM_MODE_COMPACT = 0,
	STAR6E_STREAM_MODE_RTP = 1,
} Star6eStreamMode;

typedef struct {
	Star6eStreamMode stream_mode;
	VencOutputUri uri;
	int requested_connected_udp;
	int allow_unix_encoder_stall;
	int has_server;
	int fec; /* outgoing.fec: RS parity after each frame (udp/unix RTP) */
} Star6eOutputSetup;

/* RS parity, protocols/venc-fec.md. */
#define STAR6E_FEC_PT 100
#define STAR6E_FEC_SSRC_XOR 0x52534643u
#define STAR6E_FEC_MAX_SRC 256    /* source packets protected per frame */
#define STAR6E_FEC_BLOCK_MAX 40   /* sources per block (encode-time cap) */
#define STAR6E_FEC_SYM_MAX (2 + VENC_FEC_MAX_PAYLOAD_BYTES) /* length + payload */
#define STAR6E_FEC_HDR 6

/* Per-frame sendmmsg batch. We own `scratch[slot]` containing
 * [RTP header || payload1] concatenated — both the header (built on the
 * caller's stack by rtp_packetizer_send_packet) and payload1 (an FU-A
 * header or an AP builder buffer, both reused across packets) must be
 * copied because the caller reuses that memory before we flush.
 *
 * payload2, when present, is a slice of the encoder NAL buffer — that
 * memory is stable until MI_VENC_ReleaseStream which is called after
 * end_frame(), so we keep it as a zero-copy iovec pointer.
 *
 * iovec layout: 2 iovs per slot. iov[2*slot] -> scratch[slot], length =
 * header_len+payload1_len. iov[2*slot+1] -> external payload2 pointer
 * (msg_iovlen becomes 1 if payload2 is absent).
 *
 * Transport snapshot (socket_handle/dst/dst_len/connected_udp) is
 * captured at begin_frame() under the transport_gen seqlock so that a
 * concurrent apply_server() on the HTTP thread cannot retarget queued
 * packets mid-frame. Enqueue and flush dereference only batch fields,
 * never the live Star6eOutput transport state. */
typedef struct {
	uint8_t scratch[STAR6E_OUTPUT_BATCH_MAX][STAR6E_OUTPUT_BATCH_SLOT_SCRATCH];
	struct iovec iov[STAR6E_OUTPUT_BATCH_MAX * 2];
	struct mmsghdr msgs[STAR6E_OUTPUT_BATCH_MAX];
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
} Star6eOutputBatch;

typedef struct {
	Star6eStreamMode stream_mode;
	VencOutputUriType transport;
	int socket_handle;
	struct sockaddr_storage dst;
	socklen_t dst_len;
	int connected_udp;
	int requested_connected_udp;
	int allow_unix_encoder_stall;
	venc_ring_t *ring;
	venc_frame_ring_t *frame_ring;
	uint32_t send_errors;
	uint32_t transport_gen; /* seqlock: odd = write in progress, even = stable */
	OutputSocketQueue send_queue; /* SO_SNDBUF + learned unix:// capacity */
	/* Lifetime socket-transport counters, producer-thread only.  Mirrored
	 * into last_full_drops / last_writes at observation time so the sidecar
	 * trailer reports transport_drops / packets_sent for udp:// and unix://
	 * the same way it already does for the SHM rings. */
	/* Producer/status shared: access both with __atomic_* relaxed ops. */
	uint32_t socket_drops;
	uint32_t socket_writes;
	Star6eOutputBatch batch;
	/* RS parity state: this frame's source symbols and the parity stream's
	 * own RTP state.  Producer thread only, except fec_on (set by
	 * star6e_output_set_fec, latched into fec_frame at begin_frame) and the
	 * two counters (atomic relaxed, read by the API). */
	int fec_on;
	int fec_frame;
	uint8_t *fec_arena; /* allocated on first enable, kept until teardown */
	uint16_t fec_count;
	uint32_t fec_lost0; /* socket_drops + send_errors at begin_frame */
	uint16_t fec_base_seq;
	RtpPacketizerState fec_rtp;
	int64_t fec_credit; /* parity bytes owed, x VENC_FEC_PARITY_DIV */
	uint32_t fec_parity_pkts;
	uint32_t fec_unprotected;
	/* Window state for the reading above.  PER OUTPUT, not a file-static
	 * singleton: a second frame-shm output (dual-stream ch1) is a second
	 * ring with its own occupancy, and a shared tracker cannot represent
	 * it -- the ring would publish VHLT with a low-water that nothing ever
	 * writes, which reads as the healthiest value in the range rather than
	 * as an absent gauge. */
	VencRingLowWater low_water;
	int low_water_ready;
	/* Transport-pressure observation cache (telemetry only — never gates
	 * frame transmission).  Populated by star6e_output_observe_pressure
	 * once per frame on the producer thread and read by the sidecar emit
	 * path on the same thread (one query/frame instead of two) and by
	 * the HTTP /api/v1/transport/status callback on a separate thread.
	 *
	 * Hysteresis flag enters at fill_pct >= VENC_PRESSURE_HIGH_WATER_PCT
	 * (75) and exits at fill_pct < LOW (50).  pressure_drops counts
	 * frames observed in pressure — the wire trailer field name is
	 * preserved for ABI stability across the v0.9.2 post-encode skip
	 * rollback.  uint32_t fields are read with __atomic_load_n
	 * RELAXED off-thread; naturally aligned on ARMv7 so single-load
	 * atomic in practice.
	 *
	 * last_full_drops / last_writes / last_oversize_drops carry the
	 * lifetime counters cached at observation time so the sidecar trailer
	 * can report transport_drops / packets_sent without a second
	 * venc_ring_get_fill().  Sourced from the ring for shm:// and
	 * frame-shm://, and from socket_drops / socket_writes for udp:// and
	 * unix:// (which have no oversize concept, so that one stays 0). */
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
	/* One WARN per pipeline start (reset by star6e_output_reset's memset)
	 * when packet metadata is incomplete or invalid — the frame is aborted,
	 * never shipped truncated. */
	uint8_t trunc_warned;
	/* One WARN per pipeline start when an access unit does not fit a ring
	 * slot.  Separate one-shot from trunc_warned: the two are different
	 * faults and either must not mask the other. */
	uint8_t oversize_warned;
	/* Recovery for a MALFORMED access unit: the SDK handed us a packet
	 * table we cannot ship, the whole picture is dropped, and the
	 * decoder's reference chain stays broken until the next IDR — with a
	 * long GOP, seconds.
	 *
	 * This is an encoder-side event, NOT egress pressure, so it is not the
	 * self-actuation ring v2 removed.  The ring header draws the same line
	 * itself: other_drops is kept apart from full_drops because it "is not
	 * congestion at all and slowing down fixes nothing".  It therefore
	 * fires on EVERY transport, including udp:// and unix://, which have
	 * no ring and were never congested.
	 *
	 * A non-referenced (SVC-T) frame is droppable by construction, so
	 * losing one costs exactly one frame and must NOT trigger an IDR.
	 *
	 * Paced by the shared 100 ms per-channel IDR limiter inside the
	 * callback, so this coalesces against the scene detector's requests.
	 * NULL disables recovery. */
	void (*request_idr)(void *ctx);
	void *idr_ctx;
} Star6eOutput;

typedef struct {
	int socket_handle;
	struct sockaddr_storage dst;
	socklen_t dst_len;
} Star6eAudioSendTarget;

typedef struct {
	const Star6eOutput *video_output;
	int socket_handle;
	struct sockaddr_storage fallback_dst;
	socklen_t fallback_dst_len;
	uint16_t port_override;
	uint16_t max_payload_size;
	Star6eAudioSendTarget cached_target;
	uint32_t cached_gen;
	int cache_valid;
} Star6eAudioOutput;

typedef size_t (*Star6eOutputRtpSendFn)(Star6eOutput *output,
	const MI_VENC_Stream_t *stream, void *opaque);

/** Validate and prepare output config from URI and stream mode name. */
int star6e_output_prepare(Star6eOutputSetup *setup, const char *server_uri,
	const char *stream_mode_name, int connected_udp);

/** Check if output setup is configured for RTP streaming. */
int star6e_output_setup_is_rtp(const Star6eOutputSetup *setup);

/** Reset output state to uninitialized. */
void star6e_output_reset(Star6eOutput *output);

/** Create socket and connect to destination per setup config. */
int star6e_output_init(Star6eOutput *output, const Star6eOutputSetup *setup);

/* Live outgoing.fec (protocols/venc-fec.md).  The parity arena is
 * allocated on the first enable and kept until teardown; the encode thread
 * picks the flag up at the next begin_frame.  -1 when the output is not
 * udp:// / unix:// RTP or the allocation fails. */
int star6e_output_set_fec(Star6eOutput *output, int on);
/** Check if active output uses RTP mode. */
int star6e_output_is_rtp(const Star6eOutput *output);

/** Check if active output uses shared memory mode. */
int star6e_output_is_shm(const Star6eOutput *output);

/** Check if active output uses frame-level shared memory mode. */
int star6e_output_is_frame_shm(const Star6eOutput *output);

/** Observe transport pressure for telemetry. Updates the hysteresis
 *  flag (`output->in_pressure`), the in-pressure counter
 *  (`output->pressure_drops`), and caches the latest fill_pct + SHM
 *  lifetime counters (`output->last_*`) for later sidecar emit / HTTP
 *  status read with no extra query.  Never directs the caller to skip
 *  — the caller MUST always emit the frame.  See
 *  `venc_observe_pressure` in venc_ring.h for the rationale (skip-on-
 *  pressure broke H.265 reference chains).  Should only be called when
 *  there is a sidecar subscriber — the data has no other live consumer
 *  on the producer hot path. */
void star6e_output_observe_pressure(Star6eOutput *output);

/* Snapshot the frame-shm ring occupancy for the low-water measurement.
 * Returns -1 when the active transport is not frame-shm://.  Deliberately NOT
 * star6e_output_observe_pressure() -- that one is gated on a live sidecar
 * subscription, and the measurement must run whether or not anyone is
 * watching, because waybeam-link reads the result from the ring header. */
int star6e_output_frame_ring_fill(
	const Star6eOutput *output, venc_frame_ring_fill_t *out);

/** Validate that every vendor pack exposes complete, in-bounds NAL
 * descriptors. All consumers must reject the AU when this returns zero. */
int star6e_output_stream_packet_info_complete(
	const MI_VENC_Stream_t *stream);

/** Flatten a VENC stream's NAL payload into ONE malloc'd Annex-B access unit.
 *
 *  For the asynchronous recorder: the SDK's stream memory is only valid until
 *  ReleaseStream, so anything that writes it on another thread must own a copy
 *  first.  Returns the buffer (the caller, or the writer queue it is handed
 *  to, owns and frees it) or NULL if there is nothing valid to record.
 *
 *  *out_len gets the byte count and *out_is_idr whether any VCL NAL was an
 *  IRAP, which segment rotation needs.  The IRAP test reads H.265 NAL types
 *  (19/20) and is H.265-ONLY: on an H.264 stream those numbers mean something
 *  else, so *out_is_idr would be meaningless.  Every SigmaStar craft encodes
 *  H.265; a future H.264 path must extend this before recording works.  A stream whose
 *  packetInfo table is incomplete is refused, exactly as the synchronous
 *  writers refuse it — a partial table means the pack boundaries are unknown
 *  and half an access unit is worse than none.
 *
 *  Sized from the stream itself rather than a fixed buffer, so it does not
 *  inherit the 512 KB cliff of the automatic in
 *  star6e_ts_recorder_write_stream().  It is not unbounded, though: an access
 *  unit larger than VENC_REC_WRITER_MAX_BYTES is refused here rather than
 *  copied, because the writer queue would reject it at its cap anyway.  The
 *  caller counts that refusal — see rec_flatten_failures. */
uint8_t *star6e_output_stream_flatten(const MI_VENC_Stream_t *stream,
	size_t *out_len, int *out_is_idr);

/** Reject an incomplete AU before output/recording. Returns 1 when rejected,
 * emits a one-time warning, counts the drop, and — on every transport —
 * requests paced recovery when the dropped picture was a reference frame. */
int star6e_output_reject_incomplete_access_unit(Star6eOutput *output,
	const MI_VENC_Stream_t *stream);


/** Begin accumulating RTP packets for a frame. When the transport is UDP
 *  and SHM is not in use, subsequent star6e_output_send_rtp_parts() calls
 *  queue into a sendmmsg() batch instead of per-packet sendmsg(). The
 *  batch is flushed by star6e_output_end_frame(). Safe to call when the
 *  transport is SHM — it becomes a no-op in that case. */
void star6e_output_begin_frame(Star6eOutput *output);

/** Flush any batched RTP packets for the current frame via sendmmsg().
 *  Returns the number of messages successfully sent, or 0 if batching is
 *  inactive (no packets queued or SHM transport). */
int star6e_output_end_frame(Star6eOutput *output);

/** Send RTP header and payload parts as a single UDP datagram.
 *  payload2 may be NULL/0 for single-part payloads.
 *  When a batch is active (between begin_frame/end_frame) and the transport
 *  is UDP, the packet is queued into the batch. Otherwise it is sent
 *  immediately. */
int star6e_output_send_rtp_parts(Star6eOutput *output,
	const uint8_t *header, size_t header_len,
	const uint8_t *payload1, size_t payload1_len,
	const uint8_t *payload2, size_t payload2_len);

/** Return and reset accumulated send error count. */
uint32_t star6e_output_drain_send_errors(Star6eOutput *output);

/** Send one raw packet in compact stream mode. */
int star6e_output_send_compact_packet(Star6eOutput *output,
	const uint8_t *packet, uint32_t packet_size, uint32_t max_size);

/** Send entire encoder frame in compact stream mode. */
size_t star6e_output_send_compact_frame(Star6eOutput *output,
	const MI_VENC_Stream_t *stream, uint32_t max_size);

/** Send encoder frame via configured output mode (RTP or compact). */
size_t star6e_output_send_frame(Star6eOutput *output,
	const MI_VENC_Stream_t *stream, uint32_t max_size,
	Star6eOutputRtpSendFn rtp_send, void *opaque);

/** Change output destination URI without stopping streaming. */
int star6e_output_apply_server(Star6eOutput *output, const char *uri);

/** Close socket and release output resources. */
void star6e_output_teardown(Star6eOutput *output);

/** Reset audio output state to uninitialized. */
void star6e_audio_output_reset(Star6eAudioOutput *audio_output);

/** Initialize audio output.
 *  port_override=0 shares the active video destination.
 *  port_override!=0 uses dedicated UDP audio, following the video host for
 *  udp:// output and falling back to 127.0.0.1 for unix:// or shm:// video. */
int star6e_audio_output_init(Star6eAudioOutput *audio_output,
	const Star6eOutput *video_output, uint16_t port_override,
	uint16_t max_payload_size);

/** Return the configured UDP audio port, or the shared UDP video port. */
uint16_t star6e_audio_output_port(const Star6eAudioOutput *audio_output);

/** Send audio frame as RTP packets. */
int star6e_audio_output_send_rtp(Star6eAudioOutput *audio_output,
	const uint8_t *data, size_t len, RtpPacketizerState *rtp_state,
	uint32_t frame_ticks);

/** Send audio frame in compact mode (raw bytes, no RTP). */
int star6e_audio_output_send_compact(Star6eAudioOutput *audio_output,
	const uint8_t *data, size_t len);

/** Send audio frame using the configured output mode. */
int star6e_audio_output_send(Star6eAudioOutput *audio_output,
	const uint8_t *data, size_t len, RtpPacketizerState *rtp_state,
	uint32_t frame_ticks);

/** Release audio output resources. */
void star6e_audio_output_teardown(Star6eAudioOutput *audio_output);

#endif
