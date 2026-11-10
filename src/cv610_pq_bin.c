/* PQTools `.bin` import/export for the Hi3516CV610 ISP.
 *
 * The vendor `libbin.so` carries no public header in the package we have, so
 * the call contract below was recovered from the stripped blob.  Every offset
 * and gate named in a comment here is a disassembly finding, not a guess from
 * a related SoC's header -- the older `HI_PQ_BIN` API on Hi3516CV100 has a
 * different shape and its error codes do not apply. */

#include "cv610_pq_bin.h"

#include "cv610_pipeline.h"
#include "file_util.h"
#include "cv610_pq_bin_load.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "ot_type.h"
#include "ss_mpi_sys.h"

#define PQ_LOG        "[pq] "
#define PQ_BIN_LIB    "libbin.so"
#define PQ_AE_LIB     "libss_mpi_ae.so"

/* The pipe cv610_iq.c drives; libbin reaches the 3DNR parameters through it. */
#define PQ_VI_PIPE    0

/* A whole ISP parameter image is ~140 KB.  The cap only exists so a wrong
 * path cannot turn into an unbounded allocation. */
#define PQ_BIN_MAX_BYTES (8u * 1024u * 1024u)

/* Smallest length we will believe from OT_PQ_GetISPDataTotalLen.  It answers 4
 * on an empty module table; a real image on this SDK is 143424. */
#define PQ_ISP_LEN_FLOOR 1024u

/* Allocation slack for export; see the comment at the calloc. */
#define PQ_EXPORT_SLACK 64u

/* Mirrors the fields libbin.so actually reads: OT_PQ_BIN_ImportBinData (@0xd9c)
 * reads the two enables at +0 and +4, and OT_PQ_BIN_ImportNRXData (@0x14e4)
 * passes +8 to PQ_BIN_SetPipeNRXParam as the VI pipe.  The vendor struct's tail
 * is unidentified, so the pad keeps anything past +8 reading as zero instead of
 * off the end of our allocation. */
typedef struct {
	int isp_enable;
	int nr_enable;
	int vi_pipe;
	int reserved[13];
} PqBinModule;

typedef struct {
	void *bin;
	void *ae;
	/* Storage that libbin's `g_aeHandle` is pointed AT -- it is a `void **`
	 * and the library loads through it, so the pointed-to object has to
	 * outlive pq_lib_open().  It does not have to outlive the caller: this
	 * struct is a local in import/export and lives across the whole transfer,
	 * which is the last moment libbin reads it.  Keeping it here rather than
	 * at file scope is what avoids adding global mutable state. */
	void *ae_handle_slot;
	void *import_bin;      /* int  (*)(PqBinModule *, unsigned char *, unsigned int) */
	void *export_bin;      /* int  (*)(PqBinModule *, unsigned char *, unsigned int) */
	void *isp_total_len;   /* unsigned int (*)(void) */
	void *struct_param_len;/* unsigned int (*)(PqBinModule *) */
} PqBinLib;

typedef int (*pq_transfer_fn)(PqBinModule *, unsigned char *, unsigned int);
typedef unsigned int (*pq_isp_len_fn)(void);
typedef unsigned int (*pq_struct_len_fn)(PqBinModule *);

static void pq_lib_close(PqBinLib *lib)
{
	if (lib->bin) {
		/* Clear libbin's g_aeHandle before it can outlive the slot it points
		 * at.  musl's dlclose is a no-op, so the library stays mapped with
		 * whatever we last wrote there; leaving it aimed at a returned
		 * stack frame would be a dangling pointer for the next caller to
		 * trip over. */
		void **ae_slot = (void **)dlsym(lib->bin, "g_aeHandle");

		if (ae_slot)
			*ae_slot = NULL;
		dlclose(lib->bin);
	}
	if (lib->ae)
		dlclose(lib->ae);
	memset(lib, 0, sizeof(*lib));
}

/* libbin.so's DT_NEEDED lists only libc, but it carries undefined ss_mpi_*
 * symbols that the host process must already provide.  RTLD_NOW is deliberate:
 * a process that cannot satisfy them should fail here, with dlerror() naming
 * the symbol, rather than part-way through writing ISP registers. */
