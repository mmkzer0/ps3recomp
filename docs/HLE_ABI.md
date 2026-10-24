# HLE call ABI

Every firmware import a title calls is dispatched by `ps3_hle_call`
(`runtime/ppu/ppu_hle.cpp`) to a host C function in `libs/`. This page is the
contract between the two: which guest register a parameter comes from, what a
handler's declared return type does to `r3`/`f1`, which signatures are rejected,
and the one-off census that classified the handlers the toolkit ships.

The rule is simple: **the handler's declared C type is the truth.** The
generated registration table (`tools/gen_hle_nids.py`) instantiates
`ps3_hle_adapt<&handler>` (`runtime/ppu/ppu_hle_adapt.hpp`), which derives the
exact signature from `decltype(handler)` and marshals one guest call into one
host call. There is no universal function-pointer cast and no blanket sign
extension. A type that has no defined mapping fails the build with the
handler's name in the instantiation context.

## Parameters (PPC64 ELFv1), by 0-based position `i`

| Declared parameter | Comes from |
|---|---|
| `s8..s32`, `u8..u32`, `bool`, enum | `gpr[3+i]`; types of 4 bytes or less use the low word (the caller already extended it per its own declared type) |
| 64-bit integral | `gpr[3+i]` |
| object/function pointer | low 32 bits of `gpr[3+i]`, zero-extended, `reinterpret_cast` to the declared type |
| `float` / `double` | the next FPR: `fpr[1+k]`, where `k` is the number of FP parameters before it (max 13). It still consumes its positional GPR slot |
| `i >= 8` (integral/pointer) | the caller's parameter save area: big-endian doubleword at `r1 + 48 + 8*i` (`vm_read64`) |

The save-area offset is the ELFv1 one and is already used elsewhere in the
runtime: `libs/network/sceNpScore.c`'s `stack_arg` reads the ninth argument at
`r1 + 112` (`48 + 8*8`). Census: five registered handlers take more than eight
parameters (`_cellSpursCreateJobQueueWithJobDescriptorPool` 12,
`cellGcmSetZcull` 12, `_cellSpursCreateJobQueue` 10,
`cellSaveDataListAutoLoad` 9, `cellSpursWorkloadAttributeInitialize` 9).

## The GUEST_PTR convention for pointer parameters

A PS3 pointer is a 32-bit effective address. The toolkit's HLE libraries declare
guest pointers with real C pointer types but receive the guest EA in them and
translate with `GUEST_PTR` (`runtime/ppu/ppu_memory.h`):

```c
static int32_t cellRescInit(void* buffer) {
    void* p = GUEST_PTR(buffer, void*);   /* NULL stays NULL */
    ...
}
```

The adapter therefore maps every pointer parameter to
`reinterpret_cast<T>(static_cast<uintptr_t>(static_cast<uint32_t>(gpr)))` — the
low 32 bits, zero-extended.
The old generic path passed the whole 64-bit register; only the low 32 bits were
ever a guest address.

## Returns

| Declared return | Effect |
|---|---|
| `void` | `r3` is not touched |
| signed integral ≤ 32 bits | sign-extended into `r3` (`s32 -0x7FFEFFF1` → `0xFFFFFFFF8001000F`) |
| unsigned integral ≤ 32 bits, `bool` | zero-extended into `r3` |
| enum | by its underlying type |
| 64-bit integral | the 64-bit value, unchanged |
| `float` / `double` | `fpr[1]` (stored as a double; guest `stfs` narrows); `r3` untouched |
| pointer | **no default mapping** — see the classification below |

`std::is_signed_v` decides sign vs zero extension for the plain `char` case, so
the platform's `char` signedness is honoured rather than assumed.

## Pointer returns are classified, never guessed

