#include "rtp_packetizer.h"
#include "test_helpers.h"

#include <string.h>

typedef struct {
	size_t header_len;
	uint8_t header[12];
	size_t payload_len;
	uint8_t payload[3 + RTP_BUFFER_MAX];
} TestRtpWrite;

typedef struct {
	TestRtpWrite writes[8];
	int call_count;
	int fail_at_call;
} TestRtpWriter;

static int test_rtp_writer(const uint8_t *header, size_t header_len,
	const uint8_t *payload1, size_t payload1_len,
	const uint8_t *payload2, size_t payload2_len, void *opaque)
{
	TestRtpWriter *writer = opaque;
	TestRtpWrite *write;

	if (!writer || !header || !payload1)
		return -1;
	if (writer->fail_at_call >= 0 && writer->call_count == writer->fail_at_call)
		return -1;
	if (writer->call_count >= (int)(sizeof(writer->writes) /
		sizeof(writer->writes[0])))
		return -1;

	write = &writer->writes[writer->call_count++];
	write->header_len = header_len;
	write->payload_len = payload1_len + payload2_len;
	memcpy(write->header, header, header_len);
	memcpy(write->payload, payload1, payload1_len);
	if (payload2 && payload2_len > 0)
		memcpy(write->payload + payload1_len, payload2, payload2_len);
	return 0;
}

static int test_rtp_packetizer_send_packet_ok(void)
{
	int failures = 0;
	TestRtpWriter writer = {.fail_at_call = -1};
	RtpPacketizerState state = {
		.seq = 0x1234,
		.timestamp = 0x11223344,
		.ssrc = 0x55667788,
		.payload_type = 97,
	};
	const uint8_t payload[] = {0xAA, 0xBB, 0xCC};

	CHECK("rtp_pkt_send_packet_ok",
		rtp_packetizer_send_packet(&state, test_rtp_writer, &writer,
			payload, sizeof(payload), NULL, 0, 1) == 0 &&
		writer.call_count == 1 &&
		state.seq == 0x1235);
	CHECK("rtp_pkt_send_packet_header",
		writer.writes[0].header_len == 12 &&
		writer.writes[0].header[0] == 0x80 &&
		writer.writes[0].header[1] == (uint8_t)(0x80 | 97) &&
		writer.writes[0].header[2] == 0x12 &&
		writer.writes[0].header[3] == 0x34 &&
		writer.writes[0].header[4] == 0x11 &&
		writer.writes[0].header[5] == 0x22 &&
		writer.writes[0].header[6] == 0x33 &&
		writer.writes[0].header[7] == 0x44 &&
		writer.writes[0].header[8] == 0x55 &&
		writer.writes[0].header[9] == 0x66 &&
		writer.writes[0].header[10] == 0x77 &&
		writer.writes[0].header[11] == 0x88);
	CHECK("rtp_pkt_send_packet_payload",
		writer.writes[0].payload_len == sizeof(payload) &&
		memcmp(writer.writes[0].payload, payload, sizeof(payload)) == 0);

	return failures;
}

static int test_rtp_packetizer_send_packet_fail(void)
{
	int failures = 0;
	TestRtpWriter writer = {.fail_at_call = 0};
	RtpPacketizerState state = {
		.seq = 0x2222,
		.timestamp = 0x01020304,
		.ssrc = 0x05060708,
		.payload_type = 96,
	};
	const uint8_t payload[] = {0x99};

	CHECK("rtp_pkt_send_packet_fail",
		rtp_packetizer_send_packet(&state, test_rtp_writer, &writer,
			payload, sizeof(payload), NULL, 0, 0) == -1 &&
		writer.call_count == 0 &&
		state.seq == 0x2222);

	return failures;
}

static int test_rtp_packetizer_send_hevc_single(void)
{
	int failures = 0;
	TestRtpWriter writer = {.fail_at_call = -1};
	RtpPacketizerState state = {
		.seq = 0x0100,
		.timestamp = 0x0A0B0C0D,
		.ssrc = 0x10203040,
		.payload_type = 97,
	};
	RtpPacketizerResult result;
	const uint8_t nal[] = {0x26, 0x01, 0xAA, 0xBB};

	CHECK("rtp_pkt_hevc_single",
		rtp_packetizer_send_hevc_nal(&state, test_rtp_writer, &writer, nal,
			sizeof(nal), 1, 1400, &result) == sizeof(nal) &&
		writer.call_count == 1 &&
		state.seq == 0x0101);
	CHECK("rtp_pkt_hevc_single_result",
		result.packet_count == 1 &&
		result.payload_bytes == sizeof(nal) &&
		result.fragmented == 0);
	CHECK("rtp_pkt_hevc_single_marker",
		writer.writes[0].header[1] == (uint8_t)(0x80 | 97) &&
		writer.writes[0].payload_len == sizeof(nal) &&
		memcmp(writer.writes[0].payload, nal, sizeof(nal)) == 0);

	return failures;
}

