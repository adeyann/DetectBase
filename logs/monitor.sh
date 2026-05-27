#!/bin/bash
# logs/monitor.sh — DetectBase 통합 운영 모니터 (canonical, JSONL)
#
# 사용: ./monitor.sh [label]
#   label 기본값: sanity
#
# 환경변수:
#   INTERVAL_SEC=60     sampling 주기
#   HEARTBEAT_EVERY=30  heartbeat 출력 주기 (cycle 단위)
#   LOG_PATH            추적 log 파일 (기본 DetectBase.log)
#   METRICS_URL         Prometheus endpoint
#   CONTAINER           docker container name
#
# 출력:
#   logs/monitor_<label>.jsonl  — 1 줄 = 1 JSON record (nested per-cam)
#   stdout                       — heartbeat + 이상 알림
#
# 1 record schema:
#   ts, up_min, cut
#   pipeline {dfps, cam_active, cam_registered}
#   mem {resident_mb, vm_rss_mb, rss_anon_mb, vm_hwm_mb, jem_mb}
#   res {threads, fd}
#   events {reset, eos, wd, ftc, err, warn}        # start_cut 이후 누적
#   disk {used_pct, emergency, cleanup, skipped}
#   container {cpu_pct, mem_pct}
#   net {listen, estab}
#   thermal {npu_c}
#   cams {<id>: {rsp:{…}, inf:{…}, evt:{…}}}        # per-cam 동적 검출
#
# per-cam field 명명: `prefix avg=X max=Y` → `prefix_avg`/`prefix_max`.
# 단위 (KB/MB) 는 key suffix 로 보존 (`jem_alloc_kb`, `anon_mb`).
#
# 의존: python3, curl, docker, ss, awk

set -u

LABEL="${1:-sanity}"
ROOT=/home/claudedev/DetectBase
LOG_PATH="${LOG_PATH:-$ROOT/logs/DetectBase.log}"
OUT="$ROOT/logs/monitor_${LABEL}.jsonl"
METRICS_URL="${METRICS_URL:-http://localhost:9090/metrics}"
CONTAINER="${CONTAINER:-detectbase_service}"
INTERVAL_SEC="${INTERVAL_SEC:-60}"
HEARTBEAT_EVERY="${HEARTBEAT_EVERY:-30}"

START_TS=$(date -u +%s)
START_LOG_CUT=$(date -u -d "2 minute ago" '+%Y-%m-%dT%H:%M')

# § ALERT thresholds — 운영 사고 시그니처 기반 (5/24 storm / 5/26 PID 4924 사례 분석).
#   기준선: baseline DFPS ~115, RSS ~600MB, warn rate ~10-30/min.
#   5/24 STORM 시: warn ~6000/min, DFPS 95-100 (2-4분 지속), RSS plateau.
#   5/26 PID 4924 시: warn ~10000/min, DFPS 60-65 (44분 sustained), RSS 1300MB.
ALERT_WARN_DELTA_PER_CYCLE="${ALERT_WARN_DELTA_PER_CYCLE:-500}"  # ≥ 500 warn/cycle → storm signature
ALERT_DFPS_LOW_THRESHOLD="${ALERT_DFPS_LOW_THRESHOLD:-100}"      # DFPS < 100 = degraded
ALERT_DFPS_LOW_STREAK="${ALERT_DFPS_LOW_STREAK:-2}"              # 2 consecutive cycles → real (1 sample = reset artifact)
ALERT_RSS_MB_THRESHOLD="${ALERT_RSS_MB_THRESHOLD:-1100}"         # ≥ 1100MB = 2x baseline → suspect duplicate/leak
ALERT_WARMUP_CYCLES="${ALERT_WARMUP_CYCLES:-4}"                  # 첫 N cycle (~N min) dfps/storm alert skip — boot ramp-up false positive 방지

# state carried between cycles
PREV_WARN=0
PREV_ERR=0
PREV_WD=0
PREV_FTC=0
DFPS_LOW_STREAK=0

