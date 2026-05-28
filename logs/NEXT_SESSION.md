# NEXT_SESSION

**최종 갱신**: 2026-05-28 17:30 KST
**현 develop HEAD**: `0a886dd` Merge pull request #32 (v0.1.27 NPU batch fix). cmake VERSION = `0.1.27`. last master tag = `v0.1.18`.
**작업 중 branch**: `experiment/runtime-regression-investigation` (HEAD `e0edcbd`) — runtime regression 진단 + 도구/infra fix + 정책 신규. develop 미머지.

## 🚨 진행 중 (compact-safe 진입점)

**[5/28] v0.1.27 develop 머지 직후 runtime regression 발견 + 진단 진행**:

### 발견 (0.1.26 Release binary, 1h 운영) — `master_logs/v0.1.18/monitor_v018_teardown_fix.jsonl.gz` baseline 대비
| 지표 | v0.1.18 baseline | 0.1.26 Release 1h | 회귀 |
|---|---|---|---|
| DFPS mean | 115.6 | **99.4** | **-15%** |
| reset/h | 39 | **64** | **+64%** |
| wd | 0/h boot 외 | **3/h** (3 cam 28s window 동시) | **0→3** |
| engine_input_q_drop | 0~수십 | **440/h** | NPU backpressure |
| correlation_id mismatch (CAM 661) | 드물게 | 매분 다수 | storm window |

### 진단 중 발견된 도구/infra 결함 (이미 fix 완료, commit `e0edcbd`)
- **(F1) Main.cpp InitLogger 분기 결함** — Debug 빌드 시 Console logger only → DetectBase.log 0 bytes → monitor.sh 의 file grep (reset/wd/eos/err/warn) 모두 0 표기. **빌드 type 무관 File logger always-on** 으로 변경.
- **(F2) monitor.sh DFPS extract 결함** — log 기반 (`grep TotalDFPS`) 가 v0.1.20+ DEBUG_MODE compile-out 후 Release 빌드에서 emit 안 됨 → JSONL DFPS=0. **metric endpoint 기반 (`curl detectbase_dfps_total`) 으로 변경** (Release/Debug 모두 always-on).
- **(F3) detectbase.sh compile default Release** — 변경 → **default Debug** + `--release` flag (사용자 전용).

### 신규 정책 (commit `e0edcbd` 안 3 doc + memory)
- **AI 는 오직 Debug 빌드만**. `./detectbase.sh compile` (no flag, default Debug) / `--debug` (명시).
- **Release 빌드는 사용자 전용**. AI 가 `--release` 또는 `CMAKE_BUILD_TYPE=Release` 사용 금지.
- 이유: 진단 capability 우선 — Debug 활성 = `[DFPS]` 10초 line + DBG_PROF 100-cycle dump + jemalloc mallctl + GstRtsp 6 debug + 3 jitter gauge + DETECTOR/SioHandler trace 모두 emit. Release 빌드 운영은 사용자 의도적 deploy.

### 현재 진행 — Debug binary monitor 24h
- **Debug binary**: 17:25 KST 빌드 (file logger fix 포함). service Started 17:25 KST.
- **DetectBase.log**: 399K (file logger fix 검증 통과, 이전 0 bytes)
- **monitor**: `monitor_v0.1.27_debug_24h.jsonl` (PID 1709104, 17:28 KST 시작, 1분 cycle, 24h ETA ~ 다음날 17:28 KST)
- **첫 cycle 정상**: DFPS 116.1 (cam ceiling 동등), cam=4/4, reset=0, wd=0, err=0
- **watcher**: Monitor task `bqnxomf7v` 가동 (wd/err/DFPS<100 alert + 1h tick)

### 24h monitor 결과 해석 기준
| 결과 | 판단 |
|---|---|
| DFPS≥110 mean / wd ~0/h / reset 40~50/h | **회귀 미발생** → 0.1.26 Release 1h DFPS 99.4 는 file logger 결함으로 (F1) instrument 부재 + monitor.sh metric=0 도구 결함 (F2) 의 영향 가능. v1.0.0 진입 검토 가능 |
| DFPS<100 sustained / wd>3/h / reset>50/h / cam_loss>0 | **회귀 확정** → 코드 결함 (UAF fix / GMainContext / SafeQueue MO-1 중 하나). Step 2 (UAF/GMainContext revert control test) 진행 |
| 둘 사이 모호 | Step 3 (외부 요인 점검) + Step 5 (v0.1.18 24h control) 진행 결정 |

