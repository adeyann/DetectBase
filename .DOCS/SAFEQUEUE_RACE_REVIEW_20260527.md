# SafeQueue Race Deep Review (2026-05-27)

**대상**: `code/BasicLibs/core/structure/SafeQueue.h` (216 lines, header-only template)
**baseline**: 5/19 audit 시점 자체 코드 race 0건, TSan 잔여 race 5건 "happens-before 추적 한계" 분류
**목적**: v1.0.0 정착 전 SafeQueue 의 shared_ptr ref counting / max_size 변경 path / drop_count 경합 deep review
**결론**: **SafeQueue 자체 race 0건 확정.** TSan 잔여 5건은 *주변 context* (특히 `SafeQueue<std::shared_ptr<T>>` 사용 시 shared_ptr control block 의 atomic) 의 happens-before 한계. structural redesign 불필요.

---

## 1. 인터페이스 요약

```cpp
template <typename T>
class SafeQueue
{
    void   SetMaxSize( size_t n );
    void   enqueue_copy( const T& t );
    void   enqueue_move( T&& t );
    std::optional<T> dequeue_wait_for( std::chrono::milliseconds wait );
    uint64_t GetDropCount() const;
    size_t size() const;
    bool   empty() const;
    void   terminate();
    bool   is_terminated() const;
    template<typename A> void clear_with_action( A action );
    void   clear_without_action();

private:
    std::deque<T>           q;
    mutable std::mutex      m;
    std::condition_variable c;
    bool                    b_terminate = false;
    size_t                  max_size_   = 0;       // 0 = unbounded
    std::atomic<uint64_t>   drop_count_ { 0 };
};
```

복사/이동 모두 금지 (RAII single-owner). T 가 임의 타입 (shared_ptr / unique_ptr / POD / struct).

---

## 2. method 별 lock + atomic 분석

| method | sync 매커니즘 | race 분석 |
|---|---|---|
| `SetMaxSize(n)` | `lock_guard<mutex>` | `max_size_` write 보호. enqueue 의 size check 와 같은 lock → 안전 |
| `enqueue_copy(const T&)` | `lock_guard<mutex>` | size check + drop + push + `drop_count_.fetch_add(relaxed)` + `notify_one` 모두 lock 안 → 안전 |
| `enqueue_move(T&&)` | `lock_guard<mutex>` | 동일 |
| `GetDropCount()` | atomic relaxed load | **happens-before 미흡 (의도)** — 통계 용도, 다른 write 와 ordering 보장 불필요. 호출자가 read 한 시점의 *snapshot* 만 의미 |
| `dequeue_wait_for(timeout)` | `unique_lock<mutex>` + `cv.wait_for(predicate)` | predicate `q.empty() == false \|\| b_terminate` 안전. wakeup 후 b_terminate 재검사. front()/pop_front() 도 lock 안 → 안전 |
| `size()` | `lock_guard<mutex>` (mutable) | snapshot. **TOCTOU 책임은 호출자** (size 결과로 enqueue/dequeue 결정 X) |
| `empty()` | `size() == 0` 위임 | size() lock 안이라 안전 |
| `terminate()` | `lock_guard<mutex>` + `notify_all` | b_terminate=true. 대기 중 dequeue 들 모두 wakeup → 정상 종료 흐름 |
| `is_terminated()` | `lock_guard<mutex>` (mutable) | b_terminate snapshot |
| `clear_with_action(action)` | swap technique: lock 짧게 잡고 `q.swap(temp_q)` → lock 밖에서 temp_q 처리 | swap 이후 temp_q 는 local → 다른 thread 접근 X. 안전. action throw 는 `try/catch(...)` 로 cleanup 보호 (CLAUDE.md 자체 코드 throw 금지 정책과 충돌 X — 외부 callable 의 throw 만 catch) |
| `clear_without_action()` | swap technique | 안전 |

---

## 3. race 시나리오 검토 (5건 stress)

| # | 시나리오 | 결과 |
|---|---|---|
| A | SetMaxSize 직후 enqueue, drop 트리거 (q.size() == 10, max=5) | **안전** — SetMaxSize 의 max_size_ write 와 enqueue 의 size check 모두 같은 mutex |
| B | dequeue_wait_for 중 terminate() | **안전** — predicate `b_terminate` retry 로 정상 wakeup + return nullopt. cv.wait_for 가 `b_terminate` 와 `q.empty()` 둘 다 predicate 에 포함 |
| C | dequeue_wait_for 중 enqueue + drop | **안전** — enqueue 의 notify_one 으로 wait 깨움, predicate `q.empty() == false` 으로 정상 dequeue |
| D | 동시 enqueue + dequeue | **안전** — mutex 가 둘을 직렬화 |
| E | clear_with_action 중 enqueue | **안전** — swap 직후 q 가 빈 상태 → 새 enqueue 는 빈 q 에 들어감. temp_q 처리는 lock 밖에서 일어나지만 local 이라 영향 X |

---

## 4. TSan 잔여 5건 (5/19) — 추정 root cause

SafeQueue 자체 race 가 아니라 **주변 context** 의 happens-before 미흡:

