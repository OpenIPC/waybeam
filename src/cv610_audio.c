/*
 * CV610 analog input -> inner ACODEC -> AI -> vendor AENC/Opus -> RTP.
 *
 * The hardware path and its ordering intentionally match the standalone
 * streamer that passed the CV610 audio bring-up gates. The only integration
 * change is the transport edge: encoded Opus frames use Waybeam's shared RTP
 * packetizer and output-socket helpers.
 */

#include "cv610_audio.h"

#include "output_socket.h"
#include "audio_ring.h"
#include "rtp_packetizer.h"

#include <sched.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <time.h>
#include <unistd.h>

#include "ot_acodec.h"
#include "ot_common.h"
#include "ot_common_aenc.h"
#include "ot_common_aio.h"
#include "ss_audio_opus_adp.h"
#include "ss_mpi_audio.h"
#include "ss_mpi_sys_bind.h"

#define CV610_AUDIO_DEV 0
#define CV610_AI_CHN 0
#define CV610_AENC_CHN 0
#define CV610_ACODEC_DEVICE "/dev/acodec"
#define CV610_AUDIO_SAMPLE_RATE 48000u
/* 20 ms Opus frames (parity with the star6e/maruko 20 ms chunking): halves
 * the packet rate vs 10 ms for the same encoded bitrate. */
#define CV610_AUDIO_POINT_NUM 960u
#define CV610_AUDIO_BITRATE 32000u
#define CV610_AUDIO_PAYLOAD_TYPE 98u
#define CV610_AUDIO_MIC_GAIN 8u
#define CV610_ACODEC_POWERUP_TRIES 20
#define CV610_ACODEC_POWERUP_STEP_US 100000

struct Cv610AudioState {
	int acodec_fd;
	int aenc_fd;
	int socket_handle;
	struct sockaddr_storage destination;
	socklen_t destination_len;
	VencOutputUriType transport;
	int connected_udp;
	/* Live retarget, seqlock-protected -- the same shape the video path uses
	 * in cv610_runtime.c.  These four are rewritten by
	 * cv610_audio_apply_server() on the httpd thread and read by the capture
	 * thread in cv610_audio_write(); odd generation = a write in progress. */
	unsigned transport_gen;
	struct {
		int socket_handle;
		struct sockaddr_storage destination;
		socklen_t destination_len;
		int connected_udp;
	} tx;
	RtpPacketizerState rtp;
	uint32_t ticks_per_frame;
	pthread_t thread;
	pthread_mutex_t stats_lock;
	int running;
	int thread_started;
	int audio_initialized;
	int ai_enabled;
	int ai_chn_enabled;
	int aenc_opus_initialized;
	int aenc_created;
	int bound;
	uint64_t frames;
	uint64_t bytes;
	uint64_t packets;
	uint64_t drops;
	/* Optional tee for the TS recorder.  Owned by the runtime, set only
	 * while a TS recording is open; the capture thread reads it with an
	 * acquire load so a start/stop from the main loop is visible without
	 * a lock on the audio path.  Mirrors star6e_audio's rec_ring. */
	AudioRing *rec_ring;
};

#define AUDIO_CHECK(expr) do { \
	td_s32 check_ret = (expr); \
	if (check_ret != TD_SUCCESS) { \
		fprintf(stderr, "ERROR: %s=0x%x\n", #expr, check_ret); \
		return -1; \
	} \
} while (0)

static int cv610_acodec_apply(int fd, const char *name,
	unsigned long request, td_u32 value)
{
	if (ioctl(fd, request, &value) == 0)
		return 0;
	fprintf(stderr, "ERROR: ACODEC %s: %s\n", name, strerror(errno));
	return -1;
}

static int cv610_acodec_reset(Cv610AudioState *state)
{
	state->acodec_fd = open(CV610_ACODEC_DEVICE, O_RDWR);
	if (state->acodec_fd < 0) {
		fprintf(stderr, "ERROR: open %s: %s (is open_acodec loaded?)\n",
			CV610_ACODEC_DEVICE, strerror(errno));
		return -1;
	}
	if (ioctl(state->acodec_fd, OT_ACODEC_SOFT_RESET_CTRL) == 0)
		return 0;
	fprintf(stderr, "ERROR: ACODEC reset: %s\n", strerror(errno));
	return -1;
}

