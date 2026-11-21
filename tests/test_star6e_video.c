#include "star6e_video.h"

#include "test_helpers.h"

#include <arpa/inet.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

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

static int reserve_udp_port(uint16_t *port)
{
	int socket_handle = create_udp_receiver(port);

	if (socket_handle < 0)
		return -1;
	close(socket_handle);
	return 0;
}

static int test_star6e_video_init_rtp_state(void)
{
	VencConfig cfg;
	Star6eOutputSetup setup;
	Star6eOutput output;
	Star6eVideoState state = {0};
	int failures = 0;
	int ret;

	venc_config_defaults(&cfg);

	ret = star6e_output_prepare(&setup, "udp://127.0.0.1:5600", "rtp", 0);
	CHECK("star6e video rtp prepare", ret == 0);
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e video rtp output init", ret == 0);

	star6e_video_init(&state, &cfg, 60, &output);
	CHECK("star6e video rtp max frame", state.max_frame_size == 1400);
	CHECK("star6e video rtp payload", state.rtp_payload_size == 1400);
	CHECK("star6e video rtp sensor fps", state.sensor_framerate == 60);
	CHECK("star6e video rtp frame ticks", state.rtp_frame_ticks == 1500);
	CHECK("star6e video rtp payload type", state.rtp_state.payload_type == 97);
	CHECK("star6e video rtp frame counter", state.frame_counter == 0);

	star6e_output_teardown(&output);
	return failures;
}

static int test_star6e_video_init_compact_state(void)
{
	VencConfig cfg;
	Star6eOutputSetup setup;
	Star6eOutput output;
	Star6eVideoState state = {0};
	int failures = 0;
	int ret;

	venc_config_defaults(&cfg);
	ret = star6e_output_prepare(&setup, "udp://127.0.0.1:5600", "compact", 0);
	CHECK("star6e video compact prepare", ret == 0);
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e video compact output init", ret == 0);

	star6e_video_init(&state, &cfg, 120, &output);
	CHECK("star6e video compact sensor fps", state.sensor_framerate == 120);
	CHECK("star6e video compact frame ticks", state.rtp_frame_ticks == 0);
	CHECK("star6e video compact payload type", state.rtp_state.payload_type == 0);
	CHECK("star6e video compact params zero", state.param_sets.vps_len == 0);

	star6e_output_teardown(&output);
	return failures;
}

static int test_star6e_video_send_frame_rtp(void)
{
	static const uint8_t nal[] = { 0x02, 0x01, 0xAA, 0xBB };
	VencConfig cfg;
	Star6eOutputSetup setup;
	Star6eOutput output;
	Star6eVideoState state = {0};
	MI_VENC_Pack_t pack = {0};
	MI_VENC_Stream_t stream = {0};
	uint8_t buf[64];
	char uri[64];
	uint16_t seq_before;
	uint32_t ts_before;
	size_t total_bytes;
	ssize_t received;
	uint16_t port;
	int recv_socket;
	int failures = 0;
	int ret;

	venc_config_defaults(&cfg);

	recv_socket = create_udp_receiver(&port);
	CHECK("star6e video send receiver", recv_socket >= 0);
	snprintf(uri, sizeof(uri), "udp://127.0.0.1:%u", port);
	ret = star6e_output_prepare(&setup, uri, "rtp", 0);
	CHECK("star6e video send prepare", ret == 0);
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e video send output init", ret == 0);
	star6e_video_init(&state, &cfg, 30, &output);
	seq_before = state.rtp_state.seq;
	ts_before = state.rtp_state.timestamp;

	pack.data = (uint8_t *)nal;
	pack.length = sizeof(nal);
	pack.packNum = 1;
	pack.packetInfo[0].offset = 0;
	pack.packetInfo[0].length = sizeof(nal);
	pack.packetInfo[0].packType.h265Nalu = 1;
	stream.count = 1;
	stream.packet = &pack;

	total_bytes = star6e_video_send_frame(&state, &output, &stream, 1, 0, NULL, NULL, NULL, 0);
	CHECK("star6e video send bytes", total_bytes == sizeof(nal));
	CHECK("star6e video send frame counter", state.frame_counter == 1);
	CHECK("star6e video send seq advanced",
		state.rtp_state.seq == (uint16_t)(seq_before + 1));
	CHECK("star6e video send timestamp advanced",
		state.rtp_state.timestamp == ts_before + state.rtp_frame_ticks);

	received = recv(recv_socket, buf, sizeof(buf), 0);
	CHECK("star6e video send recv size", received == (ssize_t)(12 + sizeof(nal)));
	CHECK("star6e video send recv payload type",
		received >= 2 && (buf[1] & 0x7F) == 97);

	star6e_output_teardown(&output);
	close(recv_socket);
	return failures;
}

