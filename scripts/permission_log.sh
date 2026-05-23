#!/bin/bash
# scripts/permission_log.sh
# Claude Code 권한 거부(denied) Bash 명령을 세션 transcript 에서 추출해
# logs/permission_log.md 에 누적 정리.
#
# 사용:  ./scripts/permission_log.sh              # 최신 session 분석
#        ./scripts/permission_log.sh --all        # 프로젝트 모든 session
#        ./scripts/permission_log.sh --print      # markdown 만 stdout, 파일 안 씀
#
# 한계 (정직):
#  - DENIED 명령은 transcript 에 "Permission to use Bash with command X has been
#    denied" 마커가 명확히 남아 100% 추출 가능.
#  - APPROVED-after-prompt 는 별도 marker 없이 그냥 정상 실행으로 기록되어
#    자동 prompted 명령 자체와 구분 불가. 이 스크립트는 DENIED 만 추적.
#  - 즉 "사용자가 거부한 것" 의 종합 리스트. 거부가 잦은 패턴을 보고 안전한 것은
#    .claude/settings.json allow 에 추가하면 prompt 빈도 감소.

set -e
PROJECT_DIR="/home/claudedev/DetectBase"
TRANSCRIPT_DIR="/home/claudedev/.claude/projects/-home-claudedev-DetectBase"
OUT="${PROJECT_DIR}/logs/permission_log.md"

MODE="latest"
PRINT_ONLY=0
for a in "$@"; do
    case "$a" in
        --all)   MODE="all" ;;
        --print) PRINT_ONLY=1 ;;
    esac
done

# transcript 선택
if [[ "$MODE" == "latest" ]]; then
    FILES=$(ls -t "${TRANSCRIPT_DIR}"/*.jsonl 2>/dev/null | head -1)
else
    FILES=$(ls -t "${TRANSCRIPT_DIR}"/*.jsonl 2>/dev/null)
fi

if [[ -z "$FILES" ]]; then
    echo "[ERROR] transcript JSONL 미발견: ${TRANSCRIPT_DIR}/*.jsonl" >&2
    exit 1
fi

# DENIED 명령 추출 + 정규화 (앞 60자, 트리밍, 빈도 집계)
DENIED=$(grep -aohE "Permission to use Bash with command [^\"\\\\]{0,200}" $FILES 2>/dev/null \
    | awk -F"command " '{cmd=$2; sub(/ has been denied.*$/, "", cmd); print cmd}' \
    | awk 'NF{print}' \
    | sort | uniq -c | sort -rn)

TOTAL=$(echo "$DENIED" | wc -l)
SUM=$(echo "$DENIED" | awk '{s+=$1} END{print s+0}')

# markdown 생성
generate_md() {
    cat <<EOF
# Claude Code 권한 거부(denied) Bash 명령 기록

> 자동 생성: \`scripts/permission_log.sh\` (이 파일 직접 편집 X — 다시 실행하면 덮어씀).
> 갱신 시각: $(date '+%Y-%m-%d %H:%M:%S %Z')
> 분석 대상: $(echo "$FILES" | wc -l) 개 transcript ($MODE)

## 요약
- 고유 거부 패턴 수: **${TOTAL}**
- 총 거부 횟수: **${SUM}**
- 분석 한계: APPROVED-after-prompt 는 자동 허용과 구분 marker 가 없어 미포함.

## 거부 패턴 (빈도 내림차순)

\`\`\`
EOF
    echo "$DENIED"
    cat <<EOF
\`\`\`

## 활용 방법

- **안전(read-only / idempotent)한 패턴이 빈번**히 거부되면 \`.claude/settings.json\` \`allow\` 에 추가 → prompt 빈도 감소.
- **deny 정책 (sed / rm / rmdir / docker rm / sudo 등) 매치**는 의도적 거부 — allow 추가 금지.
- 외부 git URL / 파괴적 명령 / 휴지통 복원 등은 prompt 유지가 합리적.

## 정기 갱신

\`\`\`bash
./scripts/permission_log.sh           # 최신 session 만
./scripts/permission_log.sh --all     # 프로젝트 모든 session 통합
\`\`\`
EOF
}

if [[ "$PRINT_ONLY" -eq 1 ]]; then
    generate_md
else
    generate_md > "$OUT"
    echo "[DONE] ${OUT} 갱신 (${SUM} 회 / ${TOTAL} 종 거부)"
fi
