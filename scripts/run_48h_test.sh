#!/bin/bash
# =============================================================================
# 48시간 운영 안정성 테스트
# - 1시간 간격으로 snapshot 수집 (총 48 snapshot + summary)
# - 검증 대상: 메모리/FD/Thread leak, ERROR/WARN drift, 디스크 cleanup, DFPS 안정성
# - nohup 백그라운드 실행 권장 (SSH 끊겨도 지속)
#
# 사용:
#   nohup ./scripts/run_48h_test.sh > /dev/null 2>&1 &
#   echo $! > /tmp/48h_test.pid
#
# 결과: logs/test_48h_<timestamp>/
# =============================================================================

set -u

SCRIPT_PATH="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DETECTBASE_ROOT="$(dirname "$SCRIPT_PATH")"
LOG_FILE="${DETECTBASE_ROOT}/logs/DetectBase.log"
METRICS_URL="http://localhost:9090/metrics"

STAMP="$(date +%Y%m%d_%H%M%S)"
TEST_DIR="${DETECTBASE_ROOT}/logs/test_48h_${STAMP}"
mkdir -p "${TEST_DIR}"

START_TS=$(date +%s)
DURATION_SEC=$((48 * 3600))  # 48시간
END_TS=$((START_TS + DURATION_SEC))
INTERVAL_SEC=3600  # 1시간 간격
TOTAL_SNAPS=48

PID_FILE="${TEST_DIR}/.pid"
echo "$$" > "${PID_FILE}"

# -----------------------------------------------------------------------------
# 시작 마커 + baseline
# -----------------------------------------------------------------------------
{
    echo "===== 48시간 운영 테스트 시작 ====="
    echo "시작: $(date '+%Y-%m-%d %H:%M:%S KST')"
    echo "종료 예정: $(date -d @$END_TS '+%Y-%m-%d %H:%M:%S KST')"
    echo "snapshot 간격: 1시간 (총 ${TOTAL_SNAPS} 회)"
    echo "PID: $$"
    echo "결과: ${TEST_DIR}"
    echo "운영 컨테이너: detectbase_service (audit / restart 시 노이즈 발생 가능 — 테스트 중 자제 권고)"
} > "${TEST_DIR}/INFO.txt"

# -----------------------------------------------------------------------------
# 단일 snapshot 함수
# -----------------------------------------------------------------------------
take_snapshot() {
    local iter=$1
    local snap_file="${TEST_DIR}/snap_$(printf %02d $iter)_$(date +%Y%m%d_%H%M%S).txt"
    local elapsed_min=$(( ($(date +%s) - START_TS) / 60 ))

    {
        echo "===== snapshot $iter at $(date '+%Y-%m-%d %H:%M:%S KST') ====="
        echo "elapsed: ${elapsed_min} min ($(( elapsed_min / 60 ))h $(( elapsed_min % 60 ))m)"
        echo
        echo "----- container status -----"
        docker ps --filter name=detectbase_service --format "name={{.Names}} status={{.Status}}"
        echo
        echo "----- main process -----"
        docker exec detectbase_service ps -o pid,etime,vsz,rss,pcpu,pmem,comm -p 1 2>&1 || echo "container down"
        echo
        echo "----- /proc/1/status -----"
        docker exec detectbase_service grep -E "^(VmPeak|VmSize|VmRSS|VmHWM|RssAnon|RssFile|VmData|Threads)" /proc/1/status 2>&1 || echo "N/A"
        echo
        echo "----- file descriptors -----"
        docker exec detectbase_service bash -c 'ls /proc/1/fd | wc -l' 2>&1 || echo "N/A"
        echo "fd_count above"
        echo
        echo "----- ERROR / WARN 누적 -----"
        echo -n "ERROR: "; grep -c '"lvl":"ERROR"' "${LOG_FILE}" 2>/dev/null || echo 0
        echo -n "WARN:  "; grep -c '"lvl":"WARN"' "${LOG_FILE}" 2>/dev/null || echo 0
        echo -n "INFO:  "; grep -c '"lvl":"INFO"' "${LOG_FILE}" 2>/dev/null || echo 0
        echo
        echo "----- key metrics -----"
        curl -s "${METRICS_URL}" 2>/dev/null | grep -E "^detectbase_(dfps|camera_count|errors_total|frame_disk|frame_emergency|frame_cleanup|setting_partial|socketio_reconnect|imwrite_skipped)" | head -40 || echo "metrics endpoint down"
        echo
        echo "----- 디스크 사용량 -----"
        df -h /hdd_ext 2>/dev/null | tail -1
        df -h / 2>/dev/null | tail -1
    } > "${snap_file}" 2>&1
}

# -----------------------------------------------------------------------------
# 메인 루프
# -----------------------------------------------------------------------------
take_snapshot 0  # baseline

iter=1
while [ $(date +%s) -lt $END_TS ]; do
    sleep $INTERVAL_SEC
    take_snapshot $iter
    iter=$((iter + 1))
done

