---
name: monitoring
description: Must read before any monitoring or polling task. Defines how to use Monitor tool for long-running observation, sanity check patterns (RSS / DFPS / ERROR), and the rule against ad-hoc sleep-loop polling. Triggered on tasks like sanity monitoring, leak detection, RSS tracking, log following, deployment watching.
---

# Monitoring & Polling

## First Principle — Use Monitor, not nohup-sleep-loops

When a task requires watching state over time, use the **Monitor tool**. It streams each stdout line as a notification, lets the main agent keep working, and self-terminates on timeout or exit.

**Forbidden**: ad-hoc `nohup bash -c 'for i in 1..N; do sleep 600; ...; done' &` patterns. They detach from the harness, produce no notifications, and require manual polling. Migrate to Monitor.

## When to Use Each Tool

| Need | Tool | Reason |
|---|---|---|
| One notification when X finishes (build, deploy, single curl) | **Bash with `run_in_background`** + an `until ...; do sleep N; done` exit condition | Single completion event. Cache-friendly. |
| One event per occurrence (each new ERROR line, each PR check, each measurement tick) | **Monitor** | Each stdout line = one notification. Main agent stays free. |
| Continuous tail with selective filtering (errors only, specific patterns) | **Monitor** with `tail -f ... \| grep --line-buffered ...` | Filter at the source — only useful events become notifications. |
| Single quick check (current RSS, current DFPS) | **Bash** direct | Microseconds — Monitor overhead pointless. |

## Sanity Monitoring Pattern (this project)

For DetectBase runtime sanity (RSS leak / DFPS drop / ERROR check), use 10-minute interval over 1 hour:

```bash
RSS0=""
for i in 1 2 3 4 5 6; do
    sleep 600
    PID=$(docker inspect -f '{{.State.Pid}}' detectbase_service 2>/dev/null)
    if [[ -z "$PID" || "$PID" == "0" ]]; then
        echo "[+${i}0m] CONTAINER_DOWN"
        continue
    fi
    M=$(curl -s --max-time 3 http://localhost:9090/metrics 2>/dev/null)
    DFPS=$(echo "$M" | awk '/^detectbase_dfps_total / {printf "%.1f", $2}')
    CAM=$(echo "$M" | grep '^detectbase_camera_count{state="active"' | awk '{print $2}')
    EVT=$(echo "$M" | awk '/^detectbase_events_total\{/ {sum += $2} END {printf "%d", sum+0}')
    RSS=$(grep '^VmRSS:' /proc/$PID/status 2>/dev/null | awk '{print $2}')
    ERR=$(grep -c '"lvl":"ERROR"' /home/claudedev/DetectBase/logs/DetectBase.log 2>/dev/null)
    [[ -z "$RSS0" ]] && RSS0=$RSS
    DIFF=$(( RSS - RSS0 ))
    echo "[+${i}0m] DFPS=${DFPS} cam=${CAM} evt=${EVT} RSS=${RSS}KB(d+${DIFF}) ERR=${ERR}"
done
```

Launch via Monitor:
- `description`: e.g., `"happytimesoft baseline 60min sanity (10min × 6)"`
- `timeout_ms`: `3600000` (1 h max), or shorter for shorter watches
- `persistent`: `false` (auto-end on exit)

**Coverage (silence is not success)**: the script must emit on container death too (the `CONTAINER_DOWN` branch above). If RSS grep silently fails, a hang looks identical to a crash.

## RSS Reading Heuristics

Distinguish ramp-up from leak before declaring failure:

| Pattern | 10-min deltas | Diagnosis |
|---|---|---|
| ramp-up + plateau | +12, +5, +8, then **negative** (e.g., −19) | Normal. Working set fills, then jemalloc `background_thread` reclaims. |
| Slow leak | +5, +5, +5, +5 — sustained | Investigate. Check reconnect path, NPU pool, queue accumulation. |
| Burst growth | +30, +1, +1 | One-time large allocation. Compare against known patterns (e.g., `fs::directory_iterator` burst during emergency cleanup). |
| Hard leak | +50, +60, +70 | Stop the run, snapshot `/proc/PID/smaps`, capture with ASan or pmap. |

For this project specifically: **48h baseline tolerates +47 MB / 48 h** (RtspDetectorUnit.cpp:230 comment). Anything within that envelope after plateau is normal. Plateau reaches around Up 60~80 min on this system.

## Metric Endpoints (this project)

- Endpoint: `http://localhost:9090/metrics`
- Key gauges/counters:
  - `detectbase_dfps_total` (gauge) — detection FPS across all cams (no fixed target; track stability/plateau, not an absolute number)
  - `detectbase_camera_count{state="active"}` (gauge) — target: equal to `state="registered"`
  - `detectbase_events_total{type=...,cam=...}` (counter) — should keep increasing in production
  - `detectbase_errors_total{type=...}` (counter) — should stay near 0
  - `detectbase_frame_disk_used_pct` (gauge) — < 80% normal
- RSS not in metrics → read `/proc/$(docker inspect -f '{{.State.Pid}}' detectbase_service)/status` (VmRSS) instead.

## Monitor Tool Pitfalls

| Pitfall | Avoid by |
|---|---|
| Pipe buffering delays events by minutes | Always `grep --line-buffered` / `awk` (line-buffered) / `stdbuf -oL` in pipelines |
| Transient curl/grep failure kills the loop | Append `\|\| true` or guard with `if ...; then ... fi` for non-critical fetches |
| Silent on crash — filter only matches success | Always include failure signatures (`grep -E "OK\|FAIL\|Error\|Killed\|OOM"`) |
| Too many events → monitor auto-stopped | Tighten filter. Don't pipe raw `tail -f` without grep. |
| Unbounded command for "one notification" | Wrong tool. Use Bash `run_in_background` with `until <check>; do sleep N; done` for one-shot. |

## Polling Discipline

- **No background `nohup` polling loops.** Migrate to Monitor.
- **No `sleep 3600` leading commands.** The runtime blocks long leading sleeps; use Monitor with internal sleep loops or `until ...; do sleep N; done` inside `run_in_background`.
- **When waiting for a `run_in_background` Bash task**: do *not* poll. You are auto-notified on completion. Continue with other work or wait silently.
- **When waiting for an external/manual event** (user pasting a result, manual config change): wait silently. No timer.
- **Cache TTL awareness**: Anthropic prompt cache is 5 min. For polling cadence in Monitor scripts, prefer 270 s (stay cached) or 1200 s+ (single cache miss buys a long wait). Don't pick exactly 300 s — worst of both.

## Notification Discipline

Each Monitor stdout line becomes a chat notification. The user may be away. Be selective:
- Emit one line per *meaningful tick* (T+10m measurement, status change, terminal event).
- Don't emit raw log lines — filter to actionable signal.
- Routine OK lines are fine; reserve `PushNotification` for status flips (e.g., `ERROR` appeared, sanity FAILED, the build the user was waiting on completed).

## After a Monitor Ends

If results live in a file (not stdout), the main agent should read the file in a single follow-up step. Don't re-arm a Monitor for the same data already captured.

If a timeout cut measurements short (Monitor's `timeout_ms` is max 3,600,000 = 1 h), a **single spot measurement** afterwards usually settles the question (e.g., one more `grep VmRSS /proc/PID/status` shows whether plateau was reached).
