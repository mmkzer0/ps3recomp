# Configuration

The runtime's behavioural options live in one typed registry
([`include/ps3emu/config_options.def`](../include/ps3emu/config_options.def)).
Each option has a type, a default and a documented meaning; each is read through
a generated accessor (`ps3_cfg_<section>_<key>()`), never by name. Anything wrong
with the configuration is a [named stop](STOPS.md), `config-invalid`: nothing is
ignored, so a typo cannot leave an option at a value nobody chose.

API: [`include/ps3emu/config.h`](../include/ps3emu/config.h). Implementation:
`runtime/config/config.cpp` (parsing by the vendored
[toml++](../third_party/tomlplusplus/README.md)).

## Sources and precedence

```
built-in default  <  config file (TOML)  <  environment
```

- **File.** A title passes its own config path to `ps3_config_load()` (or `NULL`
  for none). `PS3RECOMP_CONFIG=<path>` names the file instead and wins over the
  path the title built in. A named file that cannot be read is a stop.
- **Environment.** `PS3RECOMP_<SECTION>_<KEY>`, upper case: `ppu.vscr_nj` is
  `PS3RECOMP_PPU_VSCR_NJ`. Booleans are `1`, `0`, `true` or `false`. Unsigned
  32-bit integers are decimal or `0x` hexadecimal (`0x200` and `512` are the same
  value); a sign, whitespace or trailing text is refused rather than wrapped.
- **Typos are errors.** Any `PS3RECOMP_*` variable that is not an option is a stop
  and names the closest real one. The exceptions are `PS3RECOMP_CONFIG` and the
  diagnostic and host display variables that existing code still reads directly
  (`PS3_CFG_LEGACY_ENV` in the registry); moving one of those into the registry is
  how it leaves that list. `tests/gates/test_env_names.py` fails when a
  `PS3RECOMP_*` literal read in `libs/` or `runtime/` is in neither the typed
  keys nor that list.

## File format

TOML. The first thing in the file must be the schema version:

```toml
schema = 2

[ppu]
vscr_nj = false

[title]
elf_sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
```

`schema` is required and must equal the version the runtime reads
(`PS3_CONFIG_SCHEMA_VERSION`, currently 2); any other value is refused before
the rest of the file is looked at. The version changes when an option is changed
or removed, never when one is added.

An unknown section or key, a value of the wrong type, a value out of range and a
TOML syntax error all stop, naming the file, line, column and key. Integer
(`u32`) options additionally refuse a value outside their documented range,
including a negative one:

```
PS3STOP v=1 kind=config-invalid file=title.toml line=3 col=1 key=ppu.vscr_nk detail="unknown key (did you mean 'vscr_nj'?)"
```

## Options