static int pq_lib_open(PqBinLib *lib)
{
	void **ae_slot;

	memset(lib, 0, sizeof(*lib));

	/* RTLD_LOCAL: every entry point is reached through dlsym() on this
	 * handle, so libbin never needs to export into the global scope -- and
	 * musl's dlclose is a no-op, so anything it did export would stay there
	 * for the life of the process, where it could satisfy a later plugin's
	 * lookup.  The undefined ss_mpi_* symbols it carries resolve from the
	 * executable and its DT_NEEDED libraries either way; that is what makes
	 * RTLD_NOW here a real check rather than a formality. */
	lib->bin = dlopen(PQ_BIN_LIB, RTLD_NOW | RTLD_LOCAL);
	if (!lib->bin) {
		fprintf(stderr, "WARNING: %sunable to load %s (%s)\n", PQ_LOG,
			PQ_BIN_LIB, dlerror());
		return -1;
	}

	lib->import_bin = dlsym(lib->bin, "OT_PQ_BIN_ImportBinData");
	lib->export_bin = dlsym(lib->bin, "OT_PQ_BIN_ExportBinData");
	lib->isp_total_len = dlsym(lib->bin, "OT_PQ_GetISPDataTotalLen");
	lib->struct_param_len = dlsym(lib->bin, "OT_PQ_GetStructParamLen");
	if (!lib->import_bin || !lib->export_bin || !lib->isp_total_len ||
		!lib->struct_param_len) {
		fprintf(stderr, "WARNING: %s%s is missing a PQ bin entry point\n",
			PQ_LOG, PQ_BIN_LIB);
		pq_lib_close(lib);
		return -1;
	}

	/* libbin resolves the AE MPI through this global rather than through a
	 * link-time dependency.  Left unset, the AE ext-register record -- one of
	 * the three the file carries -- has nowhere to land.
	 *
	 * The indirection is the trap: g_aeHandle is a `void **`, and libbin
	 * dereferences it TWICE (BIN_OpenDllFunc loads the global, then loads
	 * through it before calling dlsym).  Storing the dlopen handle directly
	 * makes it dlsym(*(void **)handle, ...) -- on musl that reads the first
	 * word of struct dso and resolves nothing, silently, while we print
	 * success.  So point the global at a variable that HOLDS the handle, and
	 * give that variable static lifetime: libbin reads it after this
	 * function returns. */
	lib->ae = dlopen(PQ_AE_LIB, RTLD_NOW | RTLD_LOCAL);
	ae_slot = (void **)dlsym(lib->bin, "g_aeHandle");
	if (!lib->ae || !ae_slot) {
		/* Clear it: a previous open may have left a handle there, and a
		 * stale one is worse than none. */
		if (ae_slot)
			*ae_slot = NULL;
		fprintf(stderr, "WARNING: %sno %s handle; AE parameters will not "
			"transfer\n", PQ_LOG, PQ_AE_LIB);
	} else {
		lib->ae_handle_slot = lib->ae;
		*ae_slot = &lib->ae_handle_slot;
	}

	return 0;
}

/* Both transfers refuse to run unless the ISP reports a tuning connection --
 * the same channel PQTools uses live.  We restore the previous value so a
 * one-shot import does not leave the ISP in tuning mode for the rest of the
 * session. */
static int pq_tuning_begin(td_s32 *saved)
{
	td_s32 connected = 0;

	if (ss_mpi_sys_get_tuning_connect(&connected) != TD_SUCCESS) {
		fprintf(stderr, "WARNING: %scannot read the tuning connect state\n",
			PQ_LOG);
		return -1;
	}
	*saved = connected;
	if (connected)
		return 0;

	if (ss_mpi_sys_set_tuning_connect(1) != TD_SUCCESS) {
		fprintf(stderr, "WARNING: %scannot enable the tuning connection\n",
			PQ_LOG);
		return -1;
	}
	return 0;
}

static void pq_tuning_end(td_s32 saved)
{
	if (!saved)
		(void)ss_mpi_sys_set_tuning_connect(0);
}