/* Media datagrams (PT 97) of one frame, parity skipped. */
static int fec_media_count(int sock, int *max_len)
{
	uint8_t buf[4096];
	int n = 0;
	ssize_t r;

	*max_len = 0;
	while ((r = recv(sock, buf, sizeof(buf), MSG_DONTWAIT)) > 0) {
		if ((buf[1] & 0x7F) != 97)
			continue;
		n++;
		if (r > *max_len)
			*max_len = (int)r;
	}
	return n;
}

/* With outgoing.fec the payload size is 3/2 of the smoothed slice size,
 * capped by maxPayloadSize; without it the cap itself is used. */
static int test_star6e_video_fec_payload_derived(void)
{
	static uint8_t au[2 * (3 + 2400)];
	VencConfig cfg;
	Star6eOutputSetup setup;
	Star6eOutput output;
	Star6eVideoState state = {0};
	MI_VENC_Pack_t pack = {0};
	MI_VENC_Stream_t stream = {0};
	char uri[64];
	uint16_t port;
	int sock, n, max_len, failures = 0;

	for (int s = 0; s < 2; s++) {
		uint8_t *p = au + s * (3 + 2400);

		p[0] = 0; p[1] = 0; p[2] = 1;
		p[3] = 0x02; p[4] = 0x01; /* TRAIL_R */
		memset(p + 5, 0x5A, 2400 - 2);
		p[5] = s == 0 ? 0x80 : 0x00;
	}
	pack.data = au;
	pack.length = sizeof(au);
	pack.packNum = 1;
	pack.packetInfo[0].length = sizeof(au);
	stream.count = 1;
	stream.packet = &pack;

	venc_config_defaults(&cfg);
	cfg.outgoing.max_payload_size = 3780;
	sock = create_udp_receiver(&port);
	snprintf(uri, sizeof(uri), "udp://127.0.0.1:%u", port);
	CHECK("fec payload: output up",
		sock >= 0 && star6e_output_prepare(&setup, uri, "rtp", 0) == 0 &&
		star6e_output_init(&output, &setup) == 0 &&
		star6e_output_set_fec(&output, 1) == 0);
	star6e_video_init(&state, &cfg, 60, &output);

	(void)star6e_video_send_frame(&state, &output, &stream, 1, 0, NULL,
		NULL, NULL, 0);
	n = fec_media_count(sock, &max_len);
	CHECK("fec payload: nothing learned yet -> 1400, 2 fragments a slice",
		state.payload_used == 1400 && n == 4 && max_len <= 12 + 1400 &&
		state.slice_avg == 2400);
	(void)star6e_video_send_frame(&state, &output, &stream, 1, 0, NULL,
		NULL, NULL, 0);
	n = fec_media_count(sock, &max_len);
	CHECK("fec payload: 3/2 x 2400 = 3600, one datagram a slice",
		state.payload_used == 3600 && n == 2 &&
		max_len == 12 + 2400);
	(void)star6e_output_set_fec(&output, 0);
	(void)star6e_video_send_frame(&state, &output, &stream, 1, 0, NULL,
		NULL, NULL, 0);
	(void)fec_media_count(sock, &max_len);
	CHECK("fec payload: fec off uses the ceiling", state.payload_used == 3780);

	star6e_output_teardown(&output);
	close(sock);
	return failures;
}

