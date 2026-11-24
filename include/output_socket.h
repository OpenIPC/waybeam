#ifndef OUTPUT_SOCKET_H
#define OUTPUT_SOCKET_H

#include "venc_config.h"

#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

/* Recommended /proc/sys/net/unix/max_dgram_qlen for unix:// outputs.
 *
 * AF_UNIX SOCK_DGRAM blocks on the *receiver's* queue depth, which the
 * kernel snapshots from this sysctl when the receiving socket is created.
 * The kernel default of 10 datagrams is roughly 7 ms of buffer at 15 Mbps
 * with 1400-byte RTP payloads — less than one frame at 60 fps, so every
 * frame overruns it and the encode thread stalls waiting on consumer
 * scheduling.  256 gives ~180 ms at 15 Mbps, which absorbs ordinary
 * scheduling jitter without the send path ever blocking.
 *
 * The producer cannot fix this for a consumer that is already running --
 * raising the sysctl only affects sockets created afterwards.  It has to
 * be raised at boot (see init.d/S95waybeam); we only warn. */
#define OUTPUT_SOCKET_UNIX_QLEN_RECOMMENDED 256

/* Send-queue accounting for one udp:// / unix:// socket.
 *
 * `sndbuf_capacity` is the kernel-reported SO_SNDBUF captured once at
 * socket open.  `unix_capacity` is the observed unix:// saturation point
 * in SIOCOUTQ bytes, learned the first time a send blocks or fails with
 * EAGAIN; 0 until that happens.  See output_socket_note_saturation().
 * `logged_capacity` keeps the one-shot calibration log to one line per
 * socket.
 *
 * THREADING: every field is touched by more than one thread, so all three
 * are accessed with relaxed atomics.
 *
 *   - producer (encode) thread: output_socket_note_saturation() raises
 *     `unix_capacity` and sets `logged_capacity` on the send/flush path.
 *   - HTTP/status thread: output_socket_get_fill_pct() reads both capacities
 *     to scale the reported percentage.
 *   - control thread: output_socket_capture_capacity() RESETS all three on a
 *     live transport redirect (star6e_output_apply_server /
 *     maruko_output_apply_server).
 *
 * That last one makes this multi-writer, not single-writer.  The
 * `transport_gen` seqlock does NOT serialise it: the producer reads the
 * seqlock only to snapshot the transport into its batch, then flushes
 * outside it, so a redirect can land while a flush is still calling
 * note_saturation() on this struct.
 *
 * Relaxed is the right strength: each field is an independent scalar used
 * only to scale a telemetry percentage, and nothing else is published
 * through them, so no ordering with other stores is implied or needed.
 *
 * The atomics remove the undefined behaviour, not the interleaving.  A
 * redirect racing an in-flight flush can still leave `unix_capacity` holding
 * the previous socket's learned value, because note_saturation()'s
 * compare-then-store is not itself atomic.  That is bounded and
 * self-healing: the next saturation on the new socket recalibrates it, and
 * the only consequence meanwhile is a skewed fill percentage.  Serialising
 * it properly would need a lock on the encode thread's send path, which
 * costs more than the telemetry is worth.  Without the atomics this was also
 * a plain data race the compiler could exploit — one no test can catch,
 * since the host suite never runs these threads against this path
 * concurrently. */
typedef struct {
	int sndbuf_capacity;
	int unix_capacity;
	int logged_capacity;
} OutputSocketQueue;

/** Whether a parsed URI names a destination this module can actually use.
 *
 *  Answers the question without touching anything, so a caller that is about
 *  to mutate live transport state can refuse a bad URI first.  Ring URIs
 *  (shm://, frame-shm://) carry no sockaddr and always answer 0 -- their
 *  validity is checked where the ring is created, not here.
 *
 *  output_socket_configure() already validates before it mutates, so a caller
 *  that only ever goes through it does not need this.  It exists for the
 *  callers that commit or respawn on a URI *without* reaching configure().
 */
int output_socket_destination_is_usable(const VencOutputUri *uri);

/** Fill a sockaddr_storage from a parsed udp:// or unix:// destination. */
int output_socket_fill_destination(const VencOutputUri *uri,
	struct sockaddr_storage *dst, socklen_t *dst_len);

/** Fill a UDP sockaddr_storage from host/port values. */
int output_socket_fill_udp_destination(const char *host, uint16_t port,
	struct sockaddr_storage *dst, socklen_t *dst_len);

/** Configure socket + destination for a udp:// or unix:// transport. */
int output_socket_configure(int *socket_handle, struct sockaddr_storage *dst,
	socklen_t *dst_len, VencOutputUriType *transport,
	const VencOutputUri *uri, int requested_connected_udp,
	int allow_unix_encoder_stall, int *connected_udp);

