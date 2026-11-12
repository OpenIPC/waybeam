/* test_cv610_pq_bin_load.c — the PQ bin loader's vendor-copy slack.
 *
 * The import's library-size gate accepts a 1350-byte 3DNR section, but the
 * vendor reader copies a fixed 1298 bytes from `count` 4-byte entries past the
 * OT_PQ_BIN_NRX sub-header it is handed: `OT_PQ_BIN_ImportNRXData` passes
 * `section + 36`, the count sits at sub-header +8 (section +44), and the copy
 * starts at sub-header +12 + 4*count.  With the accepted count ceiling of 16
 * the copy reaches `36 + 12 + 4*16 + 1298 = 1410` bytes from the section start,
 * i.e. up to 60 bytes past a minimal file.  The loader must allocate that
 * reach as slack, or the vendor's copy leaves the buffer -- the original heap
 * overread.
 *
 * The check performs the vendor's worst-case copy out of the loaded buffer.
 * On plain `make test` a 60-byte heap overread usually survives inside the
 * malloc rounding, so the guard that bites is `make test-asan` / `make test-ci`
 * (CI): there the loop is a heap-buffer-overflow, which a loader that sizes the
 * allocation for the file alone would trip. */
#include "cv610_pq_bin_load.h"
#include "test_helpers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TEST_PATH "/tmp/venc_test_pq_load.bin"
#define CAP       (8u * 1024u * 1024u)

static int write_pattern(const char *path, size_t len)
{
	FILE *f = fopen(path, "wb");
	size_t i;

	if (!f)
		return 0;
	for (i = 0; i < len; i++) {
		unsigned char b = (unsigned char)(i & 0xff);
		if (fwrite(&b, 1, 1, f) != 1) {
			fclose(f);
			return 0;
		}
	}
	return fclose(f) == 0;
}

int test_cv610_pq_bin_load(void)
{
	int failures = 0;
	const size_t file_len = 4096;
	const size_t section = 1350;
	size_t len = 0;
	unsigned char *buf;
	volatile unsigned sum = 0;
	size_t isp_len, copy_start, copy_end, i;

	unlink(TEST_PATH);
	CHECK("fixture write", write_pattern(TEST_PATH, file_len));

	/* A missing file fails cleanly, no crash. */
	CHECK("missing file refused",
		cv610_pq_bin_load("/tmp/venc_test_pq_load_absent.bin", CAP,
			&len) == NULL);

	buf = cv610_pq_bin_load(TEST_PATH, CAP, &len);
	CHECK("load succeeds", buf != NULL);
	CHECK("length is the file length", len == file_len);
	if (buf) {
		CHECK("content round-trips",
			buf[0] == 0 && buf[255] == 255 &&
			buf[file_len - 1] == (unsigned char)((file_len - 1) & 0xff));

		/* Simulate the vendor's worst case: isp_len ends 1350 bytes
		 * before the file end, the copy starts 36 + 12 + 4*16 into
		 * the section and runs 1298 bytes, ending 60 bytes past the
		 * file (1410 from the section start).  Touching it is only
		 * in-bounds because of the loader slack. */
		isp_len = file_len - section;
		copy_start = isp_len + 36 + 12 + 4 * 16;
		copy_end = copy_start + 1298;
		CHECK("vendor copy extends past the file bytes",
			copy_end == file_len + 60);
		for (i = copy_start; i < copy_end; i++)
			sum += buf[i];
		CHECK("slack past the file is zeroed", buf[copy_end - 1] == 0);
		CHECK("copy touched", sum != 0);
		free(buf);
	}

	/* The size cap is enforced. */
	CHECK("over-cap file refused", cv610_pq_bin_load(TEST_PATH, 1024,
		&len) == NULL);

	unlink(TEST_PATH);
	return failures;
}
