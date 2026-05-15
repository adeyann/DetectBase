# NEXT_SESSION — happytimesoft baseline 완전 복원 + git push 진행

**최종 갱신**: 2026-05-15 16:30 KST
**상황**: ✅ git push 완료 (master branch, 452 objects, https://github.com/adeyann/DetectBase) → STEP 5 (GStreamer 재작업 branch) 진행 단계

---

## 사용자 명령 (확정)

1. ✅ happytimesoft baseline 으로 완전 복원 (옵션 B: jemalloc/W-14/P1/P3 유지)
2. ✅ REID/AlphaRover 의존성 제거 (RtspHandler.cpp 도 이미 DetectBase 용 19KB 정리본으로 변경됨)
3. ⏳ 빌드 + sanity → git push (https://github.com/adeyann/DetectBase.git)
4. ⏸ 그 후 GStreamer 재작업 (branch 으로 단계별 commit)

---

## 진행 상황 (compact 후 진행)

### STEP 1 — RtspHandler.cpp REID/AlphaRover 의존 제거: ✅ 확인 완료
- 파일 크기: DetectBase **19,221 bytes** (MAIA 27,482 ❌)
- `RtspProxyConfigXmlMaker::Make(const std::string&)` only overload (set/vector overload 없음)
- `setting_manager->GetServerSetting()` (AlphaRover 아님)
- `GetCameraIDSet()`, `GetCameraSettingsManager()`, `sptrCamInfo->GetSetting(cam_id)` 사용
- REID/AlphaRover/ReidRequestCameraUnit grep 결과 0건

### STEP 2 — clean rebuild + 운영 + 기본 sanity: ✅ PASS

| 항목 | 결과 |
|---|---|
| 이전 bin/Build-Integrate | `.deleted_backup/build_rollback_20260515/Build-Integrate_old/` 로 mv |
| docker run cmake + make -j$(nproc) | exit 0, **error 0, warning 0**, 100% Built target DetectBase |
| 산출물 | `bin/Build-Integrate/Main/DetectBase` (2,453,128 bytes, ARM aarch64) |
| `bin/DetectBase` symlink | `→ /DetectBase/bin/Build-Integrate/Main/DetectBase` (컨테이너 내부 path) |
| docker-compose up -d | detectbase_service Up |
| DFPS | **53.5** ~ 53.6 |
| Camera state | **registered=4, active=4** |
| ERROR/FATAL/CRITICAL | **0** |
| event_detected | cam 659/660/661 정상 |
| RTSP Proxy Publish Port | **555** (happytimesoft) |

### STEP 3 — Monitor tool 기반 sanity: ✅ PASS

Monitor task `byyb8z09p` (10분 간격 × 6회 + spot 측정).

| 시점 | Up | RSS (kB) | Δ from T+10 | 비고 |
|---|---|---|---|---|
| T+10 | 12분 | 517,656 | baseline | Monitor 시작 첫 측정 |
| T+20 | 22분 | 516,260 | −1,396 | |
| T+30 | 32분 | 530,092 | +12,436 | ramp-up |
| T+40 | 42분 | 535,640 | +17,984 | |
| T+50 | 52분 | 543,704 | +26,048 | ramp-up peak 근처 |
| spot (T+60+) | 78분 | **524,408** | **+6,752** | **−19,296 회수, plateau** |
| **VmHWM** | | **568,844** | **+51,188** | 그 사이 peak (jemalloc 회수 전) |

**해석**:
- ramp-up 안정화 폭 **+51 MB peak** (사용자 표현 "48h test 초반 안정화 47MB" 와 일치)
- jemalloc `background_thread:true` 가 peak 후 **−44 MB 회수**
- plateau ~525 MB (안정)
- ERROR 0, DFPS 53.2~54.7 (정상), event 19,669 (production 활발)

**결론**: happytimesoft baseline + jemalloc/W-14/P1/P3 patches **leak 없음, 안정 PASS**.

**부수 발견 (사용자 지적, 향후 cleanup 후보)**:
- W-14 (`malloc_trim(0)` in `RtspDetectorUnit.cpp:231`) 는 **jemalloc 환경에서 사실상 no-op**.
  이유: LD_PRELOAD 가 malloc/free 만 hook, `malloc_trim` 은 glibc 전용 — jemalloc 은 wrap export 안 함. glibc heap 거의 비어있어 trim 대상 X.
  실제 회수 = jemalloc background_thread (`madvise MADV_DONTNEED`).
- W-14 는 dead code 와 유사. emergency cleanup 1회 호출이라 cost 0, 빼도 무해.
- GStreamer 재작업 마무리 후 정리 권장.

---

## ★ 60분 sanity 끝난 후 (사용자 호출)

### 1. 결과 확인 (AI 가 진행)

```bash
cat /home/claudedev/DetectBase/logs/sanity_60min_20260515_145341.log
```

`=== FINAL ===` block 의 `Total delta` 가 판단 기준 안에 들어오면 PASS.

### 2. 만약 sanity PASS → git push 안내 (사용자 직접)

```bash
cd /home/claudedev/DetectBase

# 1) 상태 확인 (bin/, logs/, .deleted_backup/ 가 .gitignore 적용되는지)
git status

# 2) 첫 commit 준비
git add .
git status   # 의도하지 않은 파일 포함되었는지 한 번 더 확인

# 3) commit (한국어 메시지)
git commit -m "Initial commit: happytimesoft baseline + jemalloc/W-14/P1/P3 patches

- happytimesoft RTSP library 완전 복원 (Protocol/RTSP)
- GStreamer 시도 코드 제거 (.deleted_backup/gst_attempt_20260515/)
- jemalloc LD_PRELOAD (docker-compose.yml)
- W-14: malloc_trim(0) in RtspDetectorUnit.cpp (jemalloc 환경에서 no-op, 추후 정리)
- P1: libyaml-cpp-dev 중복 제거 (Dockerfile.build)
- P3: ClassChecker YAML→JSON

Sanity (Up 78분):
- DFPS 53.2 (4 cameras active)
- VmRSS 524,408 kB (peak VmHWM 568,844 kB 후 -44 MB 회수)
- ERROR 0 / event 19,669
- ramp-up 안정화 +51 MB → plateau 도달 확인"

# 4) branch rename (master → main)
git branch -M main

# 5) push
git push -u origin main
```

### 3. git push 후 GStreamer 재작업 branch 분기

```bash
git checkout -b feature/gstreamer-integration
```

작업 순서 (NEXT_SESSION 다음 버전 plan):
1. PoC (avdec_h264) — 단순 동작 검증
2. video forward (raw passthrough) 통합
3. ONVIF metadata payloader 통합
4. (선택) MPP hardware decoder + leak fix

---

## 만약 sanity FAIL 시

happytimesoft baseline 자체가 leak 이면 — 처음으로 복원이 잘못된 것. 확인:

```bash
# RtspHandler 가 정말 happytimesoft 사용 중인지
grep -nE "g_rtsp_cfg|CRtspProxy|rtsp_read_config" /home/claudedev/DetectBase/code/Management/worker/src/RtspHandler.cpp

# Protocol/RTSP/ 가 happytimesoft 코드 그대로인지
ls /home/claudedev/DetectBase/code/Protocol/RTSP/
diff -r /home/claudedev/DetectBase/.deleted_backup/happytimesoft_rtsp_20260514/RTSP_original /home/claudedev/DetectBase/code/Protocol/RTSP/ | head
```

만약 baseline 도 leak 이면 jemalloc + W-14 의 patch 가 누락된 것일 수도. docker-compose.yml 의 LD_PRELOAD 라인 / RtspDetectorUnit.cpp:231 의 malloc_trim 호출 확인.

---

## 핵심 파일 위치 reference

### 코드
- DetectBase root: `/home/claudedev/DetectBase`
- MAIA reference (happytimesoft 시절): `/home/claudedev/MAIA`
- `code/Protocol/RTSP/`: happytimesoft 라이브러리 (core/http/media/rtp/rtsp)
- `code/Management/worker/src/RtspHandler.cpp`: 19,221 bytes (DetectBase 용)
- `code/Management/worker/include/RtspHandler.h`: Make(string) only
- `code/BasicLibs/core/parser/xml/tiny_xml.h`: P2 롤백 후 복원 (happytimesoft 의 XML config)

### 휴지통 (.deleted_backup/)
- `gst_attempt_20260515/`: GStreamer 시도 코드 (참고)
- `happytimesoft_rtsp_20260514/`: 빈 디렉토리 (RTSP_original 은 복원됨)
- `mpp_rollback_20260515/`: Build-ASan/Build-Debug, ASan/valgrind 로그
- `build_rollback_20260515/Build-Integrate_old/`: 5월 14일 빌드 (이번 clean rebuild 직전)

### 이미지
- `detectbase:1.0` (1.17 GB, GStreamer 없는 clean image)
- `detectbase:before-gst-integration-20260514` (1.34 GB, GStreamer 빌드 deps 포함된 백업)
- `detectbase:analysis` (audit 용)

### Git
- Local: `/home/claudedev/DetectBase/.git/` (사용자가 init 완료)
- Remote: `https://github.com/adeyann/DetectBase.git` (origin 등록됨)
- Branch: `master` (default — `git branch -M main` 권장)
- 사용자: `adeyann` <kuy920516@gmail.com>
- 첫 commit 아직 없음

### `.gitignore`
```
bin/, code/build/, *.so, *.a, *.rknn, *.engine, logs/*.log, .deleted_backup/,
code/Protocol/REST/lib/, code/Protocol/SocketIO/lib/,
code/BasicLibs/core/parser/yaml-cpp/lib/, image_root/, frame/, crop/, ...
```

---

## 사용자 메모리 규칙 (재확인)

- **Git 워크플로우** (2026-05-15 도입): AI 는 git/gh 사용 가능, 단 `master` 직접 작업 금지. 본인 branch 만 사용 (자유 생성). `master` 머지는 PR + 사용자 명시 승인. 비-master branch 간 머지는 자유. force push / `git reset --hard` 금지.
- **삭제 규칙**: `rm`/`unlink`/`rmdir` 직접 금지. `.deleted_backup/` 으로 mv. 실제 `rm` 은 사용자만.
- 모든 임시 로그는 `DetectBase/logs/` 안에 (절대 /tmp 아님)
- read 용 명령 (find/grep/awk) 즉시 실행, **sed 금지** (deny 등록)
- destructive 명령 (docker kill, rm -f 등) 단독 호출