## 📋 진단 plan (`.DOCS/RUNTIME_REGRESSION_DIAGNOSIS_20260528.md` 참조)

### Step 1 (진행 중) — Debug 0.1.27 24h monitor
- file logger + metric monitor + 1분 cycle 환경
- DFPS / reset / wd / cam_loss 추세 + Debug log 의 root cause 단서

### Step 2 (조건부, Step 1 회귀 시) — UAF/GMainContext revert control test
- 2a. UAF fix 직전 commit `0f9ae2c` (GMainContext only) 빌드 + 30-60분 monitor
- 2b. master tag `v0.1.18` (모든 변경 전) 빌드 + 30-60분 monitor

### Step 3 (병행) — 외부 요인 점검
- cam server RTCP / EOS cycle 변화
- 네트워크 ping/loss

### Step 5 (조건부, Step 1-3 모호 시) — happytimesoft 또는 v0.1.18 24h
- `efeea7a` (Initial, happytimesoft only) — GStreamer 자체 의심 검증
- v0.1.18 master tag — 더 가까운 baseline
- 사용자 명시 "최후의 최후 수단" 으로 두기로 함

## 💾 commit `e0edcbd` 안 변경

```
chore(diag-infra): Debug-only build policy + file logger always + monitor.sh fix + 진단 plan
 8 files changed, 224 insertions(+), 19 deletions(-)
 create mode 100644 .DOCS/RUNTIME_REGRESSION_DIAGNOSIS_20260528.md
 M CLAUDE.md                                      (Build Type Policy)
 M .claude/skills/coding-guidelines.md            (Build Type Policy mirror)
 M code/.tool/BuildScript.sh                      (CMAKE_BUILD_TYPE env override)
 M code/Main/BASE/src/Main.cpp                    (File logger always-on)
 M detectbase.sh                                  (compile default Debug + flags)
 M logs/monitor.sh                                (DFPS extract metric 기반)
 M OPERATIONS.md                                  (§1 cheat sheet compile default)
```

memory 신규 (repo 밖, 자동 적용됨): `feedback_build_type.md` — AI Debug only / Release user only.

## 📋 남은 작업 (조건부)

### Step 1 회귀 미발생 시
- v1.0.0 release 진행 결정 — audit `--strict` (Debug 자동 강제, 5h) + 3h+ monitor + master_logs/v1.0.0/ archival + master 머지
- experiment branch 의 도구/infra fix + 정책 변경을 별도 PR 로 develop 머지 (회귀 발생 무관 이미 의미 있는 fix 라 머지 권장)

### Step 1 회귀 확정 시
- Step 2 control test → revert / fix 결정
- v1.0.0 release 보류

### 영구 pending (시간 의존)
- monitor.sh threshold tuning (1-2주 운영 데이터 누적 후)
- cam_loss fix path 검증 (v0.1.18 unref-skip) — 자연 영구 stuck 재발 시 (5/28 짧은 stuck 3건은 NULL transition 5s 안 성공해 fix path 미발화)
- SafeThread → ThreadPool (8~16 cam scale-up 시)
- NPU batch_size 본격 검증 (6+ cam scale-up 시 — input 측 fix 는 v0.1.27 에서 완료)

### v1.0.0 후
- v2.0.0 multi-engine Phase 1-5
- GStreamer 1.24+ upgrade

## 🔄 1분 cycle 정합성 (사용자 강조)

`monitor.sh` 의 `INTERVAL_SEC` default = 60 (1분 cycle). 현 monitor 정확히 그대로 작동. 운영 영향: read-only ops, < 0.001% CPU. 1분 cycle 결정적.

---

## 운영 metric / monitor (v0.1.18 정보 + 신규 fix 적용)

### Prometheus endpoint
- `http://localhost:9090/metrics`
- DFPS: `detectbase_dfps_total` (gauge, 10s 갱신) — Release/Debug 모두 always-on. monitor.sh 가 5/28 부터 이 metric 직접 추출 (이전 log 기반 결함 fix).

