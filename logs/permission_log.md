# Claude Code Bash 권한 흐름 기록 (AUTO / APPROVED / DENIED)

> 자동 생성: `scripts/permission_log.py` (직접 편집 X — 다시 실행하면 덮어씀).
> 갱신 시각: 2026-05-24 01:17:15
> 분석 대상: 1 개 transcript (latest)
> 분류 규칙:
> - **AUTO**     = `.claude/settings.json` allow 패턴 매치 (prompt 없음)
> - **APPROVED** = allow 미매치이나 정상 실행 (prompt 후 승인 추정)
> - **DENIED**   = tool_result 에 'has been denied' 마커

> 한계: shell-glob 단순 매칭이라 Claude Code 실제 정책과 미세 차이 가능.
> compound 명령(`a && b`)은 단일 패턴 미매치로 APPROVED 분류되곤 함. 추정치.

## 총괄

| 카테고리 | 총 호출 | 비중 |
|---|---:|---:|
| **AUTO**     | 12497     | 99.3% |
| **APPROVED** | 0 | 0.0% |
| **DENIED**   | 83   | 0.7% |
| **합계**     | 12580          | 100% |

### APPROVED (prompt 후 허가 추정) — 상위 12

```
```

### DENIED — 상위 12

```
    15  git ls-remote --tags https://github.com/jupp0r/prometheus-cp
     9  rmdir /home/claudedev/DetectBase/code/Management/worker/inte
     6  git ls-remote --tags https://github.com/airockchip/rknn-tool
     5  docker rm -f detectbase_service 2>&1 || true; sleep 2; ./det
     5  docker stop detectbase_service 2>&1 || true; docker rm detec
     5  echo "=== detectbase.sh stop 구현 ===" grep -A 15 "^stop_servi
     4  echo "=== TSan 컨테이너 강제 종료 ===" docker kill detectbase_tsan 2
     4  echo "=== YoloV5 ctor 실제 구현 (base ctor 호출 패턴) ===" sed -n '2
     3  awk '/void RtspDetectorUnit::ResponseThreadRunner/,/^ \}$/' 
     2  docker kill detectbase_tsan; docker rm -f detectbase_tsan
     2  docker rm -f detectbase_tsan 2>&1 echo "---" docker ps -a --
     2  sed -n '545,560p' Main/DETECTOR/src/RtspDetectorUnit.cpp 2>/
```

### AUTO (참고용) — 상위 12

```
   286  bash /home/claudedev/DetectBase/detectbase.sh compile 2>&1 |
   180  /home/claudedev/DetectBase/detectbase.sh compile 2>&1 | tee 
   154  ./detectbase.sh start 2>&1
   132  timeout 120 bash -c 'until [ "$(grep -c "DFPS" /home/clauded
   131  /home/claudedev/DetectBase/detectbase.sh compile 2>&1 | tail
   130  /home/claudedev/DetectBase/detectbase.sh start 2>&1 | tail -
   102  ./detectbase.sh compile 2>&1 | tee /home/claudedev/DetectBas
    84  grep -E "DFPS" /home/claudedev/DetectBase/logs/DetectBase.lo
    71  /home/claudedev/DetectBase/detectbase.sh stop 2>&1 | tail -3
    70  cat /tmp/claude-1001/-home-claudedev-DetectBase/a1a716c5-dc2
    70  ./detectbase.sh compile > /home/claudedev/DetectBase/logs/co
    60  /home/claudedev/DetectBase/detectbase.sh restart 2>&1 | tail
```

## 활용
- **APPROVED 가 잦은 read-only / idempotent 패턴** → `.claude/settings.json` allow 추가하면 prompt ↓.
- **DENIED** 가 잦은 deny-policy 매치(sed/rm/sudo/docker rm 등)는 의도 — allow 승격 금지.
- **AUTO** 는 이미 자동 — 참고용.

## 재실행
```bash
./scripts/permission_log.py           # 최신 session
./scripts/permission_log.py --all     # 모든 session 통합
./scripts/permission_log.py --print   # stdout 만
```