static int cv610_ai_set_attr(Cv610AudioState *state)
{
	ot_aio_attr attr;

	memset(&attr, 0, sizeof(attr));
	attr.sample_rate = (ot_audio_sample_rate)CV610_AUDIO_SAMPLE_RATE;
	attr.bit_width = OT_AUDIO_BIT_WIDTH_16;
	attr.work_mode = OT_AIO_MODE_I2S_MASTER;
	attr.snd_mode = OT_AUDIO_SOUND_MODE_MONO;
	attr.frame_num = 8;
	attr.point_num_per_frame = CV610_AUDIO_POINT_NUM;
	attr.chn_cnt = 1;
	attr.clk_share = 1;
	attr.i2s_type = OT_AIO_I2STYPE_INNERCODEC;
	AUDIO_CHECK(ss_mpi_ai_set_pub_attr(CV610_AUDIO_DEV, &attr));
	return 0;
}

static int cv610_ai_enable(Cv610AudioState *state)
{
	ot_ai_chn_param param;

	AUDIO_CHECK(ss_mpi_ai_enable(CV610_AUDIO_DEV));
	state->ai_enabled = 1;
	memset(&param, 0, sizeof(param));
	param.usr_frame_depth = 4;
	AUDIO_CHECK(ss_mpi_ai_set_chn_param(CV610_AUDIO_DEV,
		CV610_AI_CHN, &param));
	AUDIO_CHECK(ss_mpi_ai_enable_chn(CV610_AUDIO_DEV, CV610_AI_CHN));
	state->ai_chn_enabled = 1;
	return 0;
}

static int cv610_acodec_wait_for_power(Cv610AudioState *state)
{
	td_u32 fs = OT_ACODEC_FS_48000;
	int i;

	for (i = 0; i < CV610_ACODEC_POWERUP_TRIES; ++i) {
		if (ioctl(state->acodec_fd, OT_ACODEC_SET_I2S1_FS, &fs) == 0) {
			if (i > 0)
				printf("> CV610 ACODEC powered up after %d ms\n",
					i * (CV610_ACODEC_POWERUP_STEP_US / 1000));
			return 0;
		}
		if (errno != EPERM)
			break;
		usleep(CV610_ACODEC_POWERUP_STEP_US);
	}
	fprintf(stderr, "ERROR: ACODEC set-fs: %s (codec never powered up)\n",
		strerror(errno));
	return -1;
}

static int cv610_acodec_configure(Cv610AudioState *state, int muted)
{
	td_u32 gain = muted ? 0u : CV610_AUDIO_MIC_GAIN;

	if (cv610_acodec_wait_for_power(state) != 0 ||
		cv610_acodec_apply(state->acodec_fd, "set-mixer",
			OT_ACODEC_SET_MIXER_MIC, OT_ACODEC_MIXER_IN0) != 0 ||
		cv610_acodec_apply(state->acodec_fd, "boost-l",
			OT_ACODEC_ENABLE_BOOSTL, 1) != 0 ||
		cv610_acodec_apply(state->acodec_fd, "boost-r",
			OT_ACODEC_ENABLE_BOOSTR, 1) != 0 ||
		cv610_acodec_apply(state->acodec_fd, "gain-micl",
			OT_ACODEC_SET_GAIN_MICL, gain) != 0 ||
		cv610_acodec_apply(state->acodec_fd, "gain-micr",
			OT_ACODEC_SET_GAIN_MICR, gain) != 0 ||
		cv610_acodec_apply(state->acodec_fd, "adc-hpf",
			OT_ACODEC_SET_ADC_HP_FILTER, 1) != 0)
		return -1;
	printf("> CV610 ACODEC 48000 Hz IN0 boost=1 gain=%u hpf=1\n", gain);
	return 0;
}

