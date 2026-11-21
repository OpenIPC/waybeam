#include "star6e_output.h"
#include "venc_rec_writer.h"
#include "timing.h"

#include "output_socket.h"
#include "rs_fec.h"
#include "venc_config.h"
#include "test_helpers.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

typedef struct {
	int socket_handle;
	int stop;
	uint32_t packets_read;
} UnixDrainThread;

static void *delayed_unix_drain(void *opaque)
{
	UnixDrainThread *drain = opaque;
	uint8_t packet[2048];

	usleep(20000);
	while (!__atomic_load_n(&drain->stop, __ATOMIC_ACQUIRE)) {
		ssize_t n;

		do {
			n = recv(drain->socket_handle, packet, sizeof(packet),
				MSG_DONTWAIT);
			if (n > 0)
				drain->packets_read++;
		} while (n > 0);
		usleep(1000);
	}
	return NULL;
}

static int create_udp_receiver(uint16_t *port)
{
	struct sockaddr_in addr;
	socklen_t addr_len = sizeof(addr);
	struct timeval timeout = { .tv_sec = 1, .tv_usec = 0 };
	int socket_handle;

	socket_handle = socket(AF_INET, SOCK_DGRAM, 0);
	if (socket_handle < 0)
		return -1;

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(0);
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(socket_handle, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
		close(socket_handle);
		return -1;
	}
	if (getsockname(socket_handle, (struct sockaddr *)&addr, &addr_len) != 0) {
		close(socket_handle);
		return -1;
	}

	(void)setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO, &timeout,
		sizeof(timeout));
	*port = ntohs(addr.sin_port);
	return socket_handle;
}

static int create_unix_receiver(const char *name)
{
	struct sockaddr_un addr;
	struct timeval timeout = { .tv_sec = 1, .tv_usec = 0 };
	size_t name_len;
	socklen_t addr_len;
	int socket_handle;

	if (!name || !name[0])
		return -1;

	name_len = strlen(name);
	if (name_len > sizeof(addr.sun_path) - 2)
		return -1;

	socket_handle = socket(AF_UNIX, SOCK_DGRAM, 0);
	if (socket_handle < 0)
		return -1;

	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	memcpy(addr.sun_path + 1, name, name_len);
	addr_len = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + name_len);
	if (bind(socket_handle, (struct sockaddr *)&addr, addr_len) != 0) {
		close(socket_handle);
		return -1;
	}

	(void)setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO, &timeout,
		sizeof(timeout));
	return socket_handle;
}

static uint16_t output_udp_port(const Star6eOutput *output)
{
	const struct sockaddr_in *dst;

	if (!output || output->dst_len != sizeof(*dst))
		return 0;

	dst = (const struct sockaddr_in *)&output->dst;
	return ntohs(dst->sin_port);
}

static uint32_t output_udp_addr(const Star6eOutput *output)
{
	const struct sockaddr_in *dst;

	if (!output || output->dst_len != sizeof(*dst))
		return 0;

	dst = (const struct sockaddr_in *)&output->dst;
	return dst->sin_addr.s_addr;
}

static int g_test_star6e_rtp_send_called;
static int g_test_star6e_rtp_send_valid;

static size_t test_star6e_output_rtp_send_stub(Star6eOutput *output,
	const MI_VENC_Stream_t *stream, void *opaque)
{
	size_t *result = opaque;

	g_test_star6e_rtp_send_called++;
	g_test_star6e_rtp_send_valid = (output != NULL && stream != NULL);
	return result ? *result : 0;
}

static int test_star6e_output_reset_state(void)
{
	Star6eOutput output;
	int failures = 0;

	memset(&output, 0xA5, sizeof(output));
	star6e_output_reset(&output);
	CHECK("star6e output reset socket", output.socket_handle == -1);
	CHECK("star6e output reset transport",
		output.transport == VENC_OUTPUT_URI_UDP);
	CHECK("star6e output reset not rtp", !star6e_output_is_rtp(&output));
	CHECK("star6e output reset not shm", !star6e_output_is_shm(&output));
	return failures;
}

static int test_star6e_output_udp_init(void)
{
	Star6eOutputSetup setup;
	Star6eOutput output;
	int failures = 0;
	int ret;

	ret = star6e_output_prepare(&setup, "udp://127.0.0.1:5600",
		"rtp", 0);
	CHECK("star6e output udp prepare", ret == 0);
	CHECK("star6e output udp setup is rtp", star6e_output_setup_is_rtp(&setup));
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e output udp init", ret == 0);
	CHECK("star6e output udp transport",
		output.transport == VENC_OUTPUT_URI_UDP);
	CHECK("star6e output udp is rtp", star6e_output_is_rtp(&output));
	CHECK("star6e output udp is not shm", !star6e_output_is_shm(&output));
	CHECK("star6e output udp socket", output.socket_handle >= 0);
	CHECK("star6e output udp ring null", output.ring == NULL);
	CHECK("star6e output udp port", output_udp_port(&output) == 5600);
	CHECK("star6e output udp addr",
		output_udp_addr(&output) == inet_addr("127.0.0.1"));
	star6e_output_teardown(&output);
	return failures;
}

static int test_star6e_output_udp_invalid_host_rejected(void)
{
	Star6eOutputSetup setup;
	Star6eOutput output;
	int failures = 0;
	int ret;

	ret = star6e_output_prepare(&setup, "udp://localhost:5600",
		"rtp", 0);
	CHECK("star6e output invalid host prepare", ret == 0);
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e output invalid host init rejected", ret == -1);
	return failures;
}

static int test_star6e_output_udp_apply_server(void)
{
	Star6eOutputSetup setup;
	Star6eOutput output;
	int failures = 0;
	int ret;

	ret = star6e_output_prepare(&setup, "udp://127.0.0.1:5600",
		"rtp", 1);
	CHECK("star6e output udp apply prepare", ret == 0);
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e output udp apply init", ret == 0);
	ret = star6e_output_apply_server(&output, "udp://127.0.0.1:5601");
	CHECK("star6e output udp apply ok", ret == 0);
	CHECK("star6e output udp apply port", output_udp_port(&output) == 5601);
	star6e_output_teardown(&output);
	return failures;
}

static int test_star6e_output_udp_send_rtp(void)
{
	Star6eOutputSetup setup;
	Star6eOutput output;
	uint8_t header[12] = { 0x80, 97, 0, 1, 0, 0, 0, 2, 0, 0, 0, 3 };
	uint8_t payload[4] = { 1, 2, 3, 4 };
	uint8_t buf[16];
	char uri[64];
	uint16_t port;
	ssize_t received;
	int recv_socket;
	int failures = 0;
	int ret;

	recv_socket = create_udp_receiver(&port);
	CHECK("star6e output udp rtp receiver", recv_socket >= 0);
	snprintf(uri, sizeof(uri), "udp://127.0.0.1:%u", port);
	ret = star6e_output_prepare(&setup, uri, "rtp", 0);
	CHECK("star6e output udp rtp prepare", ret == 0);
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e output udp rtp init", ret == 0);
	ret = star6e_output_send_rtp_parts(&output, header, sizeof(header), payload,
		sizeof(payload), NULL, 0);
	CHECK("star6e output udp rtp send", ret == 0);
	received = recv(recv_socket, buf, sizeof(buf), 0);
	CHECK("star6e output udp rtp recv size",
		received == (ssize_t)(sizeof(header) + sizeof(payload)));
	CHECK("star6e output udp rtp recv header",
		received >= (ssize_t)sizeof(header) &&
		memcmp(buf, header, sizeof(header)) == 0);
	CHECK("star6e output udp rtp recv payload",
		received >= (ssize_t)(sizeof(header) + sizeof(payload)) &&
		memcmp(buf + sizeof(header), payload, sizeof(payload)) == 0);
	star6e_output_teardown(&output);
	close(recv_socket);
	return failures;
}

static int test_star6e_output_shm_send_rtp(void)
{
	char uri[64];
	char name[48];
	Star6eOutputSetup setup;
	Star6eOutput output;
	venc_ring_t *attached = NULL;
	uint8_t header[12] = { 0x80, 97, 0, 9, 0, 0, 0, 8, 0, 0, 0, 7 };
	uint8_t payload[5] = { 5, 4, 3, 2, 1 };
	uint8_t slot_data[32];
	uint16_t slot_len = 0;
	int failures = 0;
	int ret;

	snprintf(name, sizeof(name), "test_star6e_output_send_%ld", (long)getpid());
	snprintf(uri, sizeof(uri), "shm://%s", name);
	ret = star6e_output_prepare(&setup, uri, "rtp", 0);
	CHECK("star6e output shm send prepare", ret == 0);
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e output shm send init", ret == 0);
	ret = star6e_output_send_rtp_parts(&output, header, sizeof(header), payload,
		sizeof(payload), NULL, 0);
	CHECK("star6e output shm send", ret == 0);
	attached = venc_ring_attach(name);
	CHECK("star6e output shm attach", attached != NULL);
	ret = attached ? venc_ring_read(attached, slot_data, sizeof(slot_data),
		&slot_len) : -1;
	CHECK("star6e output shm read", ret == 0);
	CHECK("star6e output shm read slot len",
		slot_len == (uint16_t)(sizeof(header) + sizeof(payload)));
	CHECK("star6e output shm read header",
		memcmp(slot_data, header, sizeof(header)) == 0);
	CHECK("star6e output shm read payload",
		memcmp(slot_data + sizeof(header), payload, sizeof(payload)) == 0);
	if (attached)
		venc_ring_destroy(attached);
	star6e_output_teardown(&output);
	return failures;
}

/* ── RS parity (protocols/venc-fec.md) ─────────────────────────────── */

static uint32_t rd_u32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
		(uint32_t)p[2] << 8 | p[3];
}

static uint8_t fec_byte(uint16_t seq, size_t j)
{
	return (uint8_t)(seq * 37u + j * 11u + 5u);
}

/* Send one frame of `k` packets (seq from `base`, payload split 3 B + rest
 * like an FU) and collect every datagram the receiver got. */
static int fec_send_frame(Star6eOutput *output, int rx, uint16_t base,
	unsigned k, size_t (*len_of)(unsigned), uint8_t (*pkts)[1500],
	ssize_t *lens, int max_pkts)
{
	uint8_t header[12] = { 0x80, 97, 0, 0, 0, 0, 0x10, 0, 0x12, 0x34, 0x56, 0x78 };
	uint8_t payload[1400];
	int n = 0;

	star6e_output_begin_frame(output);
	for (unsigned i = 0; i < k; i++) {
		const uint16_t seq = (uint16_t)(base + i);
		const size_t len = len_of(i);

		header[1] = (uint8_t)(97 | (i == k - 1 ? 0x80 : 0));
		header[2] = (uint8_t)(seq >> 8);
		header[3] = (uint8_t)seq;
		for (size_t j = 0; j < len; j++)
			payload[j] = fec_byte(seq, j);
		if (star6e_output_send_rtp_parts(output, header, 12, payload, 3,
		    payload + 3, len - 3) != 0)
			return -1;
	}
	star6e_output_end_frame(output);
	while (n < max_pkts) {
		lens[n] = recv(rx, pkts[n], sizeof(pkts[n]), MSG_DONTWAIT);
		if (lens[n] <= 0)
			break;
		n++;
	}
	return n;
}

static size_t fec_len_mixed(unsigned i) { return 600 + (i * 97) % 800; }
static size_t fec_len_small(unsigned i) { return 60 + i * 20; }