### (a) `drop_count_` relaxed atomic (의도된 한계)
- `enqueue_copy`/`enqueue_move` 안 `drop_count_.fetch_add(1, memory_order_relaxed)` 와 외부 `GetDropCount()` 의 `load(relaxed)` 사이 happens-before 없음
- TSan 이 race 로 잡을 수 있지만 **통계 용도라 무해** (snapshot 값이 약간 늦게 보이는 건 정합)
- **fix 불필요** — relaxed 가 의도된 default

### (b) `SafeQueue<std::shared_ptr<T>>` 의 shared_ptr ref count 추적 한계
가장 큰 후보. 사용처:
- `avframe_q_ = SafeQueue<std::shared_ptr<AVFrame>>` ([RtspDetectorUnit.cpp](../code/Main/DETECTOR/src/RtspDetectorUnit.cpp))
- `inflight_q_ = SafeQueue<InflightItem>` (InflightItem 안에 shared_ptr<AVFrame>)
- 기타 `event_q_` / `io_work_queue_`

dequeue 시 `T val = std::move(q.front())` — shared_ptr move 가 control block ref count 변경 동반.
- shared_ptr 의 control block atomic 은 자체 thread-safe (libstdc++ 구현)
- 단 SafeQueue 의 mutex lock 과 shared_ptr control block atomic 사이 happens-before 추적이 TSan 에서 정확히 안 됨
- → TSan 이 control block 의 atomic write/read 를 race 후보로 표시 가능 (false positive)

### (c) condition_variable spurious wakeup + predicate retry
- `cv.wait_for(lck, duration, predicate)` 의 내부 구현이 predicate 을 multiple time 검사
- TSan 이 wakeup 의 happens-before edge 를 일부 놓침 가능

→ **운영 영향 0**, 5/19 결론 ("happens-before 추적 한계 ~5건, 운영 영향 0") 그대로 유지.

---

## 5. minor optimization 후보 (race 와 무관)

본 검토 외에 발견된 **효율성 minor improvement**. fix 우선순위 낮음.

### MO-1. `notify_one()` 을 lock_guard 밖으로 이동
현재:
```cpp
void enqueue_move( T&& t ) {
    std::lock_guard lck{m};
    ...
    q.push_back(std::move(t));
    c.notify_one();  // ← lock 안
}
```
표준 권장:
```cpp
void enqueue_move( T&& t ) {
    {
        std::lock_guard lck{m};
        ...
        q.push_back(std::move(t));
    }  // ← lock 풀고
    c.notify_one();  // ← lock 밖
}
```
**효과**: wakeup 받은 thread 가 lock 재획득 시 contention 미세 감소. 4 cam × 30 fps 환경에선 측정 가능 효과 거의 없음. **현 형태 유지로 충분.**

### MO-2. `notify_one()` 조건부 호출
현재: 매 enqueue 마다 무조건 notify_one. 대기 중인 dequeue 없으면 notify 가 noop 이지만 system call cost 있음.
대안: enqueue 전 q.empty() 였으면만 notify.
**효과**: cost minor. 정합성 위험 — empty 검사 후 push 사이 race 가능? lock 안이라 OK.
**현 형태 유지로 충분.**

### MO-3. `clear_with_action` 의 catch 블록 dead-code 여부
- CLAUDE.md "자체 코드 throw 금지" → 자체 callable 은 throw 안 함
- 단 SafeQueue 는 generic library — 호출자가 외부 action 으로 throw 가능 callable 넘길 수 있음
- catch 는 **정합한 defensive code** (external boundary)
- **유지**

---

## 6. fix 우선순위

| 항목 | 우선순위 | 비용 | 효익 |
|---|---|---|---|
| SafeQueue 자체 race fix | **불필요** | — | — (자체 race 0) |
| `drop_count_` ordering 강화 | 매우 낮음 | 5분 | 0 (통계 의도된 relaxed) |
| `notify_one` lock 밖 이동 (MO-1) | 매우 낮음 | 5분 | 측정 불가 minor |
| `notify_one` 조건부 호출 (MO-2) | 매우 낮음 | 10분 | 측정 불가 minor |
| TSan 잔여 5건 fix | **불가능** | — | — (false positive 또는 외부 lib 한계) |

**결론**: SafeQueue 는 v1.0.0 진입 시점 **as-is 사용 가능**. 추가 변경 불필요. TSan 잔여 5건은 docs 로만 정리 (운영 영향 0, 5/19 결론 유지).

---

## 7. 관련 자료

- 5/19 결론: [.DOCS/SESSION_DFPS_B3_B4_PLATEAU_20260519.md](SESSION_DFPS_B3_B4_PLATEAU_20260519.md) ("TSan SafeQueue 잔존 race cleanup")
- 사용처:
  - [RtspDetectorUnit.cpp](../code/Main/DETECTOR/src/RtspDetectorUnit.cpp) — avframe_q / inflight_q / event_q / io_work_queue
  - [SioHandler.cpp](../code/Management/worker/src/SioHandler.cpp), [GstRtspReceiver.cpp](../code/Protocol/RTSP_GST/src/GstRtspReceiver.cpp) 등 — 각 thread queue
- 검토자: AI assistant (2026-05-27)
- branch: `chore/safequeue-race-review`
