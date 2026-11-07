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
| `ppu.precise_usleep` | bool | `true` | `PS3RECOMP_PPU_PRECISE_USLEEP` | macOS only. A `sys_timer_usleep` of 20 us or more waits on a kevent timer with `NOTE_CRITICAL`, which the kernel does not coalesce; `nanosleep` wakes late by a quarter of the request or more (p50: 2.5 ms at 10 ms, 4.0 ms at 16 ms), so a guest that paces itself with `usleep(period - elapsed)` misses its deadline, which the console's accurate `usleep` does not. It changes no thread state (QoS class and priority stay), costs about 0.5 us a call, and if the host refuses the timer it logs `[usleep] precise-usleep-fallback` once and sleeps that call with `nanosleep`. `false` (`PS3RECOMP_PPU_PRECISE_USLEEP=0`) is the `nanosleep` reference path. Timing only; the Windows and Linux paths are unchanged and ignore it. Read once, at the first sleep |
| `spu.dma_repeat_limit` | u32 (1..4294967295) | `256` | `PS3RECOMP_SPU_DMA_REPEAT_LIMIT` | Identical back-to-back MFC transfers after which the SPU DMA engine backs off: it yields for the first 2 ms of the episode, where a healthy poll ends, then sleeps 1 ms per repeat. Timing only: the engine still makes progress and the guest observes nothing |
| `spu.dma_repeat_stop_secs` | u32 (0..86400) | `0` | `PS3RECOMP_SPU_DMA_REPEAT_STOP_SECS` | If non-zero, an SPU issuing identical MFC transfers for this many seconds stops with kind `spu-dma-repeat` (`image pc ea lsa cmd size count secs tid`). `0` never stops: polling a ring or label another agent writes is legitimate, and the old unconditional halt wedged those chains |
| `spu.mfc_lookup_cache` | bool | `true` | `PS3RECOMP_SPU_MFC_LOOKUP_CACHE` | Per host thread, remember the last SPU context whose MFC engine was looked up, so a repeated lookup for the same context skips the registry's read lock (every MFC channel access takes it otherwise). A release or forget of any engine publishes a new generation that drops every remembered engine, and a generation counter that would wrap turns the cache off for the process. On by default: in one title's replay the lock took over half of a frame-queue worker's CPU, and with the cache 99.7 % of lookups hit (#88). `false` (`PS3RECOMP_SPU_MFC_LOOKUP_CACHE=0`) is the reference path, every lookup under the lock. Timing only: both paths return the same engine. Read once, at the first MFC access |
| `gcm.wait_timeout_ms` | u32 (1..3600000) | `10000` | `PS3RECOMP_GCM_WAIT_TIMEOUT_MS` | How long an RSX wait may stand unsatisfied before the run stops with kind `gcm-wait-timeout`: a NV406E semaphore acquire whose label never reaches the value, the command-context buffer-full callback waiting for the RSX to leave the next segment, a guest store to a label waiting for the RSX walker's next pass, or a self-JUMP park nobody patches (`what=jump-park`). The hardware waits forever; a host watchdog, not PS3 behaviour |
| `gcm.park_resync` | bool | `false` | `PS3RECOMP_GCM_PARK_RESYNC` | Compatibility mode for a command-buffer park (a JUMP to itself the guest patches later). Off, the Reference behaviour: the RSX waits at the park however late the patch comes, GET never moves to `put` because a host deadline expired, and a park that stands the whole `gcm.wait_timeout_ms` with `put` elsewhere stops with `gcm-wait-timeout` (`what=jump-park`). On: after `gcm.park_grace_ms` GET moves to `put` and the commands in between, a fence among them, are dropped; every intervention is logged with the mode and the effective grace. For a title whose producer never patches the park; a host liveness policy, not PS3 behaviour |
| `gcm.park_grace_ms` | u32 (1..60000) | `50` | `PS3RECOMP_GCM_PARK_GRACE_MS` | How long a self-JUMP park stands with `put` elsewhere before `gcm.park_resync` moves GET to `put`. Effective only with `gcm.park_resync` on. God of War HD, loading its menu, took more than 50 ms between its append and its patch of the park |
| `gcm.handler_thread` | bool | `false` | `PS3RECOMP_GCM_HANDLER_THREAD` | Deliver the guest's vblank, flip and user handlers only on one dedicated host thread (the console's GCM interrupt thread) instead of nested in whatever guest thread made the HLE call. Needed by a title whose lock wrappers record the owner thread id and leak the lock when a handler runs inside the owner's thread (MLB The Show 16). Handlers then run concurrently with the guest threads. A compatibility mode |
| `gcm.report_spu_hold_ms` | u32 (0..60000) | `0` | `PS3RECOMP_GCM_REPORT_SPU_HOLD_MS` | If non-zero, a GET_REPORT waits up to this many milliseconds while SPU tasks that work in signal-driven rounds are in one, like an ACQUIRE (God of War: Ascension's RsxSync). Gives up after that with a logged line. 0 never holds. A compatibility timing policy |
| `eng.depth_clamp` | bool | `true` | `PS3RECOMP_ENG_DEPTH_CLAMP` | Honour the depth-clamp field of `NV4097_SET_ZMIN_MAX_CONTROL` (`0x1D78`): a fragment beyond the near or far plane is clamped to it instead of clipped, as on the RSX. `false` (`PS3RECOMP_ENG_DEPTH_CLAMP=0`) always clips at the near and far planes, as before the register was honoured. A kill switch for diagnosis (v0.19.0 #693) |
| `eng.depth_bounds` | bool | `true` | `PS3RECOMP_ENG_DEPTH_BOUNDS` | Emulate the depth-bounds test on a backend with no native one (Metal): the draw's depth is snapshotted before it and the fragment program discards a pixel whose stored depth lies outside the bounds. `false` (`PS3RECOMP_ENG_DEPTH_BOUNDS=0`) ignores the test, as before. A kill switch for diagnosis (v0.19.0 #694) |
| `vk.draw_batch` | bool | `false` | `PS3RECOMP_VK_DRAW_BATCH` | Vulkan backend: record the draw engine's draws, snapshots, texture uploads and clears into one command buffer per submission instead of a submit and fence wait each. Vertex, index and uniform data of every draw sit in host-coherent rings (176 MiB of host-visible memory); the batch is submitted and waited for at a present, a readback, an object destroy, a full ring or `vk.draw_batch_max` operations. Meant to change only when work is submitted, not what is drawn; off until a pixel diff of presented frames, on against off, is clean (#129). A device that cannot give the rings host-coherent memory stays off with a logged line. Read once, at the first draw. Replaces the unregistered `VK_DRAW_BATCH=1` |
| `vk.draw_batch_max` | u32 (1..1024) | `512` | `PS3RECOMP_VK_DRAW_BATCH_MAX` | The most operations recorded into one batched submission when `vk.draw_batch` is on. 1024 is the backend's descriptor-set count: a larger value is a `config-invalid` stop, where `VK_DRAW_BATCH_MAX` capped it silently. Replaces `VK_DRAW_BATCH_MAX` |
| `game.free_space_raw` | bool | `false` | `PS3RECOMP_GAME_FREE_SPACE_RAW` | Report the host disk's measured free space in every cellGame `hddFreeSizeKB` (capped at `INT32_MAX` KB). Off: below 4 GiB the measured value, from 4 GiB up RPCS3's 40 GiB - 256 KiB, because a guest multiplies `hddFreeSizeKB * 1024` in 32 bits and 4 GiB or more wraps to a few MB (inFAMOUS 2 failed its install check with 41.7 GB free). The figure above 4 GiB is RPCS3's, not a hardware measurement (v0.19.0 #692) |
| `trophy.register_wait_cap_vblanks` | u32 (1..16777216) | `1024` | `PS3RECOMP_TROPHY_REGISTER_WAIT_CAP_VBLANKS` | The most vblanks `sceNpTrophyRegisterContext` waits for its status events to be consumed by `cellSysutilCheckCallback` before it returns success anyway, logging `register-wait-cap` and counting it (`sceNpTrophy_register_wait_cap_count()`). The firmware blocks until the VSH has finished installing; the HLE paces status delivery over 120 vblanks (2.0 s at 59.94 Hz). The HLE's own worst case with per-vblank polling is about 121 vblanks (120 vblanks of pacing plus the 1-vblank #37 return step), longer only if the title polls rarely. The default of 1024 vblanks leaves ample margin for slow pollers (RPCS3's oracle tolerates up to 50 ms grace per event). A title that registers on the thread that does the polling hits the cap, and the events are then delivered by its later polls |
| `adec.silence_fallback` | bool | `false` | `PS3RECOMP_ADEC_SILENCE_FALLBACK` | Let `cellAdecDecodeAu` answer an AU it has no decoder for (every codec but AC3; AC3 without FFmpeg, with `PS3RECOMP_FFMPEG=0`, or when libavcodec decodes nothing from it) with `CELL_OK` and a block of silence of the codec's frame size, AUDONE and PCMOUT as for a decoded AU. Off, that AU is the `adec-no-decoder` stop (`func`, `codec`, `reason`): the console decodes it, and success for audio nobody decoded hides the gap (#130). The silence keeps a title that only counts PCM against time running; a title config that sets it says why in a comment and names the codec |
| `video.resolutions` | string | `""` | `PS3RECOMP_VIDEO_RESOLUTIONS` | The resolutions the display reports as available: a comma-separated list of cellVideoOut resolution ids (`1` 1080, `2` 720, `4` 480, `5` 576, `10` 1600x1080, `11` 1440x1080, `12` 1280x1080, `13` 960x1080). `cellVideoOutGetResolutionAvailability` answers from it, `cellVideoOutGetDeviceInfo` lists exactly these modes, and `cellVideoOutGetState` reports 720 before the title configures a resolution when 720 is listed, else the first of 480, 576, 720, 1080, 1600x1080, 1440x1080, 1280x1080, 960x1080 that is. An unknown id is a `config-invalid` stop. Empty means no restriction (every id available; the device lists 480, 576, 720, 1080). A policy, not a hardware fact: a console answers per connected display |
| `video.rsx_capture` | bool | `false` | `PS3RECOMP_VIDEO_RSX_CAPTURE` | Record the RSX command stream and every guest-memory input it consumes as a `ps3diff.rsx/1` capture (`docs/PS3DIFF.md`) under `video.rsx_capture_dir`. Off, the FIFO walker and the draw engine run as before. The capture starts at `cellGcmInit` (clean reset): a first RSX command with no GCM init before it is a `rsx-capture` stop (`reason=midstream`). Every consumed span is copied and hashed, so the run is slower |
| `video.rsx_capture_dir` | string | `""` | `PS3RECOMP_VIDEO_RSX_CAPTURE_DIR` | Where the capture goes: `capture.ndjson` and `blobs/sha256/`. Created if missing; a directory that already holds a capture is refused (`rsx-capture`, `reason=output-exists`). Empty with `video.rsx_capture` on is a `config-invalid` stop |
| `video.rsx_capture_frames` | u32 (0..1000000) | `0` | `PS3RECOMP_VIDEO_RSX_CAPTURE_FRAMES` | Close the capture after this many completed frames (flip heads) and stop capturing; the title keeps running. `0` captures until the process exits normally. A killed process leaves `capture.ndjson.partial`, which no tool accepts |
| `pad.record` | string | `""` | `PS3RECOMP_PAD_RECORD` | Record what the guest is handed by `cellPadGetData` (and the connection state `cellPadGetInfo`/`GetInfo2`/`cellPadPeriphGetInfo` report) into this file, as a `ps3recomp-pad-trace` (`docs/MODULES_REFERENCE.md`, "cellPad: record and replay"). The words are the ones stored to guest memory, after the host pad, the keyboard and the `PAD_SCRIPT`, `PAD_FILE`, `PAD_STICK` and `PAD_AUTOPRESS` injectors, keyed by `(flip, poll)`; only a change of state is written. The file is created and never overwritten: a path that exists, or one in a directory that is missing or not writable, is a `config-invalid` stop at load. Empty records nothing. May be set together with `pad.replay` to record the replayed input |
| `pad.replay` | string | `""` | `PS3RECOMP_PAD_REPLAY` | Play a trace recorded with `pad.record` back: every `cellPadGetData` answers with the recorded state of the latest record at or before the poll's `(flip, poll)`, a neutral pad before a port's first record and after the end of the trace, and the info calls report the recorded connection. The host pad and every injector are ignored. A path that is not a readable file is a `config-invalid` stop at load; a malformed file, an unknown version or keys that go backwards end the run with kind `pad-trace` (`docs/STOPS.md`). The end-of-trace line and the exit summary say how closely the guest followed the recording. Empty plays back nothing |
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
