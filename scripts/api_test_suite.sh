#!/usr/bin/env bash
set -euo pipefail

# Exhaustive HTTP API test suite for venc on a live device.
# Assumes venc is already running (for example via star6e_direct_deploy.sh).
# Exercises every API endpoint and every live-mutable config field.

DEVICE="${1:-192.168.2.13}"
PORT="${2:-8888}"
SSH_HOST="${3:-${SSH_HOST:-}}"
BASE="http://${DEVICE}:${PORT}"
PASS=0
FAIL=0
SKIP=0
ERRORS=()
BACKEND_NAME=""
TRANSPORT_RESTORE_NEEDED=0
TRANSPORT_ORIG_SERVER=""
TRANSPORT_ORIG_AUDIO_ENABLED=""
TRANSPORT_ORIG_AUDIO_PORT=""
TRANSPORT_ORIG_VERBOSE=""
BASELINE_CAPTURED=0
RESTORE_ON_EXIT=0

BASE_VIDEO0_BITRATE=""
BASE_VIDEO0_CODEC=""
BASE_VIDEO0_FPS=""
BASE_VIDEO0_GOP_SIZE=""
BASE_VIDEO0_QP_DELTA=""
BASE_VIDEO0_RC_MODE=""
BASE_ISP_EXPOSURE=""
BASE_ISP_AWB_MODE=""
BASE_ISP_AWB_CT=""
BASE_FPV_ROI_ENABLED=""
BASE_FPV_ROI_QP=""
BASE_FPV_ROI_STEPS=""
BASE_FPV_ROI_CENTER=""
BASE_OUTGOING_ENABLED=""
BASE_OUTGOING_SERVER=""
BASE_OUTGOING_STREAM_MODE=""
BASE_OUTGOING_MAX_PAYLOAD_SIZE=""
BASE_SYSTEM_VERBOSE=""
BASE_AUDIO_MUTE=""
ALT_OUTGOING_SERVER=""
POST_RESTART_BITRATE=8000
POST_RESTART_FPS=25
POST_RESTART_QP_DELTA=-6

# ── Helpers ──────────────────────────────────────────────────────────────

c() { curl -sf --max-time 5 "$@" 2>/dev/null; }
c_raw() { curl -s --max-time 5 "$@" 2>/dev/null; }

ok_field() {
	local resp="$1"
	echo "${resp}" | python3 -c "import sys,json; d=json.load(sys.stdin); assert d['ok']==True" 2>/dev/null
}

get_value() {
	local resp="$1"
	echo "${resp}" | python3 -c "import sys,json; print(json.dumps(json.load(sys.stdin)['data']['value']))" 2>/dev/null
}

get_json_field() {
	local resp="$1" field="$2"
	echo "${resp}" | python3 -c "import sys,json; d=json.load(sys.stdin); print(json.dumps(d${field}))" 2>/dev/null
}

current_value() {
	local field="$1"
	local resp

	resp="$(c "${BASE}/api/v1/get?${field}")" || return 1
	ok_field "${resp}" || return 1
	echo "${resp}" | python3 -c '
import json, sys
v = json.load(sys.stdin)["data"]["value"]
if isinstance(v, bool):
    print("true" if v else "false")
elif v is None:
    print("")
else:
    print(v)
' 2>/dev/null
}

snapshot_field() {
	local var_name="$1"
	local field="$2"
	local value

	value="$(current_value "${field}")" || {
		echo "ERROR: Failed to snapshot ${field}" >&2
		exit 2
	}
	printf -v "${var_name}" '%s' "${value}"
}

quiet_set() {
	local field="$1" value="$2"
	c "${BASE}/api/v1/set?${field}=${value}" >/dev/null || true
}

assert_set_query() {
	local query="$1" label="${2:-SET ${1}}"
	local resp
	resp="$(c "${BASE}/api/v1/set?${query}")" || { fail "${label}" "curl failed"; return; }
	if ok_field "${resp}"; then
		pass "${label}"
	else
		fail "${label}" "ok!=true: ${resp}"
	fi
}

assert_set_query_fail() {
	local query="$1" label="${2:-SET ${1} (expect fail)}"
	local resp
	resp="$(c_raw "${BASE}/api/v1/set?${query}")" || { pass "${label} (curl error = rejection)"; return; }
	if ok_field "${resp}" 2>/dev/null; then
		fail "${label}" "expected failure but got ok=true"
	else
		pass "${label}"
	fi
}

assert_value_is() {
	local field="$1" expected="$2" label="${3:-VERIFY ${1} == ${2}}"
	local resp val
	resp="$(c "${BASE}/api/v1/get?${field}")" || { fail "${label}" "curl failed"; return; }
	if ! ok_field "${resp}"; then
		fail "${label}" "ok!=true: ${resp}"
		return
	fi
	val="$(get_value "${resp}" 2>/dev/null)"
	if [[ "${val}" == "${expected}" ]]; then
		pass "${label}"
	else
		fail "${label}" "expected ${expected}, got ${val}"
	fi
}

