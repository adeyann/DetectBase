# Runtime Regression Diagnosis — 2026-05-28

**상태**: 진단 진행 중. v0.1.27 develop 머지 후 1h light monitor 에서 회귀 발견.
**branch**: `experiment/runtime-regression-investigation` (version-free, 회귀 진단 전용)
**관련 commit**: 4172ac8 (UAF fix), 0f9ae2c (GMainContext per-instance), 7591105 (NPU batch fix)

---

## 1. 발견된 회귀 (5개) + 별개 도구 결함 (1개)

### v0.1.18 master baseline (master_logs/v0.1.18/monitor_v018_teardown_fix.jsonl.gz, 11.3h 가동) ↔ 5/28 1h light monitor 비교

| # | 지표 | v0.1.18 (11.3h, mean/sum/h) | 현 0.1.26 binary (1h) | 회귀 |
|---|---|---|---|---|
| 1 | DFPS | 115.6 mean (≥110: 98.8%) | **99.4** | **-15%** |
| 2 | reset (per hour) | 39 / h (~0.65/min) | **64 / h (~1.07/min)** | **+64%** |
| 3 | wd (frame-age watchdog) | 0/h (boot only) | **3 / h** (06:28~29 동시 3 cam) | **0 → 3** |
| 4 | correlation_id mismatch (CAM 661) | 드물게 (storm 시점만) | 매분 다수 | storm window 안 |
| 5 | engine_input_q_drop | 0~수십 | **440 / h** | NPU backpressure 누적 |
| 별개 | monitor.sh JSONL `pipeline.dfps` 표기 | (정확) | **= 0 (도구 결함)** | 실제 metric 99.4. v1.0.0 후속 fix |

`detectbase_*` 메트릭 직접 curl 확인:
```
detectbase_dfps_total = 99.4
detectbase_camera_count{state="active"} = 4
detectbase_camera_count{state="registered"} = 4
detectbase_errors_total{type="engine_input_q_drop"} = 440
detectbase_gst_rtsp_reconnect_total = 64
detectbase_gst_rtsp_rtcp_timeout_total{cam_id=658} = 36
detectbase_gst_rtsp_rtcp_timeout_total{cam_id=659} = 34
detectbase_gst_rtsp_rtcp_timeout_total{cam_id=660} = 36
detectbase_gst_rtsp_rtcp_timeout_total{cam_id=661} = 36
```

### wd 3건 상세 (06:28~06:29, 28초 window 동시)
```
06:28:42  GstRtspClient[659] frame-age watchdog: 12s 무프레임 → 강제 reset
06:28:43  GstRtspClient[658] frame-age watchdog: 15s 무프레임 → 강제 reset
06:29:11  GstRtspClient[661] frame-age watchdog: 14s 무프레임 → 강제 reset
```
3 cam 동시 stuck = 동일 root cause (외부 일시 단절 또는 우리 코드 결함).

### v0.1.18 unref-skip fix path 발화 검증 (todo #10)
- log grep `TeardownPipeline.*pipeline NULL transition 실패|gst_object_unref 건너` = **0건**
- 의미: stuck 가 짧아 정상 reset path (NULL transition 5s 안 성공) 으로 회복. 이번 stuck 은 v0.1.18 fix path 의 진짜 검증 case 아님 (영구 stuck 필요).
- todo #10 은 여전히 pending — 다음 영구 stuck 발생 시 검증.

---

## 2. 원인 후보 (5개)

| 후보 | 변경 commit | 의심도 | 진단 방법 |
|---|---|---|---|
| **A. UAF fix** (`g_source_remove` → `g_source_destroy`) | `4172ac8` (5/28) | **높음** — pipeline 종료 timing 변경, RTSP/RTCP 영향 가능 | revert + 1h monitor 비교 |
| **B. GMainContext per-instance** (default global → `ctx_`) | `0f9ae2c` (5/27) | **높음** — bus watch / jitter timer scheduling 변화 | revert + 1h monitor 비교 |
| **C. NPU batch fix** (input size / memcpy offset) | `7591105` (5/28) | **매우 낮음** — batch=1 영향 0 수학적 증명. 단순 verify 만 | 새 binary restart + monitor (Step 1 동시 검증) |
| **D. 외부 요인** (네트워크 / cam server / RTCP 패턴 변화) | — | **중** — 3 cam 28초 window 동시 stuck 가 동시 외부 원인 가능 | cam server 측 RTCP 송신 빈도 / 네트워크 ping/loss 점검 |
| **E. wd false positive** (12s 임계가 너무 짧음) | — (legacy) | **중** — 5/27 baseline 0건이지만 우리 환경에서 12s 무프레임 자연 발생 가능 | `WATCHDOG_STALE_SEC = 12 → 300` (5분) 진단 |