static int fec_open(Star6eOutput *output, int fec, int *rx)
{
	Star6eOutputSetup setup;
	char uri[64];
	uint16_t port;
	int rcvbuf = 4 << 20;

	*rx = create_udp_receiver(&port);
	if (*rx < 0)
		return -1;
	(void)setsockopt(*rx, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
	snprintf(uri, sizeof(uri), "udp://127.0.0.1:%u", port);
	if (star6e_output_prepare(&setup, uri, "rtp", 0) != 0)
		return -1;
	setup.fec = fec;
	return star6e_output_init(output, &setup);
}

/* Check every parity datagram of a frame against an independent re-encode
 * of the expected source symbols; return the parity count, -1 on a fault.
 * Block sizes seen are written to ks[] (one entry per new block). */
static int fec_check_parity(uint8_t (*pkts)[1500], const ssize_t *lens,
	int n, unsigned k_total, uint16_t base, size_t (*len_of)(unsigned),
	unsigned *ks)
{
	static uint8_t sym[STAR6E_FEC_BLOCK_MAX][STAR6E_FEC_SYM_MAX];
	uint8_t want[STAR6E_FEC_SYM_MAX];
	int parity = 0, nblocks = 0;
	int last_base = -1;

	for (int p = (int)k_total; p < n; p++) {
		const uint8_t *d = pkts[p];
		const unsigned k = d[14], idx = d[15];
		const uint16_t bseq = (uint16_t)(d[16] << 8 | d[17]);
		const unsigned first = (uint16_t)(bseq - base);
		size_t maxlen = 0;

		if ((d[1] & 0x7F) != STAR6E_FEC_PT || d[12] != 0x7E ||
		    (d[13] & 0xC7) != 0x01 || k == 0 || k > STAR6E_FEC_BLOCK_MAX ||
		    rd_u32(d + 8) != (0x12345678u ^ STAR6E_FEC_SSRC_XOR) ||
		    rd_u32(d + 4) != 0x00001000u || lens[p] > 1420 ||
		    (d[1] & 0x80))
			return -1;
		if (ks && bseq != last_base)
			ks[nblocks++] = k;
		last_base = bseq;
		/* E is set exactly on the frame's last block. */
		if (((d[13] & 0x20) != 0) != (first + k == k_total))
			return -1;
		for (unsigned i = 0; i < k; i++)
			if (len_of(first + i) > maxlen)
				maxlen = len_of(first + i);
		memset(sym, 0, sizeof(sym));
		for (unsigned i = 0; i < k; i++) {
			const uint16_t seq = (uint16_t)(bseq + i);
			const size_t len = len_of(first + i);

			sym[i][0] = (uint8_t)(len >> 8);
			sym[i][1] = (uint8_t)len;
			for (size_t j = 0; j < len; j++)
				sym[i][2 + j] = fec_byte(seq, j);
		}
		rs_fec_encode(k, idx, sym[0], sizeof(sym[0]), 2 + maxlen, want);
		if (lens[p] != (ssize_t)(12 + STAR6E_FEC_HDR + 2 + maxlen) ||
		    memcmp(d + 12 + STAR6E_FEC_HDR, want, 2 + maxlen) != 0)
			return -1;
		parity++;
	}
	return parity;
}

static int test_star6e_output_fec(void)
{
	static uint8_t pkts[256][1500];
	static ssize_t lens[256];
	Star6eOutput output;
	uint64_t src_bytes = 0, par_bytes = 0;
	unsigned ks[8] = { 0 };
	int failures = 0, rx = -1, n, parity;
	uint16_t seq = 65530; /* first frame wraps the media seq */

	/* Control: fec off sends the source packets and nothing else. */
	CHECK("fec off init", fec_open(&output, 0, &rx) == 0);
	n = fec_send_frame(&output, rx, seq, 12, fec_len_mixed, pkts, lens, 256);
	CHECK("fec off: exactly the 12 source packets", n == 12);
	star6e_output_teardown(&output);
	close(rx);

	CHECK("fec on init", fec_open(&output, 1, &rx) == 0);
	n = fec_send_frame(&output, rx, seq, 12, fec_len_mixed, pkts, lens, 256);
	CHECK("fec on: sources first, untouched",
		n > 12 && (pkts[0][1] & 0x7F) == 97 && lens[0] == 12 + 600 &&
		pkts[0][12] == fec_byte(seq, 0) && (pkts[11][1] & 0x80));
	parity = fec_check_parity(pkts, lens, n, 12, seq, fec_len_mixed, NULL);
	CHECK("fec on: parity after the frame, valid, M=0", parity >= 1);
	seq = (uint16_t)(seq + 12);

	/* 100 sources split into 34 + 33 + 33, E only on the last block. */
	n = fec_send_frame(&output, rx, seq, 100, fec_len_mixed, pkts, lens, 256);
	parity = fec_check_parity(pkts, lens, n, 100, seq, fec_len_mixed, ks);
	CHECK("fec on: 100 sources -> blocks of 34, 33, 33",
		parity >= 3 && ks[0] == 34 && ks[1] == 33 && ks[2] == 33);
	seq = (uint16_t)(seq + 100);

	/* Ratio: parity datagram bytes stay at 1/4 of source datagram bytes,
	 * including 1-3 packet frames where r is mostly 0. */
	for (unsigned f = 0; f < 3000; f++) {
		const unsigned k = 1 + f % 3 + (f % 7 == 0 ? 9 : 0);

		n = fec_send_frame(&output, rx, seq, k, fec_len_small, pkts, lens, 256);
		for (int p = 0; p < n; p++)
			*(p < (int)k ? &src_bytes : &par_bytes) += (uint64_t)lens[p];
		seq = (uint16_t)(seq + k);
	}
	CHECK("fec ratio = 0.25 +- 1 %",
		par_bytes * 1000 >= src_bytes * 240 &&
		par_bytes * 1000 <= src_bytes * 260);
	CHECK("fec counters move",
		output.fec_parity_pkts > 0 && output.fec_unprotected > 0);
	star6e_output_teardown(&output);
	close(rx);
	return failures;
}

static int test_star6e_output_shm_init(void)
{
	char uri[64];
	Star6eOutputSetup setup;
	Star6eOutput output;
	int failures = 0;
	int ret;

	snprintf(uri, sizeof(uri), "shm://test_star6e_output_%ld", (long)getpid());
	ret = star6e_output_prepare(&setup, uri, "rtp", 0);
	CHECK("star6e output shm prepare", ret == 0);
	CHECK("star6e output shm setup is rtp", star6e_output_setup_is_rtp(&setup));
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e output shm init", ret == 0);
	CHECK("star6e output shm transport",
		output.transport == VENC_OUTPUT_URI_SHM);
	CHECK("star6e output shm is rtp", star6e_output_is_rtp(&output));
	CHECK("star6e output shm is shm", star6e_output_is_shm(&output));
	CHECK("star6e output shm socket", output.socket_handle == -1);
	CHECK("star6e output shm ring", output.ring != NULL);
	CHECK("star6e output shm no socket destination", output.dst_len == 0);
	star6e_output_teardown(&output);
	return failures;
}

static int test_star6e_output_udp_send_compact(void)
{
	Star6eOutputSetup setup;
	Star6eOutput output;
	uint8_t packet[20] = {
		0x80, 0xe1, 0x12, 0x34, 0, 0, 0, 1, 0, 0, 0, 2,
		9, 8, 7, 6, 5, 4, 3, 2
	};
	uint8_t buf[32];
	char uri[64];
	uint16_t port;
	ssize_t received;
	int recv_socket;
	int failures = 0;
	int ret;

	recv_socket = create_udp_receiver(&port);
	CHECK("star6e output compact receiver", recv_socket >= 0);
	snprintf(uri, sizeof(uri), "udp://127.0.0.1:%u", port);
	ret = star6e_output_prepare(&setup, uri, "compact", 0);
	CHECK("star6e output compact prepare", ret == 0);
	CHECK("star6e output compact setup not rtp",
		!star6e_output_setup_is_rtp(&setup));
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e output compact init", ret == 0);
	CHECK("star6e output compact is not rtp",
		!star6e_output_is_rtp(&output));
	ret = star6e_output_send_compact_packet(&output, packet, sizeof(packet), 64);
	CHECK("star6e output compact send", ret == 0);
	received = recv(recv_socket, buf, sizeof(buf), 0);
	CHECK("star6e output compact recv size", received == (ssize_t)sizeof(packet));
	CHECK("star6e output compact recv data",
		received == (ssize_t)sizeof(packet) &&
		memcmp(buf, packet, sizeof(packet)) == 0);
	star6e_output_teardown(&output);
	close(recv_socket);
	return failures;
}

static int test_star6e_output_unix_send_rtp(void)
{
	Star6eOutputSetup setup;
	Star6eOutput output;
	uint8_t header[12] = { 0x80, 97, 0, 1, 0, 0, 0, 2, 0, 0, 0, 3 };
	uint8_t payload[4] = { 1, 2, 3, 4 };
	uint8_t buf[16];
	char name[64];
	char uri[96];
	ssize_t received;
	int recv_socket;
	int failures = 0;
	int ret;

	snprintf(name, sizeof(name), "test_star6e_unix_rtp_%ld", (long)getpid());
	recv_socket = create_unix_receiver(name);
	CHECK("star6e unix rtp receiver", recv_socket >= 0);
	snprintf(uri, sizeof(uri), "unix://%s", name);
	ret = star6e_output_prepare(&setup, uri, "rtp", 1);
	CHECK("star6e unix rtp prepare", ret == 0);
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e unix rtp init", ret == 0);
	CHECK("star6e unix rtp transport", output.transport == VENC_OUTPUT_URI_UNIX);
	CHECK("star6e unix rtp connected udp ignored", output.connected_udp == 0);
	ret = star6e_output_send_rtp_parts(&output, header, sizeof(header), payload,
		sizeof(payload), NULL, 0);
	CHECK("star6e unix rtp send", ret == 0);
	received = recv(recv_socket, buf, sizeof(buf), 0);
	CHECK("star6e unix rtp recv size",
		received == (ssize_t)(sizeof(header) + sizeof(payload)));
	CHECK("star6e unix rtp recv data",
		received == (ssize_t)(sizeof(header) + sizeof(payload)) &&
		memcmp(buf, header, sizeof(header)) == 0 &&
		memcmp(buf + sizeof(header), payload, sizeof(payload)) == 0);
	star6e_output_teardown(&output);
	close(recv_socket);
	return failures;
}

static int test_star6e_output_unix_send_compact(void)
{
	Star6eOutputSetup setup;
	Star6eOutput output;
	uint8_t packet[20] = {
		0x80, 0xe1, 0x12, 0x34, 0, 0, 0, 1, 0, 0, 0, 2,
		9, 8, 7, 6, 5, 4, 3, 2
	};
	uint8_t buf[32];
	char name[64];
	char uri[96];
	ssize_t received;
	int recv_socket;
	int failures = 0;
	int ret;

	snprintf(name, sizeof(name), "test_star6e_unix_compact_%ld", (long)getpid());
	recv_socket = create_unix_receiver(name);
	CHECK("star6e unix compact receiver", recv_socket >= 0);
	snprintf(uri, sizeof(uri), "unix://%s", name);
	ret = star6e_output_prepare(&setup, uri, "compact", 0);
	CHECK("star6e unix compact prepare", ret == 0);
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e unix compact init", ret == 0);
	CHECK("star6e unix compact transport", output.transport == VENC_OUTPUT_URI_UNIX);
	ret = star6e_output_send_compact_packet(&output, packet, sizeof(packet), 64);
	CHECK("star6e unix compact send", ret == 0);
	received = recv(recv_socket, buf, sizeof(buf), 0);
	CHECK("star6e unix compact recv size", received == (ssize_t)sizeof(packet));
	CHECK("star6e unix compact recv data",
		received == (ssize_t)sizeof(packet) &&
		memcmp(buf, packet, sizeof(packet)) == 0);
	star6e_output_teardown(&output);
	close(recv_socket);
	return failures;
}

static int test_star6e_output_send_frame_rtp_dispatch(void)
{
	Star6eOutputSetup setup;
	Star6eOutput output;
	MI_VENC_Pack_t pack = {0};
	MI_VENC_Stream_t stream = {0};
	uint8_t data[4] = { 0 };
	size_t expected = 1234;
	size_t actual;
	int failures = 0;
	int ret;

	g_test_star6e_rtp_send_called = 0;
	g_test_star6e_rtp_send_valid = 0;
	pack.data = data;
	pack.length = sizeof(data);
	stream.count = 1;
	stream.packet = &pack;
	ret = star6e_output_prepare(&setup, "udp://127.0.0.1:5600", "rtp", 0);
	CHECK("star6e output send frame rtp prepare", ret == 0);
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e output send frame rtp init", ret == 0);
	actual = star6e_output_send_frame(&output, &stream, 1400,
		test_star6e_output_rtp_send_stub, &expected);
	CHECK("star6e output send frame rtp callback called",
		g_test_star6e_rtp_send_called == 1);
	CHECK("star6e output send frame rtp callback args",
		g_test_star6e_rtp_send_valid == 1);
	CHECK("star6e output send frame rtp return", actual == expected);
	star6e_output_teardown(&output);
	return failures;
}

static int test_star6e_output_udp_send_compact_frame(void)
{
	Star6eOutputSetup setup;
	Star6eOutput output;
	MI_VENC_Pack_t packs[2];
	MI_VENC_Stream_t stream = {0};
	uint8_t data_a[10] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };
	uint8_t data_b[12] = { 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21 };
	uint8_t buf[32];
	char uri[64];
	uint16_t port;
	ssize_t received;
	size_t total_bytes;
	int recv_socket;
	int failures = 0;
	int ret;

	memset(packs, 0, sizeof(packs));
	packs[0].data = data_a;
	packs[0].length = sizeof(data_a);
	packs[0].packNum = 2;
	packs[0].packetInfo[0].offset = 1;
	packs[0].packetInfo[0].length = 3;
	packs[0].packetInfo[1].offset = 6;
	packs[0].packetInfo[1].length = 4;

	packs[1].data = data_b;
	packs[1].length = sizeof(data_b);
	packs[1].offset = 5;

	stream.count = 2;
	stream.packet = packs;

	recv_socket = create_udp_receiver(&port);
	CHECK("star6e output compact frame receiver", recv_socket >= 0);
	snprintf(uri, sizeof(uri), "udp://127.0.0.1:%u", port);
	ret = star6e_output_prepare(&setup, uri, "compact", 0);
	CHECK("star6e output compact frame prepare", ret == 0);
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e output compact frame init", ret == 0);
	total_bytes = star6e_output_send_compact_frame(&output, &stream, 64);
	CHECK("star6e output compact frame bytes", total_bytes == 14);

	received = recv(recv_socket, buf, sizeof(buf), 0);
	CHECK("star6e output compact frame recv 0 size", received == 3);
	CHECK("star6e output compact frame recv 0 data",
		received == 3 && memcmp(buf, data_a + 1, 3) == 0);

	received = recv(recv_socket, buf, sizeof(buf), 0);
	CHECK("star6e output compact frame recv 1 size", received == 4);
	CHECK("star6e output compact frame recv 1 data",
		received == 4 && memcmp(buf, data_a + 6, 4) == 0);

	received = recv(recv_socket, buf, sizeof(buf), 0);
	CHECK("star6e output compact frame recv 2 size", received == 7);
	CHECK("star6e output compact frame recv 2 data",
		received == 7 && memcmp(buf, data_b + 5, 7) == 0);

	star6e_output_teardown(&output);
	close(recv_socket);
	return failures;
}

