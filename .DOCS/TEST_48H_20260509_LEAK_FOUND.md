# 48시간 운영 안정성 테스트 — 종합 보고

> **NOTE (2026-05-15)**: 파일명의 "LEAK_FOUND" 는 oversimplified — 실제 본 테스트의 결론은 **leak 없음 (ERROR 0, FD/Thread 안정)**.
> 다만 후반 10시간에 RSS +47 MB 증가 (glibc ptmalloc fragmentation 으로 추정) 관측 → 후속 W-14 patch (`malloc_trim(0)` in `RtspDetectorUnit.cpp:231`) 와 jemalloc 도입 (2026-05-14) 의 trigger 가 됨.
> AI 가 별도 branch 에서 `git mv` 로 정리 가능 (예: `TEST_48H_20260509_RESULT.md` 로 rename). 단 `master` 머지는 PR + 사용자 명시 승인. 파일명만 오해 소지, 내용은 유효.

---

**기간**: 2026-05-09 20:59:40 ~ 2026-05-11 21:00:02 KST
**snapshot**: 49 개 (1시간 간격)

## §1. 핵심 추이

### baseline (snap 0) vs final (snap 48)

```
[ baseline ]
===== snapshot 0 at 2026-05-09 20:59:40 KST =====
elapsed: 0 min (0h 0m)

----- container status -----
name=detectbase_service status=Up 8 hours

----- main process -----
    PID     ELAPSED    VSZ   RSS %CPU %MEM COMMAND
      1    07:56:12 2302740 554696 212  3.3 DetectBase

----- /proc/1/status -----
VmPeak:	 2433812 kB
VmSize:	 2302740 kB
VmHWM:	  572304 kB
VmRSS:	  554696 kB
RssAnon:	  487000 kB
RssFile:	   48664 kB
VmData:	  841800 kB
Threads:	37

----- file descriptors -----
26
fd_count above

----- ERROR / WARN 누적 -----
ERROR: 0
0
WARN:  5
INFO:  85726

----- key metrics -----
detectbase_frame_emergency_cleanup_total{type="half_files"} 86456
detectbase_dfps_total 53.09999847412109
detectbase_camera_count{state="active"} 4
detectbase_camera_count{state="registered"} 4
detectbase_frame_disk_used_bytes 36970078208
detectbase_frame_disk_capacity_bytes 60963737600
detectbase_frame_disk_used_pct 60.64273560550198

----- 디스크 사용량 -----
/dev/disk/by-uuid/e63616e6-fcb2-44f9-ad61-52d05754f135   57G   33G   23G  59% /
/dev/disk/by-uuid/e63616e6-fcb2-44f9-ad61-52d05754f135   57G   33G   23G  59% /

[ final ]
===== snapshot 48 at 2026-05-11 21:00:02 KST =====
elapsed: 2880 min (48h 0m)

----- container status -----
name=detectbase_service status=Up 2 days

----- main process -----
    PID     ELAPSED    VSZ   RSS %CPU %MEM COMMAND
      1  2-07:56:34 2304012 602984 212  3.6 DetectBase

----- /proc/1/status -----
VmPeak:	 2566156 kB
VmSize:	 2304012 kB
VmHWM:	  605876 kB
VmRSS:	  602984 kB
RssAnon:	  536596 kB
RssFile:	   47356 kB
VmData:	  900412 kB
Threads:	37

----- file descriptors -----
26
fd_count above

----- ERROR / WARN 누적 -----
ERROR: 0
0
WARN:  12
INFO:  226812

----- key metrics -----
detectbase_frame_emergency_cleanup_total{type="day_dir"} 2
detectbase_frame_emergency_cleanup_total{type="half_files"} 526549
detectbase_dfps_total 53.59999847412109
detectbase_camera_count{state="active"} 4
detectbase_camera_count{state="registered"} 4
detectbase_frame_disk_used_bytes 39049490432
detectbase_frame_disk_capacity_bytes 60963737600
detectbase_frame_disk_used_pct 64.05363576658397

----- 디스크 사용량 -----
/dev/disk/by-uuid/e63616e6-fcb2-44f9-ad61-52d05754f135   57G   35G   21G  63% /
/dev/disk/by-uuid/e63616e6-fcb2-44f9-ad61-52d05754f135   57G   35G   21G  63% /
```

## §2. ERROR / WARN 추이

| snap | elapsed | ERROR | WARN |
|---|---|---|---|