echo "[monitor armed $(date +%H:%M:%S)] label=$LABEL container=$CONTAINER commit=$(cd $ROOT && git log -1 --format=%h) cut=$START_LOG_CUT interval=${INTERVAL_SEC}s out=$OUT"
echo "[monitor armed] alert thresholds: warn_delta≥${ALERT_WARN_DELTA_PER_CYCLE}/cycle, dfps<${ALERT_DFPS_LOW_THRESHOLD} for ${ALERT_DFPS_LOW_STREAK} cycles, rss≥${ALERT_RSS_MB_THRESHOLD}MB, err>0, wd>0, ftc>0, cam_loss | warmup=${ALERT_WARMUP_CYCLES} cycles (dfps/storm skip during boot ramp)"

# python builder: log_tail (stdin) + env vars → 1 줄 JSONL.
# heredoc 으로 단일 정의 후 매 cycle 재사용.
PY_BUILDER=$(cat <<'PYEOF'
import sys, json, os, re

log_tail = sys.stdin.read()

def to_num(s, fb=0):
    if s is None or s == "":
        return fb
    try:
        return float(s) if "." in s else int(s)
    except ValueError:
        return fb

def parse_profile(json_line):
    """log JSON entry → flat dict (key=value 와 prefix 패턴 인식)."""
    if not json_line:
        return {}
    try:
        body = json.loads(json_line).get("msg", "")
    except json.JSONDecodeError:
        return {}
    if ": " not in body:
        return {}
    body = body.split(": ", 1)[1]
    out = {}
    for section in body.split(" | "):
        prefix = ""
        for tok in section.split():
            if "=" not in tok:
                prefix = (prefix + "_" + tok) if prefix else tok
                continue
            k, v = tok.split("=", 1)
            if v.endswith("KB"):
                v = v[:-2]; k += "_kb"
            elif v.endswith("MB"):
                v = v[:-2]; k += "_mb"
            fk = (prefix + "_" + k) if prefix else k
            num = to_num(v, None)
            if num is not None:
                out[fk] = num
    return out

cam_ids = sorted(set(re.findall(r"CAM\[(\d+)\]", log_tail)))
lines = log_tail.split("\n")

cams = {}
for cam in cam_ids:
    def last(kind):
        marker = "CAM[{}] {}-thread".format(cam, kind)
        hits = [l for l in lines if marker in l]
        return hits[-1] if hits else ""
    cams[cam] = {
        "rsp": parse_profile(last("RSP")),
        "inf": parse_profile(last("INF")),
        "evt": parse_profile(last("EVT")),
    }

rec = {
    "ts": os.environ["TS"],
    "up_min": int(os.environ["UPTIME_MIN"]),
    "cut": os.environ["START_LOG_CUT"],
    "pipeline": {
        "dfps": to_num(os.environ.get("DFPS")),
        "cam_active": to_num(os.environ.get("CAM_ACTIVE")),
        "cam_registered": to_num(os.environ.get("CAM_REG")),
    },
    "mem": {
        "resident_mb": int(os.environ["RES_MB"]),
        "vm_rss_mb": int(os.environ["VM_RSS_MB"]),
        "rss_anon_mb": int(os.environ["RSS_ANON_MB"]),
        "vm_hwm_mb": int(os.environ["VM_HWM_MB"]),
        "jem_mb": int(os.environ["JEM_MB"]),
    },
    "res": {
        "threads": to_num(os.environ.get("THREADS")),
        "fd": to_num(os.environ.get("FD_COUNT")),
    },
    "events": {
        "reset": int(os.environ["RESET"]),
        "eos": int(os.environ["EOS"]),
        "wd": int(os.environ["WD"]),
        "ftc": int(os.environ["FTC"]),
        "err": int(os.environ["ERR"]),
        "warn": int(os.environ["WRN"]),
    },
    "disk": {
        "used_pct": to_num(os.environ.get("DISK_PCT")),
        "emergency": to_num(os.environ.get("EMERG")),
        "cleanup": to_num(os.environ.get("CLEAN")),
        "skipped": to_num(os.environ.get("SKIPPED")),
    },
    "container": {
        "cpu_pct": to_num(os.environ.get("DOCKER_CPU")),
        "mem_pct": to_num(os.environ.get("DOCKER_MEM")),
    },
    "net": {
        "listen": int(os.environ["LISTEN"]),
        "estab": int(os.environ["ESTAB"]),
    },
    "thermal": {
        "npu_c": to_num(os.environ.get("NPU_TEMP")),
    },
    "cams": cams,
}
print(json.dumps(rec, separators=(",", ":")))
PYEOF
)

