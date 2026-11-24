#include "output_socket.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <linux/sockios.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

static int fill_unix_destination(const char *name,
	struct sockaddr_storage *dst, socklen_t *dst_len)
{
	struct sockaddr_un *addr;
	size_t name_len;

	if (!name || !name[0] || !dst || !dst_len)
		return -1;

	name_len = strlen(name);
	addr = (struct sockaddr_un *)dst;
	if (name_len > sizeof(addr->sun_path) - 2) {
		fprintf(stderr, "[output_socket] unix:// socket name too long\n");
		return -1;
	}

	memset(dst, 0, sizeof(*dst));
	addr->sun_family = AF_UNIX;
	memcpy(addr->sun_path + 1, name, name_len);
	*dst_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + name_len);
	return 0;
}

/* Size the kernel send buffer for one IDR burst at stress-level bitrates
 * (25+ Mbps at 120 fps). Embedded defaults can be well under 64 KiB and
 * would cause head-of-line blocking on IDR frames. Raising here is
 * advisory — setsockopt failure is non-fatal.
 *
 * This is the binding limit for udp:// only.  On unix:// the receiver's
 * max_dgram_qlen runs out first (see OUTPUT_SOCKET_UNIX_QLEN_RECOMMENDED),
 * so the raise is harmless but does nothing there. */
#define OUTPUT_SOCKET_SNDBUF_BYTES (512 * 1024)

/* Ceiling on how long any single unix:// send may block.
 *
 * AF_UNIX SOCK_DGRAM has no equivalent of UDP's fire-and-forget: when the
 * peer's receive queue is full the sender sleeps until the consumer drains
 * it.  The encode thread issues these sends between MI_VENC_GetStream and
 * MI_VENC_ReleaseStream, so an unbounded sleep holds a VENC output slot and
 * stalls capture — measured at up to 74 ms against a wedged consumer, which
 * cascades into dropped capture frames.  2 ms rides out ordinary consumer
 * scheduling gaps while keeping a single send well inside a 120 fps frame
 * period (8.3 ms).
 *
 * This bounds one sendmsg.  sendmmsg() applies the timeout per message, so
 * the per-frame bound is enforced separately by the batch flush deadline in
 * the backend output modules. */
#define OUTPUT_SOCKET_UNIX_SNDTIMEO_MS 2