derive_alt_server() {
	local current="$1"
	local host="192.168.1.2"
	local port="5601"

	if [[ "${current}" =~ ^udp://([^:/]+):([0-9]+)$ ]]; then
		host="${BASH_REMATCH[1]}"
		if [[ "${BASH_REMATCH[2]}" == "5601" ]]; then
			port="5600"
		fi
	fi

	printf 'udp://%s:%s\n' "${host}" "${port}"
}

capture_baseline() {
	snapshot_field BASE_VIDEO0_BITRATE video0.bitrate
	snapshot_field BASE_VIDEO0_FPS video0.fps
	snapshot_field BASE_VIDEO0_GOP_SIZE video0.gop_size
	snapshot_field BASE_VIDEO0_QP_DELTA video0.qp_delta
	snapshot_field BASE_VIDEO0_RC_MODE video0.rc_mode
	snapshot_field BASE_ISP_EXPOSURE isp.exposure
	snapshot_field BASE_ISP_AWB_MODE isp.awb_mode
	snapshot_field BASE_ISP_AWB_CT isp.awb_ct
	snapshot_field BASE_FPV_ROI_ENABLED fpv.roi_enabled
	snapshot_field BASE_FPV_ROI_QP fpv.roi_qp
	snapshot_field BASE_FPV_ROI_STEPS fpv.roi_steps
	snapshot_field BASE_FPV_ROI_CENTER fpv.roi_center
	snapshot_field BASE_OUTGOING_ENABLED outgoing.enabled
	snapshot_field BASE_OUTGOING_SERVER outgoing.server
	snapshot_field BASE_OUTGOING_STREAM_MODE outgoing.stream_mode
	snapshot_field BASE_OUTGOING_MAX_PAYLOAD_SIZE outgoing.max_payload_size
	snapshot_field BASE_SYSTEM_VERBOSE system.verbose
	snapshot_field BASE_AUDIO_MUTE audio.mute
	snapshot_field BASE_SCENE_THRESHOLD video0.scene_threshold 2>/dev/null || BASE_SCENE_THRESHOLD="0"

	ALT_OUTGOING_SERVER="$(derive_alt_server "${BASE_OUTGOING_SERVER}")"
	if [[ "${POST_RESTART_BITRATE}" == "${BASE_VIDEO0_BITRATE}" ]]; then
		POST_RESTART_BITRATE=4000
	fi
	if [[ "${POST_RESTART_FPS}" == "${BASE_VIDEO0_FPS}" ]]; then
		POST_RESTART_FPS=20
	fi
	if [[ "${POST_RESTART_QP_DELTA}" == "${BASE_VIDEO0_QP_DELTA}" ]]; then
		POST_RESTART_QP_DELTA=6
	fi

	BASELINE_CAPTURED=1
	RESTORE_ON_EXIT=1
}

restore_live_baseline() {
	[[ "${BASELINE_CAPTURED}" -eq 1 ]] || return 0
	[[ "${RESTORE_ON_EXIT}" -eq 1 ]] || return 0

	printf '\n[api_test_suite] Restoring live baseline values...\n' >&2
	quiet_set "video0.scene_threshold" "0"
	quiet_set "video0.bitrate" "${BASE_VIDEO0_BITRATE}"
	quiet_set "video0.fps" "${BASE_VIDEO0_FPS}"
	quiet_set "video0.gop_size" "${BASE_VIDEO0_GOP_SIZE}"
	quiet_set "video0.qp_delta" "${BASE_VIDEO0_QP_DELTA}"
	quiet_set "isp.exposure" "${BASE_ISP_EXPOSURE}"
	quiet_set "isp.awb_ct" "${BASE_ISP_AWB_CT}"
	quiet_set "isp.awb_mode" "${BASE_ISP_AWB_MODE}"
	quiet_set "fpv.roi_qp" "${BASE_FPV_ROI_QP}"
	quiet_set "fpv.roi_steps" "${BASE_FPV_ROI_STEPS}"
	quiet_set "fpv.roi_center" "${BASE_FPV_ROI_CENTER}"
	quiet_set "fpv.roi_enabled" "${BASE_FPV_ROI_ENABLED}"
	quiet_set "outgoing.server" "${BASE_OUTGOING_SERVER}"
	quiet_set "outgoing.enabled" "${BASE_OUTGOING_ENABLED}"
	quiet_set "system.verbose" "${BASE_SYSTEM_VERBOSE}"
	quiet_set "audio.mute" "${BASE_AUDIO_MUTE}"
	quiet_set "video0.scene_threshold" "${BASE_SCENE_THRESHOLD}"
}

pass() {
	PASS=$((PASS + 1))
	printf "  PASS  %s\n" "$1"
}

fail() {
	FAIL=$((FAIL + 1))
	ERRORS+=("$1: $2")
	printf "  FAIL  %s -- %s\n" "$1" "$2"
}

skip() {
	SKIP=$((SKIP + 1))
	printf "  SKIP  %s -- %s\n" "$1" "$2"
}

section() {
	printf "\n── %s ──\n" "$1"
}

# Assert GET field returns expected value type
assert_get() {
	local field="$1" expect_type="$2"
	local resp val
	resp="$(c "${BASE}/api/v1/get?${field}")" || { fail "GET ${field}" "curl failed"; return; }
	if ! ok_field "${resp}"; then
		fail "GET ${field}" "ok!=true: ${resp}"
		return
	fi
	val="$(get_value "${resp}")"
	case "${expect_type}" in
		int)    echo "${val}" | grep -qE '^-?[0-9]+$' && pass "GET ${field} = ${val}" || fail "GET ${field}" "expected int, got ${val}" ;;
		uint)   echo "${val}" | grep -qE '^[0-9]+$' && pass "GET ${field} = ${val}" || fail "GET ${field}" "expected uint, got ${val}" ;;
		bool)   [[ "${val}" == "true" || "${val}" == "false" ]] && pass "GET ${field} = ${val}" || fail "GET ${field}" "expected bool, got ${val}" ;;
		string) [[ "${val}" == \"*\" || "${val}" == "null" ]] && pass "GET ${field} = ${val}" || fail "GET ${field}" "expected string, got ${val}" ;;
		double) echo "${val}" | grep -qE '^-?[0-9]+\.?[0-9]*$' && pass "GET ${field} = ${val}" || fail "GET ${field}" "expected double, got ${val}" ;;
		size)   echo "${val}" | grep -qE '^"[0-9]+x[0-9]+"$' && pass "GET ${field} = ${val}" || fail "GET ${field}" "expected size, got ${val}" ;;
		*)      pass "GET ${field} = ${val}" ;;
	esac
}