static int cv610_aenc_start(Cv610AudioState *state)
{
	ot_aenc_attr_opus opus;
	ot_aenc_chn_attr attr;
	ot_mpp_chn source;
	ot_mpp_chn destination;

	AUDIO_CHECK(ss_mpi_aenc_opus_init());
	state->aenc_opus_initialized = 1;
	memset(&opus, 0, sizeof(opus));
	opus.sample_rate = (ot_audio_sample_rate)CV610_AUDIO_SAMPLE_RATE;
	opus.bit_width = OT_AUDIO_BIT_WIDTH_16;
	opus.snd_mode = OT_AUDIO_SOUND_MODE_MONO;
	opus.bit_rate = (ot_opus_bps)CV610_AUDIO_BITRATE;
	opus.app = OT_OPUS_APPLICATION_RESTRICTED_LOWDELAY;
	memset(&attr, 0, sizeof(attr));
	attr.type = OT_PT_OPUS;
	attr.point_num_per_frame = CV610_AUDIO_POINT_NUM;
	attr.buf_size = 8;
	attr.value = &opus;
	AUDIO_CHECK(ss_mpi_aenc_create_chn(CV610_AENC_CHN, &attr));
	state->aenc_created = 1;
	source.mod_id = OT_ID_AI;
	source.dev_id = CV610_AUDIO_DEV;
	source.chn_id = CV610_AI_CHN;
	destination.mod_id = OT_ID_AENC;
	destination.dev_id = 0;
	destination.chn_id = CV610_AENC_CHN;
	AUDIO_CHECK(ss_mpi_sys_bind(&source, &destination));
	state->bound = 1;
	/* Derived, not spelled out: this line claimed "10.0 ms" for a release
	 * after CV610_AUDIO_POINT_NUM went 480 -> 960, so the only banner a
	 * reader could check the packet rate against was wrong. */
	printf("> CV610 AENC Opus %u bit/s restricted-low-delay, %.1f ms frames\n",
		CV610_AUDIO_BITRATE,
		(double)CV610_AUDIO_POINT_NUM * 1000.0 /
			(double)CV610_AUDIO_SAMPLE_RATE);
	return 0;
}

/* Derive the audio destination from the video transport.  Shared by the
 * start path and the live retarget so the two cannot drift. */
static int cv610_audio_derive_output(const VencConfig *config,
	const VencOutputUri *video_output, VencOutputUri *out)
{
	if (!video_output) {
		fprintf(stderr, "ERROR: CV610 audio has no video transport context\n");
		return -1;
	}
	/* audio_port 0 shares the video destination (udp:// or unix://): own
	 * socket, SSRC and sequence, same peer, so a single-input consumer
	 * receives both and splits them by payload type. */
	if (config->outgoing.audio_port == 0) {
		if (video_output->type != VENC_OUTPUT_URI_UDP &&
			video_output->type != VENC_OUTPUT_URI_UNIX) {
			fprintf(stderr, "ERROR: CV610 audio port 0 needs udp:// or unix:// video\n");
			return -1;
		}
		*out = *video_output;
		return 0;
	}
	/* A dedicated port is a separate RTP/UDP stream. UDP video supplies the
	 * remote peer; unix:// and frame-shm:// are local video transports, so
	 * it targets the co-located link process on loopback (the established
	 * 5601 ingest contract). */
	memset(out, 0, sizeof(*out));
	out->type = VENC_OUTPUT_URI_UDP;
	out->port = (uint16_t)config->outgoing.audio_port;
	if (video_output->type == VENC_OUTPUT_URI_UDP) {
		snprintf(out->host, sizeof(out->host), "%s", video_output->host);
	} else if (video_output->type == VENC_OUTPUT_URI_UNIX ||
		video_output->type == VENC_OUTPUT_URI_FRAME_SHM) {
		snprintf(out->host, sizeof(out->host), "127.0.0.1");
	} else {
		fprintf(stderr, "ERROR: CV610 audio has no mapping for video transport %d\n",
			video_output->type);
		return -1;
	}
	return 0;
}

