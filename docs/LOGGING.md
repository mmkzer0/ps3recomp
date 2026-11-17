# Logging

The runtime prints through about 1 500 `fprintf(stderr, "[tag] ...")` calls. `ps3_log`
([`include/ps3emu/ps3_log.h`](../include/ps3emu/ps3_log.h), implementation
`runtime/diag/ps3_log.c`) gives those calls a tag, a per-site cap that reports what it swallowed, and
a run log file that survives the next run, on macOS, iOS, Linux and Windows. On iOS and in a Windows
GUI run there is no terminal to capture stderr; the file is the record.

(`runtime/ps3_log.h` is a different header: the `PS3_VERBOSE` gate for chatty per-wait logging.)

## What goes where

| | stderr | file (`log.file`) |
|---|---|---|
| the line, as the `fprintf` printed it | yes, byte for byte | yes, behind `<seconds> <thread> <level> ` |
| collapsed repeats (`log.collapse`) | only with `all` | yes (`file`, the default, and `all`) |
| `[log] suppressed N lines after the first L at file:line [tag]` | no | yes, at exit |
| `[log] size cap reached, N further lines not written` | no | yes, at exit |
| the `PS3STOP` banner | its own write | mirrored, after the summary above |
| muted tags (`log.mute`) | no | yes |
| lines above `log.level` | no | no |

## The invariant

**stderr stays byte-identical for a migrated call site.** ctest `PASS_REGULAR_EXPRESSION`s,
`tools/c3_gate.sh`, the in-run audit's `[jq2-xdiff]` and `[shadow]` lines and many tools grep it. The
prefix `PS3_LOG` prints is exactly the `[tag] ` the old literal had (`""` prints none), and the format
that follows is the old format. Nothing new goes to stderr unless an operator opts in
(`log.collapse = all`, `log.mute`, `log.level`). `tests/gates/test_log.py` compares the bytes against
the `fprintf` lines.

## The file

```
<seconds since the first log call, %.6f> <thread> <E|W|I|D> <message as on stderr>
```

`thread` is the host thread's name if it has one (`pthread_getname_np` or `prctl(PR_GET_NAME)`;
`GetThreadDescription` on Windows, looked up at run time because old systems lack it), else the
numeric host thread id. Whitespace in a name becomes `_`. A message that does not end in a newline
gets one in the file, so each file line is one record. The mirrored stop banner is written raw.

### Rotation

At `ps3_config_load()` the existing `log.file` is renamed to `<log.file>.prev`, replacing an older
`.prev`, and a new file is created: exactly one previous run is kept. A path that exists but is not a
regular file is refused, never renamed. The file is fully buffered (64 KiB) and the log owns a flusher
thread, started when the file opens, that flushes it every 50 ms. It has to be the log's own: hosts load
the config (which opens the file) long before `ppu_run()` calls `ps3_stdio_init()`, which is first-call-wins,
takes over stdout and stderr as well, and would drag its thread and `win32_compat` into every program
that links `config.cpp`. So a run killed or crashed at any point loses at most 50 ms of the file,
whatever `ps3_stdio` does later (`PS3_STDIO_UNBUFFERED=1` included, which is why that mode needs no
per-line flush). `ps3_stdio`'s flusher and crash path flush it too. If the thread cannot be started, the
file is flushed after every line instead.

### Failing closed

A configured `log.file` that cannot be rotated or created stops the run: kind `host-log-file`
(fields `op`, `errno`, `path_len`; [`STOPS.md`](STOPS.md)). A path whose `.prev` twin does not fit the
1024-byte host buffer stops with `host-path-limit`. A run whose log was asked for does not go on
without it.

### Lines before the configuration

The runtime prints before the config is loaded. Those lines go to stderr as ever and into a 64 KiB
buffer that is written to the file when it opens. Overflow is counted; the file says how many lines
did not fit. Before configure the level filter is `info`, nothing is muted and nothing collapses.

### Size cap

`log.max_mb` (default 256) protects device storage. At the cap the file gets one `size cap reached`
line at exit and nothing else (no summary, no banner); stderr is unaffected.

### Collapse