static int test_star6e_video_send_frame_disabled(void)
{
	static const uint8_t nal[] = { 0x02, 0x01, 0xAA, 0xBB };
	VencConfig cfg;
	Star6eOutputSetup setup;
	Star6eOutput output;
	Star6eVideoState state = {0};
	MI_VENC_Pack_t pack = {0};
	MI_VENC_Stream_t stream = {0};
	uint32_t seq_before;
	uint32_t ts_before;
	size_t total_bytes;
	int failures = 0;
	int ret;

	venc_config_defaults(&cfg);

	ret = star6e_output_prepare(&setup, "udp://127.0.0.1:5600", "rtp", 0);
	CHECK("star6e video disabled prepare", ret == 0);
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e video disabled output init", ret == 0);
	star6e_video_init(&state, &cfg, 30, &output);
	seq_before = state.rtp_state.seq;
	ts_before = state.rtp_state.timestamp;

	pack.data = (uint8_t *)nal;
	pack.length = sizeof(nal);
	pack.packNum = 1;
	pack.packetInfo[0].offset = 0;
	pack.packetInfo[0].length = sizeof(nal);
	pack.packetInfo[0].packType.h265Nalu = 1;
	stream.count = 1;
	stream.packet = &pack;

	total_bytes = star6e_video_send_frame(&state, &output, &stream, 0, 0, NULL, NULL, NULL, 0);
	CHECK("star6e video disabled bytes", total_bytes == 0);
	CHECK("star6e video disabled frame counter", state.frame_counter == 1);
	CHECK("star6e video disabled seq stable", state.rtp_state.seq == seq_before);
	CHECK("star6e video disabled ts stable", state.rtp_state.timestamp == ts_before);

	star6e_output_teardown(&output);
	return failures;
}

