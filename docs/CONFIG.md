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
  diagnostic variables that existing code still reads directly
  (`PS3_CFG_LEGACY_ENV` in the registry); moving one of those into the registry is
  how it leaves that list.

## File format

TOML. The first thing in the file must be the schema version:

```toml
schema = 1

[ppu]
vscr_nj = false

[title]
elf_sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"
```

`schema` is required and must equal the version the runtime reads
(`PS3_CONFIG_SCHEMA_VERSION`, currently 1); any other value is refused before
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
| `ppu.opd_swap_fixup` | bool | `false` | `PS3RECOMP_PPU_OPD_SWAP_FIXUP` | Redirect an indirect call whose `ctr` is not a lifted function but whose `r2` is (the guest wrote the function descriptor as `{toc, code}` instead of `{code, toc}`). The redirect compensates for a malformed title descriptor (Odin Sphere's flOw-style handler tables), not a PPC ABI guarantee; off, the call stops as `unresolved-indirect-call` and its hint names this option |
| `ppu.null_page` | string (`stop` \| `drop`) | `stop` | `PS3RECOMP_PPU_NULL_PAGE` | What a guest load below `0x10000` or store below `0x1000` does. `stop`: the run ends at the first one with kind `guest-null-access` (`access width ea fn`, plus `pc lr tid`). `drop`: the legacy handling for a title that needs it, a load logs `[null-read]` and returns what memory holds, a store logs `[null-write]` and is dropped, and a store sweep that starts in the null page (a memset on a null buffer) stays dropped until it breaks off. Hardware faults on every such access, so `drop` papers over a guest defect; a title config that sets it must carry a written reason (see below) |
| `spu.dma_repeat_limit` | u32 (1..4294967295) | `256` | `PS3RECOMP_SPU_DMA_REPEAT_LIMIT` | Identical back-to-back MFC transfers after which the SPU DMA engine backs off: it yields for the first 2 ms of the episode, where a healthy poll ends, then sleeps 1 ms per repeat. Timing only: the engine still makes progress and the guest observes nothing |
| `spu.dma_repeat_stop_secs` | u32 (0..86400) | `0` | `PS3RECOMP_SPU_DMA_REPEAT_STOP_SECS` | If non-zero, an SPU issuing identical MFC transfers for this many seconds stops with kind `spu-dma-repeat` (`image pc ea lsa cmd size count secs tid`). `0` never stops: polling a ring or label another agent writes is legitimate, and the old unconditional halt wedged those chains |
| `gcm.wait_timeout_ms` | u32 (1..3600000) | `10000` | `PS3RECOMP_GCM_WAIT_TIMEOUT_MS` | How long an RSX wait may stand unsatisfied before the run stops with kind `gcm-wait-timeout`: a NV406E semaphore acquire whose label never reaches the value, the command-context buffer-full callback waiting for the RSX to leave the next segment, a guest store to a label waiting for the RSX walker's next pass, or a self-JUMP park nobody patches (`what=jump-park`). The hardware waits forever; a host watchdog, not PS3 behaviour |
| `gcm.park_resync` | bool | `false` | `PS3RECOMP_GCM_PARK_RESYNC` | Compatibility mode for a command-buffer park (a JUMP to itself the guest patches later). Off, the Reference behaviour: the RSX waits at the park however late the patch comes, GET never moves to `put` because a host deadline expired, and a park that stands the whole `gcm.wait_timeout_ms` with `put` elsewhere stops with `gcm-wait-timeout` (`what=jump-park`). On: after `gcm.park_grace_ms` GET moves to `put` and the commands in between, a fence among them, are dropped; every intervention is logged with the mode and the effective grace. For a title whose producer never patches the park; a host liveness policy, not PS3 behaviour |
| `gcm.park_grace_ms` | u32 (1..60000) | `50` | `PS3RECOMP_GCM_PARK_GRACE_MS` | How long a self-JUMP park stands with `put` elsewhere before `gcm.park_resync` moves GET to `put`. Effective only with `gcm.park_resync` on. God of War HD, loading its menu, took more than 50 ms between its append and its patch of the park |
| `trophy.register_wait_cap_vblanks` | u32 (1..16777216) | `1024` | `PS3RECOMP_TROPHY_REGISTER_WAIT_CAP_VBLANKS` | The most vblanks `sceNpTrophyRegisterContext` waits for its status events to be consumed by `cellSysutilCheckCallback` before it returns success anyway, logging `register-wait-cap` and counting it (`sceNpTrophy_register_wait_cap_count()`). The firmware blocks until the VSH has finished installing; the HLE paces in vblanks, not wall-clock time, as RPCS3 does in real time, and the default is RPCS3's worst case for a 128-trophy title (256 events, each 1 vblank plus the 3 vblanks of its 50 ms grace). A title that registers on the thread that does the polling hits the cap, and the events are then delivered by its later polls |
| `video.resolutions` | string | `""` | `PS3RECOMP_VIDEO_RESOLUTIONS` | The resolutions the display reports as available: a comma-separated list of cellVideoOut resolution ids (`1` 1080, `2` 720, `4` 480, `5` 576, `10` 1600x1080, `11` 1440x1080, `12` 1280x1080, `13` 960x1080). `cellVideoOutGetResolutionAvailability` answers from it, `cellVideoOutGetDeviceInfo` lists exactly these modes, and `cellVideoOutGetState` reports 720 before the title configures a resolution when 720 is listed, else the first of 480, 576, 720, 1080, 1600x1080, 1440x1080, 1280x1080, 960x1080 that is. An unknown id is a `config-invalid` stop. Empty means no restriction (every id available; the device lists 480, 576, 720, 1080). A policy, not a hardware fact: a console answers per connected display |
| `video.rsx_capture` | bool | `false` | `PS3RECOMP_VIDEO_RSX_CAPTURE` | Record the RSX command stream and every guest-memory input it consumes as a `ps3diff.rsx/1` capture (`docs/PS3DIFF.md`) under `video.rsx_capture_dir`. Off, the FIFO walker and the draw engine run as before. The capture starts at `cellGcmInit` (clean reset): a first RSX command with no GCM init before it is a `rsx-capture` stop (`reason=midstream`). Every consumed span is copied and hashed, so the run is slower |
| `video.rsx_capture_dir` | string | `""` | `PS3RECOMP_VIDEO_RSX_CAPTURE_DIR` | Where the capture goes: `capture.ndjson` and `blobs/sha256/`. Created if missing; a directory that already holds a capture is refused (`rsx-capture`, `reason=output-exists`). Empty with `video.rsx_capture` on is a `config-invalid` stop |
| `video.rsx_capture_frames` | u32 (0..1000000) | `0` | `PS3RECOMP_VIDEO_RSX_CAPTURE_FRAMES` | Close the capture after this many completed frames (flip heads) and stop capturing; the title keeps running. `0` captures until the process exits normally. A killed process leaves `capture.ndjson.partial`, which no tool accepts |
| `title.elf_sha256` | string | `""` | `PS3RECOMP_TITLE_ELF_SHA256` | Optional pin: 64 hex digits, the SHA-256 of the executable `ppu_load_elf` may load, checked in addition to the hash the lifter recorded. Empty means no pin |
| `diag.dump_config` | bool | `false` | `PS3RECOMP_DIAG_DUMP_CONFIG` | Print the effective configuration to stderr after loading |

The registry's `doc` strings are the authoritative description of each option.

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
schema = 1

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
