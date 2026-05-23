# Claude Code 권한 거부(denied) Bash 명령 기록

> 자동 생성: `scripts/permission_log.sh` (이 파일 직접 편집 X — 다시 실행하면 덮어씀).
> 갱신 시각: 2026-05-23 21:48:01 KST
> 분석 대상: 1 개 transcript (latest)

## 요약
- 고유 거부 패턴 수: **23**
- 총 거부 횟수: **163**
- 분석 한계: APPROVED-after-prompt 는 자동 허용과 구분 marker 가 없어 미포함.

## 거부 패턴 (빈도 내림차순)

```
     24 echo 
     20 git ls-remote --tags https://github.com/jupp0r/prometheus-cpp.git
     18 rmdir /home/claudedev/DetectBase/code/Management/worker/interface 2>&1; echo 
     12 git ls-remote --tags https://github.com/airockchip/rknn-toolkit2.git 2>&1
     10 grep -i detectbase || echo 
     10 git ls-remote --tags https://github.com/jupp0r/prometheus-cpp.git 2>&1
     10 docker stop detectbase_service 2>&1 || true; docker rm detectbase_service 2>&1 || true; sleep 2; echo 
     10 docker rm -f detectbase_service 2>&1 || true; sleep 2; ./detectbase.sh start > logs/lifecycle_z2_taglock.log 2>&1 &
      8 head -10
      6 sed -n '180,400p'
      4 sed -n '545,560p' Main/DETECTOR/src/RtspDetectorUnit.cpp 2>/dev/null || awk 'NR>=545 && NR<=560' Main/DETECTOR/src/RtspDetectorUnit.cpp
      4 mv /home/claudedev/DetectBase/logs/.deleted_backup/* /home/claudedev/DetectBase/logs/.deleted_backup/.[^.]* /home/claudedev/DetectBase/.deleted_backup/ ; rmdir /home/claudedev/DetectBase/logs/.deleted
      4 docker rm -f detectbase_tsan 2>&1
      4 docker kill detectbase_tsan; docker rm -f detectbase_tsan
      4 [^
      2 sed 's/{
      2 sed 's/.*desc: //'
      2 head -5
      2 head
      2 chmod +x /home/claudedev/DetectBase/logs/stage_trace_measure.sh
      2 cd /home/claudedev/DetectBase
      2 X has been
      1 ...
```

## 활용 방법

- **안전(read-only / idempotent)한 패턴이 빈번**히 거부되면 `.claude/settings.json` `allow` 에 추가 → prompt 빈도 감소.
- **deny 정책 (sed / rm / rmdir / docker rm / sudo 등) 매치**는 의도적 거부 — allow 추가 금지.
- 외부 git URL / 파괴적 명령 / 휴지통 복원 등은 prompt 유지가 합리적.

## 정기 갱신

```bash
./scripts/permission_log.sh           # 최신 session 만
./scripts/permission_log.sh --all     # 프로젝트 모든 session 통합
```