static int open_socket(int *socket_handle, VencOutputUriType type,
	int allow_unix_encoder_stall)
{
	int domain;
	int sndbuf;

	if (!socket_handle)
		return -1;

	switch (type) {
	case VENC_OUTPUT_URI_UDP:
		domain = AF_INET;
		break;
	case VENC_OUTPUT_URI_UNIX:
		domain = AF_UNIX;
		break;
	default:
		fprintf(stderr, "[output_socket] unsupported socket transport\n");
		return -1;
	}

	*socket_handle = socket(domain, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (*socket_handle < 0) {
		fprintf(stderr, "[output_socket] socket() failed: %s\n",
			strerror(errno));
		return -1;
	}

	sndbuf = OUTPUT_SOCKET_SNDBUF_BYTES;
	if (setsockopt(*socket_handle, SOL_SOCKET, SO_SNDBUF,
		&sndbuf, sizeof(sndbuf)) != 0) {
		fprintf(stderr, "[output_socket] SO_SNDBUF(%d) failed: %s "
			"(keeping kernel default)\n", sndbuf, strerror(errno));
	}

	if (domain == AF_UNIX && allow_unix_encoder_stall) {
		fprintf(stderr, "[output_socket] WARNING: unix:// encoder-stall "
			"compatibility enabled; a blocked consumer can stall capture\n");
	} else if (domain == AF_UNIX) {
		struct timeval tv;

		tv.tv_sec = 0;
		tv.tv_usec = OUTPUT_SOCKET_UNIX_SNDTIMEO_MS * 1000;
		if (setsockopt(*socket_handle, SOL_SOCKET, SO_SNDTIMEO,
			&tv, sizeof(tv)) != 0) {
			fprintf(stderr, "[output_socket] SO_SNDTIMEO failed: %s "
				"(sends may block the encode loop)\n",
				strerror(errno));
		}
	}
	if (domain == AF_UNIX)
		(void)output_socket_warn_dgram_qlen();

	return 0;
}

static void close_socket_if_open(int *socket_handle)
{
	if (!socket_handle || *socket_handle < 0)
		return;

	close(*socket_handle);
	*socket_handle = -1;
}

static void disconnect_udp_socket(int socket_handle)
{
	struct sockaddr addr;

	if (socket_handle < 0)
		return;

	memset(&addr, 0, sizeof(addr));
	addr.sa_family = AF_UNSPEC;
	(void)connect(socket_handle, &addr, sizeof(addr));
}

int output_socket_fill_udp_destination(const char *host, uint16_t port,
	struct sockaddr_storage *dst, socklen_t *dst_len)
{
	struct sockaddr_in *addr;

	if (!host || !host[0] || port == 0 || !dst || !dst_len)
		return -1;

	memset(dst, 0, sizeof(*dst));
	addr = (struct sockaddr_in *)dst;
	addr->sin_family = AF_INET;
	addr->sin_port = htons(port);
	if (inet_pton(AF_INET, host, &addr->sin_addr) != 1) {
		fprintf(stderr, "[output_socket] invalid IPv4 address '%s'\n", host);
		return -1;
	}
	*dst_len = sizeof(*addr);
	return 0;
}

int output_socket_destination_is_usable(const VencOutputUri *uri)
{
	struct sockaddr_storage probe;
	socklen_t probe_len = 0;

	if (!uri)
		return 0;
	if (uri->type != VENC_OUTPUT_URI_UDP && uri->type != VENC_OUTPUT_URI_UNIX)
		return 0;
	return output_socket_fill_destination(uri, &probe, &probe_len) == 0;
}

int output_socket_fill_destination(const VencOutputUri *uri,
	struct sockaddr_storage *dst, socklen_t *dst_len)
{
	if (!uri || !dst || !dst_len)
		return -1;

	switch (uri->type) {
	case VENC_OUTPUT_URI_UDP:
		return output_socket_fill_udp_destination(uri->host, uri->port,
			dst, dst_len);
	case VENC_OUTPUT_URI_UNIX:
		return fill_unix_destination(uri->endpoint, dst, dst_len);
	default:
		fprintf(stderr, "[output_socket] shm:// is not a datagram socket transport\n");
		return -1;
	}
}

int output_socket_configure(int *socket_handle, struct sockaddr_storage *dst,
	socklen_t *dst_len, VencOutputUriType *transport,
	const VencOutputUri *uri, int requested_connected_udp,
	int allow_unix_encoder_stall, int *connected_udp)
{
	int want_connected;

	struct sockaddr_storage new_dst;
	socklen_t new_len = 0;

	if (!socket_handle || !dst || !dst_len || !transport || !uri)
		return -1;
	if (uri->type == VENC_OUTPUT_URI_SHM) {
		fprintf(stderr, "[output_socket] shm:// requires ring-buffer output\n");
		return -1;
	}

	/* Resolve the destination BEFORE touching the socket.
	 *
	 * This used to run after, and closed the fd on failure -- including on
	 * the REUSE path, where the transport type is unchanged and the socket
	 * was working.  Nothing here resolves names (the fill is inet_pton only),
	 * so `outgoing.server=udp://somehost:5600` destroyed a live output and
	 * left socket_handle -1, on every backend that calls this.  A caller that
	 * rejects the URI cannot be worse off than one that keeps sending to the
	 * old destination, so failing before any mutation is strictly safer.
	 *
	 * Filling a local and committing only on success also makes the whole
	 * function all-or-nothing: no path now leaves dst updated but the socket
	 * closed, or vice versa. */
	if (output_socket_fill_destination(uri, &new_dst, &new_len) != 0)
		return -1;

	if (*socket_handle < 0 || *transport != uri->type) {
		close_socket_if_open(socket_handle);
		if (open_socket(socket_handle, uri->type,
		    allow_unix_encoder_stall) != 0)
			return -1;
		*transport = uri->type;
	}

	*dst = new_dst;
	*dst_len = new_len;
	if (!connected_udp)
		return 0;

	want_connected = (uri->type == VENC_OUTPUT_URI_UDP && requested_connected_udp) ?
		1 : 0;
	if (uri->type == VENC_OUTPUT_URI_UDP && !want_connected)
		disconnect_udp_socket(*socket_handle);

	*connected_udp = 0;
	if (!want_connected)
		return 0;

	if (connect(*socket_handle, (const struct sockaddr *)dst, *dst_len) != 0) {
		fprintf(stderr, "[output_socket] UDP connect() failed: %s\n",
			strerror(errno));
		return 0;
	}

	*connected_udp = 1;
	return 0;
}

int output_socket_send_parts(int socket_handle,
	const struct sockaddr_storage *dst, socklen_t dst_len,
	int connected_udp,
	const uint8_t *header, size_t header_len,
	const uint8_t *payload1, size_t payload1_len,
	const uint8_t *payload2, size_t payload2_len)
{
	struct iovec vec[3];
	struct msghdr msg;
	int iovcnt;
	ssize_t sent;

	/* Set errno explicitly: callers classify the -1 by errno to separate
	 * congestion (EAGAIN) from real failures, and a stale EAGAIN left by
	 * an earlier call would otherwise be misread as a transport drop. */
	if (socket_handle < 0 || !header || !payload1 ||
	    header_len == 0 || payload1_len == 0) {
		errno = EINVAL;
		return -1;
	}
	if (!connected_udp && (!dst || dst_len == 0)) {
		errno = EINVAL;
		return -1;
	}

	vec[0].iov_base = (void *)header;
	vec[0].iov_len = header_len;
	vec[1].iov_base = (void *)payload1;
	vec[1].iov_len = payload1_len;
	iovcnt = 2;
	if (payload2 && payload2_len > 0) {
		vec[2].iov_base = (void *)payload2;
		vec[2].iov_len = payload2_len;
		iovcnt = 3;
	}

	memset(&msg, 0, sizeof(msg));
	if (connected_udp) {
		msg.msg_name = NULL;
		msg.msg_namelen = 0;
	} else {
		msg.msg_name = (void *)dst;
		msg.msg_namelen = dst_len;
	}
	msg.msg_iov = vec;
	msg.msg_iovlen = iovcnt;
	sent = sendmsg(socket_handle, &msg, 0);
	return sent < 0 ? -1 : 0;
}

int output_socket_capture_capacity(int socket_handle, OutputSocketQueue *queue)
{
	int sndbuf = 0;
	socklen_t sndbuf_len = sizeof(sndbuf);

	if (!queue)
		return -1;
	/* Reset field-by-field rather than memset: on a live redirect this
	 * runs on the control thread while the producer may still be inside a
	 * flush calling output_socket_note_saturation() on the same struct, and
	 * a bulk plain write there would race the atomic accesses. */
	__atomic_store_n(&queue->sndbuf_capacity, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&queue->unix_capacity, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&queue->logged_capacity, 0, __ATOMIC_RELAXED);
	if (socket_handle < 0)
		return -1;
	if (getsockopt(socket_handle, SOL_SOCKET, SO_SNDBUF, &sndbuf,
	    &sndbuf_len) != 0)
		return -1;
	if (sndbuf <= 0)
		return -1;
	__atomic_store_n(&queue->sndbuf_capacity, sndbuf, __ATOMIC_RELAXED);
	return 0;
}

/* AF_UNIX SOCK_DGRAM blocks on the peer's receive queue length (capped by
 * /proc/sys/net/unix/max_dgram_qlen), not on the sender's SO_SNDBUF.
 * SIOCOUTQ on the sender returns sk_wmem_alloc — the sum of per-skb
 * truesize for in-flight datagrams — which saturates at roughly
 * (qlen × per_skb_truesize) when the receiver stops reading.
 *
 * AVG_SKB_TRUESIZE_BYTES is only a bootstrap estimate for that product,
 * used until a real saturation event calibrates the capacity exactly (see
 * output_socket_note_saturation).  A 1400-byte RTP payload measures 2304
 * bytes of truesize on Linux (SKB_DATA_ALIGN of payload + headroom, from
 * the 2 KiB slab bucket, plus sizeof(struct sk_buff)); the exact figure
 * varies with architecture and kernel version, which is precisely why it
 * must not be the permanent denominator.
 *
 * The previous 4096 estimate over-counted by ~1.8× for MTU-sized RTP, which
 * capped the reportable fill at 61 % — under the 75 % high-water mark — so
 * a fully blocked unix:// socket never raised in_pressure at all. */
#define UNIX_DGRAM_AVG_SKB_TRUESIZE_BYTES 2304

static int read_unix_max_dgram_qlen(void)
{
	static int cached = -1;
	FILE *f;
	int v = 10;  /* kernel default when /proc is missing */

	if (cached > 0)
		return cached;
	f = fopen("/proc/sys/net/unix/max_dgram_qlen", "re");
	if (f) {
		if (fscanf(f, "%d", &v) != 1 || v <= 0)
			v = 10;
		fclose(f);
	}
	cached = v;
	return cached;
}

int output_socket_warn_dgram_qlen(void)
{
	static int warned;
	int qlen;
	FILE *f = fopen("/proc/sys/net/unix/max_dgram_qlen", "re");

	if (!f)
		return -1;
	if (fscanf(f, "%d", &qlen) != 1)
		qlen = -1;
	fclose(f);

	if (qlen > 0 && qlen < OUTPUT_SOCKET_UNIX_QLEN_RECOMMENDED && !warned) {
		warned = 1;
		fprintf(stderr,
			"[output_socket] WARNING: net.unix.max_dgram_qlen=%d is too "
			"shallow for unix:// video output\n"
			"[output_socket]   (~%d ms of buffer at 15 Mbps with 1400-byte "
			"RTP payloads; one 60 fps frame is ~23 packets)\n"
			"[output_socket]   Fix at boot, BEFORE consumers start: "
			"echo %d > /proc/sys/net/unix/max_dgram_qlen\n"
			"[output_socket]   The kernel snapshots this into each receiving "
			"socket at creation, so raising it now\n"
			"[output_socket]   will not help a consumer that is already "
			"running.\n",
			qlen, (qlen * 1400 * 8) / 15000,
			OUTPUT_SOCKET_UNIX_QLEN_RECOMMENDED);
	}
	return qlen;
}

static int socket_is_unix_dgram(int socket_handle)
{
	struct sockaddr_storage ss;
	socklen_t sslen = sizeof(ss);

	memset(&ss, 0, sizeof(ss));
	if (getsockname(socket_handle, (struct sockaddr *)&ss, &sslen) != 0)
		return 0;
	return ss.ss_family == AF_UNIX;
}

/* Bootstrap estimate of the unix:// queue capacity in SIOCOUTQ bytes, used
 * until a real saturation event supplies the exact figure. */
static int unix_estimated_capacity(int sndbuf_capacity)
{
	uint64_t denom64 = (uint64_t)read_unix_max_dgram_qlen() *
		UNIX_DGRAM_AVG_SKB_TRUESIZE_BYTES;
	int denom = denom64 > (uint64_t)INT_MAX ? INT_MAX : (int)denom64;

	/* Whichever of the peer queue and our own send buffer runs out first
	 * dictates when a send blocks. */
	if (sndbuf_capacity > 0 && sndbuf_capacity < denom)
		denom = sndbuf_capacity;
	return denom;
}

void output_socket_note_saturation(int socket_handle, OutputSocketQueue *queue)
{
	int queued = 0;

	if (socket_handle < 0 || !queue)
		return;
	if (!socket_is_unix_dgram(socket_handle))
		return;
	if (ioctl(socket_handle, SIOCOUTQ, &queued) != 0)
		return;
	if (queued <= __atomic_load_n(&queue->unix_capacity, __ATOMIC_RELAXED))
		return;
	__atomic_store_n(&queue->unix_capacity, queued, __ATOMIC_RELAXED);

	/* First calibration reveals the peer's real queue depth.  A consumer
	 * whose socket was created while the sysctl was lower (started before
	 * it was raised, or set low on purpose for that one socket) keeps that
	 * depth, and the startup warning cannot see it.  Note it once, on the
	 * cold path. */
	if (!__atomic_load_n(&queue->logged_capacity, __ATOMIC_RELAXED)) {
		int dgrams = queued / UNIX_DGRAM_AVG_SKB_TRUESIZE_BYTES;
		int qlen = read_unix_max_dgram_qlen();

		__atomic_store_n(&queue->logged_capacity, 1, __ATOMIC_RELAXED);
		if (qlen > 0 && dgrams < qlen / 2) {
			fprintf(stderr,
				"[output_socket] NOTE: unix:// peer queue holds ~%d "
				"datagrams (%d B), under the net.unix.max_dgram_qlen of "
				"%d venc read: the consumer's socket was created while "
				"the sysctl was lower.\n",
				dgrams, queued, qlen);
		}
	}
}

int output_socket_get_fill_pct(int socket_handle,
	const OutputSocketQueue *queue, uint8_t *out_pct)
{
	OutputSocketQueue local;
	int queued = 0;
	int denom;
	int cap_unix;
	int cap_sndbuf;
	uint64_t pct;

	if (socket_handle < 0 || !out_pct)
		return -1;
	if (!queue ||
	    __atomic_load_n(&queue->sndbuf_capacity, __ATOMIC_RELAXED) <= 0) {
		if (output_socket_capture_capacity(socket_handle, &local) != 0)
			return -1;
		if (queue)
			local.unix_capacity = __atomic_load_n(
				&queue->unix_capacity, __ATOMIC_RELAXED);
		queue = &local;
	}
	/* Snapshot both capacities once.  The producer can raise
	 * unix_capacity at any point (first saturation calibrates it), and
	 * re-reading it per use could mix an old and a new value into one
	 * result — publishing a percentage that never existed. */
	cap_unix = __atomic_load_n(&queue->unix_capacity, __ATOMIC_RELAXED);
	cap_sndbuf = __atomic_load_n(&queue->sndbuf_capacity, __ATOMIC_RELAXED);

	if (ioctl(socket_handle, SIOCOUTQ, &queued) != 0)
		return -1;
	if (queued < 0)
		queued = 0;

	if (socket_is_unix_dgram(socket_handle)) {
		denom = cap_unix > 0 ? cap_unix :
			unix_estimated_capacity(cap_sndbuf);
	} else {
		denom = cap_sndbuf;
	}

	if (denom <= 0)
		return -1;

	/* Linux reports SO_SNDBUF as 2× the requested size (kernel internal
	 * accounting).  Both queued and sndbuf use the same units, so the
	 * ratio is correct without correcting the doubling — what matters
	 * is "queued / capacity-as-the-kernel-sees-it". */
	pct = (uint64_t)queued * 100u / (uint64_t)denom;
	if (pct > 100)
		pct = 100;
	*out_pct = (uint8_t)pct;
	return 0;
}