# helper: container /proc/1/status field
proc_status_field() {
    docker exec "$CONTAINER" grep "^$1:" /proc/1/status 2>/dev/null | awk '{print $2}'
}

CYC=0
while true; do
    sleep "$INTERVAL_SEC"
    CYC=$((CYC+1))
    NOW_TS=$(date -u +%s)
    UPTIME_MIN=$(( (NOW_TS - START_TS) / 60 ))
    TS=$(date -u '+%Y-%m-%dT%H:%M:%SZ')

    LOG_TAIL=$(tail -5000 "$LOG_PATH" 2>/dev/null)

    # § SAMPLE — log: 최신 DFPS / resident / jem
    DFPS=$(echo "$LOG_TAIL" | grep -oE 'TotalDFPS:[[:space:]]+[0-9.]+' | tail -1 | grep -oE '[0-9.]+$')
    RES_KB=$(echo "$LOG_TAIL" | grep -oE 'resident=[0-9]+KB' | tail -1 | grep -oE '[0-9]+')
    RES_MB=$(( ${RES_KB:-0} / 1024 ))
    JEM_KB=$(echo "$LOG_TAIL" | grep -oE 'jem_alloc=[0-9]+KB' | tail -1 | grep -oE '[0-9]+')
    JEM_MB=$(( ${JEM_KB:-0} / 1024 ))

    # § SAMPLE — log: 이벤트 누적 (start_cut 이후)
    LOG_SLICE=$(awk -F'"ts":"' -v cut="$START_LOG_CUT" '$2 >= cut' "$LOG_PATH")
    # ResetSourceOnly OK — log 메시지 패턴 변천:
    #   Option A (~v0.1.13): "ResetSourceOnly[X] OK — PARTIAL reset duration=..."
    #   Full reset (v0.1.14+): "ResetSourceOnly[X] OK — duration=..."
    # 둘 다 잡으려면 "ResetSourceOnly.*OK" 패턴 사용.
    RESET=$(echo "$LOG_SLICE" | grep -cE "ResetSourceOnly\[[0-9]+\] OK")
    EOS=$(echo "$LOG_SLICE" | grep -c "on_eos trigger")
    WD=$(echo "$LOG_SLICE" | grep -c "frame-age watchdog")
    FTC=$(echo "$LOG_SLICE" | grep -c "Failed to connect")
    ERR=$(echo "$LOG_SLICE" | grep -c '"lvl":"ERROR"')
    WRN=$(echo "$LOG_SLICE" | grep -c '"lvl":"WARN"')

    # § SAMPLE — container /proc/1/status
    VM_RSS_KB=$(proc_status_field VmRSS)
    VM_RSS_MB=$(( ${VM_RSS_KB:-0} / 1024 ))
    RSS_ANON_KB=$(proc_status_field RssAnon)
    RSS_ANON_MB=$(( ${RSS_ANON_KB:-0} / 1024 ))
    VM_HWM_KB=$(proc_status_field VmHWM)
    VM_HWM_MB=$(( ${VM_HWM_KB:-0} / 1024 ))
    THREADS=$(proc_status_field Threads)
    FD_COUNT=$(docker exec "$CONTAINER" bash -c 'ls /proc/1/fd 2>/dev/null | wc -l' 2>/dev/null)

    # § SAMPLE — Prometheus endpoint
    METRIC_DUMP=$(curl -s --max-time 3 "$METRICS_URL" 2>/dev/null)
    CAM_ACTIVE=$(echo "$METRIC_DUMP" | grep -E '^detectbase_camera_count\{state="active"\}' | awk '{print $NF}' | head -1)
    CAM_REG=$(echo "$METRIC_DUMP" | grep -E '^detectbase_camera_count\{state="registered"\}' | awk '{print $NF}' | head -1)
    DISK_PCT=$(echo "$METRIC_DUMP" | grep -E '^detectbase_frame_disk_used_pct ' | awk '{print $NF}' | head -1)
    EMERG=$(echo "$METRIC_DUMP" | grep -E '^detectbase_frame_emergency_cleanup_total' | awk '{s+=$NF}END{print s+0}')
    CLEAN=$(echo "$METRIC_DUMP" | grep -E '^detectbase_frame_cleanup_deleted_total ' | awk '{print $NF}' | head -1)
    SKIPPED=$(echo "$METRIC_DUMP" | grep -E '^detectbase_imwrite_skipped_total' | awk '{s+=$NF}END{print s+0}')

    # § SAMPLE — docker stats (~1-2s)
    DOCKER_STATS=$(docker stats --no-stream --format "{{.CPUPerc}}|{{.MemPerc}}" "$CONTAINER" 2>/dev/null)
    DOCKER_CPU=$(echo "$DOCKER_STATS" | cut -d'|' -f1 | tr -d '%')
    DOCKER_MEM=$(echo "$DOCKER_STATS" | cut -d'|' -f2 | tr -d '%')

    # § SAMPLE — host socket (network_mode=host)
    PROC_PID=$(docker inspect -f '{{.State.Pid}}' "$CONTAINER" 2>/dev/null)
    LISTEN=0
    ESTAB=0
    if [ -n "$PROC_PID" ] && [ "$PROC_PID" != "0" ]; then
        LISTEN=$(ss -tnp 2>/dev/null | awk -v pid="pid=${PROC_PID}," '$0 ~ pid && /LISTEN/' | wc -l)
        ESTAB=$(ss -tnp 2>/dev/null | awk -v pid="pid=${PROC_PID}," '$0 ~ pid && /ESTAB/' | wc -l)
    fi

    # § SAMPLE — NPU thermal
    NPU_TEMP=""
    if [ -r /sys/class/thermal/thermal_zone0/temp ]; then
        TEMP_RAW=$(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null)
        [ -n "$TEMP_RAW" ] && NPU_TEMP=$(awk -v t="$TEMP_RAW" 'BEGIN{printf "%.1f", t/1000}')
    fi

    # § BUILD — python builder (stdin=log_tail, env=metrics, stdout=JSONL line)
    TS="$TS" UPTIME_MIN="$UPTIME_MIN" START_LOG_CUT="$START_LOG_CUT" \
    DFPS="${DFPS:-0}" CAM_ACTIVE="${CAM_ACTIVE:-0}" CAM_REG="${CAM_REG:-0}" \
    RES_MB="$RES_MB" VM_RSS_MB="$VM_RSS_MB" RSS_ANON_MB="$RSS_ANON_MB" VM_HWM_MB="$VM_HWM_MB" JEM_MB="$JEM_MB" \
    THREADS="${THREADS:-0}" FD_COUNT="${FD_COUNT:-0}" \
    RESET="$RESET" EOS="$EOS" WD="$WD" FTC="$FTC" ERR="$ERR" WRN="$WRN" \
    DISK_PCT="${DISK_PCT:-0}" EMERG="${EMERG:-0}" CLEAN="${CLEAN:-0}" SKIPPED="${SKIPPED:-0}" \
    DOCKER_CPU="${DOCKER_CPU:-0}" DOCKER_MEM="${DOCKER_MEM:-0}" \
    LISTEN="$LISTEN" ESTAB="$ESTAB" NPU_TEMP="${NPU_TEMP:-0}" \
    python3 -c "$PY_BUILDER" <<<"$LOG_TAIL" >> "$OUT"

    # § HEARTBEAT (multi-line, human-readable)
    if [ $((CYC % HEARTBEAT_EVERY)) -eq 0 ]; then
        echo "[heartbeat $(date +%H:%M:%S)] up=${UPTIME_MIN}min dfps=${DFPS:-0} cam=${CAM_ACTIVE:-?}/${CAM_REG:-?}"
        echo "  mem: rss=${RES_MB}/${VM_RSS_MB}MB anon=${RSS_ANON_MB}MB hwm=${VM_HWM_MB}MB jem=${JEM_MB}MB | thr=${THREADS:-?} fd=${FD_COUNT:-?}"
        echo "  evt: reset=$RESET eos=$EOS wd=$WD ftc=$FTC | err=$ERR warn=$WRN"
        echo "  ext: disk=${DISK_PCT:-?}% cpu=${DOCKER_CPU:-?}% mem=${DOCKER_MEM:-?}% sock=${LISTEN}L/${ESTAB}E npu=${NPU_TEMP:-?}°C"
    fi

    # § ALERT — delta 기반 (이전 cycle 대비 증가분), edge-trigger 로 spam 방지.
    #   각 alert line 은 한 줄 + [★ prefix + 카테고리 + timestamp 로 시작.
    WARN_DELTA=$(( WRN - PREV_WARN ))
    ERR_DELTA=$(( ERR - PREV_ERR ))
    WD_DELTA=$(( WD - PREV_WD ))
    FTC_DELTA=$(( FTC - PREV_FTC ))

    # 1) WARN rate spike — storm signature (correlation_id mismatch / Queue Full burst)
    #    boot ramp-up 시 누적 시작점부터 측정되므로 첫 cycle 만 skip
    if [ "$CYC" -gt 1 ] && [ "$WARN_DELTA" -ge "$ALERT_WARN_DELTA_PER_CYCLE" ]; then
        echo "[★storm $(date +%H:%M:%S)] warn +${WARN_DELTA}/cycle (total=${WRN}) — possible correlation_id storm / Queue Full burst"
    fi

    # 2) NEW ERROR — 0건이 baseline. 새 err 발생 시 한 번 알림
    if [ "$ERR_DELTA" -gt 0 ]; then
        echo "[★err $(date +%H:%M:%S)] err +${ERR_DELTA} (total=${ERR}) — DetectBase.log lvl=ERROR 발생"
    fi

    # 3) DFPS sustained low — 단발 sample (reset cycle artifact) 무시, ≥${ALERT_DFPS_LOW_STREAK} cycle 연속이면 alert
    #    boot ramp-up 첫 ${ALERT_WARMUP_CYCLES} cycle 은 skip (DetectBase init 직후 stream 안정화 ~3-4분 필요).
    if [ -n "$DFPS" ] && awk -v d="$DFPS" -v thr="$ALERT_DFPS_LOW_THRESHOLD" 'BEGIN{exit !(d>0 && d<thr)}'; then
        DFPS_LOW_STREAK=$(( DFPS_LOW_STREAK + 1 ))
        if [ "$CYC" -gt "$ALERT_WARMUP_CYCLES" ] && [ "$DFPS_LOW_STREAK" -eq "$ALERT_DFPS_LOW_STREAK" ]; then
            echo "[★dfps_low $(date +%H:%M:%S)] dfps=${DFPS} sustained ${DFPS_LOW_STREAK} cycles (<${ALERT_DFPS_LOW_THRESHOLD})"
        fi
    else
        DFPS_LOW_STREAK=0
    fi

    # 4) RSS over budget — duplicate process / leak 시그니처
    if [ "$RES_MB" -ge "$ALERT_RSS_MB_THRESHOLD" ]; then
        echo "[★memory $(date +%H:%M:%S)] rss=${RES_MB}MB (≥${ALERT_RSS_MB_THRESHOLD}MB) — 2x baseline → duplicate process / leak 의심"
    fi

    # 5) WATCHDOG fire (delta)
    if [ "$WD_DELTA" -gt 0 ]; then
        echo "[★watchdog $(date +%H:%M:%S)] wd +${WD_DELTA} (total=${WD}) — cam frame-age timeout"
    fi

    # 6) Failed-To-Connect (delta)
    if [ "$FTC_DELTA" -gt 0 ]; then
        echo "[★ftc $(date +%H:%M:%S)] ftc +${FTC_DELTA} (total=${FTC}) — connection 실패"
    fi

    # 7) Cam loss — registered 보다 active 가 작음
    if [ -n "${CAM_ACTIVE:-}" ] && [ -n "${CAM_REG:-}" ] && [ "${CAM_ACTIVE:-0}" != "0" ] && [ "${CAM_REG:-0}" != "0" ]; then
        if awk -v a="${CAM_ACTIVE:-0}" -v r="${CAM_REG:-0}" 'BEGIN{exit !(a<r)}'; then
            echo "[★cam_loss $(date +%H:%M:%S)] active=${CAM_ACTIVE}/${CAM_REG} — cam 일부 비활성"
        fi
    fi

    PREV_WARN="$WRN"
    PREV_ERR="$ERR"
    PREV_WD="$WD"
    PREV_FTC="$FTC"
done
