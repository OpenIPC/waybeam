# CV610 (Hi3516CV610) ISP tuning blobs

PQTools `.bin` files for the HiSilicon CV610 ISP. Applied by
`src/cv610_pq_bin.c` through the vendor `libbin.so`, live via the
`isp.sensorBin` config field and once at cold boot.

Not the same format as `../maruko-bin/*.bin`, despite the shared extension
and the shared config field. Those are SigmaStar blobs loaded with
`MI_ISP_*_CmdLoadBinFile`; these are HiSilicon PQ images imported with
`OT_PQ_BIN_ImportBinData`. The two are not interchangeable.

## What's here

| File | Sensor | Size | Source |
|---|---|---|---|
| `imx662.bin` | IMX662 2 MP | 144774 B | exported from our own `.181` bench, 2026-09-05 |

MD5s are in `MD5SUMS`.

## Provenance — read this before treating it as a vendor tune

`imx662.bin` is **our own state, not a vendor tune.** It is what
`GET /api/v1/iq/export_bin` serialized out of the running ISP on
`192.168.2.181` (venc 0.80.0, backend cv610) at a cold-boot state: the IMX662
sensor plugin's compiled-in `cmos_get_isp_default()` seeds from PR #229, plus
everything venc applies on top at startup.

It is therefore a **snapshot for posterity and a restore point**, not a
measured tune. Nobody has run PQTools against this sensor. Re-exporting after
real tuning work is the whole point of keeping the exporter.

It also predates 0.85.7, which moved the plugin's ISP black level from 200 to
800 (`IMX662_BLACK_LEVEL`). The snapshot carries the old 200, so importing it
puts that back — and with it the blue lift the change is meant to remove.
Re-export it from a craft running 0.85.7 or later before treating it as the
plugin's cold-boot state again.

One consequence worth knowing: an export is taken from a *live* ISP, so it
captures wherever the 3A loops had converged at that moment. Two exports of
the same craft are not byte-identical, and neither is an export taken right
after importing a file identical to that file (measured: 3.69% of the ISP
image differs). What round-trips exactly is the ISP's own read-back through
`/api/v1/iq`, which is the thing that matters.

## Install

The fleet convention is `/etc/sensors/`, same as the SigmaStar backends:

```sh
ssh root@<craft> mkdir -p /etc/sensors
scp iq-profiles/cv610-bin/imx662.bin root@<craft>:/etc/sensors/
```

`/etc/sensors` does not exist on a stock CV610 rootfs — create it. Then point
the config at it and save, so the cold-boot apply picks it up:

```sh
curl "http://<craft>/api/v1/set?isp.sensorBin=/etc/sensors/imx662.bin"
curl "http://<craft>/api/v1/save"
```

**Do not leave a `.bin` in `/tmp`.** `/tmp` is tmpfs on this board, so a path
under it survives exactly until the next reboot, after which every boot logs
`PQ bin is not a regular file` and the craft comes up on the plugin defaults.
The overlay had ~2 MB free on the reference bench; a 145 KB bin fits.

`libbin.so` must also be present (`/usr/lib/libbin.so`) or the import warns and
no-ops. It is a third-party blob whose licensing is the vendor's, so it is not
committed to this repo. `make stage SOC_BUILD=cv610` copies it into
`out/cv610/lib/` when `CV610_PQ_LIB` resolves, and prints a NOTE naming the
path when it does not:

```sh
make stage SOC_BUILD=cv610 CV610_PQ_LIB=/path/to/libbin.so
scp out/cv610/lib/libbin.so root@<craft>:/usr/lib/
```

The firmware build installs it the same way, from the staged output.

## Exporting a new one

The exporter writes a fixed path, because the endpoint is unauthenticated and a
caller-supplied path would be a write-anywhere primitive:

```sh
curl http://<craft>/api/v1/iq/export_bin
# -> {"ok":true,"data":{"path":"/tmp/isp_export.bin"}}

scp root@<craft>:/tmp/isp_export.bin iq-profiles/cv610-bin/<sensor>.bin
( cd iq-profiles/cv610-bin && md5sum *.bin > MD5SUMS )
```

Verify before committing: import it back on the bench and confirm
`/api/v1/iq` reads back what it did before the import.

```sh
scp iq-profiles/cv610-bin/<sensor>.bin root@<craft>:/tmp/rt.bin
curl -s http://<craft>/api/v1/iq > before.json
curl "http://<craft>/api/v1/set?isp.sensorBin=/tmp/rt.bin"
curl -s http://<craft>/api/v1/iq > after.json && diff before.json after.json
```

## The files are opaque

A `.bin` is integrity-checked by the vendor library. Editing one byte of an
otherwise valid file makes the import fail `0xcb000005`, so these cannot be
hand-edited, spliced, or merged — produce them with PQTools or with the
exporter above.

The lock is the **chip register map and the SDK ISP version, not the sensor**:
import validates an address/size walk against what the running chip's ISP
reports, and the format carries no sensor identity. A tune built for a
different sensor on the same SoC and SDK imports cleanly — it simply brings
that sensor's CCM, AWB, black level and noise calibration with it, which on a
mismatched sensor looks like a strong colour cast.

Layout, for anyone diffing them: `[ISP image of exactly
OT_PQ_GetISPDataTotalLen() bytes][OTPQNRX 3DNR section]`, which on this SDK is
143424 + 1350 = 144774. The ISP half is three TLV records (131072 + 8192 +
4096, each behind a 20-byte header) and its own header carries width, height
and an fps float — a quick way to tell what a donor file was made for.
