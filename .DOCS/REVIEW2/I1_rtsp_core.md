# I1 — RTSP/HTTP/RTP Proxy & Server Core (외부)

## §1. Why

카메라 RTSP 클라이언트 + 자체 RTSP proxy 서버를 통합 제공하는 외부 라이브러리.

DetectBase 는 이 라이브러리로:
1. 카메라 RTSP URL 에 연결하여 H.264/H.265/MJPEG 등 영상 수신 → F3 의 frame 파이프라인에 공급
2. 분석 결과 메타데이터를 합쳐 다시 RTSP 서버로 외부 클라이언트에 송출 (proxy out)

제거 시 잃는 것: 카메라 입력과 proxy 출력 모두 불가 → 메인 파이프라인 동작 불가.

---

## §2. Roster

### Primary (I1) — 외부 라이브러리

총 **38,643 라인** (rtsp/src/ 기준). 5 layer 로 구성:

| Layer | 위치 | 라인 합계 (대략) | 역할 |
|---|---|---|---|
| **core** | [Protocol/RTSP/core/](../../code/Protocol/RTSP/core/) | base64, md5, linked_list, hqueue, ppstack, sys_buf, sys_log, sys_os, util, word_analyse, hxml, xml_node | 기본 자료구조 / OS thread / xml |
| **http** | [Protocol/RTSP/http/](../../code/Protocol/RTSP/http/) | http_cln, http_mjpeg_cln, http_parse, http_srv | HTTP server/client + MJPEG over HTTP |
| **media** | [Protocol/RTSP/media/](../../code/Protocol/RTSP/media/) | video/audio capture/encoder/decoder, alsa, v4l2, screen_capture, file_demux, live_audio/video, lock, media_util | 미디어 캡처/코덱 (DetectBase 사용 매우 제한적 — capture/encoder 안 씀) |
| **rtp** | [Protocol/RTSP/rtp/](../../code/Protocol/RTSP/rtp/) | h264_rtp_rx, h265_rtp_rx, mjpeg_rtp_rx, mpeg4_rtp_rx, aac_rtp_rx, pcm_rtp_rx, rtp_rx, rtp_tx (1464 라인), rtcp (374 라인), bit_vector, h264/h265_util, h264_parser | RTP packetize/depacketize |
| **rtsp** | [Protocol/RTSP/rtsp/](../../code/Protocol/RTSP/rtsp/) | rtsp_cln (3114), rtsp_srv (2132), rtsp_proxy (1831), rtsp_pusher (2181), rtsp_media (2270), rtsp_parse (1559), rtsp_cfg (1053), rtsp_rcua (1192), rtsp_rsua (1766), rtsp_proxy_recorder (514), rtsp_auth, rtsp_backchannel, rtsp_crypt, rtsp_http, rtsp_srv_backchannel, rtsp_stream, rtsp_timer, rtsp_util | RTSP layer + 핵심 entity |

### Also-touches

| 흐름 | 사용 측면 |
|---|---|
| F3 (Camera Pipeline) | RTSP 클라이언트로 카메라 영상 수신. `CRtspProxy`, `ProxyVideoInfo`, `rtsp_get_proxy_nums()` 등 |
| F4 (Event Output) | proxy out 으로 분석된 영상 재송출 (RtspHandler 가 관리하는 proxy 객체에 메타데이터 push) |
| F2 (Configuration) | `g_rtsp_cfg`, `rtsp_xml_path` 로딩, `RtspProxyConfigXmlMaker::Make()` 동적 cfg xml 생성 |
| I3 | xml 파싱, base64/md5, linked_list 등 자체 자료구조 사용 (라이브러리 내부 의존이라 직접 결합 없음) |

---

## §3. How — DetectBase 가 사용하는 진입 표면

라인 단위 검토는 하지 않는다. DetectBase 코드(Management, Main)가 import 하는 헤더는 단 **5개 그룹**:

```
RtspHandler.h  →  rtsp_srv.h, rtsp_cfg.h, rtsp_proxy.h, rtsp_timer.h
```

(외 RtspDetectorBlock.h 도 RtspHandler 만 import)

### 3.1 사용 클래스 / 구조체

| 심볼 | 위치 | DetectBase 사용처 |
|---|---|---|
| `CRtspProxy` | rtsp_proxy.h | RtspHandler.cpp:160 — `p_proxy->proxy = new CRtspProxy(&p_proxy->cfg)` |
| `CRtspProxy::startConn(url, user, pass)` | 동 | RtspHandler.cpp:164 |
| `CRtspProxy::getProxyVideoInfo()` | 동 | RtspHandler.cpp:286 — `ProxyVideoInfo` 반환 |
| `ProxyVideoInfo` | rtsp_proxy.h | RtspHandler::GetProxyInfo 의 반환 타입 (프레임 width/height/fps 등) |
| `RTSP_PROXY*` (linked list 노드) | rtsp_cfg.h | RtspHandler.cpp:158 — `g_rtsp_cfg.proxy` 순회 |