int cv610_pq_bin_available(void)
{
	/* Deliberately narrower than pq_lib_open(): this only answers "is the blob
	 * here and loadable", so it touches none of the library's own globals.
	 * The answer gates what /api/v1/capabilities advertises -- a craft flashed
	 * without libbin.so should not claim a control surface it cannot serve.
	 *
	 * Not memoized.  cv610_init() is the single caller and calls it once, so a
	 * cache would buy nothing and would be exactly the file-scope mutable
	 * state the conventions rule out. */
	int ok;
	void *h;

	h = dlopen(PQ_BIN_LIB, RTLD_NOW | RTLD_LOCAL);
	if (!h) {
		fprintf(stderr, "WARNING: %s%s not loadable (%s); isp.sensorBin and "
			"the .bin export are unavailable on this craft\n", PQ_LOG,
			PQ_BIN_LIB, dlerror());
		return 0;
	}
	/* All four, matching what pq_lib_open() insists on: a partial blob that
	 * resolves only the import symbol would answer "available" here and then
	 * fail every export. */
	ok = dlsym(h, "OT_PQ_BIN_ImportBinData") != NULL &&
		dlsym(h, "OT_PQ_BIN_ExportBinData") != NULL &&
		dlsym(h, "OT_PQ_GetISPDataTotalLen") != NULL &&
		dlsym(h, "OT_PQ_GetStructParamLen") != NULL;
	if (!ok)
		fprintf(stderr, "WARNING: %s%s is missing a PQ entry point\n", PQ_LOG,
			PQ_BIN_LIB);
	dlclose(h);
	return ok;
}

int cv610_pq_bin_import(const char *path)
{
	unsigned int isp_len, nrx_len, remainder;
	PqBinModule mod;
	PqBinLib lib;
	unsigned char *buf;
	td_s32 saved;
	size_t len = 0;
	int ret;

	if (!path || !*path)
		return 0;

	/* The same readiness gate every cv610_iq entry point makes.  Both call
	 * sites are after pipeline start today, so this cannot bite -- it is here
	 * because a module that writes ISP registers should not be the one that
	 * omits it. */
	if (!cv610_pipeline_isp_ready()) {
		fprintf(stderr, "WARNING: %sISP not running; %s not applied\n",
			PQ_LOG, path);
		return -1;
	}

	if (file_util_validate_regular_file(path, "PQ bin", PQ_LOG) != 0)
		return -1;

	if (pq_lib_open(&lib) != 0)
		return -1;

	buf = cv610_pq_bin_load(path, PQ_BIN_MAX_BYTES, &len);
	if (!buf) {
		pq_lib_close(&lib);
		return -1;
	}

	/* The file is [ISP image of exactly this length][optional NRX section].
	 * The length is a property of the running SDK, so asking the library is
	 * also the version check: a tune built against a different ISP generation
	 * does not have an ISP image of this size. */
	isp_len = ((pq_isp_len_fn)lib.isp_total_len)();
	/* OT_PQ_GetISPDataTotalLen returns 4 -- the record-count word alone --
	 * when its module table is empty, so "== 0" is not the degenerate case.
	 * Anything under a kilobyte is not an ISP image. */
	if (isp_len < PQ_ISP_LEN_FLOOR || len < isp_len) {
		fprintf(stderr, "WARNING: %s%s holds %zu bytes; this SDK's ISP image "
			"is %u -- wrong ISP version for this chip\n", PQ_LOG, path, len,
			isp_len);
		free(buf);
		pq_lib_close(&lib);
		return -1;
	}
	remainder = (unsigned int)(len - isp_len);

	memset(&mod, 0, sizeof(mod));
	mod.isp_enable = 1;
	mod.vi_pipe = PQ_VI_PIPE;

	/* Gate the 3DNR half on the section length the library itself expects.
	 * A header-size check is not enough: ImportNRXData does not forward our
	 * length to PQ_BIN_SetNRDataV2, which memcpy's a fixed 1298-byte payload
	 * out of the buffer and only compares the declared size AFTERWARDS.  So a
	 * validly-headed but truncated file -- an interrupted scp, a full tmpfs --
	 * would read past the end of this allocation and push uninitialised heap
	 * into the 3DNR registers.  Asking the library for the size is the same
	 * authority we already trust for the ISP half.
	 *
	 * That answer is a gate, not a bound: the reader's interior offset grows
	 * with a per-file count, so its fixed copy can reach
	 * CV610_PQ_NRX_VENDOR_MAX bytes from the section while the library reports
	 * only 1350.  cv610_pq_bin_load() therefore sizes the buffer for the
	 * reader's worst case, not the file, and the slack is zeroed.  The gate
	 * below stays as it was -- the file-size check must not be tightened on
	 * the reader's internal layout, or valid tunes are refused.
	 *
	 * Skipping the section rather than letting the library refuse it is
	 * deliberate: its refusal is the return value of the WHOLE call and would
	 * mask an ISP half that landed. */
	nrx_len = ((pq_struct_len_fn)lib.struct_param_len)(&mod);
	mod.nr_enable = (nrx_len && remainder >= nrx_len) ? 1 : 0;
	if (remainder && !mod.nr_enable)
		fprintf(stderr, "WARNING: %s%s carries %u trailing bytes; the 3DNR "
			"section needs %u -- importing the ISP half only\n", PQ_LOG, path,
			remainder, nrx_len);

	if (pq_tuning_begin(&saved) != 0) {
		free(buf);
		pq_lib_close(&lib);
		return -1;
	}
	printf("> %sloading %s (%u B ISP + %u B 3DNR)\n", PQ_LOG, path, isp_len,
		mod.nr_enable ? nrx_len : 0);
	ret = ((pq_transfer_fn)lib.import_bin)(&mod, buf, (unsigned int)len);
	pq_tuning_end(saved);

	free(buf);
	pq_lib_close(&lib);

	if (ret != 0) {
		fprintf(stderr, "ERROR: %sOT_PQ_BIN_ImportBinData failed 0x%x for %s\n",
			PQ_LOG, (unsigned)ret, path);
		return -1;
	}
	/* The image carried an AE ext-register record, so the ceilings latched
	 * from the pre-import ISP are stale.  Invalidating them is the CALLER's
	 * job: it owns that cache, and this module has no business reaching into
	 * a sibling's state to do it. */
	printf("> %sISP tuning applied from %s\n", PQ_LOG, path);
	return 0;
}

