#!/bin/bash

# ==============================================================================
#  [DetectBase] Build Automation Script
# ==============================================================================

# ------------------------------------------------------------------------------
# [1] 설정 및 상수 정의
# ------------------------------------------------------------------------------
USE_CPM_DEPENDENCY_CACHE="ON"

# 경로 설정
BUILD_ROOT_PATH="/DetectBase/bin"
SOURCE_CODE_ORIGIN_PATH="/DetectBase/code"
SOURCE_CODE_COPY_PATH="${BUILD_ROOT_PATH}/.src"
BUILD_OUTPUT_PATH="${BUILD_ROOT_PATH}/Build"
INSTALL_PATH="${BUILD_ROOT_PATH}/.install"
DEPENDENCY_CACHE_DIRS="/DetectBase/code/.tool/_deps"

# 병렬 빌드 코어 수
PARALLEL_BUILD_JOBS=$(nproc)

# ------------------------------------------------------------------------------
# [2] 시각화(Colors) 및 유틸리티 함수
# ------------------------------------------------------------------------------
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

START_TIME=$(date +%s)

log_header() {
    echo -e "\n${CYAN}======================================================================${NC}"
    echo -e "${CYAN}:: $1${NC}"
    echo -e "${CYAN}======================================================================${NC}"
}

log_info() {
    echo -e "${BLUE}[#] $1${NC}"
}

log_success() {
    echo -e "${GREEN}[v] $1${NC}"
}

log_error() {
    echo -e "${RED}[x] ERROR: $1${NC}"
}

# 에러 핸들러 (Trap에서 호출)
cleanup_on_error() {
    log_error "Build failed or interrupted. Cleaning up..."
    rm -rf "${SOURCE_CODE_COPY_PATH}"
    exit 1
}

# 스크립트 에러(ERR)나 인터럽트(SIGINT) 발생 시 cleanup_on_error 실행
trap cleanup_on_error ERR SIGINT

# exit on first error
set -e

# ------------------------------------------------------------------------------
# [3] 메인 빌드 프로세스
# ------------------------------------------------------------------------------

# 1. 초기화
log_header "Step 1: Initialize Build Directories"
log_info "Target: ${BUILD_OUTPUT_PATH}"

rm -rf "${BUILD_OUTPUT_PATH}" "${SOURCE_CODE_COPY_PATH}"
mkdir -p "${BUILD_OUTPUT_PATH}" "${SOURCE_CODE_COPY_PATH}"
log_success "Directories initialized."

# 2. 소스 코드 복사
log_header "Step 2: Copy Source Code"
log_info "Copying from ${SOURCE_CODE_ORIGIN_PATH} to ${SOURCE_CODE_COPY_PATH}..."

# 불필요한 파일(.git, .vscode, build 등) 제외하여 속도 향상
rsync -a --delete \
    --exclude '.git' \
    --exclude '.vscode' \
    --exclude 'build' \
    --exclude '.tool' \
    "${SOURCE_CODE_ORIGIN_PATH}/" "${SOURCE_CODE_COPY_PATH}/"

log_success "Source code copied."

# 3. CPM 캐시 복사
if [[ "${USE_CPM_DEPENDENCY_CACHE}" == "ON" ]] && [[ -e "${DEPENDENCY_CACHE_DIRS}" ]]; then
    log_info "Restoring dependency cache..."
    cp -a "${DEPENDENCY_CACHE_DIRS}" "${BUILD_OUTPUT_PATH}"
fi

# 4. CMake 설정 (Configure)
log_header "Step 3: Configure CMake"

CMAKE_OPTIONS=(
    "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE:-Release}"
    "-DCMAKE_INSTALL_PREFIX=${INSTALL_PATH}"
    "-S" "${SOURCE_CODE_COPY_PATH}"
    "-B" "${BUILD_OUTPUT_PATH}"
)

cmake "${CMAKE_OPTIONS[@]}"

# 5. 빌드 실행 (Build)
log_header "Step 4: Build (Jobs: ${PARALLEL_BUILD_JOBS})"

cd "${BUILD_OUTPUT_PATH}"

# gRPC 플러그인 실행을 위한 라이브러리 경로 주입 (핵심)
export LD_LIBRARY_PATH="/usr/local/lib:${LD_LIBRARY_PATH}"

cmake --build "${BUILD_OUTPUT_PATH}" --parallel "${PARALLEL_BUILD_JOBS}"
log_success "Build phase completed."

# 6. 정리 (Cleanup)
log_header "Step 5: Cleanup"
log_info "Removing temporary source copy..."
rm -rf "${SOURCE_CODE_COPY_PATH}"
log_success "Cleanup finished."

# 7. 버전 정보 출력
VERSION_FILE="${BUILD_OUTPUT_PATH}/VERSION"
if [[ -f "${VERSION_FILE}" ]]; then
    echo -e "${YELLOW}"
    cat "${VERSION_FILE}"
    echo -en "${NC}"
else
    log_info "VERSION file not found."
fi

# 8. 종료 및 시간 측정
END_TIME=$(date +%s)
DURATION=$((END_TIME - START_TIME))

log_header "Build Summary"
log_success "Build script completed successfully!"
log_success "Total Build Time: $(($DURATION / 60))m $(($DURATION % 60))s"
echo -e ""