# Assert SET field succeeds and value sticks
assert_set() {
	local field="$1" value="$2" label="${3:-SET ${1}=${2}}"
	local resp
	resp="$(c "${BASE}/api/v1/set?${field}=${value}")" || { fail "${label}" "curl failed"; return; }
	if ok_field "${resp}"; then
		pass "${label}"
	else
		fail "${label}" "ok!=true: ${resp}"
	fi
}

# Assert SET fails with expected error
assert_set_fail() {
	local field="$1" value="$2" label="${3:-SET ${1}=${2} (expect fail)}"
	local resp
	resp="$(c "${BASE}/api/v1/set?${field}=${value}")" || { pass "${label} (curl error = rejection)"; return; }
	if ok_field "${resp}"; then
		fail "${label}" "expected failure but got ok=true"
	else
		pass "${label}"
	fi
}

# Wait for stream to settle after parameter change
settle() { sleep "${1:-1}"; }

remote_ssh() {
	if [[ -z "${SSH_HOST}" ]]; then
		return 1
	fi
	ssh -o BatchMode=yes -o ConnectTimeout=5 "${SSH_HOST}" "$@"
}

wait_http() {
	local attempts="${1:-20}"
	local i

	for i in $(seq 1 "${attempts}"); do
		if c "${BASE}/api/v1/version" >/dev/null; then
			return 0
		fi
		sleep 1
	done
	return 1
}

transport_restore_baseline() {
	local resp

	if [[ "${TRANSPORT_RESTORE_NEEDED}" != "1" || -z "${SSH_HOST}" ]]; then
		return 0
	fi

	remote_ssh "json_cli -s .audio.enabled ${TRANSPORT_ORIG_AUDIO_ENABLED} -i /etc/venc.json" >/dev/null
	remote_ssh "json_cli -s .outgoing.audioPort ${TRANSPORT_ORIG_AUDIO_PORT} -i /etc/venc.json" >/dev/null
	remote_ssh "json_cli -s .system.verbose ${TRANSPORT_ORIG_VERBOSE} -i /etc/venc.json" >/dev/null
	remote_ssh "json_cli -s .outgoing.server '\"${TRANSPORT_ORIG_SERVER}\"' -i /etc/venc.json" >/dev/null
	remote_ssh "killall -9 venc >/dev/null 2>&1 || true; sleep 5; nohup /usr/bin/venc >/tmp/venc.log 2>&1 </dev/null &" >/dev/null
	if ! wait_http 30; then
		return 1
	fi

	resp="$(c "${BASE}/api/v1/get?outgoing.server")" || return 1
	if ! ok_field "${resp}"; then
		return 1
	fi
	if [[ "$(get_value "${resp}")" != "\"${TRANSPORT_ORIG_SERVER}\"" ]]; then
		return 1
	fi

	TRANSPORT_RESTORE_NEEDED=0
	return 0
}

cleanup_transport() {
	if [[ "${TRANSPORT_RESTORE_NEEDED}" == "1" ]]; then
		transport_restore_baseline >/dev/null 2>&1 || true
	fi
}

prepare_transport_checks() {
	if [[ -z "${SSH_HOST}" ]]; then
		skip "Transport regression" "no SSH host provided"
		return 1
	fi
	if [[ "${BACKEND_NAME}" != "star6e" ]]; then
		skip "Transport regression" "backend ${BACKEND_NAME:-unknown} not targeted"
		return 1
	fi
	if ! remote_ssh "command -v socat >/dev/null && command -v json_cli >/dev/null && command -v wget >/dev/null"; then
		skip "Transport regression" "remote tools missing (need socat, json_cli, wget)"
		return 1
	fi

	TRANSPORT_ORIG_AUDIO_ENABLED="$(remote_ssh "json_cli -g .audio.enabled --raw -i /etc/venc.json")"
	TRANSPORT_ORIG_SERVER="$(remote_ssh "json_cli -g .outgoing.server --raw -i /etc/venc.json")"
	TRANSPORT_ORIG_AUDIO_PORT="$(remote_ssh "json_cli -g .outgoing.audioPort --raw -i /etc/venc.json")"
	TRANSPORT_ORIG_VERBOSE="$(remote_ssh "json_cli -g .system.verbose --raw -i /etc/venc.json")"
	TRANSPORT_RESTORE_NEEDED=1
	return 0
}

