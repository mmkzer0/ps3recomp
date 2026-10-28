# Named stops

When the runtime meets something it cannot honour, it stops. It does not return
a plausible value and carry on: the guest would continue from a state the real
console never produced, and the divergence would surface far from its cause. A
stop ends the process at the cause and names it.

API: [`include/ps3emu/stop.h`](../include/ps3emu/stop.h). Implementation:
`runtime/diag/stop.cpp`.

## What a stop prints

On stderr, a banner (prose, may change) and then one machine-readable line (the
stable interface):

```
==============================================================================
PS3RECOMP STOP: unresolved-nid
==============================================================================
  what      import has no HLE handler
  nid       0x12345678
  name      cellFooBar
  guest     pc=0x123456 lr=0x123450 tid=7
  frontier  last HLE import 0xDEADBEEF (cellProbeLastCall); inside lv2 syscall 141
  remedy    implement it, or allow-list it in [[hle.stub]]
  exit      status 70
==============================================================================
PS3STOP v=1 kind=unresolved-nid nid=0x12345678 name=cellFooBar pc=0x123456 lr=0x123450 tid=7 last_hle_nid=0xDEADBEEF last_hle=cellProbeLastCall syscall=141
```

The process then exits with status **70** (`PS3_STOP_EXIT_CODE`, sysexits'
`EX_SOFTWARE`), without running `atexit` handlers.

`PS3STOP v=1` is always the last line. After `kind=` come the caller's subject
fields in order, then `pc= lr= tid=` when the stop has a guest context, then the
frontier. A value that is not entirely `[A-Za-z0-9_.:/+@,-]` is double-quoted
with `\" \\ \n \r \t \xNN` escapes, so the line splits like a shell command.
`v`, `kind`, `pc`, `lr`, `tid`, `last_hle_nid`, `last_hle` and `syscall` are
reserved; a caller field with one of those names is emitted as `f_<name>`.

## Kinds

Ids are stable kebab-case; a new gap gets a new kind, never a free-text variant.