static int test_star6e_video_sidecar_ext(void)
{
	static const uint8_t nal[] = { 0x02, 0x01, 0xAA, 0xBB };
	VencConfig cfg;
	Star6eOutputSetup setup;
	Star6eOutput output;
	Star6eVideoState state = {0};
	MI_VENC_Pack_t pack = {0};
	MI_VENC_Stream_t stream = {0};
	RtpSidecarSubscribe sub = {0};
	RtpSidecarEncInfo enc_info = {0};
	RtpSidecarFrameExt wire = {0};
	struct sockaddr_in sidecar_addr;
	ssize_t received;
	uint16_t probe_port;
	uint16_t sidecar_port;
	int probe_socket;
	int failures = 0;
	int ret;

	venc_config_defaults(&cfg);
	ret = reserve_udp_port(&sidecar_port);
	CHECK("star6e sidecar reserve port", ret == 0);
	probe_socket = create_udp_receiver(&probe_port);
	CHECK("star6e sidecar probe socket", probe_socket >= 0);
	cfg.outgoing.sidecar_port = sidecar_port;

	ret = star6e_output_prepare(&setup, "udp://127.0.0.1:5600", "rtp", 0);
	CHECK("star6e sidecar prepare", ret == 0);
	ret = star6e_output_init(&output, &setup);
	CHECK("star6e sidecar output init", ret == 0);
	star6e_video_init(&state, &cfg, 30, &output);

	memset(&sidecar_addr, 0, sizeof(sidecar_addr));
	sidecar_addr.sin_family = AF_INET;
	sidecar_addr.sin_port = htons(sidecar_port);
	sidecar_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sub.magic = htonl(RTP_SIDECAR_MAGIC);
	sub.version = RTP_SIDECAR_VERSION;
	sub.msg_type = RTP_SIDECAR_MSG_SUBSCRIBE;
	ret = (int)sendto(probe_socket, &sub, sizeof(sub), 0,
		(struct sockaddr *)&sidecar_addr, sizeof(sidecar_addr));
	CHECK("star6e sidecar subscribe send", ret == (int)sizeof(sub));

	pack.data = (uint8_t *)nal;
	pack.length = sizeof(nal);
	pack.timestamp = 123456;
	pack.packNum = 1;
	pack.packetInfo[0].offset = 0;
	pack.packetInfo[0].length = sizeof(nal);
	pack.packetInfo[0].packType.h265Nalu = 1;
	stream.count = 1;
	stream.packet = &pack;

	enc_info.frame_size_bytes = 4321;
	enc_info.frame_type = RTP_SIDECAR_FRAME_IDR;
	enc_info.qp = 27;
	enc_info.complexity = 91;
	enc_info.scene_change = 1;
	enc_info.gop_state = 3;
	enc_info.idr_inserted = 1;
	enc_info.frames_since_idr = 15;

	(void)star6e_video_send_frame(&state, &output, &stream, 1, 0, &enc_info, NULL, NULL, 0);

	received = recv(probe_socket, &wire, sizeof(wire), 0);
	CHECK("star6e sidecar recv size",
		received == (ssize_t)sizeof(RtpSidecarFrameExt));
	CHECK("star6e sidecar recv enc flag",
		received >= (ssize_t)sizeof(RtpSidecarFrame) &&
		(wire.frame.flags & RTP_SIDECAR_FLAG_ENC_INFO) != 0);
	CHECK("star6e sidecar recv keyframe flag",
		received >= (ssize_t)sizeof(RtpSidecarFrame) &&
		(wire.frame.flags & RTP_SIDECAR_FLAG_KEYFRAME) != 0);
	CHECK("star6e sidecar recv seq_count",
		received >= (ssize_t)sizeof(RtpSidecarFrame) &&
		ntohs(wire.frame.seq_count) == 1);
	CHECK("star6e sidecar recv frame size",
		received == (ssize_t)sizeof(RtpSidecarFrameExt) &&
		ntohl(wire.enc.frame_size_bytes) == enc_info.frame_size_bytes);
	CHECK("star6e sidecar recv frame type",
		received == (ssize_t)sizeof(RtpSidecarFrameExt) &&
		wire.enc.frame_type == enc_info.frame_type);
	CHECK("star6e sidecar recv qp",
		received == (ssize_t)sizeof(RtpSidecarFrameExt) &&
		wire.enc.qp == enc_info.qp);
	CHECK("star6e sidecar recv complexity",
		received == (ssize_t)sizeof(RtpSidecarFrameExt) &&
		wire.enc.complexity == enc_info.complexity);
	CHECK("star6e sidecar recv scene change",
		received == (ssize_t)sizeof(RtpSidecarFrameExt) &&
		wire.enc.scene_change == enc_info.scene_change);
	CHECK("star6e sidecar recv gop state",
		received == (ssize_t)sizeof(RtpSidecarFrameExt) &&
		wire.enc.gop_state == enc_info.gop_state);
	CHECK("star6e sidecar recv idr inserted",
		received == (ssize_t)sizeof(RtpSidecarFrameExt) &&
		wire.enc.idr_inserted == enc_info.idr_inserted);
	CHECK("star6e sidecar recv fsi",
		received == (ssize_t)sizeof(RtpSidecarFrameExt) &&
		ntohs(wire.enc.frames_since_idr) == enc_info.frames_since_idr);

	star6e_output_teardown(&output);
	close(probe_socket);
	return failures;
}

