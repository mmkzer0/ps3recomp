# RUN.md: lv2conf on a real console

lv2conf is a differential syscall conformance suite. The same case sources (`cases/`) run on a console, as the
hwprobe SELF built from `layer1/`, and on the host against the toolkit's HLE; both write the rows of `FORMAT.md`.
A console run is the golden reference: where the toolkit disagrees with it, the toolkit is wrong. The probe calls
only lv2 syscalls (by number) and patches nothing; it touches no file outside `/dev_hdd0/tmp/hwprobe/`.

Build, copy and the console-side mechanics are in `tools/hwprobe/README.md`. Commands below are run from the
repository root with `export HWPROBE_CONSOLE=ADDR`.

## 1. Build and deploy

```
tools/hwprobe/build.sh lv2conf layer1                 # -> tools/hwprobe/out/lv2conf_l1.fake.self, no compiler warnings
tools/hwprobe/ps3ftp.py put tools/hwprobe/out/lv2conf_l1.fake.self
tools/hwprobe/ps3ftp.py put-cfg lv2conf tools/hwprobe/probes/lv2conf/cfg/full.cfg      # unattended, all cases
```

The portable check of the cases themselves, without the toolchain:

```
cc -std=c99 -fsyntax-only -Wall -Wextra -I tools/hwprobe/probes/lv2conf tools/hwprobe/probes/lv2conf/cases/*.c
```

## 2. Launch

With webMAN MOD (see `README.md`, "Running the SELF"):

```
GET /mount.ps3/dev_hdd0/tmp/hwprobe/lv2conf_l1.fake.self
GET /play.ps3
```

The first play after a probe has finished usually ends in an XMB error `8001003A` and no file; play again about
20 s later. **Do not unmount (`/mount.ps3/unmount`) before the result file is fetched**: leaving the mapped image
while the probe runs ends it (the probe sees the exit request, stops after the current case and says so in
`META,exit_requested,1`; `cases_run + cases_skipped` is then below `cases_total`).

While a run is in progress `lv2conf_last.txt` reads `(running)` and names the case in progress (`<k>/<n> <id>`),
written before every case; after the run it names the result file (first line) and `cases_run=... skipped=...`.
Fetch when the first line is a `.csv` that was not there before:

```
tools/hwprobe/ps3ftp.py get results --prefix lv2conf
```

## 3. Configuration

`cfg/*.cfg`, uploaded as `lv2conf.cfg` (`put-cfg`); read from `/dev_usb000/lv2conf.cfg` first. Keys: `pad` (1 wait for
START before the run and before exit, 0 unattended), `hold_s` (with `pad=0`, keep the final screen this long),
`screen` (0 leaves video untouched), `tag` (names the run; part of the result file name), `outdir`, `fw_version`,
`fw_type`, `console_model` (declare the console: the probe cannot read them), `groups=a,b` (run only these
groups; the others are written as `SKIP,<id>,not_selected`), `skip=` (case ids or groups not to run,
`SKIP,<id>,cfg_skip`), `case_limit_s` (watchdog, default 30: a case that stays longer ends the run, which writes
what it has and `META,aborted_at`), `progress_file` (1 mirrors the case in progress to `lv2conf_last.txt`).

`cfg/full.cfg` all cases; `cfg/smoke.cfg` the groups `time` and `process` only; `cfg/nothreads.cfg` everything
except the cases that start a second PPU thread.

## 4. Check a run

A run is complete when `META,cases_run` + `META,cases_skipped` = `META,cases_total`, there is no `META,aborted_at`,
`exit_requested` is 0, `arena_overflows` and `rows_truncated` are 0 and `ui_overlaps` is 0. Repeat runs must give
identical `CASE` rows (`TIME` rows differ).

## 5. Case groups

| Group | What it checks |
|---|---|
| `time` | timebase frequency, wall clock, time zone |
| `mutex`, `cond`, `semaphore`, `rwlock` | create/destroy attribute validation, lock state machines, error codes, object counts |
| `event` | event queues and ports: create/destroy, tryreceive, drain, send, a full queue, receive timeouts, connect/disconnect |
| `eventflag` | create validation, set/clear/get, trywait modes, timeouts |
| `memory`, `container` | allocate/free size and flag validation, alignment, available-memory arithmetic, page attributes; memory containers on slots 341/342 (and 324/325), `allocate_from_container` |
| `process`, `ppu_thread`, `timer` | pid relations, object listing, thread priority and stack calls without a second thread, timers and sleeps |
| `slot96` | the slots named by issue #96 called with arguments that name nothing |
| `threads` | cases that need a second PPU thread (contention, wake-ups, cancel, priority of a child, join/detach); `CONF_THREADS` |

