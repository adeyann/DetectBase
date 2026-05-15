# F2 — Configuration & Live Reload

## §1. Why

설정의 단일 소스 + 변경 사항이 동작 중인 객체에 자동 반영되는 흐름.

3 영역:
1. **Static profile** — 시작 시 1회 로드. 변경 안 됨. (`NetworkProfile`, `ServiceProfile`, `EngineProfile`)
2. **Dynamic settings** — 런타임 갱신 가능. REST API 로 가져옴. 4종 (`ServerSetting`, `CameraSetting`, `ExcludeCamSetting`, `ScheduleSetting`)
3. **Live reload** — 동적 settings 변경 시 등록된 callback 자동 호출. RtspDetectorUnit 같은 운영 객체가 callback 으로 설정 동기화.

제거 시 잃는 것: 모든 흐름이 시동 불가. 카메라 추가/스케줄 변경 시 재시작 필요.

---

## §2. Roster

### Primary (F2)

| 카테고리 | 파일 |
|---|---|
| **Static Profile** | [profile/EngineProfile.{h,cpp}](../../code/BasicLibs/profile/), [profile/EngineProfileParser.{h,cpp}](../../code/BasicLibs/profile/), [profile/NetworkProfileParser.{h,cpp}](../../code/BasicLibs/profile/), [profile/ServiceBlockProfile.{h,cpp}](../../code/BasicLibs/profile/), [profile/ServiceProfile.h](../../code/BasicLibs/profile/), [profile/ServiceProfileBuilder.{h,cpp}](../../code/BasicLibs/profile/) |
| **Setting Interface** | [manager/interface/ISettingData.{h,cpp}](../../code/Management/manager/interface/), [manager/interface/ISettingManager.{h,cpp}](../../code/Management/manager/interface/) |
| **Setter / Manager** | [manager/include/SetterBase.h](../../code/Management/manager/include/SetterBase.h), [manager/include/SettingManagerBase.h](../../code/Management/manager/include/SettingManagerBase.h), [manager/include/SettingManager.h](../../code/Management/manager/include/SettingManager.h), [manager/include/SettingData.h](../../code/Management/manager/include/SettingData.h), [manager/src/SettingManager.cpp](../../code/Management/manager/src/SettingManager.cpp), [manager/src/SettingData.cpp](../../code/Management/manager/src/SettingData.cpp) |
| **Live reload Mixin** | [worker/include/SettingMonitor.h](../../code/Management/worker/include/SettingMonitor.h), [worker/src/SettingMonitor.cpp](../../code/Management/worker/src/SettingMonitor.cpp) |

### Also-touches

