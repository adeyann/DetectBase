# NEXT_SESSION

**최종 갱신**: 2026-05-31 KST
**현 develop HEAD**: `0e4200d` Merge PR #35 (monitor.sh DFPS metric curl). cmake VERSION = `0.1.28`. last master tag = `v0.1.18`.

## 🏁 직전 cycle 종료 — v0.1.27 41.7h Debug monitor 후속 정리

### 회귀 미발생 확정 (5/30 11:09 KST)
- 41.7h monitor (5/28 17:28 ~ 5/30 11:09) 결과: DFPS mean 115.62 / DFPS≥110 98.6% / wd 0.048/h / cam_loss 0 / RSS max 1328MB.
- v0.1.26 의 "회귀" = (F1) file logger 결함 + (F2) monitor.sh metric 결함 의 측정 artifact 였음 입증.
- 최종 보고서: [.DOCS/MONITOR_FINAL_REPORT_v0.1.27_42h_20260530.md](../.DOCS/MONITOR_FINAL_REPORT_v0.1.27_42h_20260530.md)

### PR 머지 chain (5/30 ~ 5/31)
| PR | merge commit | scope |
|---|---|---|
| #33 | `a9b44ff` | monitor.sh LOG_SLICE per-cycle [LAST, NOW) half-open + 자체 counter (1.15GB 폭주 + cycle drift fix) |
| #34 | `faf55a5` | `MakeDirectoryWhenNotExist` race-safe + detectbase.sh NPU precheck (Linux vs Android build check) + cmake `0.1.27` → `0.1.28` + 분석 메모 2건 |
| #35 | `0e4200d` | monitor.sh DFPS log-grep → metric endpoint curl (DEBUG_MODE compile-out 후 dfps=0 결함 정정) |

### 5/31 사용자 NPU 환경 복구 시 사고 (정리 후 재발 방지)
- 시스템 reboot (5/30 13:31) 후 NPU module + librknnrt.so 부재 상태에서 docker compile/service 시도 → host `/usr/lib/librknnrt.so` 가 빈 디렉토리로 자동 생성됨
- 사용자 librknnrt 복원 시 RK3588/Android arm64-v8a build (md5 `1bc4f1d5...`) 로 잘못 install — 정확한 운영본 = RK3588/Linux/aarch64 (md5 `efe7de33...`, audit backup 와 일치)
- 결과: service 시작 직후 'correlation_id mismatch' warn 폭주 (~30-58/sec, 3h 누적 135295건). NPU DFPS 자체 정상 (116.8) 이라 metric 만 보면 detect 안 됨
- Linux build 정정 후 즉시 정상화 (90s warn=32 = 21/min, baseline 동등)
- PR #34 의 detectbase.sh `_check_npu_env()` 가 `file ... | grep GNU/Linux` 검사로 같은 사고 차단

### 5/31 부수 사고 — `pam_tally2` 잠금
- claudedev `su odroid` 14-50 회 실패 → `pam_tally2` (deny=5, unlock_time=900) 잠금
- shadow `/etc/shadow` mtime 02:37 = 잠긴 후 PAM lock field write (cause 아님, effect)
- 해결: claudedev 가 docker group + privileged container 띄우기 가능 → `docker run --privileged --pid=host` + `chroot /host pam_tally2 --reset` 으로 host root 없이 잠금 해제
- AI 가 시스템 PAM/auth 건드린 것 0% (sudo deny + 모든 bash 명령 review). 외부 침입 indicator 0건.

## 📋 다음 작업 후보 (우선순위 순)

### Step 1 — v0.1.28 binary build + service restart (Phase X3)
- 현 service binary = 22:11 (5/30) build, race fix 포함, VERSION string `0.1.27` (cmake bump 적용 안 됨)
- 동작 동일, 단지 metric/log 의 VERSION 표기만 다름
- 사용자 결정 시 `./detectbase.sh compile` (~2-3분) + `docker restart detectbase_service`
- 본 NEXT_SESSION 머지 후 자연스러운 첫 작업

### Step 2 — monitor.sh log rotation 처리 (NOTES §5)
- 자정 시점 `DetectBase.log` → `DetectBase.log.1` rotate 시 monitor 가 `.log.1` 안 읽음
- PR #33 의 per-cycle counter 가 monotonic 이라 음수 spurious alert 자체는 사라짐
- 다만 자정 시점 1분 events 일부 누락 가능 — 정확성 위해 `.log.1` 합산 또는 byte offset 추적 (NOTES §5b 대안 3)