int cv610_pq_bin_export(const char *path)
{
	unsigned int isp_len, nrx_len, need;
	PqBinModule mod;
	PqBinLib lib;
	unsigned char *buf;
	struct stat st;
	td_s32 saved;
	FILE *f;
	int ret, fd;

	if (!path || !*path) {
		fprintf(stderr, "WARNING: %sexport needs a destination path\n", PQ_LOG);
		return -1;
	}

	if (pq_lib_open(&lib) != 0)
		return -1;

	memset(&mod, 0, sizeof(mod));
	mod.isp_enable = 1;
	mod.vi_pipe = PQ_VI_PIPE;

	isp_len = ((pq_isp_len_fn)lib.isp_total_len)();
	nrx_len = ((pq_struct_len_fn)lib.struct_param_len)(&mod);

	/* nr_enable is decided by the ANSWER, not asserted before the question.
	 * OT_PQ_GetStructParamLen returns 0 when the 3DNR query or its internal
	 * malloc fails, and ExportBinData recomputes the same sum and requires
	 * EXACT equality with our length -- so a hardcoded nr_enable=1 with a
	 * zero-length section passes that check and then writes the 37-byte NRX
	 * header at buf + isp_len, one past the end.  Asking for zero bytes of
	 * 3DNR is the one request the library will happily overrun. */
	mod.nr_enable = nrx_len ? 1 : 0;
	need = isp_len + (mod.nr_enable ? nrx_len : 0);
	if (isp_len < PQ_ISP_LEN_FLOOR || need < isp_len ||
		need > PQ_BIN_MAX_BYTES) {
		fprintf(stderr, "WARNING: %simplausible export size %u + %u\n", PQ_LOG,
			isp_len, nrx_len);
		pq_lib_close(&lib);
		return -1;
	}
	if (!mod.nr_enable)
		fprintf(stderr, "WARNING: %s3DNR parameters unavailable; exporting the "
			"ISP half only\n", PQ_LOG);

	/* Slack, but `need` is still what we pass as the length.  The vendor's
	 * two size formulas disagree with each other -- the writer branches on a
	 * different field value than the sizer does, and on a path this chip does
	 * not take it would emit up to 1410 bytes into a 1350-byte region.  The
	 * length argument must stay exact (the check is a != , not a <=), so the
	 * only place to absorb that is the allocation.  The worst-case overhang
	 * is 1410 - 1350 = 60 bytes, inside PQ_EXPORT_SLACK (64). */
	buf = calloc(1, (size_t)need + PQ_EXPORT_SLACK);
	if (!buf) {
		fprintf(stderr, "WARNING: %scannot allocate %u bytes for export\n",
			PQ_LOG, need);
		pq_lib_close(&lib);
		return -1;
	}

	if (pq_tuning_begin(&saved) != 0) {
		free(buf);
		pq_lib_close(&lib);
		return -1;
	}
	ret = ((pq_transfer_fn)lib.export_bin)(&mod, buf, need);
	pq_tuning_end(saved);
	pq_lib_close(&lib);

	if (ret != 0) {
		fprintf(stderr, "ERROR: %sOT_PQ_BIN_ExportBinData failed 0x%x\n",
			PQ_LOG, (unsigned)ret);
		free(buf);
		return -1;
	}

	/* O_NOFOLLOW, and then confirm what we actually opened.
	 *
	 * The HTTP handler always passes a predictable path in a world-writable
	 * /tmp, and the endpoint is unauthenticated, so a local user can plant a
	 * symlink there and redirect this write to anything the daemon may write.
	 * O_NOFOLLOW refuses a symlinked final component; the fstat then rejects
	 * anything that is not a plain, single-linked regular file, which is what
	 * catches a planted HARD link -- O_NOFOLLOW does not, since a hard link is
	 * the same inode rather than a link object.
	 *
	 * Truncation is deliberately NOT in the open flags.  O_TRUNC executes
	 * inside open(), i.e. BEFORE the guard below can refuse, so a planted hard
	 * link to (say) the daemon's own log would be zeroed on the way to being
	 * rejected -- the refusal would destroy the file it declined to write.
	 * Truncate only once the fd is known to be a private regular file.
	 *
	 * O_NONBLOCK likewise matters: a planted FIFO would otherwise park open()
	 * forever, and this runs on the single HTTP dispatch thread under its
	 * mutex, so it would wedge the whole API and deadlock teardown's pause.
	 * Regular files ignore the flag, so nothing needs clearing afterwards.
	 *
	 * Deliberately NOT unlink-then-create: that reopens a window to re-plant
	 * between the two calls. */
	fd = open(path, O_WRONLY | O_CREAT | O_NOFOLLOW | O_NONBLOCK, 0644);
	if (fd < 0) {
		fprintf(stderr, "ERROR: %scannot write %s (%s)\n", PQ_LOG, path,
			strerror(errno));
		free(buf);
		return -1;
	}
	if (fstat(fd, &st) != 0) {
		fprintf(stderr, "ERROR: %scannot stat %s (%s); refusing to write\n",
			PQ_LOG, path, strerror(errno));
		close(fd);
		free(buf);
		return -1;
	}
	if (!S_ISREG(st.st_mode) || st.st_nlink != 1) {
		fprintf(stderr, "ERROR: %s%s is not a private regular file "
			"(mode 0%o, links %lu); refusing to write\n", PQ_LOG, path,
			(unsigned)(st.st_mode & 07777), (unsigned long)st.st_nlink);
		close(fd);
		free(buf);
		return -1;
	}
	if (ftruncate(fd, 0) != 0) {
		fprintf(stderr, "ERROR: %scannot truncate %s (%s)\n", PQ_LOG, path,
			strerror(errno));
		close(fd);
		free(buf);
		return -1;
	}
	f = fdopen(fd, "wb");
	if (!f) {
		fprintf(stderr, "ERROR: %scannot write %s\n", PQ_LOG, path);
		close(fd);
		free(buf);
		return -1;
	}
	if (fwrite(buf, 1, need, f) != need) {
		fprintf(stderr, "ERROR: %sshort write on %s\n", PQ_LOG, path);
		fclose(f);
		free(buf);
		return -1;
	}
	if (fclose(f) != 0) {
		fprintf(stderr, "ERROR: %scannot flush %s\n", PQ_LOG, path);
		free(buf);
		return -1;
	}
	free(buf);

	printf("> %sISP state exported to %s (%u B)\n", PQ_LOG, path, need);
	return (int)need;
}
