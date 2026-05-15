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
# audit — 자동화 도구 5종 (cppcheck + clang-tidy + ASan + UBSan + TSan) 실행
#  - --with-tsan : TSan 추가 (운영 정지 + smoke test 환경 자동 실행)
#  - 결과: logs/audit_<timestamp>/ (cppcheck.log / clangtidy.log / asan_run.log / tsan_run.log + summary.txt)
#  - ASan/TSan 실행 시 운영 detectbase_service 가 자동 graceful 정지/재시작됨
# -----------------------------------------------------------------------------
cmd_audit() {
    local with_tsan=false
    [[ "$1" == "--with-tsan" ]] && with_tsan=true

    local STAMP="$(date +%Y%m%d_%H%M%S)"
    local OUT_DIR="${SCRIPT_PATH}/logs/audit_${STAMP}"
    local ANALYSIS_IMG="detectbase:analysis"
    local LIBRKNN_HOST="${SCRIPT_PATH}/code/Engine/NPU/librknn_api/aarch64/librknnrt.so"
    local FRAME_HOST="${IMAGE_ROOT_PATH:-/hdd_ext/images}/frame"
    local CROP_HOST="${IMAGE_ROOT_PATH:-/hdd_ext/images}/crop"

    log_info "DetectBase 자동화 audit 시작 (5종 도구)"
    log_info "  도구: cppcheck + clang-tidy + ASan + UBSan$( ${with_tsan} && echo ' + TSan' )"
    log_info "  결과: ${OUT_DIR}"
    mkdir -p "${OUT_DIR}"

    # 1. 분석 이미지 확인 (없으면 빌드)
    if ! docker image inspect "${ANALYSIS_IMG}" >/dev/null 2>&1; then
        log_info "분석 이미지 ${ANALYSIS_IMG} 빌드 중..."
        docker build -f "${SCRIPT_PATH}/Dockerfile.analysis" -t "${ANALYSIS_IMG}" "${SCRIPT_PATH}"
    fi

    # ── [1/4] cppcheck ─────────────────────────────────────────────────────────
    log_info "[1/4] cppcheck (자체 코드, 외부 RTSP 제외)..."
    docker run --rm \
        -v "${SCRIPT_PATH}/code:/code:ro" \
        "${ANALYSIS_IMG}" \
        bash -c 'cppcheck --enable=warning,style,performance,portability \
            --suppress=missingIncludeSystem --inline-suppr --std=c++17 \
            --error-exitcode=0 -j 4 \
            /code/Main /code/Engine /code/Management /code/BasicLibs \
            /code/AbnormalActions /code/Tracker /code/VisionCommon /code/Protocol/GRPC 2>&1' \
        > "${OUT_DIR}/cppcheck.log"
    local CPPCHECK_COUNT
    CPPCHECK_COUNT=$(grep -cE "^/code/" "${OUT_DIR}/cppcheck.log" 2>/dev/null || echo 0)
    log_done "cppcheck: 자체 코드 ${CPPCHECK_COUNT}건 → cppcheck.log"

    # ── [2/4] clang-tidy ───────────────────────────────────────────────────────
    log_info "[2/4] clang-tidy (자체 cmake configure → 100% file 분석)..."
    docker run --rm \
        -v "${SCRIPT_PATH}/code:/work/code:ro" \
        "${ANALYSIS_IMG}" \
        bash -c '
            set -e
            # 자체 build 디렉토리 만들어서 fresh compile_commands.json 생성 (path 일치 보장).
            # cmake configure 만 (빌드 안 함, ~5-10s).
            rsync -a --delete /work/code/ /tmp/src/
            mkdir -p /tmp/tidy-build && cd /tmp/tidy-build
            cmake /tmp/src \
                -DCMAKE_BUILD_TYPE=Release \
                -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null 2>&1

            # 자체 코드 .cpp 파일 추출 (RTSP 외부 제외)
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
    local TIDY_WARN TIDY_ERR
    TIDY_WARN=$(grep -cE "warning:" "${OUT_DIR}/clangtidy.log" 2>/dev/null || echo 0)
    TIDY_ERR=$(grep -cE "clang-diagnostic-error" "${OUT_DIR}/clangtidy.log" 2>/dev/null || echo 0)
    log_done "clang-tidy: warning ${TIDY_WARN}건, diagnostic-error ${TIDY_ERR}건 → clangtidy.log"

    # ── [3/4] ASan + UBSan ─────────────────────────────────────────────────────
    log_info "[3/4] ASan + UBSan 빌드 + 실행 (~5-10분 + 운영 90초 정지)..."
    log_warn "  → 운영 컨테이너 ${CONTAINER_NAME} 가 graceful 정지됩니다 (audit 후 자동 재시작)"
    mkdir -p "${OUT_DIR}/asan_pkg"
    docker run --rm \
        -v "${SCRIPT_PATH}/code:/work/code:ro" \
        -v "${OUT_DIR}/asan_pkg:/work/out:rw" \
        "${ANALYSIS_IMG}" \
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
        log_info "  ASan binary 빌드 완료. 운영 정지 + 90초 실행..."
        docker-compose -f "${DOCKER_COMPOSE_FILE}" stop >/dev/null 2>&1

        docker run --rm --name detectbase_asan \
            --privileged --network host \
            --device /dev/dri/renderD129:/dev/dri/renderD129 \
            -v "${SCRIPT_PATH}":/DetectBase:rw \
            -v "${LIBRKNN_HOST}":/usr/local/lib/librknnrt.so:ro \
            -v /etc/localtime:/etc/localtime:ro \
            -v "${FRAME_HOST}":/frame:rw \
            -v "${CROP_HOST}":/crop:rw \
            -e ASAN_OPTIONS="halt_on_error=0:print_stats=1:detect_leaks=1:malloc_context_size=20" \
            -e UBSAN_OPTIONS="halt_on_error=0:print_stacktrace=1" \
            -e LD_LIBRARY_PATH="/DetectBase/logs/audit_${STAMP}/asan_pkg:/usr/local/lib" \
            -e TZ=Asia/Seoul --shm-size=512m \
            "${ANALYSIS_IMG}" \
            timeout --signal=SIGINT 90s "/DetectBase/logs/audit_${STAMP}/asan_pkg/DetectBase" \
            > "${OUT_DIR}/asan_run.log" 2>&1 || true

        log_info "  운영 재시작..."
        docker-compose -f "${DOCKER_COMPOSE_FILE}" up -d >/dev/null 2>&1
    else
        log_error "  ASan binary 빌드 실패 → asan_build.log 참조"
    fi
    local ASAN_LEAK
    ASAN_LEAK=$(grep -m1 "SUMMARY: AddressSanitizer" "${OUT_DIR}/asan_run.log" 2>/dev/null || echo "(no leak)")
    log_done "ASan/UBSan: ${ASAN_LEAK}"

    # ── [4/4] TSan (옵션) ──────────────────────────────────────────────────────
    if ${with_tsan}; then
        log_info "[4/4] TSan 빌드 + 실행 (~5-10분 + 운영 30초 정지)..."
        log_warn "  → TSan 100x 느림. 정상 운영 환경에서 packet drop hang 발생 정상 (race report 는 stderr 즉시 출력)"
        log_warn "  → 깊이 검증 시: NetworkSettings.json 의 카메라 1대 + fps_limit 1~2 로 임시 변경 권고 (.DOCS/REVIEW3/AUTOMATED_AUDIT.md §5.4 참조)"
        mkdir -p "${OUT_DIR}/tsan_pkg"
        docker run --rm \
            -v "${SCRIPT_PATH}/code:/work/code:ro" \
            -v "${OUT_DIR}/tsan_pkg:/work/out:rw" \
            "${ANALYSIS_IMG}" \
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
            log_info "  TSan binary 빌드 완료. 운영 정지 + 30초 실행 + SIGKILL..."
            docker-compose -f "${DOCKER_COMPOSE_FILE}" stop >/dev/null 2>&1

            docker run --rm --name detectbase_tsan \
                --privileged --network host \
                --device /dev/dri/renderD129:/dev/dri/renderD129 \
                -v "${SCRIPT_PATH}":/DetectBase:rw \
                -v "${LIBRKNN_HOST}":/usr/local/lib/librknnrt.so:ro \
                -v /etc/localtime:/etc/localtime:ro \
                -v "${FRAME_HOST}":/frame:rw \
                -v "${CROP_HOST}":/crop:rw \
                -e TSAN_OPTIONS="halt_on_error=0:second_deadlock_stack=1:history_size=4" \
                -e LD_LIBRARY_PATH="/DetectBase/logs/audit_${STAMP}/tsan_pkg:/usr/local/lib" \
                -e TZ=Asia/Seoul --shm-size=512m \
                "${ANALYSIS_IMG}" \
                timeout --signal=SIGKILL 30s "/DetectBase/logs/audit_${STAMP}/tsan_pkg/DetectBase" \
                > "${OUT_DIR}/tsan_run.log" 2>&1 || true

            log_info "  운영 재시작..."
            docker-compose -f "${DOCKER_COMPOSE_FILE}" up -d >/dev/null 2>&1
        else
            log_error "  TSan binary 빌드 실패 → tsan_build.log 참조"
        fi
        local TSAN_WARN
        TSAN_WARN=$(grep -cE "WARNING: ThreadSanitizer" "${OUT_DIR}/tsan_run.log" 2>/dev/null || echo 0)
        log_done "TSan: WARNING ${TSAN_WARN}건"
    fi

    # ── summary ────────────────────────────────────────────────────────────────
    {
        echo "===== Audit Summary $(date '+%Y-%m-%d %H:%M:%S KST') ====="
        echo "결과 위치: ${OUT_DIR}"
        echo
        echo "[1] cppcheck (자체 코드 결함):                ${CPPCHECK_COUNT:-?}"
        echo "[2] clang-tidy (warning):                     ${TIDY_WARN:-?}"
        echo "[3] ASan/UBSan:                               ${ASAN_LEAK:-?}"
        if ${with_tsan}; then
            echo "[4] TSan (WARNING):                       ${TSAN_WARN:-?}"
        else
            echo "[4] TSan:                                 (--with-tsan 옵션으로 추가 가능)"
        fi
        echo
        echo "각 raw log 는 ${OUT_DIR}/ 안 *.log 참조"
    } | tee "${OUT_DIR}/summary.txt"

    log_done "Audit 완료"
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
  audit     자동화 audit (cppcheck + clang-tidy + ASan + UBSan, 운영 90초 정지)
            --with-tsan 옵션 추가 시 TSan 도 실행 (운영 30초 정지)
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
    audit)    cmd_audit "$2" ;;
    help|"")  cmd_help ;;
    *)
        log_error "알 수 없는 명령: $1"
        echo
        cmd_help
        exit 1
        ;;
esac
