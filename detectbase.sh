#!/bin/bash
# =============================================================================
# DetectBase 서비스 통합 관리 스크립트
# 사용법: ./detectbase.sh <command>
# =============================================================================

SCRIPT_PATH="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DOCKER_COMPOSE_FILE="${SCRIPT_PATH}/docker-compose.yml"
CONTAINER_NAME="detectbase_service"
IMAGE_NAME="detectbase:1.0"

# .env 파일 로드 (있을 경우 — IMAGE_ROOT_PATH 등 환경변수)
if [[ -f "${SCRIPT_PATH}/.env" ]]; then
    set -a
    source "${SCRIPT_PATH}/.env"
    set +a
fi

# -----------------------------------------------------------------------------
# 출력 유틸
# -----------------------------------------------------------------------------
RED='\e[31m'
GREEN='\e[32m'
YELLOW='\e[33m'
CYAN='\e[36m'
NC='\e[0m'

log_info()  { echo -e "${CYAN}[INFO]${NC}  $1"; }
log_warn()  { echo -e "${YELLOW}[WARN]${NC}  $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }
log_done()  { echo -e "${GREEN}[DONE]${NC}  $1"; }

# -----------------------------------------------------------------------------
# NPU 환경 사전 점검 — Odroid M2 (RK3588S) NPU 의존 명령 (compile/start/restart) 진입 직전 호출.
# 부재 시 친절한 복구 안내 후 exit 1. cmd_init 의 informational 경고 (line ~81-94) 와는
# 별개 — 본 함수는 차단형. 배경: docker 가 host /usr/lib/librknnrt.so 또는
# /dev/dri/renderD129 부재 시 mount 시점에 빈 디렉토리/path 를 자동 생성하여 host file
# 을 망가뜨리는 사고를 차단한다 (한 번 디렉토리가 되면 다음 mount 도 디렉토리 유지).
# -----------------------------------------------------------------------------
_check_npu_env() {
    local librknn_src="${LIBRKNN_SRC:-/home/claudedev/tmp/rknpu2_v152/runtime/RK3588/Linux/librknn_api/aarch64/librknnrt.so}"
    local missing=()

    [[ -e /dev/dri/renderD129 ]] || missing+=("/dev/dri/renderD129 (rknpu kernel module 미적재)")
    if [[ ! -f /usr/lib/librknnrt.so ]]; then
        missing+=("/usr/lib/librknnrt.so (file 부재 또는 디렉토리로 잘못 생성됨)")
    elif ! file /usr/lib/librknnrt.so 2>/dev/null | grep -q 'GNU/Linux'; then
        # ABI 차이 차단: Android arm64-v8a build 등 잘못된 build 가 mount 되면 service 시작 직후
        # 'correlation_id mismatch' warn 폭주 (~30-58/sec) — engine response 구조 미세 차이.
        # 2026-05-30 사고: 사용자가 RK3588 Android arm64-v8a build 를 잘못 복원하여 3h 검증 회귀.
        # 정확한 build = RK3588/Linux/aarch64 (GNU/Linux ELF), audit backup 와 md5 일치.
        missing+=("/usr/lib/librknnrt.so 가 GNU/Linux build 아님 (Android build 등). 'file /usr/lib/librknnrt.so' 출력 확인")
    fi

    [[ ${#missing[@]} -eq 0 ]] && return 0

    log_error "NPU 환경 부재 — docker 진입 차단. 누락 항목:"
    for m in "${missing[@]}"; do
        log_error "  • ${m}"
    done
    log_warn ""
    log_warn "복구 명령 (sudo 필요):"
    log_warn "  1) NPU module load:"
    log_warn "       sudo modprobe rknpu"
    log_warn "     boot 시 자동 load 등록 (한 번만):"
    log_warn "       echo rknpu | sudo tee /etc/modules-load.d/rknpu.conf"
    log_warn ""
    log_warn "  2) librknnrt.so 복원 (디렉토리로 망가졌다면 제거 후 재복원):"
    log_warn "       sudo rm -rf /usr/lib/librknnrt.so"
    log_warn "       sudo cp ${librknn_src} /usr/lib/librknnrt.so"
    log_warn ""
    log_warn "  3) 확인 (build 가 GNU/Linux 인지 반드시 확인 — Android build 면 correlation_id storm 유발):"
    log_warn "       ls /dev/dri/renderD129 && file /usr/lib/librknnrt.so | grep -q 'GNU/Linux' && echo OK"
    log_warn ""
    log_warn "배경: 본 명령은 docker 에 /dev/dri/renderD129 device 와 /usr/lib/librknnrt.so file"
    log_warn "  mount 를 요구합니다. host 에 없을 시 docker 가 자동으로 빈 디렉토리를 생성해"
    log_warn "  librknnrt.so 가 망가집니다. 한 번 디렉토리가 되면 다음 mount 도 디렉토리로 유지됨."
    log_warn ""
    log_warn "복구 후 다시 시도하십시오. (LIBRKNN_SRC env var 로 source path override 가능)"
    exit 1
}

# -----------------------------------------------------------------------------
# 서브커맨드
# -----------------------------------------------------------------------------

cmd_build() {
    log_info "Docker 이미지 빌드 시작 (docker-compose build)..."
    docker-compose -f "${DOCKER_COMPOSE_FILE}" build
    log_done "Docker 이미지 빌드 완료."

    # 빌드 후 자동 init (protoc 버전 변경 가능성 → proto 재생성 필요)
    echo
    log_info "빌드 후 자동 init 실행..."
    cmd_init
}

cmd_compile() {
    _check_npu_env
    log_info "컨테이너 내 C++ 컴파일 시작 (BuildScript.sh)..."
    docker run --rm \
        -v "${SCRIPT_PATH}":/DetectBase \
        --device /dev/dri/renderD129:/dev/dri/renderD129 \
        -v /usr/lib/librknnrt.so:/usr/lib/librknnrt.so:ro \
        "${IMAGE_NAME}" \
        bash /DetectBase/code/.tool/BuildScript.sh
    log_done "C++ 컴파일 완료."
}

cmd_init() {
    log_info "초기화 시작 (proto 재생성, 의존성 점검)..."

    # 이미지 존재 확인
    if ! docker image inspect "${IMAGE_NAME}" >/dev/null 2>&1; then
        log_error "이미지 ${IMAGE_NAME} 가 없습니다. 먼저 './detectbase.sh build' 를 실행하세요."
        exit 1
    fi

    # 1. Main proto 재생성 (도커 내 protoc 로 새로 생성 — 버전/ABI 일치 보장)
    if [[ -f "${SCRIPT_PATH}/code/Protocol/GRPC/protos/MgenProto.proto" ]]; then
        log_info "Main proto 재생성: code/Protocol/GRPC/protos/MgenProto.proto"
        docker run --rm \
            -v "${SCRIPT_PATH}":/DetectBase \
            "${IMAGE_NAME}" \
            bash -c "cd /DetectBase/code/Protocol/GRPC/protos && \
                protoc --cpp_out=. --grpc_out=. \
                --plugin=protoc-gen-grpc=\$(which grpc_cpp_plugin) \
                MgenProto.proto"
        log_info "  → MgenProto.{pb,grpc.pb}.{h,cc} 갱신됨"
    fi

    # 2. 호스트 측 librknnrt.so 존재 확인 (런타임 마운트용)
    if [[ -f /usr/lib/librknnrt.so ]]; then
        log_info "호스트 librknnrt.so 확인됨: /usr/lib/librknnrt.so"
    else
        log_warn "/usr/lib/librknnrt.so 가 호스트에 없습니다. NPU 런타임 시 docker-compose 마운트 실패 가능."
        log_warn "  → RKNPU 드라이버 + rknn-toolkit2 의 librknnrt.so 설치 필요"
    fi

    # 3. 호스트 NPU 디바이스 확인
    if [[ -e /dev/dri/renderD129 ]]; then
        log_info "호스트 NPU 디바이스 확인됨: /dev/dri/renderD129"
    else
        log_warn "/dev/dri/renderD129 디바이스가 없습니다. RKNPU 드라이버 미로드 의심: 'sudo insmod rknpu.ko'"
    fi

    # 4. 로그 디렉토리 확보
    mkdir -p "${SCRIPT_PATH}/logs"
    log_info "로그 디렉토리 확보: ${SCRIPT_PATH}/logs"

    log_done "초기화 완료."
}

cmd_start() {
    _check_npu_env
    # 이미 실행 중이면 정지 후 재시작
    if docker ps -q -f name="^/${CONTAINER_NAME}$" | grep -q .; then
        log_warn "서비스 '${CONTAINER_NAME}' 가 이미 실행 중. 재시작합니다."
        cmd_stop
        sleep 3
    fi

    # 로그 디렉토리 확보
    LOG_DIR="${SCRIPT_PATH}/logs"
    mkdir -p "${LOG_DIR}"

    # 서비스 내부 로그 (심볼릭 링크 → 실제 파일)
    LOG_LINK="${SCRIPT_PATH}/service.log"
    LOG_REAL="$(readlink -f "${LOG_LINK}" 2>/dev/null || echo "${LOG_DIR}/DetectBase.log")"

    # 기존 로그 백업 (비어있지 않을 때만)
    if [[ -f "${LOG_REAL}" ]] && [[ -s "${LOG_REAL}" ]]; then
        BACKUP="${LOG_DIR}/.backup_detectbase_service_$(date '+%Y%m%d_%H%M%S').log"
        log_info "기존 로그 백업: ${LOG_REAL} → ${BACKUP}"
        mv "${LOG_REAL}" "${BACKUP}"
    fi
    touch "${LOG_REAL}"
    chmod 666 "${LOG_REAL}" 2>/dev/null || true

    log_info "서비스 시작 (docker-compose up -d)..."
    docker-compose -f "${DOCKER_COMPOSE_FILE}" up -d
    log_done "서비스 시작 완료."

    log_info "로그 follow (Ctrl+C 로 중지 — 서비스는 계속 실행됨)..."
    log_info "  로그 전체 확인: docker logs ${CONTAINER_NAME}"
    tail -n +1 -f "${LOG_LINK}"
}

cmd_stop() {
    log_info "서비스 정지 시도..."

    # docker-compose 가 stop_signal=SIGINT 를 송신하고 stop_grace_period 동안
    # DetectBase 의 PROGRAM QUIT 절차 (#01~#05) 가 완료되도록 대기 후 컨테이너 제거.
    docker-compose -f "${DOCKER_COMPOSE_FILE}" down
    log_done "서비스 정지 완료."
}

cmd_restart() {
    _check_npu_env
    cmd_stop
    sleep 3
    cmd_start
}

cmd_logs() {
    log_info "서비스 로그 follow (Ctrl+C 로 중지)..."
    docker logs -f "${CONTAINER_NAME}"
}

cmd_prune() {
    log_warn "사용 안 하는 도커 리소스 (컨테이너/이미지/네트워크/빌드캐시) 모두 삭제합니다."
    log_warn "현재 실행 중이지 않은 컨테이너 + 어떤 컨테이너도 사용하지 않는 이미지가 삭제됩니다."
    read -p "계속하시겠습니까? [y/N] " -n 1 -r
    echo
    if [[ ${REPLY} =~ ^[Yy]$ ]]; then
        docker system prune -a
        log_done "Docker 정리 완료."
    else
        log_info "취소됨."
    fi
}

cmd_all() {
    cmd_build      # build 안에서 init 자동 호출됨
    cmd_compile
    cmd_start
}

# -----------------------------------------------------------------------------
# audit — 정적 + 동적 분석 도구 (5종, 단계별 분리 실행 가능)
#  도구별 모드 (각 도구만 독립 실행):
#   ./detectbase.sh audit --only cppcheck    # 정적
#   ./detectbase.sh audit --only clang-tidy  # 정적
#   ./detectbase.sh audit --only asan        # 동적 (ASan+UBSan, 운영 정지)
#   ./detectbase.sh audit --only ubsan       # 동적 (asan 과 동일 빌드, alias)
#   ./detectbase.sh audit --only tsan        # 동적 (TSan, 운영 정지)
#  묶음 모드:
#   ./detectbase.sh audit                    # 전체 (1+2+3+5), light (ASan 60m + TSan 60m)
#   ./detectbase.sh audit --strict           # 전체, strict (ASan 240m + TSan 60m, master merge gate 용)
#   ./detectbase.sh audit --no-tsan          # cppcheck + clang-tidy + asan/ubsan
#   ./detectbase.sh audit --with-tsan        # = 전체 (backward compat)
#  강도 모드 (light = default | strict):
#   light  — ASan 60분 + TSan 60분 (develop/내부 검증용, ~1h 30min)
#   strict — ASan 240분 + TSan 60분 (master merge gate 용, ~5h)
#   ASAN_DURATION_MIN / TSAN_DURATION_SEC env var override 항상 우선
#  결과: logs/audit_<timestamp>/ (실행한 stage 의 *.log + summary.txt)
#  ASan/TSan 실행 시 운영 detectbase_service 가 자동 graceful 정지/재시작됨
# -----------------------------------------------------------------------------

# 공통 변수 setter — 모든 stage 가 공유. cmd_audit 가 호출 전 set.
_audit_set_paths() {
    AUDIT_ANALYSIS_IMG="detectbase:analysis"
    AUDIT_LIBRKNN_HOST="${SCRIPT_PATH}/code/Engine/NPU/librknn_api/aarch64/librknnrt.so"
    AUDIT_FRAME_HOST="${IMAGE_ROOT_PATH:-/hdd_ext/images}/frame"
    AUDIT_CROP_HOST="${IMAGE_ROOT_PATH:-/hdd_ext/images}/crop"
}

# 분석 image 존재 확인 + 없으면 build
_audit_ensure_analysis_image() {
    if ! docker image inspect "${AUDIT_ANALYSIS_IMG}" >/dev/null 2>&1; then
        log_info "분석 이미지 ${AUDIT_ANALYSIS_IMG} 빌드 중..."
        docker build -f "${SCRIPT_PATH}/Dockerfile.analysis" -t "${AUDIT_ANALYSIS_IMG}" "${SCRIPT_PATH}"
    fi
}

# stage 1: cppcheck
_audit_run_cppcheck() {
    local OUT_DIR="$1"
    log_info "[cppcheck] 자체 코드 정적 분석 (외부 RTSP 제외)..."
    docker run --rm \
        -v "${SCRIPT_PATH}/code:/code:ro" \
        "${AUDIT_ANALYSIS_IMG}" \
        bash -c 'cppcheck --enable=warning,style,performance,portability \
            --suppress=missingIncludeSystem --inline-suppr --std=c++17 \
            --error-exitcode=0 -j 4 \
            /code/Main /code/Engine /code/Management /code/BasicLibs \
            /code/AbnormalActions /code/Tracker /code/VisionCommon /code/Protocol/GRPC 2>&1' \
        > "${OUT_DIR}/cppcheck.log"
    CPPCHECK_COUNT=$(grep -cE "^/code/" "${OUT_DIR}/cppcheck.log" 2>/dev/null || echo 0)
    log_done "[cppcheck] 자체 코드 ${CPPCHECK_COUNT}건 → cppcheck.log"
}

# stage 2: clang-tidy
_audit_run_clangtidy() {
    local OUT_DIR="$1"
    log_info "[clang-tidy] 자체 cmake configure → 100% file 분석..."
    docker run --rm \
        -v "${SCRIPT_PATH}/code:/work/code:ro" \
        "${AUDIT_ANALYSIS_IMG}" \
        bash -c '
            set -e
            rsync -a --delete /work/code/ /tmp/src/
            mkdir -p /tmp/tidy-build && cd /tmp/tidy-build
            cmake /tmp/src \
                -DCMAKE_BUILD_TYPE=Release \
                -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null 2>&1
            FILES=$(grep -oE "\"file\":[[:space:]]*\"/tmp/src/(Main|Engine|Management|BasicLibs|AbnormalActions|Tracker|VisionCommon|Protocol/GRPC)/[^\"]*\.cpp\"" \
                /tmp/tidy-build/compile_commands.json | grep -oE "/tmp/src/[^\"]*\.cpp" | sort -u)
            echo "분석 대상 파일: $(echo "$FILES" | wc -l)"
            echo "=== analysis ==="
            echo "$FILES" | while read f; do
                [ -z "$f" ] && continue
                clang-tidy -p /tmp/tidy-build --quiet --warnings-as-errors="" \
                    --checks="-*,bugprone-*,performance-*,clang-analyzer-core.*,clang-analyzer-cplusplus.*,clang-analyzer-deadcode.*,cppcoreguidelines-pro-type-cstyle-cast,cppcoreguidelines-pro-type-member-init,modernize-use-nullptr,modernize-use-override" \
                    "$f" 2>/dev/null
            done
        ' > "${OUT_DIR}/clangtidy.log" 2>&1
    TIDY_WARN=$(grep -cE "warning:" "${OUT_DIR}/clangtidy.log" 2>/dev/null || echo 0)
    TIDY_ERR=$(grep -cE "clang-diagnostic-error" "${OUT_DIR}/clangtidy.log" 2>/dev/null || echo 0)
    log_done "[clang-tidy] warning ${TIDY_WARN}건, diagnostic-error ${TIDY_ERR}건 → clangtidy.log"
}

# stage 3: asan + ubsan (한 빌드, 같이 실행)
_audit_run_asan_ubsan() {
    local OUT_DIR="$1"
    local STAMP="$2"
    log_info "[asan/ubsan] 빌드 + 실행 (운영 정지 동반)..."
    log_warn "  → 운영 컨테이너 ${CONTAINER_NAME} 가 graceful 정지됩니다 (stage 후 자동 재시작)"
    mkdir -p "${OUT_DIR}/asan_pkg"
    docker run --rm \
        -v "${SCRIPT_PATH}/code:/work/code:ro" \
        -v "${OUT_DIR}/asan_pkg:/work/out:rw" \
        "${AUDIT_ANALYSIS_IMG}" \
        bash -c '
            set -e
            mkdir -p /tmp/asan && cd /tmp/asan
            rsync -a --delete /work/code/ /tmp/src/
            cmake /tmp/src \
                -DCMAKE_BUILD_TYPE=Debug \
                -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
                -DCMAKE_C_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
                -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined" \
                -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address,undefined" >/dev/null 2>&1
            make -j$(nproc) >/dev/null 2>&1
            cp /tmp/asan/Main/DetectBase /work/out/
            find /tmp/asan -name "*.so" -exec cp {} /work/out/ \;
        ' > "${OUT_DIR}/asan_build.log" 2>&1

    if [[ -x "${OUT_DIR}/asan_pkg/DetectBase" ]]; then
        # ASan run duration.
        #   - AUDIT_STRICT=1 (--strict 플래그) → default 240 min (master merge gate)
        #   - 그 외 (light, default)          → default 60 min (develop/내부 검증)
        #   - ASAN_DURATION_MIN env var       → 항상 override (사용자 지정)
        # 최소 60 min 강제 (init 비중 분리 위함).
        # SIGUSR1 시점: T+5min, T+15min, T+30min, T+60min, T+120min, T+240min (interval mid-run leak check)
        local _ASAN_DEFAULT=60
        if [[ "${AUDIT_STRICT:-0}" == "1" ]]; then
            _ASAN_DEFAULT=240
        fi
        local ASAN_DURATION_MIN="${ASAN_DURATION_MIN:-$_ASAN_DEFAULT}"
        if [[ $ASAN_DURATION_MIN -lt 60 ]]; then
            log_warn "  ASAN_DURATION_MIN=${ASAN_DURATION_MIN}분 → 최소 60분으로 강제 (init 비중 분리 위함)"
            ASAN_DURATION_MIN=60
        fi
        local ASAN_DURATION_SEC=$(( ASAN_DURATION_MIN * 60 ))
        log_info "  ASan binary OK. 운영 정지 + ${ASAN_DURATION_MIN}분 실행 (interval SIGUSR1 leak check)..."
        docker-compose -f "${DOCKER_COMPOSE_FILE}" stop >/dev/null 2>&1

        (
            for t_sec in 300 900 1800 3600 7200; do
                if [[ $t_sec -ge $ASAN_DURATION_SEC ]]; then break; fi
                sleep $t_sec
                docker exec detectbase_asan sh -c 'kill -USR1 1' 2>/dev/null \
                    && echo "===== SIGUSR1 sent at t+${t_sec}s =====" >> "${OUT_DIR}/asan_run.log" \
                    || echo "SIGUSR1 send failed at t+${t_sec}s" >> "${OUT_DIR}/asan_run.log"
            done
        ) &
        local SIGUSR1_BG_PID=$!

        docker run --rm --name detectbase_asan \
            --privileged --network host \
            --device /dev/dri/renderD129:/dev/dri/renderD129 \
            -v "${SCRIPT_PATH}":/DetectBase:rw \
            -v "${AUDIT_LIBRKNN_HOST}":/usr/local/lib/librknnrt.so:ro \
            -v /etc/localtime:/etc/localtime:ro \
            -v "${AUDIT_FRAME_HOST}":/frame:rw \
            -v "${AUDIT_CROP_HOST}":/crop:rw \
            -e ASAN_OPTIONS="halt_on_error=0:print_stats=1:detect_leaks=1:malloc_context_size=20" \
            -e UBSAN_OPTIONS="halt_on_error=0:print_stacktrace=1" \
            -e LD_LIBRARY_PATH="/DetectBase/logs/audit_${STAMP}/asan_pkg:/usr/local/lib" \
            -e TZ=Asia/Seoul --shm-size=512m \
            "${AUDIT_ANALYSIS_IMG}" \
            timeout --signal=SIGINT "${ASAN_DURATION_SEC}s" "/DetectBase/logs/audit_${STAMP}/asan_pkg/DetectBase" \
            >> "${OUT_DIR}/asan_run.log" 2>&1 || true

        kill "$SIGUSR1_BG_PID" 2>/dev/null || true

        log_info "  운영 재시작..."
        docker-compose -f "${DOCKER_COMPOSE_FILE}" up -d >/dev/null 2>&1
    else
        log_error "  ASan binary 빌드 실패 → asan_build.log 참조"
    fi
    ASAN_LEAK=$(grep -m1 "SUMMARY: AddressSanitizer" "${OUT_DIR}/asan_run.log" 2>/dev/null || echo "(no leak)")
    log_done "[asan/ubsan] ${ASAN_LEAK}"
}

# stage 4: tsan
_audit_run_tsan() {
    local OUT_DIR="$1"
    local STAMP="$2"
    # TSan run duration. TSAN_DURATION_SEC env var override (default 3600s = 1h, min 3600s).
    local TSAN_DURATION_SEC="${TSAN_DURATION_SEC:-3600}"
    if [[ $TSAN_DURATION_SEC -lt 3600 ]]; then
        log_warn "  TSAN_DURATION_SEC=${TSAN_DURATION_SEC}s → 최소 3600s(1h)으로 강제"
        TSAN_DURATION_SEC=3600
    fi
    log_info "[tsan] 빌드 + 실행 (~5-10분 빌드 + 운영 ${TSAN_DURATION_SEC}s 정지)..."
    log_warn "  → TSan 100x 느림. race report 는 stderr 즉시 출력 (timeout 후 SIGKILL)"
    log_warn "  → 깊이 검증 시 SettingData.cpp 의 __SANITIZE_THREAD__ guard 로 fps=1 강제 적용됨"
    mkdir -p "${OUT_DIR}/tsan_pkg"
    docker run --rm \
        -v "${SCRIPT_PATH}/code:/work/code:ro" \
        -v "${OUT_DIR}/tsan_pkg:/work/out:rw" \
        "${AUDIT_ANALYSIS_IMG}" \
        bash -c '
            set -e
            mkdir -p /tmp/tsan && cd /tmp/tsan
            rsync -a --delete /work/code/ /tmp/src/
            cmake /tmp/src \
                -DCMAKE_BUILD_TYPE=Debug \
                -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -fPIE" \
                -DCMAKE_C_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -fPIE" \
                -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread -pie" \
                -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=thread" >/dev/null 2>&1
            make -j$(nproc) >/dev/null 2>&1
            cp /tmp/tsan/Main/DetectBase /work/out/
            find /tmp/tsan -name "*.so" -exec cp {} /work/out/ \;
        ' > "${OUT_DIR}/tsan_build.log" 2>&1

    if [[ -x "${OUT_DIR}/tsan_pkg/DetectBase" ]]; then
        log_info "  TSan binary OK. 운영 정지 + ${TSAN_DURATION_SEC}s 실행 + SIGKILL..."
        docker-compose -f "${DOCKER_COMPOSE_FILE}" stop >/dev/null 2>&1

        docker run --rm --name detectbase_tsan \
            --privileged --network host \
            --device /dev/dri/renderD129:/dev/dri/renderD129 \
            -v "${SCRIPT_PATH}":/DetectBase:rw \
            -v "${AUDIT_LIBRKNN_HOST}":/usr/local/lib/librknnrt.so:ro \
            -v /etc/localtime:/etc/localtime:ro \
            -v "${AUDIT_FRAME_HOST}":/frame:rw \
            -v "${AUDIT_CROP_HOST}":/crop:rw \
            -e TSAN_OPTIONS="halt_on_error=0:second_deadlock_stack=1:history_size=4" \
            -e LD_LIBRARY_PATH="/DetectBase/logs/audit_${STAMP}/tsan_pkg:/usr/local/lib" \
            -e TZ=Asia/Seoul --shm-size=512m \
            "${AUDIT_ANALYSIS_IMG}" \
            timeout --signal=SIGKILL "${TSAN_DURATION_SEC}s" "/DetectBase/logs/audit_${STAMP}/tsan_pkg/DetectBase" \
            > "${OUT_DIR}/tsan_run.log" 2>&1 || true

        log_info "  운영 재시작..."
        docker-compose -f "${DOCKER_COMPOSE_FILE}" up -d >/dev/null 2>&1
    else
        log_error "  TSan binary 빌드 실패 → tsan_build.log 참조"
    fi
    TSAN_WARN=$(grep -cE "WARNING: ThreadSanitizer" "${OUT_DIR}/tsan_run.log" 2>/dev/null || echo 0)
    log_done "[tsan] WARNING ${TSAN_WARN}건"
}

# summary writer — 실행된 stage 만 표시
_audit_write_summary() {
    local OUT_DIR="$1"
    local STAGES="$2"   # space-separated: "cppcheck clangtidy asan tsan"
    {
        echo "===== Audit Summary $(date '+%Y-%m-%d %H:%M:%S KST') ====="
        echo "결과 위치: ${OUT_DIR}"
        echo "실행 stage: ${STAGES}"
        echo
        [[ " ${STAGES} " == *" cppcheck "*  ]] && echo "[cppcheck]   자체 코드 결함: ${CPPCHECK_COUNT:-?}"
        [[ " ${STAGES} " == *" clangtidy "* ]] && echo "[clang-tidy] warning:        ${TIDY_WARN:-?}"
        [[ " ${STAGES} " == *" asan "*      ]] && echo "[asan/ubsan] ${ASAN_LEAK:-?}"
        [[ " ${STAGES} " == *" tsan "*      ]] && echo "[tsan]       WARNING:        ${TSAN_WARN:-?}"
        echo
        echo "각 raw log 는 ${OUT_DIR}/ 안 *.log 참조"
    } | tee "${OUT_DIR}/summary.txt"
}

# CLI dispatch
cmd_audit() {
    local mode="all"   # all | no-tsan | only
    local only_tool=""
    # 강도 모드 — _audit_run_asan_ubsan 가 환경변수로 참조.
    #   AUDIT_STRICT=0 (light, default) → ASan 60분
    #   AUDIT_STRICT=1 (--strict)       → ASan 240분
    export AUDIT_STRICT="${AUDIT_STRICT:-0}"

    while [[ $# -gt 0 ]]; do
        case "$1" in
            --with-tsan) mode="all" ;;          # backward compat
            --no-tsan)   mode="no-tsan" ;;
            --light)     AUDIT_STRICT=0 ;;      # ASan 60분 (default)
            --strict)    AUDIT_STRICT=1 ;;      # ASan 240분 (master merge gate)
            --only)
                mode="only"
                only_tool="$2"
                shift
                ;;
            *)
                log_error "Unknown audit option: $1"
                log_info  "Usage: ./detectbase.sh audit [--light|--strict] [--with-tsan|--no-tsan|--only <cppcheck|clang-tidy|asan|ubsan|tsan>]"
                return 1
                ;;
        esac
        shift
    done

    local STAMP="$(date +%Y%m%d_%H%M%S)"
    local OUT_DIR="${SCRIPT_PATH}/logs/audit_${STAMP}"
    mkdir -p "${OUT_DIR}"

    _audit_set_paths
    _audit_ensure_analysis_image

    local _strength="light"
    [[ "${AUDIT_STRICT}" == "1" ]] && _strength="strict"
    log_info "DetectBase audit 시작 — mode=${mode}${only_tool:+ tool=${only_tool}} strength=${_strength}"
    log_info "  결과: ${OUT_DIR}"

    local STAGES_DONE=""
    case "${mode}" in
        all)
            _audit_run_cppcheck    "${OUT_DIR}";          STAGES_DONE+="cppcheck "
            _audit_run_clangtidy   "${OUT_DIR}";          STAGES_DONE+="clangtidy "
            _audit_run_asan_ubsan  "${OUT_DIR}" "${STAMP}"; STAGES_DONE+="asan "
            _audit_run_tsan        "${OUT_DIR}" "${STAMP}"; STAGES_DONE+="tsan "
            ;;
        no-tsan)
            _audit_run_cppcheck    "${OUT_DIR}";          STAGES_DONE+="cppcheck "
            _audit_run_clangtidy   "${OUT_DIR}";          STAGES_DONE+="clangtidy "
            _audit_run_asan_ubsan  "${OUT_DIR}" "${STAMP}"; STAGES_DONE+="asan "
            ;;
        only)
            case "${only_tool}" in
                cppcheck)
                    _audit_run_cppcheck "${OUT_DIR}"; STAGES_DONE="cppcheck "
                    ;;
                clang-tidy|clangtidy)
                    _audit_run_clangtidy "${OUT_DIR}"; STAGES_DONE="clangtidy "
                    ;;
                asan|ubsan)
                    # asan/ubsan 은 같은 빌드 (alias)
                    _audit_run_asan_ubsan "${OUT_DIR}" "${STAMP}"; STAGES_DONE="asan "
                    ;;
                tsan)
                    _audit_run_tsan "${OUT_DIR}" "${STAMP}"; STAGES_DONE="tsan "
                    ;;
                *)
                    log_error "Unknown --only tool: ${only_tool} (cppcheck|clang-tidy|asan|ubsan|tsan)"
                    return 1
                    ;;
            esac
            ;;
    esac

    _audit_write_summary "${OUT_DIR}" "${STAGES_DONE}"
    log_done "Audit 완료 — ${OUT_DIR}"
}