| Kind | Raised when |
|---|---|
| `identity-mismatch` | the executable loaded is not the one the runtime was lifted from, or does not match the `title.elf_sha256` pin |
| `lifter-abi` | the lifted code was generated for a lifted-code ABI this runtime does not implement (`PPU_LIFTER_ABI` in the generated header against `PPU_RUNTIME_LIFTER_ABI`), so hooks the runtime assumes would not run |
| `config-invalid` | the configuration is wrong: bad file, unknown key, wrong type, out of range, unknown `PS3RECOMP_*` variable; a diagnostic variable that names more than its table holds (`PS3RECOMP_METAL_FRAME_DUMP_LIST` with more than 32 present numbers: `env`, `count`, `max`) |
| `unresolved-nid` | a firmware import has no handler and no `[[hle.stub]]` entry |
| `unimplemented-syscall` | an lv2 syscall nobody implements and no `[[lv2.stub]]` entry |
| `unlifted-function` | a call into a function the lift does not contain |
| `unresolved-indirect-call` | an indirect call whose target is no lifted function |
| `unknown-instruction` | an instruction the lifter did not know |
| `dispatch-depth-exceeded` | `ps3_indirect_call` nested deeper than its host safety limit (4000) |
| `spu-dma-repeat` | an SPU issued identical MFC transfers continuously past `spu.dma_repeat_stop_secs` |
| `spu-mfc-error` | an SPU issued an MFC transfer the hardware rejects: a size over 16 KiB, a size not a multiple of 16 for a transfer of 16 bytes or more, LSA/EA alignment the MFC does not accept, an LSA range past the 256 KiB local store, an EA range outside committed guest memory (including one that wraps 4 GB), a PUT into the unmapped null page, a storage-control range that is not a whole 128-byte range, or SDCRZ in a tag group with a parked list. The hardware raises an MFC command/alignment error and the SPU does not run on. A zero-byte transfer is not one: RPCS3 executes a command only when its size is non-zero, and the runtime completes it without moving data |
| `spu-label-window` | an SPU MFC PUT/PUTL whose span overlaps the guest store window a device model registered (the RSX label window), or an atomic line store (PUTLLC, PUTLLUC, PUTQLLUC) of a 128-byte line that does. The runtime cannot tell whether the store produces the value an RSX ACQUIRE is waiting for (it would have to land before the walker reads the ACQUIRE, and waiting for a pass would deadlock that producer) or overwrites a label the walker has not read yet (it would have to land after a pass), so it counts the store and stops rather than guess. Every such store is counted, including the one the stop reports (`count`), and `cmd` names the form that was stopped (`0x20` a PUT and a PUTL element, whose list bit the list runner strips; `0xB4`/`0xB0`/`0xB8` the atomic stores). A GETLLAR is a load and is not one |
| `unresolved-guest-callback` | a host-initiated guest callback (`ppu_guest_call` / `ppu_guest_call_ct`) whose OPD or code word is 0 or is not a lifted function, or that cannot be given guest stack space |
| `hle-unsupported-return` | an HLE handler whose declared return type is a host pointer: there is no guest address for it (classified in `tools/gen_hle_nids.py`, see [HLE_ABI.md](HLE_ABI.md)) |
| `spu-job-crt-version` | a SPURS job whose CRT marker at LS `0x30` is not `JOBCRT Ver13` or `JOBCRT Ver14` (Ver12, Ver15, garbage): the loader models those two CRTs only |
| `spu-job-memory` | a SPURS job with a region ending past the job runtime area at the top of local store: image plus bss (`eaBinary` high word x 16), ioBuffer, oBuffer, the scratch + stack block (a `sizeStack` of `0xFFFF` puts `r1` at `0xFFFF0`, which LS masking lands on the context), or a cache buffer. The regions are laid out as `_cellSpursCheckJob` sizes them, each base `align1024`; the end checked is `base + size` (the stack block's aligned top), and an empty region is not checked |
| `spu-job-jm-entry` | a Ver14 job called the jm interface's scheduler-switch entry (word 0), which the runtime does not model |
| `spu-job-dma-list` | a SPURS job descriptor names no binary (`eaBinary` or `sizeBinary` is zero), a binary or cache-list entry that leaves guest memory, or its input DMA list cannot be staged into the job's ioBuffer: the list leaves guest memory, an element sets the stall-and-notify bit (bit 31 of the size word; a job start has no party to acknowledge the stall), has size 0, a size over 16 KB or of the wrong alignment, a null or misaligned EA, an EA outside guest memory, or the staged elements (a transfer under 16 bytes at its offset `EAL & 15` in its quadword) do not fit `sizeInOrInOut`. Raised before any guest read of the binary or the cache and before any local-store write |
| `jq2-pool-release` | a host-side `CopyPushJobBody` descriptor-pool release named an EA that is not a live block of the queue's pool: outside every class region (`reason=foreign`), inside a class region but not on a block start (`reason=misaligned`), already on the free list (`reason=double-free`), or the pool's guest count and host free map disagree (`reason=count-mismatch`). Fields: `queue`, `ea`, `class` (decimal, or `none` when no class owns the EA), `reason` |
| `jq2-job-end` | a job the JobQueue2 executor ran ended other than by returning to the job manager, whichever way it ran (interpreted: not `stop 0` just past the link; lifted: a halt-assertion or any end that is not a branch onto the link, `0x3FF00`), or the SPURS job loader refused it. The port is not credited and the pool slot is not released |
| `jq2-ring-entry` | the JobQueue2 command ring holds an entry the executor cannot run: JOBLIST, FLUSH or an unknown command, or a JOB/COPY whose descriptor EA is 0 or outside guest memory. Only the executor's own writer produces entries it models |
| `jq2-wake` | a `Port2Sync` waiter the executor cannot park or signal: a credit completed a registered kind-0 waiter whose EA is not a live fiber (`reason=fiber-notify`: null, misaligned, unknown or finished; the line was written by something else or the fiber was destroyed while waiting), has another wait kind (`reason=wait-kind`) or an event-flag id above 63 (`reason=waiter-id`; the firmware's PM wake rejects unreduced ids above 63, so a credit trying to wake a registered waiter with id >= 64 stops; id0 leaks, ids 1..63 wake normally; a dropped waiter stays blocked through queue teardown where destroy gets EBUSY); all 128 guest bits are occupied (`reason=waiter-exhausted id=128`); the blocked Sync's event-flag wait returned non-zero, e.g. ESRCH from an invalid flag or one destroyed before the wait (`reason=wait`); or a fiber's `cellFiberPpuWaitSignal` refused the wait (`reason=fiber-wait`). A fiber pusher parked on a full ring has the same two failures: its wait is refused (`reason=push-room-wait`) or the intake pass cannot signal it (`reason=push-room-notify`). A valid kind-0 waiter is signalled, not stopped |
| `jq2-state` | a JobQueue2 executor invariant broke: a finished job was not in flight in the scheduler, a ring entry named a port EA that is not a port block, the ring writer refused an entry the front end had validated, a port's queue slot was not allocated, a worker thread could not be started, or a push passed its checks while Shutdown ran and reached the ring after the workers were told to leave (`reason=push-after-shutdown`) |
| `jq2-jm-call` | a JobQueue2 job called jm interface word 0 (`0x1500`, the policy module's scheduler switch: "yield this Execute slot to the manager with a request"). No Odin Sphere job image does; the HLE executor runs one job per worker and has no manager to switch to. The job is neither executed nor credited |
| `hle-guest-range` | an HLE handler was given a guest EA or span that is not inside guest memory: `ea` crosses `limit`, or `ea + size` wraps. The RSX's NV0039 copy raises it for a source or destination span (`arg=m2mf-source` / `m2mf-destination`) that leaves the 32-bit offset space, does not map to one contiguous run of guest memory, or leaves guest memory, before the first byte moves. An NV308A inline-transfer word does the same for its destination (`arg` starts with `nv308a-destination`): a local offset past the local memory, a main-memory offset the IO table does not map (RPCS3 `get_address`: "RSXIO memory not mapped"), or a word that leaves the 32-bit offset space or guest memory. That `arg` carries the transfer state after the name, as the `reason` of the `gcm-unmodelled` row below does. Fields: `func`, `arg`, `ea`, `size`, `limit` |
| `hle-commit-failed` | a guest span that IS inside guest memory (`vm_range_ok`) could not be committed for a host access: `vm_commit`'s `mprotect`/`VirtualAlloc` failed. The span is valid, so this is a host fault, never an error code the firmware does not return, and the handler cannot hand the buffer to the kernel. Fields: `func`, `arg`, `ea`, `size`, `rc` |
| `hle-uninitialized` | an HLE function was called before its module's init, where the firmware's behaviour is undefined (it dereferences state the init builds), so no return value is faithful: `cellRescSetRegisterCount` before `cellRescInit`; `cellGcmMapMainMemory` before `cellGcmInit` (libgcm_sys searches IO page tables that are zero until Init). Fields: `func`, `module`, `reason` |
| `sync2-semaphore-unsupported` | a semaphore call supplies a non-null custom thread/notifier config (`reason=custom-config`), reinitializes a suspended call, finds an unmodelled waiter/queue state, or cannot wait/notify its fiber. Custom configs stop before count or queue side effects |
| `fiber-unsupported` | an initialized fiber/scheduler attribute requests an unimplemented optional callback/setting (`reason=attribute-field`), an unmodelled periodic stack-canary check (`reason=auto-check`), host execution/queue state cannot be honored, a worker event resource fails, the firmware's attempted 33rd worker has no representable 32-bit bitmap slot (`reason=worker-capacity`), or `cellFiberPpuJoinFiber` is asked to join storage that holds a live fiber state word but that this runtime never created (`reason=unknown-fiber`; the firmware, which has no registry, would join it). The raw initializer also stops if there is no calling PPU context from which to read r13 |
| `gcm-unmodelled` | the firmware has a command-stream path the runtime does not reproduce, so a stream built without it would be a guess: `cellRescSetConvertAndFlip` in a PAL-interpolating (720x576 with PAL temporal mode 1..4) or interlaced configuration, or with more than 256 vertices; a command context whose buffer-full callback did not leave room for a reservation. Also the places where the firmware accepts a call whose console result the runtime cannot name: `cellGcmMapMainMemory` with size 0 (`feature=zero-size`, libgcm_sys hands the length to lv2 untested) or with the runtime's IO mapping list full (`feature=io-mapping-slots`, the firmware keeps no such list); `cellVideoOutConfigure` on an output other than PRIMARY (`feature=video-out`). An NV0039 (M2MF) copy whose FORMAT increment is not 1, 2 or 4 (`feature=m2mf-increment`) or whose DMA object is neither `0xFEED0000` nor `0xFEED0001` (`feature=m2mf-dma`); a copy with a host buffer it cannot allocate (`feature=m2mf-overlap`). An NV308A inline-transfer COLOR word (`cellGcmSys.c` `gcm_nv308a_color`) whose NV3062 COLOR_FORMAT is not A8R8G8B8 (10), Y32 (11) or R5G6B5 (4) (`feature=nv3062-color-format`), whose destination DMA object is neither `0xFEED0000` nor `0xFEED0001` (`feature=nv3062-dma`), whose NV308A OPERATION is not SRCCOPY (3) (`feature=nv308a-operation`), whose SET_CONTEXT_SURFACE names anything but the linear NV3062 object, the swizzled NV309E included (`feature=nv308a-surface-context`), or whose SIZE_OUT or SIZE_IN is more than one row high (`feature=nv308a-rows`). Every NV308A/NV3062 stop, these and the destination `hle-guest-range` rows above, carries the transfer state in one line: `reason` (or `arg`) ends in `word=sub<S>/0x<method>/0x<data> point= size_out= size_in= color_words=<this transfer>/<ever> dst_offset= dst_pitch= dst_dma= dst_format= operation= surface= src_format= bind=0:<class>,...,7:<class>`, and the same text is logged on stderr first as `[nv308a] stop ...` / `[nv308a] range stop ...`. An NV308A or NV3062 method with no row ends as `rsx-unknown-method` and logs `[nv308a] unknown method: <that state>` before the stop. Fields: `func`, `feature`, `reason` |
| `gcm-wait-timeout` | an RSX wait stood unsatisfied for `gcm.wait_timeout_ms` (default 10 s): a NV406E semaphore ACQUIRE whose label never reached the value (`what=semaphore-acquire`), the command-context buffer-full callback waiting for the RSX to leave the next 32 KiB segment (`what=segment-busy`), a self-JUMP park the guest never patched while `put` stood elsewhere (`what=jump-park`, `func=cellGcm_rsx_process_fifo`; `offset` is the park, `value` the `put` it stood against, `current` the park's JUMP target; the Reference mode waits at the park, `gcm.park_resync` is the Compatibility mode that follows `put` instead), or a guest store to a label waiting for the RSX walker to complete a pass (`what=rsx-walker-pass`, `func=cellGcm_label_store`; `offset` is the label's offset, `value` the value being stored, `current` the label's value; the walker thread never ran, see `libs/video/cellGcmSys.c`, "guest stores to a label are ordered after the walker"). The hardware waits forever; the agent that should end the wait never ran. Fields: `func`, `what`, `offset`, `value`, `current`, `waited_ms` |
| `guest-null-access` | a guest load or store reached the unmapped null page: a load below `0x10000` or a store below `0x1000` (the bounds `ppu_null_read_report` and the store check always had). Real hardware faults on each; the flat VM would instead read back garbage or swallow the store and let the run continue on it. Default; `ppu.null_page = "drop"` keeps the legacy log-and-drop handling for a title that needs it. An HLE handler whose firmware original stores through an untested out-parameter raises it for a NULL one (`cellGcmGetConfiguration`, `cellGcmAddressToOffset`, `cellGcmMapMainMemory`; `fn` is then the handler's name, and `ppu.null_page = "drop"` does not apply). Fields: `access` (`read`/`write`), `width` (bytes), `ea`, `fn` |
| `stack-guard-fault` | the guard page of a new PPU thread's stack could not be set: `vm_protect` refused it (the guest arena, `ppu_vm_size`, does not cover the stack region) or the host `mprotect` failed. A host fault, never handed to the guest as `CELL_ENOMEM`, and never a stack without its guard. Fields: `size`, `guard`, `base` |
| `event-delivery-lost` | a kernel producer delivered an event to an id that never named a queue: 0, past the table, or an in-range slot that `sys_event_queue_create` / `sys_event_queue_create_direct` never used. This is a stale or uninitialised id in the runtime's own bookkeeping, not a guest situation. A full queue or one the guest destroyed is not a stop: lv2 drops those (RPCS3 `cellAudio.cpp` discards `queues[i]->send(...)`'s result, `sys_spu.cpp` discards `send_run_event`'s), and the runtime drops them too — counted per cause (`sys_event_kernel_drop_count`) and logged once per (producer, cause). Fields: `producer`, `queue`, `cause` (`invalid-id`), `source` |
| `jq2-unmodelled` | a JobQueue2 parameter combination the firmware accepts whose console behaviour is unknown and unmodelled by the runtime: a ring depth of 0 or a worker count of 0 (`depth`, `workers`), a PushJobBody / CopyPushJobBody of a 0-byte descriptor (`queue`, `port`, `size`: the firmware's size test is "64 or a multiple of 128", which 0 passes, and it files the entry as a 64-byte one; the HLE has no code for a 0-byte descriptor), a PushJobBody / CopyPushJobBody with flag bit 1 (`api`, `port`, `flags`: the firmware stores the bit in the entry for the PM, whose reading of it is not decoded), or a push parked on a full ring when the queue shuts down (`reason=push-blocked-at-shutdown`: the firmware's pusher waits for room and what it does when Shutdown arrives meanwhile is not decoded; reachable only for a push without `flags & 4`) |
| `spurs-task-halt` | a SPURS task whose PPU-accepted ELF the taskset policy module then cannot load, where the console's kernel halts the SPU (RPCS3 `cellSpursSpu.cpp` `spursTasksetLoadElf` fails, then `spursHalt`) and the HLE has no SPU to halt. The image is checked on the PPU first and a bad header is a return value (NOEXEC; libsre 4.93 `0x02011ACC`), so only the loader's own outcomes remain: a PT_LOAD segment outside the task area `0x3000..0x40000` (`reason=segment-outside-task-area`, FAULT), a segment whose file bytes leave the image (`reason=not-an-spu-elf`, NOEXEC), and, from the restart scan of a task an SPU created, `null-elf` and `unaligned-elf` (INVAL). An ELF address outside guest memory is `hle-guest-range`. Fields: `api`, `taskset`, `task`, `elf`, `reason` |
| `spurs-dispatch-miss` | a SPURS task or job whose SPU program is loadable but that the runtime cannot dispatch: `spu_workload_dispatch_async` / `spu_workload_dispatch_job` returned 0, because no lifted entry is registered for the image's fingerprint (their log line is `dispatch MISS`) or the host could not start the worker. The console runs the program, so reporting the task started or the job done is success for work that never ran. This is a gap of this toolkit, not firmware behaviour (contrast `spurs-task-halt`). Fields: `api`, `taskset` and `task` (a task) or `job` (a job), `image` (guest EA), `size`, `fingerprint` (FNV-1a-64 of the image, what `spu_workload_register` keys on) |
| `rsx-unknown-method` | an RSX FIFO method no consumer read and no allow-list row in `libs/video/rsx_method_class.h` classifies (S1, issue #47): the console applied the command and this runtime would drop it, so the frame cannot be trusted. The class is the bound object class (`rsx_class_name`; `NV406E` for methods below `0x100`), `method` the offset within it, `subchannel` the FIFO slot, `fifo` the IO offset of the packet header and `word` its data word. A *missing* state (an S2 row) is not this stop: it is counted and logged, because a documented gap is not an unexplained command |
| `rsx-capture` | an RSX capture or replay (`ps3diff.rsx/1`, `docs/PS3DIFF.md`) that cannot be trusted, so it ends instead of writing or replaying one that only looks complete. `reason`: `midstream` (the first RSX command came before any `cellGcmInit`: only a clean reset is a capture), `blob-missing` / `blob-altered` (a blob a replay needs is absent, or its bytes do not hash to the digest the capture names), `read-conflict` (two reads inside one event disagree on the same guest bytes, which a replay cannot reproduce), `readback-format` (the presented surface is in a format the readback contract does not define), `replay-diverged` (the replay's own effect differs from the captured one), `capture-format` (a replay input that is not a valid clean-reset `ps3diff.rsx/1` capture), `output-exists` / `io` (the output directory cannot be used or written). Fields: `func`, `reason`, `detail` |
| `host-path-limit` | a host path an HLE handler builds (a content root plus a title or data directory) does not fit its buffer. Truncating it would name a different directory, and `stat`/`mkdir` would act on that one: cellGame once cut `<stage>/game/<id>/USRDIR` inside `USRDIR` and created `U`, `US` or `USRD` next to the real directory. Fields: `func`, `need` (characters the path needs), `limit` |

Where each is raised, and what it carries beyond `pc lr tid`:

| Kind | Raised in | Fields |
|---|---|---|
| `identity-mismatch` | `ppu_load_elf` | `path size expected actual expected_from [pin_source]` |
| `lifter-abi` | `ps3_lifter_abi_check` (`runtime/diag/lifter_abi.c`), called from `ppu_load_elf` before the file is read | `expected got` (the runtime's ABI and the lift's) |
| `config-invalid` | `ps3_config_*` | `file line col key env detail` |
| `unresolved-nid` | `ps3_hle_call` (`runtime/ppu/ppu_hle.cpp`) | `nid name caller_lr` (`name` is `unknown`: the lift keeps only the NID) |
| `unimplemented-syscall` | `lv2_syscall` (`ppu_loader.cpp`) | `num caller_lr` |
| `unlifted-function` | `ppu_unlifted_stub` | `target caller_lr` |
| `unresolved-indirect-call` | `ps3_indirect_call` | `target caller_lr descriptors target_words hint` |
| `unknown-instruction` | `ppu_trap_unknown_instruction`, emitted by the lifter | `pc word mnemonic` |
| `dispatch-depth-exceeded` | `ps3_indirect_call_impl` | `target depth limit caller_lr` |
| `spu-dma-repeat` | `mfc_submit` (`runtime/spu/spu_dma.h`) | `image ea lsa cmd size count secs`, with `pc` (LS address) and `tid` (SPU id) as the site |
| `spu-mfc-error` | `mfc_do_transfer` (`runtime/spu/spu_dma.h`), reached from `mfc_submit`, `mfc_run_list` and `mfc_list_stall_ack` | `cmd lsa ea size tag reason` (`reason` is `size`, `alignment`, `ls-overrun`, `ea-uncommitted`, `null-ea` or `sdczr-stalled-tag`); `pc` is the SPU's local-store address and `tid` its SPU id; `lr` is 0, since an SPU has no link register in this model |
| `spu-label-window` | `ps3_stop_spu_label_window`, from `mfc_do_transfer` (`runtime/spu/spu_dma.h`) after the transfer's own validity checks, before any byte moves, and from `spu_mfc_atomic` (`runtime/spu/spu_channels.c`) before the lock-line lock and before any byte of the line is read or committed | `func cmd ea size tag count`: `cmd` is the transfer command (`0x20` for a PUT and for a PUTL element, whose list bit is stripped by the list runner), or the atomic command with `func=spu_mfc_atomic` (`0xB4` PUTLLC, `0xB0` PUTLLUC, `0xB8` PUTQLLUC); `ea` the transfer's effective address (`0x%llX`; the first byte, which is inside the window by at least one byte), for an atomic the 128-byte line base (`mfc_eal & ~127`); `size` its byte count (`128`, the line, for an atomic), `tag` its MFC tag (for an atomic, the value the SPU staged in `MFC_TagID`, which the atomic completes through `MFC_RdAtomicStat` rather than a tag group), and `count` the per-process number of such stores including this one (`ps3_label_window_spu_puts`). `pc` is the SPU's local-store address, `tid` its SPU id, `lr` 0 |
| `unresolved-guest-callback` | `ppu_guest_call`, `ppu_guest_call_ct` | `origin opd code toc descriptor hint`, or `origin=stack sp limit vm_size hint` when no guest stack space can be given (bad caller r1, VM too small for the host window, all host slots live, a host slot out of room); `pc`/`lr` are 0 (no guest instruction runs) and `tid` is the calling guest thread |
| `hle-unsupported-return` | `ps3_hle_call` (`runtime/ppu/ppu_hle.cpp`), from the adapter's `ps3_hle_adapt_host_ptr_stop` | `nid name reason` (`reason` is `returns a host pointer, not a guest EA`); `pc`/`lr` are the caller's return address (the import stub's caller) and `tid` is the calling guest thread |
| `spu-job-crt-version` | `spu_run_spurs_job` (`runtime/spu/spurs_job.c`) | `job image marker bytes` (`marker` is the 12 bytes at LS 0x30 with every non-printable as `?`, `bytes` the same 12 as hex) |
| `spu-job-memory` | `spu_run_spurs_job` | `job image binary bss memory io out scratch stack cache region p limit` (bytes; `bss` is `eaBinary` high word x 16, `memory` = `binary + bss`, `io`/`out` the descriptor's `sizeInOrInout`/`sizeOut`, `scratch`/`stack` `sizeScratch`/`sizeStack` x 16 (a zero stack shows the 8 KB default), `cache` the sum of the cache entry sizes read up to and including the failing one, `region` the one that crossed (`image`, `io`, `out`, `stack` = the scratch + stack block, `cache`), `p` that region's end address (64-bit, never wrapped) and `limit` the lowest reserved top-of-LS address) |
| `spu-job-jm-entry` | `spu_run_spurs_job` | `job image entry lr`; `pc`/`lr` are the trap's LS address and the caller's link register |
| `spu-job-dma-list` | `spu_run_spurs_job` | `job image index ea size ls cumulative capacity reason` (`index` is the list element, or -1 when the failure is not an element (for `cache-range` it is the cache-list entry); `ea`/`size` are the element's EA and bytes (`size` the low 15 bits of the size word), except for `descriptor-range` (the list's start and byte count, or, when the chain walker's descriptor span itself leaves guest memory, the descriptor EA and that span) and `eaBinary`/`sizeBinary`/`binary-range` (the masked binary EA and `sizeBinary`); `ls` is the local-store address the MFC would transfer the element to, its quadword base plus `EAL & 15` (0 for the binary and cache reasons); `cumulative` the bytes through the end of the element from the ioBuffer base, 64-bit and never wrapped (for `cache-range` the cache bytes through the entry); `capacity` the descriptor's `sizeInOrInOut`; `reason` is one of `eaBinary`, `sizeBinary`, `binary-range`, `cache-range`, `descriptor-range`, `stall-notify`, `size-zero`, `size-limit`, `size-align`, `ea-align`, `ea-null`, `ea-range`, `io-overrun`) |
| `jq2-pool-release` | `DescriptorPool::release` (`libs/spurs/jq2/pool.cpp`) | `queue ea class reason`; a host-side pool bookkeeping fault, so there is no guest site (`pc`/`lr`/`tid` stay 0) |
| `jq2-job-end` | `libs/spurs/jq2/executor.cpp` (`require_normal_return`, called by `interpreter_entry` and by the loader's end hook for every body, `Queue::run_job`) | `queue job port tag worker reason status stop_code spu_pc`, with `pc` the SPU's LS address, `lr` its link register and `tid` the worker index (`reason` is `halt`, `stop` or `status`); a loader refusal carries `queue job tag reason=loader rc` |
| `jq2-ring-entry` | `Queue::worker_loop`, `Queue::admit` | `queue read_index ring_ea` for an entry the scheduler does not model, or `queue reason index descriptor` (`reason=descriptor`) for a descriptor EA that is 0 or outside guest memory |
| `jq2-wake` | `Queue::wake`, `Queue::acquire_waiter`, `Queue::block_on`, `PortWait::block`, `Queue::await_room`, `Queue::signal_room_fibers` | `queue reason` plus: `port kind id` from `wake` (the wait kind from port bits 30:29 and the waiter id from `port+0x04`); `port kind fiber status` for `reason=fiber-notify` (`status` the fiber library's `ps3_fiber_notify_status`: 1 null, 2 misaligned, 3 not a live fiber); `id=128 bitmap` for `reason=waiter-exhausted` from `acquire_waiter`; `id rc` from `block_on` (`rc` the event-flag wait's return); `fiber rc` for `reason=fiber-wait` (`rc` the `cellFiberPpuWaitSignal` return); `fiber rc` for `reason=push-room-wait` from `await_room` and `fiber status` for `reason=push-room-notify` from `signal_room_fibers` |
| `jq2-state` | the executor | `queue` plus the facts of the broken invariant (`dispatch`, `port`, `slot`, `error encode`, `workers`; `port reason=push-after-shutdown` from the enqueue path, shared by plain, copy and sync pushes) |
| `jq2-jm-call` | `libs/spurs/jq2/executor.cpp` (`require_normal_return`) | `queue job port tag worker request lr status`, with `request` the job's r3 at the call (the request word) and `lr` the caller's link register (also the `lr` of the machine line); `pc` is the stop word's address, `0x1500` |
| `hle-guest-range` | `ps3_stop_guest_range` | `func arg ea size limit` |
| `hle-commit-failed` | `ps3_stop_commit_failed`, via `vm_commit_or_stop` (`runtime/memory/vm.h`), called by `sys_fs_read`/`sys_fs_write` (`runtime/syscalls/sys_fs.c`) and the cellSaveData `fileBuf` and `setBuf->buf` (the funcStat file list) (`libs/system/cellSaveData.c`) | `func arg ea size rc` |
| `hle-uninitialized` | `ps3_stop_hle_uninitialized` (`libs/video/cellResc.c`, `libs/video/cellGcmSys.c`) | `func module reason` |
| `sync2-semaphore-unsupported` | `libs/sync/cellSync2.c` (`sem_unsupported`) | `api sem config reason`; `config` is the guest config EA, never a timeout |
| `fiber-unsupported` | `libs/spurs/cellFiber.c` (`unsupported`) | `api ea field reason`; an unsupported attribute names its byte offset in `field`; worker-capacity retains the attempted guest count increment |
| `gcm-unmodelled` | `ps3_stop_gcm_unmodelled` (`libs/video/cellResc.c`, `libs/video/cellGcmSys.c`, `libs/video/cellVideoOut.c`) | `func feature reason` |
| `gcm-wait-timeout` | `ps3_stop_gcm_wait_timeout` (`libs/video/cellGcmSys.c`) | `func what offset value current waited_ms` |
| `guest-null-access` | `vm_read8/16/32/64`, `ppu_lwarx32`, `ppu_ldarx64`, `vm_write8/16/32/64` (`ppu_loader.cpp`, `ppu_null_page_stop`); `ps3_stop` in `libs/video/cellGcmSys.c` for a NULL out-parameter | `access width ea fn`; `fn` is the guest function the access came from (resolved from the host return address, `0x00000000` if none), `pc` is that same entry (lifted code keeps no guest pc), `lr` is the context's link register and `tid` the guest thread; the guest call chain is dumped to stderr first. From `cellGcmSys.c` the stop has no guest site: `ea` is `0x00000000`, `fn` the handler's name, `pc`/`lr`/`tid` stay 0 |
| `stack-guard-fault` | `sys_ppu_thread_create` (`runtime/syscalls/sys_ppu_thread.c`) | `size guard base`; `base` is the guest address the guard was for, `pc`/`lr` the creator's link register |
| `event-delivery-lost` | `sys_event_queue_push_from_kernel` (`runtime/syscalls/sys_event.c`), called from `sys_spu_thread_group_join_handler`, the SPU outbound-mailbox hook and the `RD_SPU_DONE_EVENT` probe (`runtime/syscalls/lv2_register.c`), `rsx_send_event` (`libs/video/sys_rsx.c`) and `audio_notify_event_queues` (`libs/audio/cellAudio.c`) | `producer queue cause source`: the producer's name, the queue id, and `cause=invalid-id` (the only stop; full and destroyed are counted drops). The producer is a host thread or an interrupt (an SPU completion, mailbox hook, RSX ISR or audio mix thread), so there is no guest site (`pc`/`lr`/`tid` stay 0); the frontier names the guest call in flight |
| `jq2-unmodelled` | `jq2_queue_create`, `check_push`, `stop_on_push_flag1`, `enqueue` (`libs/spurs/jq2/executor.cpp`) | `depth workers` from create; `queue port size` (size 0) from a push; `api port flags` (flag bit 1) from PushJobBody / CopyPushJobBody; `queue port reason=push-blocked-at-shutdown` from `enqueue` |
| `spurs-task-halt` | `ps3_stop_spurs_task_halt`: `cellSpursCreateTask` (`libs/spurs/cellSpurs.c`, reasons `not-an-spu-elf` for a segment whose file bytes leave the image, and `segment-outside-task-area`) and the taskset restart scan `spu_taskset_task_exited` (`runtime/spu/spu_workload.c`, `api=taskset-restart`, the same reasons plus `null-elf` and `unaligned-elf`) | `api taskset task elf reason`; `task` is the slot id (decimal) and `elf` the guest EA the probe read, with TaskInfo's low three flag bits masked (bit 3 is kept: it is what `unaligned-elf` reports). From `cellSpursCreateTask` `pc`/`lr`/`tid` are the calling guest thread's (`pc` is the caller's return address, as for the other HLE stops); from the restart scan, which runs on whichever thread exits a task or starts the pending ones, they stay 0 |
| `spurs-dispatch-miss` | `ps3_stop_spurs_dispatch_miss`: `cellSpursCreateTask` (`libs/spurs/cellSpurs.c`, `api=cellSpursCreateTask`), the taskset restart scan (`runtime/spu/spu_workload.c`, `api=taskset-restart`), the job-chain walker (`jc_run_one_job`, `api=jobchain`) | `api taskset task image size fingerprint` for a task, `api job image size fingerprint` for a job; `pc`/`lr`/`tid` from the calling guest thread for `cellSpursCreateTask`, 0 otherwise |
| `rsx-unknown-method` | `ps3_stop_rsx_unknown_method`, raised by the FIFO walker (`libs/video/cellGcmSys.c`, `gcm_method_verdict`) while consuming a method | `class method subchannel fifo word data`; no guest site (`pc`/`lr`/`tid` stay 0) |
| `rsx-capture` | `ps3_stop_rsx_capture`: `libs/video/rsx_capture.cpp` (capture writer: `midstream`, `read-conflict`, `readback-format`, `output-exists`, `io`) and the replay runner `tools/rsx_replay/rsx_replay.cpp` (`blob-missing`, `blob-altered`, `replay-diverged`, `capture-format`) | `func reason detail`; the stop is raised on the FIFO-walker or replay thread and has no guest site (`pc`/`lr`/`tid` stay 0) |
| `host-path-limit` | `host_path` (`libs/system/cellGame.c`): `content_root`, `cellGame_set_content_path`, `ensure_dirs`, `content_free_kb`, `cellGameContentPermit`, `cellGameDataCheck`, `cellGameCreateGameData` | `func need limit`; no guest site (`pc`/`lr`/`tid` stay 0) |

Lifted code does not track a guest pc, so for the first four `pc` is the caller's
return address (the import stub's caller, the `sc` site, the call site). Only
`unknown-instruction` knows the exact instruction address.

`dispatch-depth-exceeded` is a **host** limit, not a statement about valid PS3
programs: `ps3_indirect_call` recurses on the host stack, so a cyclic jump table,
a self-targeting indirect call or a tail-call chain that never converges would
overflow it. It was a quiet `return` (a fabricated guest return) before; now it
stops at the call that would have been made.

`spu-dma-repeat` is opt-in (`spu.dma_repeat_stop_secs`, default `0` = never
stop). Identical transfers for `spu.dma_repeat_limit` (default 256) get a 1 ms
back-off, which is timing only. The stop's `count` is the number of consecutive
identical transfers observed and `secs` the wall time they lasted. Its `pc` is
the SPU's local-store address and `tid` its SPU id; `lr` is 0, since an SPU has
no link register in this model.

`spu-mfc-error` is the MFC command/alignment error the hardware raises, so the
SPU cannot run on as if the transfer had completed. Two states that look like
errors are not: a zero-byte transfer completes without moving data (RPCS3's
`process_mfc_cmd` executes an MFC command only when its size is non-zero), and
an `MFC_WrListStallAck` naming a tag with no parked list is ignored (RPCS3 acts
on the acknowledgement only when the tag's stall-mask bit is set). Resuming a
parked list whose remaining elements are malformed is a real transfer and stops
like any other.

`spu-label-window` is the SPU counterpart of the PPU store hook
(`runtime/memory/label_window.{h,c}`): a PUT/PUTL into the window a device model
registered (the RSX label window) is a store the runtime cannot order. Waiting
for a walker pass, as a PPU store does, would deadlock a PUT that is the value
an RSX ACQUIRE is waiting for (the walker is inside the pass that waits on it,
so no new pass can start); landing it unordered is the #34 race. The runtime
counts it and stops, naming the EA, size, command, tag, the SPU's pc/id and the
per-process count, so a title whose protocol writes a label from an SPU can be
recognised and fixed. A PUT outside the window is untouched and costs one
window read per transfer.

The atomic line stores (`PUTLLC`, `PUTLLUC`, `PUTQLLUC`) are the same store in
a different MFC command: each writes all 128 bytes of one line, so a line that
overlaps the window is counted and stopped too (#51), and `cmd` names the atomic
form that was issued. The check runs before the lock-line lock and before any
byte of the line is read or committed, so the stop does not depend on whether a
`PUTLLC`'s reservation would have succeeded -- the fail-closed reading of the
same contract. `GETLLAR` is a load and is not checked. A line store outside the
window is untouched, like a plain PUT.

`unresolved-guest-callback` is the host-initiated direction: an HLE library
calls `ppu_guest_call` (an OPD) or `ppu_guest_call_ct` (a captured code/TOC) to
deliver a notification into guest code. A callback that never ran is not a
callback that returned 0, so a 0 OPD/code and a code no lifted function starts
at both stop. The `descriptor` field is the four words at the OPD, which tells a
clobbered descriptor from one that was never populated. **Where the firmware
API makes a callback optional, the HLE caller must test the guest value itself**
and do what the firmware does (skip, or return its parameter error); it must not
dispatches 0. Every in-tree `g_ps3_guest_caller` site does this. The HLE frontier
(`last_hle` / `syscall`) names the import or syscall the calling guest thread is
inside, since the host function name is not available.

`hle-unsupported-return` is the HLE mirror of that rule. A handler whose
declared return type is a pointer can only return a guest effective address;
the six GCM getters still return host pointers today, and truncating one into
`r3` is a fabricated success the guest would dereference as an unrelated EA.
They are listed in `tools/gen_hle_nids.py` and called through
`ps3_hle_adapt_host_ptr_stop`, which stops instead of running the body. Fixing
the handler to return the guest EA moves it to the guest-EA list
([HLE_ABI.md](HLE_ABI.md)).

A stop that has a `[[hle.stub]]` / `[[lv2.stub]]` entry for its NID or syscall
number returns the entry's `ret` instead, logging it once as `ALLOW-LISTED ...`
with the reason and config line (`docs/CONFIG.md`). Nothing else continues past a
gap: no environment variable, no blanket form.

`guest-null-access` is the default for every access into the null page, and the
first one stops: the older handling logged `[null-write] ... DROPPED` and
`[null-read]` and carried on, so a worker that read and wrote `NULL+0x5B0..0x5DC`
ran on garbage and hung later, nowhere near the pointer that was null. Bounds are
`< 0x10000` for loads and `< 0x1000` for stores. `ppu.null_page = "drop"`
(`docs/CONFIG.md`, with a written reason in the title config) restores the log,
the dropped stores and the containment of a sweep that walks up out of the page.

`spurs-task-halt` is the second half of a task start, after the PPU has accepted the image. libsre 4.93 checks more on the PPU than RPCS3's `_spurs::create_task` does: `cellSpursCreateTask` (`0x02012520`) and the shared creator it calls (`0x02011ACC`) return `CELL_SPURS_TASK_ERROR_NULL_POINTER` (`0x80410911`) for a null taskset, task id or ELF, `CELL_SPURS_TASK_ERROR_ALIGN` (`0x80410910`) for an unaligned taskset, ELF (16 bytes) or context (128 bytes above SDK `0x27FFFF`, 16 bytes at or below it), `CELL_SPURS_TASK_ERROR_INVAL` (`0x80410902`) for a context shorter than `0x400` bytes or an `lsPattern` that marks more 2 KiB blocks than the context save area holds, `CELL_SPURS_TASK_ERROR_NOEXEC` (`0x80410907`) for an image whose first 8 bytes are not `7F454C46 01020100`, whose `e_machine` is not 23, whose `e_entry` is not above `0x2FFF`, or any of whose PT_LOAD `p_vaddr` values is not above `0x2FFF`, and `CELL_SPURS_TASK_ERROR_AGAIN` (`0x80410901`) when the taskset has no free task id. Every one of those is a PPU return value, never a stop; `_cellSpursTaskAttributeInitialize` (`0x02010D3C`) runs the same header check.

What the taskset policy module finds when it loads the accepted ELF onto the SPU is not a PPU return value: `spursTasksetLoadElf` answers `NOEXEC` / `INVAL` / `FAULT` and `spursTasksetDispatch` calls `spursHalt`. The HLE runs the task at create time, so it is at the create call (or at the restart scan that starts a task an SPU created) that this halt is reported, and only the loader's own outcomes reach it: `segment-outside-task-area` (FAULT: a PT_LOAD whose `p_vaddr..p_vaddr+p_memsz` leaves `0x3000..0x40000`; the sum is 32-bit and wraps, as RPCS3's is) and `not-an-spu-elf` (NOEXEC: a segment whose file bytes leave the image); the restart scan adds `null-elf` and `unaligned-elf` (INVAL: TaskInfo.elf 0, or not 16-byte aligned -- its low three bits are flags, bit 3 is not). An ELF address outside guest memory is `hle-guest-range`, not a halt. The program-header scan stops at the first header, of any type, whose `p_paddr` is at or past `0x40000`, and an ELF with no loadable bytes is accepted, both as RPCS3's `spursTasksetLoadElf` does.

The SPURS job-chain walker (`jc_run_one_job`, `libs/spurs/cellSpurs.c`) reports a job whose descriptor names no binary, or whose binary leaves guest memory, as `spu-job-dma-list` (`reason=eaBinary`, `sizeBinary` or `binary-range`, `image=-1`, `index=-1`) through `spu_job_check_binary` (`runtime/spu/spurs_job.c`) before it fingerprints the binary. It used to log `implausible binary -- skipped` and carry on with the next job. RPCS3 does not model the job-chain policy module (`spursJobChainEntry` is a `TODO` returning false), so there is no oracle line for a skip; the firmware has `CELL_SPURS_JOB_ERROR_INVALID_BINARY` (`0x80410A1F`) for a bad binary, an error the job manager reports through the chain, never a silent skip, and the project's rule for a malformed job descriptor is a stop.

## Frontier

A stop says how far the run got without a tracer of its own: it asks providers
registered with `ps3_stop_register_frontier`. The runtime registers two, over
state it already keeps: the last HLE import (`g_last_hle_nid` / `g_last_hle_name`,
`runtime/ppu/ppu_hle.cpp`) and the lv2 syscall the stopping thread is inside
(`g_sc_inflight`, `runtime/ppu/ppu_loader.cpp`).

## Threads

Any thread may stop. Stops are serialised: concurrent stops print one banner and
one machine line, the first's; the others park until the process exits.
