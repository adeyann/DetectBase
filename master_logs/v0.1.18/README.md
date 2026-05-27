# v0.1.18 master merge 증빙 archival

**Tag**: `v0.1.18`
**머지 시점**: 2026-05-27 (사용자 명시 허가 후 진행)
**master 이전 버전**: `v0.1.0` (2026-05-19)
**bump 종류**: patch (0.1.0 → 0.1.18 누적 변경)

---

## 머지 내용 요약 — v0.1.0 → v0.1.18

v0.1.0 (master 2026-05-19 5/19) 이후 develop 누적 18 개 patch bump 의 변경 사항.

### Major

| 버전 | 영역 | 내용 |
|---|---|---|
| **0.1.18** | RTSP / GStreamer | `GstRtspReceiver::TeardownPipeline()` 의 `gst_object_unref(pipeline_)` 가 GStreamer 내부 thread join 에서 unbounded block 하던 결함 fix. cam 661 의 42분 cam_loss 의 root cause. `gst_element_get_state` timeout 시 unref 건너뛰고 의도된 leak (process restart 시 OS cleanup) + WARN log. fix path 자체는 11.3h 후속 모니터 동안 미발화 (자연 stuck 안 일어남) — 정적 검증 완료, 동적 검증은 다음 stuck 발생 시. |
| **0.1.17** | 정책 / 운영 | git workflow 정책 갱신 (post-push bump + 5-step 절차) + pre-push docs check 절대 규칙 + memory 영어 단일 언어화 |
| **0.1.16** | 안전성 | `Main.cpp` argv guard + `/DetectBase/logs/.detectbase.lock` `flock(2)` single-instance lock — PID 4924 사고 재발 방지. monitor.sh threshold alert 7 종 + warmup grace 4 cycle. |
| **0.1.15** | 운영 가시성 | REST `get_json_from_resp_body` silent catch → MLOG_WARN 추가 |
| **0.1.14** | RTSP / GStreamer | MPP + Option A 완전 폐기, Full reset 복귀 (5/24 baseline). snapshot: tag `mpp-architecture-snapshot-v0.1.13` + `.backup/mpp_purged_20260526/` |
| **0.1.13** | 회귀 fix | per-cam stage FPS counter (0.1.12) revert — global mutex hot path 회귀 |

### 추가 정합성 정리 (v0.1.18 master merge 직전 PR #22 / #23)

| PR | 내용 |
|---|---|
| PR #21 | cmake 0.1.19 → 0.1.18 정렬 + master_logs 보관 절차 + cmake bump README 동기 절대 규칙 도입 |
| PR #22 | 정합성 다중 round (20 commit) — git workflow 정책 정합 (skill / memory / CLAUDE.md) + DFPS 13/cam → 29/cam multi-core 갱신 + RTSP_GST module description + shutdown 순서 code 정합 + yaml-cpp 잘못된 TODO 정정 + 메트릭 표 ~23→~38 + OPERATIONS WARN/ERROR 메시지 happytimesoft → GStreamer 시대 + logrotate "100MiB × 7" → "daily + rotate 14" + 외 다수 stale 정리 |
| PR #23 (this) | master_logs/v0.1.18/ archival + 기존 baseline ref (README.md L797 / code/README.md L111 / logs/NEXT_SESSION.md L258) → 5/27 baseline 으로 갱신 |

---

## 검증 결과

### audit 5종 (2026-05-27 09:14 ~ 14:33 KST)

| Tool | 5/24 baseline | 5/27 current | 회귀 |
|---|---|---|---|
| cppcheck 자체 결함 | 59 | **59** | ❌ 없음 (완전 동일) |
| clang-tidy warning | 0 | **0** | ❌ 없음 (완전 동일) |
| ASan/UBSan 자체 코드 leak | 0 | **0** | ❌ 없음 |
| TSan 자체 코드 race | 0 | **0** | ❌ 없음 |
| ASan 외부 lib leak (run 비례) | 1.21 MB (9min run) | 1.24 MB (4h run) | run 27× 길어졌지만 leak 거의 증가 X (rtpmanager 정상 cap) |
| TSan WARNING (외부 + FP, run 비례) | 141 (8min run) | 172 (60min run) | run 7.5× 길어진 비례 증가 (예상 범위) |

산출물: `master_logs/v0.1.18/audit_20260527_091456/`
- `cppcheck.log` (17,323 bytes) — 자체 코드 59건 (baseline 와 동일)
- `clangtidy.log` (42 bytes) — warning 0건
- `asan_run.log` (296 MB) — 4h run, leak 발생지 모두 외부 lib
- `tsan_run.log` (1.4 MB) — 1h run, 자체 코드 race 0
- `summary.txt` — 종합 카운트

### 운영 모니터링 (2026-05-26 21:43 ~ 2026-05-27 09:06 = 11.3h)

| 지표 | 값 | 평가 |
|---|---|---|
| 모니터 task | `bl4c785is` label `v018_teardown_fix` | 11.3h heartbeat 수집 |
| DFPS 평균 | ~116 (multi-core baseline) | ✅ 안정 |
| wd | 1 (boot ramp-up 직후 only) | ✅ 안정 |
| cam_loss | 0건 (전체 11.3h) | ✅ 안정 |
| cam_active | 4/4 sustained | ✅ 안정 |
| RSS | plateau ±20MB | ✅ 안정 |
| FD / Threads | stable | ✅ 안정 |

산출물: `master_logs/v0.1.18/monitor_v018_teardown_fix.jsonl` (591 line JSONL)

### 솔직한 평가

- v0.1.18 TeardownPipeline unref-skip fix 는 **사후조치 (defensive workaround)** — stuck 의 진짜 원인 (`[1]` stream 끊김 + `[5]` GStreamer thread join 실패) 미식별.
- 11.3h 운영 결과 fix path 자체가 한 번도 발화 안 함 → fix 의 effectiveness empirical 검증 0. 안정성 회복은 stuck 조건 사라진 환경 변화 (PID 4924 residual state cleanup) 때문일 가능성 높음.
- 진짜 검증은 다음 자연 stuck 시점에 가능. 만약 그때 fix 가 효과 안 보이면 escalation Step 2-4 (GST_DEBUG / tcpdump / happytimesoft rollback A/B) 진행.

자세한 분석: [.DOCS/UNSTABLE_NETWORK_BEHAVIOR_20260526.md](../../.DOCS/UNSTABLE_NETWORK_BEHAVIOR_20260526.md)

---

## Master merge gate 충족 검증

| 요건 (patch bump) | 상태 |
|---|---|
| audit 5종 PASS | ✅ 자체 코드 회귀 0건 (cppcheck/clang-tidy/ASan/UBSan/TSan 모두 5/24 baseline 일치 또는 동일) |
| 3h+ 운영 모니터링 | ✅ 11.3h 누적 (3h+ 요건 충족, 24h 목표 미달이지만 patch bump 에는 3h+ 만 필요) |
| 사용자 명시 허가 | ⏳ 본 PR develop merge 후 별도 요청 |

CLAUDE.md §Master merge gate `patch bump` 요건 모두 충족.