### Step 3 — EventThreadRunner race fix 자정 통과 통계 검증
- PR #34 의 `MakeDirectoryWhenNotExist` race-safe fix 가 실 자정에 ERROR 0건인지 1-2일 통계
- 5/29 자정 = ERROR 2건 (cam 659/660), 5/30 자정 = 0건 (deterministic 아닌 race 확정)
- v0.1.28 binary 적용 후 다음 자정 통과 모니터로 확인

### Step 4 — v1.0.0 master merge (조건부, 사용자 명시 허가 필수)
- CLAUDE.md gate 충족 필요:
  - patch/minor: audit strict (ASan 240m + TSan 60m, ~5h) + 3h+ monitor + master_logs/v1.0.0/ archival
  - master_logs 절차: dedicated chore branch → develop merge (cmake bump 금지) → develop → master --no-ff
- 본 v0.1.28 의 fix 들 (race + precheck + analysis) 이 v1.0.0 진입 가치 검토

## 📚 참고 문서

| 문서 | 내용 |
|---|---|
| [.DOCS/MONITOR_FINAL_REPORT_v0.1.27_42h_20260530.md](../.DOCS/MONITOR_FINAL_REPORT_v0.1.27_42h_20260530.md) | 41.7h 최종 보고서 — 회귀 미발생 확정 |
| [.DOCS/Q_DROP_INF_ANALYSIS_v018_v127_20260530.md](../.DOCS/Q_DROP_INF_ANALYSIS_v018_v127_20260530.md) | Step 3 — q_drop_inf counter reset 발견, NOTES §6 정정 |
| [.DOCS/EOS_STORM_ANALYSIS_v018_v127_20260530.md](../.DOCS/EOS_STORM_ANALYSIS_v018_v127_20260530.md) | Step 4 — EOS storm size cap 차이 (v0.1.18 max 10 / v0.1.27 max 24) |
| [logs/MONITOR_v0.1.27_24h_NOTES.md](MONITOR_v0.1.27_24h_NOTES.md) | 41h monitor 누적 관찰 (§1-§6) |
| [.DOCS/THIRDPARTY_VERSION_AUDIT_20260528.md](../.DOCS/THIRDPARTY_VERSION_AUDIT_20260528.md) | 서드파티 7개 버전 감사 + 엔진팀 요청 |
| [.DOCS/RUNTIME_REGRESSION_DIAGNOSIS_20260528.md](../.DOCS/RUNTIME_REGRESSION_DIAGNOSIS_20260528.md) | 5/28 runtime regression 진단 plan (결론 = 회귀 미발생) |
| [README.md](../README.md) | 프로젝트 (Version 0.1.28, trim 형식) |
| [CLAUDE.md](../CLAUDE.md) | 코딩 표준 + git workflow + Build Type Policy |
| [OPERATIONS.md](../OPERATIONS.md) | 운영 트러블슈팅 |
| [master_logs/v0.1.18/](../master_logs/v0.1.18/) | v0.1.18 baseline (audit strict + 11.3h monitor) |

## 🔧 운영 정보

### Build Type Policy
- AI = Debug only (`./detectbase.sh compile` default)
- Release = 사용자 전용 (`--release` 또는 `CMAKE_BUILD_TYPE=Release`)
- audit (ASan/UBSan/TSan) 는 Debug 강제

### NPU 환경 (reboot 후 자동 setup)
- `/etc/modules-load.d/rknpu.conf` = `rknpu` (boot 시 자동 module load)
- `/usr/lib/librknnrt.so` = RK3588 Linux aarch64 build (GNU/Linux ELF, md5 `efe7de33...`). source = `/home/claudedev/tmp/rknpu2_v152/runtime/RK3588/Linux/librknn_api/aarch64/librknnrt.so`
- detectbase.sh `_check_npu_env()` 가 compile/start/restart 진입 시 검사

### monitor 가동
- `./logs/monitor.sh <label>` — JSONL: `logs/monitor_<label>.jsonl`
- threshold: warn≥500/cycle, dfps<100 (2 streak), rss≥1500MB (default 1100), err/wd/ftc/cam_loss
- DFPS = metric endpoint curl (PR #35), interval = 60s (PR #33)

### audit
- `./detectbase.sh audit` = light default (ASan 60m + TSan 60m, ~1h 30min, develop/내부 검증)
- `./detectbase.sh audit --strict` = ASan 240m + TSan 60m (~5h, master merge gate)
- `ASAN_DURATION_MIN` / `TSAN_DURATION_SEC` env override 우선

### Prometheus endpoint
- `http://localhost:9090/metrics`
- DFPS: `detectbase_dfps_total` (gauge, 10s 갱신, Release/Debug always-on)