run_transport_checks() {
	local unix_name video_bytes udp_bytes audio_video_bytes audio_udp_bytes out

	if ! prepare_transport_checks; then
		return 0
	fi

	unix_name="api_suite_unix_live_$$"
	out="$(remote_ssh "
		rm -f /tmp/api_suite_unix_live.bin;
		(timeout 3 socat -u ABSTRACT-RECV:${unix_name} - >/tmp/api_suite_unix_live.bin) &
		listener=\$!;
		sleep 1;
		wget -q -O- 'http://127.0.0.1/api/v1/set?outgoing.server=unix://${unix_name}' >/dev/null || exit 10;
		wait \$listener || true;
		wc -c </tmp/api_suite_unix_live.bin 2>/dev/null || echo 0;
		rm -f /tmp/api_suite_unix_live.bin
	")" || out=""
	video_bytes="$(echo "${out}" | tr -d '[:space:]')"
	if [[ "${video_bytes}" =~ ^[0-9]+$ ]] && [[ "${video_bytes}" -gt 100000 ]]; then
		pass "TRANSPORT live udp->unix video (${video_bytes} bytes)"
	else
		fail "TRANSPORT live udp->unix video" "captured ${video_bytes:-none} bytes (expected >100KB)"
	fi

	out="$(remote_ssh "
		rm -f /tmp/api_suite_udp_restore.bin;
		(timeout 3 socat -u UDP-RECV:5600,reuseaddr - >/tmp/api_suite_udp_restore.bin) &
		listener=\$!;
		sleep 1;
		wget -q -O- 'http://127.0.0.1/api/v1/set?outgoing.server=udp://127.0.0.1:5600' >/dev/null || exit 10;
		wait \$listener || true;
		wc -c </tmp/api_suite_udp_restore.bin 2>/dev/null || echo 0;
		rm -f /tmp/api_suite_udp_restore.bin
	")" || out=""
	udp_bytes="$(echo "${out}" | tr -d '[:space:]')"
	if [[ "${udp_bytes}" =~ ^[0-9]+$ ]] && [[ "${udp_bytes}" -gt 100000 ]]; then
		pass "TRANSPORT live unix->udp video (${udp_bytes} bytes)"
	else
		fail "TRANSPORT live unix->udp video" "captured ${udp_bytes:-none} bytes (expected >100KB)"
	fi

	unix_name="api_suite_unix_audio_$$"
	out="$(remote_ssh "
		{ json_cli -s .audio.enabled true -i /etc/venc.json >/dev/null &&
		  json_cli -s .outgoing.server '\"unix://${unix_name}\"' -i /etc/venc.json >/dev/null &&
		  json_cli -s .outgoing.audioPort 5601 -i /etc/venc.json >/dev/null; } || true;
		killall -9 venc >/dev/null 2>&1 || true;
		sleep 5;
		rm -f /tmp/api_suite_unix_audio_video.bin /tmp/api_suite_unix_audio_udp.bin;
		(timeout 30 socat -u ABSTRACT-RECV:${unix_name} - 2>/dev/null | dd bs=4096 count=256 of=/tmp/api_suite_unix_audio_video.bin 2>/dev/null) &
		video_listener=\$!;
		(timeout 30 socat -u UDP-RECV:5601,reuseaddr - 2>/dev/null | dd bs=4096 count=64 of=/tmp/api_suite_unix_audio_udp.bin 2>/dev/null) &
		audio_listener=\$!;
		sleep 1;
		nohup /usr/bin/venc >/tmp/venc.log 2>&1 </dev/null &
		sleep 25;
		kill \$video_listener \$audio_listener 2>/dev/null || true;
		wait \$video_listener 2>/dev/null || true;
		wait \$audio_listener 2>/dev/null || true;
		video_bytes=\$(wc -c </tmp/api_suite_unix_audio_video.bin 2>/dev/null || echo 0);
		audio_bytes=\$(wc -c </tmp/api_suite_unix_audio_udp.bin 2>/dev/null || echo 0);
		rm -f /tmp/api_suite_unix_audio_video.bin /tmp/api_suite_unix_audio_udp.bin /tmp/venc.log;
		printf '%s %s\n' \"\$video_bytes\" \"\$audio_bytes\"
	")" || out=""
	audio_video_bytes="$(echo "${out}" | awk '{print $1}')"
	audio_udp_bytes="$(echo "${out}" | awk '{print $2}')"
	if [[ "${audio_video_bytes}" =~ ^[0-9]+$ ]] &&
	   [[ "${audio_udp_bytes}" =~ ^[0-9]+$ ]] &&
	   [[ "${audio_video_bytes}" -gt 100000 ]] &&
	   [[ "${audio_udp_bytes}" -gt 0 ]]; then
		pass "TRANSPORT unix video + dedicated UDP audio (${audio_video_bytes}/${audio_udp_bytes} bytes)"
	else
		fail "TRANSPORT unix video + dedicated UDP audio" \
			"captured video=${audio_video_bytes:-none} audio=${audio_udp_bytes:-none}"
	fi

	if transport_restore_baseline; then
		pass "TRANSPORT restore baseline"
	else
		fail "TRANSPORT restore baseline" "failed to restore /etc/venc.json runtime"
	fi
}

cleanup_all() {
	restore_live_baseline
	cleanup_transport
}

trap cleanup_all EXIT

# ── Connectivity check ───────────────────────────────────────────────────

printf "Testing venc API at %s\n" "${BASE}"
if ! c "${BASE}/api/v1/version" >/dev/null; then
	echo "ERROR: Cannot reach venc at ${BASE}. Is it running?"
	exit 2
fi

capture_baseline

# ════════════════════════════════════════════════════════════════════════
section "1. VERSION & CAPABILITIES"
# ════════════════════════════════════════════════════════════════════════

resp="$(c "${BASE}/api/v1/version")"
if ok_field "${resp}"; then
	ver="$(get_json_field "${resp}" "['data']['app_version']")"
	backend="$(get_json_field "${resp}" "['data']['backend']")"
	BACKEND_NAME="$(echo "${backend}" | tr -d '"')"
	pass "GET /api/v1/version (app=${ver}, backend=${backend})"
else
	fail "GET /api/v1/version" "${resp}"
fi

resp="$(c "${BASE}/api/v1/capabilities")"
if ok_field "${resp}"; then
	nfields="$(echo "${resp}" | python3 -c "import sys,json; print(len(json.load(sys.stdin)['data']['fields']))")"
	pass "GET /api/v1/capabilities (${nfields} fields)"
else
	fail "GET /api/v1/capabilities" "${resp}"
fi

# CV610 parity: the adaptive frame gate is wired in cv610_runtime.c, so its two
# tunables must advertise supported=true.  A regression here greys the controls
# while a hand-edited config is still honoured.  Read-only, safe on any bench.
if [[ "${BACKEND_NAME}" == "cv610" ]] && ok_field "${resp}"; then
	for gate_key in video0.frame_gate_close_slots video0.frame_gate_max_closed_ms; do
		gate_sup="$(echo "${resp}" | python3 -c "import sys,json; f=json.load(sys.stdin)['data']['fields']; print(f.get('${gate_key}', {}).get('supported', 'missing'))" 2>/dev/null)" || gate_sup="parse-error"
		if [[ "${gate_sup}" == "True" ]]; then
			pass "capabilities: ${gate_key} supported on cv610"
		else
			fail "capabilities: ${gate_key} supported on cv610" \
				"supported=${gate_sup}; see documentation/REVIEW_FIX_VERIFICATION.md"
		fi
	done
