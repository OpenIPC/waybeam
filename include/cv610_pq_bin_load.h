#ifndef CV610_PQ_BIN_LOAD_H
#define CV610_PQ_BIN_LOAD_H

#include <stddef.h>

/* Whole-file loader for the PQTools `.bin` import, split out so the size
 * policy is host-testable without the vendor blob or the SigmaStar SDK.
 *
 * The vendor NRX reader reads a count out of the file and copies a fixed
 * 1298 bytes from `count` 4-byte entries past a sub-header:
 * `OT_PQ_BIN_ImportNRXData` passes `section + 36` to `PQ_BIN_SetPipeNRXParam`
 * (which reaches `PQ_BIN_SetNRDataV2`, memcpy_s at 0x12fc), the count sits at
 * sub-header +8, and the copy starts at sub-header +12 + 4*count.  With the
 * accepted count ceiling of 16 the copy can therefore reach
 * `36 + 12 + 4*16 + 1298 = 1410` bytes from the section start, while the
 * import's library-size gate (`OT_PQ_GetStructParamLen`) reports only 1350 --
 * so a buffer sized exactly for the file can be read up to 60 bytes past its
 * end, and those bytes reach `ss_mpi_vi_set_pipe_3dnr_param`.  This loader
 * allocates the reader's full reach as zeroed slack past the file's bytes, so
 * the bound holds whatever the gate reports.
 *
 * The "accepted count ceiling of 16" is not our assumption: it is enforced by
 * the vendor reader itself.  Disassembled `libbin.so` (v7, ARM EABI5):
 * `PQ_BIN_SetNRDataV2` loads the count with a 32-bit `ldr` from
 * `src + 8` (i.e. section + 44) and, at offset 0x12e0, computes `count - 1`
 * and rejects anything above 15 with a `bls` guard BEFORE the `memcpy_s` at
 * 0x12fc.  `OT_PQ_BIN_ImportNRXData` (0x14e4) passes `r1 + 36` as that `src`.
 * So no input can make the reader reach past `section + 1410`, and a file with
 * a larger count is refused rather than over-read.  Verified 2026-09-14; no
 * extra count validation is needed here. */
#define CV610_PQ_NRX_VENDOR_MAX 1410u

/* Read `path` whole into a fresh buffer carrying CV610_PQ_NRX_VENDOR_MAX of
 * zeroed slack past the file.  Refuses files larger than `max_bytes`.
 * Returns NULL on any failure, otherwise a malloc'd buffer the caller frees;
 * `*out_len` receives the file length (not the allocation size). */
unsigned char *cv610_pq_bin_load(const char *path, size_t max_bytes,
	size_t *out_len);

#endif /* CV610_PQ_BIN_LOAD_H */