static int test_star6e_output_send_frame_compact_dispatch(void)
{
	Star6eOutputSetup setup;
	Star6eOutput output;
	MI_VENC_Pack_t pack = {0};
	MI_VENC_Stream_t stream = {0};
	uint8_t data[12] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
	uint8_t buf[32];
	char uri[64];
	uint16_t port;
	ssize_t received;
	size_t total_bytes;
	int recv_socket;
	int failures = 0;
	int ret;

	pack.data = data;
	pack.length = sizeof(data);
	pack.offset = 4;
	stream.count = 1;
	stream.packet = &pack;

	recv_socket = create_udp_receiver(&port);
	CHECK("star6e output send frame compact receiver", recv_socket >= 0);
	snprintf(uri, sizeof(uri), "udp://127.0.0.1:%u", port);
	ret = star6e_output_prepare(&setup, uri, "compact", 0);
	CHECK("star6e output send frame compact prepare", ret == 0);
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e output send frame compact init", ret == 0);
	g_test_star6e_rtp_send_called = 0;
	g_test_star6e_rtp_send_valid = 0;
	total_bytes = star6e_output_send_frame(&output, &stream, 64,
		test_star6e_output_rtp_send_stub, NULL);
	CHECK("star6e output send frame compact callback skipped",
		g_test_star6e_rtp_send_called == 0);
	CHECK("star6e output send frame compact bytes", total_bytes == 8);

	received = recv(recv_socket, buf, sizeof(buf), 0);
	CHECK("star6e output send frame compact recv size", received == 8);
	CHECK("star6e output send frame compact recv data",
		received == 8 && memcmp(buf, data + 4, 8) == 0);

	star6e_output_teardown(&output);
	close(recv_socket);
	return failures;
}

static int test_star6e_output_shm_compact_rejected(void)
{
	char uri[64];
	Star6eOutputSetup setup;
	int failures = 0;
	int ret;

	snprintf(uri, sizeof(uri), "shm://test_star6e_output_bad_%ld", (long)getpid());
	ret = star6e_output_prepare(&setup, uri, "compact", 0);
	CHECK("star6e output shm compact rejected", ret == -1);
	return failures;
}

static int test_star6e_output_shm_apply_server_rejected(void)
{
	char uri[64];
	Star6eOutputSetup setup;
	Star6eOutput output;
	int failures = 0;
	int ret;

	snprintf(uri, sizeof(uri), "shm://test_star6e_output_apply_%ld", (long)getpid());
	ret = star6e_output_prepare(&setup, uri, "rtp", 0);
	CHECK("star6e output shm apply prepare", ret == 0);
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e output shm apply init", ret == 0);
	ret = star6e_output_apply_server(&output, "udp://127.0.0.1:5602");
	CHECK("star6e output shm apply rejected", ret == -1);
	star6e_output_teardown(&output);
	return failures;
}

static int test_star6e_output_empty_server_prepare(void)
{
	Star6eOutputSetup setup;
	Star6eOutput output;
	int failures = 0;
	int ret;

	ret = star6e_output_prepare(&setup, "", "rtp", 0);
	CHECK("star6e output empty prepare", ret == 0);
	CHECK("star6e output empty prepare has no server", setup.has_server == 0);
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e output empty init", ret == 0);
	CHECK("star6e output empty socket", output.socket_handle == -1);
	CHECK("star6e output empty ring", output.ring == NULL);
	star6e_output_teardown(&output);
	return failures;
}

static int test_star6e_output_prepare_defaults_to_rtp(void)
{
	Star6eOutputSetup setup;
	int failures = 0;
	int ret;

	ret = star6e_output_prepare(&setup, "udp://127.0.0.1:5600",
		"unexpected", 0);
	CHECK("star6e output default mode prepare", ret == 0);
	CHECK("star6e output default mode is rtp",
		star6e_output_setup_is_rtp(&setup));
	return failures;
}

static int test_star6e_audio_output_reset_state(void)
{
	Star6eAudioOutput audio_output;
	int failures = 0;

	memset(&audio_output, 0xA5, sizeof(audio_output));
	star6e_audio_output_reset(&audio_output);
	CHECK("star6e audio output reset socket", audio_output.socket_handle == -1);
	CHECK("star6e audio output reset port", star6e_audio_output_port(&audio_output) == 0);
	return failures;
}

static int test_star6e_audio_output_send_rtp(void)
{
	Star6eOutputSetup setup;
	Star6eOutput output;
	Star6eAudioOutput audio_output;
	RtpPacketizerState rtp_state;
	uint8_t payload[5] = { 1, 2, 3, 4, 5 };
	uint8_t buf[32];
	char uri[64];
	uint16_t port;
	ssize_t received;
	int recv_socket;
	int failures = 0;
	int ret;

	recv_socket = create_udp_receiver(&port);
	CHECK("star6e audio rtp receiver", recv_socket >= 0);
	snprintf(uri, sizeof(uri), "udp://127.0.0.1:%u", port);
	ret = star6e_output_prepare(&setup, uri, "rtp", 0);
	CHECK("star6e audio rtp prepare", ret == 0);
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e audio rtp output init", ret == 0);
	ret = star6e_audio_output_init(&audio_output, &output, 0, 1400);
	CHECK("star6e audio rtp init", ret == 0);
	CHECK("star6e audio rtp port", star6e_audio_output_port(&audio_output) == port);

	memset(&rtp_state, 0, sizeof(rtp_state));
	rtp_state.seq = 0x1234;
	rtp_state.timestamp = 0x01020304;
	rtp_state.ssrc = 0x05060708;
	rtp_state.payload_type = 110;

	ret = star6e_audio_output_send_rtp(&audio_output, payload, sizeof(payload),
		&rtp_state, 320);
	CHECK("star6e audio rtp send", ret == 0);

	received = recv(recv_socket, buf, sizeof(buf), 0);
	CHECK("star6e audio rtp recv size",
		received == (ssize_t)(12 + sizeof(payload)));
	CHECK("star6e audio rtp payload type",
		received >= 12 && buf[1] == 110);
	CHECK("star6e audio rtp seq",
		received >= 4 && buf[2] == 0x12 && buf[3] == 0x34);
	CHECK("star6e audio rtp timestamp",
		received >= 8 && buf[4] == 0x01 && buf[5] == 0x02 &&
		buf[6] == 0x03 && buf[7] == 0x04);
	CHECK("star6e audio rtp ssrc",
		received >= 12 && buf[8] == 0x05 && buf[9] == 0x06 &&
		buf[10] == 0x07 && buf[11] == 0x08);
	CHECK("star6e audio rtp payload",
		received >= (ssize_t)(12 + sizeof(payload)) &&
		memcmp(buf + 12, payload, sizeof(payload)) == 0);
	CHECK("star6e audio rtp seq advance", rtp_state.seq == 0x1235);
	CHECK("star6e audio rtp timestamp advance",
		rtp_state.timestamp == 0x01020304u + 320u);

	star6e_audio_output_teardown(&audio_output);
	star6e_output_teardown(&output);
	close(recv_socket);
	return failures;
}

static int test_star6e_audio_output_shm_dedicated_local_udp(void)
{
	char uri[64];
	Star6eOutputSetup setup;
	Star6eOutput output;
	Star6eAudioOutput audio_output;
	uint8_t payload[2] = { 3, 1 };
	uint8_t buf[16];
	uint16_t audio_port;
	ssize_t received;
	int recv_socket_audio;
	int failures = 0;
	int ret;

	snprintf(uri, sizeof(uri), "shm://test_star6e_audio_shm_%ld", (long)getpid());
	ret = star6e_output_prepare(&setup, uri, "rtp", 0);
	CHECK("star6e audio shm prepare", ret == 0);
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e audio shm output init", ret == 0);
	recv_socket_audio = create_udp_receiver(&audio_port);
	CHECK("star6e audio shm receiver", recv_socket_audio >= 0);
	ret = star6e_audio_output_init(&audio_output, &output, audio_port, 16);
	CHECK("star6e audio shm dedicated init", ret == 0);
	CHECK("star6e audio shm dedicated port",
		star6e_audio_output_port(&audio_output) == audio_port);
	ret = star6e_audio_output_send_compact(&audio_output, payload, sizeof(payload));
	CHECK("star6e audio shm dedicated send", ret == 0);
	received = recv(recv_socket_audio, buf, sizeof(buf), 0);
	CHECK("star6e audio shm dedicated recv size", received == 6);
	CHECK("star6e audio shm dedicated recv data",
		received == 6 && buf[0] == 0xAA && buf[1] == 0x01 &&
		buf[2] == 0x00 && buf[3] == 0x02 &&
		memcmp(buf + 4, payload, sizeof(payload)) == 0);
	star6e_audio_output_teardown(&audio_output);
	star6e_output_teardown(&output);
	close(recv_socket_audio);
	return failures;
}