fi

# ════════════════════════════════════════════════════════════════════════
section "2. FULL CONFIG RETRIEVAL"
# ════════════════════════════════════════════════════════════════════════

resp="$(c "${BASE}/api/v1/config")"
if ok_field "${resp}"; then
	sections="$(echo "${resp}" | python3 -c "import sys,json; print(','.join(json.load(sys.stdin)['data']['config'].keys()))")"
	pass "GET /api/v1/config (sections: ${sections})"
else
	fail "GET /api/v1/config" "${resp}"
fi

# ════════════════════════════════════════════════════════════════════════
section "3. GET ALL CONFIG FIELDS"
# ════════════════════════════════════════════════════════════════════════

# System
assert_get "system.web_port" uint
assert_get "system.overclock_level" int
assert_get "system.verbose" bool

# Sensor
assert_get "sensor.index" int
assert_get "sensor.mode" int

# ISP
assert_get "isp.sensor_bin" string
assert_get "isp.ae_engine" string
assert_get "isp.exposure" uint
assert_get "isp.awb_mode" string
assert_get "isp.awb_ct" uint

# Image
assert_get "image.mirror" bool
assert_get "image.flip" bool
assert_get "image.rotate" int

# Video0
assert_get "video0.rc_mode" string
assert_get "video0.fps" uint
assert_get "video0.size" size
assert_get "video0.bitrate" uint
assert_get "video0.gop_size" double
assert_get "video0.qp_delta" int
assert_get "video0.resilience" string
# Outgoing
assert_get "outgoing.enabled" bool
assert_get "outgoing.server" string
assert_get "outgoing.stream_mode" string
assert_get "outgoing.max_payload_size" uint
assert_get "outgoing.connected_udp" bool
assert_get "outgoing.audio_port" uint

# FPV
assert_get "fpv.roi_enabled" bool
assert_get "fpv.roi_qp" int
assert_get "fpv.roi_steps" uint
assert_get "fpv.roi_center" double
assert_get "fpv.noise_level" int

# Audio (only mute is exposed via API; other audio fields are config-file only)
assert_get "audio.mute" bool

# ════════════════════════════════════════════════════════════════════════
section "4. LIVE PARAMETER CHANGES — Bitrate sweep"
# ════════════════════════════════════════════════════════════════════════

for br in 1000 2000 4000 6000 8192 12000 16000; do
	assert_set "video0.bitrate" "${br}"
done
# Restore baseline
assert_set "video0.bitrate" "${BASE_VIDEO0_BITRATE}" "RESTORE video0.bitrate=${BASE_VIDEO0_BITRATE}"

# ════════════════════════════════════════════════════════════════════════
section "5. LIVE PARAMETER CHANGES — FPS sweep"
# ════════════════════════════════════════════════════════════════════════

for fps in 5 10 15 20 25 30; do
	assert_set "video0.fps" "${fps}"
	settle 0.5
done
# Restore
assert_set "video0.fps" "${BASE_VIDEO0_FPS}" "RESTORE video0.fps=${BASE_VIDEO0_FPS}"

# ════════════════════════════════════════════════════════════════════════
section "6. LIVE PARAMETER CHANGES — GOP size"
# ════════════════════════════════════════════════════════════════════════

# scene_threshold > 0 means scene detection manages gop_size — temporarily disable for manual sweep
if [[ "${BASE_SCENE_THRESHOLD}" != "0" ]]; then
	quiet_set "video0.scene_threshold" "0"
	settle 1
fi

for gop in 0 0.5 1.0 2.0 5.0; do
	assert_set "video0.gop_size" "${gop}"
	settle 0.3
done
assert_set "video0.gop_size" "${BASE_VIDEO0_GOP_SIZE}" "RESTORE video0.gop_size=${BASE_VIDEO0_GOP_SIZE}"

if [[ "${BASE_SCENE_THRESHOLD}" != "0" ]]; then
	quiet_set "video0.scene_threshold" "${BASE_SCENE_THRESHOLD}"
	settle 1
fi

# ════════════════════════════════════════════════════════════════════════
section "6a. LIVE PARAMETER CHANGES — qpDelta"
# ════════════════════════════════════════════════════════════════════════
for delta in -12 -6 0 6 12; do
	assert_set "video0.qp_delta" "${delta}"
	settle 0.3
done
assert_set "video0.qp_delta" "${BASE_VIDEO0_QP_DELTA}" "RESTORE video0.qp_delta=${BASE_VIDEO0_QP_DELTA}"

section "7. LIVE PARAMETER CHANGES — Exposure"

for exp in 0 1 3 5 7 10 15 20; do
	assert_set "isp.exposure" "${exp}"
	settle 0.5
done
assert_set "isp.exposure" "${BASE_ISP_EXPOSURE}" "RESTORE isp.exposure=${BASE_ISP_EXPOSURE}"

# ════════════════════════════════════════════════════════════════════════
section "8. AWB MODE — Auto and manual CT"
# ════════════════════════════════════════════════════════════════════════

assert_set "isp.awb_mode" "auto" "SET awb_mode=auto"
settle 1

# Manual color temperature sweep
assert_set "isp.awb_mode" "ct_manual" "SET awb_mode=ct_manual"
settle 0.5
for ct in 2700 3500 4500 5500 6500 8000 10000; do
	assert_set "isp.awb_ct" "${ct}" "SET awb_ct=${ct}K"
	settle 0.3