### 3.2 사용 전역 함수 / 매크로 / 전역 변수

| 심볼 | 사용처 |
|---|---|
| `g_rtsp_cfg` (전역 cfg) | RtspHandler.cpp:158 |
| `MAX_NUM_RUA` (매크로) | RtspHandler.cpp:143 — buffer 사이즈 계산 |
| `rtsp_get_proxy_nums()` | RtspHandler.cpp:144 |
| `rtsp_parse_buf_init(bufs)` | RtspHandler.cpp:146 |
| `rtsp_print_info()` | RtspHandler.cpp:147 |
| `sys_os_create_thread(fn, arg)` | RtspHandler.cpp:175-176 — RTSP 내부 스레드 시작 (rtsp_rx_thread, rtsp_task) |
| `rtsp_rx_thread`, `rtsp_task` | RtspHandler.cpp:175-176 — RTSP 라이브러리 내부 메인 루프 |
| `hrtsp` (전역 handle) | RtspHandler.cpp:178 — `r_flag`, `sys_init_flag`, `tid_pkt_rx`, `tid_main` 등 |

### 3.3 호출 시퀀스 (DetectBase 측)

```
[RtspHandler::Initialize]
  1. xml 로드 (rtsp_cfg.xml) → g_rtsp_cfg 채움
  2. rtsp_parse_buf_init(bufs)
  3. rtsp_print_info()

[RtspHandler::RunRTSP]
  1. for each (RTSP_PROXY* p : g_rtsp_cfg.proxy):
       p->proxy = new CRtspProxy(&p->cfg)
       if (init_with_static_cfg): p->proxy->startConn(url, user, pass)
       sleep random(50~400ms)  ← 동시 connect storm 방지
  2. hrtsp.r_flag = 1
  3. tid_pkt_rx = sys_os_create_thread(rtsp_rx_thread)
  4. tid_main   = sys_os_create_thread(rtsp_task)
  5. SetRtspState(Run)

[RtspHandler::StopRTSP] (검증 필요 — 라인 미확인)
  1. proxy 별 stop
  2. hrtsp.r_flag = 0
  3. thread join with timeout
```

### 3.4 RtspProxyConfigXmlMaker (DetectBase 측 wrapper)