Consecutive identical lines from the same call site: the first is written, the repeats are counted,
and `[log] previous line repeated N times` follows when a different line arrives, at exit and before
the stop banner. Two sites printing the same text do not collapse. With `log.collapse = all` stderr
collapses the same way, which knowingly breaks byte identity.

### Exit summary

At exit (and before a stop banner) every `PS3_LOG_N` site that swallowed lines gets one
`[log] suppressed N lines after the first L at file:line [tag]` in the file. A site that never
reached its limit prints nothing.

### Shutdown

The process ends under live threads: `ps3_host_exit` is `quick_exit`, a stop is `_Exit`. Every logging
operation holds a gate from its admission (where a `PS3_LOG_N` slot is taken, before the arguments are
evaluated) to the end of its output. At exit, and for a stop before it writes its banner, the closer:

1. shuts the gate: new operations are refused, those already inside get up to 250 ms to finish;
2. cancels the stragglers: from then on an operation that was admitted earlier but reaches output is
   dropped, under the output lock, from both sinks (an operation already writing holds that lock and
   finishes first). So nothing follows the `PS3STOP` line on stderr, or the summary in the file;
3. stops the log's flusher thread and waits up to 250 ms for it to leave, so that normal stream cleanup
   never races a flush (a tick re-checks the stop signal under the output lock as well);
4. takes the output lock with a 250 ms deadline. If it is not free (a stalled disk, a thread that died
   holding it) the file's final work is skipped and the process ends anyway, after one line on stderr:
   `[log] the log lock was still held at the shutdown deadline; ...`.

All the deadlines hang off the first closer's start, so a stop followed by its own mirror, or an exit
after a stop, never waits twice. An operation still inside after the bound (an argument that blocks) is
not waited for.

## Configuration

Typed registry only ([`CONFIG.md`](CONFIG.md)): `log.file`, `log.level` (`error` | `warn` | `info`
| `debug`, default `info`), `log.mute` (comma-separated whole tags), `log.collapse` (`file` | `all` |
`off`), `log.max_mb`. Environment: `PS3RECOMP_LOG_FILE` and so on. A title config sets `[log] file = "..."`.

## Migrating a call site

```c
fprintf(stderr, "[spu] x %d\n", v);          →  PS3_LOG(PS3_LOG_INFO, "spu", "x %d\n", v);

static int n = 0;                             →  PS3_LOG_N(8, PS3_LOG_INFO, "spu", "x %d\n", v);
if (PS3_LOG_FIRST(n, 8)) fprintf(stderr, "[spu] x %d\n", v);
```

- Keep the `\n` in the format; the tag is the text between the `[` and `]` the old literal had, without
  the space. Level is `PS3_LOG_INFO` unless the site is clearly an error or a warning that nothing
  depends on; a debug line that printed before must stay `INFO` or it disappears by default.
- `PS3_LOG_N` keeps `PS3_LOG_FIRST`'s contract: exactly `limit` lines (a constant of at least 1)
  get through, the counter stops there and cannot wrap, and a silenced line does not evaluate its
  arguments. What it swallowed is reported at exit. The counter is atomic.
- Other conditions around the old `fprintf` (`if (verbose && PS3_LOG_FIRST(...))`) keep their `if`.
- `PS3_LOGE/W/I/D(tag, ...)` are shorthands for `PS3_LOG` at the four levels.
- The macros are for C and C++; formats are checked as `printf` formats.

`include/ps3emu/log_first.h` stays until the migration waves have moved every user to `PS3_LOG_N`; it
is deleted with the last one.

## Linking

`runtime/config/config.cpp` calls `ps3_log_configure()`, so everything that links `config.cpp` links
`runtime/diag/ps3_log.c` too: a title and `rsx_replay` get it from `libps3recomp_runtime.a` (the strong
reference pulls the member in), CMake fixtures list it beside `config.cpp`, and a standalone fixture
built by a Python tool or script compiles it with the other sources. A fixture that stubs the stop
runtime itself cannot also link `stop.cpp`; it answers the log's reference with
`tests/ps3_log_mirror_stub.c` (a weak `ps3_config_get`/`ps3_stop` let the fixture's own definitions
win). `stop.cpp` does not depend on the log at all: it calls the hooks set by
`ps3_stop_set_mirror`, which `ps3_log_configure` installs.
