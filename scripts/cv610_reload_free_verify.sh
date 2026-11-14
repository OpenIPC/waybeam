#!/bin/sh
# cv610_reload_free_verify.sh -- run ON the CV610 target.
#
# Verifies that a venc restart no longer reloads the MPP modules, and that the
# cases which make that unsafe are caught rather than tolerated.
#
# The interesting case is a restart across a MODE CHANGE. A same-mode restart
# passes even when the VB config was silently inherited, because the inherited
# layout happens to be the right one -- so a suite that only restarts in place
# proves nothing about the thing being changed. T3 changes video0.size and then
# asserts the ENCODED SIZE moved in venc's own VPSS line, which is what moves
# out_blk, before checking which path sys_setup() took.
#
# Instruments are the markers sys_setup() prints:
#   "ok  ss_mpi_vb_set_cfg"                          -> fresh config applied
#   "ok  ss_mpi_vb_set_cfg  (adopted identical ...)" -> BUSY, layout matched
#   "FAIL VB is held by a dead owner ..."            -> BUSY, layout differed
#
# Two steps can kill the SoC, by design, and need an operator who can power
# cycle:
#   T1 attempts the unload the guard is supposed to refuse. If the guard has
#      regressed, the unload proceeds under the hub and wedges the board --
#      that is exactly the bug, observed as no network and ARP incomplete.
#   T6 hard-kills venc so MPP state is left dirty on purpose. The kill itself
#      is survivable -- venc self-heals -- but it poisons module reloading for
#      the rest of the boot, and an unload issued after this point leaves the
#      craft with no modules (and resets a video-only SoC). So it runs last.
# Everything else is a normal restart.
#
# Usage: cv610_reload_free_verify.sh [--keep-going]
set -u

INIT=/etc/init.d/S95waybeam
HUB=/etc/init.d/S97waybeam-hub
LOADER=/usr/bin/load-cv610-online
CFG=/etc/waybeam.json
LOG=/var/run/waybeam.log
BACKUP=/tmp/waybeam.json.verify-backup
KEEP=0
[ "${1:-}" = "--keep-going" ] && KEEP=1

pass=0; fail=0; note=0
ok()   { pass=$((pass+1)); echo "  PASS  $1"; }
bad()  { fail=$((fail+1)); echo "  FAIL  $1"; [ "$KEEP" = 1 ] || { echo; echo "halting so the state can be inspected"; summary; exit 1; }; }
info() { note=$((note+1)); echo "  NOTE  $1"; }
summary() { echo; echo "=== $pass passed, $fail failed, $note measurement(s) ==="; }
chk()  { desc=$1; shift; if "$@"; then ok "$desc"; else bad "$desc"; fi; }

mods()    { grep -c '^open' /proc/modules; }
venc_pid(){ pidof waybeam 2>/dev/null; }
hub_pid() { pidof waybeam_hub 2>/dev/null; }
pkts()    { wget -qO- --timeout=3 http://127.0.0.1:80/api/v1/transport/status 2>/dev/null \
             | sed 's/.*"packetsSent"://; s/[^0-9].*//'; }

flowing() {
	a=$(pkts); sleep 3; b=$(pkts)
	[ -n "$a" ] && [ -n "$b" ] && [ "$b" -gt "$a" ] 2>/dev/null
}

# The encoded size venc actually chose, straight from its own bring-up line:
#   "ok  VPSS -> 1280x720" (0.86.4 and earlier: "ok  VPSS 1920x1080 -> 1280x720 (scaled)")
# This, not the config, is what decides out_blk and the pool count.
out_size() {
	sed -n 's/.*VPSS .*-> \([0-9]*x[0-9]*\).*/\1/p' "$LOG" | tail -1
}

# video0.size is a STRING ("1920x1080"), and video0.width/height do not exist --
# setting those creates keys nothing reads, which is how the first version of
# this suite reported a mode change that never happened. Assert the geometry,
# not the key.
set_size() {
	command -v json_cli >/dev/null 2>&1 || return 1
	json_cli -s .video0.size "\"$1\"" -i "$CFG" >/dev/null 2>&1 || return 1
	[ "$(json_cli -g .video0.size --raw -i "$CFG" 2>/dev/null)" = "$1" ] || return 1
	return 0
}