# -----------------------------------------------------------------------------
# 종합 summary 생성
# -----------------------------------------------------------------------------
{
    echo "# 48시간 운영 안정성 테스트 — 종합 보고"
    echo
    echo "**기간**: $(date -d @$START_TS '+%Y-%m-%d %H:%M:%S') ~ $(date '+%Y-%m-%d %H:%M:%S') KST"
    echo "**snapshot**: ${iter} 개 (1시간 간격)"
    echo
    echo "## §1. 핵심 추이"
    echo
    echo "### baseline (snap 0) vs final (snap $((iter-1)))"
    echo
    echo '```'
    echo "[ baseline ]"
    head -50 "${TEST_DIR}"/snap_00_*.txt 2>/dev/null
    echo
    echo "[ final ]"
    tail -50 "${TEST_DIR}"/snap_$(printf %02d $((iter-1)))_*.txt 2>/dev/null
    echo '```'
    echo
    echo "## §2. ERROR / WARN 추이"
    echo
    echo '| snap | elapsed | ERROR | WARN |'
    echo '|---|---|---|---|'
    for f in "${TEST_DIR}"/snap_*.txt; do
        local snap_n=$(basename "$f" | awk -F_ '{print $2}')
        local elapsed=$(grep "^elapsed:" "$f" | awk '{print $2}')
        local err=$(grep "^ERROR:" "$f" | awk '{print $2}')
        local wrn=$(grep "^WARN:" "$f" | awk '{print $2}')
        echo "| $snap_n | ${elapsed} min | $err | $wrn |"
    done
    echo
    echo "## §3. 메모리 / FD / Thread 추이"
    echo
    echo '| snap | VmRSS (KB) | RssAnon (KB) | Threads | FD |'
    echo '|---|---|---|---|---|'
    for f in "${TEST_DIR}"/snap_*.txt; do
        local snap_n=$(basename "$f" | awk -F_ '{print $2}')
        local rss=$(grep "^VmRSS:" "$f" | awk '{print $2}')
        local rss_anon=$(grep "^RssAnon:" "$f" | awk '{print $2}')
        local threads=$(grep "^Threads:" "$f" | awk '{print $2}')
        local fd=$(awk '/^fd_count above/{print prev} {prev=$0}' "$f")
        echo "| $snap_n | $rss | $rss_anon | $threads | $fd |"
    done
    echo
    echo "## §4. 디스크 cleanup 추이"
    echo
    echo '| snap | frame_disk_used_pct | emergency_cleanup_total | frame_cleanup_deleted_total |'
    echo '|---|---|---|---|'
    for f in "${TEST_DIR}"/snap_*.txt; do
        local snap_n=$(basename "$f" | awk -F_ '{print $2}')
        local pct=$(grep "frame_disk_used_pct" "$f" | awk '{print $2}')
        local emg=$(grep "frame_emergency_cleanup_total{type=\"day_dir\"}" "$f" | awk '{print $2}')
        local cln=$(grep "^detectbase_frame_cleanup_deleted_total" "$f" | awk '{print $2}')
        echo "| $snap_n | $pct | ${emg:-0} | ${cln:-0} |"
    done
    echo
    echo "## §5. DFPS / camera_count 안정성"
    echo
    echo '| snap | DFPS | active cam | registered cam |'
    echo '|---|---|---|---|'
    for f in "${TEST_DIR}"/snap_*.txt; do
        local snap_n=$(basename "$f" | awk -F_ '{print $2}')
        local dfps=$(grep "^detectbase_dfps_total " "$f" | awk '{print $2}')
        local act=$(grep 'camera_count{state="active"}' "$f" | awk '{print $2}')
        local reg=$(grep 'camera_count{state="registered"}' "$f" | awk '{print $2}')
        echo "| $snap_n | $dfps | $act | $reg |"
    done
    echo
    echo "## §6. 결론 자동 평가"
    echo
    # leak 판정
    local first_rss=$(grep "^VmRSS:" "${TEST_DIR}"/snap_00_*.txt 2>/dev/null | awk '{print $2}')
    local last_rss=$(grep "^VmRSS:" "${TEST_DIR}"/snap_$(printf %02d $((iter-1)))_*.txt 2>/dev/null | awk '{print $2}')
    local rss_growth_pct=0
    if [ -n "$first_rss" ] && [ "$first_rss" -gt 0 ]; then
        rss_growth_pct=$(( (last_rss - first_rss) * 100 / first_rss ))
    fi
    local first_fd=$(awk '/^fd_count above/{print prev} {prev=$0}' "${TEST_DIR}"/snap_00_*.txt 2>/dev/null)
    local last_fd=$(awk '/^fd_count above/{print prev} {prev=$0}' "${TEST_DIR}"/snap_$(printf %02d $((iter-1)))_*.txt 2>/dev/null)
    local first_th=$(grep "^Threads:" "${TEST_DIR}"/snap_00_*.txt 2>/dev/null | awk '{print $2}')
    local last_th=$(grep "^Threads:" "${TEST_DIR}"/snap_$(printf %02d $((iter-1)))_*.txt 2>/dev/null | awk '{print $2}')

    echo "- VmRSS: ${first_rss} → ${last_rss} KB (${rss_growth_pct}%) — 5% 이내면 안정"
    echo "- FD:    ${first_fd} → ${last_fd}    — 변화 없음이 정상"
    echo "- Threads: ${first_th} → ${last_th} — 변화 없음이 정상"
    echo
    echo "- ERROR 누적 추이: §2 표 참조 (운영 중 0 유지가 정상)"
    echo "- 디스크 cleanup: §4 표 참조 (자연 운영으로 cap 됐는지)"
} > "${TEST_DIR}/SUMMARY.md"

rm -f "${PID_FILE}"
echo "===== 48h test 완료 $(date '+%Y-%m-%d %H:%M:%S KST') =====" >> "${TEST_DIR}/INFO.txt"