| 파일 | Primary | F2 에서 활용 |
|---|---|---|
| [InterProtocolTypes.h](../../code/BasicLibs/core/types/InterProtocolTypes.h) | I3 | F4 SocketIO 진입점에서 `InterProtocolFunc(json)` 시그니처로 setting 갱신 RPC |
| [DeviceCluster.h](../../code/BasicLibs/core/types/DeviceCluster.h) | I3 | `CameraCluster_DETECTOR` — REST API 로 받은 카메라 cluster JSON → DeviceID set |
| [ApiHandler.h](../../code/Management/worker/include/ApiHandler.h) | F4 | F2 의 `SettingManager::Initialize` 가 ApiHandler 로 REST GET 호출 |
| [json/* / yaml-cpp/*](../../code/BasicLibs/core/parser/) | I3 | profile / setting 모두 JSON 파싱 |
| [UUIDGenerator.h](../../code/BasicLibs/utils/UUIDGenerator.h) | I3 | Callback registration 의 식별자 |
| [DETECTOR.cpp](../../code/Main/DETECTOR/src/DETECTOR.cpp) | F1 | `SettingManager::GetSingletonSettingManager()->Initialize()` 호출 (Stage init) |
| [RtspDetectorUnit.cpp](../../code/Main/DETECTOR/src/RtspDetectorUnit.cpp) | F3 | `SettingMonitor` 상속 + `SubscribeSetting<ScheduleSettingData>` / `SubscribeSetting<ExcludeCamSettingData>` 로 동적 변경 수신 |

---

## §3. How

### 3.1 정적 프로필 로딩 (시작 시 1회)

```
[F1 Init]
  NetworkProfile np = NetworkProfileParser::Parse("settings/NetworkSettings.json");
    └─ NetworkProfile { local_ip, mvas_ip, mvas_api_port, mvas_sio_port,
                        grpc_client_enabled, grpc_peers,
                        grpc_server_enabled, grpc_server_bind_address, grpc_server_port }
  ServiceProfile sp = ServiceProfileBuilder::Build();
    └─ EntireServiceBlockProfileGraph (서비스 → block 의존성 그래프)
    └─ ServiceGraphValidator::Validate(graph, tag) — 사이클 / unreachable 검출
  EngineProfile ep = EngineProfileParser::Parse(yaml_path);
    └─ ModelMajorType, ModelMinorType, classes.yml path, infer requirements 등
```

이 3개는 read-only. 시작 시 인자로 전달되어 운영 객체에 박힌다.

### 3.2 SettingManager Init 시퀀스 (REST API 로 동적 데이터 로드)

```
SettingManager::GetSingletonSettingManager()->Initialize(SettingInitData{api, "DETECTOR", extend});
  ↓ try { ... } catch { runtime_error / exception / ... }
  ↓ unique_lock(initialize_mutex)
  ↓ double-check initialization_succeeded
  ↓
  InitializeImpl_DETECTOR(init_data)
    │
    ├─ api->GetMyLocalIp()
    ├─ api->GET_or_throw_if_timeout(API_GET_SERVER_BY_IP)         → server_service_id
    ├─ GetSearchServerIP(server_setter_js, api)                   → SearchServerIp 보강
    ├─ SetServerSetting(server_setter_js)
    │     └─ server_setting = make_shared<SetterBase<ServerSettingData>>(init_json, lock=false)
    │
    ├─ SetCameraCluster_DETECTOR(init_data)
    │     └─ api->GET(API_GET_CLUSTER, server_service_id)
    │     └─ camera_cluster->AddClusterData(rsp_body) → CameraIDSet
    │
    ├─ api->GET(API_GET_CAMERA, cams) → SetCameraSettings(rsp)
    │     └─ camera_settings = make_shared<SettingManagerBase<CameraSettingData>>(json, "CameraId", lock=true)
    │
    ├─ api->GET(API_GET_EXCEPTION_CAMERA, cams) → SetExcludeCamSettings(rsp)
    │     └─ exclude_cam_settings = make_shared<SettingManagerBase<ExcludeCamSettingData>>(json, "CameraId", lock=true)
    │
    └─ api->GET(API_GET_SCHEDULE, cams) → SetScheduleSettings(rsp)
          └─ schedule_settings = make_shared<SettingManagerBase<ScheduleSettingData>>(json, "CameraId", lock=true)
  ↓
  initialization_succeeded.store(true)
  ShowInitializeStatus(tag)
```

### 3.3 SetterBase<T>::Update 의 락 해제 후 callback (#10/#11 패턴)

```
SetterBase<T>::Update(json):
  Phase 1: 데이터 갱신 + 스냅샷 캡처
    if(need_data_update_lock):
      lock_guard(data_update_mtx_)
      success = UpdateInternal(json)
      if success: data_snapshot = setting_data_   ← copy
    else:
      success = UpdateInternal(json)
      if success: data_snapshot = setting_data_

  if !success: return false

  Phase 2: callback 목록 수집 (callback_mutex_ 안)
    lock_guard(callback_mutex_)
    callbacks_to_invoke = [모든 cb 의 copy]

  Phase 3: callback 호출 (모든 락 외부, snapshot 사용)
    for cb in callbacks_to_invoke:
      try { cb(data_snapshot) } catch(...) { MLOG_ERROR }
```

**핵심**: callback 이 SettingManager 의 다른 method (예: GetSetting, RegisterCallback) 를 호출해도 같은 thread 재진입 deadlock 차단.

### 3.4 SettingManagerBase<T>::RenewAfterReset 의 5-phase atomic swap

```
RenewAfterReset(json_array):
  Phase 1: 새 Setter 들 생성 + 초기화 + 기존 callback "훔치기"
    unique_lock(map_mutex_)
    for each unit_id in renew:
      new_setter = make_shared<SetterBase<T>>(lock=true)
      if new_setter->Update(data_array) == true:
        old_it = settings_.find(unit_id)
        if old_it exists:
          stolen_callbacks_map[unit_id] = old_it->second->StealCallbacks()
        map_for_swap[unit_id] = move(new_setter)
      else:
        preparation_ok = false; break
    unlock

  Phase 2: 준비 실패 시 롤백
    if !preparation_ok:
      map_for_swap.clear()
      unique_lock(map_mutex_) → 훔친 callback 들을 기존 setter 에 SetCallbacks 로 되돌림
      return false

  Phase 3: 새 Setter 에 stolen callback 이전
    for new_setter in map_for_swap:
      new_setter->SetCallbacks(move(stolen_callbacks))

  Phase 4: 원자적 map swap
    lock_guard(map_mutex_)
    settings_.swap(map_for_swap)   ← 기존 setter 들은 swap 후 map_for_swap 소멸 시 release
    unlock

  Phase 5: 새 상태에 대한 수동 callback 트리거
    lock(map_mutex_) → setter ptr 모음 → unlock
    for setter in setters_to_trigger:
      setter->TriggerCallbacks()  ← 락 외부 호출
```

**적용된 fix**:
- 1 차 리뷰의 **#10/#11 lock 외부 callback**: callback 호출 시점에 SettingManager 의 어떤 락도 보유하지 않음
- callback 이 GetSetting / Register / Unregister 호출해도 같은 thread 재진입 안전

### 3.5 SettingMonitor RAII Mixin 패턴

```
class RtspDetectorUnit : public SettingMonitor {
  ...
  void OnConstructed() {
    SubscribeSetting<ScheduleSettingData>(
      [this](const ScheduleSettingData& s){ this->ApplySchedule(s); },
      camera_id_);
    SubscribeSetting<ExcludeCamSettingData>(
      [this](const ExcludeCamSettingData& e){ this->ApplyExclude(e); },
      camera_id_);
  }

  ~RtspDetectorUnit() {
    ClearAllSubscriptions();   // [this] 캡처 callback 의 UAF 차단
    // ... 다른 멤버 destroy
  }
};
```

`SubscribeSetting<T>` 가 type-dispatch (if constexpr) 로 적절한 `Register/UnregisterXxxCallback` 선택.

`active_subscriptions_` map 에 unregister lambda 보관 → 소멸 시 자동 해제.

### 3.6 NetworkProfile 의 GRPC 분기

```
NetworkProfile {
  grpc_client_enabled (false)
  grpc_peers []                     // [{name, ip, port}, ...]
  grpc_server_enabled (false)
  grpc_server_bind_address ("0.0.0.0")
  grpc_server_port (0)
}
```

- 4조합 (off/server-only/client-only/both) 모두 지원
- enabled == false 면 GrpcEventClient/Server 인스턴스 생성 자체를 안 함 → 영향 0

---

## §4. Lifetime & Ownership

| 객체 | 소유자 | 수명 |
|---|---|---|
| `SettingManager` (singleton) | function-local static `shared_ptr<SettingManager>` in `GetSingletonSettingManager()` | 프로세스 종료까지 |
| 4개 setter (server/camera/exclude/schedule) | `SettingManager::*` 멤버 (shared_ptr) | SettingManager 종속 |
| `SetterBase::callbacks_` map | shared_ptr 안 멤버 | setter 종속 |
| `CameraCluster_DETECTOR` | `SettingManager::camera_cluster` shared_ptr | SettingManager 종속 |
| `SettingMonitor::active_subscriptions_` | 운영 객체 (e.g., RtspDetectorUnit) 의 base | 객체 종속 |
| Subscription unregister lambda | active_subscriptions_ map | SettingMonitor 종속 |
| `NetworkProfile` / `ServiceProfile` / `EngineProfile` | F1 의 `DETECTOR` 객체 멤버 (struct value) | DETECTOR 종속 |

핵심 패턴: SettingManager 가 4개 setter 를 shared_ptr 로 보유 → 운영 객체는 SettingMonitor::SubscribeSetting 으로 lambda 만 등록 → SettingMonitor 가 unregister lambda 보관 → 객체 소멸 시 ClearAllSubscriptions.

---

## §5. Concurrency

| 영역 | 락 | 보호 대상 |
|---|---|---|
| `SettingManager::initialize_mutex` | `std::mutex` + `atomic<bool> initialization_succeeded` | Initialize 의 1회성 + double-check |
| `SettingManagerBase::map_mutex_` | `std::mutex` | settings_ unordered_map (add/remove/find/swap) |
| `SetterBase::data_update_mtx_` | optional `std::mutex` (`need_data_update_lock_` 분기) | setting_data_ 의 read/write |
| `SetterBase::callback_mutex_` | `std::mutex` | callbacks_ map |
| `SettingMonitor::subscription_mutex_` | `std::mutex` | active_subscriptions_ map |

### Lock ordering (#10/#11 fix 의 핵심)

```
Update 진입 → data_update_mtx_ → UpdateInternal → unlock
            → callback_mutex_ → snapshot callbacks → unlock
            → callback() 호출 (모든 락 외부)
```

callback 이 다시 SettingManager 의 메서드를 호출 → map_mutex_ + data_update_mtx_ + callback_mutex_ 어느 것도 hold 중이 아니므로 재진입 deadlock 안전.

### data_update_mtx_ 가 optional 인 이유

- ServerSetting: read-only after init → lock 불필요 (lock=false)
- Camera/Exclude/Schedule: 동적 갱신 가능 → lock=true

### TriggerCallbacks 의 두 단계 락

```
TriggerCallbacks:
  if need_data_update_lock_: lock(data_update_mtx_)
  data_snapshot = setting_data_
  unlock(data_update_mtx_)        ← scope 종료 (또는 unique_lock unlock)
  lock(callback_mutex_)
  callbacks_to_invoke = [copy]
  unlock(callback_mutex_)
  → 락 외부 호출 cb(snapshot)
```

---

## §6. Findings

### F-F2-01 — `SettingManager::Initialize` 가 throw 잡는 try/catch — ApiHandler::GET_or_throw_if_timeout 의 throw 보호
- **등급**: WARN (CLAUDE.md 규칙)
- **위치**: [SettingManager.cpp:222-273](../../code/Management/manager/src/SettingManager.cpp#L222-L273)
- **내용**: ApiHandler 의 `GET_or_throw_if_timeout` 이 timeout 시 throw. 이를 SettingManager 가 catch.
- **현 영향**: ApiHandler 의 throw 자체가 CLAUDE.md "C++ exceptions 사용 금지" 와 충돌. 외부 라이브러리(curl) timeout 처리는 예외 패턴이 일반적이지만 코드베이스 일관성과 충돌.
- **권장**: ApiHandler 가 `std::optional` 또는 `std::expected` 패턴으로 timeout 반환하도록 변경 후 SettingManager 의 try/catch 제거. F4 분석 시 ApiHandler 검토 시 동시 결정.

### F-F2-02 — `SettingManagerBase::RenewAfterReset` 의 try/catch
- **등급**: WARN (CLAUDE.md 규칙)
- **위치**: [SettingManagerBase.h:245-296](../../code/Management/manager/include/SettingManagerBase.h#L245-L296)
- **내용**: nlohmann::json 접근에서 throw 가능 → catch 로 감쌈. 코딩 규칙 충돌.
- **현 영향**: 외부 라이브러리(nlohmann) throw 보호 차원. 사실상 안전.
- **권장**: nlohmann 의 non-throwing API (`.value()`, `.contains()`) 로 대체하여 try/catch 제거. 큰 작업.

### F-F2-03 — `SettingMonitor` callback 람다의 `[this]` 캡처 → ClearAllSubscriptions 호출 보장 의존
- **등급**: NOTE (이미 fix 적용)
- **위치**: [SettingMonitor.h:120-126](../../code/Management/worker/include/SettingMonitor.h#L120-L126)
- **내용**: subscribe lambda 가 `[this]` capture. 객체 소멸 시 ClearAllSubscriptions() 안 호출하면 dangling. 코멘트로 "멤버 destroy 전에 명시 호출 권장" 명시.
- **현 영향**: 사용자(상속 객체)의 컨벤션 의존. 실제 RtspDetectorUnit 등이 잘 호출하는지 F3 검증 항목.
- **권장**: SettingMonitor 의 dtor 에서 자동 ClearAllSubscriptions 호출하면 더 안전. 단, virtual dispatch 가 dtor 에서 안 되어 derived 의 멤버는 이미 파괴됐을 수 있음 → 기존 패턴 (derived 가 명시 호출) 유지가 정합. NOTE 로 유지.

### F-F2-04 — `SetterBase::Update` 의 callback 호출 try/catch 만 catch — 그러나 catch 내 MLOG_ERROR 만 — 카운터 미증가
- **등급**: NOTE
- **위치**: [SetterBase.h:99-105](../../code/Management/manager/include/SetterBase.h#L99-L105)
- **내용**: callback throw 시 MLOG_ERROR 만 출력. setting_callback_failure_total 같은 메트릭 없음.
- **현 영향**: 운영 중 callback 실패가 외부 관측 불가.
- **권장**: F-F6 의 메트릭 등록과 함께 `errors_total{type="setting_callback"}` 같은 카운터 increment 추가. 가치 중간.

### F-F2-05 — `SettingManagerBase::RenewAfterReset` 이 partial failure 시 모든 변경 롤백
- **등급**: NOTE (긍정 발견)
- **위치**: [SettingManagerBase.h:299-323](../../code/Management/manager/include/SettingManagerBase.h#L299-L323)
- **내용**: phase 1 도중 1개라도 실패하면 phase 2 에서 stolen callback 들을 기존 setter 에 다시 SetCallbacks → 완전한 atomic 보장.
- **권장**: 변경 없음. 좋은 패턴.

### F-F2-06 — `RegisterCallback` 이 unit_id 미존재 시 빈 setter 자동 생성
- **등급**: NOTE
- **위치**: [SettingManagerBase.h:387-395](../../code/Management/manager/include/SettingManagerBase.h#L387-L395)
- **내용**: 카메라가 cluster 에 추가되기 전에 callback 을 미리 등록 가능. 좋은 유연성.
- **현 영향**: 빈 setter 상태에서 GetSetting 시 nullopt 반환 → 호출자가 처리해야 함.
- **권장**: 변경 없음.

### F-F2-07 — `SettingMonitor::SubscribeSetting` 이 ServerSettingData / CameraSettingData 미지원
- **등급**: NOTE (의도된 결정)
- **위치**: [SettingMonitor.h:65-83](../../code/Management/worker/include/SettingMonitor.h#L65-L83)
- **내용**: ScheduleSettingData / ExcludeCamSettingData 만 분기. ServerSetting 은 read-only after init 로 의도. CameraSettingData 도 빠져있음 — 의도/누락 확인 필요.
- **현 영향**: CameraSettingData 의 callback 등록 API (RegisterCameraSettingCallback) 가 SettingManager 에 정의 안 됨. 즉 카메라 정보 동적 변경은 callback 으로 받을 수 없음. 카메라 추가/삭제 시 재시작 필요? 검증 필요.
- **권장**: F3 분석 시 카메라 추가/제거의 동적 처리 시나리오 확인. 누락이면 추가 권장.

### F-F2-08 — `MISSING_UNIT_ID = -99` 와 `UNIT_ID_NOT_SET = -1` 두 상수
- **등급**: INFO
- **위치**: [SettingManagerBase.h:21](../../code/Management/manager/include/SettingManagerBase.h#L21), [MgenTypes.h:59](../../code/BasicLibs/core/types/MgenTypes.h#L59)
- **내용**: -99 와 -1 두 종류. 의미 차이: NOT_SET = 미설정 / MISSING = JSON 에 없음. 의도된 분리.
- **권장**: 변경 없음.

### F-F2-09 — `Initialize` 가 service_tag 분기 — 단일 분기(DETECTOR) 만
- **등급**: INFO
- **위치**: [SettingManager.cpp:229-239](../../code/Management/manager/src/SettingManager.cpp#L229-L239)
- **내용**: `init_data.service_tag == EntireServiceTag::DETECTOR` 분기 외에는 모두 fail. 베이스 프로젝트라 미래 분기 (예: TRACKER, SEARCH 등) 확장 여지로 둠.
- **권장**: 변경 없음.

### F-F2-10 — `GetSearchServerIP` 가 anonymous namespace 내부에서 추가 REST 호출
- **등급**: NOTE
- **위치**: [SettingManager.cpp:16-47](../../code/Management/manager/src/SettingManager.cpp#L16-L47)
- **내용**: ServerSetting 안에 SearchServerId 가 있으면 별도 GET 으로 SearchServerIp 보강. Init 시퀀스 내부에 숨겨진 의존.
- **현 영향**: SearchServerIp 가 timeout 시 ServerSetting 자체는 채워지지만 SearchServerIp 만 빈 문자열. 우회 동작.
- **권장**: 명시적 `EnsureSearchServerIp(setting)` 메서드로 분리하면 가독성 ↑. 가치 낮음.

### F-F2-11 — Default 값 단일 출처 (DefineDefault namespace) — P39 적용 확인
- **등급**: INFO (긍정 발견)
- **위치**: [SettingData.h:18-36](../../code/Management/manager/include/SettingData.h#L18-L36)
- **내용**: ServerSettingData 의 멤버 초기값들이 DefineDefault 의 단일 진실 사용. P39 (DRY) 적용 완료.

---

## §7. Open Questions

1. **F-F2-01 / F-F2-02**: ApiHandler::GET_or_throw_if_timeout 과 nlohmann throw 의 try/catch 제거 여부? 큰 작업이고 외부 라이브러리 패턴이라 트레이드오프. 결정 필요.
2. **F-F2-07**: CameraSettingData 의 동적 변경 처리 누락 — 의도인가 추가 필요인가? F3 검증 후 결정.
3. **F-F2-04**: Setting callback 실패 메트릭 추가? F6 메트릭 확장 항목.

---

## §8. Self-Check

- [x] Primary 파일 읽음 — profile 6개 + interface 2개 + manager 6개 + worker 2개 + SettingData 1개 = 17개 (.h/.cpp 포함)
- [x] §3 호출 시퀀스 — Init / Update / RenewAfterReset / Subscribe 모두 코드 출처 + Phase 단위 명시
- [x] §4 소유권 — singleton + shared_ptr + RAII 모두 구분
- [x] §5 락·CV — initialize_mutex / map_mutex_ / data_update_mtx_ / callback_mutex_ / subscription_mutex_ 5개 모두 위치 + 보호 대상 명시. lock ordering 문서화
- [x] §6 Finding 등급 + 출처
- [x] 추측 표시 — "검증 필요" (CameraSettingData 동적 변경, callback 메트릭 등)
- [x] Also-touches 라벨 모순 없음

**검증 결과**: PASS

**보강 필요 항목**:
- F1 분석 시: SettingManager::Initialize 호출 시점이 Logger init 후 / Network/Profile load 후 / DETECTOR 부팅 직전인지 → Stage 순서 검증
- F3 분석 시: RtspDetectorUnit 의 SettingMonitor 사용 (SubscribeSetting / ClearAllSubscriptions 호출 패턴) → F-F2-03, F-F2-07 결정
- F4 분석 시: ApiHandler::GET_or_throw_if_timeout 의 throw 패턴 → F-F2-01 결정