static int test_star6e_audio_output_shared_apply_server(void)
{
	Star6eOutputSetup setup;
	Star6eOutput output;
	Star6eAudioOutput audio_output;
	uint8_t payload[3] = { 9, 8, 7 };
	uint8_t buf[16];
	char uri_a[64];
	char uri_b[64];
	uint16_t port_a;
	uint16_t port_b;
	ssize_t received;
	int recv_socket_a;
	int recv_socket_b;
	int failures = 0;
	int ret;

	recv_socket_a = create_udp_receiver(&port_a);
	CHECK("star6e audio shared receiver a", recv_socket_a >= 0);
	recv_socket_b = create_udp_receiver(&port_b);
	CHECK("star6e audio shared receiver b", recv_socket_b >= 0);
	snprintf(uri_a, sizeof(uri_a), "udp://127.0.0.1:%u", port_a);
	snprintf(uri_b, sizeof(uri_b), "udp://127.0.0.1:%u", port_b);

	ret = star6e_output_prepare(&setup, uri_a, "compact", 0);
	CHECK("star6e audio shared prepare", ret == 0);
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e audio shared output init", ret == 0);
	ret = star6e_audio_output_init(&audio_output, &output, 0, 16);
	CHECK("star6e audio shared init", ret == 0);

	ret = star6e_audio_output_send(&audio_output, payload, sizeof(payload), NULL, 0);
	CHECK("star6e audio shared send a", ret == 0);
	received = recv(recv_socket_a, buf, sizeof(buf), 0);
	CHECK("star6e audio shared recv a size", received == 7);
	CHECK("star6e audio shared recv a data",
		received == 7 && buf[0] == 0xAA && buf[1] == 0x01 &&
		buf[2] == 0x00 && buf[3] == 0x03 &&
		memcmp(buf + 4, payload, sizeof(payload)) == 0);

	ret = star6e_output_apply_server(&output, uri_b);
	CHECK("star6e audio shared apply server", ret == 0);
	CHECK("star6e audio shared port update",
		star6e_audio_output_port(&audio_output) == port_b);
	ret = star6e_audio_output_send(&audio_output, payload, sizeof(payload), NULL, 0);
	CHECK("star6e audio shared send b", ret == 0);
	received = recv(recv_socket_b, buf, sizeof(buf), 0);
	CHECK("star6e audio shared recv b size", received == 7);
	CHECK("star6e audio shared recv b data",
		received == 7 && buf[0] == 0xAA && buf[1] == 0x01 &&
		buf[2] == 0x00 && buf[3] == 0x03 &&
		memcmp(buf + 4, payload, sizeof(payload)) == 0);

	star6e_audio_output_teardown(&audio_output);
	star6e_output_teardown(&output);
	close(recv_socket_b);
	close(recv_socket_a);
	return failures;
}

static int test_star6e_audio_output_shared_switch_udp_to_unix(void)
{
	Star6eOutputSetup setup;
	Star6eOutput output;
	Star6eAudioOutput audio_output;
	uint8_t payload[3] = { 7, 6, 5 };
	uint8_t buf[16];
	char udp_uri[64];
	char unix_name[64];
	char unix_uri[96];
	uint16_t udp_port;
	ssize_t received;
	int udp_socket;
	int unix_socket;
	int failures = 0;
	int ret;

	udp_socket = create_udp_receiver(&udp_port);
	CHECK("star6e audio udp->unix receiver udp", udp_socket >= 0);
	snprintf(unix_name, sizeof(unix_name), "test_star6e_audio_udp_unix_%ld",
		(long)getpid());
	unix_socket = create_unix_receiver(unix_name);
	CHECK("star6e audio udp->unix receiver unix", unix_socket >= 0);
	snprintf(udp_uri, sizeof(udp_uri), "udp://127.0.0.1:%u", udp_port);
	snprintf(unix_uri, sizeof(unix_uri), "unix://%s", unix_name);

	ret = star6e_output_prepare(&setup, udp_uri, "compact", 0);
	CHECK("star6e audio udp->unix prepare", ret == 0);
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e audio udp->unix output init", ret == 0);
	ret = star6e_audio_output_init(&audio_output, &output, 0, 16);
	CHECK("star6e audio udp->unix audio init", ret == 0);

	ret = star6e_audio_output_send(&audio_output, payload, sizeof(payload), NULL, 0);
	CHECK("star6e audio udp->unix send udp", ret == 0);
	received = recv(udp_socket, buf, sizeof(buf), 0);
	CHECK("star6e audio udp->unix recv udp size", received == 7);

	ret = star6e_output_apply_server(&output, unix_uri);
	CHECK("star6e audio udp->unix apply unix", ret == 0);
	CHECK("star6e audio udp->unix shared port reset",
		star6e_audio_output_port(&audio_output) == 0);
	ret = star6e_audio_output_send(&audio_output, payload, sizeof(payload), NULL, 0);
	CHECK("star6e audio udp->unix send unix", ret == 0);
	received = recv(unix_socket, buf, sizeof(buf), 0);
	CHECK("star6e audio udp->unix recv unix size", received == 7);
	CHECK("star6e audio udp->unix recv unix data",
		received == 7 && buf[0] == 0xAA && buf[1] == 0x01 &&
		buf[2] == 0x00 && buf[3] == 0x03 &&
		memcmp(buf + 4, payload, sizeof(payload)) == 0);

	star6e_audio_output_teardown(&audio_output);
	star6e_output_teardown(&output);
	close(unix_socket);
	close(udp_socket);
	return failures;
}

static int test_star6e_audio_output_dedicated_port(void)
{
	Star6eOutputSetup setup;
	Star6eOutput output;
	Star6eAudioOutput audio_output;
	uint8_t payload[2] = { 4, 2 };
	uint8_t buf[16];
	char video_uri_a[64];
	char video_uri_b[64];
	uint16_t video_port_a;
	uint16_t video_port_b;
	uint16_t audio_port;
	ssize_t received;
	int recv_socket_video_a;
	int recv_socket_video_b;
	int recv_socket_audio;
	int failures = 0;
	int ret;

	recv_socket_video_a = create_udp_receiver(&video_port_a);
	CHECK("star6e audio dedicated receiver video a", recv_socket_video_a >= 0);
	recv_socket_video_b = create_udp_receiver(&video_port_b);
	CHECK("star6e audio dedicated receiver video b", recv_socket_video_b >= 0);
	recv_socket_audio = create_udp_receiver(&audio_port);
	CHECK("star6e audio dedicated receiver audio", recv_socket_audio >= 0);

	snprintf(video_uri_a, sizeof(video_uri_a), "udp://127.0.0.1:%u", video_port_a);
	snprintf(video_uri_b, sizeof(video_uri_b), "udp://127.0.0.1:%u", video_port_b);

	ret = star6e_output_prepare(&setup, video_uri_a, "compact", 0);
	CHECK("star6e audio dedicated prepare", ret == 0);
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e audio dedicated output init", ret == 0);
	ret = star6e_audio_output_init(&audio_output, &output, audio_port, 16);
	CHECK("star6e audio dedicated init", ret == 0);
	CHECK("star6e audio dedicated port",
		star6e_audio_output_port(&audio_output) == audio_port);

	ret = star6e_output_apply_server(&output, video_uri_b);
	CHECK("star6e audio dedicated apply server", ret == 0);
	CHECK("star6e audio dedicated port stable",
		star6e_audio_output_port(&audio_output) == audio_port);
	ret = star6e_audio_output_send_compact(&audio_output, payload, sizeof(payload));
	CHECK("star6e audio dedicated send", ret == 0);

	received = recv(recv_socket_audio, buf, sizeof(buf), 0);
	CHECK("star6e audio dedicated recv size", received == 6);
	CHECK("star6e audio dedicated recv data",
		received == 6 && buf[0] == 0xAA && buf[1] == 0x01 &&
		buf[2] == 0x00 && buf[3] == 0x02 &&
		memcmp(buf + 4, payload, sizeof(payload)) == 0);

	star6e_audio_output_teardown(&audio_output);
	star6e_output_teardown(&output);
	close(recv_socket_audio);
	close(recv_socket_video_b);
	close(recv_socket_video_a);
	return failures;
}

static int test_star6e_audio_output_unix_dedicated_local_udp(void)
{
	Star6eOutputSetup setup;
	Star6eOutput output;
	Star6eAudioOutput audio_output;
	uint8_t payload[2] = { 4, 2 };
	uint8_t buf[16];
	char unix_name_a[64];
	char unix_name_b[64];
	char unix_uri_a[96];
	char unix_uri_b[96];
	uint16_t audio_port;
	ssize_t received;
	int recv_socket_audio;
	int recv_socket_unix_a;
	int recv_socket_unix_b;
	int failures = 0;
	int ret;

	snprintf(unix_name_a, sizeof(unix_name_a), "test_star6e_audio_unix_a_%ld",
		(long)getpid());
	snprintf(unix_name_b, sizeof(unix_name_b), "test_star6e_audio_unix_b_%ld",
		(long)getpid());
	recv_socket_unix_a = create_unix_receiver(unix_name_a);
	CHECK("star6e audio unix dedicated receiver a", recv_socket_unix_a >= 0);
	recv_socket_unix_b = create_unix_receiver(unix_name_b);
	CHECK("star6e audio unix dedicated receiver b", recv_socket_unix_b >= 0);
	recv_socket_audio = create_udp_receiver(&audio_port);
	CHECK("star6e audio unix dedicated receiver audio", recv_socket_audio >= 0);
	snprintf(unix_uri_a, sizeof(unix_uri_a), "unix://%s", unix_name_a);
	snprintf(unix_uri_b, sizeof(unix_uri_b), "unix://%s", unix_name_b);

	ret = star6e_output_prepare(&setup, unix_uri_a, "compact", 0);
	CHECK("star6e audio unix dedicated prepare", ret == 0);
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e audio unix dedicated output init", ret == 0);
	ret = star6e_audio_output_init(&audio_output, &output, audio_port, 16);
	CHECK("star6e audio unix dedicated init", ret == 0);
	CHECK("star6e audio unix dedicated port",
		star6e_audio_output_port(&audio_output) == audio_port);

	ret = star6e_audio_output_send_compact(&audio_output, payload, sizeof(payload));
	CHECK("star6e audio unix dedicated send a", ret == 0);
	received = recv(recv_socket_audio, buf, sizeof(buf), 0);
	CHECK("star6e audio unix dedicated recv a size", received == 6);
	CHECK("star6e audio unix dedicated recv a data",
		received == 6 && buf[0] == 0xAA && buf[1] == 0x01 &&
		buf[2] == 0x00 && buf[3] == 0x02 &&
		memcmp(buf + 4, payload, sizeof(payload)) == 0);

	ret = star6e_output_apply_server(&output, unix_uri_b);
	CHECK("star6e audio unix dedicated apply b", ret == 0);
	ret = star6e_audio_output_send_compact(&audio_output, payload, sizeof(payload));
	CHECK("star6e audio unix dedicated send b", ret == 0);
	received = recv(recv_socket_audio, buf, sizeof(buf), 0);
	CHECK("star6e audio unix dedicated recv b size", received == 6);

	star6e_audio_output_teardown(&audio_output);
	star6e_output_teardown(&output);
	close(recv_socket_audio);
	close(recv_socket_unix_b);
	close(recv_socket_unix_a);
	return failures;
}

static int test_star6e_audio_output_shared_teardown_keeps_video_socket(void)
{
	Star6eOutputSetup setup;
	Star6eOutput output;
	Star6eAudioOutput audio_output;
	uint8_t header[12] = { 0x80, 97, 0, 1, 0, 0, 0, 2, 0, 0, 0, 3 };
	uint8_t payload[4] = { 1, 2, 3, 4 };
	uint8_t buf[16];
	char uri[64];
	uint16_t port;
	ssize_t received;
	int recv_socket;
	int failures = 0;
	int ret;

	recv_socket = create_udp_receiver(&port);
	CHECK("star6e audio shared teardown receiver", recv_socket >= 0);
	snprintf(uri, sizeof(uri), "udp://127.0.0.1:%u", port);
	ret = star6e_output_prepare(&setup, uri, "rtp", 0);
	CHECK("star6e audio shared teardown prepare", ret == 0);
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e audio shared teardown output init", ret == 0);
	ret = star6e_audio_output_init(&audio_output, &output, 0, 1400);
	CHECK("star6e audio shared teardown init", ret == 0);

	star6e_audio_output_teardown(&audio_output);
	ret = star6e_output_send_rtp_parts(&output, header, sizeof(header), payload,
		sizeof(payload), NULL, 0);
	CHECK("star6e audio shared teardown video send", ret == 0);
	received = recv(recv_socket, buf, sizeof(buf), 0);
	CHECK("star6e audio shared teardown recv size",
		received == (ssize_t)(sizeof(header) + sizeof(payload)));

	star6e_output_teardown(&output);
	close(recv_socket);
	return failures;
}