| Key | Type | Default | Environment | Meaning |
|---|---|---|---|---|
| `ppu.vscr_nj` | bool | `true` | `PS3RECOMP_PPU_VSCR_NJ` | Initial VSCR[NJ] of every guest PPU context. Default chosen for RPCS3 parity; a policy, not a hardware fact |
| `ppu.opd_swap_fixup` | bool | `false` | `PS3RECOMP_PPU_OPD_SWAP_FIXUP` | Redirect an indirect call whose `ctr` is not a lifted function but whose `r2` is (the guest wrote the function descriptor as `{toc, code}` instead of `{code, toc}`). The redirect compensates for a malformed title descriptor (flOw-style handler tables), not a PPC ABI guarantee; off, the call stops as `unresolved-indirect-call` and its hint names this option |
| `ppu.null_page` | string (`stop` \| `drop`) | `stop` | `PS3RECOMP_PPU_NULL_PAGE` | What a guest load below `0x10000` or store below `0x1000` does. `stop`: the run ends at the first one with kind `guest-null-access` (`access width ea fn`, plus `pc lr tid`). `drop`: the legacy handling for a title that needs it, a load logs `[null-read]` and returns what memory holds, a store logs `[null-write]` and is dropped, and a store sweep that starts in the null page (a memset on a null buffer) stays dropped until it breaks off. Hardware faults on every such access, so `drop` papers over a guest defect; a title config that sets it must carry a written reason (see below) |
| `ppu.poll_sleep_coalesce` | bool | `false` | `PS3RECOMP_PPU_POLL_SLEEP_COALESCE` | POSIX only. Honour the usleep-site seams a title registers (`ps3_usleep_site_register`, `ps3_usleep_flip_wait_register`, `runtime/syscalls/sys_timer.h`). A guest limiter that polls `usleep(30)` against a deadline the title can compute (350-400 calls and ~26000 host wakeups a frame) sleeps once to the deadline instead, spinning the last `ppu.poll_sleep_spin_us`; its own branches, loads and stores are unchanged. Only a deadline in `(now, now + max_sleep]` takes the seam (the bound is the title's registration); a deadline that is past, zero or beyond it takes the ordinary `sys_timer_usleep`, and one beyond the bound logs `[usleep] poll-sleep-bound` once. A pending GCM vblank, flip or user-command notification ends the sleep early, the handler is delivered on that thread as an HLE boundary would deliver it, and the sleep resumes. Visible: the limiter's loop ends at its deadline instead of 17 us after it, so a frame limiter ticks about 0.1 % faster. A flip-wait site (a guest loop that polls the flip status with `usleep` until it reads DONE) sleeps until the GCM flip status is DONE or the site's cap, whichever is first, ended by the pump that completes the flip (every GCM notification meanwhile is pumped on that thread, so handlers are delivered as before); the guest then re-reads the status itself, so nothing it reads or writes changes, only how many syscalls it makes and when it next looks (a DONE flip is seen at once, not up to one poll later). Off, a registered site of either kind is ignored. Windows keeps the ordinary path (its guest clock has its own base). Read once, at the first sleep |
| `ppu.poll_sleep_spin_us` | u32 (0..2000) | `100` | `PS3RECOMP_PPU_POLL_SLEEP_SPIN_US` | With `ppu.poll_sleep_coalesce`: the sleep ends this many microseconds before the deadline and the thread spins on the host clock to it. A kernel timer wakes 20-50 us late (p95 66 us on an M4 Pro), so a sleep to the deadline itself would exit after it; the spin trades that jitter for about 80 us of CPU a frame at 100. `0` is a pure sleep. The wake latency of a small core is not measured: raise it there. Read once, at the first sleep |
| `ppu.precise_usleep` | bool | `true` | `PS3RECOMP_PPU_PRECISE_USLEEP` | macOS only. A `sys_timer_usleep` of 20 us or more waits on a kevent timer with `NOTE_CRITICAL`, which the kernel does not coalesce; `nanosleep` wakes late by a quarter of the request or more (p50: 2.5 ms at 10 ms, 4.0 ms at 16 ms), so a guest that paces itself with `usleep(period - elapsed)` misses its deadline, which the console's accurate `usleep` does not. It changes no thread state (QoS class and priority stay), costs about 0.5 us a call, and if the host refuses the timer it logs `[usleep] precise-usleep-fallback` once and sleeps that call with `nanosleep`. `false` (`PS3RECOMP_PPU_PRECISE_USLEEP=0`) is the `nanosleep` reference path. Timing only; the Windows and Linux paths are unchanged and ignore it. Read once, at the first sleep |
| `ppu.usleep_spin_us` | u32 (0..100000) | `0` | `PS3RECOMP_PPU_USLEEP_SPIN_US` | POSIX hosts. A guest `sys_timer_usleep` shorter than this many microseconds yields (`sched_yield`) until a `CLOCK_MONOTONIC` deadline instead of calling `nanosleep`, which returns only after the kernel's timer slack plus a wakeup, so a guest that polls a flag with `usleep(1..10)` sees it tens of microseconds late; the yield keeps the core free for the thread the guest waits on. Longer sleeps still `nanosleep`. `0` is off. Upstream #725 (was `PS3RECOMP_USLEEP_SPIN_US`). Read on every call |
| `ppu.usleep_hires_wait_us` | u32 (0..1000) | `0` | `PS3RECOMP_PPU_USLEEP_HIRES_WAIT_US` | Windows hosts. A sub-millisecond guest `sys_timer_usleep` of at least this many microseconds waits on a high-resolution waitable timer instead of the QPC/`SwitchToThread` spin, which keeps a logical processor busy for the whole sleep. `0` is off; sleeps of 1 ms and more use the ordinary waitable timer either way. Ignored on other hosts. Upstream #628 (was `PS3_USLEEP_WAIT_US`). Read on every call |
| `ppu.host_stack_mb` | u32 (1..2048) | `256` (`32` on iOS, provisional) | `PS3RECOMP_PPU_HOST_STACK_MB` | Host stack, in MiB, reserved for every thread that runs guest PPU code on a host thread of its own: the guest PPU threads the title creates (not the main thread), the GCM interrupt thread (`libs/video/cellGcmSys.c`: the guest's vblank, flip and queue handlers) and the video decoder's idle thread (`libs/codec/cellVdec.c`). Each recompiled guest call is a real host call, so a deep guest call chain nests on it; it is address space reserved up front and committed by the OS as the thread touches it. On iOS the app's address space is small (no extended-virtual-addressing entitlement): 256 MiB per thread was refused 41-86 times a run and later Metal allocations failed sporadically (about 6 of 10 runs); with 32 MiB, 3 of 3 device runs had no refused reservation and no failed Metal allocation. **The iOS default is provisional**: that experiment measured what the host tolerates, not what the guest uses. Turn on the per-thread high-water report (`PS3_HOST_STACK_HWM=1`, [`DIAGNOSTICS.md`](DIAGNOSTICS.md)) on a device and set the value from the measured mark; a thread whose chain outgrows its stack dies with a fault. A host that refuses the reservation ends the run with kind `host-thread-stack` ([`STOPS.md`](STOPS.md)); the thread is never started on the host's default stack. The cap is 2048 because the Windows thread API takes the size in 32 bits. Read when each thread is created |
| `spu.host_stack_mb` | u32 (1..1024) | `16` (`8` on iOS) | `PS3RECOMP_SPU_HOST_STACK_MB` | Host stack in MiB for SPU group members, raw SPUs, SPURS policy workers/job chains, JobQueue2 workers and the xdiff audit thread. Async dispatch uses `spu.dispatch_stack_mb`. Each role is measured under `PS3_HOST_STACK_HWM=1`. A typical example with six policy workers, eight JQ2 workers, six raw SPUs, six group members and one audit worker reserves 216 MiB at 8 MiB rather than 432 MiB at 16 MiB; this is not a concurrency bound (32 job chains and multiple instances are supported). The smaller iOS default remains provisional pending device measurements. Replaces `SPU_HOST_STACK_BIG` (no alias); use `spu.host_stack_mb = 512` instead. Refusal stops with `host-thread-stack`, whose role and remedy identify the key. Read at each start |
| `spu.dispatch_stack_mb` | u32 (1..2048) | `256` (`32` on iOS, provisional) | `PS3RECOMP_SPU_DISPATCH_STACK_MB` | Host stack, in MiB, reserved for every async-dispatch thread: the host thread that runs a SPURS task or workload policy module image through its lifted entry (`runtime/spu/spu_workload.c`; the policy-module workers are persistent, so these threads can live for the whole run). The Bink SPU decoder's per-macroblock `brsl` chains are deep host call chains with fat lifted frames, and a 1 MiB stack blew the guard page, so the desktop default is the 256 that has always been reserved. On iOS it is 32, for the same address-space reason as `ppu.host_stack_mb`; **provisional** like that value, until a device run's high-water report (`PS3_HOST_STACK_HWM=1`, [`DIAGNOSTICS.md`](DIAGNOSTICS.md), which covers these threads as `[HOSTSTACK] spu-dispatch`) says what they use. A host that refuses the reservation ends the run with kind `host-thread-stack` (`role=spu-dispatch`, `guest_tid=0`: there is no guest thread); the thread is never started on the host's default stack. Read when each thread is created |
| `spu.dma_repeat_limit` | u32 (1..4294967295) | `256` | `PS3RECOMP_SPU_DMA_REPEAT_LIMIT` | Identical back-to-back MFC transfers after which the SPU DMA engine backs off: it yields for the first 2 ms of the episode, where a healthy poll ends, then sleeps 1 ms per repeat. Timing only: the engine still makes progress and the guest observes nothing |
| `spu.dma_repeat_stop_secs` | u32 (0..86400) | `0` | `PS3RECOMP_SPU_DMA_REPEAT_STOP_SECS` | If non-zero, an SPU issuing identical MFC transfers for this many seconds stops with kind `spu-dma-repeat` (`image pc ea lsa cmd size count secs tid`). `0` never stops: polling a ring or label another agent writes is legitimate, and the old unconditional halt wedged those chains |
| `spu.job_body` | string (`interp` \| `lifted`) | `interp` | `PS3RECOMP_SPU_JOB_BODY` | Which body runs a JobQueue2 job ([`SPU_XDIFF.md`](SPU_XDIFF.md) sections 3, 4 and 11). `interp`: always the SPU interpreter; a body registered with `spu_workload_register_img` never runs here. `lifted`: the body registered for the job's image, if the image is also **promoted** (`spu_workload_promote_img`, which a title's generated registration emits for an image whose pipeline program record says `promoted: true`), the interpreter for any other job. Registration alone is enough to capture an image (`spu.xdiff_capture`) and to replay it offline; promotion says a corpus proved the body, so a title runs `lifted` only on the images it has proved. The image is identified by the bytes the loader staged, after staging (FNV-1a-64 of the `sizeBinary << 4` bytes), never by the descriptor's address, so a different binary staged at the same address and size runs interpreted. A worker remembers the fingerprint by address, size and a word-wise content hash of the bytes, so a job costs one content hash and the byte-serial fingerprint only for bytes the worker has not seen; a hit is a claim, not a proof (section 4). A title's own `ExecutorHooks::select_body`, when it returns a body, wins and is not checked against the bytes. With `interp` the identification runs only while PS3_FTRACE tracing (kind 29), `spu.xdiff_capture` or `spu.xdiff` needs it. A lifted body is another implementation of the same program, not a guarantee: replay a captured corpus, or run `spu.xdiff` (the interpreter then shadows it), before relying on it. Read once, at the first job |
| `spu.xdiff_dir` | string | `""` | `PS3RECOMP_SPU_XDIFF_DIR` | Where SPU job fixtures (`ps3recomp.spujob/1`, [`SPU_XDIFF.md`](SPU_XDIFF.md) section 7) are written, one file per job: `<image fingerprint>-q<queue>-s<seq>.spujob`, where the queue number is the process-wide creation count. Empty writes none. Written by `spu.xdiff_capture` (the first N jobs of each image) and by `spu.xdiff` (a fixture with a `VRDT` section for each difference, replacing a captured one of the same job; at most 16 per image, the rest counted). The directory must exist, be a directory and be writable; it is not created, and it is checked once, when the first JobQueue2 queue is created. A fixture that cannot be written later is one `[jq2-xdiff] capture-failed` line per attempt (`image queue seq reason detail`) and the run goes on |
| `spu.xdiff_capture` | u32 (0..4294967295) | `0` | `PS3RECOMP_SPU_XDIFF_CAPTURE` | Record the first N jobs of each registered SPU job image on the JobQueue2 workers into `spu.xdiff_dir`, for a corpus an offline replay compares tiers on (`SPU_XDIFF.md` sections 11 and 16, commit 10a). `0` records nothing and the job path is unchanged. Only an image whose registration carries an effects inventory with no event branches is recorded, and only while the body that runs is the interpreter or the body registered for that image. Unresolved MFC command sites refuse nothing (same-program replay does not depend on a command's class); such an image's fixtures are counted apart. Any other job is counted ineligible: an image registered without an inventory, or an unknown body. Each worker remembers the last four contents staged at an address, so a loader that alternates buffers stays recordable. The first 32 `capture-ineligible` lines of a process name an address, size and reason once each; a `capture-failed` line is never suppressed. The recorder only listens: the job's results are those of a run without it, at the cost of a content hash per job, the copies and one file write per fixture. A fixture's payload is bounded at 64 MiB; a larger job is a counted `capture-limit` failure. Both options are read once, at the first queue; with N > 0 and an empty or unusable `spu.xdiff_dir`, one `capture-off` line says so and nothing is recorded for the process |
| `spu.xdiff` | string (`off` \| `sample` \| `all`) | `off` | `PS3RECOMP_SPU_XDIFF` | Whether the other tier shadows a JobQueue2 job ([`SPU_XDIFF.md`](SPU_XDIFF.md) sections 8.4, 9, 10, 11 and 14; commit 10b). The authority is the body that runs (`spu.job_body`); the shadow is the other tier, run in process on the authority's recorded world with every effect served from the transcript, so it can neither see nor change the live world. `sample`: every `spu.xdiff_period`-th job of an image is recorded on its worker and checked on one audit thread below normal priority; the worker never waits, a full queue (8 samples, 32 MiB; one sample at most 4 MiB) drops the sample and counts it. `all`: every job, at normal priority, and the worker waits for queue room. A recording over 4 MiB is a counted `void` / `capture-limit`, never partially compared. A shadow of tier T1 runs only through the image's budgeted twin (`spu_workload_register_twin`): an image without one is counted not eligible and named once. Eligibility is otherwise that of `spu.xdiff_capture` (an inventory, no event branches, a body that is the interpreter or the registered one). The authority's result is always what the guest sees. A difference is one `[jq2-xdiff] differ image queue seq reason class effect where` line, a per-image counter by verdict and reason, and with `spu.xdiff_dir` a fixture; it never changes the run unless `spu.xdiff_on_divergence` / `spu.xdiff_on_latent` say `stop`. Lines the audit prints carry `[jq2-xdiff]`; a runtime diagnostic reached inside a shadow run is prefixed `[shadow] `; a named stop raised inside a shadow does not end the process, it is `void` / `host-fault` for that sample. Shutdown drops what is queued and counts it. Recording perturbs timing: benchmarks run with `off`. Read once, at the first queue |
| `spu.xdiff_period` | u32 (1..4294967295) | `64` | `PS3RECOMP_SPU_XDIFF_PERIOD` | With `spu.xdiff = sample`: the jobs of an image numbered from its first job as 0, 1, 2, ...; those whose number is a multiple of N are shadowed (jobs 1, N+1, 2N+1, ... counting from 1), per image. `1` shadows every job. Read once, at the first queue |
| `spu.xdiff_on_divergence` | string (`report` \| `stop`) | `report` | `PS3RECOMP_SPU_XDIFF_ON_DIVERGENCE` | What a **visible** `differ` does to the run ([`SPU_XDIFF.md`](SPU_XDIFF.md) section 8.4): every `effect-*` reason, `end-class`, and `end-regs` / `end-ls` where the loader keeps the state. `report`: named line, counter, fixture. `stop`: also ends the run with kind `spu-xdiff-divergence` (`image reason effect authority subject queue seq fixture`) from the audit thread, after the job was credited. A `void` never stops a run. Read once, at the first queue |
| `spu.xdiff_on_latent` | string (`report` \| `stop`) | `report` | `PS3RECOMP_SPU_XDIFF_ON_LATENT` | The same for a **latent** `differ`: an `end-regs` / `end-ls` difference in state the JobQueue2 loader destroys. Not an accepted difference: it names a defect of the lift this input did not expose, and blocks promotion like a visible one. Read once, at the first queue |
| `spu.xdiff_budget` | u32 (1..4294967295) | `268435456` | `PS3RECOMP_SPU_XDIFF_BUDGET` | The shadow's budget before `void` / `budget` ([`SPU_XDIFF.md`](SPU_XDIFF.md) section 10): interpreted instructions for a T0 shadow, backward and cross-function transfers of the image's budgeted twin for a T1 shadow. Read once, at the first queue |
| `spurs.real_admission` | bool | `false` | `PS3RECOMP_SPURS_REAL_ADMISSION` | SPURS kernel admission as the real kernel does it: a policy module is admitted only for a ready count above the workload's current contention, a signal bit or the workload flag, and the ready count stays as the guest wrote it. Off: a nonzero ready count admits, the kernel consumes one ready unit per admission, and idle workloads are probed, so work that arrives without a kick is still found. Guest-visible: the ready-count bytes a title reads differ. Upstream #756 (was `SPURS_IDLE_PROBE=0`). Read at every admission decision |
| `spurs.shutdown_wait_ms` | u32 (1..600000) | `2000` | `PS3RECOMP_SPURS_SHUTDOWN_WAIT_MS` | How long `cellSpursWaitForWorkloadShutdown` waits for the running policy modules of a shut-down workload to drain; past it the call answers `CELL_SPURS_POLICY_MODULE_ERROR_BUSY` instead of letting the title free memory a policy module still uses. Upstream #711 (was `SPURS_SHUTDOWN_MS`). Read at each wait |
| `gcm.wait_timeout_ms` | u32 (1..3600000) | `10000` | `PS3RECOMP_GCM_WAIT_TIMEOUT_MS` | How long an RSX wait may stand unsatisfied before the run stops with kind `gcm-wait-timeout`: a NV406E semaphore acquire whose label never reaches the value, the command-context buffer-full callback waiting for the RSX to leave the next segment, a guest store to a label waiting for the RSX walker's next pass, or a self-JUMP park nobody patches (`what=jump-park`). The hardware waits forever; a host watchdog, not PS3 behaviour |
| `gcm.park_resync` | bool | `false` | `PS3RECOMP_GCM_PARK_RESYNC` | Compatibility mode for a command-buffer park (a JUMP to itself the guest patches later). Off, the Reference behaviour: the RSX waits at the park however late the patch comes, GET never moves to `put` because a host deadline expired, and a park that stands the whole `gcm.wait_timeout_ms` with `put` elsewhere stops with `gcm-wait-timeout` (`what=jump-park`). On: after `gcm.park_grace_ms` GET moves to `put` and the commands in between, a fence among them, are dropped; every intervention is logged with the mode and the effective grace. For a title whose producer never patches the park; a host liveness policy, not PS3 behaviour |
| `gcm.park_grace_ms` | u32 (1..60000) | `50` | `PS3RECOMP_GCM_PARK_GRACE_MS` | How long a self-JUMP park stands with `put` elsewhere before `gcm.park_resync` moves GET to `put`. Effective only with `gcm.park_resync` on. God of War HD, loading its menu, took more than 50 ms between its append and its patch of the park |
| `gcm.handler_thread` | bool | `false` | `PS3RECOMP_GCM_HANDLER_THREAD` | Deliver the guest's vblank, flip and user handlers only on one dedicated host thread (the console's GCM interrupt thread) instead of nested in whatever guest thread made the HLE call. Needed by a title whose lock wrappers record the owner thread id and leak the lock when a handler runs inside the owner's thread (MLB The Show 16). Handlers then run concurrently with the guest threads. A compatibility mode |
| `gcm.report_spu_hold_ms` | u32 (0..60000) | `0` | `PS3RECOMP_GCM_REPORT_SPU_HOLD_MS` | If non-zero, a GET_REPORT waits up to this many milliseconds while SPU tasks that work in signal-driven rounds are in one, like an ACQUIRE (God of War: Ascension's RsxSync). Gives up after that with a logged line. 0 never holds. A compatibility timing policy |
| `eng.depth_clamp` | bool | `true` | `PS3RECOMP_ENG_DEPTH_CLAMP` | Honour the depth-clamp field of `NV4097_SET_ZMIN_MAX_CONTROL` (`0x1D78`): a fragment beyond the near or far plane is clamped to it instead of clipped, as on the RSX. `false` (`PS3RECOMP_ENG_DEPTH_CLAMP=0`) always clips at the near and far planes, as before the register was honoured. A kill switch for diagnosis (v0.19.0 #693) |
| `eng.depth_bounds` | bool | `true` | `PS3RECOMP_ENG_DEPTH_BOUNDS` | Emulate the depth-bounds test on a backend with no native one (Metal): the draw's depth is snapshotted before it and the fragment program discards a pixel whose stored depth lies outside the bounds. `false` (`PS3RECOMP_ENG_DEPTH_BOUNDS=0`) ignores the test, as before. A kill switch for diagnosis (v0.19.0 #694) |
| `rsx.bc_path` | string (`auto` \| `cpu` \| `gpu` \| `tiered`) | `auto` | `PS3RECOMP_RSX_BC_PATH` | How the Metal backend holds a guest BC1/2/3 (DXT1-5) texture. `auto`: as BC where the device samples BC (every Mac, the M1 iPad), otherwise decoded to RGBA8Unorm by the toolkit (an iOS device whose GPU reports no BC sampling, `MTLDevice.supportsBCTextureCompression`; the iPhone X is the floor). The other values decode on every device, a BC-capable Mac included, for measuring a tier and for comparing frames against native BC. `cpu`: each level is decoded on the RSX thread (`libs/video/rsx_bc_decode.c`). `gpu`: every level of a texture the upload created is decoded by a compute kernel (`libs/video/rsx_metal_bc.m`) in a command buffer committed before the frame's own, so it runs before every draw of the frame; a texture an upload merely refreshes takes the CPU tier (a rewrite would race draws already recorded), after the decodes still pending for that texture. `tiered`: `cpu` for a texture whose largest level has under `rsx.bc_gpu_min_texels` texels, `gpu` from there, all its levels, faces and slices alike. Every tier writes exactly the texels the Apple GPU's own BC decode produces (the Metal oracle `rsx_bc_oracle_metal`, and `test_rsx_bc_metal` for the GPU tier), so the value changes where the time goes, never a pixel. Decoded textures take 8x (BC1) or 4x (BC2, BC3) the memory; a refused allocation is the `host-gpu-alloc` stop. Every `PS3_FTRACE` run records one `BC_UPLOAD` span (kind 34: format, texels of the level, tier executed) per emulated upload and, with the GPU tier built, one `BC_SETTLE` span (kind 35) per frame commit: the RSX thread's wait for the GPU-tier decodes, bounded (see below). Read once, at the first BC texture |
| `rsx.bc_gpu_min_texels` | u32 (1..4294967295) | `147456` | `PS3RECOMP_RSX_BC_GPU_MIN_TEXELS` | With `rsx.bc_path = tiered` (and `auto` where it decodes): the texels of a texture's largest level (its creation size, width times height) from which the GPU tier decodes the whole texture, every level, cube face and slice; a smaller texture is decoded entirely on the RSX thread. The tier is a property of the texture, so the CPU never writes a texture the GPU is writing (see below). Provisional, from `tests/bench/video/bench_rsx_bc_gpu.m` on an Apple M4 Pro (measured per level), not yet from an iPhone X: the first measured size where the GPU tier costs the RSX thread less than decoding the level itself, counting the wait for the decode before the frame commit (an upload followed at once by a commit, both BC1 and BC3; 384x384). Ignored by `cpu` and `gpu`. Read once, at the first BC texture |
| `vk.draw_batch` | bool | `false` | `PS3RECOMP_VK_DRAW_BATCH` | Vulkan backend: record the draw engine's draws, snapshots, texture uploads and clears into one command buffer per submission instead of a submit and fence wait each. Vertex, index and uniform data of every draw sit in host-coherent rings (176 MiB of host-visible memory, a third of each per submission in flight). A full batch (`vk.draw_batch_max` operations or a full ring), or a batch of `vk.draw_batch_min` operations while the GPU is idle, is submitted without a wait and the next is recorded while the GPU runs it: three submissions are in flight, each with its own command buffer, fence, ring slice and descriptor pool, and the walker waits only when it comes round to a slot the GPU still holds. A readback, an object destroy and the engine's submit point wait for every batch; a present or a one-shot submission is ordered after them by a barrier. Meant to change only when work is submitted, not what is drawn; off until a pixel diff of presented frames, on against off, is clean (#129). A device that cannot give the rings host-coherent memory stays off with a logged line. Read once, at the first draw. Replaces the unregistered `VK_DRAW_BATCH=1` |
| `vk.draw_batch_max` | u32 (1..1024) | `512` | `PS3RECOMP_VK_DRAW_BATCH_MAX` | The most operations recorded into one batched submission when `vk.draw_batch` is on. 1024 is the descriptor-set count of each in-flight submission's pool: a larger value is a `config-invalid` stop, where `VK_DRAW_BATCH_MAX` capped it silently. A lower value hands the GPU a frame's work sooner at the price of more submissions. Replaces `VK_DRAW_BATCH_MAX` |
| `vk.draw_batch_min` | u32 (0..1024) | `16` | `PS3RECOMP_VK_DRAW_BATCH_MIN` | Adaptive kick: once a batched submission holds this many operations, it is submitted as soon as the GPU is idle (the newest submission in flight has finished, or none is) instead of waiting for `vk.draw_batch_max`. The first batch of a frame is then a small kickstart, and later batches grow to what the walker records while the GPU runs the one before. The GPU is polled (`vkGetFenceStatus`) at this count and then every 16 operations. `0` keeps the fixed cap alone; a value above `vk.draw_batch_max` is lowered to it, with a logged line. 16 is the better of 16 and 32 on an Apple M4 Pro (MoltenVK and KosmicKrisp, 1000 draws a frame): 30-31 ms a walker-bound frame and 24-25 ms a GPU-bound one against 40-44 and 27-41 ms with the fixed cap of 512, a fixed cap of 64 28 and 19 ms; the Ally's own A/B decides for a device with costly submissions (recon `out/M4/VK-FENCE-2026-11-21.md`, section 7). Read once, with `vk.draw_batch` |
| `game.free_space_raw` | bool | `false` | `PS3RECOMP_GAME_FREE_SPACE_RAW` | Report the host disk's measured free space in every cellGame `hddFreeSizeKB` (capped at `INT32_MAX` KB). Off: below 4 GiB the measured value, from 4 GiB up RPCS3's 40 GiB - 256 KiB, because a guest multiplies `hddFreeSizeKB * 1024` in 32 bits and 4 GiB or more wraps to a few MB (inFAMOUS 2 failed its install check with 41.7 GB free). The figure above 4 GiB is RPCS3's, not a hardware measurement (v0.19.0 #692) |
| `trophy.register_wait_cap_vblanks` | u32 (1..16777216) | `1024` | `PS3RECOMP_TROPHY_REGISTER_WAIT_CAP_VBLANKS` | The most vblanks `sceNpTrophyRegisterContext` waits for its status events to be consumed by `cellSysutilCheckCallback` before it returns success anyway, logging `register-wait-cap` and counting it (`sceNpTrophy_register_wait_cap_count()`). The firmware blocks until the VSH has finished installing; the HLE paces status delivery over 120 vblanks (2.0 s at 59.94 Hz). The HLE's own worst case with per-vblank polling is about 121 vblanks (120 vblanks of pacing plus the 1-vblank #37 return step), longer only if the title polls rarely. The default of 1024 vblanks leaves ample margin for slow pollers (RPCS3's oracle tolerates up to 50 ms grace per event). A title that registers on the thread that does the polling hits the cap, and the events are then delivered by its later polls |
| `trophy.pace_ms` | u32 (1..600000) | `2000` | `PS3RECOMP_TROPHY_PACE_MS` | The minimum total time, in milliseconds, that `sceNpTrophyRegisterContext` takes to deliver its status events. The firmware's own timing is wall-clock (the VSH installs the trophies); 2000 ms is RPCS3's synthetic minimum, not a measured figure. The HLE counts it in vblanks, converted once per registration at the rate the host raises them (`cellGcm_set_vblank_rate`): 120 vblanks at 59.94 Hz, 240 at 119.88 Hz, so a host that runs its vblank grid faster than the video-out mode still waits the same wall time. A host that never sets the rate gets the mode's 59.94 Hz |
| `adec.silence_fallback` | bool | `false` | `PS3RECOMP_ADEC_SILENCE_FALLBACK` | Let `cellAdecDecodeAu` answer an AU it has no decoder for (every codec but AC3; AC3 without FFmpeg, with `PS3RECOMP_FFMPEG=0`, or when libavcodec decodes nothing from it) with `CELL_OK` and a block of silence of the codec's frame size, AUDONE and PCMOUT as for a decoded AU. Off, that AU is the `adec-no-decoder` stop (`func`, `codec`, `reason`): the console decodes it, and success for audio nobody decoded hides the gap (#130). The silence keeps a title that only counts PCM against time running; a title config that sets it says why in a comment and names the codec |
| `video.resolutions` | string | `""` | `PS3RECOMP_VIDEO_RESOLUTIONS` | The resolutions the display reports as available: a comma-separated list of cellVideoOut resolution ids (`1` 1080, `2` 720, `4` 480, `5` 576, `10` 1600x1080, `11` 1440x1080, `12` 1280x1080, `13` 960x1080). `cellVideoOutGetResolutionAvailability` answers from it, `cellVideoOutGetDeviceInfo` lists exactly these modes, and `cellVideoOutGetState` reports 720 before the title configures a resolution when 720 is listed, else the first of 480, 576, 720, 1080, 1600x1080, 1440x1080, 1280x1080, 960x1080 that is. An unknown id is a `config-invalid` stop. Empty means no restriction (every id available; the device lists 480, 576, 720, 1080). A policy, not a hardware fact: a console answers per connected display |
| `video.rsx_capture` | bool | `false` | `PS3RECOMP_VIDEO_RSX_CAPTURE` | Record the RSX command stream and every guest-memory input it consumes as a `ps3diff.rsx/2` capture (`docs/PS3DIFF.md`) under `video.rsx_capture_dir`. Off, the FIFO walker and the draw engine run as before. The capture starts at `cellGcmInit` (clean reset): a first RSX command with no GCM init before it is a `rsx-capture` stop (`reason=midstream`). Every consumed span is copied and hashed, so the run is slower |
| `video.rsx_capture_dir` | string | `""` | `PS3RECOMP_VIDEO_RSX_CAPTURE_DIR` | Where the capture goes: `capture.ndjson` and `blobs/sha256/`. Created if missing; a directory that already holds a capture is refused (`rsx-capture`, `reason=output-exists`). Empty with `video.rsx_capture` on is a `config-invalid` stop |
| `video.rsx_capture_frames` | u32 (0..1000000) | `0` | `PS3RECOMP_VIDEO_RSX_CAPTURE_FRAMES` | Close the capture after this many completed frames (flip heads) and stop capturing; the title keeps running. `0` captures until the process exits normally. A killed process leaves `capture.ndjson.partial`, which no tool accepts |
| `pad.record` | string | `""` | `PS3RECOMP_PAD_RECORD` | Record what the guest is handed by `cellPadGetData` (and the connection state `cellPadGetInfo`/`GetInfo2`/`cellPadPeriphGetInfo` report) into this file, as a `ps3recomp-pad-trace` version 3 (`docs/MODULES_REFERENCE.md`, "cellPad: record and replay"). `len` and the words are what the guest read, after the host pad, the keyboard and the `PAD_SCRIPT`, `PAD_FILE`, `PAD_STICK` and `PAD_AUTOPRESS` injectors (a drained poll's `len` 0 included), keyed by the port's `GetData` ordinal; the flip clock is kept as an annotation, not a key. Only a change of state is written. The file is created and never overwritten: a path that exists, or one in a directory that is missing or not writable, is a `config-invalid` stop at load. Empty records nothing. May be set together with `pad.replay` to record the replayed input |
| `pad.replay` | string | `""` | `PS3RECOMP_PAD_REPLAY` | Play a version-3 trace recorded with `pad.record` back: poll n of a port is answered with the latest record of that port at or before n, `len` and words exactly as recorded (so a drain loop ends on the poll it ended on while recording), a neutral pad before the port's first record and past that port's own end count (the end record holds one count per port; a killed run has none and holds each port's last state after a one-line notice), and the info calls report the recorded connection at or before their own ordinal. The host pad and every injector are ignored. A path that is not a readable file is a `config-invalid` stop at load; a malformed file, a version other than 3 (version-1 and version-2 traces, keyed on the flip clock, included) or keys that go backwards end the run with kind `pad-trace` (`docs/STOPS.md`). The end-of-trace line and the exit summary say how closely the guest followed the recording, including `flip_drift`: how far the guest flip clock moved between the recording and this replay at the polls that reached a record. Empty plays back nothing |
| `pad.replay_repeat` | u32 (1..8) | `1` | `PS3RECOMP_PAD_REPLAY_REPEAT` | For a title that polls once per simulation step: serve each recorded poll of a version-3 trace this many consecutive guest polls. Guest poll p of a port is answered with exactly what `pad.replay` alone serves at recorded ordinal floor(p / N): the latest record at or before it (an ordinal with no record of its own holds the latest state), a neutral pad once floor(p / N) reaches the port's end count. A recording made at one step per 60 Hz tick therefore replays with its wall-time pacing in a build that runs N steps per tick (`2`). A title that drains several polls per step would see each drained report N times; that is not supported. The guest's flip clock and poll pattern take no part. `1` (the default) serves every recorded poll once, as before. The summary names N (`repeat=N`) when it is not 1; `reached_exactly` still counts each recorded change once, at its first serve. A version-1 or version-2 trace cannot be repeated (`reason=version`); a value outside 1..8 is a `config-invalid` stop. |
| `title.elf_sha256` | string | `""` | `PS3RECOMP_TITLE_ELF_SHA256` | Optional pin: 64 hex digits, the SHA-256 of the executable `ppu_load_elf` may load, checked in addition to the hash the lifter recorded. Empty means no pin |
| `host.work_interval` | bool | `false` | `PS3RECOMP_HOST_WORK_INTERVAL` | macOS only. The title's RSX walker (the thread that runs the FIFO and the present) tells the host's performance controller about its work: it joins an audio interval workgroup, enters the Mach time-constraint band (period 16683 µs, computation 4000 µs, constraint 16000 µs, preemptible) and wraps every FIFO pass in an interval with a deadline `host.work_interval_us` after its start (`ps3_host_work_interval_{join,begin,end}`, [`host_work_interval.h`](../include/ps3emu/host_work_interval.h); the state is thread-local, so the calls belong on one thread). In High Power mode the controller then holds the thread's cluster at its top clock, which shortens the pass and the pad-to-present latency, for more CPU power; in Low Power mode the clock is capped and nothing changes. The time-constraint policy alone moved no clock: the interval is what the controller acts on. Measured on a title's pad replay (Apple M4 Pro, High Power, three runs per arm, on top of the walker cycle cuts): pad p50 −0.47 ms and walker 3.94 GHz, but frame p99 +0.69 to +1.00 ms and +0.41 W, which is why it is off by default. With the option on, a host with no interval workgroup or one that refuses a step of the join ends the run with kind `config-invalid` (`key=host.work_interval`, `step`, `code`) after one `[work-interval]` line naming the step; a run that asked for it never silently runs without it. A refusal of one interval after a successful join is logged (the first four) and that pass runs outside an interval. With one join, one `[work-interval] <thread>: joined an interval workgroup` line proves it applied. Off, no host framework is called. AudioToolbox is linked on macOS only. Read when the thread starts |
| `host.work_interval_us` | u32 (50..16000) | `500` | `PS3RECOMP_HOST_WORK_INTERVAL_US` | With `host.work_interval`: the deadline of each interval, in microseconds from its start. The controller sizes the clock to finish the declared work by then, so a shorter deadline asks for a higher clock; 500 is the value measured. 16000 is about one frame |
| `diag.dump_config` | bool | `false` | `PS3RECOMP_DIAG_DUMP_CONFIG` | Print the effective configuration to stderr after loading |
| `log.file` | string | `""` | `PS3RECOMP_LOG_FILE` | The run log file ([`LOGGING.md`](LOGGING.md)). Empty writes none. Set, an existing file is moved to `<file>.prev` (replacing an older one: exactly one previous run survives) and a fresh file is created; every line the runtime prints through `ps3_log` goes into it as `<seconds> <thread> <E\|W\|I\|D> <line as on stderr>`, with what only a file can hold (collapsed repeats, what a per-site cap swallowed, the stop banner). stderr is unchanged. A path that names a directory, cannot be rotated or cannot be created ends the run with kind `host-log-file`; a path longer than 1018 characters (its `.prev` twin must fit a 1024-byte buffer), `host-path-limit` |
| `log.level` | string (`error` \| `warn` \| `info` \| `debug`) | `info` | `PS3RECOMP_LOG_LEVEL` | The most verbose level printed, on stderr and in the file. Migrated call sites are `info`, so the default hides nothing that printed before |
| `log.mute` | string | `""` | `PS3RECOMP_LOG_MUTE` | Comma-separated tags (`[A-Za-z0-9_.-]`, the text inside the `[..]` that starts a line) whose lines are not printed on stderr. The file keeps them. Matches whole tags, not prefixes |
| `log.collapse` | string (`file` \| `all` \| `off`) | `file` | `PS3RECOMP_LOG_COLLAPSE` | Consecutive identical lines from one call site. `file`: written once to the file, then `[log] previous line repeated N times`; stderr prints every line. `all`: stderr collapses the same way (no longer byte-identical to the unmigrated output). `off`: nothing collapses |
| `log.max_mb` | u32 (1..65535) | `256` | `PS3RECOMP_LOG_MAX_MB` | Size cap of the log file in MiB. At the cap the file gets one `size cap reached` line at exit and nothing else; stderr is unaffected |

The registry's `doc` strings are the authoritative description of each option.

**`rsx.bc_path` and the Metal routes.** The emulation lives on the draw-engine route, the only one iOS has. The
macOS vtable route (`PS3RECOMP_RSX_ENGINE=vtable`, or a draw engine that failed to start) has no decoder and holds BC
natively, so a forced value there (`cpu`, `gpu`, `tiered`) would silently measure native BC: the backend refuses it
when the route is chosen, with a `config-invalid` stop naming `rsx.bc_path`, its value and `route=vtable`. `auto`
goes on natively on that route. A GPU-tier decode whose command buffer ends in an error is the `host-gpu-command`
stop ([`STOPS.md`](STOPS.md)) at the next frame commit, naming the texture's format, size and level, never a frame
presented on an undefined texture. The commit first waits for the decodes committed so far (the settle; a texture a
frame samples must be written), for at most 8 s of active time (a suspended GPU gate does not run the bound down):
a decode still pending then is `host-gpu-command` with `reason=decode-timeout`, naming it. The wait is the
`BC_SETTLE` span (kind 35) of a `PS3_FTRACE` run. It is why `rsx.bc_gpu_min_texels` is 147456 (384x384), the size
from which the GPU tier costs the RSX thread no more than the CPU tier even for an upload followed at once by a
commit: the GPU's latency, about 0.1 ms on an Apple M4 Pro, is the floor of that cost.

**A texture is decoded by one tier.** Metal leaves the CPU writing a texture (`replaceRegion`) while the GPU may be
writing it undefined; on the paravirtual device of a CI runner the GPU-decoded levels of a texture whose smaller mips
the CPU had written read back as zeros. So the tier is chosen once per texture, at its creation, from its largest level
(`rsx.bc_gpu_min_texels`, width times height): under `tiered` and `auto` a large enough texture has every level, face
and slice decoded on the GPU, any other every level on the CPU, and the CPU never writes a texture with a GPU decode
of its own. The cases that still mix (a level whose GPU decode was refused, or a refresh of a texture a draw has been
recorded against) wait, before the CPU write, for the decodes committed so far, with the settle's bound and failure
handling; no CPU write overlaps a pending GPU write to the same texture. `BC_UPLOAD` stays one span per level, with the
tier that ran.

## Allow-listed gaps: `[[hle.stub]]` and `[[lv2.stub]]`

A gap the runtime would stop on (an unresolved firmware import, an unimplemented
syscall) may be allowed to continue, per entry, with an explicit return value and
a written reason. There is no blanket form and no environment switch.

```toml
[[hle.stub]]
nid    = 0x12345678
ret    = 0x8002cb07
reason = "the title only logs this error and carries on"

[[lv2.stub]]
num    = 141
ret    = 0
reason = "a sleep; skipping it only speeds the title up"
```

Every field is required. `reason` must say something (a blank one is refused). A
`nid` (0 to 0xFFFFFFFF) or `num` (0 to 1023) may appear once. `ret` is 32 bits:
`-2147483648` to `0xFFFFFFFF`, a negative value being its two's complement, returned
in r3 sign-extended from 32 bits. Entries are parsed and validated at load. An
`[[hle.stub]]` is consulted when an import has no handler (`unresolved-nid`) and
an `[[lv2.stub]]` when a syscall has none (`unimplemented-syscall`); a hit returns
`ret` and logs `ALLOW-LISTED ...` once with the reason and the config line, a miss
stops. An entry for something that *is* implemented is never consulted.
Lookup: `ps3_config_find_hle_stub()` and `ps3_config_find_lv2_stub()`.

## `ppu.null_page = "drop"` needs a written reason

The default stops at the first guest access into the null page. `drop` lets the
run continue past a defect real hardware would have faulted on, so it is a per-title
exception, never a blanket setting: the title's config file sets it, and the line
above it says why, as `reason` does for `[[hle.stub]]`:

```toml
[ppu]
# reason: the title's allocator probes page 0 on purpose and reads the 0s back
null_page = "drop"
```

No in-repo title config or test relies on `drop`; the loader test
(`tools/test_loader_gaps.py`) sets it only to prove the legacy handling still
works. The environment form (`PS3RECOMP_PPU_NULL_PAGE=drop`) is for a one-off
experiment, not for a title's standing configuration.

## Effective configuration

`diag.dump_config` (or `ps3_config_dump(FILE*)` at any time) prints every key, its
value and where the value came from, as TOML:

```toml
# ps3recomp effective configuration
# config file: title.toml
schema = 2

[ppu]
vscr_nj = false  # source: file:title.toml
opd_swap_fixup = false  # source: default

[spu]
dma_repeat_limit = 256  # source: default
dma_repeat_stop_secs = 30  # source: env:PS3RECOMP_SPU_DMA_REPEAT_STOP_SECS

[title]
elf_sha256 = ""  # source: default

[diag]
dump_config = true  # source: env:PS3RECOMP_DIAG_DUMP_CONFIG
```

A source is `default`, `file:<path>` or `env:<VARIABLE>`. The dump is itself a
valid config file that reproduces the same values.

## Lifetime and threading

The configuration is loaded once and is immutable afterwards.

- A title calls `ps3_config_load(path)` first thing in `main()`, before the
  executable is loaded and before any thread exists. If it does not, the first
  reader loads from the environment alone.
- The finished configuration is published with a release store and read with
  acquire loads, so any thread reads it without a lock, and the pointer from
  `ps3_config_get()` is valid for the life of the process.
- `ps3_config_load()` after the configuration is in use is a stop, not a reload: a
  value that changed under a running guest is the divergence this exists to
  prevent.

## Adding an option

Add one line to `config_options.def` and a row to the table above. Changing or
removing an option is a schema change: bump `PS3_CONFIG_SCHEMA_VERSION`.