static void cv610_audio_log_target(const VencOutputUri *o)
{
	if (o->type == VENC_OUTPUT_URI_UNIX)
		printf("unix://%s\n", o->endpoint);
	else
		printf("udp://%s:%u\n", o->host, o->port);
}

static int cv610_audio_output_start(Cv610AudioState *state,
	const VencConfig *config, const VencOutputUri *video_output)
{
	VencOutputUri audio_output;

	if (config->outgoing.audio_port < 0)
		return 0;
	if (cv610_audio_derive_output(config, video_output, &audio_output) != 0)
		return -1;
	if (output_socket_configure(&state->socket_handle, &state->destination,
		&state->destination_len, &state->transport, &audio_output,
		config->outgoing.connected_udp, 0, &state->connected_udp) != 0)
		return -1;
	printf("> CV610 audio RTP PT=%u clock=48000 to ", CV610_AUDIO_PAYLOAD_TYPE);
	cv610_audio_log_target(&audio_output);
	return 0;
}

int cv610_audio_apply_server(Cv610AudioState *state, const VencConfig *config,
	const VencOutputUri *video_output)
{
	VencOutputUri audio_output;

	/* With audioPort=0 a udp <-> unix switch reopens the audio socket too,
	 * so the fd-reuse window documented at the top of cv610_runtime.c
	 * applies to audio as well (at worst one dropped packet).
	 *
	 * A craft with audio off, or audio_port < 0, has nothing to retarget --
	 * not an error, just no work. */
	if (!state || !config || config->outgoing.audio_port < 0)
		return 0;
	if (state->socket_handle < 0)
		return 0;
	if (cv610_audio_derive_output(config, video_output, &audio_output) != 0)
		return -1;

	__atomic_fetch_add(&state->transport_gen, 1, __ATOMIC_RELEASE);
	if (output_socket_configure(&state->socket_handle, &state->destination,
		&state->destination_len, &state->transport, &audio_output,
		config->outgoing.connected_udp, 0, &state->connected_udp) != 0) {
		__atomic_fetch_add(&state->transport_gen, 1, __ATOMIC_RELEASE);
		return -1;
	}
	__atomic_fetch_add(&state->transport_gen, 1, __ATOMIC_RELEASE);
	printf("> CV610 audio retargeted to ");
	cv610_audio_log_target(&audio_output);
	return 0;
}

/* Seqlock read, once per encoded audio frame.  Mirrors
 * cv610_transport_begin_frame() on the video side. */
static void cv610_audio_begin_frame(Cv610AudioState *state)
{
	unsigned gen_before, gen_after;

	for (;;) {
		gen_before = __atomic_load_n(&state->transport_gen,
			__ATOMIC_ACQUIRE);
		if (gen_before & 1u) {
			sched_yield();
			continue;
		}
		state->tx.socket_handle = state->socket_handle;
		state->tx.destination = state->destination;
		state->tx.destination_len = state->destination_len;
		state->tx.connected_udp = state->connected_udp;
		gen_after = __atomic_load_n(&state->transport_gen,
			__ATOMIC_ACQUIRE);
		if (gen_before == gen_after)
			break;
	}
}

static int cv610_audio_write(const uint8_t *header, size_t header_len,
	const uint8_t *payload1, size_t payload1_len,
	const uint8_t *payload2, size_t payload2_len, void *opaque)
{
	Cv610AudioState *state = opaque;

	/* From the per-frame snapshot, never the live fields -- every packet of
	 * one Opus access unit must reach the same destination. */
	return output_socket_send_parts(state->tx.socket_handle,
		&state->tx.destination, state->tx.destination_len,
		state->tx.connected_udp,
		header, header_len, payload1, payload1_len, payload2, payload2_len);
}

static void cv610_audio_note_frame(Cv610AudioState *state, size_t bytes,
	int sent)
{
	pthread_mutex_lock(&state->stats_lock);
	state->frames++;
	state->bytes += bytes;
	if (sent)
		state->packets++;
	else
		state->drops++;
	pthread_mutex_unlock(&state->stats_lock);
}