static int test_audio_target_cache_gen_tracks_transport(void)
{
	Star6eOutputSetup setup;
	Star6eOutput output;
	Star6eAudioOutput audio_output;
	uint8_t payload[2] = { 0xAB, 0xCD };
	uint8_t buf[16];
	char uri_a[64];
	char uri_b[64];
	uint16_t port_a;
	uint16_t port_b;
	uint32_t gen_after_init;
	ssize_t received;
	int recv_a;
	int recv_b;
	int failures = 0;
	int ret;

	recv_a = create_udp_receiver(&port_a);
	CHECK("cache gen receiver a", recv_a >= 0);
	recv_b = create_udp_receiver(&port_b);
	CHECK("cache gen receiver b", recv_b >= 0);
	snprintf(uri_a, sizeof(uri_a), "udp://127.0.0.1:%u", port_a);
	snprintf(uri_b, sizeof(uri_b), "udp://127.0.0.1:%u", port_b);

	ret = star6e_output_prepare(&setup, uri_a, "compact", 0);
	CHECK("cache gen prepare", ret == 0);
	ret = star6e_output_init(&output, &setup);
	CHECK("cache gen output init", ret == 0);
	gen_after_init = output.transport_gen;
	CHECK("cache gen init > 0", gen_after_init > 0);

	ret = star6e_audio_output_init(&audio_output, &output, 0, 16);
	CHECK("cache gen audio init", ret == 0);
	CHECK("cache starts invalid", audio_output.cache_valid == 0);

	ret = star6e_audio_output_send(&audio_output, payload, sizeof(payload), NULL, 0);
	CHECK("cache gen send a", ret == 0);
	received = recv(recv_a, buf, sizeof(buf), 0);
	CHECK("cache gen recv a", received == 6);
	CHECK("cache now valid", audio_output.cache_valid == 1);
	CHECK("cached gen matches", audio_output.cached_gen == gen_after_init);

	ret = star6e_output_apply_server(&output, uri_b);
	CHECK("cache gen apply server", ret == 0);
	CHECK("gen incremented", output.transport_gen == gen_after_init + 2);

	ret = star6e_audio_output_send(&audio_output, payload, sizeof(payload), NULL, 0);
	CHECK("cache gen send b", ret == 0);
	received = recv(recv_b, buf, sizeof(buf), 0);
	CHECK("cache gen recv b", received == 6);
	CHECK("cached gen updated", audio_output.cached_gen == gen_after_init + 2);

	star6e_audio_output_teardown(&audio_output);
	star6e_output_teardown(&output);
	close(recv_b);
	close(recv_a);
	return failures;
}

static int test_audio_target_cache_hit_stable(void)
{
	Star6eOutputSetup setup;
	Star6eOutput output;
	Star6eAudioOutput audio_output;
	uint8_t payload[2] = { 0x11, 0x22 };
	uint8_t buf[16];
	char uri[64];
	uint16_t port;
	uint32_t gen_after_init;
	ssize_t received;
	int recv_socket;
	int failures = 0;
	int ret;

	recv_socket = create_udp_receiver(&port);
	CHECK("cache hit receiver", recv_socket >= 0);
	snprintf(uri, sizeof(uri), "udp://127.0.0.1:%u", port);

	ret = star6e_output_prepare(&setup, uri, "compact", 0);
	CHECK("cache hit prepare", ret == 0);
	ret = star6e_output_init(&output, &setup);
	CHECK("cache hit output init", ret == 0);
	gen_after_init = output.transport_gen;

	ret = star6e_audio_output_init(&audio_output, &output, 0, 16);
	CHECK("cache hit audio init", ret == 0);

	ret = star6e_audio_output_send(&audio_output, payload, sizeof(payload), NULL, 0);
	CHECK("cache hit send 1", ret == 0);
	received = recv(recv_socket, buf, sizeof(buf), 0);
	CHECK("cache hit recv 1", received == 6);
	CHECK("cache valid after send 1", audio_output.cache_valid == 1);
	CHECK("cached gen after send 1", audio_output.cached_gen == gen_after_init);

	ret = star6e_audio_output_send(&audio_output, payload, sizeof(payload), NULL, 0);
	CHECK("cache hit send 2", ret == 0);
	received = recv(recv_socket, buf, sizeof(buf), 0);
	CHECK("cache hit recv 2", received == 6);
	CHECK("gen unchanged", output.transport_gen == gen_after_init);
	CHECK("cache still valid", audio_output.cache_valid == 1);

	star6e_audio_output_teardown(&audio_output);
	star6e_output_teardown(&output);
	close(recv_socket);
	return failures;
}

/* Drive a SHM ring to a target fill % by writing directly via the ring
 * helper, then call star6e_output_observe_pressure and assert the
 * hysteresis state machine.  Skip-on-pressure was rolled back (broke
 * H.265 reference chains); the observation API is telemetry-only now. */
static void fill_ring_to_pct(venc_ring_t *ring, uint8_t target_pct)
{
	uint64_t w = __atomic_load_n(&ring->hdr->write_idx, __ATOMIC_RELAXED);
	uint64_t rd = __atomic_load_n(&ring->hdr->read_idx, __ATOMIC_RELAXED);
	uint32_t want = (uint32_t)((uint64_t)ring->hdr->slot_count *
		target_pct / 100u);
	uint32_t cur_used = (uint32_t)(w - rd);
	uint8_t pkt[16] = { 0 };

	while (cur_used < want) {
		if (venc_ring_write3(ring, pkt, sizeof(pkt), NULL, 0, NULL, 0) != 0)
			break;
		cur_used++;
	}
}

static void drain_ring_to_pct(venc_ring_t *ring, uint8_t target_pct)
{
	uint64_t w = __atomic_load_n(&ring->hdr->write_idx, __ATOMIC_RELAXED);
	uint64_t rd = __atomic_load_n(&ring->hdr->read_idx, __ATOMIC_RELAXED);
	uint32_t want = (uint32_t)((uint64_t)ring->hdr->slot_count *
		target_pct / 100u);
	uint32_t cur_used = (uint32_t)(w - rd);

	while (cur_used > want) {
		__atomic_store_n(&ring->hdr->read_idx, rd + 1, __ATOMIC_RELEASE);
		rd++;
		cur_used--;
	}
}

static int test_star6e_output_backpressure_hysteresis(void)
{
	char uri[64];
	Star6eOutputSetup setup;
	Star6eOutput output;
	int failures = 0;
	uint32_t base_drops;

	snprintf(uri, sizeof(uri), "shm://test_star6e_bp_%ld", (long)getpid());
	CHECK("bp prepare",
		star6e_output_prepare(&setup, uri, "rtp", 0) == 0);
	CHECK("bp init", star6e_output_init(&output, &setup) == 0);

	/* Empty ring — must not enter pressure. */
	star6e_output_observe_pressure(&output);
	CHECK("bp empty in_pressure", output.in_pressure == 0);
	CHECK("bp empty drops", output.pressure_drops == 0);
	CHECK("bp empty cached fill", output.last_fill_pct == 0);

	/* Fill below high water (74%) — still no pressure. */
	fill_ring_to_pct(output.ring, 74);
	star6e_output_observe_pressure(&output);
	CHECK("bp 74pct in_pressure", output.in_pressure == 0);
	CHECK("bp 74pct drops unchanged", output.pressure_drops == 0);

	/* Fill at high water (75%) — enter pressure, drops counter ticks. */
	fill_ring_to_pct(output.ring, 75);
	star6e_output_observe_pressure(&output);
	CHECK("bp 75pct in_pressure", output.in_pressure == 1);
	base_drops = output.pressure_drops;
	CHECK("bp 75pct drop counted", base_drops == 1);
	CHECK("bp 75pct cached fill", output.last_fill_pct >= 75);

	/* Drain to between low and high — must STAY in pressure (hysteresis). */
	drain_ring_to_pct(output.ring, 60);
	star6e_output_observe_pressure(&output);
	CHECK("bp 60pct still in_pressure", output.in_pressure == 1);
	CHECK("bp 60pct drop incremented", output.pressure_drops == base_drops + 1);

	/* Drain at low water (50%) — must STAY in pressure (strict <). */
	drain_ring_to_pct(output.ring, 50);
	star6e_output_observe_pressure(&output);
	CHECK("bp 50pct still in_pressure", output.in_pressure == 1);

	/* Drain below low water (49%) — exit pressure. */
	drain_ring_to_pct(output.ring, 49);
	star6e_output_observe_pressure(&output);
	CHECK("bp 49pct in_pressure cleared", output.in_pressure == 0);

	/* Non-SHM output (UDP) — observation runs but stays at 0% fill (no
	 * receiver to back the kernel send queue up). */
	star6e_output_teardown(&output);
	{
		uint16_t port = 0;
		int recv_fd = create_udp_receiver(&port);
		Star6eOutput udp_out;
		char udp_uri[64];

		CHECK("bp udp recv socket", recv_fd >= 0);
		snprintf(udp_uri, sizeof(udp_uri), "udp://127.0.0.1:%u", port);
		CHECK("bp udp prepare",
			star6e_output_prepare(&setup, udp_uri, "rtp", 0) == 0);
		CHECK("bp udp init", star6e_output_init(&udp_out, &setup) == 0);
		star6e_output_observe_pressure(&udp_out);
		CHECK("bp udp in_pressure", udp_out.in_pressure == 0);
		star6e_output_teardown(&udp_out);
		if (recv_fd >= 0)
			close(recv_fd);
	}

	return failures;
}

/* UNIX datagram backpressure: with a receiver bound but not reading, the
 * sender's SIOCOUTQ rises until SO_SNDBUF caps it.  We send raw bytes
 * directly through output->socket_handle so the test doesn't depend on
 * the higher-level RTP send path; the hysteresis state machine itself
 * is already covered by the SHM test.  This test specifically validates
 * the UNIX-fill plumbing and the observe_pressure transport switch. */
static int test_star6e_output_unix_backpressure(void)
{
	char abstract_name[64];
	char uri[80];
	Star6eOutputSetup setup;
	Star6eOutput output;
	int recv_fd = -1;
	int failures = 0;

	snprintf(abstract_name, sizeof(abstract_name),
		"test_unix_bp_%ld", (long)getpid());
	recv_fd = create_unix_receiver(abstract_name);
	CHECK("unix bp recv socket", recv_fd >= 0);

	snprintf(uri, sizeof(uri), "unix://%s", abstract_name);
	CHECK("unix bp prepare",
		star6e_output_prepare(&setup, uri, "rtp", 0) == 0);
	CHECK("unix bp init", star6e_output_init(&output, &setup) == 0);
	CHECK("unix bp transport",
		output.transport == VENC_OUTPUT_URI_UNIX);

	/* Empty queue — must not enter pressure. */
	star6e_output_observe_pressure(&output);
	CHECK("unix bp empty in_pressure", output.in_pressure == 0);

	/* Stuff the queue with MTU-sized RTP payloads — the size that
	 * actually ships — until the kernel refuses.  A saturated queue must
	 * be reported as saturated: the earlier 4096-byte per-skb truesize
	 * estimate over-counted 1400-byte datagrams by ~1.8x and capped the
	 * reportable fill at 61 %, so a fully blocked socket never crossed
	 * the 75 % high-water mark and unix:// backpressure never fired. */
	{
		char payload[1400];
		int sent = 0;
		int flags;

		memset(payload, 'X', sizeof(payload));
		flags = fcntl(output.socket_handle, F_GETFL, 0);
		(void)fcntl(output.socket_handle, F_SETFL, flags | O_NONBLOCK);
		for (int i = 0; i < 4096; i++) {
			ssize_t n = sendto(output.socket_handle, payload,
				sizeof(payload), 0,
				(const struct sockaddr *)&output.dst,
				output.dst_len);
			if (n > 0) sent++;
			else break;
		}
		CHECK("unix bp pumped some packets", sent > 0);
		(void)fcntl(output.socket_handle, F_SETFL, flags);
		/* The refusal above is the calibration event the send path
		 * uses in production. */
		output_socket_note_saturation(output.socket_handle,
			&output.send_queue);
	}

	/* A saturated queue must read as saturated and must trip pressure —
	 * unconditionally, not "if it happens to reach 75". */
	{
		uint8_t fill_pct = 0;
		int got_fill = output_socket_get_fill_pct(
			output.socket_handle,
			&output.send_queue, &fill_pct);
		CHECK("unix bp fill_pct readable", got_fill == 0);
		CHECK("unix bp sndbuf capacity captured",
			output.send_queue.sndbuf_capacity > 0);
		CHECK("unix bp capacity calibrated",
			output.send_queue.unix_capacity > 0);
		CHECK("unix bp saturated reads >= high water",
			fill_pct >= VENC_PRESSURE_HIGH_WATER_PCT);

		star6e_output_observe_pressure(&output);
		CHECK("unix bp entered pressure", output.in_pressure == 1);
		CHECK("unix bp drop counted", output.pressure_drops > 0);
		CHECK("unix bp cached fill",
			output.last_fill_pct >= VENC_PRESSURE_HIGH_WATER_PCT);
	}

	/* Draining the receiver must release the pressure flag — proves the
	 * calibrated denominator tracks both directions, not just a value
	 * pinned at 100. */
	{
		char sink[2048];
		uint8_t fill_pct = 100;

		while (recv(recv_fd, sink, sizeof(sink), MSG_DONTWAIT) > 0)
			;
		CHECK("unix bp drained fill readable",
			output_socket_get_fill_pct(output.socket_handle,
				&output.send_queue, &fill_pct) == 0);
		CHECK("unix bp drained below low water",
			fill_pct < VENC_PRESSURE_LOW_WATER_PCT);
		star6e_output_observe_pressure(&output);
		CHECK("unix bp left pressure", output.in_pressure == 0);
	}

	star6e_output_teardown(&output);
	if (recv_fd >= 0)
		close(recv_fd);
	return failures;
}