# The hub's OSD region, HIRGN_HANDLE in waybeam-hub src/osd_backend_hirgn.c.
# Fixed there so it never collides with venc's own debug OSD on handle 0.
HUB_RGN_HANDLE=8

# The compositor's job counters for that region, from /proc/umap/rgn:
#   hdl  call_cnt  job_suc  job_fail  task_suc  task_fail  end_suc  end_fail
# Read these, not the hub's perf line: the hub counts a publish it made, which
# is the stimulus. These count the composite that consumed it.
rgn_field() {
	awk -v hdl="$HUB_RGN_HANDLE" -v col="$1" '
		/region call vgs/      { in_tbl = 1; next }
		in_tbl && $1 == hdl && NF >= 8 { print $col; exit }
	' /proc/umap/rgn 2>/dev/null
}
rgn_jobs()  { rgn_field 2; }
rgn_fails() { rgn_field 4; }

# The audio encoder's two MMZ blocks: present exactly when AENC is up. Reading
# these rather than the log because the log's own message is misleading (it
# blames the module load for what is actually a dead predecessor's AI claim).
# grep -c prints "0" AND exits 1 when it matches nothing, so "|| echo 0" would
# print 0 twice. Count with awk, which has one exit path and one line of output.
aenc_blocks() {
	awk '/aenc\(0\)/ { n++ } END { print n + 0 }' /proc/umap/media-mem 2>/dev/null ||
		echo 0
}

# Is audio even configured? Without this a video-only craft would "fail" the
# audio assertion for the correct reason of having no audio.
audio_enabled() {
	command -v json_cli >/dev/null 2>&1 || return 1
	[ "$(json_cli -g .audio.enabled --raw -i "$CFG" 2>/dev/null)" = "true" ]
}

not_grep() { ! grep -q "$1" "$2"; }

osd_compositing() {
	# Two samples must advance, with no failed jobs. Retried once because the
	# counters belong to the region, not to the hub: if the hub recreates the
	# region between the samples the count RESTARTS, so b < a is a rebind in
	# progress, not a stall. Without the retry this flakes exactly when the
	# thing it is checking is working.
	i=0
	while [ "$i" -lt 3 ]; do
		i=$((i + 1))
		a=$(rgn_jobs); sleep 3; b=$(rgn_jobs)
		[ -n "$a" ] && [ -n "$b" ] || return 1
		[ "$b" -lt "$a" ] 2>/dev/null && continue
		[ "$b" -gt "$a" ] 2>/dev/null && [ "$(rgn_fails)" = 0 ] && return 0
		return 1
	done
	return 1
}

# The region is created by the hub a few seconds after ITS start, not ours, so
# a baseline sampled too early reads as "this build has no OSD" and silently
# downgrades the T2 assertion to a note. Wait for it when the hub is up.
wait_for_rgn() {
	i=0
	while [ "$i" -lt 12 ]; do
		[ -n "$(rgn_jobs)" ] && return 0
		i=$((i + 1)); sleep 2
	done
	return 1
}

# ---------------------------------------------------------------- preconditions
echo "=== preconditions ==="
cp "$CFG" "$BACKUP" || { echo "cannot back up $CFG"; exit 1; }
echo "  config backed up to $BACKUP"
[ -n "$(venc_pid)" ] || $INIT start >/dev/null 2>&1
[ -n "$(hub_pid)" ]  || $HUB  start >/dev/null 2>&1
sleep 8
base_mods=$(mods)
[ -n "$(hub_pid)" ] && wait_for_rgn
base_rgn=$(rgn_jobs)
audio_cfg=0
audio_enabled && audio_cfg=1
echo "  venc=$(venc_pid) hub=$(hub_pid) modules=$base_mods osd_jobs=${base_rgn:-none} audio=$audio_cfg"
[ -n "$(hub_pid)" ] || { echo "hub is not running -- T1/T2/T3 need it up"; exit 1; }

# ------------------------------------------------------- T1: the guard still fires
echo
echo "=== T1: unload is refused while the hub holds /dev/rgn ==="
out=$("$LOADER" stop 2>&1); rc=$?
case "$out" in
	*"refusing to unload"*) ok "loader refused, naming holders" ;;
	*) bad "loader did NOT refuse (rc=$rc): $out" ;;