static void *cv610_audio_thread(void *opaque)
{
	Cv610AudioState *state = opaque;
	int first = 1;
	int timeout_reported = 0;

	while (__atomic_load_n(&state->running, __ATOMIC_ACQUIRE)) {
		struct timeval timeout = { 1, 0 };
		ot_audio_stream stream;
		fd_set readfds;
		td_s32 ret;
		int ready;

		FD_ZERO(&readfds);
		FD_SET(state->aenc_fd, &readfds);
		ready = select(state->aenc_fd + 1, &readfds, NULL, NULL, &timeout);
		if (ready < 0 && errno == EINTR)
			continue;
		if (ready < 0) {
			fprintf(stderr, "ERROR: select AENC: %s\n", strerror(errno));
			break;
		}
		if (ready == 0) {
			if (!timeout_reported) {
				fprintf(stderr,
					"WARNING: AENC timeout waiting for audio frame; "
					"suppressing repeats until recovery\n");
				timeout_reported = 1;
			}
			continue;
		}
		if (timeout_reported) {
			fprintf(stdout, "> CV610 AENC frame delivery recovered\n");
			timeout_reported = 0;
		}
		memset(&stream, 0, sizeof(stream));
		ret = ss_mpi_aenc_get_stream(CV610_AENC_CHN, &stream, 0);
		if (ret != TD_SUCCESS)
			continue;
		if (stream.len > 0 && stream.stream) {
			AudioRing *rec = __atomic_load_n(&state->rec_ring,
				__ATOMIC_ACQUIRE);
			int sent = 0;

			/* Tee before the transport: a recording must capture the
			 * audio even when nothing is streaming it. */
			if (rec) {
				struct timespec now;

				clock_gettime(CLOCK_MONOTONIC, &now);
				audio_ring_push(rec, stream.stream,
					(uint16_t)stream.len,
					(uint64_t)now.tv_sec * 1000000ull +
						(uint64_t)now.tv_nsec / 1000ull);
			}
			cv610_audio_begin_frame(state);
			if (state->tx.socket_handle < 0) {
				sent = 1;
			} else {
				int send_ret = rtp_packetizer_send_packet(&state->rtp,
					cv610_audio_write, state, stream.stream, stream.len,
					NULL, 0, first);
				if (send_ret == 0)
					sent = 1;
				else
					/* Expose the local drop as an RTP sequence gap. The
					 * packetizer advances only after writer success. */
					state->rtp.seq++;
			}
			/* RTP time follows capture cadence even when transport drops. */
			state->rtp.timestamp += state->ticks_per_frame;
			cv610_audio_note_frame(state, stream.len, sent);
			if (sent)
				first = 0;
		}
		ret = ss_mpi_aenc_release_stream(CV610_AENC_CHN, &stream);
		if (ret != TD_SUCCESS)
			break;
	}
	/* The loop also exits by break, on an SDK or select error.  Clearing
	 * the flag here makes it mean "capture thread alive", so a status
	 * query cannot report a dead thread as running. */
	__atomic_store_n(&state->running, 0, __ATOMIC_RELEASE);
	return NULL;
}

int cv610_audio_is_running(Cv610AudioState *state)
{
	return state ? __atomic_load_n(&state->running, __ATOMIC_ACQUIRE) : 0;
}