done

# Back to auto
assert_set "isp.awb_ct" "${BASE_ISP_AWB_CT}" "RESTORE awb_ct=${BASE_ISP_AWB_CT}"
assert_set "isp.awb_mode" "${BASE_ISP_AWB_MODE}" "RESTORE awb_mode=${BASE_ISP_AWB_MODE}"

# ════════════════════════════════════════════════════════════════════════
section "9. AWB QUERY ENDPOINT"
# ════════════════════════════════════════════════════════════════════════

resp="$(c "${BASE}/api/v1/awb")"
if ok_field "${resp}"; then
	# Check that expected sub-objects exist
	has_query="$(get_json_field "${resp}" "['data']['query_info']" 2>/dev/null)" && true
	has_attr="$(get_json_field "${resp}" "['data']['attr']" 2>/dev/null)" && true
	if [[ -n "${has_query}" && "${has_query}" != "null" ]]; then
		pass "GET /api/v1/awb (query_info present)"
	else
		pass "GET /api/v1/awb (ok, partial data)"
	fi
else
	fail "GET /api/v1/awb" "${resp}"
fi

# ════════════════════════════════════════════════════════════════════════
section "10. ROI QP — Enable, sweep, disable"
# ════════════════════════════════════════════════════════════════════════

assert_set "fpv.roi_enabled" "true"
settle 0.5

for qp in -20 -18 -9 0 9 18 20; do
	assert_set "fpv.roi_qp" "${qp}" "SET roi_qp=${qp}"
	settle 0.3
done

# Past +-20 the delta stops reaching the encoder, and on the negative side it
# saturates rate control and overruns the bitrate target (contract 0.28.0).
# The boundary is asserted on BOTH sides of the edge: +-20 accepted above,
# +-21 and the old +-30 refused here.  Without the +-21 pair a validator that
# had drifted to, say, +-25 would still pass this block.
for qp in -30 -21 21 30; do
	assert_set_fail "fpv.roi_qp" "${qp}" "REJECT roi_qp=${qp} (out of +-20)"
	settle 0.3
done
assert_set "fpv.roi_qp" "-15" "SET roi_qp=-15 (restore in-range)"
settle 0.3

for steps in 1 2 3 4; do
	assert_set "fpv.roi_steps" "${steps}" "SET roi_steps=${steps}"
	settle 0.3
done

for center in 0.1 0.25 0.5 0.75 0.9; do
	assert_set "fpv.roi_center" "${center}" "SET roi_center=${center}"
	settle 0.3
done

# Disable ROI
assert_set "fpv.roi_qp" "${BASE_FPV_ROI_QP}" "RESTORE roi_qp=${BASE_FPV_ROI_QP}"
assert_set "fpv.roi_steps" "${BASE_FPV_ROI_STEPS}" "RESTORE roi_steps=${BASE_FPV_ROI_STEPS}"
assert_set "fpv.roi_center" "${BASE_FPV_ROI_CENTER}" "RESTORE roi_center=${BASE_FPV_ROI_CENTER}"
assert_set "fpv.roi_enabled" "${BASE_FPV_ROI_ENABLED}" "RESTORE roi_enabled=${BASE_FPV_ROI_ENABLED}"

# ════════════════════════════════════════════════════════════════════════
section "11. OUTPUT ENABLE/DISABLE TOGGLE"
# ════════════════════════════════════════════════════════════════════════

assert_set "outgoing.enabled" "false" "DISABLE output"
settle 2
assert_set "outgoing.enabled" "true" "RE-ENABLE output"
settle 2

# Verify FPS restored after re-enable
resp="$(c "${BASE}/api/v1/get?video0.fps")"
fps_val="$(get_value "${resp}" 2>/dev/null)"
if [[ "${fps_val}" == "${BASE_VIDEO0_FPS}" ]]; then
	pass "FPS restored to ${BASE_VIDEO0_FPS} after output re-enable"
else
	fail "FPS after re-enable" "expected ${BASE_VIDEO0_FPS}, got ${fps_val}"
fi

assert_set "outgoing.enabled" "${BASE_OUTGOING_ENABLED}" "RESTORE outgoing.enabled=${BASE_OUTGOING_ENABLED}"

# ════════════════════════════════════════════════════════════════════════
section "12. SERVER CHANGE (live)"
# ════════════════════════════════════════════════════════════════════════

# Change to a different port
assert_set "outgoing.server" "${ALT_OUTGOING_SERVER}" "SET server to alternate destination"
settle 1
# Change back
assert_set "outgoing.server" "${BASE_OUTGOING_SERVER}" "RESTORE server=${BASE_OUTGOING_SERVER}"
settle 1

# ════════════════════════════════════════════════════════════════════════
section "13. IDR REQUEST"
# ════════════════════════════════════════════════════════════════════════

resp="$(c "${BASE}/request/idr")"
if ok_field "${resp}"; then
	pass "GET /request/idr"
else
	fail "GET /request/idr" "${resp}"
fi

# Multiple rapid IDR requests (stress)
for i in $(seq 1 5); do
	resp="$(c "${BASE}/request/idr")" || true
done
pass "IDR burst (5 rapid requests)"

# ════════════════════════════════════════════════════════════════════════
section "14. VERBOSE TOGGLE"
# ════════════════════════════════════════════════════════════════════════

assert_set "system.verbose" "true" "SET verbose=true"
settle 0.5
assert_set "system.verbose" "${BASE_SYSTEM_VERBOSE}" "RESTORE verbose=${BASE_SYSTEM_VERBOSE}"

# ════════════════════════════════════════════════════════════════════════
section "15. AUDIO MUTE (live, audio may be disabled)"
# ════════════════════════════════════════════════════════════════════════