esac
chk "refusal is a non-zero exit" test "$rc" -ne 0
chk "module count unchanged ($base_mods)" test "$(mods)" = "$base_mods"

# ------------------------------------ T2: graceful restart, same mode, hub RUNNING
echo
echo "=== T2: venc restart with the hub up (the case that used to wedge) ==="
: >"$LOG"
$INIT restart >/dev/null 2>&1
sleep 8
chk "venc came back (pid $(venc_pid))" test -n "$(venc_pid)"
chk "hub survived the restart" test -n "$(hub_pid)"
chk "modules never unloaded ($base_mods)" test "$(mods)" = "$base_mods"
chk "frames flowing after restart" flowing
chk "sys_setup ran vb_set_cfg" grep -q "ss_mpi_vb_set_cfg" "$LOG"

# The check this suite used to hand to a human. venc's teardown takes the whole
# RGN table with it (mpp_cleanup -> ss_mpi_sys_exit, and MPP objects are kernel
# state shared across processes), so the hub's OSD region does not merely go
# stale -- it disappears, and before waybeam-hub#212 the hub kept painting into
# nothing without saying so.
now_rgn=$(rgn_jobs)
echo "  OSD region $HUB_RGN_HANDLE: was $base_rgn, now ${now_rgn:-GONE}, fails=$(rgn_fails)"
if [ -z "$base_rgn" ]; then
	info "hub had no OSD region before the restart (built without CV610_OSD?) -- OSD survival UNVERIFIED"
else
	chk "hub OSD still compositing after the restart (waybeam-hub#212)" \
		osd_compositing
fi

# ------------------------------------------- T3: restart ACROSS a mode change
echo
echo "=== T3: restart across a mode change, hub up ==="
base_out=$(out_size)
echo "  baseline encoded size: ${base_out:-unknown}"
if [ -z "$base_out" ]; then
	info "no VPSS size line in $LOG -- cannot tell a mode change from a no-op, T3 skipped"
elif ! set_size 640x360; then
	info "could not edit $CFG -- T3 skipped, mode-change path UNVERIFIED"
else
	: >"$LOG"
	$INIT restart >/dev/null 2>&1
	sleep 8
	new_out=$(out_size)
	if [ -z "$new_out" ] || [ "$new_out" = "$base_out" ]; then
		# The whole point of T3. Without this the suite happily "passes" a
		# same-mode restart and leaves the layout-change path unexercised.
		bad "encoded size did not move ($base_out -> ${new_out:-none}) -- NOT a mode change"
	else
		ok "encoded size moved $base_out -> $new_out, so the VB layout differs"
		if grep -q "adopted identical live config" "$LOG"; then
			bad "VB config was ADOPTED across a mode change -- pools are stale"
		elif grep -q "held by a dead owner" "$LOG"; then
			bad "vb_set_cfg refused after a *graceful* stop -- vb_exit did not release"
		elif grep -q "ok  ss_mpi_vb_set_cfg" "$LOG"; then
			ok "fresh VB config applied for the new layout"
		else
			bad "no vb_set_cfg marker at all"
		fi
		chk "venc running in the new mode" test -n "$(venc_pid)"
		chk "frames flowing in the new mode" flowing
		chk "still no module reload" test "$(mods)" = "$base_mods"
		# The nastier OSD case. The hub rebinds its region within ~1 s, but at
		# the OLD window size, and only its own 5 s source poll re-anchors it
		# to the smaller frame -- so for a few seconds an oversized region is
		# attached to this channel. Assert both that venc survived it (above)
		# and that the OSD ends up compositing anyway.
		if [ -n "$base_rgn" ]; then
			chk "hub OSD compositing after a mode-change restart" osd_compositing
		fi
	fi
fi

# ------------------------------------------------ T4: back to the original mode
echo
echo "=== T4: restore 1080p ==="
cp "$BACKUP" "$CFG"
: >"$LOG"
$INIT restart >/dev/null 2>&1
sleep 8
chk "restored mode: venc up" test -n "$(venc_pid)"
chk "restored mode: streaming" flowing