/* Release what a predecessor that died without cv610_audio_stop() left behind.
 *
 * MPP objects are kernel state, not process state, so a venc killed with
 * SIGKILL leaves the AI device claimed by the dead pid. Measured on the .181
 * bench: the next start is then refused at ss_mpi_ai_set_pub_attr with
 * OT_ERR_AI_NOT_PERM (0xa015800d) and the craft comes back SILENTLY WITHOUT
 * AUDIO -- 2 of 2 runs, while a graceful restart restores it every time. The
 * kernel does reclaim the two aenc MMB blocks (those "MMB LEAK(pid=...)" lines
 * are that reclaim reporting itself, not a leak); it does not undo the claim.
 *
 * The module cycle is what does the work. Releasing the objects one by one does
 * not: ss_mpi_ai_disable() is refused with the same NOT_PERM as the
 * set_pub_attr it was meant to unblock. A module-level exit/init pair from a
 * process that has initialised is what clears the claim -- measured, and it is
 * exactly why a SECOND graceful restart recovered audio where the first did
 * not. This folds that cycle into the first start. The pair runs AFTER the
 * caller's own ss_mpi_audio_init() on purpose: that is the ordering verified on
 * hardware.
 *
 * An earlier revision also called sys_unbind/aenc_destroy_chn/opus_deinit/
 * ai_disable_chn here, mirroring cv610_audio_stop()'s teardown. They were
 * removed: the SDK tracks those objects PROCESS-locally, so a successor cannot
 * release a dead predecessor's copies, and all four returned the same code in
 * the broken and healthy cases while only ai_disable's changed. opus_deinit was
 * worse than inert -- it printed "illegal handle(-1)!" to stderr on every clean
 * start, from a process-local static that is -1 in a fresh process. Removal was
 * re-verified on hardware, not assumed. ai_disable is kept because it is the
 * one call measured to reach kernel state.
 *
 * Same shape and the same reason as sys_setup()'s isp_exit/sys_exit/vb_exit. */
static int cv610_audio_preclean(void)
{
	td_s32 exit_ret, init_ret, ai;

	/* audio_initialized is deliberately NOT touched across the cycle. It only
	 * decides whether cv610_audio_stop() calls ss_mpi_audio_exit(), and the two
	 * error directions are not symmetric: a second exit is safe (the module's
	 * sub-exits are flag-guarded, so it is not a double free), while a SKIPPED
	 * exit leaks the kernel AUDIO_INIT claim and the /dev fd -- which is opened
	 * without O_CLOEXEC and therefore survives venc_respawn_after_exit()'s
	 * execv. A partially-succeeded re-init leaves exactly that to clean up, so
	 * the flag must stay set. */
	exit_ret = ss_mpi_audio_exit();
	init_ret = ss_mpi_audio_init();
	ai = ss_mpi_ai_disable(CV610_AUDIO_DEV);
	printf("  audio pre-clean: exit=0x%x init=0x%x ai=0x%x\n",
		   exit_ret, init_ret, ai);

	/* Fatal only when the exit actually happened and the re-init did not: that
	 * is the one combination where this function has taken the module down and
	 * failed to bring it back. If the EXIT failed, nothing was reclaimed and
	 * there is nothing to restore -- returning -1 there would turn a start that
	 * previously worked into a dead one, on a call that was not on the start
	 * path before this change. Let bring-up proceed and let the AUDIO_CHECKs
	 * below produce the real diagnosis. */
	return (exit_ret == TD_SUCCESS && init_ret != TD_SUCCESS) ? -1 : 0;
}