/* Wire-layout test: rtp_sidecar_send_frame_transport with all four
 * combinations of {enc_info, transport_info} = {NULL, set}.  Verifies:
 *   - the transport trailer always lands at the right byte offset, in
 *     particular that the "enc absent, transport present" case slides
 *     the trailer up to offset 52 instead of 64,
 *   - the TRANSPORT_INFO flag bit is set when (and only when) the
 *     trailer is appended,
 *   - old probes that read just the base frame (or frame+enc) and
 *     ignore extra bytes get a valid prefix in every layout. */
static int test_star6e_video_sidecar_transport_layouts(void)
{
	RtpSidecarSender sender;
	RtpSidecarSubscribe sub = {0};
	struct sockaddr_in sidecar_addr;
	uint16_t sidecar_port = 0;
	uint16_t probe_port = 0;
	int probe_socket;
	int failures = 0;
	uint8_t buf[128];
	ssize_t n;
	int ret;

	ret = reserve_udp_port(&sidecar_port);
	CHECK("transport layout reserve port", ret == 0);
	probe_socket = create_udp_receiver(&probe_port);
	CHECK("transport layout probe socket", probe_socket >= 0);

	ret = rtp_sidecar_sender_init(&sender, sidecar_port);
	CHECK("transport layout sender init", ret == 0);

	memset(&sidecar_addr, 0, sizeof(sidecar_addr));
	sidecar_addr.sin_family = AF_INET;
	sidecar_addr.sin_port = htons(sidecar_port);
	sidecar_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sub.magic = htonl(RTP_SIDECAR_MAGIC);
	sub.version = RTP_SIDECAR_VERSION;
	sub.msg_type = RTP_SIDECAR_MSG_SUBSCRIBE;
	ret = (int)sendto(probe_socket, &sub, sizeof(sub), 0,
		(struct sockaddr *)&sidecar_addr, sizeof(sidecar_addr));
	CHECK("transport layout subscribe send", ret == (int)sizeof(sub));
	rtp_sidecar_poll(&sender);

	/* Case 1: no enc, no shm — 52 bytes, no flags. */
	rtp_sidecar_send_frame_transport(&sender, 1, 100, 0, 1, 0, 0, NULL, NULL);
	memset(buf, 0xCC, sizeof(buf));
	n = recv(probe_socket, buf, sizeof(buf), 0);
	CHECK("transport layout case1 size",
		n == (ssize_t)sizeof(RtpSidecarFrame));
	CHECK("transport layout case1 no flags",
		((const RtpSidecarFrame *)buf)->flags == 0);

	/* Case 2: enc only — 64 bytes, ENC_INFO flag set. */
	{
		RtpSidecarEncInfo enc = {0};
		enc.frame_type = RTP_SIDECAR_FRAME_P;
		enc.qp = 30;
		rtp_sidecar_send_frame_transport(&sender, 1, 200, 1, 1, 0, 0, &enc, NULL);
		memset(buf, 0xCC, sizeof(buf));
		n = recv(probe_socket, buf, sizeof(buf), 0);
		CHECK("transport layout case2 size",
			n == (ssize_t)sizeof(RtpSidecarFrameExt));
		CHECK("transport layout case2 enc flag",
			(((const RtpSidecarFrame *)buf)->flags &
			 RTP_SIDECAR_FLAG_ENC_INFO) != 0);
		CHECK("transport layout case2 no transport flag",
			(((const RtpSidecarFrame *)buf)->flags &
			 RTP_SIDECAR_FLAG_TRANSPORT_INFO) == 0);
	}

	/* Case 3: shm only (no enc) — trailer slides up to offset 52,
	 * total size = 52 + 16 = 68 bytes. */
	{
		RtpSidecarTransportInfo shm = {0};
		const RtpSidecarTransportInfoWire *trailer;

		shm.fill_pct = 80;
		shm.in_pressure = 1;
		shm.transport_drops = 0x11223344;
		shm.pressure_drops = 0xAABBCCDD;
		shm.packets_sent = 0x55667788;
		rtp_sidecar_send_frame_transport(&sender, 1, 300, 2, 1, 0, 0, NULL, &shm);
		memset(buf, 0xCC, sizeof(buf));
		n = recv(probe_socket, buf, sizeof(buf), 0);
		CHECK("transport layout case3 size",
			n == (ssize_t)(sizeof(RtpSidecarFrame) +
			               sizeof(RtpSidecarTransportInfoWire)));
		CHECK("transport layout case3 transport flag",
			(((const RtpSidecarFrame *)buf)->flags &
			 RTP_SIDECAR_FLAG_TRANSPORT_INFO) != 0);
		CHECK("transport layout case3 no enc flag",
			(((const RtpSidecarFrame *)buf)->flags &
			 RTP_SIDECAR_FLAG_ENC_INFO) == 0);
		trailer = (const RtpSidecarTransportInfoWire *)
			(buf + sizeof(RtpSidecarFrame));
		CHECK("transport layout case3 fill_pct", trailer->fill_pct == 80);
		CHECK("transport layout case3 in_pressure", trailer->in_pressure == 1);
		CHECK("transport layout case3 transport_drops",
			ntohl(trailer->transport_drops) == 0x11223344u);
		CHECK("transport layout case3 pressure_drops",
			ntohl(trailer->pressure_drops) == 0xAABBCCDDu);
		CHECK("transport layout case3 packets_sent",
			ntohl(trailer->packets_sent) == 0x55667788u);
	}

	/* Case 4: enc + shm — total size = 80 bytes, SHM at offset 64. */
	{
		RtpSidecarEncInfo enc = {0};
		RtpSidecarTransportInfo shm = {0};
		const RtpSidecarTransportInfoWire *trailer;

		enc.frame_type = RTP_SIDECAR_FRAME_I;
		enc.qp = 25;
		shm.fill_pct = 50;
		shm.in_pressure = 0;
		shm.transport_drops = 7;
		shm.pressure_drops = 11;
		shm.packets_sent = 13;
		rtp_sidecar_send_frame_transport(&sender, 1, 400, 3, 1, 0, 0, &enc, &shm);
		memset(buf, 0xCC, sizeof(buf));
		n = recv(probe_socket, buf, sizeof(buf), 0);
		CHECK("transport layout case4 size",
			n == (ssize_t)sizeof(RtpSidecarFrameExtTransport));
		CHECK("transport layout case4 both flags",
			(((const RtpSidecarFrame *)buf)->flags &
			 (RTP_SIDECAR_FLAG_ENC_INFO | RTP_SIDECAR_FLAG_TRANSPORT_INFO))
			 == (RTP_SIDECAR_FLAG_ENC_INFO | RTP_SIDECAR_FLAG_TRANSPORT_INFO));
		trailer = (const RtpSidecarTransportInfoWire *)
			(buf + offsetof(RtpSidecarFrameExtTransport, transport));
		CHECK("transport layout case4 fill_pct", trailer->fill_pct == 50);
		CHECK("transport layout case4 in_pressure", trailer->in_pressure == 0);
		CHECK("transport layout case4 transport_drops",
			ntohl(trailer->transport_drops) == 7u);
		CHECK("transport layout case4 pressure_drops",
			ntohl(trailer->pressure_drops) == 11u);
		CHECK("transport layout case4 packets_sent",
			ntohl(trailer->packets_sent) == 13u);
	}

	rtp_sidecar_sender_close(&sender);
	close(probe_socket);
	return failures;
}

int test_star6e_video(void)
{
	int failures = 0;

	failures += test_star6e_video_init_rtp_state();
	failures += test_star6e_video_init_compact_state();
	failures += test_star6e_video_send_frame_rtp();
	failures += test_star6e_video_send_frame_disabled();
	failures += test_star6e_video_fec_payload_derived();
	failures += test_star6e_video_sidecar_ext();
	failures += test_star6e_video_sidecar_transport_layouts();
	return failures;
}