### canonical monitor
- `./logs/monitor.sh <label>` — JSONL, 70+ fields, 1분 cycle. threshold alerts 7종 (storm/err/dfps_low/memory/wd/ftc/cam_loss) + warmup 4 cycle
- env override: `ALERT_WARN_DELTA_PER_CYCLE` / `ALERT_DFPS_LOW_THRESHOLD` / `ALERT_DFPS_LOW_STREAK` / `ALERT_RSS_MB_THRESHOLD` / `ALERT_WARMUP_CYCLES`
- **현 가동**: `v0.1.27_debug_24h` (PID 1709104, 17:28 KST 시작)

### audit 강도 모드 (0.1.26 도입)
- `./detectbase.sh audit` (no flag) = light default: ASan 60m + TSan 60m (~1h 30min, develop/내부 검증)
- `./detectbase.sh audit --strict` = ASan 240m + TSan 60m (~5h, master merge gate)

### Build Type Policy (0.1.28 신규, 2026-05-28)
- AI = Debug only (`./detectbase.sh compile` default 가 Debug)
- Release = 사용자 전용 (`--release` 또는 `CMAKE_BUILD_TYPE=Release`)
- audit (ASan/UBSan/TSan) 는 Debug 강제 (기존 정책)

### single-instance lock
- `/DetectBase/logs/.detectbase.lock` — flock(2) advisory lock

### DEBUG_MODE compile-out (v0.1.20+ PR #28)
Debug 빌드 (`-DCMAKE_BUILD_TYPE=Debug` 또는 audit 5종) 에서만 활성. AI 는 Debug 만 사용 (5/28 정책).

| 영역 | 위치 |
|---|---|
| `DBG_PROF(...)` 매크로 + InfProf/RspProf/EvtProf 100-cycle dump | `code/Main/DETECTOR/src/RtspDetectorUnit.cpp` |
| jemalloc mallctl 5회 + `/proc/self/maps` 파싱 | RspProf 안 |
| event_detected MLOG_INFO | per event |
| [DFPS] 10초 line | `code/Management/worker/src/InferenceCounter.cpp:139` |
| GstRtspReceiver debug/stuck trace metric 6개 | BUS_MSG / RESET_ATTEMPT / LAST_FRAME_AGE_SEC / DEPAY_BUFFER / DECODED / RTP_IN |
| GstRtspReceiver jitterbuffer 3개 gauge | JB_PUSHED / JB_LOST / JB_RTX_COUNT |
| DETECTOR [GRPC RECV] + GrpcEventClient [GRPC OK] | per call |
| SioHandler ack response | per ack |

### DEBUG VIRTUAL LINES (v0.1.19+)
- `ServerSetting.debug_virtual_lines_enabled` toggle

---

## 참고 문서

| 문서 | 내용 |
|------|------|
| [README.md](../README.md) | 프로젝트 전체 (Version 0.1.27) |
| [CLAUDE.md](../CLAUDE.md) | 코딩 표준 + git workflow + Build Type Policy (5/28 신규) |
| [OPERATIONS.md](../OPERATIONS.md) | 운영 트러블슈팅 + NPU batch 변경 절차 (§10) + alert escalation 옵션 (§8) |
| [.DOCS/RUNTIME_REGRESSION_DIAGNOSIS_20260528.md](../.DOCS/RUNTIME_REGRESSION_DIAGNOSIS_20260528.md) | **5/28 runtime regression 진단 plan + 회귀 증거 + 원인 후보** |
| [.DOCS/MULTI_ENGINE_DESIGN_v2_0_0.md](../.DOCS/MULTI_ENGINE_DESIGN_v2_0_0.md) | v2.0.0 |
| [.DOCS/UNSTABLE_NETWORK_BEHAVIOR_20260526.md](../.DOCS/UNSTABLE_NETWORK_BEHAVIOR_20260526.md) | cam_loss / GStreamer stuck (escalation playbook) |
| [.DOCS/SAFEQUEUE_RACE_REVIEW_20260527.md](../.DOCS/SAFEQUEUE_RACE_REVIEW_20260527.md) | SafeQueue race (자체 race 0건) |
| [master_logs/v0.1.18/](../master_logs/v0.1.18/) | v0.1.18 baseline (audit strict + 11.3h monitor) |