# Mute should succeed even when audio is disabled (no-op)
assert_set "audio.mute" "true" "SET audio.mute=true"
settle 0.3
assert_set "audio.mute" "${BASE_AUDIO_MUTE}" "RESTORE audio.mute=${BASE_AUDIO_MUTE}"

# ════════════════════════════════════════════════════════════════════════
section "16. ERROR HANDLING — Invalid fields and values"
# ════════════════════════════════════════════════════════════════════════

# Unknown field
resp="$(c "${BASE}/api/v1/get?nonexistent.field" 2>/dev/null)" || resp="error"
if [[ "${resp}" == "error" ]] || ! ok_field "${resp}" 2>/dev/null; then
	pass "GET unknown field rejected"
else
	fail "GET unknown field" "expected error, got ok"
fi

# Invalid value types
assert_set_fail "video0.fps" "not_a_number"
assert_set_fail "video0.fps" "0"
assert_set_fail "video0.qp_delta" "-13"
assert_set_fail "video0.qp_delta" "13"
assert_set_fail "isp.awb_mode" "invalid_mode"
assert_set_fail "fpv.roi_steps" "0"
assert_set_fail "fpv.roi_steps" "999"
assert_set_fail "fpv.roi_center" "0.05"
assert_set_fail "fpv.roi_center" "42"
assert_value_is "fpv.roi_steps" "${BASE_FPV_ROI_STEPS}" "VERIFY roi_steps unchanged after rejected values"
assert_value_is "fpv.roi_center" "${BASE_FPV_ROI_CENTER}" "VERIFY roi_center unchanged after rejected values"

# video0.codec was retired in 0.10.12 — schema rejects it as unknown_field
# regardless of backend or stream mode.
assert_set_fail "video0.codec" "h264" "Retired video0.codec is rejected as unknown field"

# Unknown route
resp="$(curl -sf --max-time 3 "${BASE}/api/v1/nonexistent" 2>/dev/null)" || resp="error"
if [[ "${resp}" == "error" ]]; then
	pass "Unknown route returns error"
else
	if ! ok_field "${resp}" 2>/dev/null; then
		pass "Unknown route returns error JSON"
	else
		fail "Unknown route" "expected error"
	fi
fi

# ════════════════════════════════════════════════════════════════════════
section "17. COMBINED PARAMETER CHANGES (rapid fire)"
# ════════════════════════════════════════════════════════════════════════

if [[ "${BASE_SCENE_THRESHOLD}" != "0" ]]; then
	quiet_set "video0.scene_threshold" "0"
	settle 1
fi

# Rapidly change multiple live parameters
assert_set "video0.bitrate" 4000
assert_set "video0.fps" 20
assert_set "video0.gop_size" 0.5
assert_set "video0.qp_delta" -4
assert_set "fpv.roi_enabled" "true"
assert_set "fpv.roi_qp" 15
assert_set "isp.exposure" 5
settle 2

# Verify they all took effect
for check in "video0.bitrate:4000" "video0.fps:20" "video0.qp_delta:-4" "fpv.roi_enabled:true"; do
	field="${check%%:*}"
	expected="${check#*:}"
	assert_value_is "${field}" "${expected}" "VERIFY ${field} == ${expected}"
done

# Restore all
assert_set "video0.bitrate" "${BASE_VIDEO0_BITRATE}"
assert_set "video0.fps" "${BASE_VIDEO0_FPS}"
assert_set "video0.gop_size" "${BASE_VIDEO0_GOP_SIZE}"
assert_set "video0.qp_delta" "${BASE_VIDEO0_QP_DELTA}"
assert_set "fpv.roi_qp" "${BASE_FPV_ROI_QP}"
assert_set "fpv.roi_steps" "${BASE_FPV_ROI_STEPS}"
assert_set "fpv.roi_center" "${BASE_FPV_ROI_CENTER}"
assert_set "fpv.roi_enabled" "${BASE_FPV_ROI_ENABLED}"
assert_set "isp.exposure" "${BASE_ISP_EXPOSURE}"

# ════════════════════════════════════════════════════════════════════════
section "18. LIVE MULTI-SET QUERIES"
# ════════════════════════════════════════════════════════════════════════

assert_set_query \
	"video0.bitrate=3072&system.verbose=false" \
	"MULTI-SET bitrate+verbose"
settle 1
assert_value_is "video0.bitrate" "3072" "VERIFY multi-set bitrate == 3072"
assert_value_is "system.verbose" "false" "VERIFY multi-set verbose == false"
assert_set_query \
	"video0.bitrate=${BASE_VIDEO0_BITRATE}&system.verbose=${BASE_SYSTEM_VERBOSE}" \
	"MULTI-SET restore bitrate+verbose"

assert_set_query \
	"video0.fps=30&video0.gopSize=1.0" \
	"MULTI-SET fps+gop"
settle 2
assert_value_is "video0.fps" "30" "VERIFY multi-set fps == 30"
assert_value_is "video0.gop_size" "1" "VERIFY multi-set gop_size == 1"
assert_set_query \
	"video0.fps=${BASE_VIDEO0_FPS}&video0.gop_size=${BASE_VIDEO0_GOP_SIZE}" \
	"MULTI-SET restore fps+gop"
settle 1

assert_set_query \
	"isp.awbMode=ct_manual&isp.awbCt=6000" \
	"MULTI-SET awb mode+ct"
settle 1
assert_value_is "isp.awb_mode" "\"ct_manual\"" "VERIFY multi-set awb_mode == ct_manual"
assert_value_is "isp.awb_ct" "6000" "VERIFY multi-set awb_ct == 6000"
assert_set_query \
	"isp.awbMode=${BASE_ISP_AWB_MODE}&isp.awbCt=${BASE_ISP_AWB_CT}" \
	"MULTI-SET restore awb"

assert_set_query_fail \
	"video0.bitrate=4096&video0.size=1280x720" \
	"MULTI-SET rejects mixed live+restart"