/* A wedged unix:// consumer must not stall the encode thread.
 *
 * These sends run between MI_VENC_GetStream and MI_VENC_ReleaseStream, so
 * an unbounded block holds a VENC output slot and cascades into dropped
 * capture frames.  Before SO_SNDTIMEO + the per-frame flush deadline, a
 * consumer that stopped reading blocked the producer indefinitely.
 *
 * The bound asserted here is deliberately loose (250 ms for a frame that
 * should take ~4 ms) so the test cannot flake on a loaded CI box while
 * still failing outright if the bound is ever removed. */
static int test_star6e_output_unix_flush_is_bounded(void)
{
	char abstract_name[64];
	char uri[80];
	Star6eOutputSetup setup;
	Star6eOutput output;
	int recv_fd = -1;
	int failures = 0;
	uint64_t started;
	uint64_t elapsed;
	uint8_t hdr[12];
	uint8_t payload[1400];

	memset(hdr, 0, sizeof(hdr));
	hdr[0] = 0x80;
	memset(payload, 'Z', sizeof(payload));

	snprintf(abstract_name, sizeof(abstract_name),
		"test_unix_bound_%ld", (long)getpid());
	recv_fd = create_unix_receiver(abstract_name);
	CHECK("unix bound recv socket", recv_fd >= 0);

	snprintf(uri, sizeof(uri), "unix://%s", abstract_name);
	CHECK("unix bound prepare",
		star6e_output_prepare(&setup, uri, "rtp", 0) == 0);
	CHECK("unix bound init", star6e_output_init(&output, &setup) == 0);
	{
		struct timeval timeout = {0};
		socklen_t timeout_len = sizeof(timeout);

		CHECK("unix bound timeout readable",
			getsockopt(output.socket_handle, SOL_SOCKET, SO_SNDTIMEO,
				&timeout, &timeout_len) == 0);
		CHECK("unix bound timeout enabled",
			timeout.tv_sec != 0 || timeout.tv_usec != 0);
	}

	/* Fill the peer queue before starting the frame so the very first batch
	 * hits congestion. This makes the per-frame budget assertion independent
	 * of the host's configured max_dgram_qlen. */
	{
		int flags = fcntl(output.socket_handle, F_GETFL, 0);

		(void)fcntl(output.socket_handle, F_SETFL, flags | O_NONBLOCK);
		for (;;) {
			ssize_t n = sendto(output.socket_handle, payload,
				sizeof(payload), 0,
				(const struct sockaddr *)&output.dst,
				output.dst_len);
			if (n < 0)
				break;
		}
		(void)fcntl(output.socket_handle, F_SETFL, flags);
	}

	/* A frame can exceed the 64-message sendmmsg batch. The 4 ms budget is
	 * cumulative across those internal flushes: after it is exhausted, all
	 * later packets in this same frame are counted and discarded without
	 * starting another 4 ms window. */
	started = wb_monotonic_us();
	star6e_output_begin_frame(&output);
	for (int i = 0; i < STAR6E_OUTPUT_BATCH_MAX * 4; i++) {
		(void)star6e_output_send_rtp_parts(&output,
			hdr, sizeof(hdr), payload, sizeof(payload),
			NULL, 0);
	}
	(void)star6e_output_end_frame(&output);
	elapsed = wb_monotonic_us() - started;

	CHECK("unix bound flush did not hang", elapsed < 50000);
	CHECK("unix bound exhausted one frame budget",
		output.batch.flush_budget_us == 0);
	CHECK("unix bound discarded rest of frame",
		output.batch.discard_remaining == 1);
	CHECK("unix bound counted transport drops",
		__atomic_load_n(&output.socket_drops, __ATOMIC_RELAXED) > 0);
	CHECK("unix bound congestion not miscounted as error",
		output.send_errors == 0);
	CHECK("unix bound calibrated capacity",
		output.send_queue.unix_capacity > 0);

	star6e_output_teardown(&output);
	if (recv_fd >= 0)
		close(recv_fd);
	return failures;
}

/* Compatibility mode deliberately restores the old blocking contract: a
 * full unix:// peer queue holds the producer until the consumer resumes.
 * The delayed drain proves that the send actually waits, then completes the
 * entire frame without converting queue pressure into transport drops. */
static int test_star6e_output_unix_stall_mode_resumes_without_drops(void)
{
	char abstract_name[64];
	char uri[80];
	Star6eOutputSetup setup;
	Star6eOutput output;
	UnixDrainThread drain = {0};
	pthread_t drain_thread;
	int recv_fd = -1;
	int failures = 0;
	int thread_rc;
	uint64_t started;
	uint64_t elapsed;
	uint8_t hdr[12] = {0};
	uint8_t payload[1400];

	hdr[0] = 0x80;
	memset(payload, 'S', sizeof(payload));
	snprintf(abstract_name, sizeof(abstract_name),
		"test_unix_stall_%ld", (long)getpid());
	recv_fd = create_unix_receiver(abstract_name);
	CHECK("unix stall recv socket", recv_fd >= 0);

	snprintf(uri, sizeof(uri), "unix://%s", abstract_name);
	CHECK("unix stall prepare",
		star6e_output_prepare(&setup, uri, "rtp", 0) == 0);
	setup.allow_unix_encoder_stall = 1;
	CHECK("unix stall init", star6e_output_init(&output, &setup) == 0);
	{
		struct timeval timeout = { .tv_sec = 1, .tv_usec = 1 };
		socklen_t timeout_len = sizeof(timeout);

		CHECK("unix stall timeout readable",
			getsockopt(output.socket_handle, SOL_SOCKET, SO_SNDTIMEO,
				&timeout, &timeout_len) == 0);
		CHECK("unix stall timeout disabled",
			timeout.tv_sec == 0 && timeout.tv_usec == 0);
	}

	/* Saturate the queue before sending the test frame. */
	{
		int flags = fcntl(output.socket_handle, F_GETFL, 0);

		(void)fcntl(output.socket_handle, F_SETFL, flags | O_NONBLOCK);
		while (sendto(output.socket_handle, payload, sizeof(payload), 0,
		    (const struct sockaddr *)&output.dst, output.dst_len) > 0)
			;
		(void)fcntl(output.socket_handle, F_SETFL, flags);
	}

	drain.socket_handle = recv_fd;
	thread_rc = pthread_create(&drain_thread, NULL, delayed_unix_drain, &drain);
	CHECK("unix stall drain thread", thread_rc == 0);
	if (thread_rc != 0) {
		star6e_output_teardown(&output);
		close(recv_fd);
		return failures;
	}

	started = wb_monotonic_us();
	star6e_output_begin_frame(&output);
	for (int i = 0; i < STAR6E_OUTPUT_BATCH_MAX * 4; i++) {
		(void)star6e_output_send_rtp_parts(&output,
			hdr, sizeof(hdr), payload, sizeof(payload), NULL, 0);
	}
	(void)star6e_output_end_frame(&output);
	elapsed = wb_monotonic_us() - started;
	__atomic_store_n(&drain.stop, 1, __ATOMIC_RELEASE);
	pthread_join(drain_thread, NULL);

	CHECK("unix stall waited for consumer", elapsed >= 10000);
	CHECK("unix stall resumed promptly", elapsed < 1000000);
	CHECK("unix stall batch policy selected",
		output.batch.allow_unix_encoder_stall == 1);
	CHECK("unix stall frame kept", output.batch.discard_remaining == 0);
	CHECK("unix stall no transport drops",
		__atomic_load_n(&output.socket_drops, __ATOMIC_RELAXED) == 0);
	CHECK("unix stall no send errors", output.send_errors == 0);
	CHECK("unix stall sent whole frame",
		__atomic_load_n(&output.socket_writes, __ATOMIC_RELAXED) ==
		STAR6E_OUTPUT_BATCH_MAX * 4);
	CHECK("unix stall consumer drained", drain.packets_read > 0);

	star6e_output_teardown(&output);
	close(recv_fd);
	return failures;
}

/* Regression guard for the v0.9.2 rollback: post-encode frame-skip
 * was removed because it broke the H.264/H.265 reference chain.  This
 * test fills a SHM ring above high_water, observes pressure to assert
 * the flag flips, then writes a packet via star6e_output_send_rtp_parts
 * and confirms the packet actually lands in the ring (i.e. the producer
 * NEVER bails out on the basis of in_pressure).  If a future change
 * re-introduces a skip-on-pressure shortcut anywhere in the send path,
 * this assertion fails immediately. */
static int test_star6e_output_always_sends_under_pressure(void)
{
	char uri[64];
	Star6eOutputSetup setup;
	Star6eOutput output;
	int failures = 0;
	uint64_t writes_before;
	uint64_t writes_after;
	const uint8_t hdr[12] = { 0x80, 0x97, 0x00, 0x00 };
	const uint8_t pay[8] = { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11, 0x22 };

	snprintf(uri, sizeof(uri), "shm://test_star6e_always_send_%ld",
		(long)getpid());
	CHECK("always-send prepare",
		star6e_output_prepare(&setup, uri, "rtp", 0) == 0);
	CHECK("always-send init", star6e_output_init(&output, &setup) == 0);

	/* Drive the ring above high water so observe_pressure asserts. */
	fill_ring_to_pct(output.ring, 80);
	star6e_output_observe_pressure(&output);
	CHECK("always-send pressure asserted",
		output.in_pressure == 1);
	CHECK("always-send drop counted",
		output.pressure_drops > 0);

	/* Drain enough room for one slot but stay above low_water so the
	 * flag remains set when we send. */
	drain_ring_to_pct(output.ring, 70);
	CHECK("always-send still in pressure",
		output.in_pressure == 1);

	/* Snapshot ring writes counter, send a packet, confirm it landed.
	 * If a regression made the producer skip while in pressure,
	 * writes_after would equal writes_before. */
	writes_before = output.ring->stats.writes;
	CHECK("always-send rtp send rc",
		star6e_output_send_rtp_parts(&output,
			hdr, sizeof(hdr),
			pay, sizeof(pay),
			NULL, 0) == 0);
	writes_after = output.ring->stats.writes;
	CHECK("always-send ring write happened",
		writes_after == writes_before + 1);

	star6e_output_teardown(&output);
	return failures;
}

/* ── frame-ring full-drop paths ────────────────────────────────────────── */

static void test_ring_fill_p_frame(MI_VENC_Pack_t *pack,
	MI_VENC_Stream_t *stream, uint8_t *data, size_t len)
{
	memset(pack, 0, sizeof(*pack));
	pack->data = data;
	pack->length = (MI_U32)len;
	pack->packNum = 1;
	pack->packetInfo[0].offset = 0;
	pack->packetInfo[0].length = (MI_U32)len;
	pack->packetInfo[0].packType.h265Nalu = 1;  /* TRAIL_R: breaks chain */
	memset(stream, 0, sizeof(*stream));
	stream->count = 1;
	stream->packet = pack;
}