# T5 tested the init script's "reload" action, which no longer exists: it was
# built for a VB-held-by-a-dead-owner case the hardware disproved, nothing called
# it, and it was the only shipped path that unloaded modules on a running craft.
# With it gone, nothing in this suite unloads, so the contaminated-boot check it
# needed is gone too -- a hard kill can no longer poison anything this suite does.
cp "$BACKUP" "$CFG"

# --------------------------------------------------------- T6: the crash path
# LAST on purpose. Measured on .181: after a SIGKILL the next module
# unload/reload fails at sys.ko init -- 'no sys ko!' then 'load vpp.ko
# ...FAILURE!' -- for the rest of the boot, and on a video-only craft it resets
# the SoC outright. Nothing in this suite unloads any more, so that no longer
# breaks a later step; it still runs last so it cannot poison a hand-run
# 'load-cv610-online restart' someone does afterwards.
# Expected result is NOT known in advance: if the driver's .release frees VB
# when the process dies, a hard kill self-heals and set_cfg simply succeeds.
# Report what happens rather than asserting.
echo
echo "=== T6: hard-kill venc (no mpp_cleanup), then start in a DIFFERENT mode ==="
echo "    measurement, not an assertion"
killall -9 waybeam 2>/dev/null
sleep 3
set_size 640x360 || info "config edit failed; T6 degraded to a same-mode start"
: >"$LOG"
$INIT start >/dev/null 2>&1
sleep 8
if grep -q "held by a dead owner" "$LOG"; then
	info "VB survived the kill and the mismatch was REFUSED (reboot needed)"
elif grep -q "adopted identical live config" "$LOG"; then
	bad "stale VB adopted across a mode change after a crash -- silent wrong pools"
elif grep -q "ok  ss_mpi_vb_set_cfg" "$LOG"; then
	info "VB was released when the process died"
else
	info "no VB marker; inspect $LOG"
fi
grep -q "already inited" "$LOG" && bad "ISP survived the kill and blocked the restart"
# The VB marker alone says nothing about whether the daemon came up: the first
# run of this suite recorded a cheerful NOTE while venc was dead on the floor at
# ss_mpi_isp_mem_init. Assert the outcome, not just the milestone.
chk "venc actually running after the crash restart" test -n "$(venc_pid)"
chk "streaming after the crash restart" flowing

# "streaming" above is VIDEO. It passed for months while AUDIO was silently
# dead: a SIGKILL leaves the AI device claimed by the dead pid, every AI call
# from the successor is refused with OT_ERR_AI_NOT_PERM, and venc reports it as
# "needs CV610_AUDIO=1 at module load" -- so neither the log nor this suite said
# anything true. The aenc MMB blocks are the observable: the audio encoder has
# exactly two, and they are absent when audio failed to start.
if [ "$audio_cfg" = 1 ]; then
	echo "  aenc blocks: $(aenc_blocks) (2 = audio up, 0 = audio silently lost)"
	chk "audio came back after the crash restart" test "$(aenc_blocks)" = 2
	chk "no audio-start failure logged" not_grep "audio did not start" "$LOG"
else
	info "audio disabled in $CFG -- crash-restart audio recovery UNVERIFIED"
fi

leaks=$(dmesg | grep -c "MMB LEAK")
info "MMB LEAK lines this boot: $leaks (2 per hard kill; they are the kernel"
info "  RECLAIMING those blocks, not losing them -- measured: the pool returns"
info "  to its exact baseline across repeated kills)"
echo "  NOTE  module reloading is poisoned until reboot -- unloading is NOT the"
echo "        recovery after a hard kill; reboot is. venc itself self-heals above."

echo
# T6 leaves video0.size at the test value. Put the craft back on its configured
# mode and restart into it, so the bench is not silently left encoding 640x360
# under a message claiming it was restored.
cp "$BACKUP" "$CFG"
$INIT restart >/dev/null 2>&1
sleep 8
chk "config restored and venc restarted into it" test -n "$(venc_pid)"
echo "  encoded size now: $(out_size)"
echo "Config restored from $BACKUP."
echo "Still to check by hand: one reboot. (The OSD is asserted in T2 now, via"
echo "the compositor's own job counters in /proc/umap/rgn.)"
summary
[ "$fail" = 0 ]
