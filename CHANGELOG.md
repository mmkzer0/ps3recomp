# Changelog

### Unreleased
- **PPU thread exit is syscall 41, and slot 42 answers `CELL_ENOSYS` again** (#86; `runtime/syscalls/{lv2_syscall_table.h,sys_ppu_thread.c,lv2_enosys_bitmap.c}`, `runtime/ppu/tests/smoke/smoke_recomp.c`, `docs/{LV2-SYSCALL-STATUS.md,SYSCALLS.md,RUNTIME.md}`, `tests/test_ppu_thread_join_detach.c`, `tools/{test_lv2_syscall_status.py,test_ppu_thread_join_detach.py}`; FW 4.93 stock `lv2_kernel` table, 4.81 REBUG/Cobra dump, RPCS3 `d00d80231af3f9b0286f38fd72f7513583f1a9d4` `rpcs3/Emu/Cell/lv2/lv2.cpp`): the table had `SYS_PPU_THREAD_CREATE` at 41 and exit at 42, so a guest `sc 41` created a thread out of whatever stale r4/r5 held instead of ending the caller, and `sc 42` ended the thread where the firmware's shared ENOSYS stub returns `0x80010003`.
  - Slots now follow the firmware: 41 is `_sys_ppu_thread_exit`; 42 is unregistered and in the regenerated ENOSYS bitmap; `_sys_ppu_thread_create` (52) and `sys_ppu_thread_start` (53) stay unregistered because their ABI is not `sys_ppu_thread_create`'s, which is still reached by NID.
  - The smoke title created its guest thread with `sc 41`, the old numbering; it now calls the `sys_ppu_thread_create` import NID, as a lifted title does, and still joins the thread it started.
  - Audit counts: `toolkit-handler-on-hw-enosys` 1 -> 0, `hw-enosys-unregistered` 375 -> 376. The generated files were reproduced byte-identically from the unmodified tree before regeneration.
  - Tests: `ppu_thread_join_detach` case (j) builds the real table and runs sc 41 with the firmware ABI, stale r4..r9 holding a valid create: the caller ends with its r3 status, never returns and starts no thread; sc 42 answers `0xFFFFFFFF80010003` and the thread runs on. On `e78eabed` it fails with 6 errors, including a foreign thread started from the stale registers and sc 42 ending the caller. `lv2_syscall_status` expects row 42 as `hw-enosys-unregistered` with bitmap bit 42 set, and checks the suspicious verdict through a synthetic 988 map; on `e78eabed` it failed at the row-42 assertion.
- **`ftrace_report.py` names the guest call site of every import wait, splits slow frames from fast ones and measures an import's cadence** (`tools/{ftrace_report.py,test_ftrace_report.py}`). Before, a 240 s battle timeline (9M spans) took 4.5 min, because every frame re-sliced the whole span list. The import table had no call sites, so "the main thread waits 8.8 ms per frame in `Port2Sync`" could not say where. Nothing showed what grows in the slow frames, or what an audio thread does between two polls.
  - Each span is now binned once by bisection on the frame ends and shared out over the frames it overlaps: 43 s on the same timeline.
  - New `--split MS`, `--cadence NID[@TID]` with `--gap MS`, and `--json`.
  - Malformed or unreadable traces, too few flips and bad arguments exit 2 without a traceback.
  - Test: `test_ftrace_report` builds a synthetic timeline. It checks a usleep shared across a frame edge, per-site import time, the split means, one long poll gap 80 % inside a job sync, and the exit-2 cases.
- **Instruments recordings of a title can be reported from the command line** (`tools/{xctrace_xml.py,xctrace_profile.py,xctrace_audio.py}` with their `test_*.py`, `docs/TOOLS.md`). Before, an `xctrace` recording could only be read in the Instruments GUI. That GUI cannot group by lifted guest function and cannot show what a thread did while another one was silent.
  - `xctrace_xml.py` streams an `xcrun xctrace export` table. It resolves the interned `id`/`ref` values, sentinels and positional columns, and rejects a ref before its id, a row without a schema or a column-count mismatch.
  - `xctrace_profile.py` reports a Time Profiler export: per role, thread, function, line and guest function (`func_XXXXXXXX`), in time buckets, as sample gaps or as CPU bursts.
  - `xctrace_audio.py` reports an Audio System Trace: per HAL IO cycle, overloads, POIs and jitter, thread-state latency, and the cadence of a call.
  - The tests use synthetic exports only. A `--toc` export carries the recorder's environment; `docs/TOOLS.md` says to keep it out of reports.
- **`SPU_MFC_LOOKUP_CACHE` is described the right way round** (`docs/DIAGNOSTICS.md`). `=1` enables the per-thread MFC engine cache (`runtime/spu/spu_channels.c:441-475`); the index said it bypassed it.
- **The identity a present publishes names the flip whose scanout it shows** (#78; `libs/video/rsx_draw_engine.c`, `libs/video/tests/{test_rsx_draw_accounting,test_rsx_draw_dump_names}.c`, `docs/RSX_GRAPHICS.md`): `0xE944` changes the scanout, so the present it issues already shows the queued flip, but the identity kept the previous flip's ordinal until the `0xE924` head was counted. A run of identical frame dumps started at the present labelled with the previous flip, so taking the last dump labelled *k* as flip *k*'s image gave flip *k*+1's frame. The engine now tracks a pending flip (set at `0xE944`, cleared at a head with bit 31): `flip_id` and `guest_frame` name it in that window, so the queue present, the head present and the held presents of one flip carry one identity. Presents per flip, `present_seq`, pacing and the capture format are unchanged; the capture still records one frame per head, at the head's present, so captures and their replay are identical to before.
  - Visible in names: every dump from the first queue word on is labelled one flip higher than before for the same image (`present_s<n>_f<k>`, `<base>_s<n>_f<k>.ppm`); a pass dump keeps the last-head label it had. The first queue word's present is `f0` instead of `fnone`.
  - Test: `test_rsx_draw_accounting` drives FIFO `0xE944`, present, `0xE924`, present, held present for three flips and requires one flip id, guest frame and buffer across them and an unchanged capture frame (flip, guest frame, buffer, head `present_seq`); it failed on the old engine (6 checks). `test_rsx_draw_dump_names` expects the new names.
- **LV2 syscall status is audited against the stock 4.93 table, and the firmware's own ENOSYS slots answer `CELL_ENOSYS` instead of stopping** (`docs/LV2-SYSCALL-STATUS.md`, `runtime/syscalls/{lv2_enosys_bitmap.c,lv2_enosys_bitmap.h}`, `runtime/ppu/ppu_loader.cpp`, `tools/{lv2_syscall_status.py,test_lv2_syscall_status.py,test_loader_gaps.py,check_ppu_scaffold.py}`, `tests/test_loader_gaps.cpp`, `CMakeLists.txt`, `docs/{SYSCALLS.md,STOPS.md}`; FW 4.93 stock `lv2_kernel` from the official PUP, corroborated by the LV2 dump FW 4.81 REBUG/Cobra; RPCS3 `d00d80231af3f9b0286f38fd72f7513583f1a9d4` `rpcs3/Emu/Cell/lv2/lv2.cpp`): an unregistered syscall stopped even when the firmware itself had no handler for it, so a title probing an unused number got a stop where hardware answers `CELL_ENOSYS` (`0x80010003`) from the shared stub. The maps are not committed (they hold kernel addresses); only slot status is.
  - `tools/lv2_syscall_status.py <stock-map> [--dump-map <4.81-map>]` generates the audit and the 1024-bit table. The stock kernel has 632 handlers and 392 ENOSYS slots. Verdicts: `ok` 164, `toolkit-handler-on-hw-enosys` 1, `hw-enosys-unregistered` 375, `hw-real-unregistered` 468, `conflict-4.93-vs-rpcs3` 16 (slots 60-69, 213, 412, 623, 699, 904, 905; RPCS3 binds a handler the stock table stubs, so the stop stays).
  - Stock 4.93 is primary. The 4.81 dump agrees on the other 1018 slots and its 632 real handlers have identical code addresses; it has handlers at 6, 7, 8, 9, 10 and 15 (CFW additions such as peek/poke), ENOSYS on stock. Those six are in the ENOSYS bitmap (RPCS3 at the pin binds none of them), so a 4.81-only call gets the stock answer.
  - The suspicious slot was 42: `sys_ppu_thread_exit` was registered there, while stock 42 is the ENOSYS stub and RPCS3 binds `_sys_ppu_thread_exit` at 41. No handler was changed in that entry; #86 (above) carries the firmware evidence and moves exit to 41.
  - `lv2_syscall`'s default path answers `CELL_ENOSYS` for `hw-enosys-unregistered` slots, per call, with the generated bitmap; `[[lv2.stub]]` still wins, and `hw-real-unregistered` and conflict slots still stop with `unimplemented-syscall`. The answer is logged once per slot only under `PS3_SCTRACE`, never by default.
  - Tests: `loader_gaps` proves stock slot 6 (a CFW-only 4.81 handler) answers `0x80010003` with r4..r10 and guest memory intact, logs once per slot under `PS3_SCTRACE`, keeps a real unregistered slot and a conflict slot stopping, and lets the allow-list win; `lv2_syscall_status` drives the generator from synthetic stock and dump maps and queries the compiled bitmap.
- **A null-page stop raised inside a host-side handler names the handler, not a guest function** (follow-up to #83; `include/ps3emu/stop.h`, `runtime/diag/stop.cpp`, `runtime/ppu/{ppu_loader.cpp,ppu_hle.cpp}`, `tests/test_stop_attribution.cpp`, `tools/{test_stop_attribution.py,test_loader_gaps.py}`, `docs/STOPS.md`): a load or store below the null-page bounds made from a syscall or import handler used to be attributed to the nearest lifted function below the host return address, and printed it as `fn` and `pc`. In #83 that was a function the title never entered. `guest-null-access` (same kind, same exit 70) now carries `site=guest` (`fn` as before) or `site=host-handler` with `in_syscall=<num>` and/or `in_hle=<name>/<nid>`, no `fn`, and the guest call site the handler was entered from as `pc` and `lr` (lifted code keeps no pc; as in `unimplemented-syscall`).
  - How: the frontier providers also report the handler the stopping thread is inside (`ps3_stop_frontier.in_hle` / `in_syscall`: the entry `ppu_context`, its `lr`, the NID or number; new `ps3_stop_query_frontier`). Per-thread, saved and restored by value. A fault counts as the handler's own only when the active guest context is the one the handler was entered with, so a guest callback it runs (`ppu_guest_call`, a context of its own) is still `site=guest` with its `fn`.
  - Not covered: the lwmutex `hle_fast` forms and the real-PRX bridge do not mark a handler (hot path; lifted code on the caller's context); a fault there keeps the old attribution.
  - Test: `stop_attribution` builds the real loader and HLE bridge and runs a syscall handler, a context-table import and a generic-table import that load or store at address 0, the same three running a guest callback that loads it, a guest access after a handler returned, and a plain guest access. Fail-before: on the previous tree the handler cases carry `fn` and no `site`/`in_*` (a syscall stop named `fn=0x00000000`, an import stop named the unrelated registered guest function).
- **lv2 syscall 988 is the debug-interface call it is, not `sys_tty_write`** (#83; `runtime/syscalls/{sys_dbg.c,sys_dbg.h,lv2_register.c}`, `runtime/ppu/ppu_loader.cpp`, `tests/test_loader_gaps.cpp`, `tools/{test_loader_gaps.py,test_loader_memory_domain.py}`, `docs/SYSCALLS.md`; lv2 kernel dump FW 4.81 REBUG/Cobra handler `0x8000000000277688` (kind test `0x2776AC..0x2776CC`, flag test `0x2776F4`, shared ENOSYS stub `0x800000000026D2A0`; the handler is not a Cobra redirect, so stock is assumed identical), RPCS3 `d00d80231af3f9b0286f38fd72f7513583f1a9d4` `lv2.cpp:961` `null_func`): a requested quit (SIGTERM, Cmd+Q, window close) ended in the stop `guest-null-access` (exit 70) instead of exit 0. The SDK `exit()` path issues 988 as `(r3, r4) = (1, 0)` and discards the result; the alias read `r4` as a TTY buffer and the final copy loop read byte 0. The same SDK issues the real console write as 403 a few instructions after a 988.
  - 988 takes a kind id in `r3` (1, 2, 4, 8 or 16, on the low word) and a flag word in `r4` (0, low word); anything else is `CELL_EINVAL`, otherwise `CELL_OK`. It reads and writes no guest memory and has no capability gate. The firmware's `CELL_ESRCH` for an unresolvable calling process cannot occur here. RPCS3 answers `CELL_OK` for every argument (an unimplemented slot there); where they differ the firmware wins.
  - Both TTY aliases are gone (`lv2_register.c` table slot, `ppu_loader.cpp` switch label). The earlier comment claimed other titles (Tokyo Jungle, Sonic/Gunstar hubs, 4 Elements HD) need 988 as a TTY write. That is not evidenced for this SDK, where 988 and 403 are different calls; a title that proves otherwise carries that in its own port, not here. Those titles are the open risk of this change.
  - Test: `loader_gaps` runs the real `lv2_syscall` with `(r3, r4)` over the valid ids, the invalid ones (0, 3, 12, 17, `0xFFFFFFFF`), a non-zero flag and the high-word cases, with live pointers in `r5..r10`: the answer, no register but `r3` changed, no guest byte changed, no output. With the alias restored it stops with `guest-null-access` as the title did.
  - Not changed here: the stop named a meaningless `fn` and a guest PC, because `ppu_null_page_stop` attributes a host-side `vm_read8` to the nearest lifted function.
- **`sceNpTrophyAbortHandle` exists, and an aborted handle answers `SCE_NP_TROPHY_ERROR_ABORT`** (#80; `libs/network/sceNpTrophy.{c,h}`, `libs/network/tests/{test_trophy_errors.c,test_trophy_registration.c}`; FW 4.93 `libsysutil_np_trophy` `0x020014C4` with its handle check `0x0200025C`, VSH `vsh.elf` abort handler `0x001909DC` and handle lookup `0x0018DC94`/`0x0018DC0C`, RPCS3 `d00d80231af3f9b0286f38fd72f7513583f1a9d4` `sceNpTrophy.cpp` `sceNpTrophyAbortHandle`/`get_context_ex`): a title that calls it from its exit callback ended in the named stop `unresolved-nid` (exit 70) instead of a clean exit. The library keeps no abort state: it checks the handle and asks the VSH. An id outside `1..4` is `INVALID_ARGUMENT`, one the table does not hold `UNKNOWN_HANDLE`, and no init flag is read (before `Init` and after `Term` the table is empty, so `1..4` are `UNKNOWN_HANDLE`; RPCS3 has no init check there either). The VSH marks the handle and answers 0; every later request naming it answers `0x80022910` until the handle is destroyed, which does not read the mark and frees it.
  - Here the mark is `TrophyHandle.aborted`, under `s_life_lock`. `RegisterContext` (after its own argument checks, and again after its inline callback), `GetRequiredDiskSpace`, `GetGameInfo`, `GetTrophyInfo`, `UnlockTrophy`, `GetTrophyUnlockState` and `GetGameProgress` answer `ABORT` for a live aborted handle, ahead of `CONTEXT_NOT_REGISTERED`; they still do not validate the handle (unchanged). A second `AbortHandle` answers `ABORT` (the VSH lookup does; RPCS3 answers `CELL_OK`). `DestroyHandle` still succeeds and clears the mark; `Term` clears all.
  - Before the second round trip of `RegisterContext` (the abort comes before the call, or from its inline callback), the call answers `ABORT`: the VSH's `i` handler `0x001902A4` looks the handle up (`0x00190360`) and its error path stores `0x80022910` (`0x001905D8`), which the library returns as is (`0x02003F00`, `0x020019CC`); RPCS3 re-checks after `statusCb` (`sceNpTrophy.cpp:647-653`) and agrees. The context stays unregistered and no queued status runs after.
  - An abort while `RegisterContext` waits is the new named stop **`np-trophy-unmodelled`** (`reason=abort-during-registration`; fields `func handle context reason`; `docs/STOPS.md`, `include/ps3emu/stop.h`, `tests/gates/test_stop.py`). The wake is proven (VSH cancel step `0x0018E22C` signals the object the `i` handler sleeps on, `0x0018E288`, and clears the mark again, `0x0018E290`, so the handle is not left marked), but nothing re-checks the mark after it and the result is the install engine's (`0x001905BC`, callee `0x00172CAC`): `ABORT` through the mapper `0x0018F6DC`, or `CELL_OK`. The firmware does not say, so no code is invented; the slot-299 status the handler posts itself (`0x0018F1EC`) is not delivered, the stop comes first. An abort of a handle no registration waits on stays marked until `DestroyHandle`. RPCS3 does not interrupt the wait at all. Open points, evidence and the firmware-versus-RPCS3 table: research repo `out/M3/fw-np-trophy-abort/ABORT-HANDLE.md`.
  - Tests: `hle_trophy_errors` (range/unknown split before and after `Init`, the order against `RegisterContext`'s own checks, every consumer, a second abort, destroy, reuse of the id, `Term`) and `hle_trophy_registration` case `aborthandle` (before the call, from the inline callback, an abort of another handle while a registration waits, and the stop in a forked child ordered by the library's own state, not a sleep), labels `hle;t0`; `tests/gates/test_stop.py` pins the new id.
- **NV308A inline transfers write the NV3062 destination, and a transfer that still stops says what the packet was** (#81; `libs/video/{cellGcmSys.c,rsx_method_class.h}`, `libs/video/tests/{test_gcm_nv308a.c,test_rsx_method_census.c}`, `CMakeLists.txt`, `docs/{RSX_GRAPHICS.md,STOPS.md}`; RPCS3 `d00d80231af3f9b0286f38fd72f7513583f1a9d4` `Emu/RSX/NV47/HW/nv308a.cpp` `color::impl`, `rsx_methods.cpp:1723`, `rsx_methods.h:1137-1160/1247-1264`): the first gameplay slice stopped as `rsx-unknown-method class=NV308A method=0x030C` (SIZE_IN, 1x1), the entry to an image-from-CPU transfer.
  - `POINT`, `SIZE_OUT`, `SIZE_IN` and the COLOR words are state and a transfer now. COLOR is the method window `0x0400..0x1FFC` (RPCS3 binds 0x700 methods there, not 256; a method-numbering fact of the oracle, not a measured maximum row length); the word at index `i` lands at `POINT.x + i`, and words at or beyond `SIZE_OUT.x` are dropped, as RPCS3 does. The destination is NV3062 `OFFSET_DESTIN + x * bpp + PITCH.dst * y` in the DMA object `0x0188` names, with `COLOR_FORMAT` choosing 4 bytes (A8R8G8B8, Y32) or 2 (R5G6B5, big-endian, channels truncated: RPCS3's `(c * 32) / 255` equals it below 255 and overflows the red field at 255). A non-incrementing packet leaves its last word.
  - NV3062 `0x0184` is the image *source* DMA, not the destination: it used to overwrite the destination DMA. Reset values are RPCS3's (local DMA, A8R8G8B8, pitch `0x400040`).
  - Stops, not guesses, each saying it awaits hardware evidence (#63 stage B): an operation other than SRCCOPY, a surface context other than the linear NV3062 object (so no swizzled NV309E route, RPCS3 has none for NV308A), a SIZE_OUT or SIZE_IN taller than one row (hardware wraps, RPCS3 drops: they disagree), a SIZE_IN width that is not SIZE_OUT's, a format RPCS3 cannot reach, and a DMA object that is neither local nor main (`gcm-unmodelled`); a destination outside local memory, the IO mapping (the IO table alone: an unmapped main offset no longer falls back to VRAM here) or the 32-bit offset space (`hle-guest-range`). Every check runs before the first byte moves.
  - Every NV308A/NV3062 stop carries the transfer state: `word=sub/method/data point size_out size_in color_words=<transfer>/<total> dst_offset dst_pitch dst_dma dst_format operation surface src_format bind=0:<class>,...,7:<class>`, in the `reason` or `arg` of the `PS3STOP` line and on a `[nv308a]` log line; an unknown NV308A/NV3062 method logs it before `rsx-unknown-method`. `docs/STOPS.md` has the rows; no new kind or key.
  - Classification: `SIZE_IN`, `POINT`, `SIZE_OUT`, COLOR, `0x019C` and `0x02FC` are handled; the colour key, clip, pattern, ROP and beta contexts (`0x0184..0x0198`) stay inert (RPCS3 reads none) and are now pinned to handle 0; `COLOR_CONVERSION` (`0x02F8`) is a counted gap. `0x0300` on NV308A is still COLOR_FORMAT and never NV309E `SET_FORMAT`; the new fixture guards it. The census fixture counts move from 41/180 to 43/178.
  - A diagnostic for the stops the user can only reach by hand: before an `rsx-unknown-method` stop and before an NV308A envelope stop, the raw FIFO words from max(stop word - 64 words, IO offset 0) to min(`put`, stop word + 2048 words) are written to `$TMPDIR/rsx-stop-fifo.txt` (`/tmp` without `TMPDIR`; stderr if it cannot be written), with IO offset, guest EA, value, the decoded header of a header word and the method and class of a data word, so the whole NV3062/NV308A block and the COLOR words that follow `SIZE_IN` survive in a file. It reads each word through `gcm_io2ea` and `vm_read32` after a guest-memory bound, as the walker does, and changes no behaviour (`TMPDIR` only names the file's directory). `docs/STOPS.md` names it.
  - `test_gcm_nv308a` (`gcm_nv308a`, `gcm_nv308a_stops`, labels `gcm;t0`) checks destination bytes for 1x1, a 300-word row, a packet filling the COLOR method window, SIZE_OUT clipping, POINT and pitch, local and main destinations, 16- and 32-bit formats and a non-incrementing packet; that a COLOR store into main memory kills an SPU reservation on its line and precedes a later NV406E label release; each named stop with its state in a forked child; and the FIFO window file (decoded headers, the words after the stop word up to `put`, no word past `put`, the start clamp, an envelope stop).
- **`cellGcmGetVBlankCount` returns the vblank counter and no longer advances it** (O8 / #37; `libs/video/cellGcmSys.c`, `tests/gcm_initbody/{test_vblank_count.c,CMakeLists.txt}`; RPCS3 `d00d80231af3f9b0286f38fd72f7513583f1a9d4` `rpcs3/Emu/Cell/Modules/cellGcmSys.cpp:889-894` returns the counter unchanged): the getter was `return s_vblank_count++`, so every query was a vblank. The sceNpTrophy status sequence, paced by that count, drained all 256 events in one `cellSysutilCheckCallback` poll instead of spreading them over vblanks. Only `cellGcm_request_tick` and `cellGcm_vblank_signal` (hence `cellGcmTickVBlank`) raise it now, by exactly 1 each. `s_vblank_count` is `_Atomic u32`: the vblank thread and PPU threads both reach it; the raises are release `fetch_add`, the getter and the handler pump are acquire loads, the two resets are release stores. The pump reads the count once per batch, so the handler numbers of one pump are consecutive while the vblank thread raises another. `gcm_vblank_count` (`gcm;t0;tsan`): two queries with no vblank between them agree, each raised vblank adds 1 through all three entry points, the handler gets `base+1..base+n`, and a polling reader against a vblank thread never sees the count go back and does not advance it. Red against the old body (10 checks fail), green after.
- **`cellGcmSetWaitFlip(ctx)` enqueues the firmware's NV406E acquire instead of sleeping the PPU** (#77; `libs/video/cellGcmSys.{c,h}`, `tests/gcm_initbody/test_setwaitflip.c`; FW 4.93 libgcm_sys `0x02001180`, Unsafe `0x02001228`, NIDs `0x983FB9AA` / `0xDF6476BD`): the exports take the command context (guest EA) and append `{0x00040064, 0x10}`, `{0x00040068, 0}` at `current`, advancing it by 16, reserved with the buffer-full callback (`current + 16 > end` calls `callback(ctx, 4)`; Unsafe writes with no space check). The old HLE took no context, enqueued nothing and slept `1 ms` up to 64 times per call while the flip was outstanding, a PPU cost the firmware does not have. The wait is the walker's NV406E acquire on label `0x10`; the driver half that sets the label back to 0 when the flip completes was already modelled (`cellGcm_vblank_flip_label`, shared with `cellRescSetWaitFlip`; RPCS3 `RSXThread.cpp` `handle_emu_flip` → `sys_rsx.cpp` attribute `0xFEC`). The context, the segment written to and the callback OPD are range-checked (`hle-guest-range`, `arg=ctx` / `current` / `callback`).
- **Upstream v0.18.0 is merged into `dev`** (`sync/v0.18.0`, merge `69dfc765`; plan `docs/V018-MERGE-PLAN.md` draft 8 in the research repo): 32 conflicted paths, 91 regions, resolved block by block (SPU 41, PPU 20, RSX 11, tools 9, mechanical 10). The merged tree is byte-identical to the reviewed cloud resolution `939544c4`. Our contracts win where the two disagree: named stops instead of halts, the reservation model as the only coherency authority, and typed config instead of new behavioural `getenv`. The follow-ups below are what that took. Open from the merge: v0.18.0's other behavioural env knobs (#55), `YDKJ_HANDLERFIX` (#56), inline guest stores (#57), `ppu_fs` label-window ordering (#58).
- **The handler mask word follows libgcm_sys: start value 4, one bit per setter, read-modify-write** (#68; `libs/video/cellGcmSys.c`, `libs/video/tests/test_gcm_handler_mask.c`, `tools/test_gcm_handler_mask.py`; FW 4.93 libgcm_sys `0x02008240`, setters `0x02007D58`..`0x020080B4`, dossier §1, §2, §6 T6): `driver_info + 0x12C0` is 4 after `cellGcmInit` (a plain store, as the bootstrap's), and each `Set*Handler` sets or clears only its own bit by a CAS loop on the big-endian word: vblank `0x2`, flip `0x10`, second vblank `0x800`, queue `0x40`, user `0x80`. `cellGcmSetSecondVHandler` and `cellGcmSetQueueHandler` publish their bits now, and no setter sets the graphics bit `0x4` (the flip bit used to be `0x4`). The old rebuild from three slots destroyed every foreign bit. The firmware's setters choose the slot pair by a config `mode` it does not fix; the HLE models mode 1-2, the pair RPCS3 at the pin raises (head 1) and the pump's completion delivery (`r3 = 1`) already uses; a firmware trace of the config word would overturn it (named in the code). `g_gcm_handler_mask` is removed (it had no reader). `g_gcm_driver_info_ea` must be set before `cellGcmInit`. The fixture fails on the old code and passes now; a second one races two threads under ThreadSanitizer.
- **The HLE's flip and vblank delivery is pinned against the firmware's OPD contract** (#70; `libs/video/tests/{test_gcm_handler_contract.c,test_gcm_handler_contract_stubs.cpp}`, `tools/test_gcm_handler_contract.py`; FW 4.93 libgcm_sys `0x0200851C`..`0x02008548`, dossier §2, §6 T1-T3): through `ppu_gcm_pump` and the real `ppu_guest_call`, a synthetic lifted handler runs with r2 = its OPD's toc; a code word zeroed after registration, or `{toc, code}` swapped with `ppu.opd_swap_fixup` off, is the named stop `unresolved-guest-callback` (origin `opd`, code `0` or the toc), never a skip, a call or the swap redirect. The completed flip gets `r3 = 1`, as the dispatcher's `0x10` slot does; the request-time flip call (`r3 = 0`) and the vblank count are pinned as implemented (#69).
- **`sys_lwsync` links under ld64** (`runtime/syscalls/sys_lwsync.cpp`, `tests/lwsync/test_lwsync.cpp`, `tests/test_loader_gaps.cpp`): v0.18.0 took `ps3_hle_register_fast` and `ppu_thread_note_block` as weak references, which ld64 resolves by loading the archive member that defines them, so a fixture without that owner failed to link. They are now strong references, and the fixtures define what they do not link.
- **A stop flushes both standard streams before every exit** (#54; `runtime/diag/stop.cpp`, `tests/gates/{probe_stop.cpp,test_stop.py}`): `_Exit` discards stdio buffers. A stop raised while stdout was block-buffered (redirected, nested in a stop, or with a full frontier table) lost the output before it. `exit_stopped()` now flushes on every exit path. Three probe scenarios prove it fail-before and pass-after.
- **`cellPadGetData`/`cellPadClearBuf` port checks cite the firmware** (`libs/input/cellPad.c`, `libs/input/tests/test_pad_file_reports.c`; FW 4.93 libio `0x02003080`, `0x0200544C`): `INVALID_PARAMETER` only for a port number above 126 or a NULL buffer, and `NO_DEVICE` for a port that is not connected, port 7 included.
- **Upstream's SPU coherency bitmap and lock-free GETLLAR are gone; every lifted store goes out of line** (#57; `tools/ppu_lifter.py`, `runtime/ppu/{ppu_memory.h,ppu_loader.cpp}`, `runtime/spu/{spu_coherency.{c,h},spu_context.h,spu_channels.c}`, `runtime/spu/spu_coh_bitmap.h` deleted, `runtime/spu/tests/test_spu_lockline_peer.c`, `tools/test_spu_{context_registries,event_producers}.py`): the second reserved-line bitmap was published with a plain store and read only by two paths the merge had compiled out. The reservation model stays the only authority. Generated code does not change, because `vm_mm_st_ok` already returned 0.
- **The SPU DMA-repeat fixture asserts our contract** (`runtime/spu/tests/test_spu_dma_repeat.c`, `tools/test_spu_dma_repeat.py`, `docs/CONFIG.md`, `include/ps3emu/config_options.def`): at the default `spu.dma_repeat_stop_secs = 0` the episode never halts and the process never stops. The fixture inherited from v0.18.0 asserted a halt and re-arm at 30 s. The back-off is now documented as v0.18.0 implements it: the thread yields for the first 2 ms of an episode, then sleeps 1 ms per repeat.
- **`gcm.park_resync` and `gcm.park_grace_ms` replace `GCM_PARK_RESYNC` and `GCM_PARK_GRACE_MS`** (`include/ps3emu/config_options.def`, `libs/video/cellGcmSys.c`, `libs/video/tests/{gcm_fixture_stubs.h,test_gcm_park_modes.c,test_gcm_park_reuse.c,test_gcm_resync_labels.c}`, `tools/test_gcm_park_{modes,reuse}.py`, `tools/test_gcm_resync_labels.py`, `tests/gates/`, `docs/CONFIG.md`, `docs/STOPS.md`):
  - Reference mode (the default) waits at a self-JUMP park. A park that stands `gcm.wait_timeout_ms` stops with `gcm-wait-timeout what=jump-park`.
  - Compatibility mode follows `put` after the grace, which defaults to 50 ms. It logs every intervention with the mode and the grace.
- **The draw engine's register reads are classified, so the engine-on census no longer stops on the default state's scissor** (`libs/video/rsx_dispatch.c`): v0.18.0's #466 gate keeps non-effect methods from the state machine, whose `rc == 0` had answered for these registers. Each new STATE row names its engine read: scissor `0x08C0/0x08C4`, vertex textures `0x0900..0x097C`, the frequency-divider mask `0x1FC0` and the VP branch bits `0x1FF8`. `rsx_replay` is green again.
- **A method is interpreted in the class `SET_OBJECT` bound on its subchannel** (#47; `libs/video/{cellGcmSys.c,rsx_method_class.h}`, `libs/video/tests/{test_gcm_class_routing.c,test_gcm_legacy_state_filter.c}`, `tools/test_gcm_class_routing.py`, `docs/RSX_GRAPHICS.md`):
  - NV0039 and the 2D classes no longer reach the NV4097 register file. NV308A's `0x018C/0x0194/0x0198` used to move the colour and depth surfaces to local memory every flip.
  - NV308A's `COLOR_FORMAT` is no longer read as NV309E `SET_FORMAT`, and subchannel 7 is no longer NV3089's.
  - `SET_OBJECT` is handled on every class.
  - Puller (NV406E) words act on any subchannel, an NV0039-bound one included.
  - `GCM_SUBCH1_2D` and `GCM2D_DBG` are removed.
- **Driver words win on subchannel 7, and the 3D mirrors get NV4097 method numbers** (`libs/video/cellGcmSys.c`, `libs/video/tests/test_gcm_class_routing.c`, `tools/test_gcm_class_routing.py`):
  - The driver's five words (0xEB00/0xEB04, 0xE944, 0xE920/0xE924) are decoded by full address before any class route. With slot 7 rebound to NV0039, 0xE944 used to stop as unknown.
  - The 3D fast path no longer runs on slot 7. With NV4097 bound there, it used to batch 0xE944 as a vertex-texture register.
  - An NV4097 method reaches the draw engine by its own number. On slot 7, BEGIN_END used to arrive as 0xF808 and no draw ran.
- **`NV4097_INVALIDATE_L2` is inert with the engine on, and no method the legacy state machine handled can turn unknown** (`libs/video/rsx_method_class.h`, `docs/RSX_GRAPHICS.md`, `libs/video/tests/{test_gcm_class_routing.c,test_rsx_legacy_effect_methods.c}`, `tools/test_gcm_class_routing.py`): the title stopped at its first RESC flip on `0x1FD8`, which the toolkit's own convert stream emits. The class-routing fixture now checks two things with the engine on: every NV4097 method that `rsx_commands.c` handles (418) is not unknown, and the real RESC convert stream walks with zero unknown verdicts.
- **An atomic SPU line store into the RSX label window stops like a PUT** (#51; `runtime/spu/spu_channels.c`, `include/ps3emu/stop.h`, `runtime/memory/label_window.h`, `runtime/spu/tests/test_spu_label_window_main.c`, `tools/test_spu_label_window.py`, `docs/STOPS.md`): a PUTLLC, PUTLLUC or PUTQLLUC whose 128-byte line overlaps the window is counted and ends in `spu-label-window` before the lock-line lock is taken and before any byte of the line moves. `cmd` names the form.
- **Lazily initialised SPU flags are race-free across worker threads** (`runtime/spu/{spu_drain.c,spu_channels.c,spu_context.h,spu_dma.h}`, `tools/test_spu_lifter_merge_chunks.py`): TSan found three lazy initialisations from v0.18.0 that every SPU worker reads on its first run.
  - `g_spu_direct` is no longer a `volatile int`. Relaxed atomic loads read it, and one CAS publishes it.
  - The `SPU_PCSAMPLE` once-flag is now read atomically.
  - The `SPU_POISON_DMA_WATCH` cache is now thread-local.
- **The diagnostics index matches the sources again** (`docs/DIAGNOSTICS.md`): it lists 685 knobs across 91 groups. 26 behavioural knobs that are still env-only are marked for #55.
- **An SPU MFC PUT/PUTL into the RSX label window is counted and stops by name** (#34; `runtime/memory/label_window.{h,c}`, `runtime/ppu/ppu_loader.cpp`, `runtime/spu/spu_dma.h`, `libs/video/cellGcmSys.c`, `include/ps3emu/stop.h`, `runtime/diag/stop.cpp`, `tools/test_spu_label_window.py`, `runtime/spu/tests/test_spu_label_window_main.c`, `docs/RUNTIME.md`, `docs/STOPS.md`): the label window, its PPU store hook and the walker-pass counters moved out of the per-title loader into the runtime library, so the SPU MFC path links the same state; the PPU store path's behaviour is unchanged (same inline window word, same hook and ordering). An SPU PUT/PUTL whose span overlaps the registered window now ends in `spu-label-window` with the transfer's EA, size, command, MFC tag, the SPU's pc/id and the per-process count (`ps3_label_window_spu_puts()`), before any byte moves. The SPU path deliberately does not wait for a walker pass like a PPU store: a PUT that is the value an RSX ACQUIRE is waiting for would deadlock (the walker is inside the pass waiting on it), and the runtime cannot tell that case from the overwrite race, so it fails closed. A PUT outside the window is untouched and costs one window read per transfer. Test: `spu_label_window` (ctest, `spu;t0`) proves the stop fields, the PUTL element path, a one-byte overlap edge, the outside-window PUT, and the pre-fix control (no window: the same PUT lands).
- **`rsx_replay` replays an RSX capture through the real walker, draw engine and headless Metal backend, and reports live against replay** (M3 H1b, AM-21; `tools/rsx_replay/rsx_replay.cpp`, `libs/video/cellGcmSys.c` `cellGcm_rsx_replay_step`, `libs/video/rsx_metal_backend.{m,h}`, `libs/video/rsx_capture.{h,cpp}`, `libs/video/tests/test_rsx_replay_scene.c`, `tools/test_rsx_replay.py`, `docs/PS3DIFF.md`, `docs/RSX_GRAPHICS.md`): `rsx_replay <capture-dir> [--frames a-b] [--out report.json] [--manifest DIR] [--accept-unknown-methods]` replays `gcm-init` and the state events, then feeds the walker one recorded step at a time (blobs written to their guest addresses, the live PUT, the walker stopped where the step left it) and checks every recorded write (label RELEASE, report, NV0039 destination) against the guest memory the replay produced. The replay is captured by the same writer, so its manifest goes to `ps3diff rsx-compare`, and a replay that reproduced the inputs has the live `capture_hash` at every frame. Named errors (exit 2): `missing-blob`, `altered-blob`, `capture-hash`, `unknown-method`, `step-discontinuity`, `step-landing`, `write-mismatch`, `frame-identity`, `fixed-function`, and the capture-format ones; exit 4 is an environment mismatch (not comparable), 3 zero frames, 5 a diagnostic `--accept-unknown-methods` replay (not gate-valid in the header and every frame). The GET_REPORT timer is the host clock live, so the replay feeds the recorded one; `present_seq` (the title's frame clock adds presents) is reported, not compared; the label window's address is read from the label blobs. Found while proving it on the title: a cancelled step (an ACQUIRE the walker sat on for N passes) kept its read ordinals, so the capture hash depended on N; the writer now gives them back (`test_rsx_capture_walker retries`). `ctest -R rsx_replay` (label `gpu`, skipped without a Metal device) runs a synthetic scene through the walker, engine and Metal (clear, triangle, CALL/RET/JUMP, a rebind, a NV0039 copy, a textured strip, two flips), replays it twice with identical capture, state and frame hashes equal to the live ones, and breaks it each way a capture can break.
- **The differential harness runs `libspurs_jq`: 242 JQ2 cases against the lifted firmware, every declared non-match with a causal witness** (M3 stage 3, #19; `tests/diff_harness/cases_libspurs_jq.cpp`, `dh_engine.{h,cpp}`, `dh_main.cpp`, `tests/diff_harness/CMakeLists.txt`, `tools/diff_harness/run_diff.py`, `docs/DIFF_HARNESS.md`): 155 sequences and 87 single calls cover the attribute setters, both create forms, Shutdown/Join, Port2Create/Destroy, PushJob/CopyPush/PushSync/Sync, the ring, the descriptor pool and the firmware-only exports. The run is 175 match (119 of them common mode through forwarded `_sys_memset`/`_sys_strncmp`/`sys_process_get_paramsfo`/`cellFiberPpuSelf`, so 56 independent), 50 diverge, 6 no-hle, 9 inconclusive, 2 lifted-blocked, 0 unexpected. The engine gained the ELFv1 parameter save area for arguments 9..12 (`r1+0x70..`), a per-step `post` hook with `on_hle_side()` (the HLE's JQ2 workers pause intake after create, so a push sequence leaves exactly the bytes the push wrote), and import-model `model_stop`/`model_block`. The 9 inconclusive are the intentionally blocking full-ring/pool-exhaustion pushes, declared `hang` at their step; the 2 lifted-blocked are declared with their blocker (`model-stop` at libsre's `cellSpursWaitForWorkloadShutdown`, `import-policy` at the firmware's `_sys_trap_process` assertion). The 50 divergences are the HLE findings this slice reports, not fixes: page attribute 351 unmodelled (3), the pool word-0 odd-count test missing (2), create/Join registry state (5), Shutdown/Join not reading the queue-block fields (8), Port2Create stores after validation / Destroy clearing what the firmware leaves (4), check order in PushJob/CopyPush (4), zero depth/workers and zero-size pushes stopping where the firmware accepts (3), flag bit 1 (6), the firmware-only pool-area and slot-bitmap tests (6), and the per-API port checks (9). Firmware-only queue-block writes are masked with the exact ranges found by the unmasked `c00`; a mask never covers a byte the case is about.
- **The synthetic differential fixture also runs GCC-cross-compiled `synlib` variants, and the #45 lifter gaps are declared instead of hidden** (#45; `tests/diff_harness/synth/{syn_spec.h,syn_c.c,syn_cpp.cpp,syn_asm.S,syn_c_bug.c,syn_stubs.S,syn_gcc.ld,syn_module.json,ref_native.{c,h},syn_ref_register.cpp,syn_cases.cpp}`, `tools/diff_harness/{synth_pack,lift_module}.py`, `tools/diff_harness/gen_tables.py`, `tests/diff_harness/CMakeLists.txt`, `.github/workflows/dev-ci.yml`, `docs/DIFF_HARNESS.md`): a second module, written from `syn_spec.h` as C, C++, hand-written assembly (TOC-fetched jump table, CTR loop, inter-export call, import, floating point) and a deliberately buggy C, is cross-compiled with `powerpc64-linux-gnu-gcc/g++/ld` (`-m64`, 32-bit effective addresses, flat at `0x02000000`), packaged as a PRX-shaped image, and lifted by the toolkit's own lifter. The 138-case table is identical for every variant: `-O2` C/C++/assembly, `-Os`, `-O3 -funroll-loops` and the baseline all match; the bug variant diverges at exactly the six declared cases (both check-order/limit returns and the four PACK wide-field writes) with their witnesses. GCC's `-O0` and `-mcmodel=medium` switch dispatches (issue #45) lose the case bodies, so the 37 cases that reach the switch are declared `lifted-blocked` with `unlifted-function` and the step; they turn into `unexpected` when #45 is fixed, which is the intended flip. A linker tail branch to `_restgpr0_*` is the same gap and is documented; GCC 13.3 does not emit it for this code. The route is optional (visible skip 77), `PS3RECOMP_DIFF_SYNTHETIC_REQUIRED=ON` makes a missing toolchain a configure error, and the Linux dev-ci job installs it and sets that option.
- **`cellFiberPpuJoinFiber` checks in the firmware's order and no longer waits forever on a NULL status** (#28 F28; `libs/spurs/cellFiber.c`, `tests/fiber_switch/main.c`, `docs/STOPS.md`; FW 4.93 libfiber `0x02003574..0x0200369C`): the firmware tests NULL fiber, NULL status, ALIGN (`&127`), NOSYSINIT, DEADLK (the caller's own fiber), INVAL (storage of a fiber created with an onExit callback, `+0x31C` bit 0) and STAT (state word 0). The HLE accepted a NULL status and then waited for a fiber that might never run (the differential harness reproduced the hang against the lifted library), returned INVAL for a self-join and for any storage it had not created, and had no ALIGN or NOSYSINIT test. Storage the runtime never created is now judged like the firmware judges it, from its bytes; one that holds a live state word stops with `fiber-unsupported` `reason=unknown-fiber`, because the firmware would join it. Test: `fiber_switch` (before: DEADLK came back as INVAL and the NULL-status join hung until the ctest timeout).
- **`cellSync2SemaphoreEstimateBufferSize` exists, and `cellSync2SemaphoreInitialize` copies the name like the firmware** (#28; `libs/sync/cellSync2.{c,h}`, `tests/sync2_semaphore/`; FW 4.93 libsync2 `0x02004B94` and validator `0x02004B38`): the export had no handler, so a title calling it stopped with `unresolved-nid`. It now runs the shared attribute validator (NULL, SDK window `[0x270000, 0x330000]`, maxWaiters in `[1, 0x8000]`) before it tests the output pointer, and stores 0 for one waiter (Initialize rejects an external buffer then) or `8 * (maxWaiters + 1)`. Initialize copied the name with `memcpy` of 31 bytes; the firmware uses `strncpy`, so the bytes after the terminator are zero whatever the attribute held there. The differential harness found both against the lifted library. Test: `sync2_semaphore_estimate` (before: `unresolved-nid`).
- **The call-level differential harness (HLE vs the same function lifted from firmware) is in the tree as V1** (`tests/diff_harness/`, `tools/diff_harness/`, `docs/DIFF_HARNESS.md`, `docs/FIRMWARE_LLE.md`; M3 backlog #19/#28, variant a of M3-SCOPE §3): one call per case runs through the HLE handler and through the firmware lift, each side in its own forked child. The result model has two dimensions: per side `returned | guest-fault{ea,size} | named-stop{kind} | host-crash{signal} | timeout | harness-blocked{reason}`, per case `match | diverge | no-hle | inconclusive | lifted-blocked`. Two timeouts or two host crashes are `inconclusive`, never a match; a guest fault pairs with `hle-guest-range` only at the same step with overlapping non-empty spans; a match reached through a `Policy::Forward` import is marked `common_mode` with the NIDs and counted separately. The whole guest arena (minus the guest stack) is compared, so module-image and out-of-window writes enter the verdict; a 4 GiB `PROT_NONE` guest reservation makes out-of-arena accesses deterministic guest faults; the lifted ABI check covers r14-r31 sentinels, LR and SP. The hand-built JSONL is replaced by one document per run, schema `ps3recomp.diffharness/1`, written through `.partial` and an atomic rename (environment with toolkit SHA and image SHA-256, per-side states, per-function name×NID verdict counts). Exit codes: 0 with at least one compared case and no unexpected case, 1 unexpected, 2 harness error, 77 visible skip. Expectations carry a causal witness (cause kind, step, memory range, stop kind, blocker+step). Tools and the CMake hook canonicalise and refuse repository-contained firmware inputs/outputs; a configured but incomplete module is a visible `SKIP <module>: <reason>`, never a silent pass. `docs/FIRMWARE_LLE.md` now shows `--toc` for raw-mode lifts and says the output is C++.
- **A firmware-free synthetic PPU fixture proves every differential comparator dimension** (`tests/diff_harness/synth/`, `tools/diff_harness/{make_synth,test_synth,skip}.py`, `tests/diff_harness/CMakeLists.txt`, `.github/workflows/dev-ci.yml`; HARNESS_ROADMAP stage 7): a checked-in C + C++ + hand-written PPC assembly module is built for `powerpc64-unknown-linux-gnu` by a probed PPC-capable clang, linked with `ld.lld`, and lifted by the toolkit's own `ppu_lifter.py --raw --toc`; a test-only HLE registered under synthetic NIDs is compared against it. The cases mutate one dimension each (wrong return, missing write, extra zero padding, swapped import arguments, wrong fault span, zero-length fault span, return instead of fault, hang). `test_synth.py` (ctest `diff_synth`, labels `diff;t0`) requires each mutation to be detected and then proves fail-before by disabling that dimension: with it off the case must become undetected and the run must fail. No PPC clang or `ld.lld` is a visible skip (77), never a pass.
- **NV0039 (M2MF) copies run; the FIFO walker tracks SET_OBJECT and counts methods no class handles; an overlong frame-dump list stops** (M3 H1b; `libs/video/cellGcmSys.c`, `libs/video/rsx_method_class.h`, `libs/video/rsx_dispatch.c`, `libs/video/rsx_commands.c`, `libs/video/rsx_metal_backend.m`, `tools/ps3diff/rsx.py`, `docs/STOPS.md`, `docs/PS3DIFF.md`, `docs/DIAGNOSTICS.md`). Before: subchannel 1 was treated as 3D whatever it was bound to, so libgcm's M2MF setup (`0x184`..`0x328`) landed in the NV4097 register file (`0x30C`..`0x324` are alpha, blend and colour-mask state) and the copy never ran; an unknown method was a Debug-only log capped at 50 lines and an ignored `-1`; a `PS3RECOMP_METAL_FRAME_DUMP_LIST` of more than 32 numbers silently kept 32. Now: each subchannel's last SET_OBJECT handle and class are tracked (reset by `cellGcmInit`); a subchannel bound to NV0039 (`0x31337303`) routes to an M2MF handler and nothing else is rerouted. BUFFER_NOTIFY copies `LINE_COUNT` rows of `LINE_LENGTH` elements with input/output increments 1, 2 or 4 and signed pitches, in RPCS3's row order and overlap semantics (envytools `nv_m2mf.xml` for the registers); both spans are bounded in 64 bits before the first byte moves; a bad increment, an unknown DMA object or a span that leaves guest memory is a named stop (`gcm-unmodelled`, `hle-guest-range`). Under a capture the source span is a consumed read and the destination a write. Every method the walker consumes is handled, inert (an allow-list in `rsx_method_class.h`, one entry per piece of evidence) or unknown; unknown ones are counted per (class, method) (`gcm_method_stats`, `gcm_unknown_methods`), the first 16 pairs are logged, and the captured frame is `invalid` with `unknown-methods`; `ps3diff validate` and `rsx-compare` refuse such a frame. The live named stop stays off until slice S1 classifies the methods G0 found (M3-SCOPE Amendment 1, finding 5). A frame-dump list over 32 entries is a `config-invalid` stop naming the count. Tests: `test_gcm_nv0039` and `tools/test_rsx_methods.py` (ctest `rsx_methods`).
- **RSX capture at consume time: `ps3diff.rsx/1` (producer, validator, comparison)** (M3 H1b, AM-21; `libs/video/rsx_capture.{h,cpp}`, `libs/video/cellGcmSys.c`, `libs/video/rsx_draw_engine.{c,h}`, `libs/video/rsx_metal_backend.m`, `tools/ps3diff/rsx.py`, `include/ps3emu/config_options.def`, `docs/PS3DIFF.md`, `docs/STOPS.md`): with `video.rsx_capture` on, the FIFO walker records each step it takes (method packets, CALL/RET/JUMP in executed order, zero-word runs, flip words, SET_OBJECT bindings) and every span the RSX consumes (labels, vertex, index, texture and fragment-program bytes as the draw engine saw them, as content-addressed blobs), the draw engine records each draw's outcome and a per-flip frame with the identity `{flip_id, guest_frame, display_buffer}`, three hashes (capture, state, frame) and a readback read after the command buffer completed. `rsx_frame_identity` gained `guest_frame`. `tools/ps3diff.py validate` checks a capture end to end (blobs, canonical lines, the FIFO walk, CALL/RET balance, the draw accounting `submitted = executed + dropped`, the hashes) and `rsx-compare` compares two captures (not comparable on a different environment record, an error on zero compared frames). A capture that did not start at `cellGcmSetupContext`, a blob that disagrees with its digest, two reads of one event that disagree and an unsupported readback format are the stop `rsx-capture`. Capture is off by default and the walker and engine then run unchanged; the engine's revalidation hashes and its writes no longer share the consumed-read resolver, and a vertex, index or texture span that would wrap past 4 GiB is refused instead of folded. Tests: `rsx_capture` (walker and engine fixtures plus the validator's failure paths). Not in this entry: replay, NV0039 and the unknown-method classes.
- **The sceNpTrophy error codes are the firmware's `0x800229xx`, not an invented `0x805516xx` family** (#37; `libs/network/sceNpTrophy.{c,h}`; FW 4.93 `libsysutil_np_trophy` for `0x80022901` already initialized, `02` not initialized, `03` not supported, `06` invalid argument, `0A` unknown context, `0F` processing aborted, `11` unknown handle; RPCS3 `sceNpTrophy.h` at `e53ac18f` for the rest): a title compares the codes it gets with its SDK header's, so every number the HLE returned was wrong (the header's `INVALID_ARGUMENT` was `0x80551603`, the firmware's is `0x80022906`), and codes that exist in no firmware or RPCS3 source (`ALREADY_EXISTS`, `NOT_FOUND`, `CONTEXT_ALREADY_REG`, `PROCESSING`) are gone. A context or handle the library does not hold is now `UNKNOWN_CONTEXT` / `UNKNOWN_HANDLE` (the old `INVALID_CONTEXT` / `INVALID_HANDLE` names are the firmware's different codes `0x8002290E` / `0x80022917`); `sceNpTrophyDestroyContext` / `DestroyHandle` answer an id outside the table `INVALID_ARGUMENT` first, as RPCS3 does, and `sceNpTrophyRegisterContext` answers options above 1 `NOT_SUPPORTED` (FW 4.93 `0x02001664`). Test: `hle_trophy_errors` checks the guest-visible numbers through the API.
- **`sceNpTrophyRegisterContext` blocks and its processing statuses reach the title from `cellSysutilCheckCallback`, queued gradually, instead of one early completion** (#37; `libs/network/sceNpTrophy.c`, `libs/system/cellSysutil.{c,h}`, `include/ps3emu/config_options.def`, `docs/CONFIG.md`; FW 4.93 `libsysutil_np_trophy` `0x02001614..0x02001B68` and libsysutil `0x02004E98` as lifted in `TS-FW-PACKETREAD-opus.md`, RPCS3 `sceNpTrophy.cpp` at `e53ac18f`). Before: the status callback got `INSTALLED` whatever the install state, a negative return from it was ignored, and one `PROCESSING_COMPLETE` was queued for the next poll; a title that keeps a `{valid, status, completed, total}` mailbox and clears `valid` when its registration thread returns lost the completion and polled an invalid mailbox for ever (a silent boot stall at trophy setup). Now: (1) the first status is `NOT_INSTALLED` or `INSTALLED` from the install record (the store file for the community id; `sceNpTrophySetStoragePath` / `PS3_GAMEDATA_ROOT`); a record that cannot be classified is `DISC_IO`, never an assumed `INSTALLED`, the first registration writes the record, and one log line names the status and the record path. `sceNpTrophyGetRequiredDiskSpace` is 0 once installed. (2) A negative return from that first callback clears the registration and returns `PROCESSING_ABORTED` (0x8002290F), read through `ppu_guest_call`, which returns r3 (the `g_ps3_guest_caller` hook stays void for every other caller). (3) The call then blocks while SETUP 0..3/3, PROGRESS 0..N-1/N-1, FINALIZE 0..max(N-1,9)-5, COMPLETE 0/0 (RPCS3-oracle counts; the firmware does not show them) are queued one per step, paced by vblanks (`cellGcmGetVBlankCount`; RPCS3's 2 s minimum as 120 vblanks over the events, so a run does not depend on the host's speed), never more than the firmware queue's 127 at once. They run only from `cellSysutilCheckCallback`, on the polling thread, which drains everything queued so far in one call, never on the blocked thread. The call returns after a poll consumed COMPLETE and one step more (a heuristic: returning earlier is the measured failure). (4) The wait is bounded by `trophy.register_wait_cap_vblanks` (default 1024, RPCS3's worst case); at the cap it returns success, logs `register-wait-cap`, counts it (`sceNpTrophy_register_wait_cap_count()`) and a later poll delivers the rest, which is also what happens when one thread registers and polls. A vblank counter that never ticks is `gcm-wait-timeout` (`what=trophy-vblank`). (5) A context destroyed (or `sceNpTrophyTerm`) mid-lifecycle ends the call and every later status; no callback runs after the destroy returns. (6) New `cellSysutil_register_poll_hook`, a pump run by each poll. Tests: `hle_trophy_registration` (a registration thread, a pumping thread, a mailbox cleared when the call returns; eleven cases, fail-before on ten; also in the `tsan` tier).
- **A lift and the runtime it runs on cannot silently disagree about the lifted-code ABI** (review F6; `tools/ppu_lifter.py`, `tools/make_smoke_elf.py`, `include/ps3emu/lifter_abi.h`, `runtime/diag/lifter_abi.c`, `runtime/ppu/ppu_loader.cpp`, `docs/RUNTIME.md`, `docs/STOPS.md`): the generated header gained `PPU_STORE_HOOK` in the store-hook change, and a port that did not regenerate its lifted sources still linked and ran — every raw store in it went through no hook, so a label could be overwritten before the walker evaluated the ACQUIRE queued for its old value, with nothing to say so. The lifter now stamps `PPU_LIFTER_ABI` into the generated header and defines `ppu_lifter_abi` in the generated source (the table chunk); `ppu_load_elf` compares it with the runtime's `PPU_RUNTIME_LIFTER_ABI` before it reads the file and stops with the new kind **`lifter-abi`** (`expected got`) when they differ. Sources lifted before the stamp existed define no symbol at all, so the loader does not link: loud, never silent. The hand-written lifts in the test tree (smoke, fiber, loader fixtures) carry the stamp too. Test: `ctest -R lifter_abi` (a matching lift runs, another stops by name, an unstamped one fails to link).
- **The store-hook, GCM label-store and lifter-conformance suites run under `ctest`, and the store-hook fixture covers the store-conditionals and the 32-bit top** (review F7; `CMakeLists.txt`, `runtime/ppu/tests/test_ppu_store_hook.cpp`): the three Python suites were only run by the macOS tool-suites job, so the GCC t0 job never saw them. The store-hook fixture now also checks that a successful `stwcx.`/`stdcx.` leaves CR0 = EQ only, that a failed one stores nothing, clears CR0[EQ] and still runs the hook once (the wait `ppu_stwcx32` performs before it validates, which a retry loop repeats), and that a span from the top of the address space both hooks a window in the last bytes and does not reach a low window (the inline modular compare can only say "maybe"; the out-of-line 64-bit bound decides). Tests: `ctest -R 'ppu_store_hook|gcm_label_store|ppu_lift_conformance'`.
- **`cellSync.h` is includable from C++ again, and its atomic fields are pinned in both languages** (review F8; `libs/sync/cellSync.h`, `tests/test_cell_sync_header.cpp`, `docs/FAQ.md`): the header brought in `<atomic>` and exported `using std::atomic_int/atomic_uint` into every include site, so a C++ TU that included `<stdatomic.h>` first then failed inside `<atomic>` (libstdc++'s `kill_dependency` macro, C++11..20; seen with clang++18 + libstdc++13). C++ now spells the fields as plain 32-bit integers — the generated NID table is the only C++ consumer and performs no atomic operation on them — and the header asserts that both spellings are four bytes, which `cellSync.c` (C11 atomics) and the new C++ TU both compile against. The TU is the in-tree C++ consumer the GCC t0 job lacked, and it includes `<stdatomic.h>` before the header on purpose. Test: `ctest -R cell_sync_header`.
- **The SPURS task ELF check follows RPCS3 `spursTasksetLoadElf` point for point, and STOPS.md matches the code** (review F4/F5; `runtime/spu/spu_workload.c`, `tests/test_spurs_dispatch_miss.c`, `docs/STOPS.md`; RPCS3 `cellSpursSpu.cpp:1997-2047` at e53ac18f): the program-header scan now ends at the first header of any type whose `p_paddr` is at or past the task bottom (it skipped non-loadable headers and could halt on a segment RPCS3 never reaches); `p_vaddr + p_memsz` wraps in 32 bits; segments are checked against the rest of guest memory, not the 2 MiB-clamped image size; an ELF with no loadable bytes is accepted (the dispatcher bounds it); and the restart scan's 16-byte-misaligned ELF is `unaligned-elf`. STOPS.md lists the reasons the code raises (`unaligned-elf`, `elf-outside-guest-memory`, `segment-outside-task-area`) and no longer claims a misaligned-taskset check in `cellSpursCreateTask`. Tests: four new `spurs_dispatch_miss` modes (paddr break, vaddr wrap, empty ELF, restart bit 3); the paddr-break mode fails on the old scan (it halted the task).
- **SPURS task, policy-module and job error codes carry the firmware's values** (review F2, #36; `libs/spurs/cellSpurs.h`, `libs/spurs/cellSpurs.c`, `tests/test_spurs_error_codes.c`, `tests/test_spurs_queue_size.c`; libsre 4.93 `cellSpursCreateTask` 0x02012520, `cellSpursTaskExitCodeInitialize` 0x02013638; RPCS3 `cellSpurs.h` at e53ac18f): the `CELL_SPURS_TASK_ERROR_*` macros sat on the 0x804107xx base, while the firmware returns 0x804109xx (null 0x80410911, misaligned 0x80410910, state 0x8041090F). The task paths that spelled the literals out were right; `cellSpursQueueSize`, which used the macros, returned 0x80410731/0x80410730 to the guest. The task family now has its own base 0x80410900, the policy-module (0x804108xx) and job (0x80410Axx) families are added where firmware or RPCS3 back a value, and the HLE returns the macros. Test: `spurs_error_codes` checks the code the guest sees for one API per family (core, task null/align/stat) and pins the macro values; `spurs_queue_size` now expects the firmware literals (before: it pinned the old macros).
- **Every presented image is no longer black: `cellRescSetBufferAddress` copies the convert program's microcode into the fragment buffer** (M3 slice S2a; `libs/video/cellResc.c`, `docs/RSX_GRAPHICS.md`; libresc `0x02006C70`). The convert-and-flip stream names the fragment buffer by offset only, and the library fills it with the program's microcode when the title hands it over; the HLE kept the title's (zeroed) buffer, so the one-draw conversion from the composed surface into the display buffer ran an empty program, sampled no texture (the engine's pass index showed the draw with no texture unit) and wrote black on every present. The DC program (progressive, non-PAL-interpolating: the 720p/1080p case) is now one TEX instruction written big-endian into the 16 bytes at the buffer. E0/E4/E8 serve interlaced and PAL-interpolating modes, whose `cellRescSetConvertAndFlip` already stops by name, so they copy nothing. A fragment buffer outside guest memory is the `hle-guest-range` stop. Tests: `resc_convert` (the buffer holds a TEX program after SetBufferAddress, 16 bytes and no more; before: the title's bytes stay) and `resc_present_metal` (Metal, skips without a device: a composed gradient surface, the library's stream through the real draw engine, the display buffer equals the source to ±1; before: black, worst channel error 255).
- **A guest span the host cannot commit stops by name in cellSaveData too, and `vm_commit` results can no longer be dropped** (`runtime/memory/vm.h`, `runtime/syscalls/sys_fs.c`, `libs/system/cellSaveData.c`, `docs/STOPS.md`; #29). Before: cellSaveData ignored the `vm_commit` result for the funcFile `fileBuf`, so when the host could not back the pages, the kernel I/O through them faulted (SIGSEGV) instead of stopping by name. The chunked commit-or-stop path that `sys_fs_read`/`sys_fs_write` used is now the shared `vm_commit_or_stop` in `vm.h`, and cellSaveData uses it. `vm_commit` is `PS3_NODISCARD`; no caller in the tree discards it. Test: `savedata_filebuf_range`, which unmaps a page under an in-arena `fileBuf` and expects `hle-commit-failed` (before: exit by signal 11).
- **A guest store of any form into the RSX label window is ordered after the walker** (#22; `runtime/ppu/ppu_loader.cpp`, `tools/ppu_lifter.py`, `docs/RUNTIME.md`): the store hook of #17 ran for `vm_write32` only, so a 64-bit store, an 8/16-bit store, `stwcx.`/`stdcx.`, the byte-reversed stores, `stvx` and the other vector stores, and `dcbz` could still overwrite a label before the walker had evaluated the ACQUIRE queued for its old value. All of them now run the hook before the bytes land, once per store that overlaps the window by at least one byte (the hook gets the first window address the store covers); `stmw` and `stswi`/`stswx` run it per element. The lifter's raw stores call `PPU_STORE_HOOK(ea, n)` (new, in the generated header), an acquire load and one compare outside the window. HLE stores and SPU DMA are not hooked (documented). Ports regenerate their lifted sources to get the raw forms. Test: `tools/test_ppu_store_hook.py` lifts 25 store forms with the real lifter and runs them against the real loader (73 of its checks failed before the change).
- **A loadable SPURS task or job with no lifted program is the stop `spurs-dispatch-miss`** (#20; `libs/spurs/cellSpurs.c`, `runtime/spu/spu_workload.c`, `docs/STOPS.md`): `spu_workload_dispatch_async` / `spu_workload_dispatch_job` return 0 when the registry has no entry for the image (or the host cannot start the worker), and `cellSpursCreateTask`, the taskset restart scan and the job-chain walker ignored it: the task was reported started, the job done. They now stop with `api taskset task image size fingerprint` (a job: `job` instead of `taskset task`). This supersedes the "Not changed: ... still a logged MISS" remark of the entry below. Also `spu_task_elf_halt_reason`: the policy module's loader checks (RPCS3 `spursTasksetLoadElf`) — a null EA, a non-ELF, a PT_LOAD outside local store `0x3000..0x40000` (`segment-outside-task-area`), an EA outside guest memory (`hle-guest-range` from the create call, `elf-outside-guest-memory` from the restart scan) — run before any host read of the image. A title with SPU images it has not lifted now stops at the first one (`SPU_DUMP_MISS=<dir>` still writes it out first).
- **A SPURS task or job the HLE cannot dispatch is a named stop or the firmware's error, never `CELL_OK`** (#20; `libs/spurs/cellSpurs.c`, `runtime/spu/{spu_workload.c,spurs_job.c}`, `include/ps3emu/stop.h`, `docs/STOPS.md`; RPCS3 `cellSpurs.cpp` `_spurs::create_task`, `cellSpursSpu.cpp` `spursTasksetLoadElf` / `spursHalt`): `cellSpursCreateTask` returned success for a task whose ELF it could not parse (the size probe returned 0, so nothing was dispatched), the taskset restart scan left such a task pending forever, and the job-chain walker logged `implausible binary -- skipped` for a job with no binary and went on. On the console `create_task` checks only a null ELF (`0x80410911`) and a misaligned one (`0x80410910`): the HLE now returns those, before a task id is taken. An ELF the SPU-side policy module cannot load (`spursTasksetLoadElf` fails; `spursHalt`) is the new stop **`spurs-task-halt`** (`api taskset task elf reason`; the create call carries the calling guest thread as its site), from the create call and from the restart scan (`null-elf` too). A job descriptor that names no binary, or whose binary leaves guest memory, is `spu-job-dma-list` (`eaBinary` / `sizeBinary` / `binary-range`, `image=-1`) through the new `spu_job_check_binary`, before the walker fingerprints the bytes (the old `ea >= 0x10000000` cut-off also skipped binaries in user memory that are valid). Not changed: an image that parses but has no registered lifted entry is still a logged MISS. Tests: `ctest -R 'spurs_dispatch_miss|spu_taskset_elf_flags'` (fail before: every mode but the plain ones); `tests/gates/test_stop.py` pins the kind.
- **The draw engine accounts for every draw, names its dumps by frame identity, and counts pipeline failures** (`libs/video/rsx_draw_engine.{c,h}`, `libs/video/rsx_metal_backend.{m,h}`, `libs/video/tests/test_rsx_draw_{accounting,dump_names}.c`, `libs/video/tests/rsx_draw_engine_test_stub.h`, `docs/RSX_GRAPHICS.md`, `docs/DIAGNOSTICS.md`; M3 gaps 3a-3e and 3g): every early return in the draw path now goes through `eng_drop(reason)` (15 named reasons) with relaxed-atomic per-reason counters, a power-of-two `[rsx-drop] reason=<name> count=<n>` line and `rsx_draw_engine_draw_stats()`, so `submitted = executed + sum(dropped)` is checkable for every run; pipeline failures are counted per fragment program (`rsx_draw_engine_pipeline_failures()`, `[rsx-pipeline-fail]`) and no longer only reported by the 32-line detail log. One identity `{flip_id, present_seq, display_buffer}` is recorded at each present (`rsx_draw_engine_frame_identity()`): `flip_id` is the GCM flip-head ordinal — the same `0xE924` bit-31 word `cellGcmSys` counts for `GCM_DRV_EVENT_FLIP_HEAD` — and `present_seq` is the engine's present ordinal; both are recorded, neither is derived, because about three presents share one flip. Pass-dump and frame-dump files are named `s<present_seq>_f<flip_id>`, so the two presents between two draw passes that used to rewrite one `presentNNN.ppm` (G0 measured 130 of 389 present images lost that way) now get distinct files; the frame dump selects presents with `PS3RECOMP_METAL_FRAME_DUMP_RANGE`/`_LIST` and counts an unsupported surface format instead of skipping it. Tests: `ctest -R 'rsx_draw'` — accounting and identity, and the dump-naming test, which fails 10/10 against the previous engine.
- **JQ2 pushes block on a full ring and take the firmware's flag masks** (`libs/spurs/jq2/{executor.cpp,executor.hpp,jq2.h}`, `docs/JQ2.md`, `docs/STOPS.md`; FW 4.93 `PushJobBody` / `CopyPushJobBody` / `PushSync` wrappers and the ring writer `0x02001138`): a push on a full command ring returned `A01` whatever its flags, so a title that pushes with flags 1 (as Odin does) saw an error where the console's PPU waits for room. Now `A01` comes only with flags bit 2 (PushJob, PushSync), with the accounting rolled back and nothing written; without it the pusher waits until an intake pass has consumed an entry, a fiber parking in `cellFiberPpuWaitSignal` (its worker keeps running other fibers) and any other thread on the queue's progress condition. The flags are the first test of each API: PushJob `flags & ~7`, CopyPush `flags & ~3`, PushSync `flags & ~4` (so 0 and 4 only; it accepted 0 and 1 and refused 4) are `A02`. Bit 1, which the firmware stores in `w0` bit 2 of the entry for the PM (whose reading of it is not decoded), is the named stop `jq2-unmodelled` (`api port flags`) after every firmware check. Shutdown while a pusher waits is the named stop `jq2-unmodelled reason=push-blocked-at-shutdown`; a fiber pusher whose wait is refused or that cannot be signalled is `jq2-wake reason=push-room-wait` / `push-room-notify`. Removes the counted divergence `JQ-S15`. New hook `ExecutorHooks::on_push_blocked`. Tests: `ctest -R 'jq2_exec_(push|pushsync|stop_push)|jq2_fiber_(push_full|stop_push_wait)'`.
- **JQ2 pushes take every descriptor size the firmware does** (`libs/spurs/jq2/{ring.hpp,ring.cpp,executor.cpp,jq2.h}`, `docs/JQ2.md`, `docs/STOPS.md`; FW 4.93 PushBodyInternal): `PushJobBody` / `CopyPushJobBody` accept a size of 64 or any multiple of 128 up to the queue's maxDescriptor (256..896), where the HLE accepted only 64, 128 and 256 (`A02` for 384..896 on a queue that allows them; Odin creates its queue with 896). The ring carries the size as the firmware's 3-bit class in `w1` bits 3..1 (`SizeCode` is now 0, 2 ... 14), so the codec no longer refuses a class of 6 or more or a `w1` bit 3. A size of 0, which the firmware's test also passes, is the named stop `jq2-unmodelled` (`size=0`). Removes the counted divergence `JQ-S05`. Tests: `ctest -R 'jq2_ring_codec|jq2_exec_desc_sizes|jq2_exec_stop_(copy_)?push_size0'`.
- **PPU thread join/detach is the lv2 state machine, and a worker never touches a reaped slot** (`runtime/syscalls/sys_ppu_thread.{c,h}`, `docs/RUNTIME.md`, `docs/SYSCALLS.md`): a thread's epilogue used to publish `FINISHED` and release the table lock before it locked, set and signalled its join mutex, so a concurrent `sys_ppu_thread_detach` could destroy the mutex under it (and, once the slot was reused, the old worker set `finished` on the new thread, whose joiner then read its status early). The slot now carries an explicit joiner state (`JOINABLE`, `JOINING(tid)`, `DETACHED`) next to `RUNNING`/`ZOMBIE`/`FREE`; the epilogue's last access to the slot is one section under the table lock, and exactly one party reaps (the worker when detached, the joiner, or the detacher of a zombie). Error codes follow RPCS3's `ppu_join_status`: `join(self)` is `CELL_EDEADLK` (it hung), a second join or a join of a detached thread `CELL_EINVAL` (a second join joined twice), a second detach `CELL_EINVAL` (it answered `CELL_OK`), a detach while a join waits `CELL_EBUSY` (it destroyed the condition variable under the waiter). `sys_ppu_thread_exit` no longer wakes the joiner of a created thread; the epilogue does. The Windows half follows the same machine and is not built or run here. Test: `ctest -R ppu_thread_join_detach` (`ppu_thread_set_epilogue_hook` parks the worker past its last table-lock section).

- **`sys_fs_read`/`sys_fs_write` follow the RPCS3 oracle's order and bound the guest buffers they hand the kernel** (`runtime/syscalls/sys_fs.c`; RPCS3 `Emu/Cell/lv2/sys_fs.cpp` `sys_fs_read`/`sys_fs_write`): a null `nread`/`nwrite` is `CELL_EFAULT` before anything else; a null buffer writes 0 to that out-pointer and returns `CELL_EFAULT`; an fd that is not a file, or an access-mode mismatch (more than zero bytes read from `O_WRONLY`, written to `O_RDONLY`) writes 0 and returns `CELL_EBADF`; a zero-byte call is `CELL_OK` with 0 reported and the file body skipped (the flags were already stored in `sys_fs_fd_info.flags`, so no new field). The host-policy range checks slot into that order: the out-pointer's 8-byte span is checked right after its null test, before the firmware's first write of 0, and the buffer span after the fd/mode checks and before the commit and `fread`/`fwrite`; a span outside guest memory stops as `hle-guest-range` (`func=sys_fs_read`/`sys_fs_write`, `arg=buf`/`nread`/`nwrite`). `vm_commit` now receives the whole checked span (the old `(uint32_t)size` cast dropped the top half of any span above 4 GiB, so a size whose low 32 bits were in range passed the check and then read past the arena), split at a page boundary for the one arena-wide span a `u32` cannot express; a commit that fails after the span is proved in range stops as the new kind **`hle-commit-failed`** (`ps3_stop_commit_failed(func, arg, ea, size, rc)`, `docs/STOPS.md`, `tests/gates/test_stop.py`) instead of reading pages the host never made writable. `sys_fs_write` on an fd that is not a file used to write `size` to `*nwrite` and return `CELL_OK` ("pretend write succeeded"), i.e. fake success for a write that never happened (introduced in 8be0c21 for a CRT that writes through corrupted fds before initialisation — that caller now sees EBADF). `cellSaveData`'s `dispatch_func_file` checks `[fileBuf, fileBuf+fileBufSize)` before forming the host pointer or committing it; an out-of-arena `fileBuf` from the funcFile callback stops as `hle-guest-range`. Tests: `ctest -R sys_fs_guest_range` and `-R savedata_filebuf_range` (guard-paged arena, forked stop cases, the oracle's null/EFAULT, EBADF and mode-mismatch returns with `*out == 0`, the out-pointer-before-null-buffer order, and an in-range round trip).

- **GCM and VideoOut entry checks follow libgcm_sys and the RPCS3 oracle** (`libs/video/cellGcmSys.c`, `libs/video/cellVideoOut.{c,h}`, `docs/STOPS.md`; R4 phase 2c): `cellGcmMapMainMemory` answers a misaligned EA or size `CELL_GCM_ERROR_FAILURE` (0x802100FF, libgcm_sys `0x020048D8`; it was `INVALID_ALIGNMENT`), EA before size; size 0, a map before `cellGcmInit` and a full runtime mapping list are named stops (`gcm-unmodelled` `zero-size`/`io-mapping-slots`, `hle-uninitialized`) instead of invented codes. `cellGcmAddressToOffset` answers an EA with no IO page `FAILURE` (it was `INVALID_VALUE` for page 0) and no longer writes `*offset` on failure. A NULL out-parameter of `cellGcmGetConfiguration`, `cellGcmAddressToOffset` and `cellGcmMapMainMemory` is the null-page store the firmware makes (`guest-null-access`), not `INVALID_VALUE`. `cellGcmBindTile` (index 15+ is `INVALID_VALUE`), the `cellVideoOut` `!state`/`!resolution`/`!config` checks are cited as firmware/oracle-confirmed. `cellVideoOutGetState` returns `DEVICE_NOT_FOUND` for an unknown videoOut, the secondary output and a deviceIndex but 0 (RPCS3 `cellVideoOutGetState`); `cellVideoOutConfigure` on a videoOut other than PRIMARY is a named stop. The `CELL_VIDEO_OUT_ERROR_*` values are RPCS3's `0x8002B220..0x8002B228` (were `0x8002B101..108`). Tests: `tests/test_gcm_entry_checks.c` (`tools/test_gcm_entry_checks.py`), `video_resolutions_checks`, `video_resolutions_configure_secondary`; `test_gcm_io_alloc.c`, `test_gcm_io_mappings.py`, `test_gcm_map_flags.c` follow the new stops.
- **A guest store to an RSX label is ordered after the walker** (#17; `libs/video/cellGcmSys.c`, `runtime/ppu/ppu_loader.cpp` `ppu_set_store_hook`): a title that syncs the PPU and the RSX through a label queues a NV406E ACQUIRE(N) for it, kicks `put`, and overwrites the label with N+1 as soon as it builds the next frame. The ACQUIRE compares for equality (the PFIFO puller; RPCS3 `nv406e::semaphore_acquire` is `while (sema != arg)`), and nothing in the guest orders the kick and the store — the console's RSX simply fetches the kick within microseconds. The walker here is a polling host thread: on a loaded host (other builds, load average 6-13 on 12 cores) it was off the CPU for several milliseconds while the title's next store came 5.27 ms after its kick, so it waited for N for ever and the run stopped with `gcm-wait-timeout` (`semaphore-acquire`, `value=N current=N+1`; 2 of 3 loaded C3 runs, and a 15-minute unloaded soak at 785 s with the same signature). Now a 32-bit store by guest code into the label window (`vm_write32`, before the store lands) waits for one complete walker pass that began after the store was reached, when `get` is not at `put`; the walker evaluates the kicked ACQUIRE against the old value, then the store lands. A thread that has run a pass never waits (it cannot wait for itself); a walker that never runs a pass is the new `gcm-wait-timeout` `what=rsx-walker-pass` (`gcm.wait_timeout_ms`, `docs/STOPS.md`). An ACQUIRE the RSX is held in front of (the flip wait) is still missed by a store, as on the console. `ppu_set_store_hook(lo, len, fn)` is generic: a 32-bit-store hook over one window, off (one subtract and compare per store) until a device model registers. Tests: `tools/test_gcm_label_store.py` (control: a plain store overtakes a late walker and holds it at ACQUIRE(N); ordered; no walker), `tools/test_ppu_store_hook.py` (the loader's `vm_write32`).
- **The vblank has a rate and can be raised from its own thread** (`libs/video/cellVideoOut.{c,h}`, `libs/video/cellGcmSys.c`): `cellVideoOut_refresh_rate(&num, &den)` returns the current mode's refresh rate as a fraction (60000/1001 in every mode; `cellVideoOutGetState` now reports `CELL_VIDEO_OUT_REFRESH_RATE_59_94HZ`, the new flag constants `59_94HZ/50HZ/60HZ/30HZ` are in the header), so a host derives its vblank period from the mode and schedules it on absolute deadlines instead of a fixed tick. `cellGcmTickVBlank` is now `cellGcm_vblank_signal` (the counter and the handler's call, safe from a thread of its own) followed by `cellGcm_vblank_flip_label` (the driver's answer, zeroing the flip-done label, where the walker runs and after it has presented the frame behind the flip head). `ppu_gcm_pump` calls the vblank handler once per vblank raised since the last pump, with consecutive vblank numbers, instead of once for however many were pending (one pending is the old single call). All three new entry points are host helpers (`tools/test_gen_hle_nids.py`, `docs/HLE_ABI.md` census). Tests: `ctest -R "video_resolutions|resc_flipstatus|resc_waitflip"`.

- **A guest access into the null page stops** (`docs/STOPS.md`, `docs/CONFIG.md`): a load below `0x10000` or a store below `0x1000` ends the run at the first one with the new kind **`guest-null-access`** (`access width ea fn`, plus `pc lr tid`; the guest call chain goes to stderr first). It used to log `[null-read]` and return what the flat VM held, or log `[null-write] ... DROPPED` and swallow the store, so a worker that read and wrote `NULL+0x5B0..0x5DC` ran on garbage and hung later. **`ppu.null_page = "drop"`** (default `stop`; `PS3RECOMP_PPU_NULL_PAGE`) keeps the old log-and-drop handling for a title that needs it, and now carries the containment of a store sweep that walks up out of the page (a memset on a null buffer), which was opt-in through `PS3_NULL_SWEEP`. A title config that sets `drop` must say why in a comment (`docs/CONFIG.md`). Removes `PS3_NULL_READ`, `PS3_NULL_WRITE` and `PS3_NULL_SWEEP`; `PS3_NULL_READ_STACK` still adds the guest call chain to a `[null-read]` under `drop`. `vm_read16` had no null-page check at all (a halfword load from page 0 neither logged nor stopped); it has the same one as the other widths now. The inline `vm_read*` fast path is unchanged: it already sends every address below `0x10000` to the checked accessor, and stores were never inlined. Test: `ctest -R loader_gaps` (every width, the page's bounds, `drop`), `ctest -R config`.
- **cellRescSetWaitFlip(ctx)** (NID `0x0D3C22CE`; `libs/video/cellResc.c`, libresc `0x020045E8`, export `0x02000488`) was unregistered, so the title's first call after SetConvertAndFlip stopped as `unresolved-nid`. It now appends the firmware's 4-word group to the command context — NV406E SEMAPHORE_OFFSET `0x10` (the flip-done label `_cellGcmSetFlipCommand` releases as `0xFFFFFFFF`; `18 << 4` for 720x576 with a PAL temporal mode other than 0 and 5) and SEMAPHORE_ACQUIRE `0` — reserved like every library group (buffer-full callback with 4 words), with no `cellRescInit` requirement, as the firmware has it. The walker side (`cellGcmSys.c`): the driver completes a retired flip head at the next `cellGcmTickVBlank` by writing 0 to that label, which releases the ACQUIRE (a flip nobody completes is the `gcm-wait-timeout` stop), and `cellGcm_take_flip_pending_synced` presents the frame behind a stalled ACQUIRE (`get != put` there is the wait, not an undrained frame). Test: `ctest -R resc_waitflip`. `_cellGcmFunc15` is not part of this path (libresc does not import it).
- **GCM single-TU fixtures** (`libs/video/tests/test_gcm_{flip_stream,get_on_jump,get_on_release}.c`, `tools/test_gcm_*.py`): link `gcm_default_state.c` and the new `gcm_fixture_stubs.h` (`ps3_config_get`, `ps3_stop_gcm_wait_timeout`), start their streams at the command area (`io + 0x1000`, the second segment at `io + 0x9000`), and exercise the blocking ACQUIRE (held until the label is written) instead of the retired non-blocking default. `hle_observer`'s fixture takes a lock around the state its callbacks share (TSan).
- **HLE call observer** (`include/ps3emu/hle_observer.h`, `runtime/ppu/ppu_hle.cpp`): `ps3_hle_set_observer(&obs)` installs one observer that `ps3_hle_call` reports every firmware import to — `on_call` before the handler (r3..r10 as the guest left them) and `on_return` after it (r3 the return value), on the calling guest thread. The cost with none installed is one atomic load per import. For a port that compares its run with a recording (the title's C3 event gate). The dispatch body is now `ps3_hle_dispatch`; `ps3_hle_call` is the thin wrapper around it. Test: `ctest -R hle_observer` (order, arguments, return, four threads, replacement, removal).
- **ps3diff** (#13): `validate`, and with it `diff` and `convert`, now rejects a dump whose `hex` does not decode to exactly `read` lowercase bytes whose SHA-256 equals its `sha256`. The error names the dump, the record seq and the context. `diff` still compares digests; before this change a stale or truncated `hex` passed both steps silently.
- **cellRescSetConvertAndFlip(ctx, idx) builds the firmware's FIFO stream** (`libs/video/resc_convert.{c,h}`, `cellResc.c`, `cellGcmSys.c`; libresc `0x02004D4C`, libgcm_sys `0x02008948`): it appended nothing but a host flip request before (and took the guest context pointer for its index). Now the command context at `ctx` receives the convert pass built from RESC state — default 3D state, scissor/viewport, the two vertex attribute arrays, the colour surface (format from the mode's size, pitch, this frame's colour buffer), the source texture (format byte, size, pitch, float formats unfiltered), the pass-through vertex program (encoded from instruction fields, no microcode bytes), the fragment program binding, the QUADS draw — and then `_cellGcmSetFlipCommand`'s 19 words (driver queue `0xE944` with the buffer id, the NV406E label handshake, `CALL 0`, driver flip head `0xE924`), one reservation group per firmware reservation so the buffer-full callback fires at the firmware's points (`cellGcm_ctx_reserve`; a callback that leaves no room is the `gcm-unmodelled` stop). The frame toggle (`+0x110`) picks the colour buffer. Argument checks are the firmware's: not initialised `0x80210301`, source index above 7 `0x80210304`. `cellRescSetBufferAddress` is implemented (NULL `0x304`, alignment `0x303` for 128/4/64 bytes, the colour buffer offsets `offset + i * <size SetDisplayMode took>` registered with `cellGcmSetDisplayBuffer`, the quad written into the vertex buffer for the display mode's ratio mode, the program's RSX offset); `cellRescSetDisplayMode` snapshots the mode's size and pitch; `cellRescInit` installs the default Dsts records (`{8, tiled pitch of width*4, 8}`); `cellRescAdjustAspectRatio` validates `[0.5, 2.0]` and rewrites the quad. The walker (`cellGcmSys.c`) handles `0xE944` (queue the display buffer) and `0xE924` with bit 31 (present it, stopping the drain pass like the `0xFEAD` marker). Not reproduced, and named stops (`gcm-unmodelled`): interlaced configurations, 720x576 with PAL temporal mode 1..4, more than 256 vertices; the fragment program's microcode copy into the fragment buffer is not done (no microcode is held; M3 needs it). New host helpers `cellGcm_ctx_reserve`, `cellGcm_ctx_emit`, `cellGcm_emit_flip_command`, `cellGcm_address_to_offset`. Tests: `ctest -R resc_convert` (synthetic 1080p stream, toggle, walker present order, callback points, quad/vertex-program forms, stops); the title repo's `resc_stream` replays Odin's 720p configuration against the recorded stream.
- **`gcm-unmodelled`** (`docs/STOPS.md`): a new named stop for a GCM/RESC command-stream path the firmware has and the runtime does not reproduce, with `ps3_stop_gcm_unmodelled(func, feature, reason)`.
- **`_cellGcmInitBody` builds libgcm_sys's command-buffer layout** (`libs/video/cellGcmSys.c` `cellGcmSetupContext`, `libs/video/gcm_default_state.{c,h}`, `runtime/ppu/ppu_sysprx.cpp`; libgcm_sys `0x020009A8`, `0x020036D4`, `0x02003AAC`): the IO window now starts with the driver default-state subroutine (the three firmware routines `0x02005560`/`0x02005E74`/`0x0200663C` are stores of immediates and three small loops, so it is generated, 611 words and a RET, checked word for word against a recording), the context is the first 32 KiB segment at `io + 0x1000` (`end = io + 0x7FFC`), put and get start at `0x1000`, and the buffer-full callback does what `0x02003AAC` does — flush, JUMP to the next segment, wrap to `io + 0x1000` after the last, wait while the RSX is inside the new segment or outside the ring. `cellGcmGetDefaultCommandWordSize` / `cellGcmGetDefaultSegmentWordSize` return the whole-segment command words and 8192 (they returned fixed 0x400 / 0x100). `cellGcmSetDefaultCommandBuffer` resets the default context to its first segment. InitBody now enforces the firmware's argument checks (`cmdSize` ≥ 0x10000 and a word multiple, `cmdSize + 0x1000 <= ioSize`, `ioSize` and `ioAddress` 1 MiB aligned) and returns `CELL_GCM_ERROR_FAILURE`; the GCM error base is `0x80210000` (the firmware returns `0x802100FF` / `0x80210001`), not `0x80210700`, and `cellGcm_emit_flip_command` returns FAILURE for a buffer id above 7. Tests: `ctest -R gcm_initbody`; callers that wrote a stream at io 0 (`gcm_reports`, the PPU boot smoke) start it at the command area.
- **NV406E semaphore ACQUIRE blocks, with a named stop** (`libs/video/cellGcmSys.c`): the walker holds an ACQUIRE until the label holds the value (it used to consume it unless `GCM_SEMA_ACQUIRE=1`; both `GCM_SEMA_ACQUIRE` and `GCM_SEMA_ACQUIRE_MS` are removed). The one answer the runtime supplies is the driver's flip-ready handshake (a queued `0xE944` flip, then ACQUIRE label `0x30` == 1). A wait nobody ends stops after `gcm.wait_timeout_ms` (default 10 s, `docs/CONFIG.md`) with **`gcm-wait-timeout`** (`docs/STOPS.md`, `ps3_stop_gcm_wait_timeout(func, what, offset, value, current, waited_ms)`); the segment callback's wait for the RSX uses the same stop.
- **GCM_SET_USER_COMMAND is ordered** (AM-27; `cellGcmQueueUserCommand`, `ppu_gcm_pump`): the single-slot, coalescing pending marker is a bounded lock-free multi-producer ring (1024 commands); every `0xEB00` the RSX retires reaches the handler in retirement order, a full ring drops the newest and counts it (`gcm_user_ring_drops()`). The walker also counts driver events in retirement order (`gcm_driver_event_count(kind)`, `gcm_set_driver_observer(fn)`: queue-flip `0xE944`, flip-head `0xE924`, user command) for event-sequence comparisons.
- **The HLE page, and with it the label window, is where the port puts it** (`ppu_hle_inject_base`, existing): `cellGcmGetLabelAddress(i)` is `base + 0x10 * i`; a port that wants the recorded label addresses assigns the base before loading the executable.
- **`sys_process_get_sdk_version` is the title's** (`runtime/ppu/ppu_loader.cpp`, `runtime/syscalls/sdk_version.c`): `ppu_load_elf` reads `sys_process_param_t.sdk_version` from the ELF's PROC_PARAM segment (type `0x60000001`, magic `0x13BCC5F6`) into `g_ps3_sdk_version`, which the syscall always answered but nothing ever set (every title got the default `0x00350001`). A missing or malformed segment keeps the default and says so. `ps3_process_sdk_version()` is the one function the syscall and HLE libraries call (`PS3_SDK_VERSION` still overrides). Test: `ctest -R proc_param_sdk_version` (`tools/test_proc_param.py`, manufactured ELFs through the real loader).
- **cellRescGetBufferSize** follows the firmware (`0x02004B48`, helper `0x020022A0`): the colour size is the largest, over the supported modes, of the mode's Dsts buffer (`((heightAlign + h - 1) & -heightAlign) * pitch`) times its colour-buffer count, or with resourcePolicy bit 0 the buffer `cellRescSetDisplayMode` took from the record times the count; the vertex size is 384; the fragment size is 768 above SDK `0x27FFFF`, else by the config's size, interlace and PAL mode (or the current program's microcode size). It returned fixed 1080p sizes before. `cellRescSetDsts` now decodes the guest's big-endian record (it stored it raw) and rejects a mode that is not 1/2/4/8 (`0x80210304`). Test: `ctest -R resc_buffer_size`.
- **`video.resolutions`** (`docs/CONFIG.md`): a typed option naming the cellVideoOut resolution ids the display reports as available (`1` 1080, `2` 720, `4` 480, `5` 576, `10`..`13` the 1080-line anamorphic modes). `cellVideoOutGetResolutionAvailability` answers from it, `cellVideoOutGetDeviceInfo` lists exactly those modes, and `cellVideoOutGetState` / `GetConfiguration` report 720 before the title configures a resolution when it is listed, else the first of the list in 480, 576, 720, 1080 order (pitch follows). An unknown id is a `config-invalid` stop; the value is stored sorted and unique. The default is empty: no restriction, today's behaviour (every id available, the device lists 480, 576, 720, 1080). A policy for running a title at the resolution an oracle trace used, not a hardware fact. No schema bump (an addition). Tests: `ctest -R video_resolutions`, `tests/gates/test_config.py`.
- **cellResc** (`libs/video/cellResc.{c,h}`): the error numbers follow the firmware's return sites: `0x80210303` BAD_ALIGNMENT, `0x304` BAD_ARGUMENT, `0x305` LESS_MEMORY, `0x306` GCM_FLIP_QUE_FULL, `0x307` BAD_COMBINATION (every bad-argument path returned `0x303` before). `cellRescVideoOutResolutionId2RescBufferMode` (NID `0xD1CA0503`) is implemented: ids 1/2/4/5 store the buffer mode 8/4/1/2 (big-endian) and return 0, every other id or a NULL output is `0x80210304` with the output untouched, and there is no initialisation check. `cellRescGetNumColorBuffers` returns the count (`dstMode`, `palTemporalMode`, `reserved`) as the firmware does, instead of writing an out-parameter and failing every call whose third argument is the SDK's zero. Tests: `tests/resc` (`ctest -R resc`).
- **cellRescGcmSurface2RescSrc** (NID `0x01220224`, firmware `0x02003EF0`): converts the guest `CellGcmSurface` of colour target 0 to a `CellRescSrc` (texture format byte `0x85`/`0xA5` for A8R8G8B8, `0x9A`/`0xBA` for F_W16Z16Y16X16, `0xFF` otherwise; pitch, offset, width, height), scaling width/height for the anti-aliasing mode only when the init config's `size` is 24 or 28. NULL arguments are `0x80210304`; no initialisation check. `cellRescInit` now decodes the guest's big-endian config into host order (it copied the raw bytes). Tests: `ctest -R resc_surface2src`.
- **cellRescSetRegisterCount / cellRescGetRegisterCount** (NIDs `0x7AF8A37F`, `0x1DD3C4CD`): the register count of the current convert fragment program. The library keeps four programs (defaults 2, 3, 4, 4); `cellRescInit` selects the first and `cellRescSetDisplayMode` picks one by display mode, PAL temporal mode and interlace mode, as the firmware does (`0x02004900..0x02004A28`). `cellRescSetDisplayMode` now validates as the firmware does: a mode outside `supportModes` or not 1/2/4/8 is `0x80210304`, a 720x576 PAL mode the flip mode forbids is `0x80210307`. `cellResc_convert_register_count()` is the host accessor for the convert-and-flip emission. Tests: `ctest -R resc_register_count`.
- **`hle-uninitialized`** (`docs/STOPS.md`): a new named stop for an HLE call made before its module's init where the firmware's behaviour is undefined, with `ps3_stop_hle_uninitialized(func, module, reason)`. First user: `cellRescSetRegisterCount` / `cellRescGetRegisterCount` before `cellRescInit` or after `cellRescExit` (the firmware dereferences a NULL program pointer).
- **cellGcmGetTiledPitchSize** uses libgcm_sys's own pitch table (`0x200 .. 0xE000`, 31 values): a size above `0xE000` has no tiled pitch and returns 0, where the generic table that ended in `0x10000` rounded `0xE001..0x10000` up to `0x10000`. Test: `ctest -R gcm_pitch` (every size from 0 to `0x12000`).
- **Gaps are stops** (`docs/STOPS.md`): an unresolved NID, an unimplemented lv2 syscall, a call into an unlifted function, an unresolved indirect call (including a call through a NULL function pointer, which used to return with r3 untouched), a branch out of the lifted code and an executed instruction the lifter could not translate each end the run with a named stop instead of a fabricated `CELL_OK`/no-op. Per-entry `[[hle.stub]]` / `[[lv2.stub]]` in the title config (with a written reason) is the only way on. Removes `PS3_HLE_UNRESOLVED`, `PPU_STUCKMAX`, `PPU_NOOP_BAD_VCALL` and `PS3_NULL_CALL`, and the environment shortcuts in front of the unresolved-NID stop: `YDKJ_TUNERFIX` and `YDKJ_NETOFFLINE` become `[[hle.stub]]` entries in that title's config, and `YDKJ_SAVEDATA`'s inline `cellSaveDataAutoLoad2` is gone until cellSaveData implements it.
- **Lifter**: unknown instructions lift to `ppu_trap_unknown_instruction(...)` instead of a `/* TODO */` comment, and `ppu_lift_report.json` (`docs/LIFT_REPORT.md`) lists them with capped tails and unresolved targets. Lifted sources need the runtime's new trap function (it is in `ppu_loader.cpp`).
- **Lifter reachability**: only instructions the lifted C can reach from the function entry are lowered; literal pools, padding and anything after a trap are listed as `dead` in the lift report instead of becoming trap calls, and no longer seed call targets. A function entry found by the prologue scan no longer walks back over padding (`nop`, `.word`) onto data. On the Odin Sphere Leifthrasir EBOOT this takes the traps from 8,186 to 3, all of them unreferenced data.
- **Lifter code window**: function discovery, gap and continuation targets and jump-table cases are confined to `[code_lo, code_hi)`: the lowest `SHF_EXECINSTR` section (or `--base`) up to `--code-end`, which now defaults to the end of the highest `SHF_EXECINSTR` section (executable segments for section-stripped ELFs, the image end for raw input), as `ppu_loader.py` computes it. An ELF with no executable section or segment fails instead of guessing. A live branch to an address the lifter could not decode lowers to `ppu_unlifted_stub` at its label instead of falling into the next function; the lift report lists these as `undecoded_targets`.
- **cellGamePatchCheck** is implemented: `CELL_GAME_ERROR_NOTPATCH` (this runtime never boots a title as a patch), `PARAM` for bad arguments. It used to be an unresolved NID answering `CELL_OK`.
- **Named stops** (`ps3emu/stop.h`, `docs/STOPS.md`): one `PS3STOP v=1 kind=...` line plus a banner, exit status 70.
- **Typed, versioned config** (`ps3emu/config.h`, `docs/CONFIG.md`): default < TOML file < `PS3RECOMP_<SECTION>_<KEY>`, effective-config dump, unknown keys and unknown `PS3RECOMP_*` variables stop. Vendors toml++ 3.4.0.
- **`ppu.vscr_nj`** (default true, RPCS3 parity; a policy, not a hardware fact) is applied to every guest PPU context. `ppu_context_init` applies it; `ppu_context_clear` is the raw zeroing.
- **Identity gate**: the lifter records the SHA-256 of its input as `ppu_lift_image_sha256()`; `ppu_load_elf` refuses any other file. Titles must re-lift, call `ps3_config_load()` first in `main()`, and link the runtime library's new modules.
- **SPU execution observer** (`runtime/spu/spu_observe.h`): a per-context hook
  set for tools that drive an SPU context themselves. Reports every interpreted
  instruction (with a yield that resumes exactly at `ctx->pc`), every MFC
  command at enqueue including atomics, each atomic command's `MFC_RdAtomicStat`
  result, and every `rdch`/`wrch` value. Off unless a tool installs it; no
  environment knobs. Covered by `runtime/spu/tests/test_spu_observe.c` and
  documented in `docs/RUNTIME.md`.
- **JQ2 ring codec** (`libs/spurs/jq2/ring.hpp`): w1 bit 0 is the writer's phase for JOB, COPY and SYNC alike. `encode(entry, writer_phase)` takes it and `decode()` returns it as `Decoded::writer_bit`; the API `flags & 1` is `api_flag()`, recovered from the port EA in w0. Removes `JobEntry::flag`, `CopyEntry::flag`, `SyncEntry::word1` and `compose_writer_bit`.
- **JQ2 reference scheduler** (`libs/spurs/jq2/sched.hpp`, `docs/JQ2.md`): the per-queue tag barrier, SYNC admission, out-of-order marks, read-index advance, one JOB per pass with slot rotation and the strict phase compare, checked against the firmware golden cases and an independent property oracle.
- **Typed `u32` config options**, and three new ones: `ppu.opd_swap_fixup` (default off) gates the OPD-swap redirect, and `spu.dma_repeat_limit` / `spu.dma_repeat_stop_secs` replace the `SPU_DMA_REPEAT_LIMIT` / `SPU_DMA_REPEAT_SECS` environment variables. The SPU DMA engine reads them directly, so a static link cannot leave them unapplied.
- **More gaps are stops**: `dispatch-depth-exceeded` (runaway PPU dispatch recursion), `spu-dma-repeat` (an SPU re-issuing the same DMA for `spu.dma_repeat_stop_secs`; off by default) and `unresolved-guest-callback` (`ppu_guest_call` with OPD 0 or an OPD naming no lifted function used to return 0; a callback that cannot be given guest stack space stops with `origin=stack`).
- **Guest callback stacks**: host-initiated guest callbacks no longer share one fixed guest stack. They run below the calling guest thread's r1; host threads with no guest context each get a 1 MB slot in a window reserved at the top of the stack region, which guest thread stacks are no longer allocated from.
- **`_sys_spu_printf_*`** (all six sysPrxForUser NIDs) are implemented after RPCS3: callbacks are registered, attach/detach call into the guest and return its result sign-extended in r3, `CELL_ESTAT` without a callback. `tools/nid_database.py` had the names without their leading underscore.
- **Typed HLE calls** (`docs/HLE_ABI.md`): `ps3_hle_register` takes `void (*)(ppu_context *)` and the generated table calls each handler through `ps3_hle_adapt<&handler>`, which derives the declared prototype at compile time and applies the PPC64 ELFv1 rules (positional GPR/FPR/save-area arguments, pointer parameters as 32-bit guest EAs, returns sign- or zero-extended by type, doubles in f1, `void` leaving r3 alone). A negative `s32` used to reach the guest zero-extended. Unmappable signatures fail the build; pointer returns are classified (guest EAs zero-extend, six host-pointer handlers stop with the new `hle-unsupported-return` kind); the three printf exports use a variadic adapter; only names classified as firmware exports are registered (46 host helpers are not). The table carries its own include directories. `cellSheapAllocate` returns guest EAs.
- **lwmutex/lwcond**: one backend (`runtime/syscalls/sys_lwsync.cpp`) instead of two. Per-object FIFO wait queues, ownership by guest thread id, `signal`/`signal_all`/`signal_to` (they were no-ops), `ETIMEDOUT` with the mutex re-owned at its count, `EBUSY`/`EDEADLK`/`EPERM`/`ESRCH`/`EINVAL` as the firmware returns them; `lwmutex_lock` on a never-created lwmutex is `ESRCH`. Removes `PS3_LWMUTEX_TRACE`, `LWM_COUNT`, `FLOW_LOCKOK` and `FLOW_LWMTRACE`.
- **Reservations** (`runtime/memory/reservation.{h,c}`): one model for PPU `lwarx`/`ldarx`/`stwcx.`/`stdcx.` and SPU `GETLLAR`/`PUTLLC`/`PUTLLUC`. Every 128-byte line has an epoch; a conditional store succeeds only if the epoch and the reserved bytes are unchanged, so either side's commit kills the other's reservation even when it stores the same value. PPU-to-PPU reservations are per line. Plain stores into lines no SPU has reserved are still caught by value only (documented, AM-10). Removes `PPU_RESV_STORE`, `PPU_RESV_OFF` and `RESV_DIAG`. Lifted code calls `ppu_lwarx32/64`: titles must re-lift.
- **SPU DMA ordering**: an acquire fence before a GET reads guest memory and a release fence after a PUT writes it, checked by PPU/SPU message-passing litmus tests in both directions.
- **Host-callback thread ids**: a host-initiated callback without a guest context gets a synthetic thread id unique to its host thread (`0x7F000000..0x7FFEFFFF`) instead of one shared `0x7FFF0000`.
- **SPURS job start** (`docs/RUNTIME.md`): only `JOBCRT Ver13` and `Ver14` load (anything else stops as `spu-job-crt-version`); `eaBinary` is read as u64 so the bss its high word encodes is zeroed and the stack sits above it; every region the job owns must end below the runtime area or the job stops as `spu-job-memory`. Ver14 jobs get the kernel view and a jm interface whose context-swap entry waits for the tag groups (word 0 stops as `spu-job-jm-entry`).
- **ps3diff** (`docs/PS3DIFF.md`): the `ps3diff.event/2` schema succeeds `rpcs3-gdb-trace.event/1` (adds execution context, per-context order, image ids, inputs/outputs, state hash, SPU job/DMA/stop kinds). `convert` maps event/1 to v2 and back byte-identically, `validate` checks fields and ordering, `diff` reports the first divergence per context under a mask; zero compared records is exit 3.
- **JQ2 port line** (`libs/spurs/jq2/port.hpp`, `docs/JQ2.md`): the Port2 completion accounting the PPU push/Sync and the job manager share (A/B/claim, the claim-0 rollback, the wake bit), as a model over guest memory. Every update is one `resv_host_rmw` section (new, `runtime/memory/reservation.h`), so a concurrent `PUTLLC` or `stwcx.` on the line fails and retries. Checked against the firmware traces in the title repository.
- **JQ2 descriptor pool** (`libs/spurs/jq2/pool.hpp`): the `CopyPushJobBody` pool in the queue's guest memory; snapshots are bounded by guest memory (`0x80410A02`) and written with atomic stores. A release of an EA that is not a live block stops as `jq2-pool-release`.
- **cellSpursJq is the firmware's surface** (`libs/spurs/cellSpursJq.c`, `libs/spurs/jq2/jq2.h`): the 17 exports a title imports (`_cellSpursCreateJobQueue[WithJobDescriptorPool]`, Shutdown, Join, Port2 Create/Destroy/PushJobBody/CopyPushJobBody/PushSync/Sync, the attribute functions) with firmware argument decoding and error codes, backed by an HLE executor that runs jobs on host workers over the guest command ring. Removes the 14 invented upstream functions. A job is credited only after it returns to the job manager; other endings, ring entries the executor does not model, unsignallable waiters and broken invariants stop as `jq2-job-end`, `jq2-ring-entry`, `jq2-wake` and `jq2-state`. The objects are part of `libps3recomp_runtime`, so a title links one archive.
- **JQ2 job ABI** (`docs/JQ2.md`): jobs from a JobQueue2 are staged as the firmware's job manager stages them (image at LS 0x7C00, the 128-byte-aligned cache/io/out/scratch/stack layout, JobContext2 at 0x68D0, the descriptor copy below 0x3F400, the jm interface at 0x7A10, the kernel view and the entry registers), not with the JobChain layout. ELF Setting's guest effects match the firmware's byte for byte (`ps3diff`, one documented PM-private line masked). Calling the jm scheduler entry stops as `jq2-jm-call`; descriptor bits the layout does not model stop as `spu-job-dma-list`.
- **SPURS job loader**: the whole descriptor is validated before any local-store write: the binary extent, every cache entry and every input DMA-list element against guest memory, sizes and alignment, and the stall-and-notify bit (a job start has nobody to acknowledge it). Violations stop as `spu-job-dma-list` with a `reason`. Transfers under 16 bytes land at `EAL & 15` in their quadword, as the MFC places them. `spu_run_spurs_job_snapshot` runs a job from descriptor bytes captured at admission.
- **SPU diagnostic state for concurrent workers**: counters, log caps and lazy environment caches are per thread, so concurrent workers neither share nor race on them; the `SPU_SMC_WATCH` gate is read and set atomically. State that is process-wide by contract stays process-wide and atomic:
  - the trace destination: `spu_trace_init` redirects every thread, and one elected thread opens `SPU_TRACE_FILE`;
  - the `SPU_TRACE_MAX` budget (any value of 0 or below is unbounded);
  - the `SPU_JOBDRAIN` threshold;
  - the once-only dumps.
- **JQ2 Port2Sync** accepts only modes 0 and 4, as the firmware does (PPU `0x020045E8`); every other mode is `0x80410A02` with the port untouched. A waiter the firmware's job manager could not wake (id 64 or above), a failed event-flag wait, and a push that races Shutdown each stop as `jq2-wake` or `jq2-state` with a `reason`.
- **`hle-guest-range`**: an HLE argument whose guest span leaves guest memory stops by name instead of becoming a host out-of-bounds access. The firmware checks only null and alignment, so the stop comes after those checks, not in place of an error code. Every guest pointer the JQ2 surface takes is covered: queue, command ring, port, attributes, pool and Join's exit code.

### v0.15.0 — *"Seventy-Six"* (October 2026)

*76 pull requests from six contributors, merged in #325. They come from five
ports in progress: Killzone 2 and 3, inFAMOUS and inFAMOUS 2, and Twisted
Metal, plus tekulvw's correctness sweep of the runtime and Metal. Every
change ships with a test or a measurement.*

**SPU and SPURS**
- **Hardware-exact SPU float** (*[@NoahBPeterson](https://github.com/NoahBPeterson)*, #287).
  Single precision now follows the ISA: extended range, truncation, denormal
  flush and saturation. `frest` uses the hardware table. Results are checked
  against an exact-arithmetic referee, and NEON paths run 1.6-4x faster with
  bit-identical output. This replaces the previous GPL-derived float code.
- **SPURS policy scheduling** (*[@tekulvw](https://github.com/tekulvw)*, #291).
  Each instance gets a coordinator and `nSpus` persistent workers, with
  admission capped by each workload's contention, so a waiting consumer no
  longer starves its producer. Every live SPU context also keeps private DMA
  and reservation state (#290).
- Interpreter fallback resumes lifted code (#266), and machine status reports
  the interrupt-enable bit (#289). MFC `SDCRZ` is implemented (#295), FNMS
  keeps its addend for zero times an extended operand (#318), and parked-list
  diagnostics are bounded (#298). SPURS task exit-code containers (#283),
  taskset storage size (#284) and TaskInfo requeue masking (#306)
  (*@tekulvw*).
- `build_spu_workloads.py --input` discovers, validates and registers SPU ELFs
  from supplied files, and the project template runs it (*@tekulvw*, #288).
- `SPU_RdDec` no longer wraps 231 s after its base stamp
  (*[@erikomis](https://github.com/erikomis)*, #313).
- From *[@undecuple](https://github.com/undecuple)*: the SMC micro-interpreter
  addresses `lqa`/`stqa` at I16 << 2 (#268). The MFC engine table grows past
  eight contexts (#272, later replaced by #290's unbounded registry). The
  policy module's u64 argument is passed in the preferred doubleword (#309),
  and `_cellSpursAttributeInitialize` gets its SDK argument order (#310).

**PPU lifter and loader**
- **Exact VMX** (*@NoahBPeterson*, #286). About 45 VMX ops that lifted as
  no-ops are now implemented, along with VSCR saturation and CR6 compares.
  BO/BI branches are decoded (`bdnzt` and `bdzf` were always taken), CTR is
  64-bit, and get-PC idioms are recognised.
- From *@undecuple*:
  - big-endian lane order for ten VMX integer ops (#269);
  - `lwax` jump tables (#285);
  - `sc` at the end of a function falls through (#297);
  - a carry-form definition is no longer mistaken for a callee-save restore
    (#320);
  - `--chunks N` cuts lifted source by guest address and rewrites only the
    files that changed (#254).
- Loader, from *@undecuple*: TLS blocks sized by the TLS image (#253), a
  per-coroutine indirect-call depth guard (#270), and thread-locals that stay
  thread-local under MinGW (#271).
- Jump tables behind a tail-call epilogue are found (*@erikomis*, #311).
- Boot, from *@tekulvw*: PPU entry argv/envp live in stack-owned storage
  (#263), and HLE registrations regenerate when library sources change (#262).

**RSX, draw engine, Metal and Vulkan**
- **Depth:** from *@undecuple*, fragment depth export (#279), depth buffers
  sampled as textures return real depth (#280), and Z16 clear values are
  decoded (#282). From *@tekulvw*, 2D depth-compare textures (#302), depth24
  endpoints preserved (#299), depth export in the shader cache keys (#301),
  and Metal depth attachments read back to guest memory (#300). From
  *[@Kurtisone](https://github.com/Kurtisone)*, the depth bounds test (#319).
- **Colour:** from *@tekulvw*, sRGB framebuffer output (#304), per-channel
  texture input gamma (#305), fragment colour inputs clamped before source
  modifiers (#303), and register-fed DATA3F/DATA4F immediate vertices (#292).
- **Fragment decompiler:** predicated writes are a select, not a lerp
  (*@undecuple*, #278). The NV40 pack/unpack opcodes are implemented
  (*@erikomis*, #314).
- Antialiased surfaces render at sample resolution (#281), and the sampler
  cache key covers every field it decodes (#321) (*@undecuple*).
- **Metal:**
  - from *@tekulvw*: owned colour targets snapshotted in GPU order (#307),
    clears replayed over complete surfaces (#293), real window-close
    reporting (#308), and opt-in `PS3RECOMP_METAL_FPS` frame timing (#294);
  - from *@erikomis*: a command buffer that fails is reported (#316).

**cellGcm**
- From *@undecuple*: IO mapping table 16 -> 512 with no leak on refusal
  (#255), `GET_REPORT` honours its DMA context (#256), FIFO user commands
  reach `cellGcmSetUserHandler` (#273), and the in-FIFO flip wraps only the
  runtime's own ring (#275).
- Strict-order IO mapping (*@tekulvw*, #248). A parked FIFO gets time to be
  patched before the walker follows `put` (*@erikomis*, #312).

**System libraries**
- **cellFs:** directory entries (*@tekulvw*, #247), configured disc roots
  (*@tekulvw*, #249), `GetFreeSize`/`Chmod`/`Utime`/`SdataOpenByFd` (#257),
  and 64-bit sizes on Windows (#258).
- **cellSaveData:** PARAM.SFO is written and read as a real binary PSF, and
  old text saves still load (#322). UserAutoSave keeps its ninth argument
  (#276).
- **Network:**
  - from *@undecuple*: cellHttp's URI path (#260) and response getters (#261);
    sceNpBasic usable after `sceNpInit` (#259); sceNpCommerce2 `CreateCtx`
    (#323); the EDAT klicensee from `sceNpDrmIsAvailable` (#274);
  - from *@tekulvw*: no SIGPIPE on host sends (#250) and `inet_pton` (#265).
- **Other:**
  - from *@undecuple*: `sys_lwcond_t` layout (#277) and
    `cellFontInitializeWithRevision` (#324);
  - from *@erikomis*: `cellGemGetMemorySize` and the event-queue notify forms
    (#315);
  - from *@tekulvw*: the timezone syscall (#264), extended sysmodule tracking
    (#267), message-dialog abort (#251) and Sail boot parameters (#252).

### v0.14.0 — *"Outside In"* (October 2026)

*This release is mostly community work: 28 pull requests from five
contributors. Most of it came out of the Twisted Metal port on Linux, which put
the shared draw engine and the Vulkan backend under a real deferred renderer.*

**Draw engine and Vulkan**, from *[@Kurtisone](https://github.com/Kurtisone)*:
- **Surfaces read in another format** now go through guest memory: a depth
  buffer read as colour, a Z16 shadow map, a float packed into RGBA8. Each
  colour and depth write carries a serial, so the engine knows which one is
  newer (#236).
- Surfaces stay coherent around NV3089 transfers (#224), and cellGcmSys hands
  the engine its display buffers and flips (#223).
- Texel-addressed texture coordinates are scaled (#221), B8 presents its one
  channel as R, G and B (#220), and a Z16 zeta clears to its own far plane
  (#235).
- Vulkan gains RGBA16F colour targets (#222), cube maps (#229), images sized by
  the device's limit (#227) and readbacks from cached memory (#245).
- New diagnostics: `RSX_ENGINE_PASS_DUMP` dumps a frame pass by pass, with
  each pass's targets, depth and programs (#237).
  `PS3RECOMP_VK_DUMP_EVERY` dumps every nth presented frame (#219).
- **Vulkan stencil** (*[@gabryboy12-ai](https://github.com/gabryboy12-ai)*,
  #231): the depth format carries stencil, so the engine's stencil state now
  takes effect.
- **D3D12:** fragment programs whose HLSL ran past 32 KiB no longer fail to
  compile (*[@robbe1912](https://github.com/robbe1912)*, #230).

**Runtime and SPU**, from *[@Kurtisone](https://github.com/Kurtisone)* unless noted:
- **SPU-to-SPU task signals.** A SPURS task signalled from another SPU wakes
  immediately. Before, it woke only when its 1 s timed wait ran out, which made
  one frame of Twisted Metal's post-FX take 4.1 s (#242).
- **SPURS:** the guest's taskset no longer has native fields written into it
  (#240). `cellSpursBarrierInitialize` and `cellSpursGetTasksetInfo` are
  implemented (#241).
- **cellGcmSys:** a main-memory offset now resolves through the IO table first
  (#238). `ps3_ms_now` gets a clock off Windows (#244).
- **POSIX:** lwmutex and lwcond lock for real (#225), and stack scans skip
  unreadable guest pages (#217).
- **SPU disassembler:** decodes `orbi`, `xorhi` and `xorbi` (#239).
- **SPU interpreter:** `ILH` replicates its halfword (*[@tekulvw](https://github.com/tekulvw)*, #234).
- **Lifter:** `--chunk-lines` sets the size of the source chunks (#216).
- **Diagnostics:** `PS3_NULL_READ_STACK` names the guest code behind a NULL
  read (#226). The main thread's context is published as `g_ppu_main_ctx`
  (#228).

**Media**: cellVdec decodes H.264 and MPEG-2 through FFmpeg loaded at run time
(*[@undecuple](https://github.com/undecuple)*, #215). It is opt-in
(`-DPS3RECOMP_FFMPEG_RUNTIME=ON`) and FFmpeg is never linked. Without it,
cellVdec behaves as before.

**Housekeeping**:
- `fw_spu/` is no longer tracked, and `*.prx` is ignored. The tree had been
  carrying decrypted firmware PRXs and C lifted from them, against our own
  firmware policy. Thanks to *[@deucebucket](https://github.com/deucebucket)*
  for reporting it (#210, #212).
- README audit: the Getting Started commands now produce a buildable project,
  the platform table matches CI, and the provenance wording is corrected (RPCS3
  is a reference, not a code source). Changelogs before v0.14.0 moved to
  CHANGELOG.md.

### v0.13.0 — *"Player Two"* (October 2026)

*Two titles in game, and one of them online. The Simpsons Arcade Game plays a*
*two-player match over the network, and Tornado Outbreak reaches its first level*
*rendering correctly with no per-title overrides. Thirty-odd pull requests from*
*six people since v0.12.1.*

**Online**
- **NP online over [psnr](https://github.com/sp00nznet/psnr)** (#196, #200) —
  real host sockets behind `PS3_NET_ONLINE`; sceNpMatching2 rooms, signaling and
  callbacks, sceNpScore leaderboards, sceNpLookup, cellSysutilAvc2; NAT probes,
  hole punching and a relay for peers that cannot reach each other.
  `--psnr host --username NAME` on the runtime's command line. Offline is still
  the default and unchanged. See [docs/ONLINE.md](docs/ONLINE.md).

**Rendering**
- **A Vulkan backend for Linux** — *[@gabryboy12-ai](https://github.com/gabryboy12-ai)*
  (#182, #192). Opt-in; runs the guest's own shaders on the shared register-file
  draw engine and passes every `ps3recomp_host` scene under the validation layer.
- **`SET_TRANSFORM_CONSTANT` is a 32-dword window** — *[@andrebrumdev](https://github.com/andrebrumdev)*
  (#208); three registers were corrupting God of War II's skinning palette.
- Fragment-program flow control (IFE/LOOP/REP/BRK) and LIF (#187), vertex-program
  flow control, GPU resolve copies and raw-SPU proxy DMA (#189), job-chain Join
  and in-FIFO flips (#185).

**Lifter**
- *[@andrebrumdev](https://github.com/andrebrumdev)*: jump-table base arbitrated
  by validated targets (#179); callee-save snapshots only for real epilogue
  restores, and `vcmpequb.`/`vcmpequh.` set CR6 (#207).
- Conditional fallthrough at a chunk end (#202), multi-TOC jump-table ranking
  (#195), table-less computed jumps with a constant base (#193).

**SPU** — *[@sagemono](https://github.com/sagemono)*, from LittleBigPlanet:
non-r0 link returns (#174), data-only job-module entries (#175), drain resume
and `iret` unwind (#176), the taskset syscall on a planted context (#177),
`_sys_spu_image_import` segment source (#178).

**Audio and media** — cellAtrac with ATRAC3plus (#186), cellVdec/cellAdec query
and open fixes (#197, #198, #201), cellSail state changes (#190), job-chain
completion carrying the job index (#204).

**Build** — Linux and macOS building again (*[@gabryboy12-ai](https://github.com/gabryboy12-ai)*
#191, #205), `-msse4.1` probed (*[@andrebrumdev](https://github.com/andrebrumdev)* #206),
`spu_lifter --header-name` honoured (*[@tomspilman](https://github.com/tomspilman)* #181),
the onboarding docs rewritten to the pipeline the tools implement (#180), and
[AnyPS5](https://github.com/boykopovar/AnyPS5) added to the sibling projects
(*[@bojogon7](https://github.com/bojogon7)* #209).

### v0.12.1 — *"Downloads, Again"* (September 2026)

*A packaging release. v0.10.0 and v0.11.0 were both tagged and neither was ever*
*published — their release runs died on macOS with an undeclared*
*`RtlCaptureStackBackTrace`, so the newest download anyone could get was v0.9.1*
*from August. That is fixed, and Linux now builds alongside Windows and macOS.*

- **Bundles for all three platforms.** Linux joins the release matrix. It ships
  as the toolkit and runtime library rather than a game runner, and each bundle's
  `VERSION.txt` says what its platform can actually do — Windows renders and runs
  titles through D3D12, macOS renders through Metal but cannot yet load one
  because the PPU boot scaffold does not run on POSIX, and Linux has no renderer
  at all. Someone downloading the Linux zip should learn that from the file, not
  after building something with it.
- **A dry run works from any branch.** `workflow_dispatch` set the bundle version
  from the *branch* name, so a branch with a slash in it nested the staging
  directory and the upload glob found nothing — every platform built, tested and
  staged, and the run failed anyway.
- **`cellGame` content root strips `/dev_hdd0`** — *[@sagemono](https://github.com/sagemono)*
  (#163, landed as #173). `ppu_fs` strips the mount prefix, so guest
  `/dev_hdd0/game/<id>` resolves to `<root>/game/<id>`; spelling the device out
  recreated the very split v0.12.0 set out to close, one directory over.

### v0.12.0 — *"One Tree"* (September 2026)

*Nineteen open pull requests land on one branch. They came from the two people*
*who have games actually running —* [@canersaka](https://github.com/canersaka)
*with Yakuza: Dead Souls on macOS,* [@sagemono](https://github.com/sagemono)
*with LittleBigPlanet — and the capability they represent existed for months*
*without existing on `master`. Every one of caner's carried the same sentence:*
*"Windows gameplay has not been rerun for this batch." Nobody could answer that,*
*because nothing ran the ports together. That is what this release is.*

**The nineteen**

- [@canersaka](https://github.com/canersaka) (#152–#161): lifted SPU threads in
  per-thread architectural contexts with lv2 copy semantics for thread arguments;
  lv2 condition signals and static guest mutexes; cellAudio ring indices and
  block tags; cellSysutil dialog completions from the guest callback poll;
  sceNpTrophy first-use slots; cellGame content-volume space; cellSaveData
  callback ABI; **RSX full FIFO driver methods, serialized callbacks and MRT
  exports**; **macOS native host, Metal rendering and game runner**; sceNp local
  score while offline.
- [@sagemono](https://github.com/sagemono) (#162–#170): `cellGameContentErrorDialog`;
  **cellGame content root** — the host directory `/dev_hdd0/game` maps to must be
  the one `ppu_fs` resolves it to, or a title writes its save data to one
  directory, reads it from another, and reports the data *corrupt* rather than
  missing; `bi $rN` classified by basic block rather than by the lifted function,
  because the lifter splits straight-line runs into functions that chain by
  fallthrough and a split is not a control-flow edge; `hbra`/`hbrr` decoded as
  the 7-bit opcodes they are; `--extra-funcs` passthrough for images entered by
  an interrupt vector, which `--auto-functions` cannot see; EDAT license types 1
  and 2 with the missing HMAC-SHA1 hash modes; list-DMA issue tracing.

**What unifying them found**

- **`cellGameSetParamString` was missing.** Sage's cellGame work makes Twisted
  Metal reach `cellGameCreateGameData` for the first time, and that path calls a
  function nothing implemented. Exactly the kind of gap a port owner cannot see
  from inside their own title — which is the argument for unifying, stated as a
  bug.
- **One fix was found twice.** Sage's `hbra`/`hbrr` decode is byte-identical to
  one already in the tree, reached independently from a different witness.

**The gate had to be rebuilt before it could say anything**

Four things it could not do, each discovered by it letting something through:

- **Occurrence counts never reached a golden.** `ps3_ms_dump()` ran only at
  `atexit()`, and every gated port is *killed* on its timeout — so `DROPPED`, the
  rule whose own docstring reads *"Rendering stopping looks like this"*, had
  nothing on either side to compare. Counts now checkpoint on a timer.
- **It never measured pixels.** `--shots` runs each port in the configuration its
  README documents and compares against the port's own reference screenshot.
  Frames are judged by distinct colour count, never brightness: a cleared buffer
  scores 100% non-black while showing nothing.
- **`PS3_VFS_ROOT` was doubled for two ports.** The guest path is appended whole,
  so a root of `vfs/PS3_GAME/USRDIR` produced
  `vfs/PS3_GAME/USRDIR/PS3_GAME/USRDIR/…`. VF5 spun on **686,996 failed opens**
  and looked like a dead showcase title; Simpsons opened zero files and drew
  9,391 groups of nothing, which looked like a broken renderer. Both render.
  Their goldens were invalid rather than stale and are re-recorded.
- **A run straight after a build is not a measurement.** Cold page cache shifts
  guest timing enough to flip race-sensitive titles; that run is now a discarded
  warm-up, and a red is confirmed by a second run. Of five reds investigated in
  one session, four were measurement artefacts and one was real — and the real
  one came from an `UNRESOLVED` NID, a fact about the binary rather than about a
  run.

`rubberducky` leaves the gate: a Sony SDK sample recompiled from a debug fSELF
with its symbol map exposed is excellent ground truth for the lifter and says
nothing about whether a change is safe for retail titles. New:
[docs/REGRESSION_GATE.md](docs/REGRESSION_GATE.md).

### v0.11.0 — *"Correctness Pass"* (September 2026)

*Fourteen open pull requests land at once. Most of them are correctness fixes*
*found by driving real titles into real walls:* Tokyo Jungle *reaches its main*
*menu, and the bugs it exposed — a mutex that never ran its initialiser, a FIFO*
*resync that ate fences, two guest pointers dereferenced as host pointers — were*
*never title-specific.*

**Execution correctness**

- **`sys_ppu_thread_once` was unimplemented** and returned success *without
  running the initialiser* (#136). Anything waiting on state that a once-routine
  was supposed to publish waited forever. This was the keystone bug behind
  Tokyo Jungle's boot.
- **SPU `xswd` read the wrong words and stored the halves backwards** (#127) —
  small positive values became zero. **`GETLLAR` must snapshot the reservation
  from local store**, not re-read memory (#126).
- **VMX correctness batch folded back** from the port branches (#125):
  `vmsumshs`, `vsumsws`, `vmulesh` and five byte-order bugs.

**Guest pointers are not host pointers**

Two crashes, same root cause, both reachable from ordinary gameplay:

- **`cellSaveData` called the title's `funcStat` and `funcFile` as host function
  pointers.** They are guest OPDs, so this jumped into guest memory as code —
  every autosave died on it. Both now marshal through guest memory the way
  `cellSaveDataAutoLoad2` already did.
- **`cellSail` printed its `uri` and `path` arguments as host strings**, killing
  the process the moment a title opened a movie. Both are copied out of guest
  memory now, and the descriptor's long-unused `uri[512]` field is finally
  filled.

**Graphics and SPURS**

- **A FIFO resync must not swallow fences** (#137). Jumping `get` to `put` keeps
  the ring alive, but a `SET_REFERENCE` in the skipped range means the
  `cellGcmFinish` waiting on it never wakes. Losing draws costs a frame; losing
  a fence deadlocks the title. The skipped range is now swept first, and each
  resync site is named in the log.
- **SPURS carries the SPU's answer to a job query** in the completion event
  (#135), and **cancels only the finalised instance's queues**, plus a real
  `ShutdownJobChain` (#129).
- **`cellSail`**: a player handle is a guest pointer, not an index (#130), and
  async call completion is reported to the title's event handler (#133).

**Diagnostics and tooling**

- **A call graph over the lifted output** (#131), so reachability is a query
  rather than a grep.
- **Watched stores name the guest function** that made them (#132, #134), and
  condvar probes tell a lost signal from an unsatisfied predicate (#128).

**Documentation, honestly**

- Removed three documents that were surveys of a private game library, one of
  which described obtaining DRM-stripped EBOOTs. The harness no longer defaults
  to a personal path (#138).
- **The starter template could not link.** It declared a `ps3::memory` /
  `ps3::thread` / `ps3::modules` API that does not exist, and since the porting
  guide tells you to copy that template, the documented path could only ever
  produce undefined references (#114, #89). Rewritten against the real runtime
  API, and its CMake now compiles the per-game PPU scaffold and sets `/bigobj`.
- **The renderer is documented at last** — which backend each platform gets,
  that there is no CMake switch for it, and that `RSX_LIVE_DRAW` is *not*
  required to get a picture.
- `SPU_FALLBACK.md` claimed ps3recomp cannot execute SPU code, which stopped
  being true some time ago.

**CI**

- `cellSync.c` called `getenv` with no `<stdlib.h>` — invisible on Windows,
  fatal on POSIX, and one reason every Linux job had been red.
- The SPU helper suite had a broken `xswd` case that built asymmetric vectors
  with a helper documented as only safe for symmetric ones. **Windows CI was
  never running that suite at all**; it does now.

> **Known issue:** the PPU scaffold ratchet still fails on Linux and macOS
> (`ppu_loader.cpp`, 0 → 110 on gcc). It predates this release and is tracked in
> #139. Windows CI is green.

### v0.10.0 — *"Three Platforms"* (September 2026)

*Windows, macOS and Linux all build the runtime and pass the suites. A second*
*draw engine lands beside the first — [@canersaka](https://github.com/canersaka)'s,*
*brought across from his Yakuza port — the RSX backend finally gets a real*
*abstraction instead of one D3D12 file, and* You Don't Know Jack *reaches*
*gameplay.*

**A second draw engine**

- **The live NV4097 → D3D12 draw engine** — executes the guest's command stream
  as it arrives rather than replaying a recorded frame. Brought across from the
  [Yakuza: Dead Souls EX](https://github.com/canersaka/Yakuza-Dead-Souls-EX)
  port: `rsx_live_draw.c`, `rsx_dispatch.*`, `rsx_vertex_compact.*`,
  `rsx_restart_cuts.h` and the VP/FP decompiler updates are
  *[@canersaka](https://github.com/canersaka)*'s work (#113). Opt-in behind
  `RSX_LIVE_DRAW`, so the existing backend path is unchanged when it is off.
  This sits *beside* [@sagemono](https://github.com/sagemono)'s draw engine from
  v0.7.0 — two independent renderers, not a replacement.
- What a second title needed on top: the **FIFO subchannel is a binding slot,
  not an engine selector** (a title binding NV4097 on subchannel 1 had every
  `SET_SHADER_PROGRAM` dropped, so every draw ran one constant-black fragment
  program); **HDR colour surfaces** (`F_W16Z16Y16X16` targets were created as
  `R8G8B8A8_UNORM`, clamping every value above 1.0 on write); and fragment-output
  NaN guards.

**Three platforms**

- **Linux bring-up** (#100) — CI, a headless RSX backend, and the PPU boot
  scaffold taken to zero errors on POSIX. The headless path is a host-memory
  framebuffer with a CPU triangle filler: enough to prove the guest's command
  stream produced the pixels it asked for on a runner with no display and no
  GPU. It is not a renderer, and the README says so.
- **The RSX backend gets an abstraction.** Texture pixel conversion (#101), the
  `TEXTURE_CONTROL1` crossbar decode (#102), texture layout, and one primitive
  truth table with a draw record that is not a D3D structure (#103) all lifted
  out of the D3D12 backend — so Metal and the headless backend read the same
  neutral values instead of reimplementing them.
- **CI samples a real guest texture with no GPU anywhere** (#106), and the
  frame clock starts on POSIX (#105).
- Portable plumbing throughout: one thread-local spelling reachable from the
  boot scaffold, host backtrace diagnostics on POSIX, Win32 types and
  interlocked ops on POSIX, and an in-tree Win32 `dirent.h`.

**Runtime correctness**

- **`CellSyncMutex` is a ticket lock, not a 0/1 flag** (#118) — the guest word
  is big-endian `m_freed`/`m_order` and free means `m_freed == m_order`. We
  stored it host-endian and CAS'd it from zero, so a mutex the SPU left free at
  `1/1` read as `0x01000100` and `TryLock` spun forever — 160 million times, in
  the title that found it.
- **`sys_mmapper_allocate_memory`** implemented (#112), **`cellSpurs`
  `CreateJobChain` + `KickJobChain`** actually start the chain (#98),
  **`cellDiscGameGetBootDiscInfo` takes one argument, not three** (#109),
  **`sys_event` yields before refusing a receive on an invalid queue** (#110),
  and **SPU MFC PUTs into the null page are rejected** (#107).
- **The guest bounds check is inlined** (#116), and a port can **choose the
  HLE-visible GCM window base** (#104).

**Diagnostics**

- **`PPU_WVAL`** watches PPU stores by value rather than by address (#108), the
  write watch **says when it stops printing** and lets the cap be raised (#111),
  the guard **prints the writer's live arguments** on every watched-line hit
  (#99), plus `PS3_HLE_ARGS`, `PS3_WAIT_OBJ` and a settable SPU atomic-trace cap.

**Ports**

- ***You Don't Know Jack* reaches gameplay** — boots, renders its Scaleform UI,
  navigates menus, loads an episode and puts answerable questions on screen.

### v0.9.1 — *"Downloads"* (August 2026)

*A packaging release. No runtime or lifter behaviour changes: the same tree as*
*v0.9.0, plus the CI and release plumbing that makes it downloadable.*

- **Releases now ship binaries.** A `v*` tag builds an SDK bundle per platform
  — the runtime library, its public headers, the full Python pipeline and the
  docs — and attaches it with checksums. Previously a release was source-only,
  so trying the project meant reproducing the whole toolchain setup first.
  `lib/` and `include/` come from the project's own install rules, so the
  bundle cannot drift from what a local `cmake --install` produces.
- The test suites **re-run in the release job**. A tag can point at any commit,
  including one that never went through CI; without this, "downloaded from a
  release" would mean less than "green on master".
- **Windows CI.** The macOS job guarded the POSIX paths, but nothing
  guarded the platform everything actually ships on — a change made to satisfy
  Clang could have broken the clang-cl build with nobody noticing until someone
  tried to build a port.
- Bundles are built for **Windows and macOS only**: those are the two platforms
  with CI proving they compile. Shipping an untested Linux binary would repeat
  the mistake this release series started by fixing.
- Docs now point at **[ReXGlue](https://github.com/rexglue/rexglue-sdk)** for the
  Xbox 360 side rather than a tool collection of our own.

### v0.9.0 — *"Second Platform"* (August 2026)

*The first tagged release, and the first that another platform can verify.*
*Two outside contributions from [@slushiimusic](https://github.com/slushiimusic)*
*gave the project CI and a second render backend; wiring the existing test*
*suites into that CI immediately found fifteen silent miscompiles.*

**macOS, and the project's first CI** (#94, #95, #96)
- `docs/BUILDING.md` had listed Apple Clang 14+ under "full support" while the
  tree did not compile there at all. Reported in #94, which was correct.
- **#95** makes the runtime library build on arm64 macOS and adds the first CI
  workflow this repository has ever had. It caught two of its own follow-ups
  within 17 seconds of landing.
- **#96** adds an RSX **Metal** backend (clear, flip, guest vertex fetch,
  pipeline-state cache, blend translation), a POSIX host that drives
  cellGcm → RSX → backend with **no lifted game**, and .app/dmg packaging.
- CI asserts the **presented pixel**, not the exit code: the guest clear colour
  through the FIFO, and an NV4097 triangle covering the centre pixel.
- macOS still **cannot run a recompiled game** — the PPU boot scaffold is
  Win32-only. See [platform support](#platform-support).

**Fifteen VMX miscompiles, found by running the tests somewhere new**
- The conformance suite only ever compiled its generated driver on Windows.
  Everywhere else it printed `COMPILE FAILED` and executed nothing — and a
  missing compiler exited **0**, so a skip read as a pass.
- With it actually running: `vupkhsb/vupklsb/vupkhsh/vupklsh` (12 cases) read
  and wrote the big-endian vector file through host `int16_t*`/`int32_t*`,
  byte-reversing every result element. `vcmpgtsw`/`vcmpgtsh` compared
  byte-reversed values, so a mixed-sign vector produced an all-zero mask.
  `vadduwm` propagated carries the wrong way and spilled into neighbouring
  elements. The byte-width members of each family passed by luck.
- `vadduhm`, `vsubuhm`, `vsubuwm` had the same latent bug with no test
  covering them; fixed together. 1311 checks, 0 failures.
- `mtfsb0`/`mtfsb1`/`mtfsfi` reached the unhandled catch-all. `mfspr VRSAVE`
  was a bare no-op that left the destination register **stale**.

**Runtime fixes found while driving two titles**
- **The GCM label/control/offset block lived at a hardcoded `0x03000000`** —
  inside main memory, where a title's own allocator hands out blocks. flOw's
  heap swallowed it: the game memset over the control register, the FIFO drain
  read ASCII as `put` and discarded whole frames. Moved to
  `VM_HLE_INJECT_BASE`, with every user deriving from it.
- **Malformed MFC transfers were reported and then performed anyway.** Real
  hardware raises an alignment exception and moves nothing. YDKJ's FMOD mixer
  issues eight 16 KB PUTs with a 4-byte-misaligned EA marching through its own
  loaded image, which zeroed the title's OPD table; the boot now survives.
- **The host-RVA → guest-function mapper invented frames.** "Largest host
  pointer ≤ target" with no upper bound attributes any runtime or CRT address
  to whichever lifted function sits below it. It cost real debugging time on
  both titles. Now bounded, and it prints the raw RVA rather than guessing.
- **Two-level jump tables** (`lwz r30,d1(r2); lwz r11,d2(r30)`) were rejected
  outright, including the common case where the base is loaded in the prologue
  hundreds of instructions earlier.
- The VFS now tolerates a flattened extracted tree (no `PS3_GAME` level).

*Earlier versions in this changelog predate tagging. v0.9.0 was the first
tagged release; v0.9.1 is the first with downloadable artifacts.*

### v0.8.0 — *"Ground Truth"* (August 2026)
<!-- TODO(maintainer): codename is a suggestion -- rename freely. -->

*A correctness release about the guest/host boundary, and about instrumentation
that can support its own claims. Two batches: `#92` — the guest-pointer sweep,
a SPURS teardown fix, and making the logging stop changing what titles do — and
everything folded via `integrate/fold-2026-08-14`,
[@sagemono](https://github.com/sagemono)'s guest-ABI HLE batch and SPU-lifter
faithful adoption plus [@canersaka](https://github.com/canersaka)'s
guest-callback/lv2/ABI batch.*

**The guest-pointer sweep — `libs/` is clean** (#92)
- **304 → 0 candidates.** HLE entry points receive pointer arguments as raw
  32-bit guest addresses, so dereferencing one reaches whatever host address
  happens to share that number. The sweep found it was really *three* problems:
  the address; the byte order (a scalar the guest reads back needs `vm_write*`,
  not a bare store); and **layout** — a struct holding a pointer is 4 bytes per
  field to the guest and 8 to us, so every field after it shifts, and a `u64`
  cannot be moved word-at-a-time. Only the first is a translation.
- `cellFont` is the case that shows why: `CellFontRenderSurface` and the glyph
  structs lead with a host pointer, so casting a guest EA to them misplaced
  *every* field and stored floats little-endian — a title reads a glyph advance
  of `4.6e-41` and lays every string on top of itself.
- **`cellGcmGetOffsetTable` handed the title two truncated host pointers** through
  a struct whose guest form is two 4-byte fields. The host tables were always
  correct; the bug was on the way out.
- **`tools/audit_guest_ptrs.py`** finds the bug class, **`tools/audit_entry_calls.py`**
  finds a library calling its own entry points (which double-translates — it
  caught two live ones), and **`tools/test_audit_guest_ptrs.py`** pins the audit in
  both directions, because the heuristic silently stopped matching three times
  during the sweep and a clean report meant nothing until it was checked.
- `libs/guest_struct.h` documents when the easy helpers do *not* apply.

**Runtime**
- **`sys_event`: release queue waiters when SPURS finalizes.** A title's worker
  waits on the SPURS completion queue with an infinite timeout and only re-tests
  its quit flag when a completion wakes it — but `cellSpursFinalize` has already
  stopped the job chains, so none can arrive. The shutdown that called finalize
  then blocks in `sys_ppu_thread_join` forever. Cancelling the attached queues
  returns `CELL_ECANCELED` to the waiters, which is the path titles already
  handle. Lifted again on re-attach, so teardown-then-setup works.
- **Logging no longer changes what a title does.** `ps3_log_verbose()` treated a
  non-tty stderr as "be verbose" with no way to say otherwise, so *capturing a
  log turned the firehose on*; and the SPURS job path wrote two lines with an
  explicit `fflush` per job — ~6k writes/s through one `FILE` lock, enough to
  starve a guest thread for seconds. Tokyo Jungle presents four frames in 40 s
  with the log discarded and one with it redirected to a file. `PS3_VERBOSE=0`
  now decides, and the per-job pair is verbose-only.
- **A wedged thread names itself.** `g_hle_inflight[tid]` and `g_sc_inflight[tid]`
  record the HLE call and lv2 syscall each guest thread is *inside*. The
  watchdog's `CTR` is the last `bctrl` target and goes stale the moment a call
  returns — it named `sys_lwmutex_unlock` for a thread that had long since left
  it. The write-watch also covers `ppu_memory.h`'s inline `vm_write*` now, so
  "nothing writes this address" is a claim it can actually support.
- clang-cl build repaired, two missed guest-call sites, and the dropped SPU park.

**HLE & guest ABI**
- **Guest-EA / big-endian out-param marshalling** across the HLE surface — `cellUserInfo`, trophy u64s, `cellGameGetParamInt` DISC type, open PSID, `cellRtc`, save-data user paths, `cellJpgDecDecodeData`, `cellPngDec`, and the `cellAudio` read index as a BE u64 — *[@sagemono](https://github.com/sagemono)* (#79, #81)
- **`cellFsSdataOpen` with real SDATA/EDAT (NPD) decryption**, `cellFsMkdir` + the game-data dir created where the VFS looks, and a `/dev_hdd0` overlay onto the installed-update tree (`PS3_HDD0_ROOT`) — *[@sagemono](https://github.com/sagemono)* (#79)
- **Honest state, not silently-faked `CELL_OK`** — a module name matching no file fails; net reports a real offline state with real SDK NP error codes; `sys_time_get_system_time` returns real microseconds instead of a call counter — *[@sagemono](https://github.com/sagemono)* (#79)
- **Guest callbacks receive r3 through r10** — the dispatch hook plumbed only four arguments, so any callback taking more read stale registers — *[@canersaka](https://github.com/canersaka)* (#86)
- **`cellFsReaddir` writes the real 258-byte dirent ABI** + a corrected CellOS generic error-code table (#85), and **`cellGame` firmware parameter IDs, error values and buffer bounds** matched to the SDK (#84) — *[@canersaka](https://github.com/canersaka)*
- **`cellAudio` clears each consumed ring-buffer block** — an underrun replayed the previous block instead of silence — *[@canersaka](https://github.com/canersaka)* (#83)

**lv2**
- **One authoritative semaphore count** — the count lived twice (in `sys_semaphore_info::value` and in the Win32 handle), so timeout/wake races could lose tokens or park a waiter while `value` was already positive. `value` under `value_lock` is now the only count, the handle is a wake channel, and overflow returns lv2 `CELL_EBUSY`. The `sync_stress` target is wired into the build so this stays executable outside a generated title — *[@canersaka](https://github.com/canersaka)* (#88)

**Lift & decode**
- **SPU-lifter faithful adoption** — a from-scratch re-derivation of the SPU decode/lift + runtime, consolidating the interpreter and function registry into `spu_channels.c`, plus LBP SPU/SPURS bring-up and RSX texture support (G8B8/DXT, deswizzle). Credit for the underlying decoder/lifter design belongs to [@sp00nznet](https://github.com/sp00nznet) and [@canersaka](https://github.com/canersaka) — *[@sagemono](https://github.com/sagemono)* (#82)
- **SPU `sync`/`dsync`/`syncc` emit real host memory fences**, with the `sync` C bit decoded instead of folded into plain `sync` — *[@canersaka](https://github.com/canersaka)* (#87)
- **Jump-table discovery no longer silently skips every table** — a `toc` list was concatenated as an int, and the caller swallowed the exception; plus `rA=0` base resolution and generic `ld`/offset-table discovery (#78, #81)

### v0.7.0 — *"First Draw"* (July 2026)
<!-- TODO(maintainer): version + codename are a guess -- rename freely. -->
*The RSX draw engine lands. [@sagemono](https://github.com/sagemono)'s renderer executes the title's own NV4097 vertex and fragment programs on D3D12, and **You Don't Know Jack now renders** — a real double-buffered flip loop, verified by booting it. Plus [@canersaka](https://github.com/canersaka)'s PPU/SPU correctness batch, and the SPU image-import path SPURS titles need before any SPU work can run.*

**Rendering**
- **Live D3D12 draw engine** — executes the guest's real vertex program (pixel-perfect dbgfont text), then guest fragment-program pipelines, per-draw VP constants and textures, indexed draws, render-to-texture, MRT, multi-unit textures, FP predication/branches, float RTs, and a VS cache keyed by ucode hash — *[@sagemono](https://github.com/sagemono)* (#43, #76)
- **Present correctness** — present only on guest flip (partial-frame flicker), present at the clear boundary (ring-wrap blink), flips ordered against the drain, honest flip status — *[@sagemono](https://github.com/sagemono)*
- **GCM FIFO ring recycle on wrap** — the command ring never recycled (the context callback OPD was left null), so titles wedged ~200 frames in and the render path aborted with the infamous `0xC708C708`. A real command-buffer-full callback now drains and resets the ring — *[@sagemono](https://github.com/sagemono)*

**Lift & decode**
- **`blrl` dispatched as an indirect call** — it was lumped with `blr` and emitted a bare `return;`, dropping the call *and* skipping the frame epilogue, so `r1` leaked the frame size on every function-descriptor call — *[@sagemono](https://github.com/sagemono)* (#42)
- **EBOOT (`ET_EXEC`) import parsing** — the lib-stub table was only reachable via the PRX `module_info` path, so every retail main executable reported 0 imports; now located via `PT_PROC_PRX_PARAM`. Minecraft: 0 → 345 NIDs across 30 modules — *[@sagemono](https://github.com/sagemono)* (#41)
- **`lift_function` indexed by address** — an O(refs·N) mid-function pass hung the lifter indefinitely after "100% lifted" on large titles; now bisect over a cached sorted index — *[@sagemono](https://github.com/sagemono)* (#41)
- **Rotate-by-0 undefined behaviour** — `v >> (64 - n)` at n=0 is UB, and clang miscompiled the `clrldi` truncation idiom every 32-bit address cast lowers to — *[@sagemono](https://github.com/sagemono)* (#41)
- **Lifter torture suite** — 3,793 KATs against an independent PPC model — *[@sagemono](https://github.com/sagemono)* (#64)
- **VMX element-wise ops read big-endian lanes** — values were byte-reversed on x86, so every float lane op quietly computed on the wrong data — *[@canersaka](https://github.com/canersaka)* (#74)
- **D-form loads/stores treat `rA=0` as a literal zero base**, not `GPR[0]` (#72); **update forms write back `rA`** for indexed stores (#71) and FP loads/stores (#73) — dropping the write-back leaves `rA` stale and corrupts every subsequent `(rA)`-relative access — *[@canersaka](https://github.com/canersaka)*
- **`lmw`/`stmw`** (#68), **`mulldo`** overflow form (#70), **`stvlx`/`stvrx`/`stvlxl`/`stvrxl`** Cell unaligned vector stores (#61), **value-verified CAS** for `lwarx`/`stwcx` and `ldarx`/`stdcx` (#62), and an **FP correctness batch** — `fcmpu` NaN ordering, `fcti` saturation, single rounding, `vctsxs`/`vctuxs` (#60) — *[@canersaka](https://github.com/canersaka)*
- **`fpscr` bit ops + `vrsave` decoded as visible no-ops** — `mtfsb0`/`mtfsb1`/`mtfsfi` were undecoded and emitted as a raw `.word`, an invisible dropped instruction — *[@canersaka](https://github.com/canersaka)* (#69)
- **SPU**: `brsl` target parse broken by the disasm link-register fix, so every call lifted as a silent no-op (#58); `bisl`/`bisled` target register; single-round `fma`/`fms`/`fnms`, `fesd`/`frds` from the left word slot, `fi`/`frest`/`frsqest` interpolation (#51) — *[@canersaka](https://github.com/canersaka)*

**Runtime & lv2**
- **`sys_spu_image_import` implemented + SPU-image syscall numbers fixed** — import was mis-numbered 157 (that's `_sys_spu_image_import`) and collided with `SYS_SPU_IMAGE_CLOSE`, so a title's import call dispatched to the close stub and the image struct was left zeroed (entry=0 → matched no fallback → the SPU thread "instantly completed"). Replaced the zero-init stub with a real SPU-ELF32 `PT_LOAD` → `sys_spu_segment[]` parser — *[@sagemono](https://github.com/sagemono)* (#57)
- **`_sys_spu_image_import` (sysPrxForUser NID `0xEBE5F72F`)** — the user-space wrapper `libsre` invokes during `cellSpursInitialize`; previously unresolved, so the SPURS SPU kernel image was never parsed and the SPU threads came up at entry 0
- **18 lv2 syscall numbers corrected** against the SDK's own `sys/syscall.h` (475.001) — the authoritative table. Three were live collisions: the lwmutex block sat on `SYS_RAW_SPU_CREATE_INTERRUPT_TAG`/`SET_INT_MASK`/`GET_INT_MASK` (150–152) and lwcond on `SYS_SPU_IMAGE_OPEN` (156). Now 125/125 match the SDK, 0 duplicates — *[@canersaka](https://github.com/canersaka)* (#65)
- **`sys_mutex_trylock`** recursive fast path pairs the host critical section (#67); **`sys_spu_thread_group_create`** reads r5 as priority and the name from the attr struct (#66) — *[@canersaka](https://github.com/canersaka)*
- **PS3 process-entry registers** (TLS descriptor + page size) and a function registry sized for >64k-function titles — *[@sagemono](https://github.com/sagemono)*

### v0.6.7 — *"Fine Print"* (July 2026)
*The second half of the two-port review pass: [@canersaka](https://github.com/canersaka)'s remaining decode/ABI/timing fixes plus a lifter/HLE audit toolkit, and the title-agnostic robustness fixes from [@JonathanDC64](https://github.com/JonathanDC64)'s port that could be ported and build-verified cleanly.*

**Lift & decode**
- **NV40 VP/FP shader decompilers hardened** against retail-game shaders (LIT/SFL/STR/ARL/RCC/RSQ, relative constant addressing, all 16 ATTR inputs, viewport transform; FP masked-store fix) — *[@canersaka](https://github.com/canersaka)* (#47)
- **`spu_disasm`**: channel field widened 5→7 bits (channels ≥32 no longer wrap), hbra/hbrr field layout, `bisl`/`brsl`/`brasl` link register shown, `ri7` sign-extension, stop-code preserved — *[@canersaka](https://github.com/canersaka)* (#49)
- **SPU `stop` signal code preserved** to `ctx->stop_code` — SPURS leaf tasks invoke kernel syscalls via `stop <code>` (EXIT/YIELD/WAIT/…), which was being discarded — *[@JonathanDC64](https://github.com/JonathanDC64)*

**Runtime & lv2**
- **Sub-millisecond timeouts extended** to `sys_event_flag`, `sys_semaphore`, and `sys_mutex` timed waits (the same 15.6 ms-granularity fix as v0.6.5's event-queue path) — *[@canersaka](https://github.com/canersaka)* (#50)
- **`g_active_ctx` restored after guest callbacks** — it was left dangling at a stack-local scratch ctx, corrupting the crash handler / diagnostics; applied to both `ppu_guest_call` and `ppu_guest_call_ct` — *[@JonathanDC64](https://github.com/JonathanDC64)*

**HLE**
- **Corrected ABI signatures** for cellPamf, cellSail, cellHttp/cellHttpUtil/cellHttps/cellSsl, cellNet/sysNet, sceNpTrophy, and cellGameData (arg order/count/width → correct register mapping) — *[@canersaka](https://github.com/canersaka)* (#55)
- **`cellGameDataCheckCreate2`** (NID `0xC9645C41`) implemented — marshals StatGet/StatSet and fires the guest `funcStat` callback — *[@JonathanDC64](https://github.com/JonathanDC64)*

**Tooling**
- **Lifter/HLE audit kit** — PPU/SPU decode cross-checks vs Capstone / an RPCS3-output oracle, HLE ABI + dispatch-classify audits, an endianness heuristic, a lift-structure census, and an lv2 sync stress test — *[@canersaka](https://github.com/canersaka)* (#56)

### v0.6.6 — *"Two Ports, One Toolkit"* (July 2026)
*Two independent retail-game ports kept fuzzing the toolkit: [@canersaka](https://github.com/canersaka)'s **Yakuza: Dead Souls** and [@JonathanDC64](https://github.com/JonathanDC64)'s **Demon's Souls**. This release distills the title-agnostic correctness/robustness wins each surfaced — the port-specific scaffolding stays in the forks.*

**PPU / SPU lift**
- **`fsqrt`/`fsqrts` source-register decode** + **`vspltis`** printed as a signed immediate (`ppu_disasm`) — *[@canersaka](https://github.com/canersaka)* (#46)
- **`addeo`/`subfeo`/`mulhwo`/`mulhwuo` overflow forms** (previously fell through to a no-op stub) + **`vupklsb`/`vupkhsb`/`vupklsh`** unpacks — *[@canersaka](https://github.com/canersaka)* (#52)
- **Cross-function SPU tail calls forced with `musttail`** — a guest loop whose back-edge crosses a lifted-function boundary was nesting one host C frame per iteration and silently overflowing the stack (a stack-overflow SE runs no unhandled filter, so the process died with no log and exit code 0); now an O(1)-stack jump under clang — *[@JonathanDC64](https://github.com/JonathanDC64)*
- **Mid-function / gap lifting sliced O(n²)→O(span)** — a ~40-minute no-output hang on 96k+ function titles is now bounded work — *[@JonathanDC64](https://github.com/JonathanDC64)*

**Runtime & lv2**
- **Sub-millisecond `sys_timer` usleep** — was a single `SwitchToThread()`, so `usleep(<1000)` was a no-op; now paces to a QPC deadline — *[@canersaka](https://github.com/canersaka)* (#44)
- **`sys_event_queue_receive` returns the event in r4–r7** (lv2 ABI), not just the guest memory buffer — callers reading the registers saw stale values — *[@JonathanDC64](https://github.com/JonathanDC64)*
- **`sys_memory_get_page_attribute`** renumbered 358→**351** (0x15F) and implemented — *[@JonathanDC64](https://github.com/JonathanDC64)*
- **`CellFsStat` runtime-side layout** corrected to the 52-byte / 4-byte-aligned ABI — fixes the `ppu_fs.cpp` / `sys_fs.c` stat writers the v0.6.5 libs-side packing fix didn't cover — *[@JonathanDC64](https://github.com/JonathanDC64)*

**HLE libs**
- **`cellNetCtl`** integer out-params written big-endian — *[@canersaka](https://github.com/canersaka)* (#45)
- **`cellAudio`** delivers real period events to the registered notify queues (games blocked on the audio event now wake) — *[@canersaka](https://github.com/canersaka)* (#54)

### v0.6.5 — *"The Fifteen-Millisecond Tax"* (July 2026)
*Driving **You Don't Know Jack** (Scaleform UI + FMOD, 5,859 functions) from an instant crash to its running main loop surfaced one global performance bug worth more than any single lift fix — plus a batch of community lifter/HLE correctness PRs.*

**Runtime — the event-poll timer-resolution fix** (`sys_event.c`, `tests/boot_main.cpp`):
- Titles that poll an event queue every frame with a **sub-millisecond timeout** (YDKJ uses 30 µs) were throttled ~**500× game-wide**. `sys_event_queue_receive` floored the timeout to a 1 ms `SleepConditionVariableCS`, but Windows' default ~15.6 ms timer granularity inflates *any* sub-15 ms wait to a full tick — a two-queue-per-frame poll loop burned ~30 ms/frame doing nothing. Sub-millisecond timeouts now do a true non-blocking check (immediate `ETIMEDOUT` when empty), and the harness calls `timeBeginPeriod(1)`. YDKJ went from ~0.5 fps effective to ~60 Hz, reaching online-init ~6× faster.
- GCM FIFO drain moved into the 60 Hz vblank tick, so RSX sync-fence labels advance regardless of `present()` latency.

**Community PRs incorporated** — thank you:
- **cellFs big-endian out-params + `CellFsStat` PS3 packing** — *[@canersaka](https://github.com/canersaka)* (#22)
- **cellGame reads the real title id from `PARAM.SFO`** — *[@canersaka](https://github.com/canersaka)* (#24)
- **`sys_rwlock` → `EDEADLK`/`EPERM`** on writer self-relock / bad unlock — *[@canersaka](https://github.com/canersaka)* (#25)
- **`ppu_disasm`: opcodes 33/225 are `crnor`/`crnand`** (were swapped) — *[@canersaka](https://github.com/canersaka)* (#40)
- **Static firmware LLE** (`tools/lift_prx.py` + `docs/FIRMWARE_LLE.md`) — relocate a decrypted PRX and lift the real firmware module (e.g. the libsre SPURS kernel) instead of HLE-ing it; a bring-your-own-firmware method that ships no firmware — *[@canersaka](https://github.com/canersaka)* (#53)
- **cellPad DIGITAL2 face-button packing + analog-Y reflect-about-128** — *[@sagemono](https://github.com/sagemono)* (#42)

**You Don't Know Jack port** — now public at [sp00nznet/youdontknowjack](https://github.com/sp00nznet/youdontknowjack): boots the full init stack to its **main game loop** with a live D3D12 clear+flip at 60 Hz. Current frontier: the GCM command buffer's ring-wrap flush callback is `null` and a computed `0xC708C708` poison reaches the scene-graph dispatch — so only black clears render so far.

### v0.6.4 — *"Carry the One"* (July 2026)
*A pass of hard-won correctness fixes surfaced by driving flOw (PhyreEngine, ~104k functions) deep into its boot — plus the SPU-side plumbing to get a SPURS taskset actually dispatching. Most of these are silent-corruption bugs: the lift produced valid C for the *wrong* computation, so nothing crashed until a data structure quietly filled with garbage thousands of frames later.*

**Community PRs incorporated** — thank you [@canersaka](https://github.com/canersaka), who found and fixed all of the below and sent them in as pull requests:

*PPU lift*
- **XER[CA] for the shift-algebraic ops** (`sraw`/`srad`/`srawi`/`sradi`) + `mtcrf` field mask — (#21)
- **XER[CA] for `subfe`/`subfme`/`addme`** — (#26)
- **`cntlzw(0)` is 32**, not undefined `__builtin_clz` garbage — (#35)
- **PPU lifter conformance suite** + six emission fixes it found — (#37)
- **`vcmpgtsb`/`sh`/`sw`/`ub`/`uh`/`uw` handlers** (dot forms set CR6) — (#39)

*SPU lift*
- **`bi $r0` reloaded via `lqa`/`lqr` is a computed tail jump**, not a return — (#36)
- **self-referential SPU branch mislifts** — (#30)
- **complete SPU ISA coverage** (all 199 ops + double-precision) — (#31)
- **byte-correct SPU quadword helpers** (`shufb`, `cbd`/`chd`/`cwd`/`cdd`) — (#32)
- **`il` negative-immediate double sign-extension** — (#33)
- **preferred-slot-only link register + `rchcnt`** — (#34)

*Runtime*
- **`mftb`/`mftbu` read a real timebase** — (#38)

**PPU lift — the carry/borrow bug class** (`ppu_lifter.py`):
- **64-bit `add` never wrote XER[CA]** and truncated the carry-out to 32 bits. Any `adde`/`addze`/`subfe` consumer downstream (bignum math, 64-bit pointer arithmetic, the CRT's own `__eabi` helpers) then read a stale carry. Now sets CA from the true unsigned 64-bit carry-out.
- **`subfic` computed a 32-bit result and dropped the borrow** — rewritten as `EXTS(SI) - RA` over the full 64 bits with `XER[CA] = NOT borrow` (`EXTS(SI) >= RA`).
- **`subfme` was missing its XER[CA] update entirely**; `adde`/`addme` carry-out recomputed to match the PowerISA / RPCS3 `add64_flags` ADC semantics.
- **`sraw`/`srad`/`srawi`/`sradi` now set XER[CA]** (was never written) via new `ppc_sraw`/`ppc_srad` helpers — CA = `rS<0 AND any 1-bits shifted out`, which feeds the same `adde`/`subfe` consumers.
- **Fall-through tail-call / computed-`bctr` dispatch leaked the frame**: these were lifted as `ps3_indirect_call(ctx); return;` *without* the function epilogue, leaking `r1` and the callee-saved GPRs on every jump-table `switch` and tail call. Now emits the full epilogue before the tail jump.
- **`ppu_disasm.py --va`**: disassemble a window at a *virtual* address, mapping va→file offset through the ELF's `PT_LOAD` segments — fixing the long-standing footgun where `--raw --offset <vaddr>` read the wrong bytes (off by the `.text` segment base).

**SPU lift — computed jumps & wide ops** (`spu_lifter.py`, `spu_disasm.py`):
- **`bi $r0` computed tail-jumps vs. returns**: a `bi $r0` whose `r0` was just reloaded via `lqa`/`lqr` is a *computed tail jump*, not a return. Treating them all as returns made the SPURS taskset launcher fall through and the dispatcher loop forever — the exact SPU analogue of the PPU tail-call bug above. `compute_bi_r0_jumps()` now classifies them by backward-scanning for the nearest `r0` writer.
- **RRR 4-operand destination register** decoded from bits 21–27 (the low 7 bits are the `RC` *source*, not the dest) — every `selb`/`shufb`/`fma`-family op wrote the wrong register.
- **Double-precision family**: `fesd`/`frds` single↔double convert, and the `dfma`/`dfms`/`dfnms`/`dfnma` double FMA ops (3-register, `RT` is the accumulator).
- **`cgx` extended carry-generate** (RT is also a source), **`mpyhha`/`mpyhhau`** high-half multiply-accumulate, and a **`br .` self-loop trap** (target == self is a deliberate infinite-hang trap on real SPU, not forward progress).

**Runtime robustness**:
- **PPU function-hash table widened** (`ppu_loader.cpp`, `PPU_HASH_BITS` 16→18): the open-addressed registration table live-locked filling past 65,536 functions — flOw registers ~104k. Titles above ~50k functions now register in bounded time.
- **`/app_home/` VFS mapping** (`sys_fs.c`): the install-dir prefix (opened as `/app_home/...`, `app_home/...`, `e:/app_home/...`) now maps to the USRDIR root instead of appending a literal `app_home` directory that isn't on disk.
- **Page-guard watchpoint** (`ppu_guard_page()`, Windows): arm a guest EA and a VEH logs the faulting host RIP of any raw store into that 4 KB page — catches vector/`memcpy` stores that `vm_write*` instrumentation can't see. Env-gated; a diagnostic aid, off by default.
- **`cellGcmGetTiledPitchSize` ABI fix** + real flip/vblank handler dispatch (see prior commits) — the wrong return convention was corrupting the caller's stack.

**SPU SPURS plumbing**: new `libs/spurs/spurs_taskset.{c,h}` + `spurs_pm.c` (taskset descriptor + power-management scaffolding) and `runtime/spu/` workload-dispatch wiring toward running a real SPURS task kernel. Still in progress — the taskset dispatches but completion-event delivery back to the PPU isn't verified yet.

**Tooling**: `tools/test_ppu_lift.py` (PPU lifter unit harness) and `tools/rpcs3_probe/` oracle-introspection scripts used to diff our lift against the running RPCS3 reference.

### v0.6.3 — *"Knowing the Names"* (June 2026)
- **NID resolution, doubled+**: `nid_database` now auto-scans `libs/` + `runtime/` for every function the toolkit implements (`load_implemented()`), and ships 44 more curated names recovered by exact-NID brute-force against the harness corpus. Resolution went from 175 → 421 of the 660 distinct NIDs the sample titles import; the harness stub-prioritization ranking is now fully named through the high-frequency tiers.
- **Harness stub-prioritization ranking**: a new non-gating `imports` stage (via `ppu_loader`'s `proc_prx_param` → libstub walk) extracts each EBOOT's firmware imports and resolves them, and the report ranks the most-imported libraries and functions across titles — the data that says which stubs to write next.
- **`cellNetCtl` net-start dialog** implemented (`LoadAsync`/`UnloadAsync`, imported by every sampled title): posts the `LOADED`/`FINISHED` sysutil callback events so games clear the boot-time "connect to PSN" gate, and reports a connected result.
- **`cellGcmSetupContext()`** — reusable core of `_cellGcmInitBody` (the function `cellGcmInit`'s SDK macro actually calls). Captures the proven `CellGcmContextData` layout (begin/end/current/callback) from the shipping Simpsons port so game bridges stop hand-rolling it.
- **`.opd` function detection hardened**: the TOC base may live in BSS / a segment gap, so detection no longer requires it inside a file-backed range — recovered ~20–23k address-taken functions each on flOw and Marvel Ultimate Alliance.

### v0.6.2 — *"Boot Sequence"* (June 2026)
- **PPU boot scaffold** (thanks [@pauloadrianoalves](https://github.com/pauloadrianoalves), PR #3): `runtime/ppu/` (loader, HLE dispatch, sysprx, filesystem) + `runtime/host/host_main.c` — the per-game path that loads a lifted PPU image, links it with the HLE runtime, and boots it. Built per-game against the lifter-generated `ppu_recomp.h`, so it's excluded from the game-agnostic runtime library. See `docs/PPU_RECOMP.md`.
- **RSX shader decompilers** (thanks [@pauloadrianoalves](https://github.com/pauloadrianoalves)): `rsx_fp_decompiler` / `rsx_vp_decompiler` translate NV40 fragment/vertex programs to host shaders, with a validation-test corpus. See `docs/RSX_FRAGMENT_PROGRAM.md`.
- **PPU tooling**: `tools/ppu_loader.py` (image manifest / OPD table / TOC / imports) and `tools/gen_hle_nids.py`.
- *Note:* PR #3's SPU lifter/disassembler and `nid_database` changes were **not** taken — they predate and would regress the v0.6.0 SPU subsystem and the v0.6.1 NID fix.

### v0.6.1 — *"Many Hands"* (June 2026)
*The community showed up. Most of this release came in over the wall as pull requests — almost all of it discovered by [**@canersaka**](https://github.com/canersaka) while stress-testing the toolkit against a 22 MB / ~45,000-function **Yakuza: Dead Souls** port, which turns out to be a fantastic fuzzer for everything we got subtly wrong. Huge thanks to every contributor below.*

**Correctness — decode & lift** (thanks [@canersaka](https://github.com/canersaka)):
- **NID computation, fixed** (`nid_database.py`): the suffix constant was truncated to 12 bytes and corrupted past byte 8, and the digest was read big-endian. The authoritative 16-byte suffix + little-endian read now matches real EBOOT import tables — `cellSysmoduleLoadModule` → `0x32267A31`. Previously matched **0 of 354** import NIDs on a real game; now 343/354 resolve. (Our own `include/ps3emu/nid.h` already documented the correct values; the Python tool just disagreed with it.)
- **VMX/AltiVec decode tables, cross-referenced against the Power ISA manuals** (`ppu_disasm.py`): dozens of mnemonics mapped to the wrong codes (`vmaxfp`/`vminfp`, `vcfsx`/`vcfux` swapped with `vrefp`/`vrsqrtefp`, the whole `vpk*` pack family permuted, signed/unsigned `vcmpgt*` swapped, `vsumsws`, …). These decoded *silently* and lifted to valid C for the wrong operation — `vcfsx` alone was wrong 9,728× in one title.
- **VMX lifter handlers**: `addc`/`subfc` with carry-out into XER[CA] (86% of all unlifted instructions in Yakuza — 63k of 73k), the unaligned vector loads `lvlx`/`lvrx`/`lvlxl`, `ldbrx`, `vsrw`, `vsububs`, `vsum2sws`, `vupkhsh`, `vrfim`, and `vand` (which had an implementation but was missing from the dispatch list).
- **`sradi` decode for shifts ≥ 32**: the 6-bit shift carries its top bit in instruction bit 30, so the XO field reads 827 (which was unmapped) for shifts of 32+. Now composed correctly. *(Follow-up: removed a dead, incorrect `XO 827 → lhaux` block — real `lhaux` is XO 375.)*
- **Function detection seeded from the `.opd` table** (`find_functions.py`): PS3 executables list a descriptor for every address-taken function (every C++ virtual, every callback) — 21,893 of them in Yakuza, of which exactly **1** was being detected by prologue scanning. Now seeds starts from `.opd` (located by shape, since section names are often stripped) + the ELF entry, bounds them at the first `blr`, and filters phantom branch targets from data decoded as code.
- **Lifter parallelization + chunked output**: per-function translation now fans out across a process pool (`--jobs N`) — a multi-hour single-core lift on a 45k-function game drops to minutes. Plus record-form CR0 handling, runtime-matched context, and split-file C output so the generated source is actually buildable.

**Runtime & libs** (thanks [@canersaka](https://github.com/canersaka)):
- **`sys_vm` syscall family (300–313)** implemented — `sys_vm_memory_map` returned ENOSYS with an unwritten out-pointer, crashing callers on first deref. Mappings now bump-allocated from a committed 0x60000000 window.
- **`sys_memory` allocations** now come from a committed-on-demand 0x40000000–0x50000000 window (the old bump base at 0x20000000 failed every allocate with silent ENOMEM); addresses match real-hardware traces byte-for-byte, and the bump pointer is now thread-safe.
- **`sys_lwmutex_destroy` lv2 semantics**: returns ESRCH for a dead mutex / EBUSY for a held one (libc teardown branches on these) instead of unconditional OK; slot allocation is now locked.
- **TTY syscall numbers** corrected to 402/403 (`lv2_syscall_table.h` had 400/402).
- **Big-endian guest structs in `cellVideoOut`** + corrected `CellGcmContextData` layout (callback at +0xC): `GetResolution` was writing host-endian, so the guest read 720×480 as 53250×57345 and sized display buffers from garbage.

**Build & tooling**:
- **Linux/GCC build fixed** (thanks [@LucasPicoli](https://github.com/LucasPicoli)): glibc's `st_atime`/`st_mtime` macros were shadowing `CellFsStat` members; five translation units now compile clean so downstream projects get a Linux runtime to link against.
- **Runtime test harnesses excluded from the library build** (thanks [@canersaka](https://github.com/canersaka)): `runtime/spu/tests/` defines its own `main()`; the library now builds clean with MSVC.
- **`tools/show_func.py`** (thanks [@canersaka](https://github.com/canersaka)): dump a single lifted function's C, its original PowerPC disassembly, or both, straight from the chunked output.

### v0.6.0 — *"SPU, For Real This Time"* (June 2026)
- **SPU recompilation, corrected end-to-end**: rebuilt `spu_disasm.py`'s opcode tables from rpcs3's authoritative SPUOpcodes.h. Pervasive mis-decodes (lqr/stqr/fsmbi were excluded from RI16 and shadowed by spurious RI10 rotate entries; RI16 branch/load forms at wrong opcodes; ~15 wrong SPU_RR entries; missing RI8 float conversions) had silently corrupted *every* SPU lift.
- **SPU lifter fall-through fix**: `spu_lifter.py` now emits a tail-call when a function ends in a non-branch — previously such functions fell off the end and truncated execution mid-image. Added cflts/cfltu/csflt/cuflt float conversions.
- **Per-image SPU dispatch**: added `image_id` to the SPU context + an image-tagged function registry (`spu_channels.c`, `spu_begin_image()`), so a SPURS kernel, a policy module, and a job can coexist at overlapping local-store addresses.
- **SPU tooling**: `extract_spu_images.py` (pull SPU images from PRX/ELF), `find_spu_functions.py` (SPU function bounds), `wrap_spu_elf.py` (wrap raw SPU blobs).
- **SPU runtime tests**: self-contained sum / shufb / DMA / brsl-return tests under `runtime/spu/tests/`, plus `test_spu_helpers.c`. New `docs/SPU_LIFTER.md`.
- **New port target — The Simpsons Arcade Game** (NPUB30563): a Konami arcade-emulator EBOOT that drove all of the above. Boots through the CRT to full GCM init with a live D3D12 window; rendering is gated behind the game's CRI-middleware-on-SPURS task pipeline.

### v0.5.1 — *"All-Register FIFO"* (April 2026)
- **All-register FIFO flag clearing**: PhyreEngine sets +0x18 pending flags on r27/r28/r30/r26 (4 structures). Previously only cleared r31. Now clears all, solving 2 of 3 init spin levels.
- **Watchdog CTR=0 patching**: Background thread detects NULL function pointer spins, patches heap objects with NOP OPDs.
- **D3D12 backend active**: Device FL11.0, vertex-colored PSO, batched DrawInstanced, VSync present. First GPU-rendered geometry from PS3 recomp.
- **Level data rendering**: flOw XML level definitions parsed — authentic ocean gradient, snake creatures, food objects, particles. 486 vertices per frame.
- **Batched DRAW_ARRAYS**: RSX method format caps at 255 verts per command. Auto-batching splits larger draws.
- **Control register host-endian**: RSX MMIO accessed via lwbrx/stwbrx in recompiled code. Fixed endianness for put/get/ref.
- **flOw progress**: D3D12 renders Level 3 scene at ~17fps. 12 subsystems ticking. Full GCM pipeline: NV40 → RSX processor → D3D12. Render context init blocked by 3rd-level data loop (no HLE escape).

### v0.5.0 — *"Pixels on Screen"* (April 2026)
- **RSX command buffer processing wired up**: `gcm_flush_guest_cmdbuf()` now initializes RSX state via `rsx_state_init()`, passes NV40 commands to `rsx_process_command_buffer()`, and dispatches to the backend. Games can write GCM commands and see results on screen.
- **FIFO watchdog thread**: Background thread monitors `ctrl->put` for changes, scans command buffer for SET_REFERENCE (method 0x0050) and WRITE_BACK_END_LABEL (0x1D6C), updates `ctrl->get`/`ctrl->ref` automatically. Breaks cellGcmFinish spin loops without needing HLE bridge interception.
- **Control register endianness fix**: RSX DMA control registers (put/get/ref) are MMIO accessed via `stwbrx`/`lwbrx` in recompiled code (host-endian, no byte-swap). The HLE now reads/writes these in host-endian via direct `uint32_t*` access instead of `vm_write32` (which byte-swaps for big-endian).
- **Command buffer IO mapping**: `cellGcmInit` now maps the command buffer region (1MB-aligned) via `cellGcmMapMainMemory` so `cellGcmAddressToOffset` succeeds for buffer addresses. Previously only the game's explicit mapping was registered.
- **AddressToOffset fallback**: Bridge now synthesizes offsets for unmapped addresses in the heap/command buffer range (0xA00000-0x2000000), preventing failures in GCM inline code that converts arbitrary guest addresses to RSX offsets.
- **GCM callback handler upgrade**: Buffer overflow callback now flushes pending commands via `rsx_process_command_buffer`, updates control register put/get, then resets `ctx->current`. Previously only reset current without processing.
- **cellGcmSetFlip error logging**: Bridge logs failed flip commands with buffer ID and error code. Fixed buffer ID passing (game passes context ptr in r3 and buffer ID in r4, but bridge reads r3 as buffer ID).
- **flOw progress**: **Window visible with animated clear color!** Full GCM rendering pipeline working: NV40 clear commands → RSX processor → null backend → Win32 window repaint. 12 subsystems ticking with inline switch case implementation. 1200+ frames, buffer flips alternating 0/1. **Next blocker**: PhyreEngine FIFO sync protocol blocks render context creation — not cellGcmFinish, needs custom sync protocol investigation.

### v0.4.3 — *"Living Code"* (March 2026)
- **Lifter: mid-function tail-entry resolution**: When a trampoline branch targets an address inside an existing function body, the lifter now generates a real tail-entry function containing the code from that address to the function's end. Eliminated ~23K stubs in flOw (91K → 101K real functions). Iterative resolution handles chains of mid-function entries.
- **TLS `__declspec(thread)` fix**: `g_trampoline_fn` declaration mismatch caused MSVC to compile 37K+ writes as direct stores to the read-only TLS template in `.rdata`, crashing with AV WRITE. Fixed by declaring `__declspec(thread)` consistently across all translation units.
- **DRAIN_TRAMPOLINE after all calls**: Lifter now emits the trampoline drain macro after every `bl` (direct call) and `bctrl` (indirect call). Previously only emitted in the preamble; callers that set `g_trampoline_fn` and returned wouldn't have their chains drained.
- **CTR decrement wrap fix**: `bdnz`/`bdz` decremented CTR as uint64, causing loops with 32-bit counts sign-extended to ~0xFFFFFFFF_8XXXXXXX to wrap past zero into 18-quintillion-iteration infinite loops. Fixed: `ctx->ctr = (uint32_t)(ctx->ctr - 1)`.
- **MSVC compatibility in lifter output**: `__builtin_clz` replaced with `_BitScanReverse`, `__int128` multiply replaced with `_mul128`/`_umul128` intrinsics, all function/memory declarations wrapped in `extern "C"` for correct C++ linkage.
- **ppu_context struct expanded**: Added VMX/AltiVec `ppu_vr vr[32]`, `vscr`, `fpscr`, `cia`, `reserve_addr`/`reserve_value`, `thread_id` fields. Enables full VMX vector operations, atomic reservations, and thread identification.
- **sys_lwmutex_reset_all()**: New function to destroy all lwmutex/lwcond state for clean CRT abort redirect. Prevents deadlocks when CRT initialization is interrupted mid-mutex-lock.
- **RLD instruction decode fix** (ppu_disasm.py): MD-form rotate instructions use 3-bit sub-opcode (bits 27-29), MDS-form uses 4-bit (bits 27-30). Was using wrong bit fields, producing incorrect `rldcl`/`rldcr` decoding.
- **Inline VM access optimization**: Game projects can define `vm_read`/`vm_write` as inline functions in ppu_recomp.cpp preamble, bypassing function call overhead. Eliminates ~100x slowdown on byte-by-byte memory operations (critical for CRT memset of multi-GB address spaces).
- **cellSysmodule duplicate fix**: `cellSysmoduleLoadModule` returns CELL_OK for already-loaded modules instead of CELL_SYSMODULE_ERROR_DUPLICATED. Games like flOw treat the error as fatal.
- **OPD-resolved ELF constructors**: Extracted 166 constructor OPD entries from `.ctors` section, resolved through OPD to real function addresses (0x12F14, 0x1CD2C, etc.), and execute them directly. Constructors now allocate PhyreEngine objects (3 objects with malloc + lwmutex_create).
- **flOw progress**: Game boots through CRT, runs 166 constructors (creating PhyreEngine globals), loads all modules (NP, SPURS, USBD, JPGDEC, NET), registers sysutil callback with correct function pointer (0x84C630). PhyreEngine enters early init and makes 10+ memory allocations. 102K+ recompiled functions. **Next blocker**: PhyreEngine asserts on null internal pointers — needs CRT full init or manual global initialization.

### v0.4.2 — *"Main Event"* (March 2026)
- **flOw reaches game main() initialization**: CRT startup completes, abort intercepted via longjmp redirect, game enters `func_000CB9CC` with clean stack. Loads modules (GCM_SYS, SPURS, USBD, JPGDEC, NET), registers sysutil callback. First PS3 recomp project to reach game-level code execution.
- **Trampoline system for split-function chains**: `convert_trampolines.py` and `post_lift.py` transform fallthrough edges between split functions into explicit trampoline calls. When the lifter splits a large function at branch targets, the resulting pieces are chained via trampolines so control flow is preserved without relying on physical code adjacency.
- **DRAIN_TRAMPOLINE pattern**: Macro inserted after every `bl` call site in `ppu_recomp.cpp` (143K+ sites) to drain pending trampoline returns. Prevents unbounded host stack growth when recompiled split-function chains would otherwise nest deeply through call/return mismatches. Converts deep recursion into a flat loop.
- **Manual dispatch stub registration**: `indirect_dispatch.cpp` supports registering stubs for mid-function entry points that the lifter does not emit as standalone functions. Allows `bctrl` indirect calls to reach branch targets inside split functions via hash table lookup.
- **cellSysmodule complete module ID name mapping**: 30+ module IDs now map to correct human-readable names (0x00=NET, 0x0A=SPURS, 0x10=GCM_SYS, 0x17=Camera, 0x1C=USBD, etc.). Previously showed wrong names for several IDs.
- **SEH-based crash recovery**: Three CRT constructors that access unmapped guest memory are caught via structured exception handling and recovered from, allowing startup to continue past constructor failures.
- **malloc override**: Bump allocator at guest address range 0x00A00000-0x10000000 provides malloc/free for recompiled code without requiring full guest heap infrastructure.

### v0.4.1 — *"First Light+"* (March 2026)
- **RPCS3 audit**: Cross-referenced all major modules against RPCS3's implementations. Added 28 cellGcmSys functions, 5 sysPrxForUser functions, 4 cellSpurs functions. cellGcmSys now at 61+ functions (was 33).
- **D3D12 backend**: Real vertex upload from guest memory with BE byte-swap, DrawInstanced, vertex attrib + shader logging, blend/depth/stencil state callbacks, texture format decode (25 RSX→DXGI mappings). 990 lines.
- **Image encoding**: Real PNG + JPEG encoding via stb_image_write v1.16. ARGB→RGBA/RGB swizzle, configurable quality, in-memory encoding.
- **dcbz fix**: `dcbz` (data cache block zero) now zeroes 128 bytes instead of being a no-op. This was THE root cause of the flOw PhyreEngine init stall — dcbz loops ran trillions of no-op iterations.
- **VMX disasm critical fix**: lvx/stvx decode was in unreachable code path. Moved to main opcode 31 block, eliminating 6,620 TODO instructions.
- **VMX disasm duplicate key fix**: VX-form decode table had overlapping keys. Split compares into separate table with Rc bit handling. Added 60+ VMX VX-form instructions.
- **Lifter**: 100+ instruction mnemonics across 18 categories. New: dcbz (real memset), bctrl→ps3_indirect_call, VMX int/float/logical/compare/convert/splat/merge/shift, mcrf, rlwnm, creqv, mffs/mtfsf, byte-reverse loads, load algebraic, all cache/sync ops. **~10K TODOs remaining** (down from 27K).
- **sys_get_random_number**: Real crypto RNG via BCryptGenRandom / /dev/urandom.
- **stb_image_write v1.16**: Vendored for PNG/JPEG encoding.
- **cellSail**: Expanded to 28 functions with descriptor management for Tokyo Jungle.
- **sceNpCommerce2 + cellSysutilNpEula**: New modules for Tokyo Jungle.
- **RSX primitive/vertex/texture mapping**: Complete utility headers for D3D12 translation.
- **Documentation**: 8 docs updated with comprehensive rewrites. RSX_GRAPHICS.md now 16KB with full D3D12 capability matrix, vertex format tables, shader translation strategy.
- **97+ complete modules**, 250+ files, 70K+ lines of code.

### v0.4.0 — *"First Light"* (March 2026)
- **RSX Command Buffer Processor**: New NV47xx GPU command parser (`rsx_commands.h/.c`) — FIFO command buffer parsing, state tracking for surfaces, viewport, scissor, clear, blend, depth/stencil, culling, color mask, alpha test, textures (16 units × 8 registers), vertex attributes (16 × format/offset), shader programs (fragment/vertex), draw arrays + draw indexed. Backend callback interface (`rsx_backend`) with 12 dispatch points for pluggable rendering.
- **Null RSX Backend**: Win32 window backend (`rsx_null_backend.h/.c`) — creates a window, displays RSX clear color via GDI, FPS counter and debug overlay. Proves command pipeline works before D3D12/Vulkan.
- **D3D12 RSX Backend**: Real GPU rendering backend (`rsx_d3d12_backend.h/.c`) — D3D12 device (FL 11.0), DXGI swap chain (double-buffered flip-discard), RTV descriptor heap, fence-based frame sync, clear to RSX color, VSync present. Ready for vertex buffer upload and shader translation.
- **cellPamf**: Upgraded from Partial — real PAMF stream descriptor parsing (AVC profile/level/resolution, ATRAC3+ channels from actual descriptor bytes), EP table navigation via header offsets.
- **cellVdec**: Upgraded callback pipeline — generates PICOUT callbacks with populated CellVdecPicItem (codec type, PTS/DTS, dimensions) after each DecodeAu. Games checking decode completion now proceed correctly.
- **cellAdec**: Same treatment — generates PCMOUT callbacks with channel count, sample rate, bit depth. Silence output but correct pipeline flow.
- **cellDmux**: Improved callback sequencing and AU tracking for proper ES dispatch.
- **cellSpursJq**: Better job submission/completion tracking, TryWait returns BUSY correctly.
- **cellDaisy**: Marked Complete (was already a real ring buffer implementation).
- **Image decoders**: cellPngDec, cellJpgDec, cellGifDec upgraded with real stb_image v2.30 decoding paths, guest memory path translation via `cellfs_translate_path()`.
- **cellSync2**: Timed mutex lock (spin with SwitchToThread/pthread_mutex_timedlock).
- **stb_image.h**: Vendored v2.30 for PNG/JPG/GIF decoding.
- **Lifter**: Split-function fallthrough fix, branch-target function detection, bctrl→ps3_indirect_call dispatch. Scalar: rldcl/rldcr, addze/addme/subfze/subfme, mulld/divd/mulhd/mulhdu, adde/subfe, cror/crand/crnand/crxor/crnor/creqv, mcrf, rlwnm, stdux/ldux, lwarx/stwcx./ldarx/stdcx., lwbrx/stwbrx/lhbrx/sthbrx, lwax/lhaux, mftb/mftbu, lswi/stswi, tw/td, mffs/mtfsf, cache/sync no-ops. **VMX/AltiVec**: lvx/stvx/lvebx/stvebx/lvsl/lvsr, vperm, vmaddfp/vnmsubfp/vaddfp/vsubfp, vsel, vsldoi, vmsumshm, vand/vandc/vor/vxor/vnor, vaddubm/vadduhm/vadduwm/vsububm/vsubuhm/vsubuwm, vmaxub-sw/vminub-sw/vmaxfp/vminfp, vspltb/vsplth/vspltw/vspltisb/vspltish/vspltisw, vmrghb-w/vmrglb-w, vcfsx/vcfux/vctsxs/vctuxs, vrefp/vrsqrtefp, vcmpeqfp/vcmpgefp/vcmpgtfp/vcmpbfp/vcmpequb-w/vcmpgtub-w/vcmpgtsb-w. **~20K fewer TODO instructions on relift** (27K→~7K estimated).
- **Disassembler**: XO-form arithmetic split (addze/addme/subfze/subfme), rldcl/rldcr register-shift rotate, byte-reverse loads, load algebraic, mftb, string word, VMX shift helpers.
- **Runtime**: sys_tty_write/read (syscall 402/403), unimplemented syscall logging, WASAPI COM GUID fix, all duplicate symbol warnings eliminated.
- **Documentation**: CONTRIBUTING.md, docs/CUSTOM_MODULES.md, docs/FAQ.md, updated case study with flOw v0.4.0 technical lessons.
- **SPURS**: Upgraded from Partial to Complete (management + event flags + job queues + Daisy FIFO all functional).
- **95 complete modules**, only media decode (cellVdec/cellAdec need FFmpeg) remains truly partial.

### v0.3.1 — *"Finishing the Sweep"* (March 2026)
- **16 new modules** — clearing out nearly every remaining "Not Started" module
- **cellFontFT**: FreeType font rendering — 16 font slots, fallback metrics (ascender=0.8×size), glyph bitmap stubs, kerning
- **cellFreeType**: FreeType2 library wrapper — reports version 2.4.12 (PS3 firmware 4.x)
- **cellSpursJq**: SPURS Job Queue — queue create/destroy, job push (completes immediately), wait, port system, 16 max queues
- **cellDaisy**: SPURS Daisy Chain FIFO pipes — **real ring buffer** with push/pop, 32 max pipes, 256 max depth, count/free queries
- **cellHttps**: HTTPS client — TLS stub with CA cert and client cert management, verify level config, 8 handles
- **cellSubdisplay**: PS Vita Remote Play — init/start/stop lifecycle, always reports not connected, empty touch data
- **cellImeJp**: Japanese IME — character input, backspace, confirm, passthrough conversion (no kanji dictionary), 4 handles
- **cellVideoExport**: Video export to XMB — init/end, export fires NOT_SUPPORTED callback
- **cellMusicExport**: Music export to XMB — init/end, export fires NOT_SUPPORTED callback
- **cellGameRecording**: In-game video recording — start/stop/pause/resume state tracking, no actual capture
- **cellAdecAtrac3p**: ATRAC3+ audio decoder — 2048 samples/frame, mono/stereo, 44.1/48kHz, outputs silence
- **cellAdecCelp8**: CELP8 voice codec — 160 samples/frame @ 8kHz mono, multiple bitrate modes, outputs silence
- **cellVdecDivx**: DivX video decoder — Mobile/Home/HD profiles, outputs black frames
- **cellPhotoExport / cellPhotoImport**: Photo export/import to/from XMB — NOT_SUPPORTED stubs with callbacks
- **cellPrint**: USB printer support — init/end, reports 0 printers
- **cellRemotePlay**: Remote Play availability — init/end, always unavailable
- **93 complete modules** (up from 77), only ~3 niche modules remain unstarted

### v0.3.0 — *"Full Metal RSX"* (March 2026)
- **cellGcmSys**: Major upgrade from Partial to Complete — command buffer control (put/get/ref), local memory bump allocator, IO memory mapping with proper offset table population (1MB page granularity), flip handler + VBlank callbacks, 15 tile slots + 8 zcull regions, 256 report data + label slots, platform-native timestamps, GetTiledPitchSize, 27+ functions total
- **cellHttp**: Major upgrade from Partial to Complete — real HTTP/1.1 via native sockets (Winsock2/POSIX), DNS resolution via getaddrinfo, TCP connect, request formatting with custom headers, response header parsing (status code, Content-Length, Connection: close), streaming body receive, per-transaction socket lifecycle, SO_RCVTIMEO/SO_SNDTIMEO timeouts
- **cellSpurs**: Event flags now use real OS blocking — CRITICAL_SECTION + CONDITION_VARIABLE (Windows) / pthread_mutex + pthread_cond (POSIX) via side table, EventFlagWait truly blocks until condition met, broadcast on EventFlagSet
- **77 complete modules** (up from 75), 3 remaining partial (cellVdec/cellAdec need FFmpeg, cellDmux/cellSpurs management-only)

### v0.2.5 — *"The Long Tail"* (March 2026)
- **13 more modules** — mopping up the remaining "Not Started" list
- **cellFsUtility**: Recursive mkdir, file read/write/copy/size/exists helpers
- **cellSail**: Media player lifecycle — state machine with immediate finish (stub, no actual playback)
- **cellVoice**: Voice chat port management — create/delete/connect, no actual voice data
- **cellMic**: Microphone — reports no device attached
- **cellRudp**: Reliable UDP — context management, connect/send return NOT_CONNECTED
- **sceNpClans**: NP clan system — create/join/leave/search (offline stub)
- **cellMusicDecode** / **cellMusicDecode2**: Background music decode stubs
- **cellBGDL**: Background download manager — empty download list
- **cellVideoUpload**: Video upload — returns NOT_SUPPORTED
- **cellLicenseArea**: License verification — Americas default, all areas valid
- **cellOvis**: Overlay system — no-op stubs
- **cellScreenshot**: Upgraded from Partial to Complete
- **75 complete modules** (up from 62), only ~19 "Not Started" remain

### v0.2.4 — *"Peripheral Vision"* (March 2026)
- **13 new modules** in one batch — biggest single release yet
- **sceNpUtil**: Bandwidth test (fake 100 Mbps), NP environment, online ID validation, parental control
- **sceNpCommerce**: Commerce context management, store operations return NOT_CONNECTED
- **sceNpMatching2**: Matchmaking contexts with start/stop, signaling/room callbacks (offline stub)
- **sceNpSignaling**: P2P signaling contexts, connection ops return NOT_CONNECTED
- **sceNpSns**: Facebook/Twitter social integration stubs
- **cellVpost**: Video post-processing handle management, query/exec stubs
- **cellJpgEnc**: JPEG encoder handles (encode needs stb_image_write)
- **cellPngEnc**: PNG encoder handles (encode needs stb_image_write)
- **sys_interrupt**: Interrupt tag/thread tracking (no real interrupts in HLE)
- **cellSheap**: Shared heap bump allocator with block tracking, alloc/free/query
- **cellUsbd**: USB device driver — LDD registration, empty device list
- **cellCamera**: PlayStation Eye — reports no camera attached
- **cellGem**: PlayStation Move — reports no controllers connected
- **62 complete modules** (up from 49), new `libs/hardware/` directory

### v0.2.3 — *"Half a Hundred"* (March 2026)
- **sysPrxForUser**: Upgraded from stub to real — lwmutex backed by CRITICAL_SECTION/pthread_mutex, lwcond backed by CONDITION_VARIABLE/pthread_cond, real host threads, heap management, snprintf, string/mem ops
- **cellSysutil**: Upgraded — BGM playback control, system cache mount/clear, disc game check, license area
- **cellSysmodule**: Upgraded to complete — all module names mapped
- **cellPamf**: PAMF container parser — big-endian header parsing, stream queries, AVC/ATRAC3+/LPCM/AC3 codec info, entry point navigation
- **cellDmux**: AV demuxer — open/close, ES management, stream feed/reset, AU retrieval, flush callbacks
- **cellVdec**: Video decoder API — H.264/MPEG2 codec types, AU submit with AUDONE callback (actual decode needs FFmpeg)
- **cellAdec**: Audio decoder API — AAC/ATRAC3+/MP3 codec types, AU submit with AUDONE callback (actual decode needs FFmpeg)
- **cellNet**: Network core — Winsock/POSIX initialization, DNS resolver with real getaddrinfo
- **cellSsl**: SSL/TLS lifecycle — init/end, certificate stubs, cryptographic RNG (BCryptGenRandom/urandom)
- **sceNpTus**: NP Title User Storage — local variable/data storage with set/get/add/delete per slot
- **49 complete modules** (up from 42), 3 partial modules upgraded to complete

### v0.2.2 — *"Fiber Optics"* (March 2026)
- **cellL10n**: Full Unicode conversion — UTF-8 ↔ UTF-16 ↔ UTF-32 ↔ UCS-2, ISO-8859-1, ASCII, generic `l10n_convert()` API
- **cellFiber**: Cooperative multitasking with native OS fibers (Windows `CreateFiber`/POSIX `ucontext`), 64 concurrent fibers, sleep/wakeup
- **cellResc**: Resolution scaling/conversion — display mode config, buffer management, interlace tables, aspect ratio, flip/vblank handlers
- **cellHttpUtil**: URL parsing/building, percent-encoding/decoding, form URL encoding, Base64 codec
- **sceNpBasic**: Friends list, presence management, messaging, game invitations, block list (offline stub)
- **cellKey2char**: HID keyboard scancode → Unicode character conversion, US-101 layout, shift/caps handling
- **cellAvconfExt**: Audio output device info, sound availability queries, LPCM/AC3/DTS config, video gamma
- **cellGameExec**: Boot parameters, exit to shelf, boot game info
- **42 complete modules** (up from 34)

### v0.2.1 — *"Now With Sockets"* (March 2026)
- **sys_net**: Full BSD socket API — socket, bind, listen, accept, connect, send, recv, sendto, recvfrom, poll, select, setsockopt/getsockopt, getsockname, shutdown, close, gethostbyname, inet_aton, errno
- Wraps Winsock2 (Windows) and POSIX sockets (Linux/macOS) with PS3 error code translation
- PS3-specific SO_NBIO non-blocking mode support
- 128-socket descriptor table with host FD mapping
- **34 complete modules** (up from 33)
- First game port target: **flOw** (NPUA80001) — see [sp00nznet/flow](https://github.com/sp00nznet/flow)

### v0.2.0 — *"Now We're Cooking with Cell"* (March 2026)
- **33 complete module implementations** — up from 7 stubs
- **Real threading**: sys_ppu_thread creates actual host threads (CreateThread/pthreads)
- **Full synchronization suite**: Mutexes with deadlock detection, condvars, semaphores, rwlocks, event queues/flags
- **Functional filesystem**: cellFs and sys_fs with configurable PS3-to-host path mapping, real file I/O, directory enumeration
- **Save data system**: cellSaveData with full callback-driven save/load flow, directory management
- **Game utilities**: cellGame boot check, content paths, PARAM.SFO reading
- **Real gamepad input**: cellPad with XInput (Windows) and SDL2 (cross-platform) backends, analog sticks, triggers, rumble
- **Keyboard & mouse**: cellKb with raw/ASCII modes, cellMouse with delta accumulation and ring buffer
- **Real audio output**: cellAudio with WASAPI/SDL2 backends, background mixing thread @ 5.33ms, multi-port mixing, 7.1 downmix
- **Image decoders**: cellPngDec, cellJpgDec, cellGifDec with stb_image backend
- **Font rendering**: cellFont with stb_truetype backend and fallback metrics
- **Network**: cellNetCtl with real host IP detection, sceNp with configurable PSN identity
- **Trophy system**: sceNpTrophy with persistent JSON storage, unlock timestamps, progress tracking
- **SPURS framework**: cellSpurs management APIs (workloads, tasks, tasksets, event flags)
- **Sync primitives**: cellSync (atomic spinlocks, barriers, lock-free queues), cellSync2 (OS-backed mutex/cond/sem)
- **System utilities**: cellRtc (real clock with PS3 epoch), cellVideoOut, cellMsgDialog, cellOskDialog, cellUserInfo, cellScreenshot
- **Memory management**: sys_memory with bump allocator, containers, shared memory, mmapper
- **Timers**: sys_timer with QueryPerformanceCounter/clock_gettime, periodic events, PS3 timebase frequency

### v0.1.0 — *"Hello, Cell"* (March 2026)
- Initial project structure and architecture
- PS3 ELF/SELF/PRX binary parser
- PPU disassembler with full PowerPC 64-bit + VMX support
- PPU → C code lifter (instruction-level translation)
- SPU disassembler and lifter (basic support)
- NID database with 2000+ function name mappings
- Core runtime: PPU context, memory model, big-endian support
- LV2 syscall stubs: threading, memory, filesystem, PRX loading
- HLE library stubs for 15+ core modules
- Template project for bootstrapping new game ports

---

*"The Cell Processor was ahead of its time. Now it's time to bring it to ours."*
