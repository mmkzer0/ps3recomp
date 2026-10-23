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
| `config-invalid` | the configuration is wrong: bad file, unknown key, wrong type, out of range, unknown `PS3RECOMP_*` variable |
| `unresolved-nid` | a firmware import has no handler and no `[[hle.stub]]` entry |
| `unimplemented-syscall` | an lv2 syscall nobody implements and no `[[lv2.stub]]` entry |
| `unlifted-function` | a call into a function the lift does not contain |
| `unresolved-indirect-call` | an indirect call whose target is no lifted function |
| `unknown-instruction` | an instruction the lifter did not know |
| `dispatch-depth-exceeded` | `ps3_indirect_call` nested deeper than its host safety limit (4000) |
| `spu-dma-repeat` | an SPU issued identical MFC transfers continuously past `spu.dma_repeat_stop_secs` |
| `unresolved-guest-callback` | a host-initiated guest callback (`ppu_guest_call` / `ppu_guest_call_ct`) whose OPD or code word is 0 or is not a lifted function, or that cannot be given guest stack space |
| `hle-unsupported-return` | an HLE handler whose declared return type is a host pointer: there is no guest address for it (classified in `tools/gen_hle_nids.py`, see [HLE_ABI.md](HLE_ABI.md)) |
| `spu-job-crt-version` | a SPURS job whose CRT marker at LS `0x30` is not `JOBCRT Ver13` or `JOBCRT Ver14` (Ver12, Ver15, garbage): the loader models those two CRTs only |
| `spu-job-memory` | a SPURS job whose image plus bss (`eaBinary` high word x 16) reaches the job runtime area at the top of local store, or does not fit in it |
| `spu-job-jm-entry` | a Ver14 job called the jm interface's scheduler-switch entry (word 0), which the runtime does not model |

Where each is raised, and what it carries beyond `pc lr tid`:

| Kind | Raised in | Fields |
|---|---|---|
| `identity-mismatch` | `ppu_load_elf` | `path size expected actual expected_from [pin_source]` |
| `config-invalid` | `ps3_config_*` | `file line col key env detail` |
| `unresolved-nid` | `ps3_hle_call` (`runtime/ppu/ppu_hle.cpp`) | `nid name caller_lr` (`name` is `unknown`: the lift keeps only the NID) |
| `unimplemented-syscall` | `lv2_syscall` (`ppu_loader.cpp`) | `num caller_lr` |
| `unlifted-function` | `ppu_unlifted_stub` | `target caller_lr` |
| `unresolved-indirect-call` | `ps3_indirect_call` | `target caller_lr descriptors target_words hint` |
| `unknown-instruction` | `ppu_trap_unknown_instruction`, emitted by the lifter | `pc word mnemonic` |
| `dispatch-depth-exceeded` | `ps3_indirect_call_impl` | `target depth limit caller_lr` |
| `spu-dma-repeat` | `mfc_submit` (`runtime/spu/spu_dma.h`) | `image ea lsa cmd size count secs`, with `pc` (LS address) and `tid` (SPU id) as the site |
| `unresolved-guest-callback` | `ppu_guest_call`, `ppu_guest_call_ct` | `origin opd code toc descriptor hint`, or `origin=stack sp limit vm_size hint` when no guest stack space can be given (bad caller r1, VM too small for the host window, all host slots live, a host slot out of room); `pc`/`lr` are 0 (no guest instruction runs) and `tid` is the calling guest thread |
| `hle-unsupported-return` | `ps3_hle_call` (`runtime/ppu/ppu_hle.cpp`), from the adapter's `ps3_hle_adapt_host_ptr_stop` | `nid name reason` (`reason` is `returns a host pointer, not a guest EA`); `pc`/`lr` are the caller's return address (the import stub's caller) and `tid` is the calling guest thread |
| `spu-job-crt-version` | `spu_run_spurs_job` (`runtime/spu/spurs_job.c`) | `job image marker bytes` (`marker` is the 12 bytes at LS 0x30 with every non-printable as `?`, `bytes` the same 12 as hex) |
| `spu-job-memory` | `spu_run_spurs_job` | `job image binary bss memory limit` (bytes; `bss` is `eaBinary` high word x 16, `memory` = `binary + bss`, `limit` the lowest reserved top-of-LS address) |
| `spu-job-jm-entry` | `spu_run_spurs_job` | `job image entry lr`; `pc`/`lr` are the trap's LS address and the caller's link register |

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

## Frontier

A stop says how far the run got without a tracer of its own: it asks providers
registered with `ps3_stop_register_frontier`. The runtime registers two, over
state it already keeps: the last HLE import (`g_last_hle_nid` / `g_last_hle_name`,
`runtime/ppu/ppu_hle.cpp`) and the lv2 syscall the stopping thread is inside
(`g_sc_inflight`, `runtime/ppu/ppu_loader.cpp`).

## Threads

Any thread may stop. Stops are serialised: concurrent stops print one banner and
one machine line, the first's; the others park until the process exits.