/** Warn when the running max_dgram_qlen is too shallow for a unix://
 *  output.  Call once per unix:// socket setup.  Purely advisory — it
 *  reads /proc and logs; it never writes a system-wide sysctl, because
 *  raising it here would not help any consumer that is already running.
 *
 *  Returns the qlen it observed, or -1 if /proc was unreadable. */
int output_socket_warn_dgram_qlen(void);

/** Send one datagram composed of a header and up to two payload fragments.
 *
 *  When @p connected_udp is non-zero the socket is assumed to be connected
 *  (via connect()) and the destination pointer is skipped — the kernel
 *  routes to the connected peer and avoids the per-datagram destination
 *  lookup work. @p dst / @p dst_len may still be passed (they are ignored)
 *  so callers can keep a single parameter list.
 *
 *  Returns 0 on success, -1 on failure with errno preserved.  On unix://
 *  sockets errno == EAGAIN means the peer's receive queue stayed full for
 *  the whole SO_SNDTIMEO window — a transport drop, not a hard error, and
 *  callers should account for it separately.
 */
int output_socket_send_parts(int socket_handle,
	const struct sockaddr_storage *dst, socklen_t dst_len,
	int connected_udp,
	const uint8_t *header, size_t header_len,
	const uint8_t *payload1, size_t payload1_len,
	const uint8_t *payload2, size_t payload2_len);

/** output_socket_send_parts() with sendmsg() flags, e.g. MSG_DONTWAIT to
 *  try a blocking socket without waiting. */
int output_socket_send_parts_flags(int socket_handle,
	const struct sockaddr_storage *dst, socklen_t dst_len,
	int connected_udp,
	const uint8_t *header, size_t header_len,
	const uint8_t *payload1, size_t payload1_len,
	const uint8_t *payload2, size_t payload2_len, int flags);

/** Producer-side queue-fill snapshot for udp:// / unix:// outputs.
 *
 * Reads SIOCOUTQ (current bytes in send queue) and divides by the capacity
 * in @p queue.  Pass NULL, or a @p queue whose sndbuf_capacity is unset,
 * to have the function read SO_SNDBUF live via getsockopt — useful for
 * cold paths, tests, and recovery when the open-time capture failed; the
 * hot path should always pass the cached struct to keep this to a single
 * SIOCOUTQ syscall per call.
 *
 * For udp:// the denominator is the kernel-reported SO_SNDBUF.
 *
 * For unix:// the binding limit is the receiver's queue depth, not the
 * sender's SO_SNDBUF, and the sender cannot read the receiver's depth.
 * So the denominator is `queue->unix_capacity` once a send has actually
 * saturated the queue (exact), falling back to an estimate from the
 * max_dgram_qlen sysctl before that has ever happened (approximate, and
 * wrong whenever the sysctl changed after the consumer started).
 *
 * Linux reports SO_SNDBUF as 2× the requested size (the doubling is
 * internal kernel bookkeeping); both queued and sndbuf use the same
 * units, so the ratio is correct without correcting the doubling.
 *
 * Returns 0 on success and writes 0..100 into *out_pct.  Returns -1
 * for fd < 0 or any of the syscalls failing.  On UDP the queue drains
 * fast (kernel hands to NIC) so values >0 are rare in steady state;
 * on UNIX datagram a slow consumer can hold it pinned near 100. */
int output_socket_get_fill_pct(int socket_handle,
	const OutputSocketQueue *queue, uint8_t *out_pct);

/** Record that the send queue just saturated, calibrating the unix://
 *  fill denominator from what the kernel actually accepted.
 *
 *  Call immediately after a send returns EAGAIN or short-writes a batch:
 *  at that moment SIOCOUTQ is by definition the full queue, so it is an
 *  exact capacity reading.  This makes fill_pct correct regardless of the
 *  max_dgram_qlen sysctl, which is snapshotted into the receiver at its
 *  socket creation and so cannot be inferred from the sender's /proc.
 *
 *  Keeps the largest value seen — a partially-drained queue at the moment
 *  of the ioctl would otherwise ratchet the capacity down and pin
 *  fill_pct at 100. */
void output_socket_note_saturation(int socket_handle, OutputSocketQueue *queue);

/** Read the kernel-applied SO_SNDBUF for @p socket_handle into
 * @p queue->sndbuf_capacity and reset the learned unix:// capacity.
 * Call once after socket open / reconfigure; the kernel doesn't change
 * SO_SNDBUF unless someone calls setsockopt again, so the cached value is
 * stable for the socket's lifetime.
 *
 * Returns 0 on success, -1 on getsockopt failure or fd < 0 (in which case
 * the whole struct is zeroed). */
int output_socket_capture_capacity(int socket_handle, OutputSocketQueue *queue);

#endif /* OUTPUT_SOCKET_H */
