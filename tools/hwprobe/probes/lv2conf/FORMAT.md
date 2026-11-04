# FORMAT.md: lv2conf result files

One file per run, written by the PPU backend on a console (`layer1/source/main.c`) and by the host backend
against the toolkit's HLE. Both write exactly this format; a compare tool diffs the `CASE` rows of two files.
Plain text, CSV, `\n` line ends, no quoting: no field contains a comma or a line break.

## Header

The standard hwprobe header (`common/hwprobe.h`, `README.md` "Result header"), then the probe's own rows:

```
HWPROBE,1
HDR,probe,lv2conf
HDR,probe_version,1
HDR,git_describe,<...>
HDR,source_sha256,<...>
HDR,fw_version,<declared by the operator, or unknown>
HDR,fw_type,<declared, or unknown>
HDR,console_model,<declared, or unknown>
HDR,console_info_source,<operator | unavailable>
HDR,config,<tag of the run, "default" when none>
HDR,tbfreq,<timebase ticks per second as the backend read it>
```

The host backend writes `HDR,fw_type,host-hle` and `HDR,console_model,host`. `hwprobe_format.parse_header()` reads
all of this.

## Rows

| Row | Meaning | Compared |
|---|---|---|
| `CASE,<case_id>,<key>,<value>` | one deterministic observation of a case | yes, for equality |
| `TIME,<case_id>,<key>,<ticks>` | a timing observation, in timebase ticks | never for equality |
| `SKIP,<case_id>,<reason>` | the case was not run | a skip on one side is "not comparable", not a mismatch |
| `META,<key>,<value>` | facts about the run | no |

Reserved keys: the host backend writes `CASE,<id>,stop,<kind>`, `hang,1`, `crash,<signal>` and `exit,<status>`
rows itself when it has to stop, hang-kill or lose a case process; no case records a key named `stop`, `hang`,
`crash` or `exit`. The host backend's `SKIP` reasons are `threads` (`CONF_THREADS`) and `hw_only` (`CONF_HW_ONLY`);
its `META` rows are `backend`, `toolkit`, `cases_total`, `cases_run`, `cases_skipped`.

- `<case_id>` is `<group>.<name>`, lower case, characters `[a-z0-9_.]`, stable: it is the key of the golden file.
  The group is the first component; names contain no `.`.
- `<key>` is `[a-z0-9_]+`, unique within its case among `CASE` rows (and among `TIME` rows).
- `<value>` of a `CASE` row is either `0x%08x` (a return code: `conf_rec_rc`, the low 32 bits of what the kernel
  returned) or a signed decimal integer (`conf_rec`). A boolean relation is `0` or `1`.
  A `CASE` value is a return code (`0x%08x`), a signed decimal, or, for the four host-runner outcome keys `stop`,
  `hang`, `crash` and `exit` (reserved: no case may record these keys), a kind string (`stop`: the named-stop kind,
  e.g. `unimplemented-syscall`) or a decimal (`hang,1`, `crash,<signal>`, `exit,<status>`); the console backend
  never writes them.
- `<ticks>` of a `TIME` row is an unsigned decimal integer.
- Rows of one case follow each other, in the order the case recorded them; cases follow registry order
  (`cases/registry.c`). A `SKIP` row stands where the case's rows would be.
- `SKIP` reasons written by the PPU backend: `not_selected` (the config's `groups=` excludes the case), `cfg_skip`
  (the config's `skip=` lists it or its group), `hung_watchdog` (the watchdog ended the run in this case). The host
  backend writes `threads` for every `CONF_THREADS` case.

What a case may record is restricted so that two correct kernels give the same rows: relations (`id != 0`, ids
differ, an output equals what was written, a counter moved by one), return codes, and values the call is specified
to produce. Never a raw kernel id, an address, a pointer, the pid, a clock reading or anything that depends on
the console's settings (`conf.h` states the rules for case code).

## META keys (PPU backend)

`backend` (`ppu`), `build`, `cases_total` (cases in the registry), `cases_run` (cases that returned),
`cases_skipped`, `run` (the run counter of `lv2conf_runid.txt`), `arena_peak` (bytes of scratch used by the
busiest case), `arena_overflows` (scratch requests served from the overflow sink; must be 0), `rows_truncated`
(the row buffer was full; must be 0), `case_limit_s`, `cfg_groups`, `cfg_skip`, `aborted_at` (only after a
watchdog abort: the case that did not return), and the `ui_*` counters of the screen
(`ui_enabled`, `ui_draws`, `ui_deferred`, `ui_overlaps`, `ui_timed_sections`, `ui_max_draw_us`).

A complete run has `cases_run + cases_skipped == cases_total`, no `aborted_at`, `arena_overflows` 0, `rows_truncated`
0 and `ui_overlaps` 0. The host backend writes `backend,host`, `cases_total`, `cases_run`, `cases_skipped`.

## Case flags

`CONF_HW_ONLY`: not run by the host backend (a call that blocks or needs the real console). `CONF_TIMING`: the case
exists for its `TIME` rows. `CONF_THREADS`: needs a second PPU thread, the host backend skips it. Every
`CONF_THREADS` case is written for the PPU only (`cases/threads.c`), and so are its recorded results: they exist on
hardware as a reference for later host support.

## Result file names

`lv2conf_r<run>[_<tag>].csv` in `/dev_hdd0/tmp/hwprobe/` (or `/dev_usb000/` when that fails);
`lv2conf_last.txt` names the last file, or, while a run is in progress, the case it is in.
