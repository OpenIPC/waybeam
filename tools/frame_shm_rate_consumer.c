/* Rate-limited frame-SHM consumer — emulates a link with reduced capacity.
 *
 * The SIGSTOP/SIGCONT consumer used to characterise the frame gate is a
 * BINARY load: the link is either at full capacity or dead.  The realistic FPV
 * case is neither — the link degrades to some fraction of capacity and stays
 * there.  This drains at most `target_fps` frames per second and reports, once
 * per second, what the producer actually did in response.
 *
 * The question it answers: under sustained partial congestion, does the gate
 * settle the producer at a steady reduced rate, or does it oscillate between
 * full rate and zero?  Judder is unacceptable for FPV even when the mean rate
 * is right, so the per-second series matters more than the average.
 *
 * Usage: frame_shm_rate_consumer <ring> <seconds> <target_fps>
 *
 * Cross-compile for Star6E:
 *   toolchain/toolchain.sigmastar-infinity6e/bin/arm-openipc-linux-gnueabihf-gcc \
 *     -Os -Iinclude -D_GNU_SOURCE \
 *     tools/frame_shm_rate_consumer.c src/venc_frame_ring.c \
 *     -lpthread -o frame_shm_rate_consumer
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <signal.h>
#include <stdint.h>

#include "venc_frame_ring.h"

static volatile int running = 1;
static void sighandler(int sig) { (void)sig; running = 0; }

static uint64_t now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)(ts.tv_nsec / 1000);
}

int main(int argc, char **argv)
{
	const char *ring_name = (argc > 1) ? argv[1] : "venc_frame";
	int duration_s        = (argc > 2) ? atoi(argv[2]) : 20;
	int target_fps        = (argc > 3) ? atoi(argv[3]) : 40;
	uint64_t period_us;
	venc_frame_ring_t *r;
	uint8_t *buf;
	size_t buf_size = 1024 * 1024;
	uint32_t out_len = 0;
	uint64_t t_start, t_next, t_sec;
	unsigned long sec_frames = 0, total_frames = 0, total_idr = 0;
	unsigned long sec_idx = 0;
	/* Per-second delivered counts, so the series can be judged for judder
	 * rather than hidden inside a mean. */
	unsigned long series[600];
	unsigned long fill_sum = 0, fill_n = 0, fill_max = 0;
	/* Age at read = wall clock minus the frame's capture pts.  The pts is
	 * the SDK's packet timestamp, whose epoch need not match
	 * CLOCK_MONOTONIC, so the absolute value is not a latency.  The
	 * DIFFERENCE between a healthy run and a gated one is, because the
	 * offset cancels — that difference is what the gate adds on the venc
	 * side, capture to ring exit. */
	unsigned long long age_sum = 0;
	unsigned long age_n = 0;
	uint32_t age_min = 0xFFFFFFFFu, age_max = 0;

	if (target_fps <= 0)
		target_fps = 40;
	period_us = 1000000u / (uint64_t)target_fps;

	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);

	r = venc_frame_ring_attach(ring_name);
	if (!r) {
		fprintf(stderr, "Failed to attach to frame ring '%s'\n",
			ring_name);
		return 1;
	}
	buf = malloc(buf_size);
	if (!buf) {
		venc_frame_ring_destroy(r);
		return 1;
	}
	printf("Rate consumer on '%s': target %d fps (%llu us/frame), %d s\n",
		ring_name, target_fps, (unsigned long long)period_us,
		duration_s);
	fflush(stdout);

	t_start = now_us();
	t_next  = t_start;
	t_sec   = t_start;

	while (running) {
		uint64_t t = now_us();
		venc_frame_ring_fill_t fill;

		if (duration_s > 0 &&
		    (t - t_start) >= (uint64_t)duration_s * 1000000u)
			break;

		/* Pace: never read faster than the emulated link allows. */
		if (t < t_next) {
			struct timespec ts;
			uint64_t d = t_next - t;
			ts.tv_sec  = (time_t)(d / 1000000u);
			ts.tv_nsec = (long)((d % 1000000u) * 1000u);
			nanosleep(&ts, NULL);
			continue;
		}

		if (venc_frame_ring_read(r, buf, buf_size, &out_len) == 0 &&
		    out_len >= VENC_FRAME_META_SIZE) {
			VencFrameMeta m;

			memcpy(&m, buf, VENC_FRAME_META_SIZE);
			if (m.flags & VENC_FRAME_FLAG_IDR)
				total_idr++;
			{
				/* 32-bit µs wraps every ~71 min; the subtraction
				 * is correct across one wrap by construction. */
				uint32_t age = (uint32_t)now_us() - m.pts;

				age_sum += age;
				age_n++;
				if (age < age_min) age_min = age;
				if (age > age_max) age_max = age;
			}
			total_frames++;
			sec_frames++;
			t_next += period_us;
		} else {
			/* Nothing queued: the producer is behind, not us. */
			t_next = now_us();
		}

		if (venc_frame_ring_get_fill(r, &fill) == 0) {
			fill_sum += fill.used_slots;
			fill_n++;
			if (fill.used_slots > fill_max)
				fill_max = fill.used_slots;
		}

		t = now_us();
		if (t - t_sec >= 1000000u) {
			if (sec_idx < sizeof(series) / sizeof(series[0]))
				series[sec_idx++] = sec_frames;
			printf("  t=%3lus  delivered=%3lu fps  ring=%u\n",
				sec_idx, sec_frames,
				(unsigned)(fill_n ? fill_max : 0));
			fflush(stdout);
			sec_frames = 0;
			fill_max = 0;
			t_sec = t;
		}
	}

	printf("\n=== Rate-limited consumer results ===\n");
	printf("Target fps:   %d\n", target_fps);
	printf("Frames:       %lu\n", total_frames);
	printf("IDR frames:   %lu\n", total_idr);
	if (age_n) {
		printf("Age at read:  mean %.1f ms  min %.1f  max %.1f"
			"   (epoch-offset included; compare runs)\n",
			(double)age_sum / (double)age_n / 1000.0,
			(double)age_min / 1000.0, (double)age_max / 1000.0);
	}
	printf("Mean ring:    %.2f slots\n",
		fill_n ? (double)fill_sum / (double)fill_n : 0.0);
	printf("Series:      ");
	for (unsigned long i = 0; i < sec_idx; i++)
		printf(" %lu", series[i]);
	printf("\n");

	/* Judder metric: the spread of the per-second series.  A producer that
	 * settles at the link rate gives a tight series; one that alternates
	 * full-rate and zero gives the same mean with a wide spread. */
	if (sec_idx > 1) {
		unsigned long mn = series[0], mx = series[0], sum = 0;
		for (unsigned long i = 0; i < sec_idx; i++) {
			if (series[i] < mn) mn = series[i];
			if (series[i] > mx) mx = series[i];
			sum += series[i];
		}
		printf("Per-second:   min %lu  max %lu  mean %.1f  spread %lu\n",
			mn, mx, (double)sum / (double)sec_idx, mx - mn);
	}

	free(buf);
	venc_frame_ring_destroy(r);
	return 0;
}