/* A ring-full drop must discard the frame, count it, and actuate NOTHING.
 * venc measures egress pressure and publishes it; the co-located rate
 * controller (waybeam-link) reads the ring and owns every response.
 *
 * Two separate reasons, and it is worth keeping them apart because only the
 * first is about the ring being full:
 *
 *   1. Categorically, venc no longer requests an IDR on its own -- for any
 *      reason, on any transport.  Recovery belongs to the operator-selected
 *      GOP cadence or to an explicit request (/request/idr, or waybeam-link's
 *      §3.9 RECOVERY_REQUEST from the receiver, which is the only party that
 *      can see whether the decoder is actually broken).
 *   2. In THIS path specifically the request was also futile: it fired when
 *      the ring was full, so the largest frame in the stream could not be
 *      delivered anyway -- measured on a SSC338Q with the consumer stopped,
 *      13 IDRs in 12 s, none of which reached anyone.
 *
 * Reason 2 does NOT extend to the sibling drop paths (a malformed packetInfo
 * table, or an oversize frame aborting mid-append).  Those fire with a ring
 * that has room, and on every transport including plain RTP, so an IDR there
 * would have been delivered.  They are removed under reason 1 alone, and the
 * damage they leave is bounded by GDR rather than by any venc action. */
static int test_star6e_output_frame_ring_full_drop_is_inert(void)
{
	Star6eOutputSetup setup;
	Star6eOutput output;
	MI_VENC_Pack_t pack;
	MI_VENC_Stream_t stream;
	uint8_t data[8] = { 0, 0, 0, 1, 0x02, 0x01, 0xAA, 0xBB };
	int failures = 0;
	int i;

	CHECK("ring drop prepare", star6e_output_prepare(&setup,
		"frame-shm://test_full_drop", "compact", 0) == 0);
	CHECK("ring drop init", star6e_output_init(&output, &setup) == 0);
	test_ring_fill_p_frame(&pack, &stream, data, sizeof(data));

	for (i = 0; i < 8; ++i) {  /* fill all 8 slots */
		CHECK("ring drop fill slot",
			star6e_output_send_frame(&output, &stream, 0,
				NULL, NULL) > 0);
	}
	CHECK("ring drop 9th drops",
		star6e_output_send_frame(&output, &stream, 0, NULL, NULL) == 0);
	CHECK("ring drop counted",
		output.frame_ring->stats.full_drops == 1);

	/* A standing full ring keeps dropping and keeps counting -- no
	 * pacing state, because nothing is being paced. */
	for (i = 0; i < 5; ++i)
		CHECK("ring drop still drops",
			star6e_output_send_frame(&output, &stream, 0,
				NULL, NULL) == 0);
	CHECK("ring drop all counted",
		output.frame_ring->stats.full_drops == 6);
	CHECK("ring drop nothing extra written",
		output.frame_ring->stats.writes == 8);

	star6e_output_teardown(&output);
	return failures;
}

/* A pack reporting more NALs than packetInfo holds must abort the frame —
 * never ship it truncated — and leave the ring usable. */
static int test_star6e_output_frame_ring_truncation_abort(void)
{
	Star6eOutputSetup setup;
	Star6eOutput output;
	MI_VENC_Pack_t pack;
	MI_VENC_Stream_t stream;
	uint8_t data[64] = { 0 };
	size_t sent;
	int failures = 0;
	unsigned int k;

	CHECK("ring trunc prepare", star6e_output_prepare(&setup,
		"frame-shm://test_trunc_abort", "compact", 0) == 0);
	CHECK("ring trunc init", star6e_output_init(&output, &setup) == 0);

	memset(&pack, 0, sizeof(pack));
	pack.data = data;
	pack.length = sizeof(data);
	pack.packNum = 9;  /* one more than the 8-entry packetInfo table */
	for (k = 0; k < 8; ++k) {
		pack.packetInfo[k].offset = k * 4;
		pack.packetInfo[k].length = 4;
		pack.packetInfo[k].packType.h265Nalu = 1;
	}
	memset(&stream, 0, sizeof(stream));
	stream.count = 1;
	stream.packet = &pack;

	sent = star6e_output_send_frame(&output, &stream, 0, NULL, NULL);
	CHECK("ring trunc aborted", sent == 0);
	CHECK("ring trunc warned once", output.trunc_warned == 1);
	CHECK("ring trunc nothing committed",
		output.frame_ring->stats.writes == 0);
	/* The frame is gone; the consumer must be able to SEE that it is gone.
	 * Before other_drops existed this discard left no trace anywhere in
	 * the shared header. */
	CHECK("ring trunc counted locally", output.bad_au_drops == 1);
	CHECK("ring trunc published to consumer",
		output.frame_ring->hdr->other_drops == 1);
	CHECK("ring trunc not conflated with congestion",
		output.frame_ring->hdr->full_drops == 0);

	/* The abort must leave the ring writable. */
	test_ring_fill_p_frame(&pack, &stream, data, 8);
	CHECK("ring trunc ring still usable",
		star6e_output_send_frame(&output, &stream, 0, NULL, NULL) > 0);

	star6e_output_teardown(&output);
	return failures;
}

static int test_star6e_output_packet_info_validation(void)
{
	MI_VENC_Pack_t pack;
	MI_VENC_Stream_t stream;
	uint8_t data[16] = { 0 };
	int failures = 0;

	memset(&pack, 0, sizeof(pack));
	memset(&stream, 0, sizeof(stream));
	CHECK("star packetInfo null stream rejected",
		!star6e_output_stream_packet_info_complete(NULL));
	CHECK("star packetInfo empty stream rejected",
		!star6e_output_stream_packet_info_complete(&stream));

	stream.count = 1;
	stream.packet = &pack;
	CHECK("star packetInfo null data rejected",
		!star6e_output_stream_packet_info_complete(&stream));
	pack.data = data;
	pack.length = sizeof(data);
	CHECK("star packetInfo fallback accepted",
		star6e_output_stream_packet_info_complete(&stream));
	pack.offset = pack.length;
	CHECK("star packetInfo empty fallback rejected",
		!star6e_output_stream_packet_info_complete(&stream));

	pack.offset = 0;
	pack.packNum = 1;
	pack.packetInfo[0].length = 0;
	CHECK("star packetInfo zero descriptor rejected",
		!star6e_output_stream_packet_info_complete(&stream));
	pack.packetInfo[0].offset = pack.length;
	pack.packetInfo[0].length = 1;
	CHECK("star packetInfo offset rejected",
		!star6e_output_stream_packet_info_complete(&stream));
	pack.packetInfo[0].offset = 8;
	pack.packetInfo[0].length = 9;
	CHECK("star packetInfo overrun rejected",
		!star6e_output_stream_packet_info_complete(&stream));
	pack.packetInfo[0].offset = 4;
	pack.packetInfo[0].length = 8;
	CHECK("star packetInfo descriptor accepted",
		star6e_output_stream_packet_info_complete(&stream));

	return failures;
}

/* ── star6e_output_stream_flatten: the async recorder's copy ──────────── */

static int test_star6e_flatten_concatenates_nals_in_order(void)
{
	MI_VENC_Pack_t packs[2];
	MI_VENC_Stream_t stream;
	uint8_t d0[16], d1[8];
	uint8_t *out;
	size_t len = 0;
	int is_idr = -1;
	int failures = 0;
	int i;

	for (i = 0; i < 16; i++) d0[i] = (uint8_t)(0x10 + i);
	for (i = 0; i < 8; i++)  d1[i] = (uint8_t)(0xA0 + i);

	memset(packs, 0, sizeof(packs));
	memset(&stream, 0, sizeof(stream));
	/* pack 0: two descriptors, deliberately not covering the whole pack */
	packs[0].data = d0;
	packs[0].length = 16;
	packs[0].packNum = 2;
	packs[0].packetInfo[0].offset = 0;  packs[0].packetInfo[0].length = 4;
	packs[0].packetInfo[1].offset = 8;  packs[0].packetInfo[1].length = 4;
	/* pack 1: the packNum == 0 fallback, honouring offset */
	packs[1].data = d1;
	packs[1].length = 8;
	packs[1].offset = 2;
	packs[1].packNum = 0;
	stream.packet = packs;
	stream.count = 2;

	out = star6e_output_stream_flatten(&stream, &len, &is_idr);
	CHECK("flatten returned a buffer", out != NULL);
	CHECK("flatten length is the sum of the spans", len == 4 + 4 + 6);
	if (out && len == 14) {
		CHECK("flatten span 1 verbatim", memcmp(out, d0, 4) == 0);
		CHECK("flatten span 2 verbatim", memcmp(out + 4, d0 + 8, 4) == 0);
		CHECK("flatten fallback honours offset",
			memcmp(out + 8, d1 + 2, 6) == 0);
	} else {
		CHECK("flatten payload check reachable", 0);
	}
	CHECK("flatten no IRAP here", is_idr == 0);
	free(out);
	return failures;
}

static int test_star6e_flatten_detects_idr(void)
{
	MI_VENC_Pack_t pack;
	MI_VENC_Stream_t stream;
	uint8_t data[16] = { 0 };
	uint8_t *out;
	size_t len = 0;
	int is_idr = 0;
	int failures = 0;

	memset(&pack, 0, sizeof(pack));
	memset(&stream, 0, sizeof(stream));
	pack.data = data; pack.length = 16; pack.packNum = 1;
	pack.packetInfo[0].offset = 0; pack.packetInfo[0].length = 16;
	stream.packet = &pack; stream.count = 1;

	/* 19 = IDR_W_RADL */
	pack.packetInfo[0].packType.h265Nalu = 19;
	out = star6e_output_stream_flatten(&stream, &len, &is_idr);
	CHECK("flatten idr_w_radl detected", out && is_idr == 1);
	free(out);

	/* 20 = IDR_N_LP */
	is_idr = 0;
	pack.packetInfo[0].packType.h265Nalu = 20;
	out = star6e_output_stream_flatten(&stream, &len, &is_idr);
	CHECK("flatten idr_n_lp detected", out && is_idr == 1);
	free(out);

	/* 1 = TRAIL_R, not an IRAP */
	is_idr = 1;
	pack.packetInfo[0].packType.h265Nalu = 1;
	out = star6e_output_stream_flatten(&stream, &len, &is_idr);
	CHECK("flatten trail is not an IRAP", out && is_idr == 0);
	free(out);
	return failures;
}

/* The synchronous writer holds a 512 KB automatic and DROPS anything larger.
 * Sizing from the stream removes that cliff — at 19 Mbps an IRAP can exceed
 * it, and a silently dropped keyframe is the worst frame to lose. */
static int test_star6e_flatten_handles_a_frame_over_512k(void)
{
	MI_VENC_Pack_t pack;
	MI_VENC_Stream_t stream;
	size_t big = 700u * 1024u;
	uint8_t *data = malloc(big);
	uint8_t *out;
	size_t len = 0;
	int failures = 0;

	CHECK("flatten big alloc", data != NULL);
	if (!data)
		return failures;
	memset(data, 0x5A, big);

	memset(&pack, 0, sizeof(pack));
	memset(&stream, 0, sizeof(stream));
	pack.data = data; pack.length = (MI_U32)big; pack.packNum = 0;
	stream.packet = &pack; stream.count = 1;

	out = star6e_output_stream_flatten(&stream, &len, NULL);
	CHECK("flatten accepts a 700 KB access unit", out != NULL);
	CHECK("flatten big length exact", len == big);
	if (out)
		CHECK("flatten big payload intact", memcmp(out, data, big) == 0);
	free(out);
	free(data);
	return failures;
}