static int test_rtp_packetizer_send_hevc_fu(void)
{
	int failures = 0;
	TestRtpWriter writer = {.fail_at_call = -1};
	RtpPacketizerState state = {
		.seq = 0x2000,
		.timestamp = 0x01010101,
		.ssrc = 0x02020202,
		.payload_type = 97,
	};
	RtpPacketizerResult result;
	const uint8_t nal[] = {0x26, 0x01, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};

	CHECK("rtp_pkt_hevc_fu",
		rtp_packetizer_send_hevc_nal(&state, test_rtp_writer, &writer, nal,
			sizeof(nal), 1, 5, &result) == sizeof(nal) &&
		writer.call_count == 3 &&
		state.seq == 0x2003);
	CHECK("rtp_pkt_hevc_fu_result",
		result.packet_count == 3 &&
		result.payload_bytes == 14 &&
		result.fragmented == 1);
	CHECK("rtp_pkt_hevc_fu_fragments",
		writer.writes[0].payload_len == 5 &&
		writer.writes[1].payload_len == 5 &&
		writer.writes[2].payload_len == 4 &&
		writer.writes[0].payload[0] == 0x62 &&
		writer.writes[0].payload[1] == 0x01 &&
		writer.writes[0].payload[2] == 0x93 &&
		writer.writes[1].payload[2] == 0x13 &&
		writer.writes[2].payload[2] == 0x53 &&
		writer.writes[0].header[1] == 97 &&
		writer.writes[2].header[1] == (uint8_t)(0x80 | 97));

	/* 7 body bytes at 3 per fragment: equal pieces 3+2+2, never 3+3+1. */
	{
		const uint8_t nal7[] = {0x26, 0x01, 1, 2, 3, 4, 5, 6, 7};

		memset(&writer, 0, sizeof(writer));
		writer.fail_at_call = -1;
		CHECK("rtp_pkt_hevc_fu_equal_fragments",
			rtp_packetizer_send_hevc_nal(&state, test_rtp_writer,
				&writer, nal7, sizeof(nal7), 1, 6, &result) ==
			sizeof(nal7) && writer.call_count == 3 &&
			writer.writes[0].payload_len == 6 &&
			writer.writes[1].payload_len == 5 &&
			writer.writes[2].payload_len == 5 &&
			writer.writes[2].payload[4] == 7);
	}

	return failures;
}

static int test_rtp_packetizer_send_hevc_fu_partial_fail(void)
{
	int failures = 0;
	TestRtpWriter writer = {.fail_at_call = 1};
	RtpPacketizerState state = {
		.seq = 0x3000,
		.timestamp = 0x02030405,
		.ssrc = 0x06070809,
		.payload_type = 97,
	};
	RtpPacketizerResult result;
	const uint8_t nal[] = {0x26, 0x01, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE};

	CHECK("rtp_pkt_hevc_fu_partial_fail",
		rtp_packetizer_send_hevc_nal(&state, test_rtp_writer, &writer, nal,
			sizeof(nal), 1, 5, &result) == 2 &&
		writer.call_count == 1 &&
		state.seq == 0x3001);
	CHECK("rtp_pkt_hevc_fu_partial_result",
		result.packet_count == 1 &&
		result.payload_bytes == 5 &&
		result.fragmented == 1);

	return failures;
}

int test_rtp_packetizer(void)
{
	int failures = 0;
	RtpPacketizerState state = {0};
	const uint8_t payload[] = {0x11};

	CHECK("rtp_pkt_null_state",
		rtp_packetizer_send_packet(NULL, test_rtp_writer, NULL, payload,
			sizeof(payload), NULL, 0, 0) == -1);
	CHECK("rtp_pkt_null_writer",
		rtp_packetizer_send_packet(&state, NULL, NULL, payload,
			sizeof(payload), NULL, 0, 0) == -1);
	CHECK("rtp_pkt_null_payload",
		rtp_packetizer_send_packet(&state, test_rtp_writer, NULL, NULL,
			0, NULL, 0, 0) == -1);

	failures += test_rtp_packetizer_send_packet_ok();
	failures += test_rtp_packetizer_send_packet_fail();
	failures += test_rtp_packetizer_send_hevc_single();
	failures += test_rtp_packetizer_send_hevc_fu();
	failures += test_rtp_packetizer_send_hevc_fu_partial_fail();

	return failures;
}