Cv610AudioState *cv610_audio_start(const VencConfig *config,
	const VencOutputUri *video_output)
{
	Cv610AudioState *state;
	struct timespec now;

	if (!config || !config->audio.enabled)
		return NULL;
	state = calloc(1, sizeof(*state));
	if (!state) {
		fprintf(stderr, "ERROR: audio state alloc failed\n");
		return NULL;
	}
	state->acodec_fd = -1;
	state->aenc_fd = -1;
	state->socket_handle = -1;
	if (pthread_mutex_init(&state->stats_lock, NULL) != 0) {
		fprintf(stderr, "ERROR: audio stats lock init failed\n");
		goto fail;
	}
	if (ss_mpi_audio_init() != TD_SUCCESS) {
		/* This is where an absent module set lands, not the /dev/acodec open
		 * below: audio_init opens /dev/ab first, and open_aio -- the module
		 * that creates it -- is staged only under CV610_AUDIO=1
		 * (load-cv610-online). audio.enabled in the JSON is a separate switch,
		 * so the two diverging is a configuration the operator can reach. */
		fprintf(stderr, "ERROR: ss_mpi_audio_init failed "
			"(are the audio modules loaded? they need CV610_AUDIO=1 at "
			"module load, which is separate from audio.enabled)\n");
		goto fail_mutex;
	}
	state->audio_initialized = 1;
	if (cv610_audio_preclean() != 0) {
		fprintf(stderr, "ERROR: audio module re-init failed after pre-clean\n");
		goto fail_started;
	}
	/* Load-bearing order, established by the standalone hardware bring-up. */
	if (cv610_acodec_reset(state) != 0 || cv610_ai_set_attr(state) != 0 ||
		cv610_ai_enable(state) != 0 ||
		cv610_acodec_configure(state, config->audio.mute) != 0 ||
		cv610_aenc_start(state) != 0 ||
		cv610_audio_output_start(state, config, video_output) != 0)
		goto fail_started;
	clock_gettime(CLOCK_MONOTONIC, &now);
	state->rtp.seq = (uint16_t)(now.tv_nsec ^ getpid());
	state->rtp.timestamp = (uint32_t)(now.tv_nsec ^
		(now.tv_sec * CV610_AUDIO_SAMPLE_RATE));
	state->rtp.ssrc = (uint32_t)(now.tv_nsec ^ (getpid() << 8) ^
		(now.tv_sec + 1));
	state->rtp.payload_type = CV610_AUDIO_PAYLOAD_TYPE;
	state->ticks_per_frame = CV610_AUDIO_POINT_NUM;
	state->aenc_fd = ss_mpi_aenc_get_fd(CV610_AENC_CHN);
	if (state->aenc_fd < 0) {
		fprintf(stderr, "ERROR: ss_mpi_aenc_get_fd=0x%x\n", state->aenc_fd);
		goto fail_started;
	}
	__atomic_store_n(&state->running, 1, __ATOMIC_RELEASE);
	if (pthread_create(&state->thread, NULL, cv610_audio_thread, state) != 0) {
		__atomic_store_n(&state->running, 0, __ATOMIC_RELEASE);
		fprintf(stderr, "ERROR: create CV610 audio thread failed\n");
		goto fail_started;
	}
	state->thread_started = 1;
	return state;

fail_started:
	cv610_audio_stop(state);
	return NULL;
fail_mutex:
	pthread_mutex_destroy(&state->stats_lock);
fail:
	free(state);
	return NULL;
}

void cv610_audio_set_record_ring(Cv610AudioState *state, AudioRing *ring)
{
	if (!state)
		return;
	__atomic_store_n(&state->rec_ring, ring, __ATOMIC_RELEASE);
}

void cv610_audio_stop(Cv610AudioState *state)
{
	ot_mpp_chn source = { OT_ID_AI, CV610_AUDIO_DEV, CV610_AI_CHN };
	ot_mpp_chn destination = { OT_ID_AENC, 0, CV610_AENC_CHN };

	if (!state)
		return;
	if (state->thread_started) {
		__atomic_store_n(&state->running, 0, __ATOMIC_RELEASE);
		pthread_join(state->thread, NULL);
	}
	if (state->bound)
		(void)ss_mpi_sys_unbind(&source, &destination);
	if (state->aenc_created)
		(void)ss_mpi_aenc_destroy_chn(CV610_AENC_CHN);
	if (state->aenc_opus_initialized)
		(void)ss_mpi_aenc_opus_deinit();
	if (state->ai_chn_enabled)
		(void)ss_mpi_ai_disable_chn(CV610_AUDIO_DEV, CV610_AI_CHN);
	if (state->ai_enabled)
		(void)ss_mpi_ai_disable(CV610_AUDIO_DEV);
	if (state->acodec_fd >= 0)
		close(state->acodec_fd);
	if (state->audio_initialized)
		(void)ss_mpi_audio_exit();
	if (state->socket_handle >= 0)
		close(state->socket_handle);
	pthread_mutex_destroy(&state->stats_lock);
	free(state);
}

void cv610_audio_get_stats(Cv610AudioState *state, uint64_t *frames,
	uint64_t *bytes, uint64_t *packets, uint64_t *drops)
{
	if (!state)
		return;
	pthread_mutex_lock(&state->stats_lock);
	if (frames)
		*frames = state->frames;
	if (bytes)
		*bytes = state->bytes;
	if (packets)
		*packets = state->packets;
	if (drops)
		*drops = state->drops;
	pthread_mutex_unlock(&state->stats_lock);
}