---

## 3. 진단 단계 (4-step plan)

### Step 1: 새 binary (0.1.27) 동일 회귀 검증 (30-60분, 즉시)
- compile + restart 후 빠른 monitor (~30-60분)
- 비교: DFPS / reset / wd / engine_input_q_drop 추세
- **결과 분기**:
  - 동일 회귀 → 코드 결함 확정 (A or B). Step 2 진행
  - 회귀 사라짐 → 0.1.26 cycle 일시 anomaly 또는 monitor.sh 결함. 운영 진행 가능
  - C (NPU batch fix) 는 동시 검증 — batch=1 영향 0 가 실측 확인

### Step 2: Control test (시점별 git checkout + 빌드 + 30-60분 monitor)
회귀 위치 좁히기:
- **2a. UAF fix 직전 commit `0f9ae2c`** — GMainContext only, UAF fix 없음
  - 회귀 유지 → (B) GMainContext 가 원인
  - 회귀 사라짐 → (A) UAF fix 가 원인
- **2b. master tag `v0.1.18`** — 모든 변경 전 (베이스라인 자체 재현)
  - 회귀 유지 → 외부 요인 (D) 또는 환경 변화
  - 회귀 사라짐 → 변경된 코드 (A/B/C 중 하나) 가 원인 → 2a 결과로 위치 확정

### Step 3: 외부 요인 점검 (병행)
- cam server 측 RTCP 송신 빈도 / EOS cycle 변화 확인 (cam 측 운영팀 요청)
- 호스트 측 네트워크 ping / loss (`ping`, `iperf` 등)
- 4 cam 모두 동일 IP 대역인지 / 다른 트래픽 영향 / 게이트웨이 측 변화

### Step 4: wd false positive 진단 (선택, Step 1-3 후)
- `WATCHDOG_STALE_SEC = 12 → 300` (5분) 변경 + monitor
- 짧은 stuck (12-15s) 의 wd-triggered-reset path 가 회귀 원인이라면 임계 늘려서 발화 안 시킴 → DFPS 회복 확인

### Step 5 (사용자 시그널): happytimesoft (GStreamer 적용 전) 24h monitor
- 가장 깊은 control test — GStreamer 자체가 회귀 원인인지 검증
- commit: `efeea7a` (Initial, happytimesoft only) 또는 NEXT_SESSION 의 Step 4 escalation 인용된 `37dae37` parent
- 24h monitor 후 baseline 비교
- 비용 + 위험 분석은 §5 참조

---

## 4. v0.1.18 ↔ 현 (0.1.26 binary) 의 변경 list (의심 commit 들)

```
3ff9e40 docs(policy): docs/small-commit branch PR 생성 X + 머지 시도 X
7c55ecb Merge: refactor/devicecluster-inline 흡수
f0e1c29 refactor: CameraCluster_DETECTOR 를 SettingManager 에 흡수
eb23c8f Merge: chore/safequeue-race-review
38cdbb1 docs: SafeQueue race deep review
d4e6bb0 Merge: refactor/safequeue-notify-out-of-lock
2c947f0 perf: SafeQueue notify_one() lock_guard 밖 이동 (MO-1)
db63c74 Merge: refactor/safequeue-terminate-notify-out
40b6d7d perf: SafeQueue terminate() notify_all() lock_guard 밖 이동
9bf8c0d docs(NEXT_SESSION): CLOSE-WAIT defensive close
c9dc858 docs(NEXT_SESSION): GMainContext cleanup 완료 반영
0f9ae2c refactor(gst): GstRtspReceiver/ProxyServer 각자 dedicated GMainContext  ← 의심도 ↑↑
f5c734d docs(NEXT_SESSION): GMainContext cleanup 완료 반영
4172ac8 fix(gst): jitter timer + bus watch UAF — g_source_remove → g_source_destroy  ← 의심도 ↑↑
26d6b07 docs(NEXT_SESSION): UAF fix 검증 진행 중 진입점 명시
6103035 chore(audit): detectbase.sh light/strict 강도 모드 도입
3b6e3c2 docs(git): branch cleanup + remote state verification 규칙 추가
b009fb0 docs(skill): SSOT 표명 + incident detail memory 로 위임
7a2b749 chore(v0.1.26): cmake bump 0.1.23 → 0.1.26 + doc 전체 sync
... (PR #31 documentation 갱신, code 변경 X)
7591105 fix(npu): batch_size 확장성 fix (input 측, batch=1 운영 영향 0)  ← C, 의심도 ↓
... (PR #32 documentation 갱신)
```