## 6. Cases not on the console (and why)

Cobra/webMAN CFW hooks several syscall slots that stock firmware leaves unassigned (lv2 peek/poke 6 and 7, the Cobra
call 8, lv1 peek/poke 9 and 10, 11, the lv2 call 15, and others in the low range), and the table beyond slot 1023
is not defined at all. A call with zero or garbage arguments to such a slot can write kernel memory or jump, and its
answer says nothing about stock firmware. These cases were therefore removed from the suite; the host backend reports
empty slots through its own named-stop rows:

| Removed case | What it called |
|---|---|
| `slots.unsupported_process` | 6-11, 15-17, 20 |
| `slots.unsupported_thread_range` | 42, 59, 79 |
| `slots.unsupported_far` | 316-323, 420-429, 730-739 |
| `slots.beyond_table` | 1024, 4096 |
| `ppu_thread.slot_42` | 42 |
| `time.syscall_149` | 149 (RPCS3: unassigned; the console answered `CELL_ENOSYS`) |

`slots.container_neighbours` became `container.neighbour_slots` (the slots 341/342/343/324/325 are assigned).

What happened (2026-11-04, CECHC04, REBUG 4.81 CEX, Cobra 7.5, webMAN MOD 1.47.50): the first full run (195 cases)
froze the console, which stopped answering ping and FTP and had to be power-cycled. The screen read case 167/195,
`slots.unsupported_process`, which called slots 6, 7, 8, 9, 10, 11, 15, 16, 17 and 20 in this order with all
arguments zero; which number it was in when the console froze is not known (the progress marker is per case).
Slots 6 to 11 and 15 are Cobra's, so this is the likely cause. After the power-cycle the groups `time`, `process`,
`mutex`, `cond`, `semaphore`, `rwlock`, `eventflag`, `event`, `timer`, `ppu_thread`, `memory`, `container`,
`slot96` and `threads` were run one at a time (`groups=` in the config) without another hang.

## 7. Hardware results (2026-11-04)

Console: CECHC04, REBUG 4.81 CEX, Cobra 7.5, webMAN MOD 1.47.50. Build `describe v0.18.0-369-g4047733c092a-dirty`,
source sha256 `be3da1c7181d4b65c3f48d5860e7a35815c0be7ef99b3c900f9927cbae12b337`. Three unattended full runs
(`cfg/full.cfg`, runs 17 to 19 of the console's counter): 189 of 189 cases each, no skip, no abort, `exit_requested` 0,
`arena_overflows` 0, `rows_truncated` 0, `ui_overlaps` 0 (189 timed sections); the 736 `CASE`/`SKIP` rows are
byte-identical in all three; `TIME` rows differ. Each full run takes about 15 s once the SELF starts; the first
`/play.ps3` after a finished run ended in XMB error `8001003A` all three times and the second one, 28 s later, started it.
Results, the per-group runs after the power-cycle and `SHA256SUMS` are in
`~/ps3-recon/odin-sphere-m0/scratch/m4/lv2conf/hw/2026-11-04/` (private recon tree, not in the repository).

Cases per group: event 27, eventflag 19, threads 18, mutex 17, semaphore 16, rwlock 15, memory 15, timer 13,
container 12, cond 12, time 7, slot96 6, process 6, ppu_thread 6 (189; 18 of them `CONF_THREADS`, 2 `CONF_TIMING`).

Two defects of an earlier `tools/hwprobe/ps3console.py run` cut the first two runs of this probe short
(`META,exit_requested,1`, `cases_run` 3 and 1 of the selected cases): the FTP control connection raised `EOFError('')`
a few seconds after the SELF started and was not retried, and the `finally` then unmounted the image. Both are fixed
(`cd7b79fe`): every poll and fetch reconnects, and the image is unmounted only after the results are fetched, or left
mounted with the error `console left mounted; probe may still be running` when the probe may still be running. The
three full runs above were launched with `/mount.ps3/...` and `/play.ps3` over HTTP and fetched over FTP with a fresh
connection per poll, the image unmounted only after the result was fetched and the probe's hold time had passed. Runs
20 and 21 were launched with `ps3console.py run` (`--prefix lv2conf --cfg lv2conf <copy>`, where the copy is
`cfg/full.cfg` plus `fw_version=4.81`, `fw_type=CEX REBUG Cobra 7.5`, `console_model=CECHC04`, so the header names the
console); run 21 is the golden (`tests/syscalls/lv2conf/golden.csv`), and the `CASE` rows of runs 17 to 21 are identical.
