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

Lifted code does not track a guest pc, so for the first four `pc` is the caller's
return address (the import stub's caller, the `sc` site, the call site). Only
`unknown-instruction` knows the exact instruction address.

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