운영 binary 영향 commit 만 정리:
- `0f9ae2c` GMainContext per-instance — 의심도 높음
- `4172ac8` UAF fix — 의심도 높음
- `7591105` NPU batch fix — 의심도 낮음 (수학적으로 batch=1 영향 0)
- 나머지 (refactor/safequeue, refactor/devicecluster) — 영향 거의 없음 (SafeQueue MO-1 은 운영 path 안 critical, DeviceCluster 인라인은 path 동일)

→ **첫 진단 target = `0f9ae2c` + `4172ac8`**.

---

## 5. happytimesoft 24h monitor 가능성 평가

NEXT_SESSION 의 §2 cam_loss fix path 검증 Step 4: `37dae37` parent commit (happytimesoft 시점) 빌드 + 동일 환경 A/B test.

### Available commits
- `efeea7a` — Initial commit (5/15), happytimesoft baseline only, GStreamer 시도 코드 제거. **GStreamer 적용 전 가장 안정적**.
- `37dae37` (5/17) — GStreamer 통합 첫 commit. parent = `efeea7a`.

### Build 호환성
- `Dockerfile.build` 차이 — yaml-cpp 코멘트만 (P3 폐기 후 갱신). 실질 의존성 큰 변화 X. **빌드 가능 추정**.
- 그러나 efeea7a code 는 happytimesoft Protocol/RTSP 사용. 현 코드 trees Protocol/RTSP_GST 대신. 의존성 happytimesoft 라이브러리 build 필요 (Dockerfile.build 에 포함됐는지 확인 필요).

### 비용
- worktree 또는 임시 branch checkout (~5분)
- docker image rebuild 가능성 (~10-30분, Dockerfile 차이 작아 rebuild 빠름 추정)
- 운영 정지 + 새 binary 가동
- **24h monitor** = 거의 1일
- 총: ~25시간

### 위험
- happytimesoft 코드의 안정성이 GStreamer 보다 좋다는 보장 X (해피타임소프트 자체 leak/stuck 도 있었을 것)
- 의존성 호환 문제 발생 시 build fail
- 24h 운영 정지 비용

### 대안
- **v0.1.18 master tag** 24h monitor — 회귀 발생 직전 baseline. 빌드 호환성 보장 + 의심도 동등.
- (efeea7a vs v0.1.18) → v0.1.18 이 빠르고 안전. 단 GStreamer 자체 의심 검증은 못 함.

### 권장
- **Step 1-2 (UAF/GMainContext revert test)** 가 먼저. 빠르고 (1-3h) 의심도 동등.
- Step 1-2 가 회귀 위치 좁히지 못한 경우 → Step 5 (happytimesoft 또는 v0.1.18 24h monitor) 진행.

---

## 6. 즉시 결정 필요 — 어느 Step 부터?

| 옵션 | 동작 | 시간 | 의미 |
|---|---|---|---|
| **A — Step 1 30-60분** | 새 binary restart 후 빠른 monitor → 회귀 동일성 | ~1h | 기본 진단, 다른 step 의 전제 |
| **B — Step 5 happytimesoft 24h** | efeea7a checkout + build + 24h monitor | ~25h | 가장 깊은 control test, GStreamer 자체 가설 검증 |
| **C — Step 5 v0.1.18 24h** | v0.1.18 master tag checkout + 24h monitor | ~25h | 빠르고 안전, 최근 변경 영향만 검증 |
| **D — A → B 또는 A → C** | Step 1 결과 보고 후 추가 결정 | A+? | 가장 정보 효율 |

본 문서 작성 시점 monitor 정지됨 (이전 UAF fix runtime 1h JSONL: `logs/monitor_v0.1.26_uaf_fix_runtime.jsonl`, 47K, 75 cycles 보존).