cmd_help() {
    cat <<EOF
DetectBase 서비스 통합 관리 스크립트

사용법: $0 <command>

명령:
  build     Docker 이미지 빌드 (docker-compose build) + init 자동 실행
  init      proto 재생성 + 의존성 점검 (build 후 자동, 또는 proto 수정 시 수동)
  compile   컨테이너 내 C++ 소스 컴파일 (BuildScript.sh)
  start     서비스 시작 (로그 백업 + docker-compose up -d + tail)
  stop      서비스 정상 종료 (graceful shutdown + docker-compose down)
  restart   정지 후 재시작
  logs      서비스 로그 follow (docker logs -f)
  prune     사용 안 하는 도커 리소스 정리 (docker system prune -a, 확인 프롬프트)
  all       build + init + compile + start (전체 파이프라인)
  audit     정적/동적 분석 도구 (cppcheck + clang-tidy + ASan/UBSan + TSan).
            기본 = 전체 실행 (light: ASan 60m + TSan 60m). 옵션으로 부분 실행 가능:
              --light                  ASan 60분 + TSan 60분 (default, develop/내부 검증)
              --strict                 ASan 240분 + TSan 60분 (master merge gate)
              --no-tsan                cppcheck + clang-tidy + ASan/UBSan (TSan 제외)
              --with-tsan              = 전체 (backward compat)
              --only cppcheck          cppcheck 만 (정적)
              --only clang-tidy        clang-tidy 만 (정적)
              --only asan|ubsan        ASan+UBSan 만 (동적, 운영 정지)
              --only tsan              TSan 만 (동적, 운영 정지)
            env override: ASAN_DURATION_MIN, TSAN_DURATION_SEC (강도 모드보다 우선)
            결과: logs/audit_<timestamp>/
  help      이 도움말 출력

예시:
  $0 build       # 도커 이미지만 빌드
  $0 init        # proto 재생성 + 의존성 점검
  $0 compile     # C++ 소스만 빌드
  $0 start       # 서비스 시작
  $0 stop        # 서비스 정지
  $0 restart     # 재시작
  $0 logs        # 로그 보기
  $0 prune       # 도커 청소
  $0 all         # 처음부터 끝까지

흐름 가이드:
  처음 셋업      : $0 all
  Dockerfile 수정: $0 build && $0 compile && $0 restart    (build → init 자동)
  proto 수정     : $0 init && $0 compile && $0 restart
  C++ 코드만 수정: $0 compile && $0 restart
EOF
}

# -----------------------------------------------------------------------------
# Main
# -----------------------------------------------------------------------------
case "$1" in
    build)    cmd_build ;;
    init)     cmd_init ;;
    compile)  cmd_compile ;;
    start)    cmd_start ;;
    stop)     cmd_stop ;;
    restart)  cmd_restart ;;
    logs)     cmd_logs ;;
    prune)    cmd_prune ;;
    all)      cmd_all ;;
    audit)    shift; cmd_audit "$@" ;;
    help|"")  cmd_help ;;
    *)
        log_error "알 수 없는 명령: $1"
        echo
        cmd_help
        exit 1
        ;;
esac