[RtspHandler.cpp:317-...](../../code/Management/worker/src/RtspHandler.cpp#L317) — DetectBase 카메라 정보를 라이브러리가 요구하는 XML 형식으로 동적 생성. F2(설정)에서 NetworkSettings 카메라 목록 → 이 XML → RTSP 라이브러리.

---

## §4. Lifetime & Ownership

| 객체 | 소유자 | 비고 |
|---|---|---|
| `CRtspProxy*` | `RTSP_PROXY` (라이브러리 내부 linked list 노드)의 `proxy` 멤버 | DetectBase 가 `new` 로 생성 후 노드에 저장. 노드와 함께 lifecycle (라이브러리가 destroy 책임) |
| `g_rtsp_cfg.proxy` linked list | RTSP 라이브러리 (전역) | 라이브러리가 build/destroy. DetectBase 는 read-only 순회 + ptr 저장 |
| RTSP 내부 스레드 (`tid_pkt_rx`, `tid_main`) | RTSP 라이브러리 (`hrtsp`) | DetectBase 가 sys_os_create_thread 로 시작하지만 join/stop 책임은 라이브러리 (`hrtsp.r_flag = 0` 으로 종료 신호) |

⚠ **raw `new CRtspProxy`** ([RtspHandler.cpp:160](../../code/Management/worker/src/RtspHandler.cpp#L160)) — CLAUDE.md "raw new/delete 금지" 와 충돌. 단, 외부 라이브러리 인터페이스가 raw ptr 을 요구하므로(linked list 의 멤버 타입이 `CRtspProxy*`) 변경 어렵다 — **외부 라이브러리 ABI 강제 패턴**으로 분류.

---

## §5. Concurrency

- 라이브러리가 자체적으로 2개 메인 thread (`rtsp_rx_thread` + `rtsp_task`) + per-proxy worker thread 생성
- DetectBase 는 그 위에 자기 thread 를 얹지 않고, ProxyInfo 만 polling 식으로 가져온다 (RtspHandler::GetProxyInfo)
- `proxy_mtx_` ([RtspHandler.h:75](../../code/Management/worker/include/RtspHandler.h#L75)) 는 DetectBase 측의 proxy linked list 접근 (Run 도중) 보호
- 라이브러리 자체의 thread-safety 는 외부 의존 — DetectBase 가 직접 검증 안 함

---

## §6. Findings

### F-I1-01 — 외부 라이브러리 (소스 검토 범위 외)
- **등급**: INFO
- **내용**: 38,643 라인의 외부 RTSP 라이브러리. DetectBase 가 사용하는 진입 표면(§3.1~3.2 의 약 15개 심볼) 만 검토. 라이브러리 내부 결함은 본 리뷰 범위 밖.
- **현 영향**: 검증된 동작이 있고 1차 리뷰에서 안정성 확인됨. 신규 결함 발견 시에도 in-place patch 보다는 upstream 또는 wrap 으로 대응.

### F-I1-02 — `new CRtspProxy` raw new 사용
- **등급**: INFO (외부 ABI 강제 사례)
- **위치**: [RtspHandler.cpp:160](../../code/Management/worker/src/RtspHandler.cpp#L160)
- **내용**: linked list 노드가 raw ptr 을 요구. 라이브러리가 destroy 책임. CLAUDE.md 규칙 예외.
- **현 영향**: 라이브러리 인터페이스 변경 없이 해결 불가. 현 상태 유지.
- **권장**: 코드 주석 1줄로 "라이브러리 ABI 요구로 raw new 사용. destroy 는 라이브러리 책임" 정도 명시하면 코드 리뷰 시 혼동 방지. 즉시 가치 낮음.

### F-I1-03 — proxy startConn 시 50~400ms random sleep — 동시 connect storm 방지 의도
- **등급**: INFO (긍정 발견)
- **위치**: [RtspHandler.cpp:151-170](../../code/Management/worker/src/RtspHandler.cpp#L151-L170)
- **내용**: 다수 카메라 동시 connect 가 NIC/카메라 측에 부담을 주는 것을 jitter 로 분산. 의도가 코드에서만 추론 가능.
- **권장**: 주석 1줄로 의도 명시 (deferred).

### F-I1-04 — RTSP 라이브러리의 thread-safety 가 DetectBase 측에서 보장 안 됨
- **등급**: NOTE
- **내용**: 라이브러리 내부 thread (rx_thread, task) 와 DetectBase 측의 polling (`GetProxyInfo` 등) 간 동시 접근 시 안전성은 라이브러리 책임. `proxy_mtx_` 는 DetectBase 측 접근만 보호.
- **현 영향**: 1차 리뷰 + 운영 중 문제 미보고.
- **권장**: 변경 없음. 단, `getProxyVideoInfo()` 가 라이브러리 내부에서 lock 을 잡는지 검증할 가치 있음 (deferred / 외부 라이브러리 검증).

### F-I1-05 — `hrtsp` 전역 handle — 다중 RtspHandler 인스턴스 불가
- **등급**: NOTE
- **위치**: [RtspHandler.cpp:142, 178](../../code/Management/worker/src/RtspHandler.cpp)
- **내용**: 라이브러리가 전역 `hrtsp`, `g_rtsp_cfg` 를 사용하므로 한 프로세스 내 RtspHandler 인스턴스는 1개로 제한.
- **현 영향**: DetectBase 는 단일 인스턴스 패턴이라 문제 없음.
- **권장**: 변경 없음.

### F-I1-06 — media/ 디렉토리의 capture/encoder 코드는 DetectBase 미사용 (코드 size 부담)
- **등급**: INFO
- **내용**: video_capture/video_encoder/audio_capture/audio_encoder/screen_capture/v4l2/alsa 등 — 카메라가 아닌 자체 캡처 디바이스 케이스. DetectBase 는 RTSP 클라이언트만 사용.
- **현 영향**: 미사용 코드가 빌드에 포함될 수 있음. 빌드 설정에 따라 link 단계에서 제외되거나 dead code 로 strip.
- **권장**: 빌드 시간/binary 크기 최적화 필요 시 CMake 에서 media/ 일부 파일 exclude. 현재 빌드 산출물 크기에 큰 부담 없으면 deferred.

---

## §7. Open Questions

1. **F-I1-04**: `CRtspProxy::getProxyVideoInfo()` 가 라이브러리 내부에서 lock 을 잡는지 확인할까? (검증하면 RtspHandler 의 polling 안전성 명확). 외부 라이브러리 검토라 deferred 권장.

---

## §8. Self-Check

- [x] DetectBase 코드가 import 하는 RTSP 라이브러리 헤더 grep 으로 전수 확인 (rtsp_srv.h / rtsp_cfg.h / rtsp_proxy.h / rtsp_timer.h)
- [x] DetectBase 코드가 사용하는 라이브러리 심볼 grep 으로 전수 확인 (CRtspProxy, ProxyVideoInfo, g_rtsp_cfg, hrtsp, sys_os_create_thread 등)
- [x] §3 호출 시퀀스 — 코드 출처(file:line) 표기
- [x] §4 소유권 — raw new 의 ABI 강제 사유 명시
- [x] §5 동시성 — 라이브러리/DetectBase 책임 경계 명시
- [x] §6 Finding 등급 + 출처
- [x] 라이브러리 내부 라인은 검토하지 않았음을 명시 (검토 범위 정의)
- [x] 추측 vs 단정 구분 — `StopRTSP` 시퀀스는 "검증 필요"로 표기

**검증 결과**: PASS

**보강 필요 항목**:
- F4 분석 시: proxy out 으로 메타데이터(ONVIF) 를 어떻게 push 하는지 → CRtspProxy 의 추가 API 표면 확장
- F2 분석 시: RtspProxyConfigXmlMaker 의 NetworkSettings → XML 변환 검증