A handler whose declared return is a pointer must be listed, with a reason, in
`tools/gen_hle_nids.py`. The list is what selects the adapter, so a new
pointer-returning handler that is not classified fails the build (the plain
adapter's `static_assert` says to classify it). Census: 26 of the 1200 scanned
definitions return a pointer (22 registered, 4 excluded;
`python3 tools/test_gen_hle_nids.py --census`).

**Guest EA (16)** — the body returns an effective address; the adapter
zero-extends its low 32 bits (`ps3_hle_adapt_guest_ea`):

`cellFiberPpuSelf`, `cellGcmGetControlRegister`, `cellGcmGetLabelAddress`,
`cellGcmGetNotifyDataAddress`, `_sys_strcpy`, `_sys_strncat`, `_sys_memset`,
`_sys_memcpy`, `sys_heap_malloc`, `sys_heap_memalign`, `_sys_malloc`,
`_sys_memalign`, `_sys_realloc`, `_sys_heap_malloc`, `_sys_heap_memalign`,
`cellSheapAllocate`.

`cellSheapAllocate` was fixed under this contract: it used to return
`vm_base + heap_ea + offset` (a host pointer). It now returns
`heap_ea + offset`, and its block table is keyed by the guest EA so
`cellSheapFree` keeps working. `runtime/ppu/tests/test_hle.cpp` asserts the
value through `ps3_hle_call`.

**Host pointer (6)** — a real firmware export whose implementation returns a
host address today. There is no correct guest value to hand over, so the
adapter (`ps3_hle_adapt_host_ptr_stop`) never calls the body: the dispatch
stops with `kind=hle-unsupported-return`
(fields `nid`, `name`, `reason="returns a host pointer, not a guest EA"`,
site `pc`/`lr` = the caller's `lr`, `tid` = the guest thread id).

| Handler | Body returns | Known defect, to be fixed by |
|---|---|---|
| `cellGcmGetDisplayBufferByFlipIndex` | `&s_display_buffers[i]` | cellGcmSys |
| `cellGcmGetDisplayInfo` | `&s_display_buffers[i]` | cellGcmSys |
| `cellGcmGetReportDataAddress` | `&s_report_data[i]` | cellGcmSys |
| `cellGcmGetReportDataAddressLocation` | `&s_report_data[i]` | cellGcmSys |
| `cellGcmGetTileInfo` | `&s_tiles[i]` | cellGcmSys |
| `cellGcmGetZcullInfo` | `&s_zcull[i]` | cellGcmSys |

These are the firmware's guest-EA getters implemented over host arrays; once a
module returns the guest EA instead, its name moves to the guest-EA list.

## Variadic handlers (3)

`_sys_printf`, `_sys_sprintf`, `_sys_snprintf` (`libs/system/sysPrxForUser.c`)
are declared variadic. `ps3_hle_adapt_variadic` calls through the exact
variadic prototype: the fixed parameters are marshalled by the rules above, and
the remaining integer argument registers `r(3+n)..r10` are forwarded as
`uint64_t` trailing arguments, which is exactly where `va_start`/`va_arg` reads
them.

Host limits, stated rather than assumed:

- **Apple arm64**: variadic arguments are passed on the stack as 8-byte slots,
  so integer, pointer, `u64` and `double` varargs are all bit-exact. PPC64
  ELFv1 has the caller place a variadic float/double in the GPR image as well
  as in an FPR, so the forwarded word carries the double bits.
- **x86-64 SysV**: `va_arg(double)` reads the XMM save area, which these GPR
  words do not populate, so floating-point varargs are not carried there.
  Integer/pointer varargs are.
- Arguments the guest passed beyond `r10` (its parameter save area) are not
  forwarded at all; a format string must fit its arguments in the eight
  integer registers.

## Signatures that fail the build

`static_assert` with a message naming the problem (the instantiation context
names the handler):

- reference, array, member-pointer parameters or returns;
- struct/union passed or returned **by value** (take a pointer: the guest EA);
- a return type with no mapping;
- more than 13 floating-point parameters;
- a variadic handler instantiated through the plain adapter (use
  `ps3_hle_adapt_variadic`), and a variadic handler with more than eight fixed
  parameters.

`tools/hle_abi_audit.py` remains the separate question of whether a declared
type matches the firmware's. This contract only guarantees that whatever is
declared is called correctly.

## Census

Every number below is produced by a command over this checkout; the tree moves,
the commands do not.

`python3 tools/gen_hle_nids.py --all --out /tmp/x.cpp` is the run the build
uses. It prints

    wrote /tmp/x.cpp: 1158 NID handlers from 94 module(s); scanned 1209 definition name(s), excluded 51 host-side helper(s)

and `grep -c 'ps3_hle_register(0x' /tmp/x.cpp` prints `1158`. So: 94 module
sources, 1209 unique definition names, **1158 registrations**, **51 host-side
helpers** excluded. Registration is fail-closed: `classify_name` decides every
one of the 1209 (see "Firmware export or host-side helper" below), and a name
with no evidence stops the generator (exit 2) instead of being registered.

`python3 tools/test_gen_hle_nids.py --census` (requires `clang`) takes the
*definition* of every scanned name from each module source's
`clang -Xclang -ast-dump=json`, resolves typedefs to
integral/enum/pointer/record/function-pointer, and prints:

    modules scanned: 94
    scanned definition names: 1209
    definitions with a clang AST: 1209 (0 missing)
    registered: 1158 (323 in the known NID sources, 835 by SDK export shape), excluded host-side helpers: 51
    returns: integral 1124, void 58, float 1, pointer 26
    pointer returns: 26 total = 22 registered (16 guest EA + 6 host-pointer stop) + 4 excluded (cellGame_get_hdd_title_id, cellGame_get_title, cellGame_get_title_id, cellGcm_fifo_kick_event)
    parameters: pointer 1044, integral 1405, float 16, function-pointer 73, variadic 3, record 0, reference 0, array 0, enum 0
    float parameters: 16 in 12 handlers
    more than 8 parameters: _cellSpursCreateJobQueueWithJobDescriptorPool(12), cellGcmSetZcull(12), _cellSpursCreateJobQueue(10), cellSaveDataListAutoLoad(9), cellSpursWorkloadAttributeInitialize(9)

| Return class | Count |
|---|---|
| integral (including the 3 variadic `_sys_*printf`) | 1124 |
| void | 58 |
| `float` (`cellAvconfExt_display_gamma`, excluded as a helper) | 1 |
| pointer | 26 (16 guest EA + 6 host-pointer stop + 4 excluded helpers) |

Parameter features: 1044 pointer, 1405 integral, 16 floating-point across 12
handlers, 73 function-pointer, 3 variadic, 0 struct/union by value, 0
reference, 0 array. Five handlers exceed eight parameters
(`_cellSpursCreateJobQueueWithJobDescriptorPool` 12, `cellGcmSetZcull` 12,
`_cellSpursCreateJobQueue` 10, `cellSaveDataListAutoLoad` 9,
`cellSpursWorkloadAttributeInitialize` 9).
Every one of the 1158 registered signatures compiles through the adapter, so
none is in a rejected class.

### Firmware export or host-side helper

The name scan finds every non-static definition named `cell*`, `sce*`,
`sys_*`, `_cell*` or `_sys_*`, which includes host-side helpers the runtime
calls directly. `tools/gen_hle_nids.py` classifies each name with evidence and
refuses to guess:

1. `NOT_FIRMWARE_EXPORTS` — an explicit entry with its reason.
2. `HELPER_SHAPES` — the naming shapes the SDK does not use for an export: a
   `cell*` prefix followed by `_` and a lowercase letter
   (`^_?cell[A-Za-z0-9]*_[a-z]`, e.g. `cellPad_poll`, `cellfs_translate_path`,
   `cellRtcGetTime_t`), or `_syscall_` anywhere. `sys_*` and `_sys_*` are
   deliberately absent: they are the SDK's own snake_case export family
   (`sys_net_bnet_accept`, `sys_heap_malloc`, `_sys_malloc`).
3. the known NID sources (`tools/nid_database.py`'s builtin table,
   `tools/nid_from_protos.json`) or an SDK export shape (CamelCase after the
   module prefix).

A shape hit or explicit entry that also appears in the known NID sources is a
contradiction and stops the generator: the rule is narrowed, never allowed to
drop a real export. A name that fits none of the three classes also stops it
(exit 2, the name printed) and no table is written. Every dropped name is thus
absent from both known-export sources (the contradiction check), and every
kept name has evidence: the census prints the split — 323 of the 1158
registrations are in the known NID sources, the other 835 pass an SDK export
shape, and none is in neither.

None of the 51 excluded names is a firmware export, so registering one could
only shadow a real import: a title import whose 32-bit NID happened to match
would dispatch guest arguments into a host function.

**Excluded by shape (40).**

| Group | Names |
|---|---|
| cellGcmSys host plumbing | `cellGcm_display_buffer_count`, `cellGcm_fifo_kick_event`, `cellGcm_fifo_kick_wait`, `cellGcm_fifo_recycle`, `cellGcm_flip_request_count`, `cellGcm_ref_on_poll`, `cellGcm_request_tick`, `cellGcm_rsx_process_fifo`, `cellGcm_take_flip_pending`, `cellGcm_take_flip_pending_synced`, `cellGcm_control_guest_addr`, `cellGcm_replay_display_buffers_to_engine` |
| cellGcmSys guest-syscall bridge | `cellGcm_syscall_bringup`, `cellGcm_syscall_iomap`, `cellGcm_syscall_iounmap`, `cellGcm_syscall_set_fifo` |
| cellGame boot/params plumbing | `cellGame_get_title`, `cellGame_get_title_id`, `cellGame_get_hdd_title_id`, `cellGame_set_title`, `cellGame_set_title_id`, `cellGame_set_content_path`, `cellGame_init_from_paramsfo` |
| input event injection | `cellPad_poll`, `cellKb_keyEvent`, `cellMouse_moveEvent`, `cellMouse_buttonEvent`, `cellMouse_wheelEvent` |
| video / save / rtc | `cellVideoOut_set_resolution`, `cellAvconfExt_display_gamma`, `cellSaveData_set_scratch_region`, `cellSaveData_register_ctx_handlers`, `cellRtcGetTime_t`, `cellRtcSetTime_t`, `cellResc_convert_register_count` |
| filesystem path mapping | `cellfs_translate_path`, `cellfs_add_path_mapping`, `cellfs_set_root_path` |
| per-frame hooks | `cellMsgDialog_tick`, `cellSysutil_pump_seen` |

**Excluded by explicit list (7).** These names are CamelCase after the module
prefix — shape-identical to an SDK export — so only a reader can say they are
host helpers:

| Name | Why it is not an export |
|---|---|
| `cellGcmOffsetIsDisplay` | host display-offset predicate (`extern` in `rsx_d3d12_backend.c`) |
| `cellGcmResolveOffset` | host offset→EA resolver (`extern` in `rsx_vertex_fetch.c`) |
| `cellGcmResolveIO` | host IO-table resolver (`extern` in `rsx_draw_engine.c`) |
| `cellGcmResolveLocated` | host MAIN/LOCAL resolver (`extern` in `cellGcmSys.c`) |
| `cellUserInfoSelectUser_ListGet`, `cellUserInfoSelectUser_ListSet` | host-side halves of the select-user list API (`cellUserInfoSelectUser` is the export) |
| `sys_rsx_init` | internal init helper called from `lv2_register.c`, takes a host `lv2_syscall_table*` |

## Adding a handler

1. Define the function in `libs/<dir>/<module>.c` and declare it, with its real
   signature, in the module header. The definition is the truth; a missing
   header declaration is a build error at the handler's name.
2. If it returns a pointer, add its name to `GUEST_EA_RETURN` or
   `HOST_POINTER_RETURN` in `tools/gen_hle_nids.py` with a comment saying why.
3. A host-side helper must not be registered: name it with a helper shape
   (`<cell module>_<lowercase>`, `*_syscall_*`) so the rule excludes it, or add
   it to `NOT_FIRMWARE_EXPORTS` with a reason. Anything the scanner cannot
   classify stops the generator (exit 2, the name printed); it is never
   registered by guess.
4. Run `python tools/gen_hle_nids.py --all --out <build>/ppu_hle_nids.cpp`.
   The generated unit includes `ppu_hle_adapt.hpp` and the module headers by
   repository-relative path. `ps3recomp_ppu_hle_nids(...)`
   (`cmake/PS3RecompPPU.cmake`) sets the toolkit root, `runtime/ppu` and
   `include` as INCLUDE_DIRECTORIES on that source file, so they stay private
   to it; a hand-rolled build must pass the same `-I` directories.

The generated unit's includes are a source-file property, not a target
property: a title compiles its own sources without `libs/*` on the include
path.

## Gate

- `tools/test_hle_abi.py` (`ctest -R hle_abi`): every return class, the mixed
  positional case, the 9th/10th save-area parameters, the guest-EA opt-in,
  variadic forwarding, `cellSheapAllocate`'s guest EA, and the
  `hle-unsupported-return` stop (exit 70).
- `tools/test_sys_spu_printf.py`: the `s32` `_sys_spu_printf_*` family through
  the NID table, asserting the sign-extended 64-bit `r3`.
- `tools/test_gen_hle_nids.py` (`ctest -R gen_hle_nids`): every scanned name is
  a classified export or an excluded helper, the review's helper names stay out
  of the table, and a synthetic unclassifiable definition exits 2 naming it and
  writes nothing. Its `--census` mode reproduces the numbers above (needs
  `clang`).
- The full `--all` table compiles only when every handler is classified.