static int test_star6e_flatten_refuses_what_the_writers_refuse(void)
{
	MI_VENC_Pack_t pack;
	MI_VENC_Stream_t stream;
	uint8_t data[16] = { 0 };
	size_t len = 99;
	int is_idr = 9;
	int failures = 0;

	memset(&pack, 0, sizeof(pack));
	memset(&stream, 0, sizeof(stream));
	CHECK("flatten NULL stream", star6e_output_stream_flatten(NULL, &len,
		&is_idr) == NULL);
	CHECK("flatten NULL clears outputs", len == 0 && is_idr == 0);

	stream.count = 1; stream.packet = &pack;
	CHECK("flatten null pack data refused",
		star6e_output_stream_flatten(&stream, &len, &is_idr) == NULL);

	/* packNum beyond the descriptor table — the incomplete-table case */
	pack.data = data; pack.length = 16;
	pack.packNum = (MI_U32)(sizeof(pack.packetInfo) /
		sizeof(pack.packetInfo[0])) + 1;
	CHECK("flatten incomplete packetInfo refused",
		star6e_output_stream_flatten(&stream, &len, &is_idr) == NULL);

	/* Valid table but every descriptor empty -> nothing to record */
	pack.packNum = 1;
	pack.packetInfo[0].offset = 0;
	pack.packetInfo[0].length = 0;
	CHECK("flatten zero-length descriptor refused",
		star6e_output_stream_flatten(&stream, &len, &is_idr) == NULL);

	/* The discriminating case: a FIRST pack that is perfectly valid and
	 * would flatten to real bytes, followed by one with an over-long
	 * packNum.  Without the whole-stream validation this returns a buffer
	 * holding pack 0 only — a silently truncated access unit that looks
	 * successful.  With it, the stream is refused outright, which is what
	 * the synchronous writers do. */
	{
		MI_VENC_Pack_t two[2];
		MI_VENC_Stream_t st2;
		uint8_t good[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };

		memset(two, 0, sizeof(two));
		memset(&st2, 0, sizeof(st2));
		two[0].data = good; two[0].length = 8; two[0].packNum = 1;
		two[0].packetInfo[0].offset = 0;
		two[0].packetInfo[0].length = 8;
		two[1].data = good; two[1].length = 8;
		two[1].packNum = (MI_U32)(sizeof(two[1].packetInfo) /
			sizeof(two[1].packetInfo[0])) + 1;
		st2.packet = two; st2.count = 2;
		CHECK("flatten refuses a partly-valid stream outright",
			star6e_output_stream_flatten(&st2, &len, &is_idr) == NULL);
	}
	return failures;
}

/* An access unit larger than the writer queue's byte cap is refused here
 * rather than copied, because push() would reject it at the cap anyway.  The
 * refusal has to be counted by the caller, which is why it is safe — but an
 * absent guard is invisible without this: it just means a doomed
 * multi-megabyte malloc+memcpy per oversized frame on a 64 MB SoC. */
static int test_flatten_refuses_an_access_unit_over_the_queue_cap(void)
{
	MI_VENC_Stream_t stream;
	MI_VENC_Pack_t pack;
	size_t oversized = VENC_REC_WRITER_MAX_BYTES + 1;
	uint8_t *big = malloc(oversized);
	size_t len = 0;
	int is_idr = -1;
	int failures = 0;

	CHECK("cap test allocated its stimulus", big != NULL);
	if (!big)
		return failures;
	memset(big, 0x5A, oversized);

	memset(&pack, 0, sizeof(pack));
	memset(&stream, 0, sizeof(stream));
	pack.data = big;
	pack.length = (MI_U32)oversized;
	pack.packNum = 0;
	stream.packet = &pack;
	stream.count = 1;

	CHECK("flatten refuses an AU over the queue cap",
		star6e_output_stream_flatten(&stream, &len, &is_idr) == NULL);
	CHECK("refused AU reports no length", len == 0);

	/* Control: one byte under the cap is accepted, so the refusal is the
	 * cap and not simply "large streams fail". */
	pack.length = (MI_U32)(VENC_REC_WRITER_MAX_BYTES - 1);
	{
		uint8_t *out = star6e_output_stream_flatten(&stream, &len,
			&is_idr);

		CHECK("flatten accepts an AU just under the cap", out != NULL);
		CHECK("accepted AU reports its length",
			len == VENC_REC_WRITER_MAX_BYTES - 1);
		free(out);
	}
	free(big);
	return failures;
}

/* output_socket_configure() is shared by all three backends.  It used to fill
 * the destination AFTER opening or reusing the socket, and closed the fd when
 * that fill failed -- including on the REUSE path, where the type was
 * unchanged and the socket was working.  Since nothing resolves names, a
 * hostname URI destroyed a live output on every backend that calls this.
 *
 * These arms pin the whole contract, not just the fix: the reorder has to keep
 * fresh-open, same-type reuse and the close+reopen type change working, and it
 * has to leave the destination untouched when it refuses -- otherwise a caller
 * could be left sending to a destination it never committed to. */
static int test_output_socket_configure_is_all_or_nothing(void)
{
	int failures = 0;
	int handle = -1, connected = 0;
	struct sockaddr_storage dst, kept;
	socklen_t dst_len = 0, kept_len;
	VencOutputUriType transport = VENC_OUTPUT_URI_UDP;
	VencOutputUri first, second, unix_uri, bad;
	int first_fd;

	CHECK("cfg_parse_first",
		venc_config_parse_output_uri("udp://127.0.0.1:5600", &first) == 0);
	CHECK("cfg_parse_second",
		venc_config_parse_output_uri("udp://127.0.0.2:5700", &second) == 0);
	CHECK("cfg_parse_unix",
		venc_config_parse_output_uri("unix:///tmp/test_output_socket.sock",
			&unix_uri) == 0);
	/* Not resolvable and never will be: the fill is inet_pton only. */
	CHECK("cfg_parse_bad",
		venc_config_parse_output_uri("udp://somehost:5600", &bad) == 0);

	CHECK("cfg_fresh_udp", output_socket_configure(&handle, &dst, &dst_len,
		&transport, &first, 1, 0, &connected) == 0);
	CHECK("cfg_fresh_fd_open", handle >= 0);
	first_fd = handle;

	/* Same type: the socket must be reused, not churned. */
	CHECK("cfg_reuse_same_type", output_socket_configure(&handle, &dst,
		&dst_len, &transport, &second, 1, 0, &connected) == 0);
	CHECK("cfg_reuse_kept_fd", handle == first_fd);

	/* The regression: a bad destination must not take the socket down, and
	 * must not half-apply either. */
	kept = dst;
	kept_len = dst_len;
	CHECK("cfg_bad_uri_refused", output_socket_configure(&handle, &dst,
		&dst_len, &transport, &bad, 1, 0, &connected) == -1);
	CHECK("cfg_bad_uri_kept_handle", handle == first_fd);
	CHECK("cfg_bad_uri_fd_still_open", fcntl(handle, F_GETFD) != -1);
	CHECK("cfg_bad_uri_dst_untouched",
		dst_len == kept_len && memcmp(&kept, &dst, sizeof(kept)) == 0);

	/* Type change still closes and reopens. */
	CHECK("cfg_type_change", output_socket_configure(&handle, &dst, &dst_len,
		&transport, &unix_uri, 1, 0, &connected) == 0);
	CHECK("cfg_type_change_transport", transport == VENC_OUTPUT_URI_UNIX);
	CHECK("cfg_type_change_fd_open", handle >= 0 && fcntl(handle, F_GETFD) != -1);

	if (handle >= 0)
		close(handle);

	/* The helper answers the same question without touching anything, for
	 * callers that commit or respawn on a URI without reaching configure(). */
	CHECK("usable_good_udp", output_socket_destination_is_usable(&first) == 1);
	CHECK("usable_good_unix",
		output_socket_destination_is_usable(&unix_uri) == 1);
	CHECK("usable_rejects_bad_host",
		output_socket_destination_is_usable(&bad) == 0);
	CHECK("usable_rejects_null", output_socket_destination_is_usable(NULL) == 0);
	{
		VencOutputUri ring;

		/* Ring URIs carry no sockaddr; validity belongs to whoever creates
		 * the ring, so the helper must not claim them. */
		CHECK("cfg_parse_ring",
			venc_config_parse_output_uri("frame-shm://venc_frame", &ring) == 0);
		CHECK("usable_rejects_ring",
			output_socket_destination_is_usable(&ring) == 0);
	}

	/* A bad URI on a fresh context leaves nothing open. */
	{
		int fresh = -1, fresh_connected = 0;
		struct sockaddr_storage fresh_dst;
		socklen_t fresh_len = 0;
		VencOutputUriType fresh_transport = VENC_OUTPUT_URI_UDP;

		CHECK("cfg_bad_uri_fresh_refused",
			output_socket_configure(&fresh, &fresh_dst, &fresh_len,
				&fresh_transport, &bad, 1, 0, &fresh_connected) == -1);
		CHECK("cfg_bad_uri_fresh_no_handle", fresh == -1);
	}
	return failures;
}

/* A startup that cannot bring the destination up leaves the output inert so
 * the craft stays reachable, and the operator recovers by setting a working
 * outgoing.server live.  star6e_output_teardown() resets transport POLICY
 * along with transport state, and the live retarget reads that policy back
 * out of the output -- so an inert start that forgets it would silently apply
 * unconnected UDP, and no Unix stall allowance, to whatever the operator
 * recovers to.  The pipeline re-seeds both; this pins that it must. */
static int test_star6e_output_inert_start_keeps_policy(void)
{
	Star6eOutputSetup setup;
	Star6eOutput output;
	int failures = 0;

	/* A hostname: prepare accepts it, init cannot resolve it. */
	CHECK("inert_prepare",
		star6e_output_prepare(&setup, "udp://localhost:5600", "rtp", 0) == 0);
	setup.requested_connected_udp = 1;
	setup.allow_unix_encoder_stall = 1;

	CHECK("inert_init_refuses", star6e_output_init(&output, &setup) == -1);

	/* What the pipeline does on that failure. */
	star6e_output_teardown(&output);
	CHECK("inert_teardown_clears_connected",
		output.requested_connected_udp == 0);
	CHECK("inert_teardown_clears_stall",
		output.allow_unix_encoder_stall == 0);

	output.requested_connected_udp = setup.requested_connected_udp;
	output.allow_unix_encoder_stall = setup.allow_unix_encoder_stall;
	CHECK("inert_policy_restored_connected",
		output.requested_connected_udp == 1);
	CHECK("inert_policy_restored_stall",
		output.allow_unix_encoder_stall == 1);
	CHECK("inert_socket_closed", output.socket_handle < 0);

	star6e_output_teardown(&output);
	return failures;
}

int test_star6e_output(void)
{
	int failures = 0;

	failures += test_output_socket_configure_is_all_or_nothing();
	failures += test_star6e_output_inert_start_keeps_policy();
	failures += test_star6e_output_reset_state();
	failures += test_star6e_output_udp_init();
	failures += test_star6e_output_udp_invalid_host_rejected();
	failures += test_star6e_output_udp_apply_server();
	failures += test_star6e_output_udp_send_rtp();
	failures += test_star6e_output_fec();
	failures += test_star6e_output_udp_send_compact();
	failures += test_star6e_output_unix_send_rtp();
	failures += test_star6e_output_unix_send_compact();
	failures += test_star6e_output_send_frame_rtp_dispatch();
	failures += test_star6e_output_udp_send_compact_frame();
	failures += test_star6e_output_send_frame_compact_dispatch();
	failures += test_star6e_output_shm_init();
	failures += test_star6e_output_shm_send_rtp();
	failures += test_star6e_output_shm_compact_rejected();
	failures += test_star6e_output_shm_apply_server_rejected();
	failures += test_star6e_output_empty_server_prepare();
	failures += test_star6e_output_prepare_defaults_to_rtp();
	failures += test_star6e_audio_output_reset_state();
	failures += test_star6e_audio_output_send_rtp();
	failures += test_star6e_audio_output_shm_dedicated_local_udp();
	failures += test_star6e_audio_output_shared_apply_server();
	failures += test_star6e_audio_output_shared_switch_udp_to_unix();
	failures += test_star6e_audio_output_dedicated_port();
	failures += test_star6e_audio_output_unix_dedicated_local_udp();
	failures += test_star6e_audio_output_shared_teardown_keeps_video_socket();
	failures += test_audio_target_cache_gen_tracks_transport();
	failures += test_audio_target_cache_hit_stable();
	failures += test_star6e_output_backpressure_hysteresis();
	failures += test_star6e_output_unix_backpressure();
	failures += test_star6e_output_unix_flush_is_bounded();
	failures += test_star6e_output_unix_stall_mode_resumes_without_drops();
	failures += test_star6e_output_always_sends_under_pressure();
	failures += test_star6e_output_frame_ring_full_drop_is_inert();
	failures += test_star6e_output_frame_ring_truncation_abort();
	failures += test_star6e_output_packet_info_validation();
	failures += test_star6e_flatten_concatenates_nals_in_order();
	failures += test_flatten_refuses_an_access_unit_over_the_queue_cap();
	failures += test_star6e_flatten_detects_idr();
	failures += test_star6e_flatten_handles_a_frame_over_512k();
	failures += test_star6e_flatten_refuses_what_the_writers_refuse();
	return failures;
}