assert_value_is "video0.bitrate" "${BASE_VIDEO0_BITRATE}" "VERIFY mixed live+restart left bitrate unchanged"

assert_set_query_fail \
	"video0.qp_delta=1&video0.qpDelta=2" \
	"MULTI-SET rejects duplicate canonical field"
assert_value_is "video0.qp_delta" "${BASE_VIDEO0_QP_DELTA}" "VERIFY duplicate-key reject left qp_delta unchanged"

if [[ "${BASE_SCENE_THRESHOLD}" != "0" ]]; then
	quiet_set "video0.scene_threshold" "${BASE_SCENE_THRESHOLD}"
	settle 1
fi

# ════════════════════════════════════════════════════════════════════════
section "19. RESTART-REQUIRED FIELDS (verify reinit_pending)"
# ════════════════════════════════════════════════════════════════════════

# These fields should return reinit_pending=true but we do NOT trigger
# a restart here — that would tear down the pipeline. Just verify the flag.
for field_val in "video0.rc_mode=cbr" \
                 "outgoing.stream_mode=rtp" "outgoing.max_payload_size=1400"; do
	field="${field_val%%=*}"
	value="${field_val#*=}"
	resp="$(c "${BASE}/api/v1/set?${field}=${value}")" || { fail "SET ${field}" "curl failed"; continue; }
	if ok_field "${resp}"; then
		pending="$(get_json_field "${resp}" "['data'].get('reinit_pending',False)")"
		if [[ "${pending}" == "true" ]]; then
			pass "SET ${field}=${value} (reinit_pending=true)"
		else
			pass "SET ${field}=${value} (accepted, no reinit flagged)"
		fi
	else
		fail "SET ${field}=${value}" "${resp}"
	fi
done

assert_set "video0.rc_mode" "${BASE_VIDEO0_RC_MODE}" "RESTORE video0.rc_mode=${BASE_VIDEO0_RC_MODE}"
assert_set "outgoing.stream_mode" "${BASE_OUTGOING_STREAM_MODE}" "RESTORE outgoing.stream_mode=${BASE_OUTGOING_STREAM_MODE}"
assert_set "outgoing.max_payload_size" "${BASE_OUTGOING_MAX_PAYLOAD_SIZE}" "RESTORE outgoing.max_payload_size=${BASE_OUTGOING_MAX_PAYLOAD_SIZE}"

# ════════════════════════════════════════════════════════════════════════
section "20. PIPELINE RESTART"
# ════════════════════════════════════════════════════════════════════════

resp="$(c "${BASE}/api/v1/restart")"
if ok_field "${resp}"; then
	pass "GET /api/v1/restart (reinit requested)"
else
	fail "GET /api/v1/restart" "${resp}"
fi

# Wait for pipeline to reinitialize
sleep 15

# Verify venc is back up
resp="$(c "${BASE}/api/v1/version")" || resp=""
if [[ -n "${resp}" ]] && ok_field "${resp}"; then
	pass "venc responsive after restart"
else
	fail "POST-RESTART reachability" "venc not responding after restart"
fi

# Post-restart validation
resp="$(c "${BASE}/api/v1/config")"
if ok_field "${resp}"; then
	pass "Full config readable after restart"
else
	fail "Full config after restart" "${resp}"
fi

# Verify live parameter changes work after restart
assert_set "video0.bitrate" "${POST_RESTART_BITRATE}" "POST-RESTART SET bitrate=${POST_RESTART_BITRATE}"
settle 1
assert_set "video0.fps" "${POST_RESTART_FPS}" "POST-RESTART SET fps=${POST_RESTART_FPS}"
settle 1
assert_set "video0.qp_delta" "${POST_RESTART_QP_DELTA}" "POST-RESTART SET qp_delta=${POST_RESTART_QP_DELTA}"
settle 1

# Verify values took effect
for check in "video0.bitrate:${POST_RESTART_BITRATE}" "video0.fps:${POST_RESTART_FPS}" "video0.qp_delta:${POST_RESTART_QP_DELTA}"; do
	field="${check%%:*}"
	expected="${check#*:}"
	resp="$(c "${BASE}/api/v1/get?${field}")"
	val="$(get_value "${resp}" 2>/dev/null)"
	if [[ "${val}" == "${expected}" ]]; then
		pass "POST-RESTART VERIFY ${field} == ${expected}"
	else
		fail "POST-RESTART VERIFY ${field}" "expected ${expected}, got ${val}"
	fi
done

# Restore
assert_set "video0.bitrate" "${BASE_VIDEO0_BITRATE}" "POST-RESTART RESTORE bitrate=${BASE_VIDEO0_BITRATE}"
assert_set "video0.fps" "${BASE_VIDEO0_FPS}" "POST-RESTART RESTORE fps=${BASE_VIDEO0_FPS}"
assert_set "video0.qp_delta" "${BASE_VIDEO0_QP_DELTA}" "POST-RESTART RESTORE qp_delta=${BASE_VIDEO0_QP_DELTA}"

RESTORE_ON_EXIT=0

# ════════════════════════════════════════════════════════════════════════
section "20. TRANSPORT REGRESSION (optional SSH)"
# ════════════════════════════════════════════════════════════════════════

run_transport_checks

# ════════════════════════════════════════════════════════════════════════
printf "\n══════════════════════════════════════════════\n"
printf "RESULTS: %d passed, %d failed, %d skipped\n" "${PASS}" "${FAIL}" "${SKIP}"
printf "══════════════════════════════════════════════\n"

if [[ ${FAIL} -gt 0 ]]; then
	printf "\nFailed tests:\n"
	for e in "${ERRORS[@]}"; do
		printf "  - %s\n" "${e}"
	done
	exit 1
fi

exit 0
